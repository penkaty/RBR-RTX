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
// One mod folder per section (user 2026-11-02: a lit car part's new twin rewrote the one shared mod.usda, Remix reloaded the
// whole mod and the marked lamp glass flapped): "lit" stays <mods>\rbr_rtx_lit, the others are <mods>\rbr_rtx_<section>, each
// with its own mod.usda written only when its own text changes. The folders are created (with an empty mod.usda) at init, so
// Remix finds them at its start.
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
std::wstring g_dir, g_mods;                              // <mods>\rbr_rtx_lit, <mods>
std::map<std::string, std::string> g_sections;           // section -> materials (USD text inside the Looks scope)
std::map<std::string, bool> g_secDirty;
bool g_dirty = false;
unsigned g_writes = 0;
std::chrono::steady_clock::time_point g_lastWrite;

const char* const kSections[] = { "lit", "litparts", "foliage", "marked", "emissive" };

std::wstring folder_of(const std::string& section) {
  if (section == "lit") return g_dir;
  return g_mods + L"\\rbr_rtx_" + std::wstring(section.begin(), section.end());
}

std::string usda_of(const std::string& body) {
  return "#usda 1.0\n(\n    upAxis = \"Y\"\n)\n\n# Written by RBR-RTX (genmod.cpp) from the installed game; do not edit, it is rewritten.\n"
         "def Xform \"RootNode\"\n{\n    def Scope \"Looks\"\n    {\n" + body + "    }\n}\n";
}

// Writes the section's own file when its text differs from the file on disk.
void write_section_locked(const std::string& section, const wchar_t* why) {
  const std::wstring dir = folder_of(section);
  if (dir.empty() || g_mods.empty()) return;
  const std::wstring file = dir + L"\\mod.usda";
  std::string old;
  {
    std::ifstream in(file, std::ios::binary);
    old.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  const std::string usda = usda_of(g_sections[section]);
  g_secDirty[section] = false;
  if (usda == old) return;
  CreateDirectoryW(dir.c_str(), nullptr);
  // Written beside the file and moved over it, so Remix never parses half a file (2026-11-06: the car paint mod
  // was rewritten while Remix loaded mods and failed to parse, so that run had no car paint).
  const std::wstring tmp = file + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << usda;
  }
  if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    glog(L"generated mod [%S]: could not replace %s (error %lu)", section.c_str(), file.c_str(), GetLastError());
    return;
  }
  ++g_writes;
  glog(L"generated mod [%S]: %s written (%zu bytes, %s)", section.c_str(), file.c_str(), usda.size(), why);
}

void write_locked(const wchar_t* why) {
  for (auto& d : g_secDirty)
    if (d.second) write_section_locked(d.first, why);
  g_dirty = false;
  g_lastWrite = std::chrono::steady_clock::now();
}

}  // namespace

void rbrrtx_genmod_init(const wchar_t* modsDir) {
  std::lock_guard<std::mutex> g(g_mx);
  g_mods = modsDir;
  g_dir = std::wstring(modsDir) + L"\\rbr_rtx_lit";
  CreateDirectoryW(modsDir, nullptr);
  CreateDirectoryW(g_dir.c_str(), nullptr);
  for (const char* sec : kSections) {                    // every feature's folder exists before Remix starts
    const std::wstring dir = folder_of(sec);
    CreateDirectoryW(dir.c_str(), nullptr);
    if (GetFileAttributesW((dir + L"\\mod.usda").c_str()) == INVALID_FILE_ATTRIBUTES) {
      std::ofstream out(dir + L"\\mod.usda", std::ios::binary);
      out << usda_of("");
    }
  }
}

const wchar_t* rbrrtx_genmod_dir() { return g_dir.c_str(); }

void rbrrtx_genmod_set(const char* section, const std::string& materials) {
  std::lock_guard<std::mutex> g(g_mx);
  std::string& s = g_sections[section];
  if (s == materials) return;
  s = materials;
  g_secDirty[section] = true;
  g_dirty = true;
}

void rbrrtx_genmod_flush(const wchar_t* why) {
  std::lock_guard<std::mutex> g(g_mx);
  for (const char* sec : kSections) write_section_locked(sec, why);   // all of them: an older shared file may hold stale sections
  g_dirty = false;
  g_lastWrite = std::chrono::steady_clock::now();
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
