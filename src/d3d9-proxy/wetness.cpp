// wetness.cpp - wet roads and puddles that build up while it rains (user 2026-11-01: "the wet puddles accumulating,
// and the water really acting like water like in xoxor's work").
//
// The runtime (our Remix Plus branch, port of xoxor4d's rtx_fork_wetness: darkened wet ground, mirror-like puddles in
// the hollows, ripples and raindrop rings) reads one frame value, rtx.wet.globalWetness 0..1: the puddles' water level
// rises with it, so they grow from the low spots outward and dry from the edges. This file drives that value from the
// stage: it starts at the track's own condition (RBR's Dry / Damp / Wet), heads to rain_target while the game's rain
// falls (faster the harder it rains), and dries back towards the track's condition when the rain stops. Time runs only
// while driving (paused, menu and replays hold the value). Config: rbr-rtx\config\wetness.ini (RBRRTX_WETNESS).
// Remix Plus only (1.5.2 has no wet options); nothing is sent while a sweep or experiment runs (they set their own).
#define CINTERFACE
#include "common.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

bool g_on = true, g_loaded = false;
float g_base[3] = { 0.0f, 0.4f, 0.7f };               // start value for RBR's Dry / Damp / Wet track condition
float g_rainTarget = 1.0f, g_rainMin = 2.0f, g_dryMin = 15.0f, g_headStart = 1.0f, g_snowTarget = -1.0f;
std::string g_option = "rtx.wet.globalWetness", g_rainOption, g_dropsOption;
float g_w = 0.0f, g_sent = -1.0f, g_rainSent = -1.0f;
int g_dropsSent = -1;
int g_track = -1, g_cond = -1;
LARGE_INTEGER g_last = {}, g_qpf = {};
unsigned g_frames = 0;
double g_logT = 0.0;

float ini_f(const wchar_t* path, const wchar_t* key, float dflt) {
  wchar_t v[64] = {};
  GetPrivateProfileStringW(L"wetness", key, L"", v, 64, path);
  return v[0] ? static_cast<float>(_wtof(v)) : dflt;
}

std::string ini_s(const wchar_t* path, const wchar_t* key, const char* dflt) {
  wchar_t v[128] = {};
  GetPrivateProfileStringW(L"wetness", key, L"\x1", v, 128, path);
  if (v[0] == 1 && !v[1]) return dflt;
  std::string r;
  for (const wchar_t* c = v; *c; ++c) r += static_cast<char>(*c < 128 ? *c : '?');
  return r;
}

void load() {
  wchar_t path[MAX_PATH * 2];
  rbrrtx_config_path(L"RBRRTX_WETNESS", L"wetness.ini", path, MAX_PATH * 2);
  g_on = ini_f(path, L"enabled", 1.0f) != 0.0f;
  g_base[0] = ini_f(path, L"dry", g_base[0]);
  g_base[1] = ini_f(path, L"damp", g_base[1]);
  g_base[2] = ini_f(path, L"wet", g_base[2]);
  g_rainTarget = ini_f(path, L"rain_target", g_rainTarget);
  g_rainMin = (std::max)(0.05f, ini_f(path, L"rain_minutes", g_rainMin));
  g_dryMin = (std::max)(0.05f, ini_f(path, L"dry_minutes", g_dryMin));
  g_headStart = (std::max)(0.0f, ini_f(path, L"rain_head_start_minutes", g_headStart));
  g_snowTarget = ini_f(path, L"snow_target", g_snowTarget);
  g_option = ini_s(path, L"option", "rtx.wet.globalWetness");
  g_rainOption = ini_s(path, L"rain_option", "rtx.wet.rainIntensity");
  g_dropsOption = ini_s(path, L"raindrops_option", "rtx.wet.raindrops");
  QueryPerformanceFrequency(&g_qpf);
  rbrrtx_log_to(L"rbrrtx_comp.log", L"wetness (%s): %s, start dry %.2f damp %.2f wet %.2f, rain -> %.2f (%.1f min), dries in "
                L"%.1f min, head start %.1f min, -> %S%s%S", path, g_on ? L"on" : L"OFF", g_base[0], g_base[1], g_base[2],
                g_rainTarget, g_rainMin, g_dryMin, g_headStart, g_option.c_str(), g_rainOption.empty() ? L"" : L", rain -> ",
                g_rainOption.c_str());
}

// Rain now, 0..1 (the game's rain as the particles see it; the sky's name if the Remix rain is off).
float rain_now(const RbrStageInfo& si, bool* snow) {
  float pct = 0.0f;
  const int kind = rbrrtx_precip_state(&pct);
  *snow = kind == 2 || (kind == 0 && (si.skyType == 6 || si.skyType == 7));
  if (kind == 1) return std::clamp(pct / 100.0f, 0.0f, 1.0f);
  if (kind == 0) return si.skyType == 4 ? 1.0f : si.skyType == 3 ? 0.5f : 0.0f;   // HeavyRain, LightRain
  return 0.0f;
}

// One step of dt seconds: towards the rain target (time constant rain_minutes at full rain, longer in light rain) or
// back towards the track's own condition (dry_minutes).
float step(float w, float base, float rain, bool snow, double dt) {
  float target = base, tau = g_dryMin * 60.0f;
  if (snow && g_snowTarget >= 0.0f) target = g_snowTarget;
  else if (!snow && rain > 0.01f) {
    target = (std::max)(base, g_rainTarget);
    tau = g_rainMin * 60.0f / (std::max)(0.1f, rain);
  }
  return target + (w - target) * static_cast<float>(std::exp(-dt / tau));
}

}  // namespace

// Per Present. hold: a sweep or experiment owns the Remix options right now.
void rbrrtx_wetness_frame(bool hold) {
  if (!g_loaded) { g_loaded = true; load(); }
  if (!g_on || !rbrrtx_remix_plus()) return;
  RbrStageInfo si = {};
  const bool info = rbrrtx_stage_info(&si);
  const bool stage = info && (si.gameMode == 1 || si.gameMode == 2 || si.gameMode == 8 || si.gameMode == 10);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double dt = g_last.QuadPart ? static_cast<double>(now.QuadPart - g_last.QuadPart) / g_qpf.QuadPart : 0.0;
  g_last = now;
  bool snow = false;
  const float rain = stage ? rain_now(si, &snow) : 0.0f;
  const int cond = info ? std::clamp(si.wetness, 0, 2) : 0;
  if (!stage) {
    g_w = 0.0f;                                         // menu: dry; the next stage starts from its own condition
    g_track = -1;
  } else if (si.trackId != g_track || cond != g_cond) {  // a new stage (or restart with other weather)
    g_track = si.trackId;
    g_cond = cond;
    g_w = g_base[cond];
    if (rain > 0.01f && !snow && g_headStart > 0.0f)     // it has been raining for a while before the start
      g_w = step(g_w, g_base[cond], rain, snow, g_headStart * 60.0);
    rbrrtx_log_to(L"rbrrtx_comp.log", L"wetness: stage %d, track %s, %s %.0f %%: start %.2f", si.trackId,
                  rbrrtx_wetness_name(cond), snow ? L"snow" : L"rain", rain * 100.0f, g_w);
  } else if (si.gameMode == 1 || si.gameMode == 10) {   // driving or pre-stage: time runs (pause and replay hold)
    g_w = step(g_w, g_base[cond], rain, snow, (std::min)(dt, 0.25));
  }
  g_logT += dt;
  if (stage && g_logT > 30.0) {
    g_logT = 0.0;
    rbrrtx_log_to(L"rbrrtx_comp.log", L"wetness: %.2f (%s %.0f %%)", g_w, snow ? L"snow" : L"rain", rain * 100.0f);
  }
  if (hold || ++g_frames % 15) return;                  // a few times a second is plenty for a slow value
  if (std::fabs(g_w - g_sent) > 0.004f) {
    char v[32];
    sprintf_s(v, "%.3f", g_w);
    if (rbrrtx_remix_set(g_option.c_str(), v)) g_sent = g_w;
  }
  const float drops = snow ? 0.0f : rain;              // raindrop rings on puddles and wet ground while it rains
  if (!g_rainOption.empty() && std::fabs(drops - g_rainSent) > 0.01f) {
    char v[32];
    sprintf_s(v, "%.2f", drops);
    if (rbrrtx_remix_set(g_rainOption.c_str(), v)) g_rainSent = drops;
  }
  if (!g_dropsOption.empty() && (drops > 0.01f ? 1 : 0) != g_dropsSent) {
    if (rbrrtx_remix_set(g_dropsOption.c_str(), drops > 0.01f ? "True" : "False")) g_dropsSent = drops > 0.01f ? 1 : 0;
  }
}

float rbrrtx_wetness_now() { return g_w; }
