// rbrhud.cpp - RBRHUD's F4 windscreen settings under Remix (user 2026-11-05: "its menu at F4 allows the user to change
// many things including windshield opacity and even just its banner using a simple transparency limit"; top fix
// before 0.4.0). Plan: docs/research/car-and-hud-investigation-2026-11-05.md "RBRHUD done properly".
//
// RBRHUD draws the cockpit windscreen (the game's CarGlassInternal.vso) with its own pixel shader
// Plugins\RBRHUD\Shaders\car\ps\general\CarGlassInternal.pso (constant table: b0 b_transparent_regions_only,
// c0 constant0, s0 tex0). Disassembled 2026-11-05:
//   colour = tex.rgb * v1.rgb
//   b0 on:  alpha = tex.a >= threshold ? tex.a : tex.a * opacity     (c0.z = -threshold, c0.w = opacity)
//   b0 off: alpha = tex.a * opacity
// Remix never runs that shader, so the F4 values did nothing. rbrhud.ini [windscreen] opacity, transparentregionsonly,
// transparencythreshold hold them (read at start and again whenever the file changes). While they change anything,
// comp.cpp's glass split applies them to the cockpit windscreen draw (glass_split_plan): live, no copy of the sheet.
#define CINTERFACE
#include "common.hpp"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#define hlog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

namespace {

struct Settings {
  float opacity = 1.0f, threshold = 1.0f;
  bool regionsOnly = true;
  bool found = false;
};

Settings g_set;
std::wstring g_ini;
FILETIME g_iniTime = {};
ULONGLONG g_nextCheck = 0;
bool g_hud = false;                                    // RBRHUD.dll is loaded

float ini_f(const wchar_t* key, float dflt) {
  wchar_t v[64] = {};
  GetPrivateProfileStringW(L"windscreen", key, L"", v, 64, g_ini.c_str());
  return v[0] ? static_cast<float>(_wtof(v)) : dflt;
}

// rbrhud.ini next to RBRHUD.dll (Plugins\RBRHUD\rbrhud.ini), read again when its time stamp moves.
void poll_ini() {
  const ULONGLONG now = GetTickCount64();
  if (now < g_nextCheck) return;
  g_nextCheck = now + 2000;
  if (g_ini.empty()) {
    HMODULE m = GetModuleHandleW(L"RBRHUD.dll");
    g_hud = m != nullptr;
    if (!m) return;
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(m, p, MAX_PATH * 2);
    if (wchar_t* s = wcsrchr(p, L'\\')) s[1] = 0;
    g_ini = std::wstring(p) + L"RBRHUD\\rbrhud.ini";
  }
  WIN32_FILE_ATTRIBUTE_DATA fa = {};
  if (!GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &fa)) { g_set.found = false; return; }
  if (g_set.found && CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) == 0) return;
  g_iniTime = fa.ftLastWriteTime;
  Settings s;
  s.found = true;
  s.opacity = std::clamp(ini_f(L"opacity", 1.0f), 0.0f, 1.0f);
  s.threshold = std::clamp(ini_f(L"transparencythreshold", 1.0f), 0.0f, 1.0f);
  s.regionsOnly = ini_f(L"transparentregionsonly", 1.0f) != 0.0f;
  if (!g_set.found || s.opacity != g_set.opacity || s.threshold != g_set.threshold || s.regionsOnly != g_set.regionsOnly)
    hlog(L"rbrhud: windscreen opacity %.2f, %s (threshold %.2f) from %s%s", s.opacity,
         s.regionsOnly ? L"only below the threshold" : L"everywhere", s.threshold, g_ini.c_str(),
         s.opacity >= 0.999f ? L" - nothing to change" : L" - applied to the cockpit windscreen's glass split");
  g_set = s;
}

}  // namespace

// RBRHUD's F4 windscreen settings while they change anything (opacity below 1). comp.cpp applies them in the glass split.
bool rbrrtx_rbrhud_windscreen(float* opacity, float* threshold, bool* regionsOnly) {
  poll_ini();
  if (!g_hud || !g_set.found || g_set.opacity >= 0.999f) return false;
  *opacity = g_set.opacity;
  *threshold = g_set.threshold;
  *regionsOnly = g_set.regionsOnly;
  return true;
}
