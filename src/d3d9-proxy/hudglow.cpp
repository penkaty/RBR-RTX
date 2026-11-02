// hudglow.cpp - RBRHUD's in-car gauge displays glow in Remix as they do in the game (plan docs/research/api-world-plan.md
// section 4; KB re/kb/systems/rbrhud-gauges.md).
//
// RBRHUD draws needles and digits into a render-target copy of the car's interior texture and lights it with its own
// pixel shader CarTextureNoDirtEmMask: s0 = that texture, s15 = a per-car emissive mask, b1 = "dynamic emissive
// elements on" (the stage is dark): lit = tex * mask.a (always) + tex * mask.rgb (b1). Remix reads s0 only, so the
// displays were plain unlit albedo.
// Kim2091's runtime decodes a per-draw material protocol from spare D3D9 render states (rtx_fork_fnv_material.h:
// RS 149 = texture slots + flags, RS 177 = glow tint). For every draw with a texture on s15 this puts the mask on stage
// kSlot as well (the protocol's slots are 4 bits, 0xF = absent, and the runtime reads stages 0-7; RBRHUD's shader
// never samples kSlot) and sets RS 149 = diffuse s0, glow kSlot, glow x albedo (RBR-RTX bits, our runtime), glow =
// mask.a by day, mask.rgb + mask.a while b1 is on. Restored right after the draw. Other runtimes ignore RS 149.
// Settings: materials.ini [hud_glow] enabled (1), tint (D3DCOLOR hex, FFFFFF), slot (7).
#define CINTERFACE
#include "common.hpp"
#include <d3d9.h>
#include <cwchar>
#include <unordered_map>

#define wlog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

namespace {

constexpr D3DRENDERSTATETYPE kRsModifier = static_cast<D3DRENDERSTATETYPE>(149);
constexpr D3DRENDERSTATETYPE kRsEmittance = static_cast<D3DRENDERSTATETYPE>(177);
constexpr DWORD kGlowTimesAlbedo = 1u << 18, kGlowRgbPlusAlpha = 1u << 19, kGlowAlphaOnly = 1u << 20;

bool g_on = false;
DWORD g_slot = 7, g_tint = 0xFFFFFFu;
IDirect3DBaseTexture9* g_s15 = nullptr;               // not owned; compared only
BOOL g_b1 = FALSE;
bool g_applied = false;
IDirect3DBaseTexture9* g_prevSlot = nullptr;          // owned while applied
unsigned g_draws = 0, g_nightDraws = 0;

std::unordered_map<const void*, bool> g_isRt;         // s0 texture -> is a render target (RBRHUD's display copy)
decltype(IDirect3DDevice9Vtbl::SetTexture) g_setTexture = nullptr;
decltype(IDirect3DDevice9Vtbl::SetPixelShaderConstantB) g_setPsB = nullptr;

template <class F>
void patch(F* slot, F hook, F& orig) {
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(F), PAGE_EXECUTE_READWRITE, &old)) return;
  if (*slot != hook) { orig = *slot; *slot = hook; }
  VirtualProtect(slot, sizeof(F), old, &old);
}

// The gauge draw samples the display render target on s0; a mask RBRHUD leaves on s15 must not light other draws.
bool s0_is_display(IDirect3DDevice9* dev) {
  IDirect3DBaseTexture9* t = nullptr;
  if (FAILED(dev->lpVtbl->GetTexture(dev, 0, &t)) || !t) return false;
  auto it = g_isRt.find(t);
  bool rt = false;
  if (it != g_isRt.end()) rt = it->second;
  else {
    D3DSURFACE_DESC d = {};
    rt = t->lpVtbl->GetType(t) == D3DRTYPE_TEXTURE &&
         SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(t)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(t), 0, &d)) &&
         (d.Usage & D3DUSAGE_RENDERTARGET);
    if (g_isRt.size() > 4096) g_isRt.clear();
    g_isRt[t] = rt;
  }
  t->lpVtbl->Release(t);
  return rt;
}

HRESULT STDMETHODCALLTYPE hk_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
  if (rbrrtx_menu_is_drawing()) return g_setTexture(dev, stage, tex);   // the F3 menu's own draws
  if (stage == 15) g_s15 = tex;
  return g_setTexture(dev, stage, tex);
}

HRESULT STDMETHODCALLTYPE hk_SetPsB(IDirect3DDevice9* dev, UINT start, const BOOL* data, UINT count) {
  if (data && start <= 1 && start + count > 1) g_b1 = data[1 - start];
  return g_setPsB(dev, start, data, count);
}

}  // namespace

void rbrrtx_hudglow_init(void* device, bool runtimeHasProtocol) {
  wchar_t path[MAX_PATH * 2], tint[32] = {};
  rbrrtx_config_path(L"RBRRTX_MATERIALS", L"materials.ini", path, MAX_PATH * 2);
  g_on = runtimeHasProtocol && GetPrivateProfileIntW(L"hud_glow", L"enabled", 1, path) != 0;
  g_slot = GetPrivateProfileIntW(L"hud_glow", L"slot", 7, path) & 7u;
  GetPrivateProfileStringW(L"hud_glow", L"tint", L"FFFFFF", tint, 32, path);
  g_tint = std::wcstoul(tint, nullptr, 16) & 0xFFFFFFu;
  if (!g_tint) g_tint = 0xFFFFFFu;
  wlog(L"hud glow (materials.ini [hud_glow]): %s; RBRHUD emissive masks (s15) -> stage %lu, tint %06lX",
       g_on ? L"ON" : runtimeHasProtocol ? L"off" : L"off (runtime without the material protocol)", g_slot, g_tint);
  if (!g_on || !device) return;
  IDirect3DDevice9Vtbl* vt = static_cast<IDirect3DDevice9*>(device)->lpVtbl;
  patch(&vt->SetTexture, &hk_SetTexture, g_setTexture);
  patch(&vt->SetPixelShaderConstantB, &hk_SetPsB, g_setPsB);
}

void rbrrtx_hudglow_before(void* device) {
  if (!g_on || !g_s15 || !g_setTexture) return;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (!s0_is_display(dev)) return;
  dev->lpVtbl->GetTexture(dev, g_slot, &g_prevSlot);
  g_setTexture(dev, g_slot, g_s15);
  const DWORD mod = 0u | (0xFu << 4) | (g_slot << 8) | (0xFu << 12) | kGlowTimesAlbedo |
                    (g_b1 ? kGlowRgbPlusAlpha : kGlowAlphaOnly);
  dev->lpVtbl->SetRenderState(dev, kRsModifier, mod);
  dev->lpVtbl->SetRenderState(dev, kRsEmittance, 0xFF000000u | g_tint);
  g_applied = true;
  ++g_draws;
  if (g_b1) ++g_nightDraws;
}

void rbrrtx_hudglow_after(void* device) {
  if (!g_applied) return;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  dev->lpVtbl->SetRenderState(dev, kRsModifier, 0);
  dev->lpVtbl->SetRenderState(dev, kRsEmittance, 0);
  g_setTexture(dev, g_slot, g_prevSlot);
  if (g_prevSlot) g_prevSlot->lpVtbl->Release(g_prevSlot);
  g_prevSlot = nullptr;
  g_applied = false;
}

void rbrrtx_hudglow_forget() { g_isRt.clear(); }    // textures created/released: pointers may be reused

void rbrrtx_hudglow_log() {
  if (!g_on) return;
  wlog(L"hud glow: %u RBRHUD gauge draws marked since last line (%u with b1 = night glow)", g_draws, g_nightDraws);
  g_draws = g_nightDraws = 0;
}
