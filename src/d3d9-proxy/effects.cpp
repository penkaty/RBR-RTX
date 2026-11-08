// effects.cpp - every game particle through one place (docs/sessions/particle-rewrite-2026-11-08.md, re/kb/systems/particles.md).
//
// Stage 1, census (RBRRTX_EMIT_PROBE=1, launcher -EmitProbe): changes nothing the game does. Every particle RBR creates goes
// through CEmitter_CreateParticle 0x5D9580 (fastcall, ecx = emitter, name inline at +0, spawn position at +0x40 in physics
// space), reached from CEmitter_Emit (tire signals), CEmitter_Update (auto rate), CParticleSystem_Update (sparks) and a
// ground-trace spawner. The hook counts spawns per emitter and route; CParticleSystem_Update is timed at its two call sites,
// which shows what the game's own particle CPU work costs. One line per 10 s (each log line costs milliseconds).
#include "common.hpp"
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace {

#define plog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

constexpr uintptr_t kCreate = 0x005D9580;
constexpr uint8_t kCreateEntry[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x70, 0x01, 0x00, 0x00 };   // push esi; mov esi,ecx; mov eax,[esi+170h]
constexpr uintptr_t kRetEmit = 0x005DC183, kRetAuto = 0x005D971B, kRetGround = 0x0049C0B7;
constexpr uintptr_t kUpdate = 0x005E2850, kUpdateCalls[] = { 0x0048263A, 0x004826A0 };
constexpr unsigned kMaxLines = 120;
constexpr size_t kTop = 14;

using CreateFn = void*(__fastcall*)(void* emitter);
using UpdateFn = void(__thiscall*)(void* self, float dt);

enum { ROUTE_EMIT, ROUTE_AUTO, ROUTE_GROUND, ROUTE_OTHER, ROUTE_COUNT };
const wchar_t* const kRouteName[ROUTE_COUNT] = { L"signal", L"auto", L"ground", L"other" };

struct Stat {
  char name[48] = {};
  unsigned window = 0, total = 0, routes[ROUTE_COUNT] = {};
  uintptr_t otherRet = 0;
  float pos[3] = {};
  bool firstLogged = false;
};

CreateFn g_orig = nullptr;
bool g_on = false;
std::unordered_map<void*, Stat> g_stats;
long long g_updateTicks = 0, g_updateMax = 0;
unsigned g_updateCalls = 0, g_lines = 0;
ULONGLONG g_lastReport = 0;

bool read_name(const void* p, char* out, size_t n) {
  __try {
    const char* s = static_cast<const char*>(p);
    size_t i = 0;
    for (; i + 1 < n; ++i) {
      const char c = s[i];
      if (!c) break;
      if (c < 0x20 || c > 0x7E) return false;
      out[i] = c;
    }
    out[i] = 0;
    return i >= 2 && i + 1 < n;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool read_bytes(const void* p, void* out, size_t n) {
  __try { std::memcpy(out, p, n); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void note_spawn(void* emitter, uintptr_t ret) {
  Stat& s = g_stats[emitter];
  const uint8_t* e = static_cast<const uint8_t*>(emitter);
  const int route = ret == kRetEmit ? ROUTE_EMIT : ret == kRetAuto ? ROUTE_AUTO : ret == kRetGround ? ROUTE_GROUND : ROUTE_OTHER;
  ++s.window;
  ++s.total;
  ++s.routes[route];
  if (route == ROUTE_OTHER) s.otherRet = ret;
  read_bytes(e + 0x40, s.pos, sizeof(s.pos));
  if (s.firstLogged) return;
  s.firstLogged = true;
  if (!read_name(emitter, s.name, sizeof(s.name))) strcpy_s(s.name, "?");
  uint32_t cap = 0, pool = 0;
  read_bytes(e + 0x16C, &cap, 4);
  read_bytes(e + 0x170, &pool, 4);
  plog(L"effects census: new emitter %S: route %s (return %08X), slots %u, pool %s at first spawn, at %.1f %.1f %.1f", s.name,
       kRouteName[route], static_cast<unsigned>(ret), cap, pool ? L"already allocated" : L"not allocated yet", s.pos[0], s.pos[1], s.pos[2]);
}

void* __fastcall create_hook(void* emitter) {
  note_spawn(emitter, reinterpret_cast<uintptr_t>(_ReturnAddress()));
  return g_orig(emitter);
}

void __fastcall update_hook(void* self, void* /*edx*/, float dt) {
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  reinterpret_cast<UpdateFn>(kUpdate)(self, dt);
  QueryPerformanceCounter(&b);
  const long long t = b.QuadPart - a.QuadPart;
  g_updateTicks += t;
  g_updateMax = (std::max)(g_updateMax, t);
  ++g_updateCalls;
}

bool install_create_hook() {
  uint8_t* entry = reinterpret_cast<uint8_t*>(kCreate);
  uint8_t now[sizeof(kCreateEntry)] = {};
  if (!read_bytes(entry, now, sizeof(now)) || std::memcmp(now, kCreateEntry, sizeof(now))) {
    plog(L"effects census: NOT installed: CEmitter_CreateParticle has other bytes (other exe build, or another hook there)");
    return false;
  }
  uint8_t* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!tramp) return false;
  std::memcpy(tramp, kCreateEntry, sizeof(kCreateEntry));              // position independent, safe to run from here
  uint8_t* back = tramp + sizeof(kCreateEntry);
  back[0] = 0xE9;
  const int32_t toRest = static_cast<int32_t>(reinterpret_cast<intptr_t>(entry + sizeof(kCreateEntry)) - reinterpret_cast<intptr_t>(back + 5));
  std::memcpy(back + 1, &toRest, 4);
  g_orig = reinterpret_cast<CreateFn>(tramp);
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

int install_update_timer() {
  int done = 0;
  for (uintptr_t c : kUpdateCalls) {
    uint8_t* call = reinterpret_cast<uint8_t*>(c);
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    if (call[0] != 0xE8 || c + 5 + rel != kUpdate) continue;
    DWORD old = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) continue;
    const int32_t to = static_cast<int32_t>(reinterpret_cast<intptr_t>(&update_hook) - static_cast<intptr_t>(c + 5));
    std::memcpy(call + 1, &to, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    ++done;
  }
  return done;
}

}  // namespace

void rbrrtx_effects_census_init() {
  if (g_on || !rbrrtx_env_flag(L"RBRRTX_EMIT_PROBE", false)) return;
  g_on = install_create_hook();
  const int timers = g_on ? install_update_timer() : 0;
  plog(L"effects census: %s; particle update timed at %d of 2 call sites", g_on ? L"on (changes nothing)" : L"off", timers);
}

void rbrrtx_effects_census_report() {
  if (!g_on) return;
  const ULONGLONG now = GetTickCount64();
  if (now - g_lastReport < 10000) return;
  const double secs = g_lastReport ? (now - g_lastReport) / 1000.0 : 10.0;
  g_lastReport = now;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  const double avgMs = g_updateCalls ? g_updateTicks * 1000.0 / f.QuadPart / g_updateCalls : 0.0;
  const double maxMs = g_updateMax * 1000.0 / f.QuadPart;
  const unsigned calls = g_updateCalls;
  g_updateTicks = g_updateMax = 0;
  g_updateCalls = 0;
  std::vector<Stat*> active;
  for (auto& [emitter, s] : g_stats) if (s.window) active.push_back(&s);
  if (g_lines < kMaxLines && (calls || !active.empty())) {
    std::sort(active.begin(), active.end(), [](const Stat* a, const Stat* b) { return a->window > b->window; });
    float car[20];
    const bool haveCar = rbrrtx_player_car_raw(car);
    wchar_t line[1800];
    size_t used = 0;
    used += swprintf_s(line + used, 1800 - used, L"effects census %u: game particle update %.3f ms avg, %.3f max, %u calls; %zu emitters spawning:",
                       ++g_lines, avgMs, maxMs, calls, active.size());
    for (size_t i = 0; i < active.size() && i < kTop && used + 110 < 1800; ++i) {
      const Stat& s = *active[i];
      float d = -1.0f;
      if (haveCar) {
        const float dx = s.pos[0] - car[16], dy = s.pos[1] - car[17], dz = s.pos[2] - car[18];
        d = std::sqrt(dx * dx + dy * dy + dz * dz);
      }
      int best = 0;
      for (int r = 1; r < ROUTE_COUNT; ++r) if (s.routes[r] > s.routes[best]) best = r;
      used += swprintf_s(line + used, 1800 - used, L" %S %.1f/s %s %.0fm;", s.name, s.window / secs, kRouteName[best], d);
    }
    plog(L"%s", line);
  }
  for (auto& [emitter, s] : g_stats) s.window = 0;
}
