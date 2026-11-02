// comp.cpp - compatibility layer v0 ("comp mod") for RBR under RTX Remix (x86).
//
// Installed with a Remix backend unless RBRRTX_COMP=0 (see d3d9_proxy.cpp). Hooks the IDirect3D9 / IDirect3DDevice9 vtables of
// the backend the proxy loaded (Remix's bridge, or system D3D9 for tests) and:
//  1. shadows vertex-shader constants (SetVertexShaderConstantF);
//  2. identifies RBR's shaders: CreateVertexShader bytecode is hashed and matched against the game's
//     VertexShaders\**\*.vso files;
//  3. rebuilds fixed-function World/View/Projection from the constants (see re/kb/systems/shaders.md) and sets them
//     before each shader draw on the primary render target, so Remix gets a camera and per-object transforms
//     (Remix: rtx.useWorldMatricesForShaders, rtx.useVertexCapture, both default on);
//  4. adds a Remix distant light along the game's sun direction c41 through the Remix API (RBRRTX_SUN, default on;
//     needs exposeRemixApi = True in .trex\bridge.conf). Radiance: RBRRTX_SUN_RADIANCE="r,g,b".
//  5. car headlights as Remix spot lights (headlights.cpp; key L, automatic below a 10 degree sun).
// Hotkeys (keys.cpp / keys.ini, defaults): O toggles 3 and 4 at runtime, Alt+L only 4, P dumps the next frame's draws;
// N cycles the sky mode, I the windscreen. Diagnostics go to <RBRRTX_LOG_DIR>\rbrrtx_comp.log.

#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <initguid.h>
#include <d3d9.h>
#include "third_party/remix_c.h"
#include "menu/menu.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <vector>

namespace fs = std::filesystem;

namespace {

#define clog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

// ---------------------------------------------------------------- matrices (D3D row-vector convention: v' = v * M)

// A vs_1_1 `m4x4 oPos, v0, cN` computes oPos.k = dot(v0, c[N+k]); as a D3D matrix that is the transpose of the rows.
D3DMATRIX from_rows(const float (*c)[4]) {
  D3DMATRIX m;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) m.m[i][j] = c[j][i];
  return m;
}

D3DMATRIX mul(const D3DMATRIX& a, const D3DMATRIX& b) {
  D3DMATRIX r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
  return r;
}

// Double-precision 4x4 helpers: stage coordinates reach kilometres, so float inverses would jitter.
struct DMat { double m[16]; };

DMat to_d(const D3DMATRIX& a) { DMat r; for (int i = 0; i < 16; ++i) r.m[i] = (&a.m[0][0])[i]; return r; }

D3DMATRIX to_f(const DMat& a) { D3DMATRIX r; for (int i = 0; i < 16; ++i) (&r.m[0][0])[i] = static_cast<float>(a.m[i]); return r; }

DMat mul_d(const DMat& a, const DMat& b) {
  DMat r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j] + a.m[i * 4 + 3] * b.m[12 + j];
  return r;
}

bool inverse_d(const DMat& in, DMat& out) {           // cofactor expansion (MESA gluInvertMatrix)
  const double* m = in.m;
  double inv[16];
  inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
  inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
  inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
  inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
  inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
  inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
  inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
  inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
  inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
  inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
  inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
  inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
  inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
  inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
  inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
  inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
  double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-30) return false;
  for (int i = 0; i < 16; ++i) out.m[i] = inv[i] / det;
  return true;
}

bool inverse(const D3DMATRIX& in, D3DMATRIX& out) {
  DMat r;
  if (!inverse_d(to_d(in), r)) return false;
  out = to_f(r);
  return true;
}

float max_diff(const D3DMATRIX& a, const D3DMATRIX& b) {
  float d = 0;
  for (int i = 0; i < 16; ++i) d = (std::max)(d, std::fabs((&a.m[0][0])[i] - (&b.m[0][0])[i]));
  return d;
}

D3DMATRIX identity() {
  D3DMATRIX m{};
  m._11 = m._22 = m._33 = m._44 = 1.0f;
  return m;
}

// ---------------------------------------------------------------- shader identification

enum : unsigned { SH_KNOWN = 1, SH_WORLDSPACE = 2, SH_USES_C8 = 4, SH_CAR = 8, SH_SKY = 16, SH_GLASS = 32, SH_SCENERY = 64, SH_PARTICLE = 128,
           SH_SWAY = 256, SH_ANIM = 512, SH_BTB = 1024, SH_DOUBLE = 2048 };   // SWAY: wind-swayed foliage; ANIM: UV/vertex animation (c85-c87); DOUBLE: RLDouble* two-texture blend

// A matrix constant of a D3DX-compiled shader, found by name in its constant table. HLSL packs column_major by
// default: then register r+i holds column i, so the D3D (row-vector) matrix is the transpose of the registers.
struct CtabMatrix { int reg = -1, count = 0; bool columns = true; };

struct ShaderInfo {
  std::wstring name;
  unsigned flags = 0;
  CtabMatrix wvp, world, vp;                           // SH_BTB: worldviewproj, worldMatrix, viewproj
};

// Length of D3D9 shader bytecode in bytes: up to and including the end token, skipping comment blocks.
size_t bytecode_size(const DWORD* p, size_t max_dwords) {
  for (size_t i = 1; i < max_dwords; ++i) {
    const DWORD t = p[i];
    if (t == 0x0000FFFF) return (i + 1) * sizeof(DWORD);
    if ((t & 0xFFFF) == 0xFFFE) i += (t >> 16) & 0x7FFF;   // comment: skip its payload
  }
  return 0;
}

unsigned long long fnv1a(const void* data, size_t size) {
  unsigned long long h = 1469598103934665603ULL;
  const unsigned char* b = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

// Hash of the instruction tokens only (comment blocks left out). Tester 2026-10-19 (v0.2): some installs hand the
// device RBR's shaders with the comment blocks stripped (another hook in between, e.g. ReShade), so the full-bytecode
// hash never matched, every draw was "(unknown VS)" and path tracing stayed black. Instructions are the same.
unsigned long long code_hash(const DWORD* p, size_t max_dwords) {
  unsigned long long h = 1469598103934665603ULL;
  auto mix = [&h](DWORD t) { for (int k = 0; k < 4; ++k) { h ^= (t >> (8 * k)) & 0xFF; h *= 1099511628211ULL; } };
  mix(p[0]);
  for (size_t i = 1; i < max_dwords; ++i) {
    const DWORD t = p[i];
    if ((t & 0xFFFF) == 0xFFFE) { i += (t >> 16) & 0x7FFF; continue; }
    mix(t);
    if (t == 0x0000FFFF) break;
  }
  return h;
}

bool starts_with(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

// BTB stages (Bob's Track Builder, loaded by RSF's rbr_rx.dll into track slot 41) are drawn with D3DX effects from
// RX_CONTENT\COMMON\FX(_Low)\BTB_*.fx, compiled at run time, so their bytecode matches no .vso and their registers are
// whatever the compiler picked. The compiled shader keeps a constant table (comment block "CTAB") with the names:
// worldviewproj (object -> clip), worldMatrix (object -> world), viewproj (instanced variants: world comes per instance).
// Returns true for a BTB shader (it has worldviewproj or viewproj) and fills the matrices found. Names: the .fx files.
bool parse_btb_ctab(const DWORD* p, size_t max_dwords, ShaderInfo& info, std::wstring& names) {
  for (size_t i = 1; i < max_dwords; ++i) {
    const DWORD t = p[i];
    if (t == 0x0000FFFF) break;
    if ((t & 0xFFFF) != 0xFFFE) continue;
    const DWORD len = (t >> 16) & 0x7FFF;
    if (len >= 8 && p[i + 1] == 0x42415443) {          // 'CTAB'
      const unsigned char* base = reinterpret_cast<const unsigned char*>(p + i + 2);
      const size_t bytes = (len - 1) * sizeof(DWORD);
      auto dw = [&](size_t off) -> DWORD { return off + 4 <= bytes ? *reinterpret_cast<const DWORD*>(base + off) : 0; };
      auto wd = [&](size_t off) -> WORD { return off + 2 <= bytes ? *reinterpret_cast<const WORD*>(base + off) : 0; };
      const DWORD count = dw(12), infoOff = dw(16);
      bool btb = false;
      for (DWORD c = 0; c < count && infoOff + (c + 1) * 20 <= bytes; ++c) {
        const size_t ci = infoOff + c * 20;
        const DWORD nameOff = dw(ci), typeOff = dw(ci + 12);
        if (nameOff >= bytes) continue;
        std::string n;
        for (size_t k = nameOff; k < bytes && base[k] && n.size() < 64; ++k) n += static_cast<char>(base[k]);
        names += (names.empty() ? L"" : L" ") + std::wstring(n.begin(), n.end());
        if (wd(ci + 4) != 2) continue;                 // RegisterSet: D3DXRS_FLOAT4 only
        CtabMatrix m;
        m.reg = wd(ci + 6);
        m.count = wd(ci + 8);
        m.columns = typeOff < bytes && wd(typeOff) == 3;   // D3DXPC_MATRIX_COLUMNS (2 = rows)
        if (!_stricmp(n.c_str(), "worldviewproj")) { info.wvp = m; btb = true; }
        else if (!_stricmp(n.c_str(), "viewproj")) { info.vp = m; btb = true; }
        else if (!_stricmp(n.c_str(), "worldMatrix")) info.world = m;
      }
      return btb;
    }
    i += len;
  }
  return false;
}

unsigned flags_for(const std::wstring& rel) {
  const std::wstring n = fs::path(rel).filename().wstring();
  unsigned f = SH_KNOWN;
  if (starts_with(n, L"RL") || starts_with(n, L"Water")) f |= SH_WORLDSPACE;   // vertices already in world space
  if (starts_with(n, L"Car")) f |= SH_CAR;
  if (starts_with(n, L"RL") || starts_with(n, L"IO")) f |= SH_SCENERY;   // stage geometry and objects (foliage, fences)
  if (starts_with(n, L"Clouds") || starts_with(n, L"SkyDome")) f |= SH_SKY;
  if (n.find(L"Sway") != std::wstring::npos) f |= SH_SWAY;
  if (n.find(L"Anim") != std::wstring::npos) f |= SH_ANIM;
  if (starts_with(n, L"RLDouble")) f |= SH_DOUBLE;
  if (n == L"Particle.vso") f |= SH_PARTICLE;   // dust, smoke, spray, rain sprites (and the ignored car shadow quad)
  if (n == L"CarGlassInternal.vso") f |= SH_GLASS;   // windscreen seen from the cockpit (CarGlass.vso = outside + mirrors)
  static const wchar_t* uses_c8[] = { L"CarGlass.vso", L"CarPaint1.vso", L"CarPaintInternal.vso", L"CarPlastic.vso",
                                      L"CarRim.vso", L"CloudsDoubleTexture.vso", L"CloudsSingleTexture.vso",
                                      L"IODoubleTexture.vso", L"IOSingleTexture.vso", L"Particle.vso", L"SkyDome.vso" };
  for (const wchar_t* u : uses_c8)
    if (_wcsicmp(n.c_str(), u) == 0) f |= SH_USES_C8;
  return f;
}

// ---------------------------------------------------------------- state

struct Orig {
  decltype(IDirect3D9Vtbl::CreateDevice) CreateDevice;
  decltype(IDirect3DDevice9Vtbl::AddRef) AddRef;
  decltype(IDirect3DDevice9Vtbl::Release) Release;
  decltype(IDirect3DDevice9Vtbl::Reset) Reset;
  decltype(IDirect3DDevice9Vtbl::Present) Present;
  decltype(IDirect3DDevice9Vtbl::BeginScene) BeginScene;
  decltype(IDirect3DDevice9Vtbl::SetRenderTarget) SetRenderTarget;
  decltype(IDirect3DDevice9Vtbl::SetTransform) SetTransform;
  decltype(IDirect3DDevice9Vtbl::CreateVertexShader) CreateVertexShader;
  decltype(IDirect3DDevice9Vtbl::SetVertexShader) SetVertexShader;
  decltype(IDirect3DDevice9Vtbl::SetVertexShaderConstantF) SetVertexShaderConstantF;
  decltype(IDirect3DDevice9Vtbl::SetPixelShaderConstantF) SetPixelShaderConstantF;
  decltype(IDirect3DDevice9Vtbl::CreateVertexDeclaration) CreateVertexDeclaration;
  decltype(IDirect3DDevice9Vtbl::SetVertexDeclaration) SetVertexDeclaration;
  decltype(IDirect3DDevice9Vtbl::SetFVF) SetFVF;
  decltype(IDirect3DDevice9Vtbl::SetTextureStageState) SetTextureStageState;
  decltype(IDirect3DDevice9Vtbl::SetRenderState) SetRenderState;
  decltype(IDirect3DDevice9Vtbl::DrawPrimitive) DrawPrimitive;
  decltype(IDirect3DDevice9Vtbl::DrawIndexedPrimitive) DrawIndexedPrimitive;
  decltype(IDirect3DDevice9Vtbl::DrawPrimitiveUP) DrawPrimitiveUP;
  decltype(IDirect3DDevice9Vtbl::DrawIndexedPrimitiveUP) DrawIndexedPrimitiveUP;
  decltype(IDirect3DDevice9Vtbl::CreateQuery) CreateQuery;
  decltype(IDirect3DDevice9Vtbl::CreateVertexBuffer) CreateVertexBuffer;
  decltype(IDirect3DDevice9Vtbl::CreateIndexBuffer) CreateIndexBuffer;
  decltype(IDirect3DDevice9Vtbl::CreateStateBlock) CreateStateBlock;
  decltype(IDirect3DDevice9Vtbl::BeginStateBlock) BeginStateBlock;
  decltype(IDirect3DDevice9Vtbl::EndStateBlock) EndStateBlock;
  decltype(IDirect3DStateBlock9Vtbl::Capture) StateBlockCapture;
  decltype(IDirect3DStateBlock9Vtbl::Apply) StateBlockApply;
  decltype(IDirect3DQuery9Vtbl::Issue) QueryIssue;
  decltype(IDirect3DQuery9Vtbl::Release) QueryRelease;
  decltype(IDirect3DVertexBuffer9Vtbl::Lock) VBLock;
  decltype(IDirect3DVertexBuffer9Vtbl::Unlock) VBUnlock;
  decltype(IDirect3DIndexBuffer9Vtbl::Lock) IBLock;
} o{};

HMODULE g_backend = nullptr;
std::unordered_map<unsigned long long, std::wstring> g_vso;   // bytecode hash -> path under VertexShaders
std::unordered_map<unsigned long long, std::wstring> g_vsoCode;   // code_hash -> path (fallback: comments stripped)
std::unordered_map<void*, ShaderInfo> g_vs;                     // created vertex shader -> identity
unsigned g_vsKnown = 0, g_vsUnknown = 0;                        // CreateVertexShader results (unknown-shader warning)
bool g_unknownWarned = false;
unsigned g_btbShaders = 0;                                      // BTB effect shaders created (BTB track loaded)
// BTB transforms (btb_draw, btb_instanced_draw) are parked, off unless RBRRTX_BTB=1: three attempts on Ruuhimaki
// (2026-10-22, playtests/2026-10-22-btb-ruuhimaki.md) gave the same broken picture. Recognition and the stage label stay.
bool g_btbOn = false;

float g_c[256][4] = {};
bool g_cset[256] = {};
float g_psc[8][4] = {};                                // pixel shader constants (ps_1_1: c0-c7)
bool g_dirtOn = true;                                  // car dirt decal pass (dirt_draw)
unsigned g_dirtDraws = 0, g_dirtUntagged = 0;
// Ground second layer (layer_draw), Alt+Y cycles the mode. kLayerDecal (default with the baker on, the combination that
// showed Whaanga right in the 2026-11-02 run; set before the stage loads): the twin of t1 (own hash, harvest.cpp) tagged
// decal, never t1 itself, over the terrain-tagged base. kLayerBake (EXPERIMENT, grey / colourless in that run): the layer is
// t1 itself, drawn after its base and tagged terrain at run time, so the baker composites base + layer like the game's
// lrp(t1.a * v0.a, t1, t0). kLayerOff: no second layer, the game's pixel shader runs inside the bake (A/B reference).
// A change mid-stage is not applied (twins need the texture sources held at upload, and switching the tags under a loaded
// stage faded the ground to grey): g_layerWant waits for the menu or the next stage (ground_frame).
enum { kLayerOff = 0, kLayerBake = 1, kLayerDecal = 2 };
int g_layerMode = kLayerOff;
int g_layerWant = kLayerOff;                           // Alt+Y's choice, applied at the next stage change / in the menu
bool g_layerOn = false;                                // a second layer is drawn (mode != off)
bool g_bakerOn = false;                                // rtx.terrainBaker.enableBaking in the conf Remix reads
bool g_groundBaseFF = true;                            // bake mode: opaque RLDouble bases drawn without their pixel shader, alpha 1
unsigned g_layerDraws = 0, g_layerMissing = 0, g_layerUntagged = 0, g_groundFFBases = 0;
void ground_tag_layer(unsigned long long hx);
void ground_note_draw(IDirect3DDevice9* dev, bool vsDriven);
void ground_frame();
void ground_tags_flush(bool force);
void ground_tags_load();
void ground_mode_set(int mode);
void ground_layer_request(int want, bool quiet);       // Alt+Y and the F3 Debug page: the second-layer mode for the next stage load
// A/B switches (F3 Debug page; materials.ini [ground] / [skip_draws] give the starting values).
bool g_stage0Fix = true;                               // RLDouble draws whose stage 0 is DISABLE get stage 0 = texture x diffuse (comp_draw)
bool g_flareSkip = true;                               // the game's lens flare: screen-space additive draws after the sun query are not drawn
bool g_dbgHideOutlineSprite = false, g_dbgHideClouds = false, g_dbgHideSunBillboard = false;   // sun-outline candidates, live A/B (F3)
unsigned g_oqEndFrame = ~0u;                           // frame of the last occlusion query END (the game's sun visibility test)
unsigned g_flareSkipped = 0, g_hideSkipped = 0;
void f3_present(IDirect3DDevice9* dev);                // F3 menu glue (below, after ground_frame)
void f3_before_reset();
void f3_after_reset(IDirect3DDevice9* dev, HRESULT hr);
void texture_probe_draw(IDirect3DDevice9* dev);
void f3_install(IDirect3DDevice9* dev);
const wchar_t* ground_tag_name(unsigned long long hx);
bool g_haveCam = false, g_frameCam = false;
D3DMATRIX g_V = {}, g_P = {};
float g_carPos[3] = {};                               // last car draw's world position (culling keep radius)
float g_carWorld[16] = {};                             // last car draw's world matrix (lit parts: body or wheel)
bool g_haveCar = false;
unsigned g_carDraws = 0;                               // car draws seen (NUMOS ground: a fresh position after a load)
DMat g_VPinv = {};
DMat g_VPd = {};                                       // this frame's view*projection (c0 of the first world-space draw)
void* g_curVS = nullptr;
const ShaderInfo* g_curInfo = nullptr;
bool g_primaryRT = true;
void* g_backbuffer = nullptr;
std::unordered_map<void*, bool> g_rtPrimary;
unsigned g_rtLogged = 0;

// Per-frame draw counts by shader, split into primary render target / off-screen (diagnostics).
struct DrawCount { unsigned primary = 0, offscreen = 0, inQuery = 0; };   // inQuery: drawn with an occlusion query open
const ShaderInfo kFixedFunction = { L"(fixed-function)", 0 };
const ShaderInfo kUnknownShader = { L"(unknown VS)", 0 };
std::unordered_map<const ShaderInfo*, DrawCount> g_byShader;

// Vertex layouts (diagnostics): what each shader is fed. Remix needs float UVs/normals it can read from the
// declaration; RBR's world went black with "Trying to bind a texture to a mesh without UVs" (2026-10-15).
std::unordered_map<void*, std::wstring> g_declText;            // created declaration -> element list
std::unordered_set<void*> g_declRhw;                           // created declarations with POSITIONT (screen space)
void* g_curDecl = nullptr;
DWORD g_curFVF = 0;
// Which vertex input the device uses: the last of SetFVF / SetVertexDeclaration (0 = not known). The Remix bridge
// cannot tell: its GetFVF keeps the last SetFVF after a SetVertexDeclaration and GetVertexDeclaration the last
// declaration after a SetFVF (bridge client d3d9_device.cpp). DirectorsCut's menu text (ID3DXFont, declaration) was
// taken for its XYZRHW panels' FVF, drawn with identity transforms and so off screen (2026-10-22).
enum { kVinUnknown, kVinFvf, kVinDecl };
int g_vin = kVinUnknown;
bool g_recording = false;                                      // between BeginStateBlock and EndStateBlock
bool screen_space_input(IDirect3DDevice9* dev);
std::unordered_map<unsigned long long, bool> g_layoutLogged;    // (shader, layout) pairs already logged

// Material hint for Remix. Remix builds a draw's material from the fixed-function stage-0 states (COLOROP/ARGs,
// ALPHAOP/ARGs) and D3DRS_LIGHTING even when a pixel shader is bound (d3d9_rtx_utils.cpp setTextureStageState /
// setLegacyMaterialState). With a pixel shader bound those states do not affect the raster image, so around each
// shader draw we set "texture x vertex colour" (what RBR's ps_1_1 shaders compute) and restore the game's values
// right after, because RBR's own state cache must keep seeing what it set. Off by default since the 09:57 run: RBR's
// world already uses MODULATE2X(TEXTURE, DIFFUSE) and the hint blackened draws without vertex colour (car, lights).
//
// Transform bisect mode (RBRRTX_XF_MODE, default 0):
//   0 ours V+P+W, 1 ours V+P / game W, 2 game V+P / ours W, 3 game V+P+W (layer passes transforms through).
// 18:40 run: all four looked the same on the RSF main menu page (which may have no 3D backdrop at all). The 15:38 run
// had cycled sun flipped / sun off / material hint with no visible change.
const D3DTEXTURESTAGESTATETYPE kTss[6] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
                                           D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2 };
const DWORD kTssWanted[6] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE };
const DWORD kTssDefault[6] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_CURRENT };
DWORD g_tss0[6] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_CURRENT };
DWORD g_lighting = TRUE;
bool g_hintEnabled = false;
int g_testMode = 0;
const wchar_t* kTestModes[4] = { L"mode 0: ours V+P+W", L"mode 1: ours V+P, game W", L"mode 2: game V+P, ours W",
                                 L"mode 3: game V+P+W (pass-through)" };
unsigned g_hinted = 0;                                          // shader draws hinted this frame (diagnostics)

// transform slots: 0 world, 1 view, 2 projection
const D3DTRANSFORMSTATETYPE kXf[3] = { D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION };
D3DMATRIX g_dev[3] = {}, g_game[3] = {};
bool g_devValid[3] = {}, g_gameSet[3] = {};

bool g_enabled = true;
unsigned g_frame = 0, g_detailFrames = 0;

remixapi_Interface g_api = {};
bool g_apiOk = false, g_sunWanted = true, g_sunOn = true, g_sunDrawn = false;
// Remix Plus (remixplus_api.cpp) and its NUMOS sky (rtx.skyMode = 1 in the conf Remix reads; RBRRTX_NUMOS=0 turns the
// NUMOS handling off). NUMOS draws the sky, clouds and fog and lights the scene with its own sun and moon
// (tools/ref/remixplus/docs/RemixSkyAPI.md), so with it the proxy creates no sun light: the sun's direction, the
// weather and the stage fog go to NUMOS as options (numos_sun, atmo_frame). Config: rbr-rtx\config-remixplus\.
bool g_remixPlus = false, g_numos = false;
remixapi_LightHandle g_sun = nullptr;
float g_sunDir[3] = {}, g_sunRadiance[3] = { 6.0f, 5.7f, 5.2f };
unsigned g_sunCreates = 0;
// Debug sun (user 2026-10-16, judging normal maps needs low, raking light): / (keys.ini) switches to a manual sun that
// starts where the sun shown is; / again = the game's sun. Night (user 2026-10-17): below the horizon down to -18
// degrees; the sun light fades out between +2 and -4 degrees and Remix's sky light (rtx.skyBrightness) dims to 1 % at -12.
// Since 2026-11-01 (user: "control time of day and NORTH heading"; "it starts in some weird place" and "remembers the
// custom sun in different stages": under NUMOS g_sunDir was never updated, so / started from an old debug sun): the
// manual sun is a clock time on NUMOS's solar path (latitudeDegrees, dayOfYear of the conf) plus a north heading that
// turns the whole path. ; / ' = time earlier / later (1 h per second held), [ / ] = north heading (30 degrees per
// second). Both start from the sun on screen (NUMOS: the values sent last), so the first press changes nothing.
bool g_sunManual = false, g_sunMoving = false;
float g_sunAz = 0.0f, g_sunEl = 30.0f;                 // degrees; elevation above the horizon (proxy convention)
float g_dbgHour = 12.0f, g_dbgNorth = 0.0f;           // debug sun clock and north heading
float g_sunShownEl = 90.0f;                            // elevation of the sun shown (game sun after the rules, or debug sun)
float g_sunFade = 1.0f;                                // sun radiance factor of the light currently created
float g_sunScale = 1.0f;                               // live brightness (sun_brighter / sun_dimmer keys), times g_sunRadiance
float g_sunWeather = 1.0f;                             // stage atmosphere: cloud cover / rain / fog (atmosphere.ini)
float g_skyBase = -1.0f, g_skySet = -1.0f;             // rtx.skyBrightness from the conf (-1 = not read) / last set

struct Stats {
  unsigned draws, known, unknown, ff, offscreen, xformed, noCamera, scenes, skyDraws, glassSkipped, screenSpace, apiWorld;   // apiWorld: draws its retained instance made
  unsigned uiTriggerAt, oqAtUi;                        // draw number of our UI trigger (0 = none); open queries then
  unsigned btb, btbSkipped, btbInstanced, btbCopies;   // BTB effect draws; untransformed; instanced draws, their copies
  unsigned btbRebuilt, btbCamRejected;                 // merged instance buffers built this frame; camera candidates refused
  float worldErr, rlDev, recErr;
  float worldDerivErr;                                 // c8 vs the derived W (grows with distance from the map origin)
  unsigned c8Used, c8Refused;                          // car draws given RBR's exact world matrix / refused it
  float skyErr, skyCamOff;                              // sky VP split error; sky camera distance from the dome centre
} g_st = {};

// Interior-camera UI (playtests/2026-10-15-2255-terrain-esc.md runs 3-5): in cockpit views Remix painted the
// path-traced image over every HUD/menu/plugin draw, although those draws (fixed-function XYZRHW, z and zwrite off,
// identity transforms from before_draw) look like Remix UI. Two suspects, both handled here:
//  a) an occlusion query open during the UI draws: Remix rasterises such draws without its UI check
//     (d3d9_rtx.cpp:451). We mirror Remix's per-query begin/end bookkeeping (d3d9_query.cpp Issue) and log the count;
//  b) a projection Remix sees that is not the identity we think we set (set_xf caches; a state block Apply or
//     MultiplyTransform would bypass it): screen-space draws now always send identity.
// Plus xoxor4d's fix from nfsc-rtx (renderer.cpp manually_trigger_remix_injection): before the first screen-space
// draw on the primary target each frame, draw one invisible fixed-function XYZRHW quad with zwrite off, which Remix
// takes as UI and composites the path-traced image there. RBRRTX_UI_TRIGGER=0 turns it off.
std::unordered_map<void*, bool> g_oqBegun;                     // occlusion query -> begun (Remix's m_state)
int g_oqActive = 0;
bool g_uiTriggerOn = true, g_uiTriggered = false;

// Sky. RBR draws SkyDome.vso and Clouds*.vso as real meshes around the stage; under Remix such a shell is ray
// traced as an opaque occluder that blocks the sun and sky light (the stage went dark, the menu backdrop without a dome
// was lit, 2026-10-15). Modes (RBRRTX_SKY = 0..2; the sky key toggles 2 <-> 1 live; primary target only):
//   2 = draw them as Remix sky: viewport depth range 1..1 for these draws. Remix categorises a draw whose viewport
//       MinZ >= rtx.skyMinZThreshold (default 1.0) as Sky (rtx_types.cpp) and rasterises it into the sky and the sky
//       probe instead of the BVH. On screen nothing changes: the sky sits at the far plane and writes no depth.
//   1 = skip them (black sky; the 2026-10-15 fix).   0 = pass through unchanged (env only: the domes block the sun and
//       the stage fades to black over seconds; players reached it by pressing Pause twice, 2026-10-16).
// The transform bisect mode comes from RBRRTX_XF_MODE (0-3, see kTestModes).
int g_skyMode = 2;

bool is_sky_draw() { return g_enabled && g_primaryRT && g_curInfo && (g_curInfo->flags & SH_SKY); }

// Cockpit windscreen (CarGlassInternal.vso, every car; its texture is the per-car windscreen sheet). Remix traces the
// blended glass as the nearest surface, so DLSS-RR denoises the world behind it with the glass's depth/motion: blur,
// navy sky and smears through the windscreen, worst in low light (2026-10-15). RSF "Windscreen: Hide" / RBRHUD opacity 0
// still draw it (transparent), so that does not help. Skipping all CarGlass*.vso also removed the mirrors (CarGlass.vso,
// 512x512 + cube map). Default (user, 2026-10-15): windscreen skipped (also its crack texture); RBRRTX_GLASS=1 draws
// it, the windscreen key (I) toggles live. "On" still hides/smears things behind it (open issue: make it real Remix glass).
// Fixed 2026-10-16 late (user): rtx.enableStochasticAlphaBlend = True (the world behind the blended glass gets its own
// samples) + glossy glass materials (tools/pbr/make_car_glass_mod.py) -> windscreen drawn by default again;
// RBRRTX_GLASS=0 skips it.
bool g_glassOn = true;

bool is_glass_draw() { return g_enabled && g_primaryRT && g_curInfo && (g_curInfo->flags & SH_GLASS); }

// Foliage and cut-outs (2026-10-16, user: trees too thin, branches cast rectangle shadows). RBR draws scenery cut-outs
// alpha-tested with ALPHAREF 200 (0.78; RLSingleTextureSway/RLSingleTexture/IOSingleTexture) and foliage a second time
// alpha-blended with zwrite off and ALPHAREF 0 for soft edges (RLSingleTextureSway, 841 such draws in the 03:0x dumps).
// Remix takes the cut-off straight from ALPHAREF (d3d9_rtx_utils.cpp), so trees came out thin, and it ray traces the
// soft pass as a second, blended copy of every leaf card. Modes (RBRRTX_FOLIAGE, the foliage key (J) cycles live):
//   1 = scenery cut-outs use ALPHAREF g_cutoutRef (RBRRTX_ALPHAREF, default 96) and the soft pass is skipped (default)
//   2 = lower ALPHAREF, soft pass kept      0 = game values (old behaviour)
int g_foliageMode = 1;
// Opaque cut-outs (docs/research/adr-001-alphacards.md, tier 1): an alpha-tested draw whose stage-0 texture never falls
// below the ALPHAREF in effect (min alpha of mip 0, harvest.cpp) passes every pixel anyway; drawn without alpha test,
// Remix treats it as opaque (no alpha test per ray hit). Many RBR trunks, IO props and foliage atlases are such.
// RBRRTX_OPAQUE_ALPHA=0 = off; key opaque_alpha (none by default); experiment steps "opaque = 0|1".
int g_opaqueAlpha = 1, g_opaqueBase = 1;
unsigned g_opaqueDraws = 0;                            // draws made opaque this way since the last log line
// Alpha-card cut-outs (adr-001 tiers 2-3, alphacut.cpp): alpha-tested scenery draws whose texture has an outline file
// are drawn from a copy of their cards clipped to it. RBRRTX_ALPHACUT=0 = off; key alpha_cut (Alt+O; Alt+C also pressed the game's camera key C); experiment
// steps "alphacut = 0|1".
int g_alphaCut = 1, g_alphaCutBase = 1;
// Go/no-go timing (experiment step "alphacut_solid = 1"): clipped copies drawn with the alpha test forced off, hull ones
// too (wrong look: solid outlines). Tells whether the extra triangles cost more than the alpha test they could save.
bool g_alphaCutSolid = false;
struct { bool valid; INT base; UINT minIdx, numV, startIdx, count; } g_dip = {};
bool g_inDip = false, g_dipTriList = false;            // a DrawIndexedPrimitive (any type / a triangle list) is in flight
float g_btbShift[3] = {};                              // BTB space + shift = RBR world (see btb_camera)
bool g_btbPrev = false;                                // last frame had BTB draws: RBR draws use RBR's own camera
DMat g_rbrVPinv = {};                                  // (RBR view*projection)^-1 from the last car draw with c8
bool g_rbrVPvalid = false;   // the DrawIndexedPrimitive in flight
DWORD g_cutoutRef = 96;
unsigned g_softSkipped = 0, g_refLowered = 0, g_midQuadSkipped = 0;   // g_midQuadSkipped: is_midscene_quad

// Frames since the last comp log line (the per-frame averages in that line divide by it).
unsigned g_logFrames = 0;
UINT g_primCap = UINT_MAX, g_pendingPrims = 0;                             // set per draw by comp_draw, applied by the draw hooks
UINT cap_prims(UINT count) { return count < g_primCap ? count : g_primCap; }
// Off-screen scenery (run 18:41 dumps): every frame RBR redraws sky, ground and foliage into its car reflection cube
// map (6 faces of 128x128, ~200 draws; re/kb passes-and-ui "Off-screen render targets"). Remix rasterizes those and
// does not use them (car paint takes t0 only; reflections are path traced), so they only cost bridge traffic.
// Skipped by default; RBRRTX_OFFSCREEN=1 keeps them. Other off-screen targets (RBRHUD gauges) use other shaders.
bool g_skipOffscreen = true;
unsigned g_offSkipped = 0;
bool g_harvestUsage = false;   // RBRRTX_HARVEST set: also log which shader draws which texture hash (harvest.cpp)

bool is_scenery_draw() { return g_enabled && g_primaryRT && g_curInfo && (g_curInfo->flags & SH_SCENERY); }

// Fixed-function static scenery (STATUS step 3; GTA4/SR3 idea, docs/research/refmods-gta4-sr3.md A2). A draw with a
// vertex shader is vertex-captured by Remix: the VS is re-run into a capture buffer every frame and the BLAS is built
// from that, so even static rocks/road/buildings count as new geometry each frame. Drawn with no vertex shader, Remix
// reads the game's own vertex buffer (static, hashed, cached BLAS; opacity micromaps can stick). RBR's RL*/IO* layouts
// are FF-legal (FLOAT3 position, D3DCOLOR, FLOAT2 UVs) and RL* vertices are in world space, so only the VS is unbound:
// the transforms are the ones before_draw already sets, the pixel shader stays bound (Remix ignores it). Lost in the
// raster image only: RBR's VS haze/scattering on the vertex colour, which Remix never used (decl colour wins, see
// re/kb/systems/remix-source.md Phase 2). Never for *Anim (UV/vertex animation) shaders, and never for IO* = the
// interactive objects the car can push (start clock board, signs, cones): FF needs an exact WORLD and the derived one
// put them in the wrong place (Rosciszow-Walim 4/5, 2026-10-17; vertex capture is immune, it undoes our matrices);
// they move anyway, so a cached BLAS buys nothing there.
// Modes (RBRRTX_FF, ff_scenery key cycles 0 -> 1 -> 2 -> 0, K steps "ff = n"):
//   0 = off (vertex capture, the old way)   1 = static RL* scenery (2026-10-17 K runs: -1.1 ms Walim, -2.4
//   Chirdonhead, -5.9 Santa Lucia)   2 = also swaying foliage/flags/grass farther than g_ffSwayNear from the camera
//   (40 m, RBRRTX_FF_SWAY_NEAR): those stop swaying, near ones keep it (GTA4 idea; all of it frozen gave another
//   -0.5..-1.9 ms, but the user saw grass and flags stop). Default since 2026-10-17 09:06: user saw near sway kept and
//   "visibly way better performance"; K -0.4 ms vs mode 1 (noisy run).
//   Since 2026-10-21 the sway distance defaults to 0 = every swaying draw fixed-function (no sway at all): with OMM off
//   -2.5 ms Legazpi / -1 ms Chirdonhead vs 40 m, and swaying geometry smudged under the denoiser (adr-001, OMM-off run).
//   RBRRTX_FF_SWAY_NEAR = metres brings near sway back.
// The VS is unbound lazily: consecutive FF draws keep it null, the next shader draw rebinds the game's VS (g_vsUnbound).
int g_ffMode = 2, g_ffBase = 2;                        // g_ffBase: start value (env), restored after K steps
bool g_vsUnbound = false;                              // device has no VS while the game thinks g_curVS is bound
unsigned g_ffDraws = 0, g_ffSwayFar = 0;                // FF scenery draws / of those swaying ones, since the last log line
// Mode 2: swaying draws nearer than this (metres) keep their VS. -1 = none (also a batch whose bounds hold the camera).
float g_ffSwayNear = -1.0f;
float g_ffSwayNearBase = -1.0f;                        // start value; experiment steps "ff_sway_near = m" change it

// 0 = keep the VS, 1 = FF, 2 = swaying draw in mode 2: FF only if far enough (ff_sway_far, needs the draw's bounds).
int ff_scenery_class() {
  if (g_ffMode == 0 || !g_enabled || !g_primaryRT || !g_haveCam || g_testMode != 0 || !g_curInfo) return 0;
  const unsigned f = g_curInfo->flags;
  const bool rl = (f & SH_SCENERY) && (f & SH_WORLDSPACE);   // RL* only (IO* = object space, interactive)
  if (!rl || (f & SH_ANIM)) return 0;
  if (!(f & SH_SWAY)) return 1;
  return g_ffMode == 2 ? 2 : 0;
}


bool all_set(int first, int count) {
  for (int i = first; i < first + count; ++i)
    if (!g_cset[i]) return false;
  return true;
}

bool in_backend(const void* p) {
  HMODULE m = nullptr;
  return p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 static_cast<LPCWSTR>(p), &m) && m == g_backend;
}

template <class F>
void patch(F* slot, F hook, F& orig) {
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(F), PAGE_EXECUTE_READWRITE, &old)) return;
  if (*slot != hook) { orig = *slot; *slot = hook; }
  VirtualProtect(slot, sizeof(F), old, &old);
}

void set_xf(IDirect3DDevice9* dev, int i, const D3DMATRIX& m) {
  if (g_devValid[i] && std::memcmp(&g_dev[i], &m, sizeof(D3DMATRIX)) == 0) return;
  o.SetTransform(dev, kXf[i], &m);
  g_dev[i] = m;
  g_devValid[i] = true;
}

// The game's last value for a transform slot (identity if it never set one).
void set_game_xf(IDirect3DDevice9* dev, int i) { set_xf(dev, i, g_gameSet[i] ? g_game[i] : identity()); }

void load_vso_hashes() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const fs::path dir = fs::path(exe).parent_path() / L"VertexShaders";
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file() || _wcsicmp(it->path().extension().c_str(), L".vso") != 0) continue;
    std::ifstream f(it->path(), std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 8) continue;
    size_t size = bytecode_size(reinterpret_cast<const DWORD*>(data.data()), data.size() / 4);
    if (!size) size = data.size();
    g_vso[fnv1a(data.data(), size)] = fs::relative(it->path(), dir).wstring();
    g_vsoCode[code_hash(reinterpret_cast<const DWORD*>(data.data()), data.size() / 4)] = fs::relative(it->path(), dir).wstring();
  }
  clog(L"hashed %zu shader files under %s", g_vso.size(), dir.c_str());
}

// ---------------------------------------------------------------- sun light (Remix API)

float conf_float(const char* name, float dflt);
void conf_vec3(const char* name, float out[3], float dflt);
void remix_set(const char* key, const char* value);

void init_remix_api() {
  if (!g_sunWanted || !g_backend) return;
  auto init = reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(g_backend, "remixapi_InitializeLibrary"));
  if (!init) { clog(L"sun: backend has no remixapi_InitializeLibrary (not Remix); sun disabled"); return; }
  remixapi_InitializeLibraryInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
  remixapi_ErrorCode err = init(&info, &g_api);
  g_apiOk = err == REMIXAPI_ERROR_CODE_SUCCESS && in_backend(reinterpret_cast<const void*>(g_api.CreateLight)) &&
            in_backend(reinterpret_cast<const void*>(g_api.DestroyLight)) &&
            in_backend(reinterpret_cast<const void*>(g_api.DrawLightInstance));
  if (err == REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION) {   // Remix Plus (API 1.x): through remixplus_api.cpp
    g_api = {};
    err = static_cast<remixapi_ErrorCode>(rbrrtx_remixplus_init(g_backend, &g_api, sizeof(g_api)));
    g_apiOk = g_remixPlus = err == REMIXAPI_ERROR_CODE_SUCCESS;
    if (g_apiOk) {
      g_numos = conf_float("rtx.skyMode", 0.0f) >= 0.5f && rbrrtx_env_flag(L"RBRRTX_NUMOS", true);
      // Only NUMOS fogs (user 2026-10-29). rtx.enableFog is Remix's composite of the GAME's D3D9 fog states (RBR's fog
      // shaders, and the FF scenery draws with D3DRS_FOGENABLE): on Remix Plus its colour is scaled by the renderer's
      // pre-exposure (rtx_composite.cpp), so a few % of linear fog at the dashboard became a grey wall over the whole
      // view (run 20261029-053643: Witoszow, Legazpi, Peklo; NUMOS settings changed nothing). The stage's fog reaches
      // NUMOS instead (numos_weather). Also off in config-remixplus\rtx.conf; sent here so no saved layer brings it back.
      if (g_numos) remix_set("rtx.enableFog", "False");
      // A stage's weather at once (user 2026-10-30: the stage started in the last weather and moved into its own):
      // blend 1 = "the game blends itself, fully in the new one" (rtx_numos_weather.cpp), no 2-10 s transition.
      if (g_numos) remix_set("rtx.weather.gameWeatherBlend", "1");
      const RbrApiExt* x = rbrrtx_api_ext();
      clog(L"sun: Remix API ready (Remix Plus, API %llu.%llu.%llu through remixplus_api.cpp); NUMOS sky %s",
           x ? x->version >> 48 : 0ull, x ? (x->version >> 16) & 0xFFFFFFFFull : 0ull, x ? x->version & 0xFFFFull : 0ull, g_numos
           ? L"on: the sun, weather and fog go to rtx.atmosphere.* / rtx.weather.*, no proxy sun light"
           : L"off (rtx.skyMode is not 1): the proxy's own sun and haze as on Remix 1.5.2");
      if (x)
        clog(L"Remix API through the bridge: retained instances %s, batched meshes %s, D3D9 texture import %s, camera %s, "
             L"materials %s, per-frame instances %s", x->createRetained && x->drawRetained ? L"yes" : L"NO",
             x->createMeshBatched ? L"yes" : L"NO", x->importD3D9Texture ? L"yes" : L"NO", x->setupCamera ? L"yes" : L"NO",
             x->createMaterial ? L"yes" : L"NO", x->drawInstance ? L"yes" : L"NO");
    }
  } else if (g_apiOk) clog(L"sun: Remix API ready");
  if (g_apiOk) return;
  if (err == REMIXAPI_ERROR_CODE_NOT_INITIALIZED) clog(L"sun: Remix API refused; set exposeRemixApi = True in .trex\\bridge.conf");
  else clog(L"sun: Remix API unavailable (error %d)", static_cast<int>(err));
}

// Stage sun rule (user 2026-10-17, reworked 2026-10-21). RBR takes the sun (shader constant c41) from
// Maps\<id>\TrackSettings<id>.ini SunDir, one per section = time of day x weather, so c41 already follows the
// scene's time and weather. Rule, for the Remix light only (the game's own constants stay):
//  - a valid stage sun (up to g_sunValidMax, 80 deg) is used 1:1, so Remix shadows match the stage's baked ones;
//  - missing or 0,-1,0 SunDir (183 of 3364 sections; the game then lights from straight overhead: flat light, no
//    shadows to read the road by) or above 80 deg (61 sections): fallback from atmosphere.ini, per time of day the
//    game's own most used preset (time.* fallback_sun), its elevation capped by the lowest sun_max_elev of the
//    region / sky / clouds / time / stage sections (60 or lower, user).
// RBRRTX_SUN_MAX_ELEV = degrees: optional cap on valid stage suns too (default none).
// RBRRTX_SUN_FALLBACK = "azimuth,elevation": fixed fallback, overrides atmosphere.ini (degrees, azimuth = atan2(z, x)).
float g_sunMaxElev = 90.0f, g_sunValidMax = 80.0f;
float g_sunFallbackAz = 167.5f, g_sunFallbackEl = 40.0f;   // current fallback (atmosphere.ini per stage/time)
bool g_sunFallbackFixed = false;                       // RBRRTX_SUN_FALLBACK set: atmosphere.ini does not change it
// Per-stage sun direction (atmosphere.ini [stage.<id>] sun_dir.<Time> / sun_dir = "azimuth, elevation"): replaces the
// game's SunDir where it does not match the stage's own bake (Carvalho de Rei reverse: file 53.8 deg, bake ~22 deg).
bool g_sunOverride = false;
// Main menu (and any time no stage is loaded): RBR's menu sun (162.5 / 22.9) does not match the menu backdrop's bake.
// The user turned it to azimuth 28.0, height kept (2026-10-22). atmosphere.ini [atmosphere] menu_sun_azimuth
// (none = off); the elevation stays the game's.
bool g_inMenu = true;
float g_menuSunAz = 28.0f;
bool g_menuSunOn = true;
float g_sunOverrideAz = 0.0f, g_sunOverrideEl = 0.0f;
// Suns kept in game with the sun_save key (\, user 2026-10-22): rbr-rtx\config\sun-saved.ini, same [stage.<id>]
// sun_dir.<Time> keys as atmosphere.ini plus name/weather, read before atmosphere.ini. Not in the package, so an
// update keeps it; testers send it in to be merged into atmosphere.ini.
wchar_t g_sunSavedPath[MAX_PATH * 2] = {};
RbrStageInfo g_atmoStage = {};                         // stage of the look applied last (atmo_frame)
int g_sunRuleLogged = -1;                              // last rule logged (0 none, 1 capped, 2 fallback)
// Night weather (user 2026-10-23): RSF night presets are day sections with a night comment (Maps\<id>\TrackSettings
// <id>.ini, "Comment = NIGHT", "NIGHT/DARK", "Starry Night" in ~150 sections, or "; Night" after the section name),
// and their SunDir is often a normal day sun (Kaihuavaara 2021 O_E_nosnow_clear: 17 degrees). On such a stage the
// Remix sun's elevation is held inside atmosphere.ini [atmosphere] night_sun = min, max (default -5, 5; none = off).
bool g_night = false, g_nightOn = true;
float g_nightEvMin = 3.0f;                             // auto exposure lower limit at full night (debug_sky)
float g_nightMin = -5.0f, g_nightMax = 5.0f;
// Night presets in the game's evening or morning slot (user 2026-10-30: Rallysprint Hondarribia 2011, section
// [364O_E_hazy_heavycloud] "Night" in the Evening slot, "should be late evening"): held in dusk_sun instead. A night the
// stage's own name gives (Sardian Night) and night presets in the noon slot stay in night_sun.
bool g_nightDusk = false;
float g_duskMin = -9.0f, g_duskMax = -5.0f;

void sun_rule(float d[3]) {
  const float horiz = std::sqrt(d[0] * d[0] + d[2] * d[2]);
  const float elev = std::atan2(-d[1], horiz) * 57.2958f;
  int rule = 0;
  float az = std::atan2(d[2], d[0]) * 57.2958f, el = elev;
  if (g_inMenu && g_menuSunOn) { rule = 4; az = g_menuSunAz; }
  else if (g_sunOverride) { rule = 3; az = g_sunOverrideAz; el = g_sunOverrideEl; }
  else if (elev > g_sunValidMax) { rule = 2; az = g_sunFallbackAz; el = g_sunFallbackEl; }
  else if (elev > g_sunMaxElev) { rule = 1; el = g_sunMaxElev; }
  const float nMin = g_nightDusk ? g_duskMin : g_nightMin, nMax = g_nightDusk ? g_duskMax : g_nightMax;
  if (!g_inMenu && g_night && g_nightOn && (el > nMax || el < nMin)) {
    rule = 5;
    el = std::clamp(el, nMin, nMax);
  }
  if (rule) {
    const float e = el * 0.0174533f, a = az * 0.0174533f;
    d[0] = std::cos(e) * std::cos(a);
    d[1] = -std::sin(e);
    d[2] = std::cos(e) * std::sin(a);
  }
  if (rule != g_sunRuleLogged) {
    g_sunRuleLogged = rule;
    clog(L"sun rule: game sun elevation %.1f deg -> %s (azimuth %.1f, elevation %.1f)", elev,
         rule == 5 ? (g_nightDusk ? L"night weather in an evening/morning slot, held in dusk_sun (atmosphere.ini)" : L"night weather, held in night_sun (atmosphere.ini)") : rule == 4 ? L"menu azimuth (atmosphere.ini)" : rule == 3 ? L"stage override (atmosphere.ini)" : rule == 2 ? L"missing or overhead, fallback" : rule == 1 ? L"capped" : L"kept 1:1", az, el);
  }
}

void remix_set(const char* key, const char* value);

// NUMOS sun (g_numos): the direction the proxy would light with (sun_rule, debug sun) as NUMOS angles. NUMOS's sun points
// to (cos e sin a, sin e, cos e cos a) in Remix's world (Y up, numos renderer.cpp updateCelestial), so a = atan2(x, z)
// of the vector to the sun, -d. config-remixplus\atmosphere.ini [numos] azimuth_offset / azimuth_sign correct it if
// the shadows turn out mirrored or rotated against the stage's baked ones. Sent when it moved 0.05 degrees.
float g_numosAzOffset = 0.0f, g_numosAzSign = 1.0f;
float g_numosSunIntensity = 1.09f, g_numosLightScale = 1.0f;   // the conf's rtx.atmosphere.sunIntensity / directionalLightRadianceScale
float g_numosSunWeather = 1.0f;                        // atmosphere.ini "sun" factors (per-stage softer sun)
float g_numosSentEl = -1000.0f, g_numosSentAz = -1000.0f, g_numosSentI = -1.0f, g_numosSentL = -1.0f;
// NUMOS ground level (run 20261029-045150, marks on Witoszow and Peklo, sweep on Legazpi: a grey wall over the whole
// view, the dashboard included, on forest stages; open Gestel was fine). NUMOS's automatic ground level comes from
// rays cast down from 4 km above the camera, smoothed over 4 km (numos renderer.cpp updateGroundLevel); over forest
// they land on the canopy, so "ground" sits above the road and the camera underground, where the height fog and the
// atmosphere no longer hold. [numos] ground = car (default): groundLevelAuto off and groundLevelWorldUnits = the car's
// height - ground_offset, low-passed over 3 s (jumps at a new stage), sent when it moved 1 m. ground = auto: NUMOS's own.
bool g_numosGroundCar = true, g_numosGroundManual = false;
float g_numosGroundOffset = 0.5f, g_numosGround = 0.0f, g_numosGroundSent = -1.0e9f;
bool g_numosFogAlbedo = true;                          // [numos] fog_albedo_from_game: albedo from the stage's fog colour
float g_numosFogExtra = 20.0f;                         // [numos] fog_extra: metres added to every fog visibility (user 2026-10-29)
bool g_numosFogHalfway = true;                         // [numos] stage_fog_match = halfway (else end)
// Main menu under NUMOS (user 2026-10-30): the base weather (no preset: "Menu, RBR" binds none unless a preset is made
// for it) and the menu sun as before (sun_rule rule 4). Coming back from a stage (user, second request: "its time of
// day only applies to the main menu and slowly moves forward over the next 20 seconds"), the menu starts at the
// stage's time of day and the clock runs forward to the menu sun's next time over [menu] transition_seconds: the
// stage's sun height gives its hour on NUMOS's solar path (latitudeDegrees, dayOfYear of the conf; afternoon for
// evening and night stages), the menu sun's height the hour it ends at; the azimuth turns as the real sun's would and
// lands on the menu sun's. The menu car's lamps follow the sun's height (headlights.cpp).
float g_menuBlendSeconds = 20.0f;
bool g_menuBlend = false;
ULONGLONG g_menuBlendStart = 0;
float g_menuBlendH0 = 0.0f;                            // clock hour of the stage the menu came from
float g_menuSunEl = 90.0f;                             // the menu sun's height now (blend or menu sun)
float g_stageSentEl = -1000.0f;                        // the last stage sun sent to NUMOS (elevation), -1000 = none
int g_stageSentTime = -1;                              // its game time of day (0 morning, 1 noon, 2 evening)
// The solar path the run forward uses (read from the conf once, when it starts: conf_float reads the whole file).
struct SolarPath { float lat, dec; };
SolarPath g_menuBlendPath = { 0.785f, 0.0f };
// Moon (user 2026-10-30: "some moonlight, it can not be just pitch black ... realistic values"; "make the moon phase
// follow the date of the user's PC"). NUMOS's real moon follows its own date and clock (realMoonEnabled), which our
// stages never set, so it could stand below the horizon all night. Instead NUMOS's game moon 0 (rtx.atmosphere.moon0.*):
// [numos] moon_phase = date (today's phase, moon_phase_today) or a number (0 new, 0.5 full); its light is the real
// Moon's at that phase (~0.25 lux full, nothing at new moon). Placed opposite the sun, at least moon_min_elevation high
// at night, so it is up whenever there is light to give.
float moon_phase_today();
float g_moonPhase = 0.5f, g_moonMinEl = 25.0f;
bool g_moonOn = true;
float g_moonSentEl = -1000.0f, g_moonSentAz = -1000.0f;

// The sun on NUMOS's solar path (its renderer.cpp computeTimeCycleSunAngles: the conf's latitudeDegrees and dayOfYear).
SolarPath solar_path() {
  const float doy = conf_float("rtx.atmosphere.dayOfYear", 80.0f);
  return { conf_float("rtx.atmosphere.latitudeDegrees", 45.0f) * 0.0174533f,
           23.44f * 0.0174533f * std::sin(2.0f * 3.14159265f * (284.0f + doy) / 365.0f) };
}
float solar_elevation(const SolarPath& s, float h) {     // degrees at clock hour h
  const float ha = (h - 12.0f) * 15.0f * 0.0174533f;
  const float x = std::sin(s.lat) * std::sin(s.dec) + std::cos(s.lat) * std::cos(s.dec) * std::cos(ha);
  return std::asin(std::clamp(x, -1.0f, 1.0f)) * 57.2958f;
}
float solar_azimuth(const SolarPath& s, float h) {       // compass degrees (0 north, 90 east) at clock hour h
  const float ha = (h - 12.0f) * 15.0f * 0.0174533f;
  return std::atan2(-std::sin(ha) * std::cos(s.dec),
                    std::cos(s.lat) * std::sin(s.dec) - std::sin(s.lat) * std::cos(s.dec) * std::cos(ha)) * 57.2958f;
}
float solar_hour(const SolarPath& s, float el, bool afternoon) {   // the hour the sun stands el high (clamped)
  const float c = (std::sin(el * 0.0174533f) - std::sin(s.lat) * std::sin(s.dec)) / (std::cos(s.lat) * std::cos(s.dec));
  const float ha = std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.2958f / 15.0f;
  return afternoon ? 12.0f + ha : 12.0f - ha;
}

// The menu's sun while it runs forward from the stage's time (g_menuBlend): el/az in NUMOS's terms, given the menu
// sun's (where it ends). False when the run is over.
bool menu_blend_sun(float* el, float* az) {
  if (!g_menuBlend) return false;
  const float t = static_cast<float>(GetTickCount64() - g_menuBlendStart) * 0.001f / (std::max)(g_menuBlendSeconds, 0.1f);
  if (t >= 1.0f) { g_menuBlend = false; clog(L"atmosphere: menu: back at the menu sun"); return false; }
  const SolarPath& s = g_menuBlendPath;
  float h1 = 1000.0f;                                  // the menu sun's next time after the stage's hour
  for (float base : { 0.0f, 24.0f, 48.0f })
    for (bool pm : { false, true }) {
      const float h = base + solar_hour(s, *el, pm);
      if (h > g_menuBlendH0 + 0.01f && h < h1) h1 = h;
    }
  if (h1 > 999.0f) { g_menuBlend = false; return false; }
  const float e = t * t * (3.0f - 2.0f * t);            // eased: slow out of the stage's time, slow into the menu's
  const float h = g_menuBlendH0 + (h1 - g_menuBlendH0) * e;
  *az = std::fmod(*az + std::remainder(solar_azimuth(s, h) - solar_azimuth(s, h1), 360.0f) + 720.0f, 360.0f);
  *el = solar_elevation(s, h);
  return true;
}

void numos_moon(float sunEl, float sunAz) {
  if (!g_moonOn) return;
  const float el = sunEl < 0.0f ? (std::max)(-sunEl, g_moonMinEl) : -sunEl;   // below the horizon by day
  const float az = std::fmod(sunAz + 180.0f, 360.0f);
  if (std::fabs(el - g_moonSentEl) < 0.1f && std::fabs(std::remainder(az - g_moonSentAz, 360.0f)) < 0.1f) return;
  char v[32];
  sprintf_s(v, "%.2f", el);
  remix_set("rtx.atmosphere.moon0.elevation", v);
  sprintf_s(v, "%.2f", az);
  remix_set("rtx.atmosphere.moon0.rotation", v);
  if (g_moonSentEl < -999.0f) clog(L"NUMOS moon: phase %.2f, elevation %.1f, rotation %.1f (opposite the sun)", g_moonPhase, el, az);
  g_moonSentEl = el;
  g_moonSentAz = az;
}

void numos_sun(const float d[3], float elev) {
  if (g_sun) { g_api.DestroyLight(g_sun); g_sun = nullptr; }
  if (!g_apiOk || !g_enabled) return;
  float az = std::atan2(-d[0], -d[2]) * 57.2958f * g_numosAzSign + g_numosAzOffset;
  az = std::fmod(az + 720.0f, 360.0f);
  if (g_inMenu) {
    if (!g_sunManual) menu_blend_sun(&elev, &az);
    g_menuSunEl = elev;
  } else if (!g_sunManual) {                           // the menu runs forward from the stage's own sun, not a debug sun
    g_stageSentEl = elev;
  }
  numos_moon(elev, az);
  char v[32];
  const float dAz = std::fabs(std::remainder(az - g_numosSentAz, 360.0f));
  if (std::fabs(elev - g_numosSentEl) >= 0.05f || dAz >= 0.05f) {
    sprintf_s(v, "%.3f", elev);
    remix_set("rtx.atmosphere.sunElevation", v);
    sprintf_s(v, "%.3f", az);
    remix_set("rtx.atmosphere.sunRotation", v);
    if (g_numosSentEl < -999.0f) clog(L"NUMOS sun: elevation %.1f, rotation %.1f (proxy direction %.3f %.3f %.3f)", elev, az, d[0], d[1], d[2]);
    g_numosSentEl = elev;
    g_numosSentAz = az;
  }
  const float k = g_numosSunIntensity * g_sunScale * g_numosSunWeather;   // sun_brighter / sun_dimmer keys, stage factor
  if (std::fabs(k - g_numosSentI) > 0.001f) {
    sprintf_s(v, "%.4f", k);
    remix_set("rtx.atmosphere.sunIntensity", v);
    g_numosSentI = k;
  }
  const float l = g_sunOn ? g_numosLightScale : 0.0f;  // sun key: NUMOS's sun and moon light off, the sky stays
  if (l != g_numosSentL) {
    sprintf_s(v, "%.3f", l);
    remix_set("rtx.atmosphere.directionalLightRadianceScale", v);
    g_numosSentL = l;
  }
}

}  // namespace

// Lux per unit of irradiance from an API light under NUMOS, 0 without NUMOS. NUMOS's sun (sunIlluminance x
// sunIntensity, the conf values) is the real sun's 127,000 lux (numos renderer.cpp luxPerUnit), and Remix Plus scales
// every API light by 2^rtx.hostCalibration.lightsEv on top (rtx_host_calibration.h).
float rbrrtx_numos_lux_per_unit() {
  if (!g_numos) return 0.0f;
  float il[3];
  conf_vec3("rtx.atmosphere.sunIlluminance", il, 15.0f);
  const float sun = (0.2126f * il[0] + 0.7152f * il[1] + 0.0722f * il[2]) * conf_float("rtx.atmosphere.sunIntensity", 1.09f);
  return sun > 0.0f ? 127000.0f / sun * std::exp2(conf_float("rtx.hostCalibration.lightsEv", 0.0f)) : 0.0f;
}

// The sun's height as the scene is lit now (degrees): the menu's under NUMOS, else the stage's after the sun rule.
float rbrrtx_sun_shown_elevation() { return g_numos && g_inMenu ? g_menuSunEl : g_sunShownEl; }

// The menu's sun height under NUMOS (degrees: the menu sun, or the run forward from a stage's time); false off the
// menu and on 1.5.2.
bool rbrrtx_menu_sun_elevation(float* el) {
  if (!g_numos || !g_inMenu || g_numosSentEl < -999.0f) return false;
  *el = g_menuSunEl;
  return true;
}

namespace {

void numos_ground() {
  if (!g_numos || !g_apiOk || !g_enabled) return;
  // Run 20261029-053643: pauses, replays and loads flip g_inMenu for a moment, and the first value of a stage was the
  // car of the loading screen (-1.5 on every stage). So: auto again only after 3 s out of a stage, and a stage's first
  // value only after 30 fresh car draws.
  static ULONGLONG last = 0, outSince = 0;
  static unsigned enterDraws = 0;
  static bool waiting = true;
  static int track = -1, pendingTrack = -1;
  const ULONGLONG now = GetTickCount64();
  if (!g_numosGroundCar || g_inMenu || !g_haveCar) {   // menu or ground = auto: NUMOS measures again
    if (!outSince) outSince = now;
    waiting = true;
    pendingTrack = -1;
    if (g_numosGroundManual && (!g_numosGroundCar || now - outSince > 3000)) {
      remix_set("rtx.atmosphere.groundLevelAuto", "True");
      g_numosGroundManual = false;
      g_numosGroundSent = -1.0e9f;
      clog(L"NUMOS ground: automatic again (%s)", g_numosGroundCar ? L"no stage" : L"[numos] ground = auto");
    }
    return;
  }
  outSince = 0;
  if (waiting || track != g_atmoStage.trackId) {
    if (!waiting || pendingTrack != g_atmoStage.trackId) { waiting = true; pendingTrack = g_atmoStage.trackId; enterDraws = g_carDraws; }
    if (g_carDraws - enterDraws < 30) return;
    waiting = false;
  }
  const float dt = last ? (std::min)(static_cast<float>(now - last) * 0.001f, 0.5f) : 0.0f;
  last = now;
  const float target = g_carPos[1] - g_numosGroundOffset;
  if (!g_numosGroundManual || track != g_atmoStage.trackId) g_numosGround = target;   // a new stage: jump there
  else g_numosGround += (target - g_numosGround) * (std::min)(1.0f, dt / 3.0f);
  char v[32];
  if (!g_numosGroundManual) {
    remix_set("rtx.atmosphere.groundLevelAuto", "False");
    g_numosGroundManual = true;
  }
  if (track != g_atmoStage.trackId || std::fabs(g_numosGround - g_numosGroundSent) >= 1.0f) {
    if (track != g_atmoStage.trackId)
      clog(L"NUMOS ground: from the car, %.1f (car height %.1f - %.2f m); stage %d", g_numosGround, g_carPos[1],
           g_numosGroundOffset, g_atmoStage.trackId);
    track = g_atmoStage.trackId;
    sprintf_s(v, "%.2f", g_numosGround);
    remix_set("rtx.atmosphere.groundLevelWorldUnits", v);
    g_numosGroundSent = g_numosGround;
  }
}

void update_sun() {
  if (!g_cset[41] && !g_sunManual) return;
  float d[3] = { g_c[41][0], g_c[41][1], g_c[41][2] };  // direction the light travels: y < 0 = sun above the horizon
  if (g_sunManual) {
    const float el = g_sunEl * 0.0174533f, az = g_sunAz * 0.0174533f;
    d[0] = std::cos(el) * std::cos(az);
    d[1] = -std::sin(el);
    d[2] = std::cos(el) * std::sin(az);
  } else if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1e-8f) {
    sun_rule(d);
  }
  const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (len < 1e-4f) return;
  for (float& v : d) v /= len;
  const float elev = std::asin(std::clamp(-d[1], -1.0f, 1.0f)) * 57.2958f;
  g_sunShownEl = elev;                                 // also with the sun key off: the headlights' automatic switch
  if (g_numos) { std::memcpy(g_sunDir, d, sizeof(d)); numos_sun(d, elev); return; }   // g_sunDir: sun_save
  if (!g_apiOk || !g_enabled || !g_sunOn) return;
  const float fade = std::clamp((elev + 4.0f) / 6.0f, 0.0f, 1.0f);   // twilight: full at +2, gone at -4 degrees
  if (fade <= 0.0f) {                                  // night: no sun light at all
    if (g_sun) { g_api.DestroyLight(g_sun); g_sun = nullptr; }
    g_sunFade = 0.0f;
    return;
  }
  const float cosDelta = d[0] * g_sunDir[0] + d[1] * g_sunDir[1] + d[2] * g_sunDir[2];
  if (!g_sun || cosDelta < 0.99999f || std::fabs(fade - g_sunFade) > 0.02f) {   // ~0.25 degrees: lights are immutable, so recreate
    g_sunFade = fade;
    // Remix Plus: the same hash again updates the live sun in place (remixplus_api.cpp header): no destroy.
    const remixapi_LightHandle oldSun = g_sun;
    if (g_sun && !g_remixPlus) g_api.DestroyLight(g_sun);
    g_sun = nullptr;
    remixapi_LightInfoDistantEXT distant = {};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = { d[0], d[1], d[2] };
    distant.angularDiameterDegrees = 0.53f;
    distant.volumetricRadianceScale = 1.0f;
    remixapi_LightInfo light = {};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &distant;
    light.hash = 0x524252'53554E00ULL;                  // "RBRSUN"
    const float k = fade * g_sunScale * g_sunWeather;
    light.radiance = { g_sunRadiance[0] * k, g_sunRadiance[1] * k, g_sunRadiance[2] * k };
    const remixapi_ErrorCode err = g_api.CreateLight(&light, &g_sun);
    std::memcpy(g_sunDir, d, sizeof(d));
    if (++g_sunCreates <= 20 || err != REMIXAPI_ERROR_CODE_SUCCESS)
      clog(L"sun: %s light, direction (%.3f, %.3f, %.3f), radiance (%.2f, %.2f, %.2f), error %d",
           g_sunCreates == 1 ? L"created" : L"re-created", d[0], d[1], d[2],
           g_sunRadiance[0] * k, g_sunRadiance[1] * k, g_sunRadiance[2] * k, static_cast<int>(err));
    if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
      if (oldSun && g_remixPlus) g_api.DestroyLight(oldSun);   // a failed in-place update: the old sun goes
      g_sun = nullptr;
      return;
    }
  }
  g_api.DrawLightInstance(g_sun);
}

void destroy_sun(const wchar_t* why);

// Every Remix option the proxy sets goes through here. The Remix API always writes to the Alt+X User Settings layer
// (rtx_remix_api.cpp remixapi_SetConfigVariable -> getUserLayer(); no other layer, no way to clear), so a "Save" in
// Alt+X would keep these test values. Each one is logged to rbrrtx_api_options.log; after the run rbr-remix.ps1 removes
// from the saved user.conf every option whose value is still the proxy's (user 2026-10-17: tweak Alt+X with confidence).
void remix_set(const char* key, const char* value) {
  if (!g_apiOk || !g_api.SetConfigVariable) return;
  const remixapi_ErrorCode err = g_api.SetConfigVariable(key, value);
  rbrrtx_log_to(L"rbrrtx_api_options.log", L"set %S = %S%s", key, value, err == REMIXAPI_ERROR_CODE_SUCCESS ? L"" : L"  (refused)");
}

// Night with the debug sun: Remix lights the scene from the sky too, so the sky light dims with the sun (1 at the
// horizon, 1 % at -12 degrees). Auto exposure would brighten the dark scene back up (evMin 0 .. evMax 4 = up to 16x,
// test 2026-10-17: a grey night "lit from below"), so the lower exposure limit rises with the night to night_ev_min (atmosphere.ini, 3.0).
// Base values from the conf Remix reads (last line wins); all go back when the debug sun is switched off. Set through
// remix_set like K.
float conf_float(const char* name, float dflt) {
  wchar_t path[MAX_PATH];
  rbrrtx_config_path(L"DXVK_RTX_CONFIG_FILE", L"rtx.conf", path, MAX_PATH);
  std::ifstream in(path);
  float v = dflt;
  for (std::string l; std::getline(in, l);) {
    const size_t k = l.find(name);
    if (k == std::string::npos || l.find_first_not_of(" \t") != k || l.find_first_of(" \t=", k) != k + std::strlen(name)) continue;
    const size_t eq = l.find('=', k);
    if (eq != std::string::npos) v = static_cast<float>(std::atof(l.c_str() + eq + 1));
  }
  return v;
}

void debug_sky() {
  static float evMinBase = 0.0f, evMax = 4.0f, evMinSet = -100.0f;
  static bool confRead = false;
  // Remix Plus: NUMOS dims its own sky; its rebuilt auto exposure has no evMinValue.
  if (!g_apiOk || !g_api.SetConfigVariable || g_remixPlus) return;
  if (!confRead) {                                     // g_skyBase may already come from the stage atmosphere
    confRead = true;
    if (g_skyBase < 0.0f) g_skyBase = conf_float("rtx.skyBrightness", 1.0f);
    evMinBase = conf_float("rtx.autoExposure.evMinValue", -2.0f);
    evMax = conf_float("rtx.autoExposure.evMaxValue", 5.0f);
  }
  const float f = g_sunManual ? std::clamp((g_sunEl + 12.0f) / 12.0f, 0.01f, 1.0f) : 1.0f;
  if (g_skySet < 0.0f && f >= 1.0f) return;            // never touched: leave the options alone
  char v[32];
  const float sky = g_skyBase * f;
  if (std::fabs(sky - g_skySet) >= 0.01f * g_skyBase) {
    sprintf_s(v, "%.4f", sky);
    remix_set("rtx.skyBrightness", v);
    g_skySet = sky;
  }
  const float night = (1.0f - f) / 0.99f;              // 0 at the horizon .. 1 at -12 degrees
  // Night end point: atmosphere.ini night_ev_min (default 3.0; was evMax - 1 = 4, too bright for the user, 2026-10-23).
  const float evNight = (std::min)((std::max)(evMinBase, g_nightEvMin), evMax - 0.5f);
  const float evMin = evMinBase + (evNight - evMinBase) * night;
  if (std::fabs(evMin - evMinSet) >= 0.05f) {
    sprintf_s(v, "%.3f", evMin);
    remix_set("rtx.autoExposure.evMinValue", v);
    evMinSet = evMin;
  }
}

// ---------------------------------------------------------------- stage atmosphere
std::vector<std::wstring> split_list(const std::wstring& v);   // culling section
extern int g_expStep;                                          // experiment / sweep runner (-1 = idle)
// RBR's weather and the stage's region drive Remix's volumetrics (user 2026-10-17): haze density (transmittance
// measurement distance, lower = denser), haze colour, scattering albedo and anisotropy, sky light and sun strength.
// rbr-rtx\config\atmosphere.ini (env RBRRTX_ATMOSPHERE): [region.<name>] with countries = FI,SE,..., [sky.<SkyType>],
// [clouds.<CloudType>], [time.<TimeOfDay>]; every factor multiplies the rtx.conf value (the user's tuned look: Santa
// Lucia, Spain, Crisp, Clear = all 1). Stage settings come from game_state.cpp (RSF stage id -> country). Checked
// every 30 frames while loading / driving; only options that differ from rtx.conf (or from what was sent last) go
// to Remix (API user layer: Alt+X lists them as unsaved; rbr-remix.ps1 strips them from a saved user.conf).
// 2026-10-24 (user: rain looked far too clear next to the raster image; descriptions name the real weather): sections
// [sky.<Sky>.<Time>] and [stage.<id>.<Sky>] too; the stage section's description (weather.cpp) picks <Sky>, <Clouds>
// and <Time> (Morning Noon Evening Night Dawn Sunset) when it names them; stage_fog = s takes the haze from the
// stage's own FogStart/FogEnd (extinction 2 / (FogEnd - FogStart) x s); density multiplies how thick the haze is. The file
// is read again when it is saved (live tuning in a stage).
struct AtmoFactors {
  float dist = 1.0f, col[3] = { 1.0f, 1.0f, 1.0f }, sky = 1.0f, sun = 1.0f;
  float density = 1.0f;                                // haze extinction x this (both ways of setting the haze)
  float stageFog = -1.0f;                              // absolute when set: use the stage's fog x this (0 = off)
  float albedo = -1.0f, aniso = -1000.0f;              // absolute values when set
  float sunMaxElev = -1.0f;                            // absolute when set: fallback sun cap (deg)
  float fbAz = -1000.0f, fbEl = -1.0f;                 // absolute when set: fallback sun "azimuth, elevation" (deg)
  float aerosol = 1.0f;                                // NUMOS: x rtx.atmosphere.aerosolDensity (region haze)
  float fogVis = -1.0f;                                // NUMOS, absolute when set: fog visibility (m) without stage fog
};
struct AtmoRegion { std::wstring name; std::vector<std::wstring> countries; AtmoFactors f; };
bool g_atmoEnabled = true, g_atmoLoaded = false;
std::vector<AtmoRegion> g_atmoRegions;
std::unordered_map<std::wstring, AtmoFactors> g_atmoSections;   // "sky.Crisp", "clouds.Clear", "time.Noon"
std::wstring g_atmoKey, g_atmoLine = L"not applied yet";
wchar_t g_atmoPath[MAX_PATH * 2] = {};
std::unordered_map<std::string, std::string> g_atmoSent;
struct { float dist, col[3], albedo[3], aniso, sky; } g_atmoBase = {};

void conf_vec3(const char* name, float out[3], float dflt) {
  out[0] = out[1] = out[2] = dflt;
  wchar_t path[MAX_PATH];
  rbrrtx_config_path(L"DXVK_RTX_CONFIG_FILE", L"rtx.conf", path, MAX_PATH);
  std::ifstream in(path);
  for (std::string l; std::getline(in, l);) {
    const size_t k = l.find(name);
    if (k == std::string::npos || l.find_first_not_of(" \t") != k || l.find_first_of(" \t=", k) != k + std::strlen(name)) continue;
    const size_t eq = l.find('=', k);
    if (eq != std::string::npos) sscanf_s(l.c_str() + eq + 1, " %f , %f , %f", &out[0], &out[1], &out[2]);
  }
}

AtmoFactors atmo_read(const wchar_t* path, const wchar_t* sec) {
  AtmoFactors f;
  wchar_t v[128];
  if (GetPrivateProfileStringW(sec, L"distance", L"", v, 128, path)) f.dist = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"colour", L"", v, 128, path)) swscanf_s(v, L"%f , %f , %f", &f.col[0], &f.col[1], &f.col[2]);
  if (GetPrivateProfileStringW(sec, L"sky", L"", v, 128, path)) f.sky = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"sun", L"", v, 128, path)) f.sun = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"albedo", L"", v, 128, path)) f.albedo = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"anisotropy", L"", v, 128, path)) f.aniso = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"sun_max_elev", L"", v, 128, path)) f.sunMaxElev = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"fallback_sun", L"", v, 128, path)) swscanf_s(v, L"%f , %f", &f.fbAz, &f.fbEl);
  if (GetPrivateProfileStringW(sec, L"density", L"", v, 128, path)) f.density = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"stage_fog", L"", v, 128, path)) f.stageFog = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"aerosol", L"", v, 128, path)) f.aerosol = static_cast<float>(_wtof(v));
  if (GetPrivateProfileStringW(sec, L"fog_visibility", L"", v, 128, path)) f.fogVis = static_cast<float>(_wtof(v));
  return f;
}

FILETIME g_atmoTime = {};

void atmo_load() {
  g_atmoLoaded = true;
  wchar_t* path = g_atmoPath;
  rbrrtx_config_path(L"RBRRTX_ATMOSPHERE", L"atmosphere.ini", path, MAX_PATH * 2);
  g_atmoRegions.clear();
  g_atmoSections.clear();
  {
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a)) g_atmoTime = a.ftLastWriteTime;
  }
  rbrrtx_config_path(L"RBRRTX_SUN_SAVED", L"sun-saved.ini", g_sunSavedPath, MAX_PATH * 2);
  g_atmoEnabled = GetPrivateProfileIntW(L"atmosphere", L"enable", 1, path) != 0;
  {
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"atmosphere", L"menu_sun_azimuth", L"28.0", v, 32, path);
    g_menuSunOn = swscanf_s(v, L"%f", &g_menuSunAz) == 1;
    GetPrivateProfileStringW(L"atmosphere", L"night_ev_min", L"3.0", v, 32, path);
    g_nightEvMin = static_cast<float>(_wtof(v));
    GetPrivateProfileStringW(L"atmosphere", L"night_sun", L"-5, 5", v, 32, path);
    float lo, hi;
    g_nightOn = swscanf_s(v, L"%f , %f", &lo, &hi) == 2;
    if (g_nightOn) { g_nightMin = (std::min)(lo, hi); g_nightMax = (std::max)(lo, hi); }
    GetPrivateProfileStringW(L"atmosphere", L"dusk_sun", L"none", v, 32, path);   // set in config-remixplus only
    if (swscanf_s(v, L"%f , %f", &lo, &hi) == 2) { g_duskMin = (std::min)(lo, hi); g_duskMax = (std::max)(lo, hi); }
    else { g_duskMin = g_nightMin; g_duskMax = g_nightMax; }   // none: night presets all in night_sun
  }
  wchar_t names[8192];
  if (!GetPrivateProfileSectionNamesW(names, 8192, path)) { g_atmoEnabled = false; clog(L"atmosphere: no %s, off", path); return; }
  for (const wchar_t* sec = names; *sec; sec += wcslen(sec) + 1) {
    const std::wstring s = sec;
    if (s.rfind(L"region.", 0) == 0) {
      AtmoRegion r;
      r.name = s.substr(7);
      wchar_t c[512];
      GetPrivateProfileStringW(sec, L"countries", L"", c, 512, path);
      r.countries = split_list(c);
      r.f = atmo_read(path, sec);
      g_atmoRegions.push_back(r);
    } else if (s.rfind(L"sky.", 0) == 0 || s.rfind(L"clouds.", 0) == 0 || s.rfind(L"time.", 0) == 0 ||
               s.rfind(L"stage.", 0) == 0) {
      g_atmoSections[s] = atmo_read(path, sec);
    }
  }
  g_atmoBase.dist = conf_float("rtx.volumetrics.transmittanceMeasurementDistanceMeters", 200.0f);
  conf_vec3("rtx.volumetrics.transmittanceColor", g_atmoBase.col, 0.999f);
  conf_vec3("rtx.volumetrics.singleScatteringAlbedo", g_atmoBase.albedo, 0.999f);
  g_atmoBase.aniso = conf_float("rtx.volumetrics.anisotropy", 0.0f);
  g_atmoBase.sky = conf_float("rtx.skyBrightness", 1.0f);
  if (g_numos) {
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"numos", L"azimuth_offset", L"0", v, 32, path);
    g_numosAzOffset = static_cast<float>(_wtof(v));
    GetPrivateProfileStringW(L"numos", L"azimuth_sign", L"1", v, 32, path);
    g_numosAzSign = _wtof(v) < 0.0 ? -1.0f : 1.0f;
    g_numosSunIntensity = conf_float("rtx.atmosphere.sunIntensity", 1.09f);
    g_numosLightScale = conf_float("rtx.atmosphere.directionalLightRadianceScale", 1.0f);
    g_numosSentEl = g_numosSentAz = -1000.0f;          // send the sun again (the [numos] corrections may have changed)
    GetPrivateProfileStringW(L"numos", L"ground", L"car", v, 32, path);
    g_numosGroundCar = _wcsicmp(v, L"auto") != 0;
    GetPrivateProfileStringW(L"numos", L"ground_offset", L"0.5", v, 32, path);
    g_numosGroundOffset = static_cast<float>(_wtof(v));
    g_numosGroundSent = -1.0e9f;                       // send the ground again
    g_numosFogAlbedo = GetPrivateProfileIntW(L"numos", L"fog_albedo_from_game", 1, path) != 0;
    GetPrivateProfileStringW(L"numos", L"fog_extra", L"20", v, 32, path);
    g_numosFogExtra = (std::max)(0.0f, static_cast<float>(_wtof(v)));
    GetPrivateProfileStringW(L"numos", L"stage_fog_match", L"halfway", v, 32, path);
    g_numosFogHalfway = _wcsicmp(v, L"end") != 0;
    GetPrivateProfileStringW(L"menu", L"transition_seconds", L"20", v, 32, path);
    g_menuBlendSeconds = (std::max)(0.0f, static_cast<float>(_wtof(v)));
    GetPrivateProfileStringW(L"numos", L"moon_phase", L"date", v, 32, path);
    g_moonOn = _wcsicmp(v, L"none") != 0;
    g_moonPhase = !_wcsicmp(v, L"date") ? moon_phase_today() : std::clamp(static_cast<float>(_wtof(v)), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"numos", L"moon_min_elevation", L"25", v, 32, path);
    g_moonMinEl = std::clamp(static_cast<float>(_wtof(v)), 0.0f, 89.0f);
    g_moonSentEl = g_moonSentAz = -1000.0f;
    if (g_apiOk) {                                     // the game moon instead of NUMOS's real one (none = the real one)
      char p[16];
      sprintf_s(p, "%.3f", g_moonPhase);
      remix_set("rtx.atmosphere.realMoonEnabled", g_moonOn ? "False" : "True");
      remix_set("rtx.atmosphere.moon0.enabled", g_moonOn ? "True" : "False");
      if (g_moonOn) remix_set("rtx.atmosphere.moon0.phase", p);
    }
    clog(L"atmosphere: NUMOS sun rotation = %s atan2 + %.1f deg; conf sunIntensity %.3f, aerosolDensity %.3f; ground %s; "
         L"fog albedo %s", g_numosAzSign < 0.0f ? L"-" : L"", g_numosAzOffset, g_numosSunIntensity,
         conf_float("rtx.atmosphere.aerosolDensity", 1.1f), g_numosGroundCar ? L"from the car" : L"NUMOS's own (auto)",
         g_numosFogAlbedo ? L"from the stage's fog colour" : L"presets / conf");
  }
  clog(L"atmosphere: %s, %zu regions, %zu weather sections from %s; base haze %.0f m colour (%.2f %.2f %.2f) sky %.2f",
       g_atmoEnabled ? L"on" : L"off (enable = 0)", g_atmoRegions.size(), g_atmoSections.size(), path, g_atmoBase.dist,
       g_atmoBase.col[0], g_atmoBase.col[1], g_atmoBase.col[2], g_atmoBase.sky);
}

void atmo_reset() { g_atmoKey.clear(); g_atmoSent.clear(); }

// Sends one option if it differs from the conf value (first time) or from what was sent last.
void atmo_send(const char* key, const std::string& value, const std::string& confValue) {
  auto it = g_atmoSent.find(key);
  if (it == g_atmoSent.end() ? value == confValue : it->second == value) return;
  remix_set(key, value.c_str());
  g_atmoSent[key] = value;
}

std::string fmt3(const float v[3]) { char b[64]; sprintf_s(b, "%.3f, %.3f, %.3f", v[0], v[1], v[2]); return b; }
std::string fmt1(float v) { char b[32]; sprintf_s(b, "%.3f", v); return b; }

// Which car the player drives. RSF loads every car into one slot (carId 6), so the slot number says nothing: the chosen
// car is written to <game>\Cars\Cars.ini [Car0<slot>], so [Car06] for RSF (CarName, ShaderFile = the car's own shaders .ini).
std::wstring car_desc(int carId) {
  if (carId < 0 || carId > 7) return L"?";
  wchar_t ini[MAX_PATH] = {};
  DWORD n = GetModuleFileNameW(nullptr, ini, MAX_PATH);
  wchar_t* slash = (n > 0 && n < MAX_PATH) ? wcsrchr(ini, L'\\') : nullptr;
  if (!slash) return L"?";
  wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), L"Cars\\Cars.ini");
  wchar_t sec[8], name[128] = {}, shaders[MAX_PATH] = {};
  swprintf_s(sec, L"Car%02d", carId);
  GetPrivateProfileStringW(sec, L"CarName", L"", name, 128, ini);
  GetPrivateProfileStringW(sec, L"ShaderFile", L"", shaders, MAX_PATH, ini);
  return L"\"" + std::wstring(name[0] ? name : L"?") + L"\" ([" + sec + L"] in Cars\\Cars.ini, shaders " +
         (shaders[0] ? shaders : L"none") + L")";
}

// The stage's weather section for the current stage/weather (read once per change) and what its description names.
StageWeather g_stageWx;
WeatherText g_stageWxText;
bool g_stageWxNamed = false;

// NUMOS weather (g_numos, user 2026-10-29: the game's chosen weather and conditions, through NUMOS). Per stage/weather:
//  - rtx.weather.gameWeather = the ids below, most specific first; the first one a preset binds wins (RemixSkyAPI.md
//    section 2). Presets: config-remixplus\weather\rbr-*.conf (weather.bindings = HeavyRain, LightFog_HeavyCloud, ...),
//    edited in Alt+X > Sky > Weather. They leave the fog out, so the stage's own fog below shows.
//  - rtx.atmosphere.fogVisibilityMeters from the stage's FogEnd (stage_fog = s: FogEnd / s, all fogged at FogEnd as
//    on the raster image) or fog_visibility = m (0 = none) of the atmosphere.ini sections; density divides it.
//  - rtx.atmosphere.aerosolDensity = the conf value x the sections' aerosol factors (region haze); presets written
//    "aerosolDensity = x<v>" multiply this.
//  - rtx.skyProvider.worldChangeCount: bumped for every new stage, so NUMOS measures the ground level again.
// The sun stays the proxy's (update_sun -> numos_sun); the "sun" factors scale NUMOS's sunIntensity (softer per stage).
void numos_weather(const RbrStageInfo& si, const std::wstring& sky, const std::wstring& clouds, const std::wstring& tod,
                   const wchar_t* region, const AtmoFactors& total, float stageFog, const wchar_t* sunFrom) {
  auto narrow = [](const std::wstring& w) { std::string r; for (wchar_t ch : w) r += static_cast<char>(ch); return r; };
  const std::string s = narrow(sky), c = narrow(clouds), t = narrow(tod);   // ASCII names (game_state.cpp)
  const std::string st = "stage" + std::to_string(si.trackId);
  const std::string ids = st + "_" + s + "_" + c + ", " + st + "_" + s + ", " + st + ", " + s + "_" + c + "_" + t + ", " +
                          s + "_" + c + ", " + s + ", " + c + ", RBR";
  atmo_send("rtx.weather.gameWeather", ids, "");
  static int lastTrack = -1, worldChanges = 0;
  if (si.trackId != lastTrack) {
    lastTrack = si.trackId;
    remix_set("rtx.skyProvider.worldChangeCount", std::to_string(++worldChanges).c_str());
  }
  const float confAerosol = conf_float("rtx.atmosphere.aerosolDensity", 1.1f);
  atmo_send("rtx.atmosphere.aerosolDensity", fmt1(confAerosol * total.aerosol), fmt1(confAerosol));
  const float confFog = conf_float("rtx.atmosphere.fogVisibilityMeters", 0.0f);
  const StageWeather& sw = g_stageWx;
  const float dens = (std::max)(0.01f, total.density);
  float vis = confFog;
  wchar_t fogNote[128] = L"conf value";
  if (stageFog > 0.0f && sw.found && sw.useFog && sw.haveFog == 3 && sw.fogEnd > 1.0f) {
    // RBR's raster fog is linear, FogStart..FogEnd; NUMOS's visibility is where contrast falls to 2 % (transmittance
    // exp(-3.912 d / V)). visibility = FogEnd (until 2026-10-30) made NUMOS's fog far thicker on the way: 32 % at a
    // tenth of FogEnd where RBR has 10 %, 86 % at half where RBR has 50 % (user: haze too close to the camera, objects
    // grey and flat). halfway: half fogged where RBR is half fogged, V = 3.912 / ln 2 x (FogStart + FogEnd) / 2.
    const float end = g_numosFogHalfway ? 2.822f * (std::max)(1.0f, sw.fogStart + sw.fogEnd) : sw.fogEnd;
    vis = end / stageFog / dens;
    swprintf_s(fogNote, L"stage fog %.0f..%.0f m%s / %.2f / density %.2f", sw.fogStart, sw.fogEnd,
               g_numosFogHalfway ? L" matched halfway" : L"", stageFog, dens);
  } else if (total.fogVis >= 0.0f) {
    vis = total.fogVis > 0.0f ? total.fogVis / dens : 0.0f;
    swprintf_s(fogNote, L"fog_visibility %.0f m / density %.2f", total.fogVis, dens);
  }
  if (vis > 0.0f) {                                    // every fog a little clearer (user 2026-10-29: +20 m); 0 stays no fog
    vis += g_numosFogExtra;
    swprintf_s(fogNote + wcslen(fogNote), 128 - wcslen(fogNote), L" + %.0f m", g_numosFogExtra);
  }
  atmo_send("rtx.atmosphere.fogVisibilityMeters", fmt1(vis), fmt1(confFog));
  // The game's fog colour as NUMOS's fog albedo: RBR's night fog is near black (Peklo Snow R 0.01 0.02 0.03), its day fog
  // near white. NUMOS's own 0.9 lit a night fog into a bright wall. Only when the stage's fog is on.
  const float confAlbedo = conf_float("rtx.atmosphere.fogAlbedo", 0.9f);
  float albedo = confAlbedo;
  if (g_numosFogAlbedo && sw.found && sw.useFog && sw.fogCol[0] >= 0.0f && sw.fogCol[1] >= 0.0f && sw.fogCol[2] >= 0.0f)
    albedo = std::clamp(0.95f * (std::max)({ sw.fogCol[0], sw.fogCol[1], sw.fogCol[2] }), 0.03f, 0.95f);
  atmo_send("rtx.atmosphere.fogAlbedo", fmt1(albedo), fmt1(confAlbedo));
  g_numosSunWeather = total.sun;
  wchar_t line[1024];
  swprintf_s(line, L"NUMOS: stage %d %s (%S, region %s), sky %s, clouds %s, %s%s -> weather ids \"%S\"; fog visibility %.0f m (%s), "
                   L"fog albedo %.2f, aerosol x%.2f, sun x%.2f, fallback sun %.1f/%.1f, stage sun %s",
             si.trackId, si.name[0] ? si.name : L"?", si.country[0] ? si.country : "?", region, sky.c_str(), clouds.c_str(),
             tod.c_str(), g_stageWxNamed ? L" (from the stage's description)" : L"", ids.c_str(), vis, fogNote, albedo,
             total.aerosol, total.sun, g_sunFallbackAz, g_sunFallbackEl, sunFrom);
  g_atmoLine = line;
  clog(L"atmosphere: %s", line);
}

// The Moon's phase today (the PC's date, user 2026-10-30: "make the moon phase follow the date of the user's PC"):
// 0 new, 0.5 full; mean synodic month from the new moon of 2000-01-06 18:14 UTC (within about half a day).
float moon_phase_today() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  const double t = static_cast<double>((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
  const double days = t / 864000000000.0 - 145731.0;   // days since 2000-01-01 00:00 UTC (FILETIME counts from 1601)
  const double cycles = (days - 5.7597) / 29.530588853;
  return static_cast<float>(cycles - std::floor(cycles));
}

// Menu <-> stage under NUMOS: the menu gets the base weather at once and, after a stage, runs forward from the stage's
// time to the menu sun (menu_blend_sun); a stage sends its own sun again (numos_weather then sends its weather).
void numos_menu(bool menu) {
  if (!g_numos || !g_apiOk) return;
  if (menu) {
    atmo_send("rtx.weather.gameWeather", "Menu, RBR", "");
    const std::string fog = fmt1(conf_float("rtx.atmosphere.fogVisibilityMeters", 0.0f));
    const std::string aer = fmt1(conf_float("rtx.atmosphere.aerosolDensity", 1.1f));
    const std::string alb = fmt1(conf_float("rtx.atmosphere.fogAlbedo", 0.9f));
    atmo_send("rtx.atmosphere.fogVisibilityMeters", fog, fog);
    atmo_send("rtx.atmosphere.aerosolDensity", aer, aer);
    atmo_send("rtx.atmosphere.fogAlbedo", alb, alb);
    remix_set("rtx.atmosphere.timeCycleEnable", "False");   // the 2-minute menu day of 2026-10-30 is gone
    g_numosSunWeather = 1.0f;
    g_menuBlend = g_stageSentEl > -999.0f && g_menuBlendSeconds > 0.0f;
    if (g_menuBlend) {
      g_menuBlendPath = solar_path();
      g_menuBlendH0 = solar_hour(g_menuBlendPath, g_stageSentEl, g_stageSentTime != 0);
      g_menuBlendStart = GetTickCount64();
    }
    clog(L"atmosphere: menu: base weather (\"Menu, RBR\"), conf fog / haze; %s", g_menuBlend
         ? (L"from the stage's time (sun " + std::to_wstring(g_stageSentEl).substr(0, 5) + L" deg, " +
            std::to_wstring(g_menuBlendH0).substr(0, 5) + L" h) forward to the menu sun in " +
            std::to_wstring(g_menuBlendSeconds).substr(0, 4) + L" s").c_str()
         : L"the menu sun");
    g_stageSentEl = -1000.0f;
  } else {
    g_menuBlend = false;
    g_numosSentEl = g_numosSentAz = -1000.0f;          // the stage's sun again at once
  }
}

void atmo_frame() {
  if (g_frame % 30 != 0 || !g_apiOk || g_expStep >= 0) return;   // K / sweep runs keep their own values
  if (!g_atmoLoaded) atmo_load();
  {                                                    // atmosphere.ini saved: read again and apply (live tuning)
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (g_atmoPath[0] && GetFileAttributesExW(g_atmoPath, GetFileExInfoStandard, &a) &&
        CompareFileTime(&a.ftLastWriteTime, &g_atmoTime) != 0) {
      atmo_load();
      g_atmoKey.clear();
      clog(L"atmosphere: %s saved: read again", g_atmoPath);
    }
  }
  RbrStageInfo si;
  const bool haveInfo = rbrrtx_stage_info(&si);
  const bool inStage = haveInfo && si.trackId > 0 &&
                       (si.gameMode == 1 || si.gameMode == 2 || si.gameMode == 5 || si.gameMode == 8 || si.gameMode == 10);
  // The menu only after two checks (60 frames) with the game in its menu (mode 3) or no stage: between "loading" and
  // "pre-stage" RBR reports another mode for a moment, and the menu sun (day) went to NUMOS for half a second at every
  // stage start (run 20261030-055723, Peklo 05:59:53.5: night -> day -> night; the user saw the sun move into place
  // and a night stage start in daylight). Any other mode keeps the state it had.
  static int menuChecks = 0;
  static bool menuApplied = false;
  menuChecks = inStage ? 0 : (!haveInfo || si.trackId <= 0 || si.gameMode == 3) ? menuChecks + 1 : menuChecks;
  const bool menu = inStage ? false : menuChecks >= 2 ? true : g_inMenu;
  if (menu != g_inMenu || !menuApplied) {              // menu <-> stage: log the sun rule again
    g_inMenu = menu;
    g_sunRuleLogged = -1;
    if (g_inMenu) g_atmoKey.clear();                   // the next stage applies its look again
    if (g_sunManual && menuApplied) {                  // run 20261030-072711: a debug sun lowered to -18 on Noiker
      g_sunManual = false;                             // stayed on and made every later stage full night
      clog(L"debug sun off (%s): the game's sun again", g_inMenu ? L"back in the menu" : L"new stage");
    }
    numos_menu(g_inMenu);
    rbrrtx_world_lights_reset();                       // lamp positions are per scene
    rbrrtx_apiworld_reset(g_inMenu ? L"menu" : L"stage");
    rbrrtx_hudglow_forget();
    menuApplied = true;
  }
  static std::wstring nightKey;
  if (inStage) {                                       // night weather: also with the atmosphere off
    wchar_t k[64];
    swprintf_s(k, L"%d|%d|%d|%d", si.trackId, si.skyType, si.clouds, si.timeOfDay);
    g_stageSentTime = si.timeOfDay;
    if (nightKey != k) {
      nightKey = k;
      if (g_sunManual) { g_sunManual = false; clog(L"debug sun off (new stage): the game's sun again"); }
      rbrrtx_stage_weather(si, &g_stageWx);
      g_stageWxNamed = !g_stageWx.description.empty() &&
                       rbrrtx_weather_from_text(g_stageWx.description, si.skyType, &g_stageWxText);
      if (!g_stageWxNamed) g_stageWxText = WeatherText();
      // A section that names no time: the stage's own name can ("Sardian Night": section [126O_E_heavyrain_heavycloud]
      // without a description sent the game's 44 deg day sun to NUMOS, run 20261030-043219). Only night is taken.
      // Whole words only (the description parser's stems like "dark" or "yo" are too loose for stage names).
      bool nightFromName = false;
      if (g_stageWxText.time < 0) {
        std::wstring word;
        for (const wchar_t* p = si.name;; ++p) {
          if (*p && iswalpha(*p)) { word += static_cast<wchar_t>(towlower(*p)); continue; }
          for (const wchar_t* n : { L"night", L"noche", L"nocturne", L"nocturna", L"nuit", L"notte", L"nacht", L"noc", L"midnight" })
            if (word == n) nightFromName = true;
          word.clear();
          if (!*p) break;
        }
        if (nightFromName) {
          g_stageWxText.time = 3;
          g_stageWxText.night = true;
        }
      }
      if (g_stageWx.found)
        clog(L"stage weather: section [%s], description \"%s\"%s -> %s%s%s%s%s%s; game fog %s %.0f..%.0f m, colour (%.2f %.2f %.2f)",
             g_stageWx.section.c_str(), g_stageWx.description.c_str(), g_stageWx.descAbove ? L" (comment above the section)" : L"",
             g_stageWxNamed ? L"names" : L"names nothing (the section name counts)",
             g_stageWxText.sky >= 0 ? L" sky " : L"", g_stageWxText.sky >= 0 ? rbrrtx_sky_name(g_stageWxText.sky) : L"",
             g_stageWxText.clouds >= 0 ? (std::wstring(L" clouds ") + rbrrtx_cloud_name(g_stageWxText.clouds)).c_str() : L"",
             g_stageWxText.time >= 0 ? L" time " : L"", g_stageWxText.time >= 0 ? rbrrtx_weather_time_name(g_stageWxText.time) : L"",
             g_stageWx.useFog ? L"on" : L"OFF", g_stageWx.fogStart, g_stageWx.fogEnd, g_stageWx.fogCol[0], g_stageWx.fogCol[1],
             g_stageWx.fogCol[2]);
      else clog(L"stage weather: no TrackSettings section for stage %d %s %s %s", si.trackId, rbrrtx_time_name(si.timeOfDay),
                rbrrtx_sky_name(si.skyType), rbrrtx_cloud_name(si.clouds));
      g_night = g_stageWxText.night;
      g_nightDusk = g_night && !nightFromName && si.timeOfDay != 1;   // a night preset in the evening / morning slot
      g_sunRuleLogged = -1;
      if (g_night) clog(L"night weather: %s [%s] says night (game slot %s); sun elevation held in %.1f..%.1f deg (%s)%s",
                        nightFromName ? L"the stage's name, section" : L"section", g_stageWx.section.c_str(),
                        rbrrtx_time_name(si.timeOfDay), g_nightDusk ? g_duskMin : g_nightMin,
                        g_nightDusk ? g_duskMax : g_nightMax, g_nightDusk ? L"dusk_sun" : L"night_sun",
                        g_nightOn ? L"" : L" (OFF: atmosphere.ini night_sun = none)");
    }
  } else if (g_inMenu) {                               // the loading glitch above keeps the stage's night
    nightKey.clear();
    g_night = g_nightDusk = false;
    g_stageWx = StageWeather();
    g_stageWxText = WeatherText();
    g_stageWxNamed = false;
  }
  if (!g_atmoEnabled || !inStage) return;
  wchar_t key[96];
  swprintf_s(key, L"%d|%d|%d|%d", si.trackId, si.skyType, si.clouds, si.timeOfDay);
  if (g_atmoKey == key) return;
  g_atmoKey = key;
  static int carLogged = -1;
  if (si.carId != carLogged) { carLogged = si.carId; clog(L"car: slot %d = %s", si.carId, car_desc(si.carId).c_str()); }
  const std::wstring country(si.country, si.country + strlen(si.country));
  const AtmoRegion* region = nullptr;
  for (const auto& r : g_atmoRegions)
    for (const auto& c : r.countries) if (!_wcsicmp(c.c_str(), country.c_str())) { region = &r; break; }
  AtmoFactors total;
  float stageFog = -1.0f;
  float albedo = -1.0f, aniso = -1000.0f;
  float sunCap = 60.0f, fbAz = 167.5f, fbEl = 40.0f;   // defaults when atmosphere.ini has none
  auto mul = [&](const AtmoFactors* f) {
    if (!f) return;
    total.dist *= f->dist; total.sky *= f->sky; total.sun *= f->sun;
    for (int i = 0; i < 3; ++i) total.col[i] *= f->col[i];
    if (f->albedo >= 0.0f) albedo = f->albedo;
    if (f->aniso > -999.0f) aniso = f->aniso;
    if (f->sunMaxElev >= 0.0f) sunCap = (std::min)(sunCap, f->sunMaxElev);
    if (f->fbAz > -999.0f && f->fbEl >= 0.0f) { fbAz = f->fbAz; fbEl = f->fbEl; }
    total.density *= f->density;
    if (f->stageFog >= 0.0f) stageFog = f->stageFog;
    total.aerosol *= f->aerosol;
    if (f->fogVis >= 0.0f) total.fogVis = f->fogVis;
  };
  auto sec = [&](const wchar_t* prefix, const std::wstring& name) -> const AtmoFactors* {
    auto it = g_atmoSections.find(std::wstring(prefix) + name);
    return it != g_atmoSections.end() ? &it->second : nullptr;
  };
  // The weather the stage's description names wins over the game's three-part name (weather.cpp).
  const WeatherText& wt = g_stageWxText;
  const std::wstring skyName = rbrrtx_sky_name(wt.sky >= 0 ? wt.sky : si.skyType);
  const std::wstring cloudName = rbrrtx_cloud_name(wt.clouds >= 0 ? wt.clouds : si.clouds);
  std::wstring timeName = wt.time >= 0 ? rbrrtx_weather_time_name(wt.time) : rbrrtx_time_name(si.timeOfDay);
  if (!g_atmoSections.count(L"time." + timeName))      // no [time.Night] / [time.Dawn] / [time.Sunset]: nearest RBR time
    timeName = wt.time == 4 ? L"Morning" : wt.time == 3 || wt.time == 5 ? L"Evening" : timeName;
  const std::wstring stageId = std::to_wstring(si.trackId);
  mul(region ? &region->f : nullptr);
  mul(sec(L"sky.", skyName));
  mul(sec(L"clouds.", cloudName));
  mul(sec(L"time.", timeName));
  mul(sec(L"sky.", skyName + L"." + timeName));         // weather x time of day standards
  mul(sec(L"stage.", stageId));                         // per-stage look (soft baked shadows: weaker sun)
  mul(sec(L"stage.", stageId + L"." + skyName));        // per stage and weather
  if (!g_sunFallbackFixed) { g_sunFallbackAz = fbAz; g_sunFallbackEl = (std::min)(fbEl, sunCap); }
  g_atmoStage = si;
  rbrrtx_layer_release_other(si.trackId);              // ground second layer: other stages' textures go
  const wchar_t* sunFrom = L"from the game";
  {                                                    // per-stage sun direction: saved sun, then time-specific key
    const std::wstring stageSec = L"stage." + std::to_wstring(si.trackId);
    const std::wstring timeKey = std::wstring(L"sun_dir.") + rbrrtx_time_name(si.timeOfDay);
    wchar_t v[128] = {};
    if (GetPrivateProfileStringW(stageSec.c_str(), timeKey.c_str(), L"", v, 128, g_sunSavedPath)) sunFrom = L"saved (sun-saved.ini)";
    else if (GetPrivateProfileStringW(stageSec.c_str(), timeKey.c_str(), L"", v, 128, g_atmoPath) ||
             GetPrivateProfileStringW(stageSec.c_str(), L"sun_dir", L"", v, 128, g_atmoPath)) sunFrom = L"override";
    g_sunOverride = v[0] && swscanf_s(v, L"%f , %f", &g_sunOverrideAz, &g_sunOverrideEl) == 2;
    if (!g_sunOverride) sunFrom = L"from the game";
  }
  g_sunRuleLogged = -1;                                // log the rule again for the new stage / time
  if (g_numos) {
    numos_weather(si, skyName, cloudName, timeName, region ? region->name.c_str() : L"none", total, stageFog, sunFrom);
    return;
  }
  float col[3], alb[3];
  for (int i = 0; i < 3; ++i) {
    col[i] = std::clamp(g_atmoBase.col[i] * total.col[i], 0.05f, 0.999f);
    alb[i] = albedo >= 0.0f ? std::clamp(albedo, 0.0f, 0.999f) : g_atmoBase.albedo[i];
  }
  float dist = (std::max)(1.0f, g_atmoBase.dist * total.dist / (std::max)(0.01f, total.density));
  // The stage's own fog (rain, snow, fog weathers; atmosphere.ini stage_fog). The weather factors above shortened the
  // measurement distance but brought the colour near 1 (Rosciszow HeavyRain: 16 m at 0.97 = 97 % kept over 16 m, a
  // light haze): rain looked clear next to the raster's FogEnd 300 m wall. The raster fog is linear (half fogged half
  // way, all at FogEnd); the extinction 2 / (FogEnd - FogStart) follows it through the middle (37 % of the light left
  // half way, 13 % at FogEnd), keeping the hue of the factors above; the scattering albedo takes the hue of the game's
  // fog colour.
  wchar_t fogNote[160] = L"";
  const StageWeather& sw = g_stageWx;
  if (stageFog > 0.0f && sw.found && sw.useFog && sw.haveFog == 3 && sw.fogEnd > 1.0f) {
    const float range = (std::max)(10.0f, sw.fogEnd - sw.fogStart);
    const float sigma = 2.0f / range * stageFog * total.density;   // per metre
    // Grey (run 10, user: purple scene): the hue of the colour above as extinction weights gave (0.05 0.02 0.66) on
    // Rosciszow HeavyRain HeavyCloud (FogEnd 51 m): blue light alone got through. Now the same extinction for all
    // channels, tinted only by the weather factors' colour (a few %), and half of the game's fog hue in the albedo.
    const float tm = (total.col[0] + total.col[1] + total.col[2]) / 3.0f;
    dist = g_atmoBase.dist;
    for (int i = 0; i < 3; ++i)
      col[i] = std::clamp(std::exp(-sigma * dist) * (tm > 1e-3f ? total.col[i] / tm : 1.0f), 0.02f, 0.999f);
    if (sw.fogCol[0] >= 0.0f && sw.fogCol[1] >= 0.0f && sw.fogCol[2] >= 0.0f) {
      const float m = (std::max)({ sw.fogCol[0], sw.fogCol[1], sw.fogCol[2], 0.01f });
      const float a = albedo >= 0.0f ? albedo : (g_atmoBase.albedo[0] + g_atmoBase.albedo[1] + g_atmoBase.albedo[2]) / 3.0f;
      for (int i = 0; i < 3; ++i) alb[i] = std::clamp(a * (0.5f + 0.5f * sw.fogCol[i] / m), 0.0f, 0.999f);
    }
    swprintf_s(fogNote, L"; stage fog %.0f..%.0f m x%.2f: 13 %% of the light left at %.0f m", sw.fogStart, sw.fogEnd,
               stageFog * total.density, 2.0f / sigma);
  }
  const float an = aniso > -999.0f ? std::clamp(aniso, -0.99f, 0.99f) : g_atmoBase.aniso;
  const float sky = g_atmoBase.sky * total.sky;
  atmo_send("rtx.volumetrics.transmittanceMeasurementDistanceMeters", fmt1(dist), fmt1(g_atmoBase.dist));
  atmo_send("rtx.volumetrics.transmittanceColor", fmt3(col), fmt3(g_atmoBase.col));
  atmo_send("rtx.volumetrics.singleScatteringAlbedo", fmt3(alb), fmt3(g_atmoBase.albedo));
  atmo_send("rtx.volumetrics.anisotropy", fmt1(an), fmt1(g_atmoBase.aniso));
  g_skyBase = sky;                                     // debug sun night dims from this
  if (!g_sunManual) { atmo_send("rtx.skyBrightness", fmt1(sky), fmt1(g_atmoBase.sky)); g_skySet = -1.0f; }
  if (std::fabs(total.sun - g_sunWeather) > 0.001f) { g_sunWeather = total.sun; g_sunFade = -1.0f; }   // light re-created
  wchar_t line[512];
  swprintf_s(line, L"stage %d %s (%S, region %s), sky %s, clouds %s, %s%s -> haze %.0f m (x%.2f), colour (%.2f %.2f %.2f), "
                   L"albedo %.2f %.2f %.2f, anisotropy %.2f, sky light %.2f, sun x%.2f, fallback sun %.1f/%.1f, stage sun %s%s",
             si.trackId, si.name[0] ? si.name : L"?", si.country[0] ? si.country : "?", region ? region->name.c_str() : L"none",
             skyName.c_str(), cloudName.c_str(), timeName.c_str(), g_stageWxNamed ? L" (from the stage's description)" : L"",
             dist, total.dist, col[0], col[1], col[2], alb[0], alb[1], alb[2], an, sky, total.sun, g_sunFallbackAz,
             g_sunFallbackEl, sunFrom, fogNote);
  g_atmoLine = line;
  clog(L"atmosphere: %s", line);
}

void lamp_mark_frame();                                // lamp marker (below): finishes a pick, takes the key

// sun_save key: keeps the sun now drawn (debug sun, or the game/override sun) for this stage and time of day.
void sun_save() {
  const RbrStageInfo& si = g_atmoStage;
  float az = g_sunAz, el = g_sunEl;
  if (!g_sunManual) {
    if (g_sunDir[1] == 0.0f) { clog(L"sun save: no sun drawn yet, nothing saved"); return; }
    el = std::asin(std::clamp(-g_sunDir[1], -1.0f, 1.0f)) * 57.2958f;
    az = std::atan2(g_sunDir[2], g_sunDir[0]) * 57.2958f;
  }
  if (g_inMenu || si.trackId <= 0 || !g_sunSavedPath[0]) { clog(L"sun save: no stage loaded, nothing saved"); return; }
  // A sun below the horizon is the debug night sun, never a stage's sun (Zlin 478 kept -18 by accident, 2026-10-23)
  if (el < 0.0f) { clog(L"sun save: sun below the horizon (%.1f deg), nothing saved", el); return; }
  if (GetFileAttributesW(g_sunSavedPath) == INVALID_FILE_ATTRIBUTES) {   // UTF-16 file: stage names keep their letters
    static const wchar_t head[] =
        L"\xFEFF; Suns kept in game with the sun_save key (\\ by default): one [stage.<RBR track id>] per stage,\r\n"
        L"; sun_dir.<time of day> = azimuth, elevation (degrees). Used before atmosphere.ini. Delete a line to go back.\r\n"
        L"; Send this file in so the suns can go into atmosphere.ini for everyone.\r\n";
    HANDLE f = CreateFileW(g_sunSavedPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD n = 0;
      WriteFile(f, head, static_cast<DWORD>((sizeof(head) - sizeof(wchar_t))), &n, nullptr);
      CloseHandle(f);
    }
  }
  const std::wstring sec = L"stage." + std::to_wstring(si.trackId);
  const wchar_t* time = rbrrtx_time_name(si.timeOfDay);
  wchar_t v[160];
  SYSTEMTIME t;
  GetLocalTime(&t);
  WritePrivateProfileStringW(sec.c_str(), L"name", si.name[0] ? si.name : L"?", g_sunSavedPath);
  swprintf_s(v, L"%.1f, %.1f", az, el);
  const bool ok = WritePrivateProfileStringW(sec.c_str(), (std::wstring(L"sun_dir.") + time).c_str(), v, g_sunSavedPath) != 0;
  swprintf_s(v, L"%s, %s, saved %04d-%02d-%02d", rbrrtx_sky_name(si.skyType), rbrrtx_cloud_name(si.clouds), t.wYear, t.wMonth, t.wDay);
  WritePrivateProfileStringW(sec.c_str(), (std::wstring(L"weather.") + time).c_str(), v, g_sunSavedPath);
  if (ok) {                                            // used from now on, also with the debug sun off
    g_sunOverride = true;
    g_sunOverrideAz = az;
    g_sunOverrideEl = el;
    g_sunRuleLogged = -1;
    MessageBeep(MB_OK);
  }
  clog(L"sun save: %s stage %d %s, %s: azimuth %.1f, elevation %.1f -> [%s] in %s", ok ? L"saved" : L"FAILED to save",
       si.trackId, si.name[0] ? si.name : L"?", time, az, el, sec.c_str(), g_sunSavedPath);
}

// ---------------------------------------------------------------- per draw

// Splits a View*Projection matrix (D3D row-vector form) into a rigid view V and a perspective projection P of the
// form [[a,0,0,0],[0,b,0,0],[e,f,c,s],[0,0,d,0]]. Uses only what RBR actually rasterises (c0-c3 of a world-space
// draw), so it cannot disagree with the image the way the c20-c23 "view" did (2-7 units off in the first RBR run).
bool decompose_vp(const DMat& vp, DMat& V, DMat& P, double& recErr) {
  auto el = [&](int r, int c) { return vp.m[r * 4 + c]; };
  double col2[4], z[4];                                  // V column 2 = VP column 3 / s
  const double s = std::sqrt(el(0, 3) * el(0, 3) + el(1, 3) * el(1, 3) + el(2, 3) * el(2, 3));
  if (s < 1e-9) return false;
  for (int r = 0; r < 4; ++r) z[r] = el(r, 3) / s;
  double vc[3][4];                                      // V columns 0..2 (rows 0..3)
  for (int r = 0; r < 4; ++r) vc[2][r] = z[r];
  const double c = (el(0, 2) * z[0] + el(1, 2) * z[1] + el(2, 2) * z[2]);
  const double d = el(3, 2) - c * z[3];
  double ab[2], ef[2];
  for (int k = 0; k < 2; ++k) {                         // VP col k = scale_k * V col k + offset_k * V col 2
    const double off = el(0, k) * z[0] + el(1, k) * z[1] + el(2, k) * z[2];
    double v[4];
    for (int r = 0; r < 4; ++r) v[r] = el(r, k) - off * z[r];
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-9) return false;
    for (int r = 0; r < 4; ++r) vc[k][r] = v[r] / len;
    ab[k] = len;
    ef[k] = off;
  }
  for (int r = 0; r < 4; ++r) col2[r] = vc[2][r];
  // keep a proper rotation (det +1); a mirrored axis goes into the projection scale instead
  const double det = vc[0][0] * (vc[1][1] * col2[2] - vc[1][2] * col2[1]) - vc[1][0] * (vc[0][1] * col2[2] - vc[0][2] * col2[1]) +
                     col2[0] * (vc[0][1] * vc[1][2] - vc[0][2] * vc[1][1]);
  if (det < 0) { for (int r = 0; r < 4; ++r) vc[1][r] = -vc[1][r]; ab[1] = -ab[1]; }
  V = {};
  for (int r = 0; r < 4; ++r) { V.m[r * 4 + 0] = vc[0][r]; V.m[r * 4 + 1] = vc[1][r]; V.m[r * 4 + 2] = vc[2][r]; }
  V.m[15] = 1.0;
  P = {};
  P.m[0] = ab[0]; P.m[5] = ab[1]; P.m[8] = ef[0]; P.m[9] = ef[1]; P.m[10] = c; P.m[11] = s; P.m[14] = d;
  const DMat back = mul_d(V, P);
  recErr = 0;
  for (int i = 0; i < 16; ++i) recErr = (std::max)(recErr, std::fabs(back.m[i] - vp.m[i]));
  return true;
}

std::wstring describe_decl(const D3DVERTEXELEMENT9* e) {
  static const wchar_t* types[] = { L"FLOAT1", L"FLOAT2", L"FLOAT3", L"FLOAT4", L"D3DCOLOR", L"UBYTE4", L"SHORT2",
                                    L"SHORT4", L"UBYTE4N", L"SHORT2N", L"SHORT4N", L"USHORT2N", L"USHORT4N",
                                    L"UDEC3", L"DEC3N", L"FLOAT16_2", L"FLOAT16_4" };
  static const wchar_t* usages[] = { L"POSITION", L"BLENDWEIGHT", L"BLENDINDICES", L"NORMAL", L"PSIZE", L"TEXCOORD",
                                     L"TANGENT", L"BINORMAL", L"TESSFACTOR", L"POSITIONT", L"COLOR", L"FOG",
                                     L"DEPTH", L"SAMPLE" };
  std::wstring s;
  for (; e && e->Stream != 0xFF; ++e) {
    wchar_t part[96];
    swprintf_s(part, L"%ss%u+%u %s %s%u", s.empty() ? L"" : L", ", e->Stream, e->Offset,
               e->Type < 17 ? types[e->Type] : L"?", e->Usage < 14 ? usages[e->Usage] : L"?", e->UsageIndex);
    s += part;
  }
  return s;
}

// Logs each new (shader, vertex layout) pair once, with the stride of stream 0.
void log_layout(IDirect3DDevice9* dev, const ShaderInfo* key) {
  const unsigned long long pair = (static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(key)) << 32) ^
                                  (g_curDecl ? reinterpret_cast<uintptr_t>(g_curDecl) : g_curFVF);
  if (g_layoutLogged.size() >= 200 || !g_layoutLogged.emplace(pair, true).second) return;
  std::wstring layout;
  if (g_curDecl) {
    auto it = g_declText.find(g_curDecl);
    layout = it != g_declText.end() ? it->second : L"(declaration created before the hooks)";
  } else {
    wchar_t f[32];
    swprintf_s(f, L"FVF 0x%08lX", g_curFVF);
    layout = f;
  }
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  if (SUCCEEDED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride)) && vb) vb->lpVtbl->Release(vb);
  clog(L"layout: %s stride %u%s : %s", fs::path(key->name).filename().c_str(), stride,
       g_primaryRT ? L"" : L" (off-screen)", layout.c_str());
}

// Logs the device state of the first on-screen draw of each shader once a camera exists (diagnostics): what Remix
// sees besides the vertices - bound textures, texture-coordinate transforms, blending/alpha test, culling.
std::unordered_map<const ShaderInfo*, bool> g_stateLogged;
void log_draw_state(IDirect3DDevice9* dev, const ShaderInfo* key) {
  if (!g_primaryRT || !g_haveCam || g_stateLogged.size() >= 40 || !g_stateLogged.emplace(key, true).second) return;
  std::wstring s;
  wchar_t part[192];
  swprintf_s(part, L"game stage0 colorop %lu arg1 0x%lx arg2 0x%lx alphaop %lu arg1 0x%lx arg2 0x%lx, lighting %lu; ",
             g_tss0[0], g_tss0[1], g_tss0[2], g_tss0[3], g_tss0[4], g_tss0[5], g_lighting);
  s += part;
  for (DWORD st = 0; st < 2; ++st) {
    IDirect3DBaseTexture9* bt = nullptr;
    dev->lpVtbl->GetTexture(dev, st, &bt);
    if (bt) {
      const D3DRESOURCETYPE rt = bt->lpVtbl->GetType(bt);
      D3DSURFACE_DESC d = {};
      if (rt == D3DRTYPE_TEXTURE) reinterpret_cast<IDirect3DTexture9*>(bt)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(bt), 0, &d);
      swprintf_s(part, L"t%lu %s %ux%u fmt %d levels %lu; ", st, rt == D3DRTYPE_TEXTURE ? L"2D" : rt == D3DRTYPE_CUBETEXTURE ? L"cube" : L"other",
                 d.Width, d.Height, static_cast<int>(d.Format), bt->lpVtbl->GetLevelCount(bt));
      bt->lpVtbl->Release(bt);
    } else {
      swprintf_s(part, L"t%lu none; ", st);
    }
    s += part;
    DWORD tci = 0, ttf = 0;
    dev->lpVtbl->GetTextureStageState(dev, st, D3DTSS_TEXCOORDINDEX, &tci);
    dev->lpVtbl->GetTextureStageState(dev, st, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
    swprintf_s(part, L"tss%lu texcoordindex 0x%lx transformflags %lu; ", st, tci, ttf);
    s += part;
    if (ttf) {
      D3DMATRIX m = {};
      dev->lpVtbl->GetTransform(dev, static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + st), &m);
      swprintf_s(part, L"texture%lu matrix [%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f ..]; ", st, m._11, m._12, m._13, m._14,
                 m._21, m._22, m._23, m._24, m._31, m._32);
      s += part;
    }
  }
  const D3DRENDERSTATETYPE rs[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,
                                    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_COLORVERTEX, D3DRS_FOGENABLE };
  const wchar_t* names[] = { L"blend", L"src", L"dst", L"alphatest", L"aref", L"z", L"zwrite", L"cull", L"lighting", L"colorvertex", L"fog" };
  for (int i = 0; i < 11; ++i) {
    DWORD v = 0;
    dev->lpVtbl->GetRenderState(dev, rs[i], &v);
    swprintf_s(part, L"%s %lu ", names[i], v);
    s += part;
  }
  IDirect3DPixelShader9* ps = nullptr;
  dev->lpVtbl->GetPixelShader(dev, &ps);
  swprintf_s(part, L"ps %p", static_cast<void*>(ps));
  if (ps) ps->lpVtbl->Release(ps);
  s += part;
  clog(L"state: %s : %s", fs::path(key->name).filename().c_str(), s.c_str());
}

// Frame dump (dump key, default P): one log line per draw of the next frame, to see what RBR draws in a given screen
// (e.g. the in-stage ESC menu, invisible under Remix, 2026-10-15). Capped so a dump cannot flood the log.
unsigned g_dumpFrames = 0, g_dumpDraws = 0;

void tex_desc(IDirect3DDevice9* dev, DWORD stage, wchar_t* out, size_t n) {
  IDirect3DBaseTexture9* bt = nullptr;
  dev->lpVtbl->GetTexture(dev, stage, &bt);
  if (!bt) { swprintf_s(out, n, L"-"); return; }
  D3DSURFACE_DESC d = {};
  const D3DRESOURCETYPE rt = bt->lpVtbl->GetType(bt);
  if (rt == D3DRTYPE_TEXTURE) reinterpret_cast<IDirect3DTexture9*>(bt)->lpVtbl->GetLevelDesc(reinterpret_cast<IDirect3DTexture9*>(bt), 0, &d);
  const char* f = reinterpret_cast<const char*>(&d.Format);   // FOURCC formats print as text
  if (d.Format > 0xFFFF) swprintf_s(out, n, L"%p %ux%u %c%c%c%c%s", static_cast<void*>(bt), d.Width, d.Height, f[0], f[1], f[2], f[3],
                                    (d.Usage & D3DUSAGE_RENDERTARGET) ? L" RT" : L"");
  else swprintf_s(out, n, L"%p %s %ux%u fmt %d%s", static_cast<void*>(bt), rt == D3DRTYPE_TEXTURE ? L"2D" : L"cube", d.Width, d.Height,
                  static_cast<int>(d.Format), (d.Usage & D3DUSAGE_RENDERTARGET) ? L" RT" : L"");
  bt->lpVtbl->Release(bt);
}

// Fixed-function draws on the primary target (HUD, menus, plugin overlays): every state that decides what such a draw
// shows, and stage 0's texture as a .dds (small A8R8G8B8/X8R8G8B8 ones, once per dump) next to the log. Written for
// DirectorsCut's menu text, blank under Remix while its panels draw (2026-10-22).
std::wstring sweep_log_dir();
std::unordered_set<void*> g_dumpTexSaved;

void dump_ff_state(IDirect3DDevice9* dev) {
  auto rs = [&](D3DRENDERSTATETYPE s) { DWORD v = 0; dev->lpVtbl->GetRenderState(dev, s, &v); return v; };
  auto ts = [&](DWORD st, D3DTEXTURESTAGESTATETYPE s) { DWORD v = 0; dev->lpVtbl->GetTextureStageState(dev, st, s, &v); return v; };
  auto ss = [&](D3DSAMPLERSTATETYPE s) { DWORD v = 0; dev->lpVtbl->GetSamplerState(dev, 0, s, &v); return v; };
  RECT sc = {};
  dev->lpVtbl->GetScissorRect(dev, &sc);
  clog(L"    ff: tss0 col %lu(%lx,%lx) alpha %lu(%lx,%lx) tci %lx ttf %lu; tss1 col %lu alpha %lu; alphafunc %lu tfactor %08lx "
       L"cw %lx blendop %lu sepalpha %lu srgbw %lu light %lu colvtx %lu diffsrc %lu fog %lu cull %lu fill %lu scissor %lu (%ld,%ld-%ld,%ld) "
       L"clip %lu planes %lx; samp0 mag %lu min %lu mip %lu maxmip %lu lodbias %08lx srgb %lu addr %lu/%lu",
       ts(0, D3DTSS_COLOROP), ts(0, D3DTSS_COLORARG1), ts(0, D3DTSS_COLORARG2), ts(0, D3DTSS_ALPHAOP), ts(0, D3DTSS_ALPHAARG1),
       ts(0, D3DTSS_ALPHAARG2), ts(0, D3DTSS_TEXCOORDINDEX), ts(0, D3DTSS_TEXTURETRANSFORMFLAGS), ts(1, D3DTSS_COLOROP),
       ts(1, D3DTSS_ALPHAOP), rs(D3DRS_ALPHAFUNC), rs(D3DRS_TEXTUREFACTOR), rs(D3DRS_COLORWRITEENABLE), rs(D3DRS_BLENDOP),
       rs(D3DRS_SEPARATEALPHABLENDENABLE), rs(D3DRS_SRGBWRITEENABLE), rs(D3DRS_LIGHTING), rs(D3DRS_COLORVERTEX),
       rs(D3DRS_DIFFUSEMATERIALSOURCE), rs(D3DRS_FOGENABLE), rs(D3DRS_CULLMODE), rs(D3DRS_FILLMODE), rs(D3DRS_SCISSORTESTENABLE),
       sc.left, sc.top, sc.right, sc.bottom, rs(D3DRS_CLIPPING), rs(D3DRS_CLIPPLANEENABLE), ss(D3DSAMP_MAGFILTER),
       ss(D3DSAMP_MINFILTER), ss(D3DSAMP_MIPFILTER), ss(D3DSAMP_MAXMIPLEVEL), ss(D3DSAMP_MIPMAPLODBIAS), ss(D3DSAMP_SRGBTEXTURE),
       ss(D3DSAMP_ADDRESSU), ss(D3DSAMP_ADDRESSV));
  std::wstring decl = L"(none)";
  IDirect3DVertexDeclaration9* vd = nullptr;
  dev->lpVtbl->GetVertexDeclaration(dev, &vd);
  if (vd) {
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1] = {};
    UINT n = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(vd->lpVtbl->GetDeclaration(vd, el, &n))) {
      decl.clear();
      for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
        wchar_t e[48];
        swprintf_s(e, L"%s s%u+%u t%u u%u.%u", i ? L"," : L"", el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage, el[i].UsageIndex);
        decl += e;
      }
    }
    vd->lpVtbl->Release(vd);
  }
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT off = 0, stride = 0;
  dev->lpVtbl->GetStreamSource(dev, 0, &vb, &off, &stride);
  if (vb) vb->lpVtbl->Release(vb);
  D3DMATRIX w = {}, v = {}, p = {};
  dev->lpVtbl->GetTransform(dev, D3DTS_WORLD, &w);
  dev->lpVtbl->GetTransform(dev, D3DTS_VIEW, &v);
  dev->lpVtbl->GetTransform(dev, D3DTS_PROJECTION, &p);
  std::wstring tex = L"";
  IDirect3DBaseTexture9* bt = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &bt);
  if (bt) {
    wchar_t tx[160] = L"";
    if (bt->lpVtbl->GetType(bt) == D3DRTYPE_TEXTURE) {
      IDirect3DTexture9* t = reinterpret_cast<IDirect3DTexture9*>(bt);
      D3DSURFACE_DESC d = {};
      t->lpVtbl->GetLevelDesc(t, 0, &d);
      swprintf_s(tx, L"; t0 levels %lu pool %d usage %lx hash %016llX", t->lpVtbl->GetLevelCount(t), static_cast<int>(d.Pool), d.Usage,
                 rbrrtx_texture_hash(bt));
      tex = tx;
      if ((d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) && d.Width <= 512 && d.Height <= 512 &&
          g_dumpTexSaved.insert(bt).second) {
        wchar_t path[MAX_PATH * 2];
        swprintf_s(path, L"%s\\dump-%u-tex-%p.dds", sweep_log_dir().c_str(), g_frame, static_cast<void*>(bt));
        tex += rbrrtx_save_texture_dds(bt, path) ? L" saved " + fs::path(path).filename().wstring() : L" (not saved)";
      }
    }
    bt->lpVtbl->Release(bt);
  }
  clog(L"    ff: decl %s; stride %u; world %.2f %.2f view %.2f %.2f proj %.4f %.4f %.4f %.4f%s", decl.c_str(), stride,
       w._11, w._41, v._11, v._43, p._11, p._22, p._41, p._44, tex.c_str());
}

bool draw_world_sphere(IDirect3DDevice9* dev, float c[3], float* r, bool force = false);

void dump_draw(IDirect3DDevice9* dev, const wchar_t* fate) {
  if (++g_dumpDraws > 1500) return;
  const ShaderInfo* key = !g_curVS ? &kFixedFunction : g_curInfo ? g_curInfo : &kUnknownShader;
  D3DVIEWPORT9 vp = {};
  dev->lpVtbl->GetViewport(dev, &vp);
  DWORD fvf = 0, blend = 0, src = 0, dst = 0, z = 0, zw = 0, at = 0, aref = 0;
  dev->lpVtbl->GetFVF(dev, &fvf);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  dev->lpVtbl->GetRenderState(dev, D3DRS_SRCBLEND, &src);
  dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ZENABLE, &z);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &aref);
  IDirect3DPixelShader9* ps = nullptr;
  dev->lpVtbl->GetPixelShader(dev, &ps);
  if (ps) ps->lpVtbl->Release(ps);
  wchar_t t0[96], t1[96];
  tex_desc(dev, 0, t0, 96);
  tex_desc(dev, 1, t1, 96);
  if (g_vin == kVinFvf) fvf = g_curFVF;
  // Remix hash of t0 and the draw's world bounding sphere (lights.ini [lamp.*] texture = that hash, 2026-11-01).
  wchar_t where[160] = L"";
  {
    IDirect3DBaseTexture9* bt = nullptr;
    unsigned long long hx = 0;
    if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &bt)) && bt) { hx = rbrrtx_texture_hash(bt); bt->lpVtbl->Release(bt); }
    float c[3], r = 0.0f;
    const bool have = g_curVS && g_frameCam && draw_world_sphere(dev, c, &r, true);   // a dump reads every range
    if (have) swprintf_s(where, L"; hash %016llX at (%.1f %.1f %.1f) r %.1f%s", hx, c[0], c[1], c[2], r, ground_tag_name(hx));
    else if (hx) swprintf_s(where, L"; hash %016llX%s", hx, ground_tag_name(hx));
  }
  clog(L"  dump #%u %s %s %s: vp %lu,%lu %lux%lu z %.2f..%.2f %s 0x%lx%s blend %lu %lu/%lu z %lu zw %lu at %lu/%lu ps %s%s; t0 %s; t1 %s%s",
       g_dumpDraws, fs::path(key->name).filename().c_str(), g_primaryRT ? L"P" : L"O", fate, vp.X, vp.Y, vp.Width, vp.Height,
       vp.MinZ, vp.MaxZ, g_vin == kVinDecl ? L"decl" : L"fvf", g_vin == kVinDecl ? reinterpret_cast<uintptr_t>(g_curDecl) : fvf,
       screen_space_input(dev) ? L" (screen-space)" : L"", blend, src, dst,
       z, zw, at, aref, ps ? L"yes" : L"no", g_oqActive > 0 ? L" IN-OCCLUSION-QUERY" : L"", t0, t1, where);
  if (g_curVS && !g_curInfo) {                         // unknown vertex shader: which one, and what it is fed
    auto it = g_curDecl ? g_declText.find(g_curDecl) : g_declText.end();
    clog(L"    vs %p decl %s", g_curVS, it != g_declText.end() ? it->second.c_str() : L"(fvf)");
  }
  if (!g_curVS && g_primaryRT) dump_ff_state(dev);
}

// Sets the stage-0 states Remix reads to "texture x vertex colour" and lighting off; true = after_draw must restore.
bool apply_material_hint(IDirect3DDevice9* dev) {
  if (!g_hintEnabled) return false;
  for (int i = 0; i < 6; ++i)
    if (g_tss0[i] != kTssWanted[i]) o.SetTextureStageState(dev, 0, kTss[i], kTssWanted[i]);
  if (g_lighting) o.SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  ++g_hinted;
  return true;
}

// One invisible UI draw for Remix (see g_uiTriggerOn): a quad smaller than a pixel in the top-left corner, no shaders,
// zwrite off, all colour channels on (Remix ignores draws with colour write off), fog off. Transforms are identity
// already (before_draw). Goes straight to the backend, not through our hooks; the game's states are restored.
void trigger_ui(IDirect3DDevice9* dev) {
  // Remix takes this frame's lights at this draw: the car lamps go out from the car as drawn so far (headlights.cpp).
  if (g_enabled && g_apiOk) rbrrtx_headlights_refine();
  struct V { float x, y, z, rhw; D3DCOLOR c; };
  const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0 }, { -0.49f, -0.5f, 0, 1, 0 }, { -0.5f, -0.495f, 0, 1, 0 },
                      { -0.49f, -0.495f, 0, 1, 0 } };
  DWORD zw = 0, cw = 0, fog = 0;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
  dev->lpVtbl->GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
  dev->lpVtbl->GetRenderState(dev, D3DRS_FOGENABLE, &fog);
  IDirect3DPixelShader9* ps = nullptr;
  dev->lpVtbl->GetPixelShader(dev, &ps);
  DWORD fvf = 0;
  dev->lpVtbl->GetFVF(dev, &fvf);
  // DrawPrimitiveUP unbinds stream 0 (D3D9 rule; DXVK nulls vertexBuffers[0]) and SetFVF replaces the vertex
  // declaration. RBR and its plugins cache device state and do not re-bind, so both are restored below; without it the
  // draw after the trigger (and later draws from the same buffer) drew nothing: DirectorsCut overlay text, 2026-10-16.
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT vbOffset = 0, vbStride = 0;
  dev->lpVtbl->GetStreamSource(dev, 0, &vb, &vbOffset, &vbStride);
  IDirect3DVertexDeclaration9* decl = nullptr;
  dev->lpVtbl->GetVertexDeclaration(dev, &decl);
  // Remix takes the quad as UI only with an orthographic PROJECTION ([3][3] == 1): the callers outside the screen-space
  // path have the game's (or a plugin's) perspective matrix set.
  D3DMATRIX proj = {};
  dev->lpVtbl->GetTransform(dev, D3DTS_PROJECTION, &proj);
  const bool projSwap = proj._44 != 1.0f;
  const D3DMATRIX id = identity();
  if (projSwap) o.SetTransform(dev, D3DTS_PROJECTION, &id);
  if (zw) o.SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
  if (cw != 0xF) o.SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
  if (fog) o.SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
  if (ps) dev->lpVtbl->SetPixelShader(dev, nullptr);
  o.SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  o.DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
  if (g_vin == kVinDecl && g_curDecl) o.SetVertexDeclaration(dev, static_cast<IDirect3DVertexDeclaration9*>(g_curDecl));
  else if (g_vin == kVinFvf) o.SetFVF(dev, g_curFVF);
  else if (fvf) o.SetFVF(dev, fvf);                    // set before our hooks: what the bridge reports
  else if (decl) o.SetVertexDeclaration(dev, decl);
  dev->lpVtbl->SetStreamSource(dev, 0, vb, vbOffset, vbStride);   // not hooked
  if (vb) vb->lpVtbl->Release(vb);
  if (decl) decl->lpVtbl->Release(decl);
  if (ps) { dev->lpVtbl->SetPixelShader(dev, ps); ps->lpVtbl->Release(ps); }
  if (fog) o.SetRenderState(dev, D3DRS_FOGENABLE, fog);
  if (cw != 0xF) o.SetRenderState(dev, D3DRS_COLORWRITEENABLE, cw);
  if (zw) o.SetRenderState(dev, D3DRS_ZWRITEENABLE, zw);
  if (projSwap) o.SetTransform(dev, D3DTS_PROJECTION, &proj);
  g_st.uiTriggerAt = g_st.draws;
  g_st.oqAtUi = static_cast<unsigned>(g_oqActive);
}

// Returns true when the draw was given a material hint that after_draw must undo.
// A matrix constant of the current BTB shader as a D3D matrix (false: not in the table or not set this session).
bool ctab_matrix(const CtabMatrix& m, D3DMATRIX& out) {
  if (m.reg < 0 || m.count < 3 || m.reg + 4 > 256) return false;
  float r[4][4];
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) r[i][k] = i < m.count ? g_c[m.reg + i][k] : (k == 3 ? 1.0f : 0.0f);
  if (!all_set(m.reg, m.count)) return false;
  // column_major: registers = columns, read like RBR's m4x4 constants (3 registers: the unused 4th column (0,0,0,1)
  // was dropped by the compiler). row_major: registers = rows.
  if (m.columns) out = from_rows(r);
  else for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) out.m[i][k] = r[i][k];
  return true;
}

// BTB camera. rbr_rx draws camera-relative (run 01:11: the camera from BTB matrices sat at (0,0,0) while RBR's c50
// said (-58, 148, 28), so road and trees floated ~148 m above the car), and with its own projection (other near/far).
// One shared, absolute system anchored on the game's camera: rotation and projection come from BTB's view*projection,
// the position from c50 (RBR uploads it before the sky, the first draws of a frame). shift = c50 - BTB camera
// position, so BTB space + shift = RBR world space (shift = 0 if rbr_rx ever draws absolute).
// RBR's own draws in a BTB frame (car, cockpit) then use their exact world (c8) or c0 * (RBR view*projection)^-1,
// never BTB's camera, whose projection differs.
bool btb_camera(const DMat& vpRel) {
  DMat V, P, vinv;
  double rec = 0;
  if (!decompose_vp(vpRel, V, P, rec) || !inverse_d(V, vinv)) return false;
  const double cam[3] = { vinv.m[12], vinv.m[13], vinv.m[14] };
  double shift[3] = { 0, 0, 0 };
  if (g_cset[50]) for (int k = 0; k < 3; ++k) shift[k] = g_c[50][k] - cam[k];
  const double rel = std::sqrt(cam[0] * cam[0] + cam[1] * cam[1] + cam[2] * cam[2]);
  const double toC50 = std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
  if (rel > 1.0 && toC50 > 1.0) { ++g_st.btbCamRejected; return false; }   // neither relative nor absolute: bad W
  DMat T = {};                                         // T(-shift): RBR world -> BTB space
  T.m[0] = T.m[5] = T.m[10] = T.m[15] = 1.0;
  T.m[12] = -shift[0]; T.m[13] = -shift[1]; T.m[14] = -shift[2];
  const DMat vpAbs = mul_d(T, vpRel), vAbs = mul_d(T, V);
  DMat vpinv;
  if (!inverse_d(vpAbs, vpinv)) return false;
  g_V = to_f(vAbs); g_P = to_f(P); g_VPinv = vpinv;
  g_haveCam = g_frameCam = true;
  g_st.recErr = static_cast<float>(rec);
  for (int k = 0; k < 3; ++k) g_btbShift[k] = static_cast<float>(shift[k]);
  return true;
}

// BTB draw: the frame's first BTB draw with a usable worldMatrix gives the camera (W^-1 * worldviewproj, accepted only
// when it puts the camera at BTB's origin or at c50); every draw's world = worldviewproj * camera^-1.
bool btb_draw(IDirect3DDevice9* dev) {
  ++g_st.btb;
  D3DMATRIX wvp, world;
  if (g_curInfo->wvp.reg < 0) return false;            // instanced: btb_instanced_draw sets the transforms
  if (!ctab_matrix(g_curInfo->wvp, wvp)) { ++g_st.btbSkipped; return false; }
  DMat winv;
  if (!g_frameCam && ctab_matrix(g_curInfo->world, world) && inverse_d(to_d(world), winv)) btb_camera(mul_d(winv, to_d(wvp)));
  if (!g_haveCam) { ++g_st.noCamera; return false; }
  set_xf(dev, 1, g_V);
  set_xf(dev, 2, g_P);
  set_xf(dev, 0, to_f(mul_d(to_d(wvp), g_VPinv)));
  ++g_st.xformed;
  return apply_material_hint(dev);
}

// Instanced BTB draw (stream 0 frequency INDEXEDDATA|n, stream 1 = one float4x4 per copy as TEXCOORD1-4 rows; HLSL
// vpos = mul(float4x4(rows), pos), pos = mul(vpos, viewproj)). Remix places a draw with one world matrix, and one
// draw per copy was 22,000 draws a frame (run 01:11: 6 fps). So the copies are merged on the CPU into one vertex/index
// buffer pair in BTB space (positions and normals through each copy's matrix), cached per draw and rebuilt only when
// its source (buffers, ranges, instance data hash) changes. Drawn once with stream 1 = an identity matrix at stride 0
// (the game's shader then rasterises the merged copies as before) and Remix world = viewproj * camera^-1.
struct BtbMerged {
  unsigned long long instHash = 0;
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  UINT verts = 0, prims = 0, stride = 0;
};
std::unordered_map<std::string, BtbMerged> g_btbMerged;
IDirect3DVertexBuffer9* g_btbIdentity = nullptr;        // one float4x4 identity (stream 1 at stride 0)

void btb_merged_release(BtbMerged& m) {
  if (m.vb) { m.vb->lpVtbl->Release(m.vb); m.vb = nullptr; }
  if (m.ib) { m.ib->lpVtbl->Release(m.ib); m.ib = nullptr; }
}

void btb_merged_clear() {
  for (auto& e : g_btbMerged) btb_merged_release(e.second);
  g_btbMerged.clear();
}

// Offsets of stream-0 FLOAT3 POSITION / NORMAL in the current declaration (-1 = none).
void btb_decl_offsets(IDirect3DDevice9* dev, int& pos, int& nrm) {
  pos = nrm = -1;
  IDirect3DVertexDeclaration9* decl = nullptr;
  if (FAILED(dev->lpVtbl->GetVertexDeclaration(dev, &decl)) || !decl) return;
  D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH + 1];
  UINT n = MAXD3DDECLLENGTH + 1;
  if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, e, &n)))
    for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
      if (e[i].Stream == 0 && e[i].Type == D3DDECLTYPE_FLOAT3 && e[i].UsageIndex == 0) {
        if (e[i].Usage == D3DDECLUSAGE_POSITION) pos = e[i].Offset;
        else if (e[i].Usage == D3DDECLUSAGE_NORMAL) nrm = e[i].Offset;
      }
  decl->lpVtbl->Release(decl);
}

// Fills m with the merged copies of the draw in flight. Locks are READONLY; nothing of the game's is changed.
bool btb_merge_build(IDirect3DDevice9* dev, BtbMerged& m, const std::vector<unsigned char>& rows, UINT n,
                     IDirect3DVertexBuffer9* vb0, UINT off0, UINT stride0, IDirect3DIndexBuffer9* ib0, int posOff, int nrmOff) {
  D3DINDEXBUFFER_DESC id = {};
  if (FAILED(ib0->lpVtbl->GetDesc(ib0, &id))) return false;
  const UINT isz = id.Format == D3DFMT_INDEX32 ? 4 : 2, nIdx = g_dip.count * 3;
  std::vector<unsigned> idx(nIdx);
  std::vector<unsigned char> src(static_cast<size_t>(g_dip.numV) * stride0);
  void* p = nullptr;
  if (FAILED(ib0->lpVtbl->Lock(ib0, g_dip.startIdx * isz, nIdx * isz, &p, D3DLOCK_READONLY)) || !p) return false;
  for (UINT i = 0; i < nIdx; ++i) idx[i] = isz == 4 ? static_cast<const unsigned*>(p)[i] : static_cast<const WORD*>(p)[i];
  ib0->lpVtbl->Unlock(ib0);
  for (unsigned v : idx) if (v < g_dip.minIdx || v >= g_dip.minIdx + g_dip.numV) return false;
  const UINT first = off0 + (static_cast<UINT>(g_dip.base) + g_dip.minIdx) * stride0;
  if (FAILED(vb0->lpVtbl->Lock(vb0, first, g_dip.numV * stride0, &p, D3DLOCK_READONLY)) || !p) return false;
  std::memcpy(src.data(), p, src.size());
  vb0->lpVtbl->Unlock(vb0);
  const UINT verts = n * g_dip.numV;
  const bool idx32 = verts > 65535;
  if (FAILED(dev->lpVtbl->CreateVertexBuffer(dev, verts * stride0, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m.vb, nullptr)) ||
      FAILED(dev->lpVtbl->CreateIndexBuffer(dev, n * nIdx * (idx32 ? 4 : 2), D3DUSAGE_WRITEONLY,
                                            idx32 ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_MANAGED, &m.ib, nullptr)))
    return false;
  void* dst = nullptr;
  if (FAILED(m.vb->lpVtbl->Lock(m.vb, 0, 0, &dst, 0)) || !dst) return false;
  unsigned char* out = static_cast<unsigned char*>(dst);
  for (UINT c = 0; c < n; ++c) {
    const float (*r)[4] = reinterpret_cast<const float (*)[4]>(&rows[c * 64]);   // vpos_k = dot(row k, (p, 1))
    for (UINT v = 0; v < g_dip.numV; ++v) {
      unsigned char* dv = out + (static_cast<size_t>(c) * g_dip.numV + v) * stride0;
      std::memcpy(dv, &src[static_cast<size_t>(v) * stride0], stride0);
      float q[3], w[3];
      std::memcpy(q, dv + posOff, 12);
      for (int k = 0; k < 3; ++k) w[k] = r[k][0] * q[0] + r[k][1] * q[1] + r[k][2] * q[2] + r[k][3];
      std::memcpy(dv + posOff, w, 12);
      if (nrmOff >= 0) {
        std::memcpy(q, dv + nrmOff, 12);
        for (int k = 0; k < 3; ++k) w[k] = r[k][0] * q[0] + r[k][1] * q[1] + r[k][2] * q[2];
        const float l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (l > 1e-12f) for (float& x : w) x /= l;
        std::memcpy(dv + nrmOff, w, 12);
      }
    }
  }
  m.vb->lpVtbl->Unlock(m.vb);
  if (FAILED(m.ib->lpVtbl->Lock(m.ib, 0, 0, &dst, 0)) || !dst) return false;
  for (UINT c = 0; c < n; ++c)
    for (UINT i = 0; i < nIdx; ++i) {
      const unsigned v = c * g_dip.numV + (idx[i] - g_dip.minIdx);
      if (idx32) static_cast<unsigned*>(dst)[c * nIdx + i] = v;
      else static_cast<WORD*>(dst)[c * nIdx + i] = static_cast<WORD>(v);
    }
  m.ib->lpVtbl->Unlock(m.ib);
  m.verts = verts; m.prims = n * g_dip.count; m.stride = stride0;
  return true;
}

// The merged buffers for the draw in flight: cached, rebuilt when the instance data changes.
BtbMerged* btb_merged(IDirect3DDevice9* dev, UINT n, IDirect3DVertexBuffer9* inst, UINT instOff, UINT instStride) {
  if (g_dip.base < 0 || g_dip.numV == 0 || g_dip.count == 0 || !g_dipTriList || instStride < 64) return nullptr;
  if (static_cast<unsigned long long>(n) * g_dip.numV > 1500000ull) return nullptr;
  IDirect3DVertexBuffer9* vb0 = nullptr;
  IDirect3DIndexBuffer9* ib0 = nullptr;
  UINT off0 = 0, stride0 = 0;
  if (FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb0, &off0, &stride0)) || !vb0) return nullptr;
  dev->lpVtbl->GetIndices(dev, &ib0);
  int posOff = -1, nrmOff = -1;
  btb_decl_offsets(dev, posOff, nrmOff);
  BtbMerged* result = nullptr;
  std::vector<unsigned char> rows(static_cast<size_t>(n) * 64);
  void* p = nullptr;
  if (ib0 && posOff >= 0 && stride0 >= static_cast<UINT>(posOff) + 12 &&
      SUCCEEDED(inst->lpVtbl->Lock(inst, instOff, n * instStride, &p, D3DLOCK_READONLY)) && p) {
    for (UINT i = 0; i < n; ++i) std::memcpy(&rows[i * 64], static_cast<const unsigned char*>(p) + i * instStride, 64);
    inst->lpVtbl->Unlock(inst);
    char key[192];
    sprintf_s(key, "%p|%u|%u|%p|%d|%u|%u|%u|%u|%p|%u|%u|%u", static_cast<void*>(vb0), off0, stride0, static_cast<void*>(ib0),
              g_dip.base, g_dip.minIdx, g_dip.numV, g_dip.startIdx, g_dip.count, static_cast<void*>(inst), instOff, instStride, n);
    const unsigned long long h = fnv1a(rows.data(), rows.size());
    BtbMerged& m = g_btbMerged[key];
    if (m.vb && m.instHash == h) result = &m;
    else {
      btb_merged_release(m);
      if (btb_merge_build(dev, m, rows, n, vb0, off0, stride0, ib0, posOff, nrmOff)) {
        m.instHash = h;
        result = &m;
        ++g_st.btbRebuilt;
      } else btb_merged_release(m);
    }
  }
  vb0->lpVtbl->Release(vb0);
  if (ib0) ib0->lpVtbl->Release(ib0);
  return result;
}

// false = not handled (the caller draws as usual).
bool btb_instanced_draw(IDirect3DDevice9* dev, HRESULT& hr) {
  if (!g_btbOn || !g_inDip || !g_enabled || !g_primaryRT || !g_curInfo || !(g_curInfo->flags & SH_BTB) || g_curInfo->vp.reg < 0) return false;
  UINT f0 = 0, f1 = 0;
  if (FAILED(dev->lpVtbl->GetStreamSourceFreq(dev, 0, &f0)) || !(f0 & D3DSTREAMSOURCE_INDEXEDDATA)) return false;
  const UINT n = f0 & 0x3FFFFFFFu;
  D3DMATRIX vpm;
  if (!n || !ctab_matrix(g_curInfo->vp, vpm)) return false;
  if (!g_frameCam) btb_camera(to_d(vpm));              // viewproj is BTB's camera without any world in it
  if (!g_haveCam) return false;
  if (g_btbMerged.size() > 4096) btb_merged_clear();
  if (!g_btbIdentity) {
    void* p = nullptr;
    const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    if (SUCCEEDED(dev->lpVtbl->CreateVertexBuffer(dev, 64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g_btbIdentity, nullptr)) &&
        SUCCEEDED(g_btbIdentity->lpVtbl->Lock(g_btbIdentity, 0, 0, &p, 0)) && p) {
      std::memcpy(p, id, 64);
      g_btbIdentity->lpVtbl->Unlock(g_btbIdentity);
    }
  }
  IDirect3DVertexBuffer9 *inst = nullptr, *vb0 = nullptr;
  IDirect3DIndexBuffer9* ib0 = nullptr;
  UINT instOff = 0, instStride = 0, off0 = 0, stride0 = 0;
  dev->lpVtbl->GetStreamSourceFreq(dev, 1, &f1);
  if (FAILED(dev->lpVtbl->GetStreamSource(dev, 1, &inst, &instOff, &instStride)) || !inst) return false;
  BtbMerged* m = g_btbIdentity ? btb_merged(dev, n, inst, instOff, instStride) : nullptr;
  if (!m) { inst->lpVtbl->Release(inst); ++g_st.btbSkipped; return false; }
  dev->lpVtbl->GetStreamSource(dev, 0, &vb0, &off0, &stride0);
  dev->lpVtbl->GetIndices(dev, &ib0);
  dev->lpVtbl->SetStreamSourceFreq(dev, 0, 1);
  dev->lpVtbl->SetStreamSourceFreq(dev, 1, 1);
  dev->lpVtbl->SetStreamSource(dev, 0, m->vb, 0, m->stride);
  dev->lpVtbl->SetStreamSource(dev, 1, g_btbIdentity, 0, 0);
  dev->lpVtbl->SetIndices(dev, m->ib);
  set_xf(dev, 1, g_V);
  set_xf(dev, 2, g_P);
  set_xf(dev, 0, to_f(mul_d(to_d(vpm), g_VPinv)));
  hr = o.DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, m->verts, 0, m->prims);
  dev->lpVtbl->SetIndices(dev, ib0);
  dev->lpVtbl->SetStreamSource(dev, 0, vb0, off0, stride0);
  dev->lpVtbl->SetStreamSource(dev, 1, inst, instOff, instStride);
  dev->lpVtbl->SetStreamSourceFreq(dev, 0, f0);
  dev->lpVtbl->SetStreamSourceFreq(dev, 1, f1);
  if (vb0) vb0->lpVtbl->Release(vb0);
  if (ib0) ib0->lpVtbl->Release(ib0);
  inst->lpVtbl->Release(inst);
  ++g_st.btbInstanced;
  g_st.btbCopies += n;
  ++g_st.xformed;
  return true;
}

// Start clock digits (HedgeHog3D, Textures\start_clock_digits.dds; re/out/hh3d/dec_10070b20.c): every frame the game
// rewrites the 32 vertices of a dynamic buffer (D3DLOCK_DISCARD, XYZ|DIFFUSE|TEX1) with the same positions and new
// digit UVs and draws them fixed-function (8 quads, 16 triangles, index pattern 4k,4k+1,4k+2 / 4k+2,4k+1,4k+3). Under
// path tracing the clock kept the digits of the frame Remix first saw it until ray tracing was toggled (a fresh
// BLAS); flipping a position bit when the UVs changed (2026-10-22 19:45 build) did not help. So each UV change gets a
// new mesh for Remix: the draw switches to the next of three index buffers of ours (written once; the same quads in
// another order: another index hash). Remix drops a BLAS not used in the last frame (rtx.numFramesToKeepBLAS = 1), so
// the one met again three changes later is built anew from the current vertices. RBRRTX_CLOCK=0 = off.
constexpr unsigned long long kClockDigitsHash = 0x544BB1AB506B2F73ull;
bool g_clockOn = true;
IDirect3DVertexBuffer9* g_clockVB = nullptr;           // not owned; compared only
void* g_clockData = nullptr;                           // pointer of the open Lock on g_clockVB
UINT g_clockBytes = 0;
unsigned g_clockUv = 0, g_clockChanges = 0, g_clockVersion = 0, g_clockIbVersion = ~0u;
bool g_clockNow = false;                               // the current draw is the clock
IDirect3DIndexBuffer9* g_clockIB[3] = {};              // ours, managed (survives Reset)
unsigned g_clockSlot = 0;

HRESULT STDMETHODCALLTYPE hk_VBLock(IDirect3DVertexBuffer9* vb, UINT off, UINT size, void** data, DWORD flags) {
  const HRESULT hr = o.VBLock(vb, off, size, data, flags);
  if (SUCCEEDED(hr) && !(flags & D3DLOCK_READONLY)) rbrrtx_apiworld_written(vb);   // API world: buffer is dynamic
  if (vb == g_clockVB) {
    g_clockData = SUCCEEDED(hr) && data && off == 0 && !(flags & D3DLOCK_READONLY) ? *data : nullptr;
    g_clockBytes = size;
    if (g_clockData && !size) {
      D3DVERTEXBUFFER_DESC d = {};
      vb->lpVtbl->GetDesc(vb, &d);
      g_clockBytes = d.Size;
    }
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_VBUnlock(IDirect3DVertexBuffer9* vb) {
  if (vb == g_clockVB && g_clockData) {
    const unsigned char* p = static_cast<const unsigned char*>(g_clockData);
    const UINT n = std::min<UINT>(g_clockBytes / 24, 32);
    unsigned h = 2166136261u;                          // FNV-1a over the UV dwords of the 32 drawn vertices
    for (UINT i = 0; i < n; ++i) {
      unsigned uv[2];
      std::memcpy(uv, p + i * 24 + 16, 8);
      h = (h ^ uv[0]) * 16777619u;
      h = (h ^ uv[1]) * 16777619u;
    }
    if (h != g_clockUv) { g_clockUv = h; ++g_clockVersion; ++g_clockChanges; }
    g_clockData = nullptr;
  }
  return o.VBUnlock(vb);
}

HRESULT STDMETHODCALLTYPE hk_IBLock(IDirect3DIndexBuffer9* ib, UINT off, UINT size, void** data, DWORD flags) {
  const HRESULT hr = o.IBLock(ib, off, size, data, flags);
  if (SUCCEEDED(hr) && !(flags & D3DLOCK_READONLY)) rbrrtx_apiworld_written(ib);
  return hr;
}

// apiworld.cpp built instances from these buffers: hook Lock on their vtables (shared by every buffer of the kind)
// so a later write by the game reaches rbrrtx_apiworld_written.
void watch_buffer_writes(void* vb, void* ib) {
  if (vb) {
    IDirect3DVertexBuffer9Vtbl* v = static_cast<IDirect3DVertexBuffer9*>(vb)->lpVtbl;
    patch(&v->Lock, &hk_VBLock, o.VBLock);
    patch(&v->Unlock, &hk_VBUnlock, o.VBUnlock);
  }
  if (ib) patch(&static_cast<IDirect3DIndexBuffer9*>(ib)->lpVtbl->Lock, &hk_IBLock, o.IBLock);
}

void clock_note(IDirect3DDevice9* dev) {
  IDirect3DBaseTexture9* t = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t);
  if (!t) return;
  t->lpVtbl->Release(t);
  if (rbrrtx_texture_hash(t) != kClockDigitsHash) return;
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT off = 0, stride = 0;
  if (FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &off, &stride)) || !vb) return;
  vb->lpVtbl->Release(vb);
  if (stride != 24 || off != 0) return;
  if (vb != g_clockVB) {
    g_clockVB = vb;
    IDirect3DVertexBuffer9Vtbl* v = vb->lpVtbl;
    patch(&v->Lock, &hk_VBLock, o.VBLock);
    patch(&v->Unlock, &hk_VBUnlock, o.VBUnlock);
    clog(L"start clock digits: buffer %p, each digit change drawn as a new mesh", static_cast<void*>(vb));
  }
  g_clockNow = true;
}

// The clock's draw with our rotated indices; false = draw it as the game does.
bool clock_draw(IDirect3DDevice9* dev, HRESULT& hr) {
  if (!g_clockNow) return false;
  g_clockNow = false;
  if (!g_dip.valid || g_dip.count != 16 || g_dip.startIdx != 0 || g_dip.base != 0 || g_dip.minIdx != 0 || g_dip.numV != 32)
    return false;
  if (g_clockIbVersion != g_clockVersion) {            // digits changed: next index buffer
    g_clockIbVersion = g_clockVersion;
    g_clockSlot = (g_clockSlot + 1) % 3;
  }
  IDirect3DIndexBuffer9*& ib = g_clockIB[g_clockSlot];
  if (!ib) {
    void* p = nullptr;
    if (FAILED(dev->lpVtbl->CreateIndexBuffer(dev, 48 * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib,
                                              nullptr)) || !ib) {
      ib = nullptr;
      return false;
    }
    if (FAILED(ib->lpVtbl->Lock(ib, 0, 0, &p, 0)) || !p) { ib->lpVtbl->Release(ib); ib = nullptr; return false; }
    unsigned short* ix = static_cast<unsigned short*>(p);
    for (unsigned i = 0; i < 8; ++i) {
      // Slot 0: the game's quad order, 1: reversed, 2: shifted by one. Each quad keeps the game's own six indices
      // (A,B,C / C,B,D): rotating triangle corners (19:54 and 20:05 builds) lost one triangle per quad under path
      // tracing, and the A,B,C / A,C,D pattern would make Remix turn the quads into camera-facing billboards
      // (rtx_instance_manager.cpp createBillboards).
      const unsigned q = g_clockSlot == 0 ? i : g_clockSlot == 1 ? 7 - i : (i + 1) % 8;
      const unsigned short b = static_cast<unsigned short>(4 * q);
      const unsigned short quad[6] = { b, static_cast<unsigned short>(b + 1), static_cast<unsigned short>(b + 2),
                                       static_cast<unsigned short>(b + 2), static_cast<unsigned short>(b + 1),
                                       static_cast<unsigned short>(b + 3) };
      for (unsigned short x : quad) *ix++ = x;
    }
    ib->lpVtbl->Unlock(ib);
  }
  IDirect3DIndexBuffer9* gameIB = nullptr;
  dev->lpVtbl->GetIndices(dev, &gameIB);
  dev->lpVtbl->SetIndices(dev, ib);
  hr = o.DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 32, 0, 16);
  dev->lpVtbl->SetIndices(dev, gameIB);
  if (gameIB) gameIB->lpVtbl->Release(gameIB);
  return true;
}

// The first world-space (RL*) draw of the frame defines the camera: its c0 is view*projection. Also called for the
// draws the API world skips (apiworld.cpp), else the frame keeps an old camera and the car is placed against it.
void frame_camera(const D3DMATRIX& wvp) {
  if (g_frameCam) return;
  DMat V, P, vpinv;
  double rec = 0;
  const DMat vp = to_d(wvp);
  if (!decompose_vp(vp, V, P, rec) || !inverse_d(vp, vpinv)) return;
  g_V = to_f(V); g_P = to_f(P); g_VPinv = vpinv; g_VPd = vp;
  g_haveCam = true;
  g_frameCam = true;
  g_st.recErr = static_cast<float>(rec);
  D3DMATRIX vi;                                        // windscreen drops in cockpit views ride on the camera
  if (g_enabled && g_apiOk && g_primaryRT && inverse(g_V, vi) && g_P._22 > 1e-4f) {
    rbrrtx_headlights_camera(&vi._11);                 // cockpit or not: is the camera in the cabin
    rbrrtx_windscreen_camera(&vi._11, 2.0f * std::atan(1.0f / g_P._22) * 57.29578f);
  }
}

bool before_draw(IDirect3DDevice9* dev) {
  ++g_st.draws;
  g_clockNow = false;
  {
    const ShaderInfo* key = !g_curVS ? &kFixedFunction : g_curInfo ? g_curInfo : &kUnknownShader;
    DrawCount& dc = g_byShader[key];
    if (g_primaryRT) ++dc.primary; else ++dc.offscreen;
    if (g_oqActive > 0) ++dc.inQuery;   // Remix rasterizes these instead of ray tracing them (d3d9_rtx.cpp:451)
    log_layout(dev, key);
    log_draw_state(dev, key);
  }
  if (!g_curVS) {                                      // fixed function: give the game its own transforms back
    ++g_st.ff;
    // Screen-space quads (XYZRHW: menus, HUD, flares) ignore transforms in D3D, but Remix calls a draw UI only with an
    // orthographic PROJECTION ([3][3] == 1) + ZWRITE off, and composites at the first UI draw. RBR never sets PROJECTION,
    // so our last perspective matrix stayed set: the ESC menu was rasterised as non-UI and then painted over by the
    // path-traced image at Present (frame dump 2026-10-15 23:15; re/kb/systems/remix-source.md row 14). For these draws
    // force identity: restoring the game's own matrices fixed external cameras only (23:26); in cockpit views UI stayed
    // hidden even with forced identity (23:37); see g_uiTriggerOn for what is tried next.
    const bool screenSpace = screen_space_input(dev);
    if (g_clockOn && !screenSpace && g_primaryRT) clock_note(dev);
    // Backfire: RBR draws the exhaust pop sprite fixed-function (usage.tsv: g_pop.dds, additive, z-write off).
    if (!screenSpace && g_primaryRT && rbrrtx_rain_probe_on()) {   // rain probe: blended FF world draws per texture
      DWORD blend = 0;
      dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
      IDirect3DBaseTexture9* t = nullptr;
      if (blend && SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &t)) && t) {
        t->lpVtbl->Release(t);
        rbrrtx_rain_note_draw(rbrrtx_texture_hash(t), g_pendingPrims, true);
      }
    }
    if (!screenSpace && g_primaryRT) {
      const unsigned long long pop = rbrrtx_headlights_pop_texture();
      IDirect3DBaseTexture9* t = nullptr;
      if (pop && SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &t)) && t) {
        t->lpVtbl->Release(t);
        if (rbrrtx_texture_hash(t) == pop) rbrrtx_headlights_pop_drawn();
      }
    }
    for (int i = 0; i < 3; ++i)
      if (screenSpace) { g_devValid[i] = false; set_xf(dev, i, identity()); }   // uncached: suspect b)
      else if (g_gameSet[i]) set_xf(dev, i, g_game[i]);
    if (screenSpace) ++g_st.screenSpace;
    // Overlay = screen-space, or depth test and depth writes off: PaceNote pastes its HUD (drawn into its own render
    // target) with a perspective projection, which Remix does not take as UI, so in replays without another HUD
    // element it was painted over (2026-10-22 dump: no trigger in that frame). RBR's only depth-off FF draw in the
    // scene is untextured (dump 2026-10-16 #127).
    bool overlay = screenSpace;
    if (!overlay && g_uiTriggerOn && !g_uiTriggered && g_primaryRT) {
      DWORD z = 1, zw = 1;
      dev->lpVtbl->GetRenderState(dev, D3DRS_ZENABLE, &z);
      dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
      overlay = !z && !zw;
    }
    // A plugin drawing its HUD into its own render target after the 3D scene (PaceNote: a 300x450 target, pasted onto
    // the screen later): what it draws before Remix composites does not show (2026-10-22 dumps: its menu showed only
    // when something earlier in the frame triggered, e.g. the pause menu or RSF's frame counter). Trigger first, on
    // the back buffer.
    if (!g_primaryRT && g_uiTriggerOn && !g_uiTriggered && g_enabled && g_st.xformed > 0 && g_backbuffer) {
      IDirect3DBaseTexture9* t0 = nullptr;
      dev->lpVtbl->GetTexture(dev, 0, &t0);
      if (t0) {
        t0->lpVtbl->Release(t0);
        IDirect3DSurface9* rt = nullptr;
        D3DVIEWPORT9 vp = {};
        dev->lpVtbl->GetViewport(dev, &vp);
        if (SUCCEEDED(dev->lpVtbl->GetRenderTarget(dev, 0, &rt)) && rt) {
          g_uiTriggered = true;
          o.SetRenderTarget(dev, 0, static_cast<IDirect3DSurface9*>(g_backbuffer));   // also resets the viewport
          trigger_ui(dev);
          o.SetRenderTarget(dev, 0, rt);
          dev->lpVtbl->SetViewport(dev, &vp);
          rt->lpVtbl->Release(rt);
        }
      }
    }
    if (overlay && g_uiTriggerOn && !g_uiTriggered && g_primaryRT && g_enabled && g_st.xformed > 0) {
      // Only a textured quad counts as the start of the HUD: RBR also draws an untextured additive screen-space quad in
      // the middle of the 3D scene (Zlin chase-cam dump 05:52, #1155 between the car and the water). Triggering there
      // made Remix rasterize every later draw, so the car body lost ray tracing in chase cameras (d3d9_rtx.cpp:573).
      IDirect3DBaseTexture9* t0 = nullptr;
      dev->lpVtbl->GetTexture(dev, 0, &t0);
      const bool textured = t0 != nullptr;
      if (t0) t0->lpVtbl->Release(t0);
      if (textured) { g_uiTriggered = true; trigger_ui(dev); }
    }
    return false;
  }
  if (g_curInfo) ++g_st.known; else ++g_st.unknown;
  if (!g_primaryRT) { ++g_st.offscreen; return false; }
  if (!g_enabled) return false;

  const D3DMATRIX wvp = from_rows(&g_c[0]);
  if (g_skyMode == 2 && is_sky_draw()) {
    // Sky draws (mode 2) get their OWN camera, split from their own c0 (rotation-only view, scale, RotateX; the clouds
    // use another scale/offset). Remix rebuilds each sky vertex as viewToWorld * invProj * clip and renders it into the
    // sky probe from that view's position (rtx_context.cpp rasterizeToSkyProbe). With the main camera's V/P the cloud
    // dome landed as a warped blob off to one side of the probe (capture 22:44 probe). Here V*P equals c0 exactly, so
    // the dome sits around the camera whatever RBR builds. P is divided by its [2][3] (same projective transform).
    DMat V, P;
    double rec = 0;
    if (decompose_vp(to_d(wvp), V, P, rec)) {
      const double s = P.m[11];
      for (double& e : P.m) e /= s;
      set_xf(dev, 0, identity());
      set_xf(dev, 1, to_f(V));
      set_xf(dev, 2, to_f(P));
      g_st.skyErr = (std::max)(g_st.skyErr, static_cast<float>(rec));
      g_st.skyCamOff = (std::max)(g_st.skyCamOff, static_cast<float>(std::sqrt(V.m[12] * V.m[12] + V.m[13] * V.m[13] + V.m[14] * V.m[14])));
      ++g_st.xformed;
      return apply_material_hint(dev);
    }
  }
  if (g_curInfo && (g_curInfo->flags & SH_BTB)) {
    if (!g_btbOn) { ++g_st.btb; ++g_st.btbSkipped; return false; }
    return btb_draw(dev);
  }
  const bool worldSpace = g_curInfo && (g_curInfo->flags & SH_WORLDSPACE);
  if (worldSpace) frame_camera(wvp);
  if (!g_haveCam) { ++g_st.noCamera; return false; }
  D3DMATRIX world = to_f(mul_d(to_d(wvp), g_VPinv));
  if (worldSpace) {                                    // should be identity; how far off is it?
    const float off = max_diff(world, identity());
    g_st.rlDev = (std::max)(g_st.rlDev, off);
    if (off < 1e-3f) world = identity();
  }
  const bool btbFrame = g_btbPrev || g_st.btb > 0;   // the camera is BTB's (other projection): see btb_camera
  if (g_curInfo && (g_curInfo->flags & SH_USES_C8) && all_set(8, 4)) {
    // The shader also gets the exact world matrix in c8-c11: use it when it agrees with the derived one.
    const D3DMATRIX exact = from_rows(&g_c[8]);
    // Checked FORWARD: c8 x VP must put the model's origin and its three unit axes where c0 does on screen (NDC). The
    // old check compared c8 with the derived W = c0 x VP^-1, but that inverse is ill-conditioned (RBR's P._33 ~ 1.0001)
    // and its error grows with the distance from the map origin: 0.2-2 m on Legazpi (~2 km out), 2.9 on Peklo (~3 km),
    // so c8 was refused there and the car (and the lamps on it) got the noisy derived matrix: lamps jumping around
    // the car in the Peklo replay, roaming while driving (runs 20261030-051950, -053456).
    const DMat fwd = mul_d(to_d(exact), g_VPd), c0 = to_d(wvp);
    double scr = 0.0;
    for (int k = 0; k < 4; ++k) {                      // points 0, x, y, z of the model: row 3 (+ row k)
      double a[4], b[4];
      for (int j = 0; j < 4; ++j) {
        a[j] = fwd.m[12 + j] + (k < 3 ? fwd.m[k * 4 + j] : 0.0);
        b[j] = c0.m[12 + j] + (k < 3 ? c0.m[k * 4 + j] : 0.0);
      }
      // Behind the camera or within 1 m of it: compare clip space relative to its size. Near the camera plane x / w
      // magnifies c0's float rounding (bonnet/cockpit views, run 20261101-035724 Osli: 2 car draws a frame refused at
      // 0.008 NDC, their derived W up to 4.35 m off, the body match lost: lamps flipped between body and prediction).
      if (a[3] < 1.0 || b[3] < 1.0) {
        const double m = (std::max)({ 1.0, std::fabs(b[0]), std::fabs(b[1]), std::fabs(b[3]) });
        scr = (std::max)(scr, (std::max)({ std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[3] - b[3]) }) / m);
        continue;
      }
      scr = (std::max)(scr, (std::max)(std::fabs(a[0] / a[3] - b[0] / b[3]), std::fabs(a[1] / a[3] - b[1] / b[3])));
    }
    const float err = static_cast<float>(scr);
    if (!btbFrame) {
      g_st.worldErr = (std::max)(g_st.worldErr, err);
      g_st.worldDerivErr = (std::max)(g_st.worldDerivErr, max_diff(world, exact));
    }
    if (err < 2e-3f || btbFrame) { world = exact; ++g_st.c8Used; }   // 2e-3 NDC: about a pixel at 1080p
    else ++g_st.c8Refused;
    DMat winv;
    if (btbFrame && inverse_d(to_d(exact), winv) && inverse_d(mul_d(winv, to_d(wvp)), g_rbrVPinv)) g_rbrVPvalid = true;
  } else if (btbFrame && g_rbrVPvalid && !worldSpace) {
    world = to_f(mul_d(to_d(wvp), g_rbrVPinv));        // c0 = W * RBR's own view*projection
  }
  const bool oursVP = g_testMode == 0 || g_testMode == 1, oursW = g_testMode == 0 || g_testMode == 2;
  if (oursVP) { set_xf(dev, 1, g_V); set_xf(dev, 2, g_P); } else { set_game_xf(dev, 1); set_game_xf(dev, 2); }
  if (oursW) set_xf(dev, 0, world); else set_game_xf(dev, 0);
  if (g_curInfo && (g_curInfo->flags & SH_CAR)) {      // the car's position: centre of the culling keep radius
    g_carPos[0] = world._41; g_carPos[1] = world._42; g_carPos[2] = world._43;
    std::memcpy(g_carWorld, &world._11, sizeof(g_carWorld));
    g_haveCar = true;
    ++g_carDraws;
    DWORD blend = FALSE;                               // the ghost car is drawn blended (opacity 0.4)
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
    rbrrtx_headlights_car_draw(&world._11, g_pendingPrims, blend != FALSE, (g_curInfo->flags & SH_GLASS) != 0);
  }
  ++g_st.xformed;
  return apply_material_hint(dev);
}

// Restores the game's stage-0 states and lighting after a hinted draw.
void after_draw(IDirect3DDevice9* dev) {
  for (int i = 0; i < 6; ++i)
    if (g_tss0[i] != kTssWanted[i]) o.SetTextureStageState(dev, 0, kTss[i], g_tss0[i]);
  if (g_lighting) o.SetRenderState(dev, D3DRS_LIGHTING, g_lighting);
}

void log_frame(const wchar_t* why) {
  if (wcscmp(why, L"periodic") && wcscmp(why, L"first frames with a camera")) rbrrtx_report_event(why);
  D3DMATRIX vinv = {};
  const bool okInv = g_haveCam && inverse(g_V, vinv);
  const float fovY = g_haveCam && g_P._22 != 0 ? 2.0f * std::atan(1.0f / std::fabs(g_P._22)) * 57.29578f : 0.0f;
  clog(L"frame %u (%s): draws %u [known %u, unknown %u, fixed-function %u, offscreen %u, BTB %u (%u untransformed, %u instanced = %u copies merged, %u rebuilt, %u camera candidates refused, shift (%.1f %.1f %.1f))], transformed %u, no-camera %u, "
       L"scenes %u, world-check (c8 x VP vs c0 on screen) max err %.5f, c8 used %u / refused %u, derived W err %.3f, world-space dev %.5f, VP split err %.6f, comp %s, sky mode %d (%u sky draws, split err %.5f, camera off-centre %.2f), windscreen %s (%u skipped), %u screen-space, "
       L"UI trigger %s at draw %u (occlusion queries open then %u, now %d), %s",
       g_frame, why, g_st.draws, g_st.known, g_st.unknown, g_st.ff, g_st.offscreen, g_st.btb, g_st.btbSkipped, g_st.btbInstanced, g_st.btbCopies, g_st.btbRebuilt, g_st.btbCamRejected, g_btbShift[0], g_btbShift[1], g_btbShift[2], g_st.xformed, g_st.noCamera,
       g_st.scenes, g_st.worldErr, g_st.c8Used, g_st.c8Refused, g_st.worldDerivErr, g_st.rlDev, g_st.recErr, g_enabled ? L"ON" : L"OFF", g_skyMode,
       g_st.skyDraws, g_st.skyErr, g_st.skyCamOff, g_glassOn ? L"on" : L"off", g_st.glassSkipped, g_st.screenSpace, g_uiTriggerOn ? L"on" : L"off",
       g_st.uiTriggerAt, g_st.oqAtUi, g_oqActive, kTestModes[g_testMode]);
  clog(L"  foliage mode %d (cut-out ALPHAREF %lu): %u draws with lowered ALPHAREF, %u soft passes skipped; %u mid-scene screen quads skipped; off-screen scenery %s (%.0f draws skipped per frame) since last line",
       g_foliageMode, g_cutoutRef, g_refLowered, g_softSkipped, g_midQuadSkipped,
       g_skipOffscreen ? L"skipped" : L"drawn",
       g_logFrames ? static_cast<double>(g_offSkipped) / g_logFrames : 0.0);
  clog(L"  fixed-function scenery now mode %d (%s); since last line %.0f draws per frame without their vertex shader, %.0f of them swaying ones beyond %.0f m",
       g_ffMode, g_ffMode == 0 ? L"off, vertex capture" : g_ffMode == 1 ? L"static RL scenery" : L"static RL scenery + far swaying foliage",
       g_logFrames ? static_cast<double>(g_ffDraws) / g_logFrames : 0.0,
       g_logFrames ? static_cast<double>(g_ffSwayFar) / g_logFrames : 0.0, g_ffSwayNear);
  clog(L"  opaque cut-outs %s: %.0f alpha-tested draws per frame drawn without alpha test (texture never below ALPHAREF)",
       g_opaqueAlpha ? L"ON" : L"off", g_logFrames ? static_cast<double>(g_opaqueDraws) / g_logFrames : 0.0);
  clog(L"  car dirt decals %s: %.0f per frame (%u with an untagged render-target dirt map) since last line",
       g_dirtOn ? L"ON" : L"off", g_logFrames ? static_cast<double>(g_dirtDraws) / g_logFrames : 0.0, g_dirtUntagged);
  g_dirtDraws = g_dirtUntagged = 0;
  {
    unsigned held = 0, twins = 0;
    double mb = 0;
    rbrrtx_layer_stats(&held, &twins, &mb);
    clog(L"  ground second layer %s (terrain baker %s): %.0f per frame (%u without twin or UV set, %u untagged); %u sources held, %u twins, %.0f MB",
         g_layerMode == kLayerBake ? L"BAKE" : g_layerMode == kLayerDecal ? L"DECAL" : L"off", g_bakerOn ? L"on" : L"off", g_logFrames ? static_cast<double>(g_layerDraws) / g_logFrames : 0.0,
         g_layerMissing, g_layerUntagged, held, twins, mb);
    g_layerDraws = g_layerMissing = g_layerUntagged = 0;
  }
  if (g_clockChanges) clog(L"  start clock digits: %u changes drawn as new meshes since last line", g_clockChanges);
  g_clockChanges = 0;
  wchar_t acLine[400];
  rbrrtx_alphacut_summary(acLine, 400, g_logFrames);
  clog(L"  alpha-card cut-outs %s: %s", g_alphaCut ? L"ON" : L"off", acLine);
  rbrrtx_radnorm_summary(acLine, 400, g_logFrames);
  clog(L"  radial foliage normals %s", acLine);
  g_refLowered = g_softSkipped = g_midQuadSkipped = g_logFrames = g_offSkipped = g_ffDraws = g_ffSwayFar = 0;
  g_opaqueDraws = 0;
  if (okInv)
    clog(L"  camera pos (%.2f, %.2f, %.2f) vs c50 (%.2f, %.2f, %.2f); camera up (%.3f, %.3f, %.3f); fovY %.1f deg, "
         L"P._11 %.3f P._22 %.3f P._31 %.3f P._32 %.3f P._33 %.4f P._34 %.3f P._43 %.3f; sun c41 (%.3f, %.3f, %.3f)",
         vinv._41, vinv._42, vinv._43, g_c[50][0], g_c[50][1], g_c[50][2], vinv._21, vinv._22, vinv._23, fovY,
         g_P._11, g_P._22, g_P._31, g_P._32, g_P._33, g_P._34, g_P._43, g_c[41][0], g_c[41][1], g_c[41][2]);
  if (g_cset[55] && g_cset[63])                        // scattering inputs: sun colour/intensity for Remix later
    clog(L"  test %s; c55 (%.3f %.3f %.3f %.3f) c56 (%.3f %.3f %.3f) c62 sun colour (%.3f %.3f %.3f %.3f) c63 HG (%.3f %.3f %.3f) c65 (%.3f %.3f)",
         kTestModes[g_testMode], g_c[55][0], g_c[55][1], g_c[55][2], g_c[55][3], g_c[56][0], g_c[56][1], g_c[56][2],
         g_c[62][0], g_c[62][1], g_c[62][2], g_c[62][3], g_c[63][0], g_c[63][1], g_c[63][2], g_c[65][0], g_c[65][1]);
  std::wstring line;                                   // "name primary/offscreen", most draws first
  std::vector<std::pair<const ShaderInfo*, DrawCount>> rows(g_byShader.begin(), g_byShader.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.second.primary + a.second.offscreen > b.second.primary + b.second.offscreen; });
  for (const auto& r : rows) {
    wchar_t part[160];
    if (r.second.inQuery)
      swprintf_s(part, L"%s %u/%u (%u in occlusion query), ", fs::path(r.first->name).filename().c_str(), r.second.primary,
                 r.second.offscreen, r.second.inQuery);
    else
      swprintf_s(part, L"%s %u/%u, ", fs::path(r.first->name).filename().c_str(), r.second.primary, r.second.offscreen);
    line += part;
  }
  clog(L"  draws by shader (primary/off-screen): %s", line.c_str());
  // Once per run: a frame whose draws are almost all "(unknown VS)" means the proxy cannot find the camera, so Remix
  // gets no valid camera and the path-traced image is black (raster via K still fine). Tester 2026-10-19 (v0.2).
  unsigned known = 0, unknown = 0;
  for (const auto& r : rows) (r.first == &kUnknownShader ? unknown : known) += r.second.primary;
  if (!g_unknownWarned && unknown >= 50 && unknown > 4 * known) {
    g_unknownWarned = true;
    clog(L"WARNING: game shaders not recognised (%u of %u draws, %u of %u created shaders unknown). Another mod changes "
         L"RBR's vertex shaders before they reach RBR-RTX (ReShade, a shader pack or a graphics plugin), so path tracing "
         L"has no camera and stays black. Remove it, or check that VertexShaders\\ holds the game's original files.",
         unknown, unknown + known, g_vsUnknown, g_vsKnown + g_vsUnknown);
  }
}

// ---------------------------------------------------------------- performance experiments (key K)
// Where does the frame time go (user 2026-10-17: trees take 85 -> 60 fps)? One press of K runs the steps of
// rbr-rtx\config\experiments.ini (env RBRRTX_EXPERIMENTS) while the player keeps the view still: each step sets Remix
// options live (SetConfigVariable, user layer, applied at frame end) and/or hides draws whose shader file name
// contains a listed part ("skip = Sway" = swaying foliage), waits settle_seconds, then averages the frame time over
// measure_seconds. Baseline first and last (drift check); the log gets one line per step and a summary. K again =
// abort. Afterwards every option is back at its [baseline] value. Options touched here land in the same layer as the
// Alt+X user settings (see remix_set): rbr-remix.ps1 strips them from a saved user.conf after the run.
// Distance culling (defined further down, before comp_draw).
void cull_set(const std::wstring& k, const std::wstring& v);
void cull_load();
void cull_log_settings(const wchar_t* why);
void cull_log_stats();
void cull_frame_tick();
bool cull_toggle();

struct ExpStep {
  std::wstring name;
  std::vector<std::pair<std::string, std::string>> sets;
  std::vector<std::wstring> skip;
  std::vector<std::wstring> solid;                           // alpha test off for these shader name parts
  int ff = -1;                                               // fixed-function scenery mode for this step (-1 = start value)
  int opaque = -1;                                           // opaque cut-outs for this step (-1 = start value)
  int alphacut = -1;                                         // alpha-card cut-outs for this step (-1 = start value)
  bool alphacutSolid = false;                                // clipped copies without alpha test (timing only)
  float ffSwayNear = -2.0f;                                  // mode 2 sway distance for this step (-2 = start value;
                                                             // 0 = every swaying draw fixed-function, none captured)
  int sun = -1;                                              // 0 = Remix sun off for this step (-1 = as it was)
  bool shot = false, dump = false, capture = false;          // sweep: screenshot / draw list / Remix USD capture
  double measure = -1.0;                                     // measure_seconds for this step only (-1 = [run])
  double settle = -1.0;                                      // settle_seconds for this step only (-1 = [run])
  bool ref = false;                                          // perf reference: later steps are ranked against it
  std::vector<std::pair<bool*, bool>> proxy;                 // proxy.<switch> = 0/1 (g_expSwitches)
};
// Proxy switches a sweep step can set (proxy.<name> = 0/1); all go back to their run-start values after each step.
bool g_litOn = true;                                         // lit car parts twin textures (litparts.cpp)
bool g_sweepPrecip = false;                                  // emit Remix rain/drops while a sweep runs (normally not)
extern bool g_dirtOn, g_glassOn;
const std::pair<const wchar_t*, bool*> g_expSwitches[] = {
  {L"dirt", &g_dirtOn}, {L"glass", &g_glassOn}, {L"litparts", &g_litOn}, {L"precip", &g_sweepPrecip},
};
bool g_expSwitchSaved[std::size(g_expSwitches)] = {};
struct PerfRow { std::wstring name; double ms, ref; };
std::vector<PerfRow> g_perfRows;                             // steps after a ref step, ranked in summary.txt
double g_perfRef = 0.0;
std::vector<ExpStep> g_exp;                                  // [0] = baseline (no changes), then the ini steps
std::vector<std::pair<std::string, std::string>> g_expBase;  // [baseline] option values, restored around every step
std::vector<std::string> g_expDirty;                         // options the running step changed (restored next)
bool g_expApplyBase = true;                                  // [run] apply_baseline: send all of [baseline] at the start
double g_expSettle = 3.0, g_expMeasure = 5.0;
// Stage sweep (Alt+K, rbr-rtx\config\sweep.ini): the same runner, plus per step a screenshot, a draw list with
// texture hashes, Remix debug views and a Remix USD capture; output in <logs>\sweep-<time>\ (see sweep_begin).
bool g_sweep = false, g_sweepShots = false;
std::wstring g_sweepDir;
FILE* g_sweepTsv = nullptr;                                  // draws.tsv while a dump frame runs
unsigned g_sweepDumpFrames = 0, g_sweepDumpN = 0;
std::vector<std::wstring> g_sweepLines;                      // summary.txt, one line per step
std::vector<std::wstring> g_captureBefore;                   // capture files before the capture step
unsigned g_captureKeyUp = 0;                                 // frame to send the capture key's key-up
bool g_sunOnBefore = true;
HWND g_wnd = nullptr;                                        // the game's device window (screenshots, capture key)
int g_expStep = -1;                                          // running step, -1 = idle
size_t g_expPos = 0;                                         // index in g_expOrder
std::vector<int> g_expOrder;                                 // baseline, steps..., baseline
std::vector<double> g_expMs;                                 // result per g_expOrder entry
LARGE_INTEGER g_expStepStart = {}, g_expMeasureStart = {}, g_qpf = {};
unsigned g_expFrames = 0, g_expSkipped = 0, g_expSolid = 0;
bool g_expMeasuring = false;
std::unordered_map<const ShaderInfo*, bool> g_expSkipCache, g_expSolidCache;

std::string narrow(const std::wstring& w) {                  // option names/values are ASCII
  std::string s;
  for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
  return s;
}

void exp_load(bool sweep) {
  g_exp.clear();
  g_expBase.clear();
  g_expApplyBase = true;
  g_sweepShots = false;
  wchar_t path[MAX_PATH * 2] = {};
  if (sweep) rbrrtx_config_path(L"RBRRTX_SWEEP", L"sweep.ini", path, MAX_PATH * 2);
  else rbrrtx_config_path(L"RBRRTX_EXPERIMENTS", L"experiments.ini", path, MAX_PATH * 2);
  g_exp.push_back({L"baseline", {}, {}});
  wchar_t buf[8192];
  if (GetPrivateProfileSectionNamesW(buf, 8192, path) == 0) { clog(L"experiments: no %s", path); return; }
  for (const wchar_t* sec = buf; *sec; sec += wcslen(sec) + 1) {
    wchar_t body[8192];
    GetPrivateProfileSectionW(sec, body, 8192, path);
    ExpStep step;
    step.name = sec;
    for (const wchar_t* line = body; *line; line += wcslen(line) + 1) {
      std::wstring l = line;
      const size_t eq = l.find(L'=');
      if (l.empty() || l[0] == L';' || eq == std::wstring::npos) continue;
      auto trim = [](std::wstring s) {
        const size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
        return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
      };
      const std::wstring k = trim(l.substr(0, eq));
      std::wstring v = trim(l.substr(eq + 1));
      if (!v.empty() && v[0] == L'@') {                       // value from a file next to the ini (long hash lists)
        std::wstring file = path;
        file = file.substr(0, file.find_last_of(L'\\') + 1) + v.substr(1);
        std::wifstream in(file);
        std::wstring content((std::istreambuf_iterator<wchar_t>(in)), std::istreambuf_iterator<wchar_t>());
        if (content.empty()) clog(L"experiments: %s = %s: file not found or empty", k.c_str(), v.c_str());
        v = trim(content);
        while (!v.empty() && (v.back() == L'\n' || v.back() == L'\r')) v.pop_back();
      }
      if (!_wcsicmp(sec, L"run")) {
        if (k == L"settle_seconds") g_expSettle = (std::max)(0.2, _wtof(v.c_str()));
        if (k == L"measure_seconds") g_expMeasure = (std::max)(0.1, _wtof(v.c_str()));
        if (k == L"apply_baseline") g_expApplyBase = _wtoi(v.c_str()) != 0;
        if (k == L"shots") g_sweepShots = _wtoi(v.c_str()) != 0;
      } else if (!_wcsicmp(sec, L"baseline")) {
        g_expBase.emplace_back(narrow(k), narrow(v));
      } else if (k == L"name") {
        step.name = v;
      } else if (k == L"ff") {
        step.ff = std::clamp(_wtoi(v.c_str()), 0, 2);
      } else if (k == L"opaque") {
        step.opaque = _wtoi(v.c_str()) != 0 ? 1 : 0;
      } else if (k == L"alphacut") {
        step.alphacut = _wtoi(v.c_str()) != 0 ? 1 : 0;
      } else if (k == L"alphacut_solid") {
        step.alphacutSolid = _wtoi(v.c_str()) != 0;
      } else if (k == L"ff_sway_near") {
        // <= 0: -1 m, so a batch whose bounds hold the camera (distance 0) goes fixed-function too
        step.ffSwayNear = _wtof(v.c_str()) > 0 ? static_cast<float>(_wtof(v.c_str())) : -1.0f;
      } else if (k == L"measure_seconds") {
        step.measure = (std::max)(0.1, _wtof(v.c_str()));
      } else if (k == L"settle_seconds") {
        step.settle = (std::max)(0.2, _wtof(v.c_str()));
      } else if (k == L"ref") {
        step.ref = _wtoi(v.c_str()) != 0;
      } else if (k.rfind(L"proxy.", 0) == 0) {
        bool found = false;
        for (const auto& sw : g_expSwitches)
          if (k.substr(6) == sw.first) { step.proxy.emplace_back(sw.second, _wtoi(v.c_str()) != 0); found = true; }
        if (!found) clog(L"experiments: [%s] %s: unknown proxy switch", sec, k.c_str());
      } else if (k == L"sun") {
        step.sun = _wtoi(v.c_str()) != 0 ? 1 : 0;
      } else if (k == L"shot" || k == L"dump" || k == L"capture") {
        (k == L"shot" ? step.shot : k == L"dump" ? step.dump : step.capture) = _wtoi(v.c_str()) != 0;
      } else if (k == L"skip" || k == L"solid") {
        auto& list = k == L"skip" ? step.skip : step.solid;
        for (size_t a = 0; a <= v.size();) {
          size_t b = v.find(L',', a);
          if (b == std::wstring::npos) b = v.size();
          const std::wstring part = trim(v.substr(a, b - a));
          if (!part.empty()) list.push_back(part);
          a = b + 1;
        }
      } else if (k.rfind(L"rtx.", 0) == 0 || k.rfind(L"cull.", 0) == 0) {
        step.sets.emplace_back(narrow(k), narrow(v));
      }
    }
    if (_wcsicmp(sec, L"run") && _wcsicmp(sec, L"baseline")) g_exp.push_back(step);
  }
  clog(L"experiments: %zu steps from %s (settle %.1f s, measure %.1f s, %zu baseline options)", g_exp.size() - 1, path,
       g_expSettle, g_expMeasure, g_expBase.size());
}

void exp_set(const std::string& k, const std::string& v) {
  if (k.rfind("cull.", 0) == 0) {                          // proxy culling setting, e.g. cull.offscreen_far_m = 100
    cull_set(std::wstring(k.begin() + 5, k.end()), std::wstring(v.begin(), v.end()));
    return;
  }
  remix_set(k.c_str(), v.c_str());
}

// Every step starts from the baseline: the options the previous step changed go back to their [baseline] values (all
// of [baseline] is sent once when the run starts, unless apply_baseline = 0), then the step's own are set. Fewer
// Remix API calls than re-sending the whole baseline per step (it held a 5855-hash list, 2026-10-17).
void exp_apply(int step) {
  cull_load();                                               // culling back to culling.ini, then the step's cull.* lines
  const std::vector<std::pair<std::string, std::string>> none;
  const auto& next = step > 0 ? g_exp[step].sets : none;
  for (const auto& k : g_expDirty) {
    if (std::any_of(next.begin(), next.end(), [&](const auto& s) { return s.first == k; })) continue;
    for (const auto& b : g_expBase) if (b.first == k) exp_set(b.first, b.second);
  }
  g_expDirty.clear();
  g_expSkipCache.clear();
  g_expSolidCache.clear();
  g_expStep = step;
  for (size_t i = 0; i < std::size(g_expSwitches); ++i) *g_expSwitches[i].second = g_expSwitchSaved[i];
  if (step > 0) for (const auto& sw : g_exp[step].proxy) *sw.first = sw.second;
  g_ffMode = step > 0 && g_exp[step].ff >= 0 ? g_exp[step].ff : g_ffBase;
  g_opaqueAlpha = step > 0 && g_exp[step].opaque >= 0 ? g_exp[step].opaque : g_opaqueBase;
  g_alphaCut = step > 0 && g_exp[step].alphacut >= 0 ? g_exp[step].alphacut : g_alphaCutBase;
  g_alphaCutSolid = step > 0 && g_exp[step].alphacutSolid;
  g_ffSwayNear = step > 0 && g_exp[step].ffSwayNear > -2.0f ? g_exp[step].ffSwayNear : g_ffSwayNearBase;
  const bool sunWanted = step > 0 && g_exp[step].sun >= 0 ? g_exp[step].sun != 0 : g_sunOnBefore;
  if (sunWanted != g_sunOn) {
    g_sunOn = sunWanted;
    if (!g_sunOn) destroy_sun(L"experiment step");
  }
  if (step < 0) return;
  for (const auto& kv : g_exp[step].sets) { exp_set(kv.first, kv.second); g_expDirty.push_back(kv.first); }
  QueryPerformanceCounter(&g_expStepStart);
  g_expMeasuring = false;
  g_expFrames = g_expSkipped = g_expSolid = 0;
}

bool exp_match(const std::vector<std::wstring>& parts, std::unordered_map<const ShaderInfo*, bool>& cache) {
  if (g_expStep <= 0 || !g_curInfo || parts.empty()) return false;
  auto it = cache.find(g_curInfo);
  if (it != cache.end()) return it->second;
  bool hit = false;
  for (const auto& part : parts) if (g_curInfo->name.find(part) != std::wstring::npos) { hit = true; break; }
  cache[g_curInfo] = hit;
  return hit;
}

bool exp_skip_draw() { return g_expStep > 0 && exp_match(g_exp[g_expStep].skip, g_expSkipCache); }
bool exp_solid_draw() { return g_expStep > 0 && exp_match(g_exp[g_expStep].solid, g_expSolidCache); }

void atmo_reset();
void sweep_end(const wchar_t* why);

void exp_finish(const wchar_t* why) {
  exp_apply(-1);
  atmo_reset();                                              // stage atmosphere is sent again on the next check
  if (g_sweep) sweep_end(why);
  else MessageBeep(MB_ICONASTERISK);                         // done signal, same as the sweep's (sweep_end beeps)
  g_sweep = false;
  if (g_expMs.empty()) { clog(L"experiments: %s", why); return; }
  const double base = g_expMs[0];
  clog(L"experiments: %s. Summary (frame time; minus = faster than the first baseline):", why);
  for (size_t i = 0; i < g_expMs.size(); ++i) {
    const ExpStep& s = g_exp[g_expOrder[i]];
    clog(L"  %-44s %6.2f ms %6.1f fps  %+6.2f ms", s.name.c_str(), g_expMs[i], 1000.0 / g_expMs[i], g_expMs[i] - base);
  }
}

void sweep_begin();
void sweep_dump_start();
void sweep_dump_end();
void post_key(UINT msg, WPARAM vk);
std::wstring capture_dir();
std::vector<std::wstring> list_captures();

void exp_frame() {
  if (g_captureKeyUp && g_frame >= g_captureKeyUp) { post_key(WM_KEYUP, VK_PAUSE); g_captureKeyUp = 0; }
  const bool kExp = rbrrtx_key_pressed(RBRKEY_EXPERIMENT), kSweep = rbrrtx_key_pressed(RBRKEY_SWEEP);
  if (kExp || kSweep) {
    if (g_expStep >= 0) { exp_finish(L"aborted (key)"); return; }
    g_sweep = kSweep;
    exp_load(g_sweep);
    // A Remix option a step changes but [baseline] does not name would stay changed after the run (2026-10-17: a
    // red-glow run left NRC off, emissives 0, atmosphere and bloom off until restart). Refuse such a file.
    std::string missing;
    for (const auto& st : g_exp)
      for (const auto& kv : st.sets)
        if (kv.first.rfind("rtx.", 0) == 0 && missing.find(kv.first) == std::string::npos &&
            std::none_of(g_expBase.begin(), g_expBase.end(), [&](const auto& b) { return b.first == kv.first; }))
          missing += (missing.empty() ? "" : ", ") + kv.first;
    if (!missing.empty()) {
      clog(L"experiments: NOT started: add these to [baseline] with their normal values: %S", missing.c_str());
      g_sweep = false;
      return;
    }
    if (!g_apiOk) clog(L"experiments: Remix API not available, option steps will do nothing (proxy skips still work)");
    g_expOrder.clear();
    g_expMs.clear();
    for (size_t i = 0; i < g_exp.size(); ++i) g_expOrder.push_back(static_cast<int>(i));
    g_expOrder.push_back(0);                                  // baseline again: did the scene drift?
    QueryPerformanceFrequency(&g_qpf);
    g_expPos = 0;
    g_sunOnBefore = g_sunOn;
    for (size_t i = 0; i < std::size(g_expSwitches); ++i) g_expSwitchSaved[i] = *g_expSwitches[i].second;
    g_perfRows.clear();
    g_perfRef = 0.0;
    g_expDirty.clear();
    if (g_expApplyBase) for (const auto& kv : g_expBase) exp_set(kv.first, kv.second);
    if (g_sweep) sweep_begin();
    wchar_t why[200];
    swprintf_s(why, L"%s: %s started (%zu steps, about %.0f s; keep the view still)", rbrrtx_key_name(g_sweep ? RBRKEY_SWEEP : RBRKEY_EXPERIMENT),
               g_sweep ? L"stage sweep" : L"experiment run", g_expOrder.size(), g_expOrder.size() * (g_expSettle + g_expMeasure));
    log_frame(why);
    exp_apply(g_expOrder[0]);
    return;
  }
  if (g_expStep < 0) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double inStep = static_cast<double>(now.QuadPart - g_expStepStart.QuadPart) / g_qpf.QuadPart;
  const ExpStep& s = g_exp[g_expStep];
  if (!g_expMeasuring) {
    if (inStep >= (s.settle > 0 ? s.settle : g_expSettle)) {
      g_expMeasuring = true; g_expMeasureStart = now; g_expFrames = 0; g_expSkipped = 0;
      if (g_sweep && s.dump) sweep_dump_start();
      if (g_sweep && s.capture) {                            // Remix USD capture: hotkey Pause for this step (sweep.ini sets it)
        g_captureBefore = list_captures();
        post_key(WM_KEYDOWN, VK_PAUSE);
        g_captureKeyUp = g_frame + 5;
        clog(L"sweep: capture key sent (captures go to %s)", capture_dir().c_str());
      }
    }
    return;
  }
  ++g_expFrames;
  const double measured = static_cast<double>(now.QuadPart - g_expMeasureStart.QuadPart) / g_qpf.QuadPart;
  if (measured < (s.measure > 0 ? s.measure : g_expMeasure) || g_expFrames < 10) return;
  const double ms = measured * 1000.0 / g_expFrames;
  g_expMs.push_back(ms);
  if (s.ref) g_perfRef = ms;
  else if (g_perfRef > 0 && g_expStep > 0) g_perfRows.push_back({s.name, ms, g_perfRef});
  clog(L"experiment %zu/%zu %s: %.2f ms (%.1f fps) over %u frames, %.1f draws hidden, %.1f made solid per frame", g_expPos + 1,
       g_expOrder.size(), s.name.c_str(), ms, 1000.0 / ms, g_expFrames, static_cast<double>(g_expSkipped) / g_expFrames,
       static_cast<double>(g_expSolid) / g_expFrames);
  if (g_sweep) {
    std::wstring shotName = L"-";
    double luma = -1.0;
    if (g_sweepShots || s.shot) {
      wchar_t file[160];
      std::wstring clean = s.name;
      for (wchar_t& c : clean) if (!iswalnum(c)) c = L'-';
      swprintf_s(file, L"%02zu-%s.png", g_expPos + 1, clean.substr(0, 60).c_str());
      shotName = rbrrtx_screenshot(g_wnd, (g_sweepDir + L"\\" + file).c_str(), &luma) ? file : L"(failed)";
      if (luma >= 0 && luma < 1.0) shotName += L" (black: exclusive full screen? use windowed/borderless)";
    }
    if (s.capture) {
      const auto after = list_captures();
      std::wstring added;
      for (const auto& f : after)
        if (std::find(g_captureBefore.begin(), g_captureBefore.end(), f) == g_captureBefore.end()) added += (added.empty() ? L"" : L", ") + f;
      clog(L"sweep: USD capture %s", added.empty() ? L"NOT found (Remix may still be writing it; check the captures folder)" : added.c_str());
      shotName += L"; capture " + (added.empty() ? std::wstring(L"not found yet in ") + capture_dir() : added);
    }
    wchar_t line[600];
    swprintf_s(line, L"%02zu %s\t%.2f\t%.1f\t%.1f\t%s\t%.1f", g_expPos + 1, s.name.c_str(), ms, 1000.0 / ms,
               static_cast<double>(g_expSkipped) / g_expFrames, shotName.c_str(), luma);
    g_sweepLines.push_back(line);
  }
  if (++g_expPos >= g_expOrder.size()) { exp_finish(L"done, options back at baseline"); return; }
  exp_apply(g_expOrder[g_expPos]);
}

void end_frame(IDirect3DDevice9* dev) {
  if (g_enabled && g_apiOk) rbrrtx_headlights_refine();   // no UI trigger this frame: lamps before Present
  if (g_apiOk) {                                            // generated mod: lit car parts, foliage (Remix reloads it)
    rbrrtx_litparts_frame(rbrrtx_car_model_path(), rbrrtx_headlights_game_lamps());
    rbrrtx_foliage_frame(g_inMenu);
    rbrrtx_genmod_tick();
  }
  if (g_sweepDumpFrames && --g_sweepDumpFrames == 0) sweep_dump_end();
  atmo_frame();
  exp_frame();
  cull_frame_tick();
  if (rbrrtx_key_pressed(RBRKEY_CULLING)) {
    wchar_t w[64];
    swprintf_s(w, L"%s: culling %s", rbrrtx_key_name(RBRKEY_CULLING), cull_toggle() ? L"ON" : L"off");
    log_frame(w);
    cull_log_settings(L"key");
  }
  if (rbrrtx_key_pressed(RBRKEY_RAYTRACING)) {             // raster comparison: Remix draws the game as plain DXVK
    // Raster draws need the game's own draws back: FF scenery (road, terrain) and the comp layer's matrices are made for
    // Remix's ray tracing and rasterised as missing/flat parts (user screenshot 2026-10-17). Culling too.
    static bool rtOn = true, compWas = true, cullWas = false;
    rtOn = !rtOn;
    remix_set("rtx.enableRaytracing", rtOn ? "True" : "False");
    if (!rtOn) {
      compWas = g_enabled;
      if (g_enabled) { g_enabled = false; for (int i = 0; i < 3; ++i) set_game_xf(dev, i); destroy_sun(L"raster"); }
      g_ffMode = 0;
      cullWas = !cull_toggle();                              // toggled: on -> off, done
      if (!cullWas) cull_toggle();                           // it was off: back to off
    } else {
      g_enabled = compWas;
      g_ffMode = g_ffBase;
      if (cullWas) cull_toggle();
    }
    wchar_t w[80];
    swprintf_s(w, L"%s: ray tracing %s", rbrrtx_key_name(RBRKEY_RAYTRACING), rtOn ? L"ON" : L"off (raster)");
    log_frame(w);
  }
  // Hotkeys (keys.cpp, rbr-rtx\config\keys.ini; defaults P O L J U I N H [ ] ; '). Until 2026-10-17 they were
  // Scroll Lock / Pause with Ctrl/Alt/Shift, which compact keyboards do not have. Every key is read every frame.
  const bool kDump = rbrrtx_key_pressed(RBRKEY_DUMP), kLayer = rbrrtx_key_pressed(RBRKEY_LAYER),
             kSun = rbrrtx_key_pressed(RBRKEY_SUN), kFoliage = rbrrtx_key_pressed(RBRKEY_FOLIAGE),
             kDust = rbrrtx_key_pressed(RBRKEY_DUST), kGlass = rbrrtx_key_pressed(RBRKEY_WINDSCREEN),
             kSky = rbrrtx_key_pressed(RBRKEY_SKY), kDebugSun = rbrrtx_key_pressed(RBRKEY_DEBUG_SUN);
  const bool kBrighter = rbrrtx_key_pressed(RBRKEY_SUN_BRIGHTER), kDimmer = rbrrtx_key_pressed(RBRKEY_SUN_DIMMER);
  const bool kFf = rbrrtx_key_pressed(RBRKEY_FF_SCENERY), kOpaque = rbrrtx_key_pressed(RBRKEY_OPAQUE_ALPHA);
  const bool kAlphaCut = rbrrtx_key_pressed(RBRKEY_ALPHACUT);
  const bool kRadNorm = rbrrtx_key_pressed(RBRKEY_FOLIAGE_NORMALS);
  wchar_t why[256];
  if (g_dumpFrames && --g_dumpFrames == 0) clog(L"frame dump end: %u draws", g_dumpDraws);
  if (kDump) {                                         // dump every draw of the next frame (dump_draw)
    g_dumpFrames = 1;                                  // the whole next frame, ended at its end_frame
    g_dumpDraws = 0;
    g_dumpTexSaved.clear();
    swprintf_s(why, L"%s: frame dump of the next frame", rbrrtx_key_name(RBRKEY_DUMP));
    log_frame(why);
  }
  if (kFoliage) {                                      // foliage mode 1 -> 2 -> 0
    g_foliageMode = g_foliageMode == 1 ? 2 : g_foliageMode == 2 ? 0 : 1;
    swprintf_s(why, L"%s: %s", rbrrtx_key_name(RBRKEY_FOLIAGE), g_foliageMode == 1 ? L"foliage mode 1 (low ALPHAREF, soft pass skipped)" : g_foliageMode == 2 ? L"foliage mode 2 (low ALPHAREF, soft pass kept)" : L"foliage mode 0 (game values)");
    log_frame(why);
  }
  if (kFf) {                                           // fixed-function scenery 0 -> 1 -> 2 -> 0
    g_ffMode = g_ffBase = (g_ffMode + 1) % 3;           // also what K steps return to
    swprintf_s(why, L"%s: fixed-function scenery mode %d", rbrrtx_key_name(RBRKEY_FF_SCENERY), g_ffMode);
    log_frame(why);
  }
  if (kOpaque) {
    g_opaqueAlpha = g_opaqueBase = !g_opaqueAlpha;      // also what K steps return to
    swprintf_s(why, L"%s: opaque cut-outs %s", rbrrtx_key_name(RBRKEY_OPAQUE_ALPHA), g_opaqueAlpha ? L"ON (no alpha test)" : L"off (game's alpha test)");
    log_frame(why);
  }
  if (kAlphaCut) {
    g_alphaCut = g_alphaCutBase = !g_alphaCut;          // also what K steps return to
    swprintf_s(why, L"%s: alpha-card cut-outs %s", rbrrtx_key_name(RBRKEY_ALPHACUT), g_alphaCut ? L"ON (clipped cards)" : L"off (game's cards)");
    log_frame(why);
  }
  if (kRadNorm) {
    const bool on = rbrrtx_radnorm_toggle();
    swprintf_s(why, L"%s: radial foliage normals %s", rbrrtx_key_name(RBRKEY_FOLIAGE_NORMALS), on ? L"ON" : L"off (flat cards)");
    log_frame(why);
  }
  if (kSun) {                                          // only the Remix sun on/off (camera and sky stay)
    g_sunOn = !g_sunOn;
    if (!g_sunOn) destroy_sun(L"sun key off");
    swprintf_s(why, L"%s: sun %s", rbrrtx_key_name(RBRKEY_SUN), g_sunOn ? L"ON" : L"OFF");
    log_frame(why);
  }
  if (kLayer) {                                        // the whole comp layer on/off
    g_enabled = !g_enabled;
    if (!g_enabled) {                                  // hand Remix the game's own matrices, as without the layer
      for (int i = 0; i < 3; ++i) set_game_xf(dev, i);
      destroy_sun(L"comp off");
    }
    swprintf_s(why, L"%s: comp %s", rbrrtx_key_name(RBRKEY_LAYER), g_enabled ? L"ON" : L"OFF");
    log_frame(why);
  }
  if (kGlass) {                                        // windscreen on/off
    g_glassOn = !g_glassOn;
    swprintf_s(why, L"%s: windscreen %s", rbrrtx_key_name(RBRKEY_WINDSCREEN), g_glassOn ? L"ON" : L"OFF");
    log_frame(why);
  }
  if (rbrrtx_key_pressed(RBRKEY_RAIN)) rbrrtx_rain_toggle();          // rain/snow: Remix <-> the game's own
  if (kDust && rbrrtx_particles_active()) rbrrtx_particles_toggle();   // Remix API dust on/off
  // Ray interaction budgets: rtx.conf (secondary 2, primary/PSR 16) -> large (8, 64) -> secondary 1 (user 2026-10-24).
  // Large = the test for white wedges in dense forest (2026-11-01): the last allowed interaction skips every remaining
  // cut-out card, so a ray through many stacked cards lands on sky.
  if (rbrrtx_key_pressed(RBRKEY_BOUNCES)) {
    static int state = 0;
    state = (state + 1) % 3;
    static const char* sec[3] = { "2", "8", "1" };
    static const char* prim[3] = { "16", "64", "16" };
    remix_set("rtx.secondaryRayMaxInteractions", sec[state]);
    remix_set("rtx.primaryRayMaxInteractions", prim[state]);
    remix_set("rtx.psrRayMaxInteractions", prim[state]);
    swprintf_s(why, L"%s: ray interactions secondary %hs, primary/PSR %hs%s", rbrrtx_key_name(RBRKEY_BOUNCES), sec[state],
               prim[state], state == 0 ? L" (rtx.conf)" : state == 1 ? L" (large: dense-forest test)" : L"");
    log_frame(why);
  }
  if (kSky) {                                          // sky mode 2 <-> 1
    g_skyMode = g_skyMode == 2 ? 1 : 2;
    swprintf_s(why, L"%s: %s", rbrrtx_key_name(RBRKEY_SKY), g_skyMode == 2 ? L"sky mode 2 (Remix sky)" : L"sky mode 1 (skip)");
    log_frame(why);
  }
  ++g_logFrames;
  if (kBrighter || kDimmer) {                          // sun brightness live (sun_brighter / sun_dimmer), game or debug sun
    g_sunScale = std::clamp(g_sunScale * (kBrighter ? 1.1f : 1.0f / 1.1f), 0.05f, 20.0f);
    g_sunFade = -1.0f;                                 // forces the light to be re-created with the new radiance
    swprintf_s(why, L"%s: sun brightness x%.2f = radiance (%.2f, %.2f, %.2f); keep it: -SunRadiance %.2f,%.2f,%.2f",
               rbrrtx_key_name(kBrighter ? RBRKEY_SUN_BRIGHTER : RBRKEY_SUN_DIMMER), g_sunScale,
               g_sunRadiance[0] * g_sunScale, g_sunRadiance[1] * g_sunScale, g_sunRadiance[2] * g_sunScale,
               g_sunRadiance[0] * g_sunScale, g_sunRadiance[1] * g_sunScale, g_sunRadiance[2] * g_sunScale);
    log_frame(why);
  }
  static SolarPath dbgPath = { 0.785f, 0.0f };
  static ULONGLONG dbgLast = 0;
  if (kDebugSun) {                                     // manual debug sun on/off (g_sunManual)
    g_sunManual = !g_sunManual;
    if (g_sunManual) {                                 // start from the sun on screen
      float el = g_sunShownEl, naz = 0.0f;             // naz: NUMOS rotation (compass-like, 0 = north)
      if (g_numos && g_numosSentEl > -999.0f) { el = g_numosSentEl; naz = g_numosSentAz; }
      else if (g_sunDir[0] != 0.0f || g_sunDir[1] != 0.0f || g_sunDir[2] != 0.0f) {
        el = std::asin(std::clamp(-g_sunDir[1], -1.0f, 1.0f)) * 57.2958f;
        naz = std::atan2(-g_sunDir[0], -g_sunDir[2]) * 57.2958f * g_numosAzSign + g_numosAzOffset;
      }
      dbgPath = solar_path();
      // A sun higher than the conf's path reaches at noon: the path's latitude lowered until it does.
      if (el > 90.0f - std::fabs(dbgPath.lat - dbgPath.dec) * 57.2958f)
        dbgPath.lat = dbgPath.dec + (90.0f - (std::min)(el, 89.9f)) * 0.0174533f;
      // Morning stages run the clock from the morning side, every other one (and the menu) from the afternoon side.
      g_dbgHour = solar_hour(dbgPath, el, g_inMenu || g_stageSentTime != 0);
      g_dbgNorth = std::remainder(naz - solar_azimuth(dbgPath, g_dbgHour), 360.0f);
      dbgLast = GetTickCount64();
      g_sunMoving = true;                              // sets g_sunAz / g_sunEl below and logs the start
    }
    swprintf_s(why, L"%s: %s sun", rbrrtx_key_name(RBRKEY_DEBUG_SUN), g_sunManual ? L"debug (time of day / north heading)" : L"game");
    log_frame(why);
  }
  if (g_sunManual) {
    const ULONGLONG now = GetTickCount64();
    const float dt = (std::min)(static_cast<float>(now - dbgLast) * 0.001f, 0.1f);
    dbgLast = now;
    const float dH = ((rbrrtx_key_held(RBRKEY_SUN_UP) ? 1.0f : 0.0f) - (rbrrtx_key_held(RBRKEY_SUN_DOWN) ? 1.0f : 0.0f)) * dt;
    const float dN = ((rbrrtx_key_held(RBRKEY_SUN_RIGHT) ? 30.0f : 0.0f) - (rbrrtx_key_held(RBRKEY_SUN_LEFT) ? 30.0f : 0.0f)) * dt;
    const bool moving = dH != 0.0f || dN != 0.0f;
    if (moving || g_sunMoving) {
      g_dbgHour = std::fmod(g_dbgHour + dH + 24.0f, 24.0f);
      g_dbgNorth = std::remainder(g_dbgNorth + dN, 360.0f);
      const float el = std::clamp(solar_elevation(dbgPath, g_dbgHour), -18.0f, 90.0f);
      // NUMOS rotation -> the proxy's direction (numos_sun's mapping backwards), then its azimuth = atan2(z, x).
      const float a = (solar_azimuth(dbgPath, g_dbgHour) + g_dbgNorth - g_numosAzOffset) * g_numosAzSign * 0.0174533f;
      g_sunEl = el;
      g_sunAz = std::atan2(-std::cos(a), -std::sin(a)) * 57.2958f;
    }
    if (!moving && g_sunMoving) {                      // log where the sun was left
      swprintf_s(why, L"debug sun at %02d:%02d, north heading %+.1f: elevation %.1f%s", static_cast<int>(g_dbgHour),
                 static_cast<int>(std::fmod(g_dbgHour, 1.0f) * 60.0f), g_dbgNorth, g_sunEl,
                 g_sunEl <= -4.0f ? L" (night: no sun light)" : g_sunEl < 2.0f ? L" (twilight)" : L"");
      log_frame(why);
    }
    g_sunMoving = moving;
  }
  if (rbrrtx_key_pressed(RBRKEY_SUN_SAVE)) sun_save();
  lamp_mark_frame();                                   // lamp_mark key: finishes the armed pick, arms the next
  rbrrtx_headlights_frame_end(g_sunShownEl, g_enabled && g_apiOk);   // headlights key, automatic switch, car model
  {
    D3DMATRIX vinv;
    const float cam[3] = { 0, 0, 0 };
    if (g_frameCam && inverse(g_V, vinv)) {
      const float c[3] = { vinv._41, vinv._42, vinv._43 };
      rbrrtx_world_lights_frame(rbrrtx_sun_shown_elevation(), c, g_enabled && g_apiOk);
    } else {
      rbrrtx_world_lights_frame(rbrrtx_sun_shown_elevation(), cam, g_enabled && g_apiOk);
    }
  }
  numos_ground();                                      // NUMOS: ground level from the car (no-op without NUMOS)
  if (g_numos && g_apiOk) {
    // NUMOS's lamp glow in fog lives in a froxel grid whose history is reprojected with the camera only (aerial-
    // perspective-v2.md section 2, 32 frames): a lamp that moves with the car leaves a trail that catches up when the car
    // stops (user 2026-10-29). While any car lamp is lit the history is shortened (headlights.ini numos_history_frames).
    static float histBase = -1.0f;
    static int histSent = -1;
    if (histBase < 0.0f) histBase = conf_float("rtx.atmosphere.aerialPerspectiveHistoryFrames", 32.0f);
    const int lit = rbrrtx_headlights_history_frames();   // 0 = lamps off / not asked
    // Only ever shorter than the user's own setting (2026-10-30: conf 6, lamps lit made it 32 = a longer glow trail).
    const int want = lit > 0 ? (std::min)(lit, static_cast<int>(histBase)) : static_cast<int>(histBase);
    if (want != histSent) {
      if (histSent >= 0 || lit > 0) {
        remix_set("rtx.atmosphere.aerialPerspectiveHistoryFrames", std::to_string(want).c_str());
        clog(L"NUMOS: froxel history %d frames (%s)", want, lit > 0 ? L"car lamps lit: lamp glow follows the car" : L"back to your setting");
      }
      histSent = want;
    }
  }
  if (g_enabled && g_apiOk && !g_sweep) rbrrtx_windscreen_frame_end();   // drops on the windscreen RBR drew this frame
  rbrrtx_particles_frame_end();                       // particle test key, emit probe report
  rbrrtx_wetness_frame(g_expStep >= 0);               // wetness.cpp: puddles build up while it rains
  {
    // Remix's fallback light (rtx.fallbackLightMode 1 = only while no light exists) went out whenever the
    // headlights came on at night (user 2026-10-23: it is their moonlight). Lights on and no sun: Always (2).
    // Only when the user's own setting is 1: it is read again (rtx.conf, then the Alt+X save <game>\user.conf) at
    // every headlights switch, so a Never (0) saved in Alt+X is left alone (user 2026-10-23: L kept turning it on).
    static int baseMode = -1, sentMode = -1;
    static bool keepWas = false;
    const bool keep = g_apiOk && rbrrtx_headlights_keep_fallback() && !g_sun;
    if (baseMode < 0 || keep != keepWas) {
      keepWas = keep;
      int mode = static_cast<int>(conf_float("rtx.fallbackLightMode", 1.0f));
      wchar_t user[MAX_PATH] = {};
      const DWORD n = GetModuleFileNameW(nullptr, user, MAX_PATH);
      if (n > 0 && n < MAX_PATH) {
        std::ifstream in(fs::path(user).parent_path() / L"user.conf");
        for (std::string l; std::getline(in, l);) {
          const size_t k = l.find("rtx.fallbackLightMode");
          if (k != std::string::npos && l.find_first_not_of(" \t") == k && l.find('=', k) != std::string::npos)
            mode = std::atoi(l.c_str() + l.find('=', k) + 1);
        }
      }
      if (baseMode < 0) sentMode = mode;               // what Remix starts with
      baseMode = mode;
    }
    const int want = keep && baseMode == 1 ? 2 : baseMode;
    if (want != sentMode && (want == 2 || sentMode == 2)) {   // only undo what we did, never set the user's value
      sentMode = want;
      remix_set("rtx.fallbackLightMode", want == 2 ? "2" : want == 0 ? "0" : "1");
      clog(L"fallback light: rtx.fallbackLightMode %d (%s)", want, want == 2 ? L"headlights on at night: kept" : L"back to your setting");
    } else if (want != sentMode) {
      sentMode = want;                                 // the user's own change: follow it silently
    }
  }
  if (rbrrtx_key_pressed(RBRKEY_GROUND_LAYER)) {       // bake -> old decal layer -> off
    const int want = g_layerWant == kLayerDecal ? kLayerBake : g_layerWant == kLayerBake ? kLayerOff : kLayerDecal;   // decal -> bake -> off
    const bool now = g_inMenu;                         // in the menu a switch is clean; on a stage it waits for the next load
    ground_layer_request(want, true);
    wchar_t msg[200];
    swprintf_s(msg, L"%s: ground second layer %s%s", rbrrtx_key_name(RBRKEY_GROUND_LAYER),
               want == kLayerBake ? L"BAKE (EXPERIMENT: second texture composited into the terrain bake)"
               : want == kLayerDecal ? L"DECAL (twin-texture decal layer over the terrain-tagged base)" : L"off (game shader only)",
               now ? L"" : L" - applies at the next stage load (go to the menu first)");
    log_frame(msg);
    Beep(want == kLayerBake ? 1320 : want == kLayerDecal ? 880 : 440, now ? 70 : 160);   // high / mid / low = bake / decal / off; long = later
  }
  ground_frame();
  debug_sky();
  if (g_apiOk) {                                       // retained world: hides, shows, stats
    const D3DMATRIX vp = mul(g_V, g_P);
    rbrrtx_apiworld_frame_end(g_frameCam ? &vp.m[0][0] : nullptr, g_enabled);
  }
  if (rbrrtx_key_pressed(RBRKEY_API_WORLD)) {
    wchar_t w[96];
    swprintf_s(w, L"%s: world through the Remix API: %s", rbrrtx_key_name(RBRKEY_API_WORLD), rbrrtx_apiworld_cycle());
    log_frame(w);
  }
  g_frameCam = false;
  g_btbPrev = g_st.btb > 0;
  const bool cameraNow = g_st.xformed > 0;
  if (cameraNow && g_detailFrames < 3) { ++g_detailFrames; log_frame(L"first frames with a camera"); }
  else if (g_frame < 3 || g_frame % 600 == 0) { log_frame(L"periodic"); cull_log_stats(); if (g_frame) rbrrtx_hudglow_log(); }
  rbrrtx_report_frame(g_wnd, { g_st.draws, g_st.xformed, g_st.ff, g_st.btb, { g_carPos[0], g_carPos[1], g_carPos[2] } });
  g_st = {};
  g_hinted = 0;
  g_byShader.clear();
  ++g_frame;
  rbrrtx_alphacut_frame(g_frame);
  rbrrtx_radnorm_frame(g_frame);
  g_sunDrawn = false;
  g_uiTriggered = false;
}

// ---------------------------------------------------------------- device hooks

HRESULT STDMETHODCALLTYPE hk_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count) {
  if (rbrrtx_menu_is_drawing()) return o.SetVertexShaderConstantF(dev, start, data, count);   // the F3 menu's own draws are not tracked
  if (data && start < 256 && !g_recording) {
    const UINT n = (std::min)(count, 256u - start);
    std::memcpy(g_c[start], data, n * 4 * sizeof(float));
    for (UINT i = start; i < start + n; ++i) g_cset[i] = true;
  }
  return o.SetVertexShaderConstantF(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE hk_SetPixelShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count) {
  if (rbrrtx_menu_is_drawing()) return o.SetPixelShaderConstantF(dev, start, data, count);
  if (data && start < 8 && !g_recording)               // ps_1_1 has c0-c7; c3 = car dirt colour (dirt_draw)
    std::memcpy(g_psc[start], data, (std::min)(count, 8u - start) * 4 * sizeof(float));
  return o.SetPixelShaderConstantF(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE hk_CreateVertexShader(IDirect3DDevice9* dev, const DWORD* fn, IDirect3DVertexShader9** out) {
  const HRESULT hr = o.CreateVertexShader(dev, fn, out);
  ++g_rbrrtxCreates;
  if (SUCCEEDED(hr) && fn && out && *out) {
    const size_t size = bytecode_size(fn, 1u << 16);
    const unsigned long long h = fnv1a(fn, size);
    ShaderInfo info;
    const std::wstring* name = nullptr;
    const wchar_t* how = L"";
    auto it = g_vso.find(h);
    if (it != g_vso.end()) name = &it->second;
    else if ((it = g_vsoCode.find(code_hash(fn, 1u << 16))) != g_vsoCode.end()) { name = &it->second; how = L", matched without comments"; }
    std::wstring ctab;
    if (name) { info.name = *name; info.flags = flags_for(*name); ++g_vsKnown; }
    else if (parse_btb_ctab(fn, 1u << 16, info, ctab)) {
      info.name = info.wvp.reg >= 0 ? L"(BTB effect)" : L"(BTB effect, instanced)";
      info.flags = SH_KNOWN | SH_BTB | SH_SCENERY;
      ++g_vsKnown;
      ++g_btbShaders;
    }
    else { info.name = L"(not an RBR .vso)"; ++g_vsUnknown; }
    g_vs[*out] = info;
    if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_vs(*out, fn, name ? name->c_str() : nullptr);
    clog(L"VS %p = %s (%zu bytes, hash %016llx%s)", static_cast<void*>(*out), info.name.c_str(), size, h, how);
    if (info.flags & SH_BTB)
      clog(L"  BTB constants: worldviewproj c%d/%d, worldMatrix c%d/%d, viewproj c%d/%d (%s); table: %s", info.wvp.reg,
           info.wvp.count, info.world.reg, info.world.count, info.vp.reg, info.vp.count,
           info.wvp.columns ? L"column_major" : L"row_major", ctab.c_str());
  }
  return hr;
}

void track_vs(void* vs) {
  g_curVS = vs;
  g_vsUnbound = false;                                 // the device gets the game's VS again
  g_curInfo = nullptr;
  if (vs) {
    auto it = g_vs.find(vs);
    if (it != g_vs.end() && (it->second.flags & SH_KNOWN)) g_curInfo = &it->second;
  }
}

// State blocks. ID3DXSprite / ID3DXFont (DirectorsCut's menu text) set their vertex declaration, WORLD and VIEW and
// render states by recording state blocks (BeginStateBlock .. EndStateBlock) and Apply-ing them around each batch, then
// Apply a Captured block to give the device back (traced through the bridge 2026-10-22). Calls made while recording
// change nothing on the device, and Apply changes it behind our Set* hooks. So per block we keep what it holds of the
// state we track (transforms, vertex input, VS), filled from the recorded calls or at Capture, and put it back at Apply.
struct BlockState {
  bool hasXf[3] = {}, hasVin = false, hasVS = false;
  D3DMATRIX game[3] = {}, dev[3] = {};
  bool gameSet[3] = {}, devValid[3] = {};
  int vin = kVinUnknown;
  void* decl = nullptr;
  DWORD fvf = 0;
  void* vs = nullptr;
  bool vsUnbound = false;
};
std::unordered_map<void*, BlockState> g_blocks;
BlockState g_rec;

void capture_into(BlockState& b) {
  for (int i = 0; i < 3; ++i)
    if (b.hasXf[i]) { b.game[i] = g_game[i]; b.gameSet[i] = g_gameSet[i]; b.dev[i] = g_dev[i]; b.devValid[i] = g_devValid[i]; }
  if (b.hasVin) { b.vin = g_vin; b.decl = g_curDecl; b.fvf = g_curFVF; }
  if (b.hasVS) { b.vs = g_curVS; b.vsUnbound = g_vsUnbound; }
}

HRESULT STDMETHODCALLTYPE hk_StateBlockCapture(IDirect3DStateBlock9* sb) {
  const HRESULT hr = o.StateBlockCapture(sb);
  auto it = g_blocks.find(sb);
  if (SUCCEEDED(hr) && it != g_blocks.end()) capture_into(it->second);
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_StateBlockApply(IDirect3DStateBlock9* sb) {
  const HRESULT hr = o.StateBlockApply(sb);
  auto it = g_blocks.find(sb);
  if (FAILED(hr) || it == g_blocks.end()) return hr;
  const BlockState& b = it->second;
  for (int i = 0; i < 3; ++i)
    if (b.hasXf[i]) { g_game[i] = b.game[i]; g_gameSet[i] = b.gameSet[i]; g_dev[i] = b.dev[i]; g_devValid[i] = b.devValid[i]; }
  if (b.hasVin) { g_vin = b.vin; g_curDecl = b.decl; g_curFVF = b.fvf; }
  if (b.hasVS) { track_vs(b.vs); g_vsUnbound = b.vsUnbound; }
  return hr;
}

void hook_block(IDirect3DStateBlock9* sb, const BlockState& b) {
  g_blocks[sb] = b;                                    // a new block may sit at a released one's address
  patch(&sb->lpVtbl->Capture, &hk_StateBlockCapture, o.StateBlockCapture);
  patch(&sb->lpVtbl->Apply, &hk_StateBlockApply, o.StateBlockApply);
}

HRESULT STDMETHODCALLTYPE hk_CreateStateBlock(IDirect3DDevice9* dev, D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out) {
  if (rbrrtx_menu_is_drawing()) return o.CreateStateBlock(dev, type, out);   // the menu's per-frame block: nothing tracked, its Apply restores the device
  const HRESULT hr = o.CreateStateBlock(dev, type, out);
  if (SUCCEEDED(hr) && out && *out) {                 // holds the current state of its type from the start
    BlockState b;
    b.hasVin = b.hasVS = type == D3DSBT_ALL || type == D3DSBT_VERTEXSTATE;
    for (int i = 0; i < 3; ++i) b.hasXf[i] = type == D3DSBT_ALL;
    capture_into(b);
    hook_block(*out, b);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_BeginStateBlock(IDirect3DDevice9* dev) {
  const HRESULT hr = o.BeginStateBlock(dev);
  if (SUCCEEDED(hr)) { g_recording = true; g_rec = BlockState(); }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_EndStateBlock(IDirect3DDevice9* dev, IDirect3DStateBlock9** out) {
  const HRESULT hr = o.EndStateBlock(dev, out);
  g_recording = false;
  if (SUCCEEDED(hr) && out && *out) hook_block(*out, g_rec);
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
  if (rbrrtx_menu_is_drawing()) return o.SetVertexShader(dev, vs);
  if (g_recording) { g_rec.hasVS = true; g_rec.vs = vs; g_rec.vsUnbound = false; }
  else track_vs(vs);
  return o.SetVertexShader(dev, vs);
}

HRESULT STDMETHODCALLTYPE hk_CreateVertexDeclaration(IDirect3DDevice9* dev, const D3DVERTEXELEMENT9* e,
                                                     IDirect3DVertexDeclaration9** out) {
  const HRESULT hr = o.CreateVertexDeclaration(dev, e, out);
  if (SUCCEEDED(hr) && out && *out) {
    g_declText[*out] = describe_decl(e);
    bool rhw = false;
    for (const D3DVERTEXELEMENT9* p = e; p && p->Stream != 0xFF; ++p) rhw |= p->Usage == D3DDECLUSAGE_POSITIONT;
    if (rhw) g_declRhw.insert(*out); else g_declRhw.erase(*out);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetVertexDeclaration(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl) {
  if (rbrrtx_menu_is_drawing()) return o.SetVertexDeclaration(dev, decl);
  if (g_recording) { g_rec.hasVin = true; g_rec.vin = kVinDecl; g_rec.decl = decl; g_rec.fvf = 0; }
  else { g_curDecl = decl; g_vin = kVinDecl; }
  return o.SetVertexDeclaration(dev, decl);
}

HRESULT STDMETHODCALLTYPE hk_SetFVF(IDirect3DDevice9* dev, DWORD fvf) {
  if (rbrrtx_menu_is_drawing()) return o.SetFVF(dev, fvf);
  if (g_recording) { g_rec.hasVin = true; g_rec.vin = kVinFvf; g_rec.decl = nullptr; g_rec.fvf = fvf; }
  else { g_curDecl = nullptr; g_curFVF = fvf; g_vin = kVinFvf; }
  return o.SetFVF(dev, fvf);
}

// Pre-transformed (XYZRHW / POSITIONT) vertices: HUD, menus, plugin overlays.
bool screen_space_input(IDirect3DDevice9* dev) {
  if (g_vin == kVinDecl) return g_curDecl && g_declRhw.count(g_curDecl);
  DWORD fvf = g_curFVF;
  if (g_vin == kVinUnknown) dev->lpVtbl->GetFVF(dev, &fvf);   // set before our hooks
  return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
}

HRESULT STDMETHODCALLTYPE hk_SetTextureStageState(IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD v) {
  if (rbrrtx_menu_is_drawing()) return o.SetTextureStageState(dev, stage, type, v);
  if (g_recording) return o.SetTextureStageState(dev, stage, type, v);
  if (stage == 0)
    for (int i = 0; i < 6; ++i)
      if (type == kTss[i]) g_tss0[i] = v;
  return o.SetTextureStageState(dev, stage, type, v);
}

HRESULT STDMETHODCALLTYPE hk_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD v) {
  if (rbrrtx_menu_is_drawing()) return o.SetRenderState(dev, state, v);
  if (state == D3DRS_LIGHTING && !g_recording) g_lighting = v;
  return o.SetRenderState(dev, state, v);
}

HRESULT STDMETHODCALLTYPE hk_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
  if (rbrrtx_menu_is_drawing()) return o.SetTransform(dev, state, m);
  static unsigned logged[3] = {};
  for (int i = 0; i < 3; ++i)                          // diagnostics: does the game (or a plugin) set these itself?
    if (state == kXf[i] && m && logged[i] < 5) {
      ++logged[i];
      clog(L"game SetTransform %s at frame %u: _11 %.3f _22 %.3f _33 %.4f _34 %.3f _43 %.3f _44 %.3f", i == 0 ? L"WORLD" : i == 1 ? L"VIEW"
           : L"PROJECTION", g_frame, m->_11, m->_22, m->_33, m->_34, m->_43, m->_44);
    }
  BlockState* rec = g_recording ? &g_rec : nullptr;   // recorded: lands on the device at Apply
  for (int i = 0; i < 3; ++i)
    if (state == kXf[i] && m) {
      if (rec) { rec->hasXf[i] = true; rec->game[i] = rec->dev[i] = *m; rec->gameSet[i] = rec->devValid[i] = true; }
      else { g_game[i] = *m; g_gameSet[i] = true; g_dev[i] = *m; g_devValid[i] = true; }
    }
  return o.SetTransform(dev, state, m);
}

bool is_primary_rt(IDirect3DSurface9* s) {
  if (!s) return false;
  if (s == g_backbuffer) return true;
  auto it = g_rtPrimary.find(s);
  if (it != g_rtPrimary.end()) return it->second;
  void* container = nullptr;
  bool texture = SUCCEEDED(s->lpVtbl->GetContainer(s, IID_IDirect3DTexture9, &container)) && container;
  if (container) static_cast<IUnknown*>(container)->lpVtbl->Release(static_cast<IUnknown*>(container));
  container = nullptr;
  if (!texture) {
    texture = SUCCEEDED(s->lpVtbl->GetContainer(s, IID_IDirect3DCubeTexture9, &container)) && container;
    if (container) static_cast<IUnknown*>(container)->lpVtbl->Release(static_cast<IUnknown*>(container));
  }
  g_rtPrimary[s] = !texture;
  if (g_rtLogged < 40) {                              // each new render target once: size, format, kind
    ++g_rtLogged;
    D3DSURFACE_DESC d = {};
    s->lpVtbl->GetDesc(s, &d);
    clog(L"render target %p: %ux%u format %d usage 0x%lx -> %s", static_cast<void*>(s), d.Width, d.Height,
         static_cast<int>(d.Format), d.Usage, texture ? L"texture (off-screen)" : L"surface (treated as primary)");
  }
  return !texture;
}

HRESULT STDMETHODCALLTYPE hk_SetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* rt) {
  if (index == 0) g_primaryRT = is_primary_rt(rt);
  if (rbrrtx_gauge_probe_on && index == 0) rbrrtx_gauge_probe_rt(rt);
  return o.SetRenderTarget(dev, index, rt);
}

HRESULT STDMETHODCALLTYPE hk_BeginScene(IDirect3DDevice9* dev) {
  const HRESULT hr = o.BeginScene(dev);
  ++g_st.scenes;
  if (SUCCEEDED(hr) && !g_sunDrawn) {
    g_sunDrawn = true;
    if (g_apiOk) rbrrtx_apiworld_replay();             // retained world instances, before this frame's draws
    update_sun();
    if (g_enabled && g_apiOk) rbrrtx_headlights_emit();   // once per frame, with the sun: from RBR's car object
    if (g_enabled && g_apiOk) {                            // particles.cpp, windscreen.cpp (a sweep stops them unless proxy.precip)
      const bool quiet = g_sweep && !g_sweepPrecip;
      rbrrtx_particles_emit(quiet);
      rbrrtx_windscreen_emit(quiet);
    }
  }
  return hr;
}

void destroy_sun(const wchar_t* why) {
  if (!g_sun) return;
  const remixapi_ErrorCode err = g_api.DestroyLight(g_sun);
  g_sun = nullptr;
  clog(L"sun: destroyed (%s), error %d", why, static_cast<int>(err));
}

// The final Release destroys the device; drop our Remix API light first. A light still alive when the bridge
// shuts down made exits take ~30 s with Remix's Reflex mode on (found 2026-10-15).
ULONG STDMETHODCALLTYPE hk_Release(IDirect3DDevice9* dev) {
  const ULONG menuRefs = static_cast<ULONG>(rbrrtx_menu_device_refs());   // the F3 menu's DX9 backend holds one reference
  if (g_apiOk || menuRefs) {
    const ULONG refs = o.AddRef(dev);
    o.Release(dev);
    if (refs == 2 + menuRefs) {                        // the game's last reference (plus the menu's)
      if (menuRefs) rbrrtx_menu_release_device();
      if (g_apiOk) { destroy_sun(L"device released"); rbrrtx_apiworld_reset(L"device released"); rbrrtx_headlights_destroy(); rbrrtx_particles_destroy(); rbrrtx_windscreen_destroy(); rbrrtx_litparts_destroy(); }
    }
  }
  return o.Release(dev);
}

HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
  if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_frame();
  end_frame(dev);
  f3_present(dev);                                     // F3 menu: list updates, texture probe, the menu's own scene
  const HRESULT hr = o.Present(dev, src, dst, wnd, dirty);
  if (g_remixPlus) rbrrtx_remixplus_frame();           // light destroys before this Present no longer block their hash
  return hr;
}

void remember_backbuffer(IDirect3DDevice9* dev) {
  IDirect3DSurface9* bb = nullptr;
  if (SUCCEEDED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
    g_backbuffer = bb;
    D3DSURFACE_DESC d = {};
    bb->lpVtbl->GetDesc(bb, &d);
    clog(L"back buffer %p: %ux%u format %d", static_cast<void*>(bb), d.Width, d.Height, static_cast<int>(d.Format));
    bb->lpVtbl->Release(bb);
  }
  g_rtPrimary.clear();
  g_primaryRT = true;
}

HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
  f3_before_reset();                                   // default-pool menu objects and held texture references go first
  const HRESULT hr = o.Reset(dev, pp);
  f3_after_reset(dev, hr);
  if (SUCCEEDED(hr)) {                                 // Reset restores default device state
    std::memset(g_cset, 0, sizeof(g_cset));
    for (int i = 0; i < 3; ++i) g_devValid[i] = g_gameSet[i] = false;
    g_haveCam = g_frameCam = false;
    g_curVS = nullptr;
    g_curInfo = nullptr;
    g_curDecl = nullptr;
    g_curFVF = 0;
    g_vin = kVinUnknown;
    std::memcpy(g_tss0, kTssDefault, sizeof(g_tss0));
    g_lighting = TRUE;
    remember_backbuffer(dev);
    clog(L"device Reset");
    if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_event(L"device Reset");
  }
  return hr;
}

// Untextured fixed-function screen-space quad on the primary target before the HUD has started (no UI trigger yet,
// some 3D already drawn): RBR draws some in the middle of the scene (Zlin dump 05:52 #1155, additive, after the car;
// Valstagna 06:00: a multiplicative DESTCOLOR/ZERO one).
// Remix would rasterize it into an image the path tracer then paints over, or, with ZWRITE off, take it as the first
// UI draw and rasterize the rest of the scene. Not drawn. Exception: inside an occlusion query (that quad is RBR's sun
// visibility test, dump 06:00): Remix rasterizes query draws without its UI check, and the game needs the result.
bool is_midscene_quad(IDirect3DDevice9* dev) {
  if (g_curVS || !g_primaryRT || !g_enabled || g_uiTriggered || g_st.xformed == 0 || g_oqActive > 0) return false;
  if (!screen_space_input(dev)) return false;
  IDirect3DBaseTexture9* t0 = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t0);
  if (t0) { t0->lpVtbl->Release(t0); return false; }
  return true;
}

// ---------------------------------------------------------------- distance culling (culling.ini, culling key, experiment steps)
// K runs 2026-10-17 (Chirdonhead forest): foliage costs ~2.9 ms of ~15 ms, mostly by being in the scene (~1.8 ms; its
// alpha test only ~1.1), ground ~2.1, objects ~1.1. RBR submits geometry the camera cannot see (behind, beside), which
// Remix still traces for shadows, reflections and bounce light. Culling those far away costs little in the picture;
// culling VISIBLE far geometry deletes it from the image, so that is a separate, optional threshold (default off).
// Per draw: the bounding sphere of its vertex range is read ONCE from the vertex buffer (the Remix bridge client keeps
// a full copy; a READONLY lock returns it without sending anything, lockable_buffer.h:192) and cached by
// (buffer, generation, offset, stride, first vertex, count). Then each frame: sphere -> world (world-space RL*/Water
// shaders: identity; others: W from wvp * (VP)^-1) -> distance to the car (never cull inside keep_radius_m) and a
// frustum test. Two thresholds per decision: cull beyond X, bring back below X * (1 - hysteresis).
struct CullCfg {
  bool enabled = false;
  std::vector<std::wstring> classes = { L"Sway", L"ObjPos", L"IO", L"RLSingleTexture.vso", L"RLDouble" };
  float offscreenFar = 100.0f;                 // outside the view and farther than this: culled
  float visibleFar = 0.0f;                     // inside the view: only beyond this distance (0 = never)
  float visibleMaxPct = 0.0f;                  // ...and only if smaller on screen than this % of its height (0 = any size)
  // Visible culling never touches ground/road: only these classes (Hualapai 2026-10-17: culled RLDouble = road gone).
  std::vector<std::wstring> visibleClasses = { L"Sway", L"ObjPos", L"IO", L"RLSingleTexture.vso" };
  float keepRadius = 30.0f;                    // never cull within this distance of the car
  float hysteresis = 0.1f;
  float frustumMargin = 5.0f;                  // metres added to the sphere for the view test
};
CullCfg g_cull;
struct CullEntry { float c[3] = {}, r = 0; bool valid = false, culled = false, ffFar = false; unsigned lastFrame = 0; };
std::unordered_map<uint64_t, CullEntry> g_cullCache;
std::unordered_map<const void*, unsigned> g_vbGen;          // bumped when the game creates a buffer at an address
std::unordered_map<const ShaderInfo*, bool> g_cullClassCache, g_cullVisCache;
UINT g_drawFirstVert = 0, g_drawNumVert = 0;                 // set by the draw hooks (0 = not cullable, e.g. UP draws)
D3DPRIMITIVETYPE g_drawType = D3DPT_TRIANGLELIST;           // primitive type of the draw (lamp marker)
struct CullStats { unsigned considered = 0, offscreen = 0, visibleFar = 0, kept = 0, unreadable = 0, misses = 0;
                   unsigned dist[5] = {};                       // dist buckets: <50, <100, <200, <400, >=400 m
                   unsigned size[5] = {}; } g_cs;               // visible candidates by screen size: <0.5, <1, <2, <5, >=5 %
unsigned g_csFrames = 0;

std::vector<std::wstring> split_list(const std::wstring& v) {
  std::vector<std::wstring> out;
  for (size_t a = 0; a <= v.size();) {
    size_t b = v.find(L',', a);
    if (b == std::wstring::npos) b = v.size();
    std::wstring p = v.substr(a, b - a);
    const size_t s = p.find_first_not_of(L" \t"), e = p.find_last_not_of(L" \t");
    if (s != std::wstring::npos) out.push_back(p.substr(s, e - s + 1));
    a = b + 1;
  }
  return out;
}

void cull_set(const std::wstring& k, const std::wstring& v) {
  if (k == L"enabled") g_cull.enabled = _wtoi(v.c_str()) != 0 || !_wcsicmp(v.c_str(), L"true");
  else if (k == L"classes") { g_cull.classes = split_list(v); g_cullClassCache.clear(); }
  else if (k == L"visible_classes") { g_cull.visibleClasses = split_list(v); g_cullVisCache.clear(); }
  else if (k == L"visible_max_screen_pct") g_cull.visibleMaxPct = static_cast<float>(_wtof(v.c_str()));
  else if (k == L"offscreen_far_m") g_cull.offscreenFar = static_cast<float>(_wtof(v.c_str()));
  else if (k == L"visible_far_m") g_cull.visibleFar = static_cast<float>(_wtof(v.c_str()));
  else if (k == L"keep_radius_m") g_cull.keepRadius = static_cast<float>(_wtof(v.c_str()));
  else if (k == L"hysteresis") g_cull.hysteresis = std::clamp(static_cast<float>(_wtof(v.c_str())), 0.0f, 0.5f);
  else if (k == L"frustum_margin_m") g_cull.frustumMargin = static_cast<float>(_wtof(v.c_str()));
  else return;
  for (auto& e : g_cullCache) e.second.culled = false;   // decisions restart under the new settings
}

void cull_load() {
  g_cull = CullCfg();
  g_cullClassCache.clear();
  g_cullVisCache.clear();
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_CULLING", L"culling.ini", path, MAX_PATH * 2);
  wchar_t body[4096];
  if (!GetPrivateProfileSectionW(L"culling", body, 4096, path)) return;
  for (const wchar_t* line = body; *line; line += wcslen(line) + 1) {
    std::wstring l = line;
    const size_t eq = l.find(L'=');
    if (eq == std::wstring::npos || l[0] == L';') continue;
    auto trim = [](std::wstring s) { const size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
                                      return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1); };
    cull_set(trim(l.substr(0, eq)), trim(l.substr(eq + 1)));
  }
}

void cull_log_settings(const wchar_t* why) {
  std::wstring cls;
  for (const auto& c : g_cull.classes) cls += (cls.empty() ? L"" : L",") + c;
  std::wstring vcls;
  for (const auto& c : g_cull.visibleClasses) vcls += (vcls.empty() ? L"" : L",") + c;
  clog(L"culling (%s): %s, classes %s, off-screen beyond %.0f m, visible (%s) beyond %s if under %.1f %% of the screen, keep radius %.0f m, hysteresis %.2f",
       why, g_cull.enabled ? L"ON" : L"off", cls.c_str(), g_cull.offscreenFar, vcls.c_str(),
       g_cull.visibleFar > 0 ? std::to_wstring(static_cast<int>(g_cull.visibleFar)).append(L" m").c_str() : L"never",
       g_cull.visibleMaxPct, g_cull.keepRadius, g_cull.hysteresis);
}

void cull_log_stats() {
  if (!g_csFrames || !g_cs.considered) { g_cs = {}; g_csFrames = 0; return; }
  const double n = g_csFrames;
  clog(L"  culling %s: per frame %.1f considered, %.1f culled off-screen, %.1f culled visible-far, %.1f kept near car; "
       L"distances <50/<100/<200/<400/more m: %.1f/%.1f/%.1f/%.1f/%.1f; visible candidates by screen size <0.5/<1/<2/<5/more %%: "
       L"%.1f/%.1f/%.1f/%.1f/%.1f; cache %zu ranges (%u new, %u unreadable)",
       g_cull.enabled ? L"ON" : L"off (counting only)", g_cs.considered / n, g_cs.offscreen / n, g_cs.visibleFar / n,
       g_cs.kept / n, g_cs.dist[0] / n, g_cs.dist[1] / n, g_cs.dist[2] / n, g_cs.dist[3] / n, g_cs.dist[4] / n,
       g_cs.size[0] / n, g_cs.size[1] / n, g_cs.size[2] / n, g_cs.size[3] / n, g_cs.size[4] / n,
       g_cullCache.size(), g_cs.misses, g_cs.unreadable);
  g_cs = {};
  g_csFrames = 0;
}

bool cull_visible_class() {
  if (!g_curInfo) return false;
  auto it = g_cullVisCache.find(g_curInfo);
  if (it != g_cullVisCache.end()) return it->second;
  bool hit = false;
  for (const auto& part : g_cull.visibleClasses) if (g_curInfo->name.find(part) != std::wstring::npos) { hit = true; break; }
  g_cullVisCache[g_curInfo] = hit;
  return hit;
}

bool cull_class() {
  if (!g_curInfo) return false;
  auto it = g_cullClassCache.find(g_curInfo);
  if (it != g_cullClassCache.end()) return it->second;
  bool hit = false;
  for (const auto& part : g_cull.classes) if (g_curInfo->name.find(part) != std::wstring::npos) { hit = true; break; }
  g_cullClassCache[g_curInfo] = hit;
  return hit;
}

int g_cullBuildBudget = -1;                                  // new spheres cull_entry may still read now (-1 = any)

// Object-space bounding sphere of the current draw's vertex range, read once and cached; nullptr if unreadable.
CullEntry* cull_entry(IDirect3DDevice9* dev) {
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  if (FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride)) || !vb) return nullptr;
  const unsigned gen = g_vbGen[vb];
  const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)) * 0x9E3779B97F4A7C15ull) ^
                       (static_cast<uint64_t>(gen) << 56) ^ (static_cast<uint64_t>(offset) << 40) ^
                       (static_cast<uint64_t>(stride) << 32) ^ (static_cast<uint64_t>(g_drawFirstVert) << 12) ^ g_drawNumVert;
  CullEntry& e = g_cullCache[key];
  if (e.valid || e.lastFrame == UINT_MAX) { vb->lpVtbl->Release(vb); return e.valid ? &e : nullptr; }
  if (g_cullBuildBudget == 0) { vb->lpVtbl->Release(vb); return nullptr; }   // later frame
  if (g_cullBuildBudget > 0) --g_cullBuildBudget;
  ++g_cs.misses;
  e.lastFrame = UINT_MAX;                                     // "tried": an unreadable range is not retried
  // Position element of stream 0 (FLOAT3/FLOAT4) from the vertex declaration.
  int posOffset = -1;
  IDirect3DVertexDeclaration9* decl = nullptr;
  if (SUCCEEDED(dev->lpVtbl->GetVertexDeclaration(dev, &decl)) && decl) {
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, el, &n)))
      for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
        if (el[i].Stream == 0 && el[i].Usage == D3DDECLUSAGE_POSITION && el[i].UsageIndex == 0 &&
            (el[i].Type == D3DDECLTYPE_FLOAT3 || el[i].Type == D3DDECLTYPE_FLOAT4)) posOffset = el[i].Offset;
    decl->lpVtbl->Release(decl);
  }
  D3DVERTEXBUFFER_DESC desc = {};
  void* data = nullptr;
  const UINT start = offset + g_drawFirstVert * stride, bytes = g_drawNumVert * stride;
  if (posOffset < 0 || stride < 12 || FAILED(vb->lpVtbl->GetDesc(vb, &desc)) || start + bytes > desc.Size ||
      FAILED(vb->lpVtbl->Lock(vb, start, bytes, &data, D3DLOCK_READONLY)) || !data) {
    ++g_cs.unreadable;
    vb->lpVtbl->Release(vb);
    return nullptr;
  }
  float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
  const uint8_t* p = static_cast<const uint8_t*>(data) + posOffset;
  for (UINT i = 0; i < g_drawNumVert; ++i, p += stride) {
    float v[3];
    std::memcpy(v, p, sizeof(v));
    for (int k = 0; k < 3; ++k) { lo[k] = (std::min)(lo[k], v[k]); hi[k] = (std::max)(hi[k], v[k]); }
  }
  vb->lpVtbl->Unlock(vb);
  vb->lpVtbl->Release(vb);
  float r2 = 0;
  for (int k = 0; k < 3; ++k) { e.c[k] = 0.5f * (lo[k] + hi[k]); const float h = 0.5f * (hi[k] - lo[k]); r2 += h * h; }
  e.r = std::sqrt(r2);
  e.valid = std::isfinite(e.r) && e.r < 1e6f;
  return e.valid ? &e : nullptr;
}

// Ground under a wheel (particles.cpp asks when a wheel meets a new surface; rbrrtx_surface_lookup_on is false
// otherwise, so this costs nothing then). Opaque scenery triangle-list draws only: the draw's cached bounding sphere
// (cull_entry, at most 30 new spheres read per frame) must cover the wheel point; then the draw's triangles are read
// once (READONLY locks), moved to world space with the draw's own world matrix, and the triangle straight below or
// above the point within 1 m reports its height difference and the stage-0 texture's average colour.
void surface_lookup(IDirect3DDevice9* dev) {
  static unsigned frame = UINT_MAX, built = 0;
  if (frame != g_frame) { frame = g_frame; built = 0; }
  float pt[3];
  if (!g_dip.valid || !g_curInfo || (g_curInfo->flags & (SH_CAR | SH_SKY | SH_GLASS | SH_PARTICLE | SH_SWAY)) ||
      !rbrrtx_surface_lookup_point(pt))
    return;
  DWORD blend = 0;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  if (blend) return;
  const int before = 30 - static_cast<int>(built);
  g_cullBuildBudget = before < 0 ? 0 : before;
  CullEntry* e = cull_entry(dev);
  built += static_cast<unsigned>(before - g_cullBuildBudget);
  g_cullBuildBudget = -1;
  if (!e) return;
  const bool world = (g_curInfo->flags & SH_WORLDSPACE) != 0;
  D3DMATRIX w = {};
  if (!world) w = to_f(mul_d(to_d(from_rows(&g_c[0])), g_VPinv));
  auto xf = [&](const float* v, float* out) {
    if (world) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; return; }
    for (int k = 0; k < 3; ++k) out[k] = v[0] * w.m[0][k] + v[1] * w.m[1][k] + v[2] * w.m[2][k] + w.m[3][k];
  };
  float c[3], r = e->r;
  xf(e->c, c);
  if (!world) {
    float s = 0;
    for (int i = 0; i < 3; ++i) s = (std::max)(s, std::sqrt(w.m[i][0] * w.m[i][0] + w.m[i][1] * w.m[i][1] + w.m[i][2] * w.m[i][2]));
    r *= s;
  }
  const float dx = pt[0] - c[0], dz = pt[2] - c[2];
  if (dx * dx + dz * dz > r * r || std::fabs(pt[1] - c[1]) > r + 1.0f) return;
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  UINT off = 0, stride = 0;
  int posOff = -1, nrmOff = -1;
  btb_decl_offsets(dev, posOff, nrmOff);
  if (posOff < 0 || FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &off, &stride)) || !vb) return;
  if (FAILED(dev->lpVtbl->GetIndices(dev, &ib)) || !ib) { vb->lpVtbl->Release(vb); return; }
  D3DINDEXBUFFER_DESC id = {};
  std::vector<unsigned> idx;
  std::vector<float> pos;
  void* p = nullptr;
  const UINT isz = SUCCEEDED(ib->lpVtbl->GetDesc(ib, &id)) && id.Format == D3DFMT_INDEX32 ? 4 : 2, nIdx = g_dip.count * 3;
  if (SUCCEEDED(ib->lpVtbl->Lock(ib, g_dip.startIdx * isz, nIdx * isz, &p, D3DLOCK_READONLY)) && p) {
    idx.resize(nIdx);
    for (UINT i = 0; i < nIdx; ++i) idx[i] = isz == 4 ? static_cast<const unsigned*>(p)[i] : static_cast<const WORD*>(p)[i];
    ib->lpVtbl->Unlock(ib);
  }
  const UINT first = off + (static_cast<UINT>(g_dip.base) + g_dip.minIdx) * stride;
  if (!idx.empty() && SUCCEEDED(vb->lpVtbl->Lock(vb, first, g_dip.numV * stride, &p, D3DLOCK_READONLY)) && p) {
    pos.resize(static_cast<size_t>(g_dip.numV) * 3);
    for (UINT i = 0; i < g_dip.numV; ++i) xf(reinterpret_cast<const float*>(static_cast<const uint8_t*>(p) + i * stride + posOff), &pos[i * 3]);
    vb->lpVtbl->Unlock(vb);
  }
  vb->lpVtbl->Release(vb);
  ib->lpVtbl->Release(ib);
  float best = 1.0f;
  for (size_t t = 0; t + 2 < idx.size() && !pos.empty(); t += 3) {
    const unsigned a = idx[t] - g_dip.minIdx, b = idx[t + 1] - g_dip.minIdx, cc = idx[t + 2] - g_dip.minIdx;
    if (a >= g_dip.numV || b >= g_dip.numV || cc >= g_dip.numV) continue;
    const float *A = &pos[a * 3], *B = &pos[b * 3], *C = &pos[cc * 3];
    const float d = (B[2] - C[2]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[2] - C[2]);   // barycentric in XZ
    if (std::fabs(d) < 1e-8f) continue;
    const float l0 = ((B[2] - C[2]) * (pt[0] - C[0]) + (C[0] - B[0]) * (pt[2] - C[2])) / d;
    const float l1 = ((C[2] - A[2]) * (pt[0] - C[0]) + (A[0] - C[0]) * (pt[2] - C[2])) / d;
    const float l2 = 1.0f - l0 - l1;
    if (l0 < 0 || l1 < 0 || l2 < 0) continue;
    const float dh = std::fabs(pt[1] - (l0 * A[1] + l1 * B[1] + l2 * C[1]));
    if (dh < best) best = dh;
  }
  if (best >= 1.0f) return;
  IDirect3DBaseTexture9* tex = nullptr;
  float rgb[3];
  if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &tex)) && tex) {
    if (rbrrtx_texture_avg_rgb(tex, rgb)) rbrrtx_surface_lookup_hit(best, rgb, rbrrtx_texture_hash(tex));
    tex->lpVtbl->Release(tex);
  }
}

// Sphere fully outside the view frustum of the current camera (row-vector D3D matrices, clip z 0..w)?
bool outside_view(const float c[3], float r) {
  const D3DMATRIX vp = to_f(mul_d(to_d(g_V), to_d(g_P)));
  const float (*m)[4] = vp.m;
  const float planes[6][4] = {
    { m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0] },   // left
    { m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0] },   // right
    { m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1] },   // bottom
    { m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1] },   // top
    { m[0][2], m[1][2], m[2][2], m[3][2] },                                           // near
    { m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2] },   // far
  };
  for (const auto& pl : planes) {
    const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
    if (len <= 0) continue;
    if ((pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] + pl[3]) / len < -r) return true;
  }
  return false;
}

void cull_frame_tick() { ++g_csFrames; }
bool cull_toggle() { g_cull.enabled = !g_cull.enabled; for (auto& e : g_cullCache) e.second.culled = false; return g_cull.enabled; }

// True = do not draw. Counts statistics even with culling off, so the log shows what it would do.
// Only with this frame's camera (g_frameCam): the camera comes from the frame's first world-space draw, which runs after
// this test. With an older camera (Toikkala/Toivonvaara, run 20261024-000533: the menu's camera at (4, -1, 63) kept
// while the stage sat at (2838, 139, 1049)) every world-space draw was culled as off-screen before it could renew the
// camera, so it never renewed: 827 of 849 draws culled for the whole stage, "only ~15 draws, unknown shaders".
// The current draw's bounding sphere in world space (cull_entry's, moved with this draw's W); needs the frame camera.
// force: read a new range even when this frame's budget is spent (dumps, the few lamp draws).
bool draw_world_sphere(IDirect3DDevice9* dev, float c[3], float* r, bool force) {
  if (!g_curInfo || g_drawNumVert == 0) return false;
  const int saved = g_cullBuildBudget;
  if (force) g_cullBuildBudget = -1;
  CullEntry* e = cull_entry(dev);
  if (force) g_cullBuildBudget = saved;
  if (!e) return false;
  c[0] = e->c[0]; c[1] = e->c[1]; c[2] = e->c[2];
  *r = e->r;
  if (!(g_curInfo->flags & SH_WORLDSPACE)) {               // object space -> world with this draw's W
    const D3DMATRIX w = to_f(mul_d(to_d(from_rows(&g_c[0])), g_VPinv));
    const float x = c[0], y = c[1], z = c[2];
    for (int k = 0; k < 3; ++k) c[k] = x * w.m[0][k] + y * w.m[1][k] + z * w.m[2][k] + w.m[3][k];
    const float sx = std::sqrt(w.m[0][0] * w.m[0][0] + w.m[0][1] * w.m[0][1] + w.m[0][2] * w.m[0][2]);
    const float sy = std::sqrt(w.m[1][0] * w.m[1][0] + w.m[1][1] * w.m[1][1] + w.m[1][2] * w.m[1][2]);
    const float sz = std::sqrt(w.m[2][0] * w.m[2][0] + w.m[2][1] * w.m[2][1] + w.m[2][2] * w.m[2][2]);
    *r *= (std::max)(sx, (std::max)(sy, sz));
  }
  return true;
}

// materials.ini [skip_draws] textures = <hash>, ...: draws never made (user 2026-11-01: the game's sun flare
// 40AA313D3EE0E9B6, a screen-space additive sprite at the sun; only ignored in Remix it drew as a square).
std::unordered_set<unsigned long long> g_skipTex;
unsigned g_skipTexDraws = 0;

void load_skip_textures() {
  wchar_t path[MAX_PATH * 2], v[2048] = {};
  rbrrtx_config_path(L"RBRRTX_MATERIALS", L"materials.ini", path, MAX_PATH * 2);
  GetPrivateProfileStringW(L"skip_draws", L"textures", L"", v, 2048, path);
  for (wchar_t* ctx = nullptr, *t = wcstok_s(v, L", ", &ctx); t; t = wcstok_s(nullptr, L", ", &ctx))
    if (const unsigned long long h = _wcstoui64(t, nullptr, 16)) g_skipTex.insert(h);
  g_flareSkip = GetPrivateProfileIntW(L"skip_draws", L"lens_flare", 1, path) != 0;
  clog(L"skip draws: %zu texture(s) never drawn (materials.ini [skip_draws]); lens flare (screen-space additive draws after the sun query) %s",
       g_skipTex.size(), g_flareSkip ? L"skipped" : L"left alone");
}

// The game's lens flare, structurally (user 2026-11-02: not one hash, the whole family): fixed-function screen-space
// (XYZRHW) draws with a texture, additive (destination ONE), depth test off, in the 3D part of the frame - after the sun's
// occlusion query ended this frame, or before the first HUD draw. Zlin dump 01:53:21: #300 untextured quad, then #301-309
// 40AA313D3EE0E9B6 (128x128), 2B06468C18F57C43 / D72822A228C6CD91 / 57E264023A357AE2 (32x32), all blend 5/2, z off. The HUD's
// own additive draws come after the trigger and keep drawing. The F3 Debug page also hides the sun-outline candidates live:
// 544BB1AB506B2F73 (a 1024x128 world sprite), 40AA313D3EE0E9B6 and the cloud layer (CloudsSingleTexture).
bool lens_flare_or_hidden(IDirect3DDevice9* dev) {
  if (!g_primaryRT || !g_enabled) return false;
  if (g_dbgHideClouds && g_curVS && g_curInfo && g_curInfo->name.find(L"CloudsSingleTexture") != std::wstring::npos) { ++g_hideSkipped; return true; }
  if (g_curVS || !(g_flareSkip || g_dbgHideOutlineSprite || g_dbgHideSunBillboard)) return false;
  IDirect3DBaseTexture9* t = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t);
  if (!t) return false;
  const unsigned long long hx = rbrrtx_texture_hash(t);
  t->lpVtbl->Release(t);
  if ((g_dbgHideOutlineSprite && hx == 0x544BB1AB506B2F73ull) || (g_dbgHideSunBillboard && hx == 0x40AA313D3EE0E9B6ull)) { ++g_hideSkipped; return true; }
  if (!g_flareSkip || g_st.xformed == 0 || !screen_space_input(dev)) return false;
  DWORD blend = 0, dest = 0, z = 1;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dest);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ZENABLE, &z);
  if (!blend || dest != D3DBLEND_ONE || z) return false;
  const bool afterQuery = g_oqEndFrame == g_frame;
  static std::unordered_set<unsigned long long> seen;
  if (afterQuery || !g_uiTriggered) {
    ++g_flareSkipped;
    if (seen.insert(hx).second && seen.size() <= 24)
      clog(L"lens flare: screen-space additive draw skipped, texture %016llX (%s, frame %u, %u skipped so far)", hx,
           afterQuery ? L"after the sun occlusion query" : L"before the first HUD draw", g_frame, g_flareSkipped);
    return true;
  }
  static std::unordered_set<unsigned long long> kept;
  if (kept.insert(hx).second && kept.size() <= 12)
    clog(L"lens flare: additive screen-space draw KEPT (HUD part of the frame), texture %016llX", hx);
  return false;
}

// A lamp inside an atlas (lights.ini uv = ...): the points of the groups of this draw's vertices whose TEXCOORD0 lies in
// the rectangle (stream 0 only), in the draw's own space; cached per buffer range like cull_entry. A group's point is its
// centre, or its highest / lowest y (anchor 0 top, 1 centre, 2 bottom; pole kinds: bottom).
std::unordered_map<uint64_t, std::vector<std::array<float, 3>>> g_uvLampCache;

// Stream-0 layout of the bound vertex declaration: offsets of POSITION (FLOAT3/FLOAT4) and TEXCOORD0, and its type; -1 = absent.
void stream0_layout(IDirect3DDevice9* dev, int* posOff, int* uvOff, int* uvType) {
  IDirect3DVertexDeclaration9* decl = nullptr;
  if (FAILED(dev->lpVtbl->GetVertexDeclaration(dev, &decl)) || !decl) return;
  D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
  UINT n = MAXD3DDECLLENGTH + 1;
  if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, el, &n)))
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
      if (el[i].Stream != 0 || el[i].UsageIndex != 0) continue;
      if (el[i].Usage == D3DDECLUSAGE_POSITION && (el[i].Type == D3DDECLTYPE_FLOAT3 || el[i].Type == D3DDECLTYPE_FLOAT4)) *posOff = el[i].Offset;
      if (el[i].Usage == D3DDECLUSAGE_TEXCOORD) { *uvOff = el[i].Offset; *uvType = el[i].Type; }
    }
  decl->lpVtbl->Release(decl);
}

const std::vector<std::array<float, 3>>* uv_lamp_points(IDirect3DDevice9* dev, const float (*rects)[4], int nRects,
                                                      float poleMin, int anchor, unsigned long long tex) {
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  if (FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride)) || !vb) return nullptr;
  const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)) * 0x9E3779B97F4A7C15ull) ^
                       (static_cast<uint64_t>(g_vbGen[vb]) << 56) ^ (static_cast<uint64_t>(offset) << 40) ^
                       (static_cast<uint64_t>(g_drawFirstVert) << 12) ^ g_drawNumVert ^ (tex * 0xC2B2AE3D27D4EB4Full) ^
                       (static_cast<uint64_t>(anchor + 1) * 0x94D049BB133111EBull);
  auto it = g_uvLampCache.find(key);
  if (it != g_uvLampCache.end()) { vb->lpVtbl->Release(vb); return &it->second; }
  std::vector<std::array<float, 3>>& out = g_uvLampCache[key];
  int posOff = -1, uvOff = -1, uvType = -1;
  stream0_layout(dev, &posOff, &uvOff, &uvType);
  D3DVERTEXBUFFER_DESC desc = {};
  void* data = nullptr;
  const UINT start = offset + g_drawFirstVert * stride, bytes = g_drawNumVert * stride;
  if (posOff < 0 || uvOff < 0 || (uvType != D3DDECLTYPE_FLOAT2 && uvType != D3DDECLTYPE_FLOAT16_2) || stride < 12 ||
      FAILED(vb->lpVtbl->GetDesc(vb, &desc)) || start + bytes > desc.Size ||
      FAILED(vb->lpVtbl->Lock(vb, start, bytes, &data, D3DLOCK_READONLY)) || !data) {
    vb->lpVtbl->Release(vb);
    clog(L"world lights: texture %016llX: vertices not readable here (position %d, uv %d type %d)", tex, posOff, uvOff, uvType);
    return &out;                                       // empty: tried once
  }
  auto half = [](uint16_t h) {                         // IEEE half -> float (no denormals needed for UVs)
    const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF, sgn = (h & 0x8000u) << 16;
    const uint32_t f = e == 0 ? sgn : sgn | ((e + 112) << 23) | (m << 13);
    float r; std::memcpy(&r, &f, 4); return r;
  };
  struct Group { float sum[3], n, lo[3], hi[3]; };
  std::vector<Group> groups;
  std::vector<std::array<float, 3>> all;               // pole test: every vertex of the draw
  if (poleMin > 0) all.reserve(g_drawNumVert);
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (UINT i = 0; i < g_drawNumVert; ++i, p += stride) {
    float x[3];
    std::memcpy(x, p + posOff, 12);
    if (poleMin > 0) all.push_back({ x[0], x[1], x[2] });
    float uv[2];
    if (uvType == D3DDECLTYPE_FLOAT2) std::memcpy(uv, p + uvOff, 8);
    else { uint16_t h[2]; std::memcpy(h, p + uvOff, 4); uv[0] = half(h[0]); uv[1] = half(h[1]); }
    const float u = uv[0] - std::floor(uv[0]), v = uv[1] - std::floor(uv[1]);   // tiled UVs: the atlas cell
    bool in = false;
    for (int r = 0; r < nRects && !in; ++r)
      in = u >= rects[r][0] && u <= rects[r][2] && v >= rects[r][1] && v <= rects[r][3];
    if (!in) continue;
    bool joined = false;
    for (auto& g : groups) {
      const float dx = g.sum[0] / g.n - x[0], dy = g.sum[1] / g.n - x[1], dz = g.sum[2] / g.n - x[2];
      if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) {
        for (int k = 0; k < 3; ++k) { g.sum[k] += x[k]; g.lo[k] = (std::min)(g.lo[k], x[k]); g.hi[k] = (std::max)(g.hi[k], x[k]); }
        g.n += 1;
        joined = true;
        break;
      }
    }
    if (!joined && groups.size() < 256) groups.push_back({ { x[0], x[1], x[2] }, 1.0f, { x[0], x[1], x[2] }, { x[0], x[1], x[2] } });
  }
  vb->lpVtbl->Unlock(vb);
  vb->lpVtbl->Release(vb);
  // Pole test (lights-auto.ini, auto = 1; Y up): the group is a lamp head when it is small, nothing of the same draw
  // stands more than 0.5 m above it within 0.6 m (a window or a sign in a wall has the roof or the board above), and
  // the draw reaches at least poleMin below it within 1.5 m (its pole; a bright patch on a low sign has no 3 m drop).
  unsigned rejected = 0;
  for (const auto& g : groups) {
    const float c[3] = { g.sum[0] / g.n, g.sum[1] / g.n, g.sum[2] / g.n };
    if (poleMin > 0) {
      const float size = (std::max)(g.hi[0] - g.lo[0], (std::max)(g.hi[1] - g.lo[1], g.hi[2] - g.lo[2]));
      float low = c[1];
      bool covered = false;
      for (const auto& q : all) {
        const float dx = q[0] - c[0], dz = q[2] - c[2], h2 = dx * dx + dz * dz;
        if (h2 < 1.5f * 1.5f) low = (std::min)(low, q[1]);
        if (h2 < 0.6f * 0.6f && q[1] > g.hi[1] + 0.5f) covered = true;
      }
      if (size > 1.5f || covered || c[1] - low < poleMin) { ++rejected; continue; }
      out.push_back({ c[0], g.lo[1], c[2] });          // the bulb sits at the bottom of the glass
      continue;
    }
    out.push_back({ c[0], anchor == 0 ? g.hi[1] : anchor == 2 ? g.lo[1] : c[1], c[2] });   // anchor: top / centre / bottom of the group
  }
  static unsigned s_logged = 0;                        // generated kinds: hundreds of textures, log the finds
  if (poleMin <= 0 || !out.empty() || ++s_logged <= 20)
    clog(L"world lights: texture %016llX: %zu lamp(s) in a draw of %u vertices (%d rect(s), first %.3f %.3f - %.3f %.3f)%s",
         tex, out.size(), g_drawNumVert, nRects, rects[0][0], rects[0][1], rects[0][2], rects[0][3],
         poleMin > 0 ? (L", pole test: " + std::to_wstring(rejected) + L" group(s) rejected").c_str() : L"");
  return &out;
}

// ---- lamp marker (key lamp_mark, Alt+PageUp by default; user 2026-11-01: the automatic detector found no street lamps)
// Aim the screen centre at a lamp head and press the key: the next primary frame casts a ray from the camera through the
// screen centre against the triangles of every world draw (vertex shader, stream-0 position + TEXCOORD0, triangle
// list/strip/fan, not the car). The nearest hit gives the texture and the island around it (triangles connected to the
// hit one, vertices welded at the same place, all within 2.5 m of the hit point); the island's UV rectangle (the atlas
// cell wrapped as uv_lamp_points does) is written to lights-marked.ini and the lamp kinds are re-read at once. Every
// lamp drawn with that rectangle of the texture, on every stage that shares it, then gets a light.
struct LampPick {
  bool armed = false, ray = false, hit = false, lastCam = false;   // lastCam: the ray comes from the camera of the frame before
  double org[3] = {}, dir[3] = {};
  double t = 1e30;                                     // distance of the nearest hit
  float point[3] = {}, rect[4] = {};
  unsigned long long tex = 0;
  unsigned island = 0, drawVerts = 0;                  // island vertices; vertices of the hit draw
  unsigned draws = 0, tested = 0, unreadable = 0, retries = 0;
  std::wstring shader;
  // The F3 texture picker (g_tprobe) shares this machinery: no island, and it also keeps t1's hash and both textures
  // (AddRef'd, for the menu thumbnails) of the nearest hit.
  bool probe = false;
  unsigned long long tex1 = 0;
  unsigned flags = 0;
  std::string kind;                                    // what kind of draw the nearest hit is (F3 picker)
  bool skipFoliage = false, foliage = false;           // skipFoliage: swaying / alpha-tested cards are not hit (lamp marking); foliage: the nearest hit is one
  IDirect3DBaseTexture9* keep[2] = {};
} g_pick;

struct PickDraw {                                      // one draw's vertices in world space
  std::vector<std::array<float, 3>> pos;
  std::vector<std::array<float, 2>> uv;
  std::vector<uint32_t> tri;                           // 3 vertex ids per triangle
};

float half_float(uint16_t h) {                         // IEEE half -> float (no denormals needed for UVs)
  const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF, sgn = (h & 0x8000u) << 16;
  const uint32_t f = e == 0 ? sgn : sgn | ((e + 112) << 23) | (m << 13);
  float r; std::memcpy(&r, &f, 4); return r;
}

// The hit triangle's island: triangles reachable through shared (welded) vertices, every vertex within 2.5 m of the hit
// point. rect = its UV bounding box, padded, in the atlas cell of the hit triangle. Returns the vertex count.
unsigned lamp_island(const PickDraw& d, size_t hitTri, const double hp[3], float rect[4]) {
  const size_t nv = d.pos.size(), nt = d.tri.size() / 3;
  std::unordered_map<uint64_t, uint32_t> weld;         // vertices at the same place (UV seams, hard normals duplicate them)
  std::vector<uint32_t> w(nv);
  for (size_t i = 0; i < nv; ++i) {
    uint64_t k = 1469598103934665603ull;
    for (int a = 0; a < 3; ++a) { k ^= static_cast<uint64_t>(static_cast<int64_t>(std::llround(d.pos[i][a] * 1000.0))); k *= 1099511628211ull; }
    w[i] = weld.emplace(k, static_cast<uint32_t>(weld.size())).first->second;
  }
  std::vector<std::vector<uint32_t>> adj(weld.size());
  for (size_t t = 0; t < nt; ++t) for (int k = 0; k < 3; ++k) adj[w[d.tri[t * 3 + k]]].push_back(static_cast<uint32_t>(t));
  auto near_hit = [&](uint32_t v) {
    const double dx = d.pos[v][0] - hp[0], dy = d.pos[v][1] - hp[1], dz = d.pos[v][2] - hp[2];
    return dx * dx + dy * dy + dz * dz <= 2.5 * 2.5;
  };
  std::vector<char> seen(nt, 0), used(nv, 0);
  std::vector<size_t> queue{ hitTri };
  seen[hitTri] = 1;
  for (size_t qi = 0; qi < queue.size(); ++qi)
    for (int k = 0; k < 3; ++k) {
      const uint32_t v = d.tri[queue[qi] * 3 + k];
      used[v] = 1;
      for (uint32_t t2 : adj[w[v]]) {
        if (seen[t2] || !near_hit(d.tri[t2 * 3]) || !near_hit(d.tri[t2 * 3 + 1]) || !near_hit(d.tri[t2 * 3 + 2])) continue;
        seen[t2] = 1;
        queue.push_back(t2);
      }
    }
  float cu = 0, cv = 0;                                // the hit triangle's UV centre picks the atlas cell
  for (int k = 0; k < 3; ++k) { cu += d.uv[d.tri[hitTri * 3 + k]][0] / 3; cv += d.uv[d.tri[hitTri * 3 + k]][1] / 3; }
  const float bu = cu - std::floor(cu), bv = cv - std::floor(cv);
  float lo[2] = { 2.0f, 2.0f }, hi[2] = { -1.0f, -1.0f };
  unsigned count = 0;
  for (size_t i = 0; i < nv; ++i) {
    if (!used[i]) continue;
    ++count;
    float du = d.uv[i][0] - cu, dv = d.uv[i][1] - cv;
    du -= std::round(du);                              // the tiling copy nearest the hit triangle
    dv -= std::round(dv);
    lo[0] = (std::min)(lo[0], bu + du); hi[0] = (std::max)(hi[0], bu + du);
    lo[1] = (std::min)(lo[1], bv + dv); hi[1] = (std::max)(hi[1], bv + dv);
  }
  const float pad = 0.002f;
  rect[0] = std::clamp(lo[0] - pad, 0.0f, 1.0f); rect[1] = std::clamp(lo[1] - pad, 0.0f, 1.0f);
  rect[2] = std::clamp(hi[0] + pad, 0.0f, 1.0f); rect[3] = std::clamp(hi[1] + pad, 0.0f, 1.0f);
  return count;
}

// The screen-centre ray through g_V / g_P into a LampPick.
bool pick_ray_from_camera(LampPick& pk) {
  D3DMATRIX vi;
  if (!inverse(g_V, vi) || std::fabs(g_P._11) < 1e-6f || std::fabs(g_P._22) < 1e-6f) return false;
  const double vx = -g_P._31 / g_P._11, vy = -g_P._32 / g_P._22;
  double len = 0;
  for (int k = 0; k < 3; ++k) {
    pk.org[k] = vi.m[3][k];
    pk.dir[k] = vx * vi.m[0][k] + vy * vi.m[1][k] + vi.m[2][k];
    len += pk.dir[k] * pk.dir[k];
  }
  len = std::sqrt(len);
  for (double& x : pk.dir) x /= len;
  pk.ray = true;
  return true;
}

// Fixed-function vertex layout (plain XYZ + optional normal / point size / colours + first texture set as 2D) for draws
// that have no declaration to read: offsets of POSITION and TEXCOORD0, -1 = absent.
void fvf_layout(DWORD fvf, int* posOff, int* uvOff, int* uvType) {
  if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) return;
  *posOff = 0;
  int off = 12;
  if (fvf & D3DFVF_NORMAL) off += 12;
  if (fvf & D3DFVF_PSIZE) off += 4;
  if (fvf & D3DFVF_DIFFUSE) off += 4;
  if (fvf & D3DFVF_SPECULAR) off += 4;
  if (((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) >= 1 && (fvf & (3u << 16)) == 0) { *uvOff = off; *uvType = D3DDECLTYPE_FLOAT2; }
}

// What the draw is, for the F3 picker: the shader class (flags) or "fixed-function".
const char* pick_kind(bool ff, unsigned flags) {
  if (ff) return "fixed-function world draw (game's own transform)";
  if (flags & SH_CAR) return "car";
  if (flags & SH_GLASS) return "glass";
  if (flags & SH_SWAY) return "swaying foliage (vertex shader)";
  if (flags & SH_DOUBLE) return "ground / two-texture scenery (RLDouble)";
  if ((flags & SH_SCENERY) && (flags & SH_WORLDSPACE)) return "static scenery (world-space vertex shader)";
  if (flags & SH_SCENERY) return "scenery (object-space vertex shader)";
  return "world draw (vertex shader)";
}

// Foliage cards: swaying vertex shaders (RL*Sway) or any alpha-tested draw that is not the two-texture RLDouble scenery.
bool is_foliage_draw(IDirect3DDevice9* dev) {
  if (g_curInfo && (g_curInfo->flags & SH_SWAY)) return true;
  if (g_curInfo && (g_curInfo->flags & SH_DOUBLE)) return false;
  DWORD at = 0;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  return at != 0;
}

// Per world draw while armed (comp_draw): the ray against this draw's triangles; keeps the nearest hit of the frame.
// The F3 picker (pk.probe) also takes the car, glass and fixed-function world draws (not screen-space ones: HUD, flare).
void pick_draw(IDirect3DDevice9* dev, LampPick& pk) {
  // Without this frame's camera the ray armed from the last stage camera (lamp_mark_frame) still stands: world-space
  // draws carry world coordinates (user 2026-11-02: "no world camera this frame" on a stage).
  if (!pk.armed || !g_primaryRT || !(g_frameCam || pk.ray) || g_drawNumVert == 0) return;
  const bool ffDraw = !g_curVS;
  if (ffDraw) {
    if (screen_space_input(dev)) return;                 // HUD, flare: no world position
  } else {
    if (!g_curInfo) return;
    if (g_curInfo->flags & (SH_SKY | SH_PARTICLE)) return;
    if (!pk.probe && (g_curInfo->flags & (SH_CAR | SH_GLASS))) return;
  }
  if (g_drawType != D3DPT_TRIANGLELIST && g_drawType != D3DPT_TRIANGLESTRIP && g_drawType != D3DPT_TRIANGLEFAN) return;
  const bool foliageDraw = is_foliage_draw(dev);
  if (pk.skipFoliage && foliageDraw) return;             // the lamp behind a tree (user 2026-11-02: the marker hit a tree 46 m away)
  if (g_frameCam && (!pk.ray || pk.lastCam)) {   // the screen-centre ray (P._31/_32: an off-centre projection)
    if (!pick_ray_from_camera(pk)) return;
    pk.lastCam = false;
  }
  const double* org = pk.org;
  const double* dir = pk.dir;
  ++pk.draws;
  float sc[3], sr = 0.0f;
  if (!ffDraw && draw_world_sphere(dev, sc, &sr, true)) {   // cheap first: the bounding sphere must be on the ray
    const double lx = sc[0] - org[0], ly = sc[1] - org[1], lz = sc[2] - org[2], reach = sr + 1.0;
    const double tca = lx * dir[0] + ly * dir[1] + lz * dir[2];
    if (lx * lx + ly * ly + lz * lz - tca * tca > reach * reach || tca + reach < 0 || (pk.hit && tca - reach > pk.t)) return;
  }
  ++pk.tested;
  IDirect3DBaseTexture9* bt = nullptr;
  if (FAILED(dev->lpVtbl->GetTexture(dev, 0, &bt)) || !bt) return;
  const unsigned long long hx = rbrrtx_texture_hash(bt);
  bt->lpVtbl->Release(bt);
  int posOff = -1, uvOff = -1, uvType = -1;
  stream0_layout(dev, &posOff, &uvOff, &uvType);
  if (ffDraw && (posOff < 0 || uvOff < 0)) {           // an FVF draw: the layout comes from the FVF
    DWORD fvf = g_curFVF;
    if (g_vin != kVinFvf) dev->lpVtbl->GetFVF(dev, &fvf);
    posOff = uvOff = uvType = -1;
    fvf_layout(fvf, &posOff, &uvOff, &uvType);
  }
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  if (FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride)) || !vb) { ++pk.unreadable; return; }
  D3DVERTEXBUFFER_DESC desc = {};
  void* data = nullptr;
  const UINT nv = g_drawNumVert, start = offset + g_drawFirstVert * stride, bytes = nv * stride;
  if (posOff < 0 || uvOff < 0 || (uvType != D3DDECLTYPE_FLOAT2 && uvType != D3DDECLTYPE_FLOAT16_2) || stride < 12 ||
      FAILED(vb->lpVtbl->GetDesc(vb, &desc)) || start + bytes > desc.Size ||
      FAILED(vb->lpVtbl->Lock(vb, start, bytes, &data, D3DLOCK_READONLY)) || !data) {
    vb->lpVtbl->Release(vb);
    ++pk.unreadable;
    return;
  }
  const bool world = !ffDraw && (g_curInfo->flags & SH_WORLDSPACE) != 0;
  const D3DMATRIX w = world ? D3DMATRIX{} : ffDraw ? (g_gameSet[0] ? g_game[0] : identity()) : to_f(mul_d(to_d(from_rows(&g_c[0])), g_VPinv));
  PickDraw d;
  d.pos.resize(nv);
  d.uv.resize(nv);
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (UINT i = 0; i < nv; ++i, p += stride) {
    float x[3], uv[2];
    std::memcpy(x, p + posOff, 12);
    if (uvType == D3DDECLTYPE_FLOAT2) std::memcpy(uv, p + uvOff, 8);
    else { uint16_t h[2]; std::memcpy(h, p + uvOff, 4); uv[0] = half_float(h[0]); uv[1] = half_float(h[1]); }
    if (!world) {
      const float q[3] = { x[0], x[1], x[2] };
      for (int k = 0; k < 3; ++k) x[k] = q[0] * w.m[0][k] + q[1] * w.m[1][k] + q[2] * w.m[2][k] + w.m[3][k];
    }
    d.pos[i] = { x[0], x[1], x[2] };
    d.uv[i] = { uv[0], uv[1] };
  }
  vb->lpVtbl->Unlock(vb);
  vb->lpVtbl->Release(vb);
  // The vertex sequence of the draw (indices relative to its first vertex, or 0..n-1), then its triangles.
  std::vector<uint32_t> seq;
  if (g_inDip) {
    IDirect3DIndexBuffer9* ib = nullptr;
    D3DINDEXBUFFER_DESC id = {};
    void* ip = nullptr;
    if (FAILED(dev->lpVtbl->GetIndices(dev, &ib)) || !ib) { ++pk.unreadable; return; }
    const UINT isz = (ib->lpVtbl->GetDesc(ib, &id), id.Format == D3DFMT_INDEX32) ? 4 : 2;
    const UINT nIdx = g_drawType == D3DPT_TRIANGLELIST ? g_dip.count * 3 : g_dip.count + 2;
    if (g_dip.startIdx * isz + nIdx * isz > id.Size || FAILED(ib->lpVtbl->Lock(ib, g_dip.startIdx * isz, nIdx * isz, &ip, D3DLOCK_READONLY)) || !ip) {
      ib->lpVtbl->Release(ib);
      ++pk.unreadable;
      return;
    }
    seq.resize(nIdx);
    for (UINT i = 0; i < nIdx; ++i)
      seq[i] = (isz == 4 ? static_cast<const uint32_t*>(ip)[i] : static_cast<const uint16_t*>(ip)[i]) - g_dip.minIdx;
    ib->lpVtbl->Unlock(ib);
    ib->lpVtbl->Release(ib);
  } else {
    seq.resize(nv);
    for (UINT i = 0; i < nv; ++i) seq[i] = i;
  }
  auto add = [&](uint32_t a, uint32_t b, uint32_t c) {
    if (a < nv && b < nv && c < nv && a != b && b != c && a != c) { d.tri.push_back(a); d.tri.push_back(b); d.tri.push_back(c); }
  };
  if (g_drawType == D3DPT_TRIANGLELIST) for (size_t i = 0; i + 2 < seq.size(); i += 3) add(seq[i], seq[i + 1], seq[i + 2]);
  else if (g_drawType == D3DPT_TRIANGLESTRIP) for (size_t i = 0; i + 2 < seq.size(); ++i) add(seq[i], seq[i + 1], seq[i + 2]);
  else for (size_t i = 1; i + 1 < seq.size(); ++i) add(seq[0], seq[i], seq[i + 1]);
  // Moller-Trumbore, both sides (the glass of a lamp faces down, the winding does not matter).
  double best = pk.t;
  size_t bestTri = SIZE_MAX;
  for (size_t t = 0; t < d.tri.size() / 3; ++t) {
    const auto& A = d.pos[d.tri[t * 3]];
    const auto& B = d.pos[d.tri[t * 3 + 1]];
    const auto& C = d.pos[d.tri[t * 3 + 2]];
    const double e1[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, e2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
    const double pv[3] = { dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0] };
    const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::fabs(det) < 1e-12) continue;
    const double s[3] = { org[0] - A[0], org[1] - A[1], org[2] - A[2] };
    const double u = (s[0] * pv[0] + s[1] * pv[1] + s[2] * pv[2]) / det;
    if (u < 0.0 || u > 1.0) continue;
    const double qv[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
    const double v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) / det;
    if (v < 0.0 || u + v > 1.0) continue;
    const double tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
    if (tt > 0.05 && tt < best) { best = tt; bestTri = t; }
  }
  if (bestTri == SIZE_MAX) return;
  pk.hit = true;
  pk.t = best;
  pk.tex = hx;
  pk.drawVerts = nv;
  pk.shader = ffDraw ? L"(fixed-function)" : g_curInfo->name;
  double hp[3];
  for (int k = 0; k < 3; ++k) { hp[k] = org[k] + dir[k] * best; pk.point[k] = static_cast<float>(hp[k]); }
  pk.foliage = foliageDraw;
  if (pk.probe) {                                      // F3 texture picker: t1, flags, and the textures kept for thumbnails
    pk.flags = ffDraw ? 0 : g_curInfo->flags;
    pk.kind = std::string(pick_kind(ffDraw, pk.flags)) + (foliageDraw ? ", foliage card" : "");
    pk.tex1 = 0;
    for (int s = 0; s < 2; ++s) {
      if (pk.keep[s]) { pk.keep[s]->lpVtbl->Release(pk.keep[s]); pk.keep[s] = nullptr; }
      IDirect3DBaseTexture9* b = nullptr;
      if (FAILED(dev->lpVtbl->GetTexture(dev, s, &b)) || !b) continue;
      if (s == 1) pk.tex1 = rbrrtx_texture_hash(b);
      if (b->lpVtbl->GetType(b) == D3DRTYPE_TEXTURE) pk.keep[s] = b; else b->lpVtbl->Release(b);
    }
  }
  pk.island = lamp_island(d, bestTri, hp, pk.rect);
}

void lamp_pick_draw(IDirect3DDevice9* dev) {
  if (g_pick.armed) pick_draw(dev, g_pick);
}

// Writes the mark into lights-marked.ini (plain text, CRLF): a new [lamp.mark_<hash>_<n>], or, when the rectangle
// overlaps one already marked for the texture, that section's rectangle replaced. Returns the section name.
std::string lamp_mark_write(const wchar_t* path, unsigned long long tex, const float rect[4], const std::string& note, float glow, bool* replaced) {
  std::string text;
  { std::ifstream in(path, std::ios::binary); if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); }
  std::vector<std::string> lines;
  for (size_t a = 0; a < text.size();) {
    size_t b = text.find('\n', a);
    if (b == std::string::npos) b = text.size();
    std::string l = text.substr(a, b - a);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
    a = b + 1;
  }
  if (lines.empty()) {
    lines = { "; Lamps marked in game with the lamp_mark key (Alt+PageUp by default): aim the screen centre at a lamp head and press it.",
              "; One section per lamp type: the texture and the rectangle of it that the lamp head uses. Every lamp drawn with",
              "; that part of the texture, on every stage, gets a light (worldlights.cpp; lumens, colour and size from lights.ini",
              "; [auto]; lights show at dusk/night). Delete a section to remove it. Read at game start and right after a mark.",
              "; glow = emissive intensity of that part of the texture while the lamps are on (0 = no glow; for a closed lamp the glass",
              "; glows, set from lights.ini [lights] mark_glow; the first night after a change the mod reloads once). Optional per",
              "; section: offset = x, y, z (default 0, -0.1, 0: the light sits just below the glass), lumens, colour_temperature, radius.",
              "" };
  }
  char hex[24];
  snprintf(hex, sizeof(hex), "%016llX", tex);
  const std::string prefix = std::string("[lamp.mark_") + hex + "_";
  auto overlaps = [](const float a[4], const float b[4]) {
    const float ww = (std::min)(a[2], b[2]) - (std::max)(a[0], b[0]), hh = (std::min)(a[3], b[3]) - (std::max)(a[1], b[1]);
    if (ww <= 0 || hh <= 0) return false;
    return ww * hh > 0.25f * (std::min)((a[2] - a[0]) * (a[3] - a[1]), (b[2] - b[0]) * (b[3] - b[1]));
  };
  int maxN = 0, sameN = 0;
  size_t sameStart = 0, sameEnd = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].compare(0, prefix.size(), prefix) != 0) continue;
    const int n = std::atoi(lines[i].c_str() + prefix.size());
    maxN = (std::max)(maxN, n);
    size_t end = i + 1;
    float r[4] = {};
    bool haveUv = false;
    for (; end < lines.size() && !lines[end].empty() && lines[end][0] != '[' && lines[end][0] != ';'; ++end)
      if (lines[end].compare(0, 2, "uv") == 0 && lines[end].find('=') != std::string::npos)
        haveUv = sscanf_s(lines[end].c_str() + lines[end].find('=') + 1, "%f , %f , %f , %f", &r[0], &r[1], &r[2], &r[3]) == 4;
    if (!sameN && haveUv && overlaps(rect, r)) {
      sameN = n;
      sameEnd = end;
      sameStart = i;
      while (sameStart > 0 && !lines[sameStart - 1].empty() && lines[sameStart - 1][0] == ';') --sameStart;   // its comment
    }
  }
  const int n = sameN ? sameN : maxN + 1;
  char name[64], uv[96], gl[48];
  snprintf(name, sizeof(name), "lamp.mark_%s_%d", hex, n);
  snprintf(uv, sizeof(uv), "uv = %.4f, %.4f, %.4f, %.4f", rect[0], rect[1], rect[2], rect[3]);
  snprintf(gl, sizeof(gl), "glow = %.1f", glow);
  const std::vector<std::string> block = { "; " + note, std::string("[") + name + "]", std::string("texture = ") + hex, uv, "anchor = bottom", gl };
  if (sameN) {
    lines.erase(lines.begin() + sameStart, lines.begin() + sameEnd);
    lines.insert(lines.begin() + sameStart, block.begin(), block.end());
  } else {
    if (!lines.back().empty()) lines.push_back("");
    lines.insert(lines.end(), block.begin(), block.end());
    lines.push_back("");
  }
  std::string out;
  for (const std::string& l : lines) out += l + "\r\n";
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!f) return std::string();
  *replaced = sameN != 0;
  return name;
}

// Audible result of a mark (off the render thread: Beep blocks): success = two short rising tones, failure = one low long.
void lamp_mark_beep(bool ok) {
  CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
    if (p) { Beep(880, 90); Beep(1320, 130); } else Beep(220, 450);
    return 0;
  }, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(ok ? 1 : 0)), 0, nullptr);
}

// One lamp hit (the key marker's nearest hit, or the F3 picker's): what a mark needs.
struct LampHit {
  unsigned long long tex = 0;
  float rect[4] = {}, point[3] = {};
  double t = 0;
  unsigned island = 0, drawVerts = 0;
  std::wstring shader;
  bool fixedFunction = false;
};

LampHit lamp_hit_of(const LampPick& pk) {
  LampHit h;
  h.tex = pk.tex;
  std::memcpy(h.rect, pk.rect, sizeof(h.rect));
  std::memcpy(h.point, pk.point, sizeof(h.point));
  h.t = pk.t;
  h.island = pk.island;
  h.drawVerts = pk.drawVerts;
  h.shader = pk.shader;
  h.fixedFunction = pk.shader == L"(fixed-function)";
  return h;
}

// Writes the mark for a hit into lights-marked.ini and re-reads the lamp kinds. msg gets the one-line result.
bool lamp_mark_commit(const LampHit& h, std::string* msg) {
  wchar_t path[MAX_PATH * 2];
  rbrrtx_lights_marked_path(path, MAX_PATH * 2, true);
  SYSTEMTIME st;
  GetLocalTime(&st);
  const RbrStageInfo& si = g_atmoStage;
  std::string stage = "menu";
  if (!g_inMenu) {
    stage = "stage " + std::to_string(si.trackId) + " ";
    for (const wchar_t* c = si.name; *c; ++c) stage += *c < 128 && *c >= 32 ? static_cast<char>(*c) : '?';
  }
  char note[320];
  snprintf(note, sizeof(note), "%s, marked %04d-%02d-%02d %02d:%02d, hit at (%.2f %.2f %.2f) %.1f m away, %u island vertices", stage.c_str(),
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, h.point[0], h.point[1], h.point[2], h.t, h.island);
  bool replaced = false;
  const std::string sec = lamp_mark_write(path, h.tex, h.rect, note, rbrrtx_world_lights_mark_glow(), &replaced);
  if (sec.empty()) {
    clog(L"lamp mark: FAILED to write %s", path);
    if (msg) *msg = "could not write lights-marked.ini";
    return false;
  }
  clog(L"lamp mark: %S texture %016llX (shader %s, %u vertices in the draw) hit at (%.2f %.2f %.2f), %.1f m away; island %u vertices, "
       L"uv %.4f, %.4f, %.4f, %.4f -> [%S] %s in %s%s",
       stage.c_str(), h.tex, h.shader.c_str(), h.drawVerts, h.point[0], h.point[1], h.point[2],
       h.t, h.island, h.rect[0], h.rect[1], h.rect[2], h.rect[3], sec.c_str(),
       replaced ? L"replaced" : L"added", path,
       h.fixedFunction ? L" (a fixed-function draw: lights are only placed from vertex-shader draws, this mark will not light)" : L"");
  g_uvLampCache.clear();
  rbrrtx_world_lights_reload();
  if (msg) *msg = std::string(replaced ? "replaced " : "added ") + "[" + sec + "]";
  return true;
}

// After the armed frame (Present): the nearest hit becomes a mark.
void lamp_pick_finish() {
  if (!g_pick.draws && g_pick.retries < 5) { ++g_pick.retries; return; }   // a frame without a single world draw: the next one
  g_pick.armed = false;
  if (!g_pick.ray) {
    clog(L"lamp mark: no world camera this frame and no earlier stage camera (menu or loading, %s), nothing marked", g_inMenu ? L"in a menu" : L"on a stage");
    lamp_mark_beep(false);
    return;
  }
  if (g_pick.lastCam) clog(L"lamp mark: no world camera in the armed frame, ray from the last stage camera (%.1f %.1f %.1f)", g_pick.org[0], g_pick.org[1], g_pick.org[2]);
  if (!g_pick.hit || g_pick.t > 150.0) {
    if (g_pick.hit)
      clog(L"lamp mark: the nearest surface on the screen-centre ray is %.1f m away (limit 150 m; texture %016llX), nothing marked", g_pick.t, g_pick.tex);
    else
      clog(L"lamp mark: nothing hit by the ray through the screen centre (%u world draws, %u tested, %u with unreadable buffers; swaying and alpha-tested foliage is skipped, not the car)",
           g_pick.draws, g_pick.tested, g_pick.unreadable);
    lamp_mark_beep(false);
    return;
  }
  lamp_mark_beep(lamp_mark_commit(lamp_hit_of(g_pick), nullptr));
}

// Marked lamps of lights-marked.ini for the F3 page: section, texture, uv rectangle, note line above it.
struct LampMark { std::string section, note; unsigned long long tex = 0; float rect[4] = {}; };

std::vector<LampMark> lamp_marks_read(std::vector<std::string>* linesOut, std::vector<std::pair<size_t, size_t>>* spans) {
  std::vector<LampMark> out;
  wchar_t path[MAX_PATH * 2];
  rbrrtx_lights_marked_path(path, MAX_PATH * 2, false);
  std::string text;
  { std::ifstream in(path, std::ios::binary); if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); }
  std::vector<std::string> lines;
  for (size_t a = 0; a < text.size();) {
    size_t b = text.find('\n', a);
    if (b == std::string::npos) b = text.size();
    std::string l = text.substr(a, b - a);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
    a = b + 1;
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].compare(0, 11, "[lamp.mark_") != 0) continue;
    LampMark m;
    const size_t close = lines[i].find(']');
    m.section = lines[i].substr(1, close == std::string::npos ? std::string::npos : close - 1);
    size_t end = i + 1;
    for (; end < lines.size() && !lines[end].empty() && lines[end][0] != '[' && lines[end][0] != ';'; ++end) {
      const size_t eq = lines[end].find('=');
      if (eq == std::string::npos) continue;
      if (lines[end].compare(0, 7, "texture") == 0) m.tex = std::strtoull(lines[end].c_str() + eq + 1, nullptr, 16);
      else if (lines[end].compare(0, 2, "uv") == 0) sscanf_s(lines[end].c_str() + eq + 1, "%f , %f , %f , %f", &m.rect[0], &m.rect[1], &m.rect[2], &m.rect[3]);
    }
    size_t start = i;
    while (start > 0 && !lines[start - 1].empty() && lines[start - 1][0] == ';') --start;   // its comment lines
    if (start < i) m.note = lines[i - 1].size() > 2 ? lines[i - 1].substr(2) : std::string();
    out.push_back(m);
    if (spans) spans->push_back({ start, end });
  }
  if (linesOut) *linesOut = std::move(lines);
  return out;
}

bool lamp_mark_remove(int index) {
  std::vector<std::string> lines;
  std::vector<std::pair<size_t, size_t>> spans;
  const std::vector<LampMark> marks = lamp_marks_read(&lines, &spans);
  if (index < 0 || index >= static_cast<int>(marks.size())) return false;
  const auto sp = spans[index];
  lines.erase(lines.begin() + sp.first, lines.begin() + sp.second);
  if (sp.first < lines.size() && lines[sp.first].empty() && sp.first > 0 && lines[sp.first - 1].empty()) lines.erase(lines.begin() + sp.first);   // the blank line it leaves
  std::string out;
  for (const std::string& l : lines) out += l + "\r\n";
  wchar_t path[MAX_PATH * 2];
  rbrrtx_lights_marked_path(path, MAX_PATH * 2, false);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!f) return false;
  clog(L"lamp mark: [%S] removed from lights-marked.ini (menu)", marks[index].section.c_str());
  g_uvLampCache.clear();
  rbrrtx_world_lights_reload();
  return true;
}

// Per Present: finishes an armed pick, then the key arms the next frame's.
void lamp_mark_frame() {
  if (g_pick.armed) lamp_pick_finish();
  if (rbrrtx_key_pressed(RBRKEY_LAMP_MARK)) {
    g_pick = LampPick();
    g_pick.armed = true;
    g_pick.skipFoliage = true;                         // a lamp is never a tree: the ray goes through swaying / alpha-tested cards
    if (g_haveCam && !g_inMenu && pick_ray_from_camera(g_pick)) g_pick.lastCam = true;   // the last valid stage camera, kept as the fallback
  }
}

// Lamp textures (worldlights.cpp): the lamp sits on this draw's bounding sphere.
void world_light_place(IDirect3DDevice9* dev, unsigned long long hx) {
  if (!g_curVS) {                                          // fixed-function (e.g. screen-space sprites): no world place
    static std::unordered_set<unsigned long long> told;
    if (told.insert(hx).second)
      clog(L"world lights: texture %016llX is drawn fixed-function%s: no lamp placed from it", hx,
           screen_space_input(dev) ? L" in screen space" : L"");
    return;
  }
  float rects[8][4], poleMin = 0.0f;
  int anchor = 1;
  if (const int nRects = rbrrtx_world_lights_uvs(hx, rects, 8, &poleMin, &anchor)) {
    if (!g_curInfo || g_drawNumVert == 0) return;
    const auto* pts = uv_lamp_points(dev, rects, nRects, poleMin, anchor, hx);
    if (!pts) return;
    const bool world = (g_curInfo->flags & SH_WORLDSPACE) != 0;
    const D3DMATRIX w = world ? D3DMATRIX{} : to_f(mul_d(to_d(from_rows(&g_c[0])), g_VPinv));
    for (const auto& q : *pts) {
      float c[3] = { q[0], q[1], q[2] };
      if (!world)
        for (int k = 0; k < 3; ++k) c[k] = q[0] * w.m[0][k] + q[1] * w.m[1][k] + q[2] * w.m[2][k] + w.m[3][k];
      rbrrtx_world_lights_seen(hx, c, 0.0f, g_inMenu);
    }
    return;
  }
  if (poleMin > 0) return;                                  // generated kinds place lamps from rectangles only
  float c[3], r = 0.0f;
  if (draw_world_sphere(dev, c, &r, true)) rbrrtx_world_lights_seen(hx, c, r, g_inMenu);
}

// Returns the twin texture to draw with (a marked lamp whose glass glows while the lamps are on), else nullptr.
void* world_light_draw(IDirect3DDevice9* dev) {
  if (!g_primaryRT || !g_frameCam || !rbrrtx_world_lights_wanted()) return nullptr;
  IDirect3DBaseTexture9* bt = nullptr;
  if (FAILED(dev->lpVtbl->GetTexture(dev, 0, &bt)) || !bt) return nullptr;
  const unsigned long long hx = rbrrtx_texture_hash(bt);
  const bool lamp = rbrrtx_world_lights_texture(hx);
  void* glow = lamp && g_curVS ? rbrrtx_world_lights_glow(dev, hx, bt, g_inMenu) : nullptr;
  bt->lpVtbl->Release(bt);
  if (lamp) world_light_place(dev, hx);
  return glow;
}

bool cull_draw(IDirect3DDevice9* dev) {
  if (!g_primaryRT || !g_enabled || !g_frameCam || !g_curVS || g_drawNumVert == 0 || !cull_class()) return false;
  float c[3], r = 0.0f;
  if (!draw_world_sphere(dev, c, &r)) return false;
  CullEntry* e = cull_entry(dev);                          // cached: the same entry, for its culled flag
  float ref[3];
  if (g_haveCar) { ref[0] = g_carPos[0]; ref[1] = g_carPos[1]; ref[2] = g_carPos[2]; }
  else {                                                     // no car seen yet: the camera
    D3DMATRIX vinv;
    if (!inverse(g_V, vinv)) return false;
    ref[0] = vinv._41; ref[1] = vinv._42; ref[2] = vinv._43;
  }
  const float dx = c[0] - ref[0], dy = c[1] - ref[1], dz = c[2] - ref[2];
  const float dist = (std::max)(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) - r);   // to the sphere's surface
  ++g_cs.considered;
  ++g_cs.dist[dist < 50 ? 0 : dist < 100 ? 1 : dist < 200 ? 2 : dist < 400 ? 3 : 4];
  if (dist < g_cull.keepRadius) { ++g_cs.kept; e->culled = false; return false; }
  const float back = 1.0f - g_cull.hysteresis;              // a culled range returns only below threshold * back
  const bool offscreen = outside_view(c, r + g_cull.frustumMargin);
  bool cull = false;
  if (offscreen && g_cull.offscreenFar > 0) cull = dist > g_cull.offscreenFar * (e->culled ? back : 1.0f);
  if (!offscreen && g_cull.visibleFar > 0 && cull_visible_class()) {
    // On-screen size: sphere diameter as % of the screen height = 100 * r / dist * P._22 (P._22 = 1 / tan(fovY / 2)).
    const float pct = 100.0f * r / (std::max)(dist, 1.0f) * std::fabs(g_P._22);
    const bool tiny = g_cull.visibleMaxPct <= 0 || pct < g_cull.visibleMaxPct * (e->culled ? 1.0f / back : 1.0f);
    cull = tiny && dist > g_cull.visibleFar * (e->culled ? back : 1.0f);
    ++g_cs.size[pct < 0.5f ? 0 : pct < 1 ? 1 : pct < 2 ? 2 : pct < 5 ? 3 : 4];
  }
  if (!g_cull.enabled) return false;
  e->culled = cull;
  if (cull) { if (offscreen) ++g_cs.offscreen; else ++g_cs.visibleFar; }
  return cull;
}

// Mode 2 of the fixed-function scenery: a swaying RL* draw (world space) goes FF only when its bounding sphere is
// farther than g_ffSwayNear from the camera (10 % hysteresis so a batch at the edge does not flip every frame).
// Unreadable bounds = keep the sway.
bool ff_sway_far(IDirect3DDevice9* dev) {
  if (g_drawNumVert == 0) return false;
  CullEntry* e = cull_entry(dev);
  if (!e) return false;
  D3DMATRIX vinv;
  if (!inverse(g_V, vinv)) return false;
  const float dx = e->c[0] - vinv._41, dy = e->c[1] - vinv._42, dz = e->c[2] - vinv._43;
  const float dist = (std::max)(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) - e->r);
  e->ffFar = dist > g_ffSwayNear * (e->ffFar ? 0.9f : 1.0f);
  return e->ffFar;
}

// ---- stage sweep helpers
std::wstring sweep_log_dir() {
  wchar_t dir[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"RBRRTX_LOG_DIR", dir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return dir;
  wchar_t cfg[MAX_PATH * 2] = {};                            // installed add-on: <game>\rbr-rtx\logs
  rbrrtx_config_path(L"RBRRTX_SWEEP_LOGS_UNSET", L"x", cfg, MAX_PATH * 2);
  fs::path p = fs::path(cfg).parent_path().parent_path() / L"logs";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p.wstring();
}

std::wstring capture_dir() {
  wchar_t d[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"DXVK_CAPTURE_PATH", d, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return d;
  return (fs::current_path() / L"rtx-remix" / L"captures").wstring();
}

std::vector<std::wstring> list_captures() {
  std::vector<std::wstring> out;
  std::error_code ec;
  for (auto it = fs::directory_iterator(capture_dir(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::wstring ext = it->path().extension().wstring();
    if (!_wcsicmp(ext.c_str(), L".usd") || !_wcsicmp(ext.c_str(), L".usda") || !_wcsicmp(ext.c_str(), L".usdc"))
      out.push_back(it->path().filename().wstring());
  }
  return out;
}

// Posts a key to the game window: the Remix bridge forwards every window message to Remix's ImGui (bridge
// client window.cpp RemixWndProc), which is how Remix sees its capture hotkey (a single key: ImGui reads Ctrl/Shift
// from the server's own keyboard state, which a posted message does not change).
// Real key input (SendInput), only while the game window is in front. A posted WM_KEYDOWN F9 (until 2026-10-17)
// never reached Remix and switched DirectorsCut to its free camera. Pause = user's pick (Numpad 0 opened an RBR plugin menu).
void post_key(UINT msg, WPARAM vk) {
  if (!g_wnd || GetForegroundWindow() != g_wnd) { clog(L"sweep: capture key not sent (game window not in front)"); return; }
  INPUT in = {};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = static_cast<WORD>(vk);
  in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
  in.ki.dwFlags = msg == WM_KEYUP ? KEYEVENTF_KEYUP : 0;
  SendInput(1, &in, sizeof(in));
}

void sweep_write(const std::wstring& name, const std::wstring& text) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, (g_sweepDir + L"\\" + name).c_str(), L"w, ccs=UTF-8") || !f) return;
  fputws(text.c_str(), f);
  fclose(f);
}

std::wstring stage_line(const RbrStageInfo& si) {
  wchar_t s[1024];
  swprintf_s(s, L"stage %d \"%s\" (%S, surface tarmac %d / gravel %d / snow %d %%), car slot %d = %s, sky %s, clouds %s, %s, "
                L"surface %s (age %d), tyres %d, weather %s, game mode %d",
             si.trackId, si.trackId == 41 && g_btbShaders ? L"BTB track via rbr_rx (slot 41, name not read)" : si.name[0] ? si.name : L"?",
             si.country[0] ? si.country : "?", si.tarmac, si.gravel, si.snow, si.carId, car_desc(si.carId).c_str(), rbrrtx_sky_name(si.skyType), rbrrtx_cloud_name(si.clouds), rbrrtx_time_name(si.timeOfDay),
             rbrrtx_wetness_name(si.wetness), si.surfaceAge, si.tyreType,
             si.weatherType == 0 ? L"Good" : si.weatherType == 1 ? L"Random" : si.weatherType == 2 ? L"Bad" : L"?", si.gameMode);
  return s;
}

void sweep_begin() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf_s(name, L"sweep-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  g_sweepDir = (fs::path(sweep_log_dir()) / name).wstring();
  std::error_code ec;
  fs::create_directories(g_sweepDir, ec);
  g_sweepLines.clear();
  RbrStageInfo si;
  rbrrtx_stage_info(&si);
  D3DMATRIX vinv = {};
  const bool cam = g_haveCam && inverse(g_V, vinv);
  wchar_t s[1024];
  std::wstring r = L"RBR-RTX stage sweep " + std::wstring(name) + L"\n" + stage_line(si) + L"\n";
  swprintf_s(s, L"camera (%.1f, %.1f, %.1f) %s, fovY %.1f deg; car (%.1f, %.1f, %.1f)%s\n", vinv._41, vinv._42, vinv._43,
             cam ? L"" : L"(no camera yet)", g_P._22 != 0 ? 2.0f * std::atan(1.0f / std::fabs(g_P._22)) * 57.29578f : 0.0f,
             g_carPos[0], g_carPos[1], g_carPos[2], g_haveCar ? L"" : L" (no car draw seen)");
  r += s;
  const float el = std::asin(std::clamp(-g_sunDir[1], -1.0f, 1.0f)) * 57.2958f, az = std::atan2(g_sunDir[2], g_sunDir[0]) * 57.2958f;
  swprintf_s(s, L"sun: %s, %s, Remix light azimuth %.1f elevation %.1f, game c41 (%.3f, %.3f, %.3f), brightness x%.2f, weather x%.2f\n",
             g_sunOn ? L"on" : L"OFF", g_sunManual ? L"debug sun" : L"game sun", az, el, g_c[41][0], g_c[41][1], g_c[41][2],
             g_sunScale, g_sunWeather);
  r += s;
  rbrrtx_headlights_status(s, 1024);
  r += s;
  rbrrtx_litparts_status(s, 1024);
  r += L"\n" + std::wstring(s);
  rbrrtx_foliage_status(s, 1024);
  r += L"\n" + std::wstring(s);
  r += L"\n";
  swprintf_s(s, L"proxy: comp %s, FF scenery mode %d (sway near %.0f m), foliage mode %d (ALPHAREF %lu), sky mode %d, "
                L"windscreen %s, culling %s, transform mode %d\n",
             g_enabled ? L"ON" : L"off", g_ffMode, g_ffSwayNear, g_foliageMode, g_cutoutRef, g_skyMode,
             g_glassOn ? L"on" : L"skipped", g_cull.enabled ? L"ON" : L"off", g_testMode);
  r += s;
  wchar_t conf[MAX_PATH] = {};
  rbrrtx_config_path(L"DXVK_RTX_CONFIG_FILE", L"rtx.conf", conf, MAX_PATH);
  r += L"Remix config: " + std::wstring(conf) + L"\nRemix captures: " + capture_dir() + L"\n";
  r += L"atmosphere: " + g_atmoLine + L"\n";
  sweep_write(L"stage.txt", r);
  clog(L"sweep: started, output %s; %s", g_sweepDir.c_str(), stage_line(si).c_str());
}

void sweep_end(const wchar_t* why) {
  if (g_sweepTsv) { fclose(g_sweepTsv); g_sweepTsv = nullptr; }
  g_sweepDumpFrames = 0;
  std::wstring r = L"RBR-RTX stage sweep: " + std::wstring(why) + L"\nstep\tframe ms\tfps\tdraws hidden\tscreenshot\tmean brightness\n";
  for (const auto& l : g_sweepLines) r += l + L"\n";
  if (!g_perfRows.empty()) {                                 // perf bisect: what each switched-off suspect saves
    auto rows = g_perfRows;
    std::sort(rows.begin(), rows.end(), [](const PerfRow& a, const PerfRow& b) { return a.ms - a.ref < b.ms - b.ref; });
    r += L"\nPerf bisect, ranked (saving = reference ms - step ms; reference = the last ref step before it):\n"
         L"saving ms\tstep ms\tref ms\tstep\n";
    for (const auto& p : rows) {
      wchar_t line[300];
      swprintf_s(line, L"%+.2f\t%.2f\t%.2f\t%s\n", p.ref - p.ms, p.ms, p.ref, p.name.c_str());
      r += line;
    }
  }
  r += L"\nFiles:stage.txt (stage, weather, camera, sun, proxy modes), draws.tsv (every draw of one frame: shader, fate, "
       L"textures with Remix hashes), NN-*.png (one per step), textures.tsv (unique texture hashes of that frame).\n";
  sweep_write(L"summary.txt", r);
  rbrrtx_report_sweep(g_sweepDir.c_str());
  clog(L"sweep: %s, output %s", why, g_sweepDir.c_str());
  MessageBeep(MB_ICONASTERISK);                              // done signal: the player can move again
}

// One TSV line per draw of the dump frame (called from comp_draw before anything is skipped).
std::unordered_map<unsigned long long, std::wstring> g_sweepTex;   // hash -> "WxH fmt, first shader"
void sweep_tex(IDirect3DDevice9* dev, DWORD stage, wchar_t* hash, wchar_t* desc, size_t n, const ShaderInfo* key) {
  IDirect3DBaseTexture9* bt = nullptr;
  dev->lpVtbl->GetTexture(dev, stage, &bt);
  if (!bt) { wcscpy_s(hash, 20, L"-"); wcscpy_s(desc, n, L"-"); return; }
  const unsigned long long hx = rbrrtx_texture_hash(bt);
  bt->lpVtbl->Release(bt);
  swprintf_s(hash, 20, hx ? L"%016llX" : L"?", hx);
  tex_desc(dev, stage, desc, n);
  if (hx && !g_sweepTex.count(hx)) g_sweepTex[hx] = std::wstring(desc) + L"\t" + fs::path(key->name).filename().wstring();
}

void sweep_dump_draw(IDirect3DDevice9* dev, const wchar_t* fate) {
  if (!g_sweepTsv || ++g_sweepDumpN > 20000) return;
  const ShaderInfo* key = !g_curVS ? &kFixedFunction : g_curInfo ? g_curInfo : &kUnknownShader;
  DWORD blend = 0, src = 0, dst = 0, zw = 0, at = 0, aref = 0, cull = 0;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  dev->lpVtbl->GetRenderState(dev, D3DRS_SRCBLEND, &src);
  dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &aref);
  dev->lpVtbl->GetRenderState(dev, D3DRS_CULLMODE, &cull);
  wchar_t h0[20], h1[20], d0[96], d1[96];
  sweep_tex(dev, 0, h0, d0, 96, key);
  sweep_tex(dev, 1, h1, d1, 96, key);
  float dist = -1.0f, radius = -1.0f;
  if (g_curInfo && (g_curInfo->flags & SH_WORLDSPACE) && g_drawNumVert && g_haveCam) {   // world-space: sphere is in world units
    if (CullEntry* e = cull_entry(dev)) {
      D3DMATRIX vinv = {};
      if (inverse(g_V, vinv)) {
        const float dx = e->c[0] - vinv._41, dy = e->c[1] - vinv._42, dz = e->c[2] - vinv._43;
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        radius = e->r;
      }
    }
  }
  const int ffc = g_curVS ? ff_scenery_class() : 0;
  fwprintf(g_sweepTsv, L"%u\t%s\t%s\t%s\t%u\t%u\t%d\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%.1f\t%.1f\t%s\t%s\t%s\t%s\n",
           g_sweepDumpN, fs::path(key->name).filename().c_str(), g_primaryRT ? L"P" : L"O", fate, g_pendingPrims, g_drawNumVert,
           ffc, at, aref, blend, src, dst, zw, cull, dist, radius, h0, d0, h1, d1);
}

void sweep_dump_start() {
  if (_wfopen_s(&g_sweepTsv, (g_sweepDir + L"\\draws.tsv").c_str(), L"w, ccs=UTF-8") || !g_sweepTsv) { g_sweepTsv = nullptr; return; }
  fwprintf(g_sweepTsv, L"n\tshader\ttarget\tfate\tprims\tverts\tff_class\talphatest\talpharef\tblend\tsrc\tdst\tzwrite\tcull\t"
                       L"dist_m\tradius_m\tt0_hash\tt0\tt1_hash\tt1\n");
  g_sweepDumpN = 0;
  g_sweepTex.clear();
  g_sweepDumpFrames = 1;                                     // the next whole frame (ended in end_frame)
}

void sweep_dump_end() {
  if (!g_sweepTsv) return;
  fclose(g_sweepTsv);
  g_sweepTsv = nullptr;
  std::wstring t = L"hash\ttexture\tfirst shader\n";
  for (const auto& kv : g_sweepTex) {
    wchar_t hx[20];
    swprintf_s(hx, L"%016llX", kv.first);
    t += std::wstring(hx) + L"\t" + kv.second + L"\n";
  }
  sweep_write(L"textures.tsv", t);
  clog(L"sweep: draw list of one frame: %u draws, %zu textures with a Remix hash", g_sweepDumpN, g_sweepTex.size());
}

// Opaque cut-out (g_opaqueAlpha): the alpha test of this (alpha-tested) draw passes every texel of its stage-0 texture.
// Blended draws keep their state (Remix routes them by blend, not by the test).
bool opaque_cutout(IDirect3DDevice9* dev) {
  DWORD blend = 0, func = 0, ref = 0;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  if (blend) return false;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAFUNC, &func);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &ref);
  IDirect3DBaseTexture9* bt = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &bt);
  if (!bt) return false;
  const int minA = rbrrtx_texture_min_alpha(bt);
  const unsigned long long hx = minA >= 0 ? rbrrtx_texture_hash(bt) : 0;
  bt->lpVtbl->Release(bt);
  if (minA < 0) return false;
  const int r = static_cast<int>(ref & 0xFF);
  const bool yes = func == D3DCMP_GREATEREQUAL ? minA >= r : func == D3DCMP_GREATER ? minA > r : func == D3DCMP_ALWAYS;
  static std::unordered_set<unsigned long long> logged;    // each texture once per run (check against the harvest)
  if (yes && logged.insert(hx).second)
    clog(L"opaque cut-out: texture %016llX min alpha %d, ALPHAREF %d (func %lu), shader %s", hx, minA, r, func,
         g_curInfo ? fs::path(g_curInfo->name).filename().c_str() : g_curVS ? L"(unknown VS)" : L"(fixed-function)");
  return yes;
}

// Draws the clipped copy (rbrrtx_alphacut_find) in place of the game's buffers, then gives the game's back.
// No triangles left = every card missed the outline: nothing to draw.
HRESULT alphacut_draw(IDirect3DDevice9* dev, const RbrAlphaCut& ac) {
  if (!ac.tris) return D3D_OK;
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  UINT offset = 0, stride = 0;
  dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride);
  dev->lpVtbl->GetIndices(dev, &ib);
  dev->lpVtbl->SetStreamSource(dev, 0, static_cast<IDirect3DVertexBuffer9*>(ac.vb), 0, ac.stride);
  dev->lpVtbl->SetIndices(dev, static_cast<IDirect3DIndexBuffer9*>(ac.ib));
  const HRESULT hr = o.DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, ac.verts, 0, ac.tris);
  dev->lpVtbl->SetStreamSource(dev, 0, vb, offset, stride);
  dev->lpVtbl->SetIndices(dev, ib);
  if (vb) vb->lpVtbl->Release(vb);
  if (ib) ib->lpVtbl->Release(ib);
  return hr;
}

// Car dirt (user 2026-10-22: dirt has to show; it always renders as a decal in Remix). RBR blends dirt only in the car
// pixel shaders: CarTexture/CarColor lrp(t1.a, c3, t0), CarPaint1 lrp(t2.a * c3.a, c3, t0) (re/kb materials-textures).
// Remix ignores pixel shaders and takes stage 0 alone, so cars were always clean with ray tracing. After such a draw the
// same geometry is drawn again without the pixel shader: stage 0 = the dirt map (alpha = dirt amount), colour = c3
// (texture factor), alpha blended, no depth write. Its texture hash goes into rtx.decalTextures (with rtx.conf's own),
// so Remix lays it on the body instead of tracing a second surface. Render-target dirt maps have no hash: drawn untagged.
// RBRRTX_CAR_DIRT=0 = off. Raster (key 0) turns the layer off, and the game's own shader shows the dirt there.
std::string g_decalBase;                               // rtx.decalTextures from rtx.conf ("" = none)
std::set<unsigned long long> g_dirtHashes;
// F3 menu texture tags (glue below): class 0 terrain, 1 decal, 2 animated water, 3 ignore. g_f3Tag = added in the menu,
// g_f3Off = taken out of a list that rtx.conf or the run-time tags put the hash in.
std::set<unsigned long long> g_f3Tag[4], g_f3Off[4];
void send_decal_list();

std::string conf_string(const char* name) {
  wchar_t path[MAX_PATH];
  rbrrtx_config_path(L"DXVK_RTX_CONFIG_FILE", L"rtx.conf", path, MAX_PATH);
  std::ifstream in(path);
  std::string v;
  for (std::string l; std::getline(in, l);) {
    const size_t k = l.find(name);
    if (k == std::string::npos || l.find_first_not_of(" \t") != k || l.find_first_of(" \t=", k) != k + std::strlen(name)) continue;
    const size_t eq = l.find('=', k);
    if (eq == std::string::npos) continue;
    v = l.substr(l.find_first_not_of(" \t", eq + 1) == std::string::npos ? l.size() : l.find_first_not_of(" \t", eq + 1));
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
  }
  return v;
}

void decal_tag(unsigned long long hx, const wchar_t* what) {
  if (!g_dirtHashes.insert(hx).second) return;
  send_decal_list();                                   // rtx.conf's list + the car dirt maps + the menu's tags
  clog(L"%s: %016llX tagged as decal (%zu decal textures added)", what, hx, g_dirtHashes.size());
}

// ---------------------------------------------------------------- ground blend: runtime terrain tags (user 2026-11-02)
// The Remix terrain baker bakes every draw whose texture hash is in rtx.terrainTextures. The conf holds a catalog (5856
// hashes) but encrypted stages (Whaanga) are missing from it, so the base texture t0 of every RLDouble* draw and, in bake
// mode, the layer texture t1 are added at run time. One SetConfigVariable per batch (>= 90 frames apart), never per frame.
// Textures in rtx.decalTextures stay decals: they are never added, and they are taken out of the catalog part
// (a draw in both lists would be baked). The runtime tags restart at every stage change, so the list stays as small as the
// stage.
struct GroundStats {
  unsigned draws = 0, vsDriven = 0, frames = 0, baseNew = 0, baseRuntime = 0, lowAlpha = 0, layerNew = 0, layerRuntime = 0,
           layerDraws = 0, decalSkipped = 0, ffDraws = 0, stage0Fixed = 0, layerGated = 0, catalogRejected = 0, layerDrawn = 0;
};
GroundStats g_gs;
std::unordered_set<unsigned long long> g_groundBase, g_groundLayer, g_groundDecalSkipped;
std::unordered_set<unsigned long long> g_groundReject;     // rtx.conf catalog hashes this stage drew as non-ground: left out of the sent list
std::vector<unsigned long long> g_terrainConf;              // rtx.conf's own terrain list, in order, decal textures taken out
std::unordered_set<unsigned long long> g_terrainConfSet, g_decalConfSet;
bool g_groundLoaded = false, g_groundDirty = false;
unsigned g_groundFlushFrame = 0, g_groundFlushes = 0;
size_t g_groundSent = 0;
int g_groundKey = -2;                                       // -1 menu, else stage id
wchar_t g_groundName[80] = L"";
ULONGLONG g_groundStart = 0, g_groundPrinted = 0;
unsigned g_groundPrintedTags = 0;
bool g_groundNoted = false;

std::vector<unsigned long long> parse_hashes(const std::string& s) {
  std::vector<unsigned long long> v;
  for (size_t p = 0; (p = s.find("0x", p)) != std::string::npos;) {
    char* e = nullptr;
    const unsigned long long h = std::strtoull(s.c_str() + p, &e, 16);
    const size_t n = static_cast<size_t>(e - (s.c_str() + p));
    if (n > 2) v.push_back(h);
    p += n > 2 ? n : 2;
  }
  return v;
}

void ground_tags_load() {
  g_groundLoaded = true;
  for (unsigned long long h : parse_hashes(conf_string("rtx.decalTextures"))) g_decalConfSet.insert(h);
  size_t stripped = 0;
  for (unsigned long long h : parse_hashes(conf_string("rtx.terrainTextures"))) {
    if (g_decalConfSet.count(h)) { ++stripped; continue; }
    if (g_terrainConfSet.insert(h).second) g_terrainConf.push_back(h);
  }
  if (stripped) g_groundDirty = true;                       // the catalog part is sent once without the decal textures
  clog(L"ground blend: conf terrain list %zu hashes (%zu also in rtx.decalTextures: taken out, they stay decals), decal list %zu; baker %s; second layer mode %s",
       g_terrainConf.size(), stripped, g_decalConfSet.size(), g_bakerOn ? L"ON" : L"off",
       g_layerMode == kLayerBake ? L"bake" : g_layerMode == kLayerDecal ? L"decal" : L"off");
}

bool ground_is_decal(unsigned long long hx) {
  if (g_f3Off[1].count(hx)) return false;               // taken out of the decal list in the F3 menu
  return g_decalConfSet.count(hx) || g_dirtHashes.count(hx) || g_f3Tag[1].count(hx);
}

void ground_tags_flush(bool force) {
  if (!g_groundLoaded || !g_groundDirty || (!force && g_frame - g_groundFlushFrame < 90)) return;
  g_groundDirty = false;
  g_groundFlushFrame = g_frame;
  if (!g_bakerOn && g_f3Tag[0].empty() && g_f3Off[0].empty()) return;   // baker off in the conf: terrain decals from the conf list, no tags
  std::unordered_set<unsigned long long> seen;
  std::string list;
  char b[24];
  auto add = [&](unsigned long long h) {
    if (ground_is_decal(h) || g_f3Off[0].count(h) || !seen.insert(h).second) return;
    sprintf_s(b, "0x%016llX", h);
    if (!list.empty()) list += ", ";
    list += b;
  };
  for (unsigned long long h : g_terrainConf) if (!g_groundReject.count(h)) add(h);   // catalog hashes drawn as walls/roofs/slopes this stage stay out
  if (g_bakerOn) {                                          // (the run-time ground tags are for the baker)
    for (unsigned long long h : g_groundBase) add(h);
    if (g_layerMode == kLayerBake) for (unsigned long long h : g_groundLayer) add(h);
  }
  for (unsigned long long h : g_f3Tag[0]) add(h);           // tagged in the F3 menu
  remix_set("rtx.terrainTextures", list.c_str());
  g_groundSent = seen.size();
  ++g_groundFlushes;
  clog(L"ground blend: rtx.terrainTextures sent: %zu hashes (%zu conf, %zu of them left out as not ground, %zu RLDouble base, %zu second-layer); update %u",
       seen.size(), g_terrainConf.size(), g_groundReject.size(), g_groundBase.size(), g_layerMode == kLayerBake ? g_groundLayer.size() : size_t(0),
       g_groundFlushes);
}

// Ground or not (user 2026-11-02, Zlin: every texture flagged terrain that was black or white): RLDouble* is the game's
// two-texture scenery shader, not only ground. Zlin SS1 draws 79 base textures with it, of which the classifier
// (docs/research/textures-classify-478.csv) calls 12 house walls, 3 roofs, 4 signs, 1 fence and 24 "other" - the run-time
// tag made all of them terrain, and the baker projects terrain from above, so walls and roofs came out black or white.
// A texture becomes a run-time terrain tag only when its geometry is ground: the area share of triangles facing up
// (normal.y > 0.9, under 26 degrees; Y up) is at least 60 % over up to three sampled draws. Catalog textures (conf list)
// are tagged as before. Unreadable buffers count as ground (Whaanga, which was right, stays as it was).
struct GroundGeom { unsigned samples = 0, unreadable = 0; double area = 0, up = 0; int verdict = -1; };   // verdict -1 open, 0 no, 1 yes
std::unordered_map<unsigned long long, GroundGeom> g_groundGeom;
unsigned g_groundGeomFrame = 0, g_groundGeomThisFrame = 0, g_groundNotGround = 0;

bool draw_up_area(IDirect3DDevice9* dev, double* up, double* area) {
  if (!g_curInfo || !(g_curInfo->flags & SH_WORLDSPACE) || g_drawNumVert == 0 || g_drawNumVert > 60000) return false;
  int posOff = -1, uvOff = -1, uvType = -1;
  stream0_layout(dev, &posOff, &uvOff, &uvType);
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  if (posOff < 0 || FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride)) || !vb || stride < 12) return false;
  D3DVERTEXBUFFER_DESC desc = {};
  void* data = nullptr;
  const UINT nv = g_drawNumVert, start = offset + g_drawFirstVert * stride, bytes = nv * stride;
  if (FAILED(vb->lpVtbl->GetDesc(vb, &desc)) || start + bytes > desc.Size || FAILED(vb->lpVtbl->Lock(vb, start, bytes, &data, D3DLOCK_READONLY)) || !data) {
    vb->lpVtbl->Release(vb);
    return false;
  }
  std::vector<std::array<float, 3>> pos(nv);
  const uint8_t* q = static_cast<const uint8_t*>(data);
  for (UINT i = 0; i < nv; ++i, q += stride) std::memcpy(pos[i].data(), q + posOff, 12);
  vb->lpVtbl->Unlock(vb);
  vb->lpVtbl->Release(vb);
  std::vector<uint32_t> seq;
  if (g_inDip) {
    IDirect3DIndexBuffer9* ib = nullptr;
    D3DINDEXBUFFER_DESC id = {};
    void* ip = nullptr;
    if (FAILED(dev->lpVtbl->GetIndices(dev, &ib)) || !ib) return false;
    const UINT isz = (ib->lpVtbl->GetDesc(ib, &id), id.Format == D3DFMT_INDEX32) ? 4 : 2;
    const UINT nIdx = g_drawType == D3DPT_TRIANGLELIST ? g_dip.count * 3 : g_dip.count + 2;
    if (g_dip.startIdx * isz + nIdx * isz > id.Size || FAILED(ib->lpVtbl->Lock(ib, g_dip.startIdx * isz, nIdx * isz, &ip, D3DLOCK_READONLY)) || !ip) {
      ib->lpVtbl->Release(ib);
      return false;
    }
    seq.resize(nIdx);
    for (UINT i = 0; i < nIdx; ++i)
      seq[i] = (isz == 4 ? static_cast<const uint32_t*>(ip)[i] : static_cast<const uint16_t*>(ip)[i]) - g_dip.minIdx;
    ib->lpVtbl->Unlock(ib);
    ib->lpVtbl->Release(ib);
  } else {
    seq.resize(nv);
    for (UINT i = 0; i < nv; ++i) seq[i] = i;
  }
  double a = 0, u = 0;
  auto tri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
    if (i0 >= nv || i1 >= nv || i2 >= nv) return;
    const auto &A = pos[i0], &B = pos[i1], &C = pos[i2];
    const double e1[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, e2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
    const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-9) return;
    a += len;
    if (std::fabs(n[1]) / len > 0.9) u += len;          // up or down: the winding does not matter
  };
  if (g_drawType == D3DPT_TRIANGLELIST) for (size_t i = 0; i + 2 < seq.size(); i += 3) tri(seq[i], seq[i + 1], seq[i + 2]);
  else if (g_drawType == D3DPT_TRIANGLESTRIP) for (size_t i = 0; i + 2 < seq.size(); ++i) tri(seq[i], seq[i + 1], seq[i + 2]);
  else for (size_t i = 1; i + 1 < seq.size(); ++i) tri(seq[0], seq[i], seq[i + 1]);
  if (a <= 0) return false;
  *up = u;
  *area = a;
  return true;
}

// 1 = the texture counts as ground, 0 = not (walls, roofs, signs, steep slopes), -1 = still open. At most 4 geometry reads
// per frame; an open verdict waits.
int ground_verdict(IDirect3DDevice9* dev, unsigned long long hx) {
  GroundGeom& g = g_groundGeom[hx];
  if (g.verdict >= 0) return g.verdict;
  if (g_groundGeomFrame != g_frame) { g_groundGeomFrame = g_frame; g_groundGeomThisFrame = 0; }
  if (g_groundGeomThisFrame >= 4) return -1;
  ++g_groundGeomThisFrame;
  double up = 0, area = 0;
  if (draw_up_area(dev, &up, &area)) { g.up += up; g.area += area; ++g.samples; }
  else if (++g.unreadable >= 3 && !g.samples) g.verdict = 1;                      // never readable: as before
  if (g.samples >= 3 || (g.samples && g.unreadable >= 3)) {
    g.verdict = g.up >= 0.6 * g.area ? 1 : 0;
    if (!g.verdict) {
      ++g_groundNotGround;
      clog(L"ground blend: %016llX is not ground (%.0f %% of its triangle area faces up, %u draws sampled): no terrain tag%s", hx,
           100.0 * g.up / g.area, g.samples, g_terrainConfSet.count(hx) ? L", taken out of the rtx.conf terrain catalog list" : L"");
    }
  }
  return g.verdict;
}

// An RLDouble* draw (fixed-function or not): its base texture t0 becomes terrain when it is ground. The verdict is read for
// every RLDouble base (also with the baker off: the second layer is drawn only over ground), catalog hashes included
// (user 2026-11-02: the same geometry gate for every terrain hash that is sent).
void ground_note_draw(IDirect3DDevice9* dev, bool vsDriven) {
  if (!g_groundLoaded) ground_tags_load();
  ++g_gs.draws;
  if (vsDriven) ++g_gs.vsDriven; else ++g_gs.ffDraws;
  IDirect3DBaseTexture9* t0 = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t0);
  if (!t0) return;
  const unsigned long long hx = rbrrtx_texture_hash(t0);
  if (!hx || g_groundBase.count(hx)) { t0->lpVtbl->Release(t0); return; }
  if (ground_is_decal(hx)) {
    t0->lpVtbl->Release(t0);
    if (g_bakerOn && g_groundDecalSkipped.insert(hx).second) ++g_gs.decalSkipped;
    return;
  }
  const int verdict = ground_verdict(dev, hx);
  if (verdict == 0 && g_terrainConfSet.count(hx) && g_groundReject.insert(hx).second) {
    ++g_gs.catalogRejected;
    g_groundDirty = true;                                   // the list is sent again without it
  }
  if (verdict != 1 || !g_bakerOn) { t0->lpVtbl->Release(t0); return; }   // walls, roofs, signs: not a bake candidate
  const int minAlpha = rbrrtx_texture_min_alpha(t0);
  t0->lpVtbl->Release(t0);
  g_groundBase.insert(hx);
  ++g_gs.baseNew;
  if (minAlpha >= 0 && minAlpha < 255) ++g_gs.lowAlpha;
  if (!g_terrainConfSet.count(hx)) { ++g_gs.baseRuntime; g_groundDirty = true; }
}

// The second layer (decal twin / bake layer) is only drawn over ground: its base t0 must have the "ground" verdict
// (up-facing triangles). A base the verdict has not reached yet waits; a base not in the table (a decal-listed texture)
// keeps the old behaviour. Facades and roofs got the same twin decal before (user 2026-11-02).
bool ground_layer_allowed(IDirect3DDevice9* dev) {
  IDirect3DBaseTexture9* t0 = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t0);
  if (!t0) return true;
  const unsigned long long hx = rbrrtx_texture_hash(t0);
  t0->lpVtbl->Release(t0);
  const auto it = g_groundGeom.find(hx);
  if (it == g_groundGeom.end() || it->second.verdict == 1) return true;
  if (it->second.verdict == 0) ++g_gs.layerGated;
  return false;
}

// A second-layer draw in bake mode: its texture t1 becomes terrain too (drawn after the base, so the bake blends it in).
void ground_tag_layer(unsigned long long hx) {
  if (!g_groundLoaded) ground_tags_load();
  ++g_gs.layerDraws;
  if (g_groundLayer.count(hx)) return;
  if (ground_is_decal(hx)) {
    if (g_groundDecalSkipped.insert(hx).second) ++g_gs.decalSkipped;
    return;
  }
  g_groundLayer.insert(hx);
  ++g_gs.layerNew;
  if (!g_terrainConfSet.count(hx)) { ++g_gs.layerRuntime; g_groundDirty = true; }
}

// The mode changes (menu, stage change, init): the tags follow at once.
void ground_mode_set(int mode) {
  g_layerMode = g_layerWant = mode;
  g_layerOn = mode != kLayerOff;
  rbrrtx_layer_enable(mode == kLayerDecal);            // decal mode holds texture sources for its twins
  rbrrtx_apiworld_reset(L"ground layer switched");     // RLDouble* instances carry layer A only
  g_groundDirty = true;                                // bake: layer textures in or out of the terrain list
  ground_tags_flush(true);
}

// Alt+Y and the F3 Debug page: in a menu the mode switches at once, on a stage it waits for the next load (twins need the
// texture sources held at upload, and switching the tags under a loaded stage faded the ground to grey).
void ground_layer_request(int want, bool quiet) {
  if (want < kLayerOff || want > kLayerDecal) return;
  if (want == kLayerDecal) rbrrtx_layer_enable(true);  // sources are held from now on: the next stage load gets its twins
  if (g_inMenu) ground_mode_set(want); else g_layerWant = want;
  if (!quiet)
    clog(L"ground blend: second layer mode %s requested (%s)", want == kLayerBake ? L"BAKE" : want == kLayerDecal ? L"DECAL" : L"off",
         g_inMenu ? L"applied in the menu" : L"applies at the next stage load");
}

const wchar_t* ground_tag_name(unsigned long long hx) {
  if (!hx) return L"";
  if (ground_is_decal(hx)) return L" [DECAL-TAGGED]";
  if (g_layerMode == kLayerBake && g_groundLayer.count(hx)) return L" [TERRAIN: runtime, second layer]";
  if (g_groundBase.count(hx)) return g_terrainConfSet.count(hx) ? L" [TERRAIN: catalog, RLDouble base]" : L" [TERRAIN: runtime, RLDouble base]";
  if (g_terrainConfSet.count(hx)) return L" [TERRAIN: catalog]";
  return L"";
}

void ground_stage_line(const wchar_t* when) {
  const unsigned fr = g_gs.frames ? g_gs.frames : 1;
  clog(L"ground blend %s [%s]: second layer %s, baker %s; RLDouble draws %.1f per frame (%.1f stay vertex-shader driven: the baker skips those); "
       L"base textures tagged %u (%u not in the catalog, %u with alpha < 1, %.1f bases per frame drawn without pixel shader, alpha 1); "
       L"layer textures tagged %u (%u not in the catalog), layer draws %.1f per frame; %u of your decal textures left alone; %u base textures judged not ground (walls, roofs, signs: untagged); terrain list %zu hashes, %u updates",
       when, g_groundKey < 0 ? L"menu" : (std::wstring(g_groundName) + L" #" + std::to_wstring(g_groundKey)).c_str(),
       g_layerMode == kLayerBake ? L"BAKE" : g_layerMode == kLayerDecal ? L"DECAL" : L"off", g_bakerOn ? L"ON" : L"off",
       double(g_gs.draws) / fr, double(g_gs.vsDriven) / fr, g_gs.baseNew, g_gs.baseRuntime, g_gs.lowAlpha,
       double(g_groundFFBases) / fr, g_gs.layerNew, g_gs.layerRuntime, double(g_gs.layerDraws) / fr, g_gs.decalSkipped,
       g_groundNotGround, g_groundSent, g_groundFlushes);
  clog(L"ground blend paths [%s], per frame: %.1f RLDouble draws = %.1f fixed-function (vertex shader unbound: Remix reads the fixed-function stage state; %.1f of them had stage 0 on DISABLE, "
       L"the RLDoubleTextureSpecular facades and roads, and got texture x diffuse%s; %.1f ground bases drawn without pixel shader in BAKE mode) + %.1f vertex-shader driven (Remix takes the first textures); "
       L"second layer %s: %.1f drawn per frame, %u skipped as not ground; terrain: %u rtx.conf catalog hashes judged not ground and left out of the list, %u run-time bases tagged; "
       L"facades/roofs/signs take the plain normal-material path (first texture only)",
       when, double(g_gs.draws) / fr, double(g_gs.ffDraws) / fr, double(g_gs.stage0Fixed) / fr, g_stage0Fix ? L"" : L" (switched OFF in the menu: flat colour expected)",
       double(g_groundFFBases) / fr, double(g_gs.vsDriven) / fr,
       g_layerMode == kLayerBake ? L"BAKE" : g_layerMode == kLayerDecal ? L"DECAL" : L"off", double(g_gs.layerDrawn) / fr, g_gs.layerGated,
       g_gs.catalogRejected, g_gs.baseRuntime);
  g_groundPrinted = GetTickCount64();
  g_groundPrintedTags = g_gs.baseNew + g_gs.layerNew;
}

// Once per frame: batched tag update, and the per-stage line (menu counts as a stage): printed at the stage change, 20 s
// into a stage, and again every 2 minutes when tags were added since.
void ground_frame() {
  if (!g_groundLoaded) ground_tags_load();
  ++g_gs.frames;
  ground_tags_flush(false);
  if (g_frame % 30 != 7) return;
  const int key = g_inMenu ? -1 : g_atmoStage.trackId;
  const ULONGLONG now = GetTickCount64();
  if (g_layerWant != g_layerMode && (g_inMenu || key != g_groundKey)) {   // Alt+Y's choice: the menu or a stage change
    clog(L"ground blend: second layer mode %s applied (%s)",
         g_layerWant == kLayerBake ? L"BAKE" : g_layerWant == kLayerDecal ? L"DECAL" : L"off", g_inMenu ? L"in the menu" : L"stage changed");
    ground_mode_set(g_layerWant);
  }
  if (key != g_groundKey) {
    if (g_groundKey != -2 && g_gs.draws) ground_stage_line(L"end");
    if (g_groundKey != -2) {                           // a new stage: its own tags
      g_groundBase.clear();
      g_groundLayer.clear();
      g_groundDecalSkipped.clear();
      g_groundGeom.clear();
      g_groundReject.clear();
      g_groundNotGround = 0;
      g_gs = GroundStats();
      g_groundFFBases = 0;
      g_groundDirty = true;
    }
    g_groundKey = key;
    wcsncpy_s(g_groundName, g_atmoStage.name, _TRUNCATE);
    g_groundStart = g_groundPrinted = now;
    g_groundPrintedTags = 0;
    g_groundNoted = false;
    return;
  }
  if (!g_gs.draws) return;
  if (!g_groundNoted && now - g_groundStart > 20000) { g_groundNoted = true; ground_stage_line(L"so far"); }
  else if (g_groundNoted && now - g_groundPrinted > 120000 && g_gs.baseNew + g_gs.layerNew != g_groundPrintedTags) ground_stage_line(L"so far");
}

// ---------------------------------------------------------------- F3 menu glue (menu/menu.cpp, docs/research/f3-menu-plan.md)
// The menu is Dear ImGui drawn through the game's own device in front of Present (f3_present), fed by a subclass of the
// game window (f3_wndproc). Everything here is the proxy's side: what the menu's pages call back into.

// Texture tags: class 0 terrain, 1 decal, 2 animated water, 3 ignore. The menu toggles a hash in its own set (g_f3Tag), or
// takes one out of a list that rtx.conf or the run-time ground tags put it in (g_f3Off). The lists go to Remix through
// remix_set when something changed (>= 20 frames apart): terrain through ground_tags_flush (its list also holds the
// run-time ground tags), decal through send_decal_list, water and ignore through f3_send_list. menu.ini [textures] keeps
// the sets (<class>= and <class>_off=) and is applied on top of rtx.conf at the start. user.conf is never written.
const char* const kF3Class[4] = { "terrain", "decal", "water", "ignore" };
const char* const kF3Key[4] = { "rtx.terrainTextures", "rtx.decalTextures", "rtx.animatedWaterTextures", "rtx.ignoreTextures" };
std::vector<unsigned long long> g_f3ConfVec[4];            // rtx.conf's own list per class, in order (raw)
std::set<unsigned long long> g_f3Conf[4];
bool g_f3ConfLoaded = false, g_f3Dirty[4] = {};
unsigned g_f3SendFrame = 0;

void f3_conf_load() {
  if (g_f3ConfLoaded) return;
  g_f3ConfLoaded = true;
  for (int c = 0; c < 4; ++c) {
    g_f3ConfVec[c] = parse_hashes(conf_string(kF3Key[c]));
    g_f3Conf[c].insert(g_f3ConfVec[c].begin(), g_f3ConfVec[c].end());
  }
}

// In rtx.conf or in this session's run-time tags (not the menu's own set).
bool f3_base_has(unsigned long long h, int c) {
  if (g_f3Conf[c].count(h)) return true;
  if (c == 0) return g_groundBase.count(h) || (g_layerMode == kLayerBake && g_groundLayer.count(h));
  if (c == 1) return g_dirtHashes.count(h) != 0;
  return false;
}

bool f3_tagged(unsigned long long h, int c) {
  f3_conf_load();
  if (g_f3Off[c].count(h)) return false;
  return g_f3Tag[c].count(h) || f3_base_has(h, c);
}

void f3_untag(unsigned long long h, int c) {
  g_f3Tag[c].erase(h);
  if (f3_base_has(h, c)) g_f3Off[c].insert(h);
  g_f3Dirty[c] = true;
}

// Menu action: toggle the hash in class cls (0..3), or 4 = clear from all four. 0 failed, 1 tagged, 2 removed.
int f3_mark(unsigned long long h, int cls) {
  if (!h || cls < 0 || cls > 4) return 0;
  f3_conf_load();
  if (cls == 4) {
    for (int c = 0; c < 4; ++c) if (f3_tagged(h, c)) f3_untag(h, c);
    clog(L"menu: %016llX cleared from every list", h);
    return 2;
  }
  int result = 2;
  if (f3_tagged(h, cls)) {
    f3_untag(h, cls);
  } else {
    for (int c = 0; c < 4; ++c) if (c != cls && f3_tagged(h, c)) f3_untag(h, c);   // one class per texture
    g_f3Off[cls].erase(h);
    g_f3Tag[cls].insert(h);
    g_f3Dirty[cls] = true;
    result = 1;
  }
  clog(L"menu: %016llX %s %S (menu tags %zu, taken out %zu)", h, result == 1 ? L"tagged" : L"untagged", kF3Class[cls],
       g_f3Tag[cls].size(), g_f3Off[cls].size());
  return result;
}

// The list as Remix gets it: rtx.conf's entries (minus the ones the menu took out), then the menu's.
void f3_send_list(int c) {
  std::set<unsigned long long> seen;
  std::string list;
  char b[24];
  auto add = [&](unsigned long long h) {
    if (g_f3Off[c].count(h) || !seen.insert(h).second) return;
    sprintf_s(b, "0x%016llX", h);
    if (!list.empty()) list += ", ";
    list += b;
  };
  for (unsigned long long h : g_f3ConfVec[c]) add(h);
  for (unsigned long long h : g_f3Tag[c]) add(h);
  remix_set(kF3Key[c], list.c_str());
  clog(L"menu: %S sent: %zu hashes", kF3Key[c], seen.size());
}

void send_decal_list() {
  static bool baseRead = false;
  if (!baseRead) { baseRead = true; g_decalBase = conf_string("rtx.decalTextures"); }
  std::set<unsigned long long> seen;
  std::string list;
  char b[24];
  auto add = [&](unsigned long long h) {
    if (g_f3Off[1].count(h) || !seen.insert(h).second) return;
    sprintf_s(b, "0x%016llX", h);
    if (!list.empty()) list += ", ";
    list += b;
  };
  for (unsigned long long h : parse_hashes(g_decalBase)) add(h);
  for (unsigned long long h : g_dirtHashes) add(h);
  for (unsigned long long h : g_f3Tag[1]) add(h);
  remix_set("rtx.decalTextures", list.c_str());
}

// Per Present: batched list updates (nothing is sent unless the menu changed a tag).
void f3_tags_frame() {
  if (!g_apiOk || g_frame - g_f3SendFrame < 20) return;
  bool any = false;
  for (int c = 0; c < 4; ++c) any |= g_f3Dirty[c];
  if (!any) return;
  g_f3SendFrame = g_frame;
  f3_conf_load();
  for (int c = 0; c < 4; ++c) {
    if (!g_f3Dirty[c]) continue;
    g_f3Dirty[c] = false;
    if (c == 0) {
      if (!g_groundLoaded) ground_tags_load();
      g_groundDirty = true;
      ground_tags_flush(true);
    } else if (c == 1) {
      send_decal_list();
    } else {
      f3_send_list(c);
    }
  }
}

// menu.ini [textures]: terrain= decal= water= ignore= (hashes added by the menu), <class>_off= (taken out of the lists).
void f3_ini_set(const char* key, const char* value) {
  const std::string k = key ? key : "";
  for (int c = 0; c < 4; ++c) {
    const bool off = k == std::string(kF3Class[c]) + "_off";
    if (!off && k != kF3Class[c]) continue;
    for (unsigned long long h : parse_hashes(value ? value : "")) (off ? g_f3Off[c] : g_f3Tag[c]).insert(h);
    g_f3Dirty[c] = true;
    return;
  }
}

void f3_ini_dump(void (*emit)(const char*, void*), void* user) {
  for (int c = 0; c < 4; ++c)
    for (int off = 0; off < 2; ++off) {
      const std::set<unsigned long long>& s = off ? g_f3Off[c] : g_f3Tag[c];
      if (s.empty()) continue;
      std::string line = std::string(kF3Class[c]) + (off ? "_off=" : "=");
      char b[24];
      bool first = true;
      for (unsigned long long h : s) {
        sprintf_s(b, "0x%016llX", h);
        line += (first ? "" : ", ") + std::string(b);
        first = false;
      }
      emit(line.c_str(), user);
    }
}

// "terrain (rtx.conf, menu), decal (removed in menu)" for the Textures page.
void f3_classes_text(unsigned long long h, char* buf, size_t n) {
  f3_conf_load();
  std::string s;
  for (int c = 0; c < 4; ++c) {
    std::string src;
    auto add = [&](const char* w) { src += (src.empty() ? "" : ", ") + std::string(w); };
    if (g_f3Off[c].count(h)) {
      if (f3_base_has(h, c)) add("removed in menu");
    } else {
      if (g_f3Conf[c].count(h)) add("rtx.conf");
      if (c == 0 && g_groundBase.count(h)) add("ground base, run time");
      if (c == 0 && g_layerMode == kLayerBake && g_groundLayer.count(h)) add("ground layer, run time");
      if (c == 1 && g_dirtHashes.count(h)) add("car dirt");
      if (g_f3Tag[c].count(h)) add("menu");
    }
    if (!src.empty()) s += (s.empty() ? "" : "; ") + std::string(kF3Class[c]) + " (" + src + ")";
  }
  strncpy_s(buf, n, s.c_str(), _TRUNCATE);
}

// ---- texture picker (Textures page): the lamp marker's screen-centre ray against the world draws, every frame while the
// page asks for new results (pick_draw with probe set: no island, keeps t1's hash and both textures AddRef'd for thumbnails).
LampPick g_tprobe;
struct TProbeResult {
  bool found = false;
  unsigned long long hash[2] = {};
  IDirect3DBaseTexture9* tex[2] = {};
  std::string shader, kind;
  float dist = 0.0f;
  LampHit lamp;                                        // the hit as a lamp mark would take it (island rectangle in the texture)
  bool foliage = false;
} g_tpRes;
bool g_tpSkipFoliage = false;                          // the page's "skip foliage" box
unsigned g_tpWantUntil = 0;                            // the page asked for new results up to this frame

std::string f3_narrow(const wchar_t* w) {
  std::string s;
  for (; w && *w; ++w) s += *w < 128 && *w >= 32 ? static_cast<char>(*w) : '?';
  return s;
}

void f3_release_tex(IDirect3DBaseTexture9* (&t)[2]) {
  for (IDirect3DBaseTexture9*& x : t)
    if (x) { x->lpVtbl->Release(x); x = nullptr; }
}

void f3_probe_free() {
  f3_release_tex(g_tprobe.keep);
  g_tprobe = LampPick();
  f3_release_tex(g_tpRes.tex);
  g_tpRes = TProbeResult();
}

void texture_probe_draw(IDirect3DDevice9* dev) {
  if (g_tprobe.armed) pick_draw(dev, g_tprobe);
}

// Per Present, before the menu draws: publishes the armed frame's nearest hit and arms the next frame while the page keeps asking.
void f3_probe_frame() {
  const bool want = rbrrtx_menu_is_open() && g_frame < g_tpWantUntil;
  if (g_tprobe.armed) {
    if (!g_tprobe.draws && g_tprobe.retries < 5 && want) { ++g_tprobe.retries; return; }   // a frame without world draws: the next one
    if (want) {
      f3_release_tex(g_tpRes.tex);
      g_tpRes.found = g_tprobe.ray && g_tprobe.hit && g_tprobe.t < 1000.0;
      g_tpRes.hash[0] = g_tprobe.tex;
      g_tpRes.hash[1] = g_tprobe.tex1;
      g_tpRes.shader = f3_narrow(g_tprobe.shader.c_str());
      g_tpRes.kind = g_tprobe.kind;
      g_tpRes.lamp = lamp_hit_of(g_tprobe);
      g_tpRes.foliage = g_tprobe.foliage;
      g_tpRes.dist = static_cast<float>(g_tprobe.t);
      for (int s = 0; s < 2; ++s) { g_tpRes.tex[s] = g_tprobe.keep[s]; g_tprobe.keep[s] = nullptr; }   // ownership moves
    }
    f3_release_tex(g_tprobe.keep);
    g_tprobe = LampPick();
  }
  if (want) {
    g_tprobe.armed = g_tprobe.probe = true;
    g_tprobe.skipFoliage = g_tpSkipFoliage;
    if (g_haveCam && !g_inMenu && pick_ray_from_camera(g_tprobe)) g_tprobe.lastCam = true;   // the last valid stage camera as fallback
  } else if (!rbrrtx_menu_is_open() && (g_tpRes.tex[0] || g_tpRes.tex[1])) {
    f3_release_tex(g_tpRes.tex);                       // menu closed: no reference held on game textures
  }
}

int f3_probe_centre(rbrrtx_menu_pick* out, int wantNew) {
  g_tpWantUntil = wantNew ? g_frame + 3 : 0;
  if (!out) return 0;
  g_tpSkipFoliage = out->skip_foliage != 0;
  *out = rbrrtx_menu_pick();
  if (!g_tpRes.found) return 0;
  out->found = 1;
  out->hash[0] = g_tpRes.hash[0];
  out->hash[1] = g_tpRes.hash[1];
  for (int s = 0; s < 2; ++s) out->texture[s] = g_tpRes.tex[s];
  strncpy_s(out->shader, g_tpRes.shader.c_str(), _TRUNCATE);
  strncpy_s(out->kind, g_tpRes.kind.c_str(), _TRUNCATE);
  out->foliage = g_tpRes.foliage ? 1 : 0;
  out->has_island = g_tpRes.lamp.island > 0 && g_tpRes.lamp.rect[2] > g_tpRes.lamp.rect[0] && g_tpRes.lamp.rect[3] > g_tpRes.lamp.rect[1];
  out->island_verts = g_tpRes.lamp.island;
  std::memcpy(out->uv_rect, g_tpRes.lamp.rect, sizeof(out->uv_rect));
  out->distance = g_tpRes.dist;
  return 1;
}

// ---- lamp marking from the picker (Textures page)
std::vector<LampMark> g_f3Lamps;
bool g_f3LampsLoaded = false;

void f3_lamps_reload() { g_f3Lamps = lamp_marks_read(nullptr, nullptr); g_f3LampsLoaded = true; }

int f3_lamp_mark(char* msg, size_t n) {
  std::string m;
  if (!g_tpRes.found) { strncpy_s(msg, n, "no hit to mark", _TRUNCATE); return 0; }
  if (g_tpRes.foliage) { strncpy_s(msg, n, "that hit is a foliage card, not a lamp", _TRUNCATE); lamp_mark_beep(false); return 0; }
  if (g_tpRes.lamp.island == 0) { strncpy_s(msg, n, "no island found at the hit (the draw has no readable UVs)", _TRUNCATE); return 0; }
  const bool ok = lamp_mark_commit(g_tpRes.lamp, &m);
  f3_lamps_reload();
  strncpy_s(msg, n, m.c_str(), _TRUNCATE);
  return ok ? 1 : 0;
}

int f3_lamp_list(int i, char* label, size_t n) {
  static unsigned loadedFrame = 0;
  if (!g_f3LampsLoaded || (i == 0 && g_frame - loadedFrame >= 30)) { f3_lamps_reload(); loadedFrame = g_frame; }   // the key marker may have added one
  if (i < 0 || i >= static_cast<int>(g_f3Lamps.size())) return 0;
  const LampMark& m = g_f3Lamps[i];
  char b[320];
  snprintf(b, sizeof(b), "%016llX uv %.3f %.3f %.3f %.3f  %s", m.tex, m.rect[0], m.rect[1], m.rect[2], m.rect[3], m.note.c_str());
  strncpy_s(label, n, b, _TRUNCATE);
  return 1;
}

int f3_lamp_remove(int i) {
  const bool ok = lamp_mark_remove(i);
  f3_lamps_reload();
  return ok ? 1 : 0;
}

// ---- Debug page: A/B switches (choice or checkbox), listed here, drawn by menu.cpp. Each change is logged.
struct F3Switch {
  const char* group;
  const char* label;
  const char* options;                                 // "a|b|c" = choice (value = index), "" = checkbox
  const char* note;
  int (*get)();
  void (*set)(int);
};

int sw_ground_get() { return g_layerWant == kLayerBake ? 0 : g_layerWant == kLayerDecal ? 1 : 2; }   // bake | decal | off
void sw_ground_set(int v) { ground_layer_request(v == 0 ? kLayerBake : v == 1 ? kLayerDecal : kLayerOff, false); }
int sw_stage0_get() { return g_stage0Fix ? 1 : 0; }
void sw_stage0_set(int v) { g_stage0Fix = v != 0; clog(L"menu: stage-0 fix for RLDouble facades %s", g_stage0Fix ? L"ON" : L"OFF"); }

int sw_flare_get() { return g_flareSkip ? 1 : 0; }
void sw_flare_set(int v) { g_flareSkip = v != 0; clog(L"menu: lens flare skip %s", g_flareSkip ? L"ON" : L"OFF (the game's flare draws)"); }
int sw_outline_get() { return g_dbgHideOutlineSprite ? 1 : 0; }
void sw_outline_set(int v) { g_dbgHideOutlineSprite = v != 0; clog(L"menu: sun-outline candidate A: 544BB1AB506B2F73 (1024x128 world sprite) %s", g_dbgHideOutlineSprite ? L"HIDDEN" : L"drawn"); }
int sw_clouds_get() { return g_dbgHideClouds ? 1 : 0; }
void sw_clouds_set(int v) { g_dbgHideClouds = v != 0; clog(L"menu: sun-outline candidate B: CloudsSingleTexture (cloud layer, alpha edge) %s", g_dbgHideClouds ? L"HIDDEN" : L"drawn"); }
int sw_billboard_get() { return g_dbgHideSunBillboard ? 1 : 0; }
void sw_billboard_set(int v) { g_dbgHideSunBillboard = v != 0; clog(L"menu: sun-outline candidate C: 40AA313D3EE0E9B6 (128x128 sun glow) %s", g_dbgHideSunBillboard ? L"HIDDEN" : L"drawn"); }

const F3Switch kF3Switches[] = {
  { "Ground", "Second layer (applies at the next stage load)", "bake|decal|off",
    "bake = t1 baked into the terrain (experiment); decal = twin-texture decal over the baked base (default); off = game shader only. In a menu it applies at once.",
    sw_ground_get, sw_ground_set },
  { "Ground", "Facade stage-0 fix (RLDoubleTextureSpecular)", "",
    "ON: scenery drawn without its vertex shader gets texture x diffuse on stage 0 when the game leaves it DISABLE. OFF = the flat-colour facades of the 01:46 run (A/B). Live.",
    sw_stage0_get, sw_stage0_set },
  { "Sun", "Skip the game's lens flare (screen-space additive draws after the sun query)", "",
    "ON (default; materials.ini [skip_draws] lens_flare): no flare rings or sprites. The log lists every texture skipped and any additive HUD draw kept.",
    sw_flare_get, sw_flare_set },
  { "Sun", "Hide 544BB1AB506B2F73 (sun-outline candidate A: 1024x128 world sprite)", "",
    "Debug A/B: look at the sun with it hidden; if the black outline goes, it was this sprite.", sw_outline_get, sw_outline_set },
  { "Sun", "Hide the cloud layer (candidate B: CloudsSingleTexture alpha edge)", "",
    "Debug A/B: if the outline stays with the sprite hidden but goes with the clouds hidden, it is the cloud texture's alpha edge.", sw_clouds_get, sw_clouds_set },
  { "Sun", "Hide 40AA313D3EE0E9B6 (candidate C: 128x128 sun glow)", "",
    "Debug A/B: the sun glow sprite (also covered by the flare skip).", sw_billboard_get, sw_billboard_set },
};

int f3_switch_get(int i, rbrrtx_menu_switch* sw) {
  if (i < 0 || i >= static_cast<int>(_countof(kF3Switches)) || !sw) return 0;
  const F3Switch& d = kF3Switches[i];
  strncpy_s(sw->group, d.group, _TRUNCATE);
  strncpy_s(sw->label, d.label, _TRUNCATE);
  strncpy_s(sw->options, d.options, _TRUNCATE);
  strncpy_s(sw->note, d.note, _TRUNCATE);
  sw->value = d.get();
  return 1;
}

void f3_switch_set(int i, int v) {
  if (i >= 0 && i < static_cast<int>(_countof(kF3Switches))) kF3Switches[i].set(v);
}

// ---- Info page
void f3_info_text(char* buf, size_t n) {
  RbrStageInfo si = {};
  wchar_t w[1024];
  int len = 0;
  if (rbrrtx_stage_info(&si) && !g_inMenu)
    len = swprintf_s(w, L"Stage %d %s\nWeather: %s, %s, %s, track %s\n", si.trackId, si.name, rbrrtx_time_name(si.timeOfDay),
                     rbrrtx_cloud_name(si.clouds), rbrrtx_sky_name(si.skyType), rbrrtx_wetness_name(si.wetness));
  else
    len = swprintf_s(w, L"In a menu (no stage loaded)\n");
  len += swprintf_s(w + len, _countof(w) - len, L"Ground second layer: %s%s\nTerrain baker (rtx.conf): %s\n",
                    g_layerMode == kLayerBake ? L"BAKE" : g_layerMode == kLayerDecal ? L"DECAL" : L"off",
                    g_layerWant != g_layerMode ? (g_layerWant == kLayerBake ? L" (BAKE at the next stage load)" : g_layerWant == kLayerDecal ? L" (DECAL at the next stage load)" : L" (off at the next stage load)") : L"",
                    g_bakerOn ? L"ON" : L"off");
  len += swprintf_s(w + len, _countof(w) - len, L"Remix Plus API: %s, compat layer: %s, wetness %.2f\n", g_apiOk ? L"on" : L"off",
                    g_enabled ? L"on" : L"off", rbrrtx_wetness_now());
  strncpy_s(buf, n, f3_narrow(w).c_str(), _TRUNCATE);
}

int f3_key_line(int i, char* buf, size_t n) {
  int seen = 0;
  for (int a = 0; a < RBRKEY_COUNT; ++a) {
    if (a == RBRKEY_MENU || !rbrrtx_key_vk(a, true)) continue;
    if (seen++ != i) continue;
    strncpy_s(buf, n, (f3_narrow(rbrrtx_key_name(a)) + ":  " + f3_narrow(rbrrtx_key_label(a))).c_str(), _TRUNCATE);
    return 1;
  }
  return 0;
}

void f3_config_dir(wchar_t* buf, size_t n) {
  rbrrtx_config_path(L"RBRRTX_MENU_DIR_UNSET", L"menu.ini", buf, n);   // the config folder; the menu adds menu.ini
  if (wchar_t* slash = wcsrchr(buf, L'\\')) *slash = 0;
}

int f3_remix_set(const char* key, const char* value) {
  if (!g_apiOk || !g_api.SetConfigVariable) return 0;
  remix_set(key, value);
  return 1;
}

// ---- window subclass and device hooks
WNDPROC g_f3NextProc = nullptr;
HWND g_f3Wnd = nullptr;
bool g_f3Unicode = true;

LRESULT CALLBACK f3_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  if (rbrrtx_menu_wndproc(w, m, wp, lp)) return 0;     // taken by the open menu (or the F3 key itself)
  return g_f3Unicode ? CallWindowProcW(g_f3NextProc, w, m, wp, lp) : CallWindowProcA(g_f3NextProc, w, m, wp, lp);
}

// At CreateDevice: callbacks, toggle key, ImGui context and the window subclass. RBRRTX_MENU=0 leaves the menu out.
// The Remix bridge detours SetWindowLong(GWLP_WNDPROC): once it has installed its own proc (RemixWndProc) our subclass
// is stored behind it (it forwards to "the game's proc" = ours), so Remix sees a key first and the Remix panel (Alt+X,
// which swallows input while it is up) is not disturbed. The log line shows which case it was.
void f3_install(IDirect3DDevice9* dev) {
  if (!rbrrtx_env_flag(L"RBRRTX_MENU", true) || !g_wnd) return;
  static rbrrtx_menu_callbacks cb = {};
  cb.remix_set = f3_remix_set;
  cb.probe_centre = f3_probe_centre;
  cb.mark_texture = f3_mark;
  cb.texture_classes = f3_classes_text;
  cb.texture_ini = f3_ini_set;
  cb.texture_ini_dump = f3_ini_dump;
  cb.info_text = f3_info_text;
  cb.key_line = f3_key_line;
  cb.lamp_mark = f3_lamp_mark;
  cb.lamp_list = f3_lamp_list;
  cb.lamp_remove = f3_lamp_remove;
  cb.switch_get = f3_switch_get;
  cb.switch_set = f3_switch_set;
  cb.config_dir = f3_config_dir;
  rbrrtx_menu_set_callbacks(&cb);
  rbrrtx_menu_set_toggle_vk(rbrrtx_key_vk(RBRKEY_MENU, false));
  if (!rbrrtx_menu_init(g_wnd, dev)) { clog(L"menu: init failed, no F3 menu"); return; }
  if (g_f3Wnd != g_wnd) {
    g_f3Unicode = IsWindowUnicode(g_wnd) != FALSE;
    const LONG_PTR before = g_f3Unicode ? GetWindowLongPtrW(g_wnd, GWLP_WNDPROC) : GetWindowLongPtrA(g_wnd, GWLP_WNDPROC);
    const LONG_PTR prev = g_f3Unicode ? SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(f3_wndproc))
                                      : SetWindowLongPtrA(g_wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(f3_wndproc));
    const LONG_PTR now = g_f3Unicode ? GetWindowLongPtrW(g_wnd, GWLP_WNDPROC) : GetWindowLongPtrA(g_wnd, GWLP_WNDPROC);
    if (prev) { g_f3NextProc = reinterpret_cast<WNDPROC>(prev); g_f3Wnd = g_wnd; }
    clog(L"menu: F3 menu %s; window procedure before %p, previous %p, now %p, ours %p (now != ours: something else, probably the Remix bridge, sits in front)",
         prev ? L"installed" : L"NOT installed (SetWindowLong failed)", reinterpret_cast<void*>(before), reinterpret_cast<void*>(prev),
         reinterpret_cast<void*>(now), reinterpret_cast<void*>(f3_wndproc));
  }
  clog(L"menu: toggle key vk 0x%02X", rbrrtx_key_vk(RBRKEY_MENU, false));
  rbrrtx_dinput_block_install();                       // the game gets neutral keyboard / mouse / wheel state while the menu is open
}

// hk_Reset: the DX9 backend's default-pool objects and our texture references must be gone before Reset.
void f3_before_reset() {
  rbrrtx_menu_device_lost();
  f3_probe_free();
}

void f3_after_reset(IDirect3DDevice9* dev, HRESULT hr) {
  if (SUCCEEDED(hr)) rbrrtx_menu_device_reset(dev);
}

// Per Present, after the proxy's own end-of-frame work and before the original Present: pending list updates, the
// texture probe, then the menu in its own scene (BeginScene/EndScene around it, through the original methods so the hooks'
// per-scene bookkeeping does not see it). Only on the primary target; closed = a few loads and compares.
void f3_present(IDirect3DDevice9* dev) {
  rbrrtx_menu_tick();
  f3_tags_frame();
  f3_probe_frame();
  if (!rbrrtx_menu_is_open() || !g_primaryRT) return;
  const bool began = SUCCEEDED(o.BeginScene(dev));
  rbrrtx_menu_frame(dev);
  if (began) dev->lpVtbl->EndScene(dev);
  static bool atlasLogged = false;
  if (!atlasLogged) {
    if (void* atlas = rbrrtx_menu_font_texture()) {    // tag it in rtx.uiTextures only if it ever shows up in the scene
      atlasLogged = true;
      clog(L"menu: ImGui font atlas texture %p, Remix hash %016llX (not tagged; add it to rtx.uiTextures if menu text appears in the path-traced scene)",
           atlas, rbrrtx_texture_hash(atlas));
    }
  }
}

// Stage of the dirt map for this car shader, -1 = no dirt.
int dirt_stage() {
  if (!g_curInfo || !(g_curInfo->flags & SH_CAR)) return -1;
  const std::wstring n = fs::path(g_curInfo->name).filename().wstring();
  if (!_wcsicmp(n.c_str(), L"CarTexture.vso") || !_wcsicmp(n.c_str(), L"CarColor.vso")) return 1;
  if (!_wcsicmp(n.c_str(), L"CarPaint1.vso")) return 2;
  return -1;
}

template <class Draw>
void dirt_draw(IDirect3DDevice9* dev, Draw draw) {
  const int stage = dirt_stage();
  if (stage < 0 || !g_dirtOn || !g_enabled || !g_primaryRT || !g_curVS) return;
  IDirect3DBaseTexture9* dirt = nullptr;
  dev->lpVtbl->GetTexture(dev, stage, &dirt);
  if (!dirt) return;
  const unsigned long long hx = rbrrtx_texture_hash(dirt);
  if (hx) decal_tag(hx, L"car dirt map"); else ++g_dirtUntagged;
  const float* c3 = g_psc[3];
  auto u8 = [](float f) { return static_cast<DWORD>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
  const DWORD factor = ((stage == 2 ? u8(c3[3]) : 255u) << 24) | (u8(c3[0]) << 16) | (u8(c3[1]) << 8) | u8(c3[2]);
  static const D3DRENDERSTATETYPE rs[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
                                           D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_TEXTUREFACTOR };
  static const D3DTEXTURESTAGESTATETYPE ts[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP,
                                                 D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2 };
  DWORD rsOld[7], ts0Old[6], ts1Color = 0, ts1Alpha = 0;
  for (int i = 0; i < 7; ++i) dev->lpVtbl->GetRenderState(dev, rs[i], &rsOld[i]);
  for (int i = 0; i < 6; ++i) dev->lpVtbl->GetTextureStageState(dev, 0, ts[i], &ts0Old[i]);
  dev->lpVtbl->GetTextureStageState(dev, 1, D3DTSS_COLOROP, &ts1Color);
  dev->lpVtbl->GetTextureStageState(dev, 1, D3DTSS_ALPHAOP, &ts1Alpha);
  IDirect3DBaseTexture9* t0 = nullptr;
  IDirect3DPixelShader9* ps = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t0);
  dev->lpVtbl->GetPixelShader(dev, &ps);
  const DWORD rsNew[7] = { TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, FALSE, factor };
  const DWORD tsNew[6] = { D3DTOP_SELECTARG1, D3DTA_TFACTOR, D3DTA_TEXTURE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_TFACTOR };
  for (int i = 0; i < 7; ++i) o.SetRenderState(dev, rs[i], rsNew[i]);
  for (int i = 0; i < 6; ++i) o.SetTextureStageState(dev, 0, ts[i], tsNew[i]);
  o.SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  o.SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->lpVtbl->SetTexture(dev, 0, dirt);
  dev->lpVtbl->SetPixelShader(dev, nullptr);
  draw();
  ++g_dirtDraws;
  dev->lpVtbl->SetPixelShader(dev, ps);
  dev->lpVtbl->SetTexture(dev, 0, t0);
  o.SetTextureStageState(dev, 1, D3DTSS_COLOROP, ts1Color);
  o.SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, ts1Alpha);
  for (int i = 0; i < 6; ++i) o.SetTextureStageState(dev, 0, ts[i], ts0Old[i]);
  for (int i = 0; i < 7; ++i) o.SetRenderState(dev, rs[i], rsOld[i]);
  if (ps) ps->lpVtbl->Release(ps);
  if (t0) t0->lpVtbl->Release(t0);
  dirt->lpVtbl->Release(dirt);
}

// Second ground layer (user 2026-10-22: blend like raster, VERY important; Whaanga Coast Sprint sea showed grass).
// RLDoubleTexture*: lrp(t1.a * v0.a, t1, t0) in the pixel shader (re/kb materials-textures); Remix shows t0 alone. After
// such a draw (fixed-function scenery, VS unbound) the geometry is drawn again with a twin of t1 (harvest.cpp: same
// image, own hash, tagged decal) in stage 0, alpha = texture x vertex alpha, blended, no depth write, and a vertex
// declaration with TEXCOORD0/1 swapped so stage 0 and Remix both read t1's UV set.
// Modes (g_layerMode, Alt+Y cycles off -> decal -> bake; env RBRRTX_GROUND_LAYER = 0 off / 1 bake / 2 decal; default off,
// as before 2026-11-02: baker off in rtx.conf): DECAL is the old twin-texture decal layer (off by default since
// 2026-10-22: black decal holes on Gabiria road markings, KNOWN-ISSUES; it never tags a game texture, only the twin, and
// needs the layer on before the stage loads: the twins are made from sources held at upload). BAKE (EXPERIMENT 2026-11-02,
// tested badly: grey / colourless ground, fading) switches the baker on, tags the base t0 of each RLDouble draw and the
// layer t1 as terrain at run time (ground_note_draw, ground_tag_layer, docs/research/terrain-blend-2026-11-02.md) and
// draws the layer from t1 itself, so the baker composites base then layer in draw order.
std::unordered_map<void*, IDirect3DVertexDeclaration9*> g_uvSwapped;   // game declaration -> TEXCOORD0/1 swapped

IDirect3DVertexDeclaration9* uv_swapped(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl) {
  auto it = g_uvSwapped.find(decl);
  if (it != g_uvSwapped.end()) return it->second;
  D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
  UINT n = MAXD3DDECLLENGTH + 1;
  IDirect3DVertexDeclaration9* out = nullptr;
  bool has1 = false;
  if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, el, &n))) {
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
      if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].UsageIndex <= 1) { has1 |= el[i].UsageIndex == 1; el[i].UsageIndex ^= 1; }
    if (has1) o.CreateVertexDeclaration(dev, el, &out);
  }
  g_uvSwapped[decl] = out;                             // nullptr = no second UV set: no second layer
  return out;
}

// Vertex normals on fixed-function scenery. RLDoubleTextureSpecular* (and other specular layouts) carry a FLOAT3 NORMAL
// the game uses only for its specular term; drawn fixed-function, Remix shades with it. On Tanner's bridge deck those
// normals point sideways: the deck rendered dark and mirror-like (sweeps 2026-10-22 20:35/20:36, shading normals view
// blue, ground green). Without NORMAL Remix uses the triangle normals, as for all other RL ground.
// RBRRTX_FF_NORMALS=1 keeps them.
bool g_ffNormals = false;
std::unordered_map<void*, IDirect3DVertexDeclaration9*> g_noNormal;   // game declaration -> copy without NORMAL

IDirect3DVertexDeclaration9* no_normal(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl) {
  auto it = g_noNormal.find(decl);
  if (it != g_noNormal.end()) return it->second;
  D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1], kept[MAXD3DDECLLENGTH + 1];
  UINT n = MAXD3DDECLLENGTH + 1, k = 0;
  IDirect3DVertexDeclaration9* out = nullptr;
  if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, el, &n))) {
    for (UINT i = 0; i < n; ++i)                       // the D3DDECL_END element is copied too
      if (el[i].Stream == 0xFF || el[i].Usage != D3DDECLUSAGE_NORMAL) kept[k++] = el[i];
    if (k < n) o.CreateVertexDeclaration(dev, kept, &out);
  }
  g_noNormal[decl] = out;                              // nullptr = no NORMAL: draw as is
  return out;
}

// Radial foliage normals (radnormals.cpp): the game declaration without its NORMAL, plus a FLOAT3 NORMAL read from
// stream kRadStream (our buffer, numbered like stream 0). nullptr if the game already uses that stream.
constexpr DWORD kRadStream = 4;
std::unordered_map<void*, IDirect3DVertexDeclaration9*> g_radDecl;

IDirect3DVertexDeclaration9* rad_normal_decl(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl) {
  auto it = g_radDecl.find(decl);
  if (it != g_radDecl.end()) return it->second;
  D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1], kept[MAXD3DDECLLENGTH + 2];
  UINT n = MAXD3DDECLLENGTH + 1, k = 0;
  IDirect3DVertexDeclaration9* out = nullptr;
  bool free = true;
  if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, el, &n))) {
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
      if (el[i].Stream == kRadStream) free = false;
      if (el[i].Usage != D3DDECLUSAGE_NORMAL) kept[k++] = el[i];
    }
    kept[k++] = { static_cast<WORD>(kRadStream), 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 };
    kept[k++] = D3DDECL_END();
    if (free) o.CreateVertexDeclaration(dev, kept, &out);
  }
  g_radDecl[decl] = out;
  return out;
}

template <class Draw>
void layer_draw(IDirect3DDevice9* dev, Draw draw) {
  if (!g_layerOn || !g_enabled || !g_primaryRT || !g_vsUnbound || !g_curInfo || g_vin != kVinDecl ||
      !(g_curInfo->flags & SH_DOUBLE)) return;
  if (!ground_layer_allowed(dev)) return;                // facades, roofs, signs, slopes: no second layer
  IDirect3DBaseTexture9* t1 = nullptr;
  dev->lpVtbl->GetTexture(dev, 1, &t1);
  if (!t1) return;
  // Bake mode: t1 itself is the layer (a texture drawn as a ground layer is ground wherever it is used, so it needs no
  // twin of its own and no held source). Decal mode: the twin, which has a hash of its own.
  const bool bake = g_layerMode == kLayerBake;
  IDirect3DBaseTexture9* twin = bake ? t1 : static_cast<IDirect3DBaseTexture9*>(rbrrtx_layer_twin(t1));
  IDirect3DVertexDeclaration9* decl = nullptr;
  dev->lpVtbl->GetVertexDeclaration(dev, &decl);
  IDirect3DVertexDeclaration9* swapped = decl ? uv_swapped(dev, decl) : nullptr;
  unsigned long long hx = twin && swapped ? rbrrtx_texture_hash(twin) : 0;
  const bool selfHash = !bake && hx && hx == rbrrtx_texture_hash(t1);   // a twin must have a hash of its own
  if (selfHash) {
    static bool warned = false;
    if (!warned) { warned = true; clog(L"ground second layer: twin of %016llX has the game texture's own hash, layer draw skipped (would make the game texture a decal)", hx); }
  }
  if (!twin || !swapped || (bake && !hx) || selfHash) {
    if (!twin || !swapped) ++g_layerMissing; else ++g_layerUntagged;   // bake: an unhashed layer cannot be tagged terrain
    if (decl) decl->lpVtbl->Release(decl);
    t1->lpVtbl->Release(t1);
    return;
  }
  if (bake) ground_tag_layer(hx);
  else if (hx) decal_tag(hx, L"ground second layer");
  else ++g_layerUntagged;
  if (g_dumpFrames) clog(L"  dump layer draw: t1 %016llX second layer, %s", hx, bake ? L"tagged TERRAIN (baked)" : L"twin tagged decal");
  static const D3DRENDERSTATETYPE rs[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
                                           D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE };
  static const D3DTEXTURESTAGESTATETYPE ts[] = { D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX };
  // Bake: the render target's alpha is the bake's opacity. SRCALPHA blending writes a * a + dst.a * (1 - a) into it, so
  // the base's alpha 1 would sink to 0.75 where the layer is half strong: alpha keeps the destination instead.
  static const D3DRENDERSTATETYPE rsA[] = { D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA,
                                            D3DRS_BLENDOPALPHA };
  static const D3DTEXTURESTAGESTATETYPE tsC[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2 };
  DWORD rsOld[6], rsAOld[4] = {}, tsOld[4], tsCOld[3] = {}, ts1Color = 0, ts1Alpha = 0;
  for (int i = 0; i < 4 && bake; ++i) dev->lpVtbl->GetRenderState(dev, rsA[i], &rsAOld[i]);
  for (int i = 0; i < 3 && bake; ++i) dev->lpVtbl->GetTextureStageState(dev, 0, tsC[i], &tsCOld[i]);
  for (int i = 0; i < 6; ++i) dev->lpVtbl->GetRenderState(dev, rs[i], &rsOld[i]);
  for (int i = 0; i < 4; ++i) dev->lpVtbl->GetTextureStageState(dev, 0, ts[i], &tsOld[i]);
  dev->lpVtbl->GetTextureStageState(dev, 1, D3DTSS_COLOROP, &ts1Color);
  dev->lpVtbl->GetTextureStageState(dev, 1, D3DTSS_ALPHAOP, &ts1Alpha);
  IDirect3DBaseTexture9* t0 = nullptr;
  IDirect3DPixelShader9* ps = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t0);
  dev->lpVtbl->GetPixelShader(dev, &ps);
  const DWORD rsNew[6] = { TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, FALSE };
  const DWORD tsNew[4] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, 0 };
  const DWORD rsANew[4] = { TRUE, D3DBLEND_ZERO, D3DBLEND_ONE, D3DBLENDOP_ADD };
  for (int i = 0; i < 6; ++i) o.SetRenderState(dev, rs[i], rsNew[i]);
  for (int i = 0; i < 4 && bake; ++i) o.SetRenderState(dev, rsA[i], rsANew[i]);
  if (bake) {                                          // no pixel shader: stage 0 must really sample the texture (see ground_base_begin)
    o.SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    o.SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    o.SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  }
  for (int i = 0; i < 4; ++i) o.SetTextureStageState(dev, 0, ts[i], tsNew[i]);   // decal mode: colour ops stay the game's
  o.SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  o.SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->lpVtbl->SetTexture(dev, 0, twin);
  dev->lpVtbl->SetPixelShader(dev, nullptr);
  o.SetVertexDeclaration(dev, swapped);
  draw();
  ++g_layerDraws;
  ++g_gs.layerDrawn;
  o.SetVertexDeclaration(dev, decl);
  dev->lpVtbl->SetPixelShader(dev, ps);
  dev->lpVtbl->SetTexture(dev, 0, t0);
  o.SetTextureStageState(dev, 1, D3DTSS_COLOROP, ts1Color);
  o.SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, ts1Alpha);
  for (int i = 0; i < 4; ++i) o.SetTextureStageState(dev, 0, ts[i], tsOld[i]);
  for (int i = 0; i < 6; ++i) o.SetRenderState(dev, rs[i], rsOld[i]);
  for (int i = 0; i < 4 && bake; ++i) o.SetRenderState(dev, rsA[i], rsAOld[i]);
  for (int i = 0; i < 3 && bake; ++i) o.SetTextureStageState(dev, 0, tsC[i], tsCOld[i]);
  if (ps) ps->lpVtbl->Release(ps);
  if (t0) t0->lpVtbl->Release(t0);
  decl->lpVtbl->Release(decl);
  t1->lpVtbl->Release(t1);
}

// RLDouble* draws without their vertex shader (fixed-function path): the game's stage-0 colour state is only meaningful
// with its ps_1_x pixel shader. RLDoubleTextureSpecular (130-307 draws per frame on Zlin: facades, roads) leaves stage 0
// on COLOROP DISABLE (log "state: RLDoubleTextureSpecular.vso : game stage0 colorop 1"), and Remix' fixed-function texture
// search stops at a DISABLE stage (d3d9_rtx.cpp processTextures): no texture, flat vertex colour (user 2026-11-02: facades
// and roads without texture; they went through the API world before the baker made all RLDouble* D3D9 draws, commit 50c5aad).
// The stage becomes texture x diffuse for the draw (and its second-layer draw) and is put back after.
struct Stage0Fix {
  DWORD op = 0, a1 = 0, a2 = 0;
  bool on = false;
};

bool stage0_fix_begin(IDirect3DDevice9* dev, Stage0Fix& f) {
  DWORD op = 0;
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_COLOROP, &op);
  if (op != D3DTOP_DISABLE) return false;
  f.op = op;
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_COLORARG1, &f.a1);
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_COLORARG2, &f.a2);
  o.SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  o.SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  o.SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  f.on = true;
  return true;
}

void stage0_fix_end(IDirect3DDevice9* dev, Stage0Fix& f) {
  if (!f.on) return;
  o.SetTextureStageState(dev, 0, D3DTSS_COLOROP, f.op);
  o.SetTextureStageState(dev, 0, D3DTSS_COLORARG1, f.a1);
  o.SetTextureStageState(dev, 0, D3DTSS_COLORARG2, f.a2);
  f = Stage0Fix();
}

// Base of a bake-mode ground draw (RLDouble*, opaque, VS unbound): drawn without its pixel shader, so the bake holds
// t0 x vertex colour (the game's lrp would blend t1 into the base, and the layer draw adds t1 again) with alpha 1 (the
// base texture's own alpha would otherwise leave holes in the bake's opacity). Stage 1 off, as for the layer draw.
struct GroundBase {
  IDirect3DPixelShader9* ps = nullptr;
  DWORD aOp = 0, aArg = 0, c1 = 0, a1 = 0, factor = 0, uv = 0, cOp = 0, cArg1 = 0, cArg2 = 0;
  bool on = false;
};

bool ground_base_begin(IDirect3DDevice9* dev, GroundBase& gb) {
  DWORD blend = 0, at = 0;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  if (blend || at) return false;                       // a blended or cut-out base keeps its own alpha and shader
  dev->lpVtbl->GetPixelShader(dev, &gb.ps);
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_ALPHAOP, &gb.aOp);
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, &gb.aArg);
  dev->lpVtbl->GetTextureStageState(dev, 1, D3DTSS_COLOROP, &gb.c1);
  dev->lpVtbl->GetTextureStageState(dev, 1, D3DTSS_ALPHAOP, &gb.a1);
  dev->lpVtbl->GetRenderState(dev, D3DRS_TEXTUREFACTOR, &gb.factor);
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, &gb.uv);
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_COLOROP, &gb.cOp);
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_COLORARG1, &gb.cArg1);
  dev->lpVtbl->GetTextureStageState(dev, 0, D3DTSS_COLORARG2, &gb.cArg2);
  // The game's stage 0 colour state is only meaningful with its pixel shader bound (it may be DISABLE or left from an
  // earlier draw): without that, the bake sees vertex colour only = colourless / grey / black or white ground (user 2026-11-02).
  o.SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  o.SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  o.SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  o.SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
  o.SetRenderState(dev, D3DRS_TEXTUREFACTOR, gb.factor | 0xFF000000u);
  o.SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  o.SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  o.SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  o.SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->lpVtbl->SetPixelShader(dev, nullptr);
  gb.on = true;
  return true;
}

void ground_base_end(IDirect3DDevice9* dev, GroundBase& gb) {
  if (!gb.on) return;
  dev->lpVtbl->SetPixelShader(dev, gb.ps);
  if (gb.ps) gb.ps->lpVtbl->Release(gb.ps);
  o.SetTextureStageState(dev, 1, D3DTSS_COLOROP, gb.c1);
  o.SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, gb.a1);
  o.SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, gb.aOp);
  o.SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, gb.aArg);
  o.SetTextureStageState(dev, 0, D3DTSS_COLOROP, gb.cOp);
  o.SetTextureStageState(dev, 0, D3DTSS_COLORARG1, gb.cArg1);
  o.SetTextureStageState(dev, 0, D3DTSS_COLORARG2, gb.cArg2);
  o.SetRenderState(dev, D3DRS_TEXTUREFACTOR, gb.factor);
  o.SetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, gb.uv);
  gb = GroundBase();
}

// The world through the Remix API (apiworld.cpp): static RL* world-space scenery, declarations only. RLDouble* (two-
// texture ground) only while the ground second layer (Alt+Y) is off: Remix draws it with layer A alone then, as the
// instance does; the layer's decal draw needs the D3D9 draw (layer_draw).
bool api_world_class() {
  if (!g_enabled || !g_apiOk || !g_primaryRT || !g_haveCam || g_testMode != 0 || !g_curInfo || g_vin != kVinDecl) return false;
  const unsigned f = g_curInfo->flags;
  return (f & SH_SCENERY) && (f & SH_WORLDSPACE) && !(f & (SH_ANIM | SH_BTB)) && (!(f & SH_DOUBLE) || !(g_layerOn || g_bakerOn));   // RLDouble: D3D9 draws when a layer is drawn or the baker bakes them
}

// All draws: glass skip (g_glassOn), sky handling (see g_skyMode), then transforms for Remix (before_draw/after_draw).
template <class Draw>
HRESULT comp_draw(IDirect3DDevice9* dev, Draw draw) {
  if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_draw(g_curVS, g_primaryRT);
  const bool glassSkip = !g_glassOn && is_glass_draw();
  const bool sky = is_sky_draw();
  const bool midQuad = !glassSkip && !sky && is_midscene_quad(dev);
  const wchar_t* fate = glassSkip ? L"SKIPPED(glass)" : midQuad ? L"SKIPPED(mid-scene quad)" : sky ? (g_skyMode == 1 ? L"SKIPPED(sky)" : L"sky") : L"drawn";
  if (g_dumpFrames) dump_draw(dev, fate);
  if (g_sweepTsv) sweep_dump_draw(dev, exp_skip_draw() ? L"SKIPPED(step)" : fate);
  if (glassSkip) { ++g_st.glassSkipped; if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_skip(L"glass"); return D3D_OK; }
  if (g_skipOffscreen && g_enabled && !g_primaryRT && g_curInfo && (g_curInfo->flags & (SH_SCENERY | SH_SKY))) {
    ++g_offSkipped;
    if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_skip(L"off-screen scenery");
    return D3D_OK;
  }
  if (midQuad) { ++g_midQuadSkipped; if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_skip(L"mid-scene quad"); return D3D_OK; }
  if (exp_skip_draw()) { ++g_expSkipped; if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_skip(L"experiment (key K)"); return D3D_OK; }   // performance experiment (key K)
  if (rbrrtx_surface_lookup_on) surface_lookup(dev);         // a wheel on a new surface (particles.cpp): ground colour
  if (lens_flare_or_hidden(dev)) { if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_skip(L"lens flare / debug hide"); return D3D_OK; }
  if (g_primaryRT && !g_skipTex.empty()) {                  // materials.ini [skip_draws]: never drawn (the game's sun flare)
    IDirect3DBaseTexture9* t = nullptr;
    if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &t)) && t) {
      const unsigned long long hx = rbrrtx_texture_hash(t);
      t->lpVtbl->Release(t);
      if (g_skipTex.count(hx)) { ++g_skipTexDraws; return D3D_OK; }
    }
  }
  // Rain/snow replaced by Remix particles (particles.cpp): the game's drops are not drawn (pools are 0 as well).
  if (g_primaryRT && rbrrtx_rain_cutting() && (!g_curVS || (g_curInfo && (g_curInfo->flags & SH_PARTICLE)))) {
    IDirect3DBaseTexture9* t = nullptr;
    if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &t)) && t) {
      t->lpVtbl->Release(t);
      if (rbrrtx_rain_skip_texture(rbrrtx_texture_hash(t))) return D3D_OK;
    }
  }
  if (cull_draw(dev)) {                                      // distance culling (culling.ini, key M)
    if (rbrrtx_gauge_probe_on) rbrrtx_gauge_probe_skip(L"culling");
    return D3D_OK;
  }
  lamp_pick_draw(dev);                                       // lamp marker armed: the screen-centre ray against this draw
  texture_probe_draw(dev);                                   // F3 texture picker: the same ray, every frame while the page asks
  void* lampGlow = world_light_draw(dev);                    // lamp textures (lights.ini): a light at the object
  g_primCap = UINT_MAX;
  if (g_primaryRT && g_curInfo && (g_curInfo->flags & SH_PARTICLE) && rbrrtx_rain_probe_on()) {   // rain probe
    IDirect3DBaseTexture9* t = nullptr;
    if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &t)) && t) {
      t->lpVtbl->Release(t);
      rbrrtx_rain_note_draw(rbrrtx_texture_hash(t), g_pendingPrims, false);
    }
  }
  if (sky && g_skyMode == 1) { ++g_st.skyDraws; return D3D_OK; }
  DWORD oldRef = 0;
  bool refChanged = false;
  if (g_foliageMode != 0 && is_scenery_draw()) {
    DWORD at = 0, blend = 0, zw = 0;
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
    if (at) {
      dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
      dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
      dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &oldRef);
      if (blend && !zw && oldRef == 0 && g_foliageMode == 1) { ++g_softSkipped; return D3D_OK; }   // soft-edge pass
      if (!blend && oldRef > g_cutoutRef) {
        refChanged = SUCCEEDED(o.SetRenderState(dev, D3DRS_ALPHAREF, g_cutoutRef));
        ++g_refLowered;
      }
    }
  }
  if (g_enabled && g_primaryRT && g_curInfo && (g_curInfo->flags & SH_DOUBLE) && g_vin == kVinDecl)   // RLDouble ground: terrain tags, stage log
    ground_note_draw(dev, ff_scenery_class() != 1);
  // Registered once as a retained Remix API instance (apiworld.cpp): not sent again (alpha state as lowered above).
  if (g_dip.valid && api_world_class()) {
    const bool sway = (g_curInfo->flags & SH_SWAY) != 0;
    if (rbrrtx_apiworld_wants(sway)) {
      if (sway) {                                     // foliage.cpp still learns the leaf textures
        IDirect3DBaseTexture9* ft = nullptr;
        if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &ft)) && ft) { ft->lpVtbl->Release(ft); rbrrtx_foliage_draw(ft); }
      }
      const RbrWorldDraw wd = { g_dip.base, g_dip.minIdx, g_dip.numV, g_dip.startIdx, g_dip.count, sway };
      frame_camera(from_rows(&g_c[0]));               // this draw may be the one that defines the frame's camera
      if (rbrrtx_apiworld_draw(dev, wd)) {
        ++g_st.apiWorld;
        if (refChanged) o.SetRenderState(dev, D3DRS_ALPHAREF, oldRef);
        return D3D_OK;
      }
    }
  }
  if (g_harvestUsage) rbrrtx_harvest_note_draw(dev, g_curInfo ? fs::path(g_curInfo->name).filename().c_str() : g_curVS ? L"(unknown VS)" : L"(fixed-function)");
  D3DVIEWPORT9 vp;
  bool vpChanged = false;
  if (sky && g_skyMode == 2 && SUCCEEDED(dev->lpVtbl->GetViewport(dev, &vp))) {
    D3DVIEWPORT9 skyVp = vp;
    skyVp.MinZ = skyVp.MaxZ = 1.0f;
    vpChanged = SUCCEEDED(dev->lpVtbl->SetViewport(dev, &skyVp));
    ++g_st.skyDraws;
  }
  const bool hinted = before_draw(dev);
  if (g_dumpFrames && !g_curVS && g_dumpDraws <= 1500) {   // what Remix's UI check reads for this draw
    D3DMATRIX p = {};
    DWORD zw = 0, cw = 0;
    dev->lpVtbl->GetTransform(dev, D3DTS_PROJECTION, &p);
    dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
    dev->lpVtbl->GetRenderState(dev, D3DRS_COLORWRITEENABLE, &cw);
    clog(L"    -> proj [3][3] %.3f [2][3] %.3f, zw %lu, colour write 0x%lx, occlusion queries open %d, UI trigger at draw %u",
         p._44, p._34, zw, cw, g_oqActive, g_st.uiTriggerAt);
  }
  bool atOff = false;                                  // drawn without alpha test: experiment class or opaque cut-out
  const bool expSolid = exp_solid_draw();
  if (expSolid || g_opaqueAlpha) {
    DWORD at = 0;
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
    const bool opaque = at && !expSolid && opaque_cutout(dev);
    if (at && (expSolid || opaque) && SUCCEEDED(o.SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE))) {
      atOff = true;
      ++g_expSolid;
      if (opaque) ++g_opaqueDraws;
    }
  }
  // Fixed-function scenery (g_ffMode): unbind the VS for this draw, or give the game's VS back after FF draws.
  const int ffClass = g_curVS ? ff_scenery_class() : 0;
  const bool ff = ffClass == 1 || (ffClass == 2 && ff_sway_far(dev));
  if (ff && ffClass == 2) ++g_ffSwayFar;
  if (ff && !g_vsUnbound) g_vsUnbound = SUCCEEDED(o.SetVertexShader(dev, nullptr));
  else if (!ff && g_vsUnbound && g_curVS) {
    o.SetVertexShader(dev, static_cast<IDirect3DVertexShader9*>(g_curVS));
    g_vsUnbound = false;
  }
  const bool litOff = ff && g_lighting && SUCCEEDED(o.SetRenderState(dev, D3DRS_LIGHTING, FALSE));   // no normals
  if (ff) ++g_ffDraws;
  IDirect3DVertexDeclaration9* nn = ff && !g_ffNormals && g_vin == kVinDecl && g_curDecl
      ? no_normal(dev, static_cast<IDirect3DVertexDeclaration9*>(g_curDecl)) : nullptr;
  const bool normalsOff = nn && SUCCEEDED(o.SetVertexDeclaration(dev, nn));
  RbrAlphaCut ac = {};
  const bool cut = g_alphaCut && g_dip.valid && !atOff && is_scenery_draw() &&
                   rbrrtx_alphacut_find(dev, g_dip.base, g_dip.minIdx, g_dip.numV, g_dip.startIdx, g_dip.count, &ac);
  if (cut && (ac.alphaTestOff || g_alphaCutSolid) && SUCCEEDED(o.SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE))) atOff = true;
  // Radial normals on fixed-function swaying foliage (RL*Sway: trees, bushes, grass; one-card clumps stay flat).
  IDirect3DVertexBuffer9* radPrev = nullptr;
  UINT radPrevOff = 0, radPrevStride = 0;
  bool radOn = false;
  if (ff && ffClass == 2 && !cut && g_dip.valid && g_vin == kVinDecl && g_curDecl && rbrrtx_radnorm_enabled()) {
    IDirect3DVertexDeclaration9* rd = rad_normal_decl(dev, static_cast<IDirect3DVertexDeclaration9*>(g_curDecl));
    void* nb = rd ? rbrrtx_radnorm_find(dev, g_dip.base, g_dip.minIdx, g_dip.numV, g_dip.startIdx, g_dip.count) : nullptr;
    if (nb) {
      dev->lpVtbl->GetStreamSource(dev, kRadStream, &radPrev, &radPrevOff, &radPrevStride);
      radOn = SUCCEEDED(dev->lpVtbl->SetStreamSource(dev, kRadStream, static_cast<IDirect3DVertexBuffer9*>(nb), 0, 12)) &&
              SUCCEEDED(o.SetVertexDeclaration(dev, rd));
    }
  }
  // Lit car parts (litparts.cpp): a hot brake disc or a lit lamp lens is drawn with its twin texture, which the generated
  // mod makes emissive (albedo and glow from the car's atlas file). Swaying foliage: its texture becomes thin opaque.
  IDirect3DBaseTexture9* litPrev = nullptr;
  bool litSwap = false;
  if (g_litOn && g_enabled && g_apiOk && g_primaryRT && g_curInfo && (g_curInfo->flags & SH_CAR) && g_dip.valid) {
    if (void* twin = rbrrtx_litparts_draw(dev, g_dip.base, g_dip.minIdx, g_dip.numV, g_dip.startIdx, g_dip.count, g_carWorld)) {
      dev->lpVtbl->GetTexture(dev, 0, &litPrev);
      litSwap = SUCCEEDED(dev->lpVtbl->SetTexture(dev, 0, static_cast<IDirect3DBaseTexture9*>(twin)));
    }
  }
  if (g_enabled && g_apiOk && g_primaryRT && g_curInfo && (g_curInfo->flags & SH_SWAY)) {
    IDirect3DBaseTexture9* ft = nullptr;
    if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &ft)) && ft) { ft->lpVtbl->Release(ft); rbrrtx_foliage_draw(ft); }
  }
  if (!litSwap && g_primaryRT && g_enabled) {                // the lamp halo sprite (headlights.ini halo_*): a thinner twin
    if (const unsigned long long halo = rbrrtx_headlights_halo_texture()) {
      IDirect3DBaseTexture9* ht = nullptr;
      if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, 0, &ht)) && ht) {
        if (rbrrtx_texture_hash(ht) == halo)
          if (void* twin = rbrrtx_headlights_halo_twin(dev)) {
            litPrev = ht;                                   // restored (and released) after the draw
            ht = nullptr;
            litSwap = SUCCEEDED(dev->lpVtbl->SetTexture(dev, 0, static_cast<IDirect3DBaseTexture9*>(twin)));
          }
        if (ht) ht->lpVtbl->Release(ht);
      }
    }
  }
  if (lampGlow && !litSwap) {                                // a marked lamp's glass glows (worldlights.cpp): its twin texture
    dev->lpVtbl->GetTexture(dev, 0, &litPrev);
    litSwap = SUCCEEDED(dev->lpVtbl->SetTexture(dev, 0, static_cast<IDirect3DBaseTexture9*>(lampGlow)));
  }
  GroundBase groundBase;                                // bake mode: opaque RLDouble base drawn without its pixel shader, alpha 1
  if (g_groundBaseFF && g_layerMode == kLayerBake && g_bakerOn && ff && g_vsUnbound && g_enabled && g_primaryRT && g_vin == kVinDecl &&
      g_curInfo && (g_curInfo->flags & SH_DOUBLE) && ground_base_begin(dev, groundBase)) ++g_groundFFBases;
  Stage0Fix stage0;                                     // RLDouble* with stage 0 on DISABLE: texture x diffuse for this draw (and its layer)
  if (g_stage0Fix && !groundBase.on && ff && g_vsUnbound && g_enabled && g_primaryRT && g_vin == kVinDecl && g_curInfo &&
      (g_curInfo->flags & SH_DOUBLE) && stage0_fix_begin(dev, stage0)) ++g_gs.stage0Fixed;
  HRESULT hr = D3D_OK;
  if (cut) hr = alphacut_draw(dev, ac);
  else if (!clock_draw(dev, hr) && !btb_instanced_draw(dev, hr)) {
    if (g_enabled && g_primaryRT) rbrrtx_hudglow_before(dev);
    hr = draw();
    rbrrtx_hudglow_after(dev);
  }
  ground_base_end(dev, groundBase);
  if (litSwap) dev->lpVtbl->SetTexture(dev, 0, litPrev);
  if (litPrev) litPrev->lpVtbl->Release(litPrev);
  if (SUCCEEDED(hr)) { if (ff) layer_draw(dev, draw); else dirt_draw(dev, draw); }
  stage0_fix_end(dev, stage0);
  if (normalsOff || radOn) o.SetVertexDeclaration(dev, static_cast<IDirect3DVertexDeclaration9*>(g_curDecl));
  if (radOn || radPrev) dev->lpVtbl->SetStreamSource(dev, kRadStream, radPrev, radPrevOff, radPrevStride);
  if (radPrev) radPrev->lpVtbl->Release(radPrev);
  if (litOff) o.SetRenderState(dev, D3DRS_LIGHTING, g_lighting);
  if (atOff) o.SetRenderState(dev, D3DRS_ALPHATESTENABLE, TRUE);
  if (hinted) after_draw(dev);
  if (refChanged) o.SetRenderState(dev, D3DRS_ALPHAREF, oldRef);
  if (vpChanged) dev->lpVtbl->SetViewport(dev, &vp);
  return hr;
}

UINT verts_for(D3DPRIMITIVETYPE t, UINT prims) {
  switch (t) {
    case D3DPT_TRIANGLELIST: return prims * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return prims + 2;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    default: return prims;
  }
}

HRESULT STDMETHODCALLTYPE hk_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT count) {
  if (rbrrtx_menu_is_drawing()) return o.DrawPrimitive(dev, t, start, count);
  g_pendingPrims = count;
  g_drawType = t;
  g_drawFirstVert = start;
  g_drawNumVert = verts_for(t, count);
  g_dip.valid = false;
  return comp_draw(dev, [&] { return o.DrawPrimitive(dev, t, start, cap_prims(count)); });
}

HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minIdx,
                                                  UINT numV, UINT startIdx, UINT count) {
  if (rbrrtx_menu_is_drawing()) return o.DrawIndexedPrimitive(dev, t, base, minIdx, numV, startIdx, count);
  g_pendingPrims = count;
  g_drawType = t;
  g_drawFirstVert = base >= 0 ? static_cast<UINT>(base) + minIdx : 0;
  g_drawNumVert = base >= 0 ? numV : 0;
  g_dip = { t == D3DPT_TRIANGLELIST && base >= 0, base, minIdx, numV, startIdx, count };
  g_inDip = true;
  g_dipTriList = t == D3DPT_TRIANGLELIST;
  const HRESULT hr = comp_draw(dev, [&] { return o.DrawIndexedPrimitive(dev, t, base, minIdx, numV, startIdx, cap_prims(count)); });
  g_dip.valid = false;
  g_inDip = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) {
  if (rbrrtx_menu_is_drawing()) return o.DrawPrimitiveUP(dev, t, count, data, stride);
  g_pendingPrims = count;
  g_drawNumVert = 0;                                   // no vertex buffer: not culled
  g_dip.valid = false;
  return comp_draw(dev, [&] { return o.DrawPrimitiveUP(dev, t, cap_prims(count), data, stride); });
}

HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minIdx, UINT numV,
                                                    UINT count, const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
  if (rbrrtx_menu_is_drawing()) return o.DrawIndexedPrimitiveUP(dev, t, minIdx, numV, count, idx, fmt, data, stride);
  g_pendingPrims = count;
  g_drawNumVert = 0;
  g_dip.valid = false;
  return comp_draw(dev, [&] { return o.DrawIndexedPrimitiveUP(dev, t, minIdx, numV, cap_prims(count), idx, fmt, data, stride); });
}

// Occlusion queries (suspect a in g_uiTriggerOn): same bookkeeping as Remix's D3D9Query::Issue, so g_oqActive is
// Remix's m_activeOcclusionQueries as seen from the game side. All backend query objects share one vtable.
HRESULT STDMETHODCALLTYPE hk_QueryIssue(IDirect3DQuery9* q, DWORD flags) {
  if (q->lpVtbl->GetType(q) == D3DQUERYTYPE_OCCLUSION) {
    bool& begun = g_oqBegun[q];
    if ((flags & D3DISSUE_BEGIN) && !begun) { begun = true; ++g_oqActive; }
    else if ((flags & D3DISSUE_END) && begun) { begun = false; --g_oqActive; }
    if (flags & D3DISSUE_END) g_oqEndFrame = g_frame;
  }
  return o.QueryIssue(q, flags);
}

ULONG STDMETHODCALLTYPE hk_QueryRelease(IDirect3DQuery9* q) {
  const ULONG refs = o.QueryRelease(q);
  if (refs == 0) {
    auto it = g_oqBegun.find(q);
    if (it != g_oqBegun.end()) {
      if (it->second) {                                // Remix never decrements for this one: stuck count
        --g_oqActive;
        clog(L"occlusion query %p released while begun (frame %u): Remix's open-query count stays raised", static_cast<void*>(q), g_frame);
      }
      g_oqBegun.erase(it);
    }
  }
  return refs;
}

// A new buffer at an address cached by the culling must not reuse the old buffer's bounds (stage reloads).
HRESULT STDMETHODCALLTYPE hk_CreateVertexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                IDirect3DVertexBuffer9** vb, HANDLE* shared) {
  const HRESULT hr = o.CreateVertexBuffer(dev, len, usage, fvf, pool, vb, shared);
  if (SUCCEEDED(hr) && vb && *vb) {
    ++g_vbGen[*vb];
    rbrrtx_apiworld_forget(*vb);
    rbrrtx_alphacut_forget(*vb);
    rbrrtx_radnorm_forget(*vb);
    if (*vb == g_clockVB) g_clockVB = nullptr;         // a new buffer at the clock's old address
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateIndexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                               IDirect3DIndexBuffer9** ib, HANDLE* shared) {
  const HRESULT hr = o.CreateIndexBuffer(dev, len, usage, fmt, pool, ib, shared);
  if (SUCCEEDED(hr) && ib && *ib) { rbrrtx_apiworld_forget(*ib); rbrrtx_alphacut_forget(*ib); rbrrtx_radnorm_forget(*ib); }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateQuery(IDirect3DDevice9* dev, D3DQUERYTYPE type, IDirect3DQuery9** out) {
  const HRESULT hr = o.CreateQuery(dev, type, out);
  if (SUCCEEDED(hr) && out && *out) {
    static unsigned logged = 0;
    if (logged < 10) { ++logged; clog(L"CreateQuery type %d -> %p (frame %u)", static_cast<int>(type), static_cast<void*>(*out), g_frame); }
    IDirect3DQuery9Vtbl* v = (*out)->lpVtbl;
    patch(&v->Issue, &hk_QueryIssue, o.QueryIssue);
    patch(&v->Release, &hk_QueryRelease, o.QueryRelease);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                          D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  const HRESULT hr = o.CreateDevice(d3d, adapter, type, wnd, flags, pp, out);
  clog(L"CreateDevice -> 0x%08lX, device %p", hr, out ? static_cast<void*>(*out) : nullptr);
  if (FAILED(hr) || !out || !*out) return hr;
  IDirect3DDevice9* dev = *out;
  g_wnd = pp && pp->hDeviceWindow ? pp->hDeviceWindow : wnd;   // sweep screenshots / capture key
  IDirect3DDevice9Vtbl* v = dev->lpVtbl;
  o.AddRef = v->AddRef;
  patch(&v->Release, &hk_Release, o.Release);
  patch(&v->Reset, &hk_Reset, o.Reset);
  patch(&v->Present, &hk_Present, o.Present);
  patch(&v->BeginScene, &hk_BeginScene, o.BeginScene);
  patch(&v->SetRenderTarget, &hk_SetRenderTarget, o.SetRenderTarget);
  patch(&v->SetTransform, &hk_SetTransform, o.SetTransform);
  patch(&v->CreateVertexShader, &hk_CreateVertexShader, o.CreateVertexShader);
  patch(&v->SetVertexShader, &hk_SetVertexShader, o.SetVertexShader);
  patch(&v->SetVertexShaderConstantF, &hk_SetVertexShaderConstantF, o.SetVertexShaderConstantF);
  patch(&v->SetPixelShaderConstantF, &hk_SetPixelShaderConstantF, o.SetPixelShaderConstantF);
  patch(&v->CreateVertexDeclaration, &hk_CreateVertexDeclaration, o.CreateVertexDeclaration);
  patch(&v->SetVertexDeclaration, &hk_SetVertexDeclaration, o.SetVertexDeclaration);
  patch(&v->SetFVF, &hk_SetFVF, o.SetFVF);
  patch(&v->CreateStateBlock, &hk_CreateStateBlock, o.CreateStateBlock);
  patch(&v->BeginStateBlock, &hk_BeginStateBlock, o.BeginStateBlock);
  patch(&v->EndStateBlock, &hk_EndStateBlock, o.EndStateBlock);
  patch(&v->SetTextureStageState, &hk_SetTextureStageState, o.SetTextureStageState);
  patch(&v->SetRenderState, &hk_SetRenderState, o.SetRenderState);
  patch(&v->DrawPrimitive, &hk_DrawPrimitive, o.DrawPrimitive);
  patch(&v->DrawIndexedPrimitive, &hk_DrawIndexedPrimitive, o.DrawIndexedPrimitive);
  patch(&v->DrawPrimitiveUP, &hk_DrawPrimitiveUP, o.DrawPrimitiveUP);
  patch(&v->DrawIndexedPrimitiveUP, &hk_DrawIndexedPrimitiveUP, o.DrawIndexedPrimitiveUP);
  patch(&v->CreateQuery, &hk_CreateQuery, o.CreateQuery);
  patch(&v->CreateVertexBuffer, &hk_CreateVertexBuffer, o.CreateVertexBuffer);
  patch(&v->CreateIndexBuffer, &hk_CreateIndexBuffer, o.CreateIndexBuffer);
  remember_backbuffer(dev);
  rbrrtx_harvest_install(dev);
  if (rbrrtx_env_flag(L"RBRRTX_GAUGE_PROBE", false)) rbrrtx_gauge_probe_init(dev);
  init_remix_api();
  rbrrtx_headlights_init(g_apiOk ? &g_api : nullptr);
  rbrrtx_world_lights_init(g_apiOk ? &g_api : nullptr);
  rbrrtx_apiworld_init(g_apiOk && g_remixPlus ? rbrrtx_api_ext() : nullptr);
  rbrrtx_hudglow_init(dev, g_apiOk && g_remixPlus && rbrrtx_api_ext() && rbrrtx_api_ext()->createRetained);
  load_skip_textures();
  rbrrtx_particles_init(g_apiOk ? &g_api : nullptr);
  rbrrtx_windscreen_init(g_apiOk ? &g_api : nullptr);
  f3_install(dev);                                     // F3 menu (RBRRTX_MENU=0 leaves it out)
  clog(L"device hooks installed (comp %s, sun %s)", g_enabled ? L"on" : L"off", g_apiOk ? L"on" : L"off");
  return hr;
}

}  // namespace

void rbrrtx_watch_buffer_writes(void* vb, void* ib) { watch_buffer_writes(vb, ib); }

bool rbrrtx_remix_set(const char* key, const char* value) {
  if (!g_apiOk || !g_api.SetConfigVariable) return false;
  remix_set(key, value);
  return true;
}

std::wstring rbrrtx_stage_line(const RbrStageInfo& si) { return stage_line(si); }   // report.cpp

void rbrrtx_comp_install(void* d3d9, HMODULE backend) {
  static bool loaded = false;
  if (!d3d9) return;
  g_backend = backend;
  if (!loaded) {
    loaded = true;
    rbrrtx_keys_load();
    cull_load();
    cull_log_settings(L"culling.ini");
    g_sunWanted = rbrrtx_env_flag(L"RBRRTX_SUN", true);
    wchar_t sky[8];
    if (GetEnvironmentVariableW(L"RBRRTX_SKY", sky, 8)) g_skyMode = std::clamp(_wtoi(sky), 0, 2);
    g_glassOn = rbrrtx_env_flag(L"RBRRTX_GLASS", true);
    g_btbOn = rbrrtx_env_flag(L"RBRRTX_BTB", false);
    g_dirtOn = rbrrtx_env_flag(L"RBRRTX_CAR_DIRT", true);
    {                                                  // ground second layer: bake when the Remix terrain baker is on, else off
      const std::string bk = conf_string("rtx.terrainBaker.enableBaking");
      g_bakerOn = !bk.empty() && (bk[0] == 'T' || bk[0] == 't' || bk[0] == '1');
      wchar_t lv[16] = {};
      int mode = g_bakerOn ? kLayerDecal : kLayerOff;  // decal from the load of the first stage: its twins need the sources held at upload
      if (GetEnvironmentVariableW(L"RBRRTX_GROUND_LAYER", lv, 16)) {   // 0 / off, 1 / bake, 2 / decal
        const wchar_t c = static_cast<wchar_t>(towlower(lv[0]));
        mode = c == L'2' || c == L'd' ? kLayerDecal : c == L'0' || c == L'o' || c == L'f' ? kLayerOff : kLayerBake;
      }
      g_layerMode = g_layerWant = mode;
      g_layerOn = mode != kLayerOff;
      rbrrtx_layer_enable(mode == kLayerDecal);
      g_groundBaseFF = rbrrtx_env_flag(L"RBRRTX_GROUND_BASE_FF", true);
      g_stage0Fix = rbrrtx_env_flag(L"RBRRTX_STAGE0_FIX", true);
    }
    g_clockOn = rbrrtx_env_flag(L"RBRRTX_CLOCK", true);
    g_ffNormals = rbrrtx_env_flag(L"RBRRTX_FF_NORMALS", false);
    g_uiTriggerOn = rbrrtx_env_flag(L"RBRRTX_UI_TRIGGER", true);
    g_skipOffscreen = !rbrrtx_env_flag(L"RBRRTX_OFFSCREEN", false);
    wchar_t fol[16];
    if (GetEnvironmentVariableW(L"RBRRTX_FOLIAGE", fol, 16)) g_foliageMode = std::clamp(_wtoi(fol), 0, 2);
    if (GetEnvironmentVariableW(L"RBRRTX_FF", fol, 16)) g_ffMode = g_ffBase = std::clamp(_wtoi(fol), 0, 2);
    g_opaqueAlpha = g_opaqueBase = rbrrtx_env_flag(L"RBRRTX_OPAQUE_ALPHA", true) ? 1 : 0;
    g_alphaCut = g_alphaCutBase = rbrrtx_env_flag(L"RBRRTX_ALPHACUT", false) ? 1 : 0;
    rbrrtx_alphacut_init();
    rbrrtx_radnorm_load();
    if (GetEnvironmentVariableW(L"RBRRTX_FF_SWAY_NEAR", fol, 16)) g_ffSwayNear = _wtof(fol) > 0 ? static_cast<float>(_wtof(fol)) : -1.0f;
    g_ffSwayNearBase = g_ffSwayNear;
    if (GetEnvironmentVariableW(L"RBRRTX_ALPHAREF", fol, 16)) g_cutoutRef = static_cast<DWORD>(std::clamp(_wtoi(fol), 1, 255));
    const DWORD hv = GetEnvironmentVariableW(L"RBRRTX_HARVEST", fol, 16);
    g_harvestUsage = hv > 0 && !(hv < 16 && wcscmp(fol, L"0") == 0);
    wchar_t xf[8];
    if (GetEnvironmentVariableW(L"RBRRTX_XF_MODE", xf, 8)) g_testMode = std::clamp(_wtoi(xf), 0, 3);
    wchar_t rad[64];
    if (GetEnvironmentVariableW(L"RBRRTX_SUN_MAX_ELEV", rad, 64)) g_sunMaxElev = std::clamp(static_cast<float>(_wtof(rad)), 5.0f, 90.0f);
    if (GetEnvironmentVariableW(L"RBRRTX_SUN_FALLBACK", rad, 64)) {
      swscanf_s(rad, L"%f,%f", &g_sunFallbackAz, &g_sunFallbackEl);
      g_sunFallbackFixed = true;
    }
    if (GetEnvironmentVariableW(L"RBRRTX_SUN_RADIANCE", rad, 64))
      swscanf_s(rad, L"%f,%f,%f", &g_sunRadiance[0], &g_sunRadiance[1], &g_sunRadiance[2]);
    load_vso_hashes();
  }
  IDirect3D9* d3d = static_cast<IDirect3D9*>(d3d9);
  patch(&d3d->lpVtbl->CreateDevice, &hk_CreateDevice, o.CreateDevice);
  clog(L"comp installed on IDirect3D9 %p", d3d9);
}

bool rbrrtx_remix_plus() { return g_remixPlus; }

// DLL_PROCESS_DETACH by FreeLibrary (not at process exit): the menu goes and the window gets its procedure back.
void rbrrtx_comp_shutdown() {
  if (g_f3Wnd && g_f3NextProc) {
    const LONG_PTR cur = g_f3Unicode ? GetWindowLongPtrW(g_f3Wnd, GWLP_WNDPROC) : GetWindowLongPtrA(g_f3Wnd, GWLP_WNDPROC);
    if (cur == reinterpret_cast<LONG_PTR>(f3_wndproc)) {
      if (g_f3Unicode) SetWindowLongPtrW(g_f3Wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_f3NextProc));
      else SetWindowLongPtrA(g_f3Wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_f3NextProc));
    }
    g_f3Wnd = nullptr;
  }
  f3_probe_free();
  rbrrtx_dinput_block_remove();
  rbrrtx_menu_shutdown();
}
