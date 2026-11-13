// rtss.cpp - frame times from RivaTuner Statistics Server's shared memory (user 2026-11-13: "use shared memory for
// RTSS"), for the stage sweep. RTSS measures the process that presents: with the Remix bridge that is NvRemixBridge.exe
// (the 64-bit server), not the game, so its entry is preferred over our own process id. Read only; nothing is written.
//
// Layout (RTSSSharedMemory.h of the RTSS SDK, version 2.x): header dwSignature 'RTSS', dwVersion, dwAppEntrySize,
// dwAppArrOffset, dwAppArrSize; each app entry: dwProcessID, szName[MAX_PATH], dwFlags, dwTime0, dwTime1, dwFrames,
// dwFrameTime (the last frame's time in microseconds).

#include "common.hpp"
#include <cstring>
#include <cctype>

namespace {
HANDLE g_map = nullptr;
const unsigned char* g_view = nullptr;
ULONGLONG g_lastTry = 0;
constexpr DWORD kSignature = 0x52545353;               // 'RTSS'
constexpr size_t kName = 4, kFrameTime = 4 + MAX_PATH + 4 * 4;

bool open_map() {
  if (g_view) return true;
  const ULONGLONG now = GetTickCount64();
  if (g_lastTry && now - g_lastTry < 5000) return false;   // RTSS not running: try again in a few seconds
  g_lastTry = now;
  g_map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"RTSSSharedMemoryV2");
  if (!g_map) return false;
  g_view = static_cast<const unsigned char*>(MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, 0));
  if (!g_view) { CloseHandle(g_map); g_map = nullptr; return false; }
  return true;
}

DWORD rd(const unsigned char* p) { DWORD v; std::memcpy(&v, p, 4); return v; }

bool name_has(const unsigned char* entry, const char* part) {
  char name[MAX_PATH];
  std::memcpy(name, entry + kName, MAX_PATH);
  name[MAX_PATH - 1] = 0;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return std::strstr(name, part) != nullptr;
}
}  // namespace

bool rbrrtx_rtss_frame_ms(double* ms) {
  if (!open_map()) return false;
  __try {
    if (rd(g_view) != kSignature || rd(g_view + 4) < 0x00020000) return false;
    const DWORD size = rd(g_view + 8), offset = rd(g_view + 12), count = rd(g_view + 16);
    if (size < kFrameTime + 4 || count > 1024) return false;
    const DWORD pid = GetCurrentProcessId();
    const unsigned char* bridge = nullptr;
    const unsigned char* own = nullptr;
    for (DWORD i = 0; i < count; ++i) {
      const unsigned char* e = g_view + offset + static_cast<size_t>(i) * size;
      const DWORD epid = rd(e);
      if (!epid) continue;
      if (!bridge && name_has(e, "nvremixbridge")) bridge = e;
      if (epid == pid) own = e;
    }
    const unsigned char* e = bridge ? bridge : own;
    if (!e) return false;
    const DWORD us = rd(e + kFrameTime);
    if (us == 0 || us > 2000000) return false;
    *ms = us / 1000.0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {             // RTSS closed and unmapped under us
    g_view = nullptr;
    g_map = nullptr;
    return false;
  }
}
