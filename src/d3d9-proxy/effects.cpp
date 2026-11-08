// effects.cpp - every game particle effect through the Remix API, Remix Plus only (docs/sessions/particle-rewrite-2026-11-08.md,
// re/kb/systems/particles.md, DECISIONS 2026-11-08). Was particles.cpp; its run-by-run history is in git.
// Wheel dust and tyre smoke: CEmitter_Emit is called with amount 0 for the dry wheel emitters, which drive the Remix systems per wheel.
// Rain and snow: the game's CEmitterRain pools are 0, one Remix spawner follows the camera. Everything else: one row per effect type
// at the place the game's emitter fired (section "every other game emitter"). Key 0 (raster) gives the game's own back.
// Units: rtx.sceneScale stays 1, so Remix's centimetre values are RBR metres. Physics space = render space with Y and Z swapped.
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <d3d9.h>
#include "third_party/remix_c.h"
#include "menu/menu.hpp"
#include <algorithm>
#include <intrin.h>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <string>

#pragma intrinsic(_ReturnAddress)

bool rbrrtx_surface_lookup_on = false;

namespace {

#define plog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

constexpr uintptr_t kEmit = 0x005DC100, kEmitCall = 0x005DDEB8;
using EmitFn = void(__thiscall*)(void* self, void* arg1, const float* pos, const float* vel, float amount);

// ---------------------------------------------------------------- probe

struct Stat { void* arg1 = nullptr; unsigned calls = 0; double amount = 0; float pos[3] = {}, vel[3] = {}; };
bool g_probe = false, g_hooked = false;
std::unordered_map<void*, Stat> g_stats;
ULONGLONG g_lastReport = 0;
unsigned g_reports = 0, g_totalCalls = 0;
constexpr unsigned kMaxReports = 30;                   // one line per 10 s: ~5 min of driving

bool readable_string(uintptr_t p, char* out, size_t n) {
  __try {
    size_t i = 0;
    for (; i + 1 < n; ++i) {
      const char c = *reinterpret_cast<volatile const char*>(p + i);
      if (!c) break;
      if (c < 0x20 || c > 0x7E) return false;
      out[i] = c;
    }
    out[i] = 0;
    return i >= 4 && i + 1 < n;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int game_emitter_kind(void* arg1);                    // 0 kept, 1 dust, 2 smoke (settings section below)
void note_dust(const float* pos);                      // nearest wheel throws dust now (wheels section below)
bool wheel_dusty(const float* pos);                    // nearest wheel threw dust recently
void note_smoke(const float* pos);                     // nearest wheel slides (game smoke request)

void __fastcall emit_hook(void* self, void* /*edx*/, void* arg1, const float* pos, const float* vel, float amount) {
  if (amount != 0.0f && pos) {                         // cut = never created: no simulation, no draws
    const int kind = game_emitter_kind(arg1);
    if (kind == 1) { note_dust(pos); amount = 0.0f; }
    else if (kind == 2) { if (!wheel_dusty(pos)) note_smoke(pos); amount = 0.0f; }
  }
  if (!g_probe) { reinterpret_cast<EmitFn>(kEmit)(self, arg1, pos, vel, amount); return; }
  ++g_totalCalls;
  Stat& s = g_stats[self];
  s.arg1 = arg1;
  ++s.calls;
  s.amount += amount;
  if (pos) std::memcpy(s.pos, pos, sizeof(s.pos));
  if (vel) std::memcpy(s.vel, vel, sizeof(s.vel));
  reinterpret_cast<EmitFn>(kEmit)(self, arg1, pos, vel, amount);
}

// ---------------------------------------------------------------- wheels (RBR's tire signal)

struct Wheel {
  float p[3] = {};                                     // contact point, physics space
  ULONGLONG t = 0;                                     // last signal
  uint32_t mat = UINT32_MAX, cand = UINT32_MAX;        // surface in use / candidate (settles after kSettleMs)
  ULONGLONG candT = 0;
  ULONGLONG dustT = 0;                                 // last time the game asked for dust at this wheel
  ULONGLONG smokeStart = 0, smokeLast = 0;             // this slide's first and last smoke request (no dust)
};
Wheel g_wheels[4];
constexpr ULONGLONG kSettleMs = 250;
constexpr uintptr_t kTireVtSlot = 0x0074DC58, kTireThunk = 0x005E3C70;
using InvokeFn = void(__thiscall*)(void* conn, const void* sig);
bool g_tireHooked = false;

void __fastcall tire_invoke(void* conn, void* /*edx*/, const uint8_t* sig) {
  const uint32_t w = *reinterpret_cast<const uint32_t*>(sig);
  if (w < 4) {
    Wheel& wh = g_wheels[w];
    const float* a = reinterpret_cast<const float*>(sig + 0x10);
    const float* b = reinterpret_cast<const float*>(sig + 0x20);
    for (int i = 0; i < 3; ++i) wh.p[i] = 0.5f * (a[i] + b[i]);
    wh.t = GetTickCount64();
    const float blend = *reinterpret_cast<const float*>(sig + 0xA8);
    const uint32_t m = *reinterpret_cast<const uint32_t*>(sig + (blend < 0.5f ? 0xA0 : 0xA4));
    if (m != wh.cand) { wh.cand = m; wh.candT = wh.t; }
  }
  reinterpret_cast<InvokeFn>(kTireThunk)(conn, sig);
}

// The wheel nearest a spawn point (physics space, same midpoint as the tire signal's), -1 if none within 1 m.
int nearest_wheel(const float* pos) {
  int best = -1;
  float bd = 1.0f;
  for (int w = 0; w < 4; ++w) {
    const float* p = g_wheels[w].p;
    const float d = (p[0] - pos[0]) * (p[0] - pos[0]) + (p[1] - pos[1]) * (p[1] - pos[1]) + (p[2] - pos[2]) * (p[2] - pos[2]);
    if (d < bd) { bd = d; best = w; }
  }
  return best;
}

ULONGLONG g_dustHoldMs = 300;

void note_dust(const float* pos) {
  const int w = nearest_wheel(pos);
  if (w >= 0) g_wheels[w].dustT = GetTickCount64();
}

void note_smoke(const float* pos) {
  const int w = nearest_wheel(pos);
  if (w < 0) return;
  Wheel& wh = g_wheels[w];
  const ULONGLONG now = GetTickCount64();
  if (now - wh.smokeLast > g_dustHoldMs) wh.smokeStart = now;   // a new slide
  wh.smokeLast = now;
}

bool wheel_dusty(const float* pos) {
  const int w = nearest_wheel(pos);
  return w >= 0 && GetTickCount64() - g_wheels[w].dustT < g_dustHoldMs;
}

void install_tire_hook() {
  uint32_t* slot = reinterpret_cast<uint32_t*>(kTireVtSlot);
  if (*slot != kTireThunk) {
    plog(L"particles: tire signal hook NOT installed: vtable slot %p holds %08X (other exe build?)", slot, *slot);
    return;
  }
  DWORD old = 0;
  if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return;
  *slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&tire_invoke));
  VirtualProtect(slot, 4, old, &old);
  g_tireHooked = true;
  plog(L"particles: tire signal hook at %p (wheel points and surfaces)", slot);
}

void install_probe() {
  uint8_t* call = reinterpret_cast<uint8_t*>(kEmitCall);
  int32_t rel = 0;
  std::memcpy(&rel, call + 1, 4);
  if (call[0] != 0xE8 || reinterpret_cast<uintptr_t>(call) + 5 + rel != kEmit) {
    plog(L"particles: emit probe NOT installed: the call at %p is not CEmitter_Emit (other exe build?)", call);
    return;
  }
  DWORD old = 0;
  if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return;
  const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&emit_hook) - (reinterpret_cast<uintptr_t>(call) + 5));
  std::memcpy(call + 1, &to, 4);
  VirtualProtect(call, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), call, 5);
  g_hooked = true;
  plog(L"particles: CEmitter_Emit hook at %p (game dust cut%s)", call, g_probe ? L"; probe: a line every 10 s, at most 30" : L"");
}

// arg1 starts with the emitter's name (run 2026-10-23); read it without faulting on anything else.
void emitter_name(void* arg1, char* out, size_t n) {
  if (!readable_string(reinterpret_cast<uintptr_t>(arg1), out, n)) strcpy_s(out, n, "?");
}

void probe_report() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_lastReport < 10000) return;
  const double secs = g_lastReport ? (now - g_lastReport) / 1000.0 : 10.0;
  g_lastReport = now;
  if (g_stats.empty() || g_reports >= kMaxReports) { g_stats.clear(); return; }
  // Sum per emitter name (four wheels share one), one line in all.
  std::unordered_map<std::string, double> byName;
  for (const auto& [self, s] : g_stats) {
    char name[64];
    emitter_name(s.arg1, name, sizeof(name));
    byName[name] += s.amount;
  }
  wchar_t line[1024] = {};
  size_t used = 0;
  for (const auto& [name, amount] : byName)
    if (used + 80 < 1024)
      used += swprintf_s(line + used, 1024 - used, L" %S %.1f/s;", name.c_str(), amount / secs);
  plog(L"particles: report %u (%zu emitters):%s", ++g_reports, g_stats.size(), line);
  g_stats.clear();
}

// ---------------------------------------------------------------- settings (particles.ini, live)

unsigned g_precipEpoch = 0;                            // bumped on every settings read (rain/snow section)
void load_game_textures(const wchar_t* list);          // rain/snow section below
void rows_load(bool found);                            // every other game emitter, below
void rows_stage_reset();

struct DustCfg {
  int wheels = 4;                                      // 4 = all wheels, 2 = the two rear wheels
  float perWheelRate = 25.0f;                          // puffs per second per wheel
  unsigned maxParticles = 150000;
  float lifeMin = 4.0f, lifeMax = 8.0f;                // s
  float startSizeMin = 0.9f, startSizeMax = 1.3f, endSizeMin = 7.0f, endSizeMax = 9.0f;   // m
  float startColorA[4] = { 0.50f, 0.43f, 0.34f, 0.45f }, startColorB[4] = { 0.62f, 0.54f, 0.43f, 0.60f };
  float endColorA[4] = { 0.58f, 0.52f, 0.43f, 0.0f }, endColorB[4] = { 0.68f, 0.62f, 0.52f, 0.0f };
  float riseSpeed = 1.5f, cone = 35.0f, drag = 0.8f, gravity = -0.4f;
  float turbulenceForce = 1.0f, turbulenceFrequency = 0.3f, rotation = 0.15f;
  float onKmh = 25.0f, offKmh = 20.0f, height = 0.1f;
  int hideCams[8] = { 5, 6 };
  int nHideCams = 2;
  wchar_t dustGame[256] = L"tirespray, dust, sand";    // game dust emitters: drive the Remix dust, always cut
  wchar_t smokeGame[256] = L"tiresmoke";               // cut only on a wheel throwing dust (gravel), kept on tarmac
  wchar_t keepGame[256] = L"wet, water, splash";       // never cut (name contains one of these)
  float dustHoldMs = 300.0f;
  bool surfaceColour = true;                           // puffs take the colour of the surface under the wheel
  float surfaceMix = 0.8f, surfaceBrightness = 1.15f;  // share of the surface colour; dust is lighter than the road
  bool enabled = true;                                 // [smoke] enabled
  float slideSeconds = 1.5f;                           // [smoke]: a slide this long before the tyres smoke
  // Rain / snow ([rain], [snow]; dust leaves them at these values): particles are widthRatio x size wide.
  float widthRatio = 1.0f, trail = 0.0f, velFromMotion = 0.0f, collisionThickness = 0.5f;
  bool align = false, collision = false;
  int billboard = 0;                                   // Remix: 0 face camera, 1 up axis locked, 2 camera position, 3 world up
  float box = 30.0f, ahead = 6.0f, aheadPerKmh = 0.08f, above = 12.0f;   // spawner square above/ahead of the camera
  int layers = 1;                                      // spawner squares stacked from `above` down to `above - depth`
  float depth = 0.0f;
  float holeLength = 0.0f, holeWidth = 0.0f;           // > 0: spawner on the car with a car-sized gap (nothing inside)
  int collisionMode = 2;                               // Remix: 0 bounce (takes the hit surface's velocity), 1 stop, 2 kill
  float restitution = 0.0f;                            // bounce: share of the speed kept
  bool holdColour = false;                             // start colour held for half the life, then to the end colour
  // Shelter (user 2026-11-01: rain fell inside the car): Kim's runtime traces each new particle up against the whole
  // scene and moves it to open sky, and traces its fall so it dies on the first opaque surface (a roof the camera
  // cannot see, the car's own roof). Sent as bit 0x80 of collisionMode (our runtime, rtx_remix_api.cpp).
  bool shelter = false;
  // Car kill box (user 2026-11-01: rain fell into the moving car, the spawn-time shelter trace sees last frame's world):
  // drops that enter the box of [killbox] are killed every step in the runtime (patch particles-killbox: collisionMode
  // bit 0x40, only sent once the runtime says it has it).
  bool killCar = false;
  // Rain/snow: Remix Plus lights particles from its froxel radiance cache, which is near black under trees and heavy
  // cloud: flakes went black against the sky (user 2026-11-01, Toivonvaara HeavySnow). A little white emission keeps
  // them visible there (emissive intensity; 0 = none).
  float selfLight = 0.0f;
  int spawner = 0;                                     // 0 squares (above), 1 car shell: roof + sides + rear (snow wake)
  float wakeOnKmh = 25.0f;                             // [snow_wake]: drawn from this car speed, rate in speed steps
  wchar_t tex[64] = L"dust_puff.dds";
} g_cfg, g_smoke, g_snowDust;
float g_maxCoverage = 1.0f, g_coverageSent = -1.0f;   // rtx.particles.maxScreenCoverage; [dust] max_screen_coverage

// Rain/snow presets (user 2026-10-23 run 2): light rain = many small drops at the run-2 speed; heavy rain = big drops,
// faster, shorter streaks; light snow = many fluffy flakes falling slowly; heavy snow = big flakes, faster and dense.
enum { PRE_RAIN_LIGHT, PRE_RAIN_HEAVY, PRE_SNOW_LIGHT, PRE_SNOW_HEAVY, PRE_SNOW_WAKE, PRE_COUNT };
const wchar_t* const kPreSection[PRE_COUNT] = { L"rain_light", L"rain_heavy", L"snow_light", L"snow_heavy", L"snow_wake" };
DustCfg g_pre[PRE_COUNT];
float g_heavyFrom = 75.0f;                             // weather intensity (percent) from which the heavy preset is used

// [killbox] (user 2026-11-01, rain in the car): the player's car as an oriented box the runtime kills drops in, every
// frame. Off until the runtime has it (patches/particles-killbox.patch); the proxy then sends the box each frame as the
// game value "__rbrrtx.carbox" and sets collisionMode bit 0x40 on the systems with kill_in_car_box = 1. The box is
// centred on the car's root: `base` is its bottom relative to the root, sizes in metres, margin added on every side.
struct KillBoxCfg { bool enabled = false; float length = 4.6f, width = 2.1f, height = 2.1f, base = -0.6f, margin = 0.15f; } g_kb;

// Tarmac tyre smoke defaults (user 2026-10-23: puffs 50 % smaller than the gravel dust): grey-white, rising a little.
DustCfg smoke_defaults() {
  DustCfg c;
  c.perWheelRate = 20.0f;
  c.lifeMin = 2.0f; c.lifeMax = 4.0f;
  c.startSizeMin = 0.45f; c.startSizeMax = 0.65f; c.endSizeMin = 3.5f; c.endSizeMax = 4.5f;
  const float sa[4] = { 0.72f, 0.72f, 0.72f, 0.35f }, sb[4] = { 0.82f, 0.82f, 0.82f, 0.45f };
  const float ea[4] = { 0.80f, 0.80f, 0.80f, 0.0f }, eb[4] = { 0.90f, 0.90f, 0.90f, 0.0f };
  std::memcpy(c.startColorA, sa, sizeof(sa)); std::memcpy(c.startColorB, sb, sizeof(sb));
  std::memcpy(c.endColorA, ea, sizeof(ea)); std::memcpy(c.endColorB, eb, sizeof(eb));
  c.riseSpeed = 1.0f; c.cone = 45.0f; c.drag = 1.0f; c.gravity = 0.15f;
  c.turbulenceForce = 1.5f; c.turbulenceFrequency = 0.4f;
  c.onKmh = 5.0f; c.offKmh = 0.0f;
  c.surfaceColour = false;
  return c;
}

// Rain/snow preset defaults. Rain values started from xoxor4d's NFS Carbon rain (nfsc-rtx comp_settings: thin streaks
// along their velocity with a motion trail, short life, killed where they hit). Units: m, s; RBR metres are Remix units
// (sceneScale 1). per_wheel_rate = particles per second for the whole spawner.
void set_colour(DustCfg& c, const float a[4], const float b[4]) {
  std::memcpy(c.startColorA, a, 16); std::memcpy(c.startColorB, b, 16);
  std::memcpy(c.endColorA, a, 16); std::memcpy(c.endColorB, b, 16);
}

DustCfg precip_defaults(int pre) {
  DustCfg c;
  c.maxParticles = 30000;
  c.collision = true;
  c.shelter = true;
  c.killCar = true;
  c.rotation = 0.0f;
  if (pre == PRE_RAIN_LIGHT || pre == PRE_RAIN_HEAVY) {
    const float a[4] = { 0.70f, 0.72f, 0.76f, 0.25f }, b[4] = { 0.85f, 0.87f, 0.90f, 0.40f };
    set_colour(c, a, b);
    c.cone = 3.0f; c.drag = 0.0f; c.gravity = 0.0f; c.turbulenceForce = 0.0f;   // riseSpeed = fall speed
    c.align = true; c.billboard = 1;
    c.box = 30.0f; c.ahead = 6.0f; c.aheadPerKmh = 0.12f; c.above = 12.0f;
    wcscpy_s(c.tex, L"rain_drop.dds");
    if (pre == PRE_RAIN_LIGHT) {                       // many small drops, 9 m/s (the run-2 speed)
      c.perWheelRate = 10000.0f;
      c.lifeMin = 1.3f; c.lifeMax = 1.8f;
      c.startSizeMin = c.endSizeMin = 0.12f; c.startSizeMax = c.endSizeMax = 0.2f; c.widthRatio = 0.06f;
      c.riseSpeed = 9.0f; c.trail = 1.0f;
    } else {                                           // big drops, fast, shorter streaks
      c.perWheelRate = 7000.0f;
      c.lifeMin = 0.8f; c.lifeMax = 1.1f;
      c.startSizeMin = c.endSizeMin = 0.18f; c.startSizeMax = c.endSizeMax = 0.28f; c.widthRatio = 0.12f;
      c.riseSpeed = 16.0f; c.trail = 0.5f;
      const float ha[4] = { 0.72f, 0.74f, 0.78f, 0.35f }, hb[4] = { 0.88f, 0.90f, 0.93f, 0.55f };
      set_colour(c, ha, hb);
    }
  } else {
    const float a[4] = { 0.92f, 0.94f, 0.97f, 0.85f }, b[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    set_colour(c, a, b);
    c.billboard = 0; c.rotation = 0.3f;
    c.box = 30.0f; c.ahead = 3.0f; c.aheadPerKmh = 0.1f;
    c.layers = 5;                                      // a volume: slow flakes from a square far above would land behind
    c.holeLength = 5.0f; c.holeWidth = 2.4f;           // run 3: layers at cabin height made flakes inside the cockpit
    c.aheadPerKmh = 0.0f;
    // Run 4 (user): snow should react to the car. Bounce: a flake hitting a surface takes that surface's velocity
    // (Remix collision, sceneVel from motion vectors), so flakes touching the moving car are carried and thrown by it;
    // with little restitution they settle on the ground and fade (colour held, then alpha to 0).
    c.collisionMode = 0; c.restitution = 0.0f; c.holdColour = true;   // run 6 (user): less bouncy, more drag
    c.endColorA[3] = 0.0f; c.endColorB[3] = 0.0f;
    wcscpy_s(c.tex, L"snow_flake.dds");
    if (pre == PRE_SNOW_LIGHT) {                       // lots of fluffy flakes, slow
      c.perWheelRate = 4000.0f;
      c.lifeMin = 6.0f; c.lifeMax = 9.0f;
      c.startSizeMin = c.endSizeMin = 0.05f; c.startSizeMax = c.endSizeMax = 0.10f;   // run 4: 3-6 cm did not show
      c.riseSpeed = 0.8f; c.cone = 30.0f; c.drag = 1.2f; c.gravity = -0.3f;
      c.turbulenceForce = 1.5f; c.turbulenceFrequency = 0.3f;
      c.above = 7.0f; c.depth = 8.0f;
    } else {                                           // big flakes, faster, dense
      c.perWheelRate = 6000.0f;
      c.lifeMin = 3.0f; c.lifeMax = 4.5f;
      c.startSizeMin = c.endSizeMin = 0.09f; c.startSizeMax = c.endSizeMax = 0.18f;
      c.riseSpeed = 3.5f; c.cone = 15.0f; c.drag = 0.8f; c.gravity = -0.8f;
      c.turbulenceForce = 1.0f; c.turbulenceFrequency = 0.4f;
      c.above = 9.0f; c.depth = 9.0f;
    }
    if (pre == PRE_SNOW_WAKE) {                        // snow the car stirs up: leaves the car's roof, sides and rear,
      c.layers = 1; c.depth = 0.0f;                    // takes most of the car's motion, then drag stops it and
      c.spawner = 1;                                   // turbulence swirls it behind the car
      c.perWheelRate = 1500.0f; c.maxParticles = 12000;
      c.lifeMin = 1.2f; c.lifeMax = 2.5f;
      c.startSizeMin = c.endSizeMin = 0.05f; c.startSizeMax = c.endSizeMax = 0.12f;
      const float wa[4] = { 0.92f, 0.94f, 0.97f, 0.7f }, wb[4] = { 1.0f, 1.0f, 1.0f, 0.9f };
      set_colour(c, wa, wb);
      c.endColorA[3] = 0.0f; c.endColorB[3] = 0.0f;
      c.riseSpeed = 1.0f; c.cone = 60.0f; c.drag = 2.0f; c.gravity = -0.5f;
      c.turbulenceForce = 3.0f; c.turbulenceFrequency = 0.8f;
      c.velFromMotion = 0.7f;
      c.collision = true; c.collisionMode = 2; c.holdColour = true; c.shelter = false;   // starts on the car itself
      c.killCar = false;                               // ... so the car box must not kill it
      c.holeLength = 4.2f; c.holeWidth = 1.75f; c.above = 1.35f;   // car shell: length, width, roof above the car's root
      c.wakeOnKmh = 25.0f;
    }
  }
  return c;
}

// Per game emitter (arg1, starts with its name): hidden or kept. Cleared when the settings change.
std::unordered_map<void*, int> g_gameKind;
bool g_cutGame = false;                                // Remix dust runs: the game's dust is cut

bool name_has_any(const char* name, const wchar_t* list) {
  wchar_t lower[96] = {};
  for (int i = 0; name[i] && i < 95; ++i) lower[i] = static_cast<wchar_t>(towlower(static_cast<unsigned char>(name[i])));
  wchar_t buf[256];
  wcscpy_s(buf, list);
  wchar_t* ctx = nullptr;
  for (wchar_t* t = wcstok_s(buf, L", ", &ctx); t; t = wcstok_s(nullptr, L", ", &ctx)) {
    for (wchar_t* c = t; *c; ++c) *c = static_cast<wchar_t>(towlower(*c));
    if (wcsstr(lower, t)) return true;
  }
  return false;
}

int game_emitter_kind(void* arg1) {
  if (!g_cutGame) return 0;
  const auto it = g_gameKind.find(arg1);
  if (it != g_gameKind.end()) return it->second;
  char name[64];
  emitter_name(arg1, name, sizeof(name));
  const int kind = name_has_any(name, g_cfg.keepGame) ? 0 : name_has_any(name, g_cfg.dustGame) ? 1 :
                   name_has_any(name, g_cfg.smokeGame) ? 2 : 0;
  g_gameKind[arg1] = kind;
  plog(L"particles: game emitter %S: %s", name, kind == 1 ? L"dust: never spawned, drives the Remix dust" :
       kind == 2 ? L"smoke: never spawned, drives the Remix tyre smoke (dry tarmac, moving, slide long enough)" : L"kept");
  return kind;
}

wchar_t g_cfgPath[MAX_PATH * 2] = {};
FILETIME g_cfgTime = {};
ULONGLONG g_cfgChecked = 0;

const wchar_t* g_sec = L"dust";                        // ini section the readers below use

float cfg_float(const wchar_t* key, float dflt, float lo, float hi) {
  wchar_t v[64] = {};
  GetPrivateProfileStringW(g_sec, key, L"", v, 64, g_cfgPath);
  if (!v[0]) return dflt;
  const float f = static_cast<float>(_wtof(v));
  return f < lo ? lo : f > hi ? hi : f;
}

void cfg_rgba(const wchar_t* key, float out[4]) {
  wchar_t v[128] = {};
  GetPrivateProfileStringW(g_sec, key, L"", v, 128, g_cfgPath);
  float c[4];
  if (v[0] && swscanf_s(v, L"%f , %f , %f , %f", &c[0], &c[1], &c[2], &c[3]) == 4) std::memcpy(out, c, sizeof(c));
}

// The per-effect keys of the current section (g_sec) over c's own defaults.
void load_fx(DustCfg& c) {
  c.perWheelRate = cfg_float(L"per_wheel_rate", c.perWheelRate, 0.0f, 10000.0f);
  c.maxParticles = static_cast<unsigned>(cfg_float(L"max_particles", static_cast<float>(c.maxParticles), 100.0f, 1000000.0f));
  c.lifeMin = cfg_float(L"life_min", c.lifeMin, 0.1f, 60.0f);
  c.lifeMax = cfg_float(L"life_max", c.lifeMax, c.lifeMin, 60.0f);
  c.startSizeMin = cfg_float(L"start_size_min", c.startSizeMin, 0.01f, 50.0f);
  c.startSizeMax = cfg_float(L"start_size_max", c.startSizeMax, c.startSizeMin, 50.0f);
  c.endSizeMin = cfg_float(L"end_size_min", c.endSizeMin, 0.01f, 100.0f);
  c.endSizeMax = cfg_float(L"end_size_max", c.endSizeMax, c.endSizeMin, 100.0f);
  cfg_rgba(L"start_color_min", c.startColorA);
  cfg_rgba(L"start_color_max", c.startColorB);
  cfg_rgba(L"end_color_min", c.endColorA);
  cfg_rgba(L"end_color_max", c.endColorB);
  c.riseSpeed = cfg_float(L"rise_speed", c.riseSpeed, 0.0f, 100.0f);
  c.cone = cfg_float(L"spread_degrees", c.cone, 0.0f, 180.0f);
  c.drag = cfg_float(L"drag", c.drag, 0.0f, 100.0f);
  c.gravity = cfg_float(L"gravity", c.gravity, -100.0f, 100.0f);
  c.turbulenceForce = cfg_float(L"turbulence", c.turbulenceForce, 0.0f, 100.0f);
  c.turbulenceFrequency = cfg_float(L"turbulence_frequency", c.turbulenceFrequency, 0.0f, 100.0f);
  c.rotation = cfg_float(L"spin", c.rotation, 0.0f, 10.0f);
  c.onKmh = cfg_float(L"on_above_kmh", c.onKmh, 0.0f, 300.0f);
  c.offKmh = cfg_float(L"off_below_kmh", c.offKmh, 0.0f, c.onKmh);
  c.height = cfg_float(L"height", c.height, -2.0f, 5.0f);
  c.surfaceColour = cfg_float(L"surface_colour", 1.0f, 0.0f, 1.0f) >= 0.5f;
  c.surfaceMix = cfg_float(L"surface_mix", c.surfaceMix, 0.0f, 1.0f);
  c.surfaceBrightness = cfg_float(L"surface_brightness", c.surfaceBrightness, 0.0f, 4.0f);
  c.enabled = cfg_float(L"enabled", c.enabled ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
  c.slideSeconds = cfg_float(L"slide_seconds", c.slideSeconds, 0.0f, 30.0f);
  c.widthRatio = cfg_float(L"width_ratio", c.widthRatio, 0.001f, 100.0f);
  c.trail = cfg_float(L"motion_trail", c.trail, 0.0f, 100.0f);
  c.velFromMotion = cfg_float(L"velocity_from_motion", c.velFromMotion, -10.0f, 10.0f);
  c.align = cfg_float(L"align_to_velocity", c.align ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
  c.collision = cfg_float(L"collision", c.collision ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
  c.collisionThickness = cfg_float(L"collision_thickness", c.collisionThickness, 0.01f, 10.0f);
  c.billboard = static_cast<int>(cfg_float(L"billboard", static_cast<float>(c.billboard), 0.0f, 3.0f));
  c.box = cfg_float(L"spawner_size", c.box, 1.0f, 500.0f);
  c.ahead = cfg_float(L"spawner_ahead", c.ahead, -100.0f, 200.0f);
  c.aheadPerKmh = cfg_float(L"spawner_ahead_per_kmh", c.aheadPerKmh, 0.0f, 5.0f);
  c.above = cfg_float(L"spawner_above", c.above, -10.0f, 200.0f);
  c.layers = static_cast<int>(cfg_float(L"spawner_layers", static_cast<float>(c.layers), 1.0f, 16.0f));
  c.depth = cfg_float(L"spawner_depth", c.depth, 0.0f, 100.0f);
  c.holeLength = cfg_float(L"car_gap_length", c.holeLength, 0.0f, 50.0f);
  c.holeWidth = cfg_float(L"car_gap_width", c.holeWidth, 0.0f, 50.0f);
  c.collisionMode = static_cast<int>(cfg_float(L"collision_mode", static_cast<float>(c.collisionMode), 0.0f, 2.0f));
  c.restitution = cfg_float(L"collision_restitution", c.restitution, 0.0f, 1.0f);
  c.holdColour = cfg_float(L"hold_colour", c.holdColour ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
  c.shelter = cfg_float(L"shelter", c.shelter ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
  c.killCar = cfg_float(L"kill_in_car_box", c.killCar ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
  c.selfLight = cfg_float(L"self_light", c.selfLight, 0.0f, 100.0f);
  c.wakeOnKmh = cfg_float(L"on_above_kmh_wake", c.wakeOnKmh, 0.0f, 300.0f);
  GetPrivateProfileStringW(g_sec, L"texture", c.tex, c.tex, 64, g_cfgPath);
}

void load_cfg() {
  DustCfg c;
  const bool found = GetFileAttributesW(g_cfgPath) != INVALID_FILE_ATTRIBUTES;
  if (found) {
    c.wheels = cfg_float(L"wheels", 4.0f, 2.0f, 4.0f) < 3.0f ? 2 : 4;
    load_fx(c);
    wchar_t v[128] = {};
    wchar_t list[256];
    GetPrivateProfileStringW(L"dust", L"dust_emitters", c.dustGame, list, 256, g_cfgPath);
    wcscpy_s(c.dustGame, list);
    GetPrivateProfileStringW(L"dust", L"smoke_emitters", c.smokeGame, list, 256, g_cfgPath);
    wcscpy_s(c.smokeGame, list);
    c.dustHoldMs = cfg_float(L"dust_hold_ms", c.dustHoldMs, 50.0f, 5000.0f);
    GetPrivateProfileStringW(L"dust", L"keep_game_emitters", c.keepGame, list, 256, g_cfgPath);
    wcscpy_s(c.keepGame, list);
    GetPrivateProfileStringW(L"dust", L"hide_in_cameras", L"5, 6", v, 128, g_cfgPath);
    c.nHideCams = 0;
    wchar_t* ctx = nullptr;
    for (wchar_t* t = wcstok_s(v, L", ", &ctx); t && c.nHideCams < 8; t = wcstok_s(nullptr, L", ", &ctx))
      c.hideCams[c.nHideCams++] = _wtoi(t);
  }
  g_maxCoverage = cfg_float(L"max_screen_coverage", g_maxCoverage, 0.0f, 16.0f);
  g_cfg = c;
  DustCfg sm = smoke_defaults();
  if (found) { g_sec = L"smoke"; load_fx(sm); g_sec = L"dust"; }
  g_smoke = sm;
  DustCfg sd = c;
  if (found) { g_sec = L"snow_dust"; load_fx(sd); g_sec = L"dust"; }
  g_snowDust = sd;
  for (int i = 0; i < PRE_COUNT; ++i) {
    g_pre[i] = precip_defaults(i);
    if (found) { g_sec = kPreSection[i]; load_fx(g_pre[i]); g_sec = L"dust"; }
  }
  g_sec = L"precipitation";
  g_heavyFrom = cfg_float(L"heavy_from", 75.0f, 0.0f, 101.0f);
  g_sec = L"killbox";
  g_kb = KillBoxCfg();
  g_kb.enabled = cfg_float(L"enabled", 0.0f, 0.0f, 1.0f) >= 0.5f;
  g_kb.length = cfg_float(L"length", g_kb.length, 0.5f, 20.0f);
  g_kb.width = cfg_float(L"width", g_kb.width, 0.5f, 10.0f);
  g_kb.height = cfg_float(L"height", g_kb.height, 0.5f, 10.0f);
  g_kb.base = cfg_float(L"base", g_kb.base, -5.0f, 5.0f);
  g_kb.margin = cfg_float(L"margin", g_kb.margin, 0.0f, 2.0f);
  g_sec = L"dust";
  g_precipEpoch++;                                     // new values: new Remix particle systems
  wchar_t gt[512] = {};                                // a missing file gives the default
  GetPrivateProfileStringW(L"precipitation", L"game_textures", L"40A8944300F54468, 869E5A5AF4FA4D7B, 254D6B130606B00C",
                           gt, 512, g_cfgPath);
  load_game_textures(gt);
  g_gameKind.clear();
  rows_load(found);
  g_dustHoldMs = static_cast<ULONGLONG>(c.dustHoldMs);
  plog(L"particles: game emitters: dust \"%s\", smoke \"%s\", kept \"%s\"; a wheel stays dusty %.0f ms", c.dustGame,
       c.smokeGame, c.keepGame, c.dustHoldMs);
  plog(L"particles: dust settings %s%s: %d wheels x %.0f/s, cap %u, life %.1f-%.1f s, size %.2f-%.2f -> %.1f-%.1f m, "
       L"rise %.1f m/s, spread %.0f, drag %.2f, gravity %.2f, turbulence %.2f/%.2f, on above %.0f km/h, off below %.0f, "
       L"%d hidden cameras; surface colour %s (mix %.2f, brightness %.2f)", found ? L"from " : L"built in, no file ", g_cfgPath, c.wheels, c.perWheelRate, c.maxParticles,
       c.lifeMin, c.lifeMax, c.startSizeMin, c.startSizeMax, c.endSizeMin, c.endSizeMax, c.riseSpeed, c.cone, c.drag,
       c.gravity, c.turbulenceForce, c.turbulenceFrequency, c.onKmh, c.offKmh, c.nHideCams,
       c.surfaceColour ? L"on" : L"off", c.surfaceMix, c.surfaceBrightness);
  for (int i = 0; i < PRE_COUNT; ++i) {
    const DustCfg& p = g_pre[i];
    plog(L"particles: %s %.0f/s, life %.1f-%.1f s, %.3f-%.3f m (width x %.2f), fall %.1f m/s, trail %.1f, spawner %.0f m "
         L"x %d layers (%.1f m above, %.1f deep), %.1f m ahead + %.2f per km/h", kPreSection[i], p.perWheelRate, p.lifeMin,
         p.lifeMax, p.startSizeMin, p.startSizeMax, p.widthRatio, p.riseSpeed, p.trail, p.box, p.layers, p.above, p.depth,
         p.ahead, p.aheadPerKmh);
  }
  plog(L"particles: heavy rain/snow presets from %.0f %% weather intensity", g_heavyFrom);
  plog(L"particles: tarmac smoke %s: after a %.1f s slide, %.0f/s per wheel, life %.1f-%.1f s, size %.2f-%.2f -> %.1f-%.1f m, "
       L"rise %.1f m/s, gravity %.2f, above %.0f km/h", sm.enabled ? L"on" : L"off", sm.slideSeconds, sm.perWheelRate,
       sm.lifeMin, sm.lifeMax, sm.startSizeMin, sm.startSizeMax, sm.endSizeMin, sm.endSizeMax, sm.riseSpeed, sm.gravity, sm.onKmh);
}

// Once a second: the file saved again (Notepad++) -> read it again.
void check_cfg() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_cfgChecked < 1000) return;
  g_cfgChecked = now;
  WIN32_FILE_ATTRIBUTE_DATA a = {};
  if (!GetFileAttributesExW(g_cfgPath, GetFileExInfoStandard, &a)) return;
  if (CompareFileTime(&a.ftLastWriteTime, &g_cfgTime) == 0) return;
  const bool first = g_cfgTime.dwLowDateTime == 0 && g_cfgTime.dwHighDateTime == 0;
  g_cfgTime = a.ftLastWriteTime;
  if (!first) { load_cfg(); rbrrtx_report_event(L"particles.ini saved: dust settings read again"); }
}

// RBR's camera: [[[0x165FC68]+0x758]+0x10]+0 (NGPCarMenu RBRAPI.h), -1 = not readable.
int camera_type() {
  __try {
    const uintptr_t car = *reinterpret_cast<volatile uintptr_t*>(0x0165FC68);
    if (!car) return -1;
    const uintptr_t cam = *reinterpret_cast<volatile uintptr_t*>(car + 0x758);
    if (!cam) return -1;
    const uintptr_t info = *reinterpret_cast<volatile uintptr_t*>(cam + 0x10);
    return info ? *reinterpret_cast<volatile int*>(info) : -1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Car body drawn (outside view) = dust; not drawn (cockpit) = no dust; the headlights' own test. Until that is known:
// RBR's camera type against hide_in_cameras.
bool camera_hidden() {
  static int was = -2;
  const int body = rbrrtx_car_body_visible();
  bool hide = body == 0;
  int state = body;
  if (body < 0) {
    const int cam = camera_type();
    for (int i = 0; i < g_cfg.nHideCams; ++i) hide |= cam == g_cfg.hideCams[i];
    state = 100 + cam;
  }
  if (state != was) {
    was = state;
    if (body >= 0) plog(L"particles: car body %s: dust %s", body ? L"drawn (outside view)" : L"not drawn (cockpit view)", hide ? L"hidden" : L"shown");
    else plog(L"particles: car body not known yet, camera type %d: dust %s", state - 100, hide ? L"hidden" : L"shown");
  }
  return hide;
}

// ---------------------------------------------------------------- surface colours (one lookup per new surface)

struct SurfaceColour { float rgb[3] = {}; bool found = false; };
std::unordered_map<uint32_t, SurfaceColour> g_surface;     // material -> colour, this stage
struct Lookup {
  bool on = false;
  int wheel = 0;
  uint32_t mat = 0;
  float pt[3] = {};                                    // render space (Y up)
  unsigned frames = 0;
  bool hit = false;
  float dh = 0, rgb[3] = {};
  unsigned long long tex = 0;
} g_lookup;
int g_stageTrack = -1;
ULONGLONG g_stageChecked = 0;

void surface_frame() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_stageChecked > 2000) {                   // a new stage: its materials have other textures
    g_stageChecked = now;
    RbrStageInfo si = {};
    if (rbrrtx_stage_info(&si) && si.trackId != g_stageTrack) {
      g_stageTrack = si.trackId;
      g_surface.clear();
      g_lookup.on = false;
      rbrrtx_surface_lookup_on = false;
      for (Wheel& w : g_wheels) w.mat = w.cand = UINT32_MAX;
      rows_stage_reset();
    }
  }
  if (g_lookup.on) {
    if (!g_lookup.hit && ++g_lookup.frames < 40) return;
    if (g_lookup.hit) {                                // a miss is not kept: tried again when a wheel next meets it
      SurfaceColour& sc = g_surface[g_lookup.mat];
      sc.found = true;
      std::memcpy(sc.rgb, g_lookup.rgb, sizeof(sc.rgb));
      plog(L"particles: surface %u (wheel %d): texture %016llX under the wheel (%.2f m), colour %.2f %.2f %.2f", g_lookup.mat,
           g_lookup.wheel, g_lookup.tex, g_lookup.dh, sc.rgb[0], sc.rgb[1], sc.rgb[2]);
    } else {
      plog(L"particles: surface %u (wheel %d): no ground texture found under the wheel yet; ini colours for now",
           g_lookup.mat, g_lookup.wheel);
    }
    g_lookup.on = false;
    rbrrtx_surface_lookup_on = false;
  }
  for (int w = 0; w < 4; ++w) {                        // settled surface changes; at most one new lookup at a time
    Wheel& wh = g_wheels[w];
    if (wh.cand == wh.mat || now - wh.candT < kSettleMs || now - wh.t > 250) continue;
    wh.mat = wh.cand;
    if (!g_cfg.surfaceColour || g_lookup.on || g_surface.count(wh.mat)) continue;
    g_lookup = {};
    g_lookup.on = true;
    g_lookup.wheel = w;
    g_lookup.mat = wh.mat;
    g_lookup.pt[0] = wh.p[0]; g_lookup.pt[1] = wh.p[2]; g_lookup.pt[2] = wh.p[1];   // physics -> render (Y up)
    rbrrtx_surface_lookup_on = true;
  }
}

// ---------------------------------------------------------------- API emitter test

const remixapi_Interface* g_api = nullptr;
bool g_test = false, g_made = false, g_failed = false;
bool g_active = false;                                 // Remix dust in this run (key 7 is ours, game dust cut)
unsigned g_drawn = 0;
// Speed gate: RBR's car position between frames (physics space, m), smoothed. Jumps (replay cuts, restarts) of
// more than 100 m/s are not speeds and are skipped.
LARGE_INTEGER g_lastT = {};
float g_lastPos[3] = {}, g_kmh = 0.0f, g_dir[3] = { 1, 0, 0 };   // g_dir: travel direction, physics space
bool g_havePos = false, g_moving = false;

void update_speed(const float* pos) {
  LARGE_INTEGER t, f;
  QueryPerformanceCounter(&t);
  QueryPerformanceFrequency(&f);
  if (g_havePos) {
    const double dt = static_cast<double>(t.QuadPart - g_lastT.QuadPart) / f.QuadPart;
    if (dt > 0.0005) {
      const float dx = pos[0] - g_lastPos[0], dy = pos[1] - g_lastPos[1], dz = pos[2] - g_lastPos[2];
      const float ms = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / dt);
      if (ms < 100.0f) {
        g_kmh += (ms * 3.6f - g_kmh) * 0.2f;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > 0.01f) {                               // smoothed, kept while standing
          for (int i = 0; i < 3; ++i) g_dir[i] += ((i == 0 ? dx : i == 1 ? dy : dz) / d - g_dir[i]) * 0.2f;
          const float l = std::sqrt(g_dir[0] * g_dir[0] + g_dir[1] * g_dir[1] + g_dir[2] * g_dir[2]);
          if (l > 1e-4f) for (float& v : g_dir) v /= l;
        }
      }
    }
  }
  g_lastT = t;
  std::memcpy(g_lastPos, pos, sizeof(g_lastPos));
  g_havePos = true;
  const bool was = g_moving;
  g_moving = g_moving ? g_kmh > g_cfg.offKmh : g_kmh > g_cfg.onKmh;
  if (was != g_moving) plog(L"particles: car %s (%.0f km/h): test dust %s", g_moving ? L"moving" : L"slow",
                            g_kmh, g_moving ? L"on" : L"off");
}
remixapi_MaterialHandle g_mat = nullptr;
remixapi_MeshHandle g_mesh = nullptr;
wchar_t g_tex[MAX_PATH * 2] = {};

// Over life: start -> end (two keyframes; three with hold_colour); colours RGBA. The animated curves are pointers into
// the caller, so a descriptor and its curves live together: dust builds one per wheel, rain/snow keep theirs (below)
// and no longer rebuild them every frame.
struct DescStore {
  remixapi_Float4D minColor[3] = {}, maxColor[3] = {};
  remixapi_Float2D minSize[2] = {}, maxSize[2] = {};
  float minRot[1] = {}, maxRot[1] = {};
  remixapi_Float3D maxVel[1] = { { -1.0f, -1.0f, -1.0f } };   // negative = unlimited
};
bool g_kbRuntime = false;                              // the runtime answered "__rbrrtx.killbox.v" (killbox section)

// One ini colour moved toward the surface colour: the surface's hue at this colour's own brightness (relative to the
// four ini colours), times surface_brightness, mixed in by surface_mix. Quantised to 1/32 so near-equal surfaces
// share one Remix particle system.
remixapi_Float4D tinted(const DustCfg& c, const float* k, const float* tint, float meanLum) {
  remixapi_Float4D out = { k[0], k[1], k[2], k[3] };
  if (!tint) return out;
  const float tl = (tint[0] + tint[1] + tint[2]) / 3.0f;
  if (tl <= 1e-3f || meanLum <= 1e-3f) return out;
  const float scale = (k[0] + k[1] + k[2]) / 3.0f / meanLum * c.surfaceBrightness;
  float* o[3] = { &out.x, &out.y, &out.z };
  for (int i = 0; i < 3; ++i) {
    const float target = tint[i] * scale;
    float v = k[i] + (target - k[i]) * c.surfaceMix;
    v = std::round((v < 0 ? 0 : v > 1 ? 1 : v) * 32.0f) / 32.0f;
    *o[i] = v;
  }
  return out;
}

// Alpha blended, texture x particle colour.
void fill_blend(remixapi_InstanceInfoBlendEXT& b, void* next) {
  b.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  b.pNext = next;
  b.alphaTestCompareOp = 7;                            // VK_COMPARE_OP_ALWAYS
  b.alphaBlendEnabled = 1;
  b.srcColorBlendFactor = 6;                           // VK_BLEND_FACTOR_SRC_ALPHA
  b.dstColorBlendFactor = 7;                           // VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
  b.colorBlendOp = 0;                                  // VK_BLEND_OP_ADD
  b.textureColorArg1Source = 1;                        // texture
  b.textureColorArg2Source = 2;                        // vertex colour (the particle's colour)
  b.textureColorOperation = 3;                         // modulate
  b.textureAlphaArg1Source = 1;
  b.textureAlphaArg2Source = 2;
  b.textureAlphaOperation = 3;
  b.srcAlphaBlendFactor = 1;                           // VK_BLEND_FACTOR_ONE
  b.dstAlphaBlendFactor = 0;                           // VK_BLEND_FACTOR_ZERO
  b.writeMask = 0xF;
}

remixapi_InstanceInfoParticleSystemEXT fx_desc(const DustCfg& c, const float* tint, DescStore& s) {
  const float meanLum = (c.startColorA[0] + c.startColorA[1] + c.startColorA[2] + c.startColorB[0] + c.startColorB[1] +
                         c.startColorB[2] + c.endColorA[0] + c.endColorA[1] + c.endColorA[2] + c.endColorB[0] +
                         c.endColorB[1] + c.endColorB[2]) / 12.0f;
  s.minColor[0] = tinted(c, c.startColorA, tint, meanLum);
  s.maxColor[0] = tinted(c, c.startColorB, tint, meanLum);
  s.minColor[1] = tinted(c, c.endColorA, tint, meanLum);
  s.maxColor[1] = tinted(c, c.endColorB, tint, meanLum);
  s.minSize[0] = { c.startSizeMin * c.widthRatio, c.startSizeMin };
  s.maxSize[0] = { c.startSizeMax * c.widthRatio, c.startSizeMax };
  s.minSize[1] = { c.endSizeMin * c.widthRatio, c.endSizeMin };
  s.maxSize[1] = { c.endSizeMax * c.widthRatio, c.endSizeMax };
  s.minRot[0] = -c.rotation;
  s.maxRot[0] = c.rotation;
  remixapi_InstanceInfoParticleSystemEXT p = {};
  p.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT;
  p.maxNumParticles = c.maxParticles;
  p.useTurbulence = c.turbulenceForce > 0.0f;
  p.hideEmitter = 1;
  p.minColor = { s.minColor, 2 };
  p.maxColor = { s.maxColor, 2 };
  if (c.holdColour) {                                  // start, start, end: the change happens in the second half
    s.minColor[2] = s.minColor[1]; s.maxColor[2] = s.maxColor[1];
    s.minColor[1] = s.minColor[0]; s.maxColor[1] = s.maxColor[0];
    p.minColor = { s.minColor, 3 };
    p.maxColor = { s.maxColor, 3 };
  }
  p.minRotationSpeed = { s.minRot, 1 };
  p.maxRotationSpeed = { s.maxRot, 1 };
  p.minSize = { s.minSize, 2 };
  p.maxSize = { s.maxSize, 2 };
  p.maxVelocity = { s.maxVel, 1 };
  p.minTimeToLive = c.lifeMin;
  p.maxTimeToLive = c.lifeMax;
  p.initialVelocityFromNormal = c.riseSpeed;           // m/s up out of the emitter quad
  p.initialVelocityConeAngleDegrees = c.cone;
  p.dragCoefficient = c.drag;
  p.initialRotationDeviationDegrees = 180.0f;
  p.gravityForce = c.gravity;
  p.turbulenceFrequency = c.turbulenceFrequency;
  p.turbulenceForce = c.turbulenceForce;
  p.spawnRatePerSecond = c.perWheelRate;               // per wheel instance
  p.motionTrailMultiplier = c.trail > 0.0f ? c.trail : 1.0f;
  p.enableMotionTrail = c.trail > 0.0f;
  p.alignParticlesToVelocity = c.align;
  p.enableCollisionDetection = c.collision;
  // Bits our runtime reads from collisionMode: 0x80 spawn shelter, 0x40 car kill box (only once the runtime said it has
  // it: an older one would read 0x42 as no collision mode at all).
  p.collisionMode = static_cast<uint8_t>(c.collisionMode | (c.shelter ? 0x80 : 0) | (c.killCar && g_kbRuntime ? 0x40 : 0));
  p.collisionRestitution = c.restitution;
  p.collisionThickness = c.collisionThickness;
  p.initialVelocityFromMotion = c.velFromMotion;
  p.billboardType = static_cast<uint8_t>(c.billboard);
  return p;
}

bool make_emitter() {
  if (g_made) return true;
  if (g_failed || !g_api) return false;
  rbrrtx_config_path(L"RBRRTX_PARTICLE_TEX", L"dust_puff.dds", g_tex, MAX_PATH * 2);
  const bool haveTex = GetFileAttributesW(g_tex) != INVALID_FILE_ATTRIBUTES;

  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.roughnessTexture = L"";
  opaque.metallicTexture = L"";
  opaque.heightTexture = L"";
  opaque.albedoConstant = { 0.6f, 0.53f, 0.43f };
  opaque.opacityConstant = 1.0f;
  opaque.roughnessConstant = 1.0f;
  opaque.useDrawCallAlphaState = 1;                    // alpha from the blend extension below
  remixapi_MaterialInfo mi = {};
  mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  mi.pNext = &opaque;
  mi.hash = 0x52425254585F6475ull;                     // "RBRTX_du"
  mi.albedoTexture = haveTex ? g_tex : L"";
  mi.normalTexture = L"";
  mi.tangentTexture = L"";
  mi.emissiveTexture = L"";
  mi.filterMode = 1;
  mi.wrapModeU = mi.wrapModeV = 2;                     // clamp to edge
  remixapi_ErrorCode err = g_api->CreateMaterial(&mi, &g_mat);
  if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
    plog(L"particles: test emitter: CreateMaterial failed, error %d", static_cast<int>(err));
    g_failed = true;
    return false;
  }

  // Flat 0.8 m quad facing up (render space is Y up); particles leave along its normal.
  static remixapi_HardcodedVertex v[4] = {};
  const float c[4][2] = { { -0.4f, -0.4f }, { 0.4f, -0.4f }, { 0.4f, 0.4f }, { -0.4f, 0.4f } };
  for (int i = 0; i < 4; ++i) {
    v[i].position[0] = c[i][0]; v[i].position[1] = 0.0f; v[i].position[2] = c[i][1];
    v[i].normal[1] = 1.0f;
    v[i].texcoord[0] = c[i][0] > 0 ? 1.0f : 0.0f; v[i].texcoord[1] = c[i][1] > 0 ? 1.0f : 0.0f;
    v[i].color = 0xFFFFFFFF;
  }
  static const uint32_t idx[6] = { 0, 2, 1, 0, 3, 2 };
  remixapi_MeshInfoSurfaceTriangles tri = {};
  tri.vertices_values = v;
  tri.vertices_count = 4;
  tri.indices_values = idx;
  tri.indices_count = 6;
  tri.material = g_mat;
  remixapi_MeshInfo mesh = {};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 0x52425254585F656Dull;                   // "RBRTX_em"
  mesh.surfaces_values = &tri;
  mesh.surfaces_count = 1;
  err = g_api->CreateMesh(&mesh, &g_mesh);
  if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
    plog(L"particles: test emitter: CreateMesh failed, error %d", static_cast<int>(err));
    g_failed = true;
    return false;
  }
  g_made = true;
  plog(L"particles: test emitter made (texture %s%s)", haveTex ? g_tex : L"none, plain colour", haveTex ? L"" : L"");
  return true;
}

// The wheels' ground points (physics space) from RBR's tire signal (all four, or the two furthest back along the
// travel direction), with the wheel number for its surface colour; else a guess around the car (wheel -1).
bool g_rearFromGame = false;
int wheel_points(const float* car, float out[4][3], int wheelOf[4]) {
  const ULONGLONG now = GetTickCount64();
  int n = 0;
  for (const Wheel& w : g_wheels) if (w.t && now - w.t < 250) ++n;
  auto back = [&](const float* p) {                    // distance ahead of the car along the travel direction
    return (p[0] - car[0]) * g_dir[0] + (p[1] - car[1]) * g_dir[1] + (p[2] - car[2]) * g_dir[2];
  };
  const bool fromGame = n == 4;
  const int count = g_cfg.wheels;
  if (fromGame && count == 4) {
    for (int i = 0; i < 4; ++i) { std::memcpy(out[i], g_wheels[i].p, sizeof(out[i])); wheelOf[i] = i; }
  } else if (fromGame) {
    int a = 0, b = 1;
    if (back(g_wheels[b].p) < back(g_wheels[a].p)) std::swap(a, b);
    for (int i = 2; i < 4; ++i) {
      if (back(g_wheels[i].p) < back(g_wheels[a].p)) { b = a; a = i; }
      else if (back(g_wheels[i].p) < back(g_wheels[b].p)) b = i;
    }
    std::memcpy(out[0], g_wheels[a].p, sizeof(out[0]));
    std::memcpy(out[1], g_wheels[b].p, sizeof(out[1]));
    wheelOf[0] = a;
    wheelOf[1] = b;
  } else {
    for (int w = 0; w < 4; ++w) wheelOf[w] = -1;
    const float side[3] = { g_dir[1], -g_dir[0], 0.0f };   // physics space is Z up
    const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1]) + 1e-6f;
    for (int w = 0; w < count; ++w)
      for (int i = 0; i < 3; ++i)
        out[w][i] = car[i] + g_dir[i] * (w < 2 ? -1.3f : 1.3f) + side[i] / sl * (w % 2 ? -0.75f : 0.75f) -
                    (i == 2 ? 0.5f : 0.0f);
  }
  static bool logged = false;
  if (fromGame != g_rearFromGame || !logged)
    plog(L"particles: wheel dust from %s", fromGame ? L"RBR's tire signals" : L"a guess around the car (no tire signals)");
  logged = true;
  g_rearFromGame = fromGame;
  return count;
}

// Physics\physics.lsp names RBR's surfaces, one per line: Tarmac_Fine_Clean_Dry ("Tarmac_Fine_Clean_Dry" ; id 37, rev 42
// A wheel's material is this id, and it changes with the track condition (Gravel_Coarse_Deep_Dry 41, _Wet 43).
std::unordered_set<uint32_t> g_tarmacDry, g_snowSurface;
bool g_tarmacRead = false;

void read_surface_table() {
  g_tarmacRead = true;
  std::ifstream in("Physics\\physics.lsp", std::ios::binary);
  std::string line;
  unsigned total = 0;
  while (std::getline(in, line)) {
    const size_t q = line.find("(\"");
    if (q == std::string::npos) continue;
    const size_t e = line.find('"', q + 2);
    if (e == std::string::npos) continue;
    const size_t id = line.find("; id ", e);
    if (id == std::string::npos) continue;
    ++total;
    const std::string name = line.substr(q + 2, e - q - 2);
    if (name.rfind("Tarmac_", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, "_Dry") == 0)
      g_tarmacDry.insert(static_cast<uint32_t>(std::atoi(line.c_str() + id + 5)));
    if (name.rfind("Snow", 0) == 0) g_snowSurface.insert(static_cast<uint32_t>(std::atoi(line.c_str() + id + 5)));
  }
  plog(L"particles: physics.lsp: %u surfaces, %zu dry tarmac (tyre smoke only there, not in rain), %zu snow (their own dust)",
       total, g_tarmacDry.size(), g_snowSurface.size());
}

bool snow_surface(uint32_t mat) {
  if (!g_tarmacRead) read_surface_table();
  return g_snowSurface.count(mat) != 0;
}

// Tyre smoke (user 2026-11-05): on tarmac only, none when damp, wet or raining.
bool smoke_surface(uint32_t mat) {
  if (!g_tarmacRead) read_surface_table();
  if (!g_tarmacDry.count(mat)) return false;
  float pct = 0.0f;
  return rbrrtx_precip_state(&pct) != 1;
}

void draw_emitter() {
  float r[20];
  if (!rbrrtx_player_car_raw(r)) return;
  update_speed(r + 16);
  if (camera_hidden() || !make_emitter()) return;
  if (g_coverageSent != g_maxCoverage) {
    char v[32];
    sprintf_s(v, "%.3f", g_maxCoverage);
    if (rbrrtx_remix_set("rtx.particles.maxScreenCoverage", v)) g_coverageSent = g_maxCoverage;
  }
  const ULONGLONG now = GetTickCount64();
  float wheel[4][3];
  int wheelOf[4];
  const int n = wheel_points(r + 16, wheel, wheelOf);
  for (int w = 0; w < n; ++w) {
    // Physics space = render space with Y and Z swapped (game_state.cpp). Emitter `height` above the ground point.
    if (wheelOf[w] < 0) continue;                      // guessed position: the game said nothing about this wheel
    const Wheel& wh = g_wheels[wheelOf[w]];
    const DustCfg* fx = nullptr;
    if (g_moving && now - wh.dustT < g_dustHoldMs) fx = snow_surface(wh.mat) ? &g_snowDust : &g_cfg;   // the game asks for dust here
    else if (g_smoke.enabled && g_kmh >= g_smoke.onKmh && now - wh.smokeLast < g_dustHoldMs &&
             wh.smokeLast - wh.smokeStart >= static_cast<ULONGLONG>(g_smoke.slideSeconds * 1000.0f) && smoke_surface(wh.mat))
      fx = &g_smoke;                                   // dry tarmac: a slide long enough
    if (!fx) continue;
    const float x = wheel[w][0], y = wheel[w][2] + fx->height, z = wheel[w][1];
    const float* tint = nullptr;
    if (fx->surfaceColour) {
      const auto it = g_surface.find(g_wheels[wheelOf[w]].mat);
      if (it != g_surface.end() && it->second.found) tint = it->second.rgb;
    }
    static DescStore store;                            // drawn at once, so one store serves every wheel
    remixapi_InstanceInfoParticleSystemEXT particles = fx_desc(*fx, tint, store);
    remixapi_InstanceInfoBlendEXT blend = {};
    fill_blend(blend, &particles);
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.pNext = &blend;
    inst.mesh = g_mesh;
    inst.transform = { { { 1, 0, 0, x }, { 0, 1, 0, y }, { 0, 0, 1, z } } };
    inst.doubleSided = 1;
    const remixapi_ErrorCode err = g_api->DrawInstance(&inst);
    if (++g_drawn == 1 || err != REMIXAPI_ERROR_CODE_SUCCESS && g_drawn < 5)
      plog(L"particles: test emitter drawn at %.2f %.2f %.2f (render space): DrawInstance returned %d", x, y, z,
           static_cast<int>(err));
  }
}

// ---------------------------------------------------------------- rain and snow (RBR's CEmitterRain -> Remix API)
// RBR's rain and snow (static RE 2026-10-23, re/kb/systems/particles.md "Rain and snow"): up to three CEmitterRain per
// stage (rain: g_rain 1000 drops + g_rain2 20; snow: g_snow 200, g_snow2 10 above 75 %, g_snow3 100). Each is a fixed
// pool of drops wrapped in a box around the camera: +0x30 pool size, +0x38 particle class (name at +0), +0x3C fall
// speed, +0x44 size, +0x0C box, +0x10 box centre (camera + forward x box/2), +0x34 drops (0x40 each, +0x2C alpha).
// Drops shown = pool x weather intensity (weather object [0x7C36A8]: +0x2C + +0x34 x +0x30, percent).
// Run 1 (2026-10-23 20:08, 6 stages): the game still draws them with ParticleQuality = off (rain: one fixed-function
// draw of ~2000 triangles a frame, texture 40A8944300F54468, plus 869E5A5AF4FA4D7B for g_rain2; snow: Particle.vso
// draws with 40A8944300F54468, 254D6B130606B00C, 869E5A5AF4FA4D7B); intensity 100 heavy, 50 light; camera and drops
// in physics space (Z up). The emitters are found through their own camera signal: CSigCamera connection at
// emitter +0x4C, vtable 0x74D1AC slot 1 = thunk 0x5DA8B0 -> rain_cam_invoke (run 1 read them through the tire
// signal's CParticleSystem, which gave nothing while driving, so Alt+7 cut nothing).
// Replacement (user 2026-10-23, run 2): the game's pools set to 0 (no update; the draws of its textures skipped as
// well while its emitters live), and one Remix particle system per weather: a square spawner facing down, spawner_above
// over the camera and spawner_ahead (+ per km/h) in front of it, drops leaving it at rise_speed. Alt+7: Remix
// rain/snow <-> the game's own (A/B). RBRRTX_RAIN_PROBE=1 adds the run-1 lines (emitters, draws per texture).
// Run 2 (user): the swap works (snow fully, rain near the camera); light and heavy weather should differ in drop size
// and speed, not only count. So four presets (particles.ini [rain_light] [rain_heavy] [snow_light] [snow_heavy]),
// heavy from heavy_from percent intensity (light 50, heavy 100), each with its own constant rate. Snow spawns from
// spawner_layers squares stacked spawner_depth deep (a volume around the camera): slow flakes from one square far
// above would land behind the car at speed.
bool g_rainProbe = false, g_rainHooked = false;
bool g_precip = false;                                 // Remix rain/snow in this run (RBRRTX_PRECIP)
bool g_replace = false;                                // game rain/snow cut (and Remix drawn when g_precip); Alt+7
constexpr uintptr_t kRainVtSlot = 0x0074D1B0, kRainThunk = 0x005DA8B0, kWeather = 0x007C36A8;
constexpr ULONGLONG kLiveMs = 250;                     // an emitter whose camera signal came this recently is alive
float g_camPos[3] = {}, g_camFwd[3] = {};             // from the rain emitters' camera signal (physics space)
unsigned g_camSigs = 0;
struct GameRain { void* em = nullptr; ULONGLONG t = 0; uint32_t pool = 0; int kind = 0; char name[32] = {}; };
GameRain g_em[4];                                      // kind: 1 rain, 2 snow, 0 unknown
struct DrawStat { unsigned long long hash = 0; unsigned draws = 0, prims = 0; bool ff = false; };
DrawStat g_rainDraws[12];
unsigned g_rainDrawOther = 0, g_rainReports = 0, g_gameSkipped = 0;
ULONGLONG g_rainLast = 0;
unsigned long long g_gameTex[8] = {};
int g_nGameTex = 0;

void load_game_textures(const wchar_t* list) {
  wchar_t buf[512];
  wcscpy_s(buf, list);
  g_nGameTex = 0;
  wchar_t* ctx = nullptr;
  for (wchar_t* t = wcstok_s(buf, L", ", &ctx); t && g_nGameTex < 8; t = wcstok_s(nullptr, L", ", &ctx))
    g_gameTex[g_nGameTex++] = wcstoull(t, nullptr, 16);
}

bool rd32(uintptr_t p, uint32_t* out) {
  __try { *out = *reinterpret_cast<volatile const uint32_t*>(p); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float rdf(uintptr_t p) {
  uint32_t v = 0;
  if (!rd32(p, &v)) return -1.0f;
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void __fastcall rain_cam_invoke(void* conn, void* /*edx*/, const uint8_t* sig) {
  std::memcpy(g_camPos, sig + 0x24, sizeof(g_camPos));
  std::memcpy(g_camFwd, sig + 0x0C, sizeof(g_camFwd));
  ++g_camSigs;
  void* em = static_cast<uint8_t*>(conn) - 0x4C;
  const ULONGLONG now = GetTickCount64();
  GameRain* slot = nullptr;
  for (GameRain& g : g_em) if (g.em == em) { slot = &g; break; }
  if (!slot) {                                         // new emitter (new stage): the oldest slot
    slot = &g_em[0];
    for (GameRain& g : g_em) if (g.t < slot->t) slot = &g;
    *slot = {};
    slot->em = em;
    uint32_t cls = 0;
    if (!rd32(reinterpret_cast<uintptr_t>(em) + 0x38, &cls) || !cls ||
        !readable_string(cls, slot->name, sizeof(slot->name)))
      strcpy_s(slot->name, "?");
    slot->kind = strstr(slot->name, "snow") ? 2 : strstr(slot->name, "rain") ? 1 : 0;
    plog(L"rain: game emitter %S at %p (%s)", slot->name, em, slot->kind == 2 ? L"snow" : slot->kind == 1 ? L"rain" : L"?");
  }
  slot->t = now;
  reinterpret_cast<InvokeFn>(kRainThunk)(conn, sig);
}

void install_rain_hook() {
  uint32_t* slot = reinterpret_cast<uint32_t*>(kRainVtSlot);
  if (*slot != kRainThunk) {
    plog(L"rain: camera signal hook NOT installed: vtable slot %p holds %08X (other exe build?)", slot, *slot);
    return;
  }
  DWORD old = 0;
  if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return;
  *slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&rain_cam_invoke));
  VirtualProtect(slot, 4, old, &old);
  g_rainHooked = true;
  plog(L"rain: camera signal hook at %p (RBR's rain/snow emitters)", slot);
}

bool em_live(const GameRain& g) { return g.em && GetTickCount64() - g.t < kLiveMs; }

// 1 rain, 2 snow, 0 none (no live game emitter).
int precip_kind() {
  int k = 0;
  for (const GameRain& g : g_em) if (em_live(g) && g.kind > k) k = g.kind;
  return k;
}

float weather_intensity() {
  uint32_t w = 0;
  if (!rd32(kWeather, &w) || !w) return 0.0f;
  return rdf(w + 0x2C) + rdf(w + 0x34) * rdf(w + 0x30);
}

// Pools 0 while replaced, the stage's own sizes back when not. Only live emitters are written (a stage being left
// frees them; a signal within kLiveMs means the stage still runs).
void rain_cut_frame() {
  for (GameRain& g : g_em) {
    if (!em_live(g)) continue;
    uint32_t* pool = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g.em) + 0x30);
    __try {
      if (g_replace && *pool) { g.pool = *pool; *pool = 0; }
      else if (!g_replace && g.pool && !*pool) { *pool = g.pool; g.pool = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
}

// ---- Remix side
struct PrecipFx { remixapi_MaterialHandle mat = nullptr; remixapi_MeshHandle mesh = nullptr; bool failed = false; };
PrecipFx g_fx[PRE_COUNT];
unsigned g_fxEpoch[PRE_COUNT] = {};
float g_camKmh = 0.0f, g_lastCam[3] = {};
LARGE_INTEGER g_camLastT = {};
bool g_haveCam = false;
int g_shownKind = -1, g_shownStep = -1;

// Snow wake spawner: the car's shell in metres (x across, y up from the car's root, z along the car): roof at
// `above`, the two sides and the rear, each facing out. Remix takes the normal from the triangle's winding.
bool make_shell(int kind, const DustCfg& c, PrecipFx& f) {
  static remixapi_HardcodedVertex v[16] = {};
  static uint32_t idx[24] = {};
  const float L = c.holeLength * 0.5f, W = c.holeWidth * 0.5f, H = c.above;
  struct Quad { float p[4][3]; float n[3]; };
  const Quad q[4] = {
    { { { -W * 0.8f, H, -L * 0.5f }, { W * 0.8f, H, -L * 0.5f }, { W * 0.8f, H, L * 0.3f }, { -W * 0.8f, H, L * 0.3f } }, { 0, 1, 0 } },
    { { { -W, 0.2f, -L }, { -W, 0.2f, L }, { -W, H, L }, { -W, H, -L } }, { -1, 0, 0 } },
    { { { W, 0.2f, -L }, { W, 0.2f, L }, { W, H, L }, { W, H, -L } }, { 1, 0, 0 } },
    { { { -W, 0.2f, -L }, { W, 0.2f, -L }, { W, H, -L }, { -W, H, -L } }, { 0, 0, -1 } },   // rear (car forward = +z)
  };
  for (int k = 0; k < 4; ++k) {
    float pt[4][3];
    std::memcpy(pt, q[k].p, sizeof(pt));
    const float e1[3] = { pt[1][0] - pt[0][0], pt[1][1] - pt[0][1], pt[1][2] - pt[0][2] };
    const float e2[3] = { pt[2][0] - pt[0][0], pt[2][1] - pt[0][1], pt[2][2] - pt[0][2] };
    const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    if (n[0] * q[k].n[0] + n[1] * q[k].n[1] + n[2] * q[k].n[2] < 0.0f)   // wrong winding: swap corners 1 and 3
      for (int i = 0; i < 3; ++i) std::swap(pt[1][i], pt[3][i]);
    for (int i = 0; i < 4; ++i) {
      remixapi_HardcodedVertex& o = v[k * 4 + i];
      o = {};
      std::memcpy(o.position, pt[i], sizeof(pt[i]));
      std::memcpy(o.normal, q[k].n, sizeof(q[k].n));
      o.texcoord[0] = i == 1 || i == 2 ? 1.0f : 0.0f; o.texcoord[1] = i >= 2 ? 1.0f : 0.0f;
      o.color = 0xFFFFFFFF;
    }
    const uint32_t b = k * 4, t[6] = { b, b + 1, b + 2, b, b + 2, b + 3 };
    std::memcpy(&idx[k * 6], t, sizeof(t));
  }
  remixapi_MeshInfoSurfaceTriangles tri = {};
  tri.vertices_values = v;
  tri.vertices_count = 16;
  tri.indices_values = idx;
  tri.indices_count = 24;
  tri.material = f.mat;
  remixapi_MeshInfo mesh = {};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 0x52425254585F7330ull + kind + 16ull * g_precipEpoch;
  mesh.surfaces_values = &tri;
  mesh.surfaces_count = 1;
  if (g_api->CreateMesh(&mesh, &f.mesh) != REMIXAPI_ERROR_CODE_SUCCESS) {
    plog(L"rain: CreateMesh failed (%s)", kPreSection[kind]);
    f.failed = true;
    return false;
  }
  plog(L"rain: Remix %s spawner made: car shell %.1f x %.1f m, roof %.2f m", kPreSection[kind], c.holeLength, c.holeWidth, H);
  return true;
}

bool make_precip(int kind) {                            // kind = preset (PRE_*)
  PrecipFx& f = g_fx[kind];
  if (f.mesh && g_fxEpoch[kind] != g_precipEpoch && g_api) {   // particles.ini saved: layers/texture may have changed
    g_api->DestroyMesh(f.mesh);
    g_api->DestroyMaterial(f.mat);
    f = {};
  }
  g_fxEpoch[kind] = g_precipEpoch;
  if (f.mesh) return true;
  if (f.failed || !g_api) return false;
  const DustCfg& c = g_pre[kind];
  const bool snow = kind >= PRE_SNOW_LIGHT;
  static wchar_t tex[PRE_COUNT][MAX_PATH * 2] = {};
  wcscpy_s(tex[kind], g_cfgPath);                      // the sprite sits next to particles.ini
  wchar_t* slash = wcsrchr(tex[kind], L'\\');
  if (slash) slash[1] = 0;
  wcscat_s(tex[kind], c.tex);
  const bool haveTex = GetFileAttributesW(tex[kind]) != INVALID_FILE_ATTRIBUTES;
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.roughnessTexture = L"";
  opaque.metallicTexture = L"";
  opaque.heightTexture = L"";
  opaque.albedoConstant = { 0.8f, 0.82f, 0.85f };
  opaque.opacityConstant = 1.0f;
  opaque.roughnessConstant = snow ? 0.9f : 0.1f;
  opaque.useDrawCallAlphaState = 1;
  remixapi_MaterialInfo mi = {};
  mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  mi.pNext = &opaque;
  // "RBRTX_r0".."r4", new per settings read like the mesh (user 2026-10-24: light snow gone after a few particles.ini
  // saves; the old material was destroyed and one with the same hash made in the same frame, and Remix may carry the
  // destroy out after the create, leaving the spawner without its material).
  mi.hash = (0x52425254585F7230ull + kind) ^ (static_cast<uint64_t>(g_precipEpoch) << 40);
  mi.albedoTexture = haveTex ? tex[kind] : L"";
  mi.normalTexture = L"";
  mi.tangentTexture = L"";
  mi.emissiveTexture = c.selfLight > 0.0f && haveTex ? tex[kind] : L"";   // the sprite's own shape and colour glow
  mi.emissiveIntensity = c.selfLight;
  mi.emissiveColorConstant = { 1.0f, 1.0f, 1.0f };
  mi.filterMode = 1;
  mi.wrapModeU = mi.wrapModeV = 2;
  if (g_api->CreateMaterial(&mi, &f.mat) != REMIXAPI_ERROR_CODE_SUCCESS) {
    plog(L"rain: CreateMaterial failed (%s)", kPreSection[kind]);
    f.failed = true;
    return false;
  }
  // Squares facing down (render space is Y up), `layers` of them from y = 0 down to y = -depth. Remix spawns over the
  // triangles' area, so each layer gets an equal share. Without a car gap: unit squares, the transform scales x/z to
  // spawner_size. With one: metres, x = across the car, z = along it, the square centred spawner_ahead in front and
  // four rectangles around a car_gap_length x car_gap_width hole at the origin (the transform puts it on the car).
  static remixapi_HardcodedVertex v[PRE_COUNT][16 * 16] = {};
  static uint32_t idx[PRE_COUNT][24 * 16] = {};
  const int layers = c.layers < 1 ? 1 : c.layers > 16 ? 16 : c.layers;
  const bool gap = c.holeLength > 0.0f && c.holeWidth > 0.0f;
  float rects[4][4] = { { -0.5f, -0.5f, 0.5f, 0.5f } };   // x0 z0 x1 z1
  int nRects = 1;
  if (c.spawner == 1) return make_shell(kind, c, f);
  if (gap) {
    const float h = c.box * 0.5f, z0 = c.ahead - h, z1 = c.ahead + h, gl = c.holeLength * 0.5f, gw = c.holeWidth * 0.5f;
    const float r[4][4] = { { -h, z0, h, -gl }, { -h, gl, h, z1 }, { -h, -gl, -gw, gl }, { gw, -gl, h, gl } };
    std::memcpy(rects, r, sizeof(r));
    nRects = 4;
  }
  int nv = 0, ni = 0;
  for (int l = 0; l < layers; ++l) {
    const float y = layers > 1 ? -c.depth * l / (layers - 1) : 0.0f;
    for (int r = 0; r < nRects; ++r) {
      const float* R = rects[r];
      if (R[2] <= R[0] || R[3] <= R[1]) continue;      // gap wider than the square
      const float q[4][2] = { { R[0], R[1] }, { R[2], R[1] }, { R[2], R[3] }, { R[0], R[3] } };
      for (int i = 0; i < 4; ++i) {
        remixapi_HardcodedVertex& o = v[kind][nv + i];
        o = {};
        o.position[0] = q[i][0]; o.position[1] = y; o.position[2] = q[i][1];
        o.normal[1] = -1.0f;
        o.texcoord[0] = i == 1 || i == 2 ? 1.0f : 0.0f; o.texcoord[1] = i >= 2 ? 1.0f : 0.0f;
        o.color = 0xFFFFFFFF;
      }
      const uint32_t b = static_cast<uint32_t>(nv), t[6] = { b, b + 1, b + 2, b, b + 2, b + 3 };
      std::memcpy(&idx[kind][ni], t, sizeof(t));
      nv += 4;
      ni += 6;
    }
  }
  remixapi_MeshInfoSurfaceTriangles tri = {};
  tri.vertices_values = v[kind];
  tri.vertices_count = nv;
  tri.indices_values = idx[kind];
  tri.indices_count = ni;
  tri.material = f.mat;
  remixapi_MeshInfo mesh = {};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 0x52425254585F7330ull + kind + 16ull * g_precipEpoch;   // "RBRTX_s0".., new per settings read
  mesh.surfaces_values = &tri;
  mesh.surfaces_count = 1;
  if (g_api->CreateMesh(&mesh, &f.mesh) != REMIXAPI_ERROR_CODE_SUCCESS) {
    plog(L"rain: CreateMesh failed (%s)", kPreSection[kind]);
    f.failed = true;
    return false;
  }
  plog(L"rain: Remix %s spawner made: %d layer(s), %s, texture %s", kPreSection[kind], layers,
       gap ? L"on the car with a gap around it" : L"in front of the camera", haveTex ? tex[kind] : L"none: plain colour");
  return true;
}

// Camera speed (km/h) from the signal's positions between frames; jumps over 100 m/s (camera cuts) are skipped.
void update_cam_speed() {
  LARGE_INTEGER t, f;
  QueryPerformanceCounter(&t);
  QueryPerformanceFrequency(&f);
  if (g_haveCam) {
    const double dt = static_cast<double>(t.QuadPart - g_camLastT.QuadPart) / f.QuadPart;
    if (dt > 0.0005) {
      const float dx = g_camPos[0] - g_lastCam[0], dy = g_camPos[1] - g_lastCam[1], dz = g_camPos[2] - g_lastCam[2];
      const float ms = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / dt);
      if (ms < 100.0f) g_camKmh += (ms * 3.6f - g_camKmh) * 0.1f;
    }
  }
  g_camLastT = t;
  std::memcpy(g_lastCam, g_camPos, sizeof(g_lastCam));
  g_haveCam = true;
}

void draw_one(int pre, float rateScale);

// ---- car kill box (user 2026-11-01). Runtime side: RBR-RTX-remixplus-patches/particles-killbox.patch. Capability check:
// the patched runtime sets the game value "__rbrrtx.killbox.v" = 1 on its first simulation frame; a runtime without it
// never answers, and then nothing is sent and bit 0x40 is never set (it would break the collision mode there).
// The pose is RBR's player car (physics -> render as draw_one), not g_carWorld: that holds the last lit car part drawn,
// which can be a wheel.
typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_SetGameValue)(const char* key, const char* value);
typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_GetGameValue)(const char* key, char* out, uint32_t size, uint32_t* actual);
bool g_kbSent = false;
int g_kbPolls = 0;
ULONGLONG g_kbPollT = 0;

bool killbox_runtime() {
  if (g_kbRuntime) return true;
  const RbrApiExt* x = rbrrtx_api_ext();
  if (!x || !x->setGameValue || !x->getGameValue || g_kbPolls >= 20) return false;   // 20 tries, 3 s apart
  const ULONGLONG now = GetTickCount64();
  if (g_kbPolls && now - g_kbPollT < 3000) return false;
  ++g_kbPolls;
  g_kbPollT = now;
  char v[8] = {};
  uint32_t n = 0;
  const remixapi_ErrorCode err = reinterpret_cast<PFN_GetGameValue>(x->getGameValue)("__rbrrtx.killbox.v", v, sizeof(v), &n);
  g_kbRuntime = err == REMIXAPI_ERROR_CODE_SUCCESS && n > 0 && v[0] == '1';
  if (g_kbRuntime) plog(L"rain: the runtime has the car kill box (try %d)", g_kbPolls);
  else if (g_kbPolls == 20) plog(L"rain: the runtime has no car kill box (patch particles-killbox); [killbox] is not used");
  return g_kbRuntime;
}

// Once per frame: the box for the runtime, or "0" once when it is no longer wanted.
void killbox_frame(bool precip) {
  const RbrApiExt* x = rbrrtx_api_ext();
  float car[20];
  const bool want = precip && g_kb.enabled && killbox_runtime() && rbrrtx_player_car_raw(car);
  if (!want) {
    if (g_kbSent && x && x->setGameValue) reinterpret_cast<PFN_SetGameValue>(x->setGameValue)("__rbrrtx.carbox", "0");
    g_kbSent = false;
    return;
  }
  float ax = car[7], az = car[8];
  const float al = std::sqrt(ax * ax + az * az);
  if (al > 1e-3f) { ax /= al; az /= al; } else { ax = 0.0f; az = 1.0f; }
  char s[192];
  snprintf(s, sizeof(s), "1 %.3f %.3f %.3f %.4f 0 %.4f %.3f %.3f %.3f", car[16], car[18] + g_kb.base + g_kb.height * 0.5f,
           car[17], ax, az, g_kb.length * 0.5f + g_kb.margin, g_kb.width * 0.5f + g_kb.margin,
           g_kb.height * 0.5f + g_kb.margin);
  reinterpret_cast<PFN_SetGameValue>(x->setGameValue)("__rbrrtx.carbox", s);
  if (!g_kbSent) plog(L"rain: car kill box on: %S", s);
  g_kbSent = true;
}

// Rain in the main menu's bad weather (comp.cpp menu weather): no game emitters there, so the camera comes from the proxy.
float g_menuRain = 0.0f;

void draw_precip() {
  int kind = precip_kind();
  float inten = kind ? weather_intensity() : 0.0f;
  if (!kind && g_menuRain > 0.0f) {
    float pos[3], fwd[3];
    if (rbrrtx_menu_camera(pos, fwd)) {                // render space; these are physics space, Y and Z swapped
      g_camPos[0] = pos[0]; g_camPos[1] = pos[2]; g_camPos[2] = pos[1];
      g_camFwd[0] = fwd[0]; g_camFwd[1] = fwd[2]; g_camFwd[2] = fwd[1];
      kind = 1;
      inten = g_menuRain;
    }
  }
  const int pre = !kind || inten < 1.0f ? -1 : (kind == 2 ? PRE_SNOW_LIGHT : PRE_RAIN_LIGHT) + (inten >= g_heavyFrom ? 1 : 0);
  if (kind != g_shownKind || pre != g_shownStep) {
    plog(L"rain: Remix %s, intensity %.0f %%", pre < 0 ? L"none" : kPreSection[pre], inten);
    g_shownKind = kind;
    g_shownStep = pre;
  }
  killbox_frame(pre >= 0);
  if (pre < 0) return;
  update_cam_speed();
  draw_one(pre, 1.0f);
  if (kind == 2 && g_pre[PRE_SNOW_WAKE].perWheelRate > 0.0f) {   // snow the car stirs up, in speed steps
    const DustCfg& w = g_pre[PRE_SNOW_WAKE];
    const float k = g_camKmh;
    const float scale = k < w.wakeOnKmh ? 0.0f : k < 50.0f ? 0.25f : k < 90.0f ? 0.5f : 1.0f;
    static float shown = -1.0f;
    if (scale != shown) { plog(L"rain: snow wake %.0f %% (%.0f km/h)", scale * 100.0f, k); shown = scale; }
    // Outside views only (user run 7: as the dust, rbrrtx_car_body_visible): from the cockpit the wake is snow
    // inside and around the cabin.
    if (scale > 0.0f && rbrrtx_car_body_visible() == 1) draw_one(PRE_SNOW_WAKE, scale);
  }
}

// One preset's spawner. rateScale: a few fixed steps only (a new rate = a new Remix particle system).
void draw_one(int pre, float rateScale) {
  if (!make_precip(pre)) return;
  DustCfg c = g_pre[pre];
  c.perWheelRate *= rateScale;
  // One descriptor per effect, kept: rebuilt only when particles.ini was read again, the wake's rate step changed or
  // the runtime's car kill box became known; the same descriptor is the same Remix particle system (no per-frame
  // arrays, hashing or re-created systems on our side).
  struct Cached { DescStore store; remixapi_InstanceInfoParticleSystemEXT desc = {}; unsigned epoch = ~0u; float scale = -1.0f; bool kb = false; };
  static Cached cache[PRE_COUNT];
  Cached& ch = cache[pre];
  if (ch.epoch != g_precipEpoch || ch.scale != rateScale || ch.kb != g_kbRuntime) {
    ch.store = DescStore();
    ch.desc = fx_desc(c, nullptr, ch.store);
    ch.epoch = g_precipEpoch;
    ch.scale = rateScale;
    ch.kb = g_kbRuntime;
  }
  remixapi_InstanceInfoParticleSystemEXT& particles = ch.desc;
  remixapi_InstanceInfoBlendEXT blend = {};
  fill_blend(blend, &particles);
  // Physics space = render space with Y and Z swapped. Ahead along the camera's level forward direction.
  const float fx = g_camFwd[0], fz = g_camFwd[1];
  const float fl = std::sqrt(fx * fx + fz * fz);
  float x = 0.0f, y = g_camPos[2] + c.above, z = 0.0f;
  remixapi_InstanceInfo inst = {};
  const bool onCar = c.spawner == 1 || (c.holeLength > 0.0f && c.holeWidth > 0.0f);
  if (onCar) {
    // On the car: its length axis (r+7, physics space, Y/Z swapped to render) made level, pointing the camera's way
    // (the model's forward sign differs per car); the gap / shell sits on the car. Car pose unreadable: not drawn
    // (run 4: the fallback below would have scaled this metre-sized mesh by spawner_size).
    float car[20];
    if (!rbrrtx_player_car_raw(car)) return;
    float ax = car[7], az = car[8];
    const float al = std::sqrt(ax * ax + az * az);
    if (al > 1e-3f) { ax /= al; az /= al; } else { ax = 0.0f; az = 1.0f; }
    if (ax * fx + az * fz < 0.0f) { ax = -ax; az = -az; }
    x = car[16];
    z = car[17];
    if (c.spawner == 1) y = car[18];                   // the shell is in metres above the car's root
    // Run 14 (user: TV cameras far from the car showed the snow box's edges): a camera more than 8 m from the car gets
    // the precipitation box around itself (the car's gap goes with it, harmless there); the wake stays on the car.
    const float cdx = g_camPos[0] - x, cdz = g_camPos[1] - z;
    if (c.spawner != 1 && cdx * cdx + cdz * cdz > 64.0f) {
      x = g_camPos[0];
      z = g_camPos[1];
    }
    inst.transform = { { { az, 0, ax, x }, { 0, 1, 0, y }, { -ax, 0, az, z } } };
  } else {
    const float ahead = c.ahead + c.aheadPerKmh * g_camKmh;
    x = g_camPos[0] + (fl > 1e-3f ? fx / fl * ahead : 0.0f);
    z = g_camPos[1] + (fl > 1e-3f ? fz / fl * ahead : 0.0f);
    inst.transform = { { { c.box, 0, 0, x }, { 0, 1, 0, y }, { 0, 0, c.box, z } } };
  }
  inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  inst.pNext = &blend;
  inst.mesh = g_fx[pre].mesh;
  inst.doubleSided = 1;
  const remixapi_ErrorCode err = g_api->DrawInstance(&inst);
  static unsigned drawn[PRE_COUNT] = {};
  if (++drawn[pre] == 1 || (err != REMIXAPI_ERROR_CODE_SUCCESS && drawn[pre] < 5) || drawn[pre] % 3000 == 0)
    plog(L"rain: Remix %s spawner drawn at %.1f %.1f %.1f (render space, %s), camera %.1f %.1f %.1f, %.0f/s: "
         L"DrawInstance returned %d", kPreSection[pre], x, y, z, onCar ? L"on the car" : L"ahead of the camera",
         g_camPos[0], g_camPos[2], g_camPos[1], c.perWheelRate, static_cast<int>(err));
}

// ---- probe lines (RBRRTX_RAIN_PROBE=1)
void rain_report() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_rainLast < 10000 || !precip_kind()) return;   // stages with rain/snow only
  g_rainLast = now;
  if (g_rainReports >= 200) return;
  RbrStageInfo si = {};
  const bool haveStage = rbrrtx_stage_info(&si);
  wchar_t line[1600];
  size_t used = swprintf_s(line, L"rain: report %u: %s sky %s mode %d; intensity %.1f; cam %.1f %.1f %.1f %.0f km/h "
                           L"(%u signals); replaced %s, game draws skipped %u;", g_rainReports + 1,
                           haveStage ? si.name : L"?", haveStage ? rbrrtx_sky_name(si.skyType) : L"?", si.gameMode,
                           weather_intensity(), g_camPos[0], g_camPos[1], g_camPos[2], g_camKmh, g_camSigs,
                           g_replace ? L"yes" : L"no", g_gameSkipped);
  g_camSigs = 0;
  g_gameSkipped = 0;
  for (const GameRain& g : g_em) {
    if (!em_live(g) || used + 200 >= 1600) continue;
    const uintptr_t p = reinterpret_cast<uintptr_t>(g.em);
    uint32_t pool = 0;
    rd32(p + 0x30, &pool);
    used += swprintf_s(line + used, 1600 - used, L" %S pool %u (kept %u) fall %.1f size %.2f box %.1f;", g.name, pool,
                       g.pool, rdf(p + 0x3C), rdf(p + 0x44), rdf(p + 0x0C));
  }
  used += swprintf_s(line + used, 1600 - used, L" draws:");
  for (DrawStat& d : g_rainDraws) {
    if (!d.draws || used + 60 >= 1600) continue;
    used += swprintf_s(line + used, 1600 - used, L" %016llX %s %u/%u;", d.hash, d.ff ? L"FF" : L"VS", d.draws, d.prims);
  }
  swprintf_s(line + used, 1600 - used, L" other %u", g_rainDrawOther);
  for (DrawStat& d : g_rainDraws) d = {};
  g_rainDrawOther = 0;
  plog(L"%s", line);
  ++g_rainReports;
}

// ---------------------------------------------------------------- every other game emitter (one row per effect type)
// Every particle RBR creates goes through CEmitter_CreateParticle 0x5D9580 (fastcall, ecx = emitter, name inline at +0, spawn
// position at +0x40 in physics space), from tire signals, auto-rate emitters, collisions, exhaust and the rest (census run
// 20261108-180301: every route fires with ParticleQuality off too, and pools are always allocated). The hook gives the game a
// dummy slot for an emitter that has a row, so nothing is simulated or drawn, and notes where it fired; draw_rows then draws
// that row's Remix system at each place that fired within hold_ms. An emitter without a row stays the game's and is logged.
// Rows are keyed on the emitter's name without the stage maker's prefix (DECISIONS 2026-11-08); settings: [fx_*] in particles.ini.
enum { R_PRECIP, R_GRAVEL, R_WET, R_GRASS, R_DEBRIS, R_DEBRIS_SMOKE, R_SPLASH, R_SPLASH_SMOKE, R_CARWATER, R_EXHAUST, R_POP, R_FIRE,
       R_FIRE_SMOKE, R_STEAM, R_SPARKS, R_GLASS, R_SNOWWALL, R_SNOWWALL_SMOKE, R_STAGE_SMOKE, R_COUNT };
const wchar_t* const kRowSection[R_COUNT] = { L"fx_precip", L"fx_gravel", L"fx_wet", L"fx_grass", L"fx_debris", L"fx_debris_smoke", L"fx_splash",
  L"fx_splash_smoke", L"fx_carwater", L"fx_exhaust", L"fx_pop", L"fx_fire", L"fx_fire_smoke", L"fx_steam", L"fx_sparks", L"fx_glass",
  L"fx_snowwall", L"fx_snowwall_smoke", L"fx_stage_smoke" };

constexpr int kSites = 6;
struct Site { float p[3] = {}; ULONGLONG t = 0; };
struct Row {
  DustCfg c;
  float holdMs = 150.0f, merge = 1.5f, maxDist = 150.0f;
  bool hideCockpit = false;
  Site sites[kSites];
  PrecipFx fx;
  unsigned fxEpoch = 0;
  DescStore store;
  remixapi_InstanceInfoParticleSystemEXT desc = {};
  unsigned descEpoch = ~0u;
} g_rows[R_COUNT];

std::unordered_map<void*, int> g_emRow;                // emitter -> row, -1 = stays the game's; cleared per stage and settings read
bool g_rowsOn = false, g_raster = false, g_censusOn = false;
float g_viewPos[3] = {};                               // render space, from comp.cpp
ULONGLONG g_viewT = 0;

using CreateFn = void*(__fastcall*)(void* emitter);
CreateFn g_createOrig = nullptr;
constexpr uintptr_t kCreate = 0x005D9580;
constexpr uint8_t kCreateEntry[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x70, 0x01, 0x00, 0x00 };   // push esi; mov esi,ecx; mov eax,[esi+170h]
alignas(16) uint8_t g_dummySlot[256] = {};

DustCfg row_cfg(float rate, unsigned maxP, float life0, float life1, float s0a, float s0b, float s1a, float s1b, float rise, float cone,
                float drag, float gravity, float r, float g, float b, float a, float turbulence = 0.0f, float light = 0.0f) {
  DustCfg c;
  c.perWheelRate = rate; c.maxParticles = maxP;
  c.lifeMin = life0; c.lifeMax = life1;
  c.startSizeMin = s0a; c.startSizeMax = s0b; c.endSizeMin = s1a; c.endSizeMax = s1b;
  c.riseSpeed = rise; c.cone = cone; c.drag = drag; c.gravity = gravity;
  c.turbulenceForce = turbulence; c.turbulenceFrequency = 0.4f; c.rotation = 0.3f;
  c.selfLight = light; c.surfaceColour = false; c.height = 0.0f;
  const float sa[4] = { r * 0.9f, g * 0.9f, b * 0.9f, a * 0.8f }, sb[4] = { r, g, b, a };
  const float ea[4] = { r * 0.9f, g * 0.9f, b * 0.9f, 0.0f }, eb[4] = { r, g, b, 0.0f };
  std::memcpy(c.startColorA, sa, 16); std::memcpy(c.startColorB, sb, 16);
  std::memcpy(c.endColorA, ea, 16); std::memcpy(c.endColorB, eb, 16);
  return c;
}

Row row_defaults(int i) {
  Row r;
  switch (i) {
    //                        rate  max   life       start size   end size     rise cone drag grav    colour
    case R_GRAVEL:       r.c = row_cfg(40, 300, 0.5f, 1.0f, 0.03f, 0.06f, 0.03f, 0.06f, 3.0f, 45, 0.2f, -9.81f, 0.32f, 0.28f, 0.22f, 1.0f); r.merge = 1.0f; r.maxDist = 80; r.hideCockpit = true; break;
    case R_WET:          r.c = row_cfg(25, 300, 0.6f, 1.4f, 0.15f, 0.30f, 0.8f, 1.6f, 1.5f, 40, 1.0f, -2.0f, 0.82f, 0.84f, 0.86f, 0.25f, 0.5f); r.merge = 1.0f; r.maxDist = 100; r.hideCockpit = true; break;
    case R_GRASS:        r.c = row_cfg(12, 120, 0.8f, 1.5f, 0.04f, 0.08f, 0.04f, 0.08f, 2.0f, 50, 1.5f, -4.0f, 0.22f, 0.32f, 0.10f, 1.0f); r.merge = 1.0f; r.maxDist = 80; r.hideCockpit = true; break;
    case R_DEBRIS:       r.c = row_cfg(30, 200, 0.6f, 1.5f, 0.05f, 0.12f, 0.05f, 0.12f, 4.0f, 70, 0.2f, -9.81f, 0.30f, 0.27f, 0.23f, 1.0f); r.holdMs = 120; r.maxDist = 100; break;
    case R_DEBRIS_SMOKE: r.c = row_cfg(15, 150, 1.0f, 3.0f, 0.3f, 0.5f, 1.5f, 2.5f, 1.0f, 50, 0.9f, 0.1f, 0.55f, 0.52f, 0.48f, 0.35f, 1.0f); r.holdMs = 150; r.maxDist = 120; break;
    case R_SPLASH:       r.c = row_cfg(120, 600, 0.4f, 1.0f, 0.12f, 0.25f, 0.3f, 0.6f, 5.0f, 35, 0.3f, -9.81f, 0.86f, 0.90f, 0.95f, 0.55f); r.maxDist = 120; break;
    case R_SPLASH_SMOKE: r.c = row_cfg(25, 200, 0.8f, 2.0f, 0.5f, 0.8f, 2.0f, 3.0f, 1.0f, 50, 1.0f, -0.3f, 0.88f, 0.90f, 0.93f, 0.25f, 0.8f); r.maxDist = 120; break;
    case R_CARWATER:     r.c = row_cfg(15, 60, 0.3f, 0.7f, 0.05f, 0.1f, 0.1f, 0.2f, 1.0f, 40, 0.5f, -6.0f, 0.86f, 0.90f, 0.95f, 0.2f); r.merge = 3.0f; r.maxDist = 60; r.hideCockpit = true; break;
    case R_EXHAUST:      r.c = row_cfg(12, 40, 0.4f, 1.0f, 0.05f, 0.1f, 0.3f, 0.5f, 0.6f, 25, 1.2f, 0.3f, 0.55f, 0.55f, 0.55f, 0.12f, 0.8f); r.merge = 0.5f; r.holdMs = 250; r.maxDist = 60; r.hideCockpit = true; break;
    case R_POP:          r.c = row_cfg(60, 20, 0.05f, 0.12f, 0.1f, 0.15f, 0.2f, 0.3f, 0.5f, 30, 0.0f, 0.0f, 1.0f, 0.55f, 0.15f, 0.9f, 0.0f, 8.0f); r.merge = 0.5f; r.holdMs = 80; r.maxDist = 80; r.hideCockpit = true; break;
    case R_FIRE:         r.c = row_cfg(20, 40, 0.3f, 0.7f, 0.2f, 0.3f, 0.4f, 0.6f, 2.0f, 20, 0.5f, 1.0f, 1.0f, 0.45f, 0.1f, 0.8f, 1.0f, 6.0f); r.holdMs = 250; r.maxDist = 120; break;
    case R_FIRE_SMOKE:   r.c = row_cfg(15, 60, 1.5f, 3.0f, 0.4f, 0.6f, 2.0f, 3.0f, 1.5f, 25, 0.7f, 0.6f, 0.08f, 0.08f, 0.08f, 0.5f, 1.2f); r.holdMs = 250; r.maxDist = 150; break;
    case R_STEAM:        r.c = row_cfg(12, 40, 0.6f, 1.4f, 0.2f, 0.3f, 0.8f, 1.2f, 1.0f, 25, 1.0f, 0.5f, 0.86f, 0.86f, 0.86f, 0.2f, 1.0f); r.merge = 3.0f; r.holdMs = 250; r.maxDist = 80; r.hideCockpit = true; break;
    case R_SPARKS:       r.c = row_cfg(80, 60, 0.15f, 0.4f, 0.01f, 0.02f, 0.01f, 0.02f, 4.0f, 90, 0.1f, -9.81f, 1.0f, 0.7f, 0.3f, 1.0f, 0.0f, 20.0f); r.c.trail = 0.5f; r.c.align = true; r.holdMs = 80; r.maxDist = 80; break;
    case R_GLASS:        r.c = row_cfg(40, 60, 0.5f, 1.0f, 0.01f, 0.03f, 0.01f, 0.03f, 2.5f, 80, 0.2f, -9.81f, 0.85f, 0.92f, 0.95f, 0.8f); r.holdMs = 120; r.maxDist = 60; break;
    case R_SNOWWALL:     r.c = row_cfg(40, 200, 0.5f, 1.2f, 0.1f, 0.2f, 0.3f, 0.5f, 3.0f, 45, 0.6f, -6.0f, 0.95f, 0.96f, 0.98f, 0.8f); r.maxDist = 120; break;
    case R_SNOWWALL_SMOKE: r.c = row_cfg(20, 150, 0.8f, 2.0f, 0.4f, 0.7f, 1.8f, 2.6f, 1.0f, 50, 1.0f, -0.3f, 0.95f, 0.96f, 0.98f, 0.3f, 0.8f); r.maxDist = 120; break;
    case R_STAGE_SMOKE:  r.c = row_cfg(8, 200, 4.0f, 8.0f, 1.0f, 1.5f, 5.0f, 7.0f, 1.0f, 20, 0.5f, 0.3f, 0.55f, 0.55f, 0.56f, 0.25f, 0.6f); r.holdMs = 400; r.merge = 3.0f; r.maxDist = 300; break;
    default: break;
  }
  return r;
}

void rows_load(bool found) {
  for (int i = 0; i < R_COUNT; ++i) {
    g_rows[i] = row_defaults(i);
    if (!found) continue;
    Row& r = g_rows[i];
    g_sec = kRowSection[i];
    load_fx(r.c);
    r.c.surfaceColour = false;
    r.holdMs = cfg_float(L"hold_ms", r.holdMs, 16.0f, 5000.0f);
    r.merge = cfg_float(L"merge_metres", r.merge, 0.1f, 50.0f);
    r.maxDist = cfg_float(L"max_distance", r.maxDist, 5.0f, 5000.0f);
    r.hideCockpit = cfg_float(L"hide_in_cockpit", r.hideCockpit ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
  }
  g_sec = L"dust";
  g_emRow.clear();
}

void rows_stage_reset() {
  g_emRow.clear();
  for (Row& r : g_rows) for (Site& s : r.sites) s = {};
}

// Lower-case emitter name -> row. Dry wheel dust and smoke (-1) are cut earlier, by emit_hook. Order matters: the longer names first.
int classify(const char* n) {
  const auto has = [n](const char* s) { return strstr(n, s) != nullptr; };
  if (has("headlights")) return -1;
  if ((has("g_rain") || has("g_snow")) && !has("snowwall")) return R_PRECIP;
  if (has("grass")) return R_GRASS;
  if (has("wet") && (has("tire") || has("gravel") || has("fakesmoke"))) return R_WET;
  if (has("tirespray") || has("tiresmoke")) return -1;
  if (has("debris_smoke")) return R_DEBRIS_SMOKE;
  if (has("debris")) return R_DEBRIS;
  if (has("splash_smoke")) return R_SPLASH_SMOKE;
  if (has("splash")) return R_SPLASH;
  if (has("carwater")) return R_CARWATER;
  if (has("exhaust")) return R_EXHAUST;
  if (has("_pop")) return R_POP;
  if (has("firesmoke")) return R_FIRE_SMOKE;
  if (has("fire")) return R_FIRE;
  if (has("steam")) return R_STEAM;
  if (has("spark")) return R_SPARKS;
  if (has("window")) return R_GLASS;
  if (has("snowwall_smoke")) return R_SNOWWALL_SMOKE;
  if (has("snowwall")) return R_SNOWWALL;
  if (has("gravelspray") || has("pebbles")) return R_GRAVEL;
  if (has("smoke")) return R_STAGE_SMOKE;
  return -1;
}

bool read_bytes(const void* p, void* out, size_t n) {
  __try { std::memcpy(out, p, n); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int row_of(void* emitter) {
  const auto it = g_emRow.find(emitter);
  if (it != g_emRow.end()) return it->second;
  char name[64];
  emitter_name(emitter, name, sizeof(name));
  char lower[64];
  size_t i = 0;
  for (; name[i] && i + 1 < sizeof(lower); ++i) lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
  lower[i] = 0;
  const int row = classify(lower);
  g_emRow[emitter] = row;
  plog(L"effects: game emitter %S: %s", name, row < 0 ? L"no row, stays the game's" : kRowSection[row]);
  return row;
}

// ---- census (RBRRTX_EMIT_PROBE=1, launcher -EmitProbe): spawns per emitter and route, one line per 10 s
enum { ROUTE_EMIT, ROUTE_AUTO, ROUTE_GROUND, ROUTE_OTHER, ROUTE_COUNT };
const wchar_t* const kRouteName[ROUTE_COUNT] = { L"signal", L"auto", L"ground", L"other" };
constexpr uintptr_t kRetEmit = 0x005DC183, kRetAuto = 0x005D971B, kRetGround = 0x0049C0B7;
struct CensusStat { char name[48] = {}; unsigned window = 0, routes[ROUTE_COUNT] = {}; float pos[3] = {}; bool seen = false; };
std::unordered_map<void*, CensusStat> g_census;
ULONGLONG g_censusLast = 0;
unsigned g_censusLines = 0;

void census_note(void* emitter, uintptr_t ret) {
  CensusStat& s = g_census[emitter];
  const int route = ret == kRetEmit ? ROUTE_EMIT : ret == kRetAuto ? ROUTE_AUTO : ret == kRetGround ? ROUTE_GROUND : ROUTE_OTHER;
  ++s.window;
  ++s.routes[route];
  read_bytes(static_cast<const uint8_t*>(emitter) + 0x40, s.pos, sizeof(s.pos));
  if (s.seen) return;
  s.seen = true;
  emitter_name(emitter, s.name, sizeof(s.name));
  plog(L"effects census: new emitter %S: route %s (return %08X), at %.1f %.1f %.1f", s.name, kRouteName[route], static_cast<unsigned>(ret),
       s.pos[0], s.pos[1], s.pos[2]);
}

void census_report() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_censusLast < 10000) return;
  const double secs = g_censusLast ? (now - g_censusLast) / 1000.0 : 10.0;
  g_censusLast = now;
  std::vector<CensusStat*> active;
  for (auto& [emitter, s] : g_census) if (s.window) active.push_back(&s);
  if (!active.empty() && g_censusLines < 120) {
    std::sort(active.begin(), active.end(), [](const CensusStat* a, const CensusStat* b) { return a->window > b->window; });
    wchar_t line[1800];
    size_t used = swprintf_s(line, L"effects census %u: %zu emitters spawning:", ++g_censusLines, active.size());
    for (size_t i = 0; i < active.size() && i < 14 && used + 90 < 1800; ++i) {
      int best = 0;
      for (int r = 1; r < ROUTE_COUNT; ++r) if (active[i]->routes[r] > active[i]->routes[best]) best = r;
      used += swprintf_s(line + used, 1800 - used, L" %S %.1f/s %s;", active[i]->name, active[i]->window / secs, kRouteName[best]);
    }
    plog(L"%s", line);
  }
  for (auto& [emitter, s] : g_census) s.window = 0;
}

void note_site(Row& r, const void* emitter) {
  float p[3];
  if (!read_bytes(static_cast<const uint8_t*>(emitter) + 0x40, p, sizeof(p))) return;
  const ULONGLONG now = GetTickCount64();
  Site* use = nullptr;
  Site* oldest = &r.sites[0];
  const float m2 = r.merge * r.merge;
  for (Site& s : r.sites) {
    if (s.t && now - s.t < static_cast<ULONGLONG>(r.holdMs)) {
      const float dx = s.p[0] - p[0], dy = s.p[1] - p[1], dz = s.p[2] - p[2];
      if (dx * dx + dy * dy + dz * dz < m2) { use = &s; break; }
    }
    if (s.t < oldest->t) oldest = &s;
  }
  if (!use) use = oldest;
  std::memcpy(use->p, p, sizeof(p));
  use->t = now;
}

void* __fastcall create_hook(void* emitter) {
  if (g_censusOn) census_note(emitter, reinterpret_cast<uintptr_t>(_ReturnAddress()));
  if (!g_rowsOn || g_raster) return g_createOrig(emitter);
  const int row = row_of(emitter);
  if (row < 0 || !g_rows[row].c.enabled) return g_createOrig(emitter);
  if (row == R_PRECIP) { if (!g_precip || !g_replace) return g_createOrig(emitter); }
  else note_site(g_rows[row], emitter);
  // The callers only write into the returned slot; it must look like a live one (class at +0x20, as CEmitter_InitParticle).
  std::memset(g_dummySlot, 0, 64);
  const uint32_t holder = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(emitter) + 0x150);
  *reinterpret_cast<uint32_t*>(g_dummySlot + 0x20) = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(holder) + 0x68);
  return g_dummySlot;
}

bool install_create_hook() {
  uint8_t* entry = reinterpret_cast<uint8_t*>(kCreate);
  uint8_t now[sizeof(kCreateEntry)] = {};
  if (!read_bytes(entry, now, sizeof(now)) || std::memcmp(now, kCreateEntry, sizeof(now))) {
    plog(L"effects: spawn hook NOT installed: CEmitter_CreateParticle has other bytes (other exe build, or another hook there)");
    return false;
  }
  uint8_t* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!tramp) return false;
  std::memcpy(tramp, kCreateEntry, sizeof(kCreateEntry));              // position independent, safe to run from here
  uint8_t* back = tramp + sizeof(kCreateEntry);
  back[0] = 0xE9;
  const int32_t toRest = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry + sizeof(kCreateEntry)) - reinterpret_cast<intptr_t>(back + 5));
  std::memcpy(back + 1, &toRest, 4);
  g_createOrig = reinterpret_cast<CreateFn>(tramp);
  DWORD old = 0;
  if (!VirtualProtect(entry, sizeof(kCreateEntry), PAGE_EXECUTE_READWRITE, &old)) return false;
  entry[0] = 0xE9;
  const int32_t toHook = static_cast<int32_t>(reinterpret_cast<intptr_t>(&create_hook) - reinterpret_cast<intptr_t>(entry + 5));
  std::memcpy(entry + 1, &toHook, 4);
  std::memset(entry + 5, 0x90, sizeof(kCreateEntry) - 5);
  VirtualProtect(entry, sizeof(kCreateEntry), old, &old);
  FlushInstructionCache(GetCurrentProcess(), entry, sizeof(kCreateEntry));
  return true;
}

// A material and a 0.8 m quad facing up per row, new per settings read (see make_precip on why the hashes change).
bool make_row(int i) {
  Row& r = g_rows[i];
  if (r.fx.mesh && r.fxEpoch != g_precipEpoch) {
    g_api->DestroyMesh(r.fx.mesh);
    g_api->DestroyMaterial(r.fx.mat);
    r.fx = {};
  }
  r.fxEpoch = g_precipEpoch;
  if (r.fx.mesh) return true;
  if (r.fx.failed) return false;
  static wchar_t tex[R_COUNT][MAX_PATH * 2] = {};
  wcscpy_s(tex[i], g_cfgPath);
  wchar_t* slash = wcsrchr(tex[i], L'\\');
  if (slash) slash[1] = 0;
  wcscat_s(tex[i], r.c.tex);
  const bool haveTex = GetFileAttributesW(tex[i]) != INVALID_FILE_ATTRIBUTES;
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.roughnessTexture = L"";
  opaque.metallicTexture = L"";
  opaque.heightTexture = L"";
  opaque.albedoConstant = { 0.8f, 0.8f, 0.8f };
  opaque.opacityConstant = 1.0f;
  opaque.roughnessConstant = 1.0f;
  opaque.useDrawCallAlphaState = 1;
  remixapi_MaterialInfo mi = {};
  mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  mi.pNext = &opaque;
  mi.hash = (0x52425254585F6630ull + i) ^ (static_cast<uint64_t>(g_precipEpoch) << 40);   // "RBRTX_f0"..
  mi.albedoTexture = haveTex ? tex[i] : L"";
  mi.normalTexture = L"";
  mi.tangentTexture = L"";
  mi.emissiveTexture = r.c.selfLight > 0.0f && haveTex ? tex[i] : L"";
  mi.emissiveIntensity = r.c.selfLight;
  mi.emissiveColorConstant = { 1.0f, 1.0f, 1.0f };
  mi.filterMode = 1;
  mi.wrapModeU = mi.wrapModeV = 2;
  if (g_api->CreateMaterial(&mi, &r.fx.mat) != REMIXAPI_ERROR_CODE_SUCCESS) { r.fx.failed = true; return false; }
  static remixapi_HardcodedVertex v[4] = {};
  const float q[4][2] = { { -0.4f, -0.4f }, { 0.4f, -0.4f }, { 0.4f, 0.4f }, { -0.4f, 0.4f } };
  for (int k = 0; k < 4; ++k) {
    v[k] = {};
    v[k].position[0] = q[k][0]; v[k].position[2] = q[k][1];
    v[k].normal[1] = 1.0f;
    v[k].texcoord[0] = q[k][0] > 0 ? 1.0f : 0.0f; v[k].texcoord[1] = q[k][1] > 0 ? 1.0f : 0.0f;
    v[k].color = 0xFFFFFFFF;
  }
  static const uint32_t idx[6] = { 0, 2, 1, 0, 3, 2 };
  remixapi_MeshInfoSurfaceTriangles tri = {};
  tri.vertices_values = v;
  tri.vertices_count = 4;
  tri.indices_values = idx;
  tri.indices_count = 6;
  tri.material = r.fx.mat;
  remixapi_MeshInfo mesh = {};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = (0x52425254585F6730ull + i) ^ (static_cast<uint64_t>(g_precipEpoch) << 40);  // "RBRTX_g0"..
  mesh.surfaces_values = &tri;
  mesh.surfaces_count = 1;
  if (g_api->CreateMesh(&mesh, &r.fx.mesh) != REMIXAPI_ERROR_CODE_SUCCESS) { r.fx.failed = true; return false; }
  plog(L"effects: %s made, texture %s", kRowSection[i], haveTex ? tex[i] : L"none: plain colour");
  return true;
}

void draw_rows() {
  const bool hidden = camera_hidden();
  const ULONGLONG now = GetTickCount64();
  const bool haveView = now - g_viewT < 500;
  float car[20];
  const bool haveCar = rbrrtx_player_car_raw(car);
  for (int i = 0; i < R_COUNT; ++i) {
    Row& r = g_rows[i];
    if (i == R_PRECIP || !r.c.enabled || (r.hideCockpit && hidden)) continue;
    for (const Site& s : r.sites) {
      if (!s.t || now - s.t >= static_cast<ULONGLONG>(r.holdMs)) continue;
      float p[3] = { s.p[0], s.p[1], s.p[2] };
      if (std::fabs(p[0]) + std::fabs(p[1]) + std::fabs(p[2]) < 0.01f) {   // car-space emitter: no world position of its own
        if (!haveCar) continue;
        p[0] = car[16]; p[1] = car[17]; p[2] = car[18];
      }
      const float x = p[0], y = p[2] + r.c.height, z = p[1];           // physics -> render: Y and Z swapped
      if (haveView) {
        const float dx = x - g_viewPos[0], dy = y - g_viewPos[1], dz = z - g_viewPos[2];
        if (dx * dx + dy * dy + dz * dz > r.maxDist * r.maxDist) continue;
      }
      if (!make_row(i)) break;
      if (r.descEpoch != g_precipEpoch) {
        r.store = DescStore();
        r.desc = fx_desc(r.c, nullptr, r.store);
        r.descEpoch = g_precipEpoch;
      }
      remixapi_InstanceInfoBlendEXT blend = {};
      fill_blend(blend, &r.desc);
      remixapi_InstanceInfo inst = {};
      inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      inst.pNext = &blend;
      inst.mesh = r.fx.mesh;
      inst.transform = { { { 1, 0, 0, x }, { 0, 1, 0, y }, { 0, 0, 1, z } } };
      inst.doubleSided = 1;
      g_api->DrawInstance(&inst);
    }
  }
}

}  // namespace

bool rbrrtx_rain_probe_on() { return g_rainProbe; }

void rbrrtx_rain_note_draw(unsigned long long hash, unsigned prims, bool ff) {
  for (DrawStat& d : g_rainDraws) {
    if (d.draws && (d.hash != hash || d.ff != ff)) continue;
    d.hash = hash;
    d.ff = ff;
    ++d.draws;
    d.prims += prims;
    return;
  }
  ++g_rainDrawOther;
}

bool rbrrtx_rain_cutting() { return g_replace && g_nGameTex && precip_kind(); }

int rbrrtx_precip_state(float* intensity) {
  const int k = g_rainHooked ? precip_kind() : 0;
  *intensity = k ? weather_intensity() : 0.0f;
  return k;
}

bool rbrrtx_rain_skip_texture(unsigned long long hash) {
  for (int i = 0; i < g_nGameTex; ++i)
    if (g_gameTex[i] == hash) { ++g_gameSkipped; return true; }
  return false;
}

void rbrrtx_rain_toggle() {
  if (!g_rainHooked) return;
  g_replace = !g_replace;
  wchar_t ev[128];
  swprintf_s(ev, L"%s: rain/snow %s", rbrrtx_key_name(RBRKEY_RAIN),
             g_replace ? (g_precip ? L"Remix (game's cut)" : L"game's cut") : L"game's own");
  plog(L"rain: %s", ev);
  rbrrtx_report_event(ev);
}

// Key 0 (user 2026-11-08): raster shows the game's own rain and snow, ray tracing ours, for comparison. New Remix systems
// when ray tracing returns (the epoch makes draw_precip replace them), as the old ones came back with far too many particles.
void rbrrtx_particles_raster(bool raster) {
  static bool replaceWas = false;
  if (raster) {
    replaceWas = g_replace;
    g_replace = false;
    g_raster = true;
    ++g_precipEpoch;
  } else {
    g_replace = replaceWas;
    g_raster = false;
  }
  plog(L"rain: key 0: %s", g_replace ? L"Remix rain/snow, game's cut" : L"game's own rain/snow");
}

void rbrrtx_particles_init(const remixapi_Interface* api) {
  g_api = api;
  g_probe = rbrrtx_env_flag(L"RBRRTX_EMIT_PROBE", false);
  g_test = rbrrtx_env_flag(L"RBRRTX_PARTICLE_TEST", true);   // on in a release; the env var 0 turns it off (user 2026-10-25: release had no particles)
  rbrrtx_config_path(L"RBRRTX_PARTICLES", L"particles.ini", g_cfgPath, MAX_PATH * 2);
  load_cfg();
  const bool plus = api && rbrrtx_remix_plus();          // the effects need Remix Plus; elsewhere the game keeps its own particles
  if (!plus) g_test = false;
  if ((g_probe || g_test) && !g_hooked) install_probe();   // game dust cut, spawn log
  if (g_test && !g_tireHooked) install_tire_hook();          // wheel points and surfaces
  g_active = g_cutGame = g_test;
  plog(L"particles: Remix dust %s (key %s)%s", g_test ? L"ON" : L"off", rbrrtx_key_name(RBRKEY_DUST),
       api ? L"" : L"; no Remix API");
  g_rainProbe = rbrrtx_env_flag(L"RBRRTX_RAIN_PROBE", false);
  g_precip = plus && rbrrtx_env_flag(L"RBRRTX_PRECIP", true);   // likewise on by default (rain, snow, windscreen drops need it)
  g_replace = g_precip;
  if ((g_rainProbe || g_precip) && !g_rainHooked) install_rain_hook();
  plog(L"rain: Remix rain/snow %s (key %s: Remix <-> game's own)%s", g_precip ? L"ON, game's cut" : L"off",
       rbrrtx_key_name(RBRKEY_RAIN), g_rainProbe ? L"; probe lines every 10 s" : L"");
  g_censusOn = g_probe;
  const bool wantRows = plus && rbrrtx_env_flag(L"RBRRTX_EFFECTS", true);
  static bool createHooked = false;
  if ((wantRows || g_censusOn) && !createHooked) createHooked = install_create_hook();
  g_rowsOn = wantRows && createHooked;
  plog(L"effects: every other game emitter %s%s", g_rowsOn ? L"through Remix" : plus ? L"stays the game's" : L"stays the game's (no Remix Plus)",
       g_censusOn ? L"; census lines every 10 s" : L"");
}

void rbrrtx_particles_camera(const float camToWorld[16]) {
  g_viewPos[0] = camToWorld[12]; g_viewPos[1] = camToWorld[13]; g_viewPos[2] = camToWorld[14];
  g_viewT = GetTickCount64();
}

void rbrrtx_particles_menu_rain(float percent) { g_menuRain = percent; }

void rbrrtx_particles_emit(bool sweeping) {
  if (g_test && !sweeping) { draw_emitter(); if (g_rowsOn) draw_rows(); }
  if (g_precip && g_replace && !sweeping) draw_precip();
}

bool rbrrtx_particles_active() { return g_active; }

void rbrrtx_particles_toggle() {
  if (!g_active) return;
  g_test = !g_test;
  wchar_t ev[128];
  swprintf_s(ev, L"%s: Remix dust %s (game dust stays off)", rbrrtx_key_name(RBRKEY_DUST), g_test ? L"ON" : L"off");
  plog(L"particles: %s", ev);
  rbrrtx_report_event(ev);
}

void rbrrtx_particles_frame_end() {
  if (g_active) { check_cfg(); surface_frame(); }
  if (g_probe) probe_report();
  if (g_censusOn) census_report();
  if (g_rainHooked) rain_cut_frame();
  if (g_rainProbe) rain_report();
  if (g_precip && !g_active) check_cfg();             // particles.ini read again on save (dust does it otherwise)
}

void rbrrtx_particles_destroy() {
  if (!g_api) return;
  if (g_mesh) g_api->DestroyMesh(g_mesh);
  if (g_mat) g_api->DestroyMaterial(g_mat);
  for (PrecipFx& f : g_fx) {
    if (f.mesh) g_api->DestroyMesh(f.mesh);
    if (f.mat) g_api->DestroyMaterial(f.mat);
    f = {};
  }
  for (Row& r : g_rows) {
    if (r.fx.mesh) g_api->DestroyMesh(r.fx.mesh);
    if (r.fx.mat) g_api->DestroyMaterial(r.fx.mat);
    r.fx = {};
  }
  g_mesh = nullptr;
  g_mat = nullptr;
  g_made = false;
}

bool rbrrtx_surface_lookup_point(float out[3]) {
  if (!g_lookup.on) return false;
  std::memcpy(out, g_lookup.pt, sizeof(g_lookup.pt));
  return true;
}

void rbrrtx_surface_lookup_hit(float dh, const float rgb[3], unsigned long long textureHash) {
  if (!g_lookup.on || (g_lookup.hit && dh >= g_lookup.dh)) return;   // the ground nearest the wheel wins
  g_lookup.hit = true;
  g_lookup.dh = dh;
  std::memcpy(g_lookup.rgb, rgb, sizeof(g_lookup.rgb));
  g_lookup.tex = textureHash;
}
