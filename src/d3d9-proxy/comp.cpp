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
// Hotkeys (keys.cpp / keys.ini, defaults): O toggles 3 and 4 at runtime, L only 4, P dumps the next frame's draws;
// N cycles the sky mode, I the windscreen. Diagnostics go to <RBRRTX_LOG_DIR>\rbrrtx_comp.log.

#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <initguid.h>
#include <d3d9.h>
#include "third_party/remix_c.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
           SH_SWAY = 256, SH_ANIM = 512, SH_BTB = 1024 };   // SWAY: wind-swayed foliage; ANIM: UV/vertex animation (c85-c87)

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
bool g_layerOn = true;                                 // ground second layer (layer_draw)
unsigned g_layerDraws = 0, g_layerMissing = 0, g_layerUntagged = 0;
bool g_haveCam = false, g_frameCam = false;
D3DMATRIX g_V = {}, g_P = {};
float g_carPos[3] = {};                               // last car draw's world position (culling keep radius)
bool g_haveCar = false;
DMat g_VPinv = {};
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
remixapi_LightHandle g_sun = nullptr;
float g_sunDir[3] = {}, g_sunRadiance[3] = { 6.0f, 5.7f, 5.2f };
unsigned g_sunCreates = 0;
// Debug sun (user 2026-10-16, judging normal maps needs low, raking light): H (keys.ini) switches to a manual sun that
// starts where the game's sun is; while it is on, hold ; / ' to lower/raise it and [ / ] to turn it around (0.5
// degrees per frame). H again = the game's sun. Night (user 2026-10-17): below the horizon down to -18 degrees; the
// sun light fades out between +2 and -4 degrees and Remix's sky light (rtx.skyBrightness) dims to 1 % at -12.
bool g_sunManual = false, g_sunMoving = false;
float g_sunAz = 0.0f, g_sunEl = 30.0f;                 // degrees; elevation above the horizon
float g_sunFade = 1.0f;                                // sun radiance factor of the light currently created
float g_sunScale = 1.0f;                               // live brightness (sun_brighter / sun_dimmer keys), times g_sunRadiance
float g_sunWeather = 1.0f;                             // stage atmosphere: cloud cover / rain / fog (atmosphere.ini)
float g_skyBase = -1.0f, g_skySet = -1.0f;             // rtx.skyBrightness from the conf (-1 = not read) / last set

struct Stats {
  unsigned draws, known, unknown, ff, offscreen, xformed, noCamera, scenes, skyDraws, glassSkipped, screenSpace;
  unsigned uiTriggerAt, oqAtUi;                        // draw number of our UI trigger (0 = none); open queries then
  unsigned btb, btbSkipped, btbInstanced, btbCopies;   // BTB effect draws; untransformed; instanced draws, their copies
  unsigned btbRebuilt, btbCamRejected;                 // merged instance buffers built this frame; camera candidates refused
  float worldErr, rlDev, recErr;
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

// Dust/smoke (Valstagna replay 05:57, user): RBR draws gravel dust as Particle.vso batches of alpha-tested, blended
// sprites. Path traced, hundreds of stacked translucent quads cost a lot and fill the screen as a brown wall (onboard
// replays at slow hairpins). Only the first g_dustKeep percent of each batch's triangles is drawn (rounded to whole
// quads). RBRRTX_DUST=<percent> (100 = all, 0 = none); the dust key cycles them live.
// Run 06:19 (user): with the sprites tagged as Remix particles (rtx.conf) full dust looked best; 15% left a few flat
// sprite planes trailing the car. Run 16:52 (user): full dust is far too expensive (the test install has ParticleMod by
// Kahoo: many fine sprites); wanted: cheap and not too dense. ParticleQuality = low (the 2004 PS_*_LOW.ini) drew no
// dust at all under Remix (run 18:26, Alt+Pause had nothing to trim), so ParticleMod's sprites. Trimming keeps the
// first part of each batch, which in a TV-cam replay (run 18:41) cut the trail into dust / gap / dust instead of
// thinning it, so the default is 100 again until thinning is spread evenly. Dust key: 100 -> 0 -> 25 -> 50.
// g_dustQuads counts drawn quads for the log.
unsigned g_dustKeep = 100, g_dustTrimmed = 0, g_dustQuads = 0, g_dustFrames = 0;
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

void init_remix_api() {
  if (!g_sunWanted || !g_backend) return;
  auto init = reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(g_backend, "remixapi_InitializeLibrary"));
  if (!init) { clog(L"sun: backend has no remixapi_InitializeLibrary (not Remix); sun disabled"); return; }
  remixapi_InitializeLibraryInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
  const remixapi_ErrorCode err = init(&info, &g_api);
  g_apiOk = err == REMIXAPI_ERROR_CODE_SUCCESS && in_backend(reinterpret_cast<const void*>(g_api.CreateLight)) &&
            in_backend(reinterpret_cast<const void*>(g_api.DestroyLight)) &&
            in_backend(reinterpret_cast<const void*>(g_api.DrawLightInstance));
  if (g_apiOk) clog(L"sun: Remix API ready");
  else if (err == REMIXAPI_ERROR_CODE_NOT_INITIALIZED) clog(L"sun: Remix API refused; set exposeRemixApi = True in .trex\\bridge.conf");
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

void sun_rule(float d[3]) {
  const float horiz = std::sqrt(d[0] * d[0] + d[2] * d[2]);
  const float elev = std::atan2(-d[1], horiz) * 57.2958f;
  int rule = 0;
  float az = std::atan2(d[2], d[0]) * 57.2958f, el = elev;
  if (g_inMenu && g_menuSunOn) { rule = 4; az = g_menuSunAz; }
  else if (g_sunOverride) { rule = 3; az = g_sunOverrideAz; el = g_sunOverrideEl; }
  else if (elev > g_sunValidMax) { rule = 2; az = g_sunFallbackAz; el = g_sunFallbackEl; }
  else if (elev > g_sunMaxElev) { rule = 1; el = g_sunMaxElev; }
  if (rule) {
    const float e = el * 0.0174533f, a = az * 0.0174533f;
    d[0] = std::cos(e) * std::cos(a);
    d[1] = -std::sin(e);
    d[2] = std::cos(e) * std::sin(a);
  }
  if (rule != g_sunRuleLogged) {
    g_sunRuleLogged = rule;
    clog(L"sun rule: game sun elevation %.1f deg -> %s (azimuth %.1f, elevation %.1f)", elev,
         rule == 4 ? L"menu azimuth (atmosphere.ini)" : rule == 3 ? L"stage override (atmosphere.ini)" : rule == 2 ? L"missing or overhead, fallback" : rule == 1 ? L"capped" : L"kept 1:1", az, el);
  }
}

void update_sun() {
  if (!g_apiOk || !g_enabled || !g_sunOn || (!g_cset[41] && !g_sunManual)) return;
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
  const float fade = std::clamp((elev + 4.0f) / 6.0f, 0.0f, 1.0f);   // twilight: full at +2, gone at -4 degrees
  if (fade <= 0.0f) {                                  // night: no sun light at all
    if (g_sun) { g_api.DestroyLight(g_sun); g_sun = nullptr; }
    g_sunFade = 0.0f;
    return;
  }
  const float cosDelta = d[0] * g_sunDir[0] + d[1] * g_sunDir[1] + d[2] * g_sunDir[2];
  if (!g_sun || cosDelta < 0.99999f || std::fabs(fade - g_sunFade) > 0.02f) {   // ~0.25 degrees: lights are immutable, so recreate
    g_sunFade = fade;
    if (g_sun) g_api.DestroyLight(g_sun);
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
    if (err != REMIXAPI_ERROR_CODE_SUCCESS) { g_sun = nullptr; return; }
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
// test 2026-10-17: a grey night "lit from below"), so the lower exposure limit rises with the night to evMax - 1.
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
  if (!g_apiOk || !g_api.SetConfigVariable) return;
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
  const float evMin = evMinBase + ((std::max)(evMinBase, evMax - 1.0f) - evMinBase) * night;
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
struct AtmoFactors {
  float dist = 1.0f, col[3] = { 1.0f, 1.0f, 1.0f }, sky = 1.0f, sun = 1.0f;
  float albedo = -1.0f, aniso = -1000.0f;              // absolute values when set
  float sunMaxElev = -1.0f;                            // absolute when set: fallback sun cap (deg)
  float fbAz = -1000.0f, fbEl = -1.0f;                 // absolute when set: fallback sun "azimuth, elevation" (deg)
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
  return f;
}

void atmo_load() {
  g_atmoLoaded = true;
  wchar_t* path = g_atmoPath;
  rbrrtx_config_path(L"RBRRTX_ATMOSPHERE", L"atmosphere.ini", path, MAX_PATH * 2);
  rbrrtx_config_path(L"RBRRTX_SUN_SAVED", L"sun-saved.ini", g_sunSavedPath, MAX_PATH * 2);
  g_atmoEnabled = GetPrivateProfileIntW(L"atmosphere", L"enable", 1, path) != 0;
  {
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"atmosphere", L"menu_sun_azimuth", L"28.0", v, 32, path);
    g_menuSunOn = swscanf_s(v, L"%f", &g_menuSunAz) == 1;
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

void atmo_frame() {
  if (g_frame % 30 != 0 || !g_apiOk || g_expStep >= 0) return;   // K / sweep runs keep their own values
  if (!g_atmoLoaded) atmo_load();
  RbrStageInfo si;
  const bool inStage = rbrrtx_stage_info(&si) && si.trackId > 0 &&
                       (si.gameMode == 1 || si.gameMode == 2 || si.gameMode == 5 || si.gameMode == 8 || si.gameMode == 10);
  if (inStage == g_inMenu) {                           // menu <-> stage: log the sun rule again
    g_inMenu = !inStage;
    g_sunRuleLogged = -1;
    if (g_inMenu) g_atmoKey.clear();                   // the next stage applies its look again
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
  };
  auto sec = [&](const wchar_t* prefix, const wchar_t* name) -> const AtmoFactors* {
    auto it = g_atmoSections.find(std::wstring(prefix) + name);
    return it != g_atmoSections.end() ? &it->second : nullptr;
  };
  mul(region ? &region->f : nullptr);
  mul(sec(L"sky.", rbrrtx_sky_name(si.skyType)));
  mul(sec(L"clouds.", rbrrtx_cloud_name(si.clouds)));
  mul(sec(L"time.", rbrrtx_time_name(si.timeOfDay)));
  mul(sec(L"stage.", std::to_wstring(si.trackId).c_str()));   // per-stage look (soft baked shadows: weaker sun)
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
  float col[3], alb[3];
  for (int i = 0; i < 3; ++i) {
    col[i] = std::clamp(g_atmoBase.col[i] * total.col[i], 0.05f, 0.999f);
    alb[i] = albedo >= 0.0f ? std::clamp(albedo, 0.0f, 0.999f) : g_atmoBase.albedo[i];
  }
  const float dist = (std::max)(1.0f, g_atmoBase.dist * total.dist);
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
  swprintf_s(line, L"stage %d %s (%S, region %s), sky %s, clouds %s, %s -> haze %.0f m (x%.2f), colour (%.2f %.2f %.2f), "
                   L"albedo %.2f, anisotropy %.2f, sky light %.2f, sun x%.2f, fallback sun %.1f/%.1f, stage sun %s",
             si.trackId, si.name[0] ? si.name : L"?", si.country[0] ? si.country : "?", region ? region->name.c_str() : L"none",
             rbrrtx_sky_name(si.skyType), rbrrtx_cloud_name(si.clouds), rbrrtx_time_name(si.timeOfDay), dist, total.dist,
             col[0], col[1], col[2], alb[0], an, sky, total.sun, g_sunFallbackAz, g_sunFallbackEl, sunFrom);
  g_atmoLine = line;
  clog(L"atmosphere: %s", line);
}

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
  clog(L"  dump #%u %s %s %s: vp %lu,%lu %lux%lu z %.2f..%.2f %s 0x%lx%s blend %lu %lu/%lu z %lu zw %lu at %lu/%lu ps %s%s; t0 %s; t1 %s",
       g_dumpDraws, fs::path(key->name).filename().c_str(), g_primaryRT ? L"P" : L"O", fate, vp.X, vp.Y, vp.Width, vp.Height,
       vp.MinZ, vp.MaxZ, g_vin == kVinDecl ? L"decl" : L"fvf", g_vin == kVinDecl ? reinterpret_cast<uintptr_t>(g_curDecl) : fvf,
       screen_space_input(dev) ? L" (screen-space)" : L"", blend, src, dst,
       z, zw, at, aref, ps ? L"yes" : L"no", g_oqActive > 0 ? L" IN-OCCLUSION-QUERY" : L"", t0, t1);
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
  if (worldSpace && !g_frameCam) {                     // first world-space draw of the frame defines the camera
    DMat V, P, vpinv;
    double rec = 0;
    const DMat vp = to_d(wvp);
    if (decompose_vp(vp, V, P, rec) && inverse_d(vp, vpinv)) {
      g_V = to_f(V); g_P = to_f(P); g_VPinv = vpinv;
      g_haveCam = true;
      g_frameCam = true;
      g_st.recErr = static_cast<float>(rec);
    }
  }
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
    const float err = max_diff(world, exact);
    if (!btbFrame) g_st.worldErr = (std::max)(g_st.worldErr, err);
    if (err < 0.05f || btbFrame) world = exact;
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
    g_haveCar = true;
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
       L"scenes %u, world-check max err %.5f, world-space dev %.5f, VP split err %.6f, comp %s, sky mode %d (%u sky draws, split err %.5f, camera off-centre %.2f), windscreen %s (%u skipped), %u screen-space, "
       L"UI trigger %s at draw %u (occlusion queries open then %u, now %d), %s",
       g_frame, why, g_st.draws, g_st.known, g_st.unknown, g_st.ff, g_st.offscreen, g_st.btb, g_st.btbSkipped, g_st.btbInstanced, g_st.btbCopies, g_st.btbRebuilt, g_st.btbCamRejected, g_btbShift[0], g_btbShift[1], g_btbShift[2], g_st.xformed, g_st.noCamera,
       g_st.scenes, g_st.worldErr, g_st.rlDev, g_st.recErr, g_enabled ? L"ON" : L"OFF", g_skyMode,
       g_st.skyDraws, g_st.skyErr, g_st.skyCamOff, g_glassOn ? L"on" : L"off", g_st.glassSkipped, g_st.screenSpace, g_uiTriggerOn ? L"on" : L"off",
       g_st.uiTriggerAt, g_st.oqAtUi, g_oqActive, kTestModes[g_testMode]);
  clog(L"  foliage mode %d (cut-out ALPHAREF %lu): %u draws with lowered ALPHAREF, %u soft passes skipped; %u mid-scene screen quads skipped; dust %u%% (%u batches trimmed, %.0f quads drawn per frame); off-screen scenery %s (%.0f draws skipped per frame) since last line",
       g_foliageMode, g_cutoutRef, g_refLowered, g_softSkipped, g_midQuadSkipped, g_dustKeep, g_dustTrimmed,
       g_dustFrames ? static_cast<double>(g_dustQuads) / g_dustFrames : 0.0, g_skipOffscreen ? L"skipped" : L"drawn",
       g_dustFrames ? static_cast<double>(g_offSkipped) / g_dustFrames : 0.0);
  clog(L"  fixed-function scenery now mode %d (%s); since last line %.0f draws per frame without their vertex shader, %.0f of them swaying ones beyond %.0f m",
       g_ffMode, g_ffMode == 0 ? L"off, vertex capture" : g_ffMode == 1 ? L"static RL scenery" : L"static RL scenery + far swaying foliage",
       g_dustFrames ? static_cast<double>(g_ffDraws) / g_dustFrames : 0.0,
       g_dustFrames ? static_cast<double>(g_ffSwayFar) / g_dustFrames : 0.0, g_ffSwayNear);
  clog(L"  opaque cut-outs %s: %.0f alpha-tested draws per frame drawn without alpha test (texture never below ALPHAREF)",
       g_opaqueAlpha ? L"ON" : L"off", g_dustFrames ? static_cast<double>(g_opaqueDraws) / g_dustFrames : 0.0);
  clog(L"  car dirt decals %s: %.0f per frame (%u with an untagged render-target dirt map) since last line",
       g_dirtOn ? L"ON" : L"off", g_dustFrames ? static_cast<double>(g_dirtDraws) / g_dustFrames : 0.0, g_dirtUntagged);
  g_dirtDraws = g_dirtUntagged = 0;
  {
    unsigned held = 0, twins = 0;
    double mb = 0;
    rbrrtx_layer_stats(&held, &twins, &mb);
    clog(L"  ground second layer %s: %.0f per frame (%u without twin or UV set, %u untagged); %u sources held, %u twins, %.0f MB",
         g_layerOn ? L"ON" : L"off", g_dustFrames ? static_cast<double>(g_layerDraws) / g_dustFrames : 0.0,
         g_layerMissing, g_layerUntagged, held, twins, mb);
    g_layerDraws = g_layerMissing = g_layerUntagged = 0;
  }
  if (g_clockChanges) clog(L"  start clock digits: %u changes drawn as new meshes since last line", g_clockChanges);
  g_clockChanges = 0;
  wchar_t acLine[400];
  rbrrtx_alphacut_summary(acLine, 400, g_dustFrames);
  clog(L"  alpha-card cut-outs %s: %s", g_alphaCut ? L"ON" : L"off", acLine);
  g_refLowered = g_softSkipped = g_midQuadSkipped = g_dustTrimmed = g_dustQuads = g_dustFrames = g_offSkipped = g_ffDraws = g_ffSwayFar = 0;
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
};
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
    if (inStep >= g_expSettle) {
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
  if (kDust) {                                         // dust 100 -> 0 -> 25 -> 50
    g_dustKeep = g_dustKeep == 100 ? 0 : g_dustKeep == 0 ? 25 : g_dustKeep == 25 ? 50 : 100;
    swprintf_s(why, L"%s: dust %u%%", rbrrtx_key_name(RBRKEY_DUST), g_dustKeep);
    log_frame(why);
  }
  if (kSky) {                                          // sky mode 2 <-> 1
    g_skyMode = g_skyMode == 2 ? 1 : 2;
    swprintf_s(why, L"%s: %s", rbrrtx_key_name(RBRKEY_SKY), g_skyMode == 2 ? L"sky mode 2 (Remix sky)" : L"sky mode 1 (skip)");
    log_frame(why);
  }
  ++g_dustFrames;
  if (kBrighter || kDimmer) {                          // sun brightness live (sun_brighter / sun_dimmer), game or debug sun
    g_sunScale = std::clamp(g_sunScale * (kBrighter ? 1.1f : 1.0f / 1.1f), 0.05f, 20.0f);
    g_sunFade = -1.0f;                                 // forces the light to be re-created with the new radiance
    swprintf_s(why, L"%s: sun brightness x%.2f = radiance (%.2f, %.2f, %.2f); keep it: -SunRadiance %.2f,%.2f,%.2f",
               rbrrtx_key_name(kBrighter ? RBRKEY_SUN_BRIGHTER : RBRKEY_SUN_DIMMER), g_sunScale,
               g_sunRadiance[0] * g_sunScale, g_sunRadiance[1] * g_sunScale, g_sunRadiance[2] * g_sunScale,
               g_sunRadiance[0] * g_sunScale, g_sunRadiance[1] * g_sunScale, g_sunRadiance[2] * g_sunScale);
    log_frame(why);
  }
  if (kDebugSun) {                                     // manual debug sun on/off (g_sunManual)
    g_sunManual = !g_sunManual;
    if (g_sunManual && g_sunDir[1] != 0.0f) {          // start from the sun currently drawn
      g_sunEl = std::asin(std::clamp(-g_sunDir[1], -1.0f, 1.0f)) * 57.2958f;
      g_sunAz = std::atan2(g_sunDir[2], g_sunDir[0]) * 57.2958f;
      // g_sunDir is the Remix sun after sun_rule (never overhead any more).
    }
    swprintf_s(why, L"%s: %s sun (azimuth %.1f, elevation %.1f)", rbrrtx_key_name(RBRKEY_DEBUG_SUN), g_sunManual ? L"manual" : L"game", g_sunAz, g_sunEl);
    log_frame(why);
  }
  if (g_sunManual) {
    const float dEl = (rbrrtx_key_held(RBRKEY_SUN_UP) ? 0.5f : 0.0f) - (rbrrtx_key_held(RBRKEY_SUN_DOWN) ? 0.5f : 0.0f);
    const float dAz = (rbrrtx_key_held(RBRKEY_SUN_RIGHT) ? 0.5f : 0.0f) - (rbrrtx_key_held(RBRKEY_SUN_LEFT) ? 0.5f : 0.0f);
    const bool moving = dEl != 0.0f || dAz != 0.0f;
    g_sunEl = std::clamp(g_sunEl + dEl, -18.0f, 90.0f);
    g_sunAz = std::fmod(g_sunAz + dAz + 540.0f, 360.0f) - 180.0f;
    if (!moving && g_sunMoving) {                      // log where the sun was left
      swprintf_s(why, L"debug sun moved to azimuth %.1f, elevation %.1f%s", g_sunAz, g_sunEl,
                 g_sunEl <= -4.0f ? L" (night: no sun light)" : g_sunEl < 2.0f ? L" (twilight)" : L"");
      log_frame(why);
    }
    g_sunMoving = moving;
  }
  if (rbrrtx_key_pressed(RBRKEY_SUN_SAVE)) sun_save();
  if (rbrrtx_key_pressed(RBRKEY_GROUND_LAYER)) {
    g_layerOn = !g_layerOn;
    wchar_t msg[64];
    swprintf_s(msg, L"%s: ground second layer %s", rbrrtx_key_name(RBRKEY_GROUND_LAYER), g_layerOn ? L"ON" : L"off");
    log_frame(msg);
  }
  debug_sky();
  g_frameCam = false;
  g_btbPrev = g_st.btb > 0;
  const bool cameraNow = g_st.xformed > 0;
  if (cameraNow && g_detailFrames < 3) { ++g_detailFrames; log_frame(L"first frames with a camera"); }
  else if (g_frame < 3 || g_frame % 600 == 0) { log_frame(L"periodic"); cull_log_stats(); }
  rbrrtx_report_frame(g_wnd, { g_st.draws, g_st.xformed, g_st.ff, g_st.btb, { g_carPos[0], g_carPos[1], g_carPos[2] } });
  g_st = {};
  g_hinted = 0;
  g_byShader.clear();
  ++g_frame;
  rbrrtx_alphacut_frame(g_frame);
  g_sunDrawn = false;
  g_uiTriggered = false;
}

// ---------------------------------------------------------------- device hooks

HRESULT STDMETHODCALLTYPE hk_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count) {
  if (data && start < 256 && !g_recording) {
    const UINT n = (std::min)(count, 256u - start);
    std::memcpy(g_c[start], data, n * 4 * sizeof(float));
    for (UINT i = start; i < start + n; ++i) g_cset[i] = true;
  }
  return o.SetVertexShaderConstantF(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE hk_SetPixelShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count) {
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
  if (g_recording) { g_rec.hasVin = true; g_rec.vin = kVinDecl; g_rec.decl = decl; g_rec.fvf = 0; }
  else { g_curDecl = decl; g_vin = kVinDecl; }
  return o.SetVertexDeclaration(dev, decl);
}

HRESULT STDMETHODCALLTYPE hk_SetFVF(IDirect3DDevice9* dev, DWORD fvf) {
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
  if (g_recording) return o.SetTextureStageState(dev, stage, type, v);
  if (stage == 0)
    for (int i = 0; i < 6; ++i)
      if (type == kTss[i]) g_tss0[i] = v;
  return o.SetTextureStageState(dev, stage, type, v);
}

HRESULT STDMETHODCALLTYPE hk_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD v) {
  if (state == D3DRS_LIGHTING && !g_recording) g_lighting = v;
  return o.SetRenderState(dev, state, v);
}

HRESULT STDMETHODCALLTYPE hk_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
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
  return o.SetRenderTarget(dev, index, rt);
}

HRESULT STDMETHODCALLTYPE hk_BeginScene(IDirect3DDevice9* dev) {
  const HRESULT hr = o.BeginScene(dev);
  ++g_st.scenes;
  if (SUCCEEDED(hr) && !g_sunDrawn) { g_sunDrawn = true; update_sun(); }
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
  if (g_sun) {
    const ULONG refs = o.AddRef(dev);
    o.Release(dev);
    if (refs == 2) destroy_sun(L"device released");
  }
  return o.Release(dev);
}

HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
  end_frame(dev);
  return o.Present(dev, src, dst, wnd, dirty);
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
  const HRESULT hr = o.Reset(dev, pp);
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
bool cull_draw(IDirect3DDevice9* dev) {
  if (!g_primaryRT || !g_enabled || !g_haveCam || !g_curVS || g_drawNumVert == 0 || !cull_class()) return false;
  CullEntry* e = cull_entry(dev);
  if (!e) return false;
  float c[3] = { e->c[0], e->c[1], e->c[2] };
  float r = e->r;
  if (!(g_curInfo->flags & SH_WORLDSPACE)) {               // object space -> world with this draw's W
    const D3DMATRIX w = to_f(mul_d(to_d(from_rows(&g_c[0])), g_VPinv));
    const float x = c[0], y = c[1], z = c[2];
    for (int k = 0; k < 3; ++k) c[k] = x * w.m[0][k] + y * w.m[1][k] + z * w.m[2][k] + w.m[3][k];
    const float sx = std::sqrt(w.m[0][0] * w.m[0][0] + w.m[0][1] * w.m[0][1] + w.m[0][2] * w.m[0][2]);
    const float sy = std::sqrt(w.m[1][0] * w.m[1][0] + w.m[1][1] * w.m[1][1] + w.m[1][2] * w.m[1][2]);
    const float sz = std::sqrt(w.m[2][0] * w.m[2][0] + w.m[2][1] * w.m[2][1] + w.m[2][2] * w.m[2][2]);
    r *= (std::max)(sx, (std::max)(sy, sz));
  }
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
  swprintf_s(s, L"proxy: comp %s, FF scenery mode %d (sway near %.0f m), foliage mode %d (ALPHAREF %lu), dust %u %%, sky mode %d, "
                L"windscreen %s, culling %s, transform mode %d\n",
             g_enabled ? L"ON" : L"off", g_ffMode, g_ffSwayNear, g_foliageMode, g_cutoutRef, g_dustKeep, g_skyMode,
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
  r += L"\nFiles: stage.txt (stage, weather, camera, sun, proxy modes), draws.tsv (every draw of one frame: shader, fate, "
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
  static bool baseRead = false;
  if (!baseRead) { baseRead = true; g_decalBase = conf_string("rtx.decalTextures"); }
  std::string list = g_decalBase;
  char b[24];
  for (unsigned long long h : g_dirtHashes) {
    sprintf_s(b, "0x%016llX", h);
    list += (list.empty() ? "" : ", ") + std::string(b);
  }
  remix_set("rtx.decalTextures", list.c_str());
  clog(L"%s: %016llX tagged as decal (%zu decal textures added)", what, hx, g_dirtHashes.size());
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
// declaration with TEXCOORD0/1 swapped so stage 0 and Remix both read t1's UV set. RBRRTX_GROUND_LAYER=0 = off.
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

template <class Draw>
void layer_draw(IDirect3DDevice9* dev, Draw draw) {
  if (!g_layerOn || !g_enabled || !g_primaryRT || !g_vsUnbound || !g_curInfo || g_vin != kVinDecl ||
      fs::path(g_curInfo->name).filename().wstring().rfind(L"RLDouble", 0) != 0) return;
  IDirect3DBaseTexture9* t1 = nullptr;
  dev->lpVtbl->GetTexture(dev, 1, &t1);
  if (!t1) return;
  IDirect3DBaseTexture9* twin = static_cast<IDirect3DBaseTexture9*>(rbrrtx_layer_twin(t1));
  t1->lpVtbl->Release(t1);
  IDirect3DVertexDeclaration9* decl = nullptr;
  dev->lpVtbl->GetVertexDeclaration(dev, &decl);
  IDirect3DVertexDeclaration9* swapped = decl ? uv_swapped(dev, decl) : nullptr;
  if (!twin || !swapped) { ++g_layerMissing; if (decl) decl->lpVtbl->Release(decl); return; }
  const unsigned long long hx = rbrrtx_texture_hash(twin);
  if (hx) decal_tag(hx, L"ground second layer"); else ++g_layerUntagged;
  static const D3DRENDERSTATETYPE rs[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
                                           D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE };
  static const D3DTEXTURESTAGESTATETYPE ts[] = { D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX };
  DWORD rsOld[6], tsOld[4], ts1Color = 0, ts1Alpha = 0;
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
  for (int i = 0; i < 6; ++i) o.SetRenderState(dev, rs[i], rsNew[i]);
  for (int i = 0; i < 4; ++i) o.SetTextureStageState(dev, 0, ts[i], tsNew[i]);   // colour ops stay the game's
  o.SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  o.SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->lpVtbl->SetTexture(dev, 0, twin);
  dev->lpVtbl->SetPixelShader(dev, nullptr);
  o.SetVertexDeclaration(dev, swapped);
  draw();
  ++g_layerDraws;
  o.SetVertexDeclaration(dev, decl);
  dev->lpVtbl->SetPixelShader(dev, ps);
  dev->lpVtbl->SetTexture(dev, 0, t0);
  o.SetTextureStageState(dev, 1, D3DTSS_COLOROP, ts1Color);
  o.SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, ts1Alpha);
  for (int i = 0; i < 4; ++i) o.SetTextureStageState(dev, 0, ts[i], tsOld[i]);
  for (int i = 0; i < 6; ++i) o.SetRenderState(dev, rs[i], rsOld[i]);
  if (ps) ps->lpVtbl->Release(ps);
  if (t0) t0->lpVtbl->Release(t0);
  decl->lpVtbl->Release(decl);
}

// All draws: glass skip (g_glassOn), sky handling (see g_skyMode), then transforms for Remix (before_draw/after_draw).
template <class Draw>
HRESULT comp_draw(IDirect3DDevice9* dev, Draw draw) {
  const bool glassSkip = !g_glassOn && is_glass_draw();
  const bool sky = is_sky_draw();
  const bool midQuad = !glassSkip && !sky && is_midscene_quad(dev);
  const wchar_t* fate = glassSkip ? L"SKIPPED(glass)" : midQuad ? L"SKIPPED(mid-scene quad)" : sky ? (g_skyMode == 1 ? L"SKIPPED(sky)" : L"sky") : L"drawn";
  if (g_dumpFrames) dump_draw(dev, fate);
  if (g_sweepTsv) sweep_dump_draw(dev, exp_skip_draw() ? L"SKIPPED(step)" : fate);
  if (glassSkip) { ++g_st.glassSkipped; return D3D_OK; }
  if (g_skipOffscreen && g_enabled && !g_primaryRT && g_curInfo && (g_curInfo->flags & (SH_SCENERY | SH_SKY))) {
    ++g_offSkipped;
    return D3D_OK;
  }
  if (midQuad) { ++g_midQuadSkipped; return D3D_OK; }
  if (exp_skip_draw()) { ++g_expSkipped; return D3D_OK; }   // performance experiment (key K)
  if (cull_draw(dev)) return D3D_OK;                         // distance culling (culling.ini, key M)
  g_primCap = UINT_MAX;
  if (g_enabled && g_primaryRT && g_curInfo && (g_curInfo->flags & SH_PARTICLE)) {
    DWORD at = 0, blend = 0;
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
    if (at && blend) {
      if (g_dustKeep == 0) { ++g_dustTrimmed; return D3D_OK; }
      if (g_dustKeep < 100) {
        g_primCap = (std::max)(2u, g_pendingPrims * g_dustKeep / 100 / 2 * 2);   // whole quads (2 triangles each)
        if (g_primCap < g_pendingPrims) ++g_dustTrimmed;
      }
      g_dustQuads += (std::min)(g_primCap, g_pendingPrims) / 2;
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
  HRESULT hr = D3D_OK;
  if (cut) hr = alphacut_draw(dev, ac);
  else if (!clock_draw(dev, hr) && !btb_instanced_draw(dev, hr)) hr = draw();
  if (SUCCEEDED(hr)) { if (ff) layer_draw(dev, draw); else dirt_draw(dev, draw); }
  if (normalsOff) o.SetVertexDeclaration(dev, static_cast<IDirect3DVertexDeclaration9*>(g_curDecl));
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
  g_pendingPrims = count;
  g_drawFirstVert = start;
  g_drawNumVert = verts_for(t, count);
  g_dip.valid = false;
  return comp_draw(dev, [&] { return o.DrawPrimitive(dev, t, start, cap_prims(count)); });
}

HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minIdx,
                                                  UINT numV, UINT startIdx, UINT count) {
  g_pendingPrims = count;
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
  g_pendingPrims = count;
  g_drawNumVert = 0;                                   // no vertex buffer: not culled
  g_dip.valid = false;
  return comp_draw(dev, [&] { return o.DrawPrimitiveUP(dev, t, cap_prims(count), data, stride); });
}

HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minIdx, UINT numV,
                                                    UINT count, const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
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
    rbrrtx_alphacut_forget(*vb);
    if (*vb == g_clockVB) g_clockVB = nullptr;         // a new buffer at the clock's old address
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateIndexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                               IDirect3DIndexBuffer9** ib, HANDLE* shared) {
  const HRESULT hr = o.CreateIndexBuffer(dev, len, usage, fmt, pool, ib, shared);
  if (SUCCEEDED(hr) && ib && *ib) rbrrtx_alphacut_forget(*ib);
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
  init_remix_api();
  clog(L"device hooks installed (comp %s, sun %s)", g_enabled ? L"on" : L"off", g_apiOk ? L"on" : L"off");
  return hr;
}

}  // namespace

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
    g_layerOn = rbrrtx_env_flag(L"RBRRTX_GROUND_LAYER", true);
    g_clockOn = rbrrtx_env_flag(L"RBRRTX_CLOCK", true);
    g_ffNormals = rbrrtx_env_flag(L"RBRRTX_FF_NORMALS", false);
    g_uiTriggerOn = rbrrtx_env_flag(L"RBRRTX_UI_TRIGGER", true);
    g_skipOffscreen = !rbrrtx_env_flag(L"RBRRTX_OFFSCREEN", false);
    wchar_t fol[16];
    if (GetEnvironmentVariableW(L"RBRRTX_FOLIAGE", fol, 16)) g_foliageMode = std::clamp(_wtoi(fol), 0, 2);
    if (GetEnvironmentVariableW(L"RBRRTX_DUST", fol, 16)) g_dustKeep = static_cast<unsigned>(std::clamp(_wtoi(fol), 0, 100));
    if (GetEnvironmentVariableW(L"RBRRTX_FF", fol, 16)) g_ffMode = g_ffBase = std::clamp(_wtoi(fol), 0, 2);
    g_opaqueAlpha = g_opaqueBase = rbrrtx_env_flag(L"RBRRTX_OPAQUE_ALPHA", true) ? 1 : 0;
    g_alphaCut = g_alphaCutBase = rbrrtx_env_flag(L"RBRRTX_ALPHACUT", true) ? 1 : 0;
    rbrrtx_alphacut_init();
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
