// Hotkeys of the comp layer, configurable in rbr-rtx\config\keys.ini (env RBRRTX_KEYS = another file).
//
// Plain keys in the H..P area by default (user, 2026-10-17): compact keyboards have no Scroll Lock / Pause, and a
// tester's on-screen keyboard lost the game's focus. A key only counts while the game window has the focus (the
// proxy reads the keyboard globally, so typing elsewhere must not toggle anything) and while neither Ctrl nor Alt
// is held (Alt+X opens Remix; other plugins use combos). Quick taps between two frames are caught too.
// Exception: a key written as Alt+<key> fires only while Alt is held (sun brightness Alt+H / Alt+N, user 2026-10-17).
//
// keys.ini:  [keys]  <action> = <key>   key = a letter or digit, F1..F24, a name from kNames below, 0x<VK hex>,
//            optionally with an Alt+ prefix, or none (action off). Missing file or entry = default.
#include "common.hpp"
#include <cwchar>
#include <cwctype>

namespace {

struct Action { const wchar_t* ini; int vk; const wchar_t* label; };
constexpr int kAlt = 0x1000;                                 // in Action::vk: the key needs Alt held

// Order = RbrKey enum in common.hpp.
Action g_actions[RBRKEY_COUNT] = {
  {L"frame_dump", 'P', L"frame dump"},
  {L"layer", 'O', L"RBR-RTX layer"},
  {L"sun", 'L', L"Remix sun"},
  {L"foliage", 'J', L"foliage mode"},
  {L"dust", 'U', L"dust"},
  {L"windscreen", 'I', L"windscreen"},
  {L"sky", 'N', L"sky mode"},
  {L"debug_sun", 'H', L"debug sun"},
  {L"debug_sun_left", VK_OEM_4, L"debug sun turn left"},     // [
  {L"debug_sun_right", VK_OEM_6, L"debug sun turn right"},   // ]
  {L"debug_sun_down", VK_OEM_1, L"debug sun lower"},         // ;
  {L"debug_sun_up", VK_OEM_7, L"debug sun raise"},           // '
  {L"experiment", 'K', L"performance experiment run"},
  {L"culling", 'M', L"distance culling on/off"},
  {L"sun_brighter", kAlt | 'H', L"sun brightness +10 %"},
  {L"sun_dimmer", kAlt | 'N', L"sun brightness -10 %"},
};
bool g_prev[RBRKEY_COUNT] = {};
wchar_t g_names[RBRKEY_COUNT][24] = {};

struct Name { const wchar_t* name; int vk; };
const Name kNames[] = {
  {L"LeftBracket", VK_OEM_4}, {L"[", VK_OEM_4}, {L"RightBracket", VK_OEM_6}, {L"]", VK_OEM_6},
  {L"Semicolon", VK_OEM_1}, {L";", VK_OEM_1}, {L"Quote", VK_OEM_7}, {L"'", VK_OEM_7},
  {L"Comma", VK_OEM_COMMA}, {L",", VK_OEM_COMMA}, {L"Period", VK_OEM_PERIOD}, {L".", VK_OEM_PERIOD},
  {L"Slash", VK_OEM_2}, {L"/", VK_OEM_2}, {L"Backslash", VK_OEM_5}, {L"\\", VK_OEM_5},
  {L"Minus", VK_OEM_MINUS}, {L"-", VK_OEM_MINUS}, {L"Equals", VK_OEM_PLUS}, {L"=", VK_OEM_PLUS},
  {L"Backquote", VK_OEM_3}, {L"`", VK_OEM_3},
  {L"ScrollLock", VK_SCROLL}, {L"Pause", VK_PAUSE}, {L"Home", VK_HOME}, {L"End", VK_END},
  {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE}, {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT},
  {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT}, {L"Right", VK_RIGHT},
  {L"Numpad0", VK_NUMPAD0}, {L"Numpad1", VK_NUMPAD1}, {L"Numpad2", VK_NUMPAD2}, {L"Numpad3", VK_NUMPAD3},
  {L"Numpad4", VK_NUMPAD4}, {L"Numpad5", VK_NUMPAD5}, {L"Numpad6", VK_NUMPAD6}, {L"Numpad7", VK_NUMPAD7},
  {L"Numpad8", VK_NUMPAD8}, {L"Numpad9", VK_NUMPAD9},
};

// -1 = not understood, 0 = none (action off).
int parse_key(const wchar_t* s) {
  while (*s == L' ' || *s == L'\t') ++s;
  wchar_t v[32] = {};
  wcsncpy_s(v, s, _TRUNCATE);
  for (size_t n = wcslen(v); n > 0 && (v[n - 1] == L' ' || v[n - 1] == L'\t'); --n) v[n - 1] = 0;
  if (!v[0]) return -1;
  if (!_wcsicmp(v, L"none")) return 0;
  if (!_wcsnicmp(v, L"Alt+", 4)) {
    const int k = parse_key(v + 4);
    return k > 0 && k < kAlt ? (kAlt | k) : -1;
  }
  if (!v[1] && iswalnum(v[0])) return static_cast<int>(towupper(v[0]));   // A..Z, 0..9 = their VK codes
  if ((v[0] == L'F' || v[0] == L'f') && iswdigit(v[1])) {
    int n = _wtoi(v + 1);
    if (n >= 1 && n <= 24) return VK_F1 + n - 1;
  }
  if (v[0] == L'0' && (v[1] == L'x' || v[1] == L'X')) {
    int vk = static_cast<int>(wcstol(v + 2, nullptr, 16));
    if (vk > 0 && vk < 256) return vk;
  }
  for (const Name& n : kNames) if (!_wcsicmp(v, n.name)) return n.vk;
  return -1;
}

void name_of(int vk, wchar_t* out, size_t size) {
  if (vk & kAlt) {
    wcscpy_s(out, size, L"Alt+");
    name_of(vk & 0xFF, out + 4, size - 4);
    return;
  }
  if (vk == 0) { wcscpy_s(out, size, L"none"); return; }
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { swprintf_s(out, size, L"%c", vk); return; }
  if (vk >= VK_F1 && vk <= VK_F24) { swprintf_s(out, size, L"F%d", vk - VK_F1 + 1); return; }
  for (const Name& n : kNames) if (n.vk == vk && n.name[1]) { wcscpy_s(out, size, n.name); return; }   // long names
  swprintf_s(out, size, L"0x%02X", vk);
}

bool game_has_focus() {
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  if (w) GetWindowThreadProcessId(w, &pid);
  return pid == GetCurrentProcessId();
}

}  // namespace

void rbrrtx_config_path(const wchar_t* env, const wchar_t* file, wchar_t* out, size_t size) {
  DWORD n = GetEnvironmentVariableW(env, out, static_cast<DWORD>(size));
  if (n > 0 && n < size) return;
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&rbrrtx_config_path), &self);
  wchar_t dir[MAX_PATH] = {};
  DWORD len = GetModuleFileNameW(self, dir, MAX_PATH);
  wchar_t* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(dir, L'\\') : nullptr;
  if (slash) *slash = 0;
  swprintf_s(out, size, L"%s\\rbr-rtx\\config\\%s", dir, file);
}

void rbrrtx_keys_load() {
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_KEYS", L"keys.ini", path, MAX_PATH * 2);
  const bool found = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
  wchar_t summary[1024] = {};
  for (int i = 0; i < RBRKEY_COUNT; ++i) {
    if (found) {
      wchar_t v[32] = {};
      GetPrivateProfileStringW(L"keys", g_actions[i].ini, L"", v, 32, path);
      if (v[0]) {
        int vk = parse_key(v);
        if (vk >= 0) g_actions[i].vk = vk;
        else rbrrtx_log_to(L"rbrrtx_comp.log", L"keys.ini: %s = \"%s\" not understood, default kept", g_actions[i].ini, v);
      }
    }
    name_of(g_actions[i].vk, g_names[i], 24);
    wchar_t part[64];
    swprintf_s(part, L"%s%s=%s", i ? L", " : L"", g_actions[i].ini, g_names[i]);
    wcscat_s(summary, part);
  }
  rbrrtx_log_to(L"rbrrtx_comp.log", L"keys (%s): %s", found ? path : L"defaults, no keys.ini", summary);
}

// Ctrl never held; Alt held exactly when the key asks for it.
bool modifiers_ok(int code) {
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) return false;
  return ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) == ((code & kAlt) != 0);
}

bool rbrrtx_key_pressed(int action) {
  if (action < 0 || action >= RBRKEY_COUNT || !g_actions[action].vk) return false;
  const SHORT s = GetAsyncKeyState(g_actions[action].vk & 0xFF);
  const bool down = (s & 0x8000) != 0;
  // Edge, or a tap that started and ended between two frames (low bit = pressed since the last query).
  const bool fire = !g_prev[action] && (down || (s & 1));
  g_prev[action] = down;
  if (!fire || !game_has_focus()) return false;
  return modifiers_ok(g_actions[action].vk);
}

bool rbrrtx_key_held(int action) {
  if (action < 0 || action >= RBRKEY_COUNT || !g_actions[action].vk) return false;
  if (!(GetAsyncKeyState(g_actions[action].vk & 0xFF) & 0x8000) || !game_has_focus()) return false;
  return modifiers_ok(g_actions[action].vk);
}

const wchar_t* rbrrtx_key_name(int action) {
  return (action >= 0 && action < RBRKEY_COUNT) ? g_names[action] : L"?";
}
