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
//
// Glowing glass (user 2026-11-02: a closed street lamp is one full mesh, e.g. Hondarribia 30AD069FFA15A009; its glass should
// glow too): a kind with glow > 0 (section key glow, emissive intensity; new marks get lights.ini [lights] mark_glow) draws
// with a twin texture while the lamps are on. First time the texture is drawn its mip chain is saved from the game's memory
// to <mods>\rbr_rtx_lit\marked\<hash>.dds (stages are encrypted: no file to point at), with a 256x256 mask that is white on
// the marked uv rectangles. The generated mod (genmod.cpp, section "marked") gives the twin's hash (one per on-level, the
// fade of the lamps in kGlowLevels steps) a material: the saved texture as albedo, the mask as emissive mask, the lamp colour
// as emissive colour. Later runs publish the section before Remix starts. Day / lamps off: the real texture is drawn.
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <d3d9.h>
#include "third_party/remix_c.h"
#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
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
  float glow = 0.0f;                                   // emissive intensity of the marked glass while lit (0 = none)
  // Spot kind (user 2026-11-05: stadium floodlights made emissive; every mesh with the texture also casts a cone).
  float cone = 0.0f;                                   // half angle in degrees, 0 = a plain omni lamp
  float softness = 0.2f, focus = 0.0f;
  float dir[3] = { 0.0f, -1.0f, 0.0f };                // world direction (normalised), used when there is no aim point
  bool aim = false;
  float aimAt[3] = {};                                 // aim = x, y, z: every lamp of the kind points at this world point
  bool alwaysOn = false;                               // always_on = 1: lit whatever the sun (stadium lights in a night race)
  bool hasRgb = false;                                 // colour given directly (F3 emissive props) instead of a temperature
  float rgb[3] = { 1.0f, 1.0f, 1.0f };
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
std::map<int, std::pair<uint64_t, Kind>> g_custom;     // F3 emissives that cast light, by emissive id (custom.cpp); merged last, they win
std::unordered_map<uint64_t, std::vector<Kind>> g_more; // the second and later F3 emissives of a texture (kind 1, 2, ..): the first is in g_kinds
std::vector<Lamp> g_lamps;
bool g_on = true;
float g_onBelow = 6.0f, g_fade = 4.0f;
unsigned g_max = 24, g_frame = 0, g_created = 0;
uint64_t g_nextHash = 0x52425257'4C000000ULL;          // "RBRWL" + a counter: one Remix light per lamp for its life

constexpr size_t kMaxRects = 8;
constexpr int kGlowLevels = 4;                         // lit-fraction steps of the glass (the lamps fade in over a few minutes)
constexpr ULONGLONG kGlowSettle = 9000;                // ms a new material needs (mod written, Remix reload) before its twin is drawn
float g_markGlow = 6.0f;                               // lights.ini [lights] mark_glow: glow written into a new mark
float g_litFraction = 0.0f;                            // lamps on 0..1 as of the last frame

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
    if (!a[0]) GetPrivateProfileStringW(ds, L"offset", marked ? L"0, -0.1, 0" : L"0, 0, 0", a, 32, d);   // marked: just below the glass
    swscanf_s(a, L"%f , %f , %f", &k.offset[0], &k.offset[1], &k.offset[2]);
    k.lumens = num(L"lumens", k.lumens);
    k.kelvin = num(L"colour_temperature", k.kelvin);
    k.radius = (std::max)(0.01f, num(L"radius", k.radius));
    k.volumetric = num(L"volumetric", k.volumetric);
    k.minHeight = num(L"min_height", k.minHeight);
    k.glow = (std::max)(0.0f, num(L"glow", 0.0f));
    k.pole = !marked && ini_f(path, s, L"auto", 0.0f) != 0.0f;
    k.cone = std::clamp(num(L"cone", 0.0f), 0.0f, 179.0f);
    k.softness = std::clamp(num(L"cone_softness", k.softness), 0.0f, 1.0f);
    k.focus = (std::max)(0.0f, num(L"focus", k.focus));
    k.alwaysOn = num(L"always_on", 0.0f) != 0.0f;
    wchar_t vec[96] = {};
    GetPrivateProfileStringW(s, L"direction", L"", vec, 96, path);
    float dv[3] = {};
    if (vec[0] && swscanf_s(vec, L"%f , %f , %f", &dv[0], &dv[1], &dv[2]) == 3) {
      const float len = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
      if (len > 1e-4f) for (int i = 0; i < 3; ++i) k.dir[i] = dv[i] / len;
    }
    vec[0] = 0;
    GetPrivateProfileStringW(s, L"aim", L"", vec, 96, path);
    k.aim = vec[0] && swscanf_s(vec, L"%f , %f , %f", &k.aimAt[0], &k.aimAt[1], &k.aimAt[2]) == 3;
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

// ---- glowing glass of marked lamps (see the header)
struct Glow {
  bool tried = false, ok = false;                      // saved / failed once
  std::wstring dds, mask;                              // files in <genmod dir>\marked
  ULONGLONG readyAt = 0;                               // twin drawn from this tick on
  IDirect3DTexture9* twin[kGlowLevels] = {};
  int lvHeld = 0;                                      // level in use: up at once, down only after kGlowHold ms of asking for less
  ULONGLONG lowerSince = 0;
};
constexpr ULONGLONG kGlowHold = 1000;
// Counters for the 300-frame log line (user 2026-11-02: the marked glass flapped after the debug sun moved; no way to tell why).
unsigned g_glowDraws = 0, g_glowLevelUp = 0, g_glowLevelDown = 0, g_createNew = 0, g_createMoved = 0, g_createLevel = 0, g_createFailed = 0,
         g_destroyedUnseen = 0;
std::unordered_map<uint64_t, Glow> g_glow;

std::wstring glow_dir() {
  const wchar_t* d = rbrrtx_genmod_dir();
  return d && d[0] ? std::wstring(d) + L"\\marked" : std::wstring();
}

std::wstring hex16(uint64_t h) {
  wchar_t b[24];
  swprintf_s(b, L"%016llX", static_cast<unsigned long long>(h));
  return b;
}

// The twin's mip 0 (16x16 A8R8G8B8) from the texture hash and the level: its Remix hash is known before Remix sees it.
uint64_t glow_twin_pixels(uint64_t src, int level, uint32_t px[256]) {
  uint64_t st = src ^ (0x5242524C'414D5000ULL + static_cast<uint64_t>(level) * 0x1000193ULL);   // "RBRLAMP"
  for (int i = 0; i < 256; ++i) {
    uint64_t z = (st += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    px[i] = static_cast<uint32_t>(z ^ (z >> 31)) | 0xFF000000u;
  }
  return rbrrtx_xxh3(px, 256 * sizeof(uint32_t));
}

// 256x256 A8R8G8B8 mask, white where the uv falls in one of the kind's rectangles (the atlas cell wraps). Named by its
// content, so a changed mark is a new asset (Remix does not reload a file it has open). Returns the path ("" = failed).
std::wstring glow_write_mask(uint64_t hash, const Kind& k) {
  const std::wstring dir = glow_dir();
  if (dir.empty() || k.uvs.empty()) return std::wstring();
  uint32_t crc = 2166136261u;                          // FNV-1a over the rectangles in 1/1024 steps
  for (const auto& r : k.uvs)
    for (float f : r) { crc ^= static_cast<uint32_t>(std::lround(f * 1024.0f)); crc *= 16777619u; }
  // The lamp's colour is in the mask: with a mask set, Remix ignores a USD material's emissive_color_constant.
  float rgb[3];
  kelvin_rgb(k.kelvin, rgb);
  auto byte = [](float c) { return static_cast<uint32_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); };
  const uint32_t lit = 0xFF000000u | (byte(rgb[0]) << 16) | (byte(rgb[1]) << 8) | byte(rgb[2]);
  crc ^= lit; crc *= 16777619u;
  wchar_t tail[40];
  swprintf_s(tail, L"_mask_%08X.dds", crc);
  const std::wstring path = dir + L"\\" + hex16(hash) + tail;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
  constexpr int N = 256;
  std::vector<uint32_t> px(N * N, 0xFF000000u);
  auto inside = [](float t, float a, float b) {
    if (a > b) std::swap(a, b);
    for (int w = -2; w <= 3; ++w) if (t + w >= a && t + w <= b) return true;
    return false;
  };
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      const float u = (x + 0.5f) / N, v = (y + 0.5f) / N;
      for (const auto& r : k.uvs)
        if (inside(u, r[0], r[2]) && inside(v, r[1], r[3])) { px[y * N + x] = lit; break; }
    }
  DWORD hdr[32] = {};
  hdr[0] = 0x20534444; hdr[1] = 124;
  hdr[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;            // caps | height | width | pitch | pixelformat
  hdr[3] = N; hdr[4] = N; hdr[5] = N * 4; hdr[7] = 1;
  hdr[19] = 32; hdr[20] = 0x41; hdr[22] = 32;         // RGB | ALPHAPIXELS, 32 bit
  hdr[23] = 0xFF0000; hdr[24] = 0xFF00; hdr[25] = 0xFF; hdr[26] = 0xFF000000u;
  hdr[27] = 0x1000;
  FILE* fp = nullptr;
  if (_wfopen_s(&fp, path.c_str(), L"wb") || !fp) return std::wstring();
  fwrite(hdr, sizeof(hdr), 1, fp);
  fwrite(px.data(), 4, px.size(), fp);
  fclose(fp);
  return path;
}

// Section "marked" from every glowing kind whose texture is saved: one material per on-level.
void glow_publish() {
  std::string body;
  for (const auto& kv : g_kinds) {
    const Kind& kd = kv.second;
    const auto g = g_glow.find(kv.first);
    if (kd.glow <= 0.0f || g == g_glow.end() || !g->second.ok) continue;
    float rgb[3];
    kelvin_rgb(kd.kelvin, rgb);
    const std::string dds = rbrrtx_genmod_asset(g->second.dds), mask = rbrrtx_genmod_asset(g->second.mask);
    for (int lv = 1; lv <= kGlowLevels; ++lv) {
      uint32_t px[256];
      const uint64_t th = glow_twin_pixels(kv.first, lv, px);
      char num[160];
      snprintf(num, sizeof(num), "                color3f inputs:emissive_color_constant = (%.3f, %.3f, %.3f)\n"
                                 "                float inputs:emissive_intensity = %.3f\n", rgb[0], rgb[1], rgb[2], kd.glow * lv / kGlowLevels);
      const std::string in =
          "                asset inputs:diffuse_texture = @" + dds + "@ (\n"
          "                    colorSpace = \"auto\"\n"
          "                )\n"
          "                bool inputs:enable_emission = 1\n"
          "                asset inputs:emissive_mask_texture = @" + mask + "@ (\n"
          "                    colorSpace = \"auto\"\n"
          "                )\n" + num;
      char c[96];
      snprintf(c, sizeof(c), "lamp glass glow, texture %016llX, lit %d/%d:", static_cast<unsigned long long>(kv.first), lv, kGlowLevels);
      body += rbrrtx_genmod_material(th, std::string(c) + " " + dds, in);
    }
  }
  rbrrtx_genmod_set("marked", body);
}

// After the kinds were (re)read: masks of saved textures follow the marks; the section is rewritten.
void glow_refresh() {
  for (auto& kv : g_glow) {
    const auto it = g_kinds.find(kv.first);
    if (!kv.second.ok || it == g_kinds.end() || it->second.glow <= 0.0f) continue;
    const std::wstring m = glow_write_mask(kv.first, it->second);
    if (!m.empty() && m != kv.second.mask) {
      kv.second.mask = m;
      kv.second.readyAt = GetTickCount64() + kGlowSettle;
    }
  }
  glow_publish();
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
  g_markGlow = (std::max)(0.0f, ini_f(path, L"lights", L"mark_glow", 6.0f));
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
  g_more.clear();
  {
    std::unordered_map<uint64_t, bool> placed;
    for (const auto& kv : g_custom) {
      if (placed.emplace(kv.second.first, true).second) g_kinds[kv.second.first] = kv.second.second;
      else g_more[kv.second.first].push_back(kv.second.second);
    }
  }
  glow_refresh();
  rbrrtx_log_to(L"rbrrtx_comp.log", L"world lights (%s): %s, %zu lamp texture(s) (%zu own, %zu found in textures, %zu marked in game), "
                L"on below sun %.1f deg (fade %.1f), at most %u", path, g_on ? L"on" : L"OFF", g_kinds.size(), own, autoKinds,
                markedKinds, g_onBelow, g_fade, g_max);
  size_t glowing = 0;
  for (const auto& kv : g_kinds) glowing += kv.second.glow > 0.0f;
  if (glowing) rbrrtx_log_to(L"rbrrtx_comp.log", L"world lights: %zu lamp texture(s) with glowing glass (new marks get glow %.1f)", glowing, g_markGlow);
}

}  // namespace

void rbrrtx_world_lights_init(const remixapi_Interface* api) {
  g_api = api;
  load();
}

// Before the backend loads (after rbrrtx_genmod_init): the glowing lamp textures saved by earlier runs get their materials
// now, so Remix has them from the first frame.
void rbrrtx_world_lights_startup() {
  load();
  for (const auto& kv : g_kinds) {
    if (kv.second.glow <= 0.0f) continue;
    Glow& g = g_glow[kv.first];
    g.dds = glow_dir() + L"\\" + hex16(kv.first) + L".dds";
    g.mask = glow_write_mask(kv.first, kv.second);
    g.tried = true;
    g.ok = !g.mask.empty() && GetFileAttributesW(g.dds.c_str()) != INVALID_FILE_ATTRIBUTES;
  }
  glow_publish();
}

float rbrrtx_world_lights_mark_glow() { return g_markGlow; }

bool rbrrtx_world_lights_wanted() { return g_api && g_on && !g_kinds.empty(); }
bool rbrrtx_world_lights_texture(unsigned long long hash) { return g_kinds.count(hash) != 0; }

int rbrrtx_world_lights_kinds(unsigned long long hash) {
  if (!g_kinds.count(hash)) return 0;
  const auto m = g_more.find(hash);
  return 1 + (m == g_more.end() ? 0 : static_cast<int>(m->second.size()));
}

namespace {
const Kind* kind_at(uint64_t hash, int kind) {
  if (kind == 0) {
    const auto it = g_kinds.find(hash);
    return it == g_kinds.end() ? nullptr : &it->second;
  }
  const auto m = g_more.find(hash);
  return m == g_more.end() || kind < 1 || kind > static_cast<int>(m->second.size()) ? nullptr : &m->second[kind - 1];
}
}  // namespace

int rbrrtx_world_lights_uvs(unsigned long long hash, int kind, float rects[][4], int max, float* poleMinHeight, int* anchor) {
  const Kind* kp = kind_at(hash, kind);
  if (!kp) return 0;
  const int n = (std::min)(max, static_cast<int>(kp->uvs.size()));
  for (int i = 0; i < n; ++i) std::copy(kp->uvs[i].begin(), kp->uvs[i].end(), rects[i]);
  *poleMinHeight = kp->pole ? kp->minHeight : 0.0f;
  *anchor = kp->anchor;
  return n;
}

// A draw with a glowing lamp texture while the lamps are on: its twin for the lit level (see the header).
void* rbrrtx_world_lights_glow(void* device, unsigned long long hash, void* baseTexture, bool inMenu) {
  const auto it = g_kinds.find(hash);
  if (it == g_kinds.end() || it->second.glow <= 0.0f || !g_on || g_litFraction <= 0.02f) return nullptr;
  const Kind& k = it->second;
  if ((k.where == 1 && !inMenu) || (k.where == 2 && inMenu)) return nullptr;
  Glow& g = g_glow[hash];
  if (!g.tried) {                                      // first sight in this install: save the texture, make the mask
    g.tried = true;
    const std::wstring dir = glow_dir();
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    g.dds = dir + L"\\" + hex16(hash) + L".dds";
    const bool have = rbrrtx_dds_is_texture(g.dds.c_str(), hash);
    if (!dir.empty() && (have || rbrrtx_save_texture_dds_mips(baseTexture, g.dds.c_str()))) {
      g.mask = glow_write_mask(hash, k);
      g.ok = !g.mask.empty();
    }
    g.readyAt = GetTickCount64() + kGlowSettle;
    rbrrtx_log_to(L"rbrrtx_comp.log", L"world lights: glass glow of %s (texture %016llX): %s %s%s", k.name.c_str(),
                  static_cast<unsigned long long>(hash), g.ok ? L"saved" : L"FAILED to save (not lockable or no folder)", g.dds.c_str(),
                  g.ok ? L"; the glass glows once the generated mod has reloaded" : L"");
    if (g.ok) glow_publish();
  }
  if (!g.ok || GetTickCount64() < g.readyAt) return nullptr;
  const int want = std::clamp(static_cast<int>(std::ceil(g_litFraction * kGlowLevels - 0.001f)), 1, kGlowLevels);
  const ULONGLONG nowTick = GetTickCount64();
  if (g.lvHeld == 0 || want > g.lvHeld) { if (g.lvHeld) ++g_glowLevelUp; g.lvHeld = want; g.lowerSince = 0; }
  else if (want < g.lvHeld) {                          // hysteresis: a sun sweeping through dusk must not change the material every frame
    if (!g.lowerSince) g.lowerSince = nowTick;
    else if (nowTick - g.lowerSince >= kGlowHold) { g.lvHeld = want; g.lowerSince = 0; ++g_glowLevelDown; }
  } else {
    g.lowerSince = 0;
  }
  const int lv = g.lvHeld;
  IDirect3DTexture9*& t = g.twin[lv - 1];
  if (!t) {
    IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
    uint32_t px[256];
    glow_twin_pixels(hash, lv, px);
    if (SUCCEEDED(dev->lpVtbl->CreateTexture(dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) && t) {
      D3DLOCKED_RECT lr = {};
      if (SUCCEEDED(t->lpVtbl->LockRect(t, 0, &lr, nullptr, 0)) && lr.pBits) {
        for (int r = 0; r < 16; ++r) std::memcpy(static_cast<char*>(lr.pBits) + r * lr.Pitch, px + r * 16, 64);
        t->lpVtbl->UnlockRect(t, 0);
      } else {
        t->lpVtbl->Release(t);
        t = nullptr;
      }
    }
  }
  if (t) ++g_glowDraws;
  return t;
}

// A draw with a lamp texture, its bounding sphere in world space.
void rbrrtx_world_lights_seen(unsigned long long hash, int kind, const float c[3], float r, bool inMenu) {
  const Kind* kp = kind_at(hash, kind);
  if (!kp) return;
  const Kind& k = *kp;
  if ((k.where == 1 && !inMenu) || (k.where == 2 && inMenu)) return;
  const float p[3] = { c[0] + k.offset[0], c[1] + (k.anchor == 0 ? r : k.anchor == 2 ? -r : 0.0f) + k.offset[1], c[2] + k.offset[2] };
  for (Lamp& l : g_lamps) {
    const float dx = l.pos[0] - p[0], dy = l.pos[1] - p[1], dz = l.pos[2] - p[2];
    if (l.kind == &k && dx * dx + dy * dy + dz * dz < 1.0f) { l.seen = g_frame; return; }   // two emissives of one texture can share a place
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
  g_litFraction = layerOn && g_on ? k : 0.0f;
  std::vector<Lamp*> lit;
  for (Lamp& l : g_lamps)
    if (l.seen == g_frame && (k > 0.0f || l.kind->alwaysOn) && layerOn && g_on) lit.push_back(&l);
    else if (l.h) { destroy(l); ++g_destroyedUnseen; }
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
    const float lv = kd.alwaysOn ? 1.0f : k;
    const bool isNew = !l->h;
    const bool posMoved = l->sent[0] != l->pos[0] || l->sent[1] != l->pos[1] || l->sent[2] != l->pos[2];
    const bool levelMoved = std::fabs(lv - l->sentK) > 0.05f;   // 5 % steps: a sweeping sun is not a re-create per frame
    const bool moved = isNew || posMoved || levelMoved;
    if (moved) {
      if (isNew) ++g_createNew; else if (posMoved) ++g_createMoved; else ++g_createLevel;
      if (!rbrrtx_remix_plus()) destroy(*l);           // 1.5.2: a re-create needs the old handle gone (headlights.cpp)
      float rgb[3];
      kelvin_rgb(kd.kelvin, rgb);
      if (kd.hasRgb) {
        const float m = (std::max)(kd.rgb[0], (std::max)(kd.rgb[1], kd.rgb[2]));
        for (int i = 0; i < 3; ++i) rgb[i] = m > 0.0f ? kd.rgb[i] / m : 1.0f;
      }
      // A spot kind's lumens leave through its cone (solid angle 2 pi (1 - cos half angle)), not the whole sphere:
      // Remix's shaping only masks a light (headlights.cpp), so the intensity is raised here.
      const float coneRad = kd.cone * 3.14159265f / 180.0f;
      const float solid = kd.cone > 0.0f ? 2.0f * 3.14159265f * (1.0f - std::cos(coneRad)) : 4.0f * 3.14159265f;
      const float rad = lv * kd.lumens / solid / (unitLux * 3.14159265f * kd.radius * kd.radius);
      remixapi_LightInfoSphereEXT sphere = {};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = { l->pos[0], l->pos[1], l->pos[2] };
      sphere.radius = kd.radius;
      sphere.shaping_hasvalue = kd.cone > 0.0f;
      if (kd.cone > 0.0f) {
        float d[3] = { kd.dir[0], kd.dir[1], kd.dir[2] };
        if (kd.aim) {
          const float v[3] = { kd.aimAt[0] - l->pos[0], kd.aimAt[1] - l->pos[1], kd.aimAt[2] - l->pos[2] };
          const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
          if (len > 1e-3f) for (int i = 0; i < 3; ++i) d[i] = v[i] / len;
        }
        sphere.shaping_value.direction = { d[0], d[1], d[2] };
        sphere.shaping_value.coneAngleDegrees = kd.cone;
        sphere.shaping_value.coneSoftness = kd.softness;
        sphere.shaping_value.focusExponent = kd.focus;
      }
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
        ++g_createFailed;
        continue;
      }
      if (++g_created <= 8)
        rbrrtx_log_to(L"rbrrtx_comp.log", L"world lights: %s lit at (%.2f %.2f %.2f), %.0f lm x %.2f, radiance %.0f",
                      kd.name.c_str(), l->pos[0], l->pos[1], l->pos[2], kd.lumens, lv, rad);
      std::copy(l->pos, l->pos + 3, l->sent);
      l->sentK = lv;
    }
    g_api->DrawLightInstance(l->h);
  }
  if (g_frame % 300 == 299) {                          // why the lights / glass change: counted since the last line
    rbrrtx_log_to(L"rbrrtx_comp.log", L"world lights: %zu lamps known, %zu lit now (lit fraction %.2f, level steps up %u / down %u, glass twin draws %u); "
                  L"CreateLight since last line: %u new, %u moved, %u level change, %u failed; %u destroyed (not seen / dark)",
                  g_lamps.size(), lit.size(), g_litFraction, g_glowLevelUp, g_glowLevelDown, g_glowDraws, g_createNew, g_createMoved, g_createLevel,
                  g_createFailed, g_destroyedUnseen);
    g_glowLevelUp = g_glowLevelDown = g_glowDraws = g_createNew = g_createMoved = g_createLevel = g_createFailed = g_destroyedUnseen = 0;
  }
  ++g_frame;
}

// How lit the lamps are at a sun height (lights.ini on_below_sun / fade_degrees), 0 when the lamps are switched off there.
float rbrrtx_world_lights_night_fraction(float sunElevation) {
  return std::clamp((g_onBelow - sunElevation) / g_fade, 0.0f, 1.0f);
}

// A new stage or the menu: the lamps of the last scene go (positions are per scene).
void rbrrtx_world_lights_reset() {
  for (Lamp& l : g_lamps) destroy(l);
  g_lamps.clear();
}

// The lamp kinds read again (after the lamp_mark key wrote lights-marked.ini): the lamps go (they point at their kind) and
// are found again from the next draws.
void rbrrtx_world_lights_set_emit(int id, unsigned long long tex, const float rgb[3], float lumens, float cone, float softness,
                                  const float dir[3], int aim, const float aimAt[3], int always, const float offset[3],
                                  const float (*rects)[4], int nrects, int anchor) {
  if (lumens <= 0.0f) {
    if (!g_custom.erase(id)) return;
  } else {
    Kind k;
    wchar_t n[48];
    swprintf_s(n, L"emissive_%d_%016llX", id, tex);
    k.name = n;
    k.anchor = 1;
    k.lumens = lumens;
    k.radius = 0.25f;
    k.hasRgb = true;
    std::copy(rgb, rgb + 3, k.rgb);
    k.cone = std::clamp(cone, 0.0f, 179.0f);
    k.softness = std::clamp(softness, 0.0f, 1.0f);
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len > 1e-4f) for (int i = 0; i < 3; ++i) k.dir[i] = dir[i] / len;
    k.aim = aim != 0;
    std::copy(aimAt, aimAt + 3, k.aimAt);
    k.alwaysOn = always != 0;
    if (offset) std::copy(offset, offset + 3, k.offset);
    // Rectangles of the texture (the part the user picked): lamps at the vertex groups drawn with them (comp.cpp
    // uv_lamp_points), at their centre. None = the whole texture: one lamp at each draw's centre, as before.
    for (int i = 0; rects && i < nrects && k.uvs.size() < kMaxRects; ++i)
      k.uvs.push_back({ rects[i][0], rects[i][1], rects[i][2], rects[i][3] });
    if (!k.uvs.empty()) k.anchor = std::clamp(anchor, 0, 2);   // top of a lamppost part = its head
    g_custom[id] = { tex, k };
  }
  rbrrtx_world_lights_reload();
}

void rbrrtx_world_lights_reload() {
  rbrrtx_world_lights_reset();
  g_kinds.clear();
  load();
}
