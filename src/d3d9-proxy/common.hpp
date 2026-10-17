// Shared declarations of the RBR-RTX d3d9 proxy.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Appends one timestamped line to <RBRRTX_LOG_DIR>\<file>; does nothing when RBRRTX_LOG_DIR is unset.
void rbrrtx_log_to(const wchar_t* file, const wchar_t* fmt, ...);

// True when the environment variable is set to something other than "0" (default when unset).
bool rbrrtx_env_flag(const wchar_t* name, bool default_value);

// comp.cpp: hooks IDirect3D9::CreateDevice on `d3d9` (an IDirect3D9*) to install the compatibility layer.
void rbrrtx_comp_install(void* d3d9, HMODULE backend);

// harvest.cpp: dev texture harvest (RBRRTX_HARVEST=<folder>); hooks CreateTexture on the device (an IDirect3DDevice9*).
void rbrrtx_harvest_install(void* device);
// harvest.cpp: notes which shader (file name) draws the texture bound to stage 0 (usage.tsv, once per pair).
void rbrrtx_harvest_note_draw(void* device, const wchar_t* shader);

// exception_trace.cpp
void rbrrtx_exception_trace_install();

// keys.cpp: hotkeys from rbr-rtx\config\keys.ini (defaults in keys.cpp). Order = g_actions in keys.cpp.
enum RbrKey {
  RBRKEY_DUMP, RBRKEY_LAYER, RBRKEY_SUN, RBRKEY_FOLIAGE, RBRKEY_DUST, RBRKEY_WINDSCREEN, RBRKEY_SKY,
  RBRKEY_DEBUG_SUN, RBRKEY_SUN_LEFT, RBRKEY_SUN_RIGHT, RBRKEY_SUN_DOWN, RBRKEY_SUN_UP, RBRKEY_EXPERIMENT, RBRKEY_CULLING, RBRKEY_COUNT
};
// <env> if set, else <proxy dir>\rbr-rtx\config\<file>.
void rbrrtx_config_path(const wchar_t* env, const wchar_t* file, wchar_t* out, size_t size);
void rbrrtx_keys_load();
// True once per press (call every frame), only while the game window has the focus and Ctrl/Alt are up.
bool rbrrtx_key_pressed(int action);
// True while held (same conditions).
bool rbrrtx_key_held(int action);
const wchar_t* rbrrtx_key_name(int action);
