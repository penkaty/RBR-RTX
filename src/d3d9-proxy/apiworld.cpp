// apiworld.cpp - static stage geometry through Kim's Remix API (user 2026-11-01: "lighten the load from the CPU",
// "optimising alpha cards as much as possible"; plan docs/research/api-world-plan.md section 1, F1).
//
// Every scenery draw the game makes crosses the Remix bridge each frame and Remix matches it to last frame's instance
// (~5.8 us per draw in RemixApi-Performance.md, plus our own per-draw work in comp_draw). Stage geometry never changes,
// so each such draw is registered ONCE here: one API mesh copied from the draw's own vertex/index range (positions are
// world space for RL* shaders), a material on the game's own texture (remixapi_ImportD3D9Texture: no copy), one
// retained instance (Kim2091's runtime, fnv/main: replayed with one DrawRetainedInstances per frame). From the next
// frame on the game's D3D9 draw is not sent at all.
// Mods and rtx.conf keep working: Kim's runtime looks an API material's replacement up by the imported albedo's D3D9
// image hash and gives the instance that texture's rtx.conf categories (rtx_fork_submit.cpp
// externalDrawMaterialReplacement / externalDrawTextureCategories), so the PBR mods, the generated thin-opaque foliage
// and CardNormals match as before.
// What is taken (comp.cpp api_world_class): RL* world-space scenery, triangle lists, stream 0 only, opaque or alpha
// tested, not blended (decals, the foliage soft-edge pass stay D3D9); RLDouble* two-texture ground only while the
// ground second layer is off (then Remix shows layer A only anyway; the blend needs the terrain layers material, later).
// Smooth-normal textures stay smooth: the runtime welds by position. Sway foliage is taken without its sway (as FF scenery mode 2 drew it since 10-21); near-tree
// wind comes later from our own script (writeup section 5).
// Lifetime: built under a per-frame budget (vertices, meshes) so loading a view does not hitch; the frame an instance
// is created or shown again the D3D9 draw still runs (the replay at the next frame's first BeginScene is the first to
// carry it, as in New Vegas RTX api_retained.cpp). An instance the game does not draw while it is in view for
// hide_frames frames is hidden (the game culled it, swapped its LOD or freed it) and shown again by its next draw;
// out of view it stays (reflections, shadows). Menu <-> stage and a new buffer at a known address drop them.
// Settings: materials.ini [api_world]; key api_world (Alt+W) cycles all -> foliage only -> off for A/B.
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <d3d9.h>
#include "third_party/remixplus/remix_c.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#define wlog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

namespace {

struct Key {
  const void* vb; const void* ib; const void* decl; const void* tex;
  unsigned vbGen, ibGen;
  UINT offset, stride;
  INT base;
  UINT minIdx, numV, startIdx, prims;
  uint32_t state;                                      // alpha test, cull, wrap (material + double-sided)
  bool operator==(const Key& o) const { return std::memcmp(this, &o, sizeof(Key)) == 0; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = 1469598103934665603ull;
    const auto* p = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof(Key); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return static_cast<size_t>(h ^ (h >> 32));
  }
};

struct Entry {
  remixapi_MeshHandle mesh = nullptr;
  uint64_t inst = 0;
  bool failed = false, visible = false, sway = false;
  unsigned seen = 0, shownAt = 0;                      // frame last drawn by the game / created or shown again
  float c[3] = {}, r = 0.0f;                           // world bounds
};

struct Material { remixapi_MaterialHandle h = nullptr; bool failed = false; };

const RbrApiExt* g_ext = nullptr;
PFN_remixapi_CreateMeshBatched g_createMesh = nullptr;
PFN_remixapi_DestroyMesh g_destroyMesh = nullptr;
PFN_remixapi_CreateMaterial g_createMat = nullptr;
PFN_remixapi_DestroyMaterial g_destroyMat = nullptr;
PFN_remixapi_DestroyTexture g_destroyTex = nullptr;
PFN_remixapi_ImportD3D9Texture g_import = nullptr;
PFN_remixapi_CreateRetainedInstances g_createInst = nullptr;
PFN_remixapi_UpdateRetainedInstances g_updateInst = nullptr;
PFN_remixapi_DestroyRetainedInstances g_destroyInst = nullptr;
PFN_remixapi_DrawRetainedInstances g_replay = nullptr;

bool g_avail = false;                                  // runtime + bridge can do it
bool g_cfgOn = true, g_cfgSway = true, g_cfgStatic = true, g_flat = true;
int g_mode = 0;                                        // key: 0 = as configured, 1 = foliage only, 2 = off
bool g_active = true;                                  // comp layer on, ray tracing on
unsigned g_maxBuilds = 64, g_maxVerts = 60000, g_hideFrames = 30;
float g_roughness = 0.9f;                              // rtx.legacyMaterial.roughnessConstant (rtx.conf)

std::unordered_map<Key, Entry, KeyHash> g_entries;
std::unordered_map<uint64_t, Material> g_materials;    // material key -> material
std::unordered_map<uint64_t, remixapi_TextureHandle> g_textures;   // texture hash -> imported (null = refused)
std::unordered_map<const void*, unsigned> g_gen;       // bumped when the game creates a buffer at an address
std::vector<remixapi_RetainedInstanceUpdate> g_updates;
uint64_t g_meshSerial = 0;                             // mesh hashes: never repeated in a session (skipRepeatedMeshCreate)
unsigned g_frame = 1, g_builtFrame = 0, g_vertsFrame = 0, g_drawnWorld = 0, g_replayedFrame = 0;
unsigned g_live = 0, g_visibleCount = 0;
// Since the last log line.
unsigned g_logFrames = 0, g_taken = 0, g_passed = 0, g_built = 0, g_hidden = 0, g_shown = 0;
double g_sentMB = 0;
unsigned g_refused[8] = {};
const wchar_t* kRefuse[8] = { L"no texture hash", L"texture not importable", L"vertex layout", L"buffers not readable",
                              L"material", L"mesh/instance", L"multi-stream", L"empty" };

bool mode_takes(bool sway) {
  if (!g_avail || !g_cfgOn || !g_active || g_mode == 2) return false;
  return sway ? g_cfgSway : (g_cfgStatic && g_mode == 0);
}

remixapi_Transform identity() {
  remixapi_Transform t = {};
  t.matrix[0][0] = t.matrix[1][1] = t.matrix[2][2] = 1.0f;
  return t;
}

void queue_visibility(Entry& e, bool visible) {
  if (!e.inst || e.visible == visible) return;
  e.visible = visible;
  remixapi_RetainedInstanceUpdate u = {};
  u.handle = e.inst;
  u.flags = REMIXAPI_RETAINED_UPDATE_VISIBILITY;
  u.visible = visible ? 1 : 0;
  g_updates.push_back(u);
  if (visible) { ++g_visibleCount; ++g_shown; } else { --g_visibleCount; ++g_hidden; }
}

void flush_updates() {
  if (g_updates.empty() || !g_updateInst) { g_updates.clear(); return; }
  for (size_t i = 0; i < g_updates.size(); i += 512)
    g_updateInst(g_updates.data() + i, static_cast<uint32_t>((std::min)(g_updates.size() - i, size_t(512))));
  g_updates.clear();
}

uint8_t wrap_mode(DWORD a) {                           // lss::Mdl::WrapMode, as New Vegas RTX api_material.cpp
  switch (a) { case D3DTADDRESS_CLAMP: return 0; case D3DTADDRESS_MIRROR: return 2; case D3DTADDRESS_BORDER: return 3; default: return 1; }
}

// Imported under the game hash with the top bits flipped: the runtime then still knows the image by its D3D9 hash
// (mods, rtx.conf lists), and our API handle cannot collide with the game's own texture.
uint64_t import_hash(uint64_t tex) { return tex ^ 0x5242525449000000ull; }

bool import_texture(IDirect3DBaseTexture9* t, uint64_t hx) {
  auto it = g_textures.find(hx);
  if (it != g_textures.end()) return it->second != nullptr;
  remixapi_TextureHandle h = nullptr;
  const remixapi_ErrorCode err = g_import(t, import_hash(hx), 0, REMIXAPI_TEXTURE_ENCODING_NONE, &h);
  const bool ok = err == REMIXAPI_ERROR_CODE_SUCCESS && h;
  if (!ok) wlog(L"api world: texture %016llX not importable (error %d), its draws stay D3D9", hx, static_cast<int>(err));
  g_textures[hx] = ok ? h : nullptr;
  return ok;
}

remixapi_MaterialHandle material_for(IDirect3DDevice9* dev, IDirect3DBaseTexture9* t, uint64_t hx, uint32_t state,
                                     DWORD alphaFunc, DWORD alphaRef, DWORD wrapU, DWORD wrapV) {
  const uint64_t mk = hx * 0x9E3779B97F4A7C15ull ^ state;
  auto it = g_materials.find(mk);
  if (it != g_materials.end()) return it->second.failed ? nullptr : it->second.h;
  Material& m = g_materials[mk];
  m.failed = true;
  if (!import_texture(t, hx)) return nullptr;
  wchar_t path[24];
  swprintf_s(path, L"0x%016llX", import_hash(hx));
  remixapi_MaterialInfoOpaqueEXT op = {};
  op.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  op.roughnessTexture = L"";
  op.metallicTexture = L"";
  op.albedoConstant = { 1.0f, 1.0f, 1.0f };
  op.opacityConstant = 1.0f;
  op.roughnessConstant = g_roughness;
  op.heightTexture = L"";
  op.alphaTestType = 7;                                // AlphaTestType::kAlways
  if (alphaFunc >= D3DCMP_NEVER && alphaFunc < D3DCMP_ALWAYS) {
    op.alphaTestType = static_cast<int>(alphaFunc) - 1;   // D3DCMPFUNC -> AlphaTestType
    op.alphaReferenceValue = static_cast<uint8_t>(alphaRef & 0xFF);
  }
  op.enableDlssControlMask = 1;                        // remix.h MaterialInfoOpaqueEXT defaults
  op.dlssControlMaskIntensity = 1.0f;
  op.dlssControlMaskToneStrength = 0.3f;
  op.dlssControlMaskStructuralStrength = 0.7f;
  remixapi_MaterialInfo mi = {};
  mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  mi.pNext = &op;
  mi.hash = 0x5242525700000000ull ^ mk ^ (mk >> 29);
  if (!mi.hash) mi.hash = 1;
  mi.albedoTexture = path;
  mi.normalTexture = L"";
  mi.tangentTexture = L"";
  mi.emissiveTexture = L"";
  mi.filterMode = 1;
  mi.wrapModeU = wrap_mode(wrapU);
  mi.wrapModeV = wrap_mode(wrapV);
  (void)dev;
  if (g_createMat(&mi, &m.h) != REMIXAPI_ERROR_CODE_SUCCESS || !m.h) { m.h = nullptr; return nullptr; }
  m.failed = false;
  return m.h;
}

float half_to_float(uint16_t h) {
  const uint32_t e = (h >> 10) & 0x1F, mt = h & 0x3FF, sgn = (h & 0x8000u) << 16;
  const uint32_t f = e == 0 ? sgn : sgn | ((e + 112) << 23) | (mt << 13);
  float r;
  std::memcpy(&r, &f, 4);
  return r;
}

// The draw's triangles as API vertices (stream 0: POSITION float3/4, TEXCOORD0 float2 / half2, COLOR0 optional).
// Normals: flat = every triangle its own vertices with its face normal (as Remix shades the FF scenery, which is drawn
// without NORMAL, comp.cpp g_ffNormals); else area-weighted vertex normals over the draw's own indices.
int build_vertices(const Key& k, std::vector<remixapi_HardcodedVertex>& out, std::vector<uint32_t>& idx, float c[3], float& r) {
  IDirect3DVertexDeclaration9* decl = static_cast<IDirect3DVertexDeclaration9*>(const_cast<void*>(k.decl));
  D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
  UINT n = MAXD3DDECLLENGTH + 1;
  if (FAILED(decl->lpVtbl->GetDeclaration(decl, el, &n))) return 2;
  int posOff = -1, uvOff = -1, uvType = -1, colOff = -1;
  for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
    if (el[i].UsageIndex != 0) continue;
    if (el[i].Usage == D3DDECLUSAGE_POSITION) {
      if (el[i].Stream != 0 || (el[i].Type != D3DDECLTYPE_FLOAT3 && el[i].Type != D3DDECLTYPE_FLOAT4)) return 2;
      posOff = el[i].Offset;
    } else if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].Stream == 0) {
      uvOff = el[i].Offset; uvType = el[i].Type;
    } else if (el[i].Usage == D3DDECLUSAGE_COLOR && el[i].Stream == 0 && el[i].Type == D3DDECLTYPE_D3DCOLOR) {
      colOff = el[i].Offset;
    }
  }
  if (posOff < 0 || uvOff < 0 || (uvType != D3DDECLTYPE_FLOAT2 && uvType != D3DDECLTYPE_FLOAT3 &&
      uvType != D3DDECLTYPE_FLOAT4 && uvType != D3DDECLTYPE_FLOAT16_2)) return 2;
  // READONLY locks: the bridge client keeps a copy of managed buffers (comp.cpp cull_entry, alphacut.cpp).
  IDirect3DVertexBuffer9* vb = static_cast<IDirect3DVertexBuffer9*>(const_cast<void*>(k.vb));
  IDirect3DIndexBuffer9* ib = static_cast<IDirect3DIndexBuffer9*>(const_cast<void*>(k.ib));
  D3DVERTEXBUFFER_DESC vd = {};
  D3DINDEXBUFFER_DESC id = {};
  if (FAILED(vb->lpVtbl->GetDesc(vb, &vd)) || FAILED(ib->lpVtbl->GetDesc(ib, &id))) return 3;
  if ((vd.Usage & D3DUSAGE_DYNAMIC) || (id.Usage & D3DUSAGE_DYNAMIC)) return 3;
  const UINT isz = id.Format == D3DFMT_INDEX32 ? 4 : 2;
  const UINT iStart = k.startIdx * isz, iBytes = k.prims * 3 * isz;
  const UINT first = static_cast<UINT>(k.base) + k.minIdx;
  const UINT vStart = k.offset + first * k.stride, vBytes = k.numV * k.stride;
  if (iStart + iBytes > id.Size || vStart + vBytes > vd.Size || k.numV == 0) return 3;
  std::vector<uint32_t> src(static_cast<size_t>(k.prims) * 3);
  void* p = nullptr;
  if (FAILED(ib->lpVtbl->Lock(ib, iStart, iBytes, &p, D3DLOCK_READONLY)) || !p) return 3;
  for (size_t i = 0; i < src.size(); ++i) {
    const uint32_t v = isz == 4 ? static_cast<const uint32_t*>(p)[i] : static_cast<const uint16_t*>(p)[i];
    src[i] = v >= k.minIdx ? v - k.minIdx : UINT32_MAX;   // relative to the locked vertex range
  }
  ib->lpVtbl->Unlock(ib);
  std::vector<uint8_t> raw(vBytes);
  if (FAILED(vb->lpVtbl->Lock(vb, vStart, vBytes, &p, D3DLOCK_READONLY)) || !p) return 3;
  std::memcpy(raw.data(), p, vBytes);
  vb->lpVtbl->Unlock(vb);

  std::vector<remixapi_HardcodedVertex> verts(k.numV);
  float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
  for (UINT i = 0; i < k.numV; ++i) {
    const uint8_t* v = raw.data() + static_cast<size_t>(i) * k.stride;
    remixapi_HardcodedVertex& o = verts[i];
    std::memcpy(o.position, v + posOff, 12);
    if (uvType == D3DDECLTYPE_FLOAT16_2) {
      uint16_t h[2];
      std::memcpy(h, v + uvOff, 4);
      o.texcoord[0] = half_to_float(h[0]); o.texcoord[1] = half_to_float(h[1]);
    } else {
      std::memcpy(o.texcoord, v + uvOff, 8);
    }
    o.color = 0xFFFFFFFFu;
    if (colOff >= 0) std::memcpy(&o.color, v + colOff, 4);
    for (int a = 0; a < 3; ++a) { lo[a] = (std::min)(lo[a], o.position[a]); hi[a] = (std::max)(hi[a], o.position[a]); }
  }
  for (int a = 0; a < 3; ++a) c[a] = 0.5f * (lo[a] + hi[a]);
  r = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));

  auto face = [&](const remixapi_HardcodedVertex& a, const remixapi_HardcodedVertex& b, const remixapi_HardcodedVertex& d, float nrm[3]) {
    const float e1[3] = { b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2] };
    const float e2[3] = { d.position[0] - a.position[0], d.position[1] - a.position[1], d.position[2] - a.position[2] };
    nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
    nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
    nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
  };
  auto unit = [](float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; } else { v[0] = 0; v[1] = 1; v[2] = 0; }
  };
  out.clear();
  idx.clear();
  if (g_flat) {
    out.reserve(src.size());
    idx.reserve(src.size());
    for (size_t t = 0; t + 2 < src.size(); t += 3) {
      if (src[t] >= k.numV || src[t + 1] >= k.numV || src[t + 2] >= k.numV) continue;
      remixapi_HardcodedVertex a = verts[src[t]], b = verts[src[t + 1]], d = verts[src[t + 2]];
      float nrm[3];
      face(a, b, d, nrm);
      unit(nrm);
      for (remixapi_HardcodedVertex* v : { &a, &b, &d }) {
        std::memcpy(v->normal, nrm, 12);
        idx.push_back(static_cast<uint32_t>(out.size()));
        out.push_back(*v);
      }
    }
  } else {
    for (auto& v : verts) v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;
    for (size_t t = 0; t + 2 < src.size(); t += 3) {
      if (src[t] >= k.numV || src[t + 1] >= k.numV || src[t + 2] >= k.numV) continue;
      float nrm[3];
      face(verts[src[t]], verts[src[t + 1]], verts[src[t + 2]], nrm);   // area weighted
      for (int j = 0; j < 3; ++j)
        for (int a = 0; a < 3; ++a) verts[src[t + j]].normal[a] += nrm[a];
      idx.push_back(src[t]); idx.push_back(src[t + 1]); idx.push_back(src[t + 2]);
    }
    for (auto& v : verts) unit(v.normal);
    out.swap(verts);
  }
  return idx.empty() ? 7 : -1;
}

void destroy_entry(Entry& e) {
  if (e.inst && g_destroyInst) g_destroyInst(&e.inst, 1);
  if (e.mesh && g_destroyMesh) g_destroyMesh(e.mesh);
  if (e.inst) { --g_live; if (e.visible) --g_visibleCount; }
  e = Entry{};
}

}  // namespace

void rbrrtx_apiworld_init(const RbrApiExt* ext) {
  g_ext = ext;
  wchar_t path[MAX_PATH * 2];
  rbrrtx_config_path(L"RBRRTX_MATERIALS", L"materials.ini", path, MAX_PATH * 2);
  auto geti = [&](const wchar_t* key, int dflt) { return static_cast<int>(GetPrivateProfileIntW(L"api_world", key, dflt, path)); };
  g_cfgOn = geti(L"enabled", 1) != 0;
  g_cfgSway = geti(L"foliage", 1) != 0;
  g_cfgStatic = geti(L"static", 1) != 0;
  g_flat = geti(L"flat_normals", 1) != 0;
  g_maxBuilds = static_cast<unsigned>((std::max)(1, geti(L"max_builds_per_frame", 64)));
  g_maxVerts = static_cast<unsigned>((std::max)(1000, geti(L"max_vertices_per_frame", 60000)));
  g_hideFrames = static_cast<unsigned>((std::max)(0, geti(L"hide_frames", 30)));
  wchar_t v[16] = {};
  if (GetEnvironmentVariableW(L"RBRRTX_API_WORLD", v, 16)) g_cfgOn = v[0] != L'0';
  if (ext) {
    g_createMesh = reinterpret_cast<PFN_remixapi_CreateMeshBatched>(ext->createMeshBatched);
    g_destroyMesh = reinterpret_cast<PFN_remixapi_DestroyMesh>(ext->destroyMesh);
    g_createMat = reinterpret_cast<PFN_remixapi_CreateMaterial>(ext->createMaterial);
    g_destroyMat = reinterpret_cast<PFN_remixapi_DestroyMaterial>(ext->destroyMaterial);
    g_destroyTex = reinterpret_cast<PFN_remixapi_DestroyTexture>(ext->destroyTexture);
    g_import = reinterpret_cast<PFN_remixapi_ImportD3D9Texture>(ext->importD3D9Texture);
    g_createInst = reinterpret_cast<PFN_remixapi_CreateRetainedInstances>(ext->createRetained);
    g_updateInst = reinterpret_cast<PFN_remixapi_UpdateRetainedInstances>(ext->updateRetained);
    g_destroyInst = reinterpret_cast<PFN_remixapi_DestroyRetainedInstances>(ext->destroyRetained);
    g_replay = reinterpret_cast<PFN_remixapi_DrawRetainedInstances>(ext->drawRetained);
  }
  const bool calls = g_createMesh && g_destroyMesh && g_createMat && g_import && g_createInst && g_updateInst &&
                     g_destroyInst && g_replay;
  // Capability probe (remix_c.h): SUCCESS when an import could be made now.
  const remixapi_ErrorCode probe = calls ? g_import(nullptr, 0, 0, 0, nullptr) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  // Not a gate: at device creation the bridge may not have registered the device yet; a texture the import refuses
  // later keeps its draws on D3D9 (logged).
  g_avail = calls;
  wlog(L"api world (materials.ini [api_world]): %s; static scenery %s, foliage %s, %s normals, budget %u meshes / %u "
       L"vertices per frame, hide after %u frames in view not drawn%s",
       !g_cfgOn ? L"off" : g_avail ? L"ON (retained instances, D3D9 texture import)"
       : L"unavailable (runtime/bridge without retained instances or D3D9 import)",
       g_cfgStatic ? L"yes" : L"no", g_cfgSway ? L"yes" : L"no", g_flat ? L"flat" : L"smooth", g_maxBuilds, g_maxVerts,
       g_hideFrames, calls && probe != REMIXAPI_ERROR_CODE_SUCCESS ? L" (import probe not yet ready)" : L"");
  if (calls && probe != REMIXAPI_ERROR_CODE_SUCCESS) wlog(L"api world: D3D9 import probe at start: error %d", static_cast<int>(probe));
}

bool rbrrtx_apiworld_wants(bool sway) { return mode_takes(sway); }

bool rbrrtx_apiworld_draw(void* device, const RbrWorldDraw& d) {
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (!mode_takes(d.sway)) return false;
  DWORD blend = FALSE, at = FALSE, func = D3DCMP_ALWAYS, ref = 0, cull = D3DCULL_CCW, wu = D3DTADDRESS_WRAP, wv = D3DTADDRESS_WRAP;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  if (blend) return false;                             // decals, soft-edge foliage pass: D3D9
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  if (at) {
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAFUNC, &func);
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &ref);
  } else {
    func = D3DCMP_ALWAYS;
  }
  dev->lpVtbl->GetRenderState(dev, D3DRS_CULLMODE, &cull);
  dev->lpVtbl->GetSamplerState(dev, 0, D3DSAMP_ADDRESSU, &wu);
  dev->lpVtbl->GetSamplerState(dev, 0, D3DSAMP_ADDRESSV, &wv);
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  IDirect3DVertexDeclaration9* decl = nullptr;
  IDirect3DBaseTexture9* tex = nullptr;
  UINT offset = 0, stride = 0;
  dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride);
  dev->lpVtbl->GetIndices(dev, &ib);
  dev->lpVtbl->GetVertexDeclaration(dev, &decl);
  dev->lpVtbl->GetTexture(dev, 0, &tex);
  Key k = {};
  k.vb = vb; k.ib = ib; k.decl = decl; k.tex = tex;
  k.vbGen = vb ? g_gen[vb] : 0;
  k.ibGen = ib ? g_gen[ib] : 0;
  k.offset = offset; k.stride = stride; k.base = d.base; k.minIdx = d.minIdx; k.numV = d.numV;
  k.startIdx = d.startIdx; k.prims = d.prims;
  k.state = (func & 0xF) | ((ref & 0xFF) << 4) | ((cull & 0x3) << 12) | ((wu & 0x7) << 14) | ((wv & 0x7) << 17);
  bool take = false;
  auto it = g_entries.find(k);
  if (it != g_entries.end()) {
    Entry& e = it->second;
    if (!e.failed) {
      e.seen = g_frame;
      ++g_drawnWorld;
      if (!e.visible) { queue_visibility(e, true); e.shownAt = g_frame; }
      take = e.shownAt != g_frame;                     // the frame it (re)appears, D3D9 draws it once more
    }
  } else if (vb && ib && decl && tex && stride && g_builtFrame < g_maxBuilds && g_vertsFrame < g_maxVerts) {
    Entry& e = g_entries[k];
    e.failed = true;
    e.sway = d.sway;
    int why = -1;
    const uint64_t hx = rbrrtx_texture_hash(tex);
    std::vector<remixapi_HardcodedVertex> verts;
    std::vector<uint32_t> idx;
    remixapi_MaterialHandle mat = nullptr;
    if (!hx) why = 0;
    else if (tex->lpVtbl->GetType(tex) != D3DRTYPE_TEXTURE) why = 1;
    else if ((why = build_vertices(k, verts, idx, e.c, e.r)) < 0 &&
             !(mat = material_for(dev, tex, hx, k.state & 0xFFF, func, ref, wu, wv)))
      why = g_textures.count(hx) && !g_textures[hx] ? 1 : 4;   // not importable / material refused
    if (why < 0) {
      remixapi_MeshInfoSurfaceTriangles s = {};
      s.vertices_values = verts.data();
      s.vertices_count = verts.size();
      s.indices_values = idx.data();
      s.indices_count = idx.size();
      s.material = mat;
      remixapi_MeshInfo mi = {};
      mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      mi.hash = 0x5242525757000000ull + ++g_meshSerial;   // "RBRWW" + a counter
      mi.surfaces_values = &s;
      mi.surfaces_count = 1;
      // Vertex colour modulates the albedo as texture stage 0 does (New Vegas RTX vertex_color_blend), alpha = texture.
      remixapi_InstanceInfoBlendEXT bl = {};
      bl.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      bl.alphaTestCompareOp = 7;                       // VK_COMPARE_OP_ALWAYS (the material holds the alpha test)
      bl.srcColorBlendFactor = 1; bl.dstColorBlendFactor = 0; bl.colorBlendOp = 0;
      bl.textureColorArg1Source = 1;                   // RtTextureArgSource::Texture
      bl.textureColorArg2Source = 2;                   // VertexColor0
      bl.textureColorOperation = 3;                    // Modulate
      bl.textureAlphaArg1Source = 1;
      bl.textureAlphaArg2Source = 2;
      bl.textureAlphaOperation = 1;                    // SelectArg1
      bl.tFactor = 0xFFFFFFFFu;
      bl.srcAlphaBlendFactor = 1; bl.dstAlphaBlendFactor = 0; bl.alphaBlendOp = 0;
      bl.writeMask = 0xF;
      bl.isVertexColorBakedLighting = 1;               // rtx.vertexColorIsBakedLighting (default, not in rtx.conf)
      remixapi_InstanceInfo ii = {};
      ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      ii.pNext = &bl;
      ii.transform = identity();
      ii.doubleSided = cull == D3DCULL_NONE ? 1 : 0;
      if (g_createMesh(&mi, &e.mesh) != REMIXAPI_ERROR_CODE_SUCCESS || !e.mesh) { e.mesh = nullptr; why = 5; }
      else {
        ii.mesh = e.mesh;
        if (g_createInst(&ii, 1, &e.inst) != REMIXAPI_ERROR_CODE_SUCCESS || !e.inst) {
          g_destroyMesh(e.mesh);
          e.mesh = nullptr;
          e.inst = 0;
          why = 5;
        }
      }
    }
    if (why >= 0) {
      ++g_refused[why];
      if (g_refused[why] <= 3)
        wlog(L"api world: draw with texture %016llX (%u vertices) stays D3D9: %s", hx, d.numV, kRefuse[why]);
    } else {
      e.failed = false;
      e.visible = true;
      e.seen = e.shownAt = g_frame;
      ++g_live; ++g_visibleCount; ++g_built; ++g_builtFrame; ++g_drawnWorld;
      g_vertsFrame += static_cast<unsigned>(verts.size());
      g_sentMB += (verts.size() * sizeof(remixapi_HardcodedVertex) + idx.size() * 4) / 1048576.0;
    }
  }
  if (vb) vb->lpVtbl->Release(vb);
  if (ib) ib->lpVtbl->Release(ib);
  if (decl) decl->lpVtbl->Release(decl);
  if (tex) tex->lpVtbl->Release(tex);
  if (take) ++g_taken; else ++g_passed;
  return take;
}

// First BeginScene of a frame: the retained table goes out before this frame's draws (and before the HUD draw that
// starts Remix's frame), after the updates flushed at the last Present (the bridge is a FIFO).
void rbrrtx_apiworld_replay() {
  if (!g_avail || !g_replay || g_replayedFrame == g_frame || g_live == 0) return;
  g_replayedFrame = g_frame;
  g_replay(nullptr);
}

// Present. viewProj = this frame's world view * projection (D3D row-vector order); null without a world camera.
void rbrrtx_apiworld_frame_end(const float* viewProj, bool active) {
  if (!g_avail) return;
  if (active != g_active) {                            // comp layer off / raster: the game's draws come back
    g_active = active;
    if (!active) for (auto& kv : g_entries) queue_visibility(kv.second, false);
    wlog(L"api world: %s", active ? L"active again" : L"paused (comp layer off or raster): every world draw is D3D9");
  }
  if (g_mode != 0) {                                   // key: hide what the current mode no longer takes
    for (auto& kv : g_entries)
      if (kv.second.visible && !mode_takes(kv.second.sway)) queue_visibility(kv.second, false);
  }
  // In view and not drawn for hide_frames: the game culled it (or swapped its LOD, or freed its buffer). Only on frames
  // that drew the world, every 4th frame.
  if (g_hideFrames && viewProj && g_drawnWorld > 0 && (g_frame & 3) == 0) {
    float pl[6][4];
    const float* m = viewProj;                         // m[row * 4 + col]
    for (int i = 0; i < 4; ++i) {
      const float c0 = m[i * 4 + 0], c1 = m[i * 4 + 1], c2 = m[i * 4 + 2], c3 = m[i * 4 + 3];
      pl[0][i] = c3 + c0; pl[1][i] = c3 - c0; pl[2][i] = c3 + c1; pl[3][i] = c3 - c1; pl[4][i] = c2; pl[5][i] = c3 - c2;
    }
    for (auto& p : pl) {
      const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
      if (l > 0) for (float& x : p) x /= l;
    }
    for (auto& kv : g_entries) {
      Entry& e = kv.second;
      if (!e.visible || g_frame - e.seen <= g_hideFrames) continue;
      bool in = true;
      for (auto& p : pl)
        if (p[0] * e.c[0] + p[1] * e.c[1] + p[2] * e.c[2] + p[3] < -e.r) { in = false; break; }
      if (in) queue_visibility(e, false);
    }
  }
  flush_updates();
  ++g_logFrames;
  if (g_logFrames >= 600) {
    std::wstring ref;
    for (int i = 0; i < 8; ++i)
      if (g_refused[i]) { wchar_t b[64]; swprintf_s(b, L", %s %u", kRefuse[i], g_refused[i]); ref += b; }
    wlog(L"api world: %u instances (%u shown), per frame %.0f draws by the API, %.0f still D3D9 of the taken classes; "
         L"since last line %u built (%.1f MB), %u hidden, %u shown again%s",
         g_live, g_visibleCount, static_cast<double>(g_taken) / g_logFrames, static_cast<double>(g_passed) / g_logFrames,
         g_built, g_sentMB, g_hidden, g_shown, ref.empty() ? L"" : (L"; stay D3D9 (draws): " + ref.substr(2)).c_str());
    g_logFrames = g_taken = g_passed = g_built = g_hidden = g_shown = 0;
    g_sentMB = 0;
    std::memset(g_refused, 0, sizeof(g_refused));
  }
  ++g_frame;
  g_builtFrame = g_vertsFrame = g_drawnWorld = 0;
}

void rbrrtx_apiworld_reset(const wchar_t* why) {
  if (g_entries.empty() && g_materials.empty() && g_textures.empty()) return;
  std::vector<uint64_t> handles;
  for (auto& kv : g_entries) if (kv.second.inst) handles.push_back(kv.second.inst);
  if (g_destroyInst)
    for (size_t i = 0; i < handles.size(); i += 512)
      g_destroyInst(handles.data() + i, static_cast<uint32_t>((std::min)(handles.size() - i, size_t(512))));
  for (auto& kv : g_entries) if (kv.second.mesh && g_destroyMesh) g_destroyMesh(kv.second.mesh);
  for (auto& kv : g_materials) if (kv.second.h && g_destroyMat) g_destroyMat(kv.second.h);
  for (auto& kv : g_textures)
    if (kv.second && g_destroyTex) g_destroyTex(kv.second);
  wlog(L"api world: %zu instances, %zu materials, %zu textures released (%s)", handles.size(), g_materials.size(),
       g_textures.size(), why);
  g_entries.clear();
  g_materials.clear();
  g_textures.clear();
  g_updates.clear();
  g_live = g_visibleCount = 0;
}

// A new buffer at an address: entries of the old one stay until hidden (in view, not drawn), new draws get new keys.
void rbrrtx_apiworld_forget(const void* buffer) { if (buffer) ++g_gen[buffer]; }

const wchar_t* rbrrtx_apiworld_cycle() {
  g_mode = (g_mode + 1) % 3;
  return g_mode == 0 ? L"all static scenery and foliage" : g_mode == 1 ? L"foliage only" : L"off (all D3D9)";
}
