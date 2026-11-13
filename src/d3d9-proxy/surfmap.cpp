// surfmap.cpp - what each ground texture IS, asked from the game's own physics (user 2026-11-13: "find out which
// material is what by using the game data"; puddles only on tarmac and the driven gravel road, wet everywhere else).
//
// RBR knows the surface under every collision triangle: the stage's .mat file holds a 16x16 grid of surface ids
// (Physics\physics.lsp: Tarmac_*, Gravel_*, Grass_*, Dirt_*, SnowOn*, Rough_*, Rock_*) per material map, and each
// collision triangle names its map and its place in it (re/kb: CTrack_LoadMaterialMaps 0x46A460, CMaterialMaps_Lookup
// 0x520240). The game's segment query (CCollisionWorld_SegmentQuery 0x4FD760, world at *0x7C755C) returns that id
// for any point. So for a ground texture's draw, comp.cpp hands over a few points on its up-facing triangles
// (surfmap_note_draw), this file drops a 2 m vertical segment through each and counts the surfaces it finds. A
// texture's class is its most frequent surface; it is "road" (may hold puddles) when at least puddle_min_share of
// its points are tarmac or gravel. Works on every stage the game can load, encrypted ones included, without driving.
//
// What it feeds:
//   - rtx.wetTextures: road textures (and comp.cpp's ground twins); rtx.wetNoPuddleTextures: every other ground
//     texture, so grass and verges get wet without standing water (runtime feat/20261113-wet-no-puddles and later;
//     older runtimes refuse the second list, logged once).
//   - the generated mod <mods>\rbr_rtx_classes: a class material per probed texture (roughness, metallic, shared
//     detail normal and roughness map from <mods>\a_rbrrtx_classmat\classes.txt). The name sorts after
//     a_rbrrtx_classmat (the catalog's guess from the texture's name) and before rbr_rtx_materials and the PBR packs.
// Verdicts are kept in <mods>\rbr_rtx_lit\surfaces.txt, so a stage seen once is right from its first frame. New
// verdicts reach Remix in the menu, or once shortly after a stage starts (a mod reload is a hitch; list changes rebuild
// instances). Settings: materials.ini [surfaces].
#define CINTERFACE
#include "common.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

#define slog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

enum Fam : int { F_NONE, F_TARMAC, F_GRAVEL, F_SNOW, F_GRASS, F_DIRT, F_ROUGH, F_ROCK, F_WATER, F_OTHER, F_COUNT };
const char* const kFamName[F_COUNT] = { "none", "tarmac", "gravel", "snow", "grass", "dirt", "rough", "rock", "water", "other" };
// Remix class (config-remixplus\material-classes.ini) per family; nullptr = no material from here.
const char* const kFamClass[F_COUNT] = { nullptr, "road_tarmac", "road_gravel", "road_snow", "ground_grass", "ground_dirt",
                                         "ground_generic", "rock", nullptr, nullptr };

struct Cfg {
  bool enabled = true, materials = true, menu = true;
  unsigned puddleFams = (1u << F_TARMAC) | (1u << F_GRAVEL);
  float puddleMinShare = 0.5f;
  int samples = 32, draws = 8, perFrame = 3;
} g_cfg;

struct Probe { unsigned n[F_COUNT] = {}; unsigned hits = 0, misses = 0, draws = 0; unsigned short ids[256] = {}; };
struct Verdict { int fam = F_NONE; float road = 0.0f; unsigned hits = 0; };
struct ClassRow { float roughness = 0.8f, metallic = 0.0f; std::string normal, rough; };

unsigned char g_fam[256] = {};                           // physics.lsp surface id -> Fam
bool g_famLoaded = false, g_probeOk = false, g_probeChecked = false;
unsigned g_faults = 0;
std::unordered_map<uint64_t, Probe> g_open;              // textures still being probed
std::map<uint64_t, Verdict> g_done;                      // settled (also from surfaces.txt)
std::map<std::string, ClassRow> g_classes;
std::string g_twins;                                     // comp.cpp's ground twins, sent with the wet list
std::wstring g_cacheFile, g_mods;
bool g_dirty = false, g_listsDirty = true, g_modDirty = false, g_noPuddleRefused = false;
unsigned g_newThisRun = 0, g_frame = 0, g_readsThisFrame = 0, g_segments = 0;
std::chrono::steady_clock::time_point g_lastNew, g_lastSend, g_stageStart;
bool g_inStage = false, g_stageModSent = false;

// The game's collision query. __fastcall with a spare edx stands in for __thiscall (ecx = this, callee cleans up).
typedef int(__fastcall* CollectFn)(void* world, void* edx, const float* mn, const float* mx, int* list);
typedef int(__fastcall* SegmentFn)(void* world, void* edx, const float* from, const float* to, float* out, int* list);
const uintptr_t kWorldPtr = 0x007C755C, kCollect = 0x004FF070, kSegment = 0x004FD760;
const unsigned char kCollectHead[8] = { 0x83, 0xEC, 0x30, 0x8B, 0x44, 0x24, 0x34, 0xF3 };
const unsigned char kSegmentHead[8] = { 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x8B, 0x84 };

bool readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

// The two functions are the ones the knowledge base describes (another exe build, or a plugin's hook, has other bytes).
bool probe_available() {
  if (g_probeChecked) return g_probeOk;
  g_probeChecked = true;
  g_probeOk = readable(reinterpret_cast<void*>(kCollect), 8) && readable(reinterpret_cast<void*>(kSegment), 8) &&
              !std::memcmp(reinterpret_cast<void*>(kCollect), kCollectHead, 8) &&
              !std::memcmp(reinterpret_cast<void*>(kSegment), kSegmentHead, 8) && readable(reinterpret_cast<void*>(kWorldPtr), 4);
  slog(L"surfaces: physics probe %s", g_probeOk ? L"available (collision query at 0x4FD760)" :
       L"NOT available: the game's collision functions are not where this build expects them; ground keeps its catalog class");
  return g_probeOk;
}

// Surface id A under the render-space point (Y up), or -1. No C++ objects in here: structured exception handling.
int surface_at(const float p[3]) {
  static int list[4100];
  int id = -1;
  __try {
    void* world = *reinterpret_cast<void**>(kWorldPtr);
    if (!world || !readable(world, 0x1018) || !*reinterpret_cast<void**>(static_cast<char*>(world) + 8)) return -1;
    const float mn[3] = { p[0] - 0.25f, p[2] - 0.25f, p[1] - 1.25f }, mx[3] = { p[0] + 0.25f, p[2] + 0.25f, p[1] + 1.25f };
    const float from[3] = { p[0], p[2], p[1] + 1.0f }, to[3] = { p[0], p[2], p[1] - 1.0f };   // physics space: Z up
    float out[32] = {};
    list[0] = 0;
    if (!(reinterpret_cast<CollectFn>(kCollect)(world, nullptr, mn, mx, list) & 0xFF) || list[0] <= 0 || list[0] > 4000) return -1;
    if (reinterpret_cast<SegmentFn>(kSegment)(world, nullptr, from, to, out, list)) id = static_cast<int>(out[10]) & 0xFF;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ++g_faults;
    id = -2;
  }
  return id;
}

int fam_of_name(const std::string& name) {
  std::string n = name;
  if (!n.compare(0, 4, "US2_") || !n.compare(0, 4, "BR2_")) n.erase(0, 4);
  auto starts = [&](const char* s) { return !n.compare(0, std::strlen(s), s); };
  if (starts("Tarmac")) return F_TARMAC;
  if (starts("SnowOn")) return F_SNOW;
  if (starts("Gravel")) return F_GRAVEL;
  if (starts("Grass")) return F_GRASS;
  if (starts("Dirt")) return F_DIRT;
  if (starts("Rough")) return F_ROUGH;
  if (starts("Rock")) return F_ROCK;
  if (starts("Water")) return F_WATER;
  return F_OTHER;
}

// Physics\physics.lsp: lines like  Gravel_Fine_Shallow_Dry ("Gravel_Fine_Shallow_Dry" ; id 17, rev 42
void load_families() {
  g_famLoaded = true;
  wchar_t exe[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
  std::wstring path(exe);
  path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"Physics\\physics.lsp";
  std::ifstream in(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  unsigned found = 0;
  for (size_t p = 0; (p = text.find("; id ", p)) != std::string::npos; p += 5) {
    const size_t q2 = text.rfind('"', p), q1 = q2 == std::string::npos || !q2 ? std::string::npos : text.rfind('"', q2 - 1);
    const int id = std::atoi(text.c_str() + p + 5);
    if (q1 == std::string::npos || p - q2 > 4 || q2 - q1 > 64 || id <= 0 || id > 255) continue;
    g_fam[id] = static_cast<unsigned char>(fam_of_name(text.substr(q1 + 1, q2 - q1 - 1)));
    ++found;
  }
  if (found < 20) { g_probeChecked = true; g_probeOk = false; }
  slog(L"surfaces: %u surface types read from %s%s", found, path.c_str(), found < 20 ? L": too few, physics probe off" : L"");
}

void load_cfg() {
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_MATERIALS", L"materials.ini", path, MAX_PATH * 2);
  auto f = [&](const wchar_t* key, float d) {
    wchar_t v[64] = {};
    GetPrivateProfileStringW(L"surfaces", key, L"", v, 64, path);
    return v[0] ? static_cast<float>(_wtof(v)) : d;
  };
  g_cfg.enabled = f(L"physics_probe", 1.0f) != 0.0f;
  g_cfg.materials = f(L"class_materials", 1.0f) != 0.0f;
  g_cfg.menu = f(L"probe_in_menu", 1.0f) != 0.0f;
  g_cfg.puddleMinShare = std::clamp(f(L"puddle_min_share", g_cfg.puddleMinShare), 0.05f, 1.0f);
  g_cfg.samples = std::clamp(static_cast<int>(f(L"samples", 32.0f)), 8, 512);
  wchar_t pf[256] = {};
  GetPrivateProfileStringW(L"surfaces", L"puddle_surfaces", L"", pf, 256, path);
  if (pf[0]) {
    g_cfg.puddleFams = 0;
    for (wchar_t* ctx = nullptr, *t = wcstok_s(pf, L", ", &ctx); t; t = wcstok_s(nullptr, L", ", &ctx))
      for (int i = 1; i < F_COUNT; ++i) {
        wchar_t w[16] = {};
        for (int k = 0; kFamName[i][k] && k < 15; ++k) w[k] = kFamName[i][k];
        if (!_wcsicmp(t, w)) g_cfg.puddleFams |= 1u << i;
      }
  }
}

// <mods>\a_rbrrtx_classmat\classes.txt (tools/pbr/make_class_materials.py): class roughness metallic normal rough-map
void load_classes() {
  std::ifstream in(g_mods + L"\\a_rbrrtx_classmat\\classes.txt");
  for (std::string line; std::getline(in, line);) {
    char cls[48] = {}, nrm[64] = {}, rgh[64] = {};
    ClassRow r;
    if (line.empty() || line[0] == '#' || sscanf_s(line.c_str(), "%47s %f %f %63s %63s", cls, 48, &r.roughness, &r.metallic, nrm, 64, rgh, 64) != 5) continue;
    if (std::strcmp(nrm, "-")) r.normal = nrm;
    if (std::strcmp(rgh, "-")) r.rough = rgh;
    g_classes[cls] = r;
  }
}

bool puddles(const Verdict& v) { return v.road >= g_cfg.puddleMinShare; }

float road_share(const Probe& p) {
  unsigned road = 0;
  for (int i = 1; i < F_COUNT; ++i) if (g_cfg.puddleFams & (1u << i)) road += p.n[i];
  return p.hits ? static_cast<float>(road) / p.hits : 0.0f;
}

void save_cache() {
  if (g_cacheFile.empty()) return;
  std::ofstream out(g_cacheFile, std::ios::binary | std::ios::trunc);
  out << "# texture hash, surface (from the game's physics), share of road points, points; written by RBR-RTX (surfmap.cpp)\n";
  char line[96];
  for (const auto& kv : g_done) {
    if (kv.second.fam == F_NONE) continue;
    sprintf_s(line, "%016llX %s %.2f %u\n", static_cast<unsigned long long>(kv.first), kFamName[kv.second.fam], kv.second.road, kv.second.hits);
    out << line;
  }
}

// The generated mod's section: one class material per settled ground texture.
void publish_mod() {
  g_modDirty = false;
  if (!g_cfg.materials) { rbrrtx_genmod_set("classes", ""); return; }
  std::string body;
  for (const auto& kv : g_done) {
    const char* cls = kFamClass[kv.second.fam];
    if (!cls) continue;
    ClassRow r;
    const auto it = g_classes.find(cls);
    if (it != g_classes.end()) r = it->second;
    else if (kv.second.fam == F_GRASS || kv.second.fam == F_ROUGH) r.roughness = 0.93f;
    char in[640];
    int n = sprintf_s(in, "                float inputs:reflection_roughness_constant = %.3f\n"
                          "                float inputs:metallic_constant = %.3f\n", r.roughness, r.metallic);
    if (!r.normal.empty())
      n += sprintf_s(in + n, sizeof(in) - n, "                asset inputs:normalmap_texture = @../a_rbrrtx_classmat/textures/%s@ (\n"
                                             "                    colorSpace = \"raw\"\n                )\n", r.normal.c_str());
    if (!r.rough.empty())
      sprintf_s(in + n, sizeof(in) - n, "                asset inputs:reflectionroughness_texture = @../a_rbrrtx_classmat/textures/%s@ (\n"
                                        "                    colorSpace = \"raw\"\n                )\n", r.rough.c_str());
    body += rbrrtx_genmod_material(kv.first, std::string(cls) + " (physics: " + kFamName[kv.second.fam] + ")", in);
  }
  rbrrtx_genmod_set("classes", body);
}

void send_lists() {
  g_listsDirty = false;
  g_lastSend = std::chrono::steady_clock::now();
  std::string wet = g_twins, dry;
  size_t nWet = 0, nDry = 0;
  char h[24];
  for (const auto& kv : g_done) {
    if (kv.second.fam == F_NONE || kv.second.fam == F_SNOW || kv.second.fam == F_WATER || kv.second.fam == F_OTHER) continue;
    std::string& to = puddles(kv.second) ? wet : dry;
    sprintf_s(h, "%s0x%016llX", to.empty() ? "" : ", ", static_cast<unsigned long long>(kv.first));
    to += h;
    ++(puddles(kv.second) ? nWet : nDry);
  }
  static bool sentWet = false;                           // nothing to add yet: rtx.conf's own list stands
  if (!wet.empty() || sentWet) sentWet = rbrrtx_remix_set("rtx.wetTextures", wet.c_str()) || sentWet;
  if (!g_noPuddleRefused && !rbrrtx_remix_set("rtx.wetNoPuddleTextures", dry.c_str())) {
    g_noPuddleRefused = true;
    slog(L"surfaces: this runtime has no rtx.wetNoPuddleTextures (needs the wet-no-puddles runtime): ground beside the road stays as rtx.conf has it");
  }
  slog(L"surfaces: wet lists sent: %zu road textures (puddles), %zu other ground (wet, no puddles)", nWet, nDry);
}

void settle(uint64_t hash, const Probe& p) {
  Verdict v;
  v.hits = p.hits;
  if (p.hits >= 6) {
    int best = F_OTHER;
    for (int i = 1; i < F_COUNT; ++i) if (p.n[i] > p.n[best]) best = i;
    // A road texture is a road even where its edge strip counts more grass points than gravel ones.
    v.road = road_share(p);
    if (v.road >= g_cfg.puddleMinShare && !(g_cfg.puddleFams & (1u << best))) best = p.n[F_TARMAC] >= p.n[F_GRAVEL] ? F_TARMAC : F_GRAVEL;
    v.fam = best;
  }
  g_done[hash] = v;
  if (v.fam == F_NONE) return;                           // kept for this scene only (rbrrtx_surfmap_frame), never saved
  g_dirty = g_listsDirty = g_modDirty = true;
  g_lastNew = std::chrono::steady_clock::now();
  int top[3] = { 0, 0, 0 };                              // the three most frequent surface ids, for the log
  for (int i = 1; i < 256; ++i) {
    if (p.ids[i] > p.ids[top[0]]) { top[2] = top[1]; top[1] = top[0]; top[0] = i; }
    else if (p.ids[i] > p.ids[top[1]]) { top[2] = top[1]; top[1] = i; }
    else if (p.ids[i] > p.ids[top[2]]) top[2] = i;
  }
  if (++g_newThisRun <= 60 || g_newThisRun % 50 == 0)
    slog(L"surfaces: %016llX = %S (%u points: tarmac %u gravel %u snow %u grass %u dirt %u rough %u rock %u other %u; ids %d x%u, %d x%u, %d x%u; %u without a surface, %u draws)%s",
         static_cast<unsigned long long>(hash), kFamName[v.fam], p.hits, p.n[F_TARMAC], p.n[F_GRAVEL], p.n[F_SNOW], p.n[F_GRASS],
         p.n[F_DIRT], p.n[F_ROUGH], p.n[F_ROCK], p.n[F_WATER] + p.n[F_OTHER], top[0], p.ids[top[0]], top[1], p.ids[top[1]], top[2], p.ids[top[2]],
         p.misses, p.draws, !kFamClass[v.fam] ? L"" : puddles(v) ? L": road, puddles" : L": wet without puddles");
}

}  // namespace

// Before Remix starts (d3d9_proxy.cpp, after rbrrtx_genmod_init).
void rbrrtx_surfmap_startup(const wchar_t* modsDir) {
  load_cfg();
  g_mods = modsDir ? modsDir : L"";
  load_classes();
  const wchar_t* dir = rbrrtx_genmod_dir();
  if (dir && dir[0]) {
    g_cacheFile = std::wstring(dir) + L"\\surfaces.txt";
    std::ifstream in(g_cacheFile);
    for (std::string line; std::getline(in, line);) {
      unsigned long long h = 0;
      char fam[16] = {};
      Verdict v;
      if (line.empty() || line[0] == '#' || sscanf_s(line.c_str(), "%llx %15s %f %u", &h, fam, 16, &v.road, &v.hits) != 4 || !h) continue;
      for (int i = 0; i < F_COUNT; ++i) if (!std::strcmp(fam, kFamName[i])) v.fam = i;
      g_done[h] = v;
    }
  }
  publish_mod();
  slog(L"surfaces: physics probe %s, class materials %s, puddles where >= %.0f %% of a texture is road; %zu textures known from earlier runs, %zu classes",
       g_cfg.enabled ? L"on" : L"off (materials.ini [surfaces])", g_cfg.materials ? L"on" : L"off", g_cfg.puddleMinShare * 100.0f,
       g_done.size(), g_classes.size());
}

// Does comp.cpp need to read this ground draw's triangles for texture `hash`? At most a few reads per frame.
bool rbrrtx_surfmap_wants(unsigned long long hash) {
  if (!g_cfg.enabled || !hash || g_done.count(hash) || (!g_inStage && !g_cfg.menu) || g_faults >= 3) return false;
  if (!g_famLoaded) load_families();
  if (!probe_available() || g_readsThisFrame >= static_cast<unsigned>(g_cfg.perFrame)) return false;
  ++g_readsThisFrame;
  return true;
}

// Points (render space, xyz each) on up-facing triangles of one draw that uses texture `hash`; n = 0: unreadable draw.
void rbrrtx_surfmap_samples(unsigned long long hash, const float* pts, unsigned n) {
  Probe& p = g_open[hash];
  ++p.draws;
  for (unsigned i = 0; i < n && g_faults < 3; ++i) {
    const int id = surface_at(pts + i * 3);
    ++g_segments;
    if (id <= 0) { ++p.misses; continue; }               // 0 = Undefined: no material maps loaded (the menu's scene)
    ++p.ids[id];
    ++p.n[g_fam[id] ? g_fam[id] : F_OTHER];
    ++p.hits;
  }
  if (g_faults >= 3) { slog(L"surfaces: the collision query faulted %u times: physics probe off for this run", g_faults); return; }
  if (p.hits >= static_cast<unsigned>(g_cfg.samples) || p.draws >= static_cast<unsigned>(g_cfg.draws)) {
    const Probe done = p;
    g_open.erase(hash);
    settle(hash, done);
  }
}

void rbrrtx_surfmap_twins(const char* list) {
  if (g_twins == (list ? list : "")) return;
  g_twins = list ? list : "";
  g_listsDirty = true;
}

// Per Present.
void rbrrtx_surfmap_frame(bool inMenu, bool apiOk) {
  ++g_frame;
  g_readsThisFrame = 0;
  const auto now = std::chrono::steady_clock::now();
  if (g_inStage == inMenu) {
    g_inStage = !inMenu;
    g_stageStart = now;
    g_stageModSent = false;
    g_open.clear();                                      // counts of a half-probed texture belong to the stage left
    for (auto it = g_done.begin(); it != g_done.end();)  // "no surface found" held for that scene only: asked again here
      it = it->second.fam == F_NONE ? g_done.erase(it) : std::next(it);
  }
  if (!g_cfg.enabled || !apiOk || !rbrrtx_remix_plus()) return;
  const double quiet = std::chrono::duration<double>(now - g_lastNew).count();
  const double sinceSend = std::chrono::duration<double>(now - g_lastSend).count();
  const double inStage = std::chrono::duration<double>(now - g_stageStart).count();
  // Lists: at once in the menu and at the first settle of a stage, then at most every 20 s (a change rebuilds instances).
  if (g_listsDirty && quiet >= 1.5 && (inMenu || sinceSend >= 20.0 || inStage < 15.0)) send_lists();
  // The mod: in the menu, or once per stage in its first 20 s (the countdown), after the probe went quiet.
  if (g_modDirty && quiet >= 2.0 && (inMenu || (!g_stageModSent && inStage < 20.0))) {
    if (!inMenu) g_stageModSent = true;
    publish_mod();
    slog(L"surfaces: %zu ground textures in the generated mod rbr_rtx_classes (%u new this run, %u collision queries)",
         g_done.size(), g_newThisRun, g_segments);
  }
  if (g_dirty && quiet >= 2.0) { g_dirty = false; save_cache(); }
}

void rbrrtx_surfmap_status(wchar_t* out, size_t n) {
  size_t road = 0, ground = 0;
  for (const auto& kv : g_done) { if (kv.second.fam == F_NONE) continue; ++(puddles(kv.second) ? road : ground); }
  swprintf_s(out, n, L"surfaces: %zu road + %zu other ground textures known (%u new this run, %zu open, probe %s)", road, ground,
             g_newThisRun, g_open.size(), !g_cfg.enabled ? L"off" : g_probeChecked && !g_probeOk ? L"unavailable" : L"on");
}
