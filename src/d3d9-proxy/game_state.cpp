// game_state.cpp - what the player picked (stage, car, weather, time of day) and screen captures. Used by the stage
// sweep (Alt+K) and the stage atmosphere (comp.cpp).
//
// RBR globals (RichardBurnsRally_SSE.exe, offsets from NGPCarMenu src/RBRAPI.h; read with SEH, they can be stale in
// menus): RBRMapSettings 0x1660800 (+0x04 track id = RSF stage id, +0x08 car, +0x38 tyre, +0x48 weather Good/Random/
// Bad, +0x50 damage), RBRMapSettingsEx 0x8938F8 (+0x10 clouds, +0x14 wetness, +0x18 surface age, +0x38 time of day,
// +0x3C sky type), game mode at [0x7EAC48]+0x728. Stage name, country and surface come from RSF's
// rsfdata\cache\stages_data.json next to the exe (read once; missing without RSF).
#include "common.hpp"
#include <cmath>
#include <objbase.h>
#include <wincodec.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace {

int read_int(uintptr_t addr) {
  __try { return *reinterpret_cast<volatile int*>(addr); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Player car block: g_CarMovement (0x8EF660, valid in pre-stage/stage/replay; NGPCarMenu RBRAPI.h:214) +0x100,
// 20 floats: +0x100 (quaternion?), +0x110 4x4 "matrix" whose 3x3 is NOT the car's rotation (2026-10-23 01:32),
// translation at +0x140 (out[16..18]). Physics space = render world with Y and Z swapped (car (2439.09, 570.89,
// 13.16) beside the cockpit camera (2439.50, 13.78, 571.03)).
bool read_car_raw(float out[20]) {
  __try {
    const uintptr_t car = *reinterpret_cast<volatile uintptr_t*>(0x008EF660);
    if (!car) return false;
    for (int i = 0; i < 20; ++i) out[i] = *reinterpret_cast<volatile float*>(car + 0x100 + 4 * i);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  for (int i = 16; i < 19; ++i) if (!(out[i] == out[i]) || std::fabs(out[i]) > 1e6f) return false;
  return true;
}

// Brake input on the same object: +0x85C throttle, +0x860 brake, +0x864 handbrake, 0..1 (NGPCarMenu RBRAPI.h:226).
bool read_car_brake(float* brake) {
  float b;
  __try {
    const uintptr_t car = *reinterpret_cast<volatile uintptr_t*>(0x008EF660);
    if (!car) return false;
    b = *reinterpret_cast<volatile float*>(car + 0x860);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  if (!(b >= -0.01f && b <= 1.01f)) return false;
  *brake = b;
  return true;
}

int read_game_mode() {
  __try {
    const uintptr_t cfg = *reinterpret_cast<volatile uintptr_t*>(0x007EAC48);
    return cfg ? *reinterpret_cast<volatile int*>(cfg + 0x728) : -1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

struct RsfStage { std::wstring name; std::string country; int tarmac = -1, gravel = -1, snow = -1; };
std::unordered_map<int, RsfStage> g_rsf;
bool g_rsfLoaded = false;

// Value of "key":"..." inside one JSON object (flat, as RSF writes it); empty for null or missing.
std::string json_str(const std::string& obj, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  size_t p = obj.find(k);
  if (p == std::string::npos) return {};
  p += k.size();
  if (p >= obj.size() || obj[p] != '"') return {};
  std::string v;
  for (++p; p < obj.size() && obj[p] != '"'; ++p) {
    if (obj[p] == '\\' && p + 1 < obj.size()) {
      ++p;
      if (obj[p] == 'u' && p + 4 < obj.size()) {                // ó -> UTF-8
        const unsigned cp = static_cast<unsigned>(strtoul(obj.substr(p + 1, 4).c_str(), nullptr, 16));
        if (cp < 0x80) v += static_cast<char>(cp);
        else if (cp < 0x800) { v += static_cast<char>(0xC0 | (cp >> 6)); v += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { v += static_cast<char>(0xE0 | (cp >> 12)); v += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); v += static_cast<char>(0x80 | (cp & 0x3F)); }
        p += 4;
        continue;
      }
    }
    v += obj[p];
  }
  return v;
}

void load_rsf() {
  g_rsfLoaded = true;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path = exe;
  path = path.substr(0, path.find_last_of(L'\\') + 1) + L"rsfdata\\cache\\stages_data.json";
  wchar_t env[MAX_PATH];                                // test override (test\game_state_test.cpp)
  const DWORD envLen = GetEnvironmentVariableW(L"RBRRTX_RSF_STAGES", env, MAX_PATH);
  if (envLen > 0 && envLen < MAX_PATH) path = env;
  std::ifstream in(path, std::ios::binary);
  const std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  for (size_t a = s.find('{'); a != std::string::npos; a = s.find('{', a + 1)) {
    const size_t b = s.find('}', a);
    if (b == std::string::npos) break;
    const std::string obj = s.substr(a, b - a + 1);
    const std::string id = json_str(obj, "id");
    if (id.empty()) continue;
    RsfStage st;
    const std::string name = json_str(obj, "name");
    wchar_t w[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, w, 127);
    st.name = w;
    st.country = json_str(obj, "short_country");
    const std::string t = json_str(obj, "tarmac"), g = json_str(obj, "gravel"), n = json_str(obj, "snow");
    if (!t.empty()) st.tarmac = atoi(t.c_str());
    if (!g.empty()) st.gravel = atoi(g.c_str());
    if (!n.empty()) st.snow = atoi(n.c_str());
    g_rsf[atoi(id.c_str())] = st;
    a = b;
  }
  rbrrtx_log_to(L"rbrrtx_comp.log", L"stage info: %zu RSF stages from %s", g_rsf.size(), path.c_str());
}

}  // namespace

bool rbrrtx_player_car_raw(float out[20]) { return read_car_raw(out); }

bool rbrrtx_player_car_brake(float* brake) { return read_car_brake(brake); }

bool rbrrtx_stage_info(RbrStageInfo* out) {
  if (!out) return false;
  RbrStageInfo& i = *out;
  i = {};
  i.trackId = read_int(0x01660804);
  i.carId = read_int(0x01660808);
  i.tyreType = read_int(0x01660838);
  i.weatherType = read_int(0x01660848);
  i.damage = read_int(0x01660850);
  i.clouds = read_int(0x008938F8 + 0x10);
  i.wetness = read_int(0x008938F8 + 0x14);
  i.surfaceAge = read_int(0x008938F8 + 0x18);
  i.timeOfDay = read_int(0x008938F8 + 0x38);
  i.skyType = read_int(0x008938F8 + 0x3C);
  i.gameMode = read_game_mode();
  i.tarmac = i.gravel = i.snow = -1;
  if (!g_rsfLoaded) load_rsf();
  auto it = g_rsf.find(i.trackId);
  if (it != g_rsf.end()) {
    wcsncpy_s(i.name, it->second.name.c_str(), _TRUNCATE);
    strncpy_s(i.country, it->second.country.c_str(), _TRUNCATE);
    i.tarmac = it->second.tarmac; i.gravel = it->second.gravel; i.snow = it->second.snow;
  }
  return i.trackId != -1;
}

const wchar_t* rbrrtx_sky_name(int v) {
  static const wchar_t* n[] = { L"Crisp", L"Hazy", L"NoRain", L"LightRain", L"HeavyRain", L"NoSnow", L"LightSnow",
                                L"HeavySnow", L"LightFog", L"HeavyFog" };
  return v >= 0 && v < 10 ? n[v] : L"?";
}
const wchar_t* rbrrtx_cloud_name(int v) {
  static const wchar_t* n[] = { L"Clear", L"PartCloud", L"LightCloud", L"HeavyCloud" };
  return v >= 0 && v < 4 ? n[v] : L"?";
}
const wchar_t* rbrrtx_time_name(int v) {
  static const wchar_t* n[] = { L"Morning", L"Noon", L"Evening" };
  return v >= 0 && v < 3 ? n[v] : L"?";
}
const wchar_t* rbrrtx_wetness_name(int v) {
  static const wchar_t* n[] = { L"Dry", L"Damp", L"Wet" };
  return v >= 0 && v < 3 ? n[v] : L"?";
}

// GDI copy of the window from the screen: Remix's server presents into the game's window, so this is the path-traced
// image (plus RBR's HUD). PNG through WIC.
bool rbrrtx_screenshot(HWND wnd, const wchar_t* png_path, double* mean_luma) {
  if (mean_luma) *mean_luma = -1.0;
  RECT rc = {};
  if (!wnd || !GetClientRect(wnd, &rc)) return false;
  POINT org = { 0, 0 };
  ClientToScreen(wnd, &org);
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return false;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;                           // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bmp && bits) {
    HGDIOBJ old = SelectObject(mem, bmp);
    ok = BitBlt(mem, 0, 0, w, h, screen, org.x, org.y, SRCCOPY) != FALSE;
    SelectObject(mem, old);
    GdiFlush();
  }
  if (ok && mean_luma) {
    const unsigned char* p = static_cast<const unsigned char*>(bits);
    double sum = 0;
    unsigned n = 0;
    for (int y = 0; y < h; y += 8)
      for (int x = 0; x < w; x += 8, ++n) {
        const unsigned char* px = p + (static_cast<size_t>(y) * w + x) * 4;
        sum += 0.0722 * px[0] + 0.7152 * px[1] + 0.2126 * px[2];
      }
    *mean_luma = n ? sum / n : 0.0;
  }
  if (ok) {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE: already STA, fine
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
         SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(png_path, GENERIC_WRITE)) &&
         SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
         SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
         SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h));
    if (ok) {
      // PNG has no 32-bit BGR: BGRA with GDI's unused alpha byte set opaque.
      unsigned char* px = static_cast<unsigned char*>(bits);
      for (size_t k = 3; k < static_cast<size_t>(w) * h * 4; k += 4) px[k] = 0xFF;
      WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
      ok = SUCCEEDED(fr->SetPixelFormat(&fmt)) && fmt == GUID_WICPixelFormat32bppBGRA;
      ok = ok && SUCCEEDED(fr->WritePixels(h, w * 4, w * h * 4, static_cast<BYTE*>(bits))) && SUCCEEDED(fr->Commit()) &&
           SUCCEEDED(enc->Commit());
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    if (SUCCEEDED(ci)) CoUninitialize();
  }
  if (bmp) DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok;
}
