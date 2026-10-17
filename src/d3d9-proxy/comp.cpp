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
#include <string>
#include <unordered_map>
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

enum : unsigned { SH_KNOWN = 1, SH_WORLDSPACE = 2, SH_USES_C8 = 4, SH_CAR = 8, SH_SKY = 16, SH_GLASS = 32, SH_SCENERY = 64, SH_PARTICLE = 128 };

struct ShaderInfo {
  std::wstring name;
  unsigned flags = 0;
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

bool starts_with(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

unsigned flags_for(const std::wstring& rel) {
  const std::wstring n = fs::path(rel).filename().wstring();
  unsigned f = SH_KNOWN;
  if (starts_with(n, L"RL") || starts_with(n, L"Water")) f |= SH_WORLDSPACE;   // vertices already in world space
  if (starts_with(n, L"Car")) f |= SH_CAR;
  if (starts_with(n, L"RL") || starts_with(n, L"IO")) f |= SH_SCENERY;   // stage geometry and objects (foliage, fences)
  if (starts_with(n, L"Clouds") || starts_with(n, L"SkyDome")) f |= SH_SKY;
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
  decltype(IDirect3DQuery9Vtbl::Issue) QueryIssue;
  decltype(IDirect3DQuery9Vtbl::Release) QueryRelease;
} o{};

HMODULE g_backend = nullptr;
std::unordered_map<unsigned long long, std::wstring> g_vso;   // bytecode hash -> path under VertexShaders
std::unordered_map<void*, ShaderInfo> g_vs;                     // created vertex shader -> identity

float g_c[256][4] = {};
bool g_cset[256] = {};
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
void* g_curDecl = nullptr;
DWORD g_curFVF = 0;
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
// Debug sun (user 2026-10-16, judging normal maps needs low, raking light): Alt+Home switches to a manual sun that
// starts where the game's sun is; while it is on, hold Alt+Page Up/Page Down to raise/lower it and Alt+Insert/End to
// turn it around (0.5 degrees per frame). Alt+Home again = the game's sun. Only the light moves, not the sky picture.
bool g_sunManual = false, g_sunMoving = false;
float g_sunAz = 0.0f, g_sunEl = 30.0f;                 // degrees; elevation above the horizon

struct Stats {
  unsigned draws, known, unknown, ff, offscreen, xformed, noCamera, scenes, skyDraws, glassSkipped, screenSpace;
  unsigned uiTriggerAt, oqAtUi;                        // draw number of our UI trigger (0 = none); open queries then
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
// was lit, 2026-10-15). Modes (RBRRTX_SKY = 0..2; the sky key (N) toggles 2 <-> 1 live; primary target only):
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
DWORD g_cutoutRef = 96;
unsigned g_softSkipped = 0, g_refLowered = 0, g_midQuadSkipped = 0;   // g_midQuadSkipped: is_midscene_quad

// Dust/smoke (Valstagna replay 05:57, user): RBR draws gravel dust as Particle.vso batches of alpha-tested, blended
// sprites. Path traced, hundreds of stacked translucent quads cost a lot and fill the screen as a brown wall (onboard
// replays at slow hairpins). Only the first g_dustKeep percent of each batch's triangles is drawn (rounded to whole
// quads). RBRRTX_DUST=<percent> (100 = all, 0 = none); the dust key (U) cycles them live.
// Run 06:19 (user): with the sprites tagged as Remix particles (rtx.conf) full dust looked best; 15% left a few flat
// sprite planes trailing the car. Run 16:52 (user): full dust is far too expensive (the test install has ParticleMod by
// Kahoo: many fine sprites); wanted: cheap and not too dense. ParticleQuality = low (the 2004 PS_*_LOW.ini) drew no
// dust at all under Remix (run 18:26, Alt+Pause had nothing to trim), so ParticleMod's sprites. Trimming keeps the
// first part of each batch, which in a TV-cam replay (run 18:41) cut the trail into dust / gap / dust instead of
// thinning it, so the default is 100 again until thinning is spread evenly. Dust key (U): 100 -> 0 -> 25 -> 50.
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

void update_sun() {
  if (!g_apiOk || !g_enabled || !g_sunOn || (!g_cset[41] && !g_sunManual)) return;
  float d[3] = { g_c[41][0], g_c[41][1], g_c[41][2] };  // direction the light travels: y < 0 = sun above the horizon
  if (g_sunManual) {
    const float el = g_sunEl * 0.0174533f, az = g_sunAz * 0.0174533f;
    d[0] = std::cos(el) * std::cos(az);
    d[1] = -std::sin(el);
    d[2] = std::cos(el) * std::sin(az);
  }
  const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (len < 1e-4f) return;
  for (float& v : d) v /= len;
  const float cosDelta = d[0] * g_sunDir[0] + d[1] * g_sunDir[1] + d[2] * g_sunDir[2];
  if (!g_sun || cosDelta < 0.99999f) {                 // ~0.25 degrees: lights are immutable, so recreate
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
    light.radiance = { g_sunRadiance[0], g_sunRadiance[1], g_sunRadiance[2] };
    const remixapi_ErrorCode err = g_api.CreateLight(&light, &g_sun);
    std::memcpy(g_sunDir, d, sizeof(d));
    if (++g_sunCreates <= 20 || err != REMIXAPI_ERROR_CODE_SUCCESS)
      clog(L"sun: %s light, direction (%.3f, %.3f, %.3f), radiance (%.2f, %.2f, %.2f), error %d",
           g_sunCreates == 1 ? L"created" : L"re-created", d[0], d[1], d[2],
           g_sunRadiance[0], g_sunRadiance[1], g_sunRadiance[2], static_cast<int>(err));
    if (err != REMIXAPI_ERROR_CODE_SUCCESS) { g_sun = nullptr; return; }
  }
  g_api.DrawLightInstance(g_sun);
}

void destroy_sun(const wchar_t* why);

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
  clog(L"  dump #%u %s %s %s: vp %lu,%lu %lux%lu z %.2f..%.2f fvf 0x%lx%s blend %lu %lu/%lu z %lu zw %lu at %lu/%lu ps %s%s; t0 %s; t1 %s",
       g_dumpDraws, fs::path(key->name).filename().c_str(), g_primaryRT ? L"P" : L"O", fate, vp.X, vp.Y, vp.Width, vp.Height,
       vp.MinZ, vp.MaxZ, fvf, (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? L" (XYZRHW screen-space)" : L"", blend, src, dst,
       z, zw, at, aref, ps ? L"yes" : L"no", g_oqActive > 0 ? L" IN-OCCLUSION-QUERY" : L"", t0, t1);
  if (g_curVS && !g_curInfo) {                         // unknown vertex shader: which one, and what it is fed
    auto it = g_curDecl ? g_declText.find(g_curDecl) : g_declText.end();
    clog(L"    vs %p decl %s", g_curVS, it != g_declText.end() ? it->second.c_str() : L"(fvf)");
  }
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
  if (zw) o.SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
  if (cw != 0xF) o.SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
  if (fog) o.SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
  if (ps) dev->lpVtbl->SetPixelShader(dev, nullptr);
  o.SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  o.DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
  if (fvf) o.SetFVF(dev, fvf);
  else if (decl) o.SetVertexDeclaration(dev, decl);
  dev->lpVtbl->SetStreamSource(dev, 0, vb, vbOffset, vbStride);   // not hooked
  if (vb) vb->lpVtbl->Release(vb);
  if (decl) decl->lpVtbl->Release(decl);
  if (ps) { dev->lpVtbl->SetPixelShader(dev, ps); ps->lpVtbl->Release(ps); }
  if (fog) o.SetRenderState(dev, D3DRS_FOGENABLE, fog);
  if (cw != 0xF) o.SetRenderState(dev, D3DRS_COLORWRITEENABLE, cw);
  if (zw) o.SetRenderState(dev, D3DRS_ZWRITEENABLE, zw);
  g_st.uiTriggerAt = g_st.draws;
  g_st.oqAtUi = static_cast<unsigned>(g_oqActive);
}

// Returns true when the draw was given a material hint that after_draw must undo.
bool before_draw(IDirect3DDevice9* dev) {
  ++g_st.draws;
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
    DWORD fvf = 0;
    dev->lpVtbl->GetFVF(dev, &fvf);
    const bool screenSpace = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    for (int i = 0; i < 3; ++i)
      if (screenSpace) { g_devValid[i] = false; set_xf(dev, i, identity()); }   // uncached: suspect b)
      else if (g_gameSet[i]) set_xf(dev, i, g_game[i]);
    if (screenSpace) {
      ++g_st.screenSpace;
      // Only a textured quad counts as the start of the HUD: RBR also draws an untextured additive screen-space quad in
      // the middle of the 3D scene (Zlin chase-cam dump 05:52, #1155 between the car and the water). Triggering there
      // made Remix rasterize every later draw, so the car body lost ray tracing in chase cameras (d3d9_rtx.cpp:573).
      IDirect3DBaseTexture9* t0 = nullptr;
      dev->lpVtbl->GetTexture(dev, 0, &t0);
      const bool textured = t0 != nullptr;
      if (t0) t0->lpVtbl->Release(t0);
      if (g_uiTriggerOn && !g_uiTriggered && textured && g_primaryRT && g_enabled && g_st.xformed > 0) { g_uiTriggered = true; trigger_ui(dev); }
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
  if (g_curInfo && (g_curInfo->flags & SH_USES_C8) && all_set(8, 4)) {
    // The shader also gets the exact world matrix in c8-c11: use it when it agrees with the derived one.
    const D3DMATRIX exact = from_rows(&g_c[8]);
    const float err = max_diff(world, exact);
    g_st.worldErr = (std::max)(g_st.worldErr, err);
    if (err < 0.05f) world = exact;
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
  D3DMATRIX vinv = {};
  const bool okInv = g_haveCam && inverse(g_V, vinv);
  const float fovY = g_haveCam && g_P._22 != 0 ? 2.0f * std::atan(1.0f / std::fabs(g_P._22)) * 57.29578f : 0.0f;
  clog(L"frame %u (%s): draws %u [known %u, unknown %u, fixed-function %u, offscreen %u], transformed %u, no-camera %u, "
       L"scenes %u, world-check max err %.5f, world-space dev %.5f, VP split err %.6f, comp %s, sky mode %d (%u sky draws, split err %.5f, camera off-centre %.2f), windscreen %s (%u skipped), %u screen-space, "
       L"UI trigger %s at draw %u (occlusion queries open then %u, now %d), %s",
       g_frame, why, g_st.draws, g_st.known, g_st.unknown, g_st.ff, g_st.offscreen, g_st.xformed, g_st.noCamera,
       g_st.scenes, g_st.worldErr, g_st.rlDev, g_st.recErr, g_enabled ? L"ON" : L"OFF", g_skyMode,
       g_st.skyDraws, g_st.skyErr, g_st.skyCamOff, g_glassOn ? L"on" : L"off", g_st.glassSkipped, g_st.screenSpace, g_uiTriggerOn ? L"on" : L"off",
       g_st.uiTriggerAt, g_st.oqAtUi, g_oqActive, kTestModes[g_testMode]);
  clog(L"  foliage mode %d (cut-out ALPHAREF %lu): %u draws with lowered ALPHAREF, %u soft passes skipped; %u mid-scene screen quads skipped; dust %u%% (%u batches trimmed, %.0f quads drawn per frame); off-screen scenery %s (%.0f draws skipped per frame) since last line",
       g_foliageMode, g_cutoutRef, g_refLowered, g_softSkipped, g_midQuadSkipped, g_dustKeep, g_dustTrimmed,
       g_dustFrames ? static_cast<double>(g_dustQuads) / g_dustFrames : 0.0, g_skipOffscreen ? L"skipped" : L"drawn",
       g_dustFrames ? static_cast<double>(g_offSkipped) / g_dustFrames : 0.0);
  g_refLowered = g_softSkipped = g_midQuadSkipped = g_dustTrimmed = g_dustQuads = g_dustFrames = g_offSkipped = 0;
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
}

// ---------------------------------------------------------------- performance experiments (key K)
// Where does the frame time go (user 2026-10-17: trees take 85 -> 60 fps)? One press of K runs the steps of
// rbr-rtx\config\experiments.ini (env RBRRTX_EXPERIMENTS) while the player keeps the view still: each step sets Remix
// options live (SetConfigVariable, user layer, applied at frame end) and/or hides draws whose shader file name
// contains a listed part ("skip = Sway" = swaying foliage), waits settle_seconds, then averages the frame time over
// measure_seconds. Baseline first and last (drift check); the log gets one line per step and a summary. K again =
// abort. Afterwards every option is back at its [baseline] value. Options touched here land in the same layer as the
// Alt+X user settings: do not "Save Settings" during a run.
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
};
std::vector<ExpStep> g_exp;                                  // [0] = baseline (no changes), then the ini steps
std::vector<std::pair<std::string, std::string>> g_expBase;  // [baseline] option values, restored around every step
double g_expSettle = 3.0, g_expMeasure = 5.0;
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

void exp_load() {
  g_exp.clear();
  g_expBase.clear();
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_EXPERIMENTS", L"experiments.ini", path, MAX_PATH * 2);
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
      const std::wstring k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
      if (!_wcsicmp(sec, L"run")) {
        if (k == L"settle_seconds") g_expSettle = (std::max)(0.5, _wtof(v.c_str()));
        if (k == L"measure_seconds") g_expMeasure = (std::max)(1.0, _wtof(v.c_str()));
      } else if (!_wcsicmp(sec, L"baseline")) {
        g_expBase.emplace_back(narrow(k), narrow(v));
      } else if (k == L"name") {
        step.name = v;
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
  if (g_apiOk && g_api.SetConfigVariable) g_api.SetConfigVariable(k.c_str(), v.c_str());
}

void exp_apply(int step) {
  cull_load();                                               // culling back to culling.ini, then the step's cull.* lines
  for (const auto& kv : g_expBase) exp_set(kv.first, kv.second);   // every step starts from the baseline
  g_expSkipCache.clear();
  g_expSolidCache.clear();
  g_expStep = step;
  if (step < 0) return;
  for (const auto& kv : g_exp[step].sets) exp_set(kv.first, kv.second);
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

void exp_finish(const wchar_t* why) {
  exp_apply(-1);
  if (g_expMs.empty()) { clog(L"experiments: %s", why); return; }
  const double base = g_expMs[0];
  clog(L"experiments: %s. Summary (frame time; minus = faster than the first baseline):", why);
  for (size_t i = 0; i < g_expMs.size(); ++i) {
    const ExpStep& s = g_exp[g_expOrder[i]];
    clog(L"  %-44s %6.2f ms %6.1f fps  %+6.2f ms", s.name.c_str(), g_expMs[i], 1000.0 / g_expMs[i], g_expMs[i] - base);
  }
}

void exp_frame() {
  if (rbrrtx_key_pressed(RBRKEY_EXPERIMENT)) {
    if (g_expStep >= 0) { exp_finish(L"aborted (key)"); return; }
    exp_load();
    if (!g_apiOk) clog(L"experiments: Remix API not available, option steps will do nothing (proxy skips still work)");
    g_expOrder.clear();
    g_expMs.clear();
    for (size_t i = 0; i < g_exp.size(); ++i) g_expOrder.push_back(static_cast<int>(i));
    g_expOrder.push_back(0);                                  // baseline again: did the scene drift?
    QueryPerformanceFrequency(&g_qpf);
    g_expPos = 0;
    wchar_t why[160];
    swprintf_s(why, L"%s: experiment run started (%zu steps, about %.0f s; keep the view still)", rbrrtx_key_name(RBRKEY_EXPERIMENT),
               g_expOrder.size(), g_expOrder.size() * (g_expSettle + g_expMeasure));
    log_frame(why);
    exp_apply(g_expOrder[0]);
    return;
  }
  if (g_expStep < 0) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double inStep = static_cast<double>(now.QuadPart - g_expStepStart.QuadPart) / g_qpf.QuadPart;
  if (!g_expMeasuring) {
    if (inStep >= g_expSettle) { g_expMeasuring = true; g_expMeasureStart = now; g_expFrames = 0; g_expSkipped = 0; }
    return;
  }
  ++g_expFrames;
  const double measured = static_cast<double>(now.QuadPart - g_expMeasureStart.QuadPart) / g_qpf.QuadPart;
  if (measured < g_expMeasure || g_expFrames < 10) return;
  const double ms = measured * 1000.0 / g_expFrames;
  g_expMs.push_back(ms);
  const ExpStep& s = g_exp[g_expStep];
  clog(L"experiment %zu/%zu %s: %.2f ms (%.1f fps) over %u frames, %.1f draws hidden, %.1f made solid per frame", g_expPos + 1,
       g_expOrder.size(), s.name.c_str(), ms, 1000.0 / ms, g_expFrames, static_cast<double>(g_expSkipped) / g_expFrames,
       static_cast<double>(g_expSolid) / g_expFrames);
  if (++g_expPos >= g_expOrder.size()) { exp_finish(L"done, options back at baseline"); return; }
  exp_apply(g_expOrder[g_expPos]);
}

void end_frame(IDirect3DDevice9* dev) {
  exp_frame();
  cull_frame_tick();
  if (rbrrtx_key_pressed(RBRKEY_CULLING)) {
    wchar_t w[64];
    swprintf_s(w, L"%s: culling %s", rbrrtx_key_name(RBRKEY_CULLING), cull_toggle() ? L"ON" : L"off");
    log_frame(w);
    cull_log_settings(L"key");
  }
  // Hotkeys (keys.cpp, rbr-rtx\config\keys.ini; defaults P O L J U I N H [ ] ; '). Until 2026-10-17 they were
  // Scroll Lock / Pause with Ctrl/Alt/Shift, which compact keyboards do not have. Every key is read every frame.
  const bool kDump = rbrrtx_key_pressed(RBRKEY_DUMP), kLayer = rbrrtx_key_pressed(RBRKEY_LAYER),
             kSun = rbrrtx_key_pressed(RBRKEY_SUN), kFoliage = rbrrtx_key_pressed(RBRKEY_FOLIAGE),
             kDust = rbrrtx_key_pressed(RBRKEY_DUST), kGlass = rbrrtx_key_pressed(RBRKEY_WINDSCREEN),
             kSky = rbrrtx_key_pressed(RBRKEY_SKY), kDebugSun = rbrrtx_key_pressed(RBRKEY_DEBUG_SUN);
  wchar_t why[128];
  if (g_dumpFrames && --g_dumpFrames == 0) clog(L"frame dump end: %u draws", g_dumpDraws);
  if (kDump) {                                         // dump every draw of the next frame (dump_draw)
    g_dumpFrames = 1;                                  // the whole next frame, ended at its end_frame
    g_dumpDraws = 0;
    swprintf_s(why, L"%s: frame dump of the next frame", rbrrtx_key_name(RBRKEY_DUMP));
    log_frame(why);
  }
  if (kFoliage) {                                      // foliage mode 1 -> 2 -> 0
    g_foliageMode = g_foliageMode == 1 ? 2 : g_foliageMode == 2 ? 0 : 1;
    swprintf_s(why, L"%s: %s", rbrrtx_key_name(RBRKEY_FOLIAGE), g_foliageMode == 1 ? L"foliage mode 1 (low ALPHAREF, soft pass skipped)" : g_foliageMode == 2 ? L"foliage mode 2 (low ALPHAREF, soft pass kept)" : L"foliage mode 0 (game values)");
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
  if (kDebugSun) {                                     // manual debug sun on/off (g_sunManual)
    g_sunManual = !g_sunManual;
    if (g_sunManual && g_sunDir[1] != 0.0f) {          // start from the sun currently drawn
      g_sunEl = std::asin(std::clamp(-g_sunDir[1], -1.0f, 1.0f)) * 57.2958f;
      g_sunAz = std::atan2(g_sunDir[2], g_sunDir[0]) * 57.2958f;
    }
    swprintf_s(why, L"%s: %s sun (azimuth %.1f, elevation %.1f)", rbrrtx_key_name(RBRKEY_DEBUG_SUN), g_sunManual ? L"manual" : L"game", g_sunAz, g_sunEl);
    log_frame(why);
  }
  if (g_sunManual) {
    const float dEl = (rbrrtx_key_held(RBRKEY_SUN_UP) ? 0.5f : 0.0f) - (rbrrtx_key_held(RBRKEY_SUN_DOWN) ? 0.5f : 0.0f);
    const float dAz = (rbrrtx_key_held(RBRKEY_SUN_RIGHT) ? 0.5f : 0.0f) - (rbrrtx_key_held(RBRKEY_SUN_LEFT) ? 0.5f : 0.0f);
    const bool moving = dEl != 0.0f || dAz != 0.0f;
    g_sunEl = std::clamp(g_sunEl + dEl, -10.0f, 90.0f);
    g_sunAz = std::fmod(g_sunAz + dAz + 540.0f, 360.0f) - 180.0f;
    if (!moving && g_sunMoving) {                      // log where the sun was left
      swprintf_s(why, L"debug sun moved to azimuth %.1f, elevation %.1f", g_sunAz, g_sunEl);
      log_frame(why);
    }
    g_sunMoving = moving;
  }
  g_frameCam = false;
  const bool cameraNow = g_st.xformed > 0;
  if (cameraNow && g_detailFrames < 3) { ++g_detailFrames; log_frame(L"first frames with a camera"); }
  else if (g_frame < 3 || g_frame % 600 == 0) { log_frame(L"periodic"); cull_log_stats(); }
  g_st = {};
  g_hinted = 0;
  g_byShader.clear();
  ++g_frame;
  g_sunDrawn = false;
  g_uiTriggered = false;
}

// ---------------------------------------------------------------- device hooks

HRESULT STDMETHODCALLTYPE hk_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* data, UINT count) {
  if (data && start < 256) {
    const UINT n = (std::min)(count, 256u - start);
    std::memcpy(g_c[start], data, n * 4 * sizeof(float));
    for (UINT i = start; i < start + n; ++i) g_cset[i] = true;
  }
  return o.SetVertexShaderConstantF(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE hk_CreateVertexShader(IDirect3DDevice9* dev, const DWORD* fn, IDirect3DVertexShader9** out) {
  const HRESULT hr = o.CreateVertexShader(dev, fn, out);
  if (SUCCEEDED(hr) && fn && out && *out) {
    const size_t size = bytecode_size(fn, 1u << 16);
    const unsigned long long h = fnv1a(fn, size);
    ShaderInfo info;
    auto it = g_vso.find(h);
    if (it != g_vso.end()) { info.name = it->second; info.flags = flags_for(it->second); }
    else info.name = L"(not an RBR .vso)";
    g_vs[*out] = info;
    clog(L"VS %p = %s (%zu bytes, hash %016llx)", static_cast<void*>(*out), info.name.c_str(), size, h);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
  g_curVS = vs;
  g_curInfo = nullptr;
  if (vs) {
    auto it = g_vs.find(vs);
    if (it != g_vs.end() && (it->second.flags & SH_KNOWN)) g_curInfo = &it->second;
  }
  return o.SetVertexShader(dev, vs);
}

HRESULT STDMETHODCALLTYPE hk_CreateVertexDeclaration(IDirect3DDevice9* dev, const D3DVERTEXELEMENT9* e,
                                                     IDirect3DVertexDeclaration9** out) {
  const HRESULT hr = o.CreateVertexDeclaration(dev, e, out);
  if (SUCCEEDED(hr) && out && *out) g_declText[*out] = describe_decl(e);
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_SetVertexDeclaration(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl) {
  g_curDecl = decl;
  return o.SetVertexDeclaration(dev, decl);
}

HRESULT STDMETHODCALLTYPE hk_SetFVF(IDirect3DDevice9* dev, DWORD fvf) {
  g_curDecl = nullptr;
  g_curFVF = fvf;
  return o.SetFVF(dev, fvf);
}

HRESULT STDMETHODCALLTYPE hk_SetTextureStageState(IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD v) {
  if (stage == 0)
    for (int i = 0; i < 6; ++i)
      if (type == kTss[i]) g_tss0[i] = v;
  return o.SetTextureStageState(dev, stage, type, v);
}

HRESULT STDMETHODCALLTYPE hk_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD v) {
  if (state == D3DRS_LIGHTING) g_lighting = v;
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
  for (int i = 0; i < 3; ++i)
    if (state == kXf[i] && m) { g_game[i] = *m; g_gameSet[i] = true; g_dev[i] = *m; g_devValid[i] = true; }
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
  DWORD fvf = 0;
  dev->lpVtbl->GetFVF(dev, &fvf);
  if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) return false;
  IDirect3DBaseTexture9* t0 = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &t0);
  if (t0) { t0->lpVtbl->Release(t0); return false; }
  return true;
}

// ---------------------------------------------------------------- distance culling (culling.ini, key M, K steps)
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
  float visibleFar = 0.0f;                     // inside the view and farther than this: culled (0 = never)
  float keepRadius = 30.0f;                    // never cull within this distance of the car
  float hysteresis = 0.1f;
  float frustumMargin = 5.0f;                  // metres added to the sphere for the view test
};
CullCfg g_cull;
struct CullEntry { float c[3] = {}, r = 0; bool valid = false, culled = false; unsigned lastFrame = 0; };
std::unordered_map<uint64_t, CullEntry> g_cullCache;
std::unordered_map<const void*, unsigned> g_vbGen;          // bumped when the game creates a buffer at an address
std::unordered_map<const ShaderInfo*, bool> g_cullClassCache;
UINT g_drawFirstVert = 0, g_drawNumVert = 0;                 // set by the draw hooks (0 = not cullable, e.g. UP draws)
struct CullStats { unsigned considered = 0, offscreen = 0, visibleFar = 0, kept = 0, unreadable = 0, misses = 0;
                   unsigned dist[5] = {}; } g_cs;               // dist buckets: <50, <100, <200, <400, >=400 m
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
  clog(L"culling (%s): %s, classes %s, off-screen beyond %.0f m, visible beyond %s, keep radius %.0f m, hysteresis %.2f",
       why, g_cull.enabled ? L"ON" : L"off", cls.c_str(), g_cull.offscreenFar,
       g_cull.visibleFar > 0 ? std::to_wstring(static_cast<int>(g_cull.visibleFar)).append(L" m").c_str() : L"never",
       g_cull.keepRadius, g_cull.hysteresis);
}

void cull_log_stats() {
  if (!g_csFrames || !g_cs.considered) { g_cs = {}; g_csFrames = 0; return; }
  const double n = g_csFrames;
  clog(L"  culling %s: per frame %.1f considered, %.1f culled off-screen, %.1f culled visible-far, %.1f kept near car; "
       L"distances <50/<100/<200/<400/more m: %.1f/%.1f/%.1f/%.1f/%.1f; cache %zu ranges (%u new, %u unreadable)",
       g_cull.enabled ? L"ON" : L"off (counting only)", g_cs.considered / n, g_cs.offscreen / n, g_cs.visibleFar / n,
       g_cs.kept / n, g_cs.dist[0] / n, g_cs.dist[1] / n, g_cs.dist[2] / n, g_cs.dist[3] / n, g_cs.dist[4] / n,
       g_cullCache.size(), g_cs.misses, g_cs.unreadable);
  g_cs = {};
  g_csFrames = 0;
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
  if (!offscreen && g_cull.visibleFar > 0) cull = dist > g_cull.visibleFar * (e->culled ? back : 1.0f);
  if (!g_cull.enabled) return false;
  e->culled = cull;
  if (cull) { if (offscreen) ++g_cs.offscreen; else ++g_cs.visibleFar; }
  return cull;
}

// All draws: glass skip (g_glassOn), sky handling (see g_skyMode), then transforms for Remix (before_draw/after_draw).
template <class Draw>
HRESULT comp_draw(IDirect3DDevice9* dev, Draw draw) {
  const bool glassSkip = !g_glassOn && is_glass_draw();
  const bool sky = is_sky_draw();
  const bool midQuad = !glassSkip && !sky && is_midscene_quad(dev);
  if (g_dumpFrames) dump_draw(dev, glassSkip ? L"SKIPPED(glass)" : midQuad ? L"SKIPPED(mid-scene quad)" : sky ? (g_skyMode == 1 ? L"SKIPPED(sky)" : L"sky") : L"drawn");
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
  bool atOff = false;                                  // performance experiment: this class drawn without alpha test
  if (exp_solid_draw()) {
    DWORD at = 0;
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
    if (at && SUCCEEDED(o.SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE))) { atOff = true; ++g_expSolid; }
  }
  const HRESULT hr = draw();
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
  return comp_draw(dev, [&] { return o.DrawPrimitive(dev, t, start, cap_prims(count)); });
}

HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minIdx,
                                                  UINT numV, UINT startIdx, UINT count) {
  g_pendingPrims = count;
  g_drawFirstVert = base >= 0 ? static_cast<UINT>(base) + minIdx : 0;
  g_drawNumVert = base >= 0 ? numV : 0;
  return comp_draw(dev, [&] { return o.DrawIndexedPrimitive(dev, t, base, minIdx, numV, startIdx, cap_prims(count)); });
}

HRESULT STDMETHODCALLTYPE hk_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) {
  g_pendingPrims = count;
  g_drawNumVert = 0;                                   // no vertex buffer: not culled
  return comp_draw(dev, [&] { return o.DrawPrimitiveUP(dev, t, cap_prims(count), data, stride); });
}

HRESULT STDMETHODCALLTYPE hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minIdx, UINT numV,
                                                    UINT count, const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
  g_pendingPrims = count;
  g_drawNumVert = 0;
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
  if (SUCCEEDED(hr) && vb && *vb) ++g_vbGen[*vb];
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
  patch(&v->CreateVertexDeclaration, &hk_CreateVertexDeclaration, o.CreateVertexDeclaration);
  patch(&v->SetVertexDeclaration, &hk_SetVertexDeclaration, o.SetVertexDeclaration);
  patch(&v->SetFVF, &hk_SetFVF, o.SetFVF);
  patch(&v->SetTextureStageState, &hk_SetTextureStageState, o.SetTextureStageState);
  patch(&v->SetRenderState, &hk_SetRenderState, o.SetRenderState);
  patch(&v->DrawPrimitive, &hk_DrawPrimitive, o.DrawPrimitive);
  patch(&v->DrawIndexedPrimitive, &hk_DrawIndexedPrimitive, o.DrawIndexedPrimitive);
  patch(&v->DrawPrimitiveUP, &hk_DrawPrimitiveUP, o.DrawPrimitiveUP);
  patch(&v->DrawIndexedPrimitiveUP, &hk_DrawIndexedPrimitiveUP, o.DrawIndexedPrimitiveUP);
  patch(&v->CreateQuery, &hk_CreateQuery, o.CreateQuery);
  patch(&v->CreateVertexBuffer, &hk_CreateVertexBuffer, o.CreateVertexBuffer);
  remember_backbuffer(dev);
  rbrrtx_harvest_install(dev);
  init_remix_api();
  clog(L"device hooks installed (comp %s, sun %s)", g_enabled ? L"on" : L"off", g_apiOk ? L"on" : L"off");
  return hr;
}

}  // namespace

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
    g_uiTriggerOn = rbrrtx_env_flag(L"RBRRTX_UI_TRIGGER", true);
    g_skipOffscreen = !rbrrtx_env_flag(L"RBRRTX_OFFSCREEN", false);
    wchar_t fol[16];
    if (GetEnvironmentVariableW(L"RBRRTX_FOLIAGE", fol, 16)) g_foliageMode = std::clamp(_wtoi(fol), 0, 2);
    if (GetEnvironmentVariableW(L"RBRRTX_DUST", fol, 16)) g_dustKeep = static_cast<unsigned>(std::clamp(_wtoi(fol), 0, 100));
    if (GetEnvironmentVariableW(L"RBRRTX_ALPHAREF", fol, 16)) g_cutoutRef = static_cast<DWORD>(std::clamp(_wtoi(fol), 1, 255));
    const DWORD hv = GetEnvironmentVariableW(L"RBRRTX_HARVEST", fol, 16);
    g_harvestUsage = hv > 0 && !(hv < 16 && wcscmp(fol, L"0") == 0);
    wchar_t xf[8];
    if (GetEnvironmentVariableW(L"RBRRTX_XF_MODE", xf, 8)) g_testMode = std::clamp(_wtoi(xf), 0, 3);
    wchar_t rad[64];
    if (GetEnvironmentVariableW(L"RBRRTX_SUN_RADIANCE", rad, 64))
      swscanf_s(rad, L"%f,%f,%f", &g_sunRadiance[0], &g_sunRadiance[1], &g_sunRadiance[2]);
    load_vso_hashes();
  }
  IDirect3D9* d3d = static_cast<IDirect3D9*>(d3d9);
  patch(&d3d->lpVtbl->CreateDevice, &hk_CreateDevice, o.CreateDevice);
  clog(L"comp installed on IDirect3D9 %p", d3d9);
}
