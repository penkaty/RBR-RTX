// Shared declarations of the RBR-RTX d3d9 proxy.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Appends one timestamped line to <RBRRTX_LOG_DIR>\<file>; does nothing when RBRRTX_LOG_DIR is unset.
void rbrrtx_log_to(const wchar_t* file, const wchar_t* fmt, ...);

// d3d9_proxy.cpp: full path of the loaded D3D9 backend (Remix bridge or system d3d9).
const wchar_t* rbrrtx_backend_path();

// report.cpp: per-stage player report, zipped to <logs>\reports\ (key report_mark marks a problem). Per frame: a few
// additions; files and zip on a background thread after the stage.
struct RbrReportFrame { unsigned draws, xformed, ff, btb; float carPos[3]; };
void rbrrtx_report_frame(HWND wnd, const RbrReportFrame& f);   // once per Present
void rbrrtx_report_event(const wchar_t* text);                  // hotkey toggles, sweeps (log_frame reasons)
void rbrrtx_report_log_line(const wchar_t* line);               // every comp log line; warnings go to problems.txt
void rbrrtx_report_sweep(const wchar_t* dir);                   // an Alt+K sweep folder finished during the stage
extern unsigned g_rbrrtxCreates;                                // textures + shaders created (hitch causes)

// True when the environment variable is set to something other than "0" (default when unset).
bool rbrrtx_env_flag(const wchar_t* name, bool default_value);

// comp.cpp: hooks IDirect3D9::CreateDevice on `d3d9` (an IDirect3D9*) to install the compatibility layer.
void rbrrtx_comp_install(void* d3d9, HMODULE backend);

// harvest.cpp: dev texture harvest (RBRRTX_HARVEST=<folder>); hooks CreateTexture on the device (an IDirect3DDevice9*).
void rbrrtx_harvest_install(void* device);
// harvest.cpp: notes which shader (file name) draws the texture bound to stage 0 (usage.tsv, once per pair).
void rbrrtx_harvest_note_draw(void* device, const wchar_t* shader);
// harvest.cpp: Remix hash (XXH3 of mip 0) of a texture uploaded while the hooks ran, 0 if unknown. Kept for every
// texture even without RBRRTX_HARVEST (hash only, no files; RBRRTX_TEXHASH=0 turns it off) for the stage sweep.
unsigned long long rbrrtx_texture_hash(void* base_texture);
// Second ground layer (harvest.cpp): a twin of a game texture with its own Remix hash (nullptr = no source held).
void* rbrrtx_layer_twin(void* texture);
void rbrrtx_layer_release_other(int track);                   // stage driven: other stages' sources and twins go
void rbrrtx_layer_stats(unsigned* held, unsigned* twins, double* mb);
// harvest.cpp: smallest alpha (0..255) of that texture's mip 0, -1 if unknown (same hooks, alpha_min.hpp).
int rbrrtx_texture_min_alpha(void* base_texture);
// harvest.cpp: writes mip 0 of a 2D texture to a .dds through a READONLY lock (frame dump; through the Remix bridge
// that is the client's copy, the bytes it sent). False if the type or format is not handled or the lock fails.
bool rbrrtx_save_texture_dds(void* base_texture, const wchar_t* path);

// alphacut.cpp: alpha-card cut-outs (adr-001 tiers 2-3). For the current alpha-tested DrawIndexedPrimitive
// (TRIANGLELIST), our copy of its cards clipped to the stage-0 texture's outline: draw `tris` triangles from
// vb/ib (stream 0 at offset 0 with `stride`, base vertex 0), alpha test off when alphaTestOff. False = draw as is.
struct RbrAlphaCut { void* vb; void* ib; unsigned stride, verts, tris; bool alphaTestOff; };
bool rbrrtx_alphacut_find(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims,
                          RbrAlphaCut* out);
void rbrrtx_alphacut_forget(const void* buffer);              // the game created a buffer at this address
void rbrrtx_alphacut_init();
void rbrrtx_alphacut_frame(unsigned frame);
void rbrrtx_alphacut_summary(wchar_t* out, size_t n, unsigned frames);   // stats since the last call

// game_state.cpp: what the player picked, read from RBR's globals (NGPCarMenu RBRAPI.h offsets) and RSF's
// rsfdata\cache\stages_data.json (name, country, surface). -1 / empty = unknown.
struct RbrStageInfo {
  int trackId, carId, tyreType, weatherType, damage;          // RBRMapSettings 0x1660800
  int clouds, wetness, surfaceAge, timeOfDay, skyType;        // RBRMapSettingsEx 0x8938F8
  int gameMode;                                               // 1 driving, 2 pause, 3 menu, 5 loading, 8 replay, 10 pre-stage
  wchar_t name[80];
  char country[8];                                            // ISO code from RSF ("PL"), "" if unknown
  int tarmac, gravel, snow;                                   // surface percent from RSF
};
bool rbrrtx_stage_info(RbrStageInfo* out);
const wchar_t* rbrrtx_sky_name(int skyType);                  // Crisp, Hazy, NoRain, LightRain, ...
const wchar_t* rbrrtx_cloud_name(int clouds);
const wchar_t* rbrrtx_time_name(int timeOfDay);
const wchar_t* rbrrtx_wetness_name(int wetness);
// Copies the window's client area from the screen into a PNG (what Remix presented). mean_luma = 0..255 (0 = black:
// exclusive full screen hides the image from GDI).
bool rbrrtx_screenshot(HWND wnd, const wchar_t* png_path, double* mean_luma);

// exception_trace.cpp
void rbrrtx_exception_trace_install();

// keys.cpp: hotkeys from rbr-rtx\config\keys.ini (defaults in keys.cpp). Order = g_actions in keys.cpp.
enum RbrKey {
  RBRKEY_DUMP, RBRKEY_LAYER, RBRKEY_SUN, RBRKEY_FOLIAGE, RBRKEY_DUST, RBRKEY_WINDSCREEN, RBRKEY_SKY,
  RBRKEY_DEBUG_SUN, RBRKEY_SUN_LEFT, RBRKEY_SUN_RIGHT, RBRKEY_SUN_DOWN, RBRKEY_SUN_UP, RBRKEY_EXPERIMENT, RBRKEY_CULLING,
  RBRKEY_SUN_BRIGHTER, RBRKEY_SUN_DIMMER, RBRKEY_FF_SCENERY, RBRKEY_SWEEP, RBRKEY_RAYTRACING, RBRKEY_OPAQUE_ALPHA, RBRKEY_ALPHACUT, RBRKEY_REPORT, RBRKEY_SUN_SAVE, RBRKEY_GROUND_LAYER, RBRKEY_COUNT
};
// <env> if set, else <proxy dir>\rbr-rtx\config\<file>.
void rbrrtx_config_path(const wchar_t* env, const wchar_t* file, wchar_t* out, size_t size);
void rbrrtx_keys_load();
// True once per press (call every frame), only while the game window has the focus, Ctrl is up and Alt is held
// only for Alt+ keys.
bool rbrrtx_key_pressed(int action);
// True while held (same conditions).
bool rbrrtx_key_held(int action);
const wchar_t* rbrrtx_key_name(int action);
