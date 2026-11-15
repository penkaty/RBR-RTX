// foliage.cpp - light through the leaves (user 2026-10-30: trees drawn as crossed alpha cards "look unnatural").
//
// Remix has a foliage model of its own: "thin opaque" materials (dxvk-remix documentation/FoliageSystem.md; any opaque
// material with subsurface_measurement_distance > 0, rtx_scene_manager.cpp, rtx.subsurface.enableThinOpaque, default
// on in 1.5.2 and Remix Plus): single scattering through a thin surface, so a card lit from behind glows in its own
// colour instead of showing its dark back, and a sunlit tree gets soft translucency across its clumps. RBR's cards
// are flat, normal-less alpha cards (docs/research/foliage-normals.md), and this is what they miss most under path
// tracing; radial normals (radnormals.cpp, foliage-normals.ini) only smooth the shading.
// Every texture a swaying foliage draw uses (SH_SWAY: RL*Sway trees, bushes, grass) gets a replacement material in
// the generated mod (genmod.cpp, section "foliage") that sets only the subsurface inputs: Remix merges the game's
// texture in as albedo and alpha (rtx_materials.h mergeLegacyMaterial), and the draw's alpha test stays (legacy alpha
// state). The light through a leaf takes the texture's average colour (autumn leaves pass orange, firs green).
// That colour goes in subsurface_single_scattering_albedo, NOT in the transmittance colour: Remix's thin opaque
// shading (brdf.slangh evalHanrahanSingleScatteringDiffuseTransmission) scatters each channel in proportion to its
// attenuation, -log(transmittance), so a green transmittance (0.43 0.85 0.21, first build) attenuates green least,
// scatters it least and lit the backs of the trees purple (user, Peklo replay 2026-10-30). The transmittance is one
// neutral grey for all: the same attenuation in every channel, the colour comes from the albedo.
// Hashes seen are kept in <mods>\rbr_rtx_lit\foliage.txt: written before Remix opens the mod at the next start, so
// known stages need no reload; new ones are written once no new one came for kQuiet seconds (one reload per new stage).
// Settings: materials.ini [foliage].
#define CINTERFACE
#include "common.hpp"
#include <d3d9.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdio>

namespace {

#define flog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

constexpr double kQuiet = 5.0;

struct Cfg {
  bool enabled = true;
  // RBR-RTX runtime (RBR-RTX-remixplus, CardNormals port): every swaying foliage texture seen goes to
  // rtx.cardNormalsTextures, so cards get bent normals on every stage, encrypted ones included (user 2026-11-01:
  // CardNormals before 3D replacement). Stock runtimes refuse the option (logged as refused): harmless.
  bool cardNormals = true;
  float thickness = 0.5f;                                // subsurface_measurement_distance, mm (Remix: 0..16)
  float transmittance[3] = { 0.5f, 0.5f, 0.5f };         // subsurface_transmittance_color: keep it grey
  float brightest = 0.8f;                                // scattering albedo = texture colour, brightest channel this
  float albedo[3] = { 0.6f, 0.75f, 0.45f };              // scattering albedo when the texture's colour is unknown
  float anisotropy = 0.3f;                               // forward scattering
  // Leaves are matte. Without a value the material takes Remix's default 0.5: glossy cards that catch every light and
  // reflection and come out brighter than the game drew them (user 2026-11-13).
  float roughness = 1.0f;
} g_cfg;

// cut: the texture has cut-out alpha (1), is solid (0, never a card: user 2026-11-13, card normals on cars and walls
// shaded them wrong), or not judged yet (-1, still a card as before).
struct Leaf { float t[3]; int cut = -1; };               // the texture's colour, brightest channel 1 (0 0 0: unknown)
std::map<uint64_t, Leaf> g_leaves;                       // texture hash -> colour
std::unordered_map<void*, int> g_seen;                   // texture -> tries (>= 0 done once hashed)
bool g_dirty = false;
std::chrono::steady_clock::time_point g_lastNew;
std::wstring g_cacheFile;
unsigned g_newThisRun = 0, g_unhashed = 0;
size_t g_cardNormalsSent = 0;                            // textures in the last rtx.cardNormalsTextures sent
size_t g_cardsAsked = 0;                                 // catalog cards + leaves when the list was last built
bool g_cardsDirty = false;                               // a leaf was judged solid / cut-out since
// The user's own edits in the live rtx.conf say "not a card": a card-normals removal (-0x..) or the texture put in
// smooth normals, decals, UI, ignore, terrain or the player model (user 2026-11-13: "learn from my edits"; ten cards
// taken off and set to smooth normals at 20:37-21:05).
std::unordered_set<uint64_t> g_userNotCard;
constexpr int kCutAlpha = 128;                           // lowest alpha below this: the texture has cut-out parts
// What the sway shader draws is not all foliage (2026-11-01, runs 20261101-102516/104520: 424 of 476 learned textures
// were spectators, signs, props, road lines, atlases; the menu's lamp-post atlas got bent card normals = stepped
// shading). config\foliage-classes.txt (tools/pbr/make_foliage_classes.py, from the catalog): "card" textures are leaves
// from the first frame, "other" never; textures not in it (encrypted stages, the menu) are learned as before, minus
// materials.ini [foliage] not_foliage. And no mid-stage changes any more: every mod rewrite made Remix reload the mod
// and every new rtx.cardNormalsTextures rebuilt the instances (markers, clock, haybales flickered on Nechanice), so
// textures learned on a stage reach the mod and the card list in the menu.
std::unordered_map<uint64_t, bool> g_classes;           // catalog: true = card, false = other
std::unordered_set<uint64_t> g_notFoliage;              // materials.ini [foliage] not_foliage
size_t g_cardsKnown = 0;                                // "card" lines in foliage-classes.txt

// The catalog's cards and the learned foliage textures as rtx.cardNormalsTextures: once the API is up, then again only
// in the menu when the list grew (a change rebuilds the instances that use them).
void send_card_normals(bool inMenu) {
  const size_t n = g_cardsKnown + g_leaves.size();
  if (!g_cfg.cardNormals || (n == g_cardsAsked && !g_cardsDirty) || (g_cardNormalsSent && !inMenu)) return;
  std::string list;
  list.reserve(n * 20);
  char h[24];
  size_t sent = 0, solid = 0, user = 0;
  auto add = [&](uint64_t x) {
    if (g_userNotCard.count(x)) { ++user; return; }
    const auto l = g_leaves.find(x);
    if (l != g_leaves.end() && l->second.cut == 0) { ++solid; return; }
    sprintf_s(h, "%s0x%016llX", list.empty() ? "" : ", ", static_cast<unsigned long long>(x));
    list += h;
    ++sent;
  };
  for (const auto& kv : g_classes) if (kv.second) add(kv.first);
  for (const auto& kv : g_leaves) if (!g_classes.count(kv.first)) add(kv.first);
  if (!rbrrtx_remix_set("rtx.cardNormalsTextures", list.c_str())) return;
  g_cardNormalsSent = sent ? sent : 1;
  g_cardsAsked = n;
  g_cardsDirty = false;
  flog(L"foliage: %zu textures sent as rtx.cardNormalsTextures (%zu catalog cards; bent card normals; RBR-RTX runtime only); "
       L"left out: %zu solid (no cut-out alpha), %zu by your rtx.conf", sent, g_cardsKnown, solid, user);
}

// The live rtx.conf's hash lists that say "not a card" (see g_userNotCard).
void load_user_not_cards() {
  g_userNotCard.clear();
  auto read = [](const char* key, bool removals) {
    const std::string* v = rbrrtx_conf_value(key);
    if (!v) return;
    size_t i = 0;
    while (i < v->size()) {
      size_t e = v->find(',', i);
      if (e == std::string::npos) e = v->size();
      std::string t = v->substr(i, e - i);
      i = e + 1;
      const size_t s = t.find_first_not_of(" \t");
      if (s == std::string::npos) continue;
      const bool neg = t[s] == '-';
      if (neg != removals) continue;
      if (const uint64_t x = _strtoui64(t.c_str() + s + (neg ? 1 : 0), nullptr, 16)) g_userNotCard.insert(x);
    }
  };
  read("rtx.cardNormalsTextures", true);
  for (const char* k : { "rtx.smoothNormalsTextures", "rtx.decalTextures", "rtx.uiTextures", "rtx.worldSpaceUiTextures",
                         "rtx.ignoreTextures", "rtx.terrainTextures", "rtx.playerModelTextures" })
    read(k, false);
}

bool not_foliage(uint64_t h) {
  const auto it = g_classes.find(h);
  return (it != g_classes.end() && !it->second) || g_notFoliage.count(h);
}

void load_cfg() {
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_MATERIALS", L"materials.ini", path, MAX_PATH * 2);
  wchar_t v[96];
  auto f = [&](const wchar_t* key, float d) {
    GetPrivateProfileStringW(L"foliage", key, L"", v, 96, path);
    return v[0] ? static_cast<float>(_wtof(v)) : d;
  };
  auto rgb = [&](const wchar_t* key, float out[3]) {
    GetPrivateProfileStringW(L"foliage", key, L"", v, 96, path);
    float r, g, b;
    if (v[0] && swscanf_s(v, L"%f , %f , %f", &r, &g, &b) == 3) {
      out[0] = std::clamp(r, 0.0f, 1.0f); out[1] = std::clamp(g, 0.0f, 1.0f); out[2] = std::clamp(b, 0.0f, 1.0f);
    }
  };
  Cfg c;
  c.enabled = f(L"thin_opaque", 1.0f) != 0.0f;
  c.cardNormals = f(L"card_normals", 1.0f) != 0.0f;
  c.thickness = std::clamp(f(L"thickness_mm", c.thickness), 0.001f, 16.0f);
  rgb(L"transmittance", c.transmittance);
  c.brightest = std::clamp(f(L"scattering_brightest", c.brightest), 0.0f, 1.0f);
  rgb(L"scattering_albedo", c.albedo);
  c.anisotropy = std::clamp(f(L"anisotropy", c.anisotropy), -0.99f, 0.99f);
  c.roughness = std::clamp(f(L"roughness", c.roughness), 0.0f, 1.0f);
  g_cfg = c;
  wchar_t nf[2048] = {};
  GetPrivateProfileStringW(L"foliage", L"not_foliage", L"", nf, 2048, path);
  for (wchar_t* ctx = nullptr, *t = wcstok_s(nf, L", ", &ctx); t; t = wcstok_s(nullptr, L", ", &ctx))
    if (const uint64_t h = _wcstoui64(t, nullptr, 16)) g_notFoliage.insert(h);
  wchar_t cp[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_FOLIAGE_CLASSES", L"foliage-classes.txt", cp, MAX_PATH * 2);
  std::ifstream in(cp);
  for (std::string line; std::getline(in, line);) {
    char kind[16] = {};
    unsigned long long h = 0;
    if (line.empty() || line[0] == '#' || sscanf_s(line.c_str(), "%llx %15s", &h, kind, 16) != 2) continue;
    const bool card = !strcmp(kind, "card");
    g_classes[h] = card;
    g_cardsKnown += card;
  }
}

void publish() {
  std::string body, cache;
  for (const auto& kv : g_leaves) {
    const Leaf& l = kv.second;
    float a[3];
    const bool known = (std::max)({ l.t[0], l.t[1], l.t[2] }) > 0.0f;
    for (int i = 0; i < 3; ++i) a[i] = known ? (std::max)(0.05f, l.t[i] * g_cfg.brightest) : g_cfg.albedo[i];
    char in[1024];
    const int head = sprintf_s(in, "                float inputs:reflection_roughness_constant = %.3f\n"
                                   "                float inputs:metallic_constant = 0\n", g_cfg.roughness);
    if (g_cfg.enabled)
      sprintf_s(in + head, sizeof(in) - head,
              "                float inputs:subsurface_measurement_distance = %.4f\n"
              "                color3f inputs:subsurface_transmittance_color = (%.4f, %.4f, %.4f)\n"
              "                color3f inputs:subsurface_single_scattering_albedo = (%.4f, %.4f, %.4f)\n"
              "                float inputs:subsurface_volumetric_anisotropy = %.4f\n",
              g_cfg.thickness, g_cfg.transmittance[0], g_cfg.transmittance[1], g_cfg.transmittance[2], a[0], a[1], a[2],
              g_cfg.anisotropy);
    body += rbrrtx_genmod_material(kv.first, g_cfg.enabled ? "swaying foliage: matte, thin opaque" : "swaying foliage: matte", in);
    char line[96];
    sprintf_s(line, "%016llX %.4f %.4f %.4f %d\n", static_cast<unsigned long long>(kv.first), l.t[0], l.t[1], l.t[2], l.cut);
    cache += line;
  }
  rbrrtx_genmod_set("foliage", body);
  if (!g_cacheFile.empty()) {
    std::ofstream out(g_cacheFile, std::ios::binary | std::ios::trunc);
    out << cache;
  }
  g_dirty = false;
}

}  // namespace

// Before Remix starts (d3d9_proxy.cpp, after rbrrtx_genmod_init).
void rbrrtx_foliage_startup() {
  load_cfg();
  const wchar_t* dir = rbrrtx_genmod_dir();
  if (dir && dir[0]) {
    g_cacheFile = std::wstring(dir) + L"\\foliage.txt";
    std::ifstream in(g_cacheFile);
    for (std::string line; std::getline(in, line);) {
      unsigned long long h = 0;
      Leaf l;
      int cut = -1;
      const int got = sscanf_s(line.c_str(), "%llx %f %f %f %d", &h, &l.t[0], &l.t[1], &l.t[2], &cut);
      if (got < 4 || !h) continue;
      l.cut = got == 5 && cut >= -1 && cut <= 1 ? cut : -1;
      const float mx = (std::max)({ l.t[0], l.t[1], l.t[2] });   // first build's file: colour x 0.85
      for (float& c : l.t) c = mx > 0.0f ? c / mx : 0.0f;
      if (!not_foliage(h)) g_leaves[h] = l;           // dropped: textures the catalog knows as no foliage
    }
  }
  for (const auto& kv : g_classes)                      // the catalog's cards: leaf material from the first frame
    if (kv.second && !g_leaves.count(kv.first)) g_leaves[kv.first] = Leaf{ { 0.0f, 0.0f, 0.0f } };
  load_user_not_cards();
  publish();
  flog(L"foliage: %zu textures are no cards by your rtx.conf (card-normal removals, smooth normals, decals, UI, ignore, terrain, player model)",
       g_userNotCard.size());
  flog(L"foliage: thin opaque %s (thickness %.2f mm, transmittance %.2f %.2f %.2f, scattering albedo = leaf colour x %.2f, "
       L"anisotropy %.2f); %zu foliage textures known from earlier runs", g_cfg.enabled ? L"ON" : L"off (materials.ini)",
       g_cfg.thickness, g_cfg.transmittance[0], g_cfg.transmittance[1], g_cfg.transmittance[2], g_cfg.brightest,
       g_cfg.anisotropy, g_leaves.size());
  flog(L"foliage: catalog classes %zu (%zu cards), %zu not_foliage (materials.ini)", g_classes.size(), g_cardsKnown,
       g_notFoliage.size());
}

// A swaying foliage draw on the main target: its stage-0 texture.
void rbrrtx_foliage_draw(void* texture) {
  if (!texture) return;
  auto it = g_seen.find(texture);
  if (it != g_seen.end() && (it->second < 0 || it->second >= 30)) return;   // done, or never hashed
  const uint64_t h = rbrrtx_texture_hash(texture);
  if (!h) {
    if (it == g_seen.end()) g_seen[texture] = 1; else if (++it->second == 30) ++g_unhashed;
    return;
  }
  g_seen[texture] = -1;
  if (not_foliage(h)) return;
  const int minA = rbrrtx_texture_min_alpha(texture);
  const int cut = minA < 0 ? -1 : minA < kCutAlpha ? 1 : 0;
  const auto known = g_leaves.find(h);
  if (known != g_leaves.end()) {                        // a leaf from the cache or the catalog: judge its alpha once
    if (known->second.cut < 0 && cut >= 0) {
      known->second.cut = cut;
      g_dirty = g_cardsDirty = true;
      if (!cut) flog(L"foliage: texture %016llX has no cut-out alpha (lowest %d): no card normals", static_cast<unsigned long long>(h), minA);
    }
    return;
  }
  Leaf l;
  l.cut = cut;
  float rgb[3];
  const float mx = rbrrtx_texture_avg_rgb(texture, rgb) ? (std::max)({ rgb[0], rgb[1], rgb[2] }) : 0.0f;
  if (mx > 0.02f) for (int i = 0; i < 3; ++i) l.t[i] = rgb[i] / mx;
  else l.t[0] = l.t[1] = l.t[2] = 0.0f;
  g_leaves[h] = l;
  g_dirty = true;
  g_lastNew = std::chrono::steady_clock::now();
  if (++g_newThisRun <= 3 || g_newThisRun % 50 == 0)
    flog(L"foliage: texture %016llX thin opaque, leaf colour (%.2f %.2f %.2f) (%u new this run)",
         static_cast<unsigned long long>(h), l.t[0], l.t[1], l.t[2], g_newThisRun);
}

void rbrrtx_foliage_frame(bool inMenu) {
  // A texture address may be reused by another texture after a stage change: look at every one again now and then
  // (once per texture: one hash lookup).
  static unsigned frames = 0;
  if (++frames % 900 == 0) g_seen.clear();
  if (frames % 60 == 0) send_card_normals(inMenu);
  if (g_dirty && inMenu && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_lastNew).count() >= kQuiet) {
    publish();
    flog(L"foliage: %zu foliage textures in the generated mod (%u new this run)", g_leaves.size(), g_newThisRun);
  }
}

void rbrrtx_foliage_status(wchar_t* out, size_t n) {
  swprintf_s(out, n, L"foliage: thin opaque %s, %zu textures known (%u new this run, %u never hashed), thickness %.2f mm",
             g_cfg.enabled ? L"on" : L"off", g_leaves.size(), g_newThisRun, g_unhashed, g_cfg.thickness);
}
