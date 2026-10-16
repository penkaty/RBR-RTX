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
