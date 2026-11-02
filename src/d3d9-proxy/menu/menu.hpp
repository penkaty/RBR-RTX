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

// What the texture picker found under the screen centre (filled by the proxy's probe callback).
typedef struct rbrrtx_menu_pick {
  int found;                       // 1 = a world draw lies under the centre
  unsigned long long hash[2];      // Remix texture hash of stage 0 and stage 1 of that draw (0 = none / unknown)
  void* texture[2];                // IDirect3DTexture9* for thumbnails; owned by the proxy, valid only during this frame
  char shader[64];                 // vertex shader name of the draw
  float distance;                  // metres from the camera
} rbrrtx_menu_pick;

// Behaviour the proxy supplies. Every member may be null; the menu then shows the page as unavailable.
typedef struct rbrrtx_menu_callbacks {
  // Sends an option to Remix (the proxy's remix_set / rbrrtx_remix_set). Returns true on success.
  // Used by the Wetness page (rtx.wet.*) and anything else that is a plain Remix option.
  int (*remix_set)(const char* key, const char* value);

  // Texture picker: the draw under the screen centre. want_new = 0 returns the last result unchanged (frozen);
  // otherwise the proxy keeps probing the next frames. Returns pick->found.
  int (*probe_centre)(rbrrtx_menu_pick* pick, int want_new);

  // Texture picker action: class 0 = Terrain, 1 = Decal, 2 = Water (animated), 3 = Ignore, 4 = clear all four.
  // The proxy toggles the hash in its own list for that class and applies it live (batched remix_set).
  // Returns 0 = failed, 1 = tagged, 2 = tag removed.
  int (*mark_texture)(unsigned long long hash, int cls);

  // Current categories of a texture as text ("terrain (conf), decal (menu)" or "none"), from the proxy's lists.
  void (*texture_classes)(unsigned long long hash, char* buf, size_t buf_len);

  // menu.ini [textures]: one key=value line per call at load (the proxy stores it) ...
  void (*texture_ini)(const char* key, const char* value);
  // ... and at save the proxy writes its lines through `emit` (one "key=value" per call, no newline).
  void (*texture_ini_dump)(void (*emit)(const char* line, void* user), void* user);

  // Info page: several lines of text (stage, weather, time, ground mode, baker...). Fills buf (NUL-terminated).
  void (*info_text)(char* buf, size_t buf_len);
  // Info page key list: line i ("Alt+K  stage sweep"), returns 0 when i is past the end.
  int (*key_line)(int i, char* buf, size_t buf_len);

  // Directory for the menu's own ini (for example <game>\rbr-rtx\config). Fills buf with a wide path.
  // If null, the menu uses the folder of the loaded d3d9.dll.
  void (*config_dir)(wchar_t* buf, size_t buf_len);
} rbrrtx_menu_callbacks;

// A page draw callback. Called once per frame while the menu is open and the page is selected.
// It may call ImGui directly (ImGui is the one copy vendored in third_party/imgui).
typedef void (*rbrrtx_menu_page_fn)(void* user);

// Lifecycle. Call from the proxy in this order:
//   set_callbacks, set_toggle_vk, init : at CreateDevice (HWND and device known). Idempotent.
//   wndproc     : from the subclassed game window procedure (see plan). Returns non-zero when the
//                 message was consumed by the open menu and must NOT reach the game.
//   tick        : once per frame (Present), also while closed: sends menu.ini values to Remix once reachable.
//   frame       : once per frame inside BeginScene/EndScene, before Present. Does nothing when closed.
//   lost/reset  : around IDirect3DDevice9::Reset (lost before, reset after).
//   shutdown    : from DLL_PROCESS_DETACH or device release.
int  rbrrtx_menu_init(HWND game_window, IDirect3DDevice9* dev);
int  rbrrtx_menu_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
void rbrrtx_menu_tick(void);
void rbrrtx_menu_frame(IDirect3DDevice9* dev);
void rbrrtx_menu_device_lost(void);
void rbrrtx_menu_device_reset(IDirect3DDevice9* dev);
void rbrrtx_menu_shutdown(void);

// Toggle key (a Windows virtual-key code, default F3; keys.ini "menu"). 0 = no key (the menu cannot be opened).
void rbrrtx_menu_set_toggle_vk(int vk);

// The DX9 backend holds one reference on the device while it is initialised. IDirect3DDevice9::Release hooks that
// count references must add this, and call release_device when the game's last reference is the only one left.
int  rbrrtx_menu_device_refs(void);
void rbrrtx_menu_release_device(void);

// The ImGui font atlas texture (IDirect3DTexture9*), null until the first frame was drawn. For logging its hash.
void* rbrrtx_menu_font_texture(void);

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

// Wires the proxy's behaviour in. Call before init (the pointer is copied).
void rbrrtx_menu_set_callbacks(const rbrrtx_menu_callbacks* cb);

// Adds a page to the menu bar. Pages show in registration order; `name` must stay valid (string literal).
// Built-in pages: Info, Wetness, Textures, Save. Modules add their own with this call.
void rbrrtx_menu_add_page(const char* name, rbrrtx_menu_page_fn draw, void* user);

#ifdef __cplusplus
} // extern "C"
#endif
