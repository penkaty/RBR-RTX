// dinput_block.cpp - neutral DirectInput state while the F3 menu is open (user 2026-11-02: the car must not drive while
// the menu is up). RBR reads keyboard, mouse and wheel through DirectInput 8 (imports DINPUT8.dll!DirectInput8Create), so
// window messages alone do not stop it.
//
// How: at install the proxy creates a throw-away DirectInput 8 object, enumerates the attached devices (plus the system
// keyboard and mouse), reads each device's vtable and replaces the GetDeviceState and GetDeviceData slots of every distinct
// vtable (dinput8.dll shares one per device class). The hooks call the original first (so the Remix bridge's own DirectInput
// hook, which wipes the state while the Remix panel is up, stays in the chain) and then, only while the menu is open:
//   keyboard (256 bytes), mouse (DIMOUSESTATE 16 / DIMOUSESTATE2 20): all zero;
//   joystick / wheel (DIJOYSTATE 80 / DIJOYSTATE2 272): the axes stay at the values they had when the menu opened
//     (a wheel has no known neutral: the pedal rest position differs per device), buttons released, hats centred;
//   buffered data (GetDeviceData): nothing is returned.
// Nothing is written to the game or to disk; the slots are put back at exit. RBRRTX_MENU_BLOCK_INPUT=0 leaves it out.
#include "common.hpp"
#include "menu/menu.hpp"

#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <vector>

namespace {

typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDeviceState)(void*, DWORD, LPVOID);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDeviceData)(void*, DWORD, void*, DWORD*, DWORD);

constexpr int kSlotGetDeviceState = 9;   // IDirectInputDevice8: IUnknown 0-2, GetCapabilities 3, ..., GetDeviceState 9, GetDeviceData 10
constexpr int kSlotGetDeviceData = 10;

// The slots are replaced per distinct vtable; every vtable keeps its own original pointers (a shared class has the same ones).
struct Patched {
  void** vtbl;
  PFN_GetDeviceState state;
  PFN_GetDeviceData data;
};
std::vector<Patched> g_patched;
std::mutex g_lock;
std::map<void*, std::vector<BYTE>> g_frozen;   // joystick state at the moment the menu opened, per device
bool g_active = false;                         // the hooks are installed
bool g_wasOpen = false;
unsigned g_wipes = 0, g_logged = 0;

const Patched* find_patch(void* self) {
  void** vt = *static_cast<void***>(self);
  for (const Patched& p : g_patched) if (p.vtbl == vt) return &p;
  return nullptr;
}

HRESULT STDMETHODCALLTYPE hk_GetDeviceState(void* self, DWORD cb, LPVOID data) {
  const Patched* p = find_patch(self);
  if (!p) return E_FAIL;   // not reached: the hook is only in patched vtables
  const HRESULT hr = p->state(self, cb, data);
  const bool open = rbrrtx_menu_is_open() != 0;
  if (!open) {
    if (g_wasOpen) { std::lock_guard<std::mutex> l(g_lock); g_frozen.clear(); g_wasOpen = false; }
    return hr;
  }
  g_wasOpen = true;
  if (hr != S_OK || !data) return hr;
  if (cb == 256 || cb == 16 || cb == 20) {
    std::memset(data, 0, cb);
  } else if (cb == 80 || cb == 272) {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<BYTE>& f = g_frozen[self];
    BYTE* d = static_cast<BYTE*>(data);
    if (f.size() != cb) f.assign(d, d + cb);   // first frame with the menu open: remember the axes
    std::memcpy(d, f.data(), cb);
    std::memset(d + 32, 0xFF, 16);              // four hats: centred (-1)
    std::memset(d + 48, 0, cb == 80 ? 32 : 128);   // buttons released
  }
  if (g_logged < 3 && (cb == 256 || cb == 16 || cb == 20 || cb == 80 || cb == 272)) {
    ++g_logged;
    rbrrtx_log_to(L"rbrrtx_comp.log", L"menu: DirectInput state of a %lu-byte device wiped while the menu is open (%s)", cb,
                  cb == 256 ? L"keyboard" : cb == 16 || cb == 20 ? L"mouse" : L"joystick / wheel: axes frozen, buttons released");
  }
  ++g_wipes;
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_GetDeviceData(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags) {
  const Patched* p = find_patch(self);
  if (!p) return E_FAIL;
  const HRESULT hr = p->data(self, cbObj, rgdod, inOut, flags);
  if (rbrrtx_menu_is_open() && rgdod && inOut) {
    std::memset(rgdod, 0, static_cast<size_t>(*inOut) * cbObj);
    *inOut = 0;
  }
  return hr;
}

// Replaces slot `i` of `vtbl` (read-only memory in dinput8.dll: the page is opened for the write).
bool write_slot(void** vtbl, int i, void* value) {
  DWORD old = 0;
  if (!VirtualProtect(&vtbl[i], sizeof(void*), PAGE_READWRITE, &old)) return false;
  vtbl[i] = value;
  VirtualProtect(&vtbl[i], sizeof(void*), old, &old);
  return true;
}

void patch_device(void* device) {
  if (!device) return;
  void** vt = *static_cast<void***>(device);
  for (const Patched& p : g_patched) if (p.vtbl == vt) return;
  Patched p = { vt, reinterpret_cast<PFN_GetDeviceState>(vt[kSlotGetDeviceState]), reinterpret_cast<PFN_GetDeviceData>(vt[kSlotGetDeviceData]) };
  if (!p.state || !p.data) return;
  g_patched.push_back(p);                                   // before the write: the hook looks the vtable up
  if (!write_slot(vt, kSlotGetDeviceState, reinterpret_cast<void*>(&hk_GetDeviceState)) ||
      !write_slot(vt, kSlotGetDeviceData, reinterpret_cast<void*>(&hk_GetDeviceData))) {
    g_patched.pop_back();
    rbrrtx_log_to(L"rbrrtx_comp.log", L"menu: DirectInput block: could not write the device vtable %p", static_cast<void*>(vt));
  }
}

// IUnknown-style raw access (no dinput.h / dxguid.lib needed).
struct RawUnknown { void** vtbl; };
typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(void*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(void*, const GUID*, void**, void*);
typedef BOOL(CALLBACK* PFN_EnumCb)(const void*, void*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumDevices)(void*, DWORD, PFN_EnumCb, void*, DWORD);
typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, const GUID&, void**, void*);

const GUID kIID_IDirectInput8W = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
const GUID kGUID_SysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID kGUID_SysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

struct EnumCtx { void* di; };

// DIDEVICEINSTANCEW: dwSize (4), guidInstance (16), ... : create a throw-away device for each attached one.
BOOL CALLBACK enum_cb(const void* inst, void* user) {
  EnumCtx* c = static_cast<EnumCtx*>(user);
  const GUID* guid = reinterpret_cast<const GUID*>(static_cast<const BYTE*>(inst) + 4);
  void* dev = nullptr;
  RawUnknown* di = static_cast<RawUnknown*>(c->di);
  if (SUCCEEDED(reinterpret_cast<PFN_CreateDevice>(di->vtbl[3])(di, guid, &dev, nullptr)) && dev) {
    patch_device(dev);
    reinterpret_cast<PFN_Release>((*static_cast<void***>(dev))[2])(dev);
  }
  return TRUE;   // DIENUM_CONTINUE
}

}  // namespace

void rbrrtx_dinput_block_install() {
  if (g_active || !rbrrtx_env_flag(L"RBRRTX_MENU_BLOCK_INPUT", false)) return;
  HMODULE di8 = GetModuleHandleW(L"dinput8.dll");
  if (!di8) di8 = LoadLibraryW(L"dinput8.dll");
  const auto create = di8 ? reinterpret_cast<PFN_DirectInput8Create>(GetProcAddress(di8, "DirectInput8Create")) : nullptr;
  void* di = nullptr;
  if (!create || FAILED(create(GetModuleHandleW(nullptr), 0x0800, kIID_IDirectInput8W, &di, nullptr)) || !di) {
    rbrrtx_log_to(L"rbrrtx_comp.log", L"menu: DirectInput block: DirectInput8Create failed, the game keeps its input while the menu is open");
    return;
  }
  RawUnknown* d = static_cast<RawUnknown*>(di);
  for (const GUID* g : { &kGUID_SysKeyboard, &kGUID_SysMouse }) {
    void* dev = nullptr;
    if (SUCCEEDED(reinterpret_cast<PFN_CreateDevice>(d->vtbl[3])(d, g, &dev, nullptr)) && dev) {
      patch_device(dev);
      reinterpret_cast<PFN_Release>((*static_cast<void***>(dev))[2])(dev);
    }
  }
  EnumCtx ctx = { di };
  reinterpret_cast<PFN_EnumDevices>(d->vtbl[4])(d, 0 /* DI8DEVCLASS_ALL */, enum_cb, &ctx, 1 /* DIEDFL_ATTACHEDONLY */);
  reinterpret_cast<PFN_Release>(d->vtbl[2])(d);
  g_active = !g_patched.empty();
  rbrrtx_log_to(L"rbrrtx_comp.log", L"menu: DirectInput block %s: %zu device vtable(s) hooked (keyboard and mouse zeroed, wheel frozen while the menu is open)",
                g_active ? L"installed" : L"NOT installed", g_patched.size());
}

// Puts the slots back (exit). A hook still running on another thread finishes on its own copy of the pointers.
void rbrrtx_dinput_block_remove() {
  if (!g_active) return;
  g_active = false;
  for (const Patched& p : g_patched) {
    write_slot(p.vtbl, kSlotGetDeviceState, reinterpret_cast<void*>(p.state));
    write_slot(p.vtbl, kSlotGetDeviceData, reinterpret_cast<void*>(p.data));
  }
}
