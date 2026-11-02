// menu.cpp - F3 in-game menu for the RBR-RTX d3d9 proxy. See menu.hpp and docs/research/f3-menu-plan.md.
//
// Draws with Dear ImGui 1.91.7 (third_party/imgui, MIT) through the game's own IDirect3DDevice9:
// ImGui_ImplDX9 for rendering, ImGui_ImplWin32 for input. Everything runs on the game's render thread.

#include "menu.hpp"

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/backends/imgui_impl_dx9.h"
#include "../third_party/imgui/backends/imgui_impl_win32.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// The win32 backend defines this but its header does not declare it (remix-comp-base does the same).
LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

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
bool g_dx9_ready = false;     // ImGui DX9 backend initialised for g_dev
bool g_open = false;
bool g_drawing = false;
bool g_cursor_shown = false;  // we called ShowCursor(TRUE) on open and undo it on close
bool g_pending_apply = false; // menu.ini values still to send to Remix (waits for remix_set)
bool g_srgb_fix = false;      // remix-comp-base forces SRGB sampler/write around its draws; off until tested

// Values the user set in the menu (or loaded from menu.ini). A key here is "locked": the proxy must not overwrite it.
std::map<std::string, float> g_touched;

char g_status[160] = "";      // one line of feedback at the bottom of the window

// Texture picker state (the probe runs while the page is visible and not frozen).
unsigned long long g_tex_hash = 0;
char g_tex_info[256] = "";
bool g_tex_freeze = false;

// ---- Wetness page: mirrors the rtx.wet.* options of the Remix runtime (rtx_fork_wetness.h) -------------------
// type 0 = float slider (min..max), type 1 = bool checkbox.
struct WetOpt {
  const char* key;
  const char* label;
  int type;
  float def;
  float lo, hi;
  const char* fmt;
};

const WetOpt kWet[] = {
  { "rtx.wet.globalWetness",             "Global Wetness",                 0, 0.00f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.raindrops",                 "Raindrops",                      1, 0.00f, 0.0f, 0.0f,  nullptr },
  { "rtx.wet.rainIntensity",             "Rain Intensity",                 0, 1.00f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.raindropScale",             "Raindrop Scale",                 0, 3.00f, 0.1f, 10.0f, "%.2f" },
  { "rtx.wet.raindropGroundScale",       "Raindrops on Wet Ground",        0, 0.50f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.minNormal",                 "Min Up Normal",                  0, 0.50f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.blendWidth",                "Blend Width",                    0, 0.10f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.fullyWetRoughnessScale",    "Fully Wet Roughness Scale",      0, 0.00f, 0.0f, 2.0f,  "%.2f" },
  { "rtx.wet.patches",                   "Wet Patches",                    1, 1.00f, 0.0f, 0.0f,  nullptr },
  { "rtx.wet.puddles",                   "Puddles",                        1, 1.00f, 0.0f, 0.0f,  nullptr },
  { "rtx.wet.puddleCoverage",            "Puddle Coverage",                0, 0.64f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.depressionWeight",          "Depression Weight",              0, 0.30f, 0.0f, 5.0f,  "%.2f" },
  { "rtx.wet.slopeLimit",                "Slope Limit",                    0, 0.053f, 0.005f, 1.0f, "%.3f" },
  { "rtx.wet.slopeFeather",              "Slope Feather",                  0, 2.40f, 0.1f, 4.0f,  "%.2f" },
  { "rtx.wet.puddleOcclusion",           "Rain Shadow Weight",             0, 0.50f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.puddleRoughness",           "Water Roughness",                0, 0.02f, 0.0f, 1.0f,  "%.3f" },
  { "rtx.wet.puddleDarkening",           "Water Darkening",                0, 0.25f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.shoreDarkening",            "Shore Darkening",                0, 0.80f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.rippleStrength",            "Ripple Strength",                0, 0.08f, 0.0f, 1.0f,  "%.3f" },
  { "rtx.wet.rippleSpeed",               "Ripple Speed",                   0, 2.50f, 0.0f, 10.0f, "%.2f" },
  { "rtx.wet.rippleVariation",           "Ripple Gusts",                   0, 0.48f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.minRoughness",              "Roughness Floor",                0, 0.05f, 0.0f, 1.0f,  "%.3f" },
  { "rtx.wet.albedoDarkening",           "Albedo Darkening",               0, 0.50f, 0.0f, 1.0f,  "%.2f" },
  { "rtx.wet.saturationBoost",           "Saturation Boost",               0, 0.18f, 0.0f, 2.0f,  "%.2f" },
};

const char* const kMenuIni = "menu.ini";

// ---- helpers -------------------------------------------------------------------------------------------------

void set_status(const char* text) {
  std::snprintf(g_status, sizeof(g_status), "%s", text);
}

// Sends one option to Remix. Bools go as "True"/"False" (what the proxy's remix_set expects).
void send_option(const char* key, int type, float v) {
  if (!g_cb.remix_set) return;
  char buf[32];
  if (type == 1) std::snprintf(buf, sizeof(buf), "%s", v >= 0.5f ? "True" : "False");
  else std::snprintf(buf, sizeof(buf), "%.6f", v);
  g_cb.remix_set(key, buf);
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

// Reads [wetness] key=value lines from menu.ini. Unknown keys are ignored. Values are queued for Remix.
void load_ini() {
  std::ifstream in(config_file_path());
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.empty() || line[0] == '[' || line[0] == ';') continue;
    const std::string key = line.substr(0, eq);
    if (const WetOpt* o = find_wet(key.c_str())) {
      const std::string val = line.substr(eq + 1);
      if (o->type == 1) g_touched[key] = (val == "True" || val == "1") ? 1.0f : 0.0f;
      else g_touched[key] = static_cast<float>(std::atof(val.c_str()));
    }
  }
  g_pending_apply = !g_touched.empty();
}

void apply_pending() {
  if (!g_pending_apply || !g_cb.remix_set) return;
  for (const auto& kv : g_touched) {
    if (const WetOpt* o = find_wet(kv.first.c_str())) send_option(o->key, o->type, kv.second);
  }
  g_pending_apply = false;
  set_status("menu.ini values sent to Remix");
}

// Show / hide the mouse cursor for the menu. ShowCursor is a counter: open adds one, close removes one.
void cursor_for_menu(bool open) {
  if (open && !g_cursor_shown) { ShowCursor(TRUE); ClipCursor(nullptr); g_cursor_shown = true; }
  else if (!open && g_cursor_shown) { ShowCursor(FALSE); g_cursor_shown = false; }
}

// ---- built-in pages -------------------------------------------------------------------------------------------

void page_info(void*) {
  char buf[512] = "";
  if (g_cb.info_text) g_cb.info_text(buf, sizeof(buf));
  if (buf[0]) ImGui::TextWrapped("%s", buf);
  else ImGui::TextDisabled("(no info callback)");
  ImGui::Separator();
  ImGui::TextUnformatted("F3   open / close this menu");
  ImGui::TextUnformatted("Alt+X  Remix settings (unchanged; the Remix panel still works)");
}

void page_wetness(void*) {
  ImGui::TextWrapped("Wet roads, puddles and raindrops (Remix Plus rtx.wet.*). Values you move here are kept "
                     "and win over the automatic rain driver until you restart. Save writes them to menu.ini.");
  ImGui::Spacing();
  if (!g_cb.remix_set) ImGui::TextDisabled("No remix_set callback: sliders show, nothing is sent.");
  for (const WetOpt& o : kWet) {
    const auto it = g_touched.find(o.key);
    float v = it != g_touched.end() ? it->second : o.def;
    bool changed = false;
    if (o.type == 1) {
      bool b = v >= 0.5f;
      if (ImGui::Checkbox(o.label, &b)) { v = b ? 1.0f : 0.0f; changed = true; }
    } else {
      if (ImGui::SliderFloat(o.label, &v, o.lo, o.hi, o.fmt)) changed = true;
    }
    if (changed) {
      g_touched[o.key] = v;
      send_option(o.key, o.type, v);
    }
  }
  ImGui::Separator();
  if (ImGui::Button("Release locks (Remix keeps the last values)")) {
    // Drops the locks; the proxy's own driver takes over again. Remix keeps the last values it was sent.
    g_touched.clear();
    set_status("locks cleared; the automatic wetness driver is back in control");
  }
}

// Texture picker: shows the texture under the screen centre of the last primary-target draw, with buttons
// that tag it for Remix. Freeze keeps the last hash so the mouse can reach the buttons.
void page_textures(void*) {
  ImGui::TextWrapped("Aim the centre of the screen at a surface, freeze, then tag its texture. Tagged textures "
                     "go to the matching rtx list (terrain, decal, animated water, ignore) and the proxy keeps them.");
  ImGui::Checkbox("Freeze (keep the last hit)", &g_tex_freeze);
  if (!g_tex_freeze && g_cb.probe_centre) {
    unsigned long long h = 0;
    char info[256] = "";
    if (g_cb.probe_centre(&h, info, sizeof(info))) {
      g_tex_hash = h;
      std::snprintf(g_tex_info, sizeof(g_tex_info), "%s", info);
    } else {
      g_tex_hash = 0;
      g_tex_info[0] = '\0';
    }
  }
  if (!g_cb.probe_centre) ImGui::TextDisabled("No probe_centre callback.");
  if (g_tex_hash) {
    ImGui::Text("hash 0x%016llX", g_tex_hash);
    if (g_tex_info[0]) ImGui::TextWrapped("%s", g_tex_info);
  } else {
    ImGui::TextDisabled("no texture under the centre");
  }
  ImGui::BeginDisabled(g_tex_hash == 0 || !g_cb.mark_texture);
  static const char* const kBtn[4] = { "Terrain", "Decal", "Water", "Ignore" };
  for (int c = 0; c < 4; ++c) {
    if (c) ImGui::SameLine();
    if (ImGui::Button(kBtn[c])) {
      const int ok = g_cb.mark_texture(g_tex_hash, c);
      std::snprintf(g_status, sizeof(g_status), ok ? "tagged 0x%016llX as %s" : "tag failed for 0x%016llX (%s)",
                    g_tex_hash, kBtn[c]);
    }
  }
  ImGui::EndDisabled();
}

void page_save(void*) {
  ImGui::TextWrapped("Writes the Wetness values to menu.ini in the RBR-RTX config folder. user.conf is never touched.");
  if (ImGui::Button("Save menu.ini")) {
    const std::wstring path = config_file_path();
    std::ofstream out(path, std::ios::trunc);
    if (out) {
      out << "; RBR-RTX menu settings (written by the F3 menu)\n[wetness]\n";
      for (const auto& kv : g_touched) out << kv.first << '=' << kv.second << '\n';
      set_status("saved");
    } else {
      set_status("could not write menu.ini");
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu value(s) locked", g_touched.size());
}

void ensure_builtin_pages() {
  if (g_builtins_added) return;
  g_builtins_added = true;
  rbrrtx_menu_add_page("Info", page_info, nullptr);
  rbrrtx_menu_add_page("Wetness", page_wetness, nullptr);
  rbrrtx_menu_add_page("Textures", page_textures, nullptr);
  rbrrtx_menu_add_page("Save", page_save, nullptr);
}

// Draws the window: one tab per registered page, status line at the bottom.
void draw_window() {
  ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("RBR-RTX menu  (F3 closes)")) {
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
    ImGui::TextUnformatted(g_status);
  }
  ImGui::End();
}

// Mouse, keyboard and text messages the open menu takes. Everything else (focus, size, paint, timers) goes on to the game.
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
  if (g_imgui_ready) return 1;
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

  // F3 toggles on the key-down edge (auto-repeat ignored). Its key-up is swallowed in both states.
  if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && wp == VK_F3) {
    if (msg == WM_KEYDOWN && !(lp & (1 << 30))) rbrrtx_menu_set_open(!g_open);
    return 1;
  }
  if (!g_open) return 0;

  ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp);   // feeds ImGui's io (mouse position, keys, text)
  return is_input_message(msg) ? 1 : 0;
}

void rbrrtx_menu_frame(IDirect3DDevice9* dev) {
  if (!g_imgui_ready || !dev) return;
  apply_pending();                       // runs even while closed, once Remix options are reachable
  if (!g_open) return;

  if (!g_dx9_ready) {
    // First frame after init or after a device change: the backend creates its objects now, draws next frame.
    if (g_dev != dev) g_dev = dev;
    if (ImGui_ImplDX9_Init(dev)) g_dx9_ready = true;
    return;
  }

  // Caller is inside BeginScene/EndScene (see plan: hook EndScene, not Present).
  DWORD srgb_samp = 0, srgb_write = 0;
  if (g_srgb_fix) {
    dev->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgb_samp);
    dev->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb_write);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 1);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, 1);
  }

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
    if (g_dx9_ready) ImGui_ImplDX9_Shutdown();
    g_dx9_ready = false;
    g_dev = dev;
    return;
  }
  if (g_dx9_ready) ImGui_ImplDX9_CreateDeviceObjects();
}

void rbrrtx_menu_shutdown(void) {
  if (!g_imgui_ready) return;
  cursor_for_menu(false);
  if (g_dx9_ready) ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_dx9_ready = false;
  g_imgui_ready = false;
  g_open = false;
  g_dev = nullptr;
  g_wnd = nullptr;
}

int rbrrtx_menu_is_open(void) { return g_open ? 1 : 0; }

void rbrrtx_menu_set_open(int open) {
  g_open = open != 0;
  cursor_for_menu(g_open);
  if (g_open) set_status("menu open: F3 closes");
}

int rbrrtx_menu_is_drawing(void) { return g_drawing ? 1 : 0; }

int rbrrtx_menu_option_locked(const char* key, float* value) {
  if (!key) return 0;
  const auto it = g_touched.find(key);
  if (it == g_touched.end()) return 0;
  if (value) *value = it->second;
  return 1;
}

void rbrrtx_menu_set_callbacks(const rbrrtx_menu_callbacks* cb) {
  g_cb = cb ? *cb : rbrrtx_menu_callbacks{};
  g_pending_apply = !g_touched.empty();   // a late remix_set still gets the saved values
}

void rbrrtx_menu_add_page(const char* name, rbrrtx_menu_page_fn draw, void* user) {
  if (!name || !draw) return;
  g_pages.push_back(Page{ name, draw, user });
}

} // extern "C"
