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
//    hook passes amount 0 to CEmitter_Emit for emitters named in hide_game_emitters (particles.ini) and not in
//    keep_game_emitters, so those particles are never created (no simulation, no draws). With amount 0 the original
//    still runs its bookkeeping (adds 0, stays below one particle, updates the emitter position) and spawns nothing.
//    Other particles (rain, splash, exhaust pop = backfire light, fire) are not CEmitter_Emit calls from the tire
//    handlers and stay as they are.
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

namespace {

#define plog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

constexpr uintptr_t kEmit = 0x005DC100, kEmitCall = 0x005DDEB8;
using EmitFn = void(__thiscall*)(void* self, void* arg1, const float* pos, const float* vel, float amount);

// ---------------------------------------------------------------- probe

struct Stat { void* arg1 = nullptr; unsigned calls = 0; double amount = 0; float pos[3] = {}, vel[3] = {}; };
bool g_probe = false, g_hooked = false;
struct Contact { float p[3]; ULONGLONG t; };
std::unordered_map<void*, Contact> g_contacts;         // per wheel+emitter object: last contact point (physics space)
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

bool hide_game_emitter(void* arg1);                   // settings section below

void __fastcall emit_hook(void* self, void* /*edx*/, void* arg1, const float* pos, const float* vel, float amount) {
  if (amount != 0.0f && hide_game_emitter(arg1)) amount = 0.0f;   // never created: no simulation, no draws
  if (pos) {
    Contact& c = g_contacts[self];
    std::memcpy(c.p, pos, sizeof(c.p));
    c.t = GetTickCount64();
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
  plog(L"particles: CEmitter_Emit hook at %p (wheel contact points%s)", call, g_probe ? L"; probe: a line every 10 s, at most 30" : L"");
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
  wchar_t hideGame[256] = L"tirespray, tiresmoke, dust, sand";   // game emitters never spawned (name contains one)
  wchar_t keepGame[256] = L"wet, water, splash";                 // ...unless the name contains one of these
} g_cfg;

// Per game emitter (arg1, starts with its name): hidden or kept. Cleared when the settings change.
std::unordered_map<void*, bool> g_gameHide;
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

bool hide_game_emitter(void* arg1) {
  if (!g_cutGame) return false;
  const auto it = g_gameHide.find(arg1);
  if (it != g_gameHide.end()) return it->second;
  char name[64];
  emitter_name(arg1, name, sizeof(name));
  const bool hide = name_has_any(name, g_cfg.hideGame) && !name_has_any(name, g_cfg.keepGame);
  g_gameHide[arg1] = hide;
  plog(L"particles: game emitter %S: %s", name, hide ? L"never spawned (Remix dust instead)" : L"kept");
  return hide;
}

wchar_t g_cfgPath[MAX_PATH * 2] = {};
FILETIME g_cfgTime = {};
ULONGLONG g_cfgChecked = 0;

float cfg_float(const wchar_t* key, float dflt, float lo, float hi) {
  wchar_t v[64] = {};
  GetPrivateProfileStringW(L"dust", key, L"", v, 64, g_cfgPath);
  if (!v[0]) return dflt;
  const float f = static_cast<float>(_wtof(v));
  return f < lo ? lo : f > hi ? hi : f;
}

void cfg_rgba(const wchar_t* key, float out[4]) {
  wchar_t v[128] = {};
  GetPrivateProfileStringW(L"dust", key, L"", v, 128, g_cfgPath);
  float c[4];
  if (v[0] && swscanf_s(v, L"%f , %f , %f , %f", &c[0], &c[1], &c[2], &c[3]) == 4) std::memcpy(out, c, sizeof(c));
}

void load_cfg() {
  DustCfg c;
  const bool found = GetFileAttributesW(g_cfgPath) != INVALID_FILE_ATTRIBUTES;
  if (found) {
    c.wheels = cfg_float(L"wheels", 4.0f, 2.0f, 4.0f) < 3.0f ? 2 : 4;
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
    wchar_t v[128] = {};
    wchar_t list[256];
    GetPrivateProfileStringW(L"dust", L"hide_game_emitters", c.hideGame, list, 256, g_cfgPath);
    wcscpy_s(c.hideGame, list);
    GetPrivateProfileStringW(L"dust", L"keep_game_emitters", c.keepGame, list, 256, g_cfgPath);
    wcscpy_s(c.keepGame, list);
    GetPrivateProfileStringW(L"dust", L"hide_in_cameras", L"5, 6", v, 128, g_cfgPath);
    c.nHideCams = 0;
    wchar_t* ctx = nullptr;
    for (wchar_t* t = wcstok_s(v, L", ", &ctx); t && c.nHideCams < 8; t = wcstok_s(nullptr, L", ", &ctx))
      c.hideCams[c.nHideCams++] = _wtoi(t);
  }
  g_cfg = c;
  g_gameHide.clear();
  plog(L"particles: game dust never spawned for emitters containing \"%s\" (except \"%s\")", c.hideGame, c.keepGame);
  plog(L"particles: dust settings %s%s: %d wheels x %.0f/s, cap %u, life %.1f-%.1f s, size %.2f-%.2f -> %.1f-%.1f m, "
       L"rise %.1f m/s, spread %.0f, drag %.2f, gravity %.2f, turbulence %.2f/%.2f, on above %.0f km/h, off below %.0f, "
       L"%d hidden cameras", found ? L"from " : L"built in, no file ", g_cfgPath, c.wheels, c.perWheelRate, c.maxParticles,
       c.lifeMin, c.lifeMax, c.startSizeMin, c.startSizeMax, c.endSizeMin, c.endSizeMax, c.riseSpeed, c.cone, c.drag,
       c.gravity, c.turbulenceForce, c.turbulenceFrequency, c.onKmh, c.offKmh, c.nHideCams);
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

bool camera_hidden() {
  static int was = -2;
  const int cam = camera_type();
  bool hide = false;
  for (int i = 0; i < g_cfg.nHideCams; ++i) hide |= cam == g_cfg.hideCams[i];
  if (cam != was) { was = cam; plog(L"particles: camera type %d: dust %s", cam, hide ? L"hidden" : L"shown"); }
  return hide;
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
remixapi_Float4D g_minColor[2] = {}, g_maxColor[2] = {};
remixapi_Float2D g_minSize[2] = {}, g_maxSize[2] = {};
float g_minRot[1] = {}, g_maxRot[1] = {};
remixapi_Float3D g_maxVel[1] = { { -1.0f, -1.0f, -1.0f } };   // negative = unlimited

remixapi_InstanceInfoParticleSystemEXT dust_desc() {
  const DustCfg& c = g_cfg;
  g_minColor[0] = { c.startColorA[0], c.startColorA[1], c.startColorA[2], c.startColorA[3] };
  g_maxColor[0] = { c.startColorB[0], c.startColorB[1], c.startColorB[2], c.startColorB[3] };
  g_minColor[1] = { c.endColorA[0], c.endColorA[1], c.endColorA[2], c.endColorA[3] };
  g_maxColor[1] = { c.endColorB[0], c.endColorB[1], c.endColorB[2], c.endColorB[3] };
  g_minSize[0] = { c.startSizeMin, c.startSizeMin };
  g_maxSize[0] = { c.startSizeMax, c.startSizeMax };
  g_minSize[1] = { c.endSizeMin, c.endSizeMin };
  g_maxSize[1] = { c.endSizeMax, c.endSizeMax };
  g_minRot[0] = -c.rotation;
  g_maxRot[0] = c.rotation;
  remixapi_InstanceInfoParticleSystemEXT p = {};
  p.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT;
  p.maxNumParticles = c.maxParticles;
  p.useTurbulence = c.turbulenceForce > 0.0f;
  p.hideEmitter = 1;
  p.minColor = { g_minColor, 2 };
  p.maxColor = { g_maxColor, 2 };
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
  p.motionTrailMultiplier = 1.0f;
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

// The wheels' ground points (physics space): RBR's recent tire contact points merged per wheel (all four, or the two
// furthest back along the travel direction); else a guess around the car position.
bool g_rearFromGame = false;
int wheel_points(const float* car, float out[4][3]) {
  const ULONGLONG now = GetTickCount64();
  float pts[8][3];
  int n = 0;
  for (auto it = g_contacts.begin(); it != g_contacts.end();) {
    if (now - it->second.t > 250) { it = g_contacts.erase(it); continue; }
    const float* p = it->second.p;
    int k = 0;
    for (; k < n; ++k) {
      const float dx = pts[k][0] - p[0], dy = pts[k][1] - p[1], dz = pts[k][2] - p[2];
      if (dx * dx + dy * dy + dz * dz < 0.16f) break;
    }
    if (k == n && n < 8) { std::memcpy(pts[n], p, sizeof(pts[n])); ++n; }
    ++it;
  }
  auto back = [&](const float* p) {                    // distance ahead of the car along the travel direction
    return (p[0] - car[0]) * g_dir[0] + (p[1] - car[1]) * g_dir[1] + (p[2] - car[2]) * g_dir[2];
  };
  const bool fromGame = n >= 4;
  const int count = g_cfg.wheels;
  if (fromGame && count == 4) {
    for (int i = 0; i < 4; ++i) std::memcpy(out[i], pts[i], sizeof(out[i]));   // n can be 5+ for a moment: 4 is enough
  } else if (fromGame) {
    int a = 0, b = 1;
    if (back(pts[b]) < back(pts[a])) std::swap(a, b);
    for (int i = 2; i < n; ++i) {
      if (back(pts[i]) < back(pts[a])) { b = a; a = i; }
      else if (back(pts[i]) < back(pts[b])) b = i;
    }
    std::memcpy(out[0], pts[a], sizeof(out[0]));
    std::memcpy(out[1], pts[b], sizeof(out[1]));
  } else {
    const float side[3] = { g_dir[1], -g_dir[0], 0.0f };   // physics space is Z up
    const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1]) + 1e-6f;
    for (int w = 0; w < count; ++w)
      for (int i = 0; i < 3; ++i)
        out[w][i] = car[i] + g_dir[i] * (w < 2 ? -1.3f : 1.3f) + side[i] / sl * (w % 2 ? -0.75f : 0.75f) -
                    (i == 2 ? 0.5f : 0.0f);
  }
  if (fromGame != g_rearFromGame || !g_drawn)
    plog(L"particles: wheel dust from %s", fromGame ? L"RBR's tire contact points" : L"a guess around the car (no tire contacts)");
  g_rearFromGame = fromGame;
  return count;
}

void draw_emitter() {
  float r[20];
  if (!rbrrtx_player_car_raw(r)) return;
  update_speed(r + 16);
  if (!g_moving || camera_hidden() || !make_emitter()) return;
  float wheel[4][3];
  const int n = wheel_points(r + 16, wheel);
  for (int w = 0; w < n; ++w) {
    // Physics space = render space with Y and Z swapped (game_state.cpp). Emitter `height` above the ground point.
    const float x = wheel[w][0], y = wheel[w][2] + g_cfg.height, z = wheel[w][1];
    remixapi_InstanceInfoParticleSystemEXT particles = dust_desc();
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

}  // namespace

void rbrrtx_particles_init(const remixapi_Interface* api) {
  g_api = api;
  g_probe = rbrrtx_env_flag(L"RBRRTX_EMIT_PROBE", false);
  g_test = rbrrtx_env_flag(L"RBRRTX_PARTICLE_TEST", false);
  rbrrtx_config_path(L"RBRRTX_PARTICLES", L"particles.ini", g_cfgPath, MAX_PATH * 2);
  load_cfg();
  if (!api) g_test = false;
  if ((g_probe || g_test) && !g_hooked) install_probe();   // wheel contact points, game dust cut
  g_active = g_cutGame = g_test;
  plog(L"particles: Remix dust %s (key %s)%s", g_test ? L"ON" : L"off", rbrrtx_key_name(RBRKEY_DUST),
       api ? L"" : L"; no Remix API");
}

void rbrrtx_particles_emit(bool sweeping) {
  if (g_test && !sweeping) draw_emitter();
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
  if (g_active) check_cfg();
  if (g_probe) probe_report();
}

void rbrrtx_particles_destroy() {
  if (!g_api) return;
  if (g_mesh) g_api->DestroyMesh(g_mesh);
  if (g_mat) g_api->DestroyMaterial(g_mat);
  g_mesh = nullptr;
  g_mat = nullptr;
  g_made = false;
}
