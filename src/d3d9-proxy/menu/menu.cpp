// menu.cpp - F3 in-game menu for the RBR-RTX d3d9 proxy. See menu.hpp and docs/research/f3-menu-plan.md.
//
// Draws with Dear ImGui 1.91.7 (third_party/imgui, MIT) through the game's own IDirect3DDevice9:
// ImGui_ImplDX9 for rendering, ImGui_ImplWin32 for input. Everything runs on the game's render thread.

#include "menu.hpp"

#pragma warning(push, 0)      // Dear ImGui is not ours: its headers stay out of the proxy's /W4
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/backends/imgui_impl_dx9.h"
#include "../third_party/imgui/backends/imgui_impl_win32.h"
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// The win32 backend defines this but its header does not declare it (remix-comp-base does the same).
LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

float rbrrtx_wetness_now();
float rbrrtx_wetness_rain_now();

namespace {

// ---- state -------------------------------------------------------------------------------------------------

struct Page {
  const char* name;
  rbrrtx_menu_page_fn draw;
  void* user;
};

std::vector<Page> g_pages;
bool g_builtins_added = false;

rbrrtx_menu_callbacks g_cb = {};
HWND g_wnd = nullptr;
IDirect3DDevice9* g_dev = nullptr;
bool g_imgui_ready = false;   // ImGui context + Win32 backend exist
bool g_dx9_ready = false;     // ImGui DX9 backend initialised for g_dev (it holds one reference on the device)
bool g_open = false;
bool g_drawing = false;
bool g_pending_apply = true;  // menu.ini values still to send to Remix (waits for remix_set)
bool g_srgb_fix = false;      // remix-comp-base forces SRGB sampler/write around its draws; off until tested
int g_toggle_vk = VK_F3;      // keys.ini "menu"; 0 = the menu cannot be opened

// Values the user set in the menu (or loaded from menu.ini). A key here is "locked": the proxy must not overwrite it.
std::map<std::string, float> g_touched;
// What menu.ini holds for [wetness] (loaded, or written by the Save button). A texture tag saves the file at once and
// must not persist slider values the user never saved.
std::map<std::string, float> g_saved;
// Remix options the user pinned (menu.ini [remix]): applied at the start after rtx.conf, win over the proxy's own sets.
std::map<std::string, std::string> g_pins;
bool g_pins_pending = false;   // some pin still to send to Remix
// Switched off 2026-11-03 (user): rtx.conf / user.conf (Alt+X Save) are the only owners of Remix settings. The "Remix
// settings" page and the pins are kept for later; set true to bring them back. Pins already in menu.ini [remix] are kept
// in the file (g_pins_parked) but not applied.
constexpr bool kRemixPins = false;
std::map<std::string, std::string> g_pins_parked;

char g_status[160] = "";      // one line of feedback at the bottom of the window

// Texture picker state (the proxy probes while the page asks for new results and it is not frozen).
bool g_tex_freeze = false;
bool g_tex_skip_foliage = false;   // the ray goes through swaying / alpha-tested foliage cards (lamps are behind trees)
char g_lamp_msg[200] = "";
int g_tex_target = 0;         // which texture of the draw the tag buttons act on: 0 = t0, 1 = t1

// ---- Wetness page: mirrors the rtx.wet.* options of the Remix runtime (rtx_fork_wetness.h) -------------------
// type 0 = float slider (min..max), type 1 = bool checkbox, type 2 = integer slider. adv: under "Advanced".
// The Alt+X "Wetness" header of our Remix runtime is gone (user 2026-11-02); this page carries all of its options.
struct WetOpt {
  const char* key;
  const char* label;
  int type;
  float def;
  float lo, hi;
  const char* fmt;
  int adv;
};

const WetOpt kWet[] = {
  { "rtx.wet.globalWetness",             "Global Wetness",                 0, 0.00f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.raindrops",                 "Raindrops",                      1, 0.00f, 0.0f, 0.0f,  nullptr },
  { "rtx.wet.rainIntensity",             "Rain Intensity",                 0, 1.00f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.raindropScale",             "Raindrop Scale",                 0, 3.00f, 0.1f, 10.0f, "%.2f" },
  { "rtx.wet.raindropDensity",           "Raindrop Density",               0, 4.00f, 1.0f, 8.0f,  "%.2f" },
  { "rtx.wet.raindropGroundScale",       "Raindrops on Wet Ground",        0, 1.00f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.minNormal",                 "Min Up Normal",                  0, 0.54f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.blendWidth",                "Blend Width",                    0, 0.33f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.fullyWetRoughnessScale",    "Fully Wet Roughness Scale",      0, 0.02f, 0.0f, 2.0f,  "%.2f" },
  { "rtx.wet.patches",                   "Wet Patches",                    1, 1.00f, 0.0f, 0.0f,  nullptr },
  { "rtx.wet.puddles",                   "Puddles",                        1, 1.00f, 0.0f, 0.0f,  nullptr },
  { "rtx.wet.puddleCoverage",            "Puddle Coverage",                0, 1.00f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.depressionWeight",          "Depression Weight",              0, 0.33f, 0.0f, 5.0f,  "%.2f" },
  { "rtx.wet.slopeLimit",                "Slope Limit",                    0, 0.150f, 0.005f, 1.0f, "%.3f" },
  { "rtx.wet.slopeFeather",              "Slope Feather",                  0, 0.33f, 0.1f, 4.0f,  "%.2f" },
  { "rtx.wet.puddleOcclusion",           "Rain Shadow Weight",             0, 1.00f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.puddleRoughness",           "Water Roughness",                0, 0.025f, 0.0f, 1.0f,  "%.3f" },
  { "rtx.wet.puddleDarkening",           "Water Darkening",                0, 0.15f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.shoreDarkening",            "Shore Darkening",                0, 0.50f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.rippleStrength",            "Ripple Strength",                0, 0.50f, 0.0f, 1.0f,  "%.3f" },
  { "rtx.wet.rippleSpeed",               "Ripple Speed",                   0, 1.00f, 0.0f, 10.0f, "%.2f" },
  { "rtx.wet.rippleVariation",           "Ripple Gusts",                   0, 0.30f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.minRoughness",              "Roughness Floor",                0, 0.05f, 0.0f, 1.0f,  "%.3f" },
  { "rtx.wet.albedoDarkening",           "Albedo Darkening",               0, 0.50f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.saturationBoost",           "Saturation Boost",               0, 0.50f, 0.0f, 2.0f,  "%.2f" },
  { "rtx.wet.rainShadow",                "Rain Shadow",                    1, 1.00f, 0.0f, 0.0f,  nullptr },
  { "rtx.wet.puddleQuality",             "Puddle Quality (0 off, 1 noise, 2 rain map)", 2, 2.0f, 0.0f, 2.0f, "%d", 1 },
  { "rtx.wet.rainShadowDither",          "Rain Shadow Dither (quality 1)", 1, 1.00f, 0.0f, 0.0f,  nullptr, 1 },
  { "rtx.wet.mapExtent",                 "Rain Map Extent",                0, 128.0f, 8.0f, 1024.0f, "%.0f", 1 },
  { "rtx.wet.mapResolution",             "Rain Map Resolution",            2, 512.0f, 64.0f, 2048.0f, "%d", 1 },
  { "rtx.wet.mapUpdateDistance",         "Rain Map Update Distance (0 = every frame)", 0, 8.0f, 0.0f, 32.0f, "%.1f", 1 },
  { "rtx.wet.mapTraceAbove",             "Rain Map Trace Above Camera",    0, 100.0f, 1.0f, 1000.0f, "%.0f", 1 },
  { "rtx.wet.mapTraceBelow",             "Rain Map Trace Below Camera",    0, 60.0f, 1.0f, 1000.0f, "%.0f", 1 },
  { "rtx.wet.occlusionMinHeight",        "Rain Shadow Min Height",         0, 2.5f, 0.0f, 50.0f,  "%.1f", 1 },
  { "rtx.wet.occlusionFeather",          "Rain Shadow Feather",            0, 1.5f, 0.0f, 10.0f,  "%.2f", 1 },
  { "rtx.wet.occlusionRayLength",        "Rain Shadow Ray Length (quality 1)",   0, 80.0f, 1.0f, 1000.0f, "%.0f", 1 },
  { "rtx.wet.occlusionDistance",         "Rain Shadow Ray Distance (quality 1)", 0, 120.0f, 0.0f, 2000.0f, "%.0f", 1 },
  { "rtx.wet.detailDistance",            "Puddle Distance",                0, 850.0f, 0.0f, 5000.0f, "%.0f", 1 },
  { "rtx.wet.raindropDistance",          "Raindrop Distance",              0, 80.1f, 0.0f, 500.0f, "%.1f", 1 },
  { "rtx.wet.microAmplitude",            "Ground Unevenness",              0, 0.033f, 0.001f, 1.0f, "%.3f", 1 },
  { "rtx.wet.noiseScale",                "Unevenness Cell Size",           0, 1.00f, 0.1f, 100.0f, "%.2f", 1 },
  { "rtx.wet.edgeWidth",                 "Edge Depth",                     0, 0.025f, 0.0001f, 0.1f, "%.4f", 1 },
  { "rtx.wet.shoreWidth",                "Shore Height",                   0, 0.030f, 0.0f, 0.5f,  "%.3f", 1 },
  { "rtx.wet.rippleWavelength",          "Ripple Wavelength (cm)",         0, 8.8f, 1.0f, 200.0f, "%.1f", 1 },
};

// Remix defaults of the simple options (gen_remix_defaults.py from the runtime's RtxOptions.md), sorted by key.
struct RxDefault { const char* key; const char* value; };
const RxDefault kRxDefaults[] = {
#include "remix_defaults.inc"
};

const char* remix_default(const char* key) {
  size_t lo = 0, hi = sizeof(kRxDefaults) / sizeof(kRxDefaults[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = std::strcmp(kRxDefaults[mid].key, key);
    if (c == 0) return kRxDefaults[mid].value;
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return nullptr;
}

// ---- helpers -------------------------------------------------------------------------------------------------

void set_status(const char* text) {
  std::snprintf(g_status, sizeof(g_status), "%s", text);
}

// Sends one option to Remix. Bools go as "True"/"False" (what the proxy's remix_set expects).
bool send_option(const char* key, int type, float v) {
  if (!g_cb.remix_set) return false;
  char buf[32];
  if (type == 1) std::snprintf(buf, sizeof(buf), "%s", v >= 0.5f ? "True" : "False");
  else if (type == 2) std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(v + (v < 0 ? -0.5f : 0.5f)));
  else std::snprintf(buf, sizeof(buf), "%.6f", v);
  return g_cb.remix_set(key, buf) != 0;
}

// The rain driver owns these. A moved slider holds for this session; they are never saved, so a stage's conditions apply.
bool wet_driver_owned(const char* key) {
  return !std::strcmp(key, "rtx.wet.globalWetness") || !std::strcmp(key, "rtx.wet.raindrops") || !std::strcmp(key, "rtx.wet.rainIntensity");
}

const WetOpt* find_wet(const char* key) {
  for (const WetOpt& o : kWet) if (std::strcmp(o.key, key) == 0) return &o;
  return nullptr;
}

// Folder for menu.ini: the proxy's callback, else the folder of the loaded d3d9.dll.
std::wstring config_file_path() {
  wchar_t dir[MAX_PATH * 2] = {};
  if (g_cb.config_dir) g_cb.config_dir(dir, sizeof(dir) / sizeof(dir[0]));
  if (!dir[0]) {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&rbrrtx_menu_init), &self)) {
      GetModuleFileNameW(self, dir, static_cast<DWORD>(sizeof(dir) / sizeof(dir[0])));
      if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = L'\0';
    }
  }
  std::wstring path(dir);
  if (!path.empty() && path.back() != L'\\') path += L'\\';
  path += L"menu.ini";
  return path;
}

std::string narrow_path(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
  return s;
}

std::string trim(const std::string& v) {
  size_t a = 0, b = v.size();
  while (a < b && (v[a] == ' ' || v[a] == '\t' || v[a] == '\r')) ++a;
  while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t' || v[b - 1] == '\r')) --b;
  return v.substr(a, b - a);
}

// Reads menu.ini: [wetness] key=value (queued for Remix), [textures] key=value (handed to the proxy).
// Unknown keys are ignored.
void load_ini() {
  std::ifstream in(config_file_path());
  if (!in) return;
  std::string raw, section;
  while (std::getline(in, raw)) {
    const std::string line = trim(raw);
    if (line.empty() || line[0] == ';') continue;
    if (line[0] == '[') {
      const size_t close = line.find(']');
      section = close == std::string::npos ? std::string() : line.substr(1, close - 1);
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
    if (section != "wetness" && section != "textures" && section != "remix") {
      if (g_cb.ini_extra_set) g_cb.ini_extra_set(section.c_str(), key.c_str(), val.c_str());
    } else if (section == "remix") {
      if (!key.empty()) (kRemixPins ? g_pins : g_pins_parked)[key] = val;
    } else if (section == "textures") {
      if (g_cb.texture_ini) g_cb.texture_ini(key.c_str(), val.c_str());
    } else if (const WetOpt* o = find_wet(key.c_str())) {
      if (wet_driver_owned(key.c_str())) continue;
      if (o->type == 1) g_touched[key] = (val == "True" || val == "1") ? 1.0f : 0.0f;
      else g_touched[key] = static_cast<float>(std::atof(val.c_str()));
    }
  }
  g_saved = g_touched;
  // rbrrtx_menu_set_callbacks runs before this load, when nothing was loaded yet: without this the saved values waited
  // for the first slider move, and rtx.conf's ones showed until then (user 2026-11-05, Chirdonhead).
  g_pending_apply = !g_touched.empty();
  g_pins_pending = !g_pins.empty();
}

int g_emitted = 0;   // lines the proxy wrote in the current save (texture tags, ...)
int g_saved_items = 0;
void emit_line(const char* line, void* user) {
  if (line && line[0]) { *static_cast<std::ofstream*>(user) << line << '\n'; ++g_emitted; }
}
void emit_raw(const char* line, void* user) {
  if (line) *static_cast<std::ofstream*>(user) << line << '\n';
}

// Writes menu.ini: [wetness] = g_saved, [textures] = the proxy's lines.
bool save_ini() {
  // Written beside the file, then swapped in: a crash or a full disk leaves the old menu.ini whole.
  const std::wstring path = config_file_path(), tmp = path + L".tmp";
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::ofstream out(tmp, std::ios::trunc);
  if (!out) {
    if (g_cb.log) { char b[400]; std::snprintf(b, sizeof(b), "save FAILED: cannot create %s", narrow_path(tmp).c_str()); g_cb.log(b); }
    return false;
  }
  out << "; RBR-RTX menu settings (written by the F3 menu; the game's user.conf is never touched)\n[wetness]\n";
  for (const auto& kv : g_saved) if (!wet_driver_owned(kv.first.c_str())) out << kv.first << '=' << kv.second << '\n';
  out << "\n[remix]\n";
  for (const auto& kv : g_pins) out << kv.first << '=' << kv.second << '\n';
  for (const auto& kv : g_pins_parked) out << kv.first << '=' << kv.second << '\n';
  out << "\n[textures]\n";
  g_emitted = 0;
  if (g_cb.texture_ini_dump) g_cb.texture_ini_dump(emit_line, &out);
  int extra = 0;
  out << "\n";
  if (g_cb.ini_extra_dump) extra = g_cb.ini_extra_dump(emit_raw, &out);
  g_saved_items = static_cast<int>(g_saved.size() + g_pins.size()) + g_emitted + extra;
  out.close();
  if (out.fail() || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    if (g_cb.log) { char b[400]; std::snprintf(b, sizeof(b), "save FAILED: cannot replace %s (error %lu)", narrow_path(path).c_str(), GetLastError()); g_cb.log(b); }
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

// Pins go to Remix once it can take them (the proxy's remix_pin_send says so), logged one line per pin.
void apply_pins() {
  if (!g_pins_pending || !g_cb.remix_pin_send) return;
  unsigned n = 0, failed = 0;
  for (const auto& kv : g_pins) {
    if (g_cb.remix_pin_send(kv.first.c_str(), kv.second.c_str())) {
      ++n;
      if (g_cb.log) { char b[300]; std::snprintf(b, sizeof(b), "pin applied: %s = %s", kv.first.c_str(), kv.second.c_str()); g_cb.log(b); }
    } else {
      ++failed;
    }
  }
  if (failed) return;                                    // Remix not reachable yet: all again on a later tick
  g_pins_pending = false;
  char b[120];
  std::snprintf(b, sizeof(b), "%u pinned Remix option(s) applied", n);
  if (g_cb.log) g_cb.log(b);
  set_status(b);
}

void apply_pending() {
  apply_pins();
  if (!g_pending_apply || !g_cb.remix_set) return;
  // Saved values only: a value never saved stays as rtx.conf has it. Not reachable yet: all again on a later tick.
  for (const WetOpt& o : kWet) {
    const auto it = g_touched.find(o.key);
    if (it == g_touched.end()) continue;
    if (!send_option(o.key, o.type, it->second)) return;
  }
  g_pending_apply = false;
}

std::atomic<int> g_di_wheel{ 0 };                         // DirectInput wheel travel since the last menu frame
ULONGLONG g_wm_wheel_tick = 0;                            // last WM_MOUSEWHEEL: the window messages are working, the DirectInput travel is not used

// While open the menu draws its own cursor (the game may hide or confine the system one) and lifts a cursor clip.
void cursor_for_menu(bool open) {
  if (!g_imgui_ready) return;
  ImGui::GetIO().MouseDrawCursor = open;
  if (open) ClipCursor(nullptr);
}

// ---- built-in pages -------------------------------------------------------------------------------------------

void page_info(void*) {
  char buf[1024] = "";
  if (g_cb.info_text) g_cb.info_text(buf, sizeof(buf));
  if (buf[0]) ImGui::TextUnformatted(buf);
  else ImGui::TextDisabled("(no info callback)");
  ImGui::Separator();
  ImGui::TextUnformatted("Keys (rbr-rtx\\config\\keys.ini)");
  if (g_toggle_vk) ImGui::TextUnformatted("menu: open / close this menu (while it is open the game window gets no keyboard or mouse)");
  ImGui::TextUnformatted("Alt+X: Remix settings (unchanged; both menus can be open)");
  if (g_cb.key_line) {
    char line[256];
    for (int i = 0; i < 64 && g_cb.key_line(i, line, sizeof(line)); ++i) ImGui::TextUnformatted(line);
  }
}

void page_wetness(void*) {
  ImGui::TextWrapped("Wet roads, puddles and raindrops (Remix Plus rtx.wet.*). Save writes moved values to menu.ini. "
                     "Global Wetness, Raindrops and Rain Intensity follow the stage: moving one holds it until the "
                     "stage ends or you press Release locks, and they are not saved.");
  ImGui::Spacing();
  if (!g_cb.remix_set) ImGui::TextDisabled("No remix_set callback: sliders show, nothing is sent.");
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1 && !ImGui::CollapsingHeader("Advanced (rain map, shadows, distances, ground detail)")) break;
    for (const WetOpt& o : kWet) {
      if (o.adv != pass) continue;
      const auto it = g_touched.find(o.key);
      float v = it != g_touched.end() ? it->second : o.def;
      if (it == g_touched.end() && wet_driver_owned(o.key)) {
        if (!std::strcmp(o.key, "rtx.wet.globalWetness")) v = rbrrtx_wetness_now();
        else if (!std::strcmp(o.key, "rtx.wet.rainIntensity")) v = rbrrtx_wetness_rain_now();
        else v = rbrrtx_wetness_rain_now() > 0.01f ? 1.0f : 0.0f;
      }
      bool changed = false;
      if (o.type == 1) {
        bool b = v >= 0.5f;
        if (ImGui::Checkbox(o.label, &b)) { v = b ? 1.0f : 0.0f; changed = true; }
      } else if (o.type == 2) {
        int iv = static_cast<int>(v + 0.5f);
        if (ImGui::SliderInt(o.label, &iv, static_cast<int>(o.lo), static_cast<int>(o.hi), o.fmt)) { v = static_cast<float>(iv); changed = true; }
      } else {
        if (ImGui::SliderFloat(o.label, &v, o.lo, o.hi, o.fmt)) changed = true;
      }
      if (changed) {
        g_touched[o.key] = v;
        send_option(o.key, o.type, v);
      }
    }
  }
  ImGui::Separator();
  if (ImGui::Button("Release locks (Remix keeps the last values)")) {
    // Drops the locks; the proxy's own driver takes over again. Remix keeps the last values it was sent.
    g_touched.clear();
    set_status("locks cleared; the automatic wetness driver is back in control");
  }
}

// One texture of the picked draw: thumbnail, hash, current categories.
void pick_texture_row(const rbrrtx_menu_pick& pick, int s) {
  ImGui::PushID(s);
  const float side = ImGui::GetTextLineHeight() * 5.0f;
  if (pick.texture[s]) {
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(pick.texture[s])), ImVec2(side, side));
    if (s == 0 && pick.has_island) {                 // the island around the hit point: the part of the texture a lamp mark would take
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      const ImVec2 p0(a.x + (b.x - a.x) * pick.uv_rect[0], a.y + (b.y - a.y) * pick.uv_rect[1]);
      const ImVec2 p1(a.x + (b.x - a.x) * pick.uv_rect[2], a.y + (b.y - a.y) * pick.uv_rect[3]);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(p0, p1, IM_COL32(255, 220, 0, 70));
      dl->AddRect(p0, p1, IM_COL32(255, 220, 0, 255), 0.0f, 0, 2.0f);
    }
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  if (ImGui::RadioButton(s == 0 ? "t0 (base texture)" : "t1 (second texture)", g_tex_target == s)) g_tex_target = s;
  if (pick.hash[s]) {
    ImGui::Text("hash 0x%016llX", pick.hash[s]);
    char cls[192] = "";
    if (g_cb.texture_classes) g_cb.texture_classes(pick.hash[s], cls, sizeof(cls));
    ImGui::TextWrapped("Remix categories: %s", cls[0] ? cls : "none");
  } else {
    ImGui::TextDisabled("no texture bound / hash unknown");
  }
  ImGui::EndGroup();
  ImGui::PopID();
}

// Lamps: mark the current hit (the highlighted island of the thumbnail) and the list of marked lamps with Remove.
// A mark needs a vertex-shader draw that is not foliage; every lamp drawn with that part of the texture gets a light
// (lights.ini [auto], lights show at dusk and night).
void lamp_section(const rbrrtx_menu_pick* pick) {
  ImGui::Separator();
  ImGui::TextUnformatted("Lamps");
  if (pick) {
    const bool can = g_cb.lamp_mark && pick->has_island && !pick->foliage;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Mark as lamp")) {
      char msg[160] = "";
      const int r = g_cb.lamp_mark(msg, sizeof(msg));
      std::snprintf(g_lamp_msg, sizeof(g_lamp_msg), r ? "lamp marked: %s" : "lamp mark failed: %s", msg);
      set_status(g_lamp_msg);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (pick->foliage) ImGui::TextDisabled("foliage card: tick \"Skip foliage\" and aim at the lamp");
    else if (!pick->has_island) ImGui::TextDisabled("no UV island at the hit");
    else ImGui::TextDisabled("island: %u vertices, uv %.3f %.3f - %.3f %.3f (yellow box)", pick->island_verts, pick->uv_rect[0], pick->uv_rect[1], pick->uv_rect[2], pick->uv_rect[3]);
  }
  if (!g_cb.lamp_list) return;
  char label[320];
  int n = 0;
  for (int i = 0; i < 200 && g_cb.lamp_list(i, label, sizeof(label)); ++i, ++n) {
    ImGui::PushID(1000 + i);
    if (ImGui::SmallButton("Remove") && g_cb.lamp_remove) {
      std::snprintf(g_lamp_msg, sizeof(g_lamp_msg), g_cb.lamp_remove(i) ? "lamp mark removed" : "could not remove the mark");
      set_status(g_lamp_msg);
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
  }
  if (!n) ImGui::TextDisabled("no lamps marked (lights-marked.ini)");
}

// Texture picker v2: the proxy casts the screen-centre ray against the triangles of the world draws (the lamp
// marker's machinery) and reports the draw under the crosshair. Freeze keeps the last hit so the mouse can reach the
// buttons; a tag button toggles the hash in the proxy's list for that class, applies it live and saves menu.ini.
void page_textures(void*) {
  ImGui::TextWrapped("Aim the middle of the screen at a surface (the game keeps running behind this window). "
                     "The draw under that point is shown live; Freeze keeps it. Choose t0 or t1, then tag it: "
                     "a tag toggles, Clear removes the hash from all four lists. Tags apply at once and are saved "
                     "in menu.ini [textures]. Categories come from rtx.conf and the lists of this session; changes "
                     "made in Alt+X are not shown.");
  ImGui::Checkbox("Freeze (keep the last hit)", &g_tex_freeze);
  ImGui::SameLine();
  ImGui::Checkbox("Skip foliage (look through trees)", &g_tex_skip_foliage);
  rbrrtx_menu_pick pick = {};
  pick.skip_foliage = g_tex_skip_foliage ? 1 : 0;
  if (!g_cb.probe_centre) {
    ImGui::TextDisabled("No probe_centre callback.");
    return;
  }
  g_cb.probe_centre(&pick, g_tex_freeze ? 0 : 1);
  if (!pick.found) {
    ImGui::TextDisabled("nothing found under the centre (world draws incl. car, foliage and fixed-function scenery; not the sky, HUD or particles)");
    lamp_section(nullptr);
    return;
  }
  ImGui::Text("%s", pick.kind[0] ? pick.kind : "draw");
  ImGui::Text("shader %s, %.1f m away", pick.shader, pick.distance);
  pick_texture_row(pick, 0);
  pick_texture_row(pick, 1);
  unsigned long long target = pick.hash[g_tex_target];
  if (!target) target = pick.hash[1 - g_tex_target];
  ImGui::Separator();
  // What each flag is for (docs/research/flags-dive-2026-11-02.md), with a check of the picked draw against the Terrain rule.
  ImGui::TextUnformatted("Flags");
  const bool flat = pick.up_share >= 0.6f;
  if (pick.up_share >= 0.0f) {
    if (flat) ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "this draw: %.0f %% of its area faces up: ground, Terrain is fine", pick.up_share * 100.0f);
    else ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "this draw: only %.0f %% of its area faces up (wall, roof, slope): NOT Terrain", pick.up_share * 100.0f);
  }
  ImGui::BulletText("Terrain: only flat, up-facing ground (road, grass, gravel). Baked top-down: walls and roofs turn black or white.");
  ImGui::BulletText("Decal: thin overlays on a surface: road markings, kerbs, cracks, dirt. Needs an opaque surface under it.");
  ImGui::BulletText("Water: full water surfaces only (animated water), not wet roads or puddles.");
  ImGui::BulletText("Ignore: not drawn at all (flare strips, fake shadows, HUD sprites). Clear: back to the normal material.");
  ImGui::BeginDisabled(target == 0 || !g_cb.mark_texture);
  static const char* const kBtn[5] = { "Terrain", "Decal", "Water", "Ignore", "Clear" };
  static const char* const kTip[5] = { "Flat up-facing ground only (baked from above).", "Markings, kerbs, cracks: a thin layer on a floor.",
                                       "Full water surfaces only.", "Never drawn (flares, fake shadows).", "Remove the hash from all four lists." };
  for (int c = 0; c < 5; ++c) {
    if (c) ImGui::SameLine();
    const bool pressed = ImGui::Button(kBtn[c]);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", kTip[c]);
    if (pressed) {
      if (c == 0 && pick.up_share >= 0.0f && !flat) set_status("warning: this draw is mostly not up-facing; Terrain may turn it black or white");
      const int r = g_cb.mark_texture(target, c);
      if (r) {
        std::snprintf(g_status, sizeof(g_status), c == 4 ? "0x%016llX cleared from all lists" : r == 1 ? "0x%016llX tagged %s" : "0x%016llX untagged %s",
                      target, kBtn[c]);
        if (!save_ini()) set_status("tag applied, but menu.ini could not be written");
      } else {
        std::snprintf(g_status, sizeof(g_status), "tag failed for 0x%016llX (%s)", target, kBtn[c]);
      }
    }
  }
  ImGui::EndDisabled();
  lamp_section(&pick);
}

// Debug page: the proxy's A/B switches (ground layer mode, facade fix, flare and sun-outline hides). Each acts at once
// (the ground layer mode at the next stage load, which the label says) and is logged by the proxy.
void page_debug(void*) {
  if (!g_cb.switch_get || !g_cb.switch_set) {
    ImGui::TextDisabled("No switch callbacks.");
    return;
  }
  char group[32] = "";
  rbrrtx_menu_switch sw = {};
  for (int i = 0; i < 64 && g_cb.switch_get(i, &sw); ++i) {
    if (std::strcmp(group, sw.group) != 0) {
      std::snprintf(group, sizeof(group), "%s", sw.group);
      ImGui::SeparatorText(group);
    }
    ImGui::PushID(i);
    if (sw.options[0]) {
      ImGui::TextUnformatted(sw.label);
      const std::string opts = sw.options;
      int idx = 0;
      for (size_t a = 0; a <= opts.size();) {
        size_t b = opts.find('|', a);
        if (b == std::string::npos) b = opts.size();
        const std::string name = opts.substr(a, b - a);
        if (idx) ImGui::SameLine();
        if (ImGui::RadioButton(name.c_str(), sw.value == idx)) g_cb.switch_set(i, idx);
        ++idx;
        a = b + 1;
      }
    } else {
      bool on = sw.value != 0;
      if (ImGui::Checkbox(sw.label, &on)) g_cb.switch_set(i, on ? 1 : 0);
    }
    if (sw.note[0]) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("%s", sw.note);
      ImGui::PopStyleColor();
    }
    ImGui::PopID();
    ImGui::Spacing();
  }
}

// Remix settings: every option the shipped rtx.conf (and the user's user.conf) sets, with Remix's own default. A pin keeps
// a value: it goes to Remix at the start after rtx.conf and wins over everything, the proxy's own sets included. (Alt+X
// Save leaves out values equal to Remix's defaults, so a value set back to the default is lost to rtx.conf: pin it here.)
void page_remix(void*) {
  if (!g_cb.remix_opt) {
    ImGui::TextDisabled("No option list callback.");
    return;
  }
  ImGui::TextWrapped("Options rtx.conf sets. Type a value and press Pin to keep it: it is sent to Remix now, saved in menu.ini "
                     "[remix], applied at every start after rtx.conf, and the proxy stops setting that option itself. Unpin removes it "
                     "(Remix keeps the value until restart). Green = pinned.");
  static char filter[64] = "";
  static char new_key[128] = "", new_val[128] = "";
  static std::map<std::string, std::string> edit;
  ImGui::SetNextItemWidth(220.0f);
  ImGui::InputText("filter", filter, sizeof(filter));
  ImGui::SameLine();
  ImGui::TextDisabled("%zu pinned", g_pins.size());

  auto pin = [&](const std::string& key, const std::string& value) {
    if (value.empty()) { set_status("empty value: nothing pinned"); return; }
    g_pins[key] = value;
    g_pins_pending = true;
    apply_pins();
    set_status(save_ini() ? "pinned and saved in menu.ini [remix]" : "pinned, but menu.ini could not be written");
  };

  ImGui::SeparatorText("Pin any option");
  ImGui::SetNextItemWidth(300.0f);
  ImGui::InputText("option", new_key, sizeof(new_key));
  ImGui::SetNextItemWidth(160.0f);
  ImGui::InputText("value", new_val, sizeof(new_val));
  ImGui::SameLine();
  if (ImGui::Button("Fill current")) {                   // user.conf over rtx.conf over Remix's default
    char ours[160] = "", user[160] = "";
    if (g_cb.remix_opt_lookup) g_cb.remix_opt_lookup(new_key, ours, user, sizeof(ours));
    const char* d = remix_default(new_key);
    std::snprintf(new_val, sizeof(new_val), "%s", user[0] ? user : ours[0] ? ours : d ? d : "");
  }
  ImGui::SameLine();
  if (ImGui::Button("Pin")) { if (new_key[0]) pin(new_key, new_val); }
  if (new_key[0]) {
    const char* d = remix_default(new_key);
    ImGui::TextDisabled("Remix default: %s", d ? d : "(unknown option or a list)");
  }

  ImGui::SeparatorText("Options in rtx.conf / user.conf");
  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
  if (ImGui::BeginTable("rx", 5, tf, ImVec2(0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.5f))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("option", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("ours", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Remix default", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("user.conf", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("value / pin", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();
    char key[160], ours[160], user[160];
    for (int i = 0; i < 2000 && g_cb.remix_opt(i, key, sizeof(key), ours, user, sizeof(ours)); ++i) {
      if (filter[0] && !std::strstr(key, filter)) continue;
      const auto pit = g_pins.find(key);
      const bool pinned = pit != g_pins.end();
      const char* d = remix_default(key);
      ImGui::TableNextRow();
      ImGui::PushID(i);
      ImGui::TableSetColumnIndex(0);
      if (pinned) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
      ImGui::TextUnformatted(key);
      if (pinned) ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(ours[0] ? ours : "-");
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(d ? d : "?");
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(user[0] ? user : "-");
      ImGui::TableSetColumnIndex(4);
      std::string& ev = edit[key];
      if (ev.empty()) ev = pinned ? pit->second : (user[0] ? user : ours);
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", ev.c_str());
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Unpin").x - 30.0f);
      if (ImGui::InputText("##v", buf, sizeof(buf))) ev = buf;
      ImGui::SameLine();
      if (ImGui::SmallButton("Pin")) pin(key, ev);
      if (pinned) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Unpin")) {
          g_pins.erase(key);
          set_status(save_ini() ? "unpinned (Remix keeps the value until restart)" : "unpinned, menu.ini could not be written");
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  // Pins of options that are in neither file (typed in): listed so they can be removed.
  for (auto it = g_pins.begin(); it != g_pins.end();) {
    char ours[160] = "", user[160] = "";
    if (g_cb.remix_opt_lookup && g_cb.remix_opt_lookup(it->first.c_str(), ours, user, sizeof(ours))) { ++it; continue; }
    ImGui::PushID(it->first.c_str());
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s = %s (typed in)", it->first.c_str(), it->second.c_str());
    ImGui::SameLine();
    const bool rm = ImGui::SmallButton("Unpin");
    ImGui::PopID();
    if (rm) { it = g_pins.erase(it); save_ini(); } else ++it;
  }
}

void page_save(void*) {
  ImGui::TextWrapped("Writes the Wetness values to menu.ini in the RBR-RTX config folder (the texture tags are saved "
                     "the moment you tag). user.conf is never touched.");
  if (ImGui::Button("Save menu.ini")) {
    g_saved = g_touched;
    if (save_ini()) std::snprintf(g_status, sizeof(g_status), "saved %d items to %s", g_saved_items, narrow_path(config_file_path()).c_str());
    else set_status("could not write menu.ini");
    if (g_cb.log) g_cb.log(g_status);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu wetness value(s) locked", g_touched.size());
}

void ensure_builtin_pages() {
  if (g_builtins_added) return;
  g_builtins_added = true;
  rbrrtx_menu_add_page("Info", page_info, nullptr);
  rbrrtx_menu_add_page("Wetness", page_wetness, nullptr);
  rbrrtx_menu_add_page("Textures", page_textures, nullptr);
  rbrrtx_menu_register_custom_pages();
  if (kRemixPins) rbrrtx_menu_add_page("Remix settings", page_remix, nullptr);
  rbrrtx_menu_add_page("Debug", page_debug, nullptr);
  rbrrtx_menu_add_page("Save", page_save, nullptr);
}

// Draws the window: one tab per registered page, status line at the bottom.
void draw_window() {
  ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("RBR-RTX menu")) {
    if (ImGui::BeginTabBar("rbrrtx_pages")) {
      for (const Page& p : g_pages) {
        if (ImGui::BeginTabItem(p.name)) {
          if (p.draw) p.draw(p.user);
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Save")) {
      g_saved = g_touched;
      if (save_ini()) {
        const std::string p = narrow_path(config_file_path());
        std::snprintf(g_status, sizeof(g_status), "saved %d items to %s", g_saved_items, p.c_str());
        if (g_cb.log) g_cb.log(g_status);
      } else {
        set_status("could not write menu.ini");
        if (g_cb.log) g_cb.log("save FAILED: menu.ini could not be written");
      }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(g_status);
  }
  ImGui::End();
}

// Mouse, keyboard and text messages the open menu takes. Everything else (focus, size, paint, timers) goes on to the game.
// Alt combinations (WM_SYSKEY*) are not taken, so Alt+X still opens the Remix panel.
bool is_input_message(UINT msg) {
  switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_DEADCHAR:
    case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
      return true;
    default:
      return false;
  }
}

} // namespace

// ---- public interface -----------------------------------------------------------------------------------------

extern "C" {

int rbrrtx_menu_init(HWND game_window, IDirect3DDevice9* dev) {
  if (g_imgui_ready) {
    if (dev && dev != g_dev) rbrrtx_menu_device_reset(dev);   // another device object (re-created): backend starts over
    return 1;
  }
  if (!game_window) return 0;
  ensure_builtin_pages();

  IMGUI_CHECKVERSION();
  if (!ImGui::CreateContext()) return 0;
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;     // never write imgui.ini into the game folder
  io.LogFilename = nullptr;
  ImGui::StyleColorsDark();

  RECT rc = {};
  GetClientRect(game_window, &rc);
  const float h = static_cast<float>(rc.bottom - rc.top);
  io.FontGlobalScale = std::clamp(h / 900.0f, 1.0f, 2.0f);   // default ImGui font is small on large screens

  if (!ImGui_ImplWin32_Init(game_window)) {
    ImGui::DestroyContext();
    return 0;
  }
  g_wnd = game_window;
  g_dev = dev;
  g_imgui_ready = true;
  load_ini();
  return 1;
}

int rbrrtx_menu_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!g_imgui_ready) return 0;

  // The toggle key (F3): on the key-down edge (auto-repeat ignored). Its key-up is swallowed in both states.
  if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && g_toggle_vk && static_cast<int>(wp) == g_toggle_vk) {
    if (msg == WM_KEYDOWN && !(lp & (1 << 30))) rbrrtx_menu_set_open(!g_open);
    return 1;
  }
  if (!g_open) return 0;
  if (msg == WM_MOUSEWHEEL) g_wm_wheel_tick = GetTickCount64();

  ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp);   // feeds ImGui's io (mouse position, keys, text)
  return is_input_message(msg) ? 1 : 0;
}

void rbrrtx_menu_set_toggle_vk(int vk) { g_toggle_vk = vk > 0 && vk < 256 ? vk : 0; }

void rbrrtx_menu_tick(void) {
  if (g_imgui_ready) apply_pending();   // once Remix options are reachable
}

void rbrrtx_menu_frame(IDirect3DDevice9* dev) {
  if (!g_imgui_ready || !dev || !g_open) return;

  if (!g_dx9_ready) {
    // First frame after init or after a device change: the backend creates its objects now, draws next frame.
    if (g_dev != dev) g_dev = dev;
    if (ImGui_ImplDX9_Init(dev)) g_dx9_ready = true;
    return;
  }

  // Caller is inside BeginScene/EndScene (the proxy opens a pair around this call in front of Present).
  DWORD srgb_samp = 0, srgb_write = 0;
  if (g_srgb_fix) {
    dev->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgb_samp);
    dev->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb_write);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 1);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, 1);
  }

  ClipCursor(nullptr);                 // the game may confine the cursor again at any time
  const int wheel = g_di_wheel.exchange(0);
  if (wheel && GetTickCount64() - g_wm_wheel_tick > 1000) ImGui::GetIO().AddMouseWheelEvent(0.0f, static_cast<float>(wheel) / WHEEL_DELTA);

  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  draw_window();
  ImGui::EndFrame();
  ImGui::Render();

  g_drawing = true;
  ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
  g_drawing = false;

  if (g_srgb_fix) {
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, srgb_samp);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb_write);
  }
}

void rbrrtx_menu_device_lost(void) {
  if (g_dx9_ready) ImGui_ImplDX9_InvalidateDeviceObjects();   // releases D3DPOOL_DEFAULT objects before Reset
}

void rbrrtx_menu_device_reset(IDirect3DDevice9* dev) {
  if (!g_imgui_ready) return;
  if (dev != g_dev) {
    // Different device object: start the DX9 backend over on the next frame.
    if (g_dx9_ready) { g_dx9_ready = false; ImGui_ImplDX9_Shutdown(); }
    g_dev = dev;
    return;
  }
  if (g_dx9_ready) ImGui_ImplDX9_CreateDeviceObjects();
}

int rbrrtx_menu_device_refs(void) { return g_dx9_ready ? 1 : 0; }

void rbrrtx_menu_release_device(void) {
  if (!g_dx9_ready) return;
  g_dx9_ready = false;                   // first: the Release it makes re-enters the proxy's Release hook
  ImGui_ImplDX9_Shutdown();
}

void* rbrrtx_menu_font_texture(void) {
  if (!g_dx9_ready) return nullptr;
  return reinterpret_cast<void*>(static_cast<uintptr_t>(ImGui::GetIO().Fonts->TexID));
}

void rbrrtx_menu_shutdown(void) {
  if (!g_imgui_ready) return;
  cursor_for_menu(false);
  if (g_dx9_ready) { g_dx9_ready = false; ImGui_ImplDX9_Shutdown(); }
  ImGui_ImplWin32_Shutdown();
  // No DestroyContext: on the game's own FreeLibrary at quit (run 20261105-211941) ImGui::Shutdown read freed window
  // memory (access violation in SetWindowActiveForSkipRefresh, swallowed by the loader). The context is left to the
  // unload; nothing else holds it.
  ImGui::SetCurrentContext(nullptr);
  g_imgui_ready = false;
  g_open = false;
  g_dev = nullptr;
  g_wnd = nullptr;
}

int rbrrtx_menu_is_open(void) { return g_open ? 1 : 0; }

void rbrrtx_menu_add_wheel(int delta) { g_di_wheel += delta; }

void rbrrtx_menu_set_open(int open) {
  g_open = open != 0;
  cursor_for_menu(g_open);
  if (g_open) set_status("menu open");
}

int rbrrtx_menu_is_drawing(void) { return g_drawing ? 1 : 0; }

int rbrrtx_menu_option_locked(const char* key, float* value) {
  if (!key) return 0;
  const auto it = g_touched.find(key);
  if (it == g_touched.end()) return 0;
  if (value) *value = it->second;
  return 1;
}

// A new stage has its own conditions: slider holds on the three values the stage drives end with the old one.
void rbrrtx_menu_release_stage_locks(void) {
  g_touched.erase("rtx.wet.globalWetness");
  g_touched.erase("rtx.wet.rainIntensity");
  g_touched.erase("rtx.wet.raindrops");
}

int rbrrtx_menu_pinned(const char* key, char* value, size_t value_len) {
  if (g_pins.empty() || !key) return 0;
  const auto it = g_pins.find(key);
  if (it == g_pins.end()) return 0;
  if (value && value_len) std::snprintf(value, value_len, "%s", it->second.c_str());
  return 1;
}

int rbrrtx_menu_probe(rbrrtx_menu_pick* pick, int want_new) {
  return g_cb.probe_centre && pick ? g_cb.probe_centre(pick, want_new) : 0;
}

int rbrrtx_menu_camera(float pos[3], float fwd[3]) { return g_cb.camera ? g_cb.camera(pos, fwd) : 0; }
void rbrrtx_menu_log(const char* line) { if (g_cb.log && line) g_cb.log(line); }
void rbrrtx_menu_set_status(const char* text) { set_status(text); }

void rbrrtx_menu_set_callbacks(const rbrrtx_menu_callbacks* cb) {
  g_cb = cb ? *cb : rbrrtx_menu_callbacks{};
  g_pending_apply = !g_touched.empty();   // a late remix_set still gets the saved values
}

void rbrrtx_menu_add_page(const char* name, rbrrtx_menu_page_fn draw, void* user) {
  if (!name || !draw) return;
  g_pages.push_back(Page{ name, draw, user });
}

} // extern "C"
