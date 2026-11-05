// prof.cpp - see prof.hpp. Single-threaded by design (the game's render thread); other threads are not timed.
#include "prof.hpp"
#include "common.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const wchar_t* const kNames[PZ_COUNT] = {
  L"draw", L"draw.backend", L"draw.extra", L"draw.cull", L"draw.worldlight", L"draw.apiworld",
  L"vsconst", L"psconst", L"rstate", L"tss", L"shader", L"xform", L"rt", L"scene", L"stateblock", L"buflock",
  L"end_frame", L"ef.litparts", L"ef.carpaint", L"ef.foliage", L"ef.genmod", L"ef.atmo", L"ef.exp", L"ef.keys", L"ef.lamps",
  L"ef.worldlights", L"ef.custom", L"ef.ground", L"ef.windscreen", L"ef.particles", L"ef.wetness", L"f3", L"present",
};
long long g_ticks[PZ_COUNT] = {};
unsigned g_calls[PZ_COUNT] = {};
long long g_freq = 0, g_lastWall = 0;
unsigned long long g_lastCpu = 0;
unsigned g_frames = 0;
int g_on = -1;

unsigned long long thread_cpu_100ns() {
  FILETIME a, b, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u)) return 0;
  return (static_cast<unsigned long long>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
         (static_cast<unsigned long long>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}
}  // namespace

long long rbrrtx_prof_now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

void rbrrtx_prof_add(RbrProfZone z, long long ticks) {
  g_ticks[z] += ticks;
  ++g_calls[z];
}

void rbrrtx_prof_frame() {
  if (g_on < 0) {
    wchar_t v[8] = {};
    g_on = !(GetEnvironmentVariableW(L"RBRRTX_PROF", v, 8) && v[0] == L'0');
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    g_lastWall = rbrrtx_prof_now();
    g_lastCpu = thread_cpu_100ns();
  }
  if (!g_on || ++g_frames < 300) return;
  const long long wall = rbrrtx_prof_now();
  const unsigned long long cpu = thread_cpu_100ns();
  const double k = 1000.0 / static_cast<double>(g_freq) / g_frames;
  const double wallMs = (wall - g_lastWall) * k, cpuMs = (cpu - g_lastCpu) / 10000.0 / g_frames;
  const double hooked = (g_ticks[PZ_DRAW_HOOK] + g_ticks[PZ_VSCONST] + g_ticks[PZ_PSCONST] + g_ticks[PZ_RSTATE] + g_ticks[PZ_TSS] +
                         g_ticks[PZ_SHADER] + g_ticks[PZ_XFORM] + g_ticks[PZ_RT] + g_ticks[PZ_SCENE] + g_ticks[PZ_STATEBLOCK] +
                         g_ticks[PZ_BUFLOCK] + g_ticks[PZ_END_FRAME] + g_ticks[PZ_F3] + g_ticks[PZ_PRESENT]) * k;
  std::wstring line;
  wchar_t b[96];
  swprintf_s(b, L"frame %.2f ms, thread cpu %.2f, in hooks %.2f, rest (game + plugins) %.2f |", wallMs, cpuMs, hooked, wallMs - hooked);
  line = b;
  for (int i = 0; i < PZ_COUNT; ++i) {
    const double ms = g_ticks[i] * k;
    if (ms < 0.005) continue;
    swprintf_s(b, L" %s=%.2f(%.0f)", kNames[i], ms, static_cast<double>(g_calls[i]) / g_frames);
    line += b;
  }
  rbrrtx_log_to(L"rbrrtx_prof.log", L"%s", line.c_str());
  std::memset(g_ticks, 0, sizeof(g_ticks));
  std::memset(g_calls, 0, sizeof(g_calls));
  g_frames = 0;
  g_lastWall = wall;
  g_lastCpu = cpu;
}
