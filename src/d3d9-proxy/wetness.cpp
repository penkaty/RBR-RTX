// wetness.cpp - wet roads and puddles from the stage's conditions (user 2026-11-01: "the wet puddles accumulating, and the
// water really acting like water like in xoxor's work"; 2026-11-05: weather is static in RBR, so the value is too).
//
// The runtime (our Remix Plus branch, port of xoxor4d's rtx_fork_wetness: darkened wet ground, mirror-like puddles in
// the hollows, ripples and raindrop rings) reads rtx.wet.globalWetness 0..1. This file sets it once per stage from the
// track's condition (RBR's Dry / Damp / Wet), fog and rain, and Rain Intensity from the rain: heavy 1, light 0.5, none 0.
// Config: rbr-rtx\config\wetness.ini (RBRRTX_WETNESS). F3 sliders still override it live.
// Remix Plus only (1.5.2 has no wet options); nothing is sent while a sweep or experiment runs (they set their own).
#define CINTERFACE
#include "common.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

bool g_on = true, g_loaded = false;
float g_base[3] = { 0.0f, 0.40f, 0.80f };             // RBR's Dry / Damp / Wet track condition (user 2026-11-05)
float g_fog = 0.15f, g_rainLight = 0.40f, g_rainHeavy = 0.80f, g_snowTarget = -1.0f;
std::string g_option = "rtx.wet.globalWetness", g_rainOption, g_dropsOption;
float g_w = 0.0f, g_sent = -1.0f, g_rainSent = -1.0f, g_rainNow = 0.0f, g_logged = -1.0f;
int g_dropsSent = -1;
unsigned g_frames = 0;

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
  g_fog = ini_f(path, L"dry_fog", g_fog);
  g_rainLight = ini_f(path, L"rain_light", g_rainLight);
  g_rainHeavy = ini_f(path, L"rain_heavy", g_rainHeavy);
  g_snowTarget = ini_f(path, L"snow_target", g_snowTarget);
  g_option = ini_s(path, L"option", "rtx.wet.globalWetness");
  g_rainOption = ini_s(path, L"rain_option", "rtx.wet.rainIntensity");
  g_dropsOption = ini_s(path, L"raindrops_option", "rtx.wet.raindrops");
  rbrrtx_log_to(L"rbrrtx_comp.log", L"wetness (%s): %s, dry %.2f (fog %.2f) damp %.2f wet %.2f, light rain %.2f, heavy rain %.2f, -> %S%s%S",
                path, g_on ? L"on" : L"OFF", g_base[0], g_fog, g_base[1], g_base[2], g_rainLight, g_rainHeavy, g_option.c_str(),
                g_rainOption.empty() ? L"" : L", rain -> ", g_rainOption.c_str());
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

// The stage's wetness: the wetter of its track condition and its weather. Fog damps a dry road (skyType 8, 9).
float stage_wetness(const RbrStageInfo& si, int cond, float rain, bool snow) {
  float w = g_base[cond];
  if (cond == 0 && (si.skyType == 8 || si.skyType == 9)) w = (std::max)(w, g_fog);
  if (snow) return g_snowTarget >= 0.0f ? g_snowTarget : w;
  if (rain > 0.01f) w = (std::max)(w, rain >= 0.75f ? g_rainHeavy : g_rainLight);
  return w;
}

}  // namespace

// Per Present. hold: a sweep or experiment owns the Remix options right now.
void rbrrtx_wetness_frame(bool hold) {
  if (!g_loaded) { g_loaded = true; load(); }
  if (!g_on || !rbrrtx_remix_plus()) return;
  RbrStageInfo si = {};
  const bool info = rbrrtx_stage_info(&si);
  const bool stage = info && (si.gameMode == 1 || si.gameMode == 2 || si.gameMode == 8 || si.gameMode == 10);
  // Stage entered or left: slider holds end, and everything goes out again (the menu sent other values than g_sent knows).
  static bool wasStage = false;
  if (stage != wasStage) {
    wasStage = stage;
    rbrrtx_menu_release_stage_locks();
    g_sent = g_rainSent = -1.0f;
    g_dropsSent = -1;
  }
  bool snow = false;
  const float rain = stage ? rain_now(si, &snow) : 0.0f;
  g_rainNow = snow ? 0.0f : rain;
  g_w = stage ? stage_wetness(si, info ? std::clamp(si.wetness, 0, 2) : 0, rain, snow) : 0.0f;
  if (stage && g_w != g_logged) {
    g_logged = g_w;
    rbrrtx_log_to(L"rbrrtx_comp.log", L"wetness: stage %d, track %s, sky %s, %s %.0f %%: %.2f", si.trackId,
                  rbrrtx_wetness_name(std::clamp(si.wetness, 0, 2)), rbrrtx_sky_name(si.skyType), snow ? L"snow" : L"rain",
                  rain * 100.0f, g_w);
  }
  if (hold || ++g_frames % 15) return;                  // a few times a second is plenty for a slow value
  float menuValue = 0.0f;                               // an option the user moved in the F3 menu is theirs (the menu sent it)
  if (std::fabs(g_w - g_sent) > 0.004f && !rbrrtx_menu_option_locked(g_option.c_str(), &menuValue)) {
    char v[32];
    sprintf_s(v, "%.3f", g_w);
    if (rbrrtx_remix_set(g_option.c_str(), v)) g_sent = g_w;
  }
  const float drops = g_rainNow;                       // raindrop rings on puddles and wet ground while it rains
  if (!g_rainOption.empty() && std::fabs(drops - g_rainSent) > 0.01f && !rbrrtx_menu_option_locked(g_rainOption.c_str(), &menuValue)) {
    char v[32];
    sprintf_s(v, "%.2f", drops);
    if (rbrrtx_remix_set(g_rainOption.c_str(), v)) g_rainSent = drops;
  }
  if (!g_dropsOption.empty() && (drops > 0.01f ? 1 : 0) != g_dropsSent && !rbrrtx_menu_option_locked(g_dropsOption.c_str(), &menuValue)) {
    if (rbrrtx_remix_set(g_dropsOption.c_str(), drops > 0.01f ? "True" : "False")) g_dropsSent = drops > 0.01f ? 1 : 0;
  }
}

float rbrrtx_wetness_now() { return g_w; }
float rbrrtx_wetness_rain_now() { return g_rainNow; }
