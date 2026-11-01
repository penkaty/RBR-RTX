// worldlights.cpp - lamps in the world that the game only paints (user 2026-11-01: "In main menu ... the visible
// lamp-post has to generate a light from dawn to dusk").
//
// config\lights.ini: one [lamp.<name>] section per lamp kind, keyed by the Remix hash of the texture its object is drawn
// with (the 8 frame dump prints "hash" and "at (x y z) r" for every draw). Every frame a draw with that texture is seen,
// its world bounding sphere (comp.cpp cull_entry, cached per buffer range) places a sphere light: anchor = top (the
// sphere's top, a thin pole's tip) or centre, plus offset. Draws whose lamps lie within 1 m of each other share one light
// (a lamp head drawn in two pieces). The lights shine while the sun shown (rbrrtx_sun_shown_elevation: stage sun, menu
// sun, debug sun) stands below on_below_sun, fading in over fade_degrees; drawn ones only, at most max_lights nearest
// the camera. Power in lumens (an LED street lamp 6,000-12,000 lm), colour as a temperature.
// uv = u0, v0, u1, v1 (a lamp in a texture atlas, e.g. the main menu's lamp post, 04153DADDBE42CA1, drawn in batches
// of swaying scenery 10-50 m across): instead of the draw's sphere, the vertices whose TEXCOORD0 falls in that
// rectangle, grouped within 1.5 m, each group's centre (comp.cpp world_light_draw); up to 8 rectangles (uv, uv2 .. uv8).
// anchor = top | centre | bottom: for a uv group its highest y, centre or lowest y (the bulb under a glass); for a draw's
// sphere the sphere's top, centre or bottom.
//
// Marking (user 2026-11-01: the automatic detector found no street lamps; European posts hang the head on an arm, the
// glass faces down and the pole is a separate draw): the lamp_mark key (Alt+PageUp, comp.cpp lamp marker) casts a ray from the
// screen centre, takes the texture and UV rectangle of the lamp head hit, and appends a [lamp.mark_<hash>_<n>] section
// to config\lights-marked.ini (anchor = bottom, no pole test, values from lights.ini [auto] unless the section sets them).
// Marks of one texture merge into one kind (several rectangles). Load order: lights-auto.ini, lights-marked.ini,
// lights.ini; the last one wins for the same texture. rbrrtx_world_lights_reload re-reads them at once after a mark.
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <d3d9.h>
#include "third_party/remix_c.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Kind {
  std::wstring name;
  int anchor = 0;                                      // 0 top, 1 centre, 2 bottom
  float offset[3] = {};
  float lumens = 8000.0f, kelvin = 3000.0f, radius = 0.15f, volumetric = 1.0f;
  int where = 0;                                       // 0 anywhere, 1 menu only, 2 stages only
  std::vector<std::array<float, 4>> uvs;               // uv, uv2.. uv8: lamp rectangles in the texture
  bool pole = false;                                   // auto = 1: keep a vertex group only on a pole (comp.cpp)
  float minHeight = 3.0f;                              // pole test: group at least this far above the draw beneath
};
struct Lamp {
  uint64_t hash = 0;
  const Kind* kind = nullptr;
  float pos[3] = {};
  float sent[3] = {};
  float sentK = -1.0f;
  remixapi_LightHandle h = nullptr;
  unsigned seen = 0;                                   // frame last drawn
};

const remixapi_Interface* g_api = nullptr;
std::unordered_map<uint64_t, Kind> g_kinds;            // texture hash -> lamp kind
std::vector<Lamp> g_lamps;
bool g_on = true;
float g_onBelow = 6.0f, g_fade = 4.0f;
unsigned g_max = 24, g_frame = 0, g_created = 0;
uint64_t g_nextHash = 0x52425257'4C000000ULL;          // "RBRWL" + a counter: one Remix light per lamp for its life

constexpr size_t kMaxRects = 8;

float ini_f(const wchar_t* path, const wchar_t* sec, const wchar_t* key, float dflt) {
  wchar_t v[64] = {};
  GetPrivateProfileStringW(sec, key, L"", v, 64, path);
  return v[0] ? static_cast<float>(_wtof(v)) : dflt;
}

// Tanner Helland's fit of the Planckian locus (as headlights.cpp kelvin_rgb), linear.
void kelvin_rgb(float k, float out[3]) {
  const float t = std::clamp(k, 1000.0f, 40000.0f) / 100.0f;
  float r = t <= 66 ? 255 : 329.7f * std::pow(t - 60, -0.1332f);
  float g = t <= 66 ? 99.47f * std::log(t) - 161.1f : 288.1f * std::pow(t - 60, -0.0755f);
  float b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5f * std::log(t - 10) - 305.0f;
  const float c[3] = { r, g, b };
  for (int i = 0; i < 3; ++i) {
    const float s = std::clamp(c[i], 0.0f, 255.0f) / 255.0f;
    out[i] = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
  }
  const float m = (std::max)(out[0], (std::max)(out[1], out[2]));
  for (int i = 0; i < 3; ++i) out[i] /= m;
}

void destroy(Lamp& l) {
  if (l.h && g_api && g_api->DestroyLight) g_api->DestroyLight(l.h);
  l.h = nullptr;
  l.sentK = -1.0f;
}

// One file's [lamp.*] sections into g_kinds (a later file replaces an earlier one's kind for the same texture).
// dflt: [auto] values of lights.ini for the generated and the marked file's sections (they carry only texture and uv; a
// value in the section wins). marked: lights-marked.ini - anchor bottom, no pole test, sections of one texture merge.
size_t load_kinds(const wchar_t* path, const wchar_t* dfltPath, bool marked = false) {
  size_t count = 0;
  std::unordered_set<uint64_t> here;                   // textures set by this file
  std::vector<wchar_t> names(1 << 20);
  const DWORD n = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), path);
  for (const wchar_t* s = names.data(); s < names.data() + n && *s; s += wcslen(s) + 1) {
    if (_wcsnicmp(s, L"lamp.", 5)) continue;
    wchar_t v[512] = {};
    GetPrivateProfileStringW(s, L"texture", L"", v, 512, path);
    Kind k;
    k.name = s + 5;
    const wchar_t* d = dfltPath ? dfltPath : path;           // generated sections: values from lights.ini [auto]
    const wchar_t* ds = dfltPath ? L"auto" : s;
    wchar_t a[32] = {};
    auto num = [&](const wchar_t* key, float dflt) { return ini_f(path, s, key, ini_f(d, ds, key, dflt)); };   // section, else [auto]
    for (const wchar_t* key : { L"uv", L"uv2", L"uv3", L"uv4", L"uv5", L"uv6", L"uv7", L"uv8" }) {
      wchar_t u[96] = {};
      std::array<float, 4> r = {};
      GetPrivateProfileStringW(s, key, L"", u, 96, path);
      if (u[0] && swscanf_s(u, L"%f , %f , %f , %f", &r[0], &r[1], &r[2], &r[3]) == 4) k.uvs.push_back(r);
    }
    // anchor: the section's, else lights.ini [auto]'s, else bottom (marked) / centre (a uv kind) / top (a draw's sphere)
    GetPrivateProfileStringW(s, L"anchor", L"", a, 32, path);
    if (!a[0] && dfltPath) GetPrivateProfileStringW(ds, L"anchor", L"", a, 32, d);
    k.anchor = !a[0] ? (marked ? 2 : k.uvs.empty() ? 0 : 1)
             : !_wcsicmp(a, L"bottom") ? 2 : (!_wcsicmp(a, L"centre") || !_wcsicmp(a, L"center")) ? 1 : 0;
    GetPrivateProfileStringW(s, L"offset", L"", a, 32, path);
    if (!a[0]) GetPrivateProfileStringW(ds, L"offset", L"0, 0, 0", a, 32, d);
    swscanf_s(a, L"%f , %f , %f", &k.offset[0], &k.offset[1], &k.offset[2]);
    k.lumens = num(L"lumens", k.lumens);
    k.kelvin = num(L"colour_temperature", k.kelvin);
    k.radius = (std::max)(0.01f, num(L"radius", k.radius));
    k.volumetric = num(L"volumetric", k.volumetric);
    k.minHeight = num(L"min_height", k.minHeight);
    k.pole = !marked && ini_f(path, s, L"auto", 0.0f) != 0.0f;
    GetPrivateProfileStringW(s, L"where", L"any", a, 32, path);
    k.where = !_wcsicmp(a, L"menu") ? 1 : !_wcsicmp(a, L"stage") ? 2 : 0;
    // texture = one hash or several, comma separated
    for (wchar_t* ctx = nullptr, *t = wcstok_s(v, L", ", &ctx); t; t = wcstok_s(nullptr, L", ", &ctx)) {
      const uint64_t hx = _wcstoui64(t, nullptr, 16);
      if (!hx) continue;
      const auto old = g_kinds.find(hx);
      if (marked && here.count(hx) && old != g_kinds.end()) {   // another rectangle of the same lamp texture
        for (const auto& r : k.uvs) if (old->second.uvs.size() < kMaxRects) old->second.uvs.push_back(r);
      } else {
        g_kinds[hx] = k;
        here.insert(hx);
        ++count;
      }
    }
  }
  return count;
}

// lights.ini, and lights-auto.ini next to it (tools\lights\find_bulbs.py: bulbs found in stage textures, pole test on)
// unless [lights] auto = 0, then lights-marked.ini (made in game by the lamp_mark key). lights.ini is read last: its
// sections win for the same texture.
void load() {
  wchar_t path[MAX_PATH * 2];
  rbrrtx_config_path(L"RBRRTX_LIGHTS", L"lights.ini", path, MAX_PATH * 2);
  g_on = ini_f(path, L"lights", L"enabled", 1.0f) != 0.0f;
  g_onBelow = ini_f(path, L"lights", L"on_below_sun", 6.0f);
  g_fade = (std::max)(0.1f, ini_f(path, L"lights", L"fade_degrees", 4.0f));
  g_max = static_cast<unsigned>((std::max)(0.0f, ini_f(path, L"lights", L"max_lights", 24.0f)));
  size_t autoKinds = 0;
  std::wstring autoPath = path;
  autoPath = autoPath.substr(0, autoPath.find_last_of(L'\\') + 1) + L"lights-auto.ini";
  if (ini_f(path, L"lights", L"auto", 1.0f) != 0.0f && GetFileAttributesW(autoPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    autoKinds = load_kinds(autoPath.c_str(), path);
  wchar_t markedPath[MAX_PATH * 2];
  rbrrtx_lights_marked_path(markedPath, MAX_PATH * 2, false);
  size_t markedKinds = 0;
  if (GetFileAttributesW(markedPath) != INVALID_FILE_ATTRIBUTES) markedKinds = load_kinds(markedPath, path, true);
  const size_t own = load_kinds(path, nullptr);
  rbrrtx_log_to(L"rbrrtx_comp.log", L"world lights (%s): %s, %zu lamp texture(s) (%zu own, %zu found in textures, %zu marked in game), "
                L"on below sun %.1f deg (fade %.1f), at most %u", path, g_on ? L"on" : L"OFF", g_kinds.size(), own, autoKinds,
                markedKinds, g_onBelow, g_fade, g_max);
}

}  // namespace

void rbrrtx_world_lights_init(const remixapi_Interface* api) {
  g_api = api;
  load();
}

bool rbrrtx_world_lights_wanted() { return g_api && g_on && !g_kinds.empty(); }
bool rbrrtx_world_lights_texture(unsigned long long hash) { return g_kinds.count(hash) != 0; }

int rbrrtx_world_lights_uvs(unsigned long long hash, float rects[][4], int max, float* poleMinHeight, int* anchor) {
  const auto it = g_kinds.find(hash);
  if (it == g_kinds.end()) return 0;
  const int n = (std::min)(max, static_cast<int>(it->second.uvs.size()));
  for (int i = 0; i < n; ++i) std::copy(it->second.uvs[i].begin(), it->second.uvs[i].end(), rects[i]);
  *poleMinHeight = it->second.pole ? it->second.minHeight : 0.0f;
  *anchor = it->second.anchor;
  return n;
}

// A draw with a lamp texture, its bounding sphere in world space.
void rbrrtx_world_lights_seen(unsigned long long hash, const float c[3], float r, bool inMenu) {
  const auto it = g_kinds.find(hash);
  if (it == g_kinds.end()) return;
  const Kind& k = it->second;
  if ((k.where == 1 && !inMenu) || (k.where == 2 && inMenu)) return;
  const float p[3] = { c[0] + k.offset[0], c[1] + (k.anchor == 0 ? r : k.anchor == 2 ? -r : 0.0f) + k.offset[1], c[2] + k.offset[2] };
  for (Lamp& l : g_lamps) {
    const float dx = l.pos[0] - p[0], dy = l.pos[1] - p[1], dz = l.pos[2] - p[2];
    if (dx * dx + dy * dy + dz * dz < 1.0f) { l.seen = g_frame; return; }
  }
  if (g_lamps.size() >= 512) return;
  Lamp l;
  l.hash = g_nextHash++;
  l.kind = &k;
  std::copy(p, p + 3, l.pos);
  l.seen = g_frame;
  g_lamps.push_back(l);
  if (g_lamps.size() <= 8)
    rbrrtx_log_to(L"rbrrtx_comp.log", L"world lights: lamp %s (texture %016llX) at (%.2f %.2f %.2f)", k.name.c_str(),
                  static_cast<unsigned long long>(hash), p[0], p[1], p[2]);
}

// Per Present: draws the lamps seen this frame (nearest first), destroys the rest. cam = camera position.
void rbrrtx_world_lights_frame(float sunElevation, const float cam[3], bool layerOn) {
  const float k = std::clamp((g_onBelow - sunElevation) / g_fade, 0.0f, 1.0f);
  std::vector<Lamp*> lit;
  for (Lamp& l : g_lamps)
    if (l.seen == g_frame && k > 0.0f && layerOn && g_on) lit.push_back(&l);
    else if (l.h) destroy(l);
  auto d2 = [&](const Lamp* l) {
    const float dx = l->pos[0] - cam[0], dy = l->pos[1] - cam[1], dz = l->pos[2] - cam[2];
    return dx * dx + dy * dy + dz * dz;
  };
  if (lit.size() > g_max) {
    std::nth_element(lit.begin(), lit.begin() + g_max, lit.end(), [&](const Lamp* a, const Lamp* b) { return d2(a) < d2(b); });
    for (size_t i = g_max; i < lit.size(); ++i) if (lit[i]->h) destroy(*lit[i]);
    lit.resize(g_max);
  }
  // Lumens -> the sphere light's radiance: an even sphere sends I = lumens / 4 pi candela every way, and a sphere
  // light's intensity is radiance x pi r^2 (headlights.cpp). Under NUMOS one unit of irradiance is unitLux lux; Remix
  // 1.5.2 has no such scale, there the headlights' default lamp (60,000 cd = radiance 1192 at r 0.05: ~6,400 lux per
  // unit) stands in.
  float unitLux = rbrrtx_numos_lux_per_unit();
  if (unitLux <= 0.0f) unitLux = 6400.0f;
  for (Lamp* l : lit) {
    const Kind& kd = *l->kind;
    const bool moved = !l->h || l->sent[0] != l->pos[0] || l->sent[1] != l->pos[1] || l->sent[2] != l->pos[2] ||
                       std::fabs(k - l->sentK) > 0.01f;
    if (moved) {
      if (!rbrrtx_remix_plus()) destroy(*l);           // 1.5.2: a re-create needs the old handle gone (headlights.cpp)
      float rgb[3];
      kelvin_rgb(kd.kelvin, rgb);
      const float rad = k * kd.lumens / (4.0f * 3.14159265f) / (unitLux * 3.14159265f * kd.radius * kd.radius);
      remixapi_LightInfoSphereEXT sphere = {};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = { l->pos[0], l->pos[1], l->pos[2] };
      sphere.radius = kd.radius;
      sphere.shaping_hasvalue = false;
      sphere.volumetricRadianceScale = kd.volumetric;
      remixapi_LightInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      info.pNext = &sphere;
      info.hash = l->hash;
      info.radiance = { rgb[0] * rad, rgb[1] * rad, rgb[2] * rad };
      const remixapi_LightHandle old = l->h;
      if (g_api->CreateLight(&info, &l->h) != REMIXAPI_ERROR_CODE_SUCCESS) {
        l->h = old;
        destroy(*l);
        continue;
      }
      if (++g_created <= 8)
        rbrrtx_log_to(L"rbrrtx_comp.log", L"world lights: %s lit at (%.2f %.2f %.2f), %.0f lm x %.2f, radiance %.0f",
                      kd.name.c_str(), l->pos[0], l->pos[1], l->pos[2], kd.lumens, k, rad);
      std::copy(l->pos, l->pos + 3, l->sent);
      l->sentK = k;
    }
    g_api->DrawLightInstance(l->h);
  }
  ++g_frame;
}

// A new stage or the menu: the lamps of the last scene go (positions are per scene).
void rbrrtx_world_lights_reset() {
  for (Lamp& l : g_lamps) destroy(l);
  g_lamps.clear();
}

// The lamp kinds read again (after the lamp_mark key wrote lights-marked.ini): the lamps go (they point at their kind) and
// are found again from the next draws.
void rbrrtx_world_lights_reload() {
  rbrrtx_world_lights_reset();
  g_kinds.clear();
  load();
}
