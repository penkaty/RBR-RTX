// Test of report.cpp: builds it with stubs for the game, keys and screenshot, drives ~3 s of fake frames on a fake
// stage (one 150 ms hitch, one key-9 mark, one warning), leaves the stage and checks the zip (listed with tar).
// Also: a folder left by a "crash" is zipped as -unfinished, and old zips beyond report.ini keep are deleted.
//   x86 cl /EHsc /std:c++17 test\report_test.cpp user32.lib gdi32.lib advapi32.lib -> report_test.exe <empty temp folder>
#define UNICODE
#define _UNICODE
#include "../report.cpp"
#include <cstdarg>

int g_track = 7, g_mode = 1;
bool g_press = false;

void rbrrtx_log_to(const wchar_t* file, const wchar_t* fmt, ...) {
  wchar_t line[2048];
  va_list a;
  va_start(a, fmt);
  _vsnwprintf_s(line, _TRUNCATE, fmt, a);
  va_end(a);
  wprintf(L"[%s] %s\n", file, line);
  rbrrtx_report_log_line(line);
}
void rbrrtx_config_path(const wchar_t*, const wchar_t* file, wchar_t* out, size_t size) {
  wchar_t dir[MAX_PATH] = {};
  GetEnvironmentVariableW(L"TEST_ROOT", dir, MAX_PATH);
  swprintf_s(out, size, L"%s\\config\\%s", dir, file);
}
bool rbrrtx_key_pressed(int action) {
  const bool r = action == RBRKEY_REPORT && g_press;
  if (r) g_press = false;
  return r;
}
const wchar_t* rbrrtx_key_name(int) { return L"9"; }
bool rbrrtx_stage_info(RbrStageInfo* out) {
  *out = {};
  out->trackId = g_track;
  out->gameMode = g_mode;
  wcscpy_s(out->name, L"Test Stage Ruuhim\u00e4ki");
  return true;
}
std::wstring rbrrtx_stage_line(const RbrStageInfo& si) { return L"stage " + std::to_wstring(si.trackId) + L" (test)"; }
bool rbrrtx_screenshot(HWND, const wchar_t* path, double*) {
  write_text(path, L"not a png");
  return true;
}
const wchar_t* rbrrtx_backend_path() { return L"C:\\Windows\\System32\\d3d9.dll"; }

int fails = 0;
void expect(bool ok, const wchar_t* what) {
  wprintf(L"%s %s\n", ok ? L"ok  " : L"FAIL", what);
  if (!ok) ++fails;
}

void frames(double seconds, int sleepMs) {
  LARGE_INTEGER f, a, b;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&a);
  do {
    Sleep(sleepMs);
    rbrrtx_report_frame(nullptr, { 1000, 900, 50, 0, { 1.0f, 2.0f, 3.0f } });
    QueryPerformanceCounter(&b);
  } while ((b.QuadPart - a.QuadPart) < seconds * f.QuadPart);
}

std::vector<fs::path> zips(const fs::path& dir, const wchar_t* part) {
  std::vector<fs::path> r;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    if (it->path().extension() == L".zip" && it->path().filename().wstring().find(part) != std::wstring::npos) r.push_back(it->path());
  return r;
}

std::string run(const std::wstring& cmd) {
  std::string out;
  if (FILE* p = _wpopen(cmd.c_str(), L"r")) {
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    _pclose(p);
  }
  return out;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { wprintf(L"usage: report_test <empty temp folder>\n"); return 2; }
  const fs::path root = argv[1];
  const fs::path reports = root / L"logs" / L"reports";
  std::error_code ec;
  fs::create_directories(root / L"config", ec);
  fs::create_directories(reports / L"report-20000101-000000-crashed", ec);
  write_text((reports / L"report-20000101-000000-crashed" / L"perf.tsv").wstring(), L"second\n");
  for (int i = 0; i < 22; ++i) write_text((reports / (L"report-1999" + std::to_wstring(1000 + i) + L"-old.zip")).wstring(), L"x");
  write_text((reports / L"report-19990101-000000-old-marked.zip").wstring(), L"x");
  write_text((root / L"config" / L"report.ini").wstring(), L"[report]\nenabled = 1\nkeep = 20\n");
  write_text((root / L"config" / L"keys.ini").wstring(), L"[keys]\nreport_mark = 9\n");
  SetEnvironmentVariableW(L"TEST_ROOT", root.c_str());
  SetEnvironmentVariableW(L"RBRRTX_LOG_DIR", (root / L"logs").c_str());

  frames(1.5, 10);
  Sleep(150);                                              // hitch
  frames(0.5, 10);
  rbrrtx_log_to(L"rbrrtx_comp.log", L"WARNING: game shaders not recognised (test)");
  rbrrtx_log_to(L"rbrrtx_comp.log", L"sun: destroyed (raster), error 0");                       // not problems
  rbrrtx_log_to(L"rbrrtx_comp.log", L"frame 5 (periodic): draws 1 [BTB 0 (0 camera candidates refused)]");
  g_press = true;                                          // key 9
  frames(2.2, 10);                                         // burst written 120 frames after the mark
  g_track = 0;
  g_mode = 3;                                              // back in the menu
  frames(1.2, 10);
  for (int i = 0; i < 100 && (zips(reports, L"-marked").size() < 2 || zips(reports, L"-unfinished").empty()); ++i) Sleep(100);
  Sleep(300);

  const auto marked = zips(reports, L"Ruuhim");
  expect(marked.size() == 1 && marked[0].filename().wstring().find(L"-marked") != std::wstring::npos, L"stage zip written, named -marked");
  expect(zips(reports, L"-unfinished").size() == 1, L"crashed folder zipped as -unfinished");
  expect(!fs::exists(reports / L"report-20000101-000000-crashed", ec), L"crashed folder removed after zipping");
  expect(zips(reports, L"-old.zip").size() <= 20 && zips(reports, L"old-marked").size() == 1, L"rotation: 20 kept, marked kept");
  if (marked.size() == 1) {
    const std::string list = run(L"tar -tf \"" + marked[0].wstring() + L"\"");
    printf("%s", list.c_str());
    for (const char* f : { "summary.txt", "stage.txt", "system.txt", "perf.tsv", "hitches.tsv", "events.tsv", "problems.txt",
                           "mark-1.png", "burst-1.tsv", "config/report.ini" })
      expect(list.find(f) != std::string::npos, std::wstring(f, f + strlen(f)).c_str());
    const std::string summary = run(L"tar -xOf \"" + marked[0].wstring() + L"\" summary.txt");
    printf("---- summary.txt\n%s", summary.c_str());
    expect(summary.find("hitches while driving: 1") != std::string::npos, L"one hitch found");
    expect(summary.find("not recognised") != std::string::npos, L"verdict: shaders not recognised");
    expect(summary.find("problem lines: 1 ") != std::string::npos, L"only the warning counted as a problem");
    expect(summary.find("marked a problem") != std::string::npos, L"verdict: marked");
    const std::string burst = run(L"tar -xOf \"" + marked[0].wstring() + L"\" burst-1.tsv");
    expect(burst.find("\n0\t") != std::string::npos && burst.find("\n119\t") != std::string::npos, L"burst holds the mark and 120 frames after");
    printf("---- system.txt\n%s", run(L"tar -xOf \"" + marked[0].wstring() + L"\" system.txt").c_str());
  }
  // Cost per frame while recording (a new stage), averaged over 200000 calls.
  g_track = 8;
  g_mode = 1;
  frames(1.1, 10);
  LARGE_INTEGER f, a, b;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&a);
  for (int i = 0; i < 200000; ++i) rbrrtx_report_frame(nullptr, { 1000, 900, 50, 0, { 1.0f, 2.0f, 3.0f } });
  QueryPerformanceCounter(&b);
  const double ns = (b.QuadPart - a.QuadPart) * 1e9 / f.QuadPart / 200000;
  wprintf(L"cost per frame while recording: %.0f ns (%.4f ms)\n", ns, ns / 1e6);
  expect(ns < 5000, L"per-frame cost under 5 microseconds");
  wprintf(fails ? L"FAILED (%d)\n" : L"PASS\n", fails);
  return fails ? 1 : 0;
}
