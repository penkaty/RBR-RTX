// RBR-RTX d3d9 proxy (x86).
//
// Dropped into the game folder as d3d9.dll. It loads the real D3D9 backend by full path and
// forwards every export to it, so the backend (e.g. RTX Remix's bridge d3d9.dll) stays in its own
// folder instead of replacing game files. Remix's bridge finds .trex\ and bridge.conf next to its
// own file, so loading it from elsewhere works.
//
// Backend selection, first match wins:
//   1. env RBRRTX_D3D9_BACKEND = full path of a d3d9.dll (dev launcher tools\remix\rbr-remix.ps1)
//   2. <proxy dir>\rbr-rtx\remix\d3d9.dll (the installed add-on). Unless already set, the DXVK/Remix
//      config, log and cache paths are pointed into <proxy dir>\rbr-rtx\ first, so RSF's own dxvk.conf
//      is not used and nothing extra is written next to the game.
//   3. <system dir>\d3d9.dll  (plain Direct3D 9: the game behaves as without the proxy)
// The comp layer runs by default with a Remix backend (1 or 2); env RBRRTX_COMP=0/1 overrides.
// Optional log: env RBRRTX_LOG_DIR = folder for rbrrtx_d3d9_proxy.log (nothing is written otherwise).
// Optional exception trace: env RBRRTX_TRACE_EXCEPTIONS=1 (see exception_trace.cpp).
//
// This is the future hook point between RBR and the renderer (draw-call fixes, Remix API lights).

#include "common.hpp"
#include <cstdarg>
#include <cstdio>
#include <cwchar>

void rbrrtx_log_to(const wchar_t* file, const wchar_t* fmt, ...) {
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"RBRRTX_LOG_DIR", dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  wchar_t path[MAX_PATH * 2];
  swprintf_s(path, L"%s\\%s", dir, file);
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || !f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fwprintf(f, L"%02u:%02u:%02u.%03u [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
  va_list args;
  va_start(args, fmt);
  vfwprintf(f, fmt, args);
  va_end(args);
  fputwc(L'\n', f);
  fclose(f);
}

bool rbrrtx_env_flag(const wchar_t* name, bool default_value) {
  wchar_t v[8];
  DWORD n = GetEnvironmentVariableW(name, v, 8);
  if (n == 0 || n >= 8) return default_value;
  return v[0] != L'0';
}

namespace {

HMODULE g_self = nullptr;
HMODULE g_backend = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
wchar_t g_backend_path[MAX_PATH * 2] = {};
bool g_remix_backend = false;  // backend came from 1 or 2 (not the system d3d9)

#define log(...) rbrrtx_log_to(L"rbrrtx_d3d9_proxy.log", __VA_ARGS__)

bool try_backend(const wchar_t* path) {
  // LOAD_WITH_ALTERED_SEARCH_PATH: the backend's own dependencies are looked up in its folder.
  g_backend = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (g_backend) log(L"backend loaded: %s", path);
  else log(L"backend FAILED (error %lu): %s", GetLastError(), path);
  return g_backend != nullptr;
}

// Sets an environment variable to <base>\<rel> unless it is already set. Remix's server process
// (NvRemixBridge.exe) inherits this process's environment, so DXVK reads these there.
void default_env_path(const wchar_t* name, const wchar_t* base, const wchar_t* rel) {
  if (GetEnvironmentVariableW(name, nullptr, 0) != 0) return;
  wchar_t v[MAX_PATH * 2];
  swprintf_s(v, L"%s\\%s", base, rel);
  SetEnvironmentVariableW(name, v);
}

BOOL CALLBACK load_backend(PINIT_ONCE, PVOID, PVOID*) {
  DWORD n = GetEnvironmentVariableW(L"RBRRTX_D3D9_BACKEND", g_backend_path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    if (try_backend(g_backend_path)) { g_remix_backend = true; return TRUE; }
  } else {
    wchar_t dir[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_self, dir, MAX_PATH);
    wchar_t* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(dir, L'\\') : nullptr;
    if (slash) {
      *slash = 0;
      wchar_t addon[MAX_PATH * 2];
      swprintf_s(addon, L"%s\\rbr-rtx", dir);
      swprintf_s(g_backend_path, L"%s\\remix\\d3d9.dll", addon);
      if (GetFileAttributesW(g_backend_path) != INVALID_FILE_ATTRIBUTES) {
        default_env_path(L"DXVK_CONFIG_FILE", addon, L"config\\dxvk.conf");
        default_env_path(L"DXVK_RTX_CONFIG_FILE", addon, L"config\\rtx.conf");
        default_env_path(L"DXVK_LOG_PATH", addon, L"logs");
        default_env_path(L"DXVK_METRICS_PATH", addon, L"logs");
        default_env_path(L"DXVK_STATE_CACHE_PATH", addon, L"cache");
        // Remix mods and captures (util_filesys.h); unset, Remix creates rtx-remix\ in the game root.
        // Kept under a folder literally named rtx-remix: the Toolkit's project wizard only accepts that name.
        default_env_path(L"DEFAULT_MODS_DIR", addon, L"rtx-remix\\mods");
        default_env_path(L"DXVK_CAPTURE_PATH", addon, L"rtx-remix\\captures");
        if (try_backend(g_backend_path)) { g_remix_backend = true; return TRUE; }
      }
    }
  }
  log(L"falling back to system d3d9");
  UINT len = GetSystemDirectoryW(g_backend_path, MAX_PATH);
  if (len > 0 && len < MAX_PATH - 10) {
    wcscat_s(g_backend_path, L"\\d3d9.dll");
    g_backend = LoadLibraryW(g_backend_path);
  }
  log(g_backend ? L"backend loaded: %s (system)" : L"system d3d9 FAILED: %s", g_backend_path);
  return TRUE;
}

FARPROC backend_proc(const char* name) {
  InitOnceExecuteOnce(&g_once, load_backend, nullptr, nullptr);
  if (!g_backend) return nullptr;
  FARPROC p = GetProcAddress(g_backend, name);
  if (!p) log(L"backend has no export %S", name);
  return p;
}

// Resolves an export once per call site and caches it.
#define BACKEND(name, type) \
  static type fn = nullptr; \
  if (!fn) fn = reinterpret_cast<type>(backend_proc(name));

}  // namespace

extern "C" {

void* WINAPI Proxy_Direct3DCreate9(UINT sdk) {
  using fn_t = void*(WINAPI*)(UINT);
  BACKEND("Direct3DCreate9", fn_t);
  void* d3d = fn ? fn(sdk) : nullptr;
  log(L"Direct3DCreate9(%u) -> %p", sdk, d3d);
  if (d3d && rbrrtx_env_flag(L"RBRRTX_COMP", g_remix_backend)) rbrrtx_comp_install(d3d, g_backend);
  return d3d;
}

HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, void** out) {
  using fn_t = HRESULT(WINAPI*)(UINT, void**);
  BACKEND("Direct3DCreate9Ex", fn_t);
  HRESULT hr = fn ? fn(sdk, out) : E_NOTIMPL;
  log(L"Direct3DCreate9Ex(%u) -> 0x%08lx", sdk, hr);
  return hr;
}

int WINAPI Proxy_D3DPERF_BeginEvent(DWORD col, LPCWSTR name) {
  using fn_t = int(WINAPI*)(DWORD, LPCWSTR);
  BACKEND("D3DPERF_BeginEvent", fn_t);
  return fn ? fn(col, name) : 0;
}

int WINAPI Proxy_D3DPERF_EndEvent() {
  using fn_t = int(WINAPI*)();
  BACKEND("D3DPERF_EndEvent", fn_t);
  return fn ? fn() : 0;
}

DWORD WINAPI Proxy_D3DPERF_GetStatus() {
  using fn_t = DWORD(WINAPI*)();
  BACKEND("D3DPERF_GetStatus", fn_t);
  return fn ? fn() : 0;
}

BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame() {
  using fn_t = BOOL(WINAPI*)();
  BACKEND("D3DPERF_QueryRepeatFrame", fn_t);
  return fn ? fn() : FALSE;
}

void WINAPI Proxy_D3DPERF_SetMarker(DWORD col, LPCWSTR name) {
  using fn_t = void(WINAPI*)(DWORD, LPCWSTR);
  BACKEND("D3DPERF_SetMarker", fn_t);
  if (fn) fn(col, name);
}

void WINAPI Proxy_D3DPERF_SetOptions(DWORD options) {
  using fn_t = void(WINAPI*)(DWORD);
  BACKEND("D3DPERF_SetOptions", fn_t);
  if (fn) fn(options);
}

void WINAPI Proxy_D3DPERF_SetRegion(DWORD col, LPCWSTR name) {
  using fn_t = void(WINAPI*)(DWORD, LPCWSTR);
  BACKEND("D3DPERF_SetRegion", fn_t);
  if (fn) fn(col, name);
}

int WINAPI Proxy_DebugSetLevel() {
  using fn_t = int(WINAPI*)();
  BACKEND("DebugSetLevel", fn_t);
  return fn ? fn() : 0;
}

void WINAPI Proxy_DebugSetMute() {
  using fn_t = void(WINAPI*)();
  BACKEND("DebugSetMute", fn_t);
  if (fn) fn();
}

int WINAPI Proxy_Direct3D9EnableMaximizedWindowedModeShim(UINT a) {
  using fn_t = int(WINAPI*)(UINT);
  BACKEND("Direct3D9EnableMaximizedWindowedModeShim", fn_t);
  return fn ? fn(a) : 0;
}

// Remix API (only present when the backend is Remix's bridge). remixapi_ErrorCode is an int.
int WINAPI Proxy_remixapi_InitializeLibrary(const void* info, void* out_interface) {
  using fn_t = int(WINAPI*)(const void*, void*);
  BACKEND("remixapi_InitializeLibrary", fn_t);
  return fn ? fn(info, out_interface) : 1;  // 1 = REMIXAPI_ERROR_CODE_GENERAL_FAILURE
}

int WINAPI Proxy_remixapi_RegisterCallbacks(void* begin_scene, void* end_scene, void* present) {
  using fn_t = int(WINAPI*)(void*, void*, void*);
  BACKEND("remixapi_RegisterCallbacks", fn_t);
  return fn ? fn(begin_scene, end_scene, present) : 1;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = inst;
    DisableThreadLibraryCalls(inst);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    log(L"proxy attached to %s", exe);
    rbrrtx_exception_trace_install();
  }
  return TRUE;
}
