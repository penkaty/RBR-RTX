// genmod.cpp - the add-on's generated Remix mod, <mods>\rbr_rtx_lit\mod.usda (folder name kept from 2026-10-23).
//
// Several parts of the proxy give game textures a Remix material by texture hash: lit lamp textures (headlights.cpp,
// "lit"), lit car parts such as hot brake discs (litparts.cpp, "litparts"), thin-opaque foliage (foliage.cpp,
// "foliage"). Each sets its section here; the file is the sections in that order, rewritten only when its content
// changes. Remix opens the mods it finds when it starts (so the file is first written before the backend loads,
// d3d9_proxy.cpp), and reloads a mod.usda that changes on disk: 1.5.2 polls the file time every second
// (rtx_mod_usd.cpp haveFilesChanged / Watchdog<1000>), Remix Plus watches it (rtx.usd.reloadOnChanged, default on).
// A reload costs a hitch, so a changed section is written at most every kMinGap seconds (rbrrtx_genmod_tick).
// Small text files the sections keep between runs (known hashes) live in the same folder.
#include "common.hpp"
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

namespace {

#define glog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

constexpr double kMinGap = 4.0;                          // seconds between two rewrites while playing
std::mutex g_mx;
std::wstring g_dir;                                      // <mods>\rbr_rtx_lit
std::map<int, std::string> g_sections;                   // order -> materials (USD text inside the Looks scope)
bool g_dirty = false;
unsigned g_writes = 0;
std::chrono::steady_clock::time_point g_lastWrite;

int order_of(const char* section) {
  if (!strcmp(section, "lit")) return 0;
  if (!strcmp(section, "litparts")) return 1;
  if (!strcmp(section, "foliage")) return 2;
  return 3;
}

void write_locked(const wchar_t* why) {
  if (g_dir.empty()) return;
  std::string body;
  for (const auto& s : g_sections) body += s.second;
  const std::wstring file = g_dir + L"\\mod.usda";
  std::string old;
  {
    std::ifstream in(file, std::ios::binary);
    old.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  const std::string usda = "#usda 1.0\n(\n    upAxis = \"Y\"\n)\n\n# Written by RBR-RTX (genmod.cpp) from the installed game; do not edit, it is rewritten.\n"
                           "def Xform \"RootNode\"\n{\n    def Scope \"Looks\"\n    {\n" + body + "    }\n}\n";
  g_dirty = false;
  g_lastWrite = std::chrono::steady_clock::now();
  if (usda == old) return;
  CreateDirectoryW(g_dir.c_str(), nullptr);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << usda;
  ++g_writes;
  glog(L"generated mod: %s written (%zu bytes, %s)", file.c_str(), usda.size(), why);
}

}  // namespace

void rbrrtx_genmod_init(const wchar_t* modsDir) {
  std::lock_guard<std::mutex> g(g_mx);
  g_dir = std::wstring(modsDir) + L"\\rbr_rtx_lit";
  CreateDirectoryW(modsDir, nullptr);
  CreateDirectoryW(g_dir.c_str(), nullptr);
}

const wchar_t* rbrrtx_genmod_dir() { return g_dir.c_str(); }

void rbrrtx_genmod_set(const char* section, const std::string& materials) {
  std::lock_guard<std::mutex> g(g_mx);
  std::string& s = g_sections[order_of(section)];
  if (s == materials) return;
  s = materials;
  g_dirty = true;
}

void rbrrtx_genmod_flush(const wchar_t* why) {
  std::lock_guard<std::mutex> g(g_mx);
  write_locked(why);
}

void rbrrtx_genmod_tick() {
  if (!g_dirty) return;
  std::lock_guard<std::mutex> g(g_mx);
  if (!g_dirty || std::chrono::duration<double>(std::chrono::steady_clock::now() - g_lastWrite).count() < kMinGap) return;
  write_locked(L"changed while playing: Remix reloads it");
}

// One opaque replacement material for texture hash `hash`. `inputs` = USD input lines (each ending in \n), indented.
std::string rbrrtx_genmod_material(unsigned long long hash, const std::string& comment, const std::string& inputs) {
  // The comment can hold a full file path: no fixed buffer (sprintf_s on a too-long line kills the game, 2026-10-30).
  char name[40];
  sprintf_s(name, "mat_%016llX", hash);
  return "        # " + comment + "\n        def Material \"" + name + "\"\n" +
         "        {\n"
         "            def Shader \"Shader\"\n"
         "            {\n"
         "                uniform token info:implementationSource = \"sourceAsset\"\n"
         "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
         "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n" +
         inputs +
         "                token outputs:out\n"
         "            }\n"
         "        }\n";
}

// A file path as a USD asset path: UTF-8, forward slashes.
std::string rbrrtx_genmod_asset(const std::wstring& path) {
  char u8[MAX_PATH * 4] = {};
  WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, u8, sizeof(u8), nullptr, nullptr);
  std::string s(u8);
  for (char& c : s) if (c == '\\') c = '/';
  return s;
}
