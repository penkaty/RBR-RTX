// menu.hpp - F3 in-game menu for the RBR-RTX d3d9 proxy (Dear ImGui, DX9 + Win32 backends).
//
// Self-contained: the proxy calls the C functions below at its hook points (see docs/research/f3-menu-plan.md)
// and supplies behaviour through rbrrtx_menu_callbacks. Pages are added with rbrrtx_menu_add_page().
// Nothing here writes user.conf or touches the game files.
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// Behaviour the proxy supplies. Every member may be null; the menu then shows the page as unavailable.
typedef struct rbrrtx_menu_callbacks {
  // Sends an option to Remix (the proxy's remix_set / rbrrtx_remix_set). Returns true on success.
  // Used by the Wetness page (rtx.wet.*) and anything else that is a plain Remix option.
  int (*remix_set)(const char* key, const char* value);

  // Texture picker probe: the texture under the screen centre of the last primary-target draw.
  // Fills hash (Remix texture hash, 0 = none), a short human-readable description (flags, size,
  // current class) and returns true if something was found.
  int (*probe_centre)(unsigned long long* hash, char* info, size_t info_len);

  // Texture picker action: class 0 = Terrain, 1 = Decal, 2 = Water (animated), 3 = Ignore.
  // The proxy adds the hash to the matching rtx.* list (keep the base list from rtx.conf, same as decal_tag).
  int (*mark_texture)(unsigned long long hash, int cls);

  // Info page: one line per call of text, e.g. stage name, weather, time. Fills buf (NUL-terminated).
  void (*info_text)(char* buf, size_t buf_len);

  // Directory for the menu's own ini (for example <game>\rbr-rtx\config). Fills buf with a wide path.
  // If null, the menu uses the folder of the loaded d3d9.dll.
  void (*config_dir)(wchar_t* buf, size_t buf_len);
} rbrrtx_menu_callbacks;

// A page draw callback. Called once per frame while the menu is open and the page is selected.
// It may call ImGui directly (ImGui is the one copy vendored in third_party/imgui).
typedef void (*rbrrtx_menu_page_fn)(void* user);

// Lifecycle. Call from the proxy in this order:
//   init        : first hk_Present / hk_EndScene (device and HWND known). Idempotent.
//   wndproc     : from the subclassed game window procedure (see plan). Returns non-zero when the
//                 message was consumed by the open menu and must NOT reach the game.
//   frame       : once per frame, from hk_Present before the original Present. Does nothing when closed.
//   lost/reset  : around IDirect3DDevice9::Reset (lost before, reset after).
//   shutdown    : from DLL_PROCESS_DETACH or device release.
int  rbrrtx_menu_init(HWND game_window, IDirect3DDevice9* dev);
int  rbrrtx_menu_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
void rbrrtx_menu_frame(IDirect3DDevice9* dev);
void rbrrtx_menu_device_lost(void);
void rbrrtx_menu_device_reset(IDirect3DDevice9* dev);
void rbrrtx_menu_shutdown(void);

// Open state. F3 toggles it. The proxy can also force it.
int  rbrrtx_menu_is_open(void);
void rbrrtx_menu_set_open(int open);

// 1 while the menu is drawing. The proxy's draw/state hooks must pass calls through untouched when this is 1
// (same job as shared::globals::imgui_is_rendering in remix-comp-base), otherwise the menu's own draws get
// tagged, transformed or counted.
int  rbrrtx_menu_is_drawing(void);

// Menu-owned option override. Returns 1 and the menu's value when the user moved (or loaded from menu.ini)
// the option, e.g. "rtx.wet.globalWetness". wetness.cpp must check this before sending its computed value,
// otherwise its per-frame rain driver overwrites the slider. Returns 0 when the proxy should keep control.
int  rbrrtx_menu_option_locked(const char* key, float* value);

// Wires the proxy's behaviour in. Call once after init (or before; the pointer is copied).
void rbrrtx_menu_set_callbacks(const rbrrtx_menu_callbacks* cb);

// Adds a page to the menu bar. Pages show in registration order; `name` must stay valid (string literal).
// Built-in pages: Info, Wetness, Textures, Save. Modules add their own with this call.
void rbrrtx_menu_add_page(const char* name, rbrrtx_menu_page_fn draw, void* user);

#ifdef __cplusplus
} // extern "C"
#endif
