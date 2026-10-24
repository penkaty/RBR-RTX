// weather_test.cpp - rbrrtx_weather_from_text on fixed descriptions, then (with a Maps folder) every description of an
// install: section name, description, what it names.
// Build (x86 cl, from src\d3d9-proxy):
//   cl /nologo /EHsc /std:c++17 /DUNICODE /D_UNICODE test\weather_test.cpp weather.cpp game_state.cpp /Fe:test\weather_test.exe /Fo:obj\ user32.lib gdi32.lib ole32.lib windowscodecs.lib
// Run: test\weather_test.exe [E:\RichardRustRally\game-copy\Maps]
#include "../common.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

void rbrrtx_log_to(const wchar_t*, const wchar_t*, ...) {}

int main(int argc, char** argv) {
  struct Case { const wchar_t* text; int gameSky; int sky, clouds, time; };
  const Case cases[] = {
    { L"Evening HeavyCloud LightSnow", 2, 6, 3, 2 },
    { L"Rainy night", 0, 3, 3, 3 },
    { L"Noche Despejada", 2, 0, 0, 3 },
    { L"MedioDia Despejado", 2, 0, 0, 1 },
    { L"Poca Lluvia", 0, 3, 3, -1 },
    { L"Mediodia Lluvia", 4, 4, 3, 1 },
    { L"PINK DAWN", 0, -1, -1, 4 },
    { L"CALIMA", 0, 1, -1, -1 },
    { L"Water in ruts", 0, -1, -1, -1 },
    { L"No Ruts", 0, -1, -1, -1 },
    { L"Morning PartCloud NoRain", 4, 2, 1, 0 },
    { L"Heavy fog", 0, 9, -1, -1 },
    { L"NIGHT/DARK", 0, -1, -1, 3 },
    { L"Lluvia fuerte", 3, 4, 3, -1 },
    { L"Wieczór, mgła", 0, 8, -1, 2 },
    { L"Starry Night", 0, -1, -1, 3 },
    { L"Sunset", 0, -1, -1, 5 },
  };
  int fails = 0;
  for (const Case& c : cases) {
    WeatherText w;
    rbrrtx_weather_from_text(c.text, c.gameSky, &w);
    const bool ok = w.sky == c.sky && w.clouds == c.clouds && w.time == c.time;
    if (!ok) ++fails;
    wprintf(L"%s  \"%s\" -> sky %s clouds %s time %s\n", ok ? L"ok  " : L"FAIL", c.text, w.sky >= 0 ? rbrrtx_sky_name(w.sky) : L"-",
            w.clouds >= 0 ? rbrrtx_cloud_name(w.clouds) : L"-", w.time >= 0 ? rbrrtx_weather_time_name(w.time) : L"-");
  }
  wprintf(L"%d of %zu fixed cases failed\n", fails, sizeof(cases) / sizeof(cases[0]));
  if (argc < 2) return fails ? 1 : 0;
  // Every description of an install: "; text" after section names and Comment lines.
  namespace fs = std::filesystem;
  unsigned n = 0, named = 0;
  for (const auto& e : fs::recursive_directory_iterator(argv[1])) {
    const std::wstring f = e.path().filename().wstring();
    if (f.rfind(L"TrackSettings", 0) != 0 || e.path().extension() != L".ini") continue;
    std::ifstream in(e.path());
    std::string sec;
    for (std::string l; std::getline(in, l);) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      std::string d;
      const size_t b = l.find_first_not_of(" \t");
      if (b == std::string::npos) continue;
      if (l[b] == '[') {
        const size_t c = l.find(']', b);
        sec = l.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
        const size_t t = c == std::string::npos ? std::string::npos : l.find_first_not_of(" \t;/#", c + 1);
        if (t != std::string::npos) d = l.substr(t);
      } else if (_strnicmp(l.c_str() + b, "comment", 7) == 0 && l.find('=', b) != std::string::npos) {
        d = l.substr(l.find('=', b) + 1);
        const size_t t = d.find_first_not_of(" \t");
        d = t == std::string::npos ? "" : d.substr(t);
      }
      if (d.empty()) continue;
      wchar_t wd[512] = {};
      MultiByteToWideChar(CP_ACP, 0, d.c_str(), -1, wd, 511);
      WeatherText w;
      const bool any = rbrrtx_weather_from_text(wd, -1, &w);
      ++n;
      named += any;
      wprintf(L"%-28S %-40s -> %s %s %s\n", sec.c_str(), wd, w.sky >= 0 ? rbrrtx_sky_name(w.sky) : L"-",
              w.clouds >= 0 ? rbrrtx_cloud_name(w.clouds) : L"-", w.time >= 0 ? rbrrtx_weather_time_name(w.time) : L"-");
    }
  }
  wprintf(L"%u descriptions, %u name a weather or time\n", n, named);
  return fails ? 1 : 0;
}
