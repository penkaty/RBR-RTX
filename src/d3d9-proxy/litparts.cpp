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
#include <array>
#include <map>
#include <set>
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
// kLampsOwn: a lamp that glows from its own picture (the mask is the texture), so the lens pattern stays (user 2026-11-09, E30 and aux lamps).
enum { kBrake = 0, kLamps = 1, kReverse = 2, kSignals = 3, kTail = 3, kLampsDay = 4, kDisc = 5, kLampsOwn = kDisc + kDiscLevels, kLampBody = kLampsOwn + 1, kLampGlass = kLampBody + 1, kLampsBright = kLampGlass + 1, kKinds = kLampsBright + 1 };
// kLampsBright: a head lamp part named by the car model (e_light_lf / _rf): only the bright parts of its picture glow (2026-11-13).
// kLampBody: a lit lamp's body on its own lit picture, no glow. kLampGlass: the lamp's flat glass front, the part that glows (user 2026-11-09).
const wchar_t* const kSignalName[kKinds] = { L"brake", L"lamps", L"reverse", L"tail", L"lamps by day", L"brake disc heat 1/6",
                                              L"brake disc heat 2/6", L"brake disc heat 3/6", L"brake disc heat 4/6",
                                              L"brake disc heat 5/6", L"brake disc heat 6/6", L"lamps (own picture)", L"lamp body", L"lamp glass", L"head lamp (bright parts)" };

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
  bool tailLens = false;                                 // the model's tail lens (e_light_lb / _rb): glows with the lamps, braking or not
  int rearRole = -1;                                     // rbrrtx_headlights_rear_lens: -1 not asked yet, 0 none, 1 tail, 2 brake
  unsigned rearTries = 0;
  int head = -1;                                         // rbrrtx_headlights_head_lamp_at: -1 not known yet, 0 no, 1 a head lamp part
  // The car model names this draw as a lamp part (a rear lens role, a head lamp, or a lamp part's exact vertex count): never
  // a wheel part or brake disc, however its matrix moves against the body (Hyundai i20 WRC, user 2026-11-14: its brake
  // lenses were judged to turn with a wheel, so only the halo sprites showed when braking).
  bool named = false;
  unsigned headTries = 0;
  bool never = false;                                    // drawn with every signal off: an ordinary part
  unsigned prims = 0;
  unsigned onBody = 0, offBody = 0;                      // frames drawn with the body's matrix / with another one
  float rel[9] = {}; bool hasRel = false;               // axes minus the body's axes, last frame
  // Turned against the body (a lens never is) = moves with a wheel. A brake disc is a small wheel part: the 220-triangle
  // parts (one per wheel) of the Whaanga run; a 2316-triangle part that turns with a wheel is the whole brake assembly or
  // hub, not a disc (user 2026-11-02: "discs glowed from the very start, bluish"), and a few-triangle part is a lens.
  bool wheelPart() const { return offBody >= 3 && !named; }
  bool disc() const { return wheelPart() && prims >= kDiscMinPrims && prims <= kDiscMaxPrims; }
  bool settled() const { return onBody + offBody >= kJudgeFrames; }   // seen long enough to tell lens from disc
  bool discQueued = false;                               // its heat steps are on their way to the generated mod
  // A lit part's class, read once from its own draw: glass over a lamp stays clear, a rear lamp keeps its own colour.
  int end = 0;                                           // +1 front lamp, -1 rear lamp, 0 not known
  bool blended = false, classed = false, headLogged = false;
  unsigned classTries = 0;
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
bool g_lampSwitchClean = false;                          // this car: a clean switch of the lamp signal was seen
std::wstring g_carFile;

// Signal state per frame: +1 clearly on, -1 clearly off, 0 changing (the game fades lamps over 0.02-0.05 s and its
// own brake threshold is unknown: kSettle frames in a row first).
int g_state[kSignals] = {};
int g_raw[kSignals] = {};
unsigned g_same[kSignals] = {};
// Triangle counts of this car's brake discs. Its unlit disc mesh has the same count and vanishes on braking like an unlit
// lens: on a standing wheel it looked like a tail lens and glowed until the brake was pressed (user 2026-11-08, Focus WRC).
std::vector<unsigned> g_discPrims;

// Twins and the materials for them.
struct Twin { IDirect3DTexture9* tex = nullptr; uint64_t hash = 0; };
std::map<std::pair<uint64_t, int>, Twin> g_twins;        // (atlas hash, signal) -> twin
struct Entry { uint64_t twin, src; int signal; std::wstring file; unsigned readyFrame = 0; };   // readyFrame: Remix has had time to load its material
std::map<uint64_t, Entry> g_entries;                     // twin hash -> material (this run + earlier runs)
std::mutex g_mx;                                         // g_entries, g_pending (worker thread)
struct Pending { uint64_t twin, src; int signal; std::wstring carDir; UINT w, h; };
bool g_carLitAtlas = false;                              // this car's lamps have a lit picture of their own (its body draws say so)
std::vector<Pending> g_pending;
std::atomic<bool> g_workerBusy{ false };
std::wstring g_cacheFile;
unsigned g_learnt = 0, g_swapped = 0, g_noHash = 0;

// Cars with a <name>_lit.dds lamp atlas (BMW M3 E30): the game draws it always (Predicate = NotUsed), so with the lamps off
// the same mesh gets the unlit <name>.dds. The unlit texture object is taken from the car's own draws of it.
struct LitAtlas { uint64_t unlit; IDirect3DBaseTexture9* unlitTex; IDirect3DBaseTexture9* litTex; };   // held while the car is
std::map<uint64_t, LitAtlas> g_litAtlas;                 // lit atlas hash -> its unlit sibling

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
  c.glow[kLampsOwn] = std::clamp(f(L"lamp_glow_own", c.glow[kLamps]), 0.0f, 10000.0f);
  c.glow[kLampBody] = 1.0f;                              // only has to pass the "is it on" tests: its material has no emission
  c.glow[kLampGlass] = c.glow[kLampsOwn];
  c.glow[kLampsBright] = std::clamp(f(L"head_lamp_glow", 0.3f), 0.0f, 10000.0f);
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

// The emission mask of a head lamp lens: one solid colour, so the lamp lights up whatever its texture looks like (carbon, painted,
// chrome: user 2026-11-08). Written once per run.
constexpr float kLampRgb[3] = { 1.0f, 0.93f, 0.82f };
std::wstring lamp_dds() {
  const wchar_t* dir = rbrrtx_genmod_dir();
  if (!dir || !dir[0]) return std::wstring();
  const std::wstring path = std::wstring(dir) + L"\\litparts_lamp.dds";
  static bool done = false;
  if (done) return path;
  const uint32_t argb = 0xFF000000u | (static_cast<uint32_t>(std::lround(kLampRgb[0] * 255.0f)) << 16) |
                        (static_cast<uint32_t>(std::lround(kLampRgb[1] * 255.0f)) << 8) | static_cast<uint32_t>(std::lround(kLampRgb[2] * 255.0f));
  uint32_t h[32] = {};
  std::memcpy(h, "DDS ", 4);
  h[1] = 124;
  h[2] = 0x100F;
  h[3] = 4; h[4] = 4; h[5] = 16;
  h[7] = 1;
  h[19] = 32; h[20] = 0x41; h[22] = 32;
  h[23] = 0x00FF0000u; h[24] = 0x0000FF00u; h[25] = 0x000000FFu; h[26] = 0xFF000000u;
  h[27] = 0x1000;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(h), 128);
  for (int i = 0; i < 16; ++i) f.write(reinterpret_cast<const char*>(&argb), 4);
  if (!f) return std::wstring();
  done = true;
  return path;
}

// Lamp glass: the atlas's brightness in a near-white tint, so the lens pattern shows and the glow is not the picture's colour.
std::wstring glass_mask_path(uint64_t src) {
  const wchar_t* dir = rbrrtx_genmod_dir();
  if (!dir || !dir[0]) return std::wstring();
  wchar_t name[48];
  swprintf_s(name, L"\\litparts_glass_%016llX.dds", static_cast<unsigned long long>(src));
  return std::wstring(dir) + name;
}

std::wstring bright_mask_path(uint64_t src) {
  const wchar_t* dir = rbrrtx_genmod_dir();
  if (!dir || !dir[0]) return std::wstring();
  wchar_t name[48];
  swprintf_s(name, L"\\litparts_bright_%016llX.dds", static_cast<unsigned long long>(src));
  return std::wstring(dir) + name;
}

std::string material_for(const Entry& e) {
  const std::string a = rbrrtx_genmod_asset(e.file);
  char glow[64];
  sprintf_s(glow, "%.3f", g_cfg.glow[e.signal]);
  // A heat step's emission mask is its own colour file, not the atlas (lens and lamp glow keep the atlas as the mask).
  std::string mask = a;
  const bool heat = e.signal >= kDisc && e.signal < kDisc + kDiscLevels;   // (2026-11-13: the later kinds were taken for heat steps)
  if (e.signal == kLampsBright) {
    const std::wstring bm = bright_mask_path(e.src);
    if (!bm.empty() && GetFileAttributesW(bm.c_str()) != INVALID_FILE_ATTRIBUTES) mask = rbrrtx_genmod_asset(bm);
  } else if (heat) {
    const std::wstring hd = heat_dds(e.signal);
    if (!hd.empty()) mask = rbrrtx_genmod_asset(hd);
  } else if (e.signal == kLamps) {
    const std::wstring ld = lamp_dds();
    if (!ld.empty()) mask = rbrrtx_genmod_asset(ld);
  } else if (e.signal == kLampGlass) {
    const std::wstring gm = glass_mask_path(e.src);
    if (!gm.empty() && GetFileAttributesW(gm.c_str()) != INVALID_FILE_ATTRIBUTES) mask = rbrrtx_genmod_asset(gm);
  } else if (e.signal == kLampBody) {
    char c[96];
    sprintf_s(c, "lit lamp body, atlas %016llX:", static_cast<unsigned long long>(e.src));
    return rbrrtx_genmod_material(e.twin, std::string(c) + " " + a,
                                  "                asset inputs:diffuse_texture = @" + a + "@ (\n"
                                  "                    colorSpace = \"auto\"\n"
                                  "                )\n"
                                  "                bool inputs:enable_emission = 0\n");
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
  if (heat) {                                            // the same colour as the constant, whichever of the two the material takes
    float c[3];
    heat_rgb(e.signal, c);
    char col[120];
    sprintf_s(col, "                color3f inputs:emissive_color_constant = (%.3f, %.3f, %.3f)\n", c[0], c[1], c[2]);
    in2 += col;
  }
  if (e.signal == kLamps) {
    char col[120];
    sprintf_s(col, "                color3f inputs:emissive_color_constant = (%.3f, %.3f, %.3f)\n", kLampRgb[0], kLampRgb[1], kLampRgb[2]);
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
    if (g_cfg.glow[e.signal] <= 0.0f || e.signal == kLampsDay) continue;   // one lamp material (user 2026-11-08), old by-day entries dropped
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
    if (p.signal == kLampsBright) {
      const std::wstring bm = bright_mask_path(p.src);
      if (bm.empty() || !rbrrtx_dds_bright_mask(found->path.c_str(), bm.c_str()))
        llog(L"lit parts: FAILED to write the bright-parts mask of atlas %016llX: the whole head lamp part glows", static_cast<unsigned long long>(p.src));
    }
    if (p.signal == kLampGlass) {
      const std::wstring gm = glass_mask_path(p.src);
      if (gm.empty() || !rbrrtx_dds_neutral_mask(found->path.c_str(), gm.c_str()))
        llog(L"lit parts: FAILED to write the glass mask of atlas %016llX: it glows in the atlas's colours", static_cast<unsigned long long>(p.src));
    }
    std::lock_guard<std::mutex> g(g_mx);
    // Not drawn for 6 s: written within 4 s (genmod kMinGap), then Remix loads it. Until then the draw stays as the game made it (no noise).
    g_entries[p.twin] = { p.twin, p.src, p.signal, found->path, g_frame + 360 };
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
  if (i == kLamps) g_lampSwitchClean = true;
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

// The centre of a draw's vertices (a sample) through its world matrix.
bool draw_centre(IDirect3DDevice9* dev, int base, unsigned minIdx, unsigned numV, const float* world, float w[3]) {
  if (!world || !numV) return false;
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT off = 0, stride = 0;
  dev->lpVtbl->GetStreamSource(dev, 0, &vb, &off, &stride);
  if (!vb) return false;
  float c[3] = {};
  void* q = nullptr;
  const INT first = base + static_cast<INT>(minIdx);
  bool ok = false;
  if (stride >= 12 && first >= 0 && SUCCEEDED(vb->lpVtbl->Lock(vb, off + static_cast<UINT>(first) * stride, numV * stride, &q, D3DLOCK_READONLY)) && q) {
    const unsigned step = (std::max)(1u, numV / 64);
    unsigned n = 0;
    for (unsigned v = 0; v < numV; v += step, ++n) {
      float xyz[3];
      std::memcpy(xyz, static_cast<const char*>(q) + static_cast<size_t>(v) * stride, 12);
      for (int k = 0; k < 3; ++k) c[k] += xyz[k];
    }
    vb->lpVtbl->Unlock(vb);
    for (float& a : c) a /= n;
    ok = true;
  }
  vb->lpVtbl->Release(vb);
  if (!ok) return false;
  w[0] = c[0] * world[0] + c[1] * world[4] + c[2] * world[8] + world[12];
  w[1] = c[0] * world[1] + c[1] * world[5] + c[2] * world[9] + world[13];
  w[2] = c[0] * world[2] + c[1] * world[6] + c[2] * world[10] + world[14];
  return true;
}

// Where a lit part sits: its vertices (a sample) through the draw's matrix, against the car's lamps. Blending from the draw.
void class_part(IDirect3DDevice9* dev, Part& p, int base, unsigned minIdx, unsigned numV, const float* world) {
  DWORD blend = FALSE;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  p.blended = blend != FALSE;
  if (++p.classTries > 120) { p.classed = true; return; }  // lamps of the model never known: it stays a front lamp
  float w[3];
  if (!draw_centre(dev, base, minIdx, numV, world, w)) return;
  p.end = rbrrtx_headlights_lamp_end(w);
  if (!p.end) return;
  p.classed = true;
  // Equal vertex counts matched small parts at the front of the car too (run 20261109-180308): a brake lens sits at the rear.
  if (p.end > 0 && p.tail && !p.everOn[kBrake]) { p.tail = false; }
  llog(L"lit parts: a %u-triangle lit part is at the %s of the car%s", p.prims, p.end > 0 ? L"front" : L"rear", p.blended ? L", blended" : L"");
}

// The twin for (texture, kind), once Remix has its material.
RbrLitDecision g_dec;                                   // rbrrtx_litparts_decision: the draw just passed

void* twin_for(IDirect3DDevice9* dev, uint64_t src, int kind, UINT w, UINT h) {
  if (kind >= 0 && kind < kKinds) {                      // the look, whether or not the twin is ready
    g_dec.kind = kind;
    g_dec.glow = kind == kLampBody ? 0.0f : g_cfg.glow[kind];   // the lamp body: an "on" marker, no emission
    std::wstring m;
    if (kind >= kDisc && kind < kDisc + kDiscLevels) m = heat_dds(kind);   // a hot disc: its heat colour, not the atlas
    else if (kind == kLampsBright) m = bright_mask_path(src);
    else if (kind == kLampGlass) m = glass_mask_path(src);
    else if (kind == kLamps) m = lamp_dds();
    g_dec.mask = !m.empty() && GetFileAttributesW(m.c_str()) != INVALID_FILE_ATTRIBUTES ? m : std::wstring();
  }
  uint64_t th = 0;
  IDirect3DTexture9* twin = make_twin(dev, src, kind, &th);
  if (!twin) return nullptr;
  bool published = true;
  {
    std::lock_guard<std::mutex> g(g_mx);
    auto en = g_entries.find(th);
    if (en != g_entries.end()) {
      published = g_frame >= en->second.readyFrame;
    } else {
      published = false;
      bool queued = false;
      for (const Pending& q : g_pending) queued = queued || q.twin == th;
      if (!queued) g_pending.push_back({ th, src, kind, car_dir(), w, h });
    }
  }
  if (!published) return nullptr;                        // every kind waits for its material (no noise on the part)
  ++g_swapped;
  return twin;
}

// A glowing lens moves a few millimetres out of the car, in front of a coplanar lens or body panel it would z-fight with.
constexpr float kLitNudge = 0.005f;
float g_nudge[3] = {};
bool g_haveNudge = false;
void note_nudge(int kind, int end) {
  g_haveNudge = false;
  float f[3];
  if (kind >= kDisc || !end || !rbrrtx_headlights_forward(f)) return;
  for (int i = 0; i < 3; ++i) g_nudge[i] = f[i] * static_cast<float>(end) * kLitNudge;
  g_haveNudge = true;
}

void forget_car() {
  g_carLitAtlas = false;
  for (auto& kv : g_litAtlas) {
    if (kv.second.unlitTex) { kv.second.unlitTex->lpVtbl->Release(kv.second.unlitTex); kv.second.unlitTex = nullptr; }
    if (kv.second.litTex) { kv.second.litTex->lpVtbl->Release(kv.second.litTex); kv.second.litTex = nullptr; }
  }
  g_heat = 0.0f;
  g_discLevel = 0;
  g_heatTick = 0;
  g_parts.clear();
  g_drawn.clear();
  g_drawnAt.clear();
  g_discPrims.clear();
  g_lampSwitchClean = false;
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
    if (sig == kLampsDay || GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES || twin_hash(src, sig) != tw) continue;
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
      // A lens can sit tilted on its own node (Lancia Delta, run 20261102-234217: its brake lens was taken for a disc and
      // stayed dark until "hot"), so a fixed offset is no wheel: only a CHANGE of the turn against the body between
      // frames (spinning or steering) counts. A standing car's disc is not hot, so nothing is lost while still.
      // The part's rotation relative to the body's, M x B^-1 (rows: row-vector matrices, B a rotation times a scale): fixed
      // for anything on the car, however the car turns. The old measure, M - B, changed with every turn of the car for a
      // node tilted against the body, and such lamps were taken for wheels on a winding road (Hyundai i20 WRC brake lenses,
      // user 2026-11-14: only the halo sprites when braking).
      Part& p = it->second;
      float turn = 0.0f, rel[9];
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
          const float s2 = body[k * 4] * body[k * 4] + body[k * 4 + 1] * body[k * 4 + 1] + body[k * 4 + 2] * body[k * 4 + 2];
          rel[r * 3 + k] = (d.m[r * 4] * body[k * 4] + d.m[r * 4 + 1] * body[k * 4 + 1] + d.m[r * 4 + 2] * body[k * 4 + 2]) /
                           (s2 > 1e-12f ? s2 : 1.0f);
          if (p.hasRel) turn = (std::max)(turn, std::fabs(rel[r * 3 + k] - p.rel[r * 3 + k]));
        }
      memcpy(p.rel, rel, sizeof rel);
      p.hasRel = true;
      const bool wasDisc = p.disc();
      if (turn < 1e-3f) ++p.onBody; else ++p.offBody;
      if ((p.mask & (1u << kBrake)) && p.disc() && std::find(g_discPrims.begin(), g_discPrims.end(), p.prims) == g_discPrims.end())
        g_discPrims.push_back(p.prims);
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
bool rbrrtx_litparts_nudge(float out[3]) {
  if (!g_haveNudge) return false;
  std::memcpy(out, g_nudge, sizeof(g_nudge));
  return true;
}

void* rbrrtx_litparts_draw(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims,
                           const float* world, bool glassShader) {
  g_dec = RbrLitDecision();
  if (!g_cfg.enabled || g_carFile.empty()) return nullptr;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  ++g_carDrawsThisFrame;
  g_haveNudge = false;
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
  // The unlit brake lens glows as a tail lamp from the start: the model names it, without waiting for the first brake press
  // (user 2026-11-09: rear lights only came on after toggling L). A part drawn while braking stops being one (judge_frame).
  // Which rear lens of the model it is: by vertex count, and by place where a tail and a brake lens share the count
  // (Celica TA64: both 14 vertices; the tail lens taken for a brake lens lost its glow at the first brake press).
  // A big lens part is looked up too when its vertex count names it (i20 WRC: e_light_lb, 705 vertices, over the small-lens
  // limit, so its tail lamps never glowed); a place-only match stays with small draws.
  if (p.rearRole < 0) {
    bool needPos = false;
    int role = rbrrtx_headlights_rear_lens(numV, nullptr, &needPos);
    if (needPos && prims > kLensMaxPrims) role = 0;
    else if (needPos) {
      float w[3];
      role = draw_centre(dev, base, minIdx, numV, world, w) ? rbrrtx_headlights_rear_lens(numV, w, nullptr) : -1;
      if (role < 0 && ++p.rearTries > 300) role = 0;   // never placed: no role
    }
    if (role >= 0) {
      p.rearRole = role;
      if (role == 1 && !p.tailLens) {
        p.tailLens = true;
        llog(L"lit parts: a %u-triangle part is a tail lamp lens by the car's model%s: it glows while the lamps are on, braking or not",
             prims, needPos ? L" and its place" : L"");
      }
    }
  }
  if (!p.named && (p.rearRole > 0 || p.head > 0 || rbrrtx_headlights_is_lamp_count(numV))) p.named = true;
  if (p.head < 0) {                                      // a head lamp part of the model: by vertex count, or by place
    bool needPos = false;
    int head = rbrrtx_headlights_head_lamp_at(numV, nullptr, &needPos);
    if (needPos) {
      float w[3];
      head = draw_centre(dev, base, minIdx, numV, world, w) ? rbrrtx_headlights_head_lamp_at(numV, w, nullptr) : -1;
      if (head < 0 && ++p.headTries > 300) head = 0;
      if (head > 0) llog(L"lit parts: a %u-triangle draw (%u vertices) is a face block of a head lamp part by its place", prims, numV);
    }
    p.head = head;
  }
  const bool headLamp = p.head > 0;
  if (!p.tail && !p.mask && !p.everOn[kBrake] && p.end <= 0 && prims <= kLensMaxPrims && p.rearRole == 2) {
    p.tail = true;
    llog(L"lit parts: a %u-triangle part is an unlit brake lens by the car's model: it glows as a tail lamp while the lamps are on", prims);
  }
  if ((p.mask || p.tail) && !p.classed) class_part(dev, p, base, minIdx, numV, world);
  g_dec.glass = glassShader;
  // See-through glass = the glass shader AND blending. A car's CarLight face blocks without a shader of their own are
  // drawn opaque with CarGlass.vso (Yaris Rally2, dump 21:06: head lamp insides and rear lenses): they are the lamp,
  // not its glass, and glow (user 2026-11-13: Yaris rear lamps dark, front lamps half lit).
  DWORD blendNow = FALSE;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blendNow);
  const bool clearGlass = glassShader && blendNow;
  g_dec.disc = p.wheelPart() || p.disc();
  g_dec.lampPart = !g_dec.disc && prims <= kLensMaxPrims * 40 &&
                   (p.tail || p.tailLens || p.rearRole > 0 || (p.mask && !p.never) || headLamp ||
                    rbrrtx_headlights_is_lamp_glass(numV) || rbrrtx_headlights_is_aux(numV) || rbrrtx_headlights_is_aux_glass(numV));
  if (!g_litAtlas.empty()) {                             // the always-drawn lit lamp atlas follows the lamps
    IDirect3DBaseTexture9* cur = nullptr;
    dev->lpVtbl->GetTexture(dev, 0, &cur);
    if (cur) {
      const uint64_t h = rbrrtx_texture_hash(cur);
      uint64_t litSrc = 0;                               // the lit atlas of this draw: its own, or its unlit sibling's
      for (auto& kv : g_litAtlas)
        if (kv.second.unlit == h) {
          if (!kv.second.unlitTex) { cur->lpVtbl->AddRef(cur); kv.second.unlitTex = cur; }
          litSrc = kv.first;
        }
      const auto lit = g_litAtlas.find(h);
      if (lit != g_litAtlas.end()) {
        litSrc = h;
        if (!lit->second.litTex) { cur->lpVtbl->AddRef(cur); lit->second.litTex = cur; }
      }
      D3DSURFACE_DESC ad = {};
      if (litSrc && cur->lpVtbl->GetType(cur) == D3DRTYPE_TEXTURE) reinterpret_cast<IDirect3DTexture9*>(cur)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(cur), 0, &ad);
      cur->lpVtbl->Release(cur);
      // A brake or reverse part has its own signal (run 20261109-180308: the E30's lit brake mesh got the unlit texture with the lamps off).
      if (litSrc && !(p.mask & ((1u << kBrake) | (1u << kReverse)))) {
        if (g_raw[kLamps] < 0) {
          // The API path draws this texture instead of the bound one (comp.cpp, RbrLampLook::texture).
          if (lit != g_litAtlas.end() && lit->second.unlitTex) { g_dec.texture = lit->second.unlitTex; return lit->second.unlitTex; }
        } else if (g_raw[kLamps] > 0 && ad.Width) {
          // These cars' lamp meshes never switch, so learning finds nothing and they had no glow at all (the E30's head lamps). Lit and
          // unlit meshes are both drawn in the same place: both get the glow, so they cannot show through each other.
          if (!p.classed) class_part(dev, p, base, minIdx, numV, world);
          const int kind = p.end < 0 ? kTail : kLampBody;
          if (kind == kLampBody) g_carLitAtlas = true;
          if (lit == g_litAtlas.end()) {                   // the unlit sibling drawn: the API draws the lit atlas
            const auto la = g_litAtlas.find(litSrc);
            if (la != g_litAtlas.end() && la->second.litTex) g_dec.texture = la->second.litTex;
          }
          if (g_cfg.glow[kind] > 0.0f)
            if (void* twin = twin_for(dev, litSrc, kind, ad.Width, ad.Height)) { note_nudge(kind, p.end); return twin; }
        }
      }
    }
  }
  if (world && ((p.mask & (1u << kBrake)) || p.tail) && p.onBody + p.offBody < 600)   // lens or disc (litparts_frame)
    g_drawnAt.push_back({ key, { world[0], world[1], world[2], world[3], world[4], world[5], world[6], world[7],
                                 world[8], world[9], world[10], world[11] } });
  // The flat glass front of a lamp glows, not the body behind it (user 2026-11-09, E30: lamps with a lit picture of their own, and
  // auxiliary lamps with glass). A low-alpha glass still glows in full: Remix adds emission before opacity.
  if (glassShader && g_cfg.glow[kLampGlass] > 0.0f && ((g_carLitAtlas && g_raw[kLamps] > 0 && rbrrtx_headlights_is_lamp_glass(numV)) ||
                                                       (rbrrtx_headlights_aux_on() && rbrrtx_headlights_is_aux_glass(numV)))) {
    if (!p.classed) class_part(dev, p, base, minIdx, numV, world);
    IDirect3DBaseTexture9* gt = nullptr;
    dev->lpVtbl->GetTexture(dev, 0, &gt);
    if (gt) {
      const uint64_t gsrc = rbrrtx_texture_hash(gt);
      D3DSURFACE_DESC gd = {};
      const bool ok2d = gt->lpVtbl->GetType(gt) == D3DRTYPE_TEXTURE &&
                        SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(gt)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(gt), 0, &gd));
      gt->lpVtbl->Release(gt);
      if (p.end >= 0 && gsrc && ok2d)
        if (void* twin = twin_for(dev, gsrc, kLampGlass, gd.Width, gd.Height)) return twin;
    }
  }
  // Head lamps by the model's names (e_light_lf / _rf, lit or unlit: the game shows one of the two, and which one is "on"
  // differs per car: Predicate on / off are swapped on the Yaris Rally2 and the Golf). No learning: the Fabia Rally2's were
  // never learnt (run 20261113-050319). The lamp's inner face block, not its glass; only the bright parts of the picture glow.
  if (!clearGlass && g_raw[kLamps] > 0 && g_cfg.glow[kLampsBright] > 0.0f && !(p.mask & ((1u << kBrake) | (1u << kReverse))) &&
      !p.tail && headLamp) {
    if (!p.classed) class_part(dev, p, base, minIdx, numV, world);
    if (p.end > 0) {
      IDirect3DBaseTexture9* ht = nullptr;
      dev->lpVtbl->GetTexture(dev, 0, &ht);
      if (ht) {
        const uint64_t hsrc = rbrrtx_texture_hash(ht);
        D3DSURFACE_DESC hd = {};
        const bool ok2d = ht->lpVtbl->GetType(ht) == D3DRTYPE_TEXTURE &&
                          SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(ht)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(ht), 0, &hd));
        ht->lpVtbl->Release(ht);
        if (hsrc && ok2d) {
          if (!p.headLogged) { p.headLogged = true; llog(L"lit parts: a %u-triangle part is a head lamp by the car's model: its bright parts glow while the lamps are on", prims); }
          return twin_for(dev, hsrc, kLampsBright, hd.Width, hd.Height);   // until its material is loaded: as the game draws it
        }
      }
    }
  }
  // Auxiliary lamps (bumper, bonnet, ramp lights): their meshes never switch, so learning finds nothing. The model names them; they
  // glow while they are on (brights, with extra_lights = 2). Their body only when they have no glass to glow.
  if (!clearGlass && !rbrrtx_headlights_has_aux_glass() && !p.mask && !p.tail && rbrrtx_headlights_aux_on() && rbrrtx_headlights_is_aux(numV) && prims <= kLensMaxPrims * 4) {
    if (!p.classed) class_part(dev, p, base, minIdx, numV, world);
    IDirect3DBaseTexture9* at = nullptr;
    dev->lpVtbl->GetTexture(dev, 0, &at);
    if (at && p.end >= 0) {
      const uint64_t asrc = rbrrtx_texture_hash(at);
      D3DSURFACE_DESC ad = {};
      const bool ok2d = at->lpVtbl->GetType(at) == D3DRTYPE_TEXTURE &&
                        SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(at)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(at), 0, &ad));
      at->lpVtbl->Release(at);
      if (asrc && ok2d && g_cfg.glow[kLampsOwn] > 0.0f)
        if (void* twin = twin_for(dev, asrc, kLampsOwn, ad.Width, ad.Height)) return twin;
    } else if (at) {
      at->lpVtbl->Release(at);
    }
  }
  // Which glow: the signal that shows it now (the brake over the lamps on a shared tail lens), else the unlit brake
  // lens as a tail lamp while the lamps are on. g_raw = the last Present's raw states. A brake disc glows by its heat
  // (update_heat; until it is hot it is drawn as the game textures it).
  // Nothing glows from a brake part before it is judged (4 frames of body matrix: a lens, a wheel: a disc), and nothing from
  // a part that turns with a wheel but is no disc (a big assembly), or from an unlit "lens" of that kind or too big for a lens:
  // its glow would be the atlas colour (grey-blue metal) over the whole part.
  if ((p.mask & (1u << kBrake)) && (!p.settled() || (p.wheelPart() && !p.disc()) || (!p.wheelPart() && p.prims > kLensMaxPrims))) return nullptr;
  const bool isDisc = (p.mask & (1u << kBrake)) && p.disc();
  g_dec.brakeDisc = isDisc;                              // through the API cold or hot (user 2026-11-13: brakes too)
  if (isDisc) {
    if (g_cfg.discGlow <= 0.0f) return nullptr;
    if (!p.discQueued) queue_disc_levels(dev, p);
    if (!g_discLevel) return nullptr;
  }
  int kind = -1;
  for (int i : { kBrake, kReverse, kLamps }) if (kind < 0 && (p.mask & (1u << i)) && g_raw[i] > 0) kind = i;
  if (kind < 0 && p.mask) for (int i = 0; i < kSignals; ++i) if (kind < 0 && (p.mask & (1u << i)) && g_raw[i] == 0) kind = i;   // signal unknown only: known off = no glow
  if (kind < 0 && p.tail && g_raw[kLamps] > 0 && g_raw[kBrake] <= 0 && p.settled() && !p.wheelPart() && p.prims <= kLensMaxPrims &&
      std::find(g_discPrims.begin(), g_discPrims.end(), p.prims) == g_discPrims.end()) kind = kTail;
  // Named and placed: no judging needed, and drawn with every signal off (the Celica draws it always) is no objection.
  if (kind < 0 && p.tailLens && g_raw[kLamps] > 0) kind = kTail;
  if (kind == kBrake && p.disc()) kind = kDisc + g_discLevel - 1;
  // Two kinds of car (user 2026-11-09): with lamp glass of its own the light is inside and the glass stays clear; with lamps
  // painted on, the blended part is all there is, so it glows. Glass means the glass shader: blending alone also marked the
  // E30's lit brake lamps (run 20261109-180308), which then never glowed. A rear lamp keeps its own colour (atlas mask), the
  // solid warm white of a head lamp over a red lens read as hazy white glass.
  if (kind >= 0 && kind < kDisc && clearGlass && rbrrtx_headlights_has_lens()) return nullptr;
  if (kind == kLamps && p.end < 0) kind = kTail;
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
  void* twin = twin_for(dev, src, kind, d.Width, d.Height);
  if (twin) note_nudge(kind, p.end);
  return twin;
}

bool rbrrtx_litparts_lamps_done() { return g_lampSwitchClean; }

const RbrLitDecision& rbrrtx_litparts_decision() { return g_dec; }
void rbrrtx_litparts_drop_twin() { if (g_swapped) --g_swapped; }

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

void rbrrtx_litparts_note_lit_atlas(unsigned long long litHash, const wchar_t* unlitFile) {
  uint64_t unlit = 0;
  UINT w = 0, h = 0;
  if (dds_hash(unlitFile, &unlit, &w, &h)) g_litAtlas[litHash] = { unlit, nullptr, nullptr };
}

void rbrrtx_litparts_destroy() {
  for (auto& kv : g_litAtlas) {
    if (kv.second.unlitTex) { kv.second.unlitTex->lpVtbl->Release(kv.second.unlitTex); kv.second.unlitTex = nullptr; }
    if (kv.second.litTex) { kv.second.litTex->lpVtbl->Release(kv.second.litTex); kv.second.litTex = nullptr; }
  }
  for (auto& kv : g_twins) if (kv.second.tex) kv.second.tex->lpVtbl->Release(kv.second.tex);
  g_twins.clear();
}
