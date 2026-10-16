// Smoke test for the d3d9 proxy: loads d3d9.dll from this exe's folder, creates IDirect3D9,
// prints the adapter count and releases it. Exit code 0 = Direct3DCreate9 returned an object.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

__declspec(noinline) void throw_for_trace() { throw std::runtime_error("smoke-test exception"); }

int main(int argc, char** argv) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  wcscpy_s(wcsrchr(path, L'\\') + 1, MAX_PATH - (wcsrchr(path, L'\\') + 1 - path), L"d3d9.dll");
  HMODULE m = LoadLibraryW(path);
  if (!m) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 2; }
  if (argc > 1 && strcmp(argv[1], "throw") == 0) {   // exercises the proxy's exception tracer
    try { throw_for_trace(); } catch (const std::exception& e) { printf("caught: %s\n", e.what()); }
  }
  using create_t = void*(WINAPI*)(UINT);
  auto create = reinterpret_cast<create_t>(GetProcAddress(m, "Direct3DCreate9"));
  if (!create) { printf("no Direct3DCreate9 export\n"); return 3; }
  void* d3d = create(32);  // D3D_SDK_VERSION
  printf("Direct3DCreate9 -> %p\n", d3d);
  if (!d3d) return 1;
  void** vtbl = *reinterpret_cast<void***>(d3d);
  using count_t = UINT(WINAPI*)(void*);
  using release_t = ULONG(WINAPI*)(void*);
  printf("GetAdapterCount -> %u\n", reinterpret_cast<count_t>(vtbl[4])(d3d));   // IDirect3D9 slot 4
  Sleep(3000);  // give the Remix bridge time to show up in the process list
  printf("Release -> %lu\n", reinterpret_cast<release_t>(vtbl[2])(d3d));       // IUnknown slot 2
  return 0;
}
