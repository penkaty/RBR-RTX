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
//    input), lamps (the game's lamp switch as headlights.cpp last sent it), reverse (gear R). A fingerprint seen in
//    >= kOnFrames frames while a signal was clearly on, and never while it was clearly off (after kOffFrames such
//    frames with car draws), is that signal's lit part. Seen once while clearly off = never one. Parts bigger than
//    kMaxPrims (a body, an interior) are never lit parts.
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

constexpr unsigned kOnFrames = 6, kOffFrames = 60, kMaxPrims = 4000, kSettle = 12;
enum { kBrake = 0, kLamps = 1, kReverse = 2, kSignals = 3 };
const wchar_t* const kSignalName[kSignals] = { L"brake", L"lamps", L"reverse" };

struct Cfg {
  bool enabled = true;
  float glow[kSignals] = { 12.0f, 20.0f, 15.0f };
} g_cfg;

struct Part {
  unsigned onFrames[kSignals] = {};
  bool offSeen[kSignals] = {};
  unsigned lastFrame = ~0u;
  int lit = -1;                                          // signal it is the lit part of
  bool never = false;                                    // drawn with every signal off: an ordinary part
  unsigned prims = 0;
};
std::unordered_map<uint64_t, Part> g_parts;              // this car's draws
unsigned g_offFrames[kSignals] = {};                     // frames with the signal clearly off and car draws
unsigned g_frame = 0;
unsigned g_carDrawsThisFrame = 0;
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

std::string material_for(const Entry& e) {
  const std::string a = rbrrtx_genmod_asset(e.file);
  char glow[64];
  sprintf_s(glow, "%.3f", g_cfg.glow[e.signal]);
  const std::string in =
      "                asset inputs:diffuse_texture = @" + a + "@ (\n"
      "                    colorSpace = \"auto\"\n"
      "                )\n"
      "                bool inputs:enable_emission = 1\n"
      "                asset inputs:emissive_mask_texture = @" + a + "@ (\n"
      "                    colorSpace = \"auto\"\n"
      "                )\n"
      "                float inputs:emissive_intensity = " + glow + "\n";
  char c[96];
  sprintf_s(c, "lit car part (%ls), atlas %016llX:", kSignalName[e.signal], static_cast<unsigned long long>(e.src));
  return rbrrtx_genmod_material(e.twin, std::string(c) + " " + a, in);
}

// Section "litparts" from every entry, and the cache file. Caller holds g_mx.
void publish_locked() {
  std::string body;
  std::string cache;
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

// Every part drawn in the frame that just ended, against that frame's signal states (+1 on, -1 off, 0 not settled).
// A lit part is dropped only when drawn with ALL signals clearly off: RBR's tail lamp lenses show with the lamps AND
// with the brake, so "drawn with the lamps off" while braking is no reason (the same run unlearnt them as lamp
// parts and learnt them again as brake parts, with another glow).
void judge_frame(const int st[kSignals]) {
  if (g_carDrawsThisFrame < 8) return;                 // no car this frame (menus, loading)
  bool allOff = true;
  for (int i = 0; i < kSignals; ++i) {
    allOff = allOff && st[i] < 0;
    if (st[i] < 0) ++g_offFrames[i];
  }
  for (auto it = g_parts.begin(); it != g_parts.end();) {
    Part& p = it->second;
    if (p.lastFrame != g_frame) {                       // not drawn for a minute and not lit: forget (dynamic buffers)
      if (p.lit < 0 && g_frame - p.lastFrame > 3600) it = g_parts.erase(it); else ++it;
      continue;
    }
    ++it;
    for (int i = 0; i < kSignals; ++i) {
      if (st[i] < 0) p.offSeen[i] = true;
      else if (st[i] > 0) ++p.onFrames[i];
    }
    if (p.lit >= 0 && allOff) {
      llog(L"lit parts: a %u-triangle %s part was drawn with brake, lamps and reverse all off: no longer treated as lit",
           p.prims, kSignalName[p.lit]);
      p.lit = -1;
      p.never = true;
    }
    if (p.lit < 0 && !p.never && p.prims <= kMaxPrims)
      for (int i = 0; i < kSignals; ++i)
        if (!p.offSeen[i] && p.onFrames[i] >= kOnFrames && g_offFrames[i] >= kOffFrames) {
          p.lit = i;
          ++g_learnt;
          llog(L"lit parts: a %u-triangle car part is drawn only with the %s on (%u frames on, none of %u off): lit part",
               p.prims, kSignalName[i], p.onFrames[i], g_offFrames[i]);
          break;
        }
  }
}

void forget_car() {
  g_parts.clear();
  for (unsigned& n : g_offFrames) n = 0;
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
  for (std::string line; std::getline(in, line);) {
    unsigned long long tw = 0, src = 0;
    int sig = -1, used = 0;
    if (sscanf_s(line.c_str(), "%llx %llx %d %n", &tw, &src, &sig, &used) < 3 || sig < 0 || sig >= kSignals || !used) continue;
    std::string path = line.substr(static_cast<size_t>(used));
    while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
    wchar_t w[MAX_PATH * 2] = {};
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w, MAX_PATH * 2);
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES || twin_hash(src, sig) != tw) continue;
    g_entries[tw] = { tw, src, sig, w };
  }
  if (!g_cfg.enabled) g_entries.clear();
  publish_locked();
  llog(L"lit parts: %s, glow brake %.1f / lamps %.1f / reverse %.1f; %zu known from earlier runs",
       g_cfg.enabled ? L"on" : L"OFF (materials.ini)", g_cfg.glow[kBrake], g_cfg.glow[kLamps], g_cfg.glow[kReverse], g_entries.size());
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
void* rbrrtx_litparts_draw(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims) {
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
  if (p.lit < 0) return nullptr;
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
  IDirect3DTexture9* twin = make_twin(dev, src, p.lit, &th);
  if (!twin) return nullptr;
  {
    std::lock_guard<std::mutex> g(g_mx);
    if (!g_entries.count(th)) {
      bool queued = false;
      for (const Pending& q : g_pending) queued = queued || q.twin == th;
      if (!queued) g_pending.push_back({ th, src, p.lit, car_dir(), d.Width, d.Height });
    }
  }
  ++g_swapped;
  return twin;
}

void rbrrtx_litparts_status(wchar_t* out, size_t n) {
  size_t entries;
  {
    std::lock_guard<std::mutex> g(g_mx);
    entries = g_entries.size();
  }
  unsigned lit[kSignals] = {};
  for (const auto& kv : g_parts) if (kv.second.lit >= 0) ++lit[kv.second.lit];
  swprintf_s(out, n, L"lit parts: %s; this car: %zu draws seen, lit parts brake %u / lamps %u / reverse %u (%u learnt this run); "
             L"%zu glowing materials; twin draws %u",
             g_cfg.enabled ? L"on" : L"off", g_parts.size(), lit[kBrake], lit[kLamps], lit[kReverse], g_learnt, entries, g_swapped);
}

void rbrrtx_litparts_destroy() {
  for (auto& kv : g_twins) if (kv.second.tex) kv.second.tex->lpVtbl->Release(kv.second.tex);
  g_twins.clear();
}
