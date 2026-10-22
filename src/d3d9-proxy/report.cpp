// Player report (user 2026-10-22): every stage drive is recorded and zipped into
// <logs>\reports\report-<date>-<time>-<stage>.zip, one file a player can send with a bug report. Key 9 (keys.ini
// report_mark) = "something is wrong right now": event line, screenshot, the frame times around that moment; the zip
// gets "-marked" and is never deleted by the rotation. rbr-rtx\config\report.ini: enabled = 0 turns it all off.
//
// Cost while driving: per frame one QueryPerformanceCounter, a ring/histogram write and a few additions (no
// allocation, no file access); once a second one stage-info read and one buffered line to perf.tsv. Everything else
// (summary, system info, config copies, Remix log filter, zip, rotation) runs on a background thread after the stage.
// A stage the game never left (crash, quit from the stage) is finished at the next start ("unfinished").
//
// Files in the zip: summary.txt, stage.txt, system.txt, perf.tsv (one line a second), hitches.tsv, events.tsv,
// problems.txt (warnings of our log + Remix log errors/warnings + crash traces), config\ (our .ini/.conf files,
// user.conf), mark-N.png + burst-N.tsv per marked problem, sweep-*\ (text files of an Alt+K sweep taken meanwhile).
// The zip is stored, not compressed: the PNGs are compressed already and the text is small. Paths under the
// Windows user profile are written as %USERPROFILE%.

#include "common.hpp"
#include <dxgi.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <share.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "version.lib")

namespace fs = std::filesystem;

std::wstring rbrrtx_stage_line(const RbrStageInfo& si);   // comp.cpp
unsigned g_rbrrtxCreates = 0;

namespace {

constexpr int kHist = 400;                                 // frame-time histogram, 0.5 ms buckets up to 200 ms
constexpr unsigned kRing = 512;                            // last frame times, for the burst around a mark
constexpr unsigned kBurstAfter = 120;                      // frames recorded after a mark before the burst is written
constexpr size_t kMaxHitches = 5000;

struct Hitch { double t; float ms; float pos[3]; int mode; unsigned creates; bool event; };

struct Rec {
  bool on = false;
  std::wstring dir, name, stage;
  int trackId = 0, mode = 0;
  FILE* perf = nullptr;
  FILE* events = nullptr;
  FILE* problems = nullptr;
  LARGE_INTEGER start = {}, last = {}, lastEvent = {};
  // the current second
  unsigned secFrames = 0;
  double secSum = 0, secMax = 0, prevAvg = 0;
  unsigned long long secDraws = 0, secXf = 0, secFf = 0, secBtb = 0;
  // the whole stage, driving only (game mode 1)
  unsigned hist[kHist] = {};
  unsigned long long frames = 0;
  double sum = 0, worst = 0, worstT = 0;
  std::vector<Hitch> hitches;
  float ring[kRing] = {};
  unsigned long long ringFrames = 0;                       // frames written into the ring
  unsigned long long markFrame = 0, burstAt = 0;
  unsigned marks = 0, shots = 0, problemLines = 0, flushTick = 0;
  unsigned lastCreates = 0;
  bool event = false, btb = false, shadersUnknown = false;
  std::vector<std::wstring> markLines, sweeps;
};

struct Job { std::wstring dir, zipName, summary; std::vector<std::wstring> sweeps; bool unfinished = false; };

bool g_init = false, g_enabled = true;
int g_keep = 20, g_maxShots = 3;
LARGE_INTEGER g_qpf = {}, g_lastCheck = {};
DWORD g_thread = 0;
HWND g_wnd = nullptr;
std::wstring g_reportsDir, g_profile;
std::vector<std::wstring> g_early;                         // problem lines logged before the stage started
std::mutex g_jobs;
Rec g_rec;

double secs(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
  return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(g_qpf.QuadPart);
}

// %USERPROFILE% instead of the user's own folder name.
std::wstring scrub(std::wstring s) {
  if (g_profile.size() < 4) return s;
  for (size_t p = 0; p + g_profile.size() <= s.size();) {
    if (!_wcsnicmp(s.c_str() + p, g_profile.c_str(), g_profile.size())) {
      s.replace(p, g_profile.size(), L"%USERPROFILE%");
      p += 13;
    } else {
      ++p;
    }
  }
  return s;
}

FILE* open_text(const std::wstring& path, const wchar_t* mode = L"w, ccs=UTF-8") {
  return _wfsopen(path.c_str(), mode, _SH_DENYNO);         // shared: Remix keeps its logs open for writing
}

void write_text(const std::wstring& path, const std::wstring& text) {
  if (FILE* f = open_text(path)) { fputws(text.c_str(), f); fclose(f); }
}

std::wstring exe_dir() {
  wchar_t p[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  return fs::path(p).parent_path().wstring();
}

bool is_problem(const wchar_t* l) {
  if (!wcsncmp(l, L"frame ", 6)) return false;              // log_frame lines ("... candidates refused ...")
  static const wchar_t* kWords[] = { L"WARNING", L"warning", L"FAILED", L"failed", L"ERROR", L"NOT ",
                                     L"refused", L"not found", L"not understood", L"not recognised" };
  for (const wchar_t* w : kWords)
    if (wcsstr(l, w)) return true;
  for (const wchar_t* p = l; (p = wcsstr(p, L"error")) != nullptr; p += 5)
    if (wcsncmp(p, L"error 0", 7) || iswdigit(p[7])) return true;   // "error 0" = Remix API success
  return false;
}

void load_config() {
  wchar_t ini[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_REPORT", L"report.ini", ini, MAX_PATH * 2);
  g_enabled = GetPrivateProfileIntW(L"report", L"enabled", 1, ini) != 0;
  g_keep = (std::max)(1, static_cast<int>(GetPrivateProfileIntW(L"report", L"keep", 20, ini)));
  g_maxShots = static_cast<int>(GetPrivateProfileIntW(L"report", L"mark_screenshots", 3, ini));
  wchar_t dir[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"RBRRTX_LOG_DIR", dir, MAX_PATH);
  fs::path logs = n > 0 && n < MAX_PATH ? fs::path(dir) : fs::path(ini).parent_path().parent_path() / L"logs";
  g_reportsDir = (logs / L"reports").wstring();
  wchar_t prof[MAX_PATH] = {};
  const DWORD pn = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
  if (pn > 0 && pn < MAX_PATH) g_profile = prof;
}

// ---------------------------------------------------------------- background: system info, files, zip

std::wstring file_version(const fs::path& p) {
  std::error_code ec;
  if (!fs::exists(p, ec)) return L"(missing)";
  std::wstring r;
  DWORD h = 0;
  const DWORD n = GetFileVersionInfoSizeW(p.c_str(), &h);
  std::vector<BYTE> buf(n ? n : 1);
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (n && GetFileVersionInfoW(p.c_str(), 0, n, buf.data()) && VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) && fi) {
    wchar_t v[64];
    swprintf_s(v, L"%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    r = v;
  } else {
    r = L"no version";
  }
  WIN32_FILE_ATTRIBUTE_DATA a = {};
  if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) {
    SYSTEMTIME t;
    FileTimeToSystemTime(&a.ftLastWriteTime, &t);
    wchar_t s[64];
    swprintf_s(s, L", %lu KB, %04u-%02u-%02u", a.nFileSizeLow / 1024, t.wYear, t.wMonth, t.wDay);
    r += s;
  }
  return r;
}

std::wstring reg_string(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t v[256] = {};
  DWORD size = sizeof(v);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, v, &size) != ERROR_SUCCESS) return L"?";
  return v;
}

// VRAM from the display driver's registry entry: DXGI's DedicatedVideoMemory is a 32-bit SIZE_T in this process and
// comes back capped (3072 MB on an 11 GB card).
unsigned long long vram_mb(const wchar_t* description, unsigned long long dxgi) {
  const wchar_t* cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
  for (int i = 0; i < 16; ++i) {
    wchar_t key[160];
    swprintf_s(key, L"%s\\%04d", cls, i);
    if (reg_string(HKEY_LOCAL_MACHINE, key, L"DriverDesc") != description) continue;
    unsigned long long bytes = 0;
    DWORD size = sizeof(bytes);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"HardwareInformation.qwMemorySize", RRF_RT_REG_QWORD, nullptr, &bytes, &size) == ERROR_SUCCESS && bytes)
      return bytes / (1024 * 1024);
  }
  return dxgi / (1024 * 1024);
}

std::wstring gpu_info() {
  wchar_t sys[MAX_PATH] = {};
  GetSystemDirectoryW(sys, MAX_PATH);
  HMODULE dxgi = LoadLibraryW((std::wstring(sys) + L"\\dxgi.dll").c_str());   // the system's, not a mod's
  if (!dxgi) return L"GPU: dxgi.dll not loaded\n";
  std::wstring r;
  using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  IDXGIFactory1* f = nullptr;
  if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 d = {};
      a->GetDesc1(&d);
      LARGE_INTEGER umd = {};
      wchar_t drv[96] = L"?";
      if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        const unsigned p3 = LOWORD(umd.LowPart >> 16) % 10, p4 = LOWORD(umd.LowPart);
        swprintf_s(drv, L"%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        if (d.VendorId == 0x10DE) swprintf_s(drv + wcslen(drv), 96 - wcslen(drv), L" (NVIDIA %u.%02u)", p3 * 100 + p4 / 100, p4 % 100);
      }
      wchar_t line[512];
      swprintf_s(line, L"GPU %u: %s, %llu MB VRAM, vendor %04X device %04X, driver %s%s\n", i, d.Description,
                 vram_mb(d.Description, d.DedicatedVideoMemory), d.VendorId, d.DeviceId, drv,
                 (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L" (software)" : L"");
      r += line;
      a->Release();
    }
    f->Release();
  }
  FreeLibrary(dxgi);
  return r.empty() ? L"GPU: none found\n" : r;
}

std::wstring display_info() {
  RECT rc = {};
  if (!g_wnd || !GetClientRect(g_wnd, &rc)) return L"display: no game window\n";
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
  const LONG style = GetWindowLongW(g_wnd, GWL_STYLE);
  const int w = rc.right, h = rc.bottom, mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
  wchar_t s[256];
  swprintf_s(s, L"display: game %dx%d, monitor %dx%d at %lu Hz, %s\n", w, h, mw, mh, dm.dmDisplayFrequency,
             (style & WS_CAPTION) ? L"windowed" : (w == mw && h == mh) ? L"borderless or exclusive full screen" : L"borderless window");
  return s;
}

std::wstring list_dlls(const fs::path& dir) {
  std::wstring r;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::wstring ext = it->path().extension().wstring();
    if (_wcsicmp(ext.c_str(), L".dll") && _wcsicmp(ext.c_str(), L".asi")) continue;
    r += L"  " + it->path().filename().wstring() + L": " + file_version(it->path()) + L"\n";
  }
  return r.empty() ? L"  (none)\n" : r;
}

std::wstring system_info() {
  static std::wstring cached;
  if (!cached.empty()) return cached + display_info();
  std::wstring r = L"RBR-RTX system info\n";
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&system_info), &self);
  wchar_t me[MAX_PATH] = {};
  GetModuleFileNameW(self, me, MAX_PATH);
  r += L"RBR-RTX proxy: " + std::wstring(me) + L", built " + std::wstring(_CRT_WIDE(__DATE__)) + L" " + _CRT_WIDE(__TIME__) + L"\n";
  const fs::path backend = rbrrtx_backend_path();
  r += L"D3D9 backend: " + backend.wstring() + L" (" + file_version(backend) + L")\n";
  r += L"Remix bridge server: " + file_version(backend.parent_path() / L".trex" / L"NvRemixBridge.exe") +
       L"; Remix runtime d3d9.dll: " + file_version(backend.parent_path() / L".trex" / L"d3d9.dll") + L"\n";
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW os = {};
  os.dwOSVersionInfoSize = sizeof(os);
  if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) fn(&os);
  const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  DWORD ubr = 0, size = sizeof(ubr);
  RegGetValueW(HKEY_LOCAL_MACHINE, cv, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
  wchar_t s[512];
  swprintf_s(s, L"Windows %lu.%lu build %lu.%lu (%s)\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, ubr,
             reg_string(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion").c_str());
  r += s;
  MEMORYSTATUSEX ms = {};
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatusEx(&ms);
  SYSTEM_INFO si = {};
  GetNativeSystemInfo(&si);
  swprintf_s(s, L"CPU: %s, %lu logical cores; RAM %llu MB\n",
             reg_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString").c_str(),
             si.dwNumberOfProcessors, ms.ullTotalPhys / (1024 * 1024));
  r += s;
  r += gpu_info();
  const fs::path game = exe_dir();
  r += L"game folder DLLs (another d3d9/dxgi/dinput8 here can conflict):\n" + list_dlls(game);
  r += L"Plugins:\n" + list_dlls(game / L"Plugins");
  cached = scrub(r);
  return cached + display_info();
}

void copy_small(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  if (!fs::is_regular_file(from, ec) || fs::file_size(from, ec) > 512 * 1024) return;
  fs::create_directories(to.parent_path(), ec);
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}

std::wstring env_path(const wchar_t* name) {
  wchar_t v[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(name, v, MAX_PATH);
  return n > 0 && n < MAX_PATH ? v : L"";
}

// Error and warning lines of Remix's and the bridge's logs, repeats counted, the last 300.
std::wstring remix_problems() {
  const std::wstring dir = env_path(L"DXVK_LOG_PATH");
  if (dir.empty()) return L"(no Remix log folder)\n";
  std::wstring r;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::wstring n = it->path().filename().wstring();
    if (_wcsicmp(it->path().extension().wstring().c_str(), L".log") || (_wcsnicmp(n.c_str(), L"remix", 5) && _wcsnicmp(n.c_str(), L"bridge", 6) &&
        _wcsnicmp(n.c_str(), L"NvRemix", 7)))
      continue;
    FILE* f = open_text(it->path().wstring(), L"r, ccs=UTF-8");
    if (!f) continue;
    std::vector<std::pair<std::wstring, unsigned>> lines;
    wchar_t buf[1024];
    while (fgetws(buf, 1024, f)) {
      std::wstring l = buf;
      while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
      if (l.size() > 2 && l[0] == L'[' && l.find(L"] ") != std::wstring::npos) l.erase(0, l.find(L"] ") + 2);   // timestamp: repeats count as one
      std::wstring low = l;
      for (auto& c : low) c = towlower(c);
      if (low.find(L"err") == std::wstring::npos && low.find(L"warn") == std::wstring::npos && low.find(L"fail") == std::wstring::npos) continue;
      auto same = std::find_if(lines.begin(), lines.end(), [&](const auto& p) { return p.first == l; });
      if (same != lines.end()) ++same->second;
      else if (lines.size() < 2000) lines.push_back({ l, 1 });
    }
    fclose(f);
    r += L"== " + n + L" (" + std::to_wstring(lines.size()) + L" different lines)\n";
    for (size_t i = lines.size() > 300 ? lines.size() - 300 : 0; i < lines.size(); ++i)
      r += lines[i].first + (lines[i].second > 1 ? L"  (x" + std::to_wstring(lines[i].second) + L")" : L"") + L"\n";
  }
  return r.empty() ? L"(no Remix logs found in " + dir + L")\n" : r;
}

unsigned crc32(const unsigned char* p, size_t n) {
  static unsigned table[256];
  static bool made = false;
  if (!made) {
    for (unsigned i = 0; i < 256; ++i) {
      unsigned c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    made = true;
  }
  unsigned c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void put16(std::vector<unsigned char>& v, unsigned x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
void put32(std::vector<unsigned char>& v, unsigned x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }

// Stored (uncompressed) zip of every file under dir, names in UTF-8.
bool zip_dir(const fs::path& dir, const fs::path& zip) {
  FILE* out = nullptr;
  if (_wfopen_s(&out, zip.c_str(), L"wb") || !out) return false;
  SYSTEMTIME t;
  GetLocalTime(&t);
  const unsigned dosTime = (t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2);
  const unsigned dosDate = ((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay;
  std::vector<unsigned char> central;
  unsigned offset = 0, count = 0;
  bool ok = true;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); ok && !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::wstring rel = fs::relative(it->path(), dir, ec).wstring();
    std::replace(rel.begin(), rel.end(), L'\\', L'/');
    const int nn = WideCharToMultiByte(CP_UTF8, 0, rel.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string name(nn > 0 ? nn - 1 : 0, '\0');
    if (nn > 1) WideCharToMultiByte(CP_UTF8, 0, rel.c_str(), -1, name.data(), nn, nullptr, nullptr);
    FILE* in = nullptr;
    if (_wfopen_s(&in, it->path().c_str(), L"rb") || !in) continue;
    std::vector<unsigned char> data;
    unsigned char buf[65536];
    for (size_t r; (r = fread(buf, 1, sizeof(buf), in)) > 0;) data.insert(data.end(), buf, buf + r);
    fclose(in);
    const unsigned crc = crc32(data.data(), data.size()), size = static_cast<unsigned>(data.size());
    std::vector<unsigned char> h;
    put32(h, 0x04034b50); put16(h, 20); put16(h, 0x0800); put16(h, 0); put16(h, dosTime); put16(h, dosDate);
    put32(h, crc); put32(h, size); put32(h, size); put16(h, static_cast<unsigned>(name.size())); put16(h, 0);
    ok = fwrite(h.data(), 1, h.size(), out) == h.size() && fwrite(name.data(), 1, name.size(), out) == name.size() &&
         (data.empty() || fwrite(data.data(), 1, data.size(), out) == data.size());
    put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0x0800); put16(central, 0);
    put16(central, dosTime); put16(central, dosDate); put32(central, crc); put32(central, size); put32(central, size);
    put16(central, static_cast<unsigned>(name.size())); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
    put32(central, 0); put32(central, offset);
    central.insert(central.end(), name.begin(), name.end());
    offset += static_cast<unsigned>(h.size() + name.size() + data.size());
    ++count;
  }
  std::vector<unsigned char> end;
  put32(end, 0x06054b50); put16(end, 0); put16(end, 0); put16(end, count); put16(end, count);
  put32(end, static_cast<unsigned>(central.size())); put32(end, offset); put16(end, 0);
  ok = ok && fwrite(central.data(), 1, central.size(), out) == central.size() && fwrite(end.data(), 1, end.size(), out) == end.size();
  ok = fclose(out) == 0 && ok;
  return ok;
}

// Oldest unmarked report zips beyond `keep` are deleted (only report-*.zip files written here).
void rotate() {
  std::vector<fs::path> zips;
  std::error_code ec;
  for (auto it = fs::directory_iterator(g_reportsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::wstring n = it->path().filename().wstring();
    if (!_wcsnicmp(n.c_str(), L"report-", 7) && !_wcsicmp(it->path().extension().wstring().c_str(), L".zip") &&
        n.find(L"-marked") == std::wstring::npos)
      zips.push_back(it->path());
  }
  std::sort(zips.begin(), zips.end());                     // names start with the date and time
  for (size_t i = 0; i + g_keep < zips.size(); ++i) fs::remove(zips[i], ec);
}

void finalize(Job job) {
  std::lock_guard<std::mutex> lock(g_jobs);
  const fs::path dir = job.dir;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  write_text((dir / L"summary.txt").wstring(), job.unfinished
      ? L"UNFINISHED: the game closed or crashed during this stage (finished at the next start). perf.tsv, events.tsv "
        L"and problems.txt hold what was written until then.\n" : job.summary);
  write_text((dir / L"system.txt").wstring(), system_info());
  wchar_t cfg[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_REPORT_CONFIG_UNSET", L"x", cfg, MAX_PATH * 2);
  for (auto it = fs::directory_iterator(fs::path(cfg).parent_path(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::wstring ext = it->path().extension().wstring();
    if (!_wcsicmp(ext.c_str(), L".ini") || !_wcsicmp(ext.c_str(), L".conf"))
      copy_small(it->path(), dir / L"config" / it->path().filename());
  }
  for (const wchar_t* env : { L"DXVK_RTX_CONFIG_FILE", L"DXVK_CONFIG_FILE" }) {
    const std::wstring p = env_path(env);
    if (!p.empty() && !fs::exists(dir / L"config" / fs::path(p).filename(), ec)) copy_small(p, dir / L"config" / fs::path(p).filename());
  }
  copy_small(fs::path(exe_dir()) / L"user.conf", dir / L"config" / L"user.conf (Alt+X saved settings)");
  std::wstring extra = L"\n==== Remix and bridge logs: errors and warnings\n" + remix_problems();
  const std::wstring logDir = env_path(L"RBRRTX_LOG_DIR");
  if (!logDir.empty()) {
    if (FILE* f = open_text(logDir + L"\\rbrrtx_exceptions.log", L"r")) {
      std::vector<std::string> tail;
      char buf[1024];
      while (fgets(buf, sizeof(buf), f)) { tail.push_back(buf); if (tail.size() > 100) tail.erase(tail.begin()); }
      fclose(f);
      extra += L"\n==== rbrrtx_exceptions.log (last 100 lines)\n";
      for (const auto& l : tail) extra += std::wstring(l.begin(), l.end());
    }
  }
  if (FILE* f = open_text((dir / L"problems.txt").wstring(), L"a, ccs=UTF-8")) { fputws(scrub(extra).c_str(), f); fclose(f); }
  for (const auto& s : job.sweeps) {
    const fs::path sd = s;
    for (auto it = fs::directory_iterator(sd, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      const std::wstring ext = it->path().extension().wstring();
      if (!_wcsicmp(ext.c_str(), L".txt") || !_wcsicmp(ext.c_str(), L".tsv"))
        copy_small(it->path(), dir / sd.filename() / it->path().filename());
    }
  }
  const fs::path zip = fs::path(g_reportsDir) / (job.zipName + L".zip"), tmp = fs::path(g_reportsDir) / (job.zipName + L".zip.tmp");
  if (zip_dir(dir, tmp) && (fs::rename(tmp, zip, ec), !ec)) {
    fs::remove_all(dir, ec);
    rotate();
  }
}

// ---------------------------------------------------------------- recording (game thread)

void ev(const wchar_t* text) {
  if (!g_rec.events) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  g_rec.lastEvent = now;
  fwprintf(g_rec.events, L"%.1f\t%s\n", secs(g_rec.start, now), scrub(text).c_str());
}

const wchar_t* mode_name(int m) {
  return m == 1 ? L"driving" : m == 2 ? L"pause" : m == 3 ? L"menu" : m == 5 ? L"loading" : m == 8 ? L"replay" : m == 10 ? L"pre-stage" : L"other";
}

void start(const RbrStageInfo& si, const LARGE_INTEGER& now) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::wstring stage;
  for (const wchar_t* c = si.name; *c && stage.size() < 32; ++c)
    if (iswalnum(*c)) stage += *c;
    else if (!stage.empty() && stage.back() != L'-') stage += L'-';
  while (!stage.empty() && stage.back() == L'-') stage.pop_back();
  wchar_t name[96];
  swprintf_s(name, L"report-%04u%02u%02u-%02u%02u%02u-%s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             stage.empty() ? L"stage" : stage.c_str());
  g_rec = Rec();
  g_rec.hitches.reserve(256);
  g_rec.name = name;
  g_rec.dir = g_reportsDir + L"\\" + name;
  std::error_code ec;
  fs::create_directories(g_rec.dir, ec);
  g_rec.perf = open_text(g_rec.dir + L"\\perf.tsv");
  g_rec.events = open_text(g_rec.dir + L"\\events.tsv");
  g_rec.problems = open_text(g_rec.dir + L"\\problems.txt");
  if (!g_rec.perf || !g_rec.events || !g_rec.problems) {
    for (FILE* f : { g_rec.perf, g_rec.events, g_rec.problems }) if (f) fclose(f);
    g_rec = Rec();
    rbrrtx_log_to(L"rbrrtx_comp.log", L"report: cannot write to %s, stage not recorded", name);
    return;
  }
  g_rec.on = true;
  g_rec.trackId = si.trackId;
  g_rec.mode = si.gameMode;
  g_rec.start = g_rec.last = now;
  g_rec.stage = rbrrtx_stage_line(si);
  fwprintf(g_rec.perf, L"second\tclock\tgame mode\tframes\tfps\tavg ms\tworst ms\tdraws\ttransformed\tfixed-function\tBTB\tcar x\tcar y\tcar z\n");
  fwprintf(g_rec.events, L"second\tevent\n");
  fwprintf(g_rec.problems, L"==== RBR-RTX log warnings (second\tline; before = logged before the stage)\n");
  for (const auto& l : g_early) fwprintf(g_rec.problems, L"before\t%s\n", l.c_str());
  g_rec.problemLines = static_cast<unsigned>(g_early.size());
  for (const auto& l : g_early) if (l.find(L"not recognised") != std::wstring::npos) g_rec.shadersUnknown = true;
  g_early.clear();
  write_text(g_rec.dir + L"\\stage.txt", L"RBR-RTX stage report " + g_rec.name + L"\n" + g_rec.stage + L"\n");
  ev((L"stage loaded: " + g_rec.stage).c_str());
  rbrrtx_log_to(L"rbrrtx_comp.log", L"report: recording %s", name);
}

std::wstring summary(double total) {
  std::wstring r = L"RBR-RTX stage report " + g_rec.name + L"\n" + g_rec.stage + L"\n\n";
  wchar_t s[512];
  const double driving = g_rec.sum / 1000.0;
  double p99 = 0, p999 = 0;
  unsigned long long cum = 0;
  for (int i = 0; i < kHist && g_rec.frames; ++i) {
    cum += g_rec.hist[i];
    if (!p99 && cum >= g_rec.frames * 99 / 100) p99 = (i + 1) * 0.5;
    if (!p999 && cum >= g_rec.frames * 999 / 1000) p999 = (i + 1) * 0.5;
  }
  std::vector<Hitch> drive;
  size_t afterHotkey = 0;
  for (const auto& h : g_rec.hitches)
    if (h.mode == 1 && h.event) ++afterHotkey;
    else if (h.mode == 1) drive.push_back(h);
  const double perMin = driving > 0 ? drive.size() * 60.0 / driving : 0.0;
  const double fps = g_rec.sum > 0 ? g_rec.frames * 1000.0 / g_rec.sum : 0.0;
  swprintf_s(s, L"recorded %.0f s, driving %.0f s (pause, replay and loading not counted below)\n", total, driving);
  r += s;
  if (g_rec.frames) {
    swprintf_s(s, L"fps: average %.1f, 1%% low %.1f, 0.1%% low %.1f; worst frame %.0f ms at second %.0f\n", fps,
               p99 > 0 ? 1000.0 / p99 : 0.0, p999 > 0 ? 1000.0 / p999 : 0.0, g_rec.worst, g_rec.worstT);
    r += s;
  }
  swprintf_s(s, L"hitches while driving: %zu (%.1f per minute; a hitch = a frame over 3x the last second's average and "
                L"over 25 ms, or over 100 ms)\n", drive.size(), perMin);
  r += s;
  if (afterHotkey) {
    swprintf_s(s, L"  + %zu within 1 s after a hotkey or event (not counted here or in fps; see hitches.tsv)\n", afterHotkey);
    r += s;
  }
  std::sort(drive.begin(), drive.end(), [](const Hitch& a, const Hitch& b) { return a.ms > b.ms; });
  for (size_t i = 0; i < drive.size() && i < 5; ++i) {
    swprintf_s(s, L"  %.0f ms at second %.1f, car (%.0f, %.0f, %.0f)%s%s\n", drive[i].ms, drive[i].t, drive[i].pos[0],
               drive[i].pos[1], drive[i].pos[2], drive[i].creates ? L", textures/shaders created" : L"",
               L"");
    r += s;
  }
  swprintf_s(s, L"problem lines: %u (problems.txt); marked problems (key 9): %u\n", g_rec.problemLines, g_rec.marks);
  r += s;
  for (const auto& m : g_rec.markLines) r += L"  " + m + L"\n";
  r += L"\nverdict:\n";
  const size_t len = r.size();
  if (g_rec.btb) r += L"- BTB stage (Bob's Track Builder): not supported with ray tracing yet; switch ray tracing off (key 0) on it\n";
  if (g_rec.shadersUnknown) r += L"- game shaders not recognised: another mod changes RBR's shaders (see problems.txt)\n";
  if (g_rec.frames && fps < 40.0) r += L"- low frame rate\n";
  if (perMin > 2.0) r += L"- stutter (frequent hitches)\n";
  if (g_rec.marks) r += L"- the player marked a problem: see events.tsv, mark-N.png, burst-N.tsv\n";
  if (r.size() == len) r += L"- no problems detected\n";
  return r;
}

void stop(const wchar_t* why, const LARGE_INTEGER& now) {
  const double total = secs(g_rec.start, now);
  ev(why);
  if (!g_rec.hitches.empty()) {
    if (FILE* f = open_text(g_rec.dir + L"\\hitches.tsv")) {
      fwprintf(f, L"second\tms\tgame mode\tcar x\tcar y\tcar z\ttextures/shaders created\thotkey or event\n");
      for (const auto& h : g_rec.hitches)
        fwprintf(f, L"%.2f\t%.1f\t%s\t%.1f\t%.1f\t%.1f\t%u\t%s\n", h.t, h.ms, mode_name(h.mode), h.pos[0], h.pos[1], h.pos[2],
                 h.creates, h.event ? L"yes" : L"");
      fclose(f);
    }
  }
  for (FILE* f : { g_rec.perf, g_rec.events, g_rec.problems }) if (f) fclose(f);
  Job job;
  job.dir = g_rec.dir;
  job.zipName = g_rec.name + (g_rec.marks ? L"-marked" : L"");
  job.summary = summary(total);
  job.sweeps = g_rec.sweeps;
  rbrrtx_log_to(L"rbrrtx_comp.log", L"report: %s (%s), zipping in the background to %s\\%s.zip", g_rec.name.c_str(), why,
                g_reportsDir.c_str(), job.zipName.c_str());
  g_rec = Rec();
  std::thread(finalize, std::move(job)).detach();
}

void write_burst() {
  wchar_t path[MAX_PATH * 2];
  swprintf_s(path, L"%s\\burst-%u.tsv", g_rec.dir.c_str(), g_rec.marks);
  if (FILE* f = open_text(path)) {
    fwprintf(f, L"frame (0 = key 9 pressed)\tms\n");
    const unsigned long long n = (std::min<unsigned long long>)(kRing, g_rec.ringFrames);
    for (unsigned long long i = 0; i < n; ++i) {
      const unsigned long long frame = g_rec.ringFrames - n + i;           // ring index of that frame = frame % kRing
      fwprintf(f, L"%lld\t%.2f\n", static_cast<long long>(frame) - static_cast<long long>(g_rec.markFrame), g_rec.ring[frame % kRing]);
    }
    fclose(f);
  }
  g_rec.burstAt = 0;
}

void mark(const LARGE_INTEGER& now, const RbrReportFrame& f) {
  ++g_rec.marks;
  MessageBeep(MB_OK);
  wchar_t line[256];
  swprintf_s(line, L"PLAYER MARKED A PROBLEM (mark %u) at second %.1f, car (%.0f, %.0f, %.0f), %s", g_rec.marks,
             secs(g_rec.start, now), f.carPos[0], f.carPos[1], f.carPos[2], mode_name(g_rec.mode));
  ev(line);
  g_rec.markLines.push_back(line);
  rbrrtx_log_to(L"rbrrtx_comp.log", L"report: %s", line);
  if (g_rec.burstAt) write_burst();                        // a second mark before the first burst was written
  g_rec.markFrame = g_rec.ringFrames;
  g_rec.burstAt = g_rec.ringFrames + kBurstAfter;
  if (static_cast<int>(g_rec.shots) < g_maxShots) {
    ++g_rec.shots;
    wchar_t png[MAX_PATH * 2];
    swprintf_s(png, L"%s\\mark-%u.png", g_rec.dir.c_str(), g_rec.marks);
    rbrrtx_screenshot(g_wnd, png, nullptr);                // ~20-50 ms stall: only on the player's own key press
    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);
    g_rec.last.QuadPart += after.QuadPart - now.QuadPart;  // the screenshot's stall is not a game hitch
  }
}

// Once a second: stage start/stop, game mode, one perf line.
void second(const LARGE_INTEGER& now, const RbrReportFrame& f) {
  RbrStageInfo si = {};
  const bool ok = rbrrtx_stage_info(&si);
  const bool inStage = ok && si.trackId > 0 &&
                       (si.gameMode == 1 || si.gameMode == 2 || si.gameMode == 5 || si.gameMode == 8 || si.gameMode == 10);
  if (g_rec.on && (!inStage || si.trackId != g_rec.trackId)) stop(inStage ? L"another stage loaded" : L"stage left", now);
  if (!g_rec.on) {
    if (inStage) start(si, now);
    return;
  }
  if (si.gameMode != g_rec.mode) {
    wchar_t s[96];
    swprintf_s(s, L"game mode %s -> %s", mode_name(g_rec.mode), mode_name(si.gameMode));
    ev(s);
    g_rec.mode = si.gameMode;
  }
  if (g_rec.secFrames) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const double n = g_rec.secFrames;
    fwprintf(g_rec.perf, L"%.0f\t%02u:%02u:%02u\t%s\t%u\t%.1f\t%.2f\t%.1f\t%.0f\t%.0f\t%.0f\t%.0f\t%.1f\t%.1f\t%.1f\n",
             secs(g_rec.start, now), t.wHour, t.wMinute, t.wSecond, mode_name(g_rec.mode), g_rec.secFrames,
             g_rec.secSum > 0 ? n * 1000.0 / g_rec.secSum : 0.0, g_rec.secSum / n, g_rec.secMax, g_rec.secDraws / n,
             g_rec.secXf / n, g_rec.secFf / n, g_rec.secBtb / n, f.carPos[0], f.carPos[1], f.carPos[2]);
    g_rec.prevAvg = g_rec.secSum / n;
  }
  g_rec.secFrames = 0;
  g_rec.secSum = g_rec.secMax = 0;
  g_rec.secDraws = g_rec.secXf = g_rec.secFf = g_rec.secBtb = 0;
  if (++g_rec.flushTick % 5 == 0)                          // a crash loses at most the last 5 s
    for (FILE* fl : { g_rec.perf, g_rec.events, g_rec.problems }) fflush(fl);
}

void init() {
  g_init = true;
  load_config();
  QueryPerformanceFrequency(&g_qpf);
  g_thread = GetCurrentThreadId();
  if (!g_enabled) { rbrrtx_log_to(L"rbrrtx_comp.log", L"report: off (report.ini enabled = 0)"); return; }
  std::error_code ec;
  fs::create_directories(g_reportsDir, ec);
  for (auto it = fs::directory_iterator(g_reportsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (!it->is_directory(ec) || _wcsnicmp(it->path().filename().c_str(), L"report-", 7)) continue;
    Job job;                                               // left by a crash or a quit from the stage
    job.dir = it->path().wstring();
    job.zipName = it->path().filename().wstring() + L"-unfinished";
    job.unfinished = true;
    std::thread(finalize, std::move(job)).detach();
  }
  rbrrtx_log_to(L"rbrrtx_comp.log", L"report: on, stage reports go to %s (keep %d, key %s marks a problem)", g_reportsDir.c_str(),
                g_keep, rbrrtx_key_name(RBRKEY_REPORT));
}

}  // namespace

void rbrrtx_report_frame(HWND wnd, const RbrReportFrame& f) {
  if (!g_init) init();
  if (!g_enabled) return;
  if (wnd) g_wnd = wnd;
  const bool marked = rbrrtx_key_pressed(RBRKEY_REPORT);   // every frame: the key code needs it for press edges
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (now.QuadPart - g_lastCheck.QuadPart >= g_qpf.QuadPart) { g_lastCheck = now; second(now, f); }
  if (!g_rec.on || g_rec.last.QuadPart == now.QuadPart) return;   // not recording, or started this frame
  const float ms = static_cast<float>(secs(g_rec.last, now) * 1000.0);
  g_rec.last = now;
  g_rec.ring[g_rec.ringFrames++ % kRing] = ms;
  ++g_rec.secFrames;
  g_rec.secSum += ms;
  if (ms > g_rec.secMax) g_rec.secMax = ms;
  g_rec.secDraws += f.draws;
  g_rec.secXf += f.xformed;
  g_rec.secFf += f.ff;
  g_rec.secBtb += f.btb;
  if (f.btb) g_rec.btb = true;
  // Within 1 s after a hotkey or event (debug sun, ray tracing toggle: ~70 ms each, run 20261022-032807) a slow frame
  // is the player's doing, not the stage's: kept in hitches.tsv, left out of the fps and stutter numbers.
  const bool nearEvent = g_rec.event || now.QuadPart - g_rec.lastEvent.QuadPart < g_qpf.QuadPart;
  if (g_rec.mode == 1 && !nearEvent) {
    ++g_rec.hist[(std::min)(static_cast<int>(ms * 2.0f), kHist - 1)];
    ++g_rec.frames;
    g_rec.sum += ms;
    if (ms > g_rec.worst) { g_rec.worst = ms; g_rec.worstT = secs(g_rec.start, now); }
  }
  const unsigned creates = g_rbrrtxCreates - g_rec.lastCreates;
  g_rec.lastCreates = g_rbrrtxCreates;
  if ((ms > 100.0f || (g_rec.prevAvg > 0 && ms > 3.0 * g_rec.prevAvg && ms > 25.0f)) && g_rec.hitches.size() < kMaxHitches)
    g_rec.hitches.push_back({ secs(g_rec.start, now), ms, { f.carPos[0], f.carPos[1], f.carPos[2] }, g_rec.mode, creates, nearEvent });
  g_rec.event = false;
  if (g_rec.burstAt && g_rec.ringFrames >= g_rec.burstAt) write_burst();
  if (marked) mark(now, f);
}

void rbrrtx_report_event(const wchar_t* text) {
  if (!g_rec.on || GetCurrentThreadId() != g_thread) return;
  g_rec.event = true;
  ev(text);
}

void rbrrtx_report_log_line(const wchar_t* line) {
  if (!g_enabled || !g_init || GetCurrentThreadId() != g_thread || !is_problem(line) || !wcsncmp(line, L"report:", 7)) return;
  const std::wstring l = scrub(line);
  if (!g_rec.on) {
    if (g_early.size() < 50) g_early.push_back(l);
    return;
  }
  if (l.find(L"not recognised") != std::wstring::npos) g_rec.shadersUnknown = true;
  ++g_rec.problemLines;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  fwprintf(g_rec.problems, L"%.1f\t%s\n", secs(g_rec.start, now), l.c_str());
}

void rbrrtx_report_sweep(const wchar_t* dir) {
  if (!g_rec.on || !dir || !dir[0]) return;
  g_rec.sweeps.push_back(dir);
  ev((L"Alt+K sweep written: " + std::wstring(dir)).c_str());
}
