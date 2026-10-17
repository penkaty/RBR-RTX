// Test of game_state.cpp outside the game: RBR's globals are not mapped here, so they must read as -1 (SEH); the RSF
// stage list (RBRRTX_RSF_STAGES = a stages_data.json) must give name/country/surface; a screenshot of the desktop
// window must be written as PNG.
//   game_state_test.exe <stages_data.json> <out.png>
#include "../game_state.cpp"
#include <cstdarg>
#include <cstdio>

void rbrrtx_log_to(const wchar_t*, const wchar_t* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vwprintf(fmt, a);
  va_end(a);
  wprintf(L"\n");
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) { wprintf(L"usage: game_state_test <stages_data.json> <out.png>\n"); return 2; }
  SetEnvironmentVariableW(L"RBRRTX_RSF_STAGES", argv[1]);
  int fails = 0;
  RbrStageInfo si;
  const bool ok = rbrrtx_stage_info(&si);
  if (ok || si.trackId != -1 || si.skyType != -1) { wprintf(L"FAIL unmapped globals: ok %d track %d sky %d\n", ok, si.trackId, si.skyType); ++fails; }
  // RSF lookup through the loaded table (game_state.cpp internals)
  auto check = [&](int id, const wchar_t* name, const char* country) {
    auto it = g_rsf.find(id);
    if (it == g_rsf.end() || it->second.name != name || it->second.country != country) {
      wprintf(L"FAIL stage %d: %s / %S\n", id, it == g_rsf.end() ? L"(missing)" : it->second.name.c_str(),
              it == g_rsf.end() ? "" : it->second.country.c_str());
      ++fails;
    } else {
      wprintf(L"stage %d = %s (%S, tarmac %d gravel %d snow %d)\n", id, it->second.name.c_str(), it->second.country.c_str(),
              it->second.tarmac, it->second.gravel, it->second.snow);
    }
  };
  check(22, L"Chirdonhead", "GB");
  check(458, L"Rosciszow-Walim", "PL");
  check(485, L"Woliborz - Jodlownik", "PL");
  check(509, L"Santa Lucia", "ES");
  wprintf(L"names: sky 0 %s, 9 %s, 10 %s; clouds 3 %s; time 2 %s\n", rbrrtx_sky_name(0), rbrrtx_sky_name(9), rbrrtx_sky_name(10),
          rbrrtx_cloud_name(3), rbrrtx_time_name(2));
  double luma = -1;
  if (!rbrrtx_screenshot(GetDesktopWindow(), argv[2], &luma)) { wprintf(L"FAIL screenshot\n"); ++fails; }
  else wprintf(L"screenshot %s, mean brightness %.1f\n", argv[2], luma);
  wprintf(fails ? L"%d FAILED\n" : L"game_state checks passed\n", fails);
  return fails;
}
