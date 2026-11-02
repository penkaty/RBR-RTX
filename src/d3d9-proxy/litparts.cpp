// litparts.cpp - lit car parts glow under Remix (user 2026-10-30: "RBR exchanges the brake disc textures under braking
// for orange ones to imitate them glowing from heat").
//
// How RBR does it: a lit part is a mesh of its own (e_brakedisc_lf_lit, e_light_brake_lb_lit, e_light_lf_lit, ...; the
// car's <car>.ini: Switch = true, SwitchBox brake_lb / light_lf, Predicate = on), shown instead of the unlit one by the
// game's lamp switch (HedgeHog3D CSigCarLight, re/kb/systems/lights-atmosphere.md section 3). It is textured from the
// car's shared atlas (material "external dynamic" -> <car>_external_dynamic.dds), so a Remix material keyed by that
// texture's hash would light the whole car. Under path tracing the orange texels are just orange paint.
//
// What happens here, per car:
// 1. Learn the lit parts. Every car draw (buffers + index range = a fingerprint) is counted per signal: brake (pedal
//    input), lamps (the game's lamp switch as headlights.cpp last sent it), reverse (gear R). At a clean switch of a
//    signal (switch_seen: the others steady, the same view on both sides) a fingerprint that appears with it is its lit
//    part; one drawn on both sides is not; one that vanishes when the brake comes on is the unlit brake lens (it glows
//    as a tail lamp while the lamps are on). Drawn with every signal off = never a lit part. Parts bigger than
//    kMaxPrims (a body, an interior) are never lit parts. headlights.cpp keeps the game's lamps off for the first car
//    frames of a stage, so night stages give one clean lamp switch at the start.
// 2. Bind a twin texture for those draws: 16x16 A8R8G8B8 whose content is derived from (atlas hash, signal), so its
//    Remix hash (XXH3_64 of mip 0, harvest.cpp) is known before Remix sees it. Only Remix sees it: the replacement
//    material below brings the atlas back.
// 3. The generated mod (genmod.cpp, section "litparts") gives the twin hash a material whose albedo and emission are
//    the atlas file (absolute path to the player's own file; found by hashing the car folder's .dds files as Remix
//    does, else the car's *_external_dynamic.dds of the same size), emissive intensity per signal (materials.ini).
//    Entries are kept in <mods>\rbr_rtx_lit\litparts.txt, so a known car glows from the first frame of the next run.
#define CINTERFACE
#include "common.hpp"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

#define llog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

constexpr unsigned kMaxPrims = 4000, kSettle = 12, kSwitchWindow = 60;
constexpr const char* kCacheTag = "# litparts v2: heat steps with their own colour files; older entries are not read";
constexpr unsigned kDiscMinPrims = 60, kDiscMaxPrims = 800, kLensMaxPrims = 600, kJudgeFrames = 4;
// Signals, and the glow kinds: tail = the unlit brake lens while the lamps are on; lamps by day = a lit head lamp lens
// with the sun up (user 2026-10-30: "too dim to be noticed in the daytime, good at night").
// brake disc = a brake part that moves with a wheel, its own glow since 2026-11-01 (user: lamp emissives separate from the
// discs, lamps 75 % dimmer; the discs and the brake lenses shared one material per car atlas until then).
// A brake disc has kDiscLevels glow kinds, one per heat step (kDisc .. kDisc + kDiscLevels - 1; a twin and a material each).
constexpr int kDiscLevels = 6;
enum { kBrake = 0, kLamps = 1, kReverse = 2, kSignals = 3, kTail = 3, kLampsDay = 4, kDisc = 5, kKinds = kDisc + kDiscLevels };
const wchar_t* const kSignalName[kKinds] = { L"brake", L"lamps", L"reverse", L"tail", L"lamps by day", L"brake disc heat 1/6",
                                              L"brake disc heat 2/6", L"brake disc heat 3/6", L"brake disc heat 4/6",
                                              L"brake disc heat 5/6", L"brake disc heat 6/6" };

struct Cfg {
  bool enabled = true;
  float glow[kKinds] = { 3.0f, 5.0f, 3.75f, 1.5f, 30.0f };   // the disc steps are filled in by load_cfg
  float dayAbove = 5.0f;                                 // sun height (degrees) from which lamp lenses use the day glow
  // Brake discs (user 2026-10-30: "the emissives of the BRAKE DISCS ... seemed to glow constantly"; 2026-11-02: "lights
  // too often, keeps warmth between brakings, starts WAY too intense and bluish"): a brake part drawn with another matrix
  // than the body's moves with a wheel - a disc. Brake lamp lenses (drawn with the body's matrix) light at once. A disc
  // has a heat: braking (pedal x speed) raises it, it cools with time and faster with speed (air), and it glows only above
  // discStart, dull red first, orange at the top, in kDiscLevels steps up to discGlow (the emissive intensity at heat 1).
  float discGlow = 3.0f, discRate = 0.04f, discCool = 45.0f, discStart = 0.35f, discFull = 1.0f;
} g_cfg;

// Evidence comes only from clean switches (switch_seen): one signal flips, the others stay settled the same, and the
// car is drawn from the same view on both sides. Run 20261030-055723 (06:02:37): with "drawn only while on" counting,
// ~100 body, chassis and interior parts became lamp parts after 230 lamp-off frames the replay showed from another
// camera (other LODs) - the whole car glowed.
struct Part {
  unsigned yes[kSignals] = {};                           // appeared when it switched on / vanished when it switched off
  unsigned no[kSignals] = {};                            // drawn on both sides of a clean switch: not switched by it
  unsigned partner[kSignals] = {};                       // vanished when it switched on: the unlit twin of a lit part
  bool everOn[kSignals] = {};                            // drawn while the signal was settled on
  unsigned lastFrame = ~0u;
  unsigned mask = 0;                                     // signals it is the lit part of (tail lenses: lamps and brake)
  bool tail = false;                                     // unlit brake lens: glows as a tail lamp with the lamps on
  bool never = false;                                    // drawn with every signal off: an ordinary part
  unsigned prims = 0;
  unsigned onBody = 0, offBody = 0;                      // frames drawn with the body's matrix / with another one
  // Turned against the body (a lens never is) = moves with a wheel. A brake disc is a small wheel part: the 220-triangle
  // parts (one per wheel) of the Whaanga run; a 2316-triangle part that turns with a wheel is the whole brake assembly or
  // hub, not a disc (user 2026-11-02: "discs glowed from the very start, bluish"), and a few-triangle part is a lens.
  bool wheelPart() const { return offBody >= 3; }
  bool disc() const { return wheelPart() && prims >= kDiscMinPrims && prims <= kDiscMaxPrims; }
  bool settled() const { return onBody + offBody >= kJudgeFrames; }   // seen long enough to tell lens from disc
  bool discQueued = false;                               // its heat steps are on their way to the generated mod
};
std::unordered_map<uint64_t, Part> g_parts;              // this car's draws
unsigned g_frame = 0;
unsigned g_carDrawsThisFrame = 0;
std::vector<uint64_t> g_drawn;                           // this frame's part keys
struct DrawnAt { uint64_t key; float m[12]; };            // the draw's matrix rows 0-2 (axes)
std::vector<DrawnAt> g_drawnAt;                          // this frame's lit-part draws and where (matrix row 3)
float g_heat = 0.0f;                                     // brake disc heat (0 cold, 1 full glow, more is capped by the glow only)
int g_discLevel = 0;                                     // glow step now (0 = none, 1..kDiscLevels)
float g_kmh = 0.0f;                                      // car speed, smoothed
float g_lastPos[3] = {};
ULONGLONG g_heatTick = 0;                                // last heat update (0 = none yet)
float g_sunEl = 90.0f;                                   // sun height at the last Present
struct Side { bool valid = false; unsigned frame = 0; int st[kSignals] = {}; std::vector<uint64_t> keys; };
Side g_side[kSignals];                                   // per signal: the last judged frame with it settled
unsigned g_switches = 0, g_switchesRefused = 0;
std::wstring g_carFile;

// Signal state per frame: +1 clearly on, -1 clearly off, 0 changing (the game fades lamps over 0.02-0.05 s and its
// own brake threshold is unknown: kSettle frames in a row first).
int g_state[kSignals] = {};
int g_raw[kSignals] = {};
unsigned g_same[kSignals] = {};

// Twins and the materials for them.
struct Twin { IDirect3DTexture9* tex = nullptr; uint64_t hash = 0; };
std::map<std::pair<uint64_t, int>, Twin> g_twins;        // (atlas hash, signal) -> twin
struct Entry { uint64_t twin, src; int signal; std::wstring file; };
std::map<uint64_t, Entry> g_entries;                     // twin hash -> material (this run + earlier runs)
std::mutex g_mx;                                         // g_entries, g_pending (worker thread)
struct Pending { uint64_t twin, src; int signal; std::wstring carDir; UINT w, h; };
std::vector<Pending> g_pending;
std::atomic<bool> g_workerBusy{ false };
std::wstring g_cacheFile;
unsigned g_learnt = 0, g_swapped = 0, g_noHash = 0;

uint64_t mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t splitmix(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// The twin's mip 0 (16x16 A8R8G8B8, tight rows), derived from the atlas hash and the signal.
void twin_pixels(uint64_t src, int signal, uint32_t px[256]) {
  uint64_t s = src ^ (0x52425254'4C495450ULL + static_cast<uint64_t>(signal) * 0x1000193ULL);   // "RBRTLITP"
  for (int i = 0; i < 256; ++i) px[i] = static_cast<uint32_t>(splitmix(s)) | 0xFF000000u;
}

uint64_t twin_hash(uint64_t src, int signal) {
  uint32_t px[256];
  twin_pixels(src, signal, px);
  return rbrrtx_xxh3(px, sizeof(px));
}

std::wstring materials_ini() {
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_MATERIALS", L"materials.ini", path, MAX_PATH * 2);
  return path;
}

void load_cfg() {
  const std::wstring p = materials_ini();
  Cfg c;
  wchar_t v[64];
  auto f = [&](const wchar_t* key, float d) {
    GetPrivateProfileStringW(L"lit_parts", key, L"", v, 64, p.c_str());
    return v[0] ? static_cast<float>(_wtof(v)) : d;
  };
  c.enabled = f(L"enable", 1.0f) != 0.0f;
  c.glow[kBrake] = std::clamp(f(L"brake_glow", c.glow[kBrake]), 0.0f, 10000.0f);
  c.glow[kLamps] = std::clamp(f(L"lamp_glow", c.glow[kLamps]), 0.0f, 10000.0f);
  c.glow[kReverse] = std::clamp(f(L"reverse_glow", c.glow[kReverse]), 0.0f, 10000.0f);
  c.glow[kTail] = std::clamp(f(L"tail_glow", c.glow[kTail]), 0.0f, 10000.0f);
  c.glow[kLampsDay] = std::clamp(f(L"lamp_glow_day", c.glow[kLampsDay]), 0.0f, 10000.0f);
  c.discGlow = std::clamp(f(L"brake_disc_glow", c.discGlow), 0.0f, 10000.0f);
  c.discRate = std::clamp(f(L"brake_disc_heat_rate", c.discRate), 0.0f, 10.0f);
  c.discCool = std::clamp(f(L"brake_disc_cool_seconds", c.discCool), 1.0f, 3600.0f);
  c.discStart = std::clamp(f(L"brake_disc_glow_start", c.discStart), 0.0f, 5.0f);
  c.discFull = std::clamp(f(L"brake_disc_glow_full", c.discFull), c.discStart + 0.05f, 6.0f);
  for (int i = 0; i < kDiscLevels; ++i)                  // step i of kDiscLevels: a little over linear, so the first steps stay dull
    c.glow[kDisc + i] = c.discGlow * std::pow(static_cast<float>(i + 1) / kDiscLevels, 1.4f);
  c.dayAbove = f(L"lamp_glow_day_above_sun", c.dayAbove);
  g_cfg = c;
}

// Remix's hash of a .dds file's mip 0 (XXH3_64) and its size. False if not readable or not handled.
bool dds_hash(const std::wstring& path, uint64_t* hash, UINT* w, UINT* h) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  char hd[128];
  if (!f.read(hd, 128) || std::memcmp(hd, "DDS ", 4)) return false;
  unsigned hgt, wid, flags, four, bits;
  std::memcpy(&hgt, hd + 12, 4);
  std::memcpy(&wid, hd + 16, 4);
  std::memcpy(&flags, hd + 80, 4);
  std::memcpy(&four, hd + 84, 4);
  std::memcpy(&bits, hd + 88, 4);
  size_t size = 0;
  if (flags & 4) {
    if (four == MAKEFOURCC('D', 'X', '1', '0')) return false;
    const size_t blk = four == MAKEFOURCC('D', 'X', 'T', '1') ? 8 : 16;
    size = (std::max)(1u, (wid + 3) / 4) * static_cast<size_t>((std::max)(1u, (hgt + 3) / 4)) * blk;
  } else {
    size = static_cast<size_t>(wid) * hgt * (bits / 8);
  }
  if (!size || size > (64u << 20)) return false;
  std::vector<char> d(size);
  if (!f.read(d.data(), static_cast<std::streamsize>(size))) return false;
  *hash = rbrrtx_xxh3(d.data(), size);
  *w = wid;
  *h = hgt;
  return true;
}

// A hot brake disc's colour per heat step (sRGB, as the file holds it): dull red at the first step, orange at the last.
void heat_rgb(int signal, float rgb[3]) {
  const float t = static_cast<float>(signal - kDisc) / (kDiscLevels - 1);
  rgb[0] = 1.0f;
  rgb[1] = 0.22f + 0.30f * t;
  rgb[2] = 0.05f * t;
}

// The emission mask of a heat step: a 4x4 solid-colour DDS of that colour, so the glow is never the atlas's own colour (the
// grey-blue metal of the disc texture: "bluish", user 2026-11-02). Written once per run (it is tiny).
std::wstring heat_dds(int signal) {
  const wchar_t* dir = rbrrtx_genmod_dir();
  if (!dir || !dir[0]) return std::wstring();
  const std::wstring path = std::wstring(dir) + L"\\litparts_heat_" + std::to_wstring(signal - kDisc) + L".dds";
  static bool done[kKinds] = {};
  if (done[signal]) return path;
  float c[3];
  heat_rgb(signal, c);
  const uint32_t argb = 0xFF000000u | (static_cast<uint32_t>(std::lround(c[0] * 255.0f)) << 16) |
                        (static_cast<uint32_t>(std::lround(c[1] * 255.0f)) << 8) | static_cast<uint32_t>(std::lround(c[2] * 255.0f));
  uint32_t h[32] = {};                                   // "DDS " + 124-byte header
  std::memcpy(h, "DDS ", 4);
  h[1] = 124;
  h[2] = 0x100F;                                         // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
  h[3] = 4; h[4] = 4; h[5] = 16;                         // height, width, pitch
  h[7] = 1;                                              // mip levels
  h[19] = 32; h[20] = 0x41; h[22] = 32;                  // pixel format: size, RGB | ALPHAPIXELS, 32 bits
  h[23] = 0x00FF0000u; h[24] = 0x0000FF00u; h[25] = 0x000000FFu; h[26] = 0xFF000000u;
  h[27] = 0x1000;                                        // TEXTURE
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(h), 128);
  for (int i = 0; i < 16; ++i) f.write(reinterpret_cast<const char*>(&argb), 4);
  if (!f) return std::wstring();
  done[signal] = true;
  return path;
}

std::string material_for(const Entry& e) {
  const std::string a = rbrrtx_genmod_asset(e.file);
  char glow[64];
  sprintf_s(glow, "%.3f", g_cfg.glow[e.signal]);
  // A heat step's emission mask is its own colour file, not the atlas (lens and lamp glow keep the atlas as the mask).
  std::string mask = a;
  if (e.signal >= kDisc) {
    const std::wstring hd = heat_dds(e.signal);
    if (!hd.empty()) mask = rbrrtx_genmod_asset(hd);
  }
  const std::string in =
      "                asset inputs:diffuse_texture = @" + a + "@ (\n"
      "                    colorSpace = \"auto\"\n"
      "                )\n"
      "                bool inputs:enable_emission = 1\n"
      "                asset inputs:emissive_mask_texture = @" + mask + "@ (\n"
      "                    colorSpace = \"auto\"\n"
      "                )\n"
      "                float inputs:emissive_intensity = " + glow + "\n";
  std::string in2 = in;
  if (e.signal >= kDisc) {                               // the same colour as the constant, whichever of the two the material takes
    float c[3];
    heat_rgb(e.signal, c);
    char col[120];
    sprintf_s(col, "                color3f inputs:emissive_color_constant = (%.3f, %.3f, %.3f)\n", c[0], c[1], c[2]);
    in2 += col;
  }
  char c[96];
  sprintf_s(c, "lit car part (%ls), atlas %016llX:", kSignalName[e.signal], static_cast<unsigned long long>(e.src));
  return rbrrtx_genmod_material(e.twin, std::string(c) + " " + a, in2);
}

// Section "litparts" from every entry, and the cache file. Caller holds g_mx.
void publish_locked() {
  std::string body;
  std::string cache = std::string(kCacheTag) + "\n";
  for (const auto& kv : g_entries) {
    const Entry& e = kv.second;
    if (g_cfg.glow[e.signal] <= 0.0f) continue;
    body += material_for(e);
    char line[64];
    sprintf_s(line, "%016llX %016llX %d ", static_cast<unsigned long long>(e.twin), static_cast<unsigned long long>(e.src), e.signal);
    cache += line + rbrrtx_genmod_asset(e.file) + "\n";
  }
  rbrrtx_genmod_set("litparts", body);
  if (!g_cacheFile.empty()) {
    std::ofstream out(g_cacheFile, std::ios::binary | std::ios::trunc);
    out << cache;
  }
}

// Worker: find the atlas file of each pending twin in its car folder, then publish.
void resolve_pending() {
  std::vector<Pending> todo;
  {
    std::lock_guard<std::mutex> g(g_mx);
    todo.swap(g_pending);
  }
  std::wstring dirDone;
  struct File { std::wstring path; uint64_t hash; UINT w, h; };
  std::vector<File> files;
  bool any = false;
  for (const Pending& p : todo) {
    if (p.carDir != dirDone) {
      files.clear();
      dirDone = p.carDir;
      std::vector<std::wstring> stack = { p.carDir };
      while (!stack.empty() && files.size() < 256) {
        const std::wstring d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
          if (fd.cFileName[0] == L'.') continue;
          const std::wstring full = d + L"\\" + fd.cFileName;
          if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { stack.push_back(full); continue; }
          const size_t n = wcslen(fd.cFileName);
          if (n < 4 || _wcsicmp(fd.cFileName + n - 4, L".dds")) continue;
          File f{ full, 0, 0, 0 };
          if (dds_hash(full, &f.hash, &f.w, &f.h)) files.push_back(f);
        } while (FindNextFileW(h, &fd) && files.size() < 256);
        FindClose(h);
      }
    }
    const File* found = nullptr;
    const wchar_t* how = L"same hash";
    for (const File& f : files) if (f.hash == p.src) { found = &f; break; }
    if (!found) {                                        // RBR re-bakes the dynamic atlas (dirt): same size, by name
      how = L"same size, by name (the game's copy differs from the file)";
      int bestRank = 99;
      for (const File& f : files) {
        if (f.w != p.w || f.h != p.h) continue;
        std::wstring low = f.path;
        for (wchar_t& c : low) c = static_cast<wchar_t>(towlower(c));
        const int rank = low.find(L"\\skins\\") != std::wstring::npos ? 50 :
                         low.find(L"_external_dynamic.dds") != std::wstring::npos ? 0 :
                         low.find(L"_external.dds") != std::wstring::npos ? 1 : 10;
        if (rank < bestRank) { bestRank = rank; found = &f; }
      }
    }
    if (!found) {
      llog(L"lit parts: no .dds in %s matches atlas %016llX (%ux%u): that %s part does not glow", p.carDir.c_str(),
           static_cast<unsigned long long>(p.src), p.w, p.h, kSignalName[p.signal]);
      continue;
    }
    llog(L"lit parts: %s part on atlas %016llX glows with %s (%s), twin %016llX", kSignalName[p.signal],
         static_cast<unsigned long long>(p.src), found->path.c_str(), how, static_cast<unsigned long long>(p.twin));
    std::lock_guard<std::mutex> g(g_mx);
    g_entries[p.twin] = { p.twin, p.src, p.signal, found->path };
    any = true;
  }
  if (any) {
    std::lock_guard<std::mutex> g(g_mx);
    publish_locked();
  }
  g_workerBusy = false;
}

IDirect3DTexture9* make_twin(IDirect3DDevice9* dev, uint64_t src, int signal, uint64_t* hashOut) {
  auto it = g_twins.find({ src, signal });
  if (it != g_twins.end()) { *hashOut = it->second.hash; return it->second.tex; }
  Twin t;
  uint32_t px[256];
  twin_pixels(src, signal, px);
  t.hash = rbrrtx_xxh3(px, sizeof(px));
  if (SUCCEEDED(dev->lpVtbl->CreateTexture(dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex, nullptr)) && t.tex) {
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(t.tex->lpVtbl->LockRect(t.tex, 0, &lr, nullptr, 0)) && lr.pBits) {
      for (int r = 0; r < 16; ++r) std::memcpy(static_cast<char*>(lr.pBits) + r * lr.Pitch, px + r * 16, 64);
      t.tex->lpVtbl->UnlockRect(t.tex, 0);
    } else {
      t.tex->lpVtbl->Release(t.tex);
      t.tex = nullptr;
    }
  }
  const uint64_t seen = t.tex ? rbrrtx_texture_hash(t.tex) : 0;   // what the harvest hooks hashed, if they ran
  if (seen && seen != t.hash)
    llog(L"lit parts: twin hash %016llX here, %016llX by the upload hooks: the material may not match",
         static_cast<unsigned long long>(t.hash), static_cast<unsigned long long>(seen));
  g_twins[{ src, signal }] = t;
  *hashOut = t.hash;
  return t.tex;
}

std::wstring car_dir() {
  const size_t slash = g_carFile.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : g_carFile.substr(0, slash);
}

std::wstring mask_names(unsigned m) {
  std::wstring s;
  for (int i = 0; i < kSignals; ++i)
    if (m & (1u << i)) s += (s.empty() ? L"" : L" + ") + std::wstring(kSignalName[i]);
  return s.empty() ? L"nothing" : s;
}

// Signal i flipped between g_side[i] (the last frame it was settled the other way) and this frame (keys sorted). Only
// a clean switch counts: the other signals settled and the same on both sides, at most kSwitchWindow frames apart,
// and the car drawn from the same view (the parts in common >= 75 % of each side; a camera cut or a LOD change swaps
// far more). RBR swaps a lit mesh for its unlit one (e_light_lf -> e_light_lf_lit), so the lit part appears with the
// signal and its unlit twin vanishes.
void switch_seen(int i, const Side& before, const std::vector<uint64_t>& now, const int st[kSignals]) {
  for (int j = 0; j < kSignals; ++j)
    if (j != i && (st[j] == 0 || st[j] != before.st[j])) return;
  if (g_frame - before.frame > kSwitchWindow) return;
  const bool on = st[i] > 0;
  const std::vector<uint64_t>& off = on ? before.keys : now;   // the side with the signal off
  const std::vector<uint64_t>& lit = on ? now : before.keys;   // ... and on
  size_t common = 0;
  for (size_t a = 0, b = 0; a < off.size() && b < lit.size();) {
    if (off[a] < lit[b]) ++a; else if (lit[b] < off[a]) ++b; else { ++common; ++a; ++b; }
  }
  if (common * 4 < off.size() * 3 || common * 4 < lit.size() * 3) { ++g_switchesRefused; return; }
  ++g_switches;
  const unsigned bit = 1u << i;
  auto other_on = [&](const Part& p) {                 // another of its signals is on: this switch says nothing
    for (int j = 0; j < kSignals; ++j) if (j != i && (p.mask & (1u << j)) && st[j] > 0) return true;
    return false;
  };
  for (size_t a = 0, b = 0; a < off.size() || b < lit.size();) {
    const bool inOff = a < off.size() && (b >= lit.size() || off[a] <= lit[b]);
    const bool inLit = b < lit.size() && (a >= off.size() || lit[b] <= off[a]);
    const uint64_t k = inOff ? off[a] : lit[b];
    if (inOff) ++a;
    if (inLit) ++b;
    auto it = g_parts.find(k);
    if (it == g_parts.end()) continue;
    Part& p = it->second;
    if (other_on(p)) continue;                         // (never = no lit part; it can still be the unlit brake lens)
    if (inOff && inLit) {
      ++p.no[i];
      if (p.mask & bit) {
        p.mask &= ~bit;
        llog(L"lit parts: a %u-triangle part stayed drawn when the %s switched: no longer its lit part (now %s)", p.prims,
             kSignalName[i], mask_names(p.mask).c_str());
      }
      if (i == kBrake && p.tail) {
        p.tail = false;
        llog(L"lit parts: a %u-triangle part stayed drawn when the brake switched: no longer an unlit brake lens", p.prims);
      }
    } else if (inLit) {
      ++p.yes[i];
      if (!(p.mask & bit) && !p.no[i] && !p.never && p.prims <= kMaxPrims) {
        p.mask |= bit;
        ++g_learnt;
        llog(L"lit parts: a %u-triangle car part appeared when the %s switched on (or vanished when it switched off): lit "
             L"part of %s", p.prims, kSignalName[i], mask_names(p.mask).c_str());
      }
    } else {
      ++p.partner[i];
      if (i == kBrake && !p.tail && !p.no[i] && !p.everOn[i] && p.prims <= kMaxPrims) {
        p.tail = true;
        llog(L"lit parts: a %u-triangle car part vanished when the brake switched on: the unlit brake lens, it glows as a "
             L"tail lamp while the lamps are on", p.prims);
      }
    }
  }
}

// The frame that just ended, against its signal states (+1 on, -1 off, 0 not settled). A part drawn with ALL signals
// clearly off is an ordinary part for good (RBR's tail lamp lenses show with the lamps AND with the brake, so drawn
// with one of them off proves nothing).
void judge_frame(const int st[kSignals]) {
  if (g_carDrawsThisFrame < 8) { g_drawn.clear(); return; }   // no car this frame (menus, loading)
  std::sort(g_drawn.begin(), g_drawn.end());
  g_drawn.erase(std::unique(g_drawn.begin(), g_drawn.end()), g_drawn.end());
  bool allOff = true;
  for (int i = 0; i < kSignals; ++i) allOff = allOff && st[i] < 0;
  for (auto it = g_parts.begin(); it != g_parts.end();) {
    Part& p = it->second;
    if (p.lastFrame != g_frame) {                       // not drawn for a minute and not lit: forget (dynamic buffers)
      if (!p.mask && !p.tail && g_frame - p.lastFrame > 3600) it = g_parts.erase(it); else ++it;
      continue;
    }
    ++it;
    for (int i = 0; i < kSignals; ++i) if (st[i] > 0) p.everOn[i] = true;
    if (p.tail && p.everOn[kBrake]) {
      p.tail = false;
      llog(L"lit parts: a %u-triangle part was drawn while braking: not an unlit brake lens", p.prims);
    }
    if (allOff && !p.never) {
      if (p.mask)
        llog(L"lit parts: a %u-triangle part of %s was drawn with brake, lamps and reverse all off: no longer treated as lit",
             p.prims, mask_names(p.mask).c_str());
      p.mask = 0;
      p.never = true;
    }
  }
  for (int i = 0; i < kSignals; ++i) {
    if (!st[i]) continue;
    Side& s = g_side[i];
    if (s.valid && s.st[i] == -st[i]) switch_seen(i, s, g_drawn, st);
    s.valid = true;
    s.frame = g_frame;
    std::memcpy(s.st, st, sizeof(s.st));
    s.keys = g_drawn;
  }
  g_drawn.clear();
}

// Brake disc heat, per Present (user 2026-11-02): the pedal x speed heats, time and the air flow cool. The glow is a step of
// the heat above discStart (kDiscLevels steps; each is a twin and a material of its own, so no material changes while playing).
void update_heat(float brake) {
  float r[20];
  const ULONGLONG now = GetTickCount64();
  if (g_cfg.discGlow <= 0.0f || !rbrrtx_player_car_raw(r)) { g_heatTick = 0; g_discLevel = 0; return; }
  if (g_heatTick && now - g_heatTick < 20) return;       // GetTickCount64 steps ~16 ms: a speed from two close ticks is noise
  const float* pos = r + 16;
  if (g_heatTick) {
    const float real = static_cast<float>(now - g_heatTick) * 0.001f;
    const float d = std::sqrt((pos[0] - g_lastPos[0]) * (pos[0] - g_lastPos[0]) + (pos[1] - g_lastPos[1]) * (pos[1] - g_lastPos[1]) +
                              (pos[2] - g_lastPos[2]) * (pos[2] - g_lastPos[2]));
    if (real < 0.5f && d / real < 100.0f) g_kmh += (d / real * 3.6f - g_kmh) * 0.3f;   // a teleport or a long gap is no speed
    const float dt = (std::min)(real, 0.1f);
    const float v = g_kmh / 100.0f;
    g_heat += brake * v * g_cfg.discRate * dt;
    g_heat -= g_heat * (1.0f + v) / g_cfg.discCool * dt;
    g_heat = std::clamp(g_heat, 0.0f, 3.0f);
  }
  g_heatTick = now;
  std::memcpy(g_lastPos, pos, sizeof(g_lastPos));
  const float frac = std::clamp((g_heat - g_cfg.discStart) / (g_cfg.discFull - g_cfg.discStart), 0.0f, 1.0f);
  const int level = frac <= (g_discLevel ? 0.0f : 0.02f) ? 0 : std::clamp(static_cast<int>(std::ceil(frac * kDiscLevels - 0.001f)), 1, kDiscLevels);
  if ((level > 0) != (g_discLevel > 0))
    llog(L"lit parts: brake discs %s (heat %.2f, %.0f km/h)", level ? L"start to glow" : L"cooled below the glow", g_heat, g_kmh);
  g_discLevel = level;
}

// A brake disc part seen: all its heat steps are made known to the generated mod now, not at the first hot moment (a twin
// shows as noise until Remix has loaded its material).
void queue_disc_levels(IDirect3DDevice9* dev, Part& p) {
  IDirect3DBaseTexture9* t0 = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t0);
  if (!t0) return;
  const uint64_t src = rbrrtx_texture_hash(t0);
  D3DSURFACE_DESC d = {};
  const bool is2d = t0->lpVtbl->GetType(t0) == D3DRTYPE_TEXTURE &&
                    SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(t0)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(t0), 0, &d));
  t0->lpVtbl->Release(t0);
  if (!src || !is2d) return;
  std::lock_guard<std::mutex> g(g_mx);
  for (int k = kDisc; k < kDisc + kDiscLevels; ++k) {
    const uint64_t th = twin_hash(src, k);
    bool queued = g_entries.count(th) != 0;
    for (const Pending& q : g_pending) queued = queued || q.twin == th;
    if (!queued) g_pending.push_back({ th, src, k, car_dir(), d.Width, d.Height });
  }
  p.discQueued = true;
}

void forget_car() {
  g_heat = 0.0f;
  g_discLevel = 0;
  g_heatTick = 0;
  g_parts.clear();
  g_drawn.clear();
  g_drawnAt.clear();
  for (Side& s : g_side) s = Side();
}

}  // namespace

// Before Remix starts (d3d9_proxy.cpp, after rbrrtx_genmod_init): settings, and the entries of earlier runs.
void rbrrtx_litparts_startup() {
  load_cfg();
  const wchar_t* dir = rbrrtx_genmod_dir();
  if (!dir || !dir[0]) return;
  g_cacheFile = std::wstring(dir) + L"\\litparts.txt";
  std::ifstream in(g_cacheFile);
  std::lock_guard<std::mutex> g(g_mx);
  std::string first;
  std::getline(in, first);
  while (!first.empty() && (first.back() == '\r' || first.back() == ' ')) first.pop_back();
  const bool current = first == kCacheTag;               // an older cache (other signal numbers, old disc materials) is dropped
  for (std::string line; current && std::getline(in, line);) {
    unsigned long long tw = 0, src = 0;
    int sig = -1, used = 0;
    if (sscanf_s(line.c_str(), "%llx %llx %d %n", &tw, &src, &sig, &used) < 3 || sig < 0 || sig >= kKinds || !used) continue;
    std::string path = line.substr(static_cast<size_t>(used));
    while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
    wchar_t w[MAX_PATH * 2] = {};
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w, MAX_PATH * 2);
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES || twin_hash(src, sig) != tw) continue;
    g_entries[tw] = { tw, src, sig, w };
  }
  if (!g_cfg.enabled) g_entries.clear();
  publish_locked();
  llog(L"lit parts: %s, glow brake lens %.1f, discs up to %.1f (heat rate %.3f/s at 100 km/h, cooling %.0f s, glow from heat %.2f to %.2f) "
       L"/ lamps %.1f, by day %.1f (sun from %.0f deg) / reverse %.1f / "
       L"tail (unlit brake lens, lamps on) %.1f; %zu known from earlier runs", g_cfg.enabled ? L"on" : L"OFF (materials.ini)",
       g_cfg.glow[kBrake], g_cfg.discGlow, g_cfg.discRate, g_cfg.discCool, g_cfg.discStart, g_cfg.discFull, g_cfg.glow[kLamps], g_cfg.glow[kLampsDay], g_cfg.dayAbove, g_cfg.glow[kReverse],
       g_cfg.glow[kTail], g_entries.size());
}

// Per Present. carFile = the player's car model (.sgc) path, empty off a stage. lamps = the game's lamp switch as last
// sent (1 on, 0 off, -1 unknown / not ours).
void rbrrtx_litparts_frame(const wchar_t* carFile, int lamps) {
  if (!g_cfg.enabled) return;
  if (!carFile || !carFile[0]) { if (!g_carFile.empty()) { g_carFile.clear(); forget_car(); } return; }
  if (g_carFile != carFile) { g_carFile = carFile; forget_car(); }
  float brake = 0.0f;
  int gear = 1;
  const bool br = rbrrtx_player_car_brake(&brake), gr = rbrrtx_player_car_gear(&gear);
  const int raw[kSignals] = { !br ? 0 : brake > 0.3f ? 1 : brake < 0.01f ? -1 : 0, lamps > 0 ? 1 : lamps == 0 ? -1 : 0,
                              !gr ? 0 : gear == 0 ? 1 : -1 };
  int before[kSignals];
  for (int i = 0; i < kSignals; ++i) {
    before[i] = g_state[i];
    g_same[i] = raw[i] == g_raw[i] ? g_same[i] + 1 : 0;
    g_raw[i] = raw[i];
    g_state[i] = g_same[i] >= kSettle ? raw[i] : 0;
  }
  // The frame that just ended, judged now: a signal counts only if it was settled the same way at both of its ends.
  // The lamp key is handled after this call in the same Present (comp.cpp end_frame), so the game drew the lit lenses
  // one frame before the state said "on": judged from the frame's start alone, toggling L unlearnt the head lamp lenses
  // (run 20261030-051950, 05:24:27).
  int st[kSignals];
  for (int i = 0; i < kSignals; ++i) st[i] = before[i] == g_state[i] ? g_state[i] : 0;
  judge_frame(st);
  update_heat(br ? brake : 0.0f);
  g_sunEl = rbrrtx_sun_shown_elevation();
  float body[16];                                      // lens or disc: drawn with the body's matrix or not
  if (!g_drawnAt.empty() && rbrrtx_car_body_draw(body))
    for (const DrawnAt& d : g_drawnAt) {
      auto it = g_parts.find(d.key);
      if (it == g_parts.end()) continue;
      // Turned against the body = spins or steers with a wheel: a disc. Only the axes count (run 20261101-111428, Fiat
      // 126p: its brake lens is a node of its own, offset from the body's origin but never turned, and the old
      // position test made it a disc: no glow for the first 1.5 s of braking, "taillights go off when braking").
      float turn = 0.0f;
      for (int k = 0; k < 3; ++k)
        turn = (std::max)(turn, std::fabs(d.m[k] - body[k]) + std::fabs(d.m[4 + k] - body[4 + k]) +
                                std::fabs(d.m[8 + k] - body[8 + k]));
      Part& p = it->second;
      const bool wasDisc = p.disc();
      if (turn < 1e-3f) ++p.onBody; else ++p.offBody;
      if (p.disc() != wasDisc && (p.mask & (1u << kBrake)))
        llog(L"lit parts: a %u-triangle brake part is %s", p.prims, p.disc()
             ? L"drawn off the body (moves with a wheel): a brake disc, it glows by its heat"
             : L"drawn with the body: a brake lamp lens, it lights at once");
      else if (p.wheelPart() && !p.disc() && p.offBody == 3 && (p.mask & (1u << kBrake)))
        llog(L"lit parts: a %u-triangle brake part turns with a wheel but is no disc (60..800 triangles): no glow", p.prims);
    }
  g_drawnAt.clear();
  ++g_frame;
  g_carDrawsThisFrame = 0;
  if (!g_workerBusy) {
    bool work;
    {
      std::lock_guard<std::mutex> g(g_mx);
      work = !g_pending.empty();
    }
    if (work) {
      g_workerBusy = true;
      std::thread(resolve_pending).detach();
    }
  }
}

// A car draw (DrawIndexedPrimitive, primary target, Remix on). Returns the texture to bind at stage 0 for it (a lit
// part's twin), else nullptr.
void* rbrrtx_litparts_draw(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims,
                           const float* world) {
  if (!g_cfg.enabled || g_carFile.empty()) return nullptr;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  ++g_carDrawsThisFrame;
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  UINT off = 0, stride = 0;
  dev->lpVtbl->GetStreamSource(dev, 0, &vb, &off, &stride);
  dev->lpVtbl->GetIndices(dev, &ib);
  uint64_t key = mix(mix(mix(reinterpret_cast<uintptr_t>(vb), reinterpret_cast<uintptr_t>(ib)), off),
                     mix(mix(static_cast<uint64_t>(static_cast<uint32_t>(base)), minIdx), mix(mix(numV, startIdx), prims)));
  if (vb) vb->lpVtbl->Release(vb);
  if (ib) ib->lpVtbl->Release(ib);
  Part& p = g_parts[key];
  p.lastFrame = g_frame;                               // judged at the next Present (judge_frame)
  p.prims = prims;
  g_drawn.push_back(key);
  if (world && ((p.mask & (1u << kBrake)) || p.tail) && p.onBody + p.offBody < 600)   // lens or disc (litparts_frame)
    g_drawnAt.push_back({ key, { world[0], world[1], world[2], world[3], world[4], world[5], world[6], world[7],
                                 world[8], world[9], world[10], world[11] } });
  // Which glow: the signal that shows it now (the brake over the lamps on a shared tail lens), else the unlit brake
  // lens as a tail lamp while the lamps are on. g_raw = the last Present's raw states. A brake disc glows by its heat
  // (update_heat; until it is hot it is drawn as the game textures it).
  // Nothing glows from a brake part before it is judged (4 frames of body matrix: a lens, a wheel: a disc), and nothing from
  // a part that turns with a wheel but is no disc (a big assembly), or from an unlit "lens" of that kind or too big for a lens:
  // its glow would be the atlas colour (grey-blue metal) over the whole part.
  if ((p.mask & (1u << kBrake)) && (!p.settled() || (p.wheelPart() && !p.disc()) || (!p.wheelPart() && p.prims > kLensMaxPrims))) return nullptr;
  const bool isDisc = (p.mask & (1u << kBrake)) && p.disc();
  if (isDisc) {
    if (g_cfg.discGlow <= 0.0f) return nullptr;
    if (!p.discQueued) queue_disc_levels(dev, p);
    if (!g_discLevel) return nullptr;
  }
  int kind = -1;
  for (int i : { kBrake, kReverse, kLamps }) if (kind < 0 && (p.mask & (1u << i)) && g_raw[i] > 0) kind = i;
  if (kind < 0 && p.mask) for (int i = 0; i < kSignals; ++i) if (kind < 0 && (p.mask & (1u << i))) kind = i;
  if (kind < 0 && p.tail && g_raw[kLamps] > 0 && g_raw[kBrake] <= 0 && p.settled() && !p.wheelPart() && p.prims <= kLensMaxPrims) kind = kTail;
  if (kind == kLamps && g_sunEl >= g_cfg.dayAbove) kind = kLampsDay;
  if (kind == kBrake && p.disc()) kind = kDisc + g_discLevel - 1;
  if (kind < 0 || g_cfg.glow[kind] <= 0.0f) return nullptr;
  IDirect3DBaseTexture9* t0 = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t0);
  if (!t0) return nullptr;
  const uint64_t src = rbrrtx_texture_hash(t0);
  D3DSURFACE_DESC d = {};
  const bool is2d = t0->lpVtbl->GetType(t0) == D3DRTYPE_TEXTURE &&
                    SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(t0)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(t0), 0, &d));
  t0->lpVtbl->Release(t0);
  if (!src || !is2d) { if (++g_noHash == 1) llog(L"lit parts: a lit part's texture has no known hash: not made to glow"); return nullptr; }
  uint64_t th = 0;
  IDirect3DTexture9* twin = make_twin(dev, src, kind, &th);
  if (!twin) return nullptr;
  bool published = true;
  {
    std::lock_guard<std::mutex> g(g_mx);
    if (!g_entries.count(th)) {
      published = false;
      bool queued = false;
      for (const Pending& q : g_pending) queued = queued || q.twin == th;
      if (!queued) g_pending.push_back({ th, src, kind, car_dir(), d.Width, d.Height });
    }
  }
  if (kind >= kDisc && !published) return nullptr;     // a disc waits for its material (no noise-textured disc)
  ++g_swapped;
  return twin;
}

void rbrrtx_litparts_status(wchar_t* out, size_t n) {
  size_t entries;
  {
    std::lock_guard<std::mutex> g(g_mx);
    entries = g_entries.size();
  }
  unsigned lit[kKinds] = {};
  for (const auto& kv : g_parts) {
    for (int i = 0; i < kSignals; ++i) if (kv.second.mask & (1u << i)) ++lit[i];
    if (kv.second.tail) ++lit[kTail];
  }
  swprintf_s(out, n, L"lit parts: %s; this car: %zu draws seen, lit parts brake %u / lamps %u / reverse %u / unlit brake "
             L"lenses %u (%u learnt this run; clean switches %u, %u refused as another view); %zu glowing materials; twin draws %u",
             g_cfg.enabled ? L"on" : L"off", g_parts.size(), lit[kBrake], lit[kLamps], lit[kReverse], lit[kTail], g_learnt,
             g_switches, g_switchesRefused, entries, g_swapped);
}

void rbrrtx_litparts_destroy() {
  for (auto& kv : g_twins) if (kv.second.tex) kv.second.tex->lpVtbl->Release(kv.second.tex);
  g_twins.clear();
}
