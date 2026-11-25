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
//
// RBRHUD's layers drawn over a gauge face (the E30's 2048x512 needle layer, user 2026-11-09: "0A0287068931ECCA is the
// dynamically updated texture with the needle moving") are render targets. They show and glow only as Remix's world-space
// UI category, and a render target's Remix hash depends on how many were created before it (d3d9_common_texture.cpp:
// XXH3(counter, XXH3({w, h, 1}))), so a tag made in Alt+X is wrong on the next run. Here a render target drawn only blended
// or alpha-tested on a car counts as such a layer; every hash its size can have (counters 0..kCounters) goes to
// rtx.worldSpaceUiTextures through the not-saved host layer. A display copy of the whole interior is drawn opaque and is
// never tagged. materials.ini [hud_glow] ui_layers = 0 turns this off.
//
// Snapshot (user 2026-11-09: "visible at the start ... became invisible when moving ... came back when stationary"): Remix
// path traces at the end of the frame and reads these targets then, while raster read them at the draw. RBRHUD redraws one
// when its value changes, so a moving needle left Remix a target that was no longer what the car was drawn with. A gauge
// target is copied at its car draw and the copy is what the draw binds. materials.ini [hud_glow] snapshot = 0 turns this off.
#define CINTERFACE
#include "common.hpp"
#include <d3d9.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "third_party/xxhash.h"

#define wlog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

namespace {

constexpr D3DRENDERSTATETYPE kRsModifier = static_cast<D3DRENDERSTATETYPE>(149);
constexpr D3DRENDERSTATETYPE kRsEmittance = static_cast<D3DRENDERSTATETYPE>(177);
constexpr DWORD kGlowTimesAlbedo = 1u << 18, kGlowRgbPlusAlpha = 1u << 19, kGlowAlphaOnly = 1u << 20;

bool g_on = false;
bool g_alphaWithoutLamps = true;
DWORD g_slot = 7, g_tint = 0xFFFFFFu;
IDirect3DBaseTexture9* g_s15 = nullptr;               // not owned; compared only
BOOL g_b1 = FALSE;
bool g_applied = false;
IDirect3DBaseTexture9* g_prevSlot = nullptr;          // owned while applied
unsigned g_draws = 0, g_nightDraws = 0, g_gameB1Draws = 0, g_layerSkips = 0;

struct RtInfo { bool rt = false; UINT w = 0, h = 0; unsigned blended = 0, opaque = 0; bool tagged = false; };
std::unordered_map<const void*, RtInfo> g_isRt;       // s0 texture -> render target? (RBRHUD's display copy or layer)
constexpr unsigned kCounters = 4096, kLayerDraws = 8;  // hashes per size; blended draws before a target counts as a layer
bool g_uiLayers = true, g_snapshot = true;
float g_uiEmissiveOn = 0.05f, g_uiEmissiveOff = 0.0f;   // rtx.worldSpaceUiEmissiveIntensity with the lamps on / off
struct LayerCopy { IDirect3DTexture9* tex = nullptr; unsigned frame = ~0u; };
std::unordered_map<const void*, LayerCopy> g_copies;  // gauge render target -> its snapshot
unsigned g_copiesMade = 0;
float g_dayGain = 8.0f, g_gainFrom = 0.0f, g_gainTo = 25.0f;   // [hud_glow] emissive_day_gain, emissive_gain_from_sun, emissive_gain_to_sun
std::set<std::pair<UINT, UINT>> g_uiSizes;            // sizes already in the list
std::string g_uiList;
bool g_uiDirty = false;
decltype(IDirect3DDevice9Vtbl::SetTexture) g_setTexture = nullptr;
decltype(IDirect3DDevice9Vtbl::SetPixelShaderConstantB) g_setPsB = nullptr;
decltype(IDirect3DDevice9Vtbl::CreatePixelShader) g_createPs = nullptr;
std::unordered_map<const void*, bool> g_psUsesS15;

// Scan D3D9 SM2/SM3 pixel shader bytecode for DCL of sampler s15 (D3DSIO_DCL, D3DSPR_SAMPLER == 10, reg 15).
// Only RBRHUD's CarTextureNoDirtEmMask.pso declares s15; other car/interior shaders must never inherit a stale mask.
bool ps_bytecode_uses_s15(const DWORD* words, UINT maxWords) {
  if (!words || maxWords < 2 || (words[0] >> 16) != 0xFFFFu || ((words[0] >> 8) & 0xFFu) < 2u) return false;
  UINT i = 1;
  while (i < maxWords) {
    const DWORD op = words[i], code = op & 0xFFFFu;
    if (code == 0xFFFFu) break;
    if (code == 0xFFFEu) { i += 1u + ((op >> 16) & 0x7FFFu); continue; }
    const UINT toks = (op >> 24) & 0xFu;
    if (code == 0x001Fu && toks >= 2u && i + 2u < maxWords) {
      const DWORD dst = words[i + 2u];
      const DWORD regType = ((dst >> 28) & 0x7u) | (((dst >> 11) & 0x3u) << 3);
      const DWORD regNum = dst & 0x7FFu;
      if (regType == 10u && regNum == 15u) return true;
    }
    i += 1u + toks;
  }
  return false;
}

bool current_ps_uses_s15(IDirect3DDevice9* dev) {
  IDirect3DPixelShader9* ps = nullptr;
  if (FAILED(dev->lpVtbl->GetPixelShader(dev, &ps)) || !ps) return false;
  auto it = g_psUsesS15.find(ps);
  if (it == g_psUsesS15.end()) {
    bool uses = false;
    UINT bytes = 0;
    if (SUCCEEDED(ps->lpVtbl->GetFunction(ps, nullptr, &bytes)) && bytes >= 8 && bytes <= 65536) {
      std::vector<DWORD> buf(bytes / 4, 0);
      if (SUCCEEDED(ps->lpVtbl->GetFunction(ps, buf.data(), &bytes)))
        uses = ps_bytecode_uses_s15(buf.data(), static_cast<UINT>(buf.size()));
    }
    if (g_psUsesS15.size() > 4096) g_psUsesS15.clear();
    it = g_psUsesS15.emplace(ps, uses).first;
  }
  const bool uses = it->second;
  ps->lpVtbl->Release(ps);
  return uses;
}

HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(IDirect3DDevice9* dev, const DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
  const HRESULT hr = g_createPs(dev, pFunction, ppShader);
  if (SUCCEEDED(hr) && ppShader && *ppShader && pFunction) {
    if (g_psUsesS15.size() > 4096) g_psUsesS15.clear();
    g_psUsesS15[*ppShader] = ps_bytecode_uses_s15(pFunction, 16384u);
  }
  return hr;
}

// The render target on s0, null for anything else. A mask RBRHUD leaves on s15 must not light other draws.
RtInfo* s0_render_target(IDirect3DDevice9* dev) {
  IDirect3DBaseTexture9* t = nullptr;
  if (FAILED(dev->lpVtbl->GetTexture(dev, 0, &t)) || !t) return nullptr;
  auto it = g_isRt.find(t);
  if (it == g_isRt.end()) {
    RtInfo i;
    D3DSURFACE_DESC d = {};
    i.rt = t->lpVtbl->GetType(t) == D3DRTYPE_TEXTURE &&
           SUCCEEDED(reinterpret_cast<IDirect3DTexture9*>(t)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(t), 0, &d)) &&
           (d.Usage & D3DUSAGE_RENDERTARGET);
    i.w = d.Width;
    i.h = d.Height;
    if (g_isRt.size() > 4096) g_isRt.clear();
    it = g_isRt.emplace(t, i).first;
  }
  t->lpVtbl->Release(t);
  return it->second.rt ? &it->second : nullptr;
}

// A new layer size: every hash a render target of that size can have (Remix's counter is small and only grows with new
// targets; 4096 = some 80 stage loads).
void tag_layer_size(UINT w, UINT h) {
  if (!g_uiSizes.emplace(w, h).second) return;
  const struct { uint32_t w, h, d; } extent = { w, h, 1 };
  const XXH64_hash_t base = XXH3_64bits(&extent, sizeof(extent));
  char b[24];
  g_uiList.reserve(g_uiList.size() + kCounters * 20);
  for (uint32_t c = 0; c < kCounters; ++c) {
    sprintf_s(b, "0x%016llX", static_cast<unsigned long long>(XXH3_64bits_withSeed(&c, sizeof(c), base)));
    if (!g_uiList.empty()) g_uiList += ", ";
    g_uiList += b;
  }
  g_uiDirty = true;
  wlog(L"hud glow: %ux%u render target drawn only blended on a car: a gauge layer, tagged as world-space UI", w, h);
}

HRESULT STDMETHODCALLTYPE hk_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
  if (rbrrtx_menu_is_drawing()) return g_setTexture(dev, stage, tex);   // the F3 menu's own draws
  if (stage == 15) g_s15 = tex;
  return g_setTexture(dev, stage, tex);
}

HRESULT STDMETHODCALLTYPE hk_SetPsB(IDirect3DDevice9* dev, UINT start, const BOOL* data, UINT count) {
  if (data && start <= 1 && start + count > 1) {
    g_b1 = data[1 - start];
    // Force RBRHUD's dynamic emissive shader constant b1 while our headlights are on, regardless of
    // stage Car_Ambient_Lighting, so raster fallback (key 0) matches our Remix lighting.
    if (g_on && rbrrtx_headlights_on() && !g_b1) {
      if (count <= 16) {
        BOOL buf[16];
        std::memcpy(buf, data, count * sizeof(BOOL));
        buf[1 - start] = TRUE;
        return g_setPsB(dev, start, buf, count);
      }
      std::vector<BOOL> buf(data, data + count);
      buf[1 - start] = TRUE;
      return g_setPsB(dev, start, buf.data(), count);
    }
  }
  return g_setPsB(dev, start, data, count);
}

}  // namespace

void rbrrtx_hudglow_init(void* device, bool runtimeHasProtocol) {
  wchar_t path[MAX_PATH * 2], tint[32] = {};
  rbrrtx_config_path(L"RBRRTX_MATERIALS", L"materials.ini", path, MAX_PATH * 2);
  g_on = runtimeHasProtocol && GetPrivateProfileIntW(L"hud_glow", L"enabled", 1, path) != 0;
  g_alphaWithoutLamps = GetPrivateProfileIntW(L"hud_glow", L"alpha_without_lamps", 1, path) != 0;
  g_slot = GetPrivateProfileIntW(L"hud_glow", L"slot", 7, path) & 7u;
  GetPrivateProfileStringW(L"hud_glow", L"tint", L"FFFFFF", tint, 32, path);
  g_tint = std::wcstoul(tint, nullptr, 16) & 0xFFFFFFu;
  if (!g_tint) g_tint = 0xFFFFFFu;
  g_uiLayers = GetPrivateProfileIntW(L"hud_glow", L"ui_layers", 1, path) != 0;
  g_snapshot = GetPrivateProfileIntW(L"hud_glow", L"snapshot", 1, path) != 0;
  wchar_t e[32] = {};
  GetPrivateProfileStringW(L"hud_glow", L"ui_emissive_lamps_on", L"0.05", e, 32, path);
  g_uiEmissiveOn = static_cast<float>(_wtof(e));
  GetPrivateProfileStringW(L"hud_glow", L"ui_emissive_lamps_off", L"0", e, 32, path);
  g_uiEmissiveOff = static_cast<float>(_wtof(e));
  GetPrivateProfileStringW(L"hud_glow", L"emissive_day_gain", L"8", e, 32, path);
  g_dayGain = (std::max)(static_cast<float>(_wtof(e)), 0.01f);
  GetPrivateProfileStringW(L"hud_glow", L"emissive_gain_from_sun", L"0", e, 32, path);
  g_gainFrom = static_cast<float>(_wtof(e));
  GetPrivateProfileStringW(L"hud_glow", L"emissive_gain_to_sun", L"25", e, 32, path);
  g_gainTo = static_cast<float>(_wtof(e));
  wlog(L"hud glow (materials.ini [hud_glow]): %s; RBRHUD emissive masks (s15) -> stage %lu, tint %06lX",
       g_on ? L"ON" : runtimeHasProtocol ? L"off" : L"off (runtime without the material protocol)", g_slot, g_tint);
  if (!g_on || !device) return;
  IDirect3DDevice9Vtbl* vt = static_cast<IDirect3DDevice9*>(device)->lpVtbl;
  patch(&vt->SetTexture, &hk_SetTexture, g_setTexture);
  patch(&vt->SetPixelShaderConstantB, &hk_SetPsB, g_setPsB);
  patch(&vt->CreatePixelShader, &hk_CreatePixelShader, g_createPs);
}

// A car draw (known car shader, or RBRHUD's own unknown one): is its render target a layer over the gauges?
void rbrrtx_hudglow_car_draw(void* device) {
  if (!g_uiLayers) return;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  RtInfo* i = s0_render_target(dev);
  if (!i || i->tagged) return;
  DWORD blend = FALSE, test = FALSE;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &test);
  if (blend || test) ++i->blended; else ++i->opaque;
  if (i->blended >= kLayerDraws && !i->opaque) { i->tagged = true; tag_layer_size(i->w, i->h); }
}

float rbrrtx_hudglow_ui_emissive(bool lampsOn) { return lampsOn ? g_uiEmissiveOn : g_uiEmissiveOff; }

// The multiplier on every emissive for the sun's height: 1 at or below emissive_gain_from_sun, emissive_day_gain at or above ..._to_sun.
float rbrrtx_hudglow_emissive_gain(float sunElevation) {
  const float t = std::clamp((sunElevation - g_gainFrom) / (std::max)(g_gainTo - g_gainFrom, 0.1f), 0.0f, 1.0f);
  return 1.0f + (g_dayGain - 1.0f) * t * t * (3.0f - 2.0f * t);
}

// A gauge render target about to be drawn on the car: copies it (once per frame) and binds the copy at stage 0. Returns the
// game's texture, referenced, for the caller to put back after the draw; null when nothing was bound.
void* rbrrtx_hudglow_layer_bind(void* device, unsigned frame) {
  if (!g_snapshot) return nullptr;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  RtInfo* i = s0_render_target(dev);
  if (!i || (!i->tagged && !(g_s15 && current_ps_uses_s15(dev)))) return nullptr;      // a layer, or a display copy lit through RBRHUD's mask
  IDirect3DBaseTexture9* t = nullptr;
  if (FAILED(dev->lpVtbl->GetTexture(dev, 0, &t)) || !t) return nullptr;
  IDirect3DTexture9* src = reinterpret_cast<IDirect3DTexture9*>(t);
  LayerCopy& c = g_copies[t];
  if (!c.tex) {
    D3DSURFACE_DESC d = {};
    if (FAILED(src->lpVtbl->GetLevelDesc(src, 0, &d)) ||
        FAILED(dev->lpVtbl->CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &c.tex, nullptr)) || !c.tex) {
      c.tex = nullptr;
      t->lpVtbl->Release(t);
      return nullptr;
    }
    if (++g_copiesMade <= 8) wlog(L"hud glow: %ux%u gauge target gets a snapshot for Remix (format %d)", d.Width, d.Height, static_cast<int>(d.Format));
  }
  if (c.frame != frame) {
    IDirect3DSurface9* from = nullptr, *to = nullptr;
    src->lpVtbl->GetSurfaceLevel(src, 0, &from);
    c.tex->lpVtbl->GetSurfaceLevel(c.tex, 0, &to);
    const bool ok = from && to && SUCCEEDED(dev->lpVtbl->StretchRect(dev, from, nullptr, to, nullptr, D3DTEXF_NONE));
    if (from) from->lpVtbl->Release(from);
    if (to) to->lpVtbl->Release(to);
    if (!ok) { t->lpVtbl->Release(t); return nullptr; }
    c.frame = frame;
  }
  if (FAILED(dev->lpVtbl->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(c.tex)))) { t->lpVtbl->Release(t); return nullptr; }
  return t;
}

// Before Reset and at the device's end: the snapshots are D3DPOOL_DEFAULT; the next draw makes them again.
void rbrrtx_hudglow_device_lost() {
  for (auto& kv : g_copies) if (kv.second.tex) kv.second.tex->lpVtbl->Release(kv.second.tex);
  g_copies.clear();
  g_s15 = nullptr;
  g_psUsesS15.clear();
}

// The list for rtx.worldSpaceUiTextures when it has grown, once; else null.
const char* rbrrtx_hudglow_ui_list() {
  if (!g_uiDirty) return nullptr;
  g_uiDirty = false;
  return g_uiList.c_str();
}

void rbrrtx_hudglow_before(void* device) {
  if (!g_on || !g_s15 || !g_setTexture) return;
  const bool lamps = rbrrtx_headlights_on();
  if (!lamps && !g_alphaWithoutLamps) return;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (!current_ps_uses_s15(dev)) return;
  if (!s0_render_target(dev)) return;
  // Layers drawn over the gauge face (the E30's 2048x512 needle layer: transparent but for the needles) are blended or
  // alpha-tested. Run 20261101-130413 showed the E30 gauges black with the glow on: the protocol material made the layer
  // solid. Only the opaque display copy glows.
  DWORD blend = FALSE, test = FALSE;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &test);
  if (blend || test) { ++g_layerSkips; return; }
  IDirect3DBaseTexture9* s15 = nullptr;
  if (FAILED(dev->lpVtbl->GetTexture(dev, 15, &s15)) || !s15) { g_s15 = nullptr; return; }
  dev->lpVtbl->GetTexture(dev, g_slot, &g_prevSlot);
  g_setTexture(dev, g_slot, s15);
  s15->lpVtbl->Release(s15);
  // Always keep mask.a (displays/needles) on when alpha_without_lamps=1, and switch mask.rgb (backlit dials /
  // interior ambient lights) directly with our headlights regardless of stage Car_Ambient_Lighting (game b1).
  const DWORD mod = 0u | (0xFu << 4) | (g_slot << 8) | (0xFu << 12) | kGlowTimesAlbedo |
                    (lamps ? kGlowRgbPlusAlpha : kGlowAlphaOnly);
  dev->lpVtbl->SetRenderState(dev, kRsModifier, mod);
  dev->lpVtbl->SetRenderState(dev, kRsEmittance, 0xFF000000u | g_tint);
  g_applied = true;
  ++g_draws;
  if (lamps) ++g_nightDraws;
  if (g_b1) ++g_gameB1Draws;
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

void rbrrtx_hudglow_forget() { g_isRt.clear(); g_s15 = nullptr; }    // textures created/released: pointers may be reused

void rbrrtx_hudglow_log() {
  if (!g_on) return;
  wlog(L"hud glow: %u RBRHUD gauge draws marked since last line (%u rgb+alpha [headlights on], %u game b1), %u blended / alpha-tested layers left unlit",
       g_draws, g_nightDraws, g_gameB1Draws, g_layerSkips);
  g_draws = g_nightDraws = g_gameB1Draws = g_layerSkips = 0;
}
