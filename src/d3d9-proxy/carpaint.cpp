// Car paint (user 2026-11-03): the alpha of a car's *_external.dds / *_external_mud.dds is the game's reflectivity mask
// (environment map strength), not opacity. Remix ignores it, so every car looked equally dull. For each of those files
// in the player's car folder this writes a roughness map from the alpha (alpha 255 = shiny, 0 = dull) into the generated
// mod and a material on the texture's hash (section "carpaint"; the game's albedo stays). F3 page "Car paint": shiny /
// dull roughness and metallic, saved in materials.ini [car_paint]; the maps are rewritten on release of a slider.
#include "common.hpp"
#include "menu/menu.hpp"
#include "third_party/imgui/imgui.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

struct Params { int on = 1; float shiny = 0.12f, dull = 0.65f, metallic = 0.0f; };
Params g_p;
bool g_loaded = false;
std::wstring g_car;                                 // car folder handled
std::atomic<bool> g_busy{ false };
std::atomic<int> g_gen{ 0 };
std::mutex g_mx;
std::string g_status = "no car yet";

void ini_path(wchar_t* p) { rbrrtx_config_path(L"RBRRTX_MATERIALS", L"materials.ini", p, MAX_PATH * 2); }

void load() {
  g_loaded = true;
  wchar_t p[MAX_PATH * 2], b[32];
  ini_path(p);
  g_p.on = GetPrivateProfileIntW(L"car_paint", L"enable", 1, p);
  GetPrivateProfileStringW(L"car_paint", L"shiny_roughness", L"0.12", b, 32, p); g_p.shiny = static_cast<float>(_wtof(b));
  GetPrivateProfileStringW(L"car_paint", L"dull_roughness", L"0.65", b, 32, p);  g_p.dull = static_cast<float>(_wtof(b));
  GetPrivateProfileStringW(L"car_paint", L"metallic", L"0", b, 32, p);           g_p.metallic = static_cast<float>(_wtof(b));
}

void save() {
  wchar_t p[MAX_PATH * 2], b[32];
  ini_path(p);
  swprintf_s(b, L"%d", g_p.on);           WritePrivateProfileStringW(L"car_paint", L"enable", b, p);
  swprintf_s(b, L"%.3f", g_p.shiny);      WritePrivateProfileStringW(L"car_paint", L"shiny_roughness", b, p);
  swprintf_s(b, L"%.3f", g_p.dull);       WritePrivateProfileStringW(L"car_paint", L"dull_roughness", b, p);
  swprintf_s(b, L"%.3f", g_p.metallic);   WritePrivateProfileStringW(L"car_paint", L"metallic", b, p);
}

// Mip-0 alpha of a DXT3/DXT5/A8R8G8B8 DDS (DXT1: none useful). Also Remix's hash of mip 0.
bool read_alpha(const std::wstring& path, std::vector<unsigned char>& a, unsigned& w, unsigned& h, unsigned long long& hash) {
  std::ifstream f(path, std::ios::binary);
  char hd[128];
  if (!f || !f.read(hd, 128) || std::memcmp(hd, "DDS ", 4)) return false;
  unsigned flags, four, bits;
  std::memcpy(&h, hd + 12, 4); std::memcpy(&w, hd + 16, 4);
  std::memcpy(&flags, hd + 80, 4); std::memcpy(&four, hd + 84, 4); std::memcpy(&bits, hd + 88, 4);
  const bool dxt5 = (flags & 4) && four == MAKEFOURCC('D', 'X', 'T', '5'), dxt3 = (flags & 4) && four == MAKEFOURCC('D', 'X', 'T', '3');
  const bool argb = !(flags & 4) && bits == 32;
  if (!(dxt5 || dxt3 || argb) || w < 4 || h < 4 || w > 4096 || h > 4096) return false;
  const size_t size = argb ? size_t(w) * h * 4 : size_t((w + 3) / 4) * ((h + 3) / 4) * 16;
  std::vector<unsigned char> d(size);
  if (!f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(size))) return false;
  hash = rbrrtx_xxh3(d.data(), size);
  a.assign(size_t(w) * h, 255);
  if (argb) { for (size_t i = 0; i < size_t(w) * h; ++i) a[i] = d[i * 4 + 3]; return true; }
  const unsigned bw = (w + 3) / 4;
  for (unsigned by = 0; by < (h + 3) / 4; ++by)
    for (unsigned bx = 0; bx < bw; ++bx) {
      const unsigned char* b = &d[(size_t(by) * bw + bx) * 16];
      unsigned char v[16];
      if (dxt3) {
        for (int i = 0; i < 16; ++i) v[i] = static_cast<unsigned char>(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
      } else {
        unsigned pal[8] = { b[0], b[1] };
        if (b[0] > b[1]) for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * b[0] + i * b[1]) / 7;
        else { for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * b[0] + i * b[1]) / 5; pal[6] = 0; pal[7] = 255; }
        unsigned long long bitsIdx = 0;
        for (int i = 0; i < 6; ++i) bitsIdx |= static_cast<unsigned long long>(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) v[i] = static_cast<unsigned char>(pal[(bitsIdx >> (3 * i)) & 7]);
      }
      for (int i = 0; i < 16; ++i) {
        const unsigned x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x < w && y < h) a[size_t(y) * w + x] = v[i];
      }
    }
  return true;
}

// R8_UNORM DDS (DX10 header), one mip.
bool write_r8(const std::wstring& path, const std::vector<unsigned char>& px, unsigned w, unsigned h) {
  unsigned hd[32] = {}, dx10[5] = { 61, 3, 0, 1, 0 };
  hd[0] = 124; hd[1] = 0x1 | 0x2 | 0x4 | 0x1000; hd[2] = h; hd[3] = w; hd[4] = w; hd[6] = 1;
  hd[18] = 32; hd[19] = 0x4; hd[20] = MAKEFOURCC('D', 'X', '1', '0'); hd[26] = 0x1000;
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write("DDS ", 4);
  f.write(reinterpret_cast<const char*>(hd), 124);
  f.write(reinterpret_cast<const char*>(dx10), 20);
  f.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()));
  return static_cast<bool>(f);
}

void build(std::wstring car, Params p, int gen) {
  std::string body;
  int n = 0;
  const std::wstring texDir = std::wstring(rbrrtx_genmod_dir()) + L"\\carpaint";
  CreateDirectoryW(texDir.c_str(), nullptr);
  // Old maps of earlier settings go (a new name per generation: Remix caches textures by path).
  WIN32_FIND_DATAW fd;
  HANDLE fh = FindFirstFileW((texDir + L"\\*.dds").c_str(), &fd);
  if (fh != INVALID_HANDLE_VALUE) {
    do DeleteFileW((texDir + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(fh, &fd));
    FindClose(fh);
  }
  // The same paint file can sit in several folders of a car (same content, same hash). USD rejects a second prim of
  // the same name, so the whole mod failed to load (user's BMW M3 E30, 2026-11-07).
  std::unordered_set<unsigned long long> done;
  if (p.on) {
    std::vector<std::wstring> stack = { car };
    while (!stack.empty()) {
      const std::wstring d = stack.back();
      stack.pop_back();
      HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
      if (h == INVALID_HANDLE_VALUE) continue;
      do {
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring full = d + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { stack.push_back(full); continue; }
        std::wstring low = fd.cFileName;
        for (wchar_t& c : low) c = towlower(c);
        const bool ext = low.size() > 13 && low.compare(low.size() - 13, 13, L"_external.dds") == 0;
        const bool mud = low.size() > 17 && low.compare(low.size() - 17, 17, L"_external_mud.dds") == 0;
        if (!ext && !mud) continue;
        std::vector<unsigned char> a;
        unsigned w = 0, hh = 0;
        unsigned long long hash = 0;
        if (!read_alpha(full, a, w, hh, hash)) {
          rbrrtx_log_to(L"rbrrtx_comp.log", L"car paint: %s: format not read (DXT3/DXT5/A8R8G8B8 only)", full.c_str());
          continue;
        }
        if (!done.insert(hash).second) continue;
        for (unsigned char& v : a) v = static_cast<unsigned char>(255.0f * (p.dull + (p.shiny - p.dull) * (v / 255.0f)) + 0.5f);
        wchar_t name[64];
        swprintf_s(name, L"\\%016llX_%d.dds", hash, gen);
        if (!write_r8(texDir + name, a, w, hh)) continue;
        char met[96] = "";
        if (p.metallic > 0.0f) sprintf_s(met, "                float inputs:metallic_constant = %.3f\n", p.metallic);
        body += rbrrtx_genmod_material(hash, "car paint " + rbrrtx_genmod_asset(full),
                                       "                asset inputs:reflectionroughness_texture = @" +
                                       rbrrtx_genmod_asset(texDir + name) + "@ (\n                    colorSpace = \"raw\"\n                )\n" + met);
        ++n;
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  rbrrtx_genmod_set("carpaint", body);
  char s[160];
  sprintf_s(s, "%d paint texture(s): roughness %.2f shiny .. %.2f dull, metallic %.2f%s", n, p.shiny, p.dull, p.metallic, p.on ? "" : " (off)");
  { std::lock_guard<std::mutex> g(g_mx); g_status = s; }
  rbrrtx_log_to(L"rbrrtx_comp.log", L"car paint: %S (%s)", s, car.c_str());
  g_busy = false;
}

void start() {
  if (g_car.empty() || g_busy.exchange(true)) return;
  std::thread(build, g_car, g_p, ++g_gen).detach();
}

bool g_again = false;

void page(void*) {
  ImGui::TextWrapped("The alpha of the car's *_external / *_external_mud texture is the game's reflectivity mask. "
                     "It becomes a roughness map: alpha 255 = Shiny, 0 = Dull. Applied when a slider is released "
                     "(Remix reloads the generated mod), saved in materials.ini [car_paint].");
  bool on = g_p.on != 0, changed = false;
  if (ImGui::Checkbox("Enable", &on)) { g_p.on = on; changed = true; }
  ImGui::SliderFloat("Shiny roughness", &g_p.shiny, 0.0f, 1.0f, "%.2f"); changed |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SliderFloat("Dull roughness", &g_p.dull, 0.0f, 1.0f, "%.2f");   changed |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SliderFloat("Metallic", &g_p.metallic, 0.0f, 1.0f, "%.2f");     changed |= ImGui::IsItemDeactivatedAfterEdit();
  if (changed) { save(); if (g_busy) g_again = true; else start(); }
  std::lock_guard<std::mutex> g(g_mx);
  ImGui::TextDisabled("%s%s", g_status.c_str(), g_busy ? " (writing...)" : "");
}

}  // namespace

// Per frame (comp.cpp, beside rbrrtx_litparts_frame): a new car -> maps for its folder.
void rbrrtx_carpaint_frame(const wchar_t* carModel) {
  if (!g_loaded) load();
  if (g_again && !g_busy) { g_again = false; start(); }
  if (!carModel || !*carModel || !*rbrrtx_genmod_dir()) return;
  std::wstring dir(carModel);
  const size_t s = dir.find_last_of(L"\\/");
  if (s == std::wstring::npos) return;
  dir.resize(s);
  if (dir == g_car) return;
  g_car = dir;
  if (g_busy) g_again = true; else start();
}

void rbrrtx_carpaint_register_page() { rbrrtx_menu_add_page("Car paint", page, nullptr); }
