// particles.cpp - Remix particles instead of RBR's dust sprites: first test build (docs/research/particles-remix.md,
// re/kb/systems/particles.md; user 2026-10-23). Nothing here changes what the game draws.
//
// 1. Emit probe (RBRRTX_EMIT_PROBE=1): RBR's tire/surface particles (dust, gravel, spray, grass) all start in
//    CEmitter_Emit 0x5DC100, __thiscall (self, arg1, float3* pos, float3* vel, float amount), ret 0x10, called only
//    from CSignalHandler_Run 0x5DDEB8. That call is pointed at emit_hook, which counts and passes it on. Every 2 s
//    with activity the log got one line per emitter plus a memory dump of each new emitter. Run 2026-10-23 12:08
//    (Sherwood Forest II): pos/vel are physics space (= car position), amount is 0 while the car stands, arg1 starts
//    with the emitter's name (br2_tirespray_a_emitter, br2_tiresmoke_a_emitter), self = one per wheel and emitter.
//    That run also showed the cost of logging: each comp log line opens and closes the file (~7 ms), so nine lines
//    every 2 s gave a 180-300 ms hitch every 2 s; and reading float fields as pointers threw 299 caught faults (each
//    one stack-walked by the exception tracer). Now: one line per 10 s, names read from arg1 only, at most 30 lines.
// 2. API emitter test (RBRRTX_PARTICLE_TEST=1; since run 4 the dust key 7 toggles it): one Remix particle system drawn
//    through the Remix API at RBR's car (CreateMaterial + CreateMesh once, DrawInstance with
//    remixapi_InstanceInfoParticleSystemEXT every frame, the nfsc-rtx rain pattern). If the bridge forwards the
//    particle extension, a brown dust plume trails the car; if not, nothing shows and the bridge log says
//    "[RemixApi_DrawInstance] Unknown sType. Skipping." (then the USD route, research doc option B).
//    Result 2026-10-23 12:08: the plume shows (the 1.5.2 bridge forwards the particle extension). User: bigger
//    plumes, fewer particles, cap 250000, and no dust while the car stands. So the emitter is drawn only above
//    25 km/h (off again below 20), speed from RBR's car position between frames. The same run crashed the bridge
//    server inside Remix one second after Remix's scene capture began (Alt+K sweep step) with the emitter on, so
//    the emitter is not drawn during a sweep (its particles die within 4 s, before the capture step).
//    User run 2 (2026-10-23): cap 150000, 30 per second, start size kept, grow to 5-7 m, live 3-6 s, and the dust
//    from under both rear wheels. Rear wheels: every tire emitter call (CEmitter_Emit, also with amount 0) carries
//    its wheel's ground contact point; the points of the last 0.25 s are merged per wheel (within 0.4 m) and the two
//    furthest back along the car's travel are the rear wheels. Without them (no tire emitters on this surface):
//    1.3 m behind the car, 0.75 m to each side. One instance per wheel, 15/s each = 30/s.
//    User run 3 (2026-10-23): front wheels too, no dust in the interior cameras, 25 per wheel, grow to 7-9 m, live
//    4-8 s, and every value in a text file: rbr-rtx\config\particles.ini (env RBRRTX_PARTICLES). The file is read
//    again within a second of being saved; a changed descriptor makes Remix start a new particle system (the old
//    puffs finish their life). Wheels: the four merged contact points (wheels = 2: the two furthest back). Camera:
//    RBR's camera type ([[[0x165FC68]+0x758]+0x10]+0, NGPCarMenu RBRAPI.h; 5 internal, 6 backseat) listed in
//    hide_in_cameras = no dust drawn.
//    User run 4 (2026-10-23): key 7 = Remix dust on/off; the game's own dust always off, and not just hidden: the
//    hook passes amount 0 to CEmitter_Emit for emitters named in dust_emitters (particles.ini; run 7) and not in
//    keep_game_emitters, so those particles are never created (no simulation, no draws). With amount 0 the original
//    still runs its bookkeeping (adds 0, stays below one particle, updates the emitter position) and spawns nothing.
//    Other particles (rain, splash, exhaust pop = backfire light, fire) are not CEmitter_Emit calls from the tire
//    handlers and stay as they are.
//    User run 5 (2026-10-23): dust coloured like the surface under each wheel, only on a surface change, never as a
//    task every frame. Wheels now come from RBR's own tire signal: CParticleSystem_OnTireParticles is reached through
//    CConnection<CParticleSystem, CSigTireParticles>::Invoke (vtable 0x74DC54 slot 1 = thunk 0x5E3C70); that slot
//    points at tire_invoke, which notes wheel (sig+0), contact point (midpoint of sig+0x10 / +0x20, physics space)
//    and surface material (sig+0xA0 A, +0xA4 B, +0xA8 blend: A while blend < 0.5, getter 0x4F3EC0), then calls the
//    thunk. Cost: a compare per tire signal. A material that stays 0.25 s is the wheel's new surface; a material
//    not seen yet on this stage starts one ground lookup (comp.cpp surface_lookup, at most 40 frames): opaque
//    scenery draws whose cached bounding sphere covers the wheel point are read once, the triangle under the point
//    gives the draw's texture, and harvest.cpp's average colour of that texture (taken when the game uploaded it)
//    is the surface colour. Colours are kept per material until the stage changes (a miss is retried later). Each wheel's puffs mix their
//    colours with it (surface_mix, surface_brightness); a different colour is a different Remix particle system.
//    User run 6 (2026-10-23): dust shown exactly when the headlights see the car body drawn (outside views) and hidden
//    when it is not (cockpit views): rbrrtx_car_body_visible (headlights.cpp). hide_in_cameras is only the fallback
//    while that is not known yet (car model offset not measured).
//    User run 7 (2026-10-23, Harju 2019 and Rallysprint Hondarribia): dust on tarmac. The Remix dust only knew speed.
//    Now it follows the game's own spawns per wheel: a dust emitter (dust_emitters: tirespray, dust, sand) asking for
//    particles (amount > 0) marks the nearest wheel dusty for dust_hold_ms and is cut; a smoke emitter
//    (smoke_emitters: tiresmoke) is cut only on a dusty wheel (on gravel its "smoke" is dust; on tarmac the game's
//    tyre smoke stays). Only dusty wheels throw Remix dust; guessed wheel positions never do. Tarmac stages (mb_*
//    emitters: tiresmoke, pebbles only) get none.
//    User run 8 (2026-10-23): game particles OFF in the launcher from now on (ParticleQuality = off; a game particle
//    called once on a surface change gave glitches and fps drops), and tarmac tyre smoke through the API too: one of
//    the first steps of moving everything to the Remix API. With particles off the game still runs its signal
//    handlers and CEmitter_Emit (run 14:12, Harju: mb_tiresmoke asked for particles), it only makes nothing. So: a
//    smoke emitter asking at a wheel that throws no dust starts that wheel's slide timer; after slide_seconds (1.5 s)
//    of requests without a gap longer than dust_hold_ms the wheel smokes with the [smoke] values (puffs half the dust's
//    size). Dust wins on a wheel asking for both. Both are cut from the game (amount 0).
//    Units: rtx.sceneScale stays 1, so Remix's centimetre values are RBR metres (sizes, speeds, gravity).
//    The descriptor never changes while drawn: Remix keys a particle system on material hash ^ descriptor hash.
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <d3d9.h>
#include "third_party/remix_c.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include <string>

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
  int spawner = 0;                                     // 0 squares (above), 1 car shell: roof + sides + rear (snow wake)
  float wakeOnKmh = 25.0f;                             // [snow_wake]: drawn from this car speed, rate in speed steps
  wchar_t tex[64] = L"dust_puff.dds";
} g_cfg, g_smoke;

// Rain/snow presets (user 2026-10-23 run 2): light rain = many small drops at the run-2 speed; heavy rain = big drops,
// faster, shorter streaks; light snow = many fluffy flakes falling slowly; heavy snow = big flakes, faster and dense.
enum { PRE_RAIN_LIGHT, PRE_RAIN_HEAVY, PRE_SNOW_LIGHT, PRE_SNOW_HEAVY, PRE_SNOW_WAKE, PRE_COUNT };
const wchar_t* const kPreSection[PRE_COUNT] = { L"rain_light", L"rain_heavy", L"snow_light", L"snow_heavy", L"snow_wake" };
DustCfg g_pre[PRE_COUNT];
float g_heavyFrom = 75.0f;                             // weather intensity (percent) from which the heavy preset is used

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
      c.collision = true; c.collisionMode = 2; c.holdColour = true;
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
       kind == 2 ? L"smoke: never spawned, drives the Remix tyre smoke (no dust at the wheel, slide long enough)" : L"kept");
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
  g_cfg = c;
  DustCfg sm = smoke_defaults();
  if (found) { g_sec = L"smoke"; load_fx(sm); g_sec = L"dust"; }
  g_smoke = sm;
  for (int i = 0; i < PRE_COUNT; ++i) {
    g_pre[i] = precip_defaults(i);
    if (found) { g_sec = kPreSection[i]; load_fx(g_pre[i]); g_sec = L"dust"; }
  }
  g_sec = L"precipitation";
  g_heavyFrom = cfg_float(L"heavy_from", 75.0f, 0.0f, 101.0f);
  g_sec = L"dust";
  g_precipEpoch++;                                     // new values: new Remix particle systems
  wchar_t gt[512] = {};                                // a missing file gives the default
  GetPrivateProfileStringW(L"precipitation", L"game_textures", L"40A8944300F54468, 869E5A5AF4FA4D7B, 254D6B130606B00C",
                           gt, 512, g_cfgPath);
  load_game_textures(gt);
  g_gameKind.clear();
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

// Over life: start -> end (two keyframes); colours RGBA. Filled from particles.ini (load_cfg).
remixapi_Float4D g_minColor[3] = {}, g_maxColor[3] = {};
remixapi_Float2D g_minSize[2] = {}, g_maxSize[2] = {};
float g_minRot[1] = {}, g_maxRot[1] = {};
remixapi_Float3D g_maxVel[1] = { { -1.0f, -1.0f, -1.0f } };   // negative = unlimited

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

remixapi_InstanceInfoParticleSystemEXT fx_desc(const DustCfg& c, const float* tint) {
  const float meanLum = (c.startColorA[0] + c.startColorA[1] + c.startColorA[2] + c.startColorB[0] + c.startColorB[1] +
                         c.startColorB[2] + c.endColorA[0] + c.endColorA[1] + c.endColorA[2] + c.endColorB[0] +
                         c.endColorB[1] + c.endColorB[2]) / 12.0f;
  g_minColor[0] = tinted(c, c.startColorA, tint, meanLum);
  g_maxColor[0] = tinted(c, c.startColorB, tint, meanLum);
  g_minColor[1] = tinted(c, c.endColorA, tint, meanLum);
  g_maxColor[1] = tinted(c, c.endColorB, tint, meanLum);
  g_minSize[0] = { c.startSizeMin * c.widthRatio, c.startSizeMin };
  g_maxSize[0] = { c.startSizeMax * c.widthRatio, c.startSizeMax };
  g_minSize[1] = { c.endSizeMin * c.widthRatio, c.endSizeMin };
  g_maxSize[1] = { c.endSizeMax * c.widthRatio, c.endSizeMax };
  g_minRot[0] = -c.rotation;
  g_maxRot[0] = c.rotation;
  remixapi_InstanceInfoParticleSystemEXT p = {};
  p.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT;
  p.maxNumParticles = c.maxParticles;
  p.useTurbulence = c.turbulenceForce > 0.0f;
  p.hideEmitter = 1;
  p.minColor = { g_minColor, 2 };
  p.maxColor = { g_maxColor, 2 };
  if (c.holdColour) {                                  // start, start, end: the change happens in the second half
    g_minColor[2] = g_minColor[1]; g_maxColor[2] = g_maxColor[1];
    g_minColor[1] = g_minColor[0]; g_maxColor[1] = g_maxColor[0];
    p.minColor = { g_minColor, 3 };
    p.maxColor = { g_maxColor, 3 };
  }
  p.minRotationSpeed = { g_minRot, 1 };
  p.maxRotationSpeed = { g_maxRot, 1 };
  p.minSize = { g_minSize, 2 };
  p.maxSize = { g_maxSize, 2 };
  p.maxVelocity = { g_maxVel, 1 };
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
  p.collisionMode = static_cast<uint8_t>(c.collisionMode);
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

void draw_emitter() {
  float r[20];
  if (!rbrrtx_player_car_raw(r)) return;
  update_speed(r + 16);
  if (camera_hidden() || !make_emitter()) return;
  const ULONGLONG now = GetTickCount64();
  float wheel[4][3];
  int wheelOf[4];
  const int n = wheel_points(r + 16, wheel, wheelOf);
  for (int w = 0; w < n; ++w) {
    // Physics space = render space with Y and Z swapped (game_state.cpp). Emitter `height` above the ground point.
    if (wheelOf[w] < 0) continue;                      // guessed position: the game said nothing about this wheel
    const Wheel& wh = g_wheels[wheelOf[w]];
    const DustCfg* fx = nullptr;
    if (g_moving && now - wh.dustT < g_dustHoldMs) fx = &g_cfg;          // the game asks for dust here
    else if (g_smoke.enabled && g_kmh >= g_smoke.onKmh && now - wh.smokeLast < g_dustHoldMs &&
             wh.smokeLast - wh.smokeStart >= static_cast<ULONGLONG>(g_smoke.slideSeconds * 1000.0f))
      fx = &g_smoke;                                   // tarmac: a slide long enough
    if (!fx) continue;
    const float x = wheel[w][0], y = wheel[w][2] + fx->height, z = wheel[w][1];
    const float* tint = nullptr;
    if (fx->surfaceColour) {
      const auto it = g_surface.find(g_wheels[wheelOf[w]].mat);
      if (it != g_surface.end() && it->second.found) tint = it->second.rgb;
    }
    remixapi_InstanceInfoParticleSystemEXT particles = fx_desc(*fx, tint);
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.pNext = &particles;
    blend.alphaTestCompareOp = 7;                        // VK_COMPARE_OP_ALWAYS
    blend.alphaBlendEnabled = 1;
    blend.srcColorBlendFactor = 6;                       // VK_BLEND_FACTOR_SRC_ALPHA
    blend.dstColorBlendFactor = 7;                       // VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
    blend.colorBlendOp = 0;                              // VK_BLEND_OP_ADD
    blend.textureColorArg1Source = 1;                    // texture
    blend.textureColorArg2Source = 2;                    // vertex colour (the particle's colour)
    blend.textureColorOperation = 3;                     // modulate
    blend.textureAlphaArg1Source = 1;
    blend.textureAlphaArg2Source = 2;
    blend.textureAlphaOperation = 3;
    blend.srcAlphaBlendFactor = 1;                       // VK_BLEND_FACTOR_ONE
    blend.dstAlphaBlendFactor = 0;                       // VK_BLEND_FACTOR_ZERO
    blend.writeMask = 0xF;
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
  mi.emissiveTexture = L"";
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

void draw_precip() {
  const int kind = precip_kind();
  const float inten = kind ? weather_intensity() : 0.0f;
  const int pre = !kind || inten < 1.0f ? -1 : (kind == 2 ? PRE_SNOW_LIGHT : PRE_RAIN_LIGHT) + (inten >= g_heavyFrom ? 1 : 0);
  if (kind != g_shownKind || pre != g_shownStep) {
    plog(L"rain: Remix %s, intensity %.0f %%", pre < 0 ? L"none" : kPreSection[pre], inten);
    g_shownKind = kind;
    g_shownStep = pre;
  }
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
  remixapi_InstanceInfoParticleSystemEXT particles = fx_desc(c, nullptr);
  remixapi_InstanceInfoBlendEXT blend = {};
  blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  blend.pNext = &particles;
  blend.alphaTestCompareOp = 7;
  blend.alphaBlendEnabled = 1;
  blend.srcColorBlendFactor = 6;
  blend.dstColorBlendFactor = 7;
  blend.colorBlendOp = 0;
  blend.textureColorArg1Source = 1;
  blend.textureColorArg2Source = 2;
  blend.textureColorOperation = 3;
  blend.textureAlphaArg1Source = 1;
  blend.textureAlphaArg2Source = 2;
  blend.textureAlphaOperation = 3;
  blend.srcAlphaBlendFactor = 1;
  blend.dstAlphaBlendFactor = 0;
  blend.writeMask = 0xF;
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

void rbrrtx_particles_init(const remixapi_Interface* api) {
  g_api = api;
  g_probe = rbrrtx_env_flag(L"RBRRTX_EMIT_PROBE", false);
  g_test = rbrrtx_env_flag(L"RBRRTX_PARTICLE_TEST", true);   // on in a release; the env var 0 turns it off (user 2026-10-25: release had no particles)
  rbrrtx_config_path(L"RBRRTX_PARTICLES", L"particles.ini", g_cfgPath, MAX_PATH * 2);
  load_cfg();
  if (!api) g_test = false;
  if ((g_probe || g_test) && !g_hooked) install_probe();   // game dust cut, spawn log
  if (g_test && !g_tireHooked) install_tire_hook();          // wheel points and surfaces
  g_active = g_cutGame = g_test;
  plog(L"particles: Remix dust %s (key %s)%s", g_test ? L"ON" : L"off", rbrrtx_key_name(RBRKEY_DUST),
       api ? L"" : L"; no Remix API");
  g_rainProbe = rbrrtx_env_flag(L"RBRRTX_RAIN_PROBE", false);
  g_precip = api && rbrrtx_env_flag(L"RBRRTX_PRECIP", true);   // likewise on by default (rain, snow, windscreen drops need it)
  g_replace = g_precip;
  if ((g_rainProbe || g_precip) && !g_rainHooked) install_rain_hook();
  plog(L"rain: Remix rain/snow %s (key %s: Remix <-> game's own)%s", g_precip ? L"ON, game's cut" : L"off",
       rbrrtx_key_name(RBRKEY_RAIN), g_rainProbe ? L"; probe lines every 10 s" : L"");
}

void rbrrtx_particles_emit(bool sweeping) {
  if (g_test && !sweeping) draw_emitter();
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
