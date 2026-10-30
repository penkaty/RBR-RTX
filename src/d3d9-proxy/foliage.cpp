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

namespace {

#define flog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

constexpr double kQuiet = 5.0;

struct Cfg {
  bool enabled = true;
  float thickness = 0.5f;                                // subsurface_measurement_distance, mm (Remix: 0..16)
  float transmittance[3] = { 0.5f, 0.5f, 0.5f };         // subsurface_transmittance_color: keep it grey
  float brightest = 0.8f;                                // scattering albedo = texture colour, brightest channel this
  float albedo[3] = { 0.6f, 0.75f, 0.45f };              // scattering albedo when the texture's colour is unknown
  float anisotropy = 0.3f;                               // forward scattering
} g_cfg;

struct Leaf { float t[3]; };                             // the texture's colour, brightest channel 1 (0 0 0: unknown)
std::map<uint64_t, Leaf> g_leaves;                       // texture hash -> colour
std::unordered_map<void*, int> g_seen;                   // texture -> tries (>= 0 done once hashed)
bool g_dirty = false;
std::chrono::steady_clock::time_point g_lastNew;
std::wstring g_cacheFile;
unsigned g_newThisRun = 0, g_unhashed = 0;

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
  c.thickness = std::clamp(f(L"thickness_mm", c.thickness), 0.001f, 16.0f);
  rgb(L"transmittance", c.transmittance);
  c.brightest = std::clamp(f(L"scattering_brightest", c.brightest), 0.0f, 1.0f);
  rgb(L"scattering_albedo", c.albedo);
  c.anisotropy = std::clamp(f(L"anisotropy", c.anisotropy), -0.99f, 0.99f);
  g_cfg = c;
}

void publish() {
  std::string body, cache;
  for (const auto& kv : g_leaves) {
    const Leaf& l = kv.second;
    float a[3];
    const bool known = (std::max)({ l.t[0], l.t[1], l.t[2] }) > 0.0f;
    for (int i = 0; i < 3; ++i) a[i] = known ? (std::max)(0.05f, l.t[i] * g_cfg.brightest) : g_cfg.albedo[i];
    char in[768];
    sprintf_s(in,
              "                float inputs:subsurface_measurement_distance = %.4f\n"
              "                color3f inputs:subsurface_transmittance_color = (%.4f, %.4f, %.4f)\n"
              "                color3f inputs:subsurface_single_scattering_albedo = (%.4f, %.4f, %.4f)\n"
              "                float inputs:subsurface_volumetric_anisotropy = %.4f\n",
              g_cfg.thickness, g_cfg.transmittance[0], g_cfg.transmittance[1], g_cfg.transmittance[2], a[0], a[1], a[2],
              g_cfg.anisotropy);
    if (g_cfg.enabled) body += rbrrtx_genmod_material(kv.first, "swaying foliage: thin opaque", in);
    char line[96];
    sprintf_s(line, "%016llX %.4f %.4f %.4f\n", static_cast<unsigned long long>(kv.first), l.t[0], l.t[1], l.t[2]);
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
      if (sscanf_s(line.c_str(), "%llx %f %f %f", &h, &l.t[0], &l.t[1], &l.t[2]) != 4 || !h) continue;
      const float mx = (std::max)({ l.t[0], l.t[1], l.t[2] });   // first build's file: colour x 0.85
      for (float& c : l.t) c = mx > 0.0f ? c / mx : 0.0f;
      g_leaves[h] = l;
    }
  }
  publish();
  flog(L"foliage: thin opaque %s (thickness %.2f mm, transmittance %.2f %.2f %.2f, scattering albedo = leaf colour x %.2f, "
       L"anisotropy %.2f); %zu foliage textures known from earlier runs", g_cfg.enabled ? L"ON" : L"off (materials.ini)",
       g_cfg.thickness, g_cfg.transmittance[0], g_cfg.transmittance[1], g_cfg.transmittance[2], g_cfg.brightest,
       g_cfg.anisotropy, g_leaves.size());
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
  if (g_leaves.count(h)) return;
  Leaf l;
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

void rbrrtx_foliage_frame() {
  // A texture address may be reused by another texture after a stage change: look at every one again now and then
  // (once per texture: one hash lookup).
  static unsigned frames = 0;
  if (++frames % 900 == 0) g_seen.clear();
  if (g_dirty && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_lastNew).count() >= kQuiet) {
    publish();
    flog(L"foliage: %zu foliage textures in the generated mod (%u new this run)", g_leaves.size(), g_newThisRun);
  }
}

void rbrrtx_foliage_status(wchar_t* out, size_t n) {
  swprintf_s(out, n, L"foliage: thin opaque %s, %zu textures known (%u new this run, %u never hashed), thickness %.2f mm",
             g_cfg.enabled ? L"on" : L"off", g_leaves.size(), g_newThisRun, g_unhashed, g_cfg.thickness);
}
