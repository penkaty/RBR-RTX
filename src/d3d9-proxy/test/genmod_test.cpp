// Test of genmod.cpp + foliage.cpp + litparts.cpp start-up outside the game: a mods folder with cached foliage hashes
// and a stale lit-part line; the generated mod.usda must hold the foliage materials (thin opaque) and drop the stale
// line. Writes <temp>\rbrrtx_genmod_test\rbr_rtx_lit\mod.usda (check it with usd-core: tools\pbr\.venv).
//   x86 cl test\genmod_test.cpp ..\genmod.cpp ..\foliage.cpp ..\litparts.cpp -> genmod_test.exe
#include "../common.hpp"
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"

void rbrrtx_log_to(const wchar_t*, const wchar_t* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vwprintf(fmt, a);
  va_end(a);
  wprintf(L"\n");
}
void rbrrtx_config_path(const wchar_t*, const wchar_t* file, wchar_t* out, size_t size) { swprintf_s(out, size, L"none\\%s", file); }
unsigned long long rbrrtx_xxh3(const void* data, size_t size) { return XXH3_64bits(data, size); }
unsigned long long rbrrtx_texture_hash(void* t) { return t == reinterpret_cast<void*>(0x1000) ? 0x1234567890ABCDEFULL : 0; }
bool rbrrtx_texture_avg_rgb(void*, float rgb[3]) { rgb[0] = 0.2f; rgb[1] = 0.4f; rgb[2] = 0.1f; return true; }
bool rbrrtx_player_car_brake(float* b) { *b = 0.0f; return true; }
float rbrrtx_sun_shown_elevation() { return 30.0f; }
bool rbrrtx_car_body_draw(float*) { return false; }
bool rbrrtx_player_car_gear(int* g) { *g = 2; return true; }

int wmain() {
  const std::filesystem::path mods = std::filesystem::temp_directory_path() / L"rbrrtx_genmod_test";
  std::filesystem::remove_all(mods);
  std::filesystem::create_directories(mods / L"rbr_rtx_lit");
  {
    std::ofstream f(mods / L"rbr_rtx_lit" / L"foliage.txt");
    f << "00000000DEADBEEF 0.5000 0.8000 0.2000\nAAAAAAAAAAAAAAAA 0.1 0.2 0.3\n";
    std::ofstream l(mods / L"rbr_rtx_lit" / L"litparts.txt");
    l << "0123456789ABCDEF 1111111111111111 0 C:/nowhere/atlas.dds\n";
  }
  rbrrtx_genmod_init(mods.c_str());
  rbrrtx_litparts_startup();
  rbrrtx_foliage_startup();
  rbrrtx_foliage_draw(reinterpret_cast<void*>(0x1000));   // a new foliage texture this run
  rbrrtx_genmod_flush(L"test");
  std::ifstream in(mods / L"rbr_rtx_lit" / L"mod.usda");
  const std::string usda((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  int fails = 0;
  auto check = [&](bool ok, const wchar_t* what) { wprintf(L"%s: %s\n", what, ok ? L"ok" : L"FAIL"); if (!ok) ++fails; };
  check(usda.find("mat_00000000DEADBEEF") != std::string::npos, L"cached foliage hash in the mod");
  check(usda.find("subsurface_measurement_distance") != std::string::npos, L"thin opaque inputs");
  check(usda.find("mat_0123456789ABCDEF") == std::string::npos, L"stale lit-part line dropped");
  check(usda.find("mat_1234567890ABCDEF") == std::string::npos, L"new foliage waits for the quiet time");
  // A lit-part comment holds the atlas path; a fixed 160-byte buffer here crashed the game (2026-10-30).
  const std::string longPath = "lit car part (lamps), atlas C84C9BE5E651C0C7: E:/rbr-dev/game-copy/Cars/" + std::string(260, 'x') + ".dds";
  const std::string mat = rbrrtx_genmod_material(0xC84C9BE5E651C0C7ULL, longPath, "");
  check(mat.find(longPath) != std::string::npos && mat.find("mat_C84C9BE5E651C0C7") != std::string::npos,
        L"material with a 330-character comment");
  wprintf(L"%s\n", (mods / L"rbr_rtx_lit" / L"mod.usda").c_str());
  return fails ? 1 : 0;
}
