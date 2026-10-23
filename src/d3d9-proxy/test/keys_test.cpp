// Test of keys.cpp parsing: builds keys.cpp with a printing log stub, loads keys.ini files given by RBRRTX_KEYS.
//   x86 cl test\keys_test.cpp -> keys_test.exe [--shipped=<package keys.ini>] [other keys.ini ...]
#include "../keys.cpp"
#include <cstdarg>
#include <cstdio>

void rbrrtx_log_to(const wchar_t*, const wchar_t* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vwprintf(fmt, a);
  va_end(a);
  wprintf(L"\n");
}

int check(const wchar_t* in, int want) {
  int got = parse_key(in);
  if (got != want) { wprintf(L"FAIL parse_key(\"%s\") = %d, want %d\n", in, got, want); return 1; }
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  int fails = 0;
  fails += check(L"P", 'P');
  fails += check(L" p ", 'P');
  fails += check(L"7", '7');
  fails += check(L"F5", VK_F5);
  fails += check(L"f12", VK_F12);
  fails += check(L"0x41", 0x41);
  fails += check(L"none", 0);
  fails += check(L"Quote", VK_OEM_7);
  fails += check(L"leftbracket", VK_OEM_4);
  fails += check(L"[", VK_OEM_4);
  fails += check(L"ScrollLock", VK_SCROLL);
  fails += check(L"Numpad5", VK_NUMPAD5);
  fails += check(L"garbage", -1);
  fails += check(L"", -1);
  fails += check(L"Alt+H", 0x1000 | 'H');
  fails += check(L"alt+F5", 0x1000 | VK_F5);
  fails += check(L"Alt+", -1);
  fails += check(L"Alt+Alt+H", -1);
  int defaults[RBRKEY_COUNT];
  for (int i = 0; i < RBRKEY_COUNT; ++i) defaults[i] = g_actions[i].vk;
  for (int i = 1; i < argc; ++i) {           // each argument: a keys.ini to load and print
    const bool shipped = !wcsncmp(argv[i], L"--shipped=", 10);   // the package keys.ini: must equal the defaults
    SetEnvironmentVariableW(L"RBRRTX_KEYS", shipped ? argv[i] + 10 : argv[i]);
    rbrrtx_keys_load();
    if (shipped)
      for (int k = 0; k < RBRKEY_COUNT; ++k)
        if (g_actions[k].vk != defaults[k]) {
          wprintf(L"FAIL default %s = 0x%X, shipped keys.ini = 0x%X\n", g_actions[k].ini, defaults[k], g_actions[k].vk);
          ++fails;
        }
    if (shipped)                             // no two actions on one key (sun moved to Alt+L for the headlights' L)
      for (int a = 0; a < RBRKEY_COUNT; ++a)
        for (int b = a + 1; b < RBRKEY_COUNT; ++b)
          if (g_actions[a].vk && g_actions[a].vk == g_actions[b].vk) {
            wprintf(L"FAIL %s and %s share a key\n", g_actions[a].ini, g_actions[b].ini);
            ++fails;
          }
  }
  wprintf(fails ? L"%d FAILED\n" : L"parse checks passed\n", fails);
  return fails;
}
