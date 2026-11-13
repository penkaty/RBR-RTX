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
#include <unordered_set>
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
  // Streams 1 and 2 (objects only, zero for the static world): the drivers keep TEXCOORD0 in stream 1, COLOR0 in 2.
  const void* s1; unsigned s1Gen; UINT s1Off, s1Stride;
  const void* s2; unsigned s2Gen; UINT s2Off, s2Stride;
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
bool g_cfgOn = true, g_cfgSway = true, g_cfgStatic = true, g_flat = true, g_cfgObjects = true;
int g_mode = 0;                                        // key: 0 = as configured, 1 = foliage only, 2 = off
bool g_active = true;                                  // comp layer on, ray tracing on
unsigned g_maxBuilds = 64, g_maxVerts = 60000, g_hideFrames = 30;
float g_roughness = 0.9f;                              // rtx.legacyMaterial.roughnessConstant (rtx.conf)

std::unordered_map<Key, Entry, KeyHash> g_entries;
std::unordered_map<uint64_t, Material> g_materials;    // material key -> material
std::unordered_map<uint64_t, remixapi_TextureHandle> g_textures;   // texture hash -> imported (null = refused)
std::unordered_map<const void*, unsigned> g_gen;       // bumped when the game creates a buffer at an address
// Buffer change detection (overnight plan P3.1): a buffer some instance was built from is watched; the game locking it
// for writing makes it dynamic (clock digits, running marshals, flags, any CPU-animated RL* geometry): its instances
// go and it stays D3D9 until a new buffer is created at that address or the scene changes.
std::unordered_set<const void*> g_watched, g_dynamic;
unsigned g_dynamicDropped = 0;
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
int build_vertices(const Key& k, std::vector<remixapi_HardcodedVertex>& out, std::vector<uint32_t>& idx, float c[3], float& r,
                   bool object = false) {   // object: smooth, the draw's own NORMAL when it has one (drivers)
  IDirect3DVertexDeclaration9* decl = static_cast<IDirect3DVertexDeclaration9*>(const_cast<void*>(k.decl));
  D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
  UINT n = MAXD3DDECLLENGTH + 1;
  if (FAILED(decl->lpVtbl->GetDeclaration(decl, el, &n))) return 2;
  int posOff = -1, uvOff = -1, uvType = -1, colOff = -1, norOff = -1, uvStream = 0, colStream = 0;
  for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
    if (el[i].UsageIndex != 0) continue;
    const int st = el[i].Stream;                       // streams 1 / 2 only where the key holds their buffer (objects)
    const bool readable = st == 0 || (st == 1 && k.s1) || (st == 2 && k.s2);
    if (el[i].Usage == D3DDECLUSAGE_POSITION) {
      if (el[i].Stream != 0 || (el[i].Type != D3DDECLTYPE_FLOAT3 && el[i].Type != D3DDECLTYPE_FLOAT4)) return 2;
      posOff = el[i].Offset;
    } else if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && readable) {
      uvOff = el[i].Offset; uvType = el[i].Type; uvStream = st;
    } else if (el[i].Usage == D3DDECLUSAGE_COLOR && readable && el[i].Type == D3DDECLTYPE_D3DCOLOR) {
      colOff = el[i].Offset; colStream = st;
    } else if (el[i].Usage == D3DDECLUSAGE_NORMAL && el[i].Stream == 0 && el[i].Type == D3DDECLTYPE_FLOAT3) {
      norOff = el[i].Offset;
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
  // Streams 1 / 2: the same vertex range (BaseVertexIndex applies to every stream).
  std::vector<uint8_t> sraw[3];
  const UINT sstr[3] = { k.stride, k.s1Stride, k.s2Stride };
  for (int st = 1; st <= 2; ++st) {
    if (uvStream != st && colStream != st) continue;
    auto* b = static_cast<IDirect3DVertexBuffer9*>(const_cast<void*>(st == 1 ? k.s1 : k.s2));
    const UINT so = (st == 1 ? k.s1Off : k.s2Off) + first * sstr[st], sn = k.numV * sstr[st];
    D3DVERTEXBUFFER_DESC sd = {};
    if (!sstr[st] || FAILED(b->lpVtbl->GetDesc(b, &sd)) || (sd.Usage & D3DUSAGE_DYNAMIC) || so + sn > sd.Size) return 3;
    if (FAILED(b->lpVtbl->Lock(b, so, sn, &p, D3DLOCK_READONLY)) || !p) return 3;
    sraw[st].assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + sn);
    b->lpVtbl->Unlock(b);
  }
  const uint8_t* const sbase[3] = { raw.data(), sraw[1].data(), sraw[2].data() };

  std::vector<remixapi_HardcodedVertex> verts(k.numV);
  float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
  for (UINT i = 0; i < k.numV; ++i) {
    const uint8_t* v = raw.data() + static_cast<size_t>(i) * k.stride;
    remixapi_HardcodedVertex& o = verts[i];
    std::memcpy(o.position, v + posOff, 12);
    const uint8_t* uv = sbase[uvStream] + static_cast<size_t>(i) * sstr[uvStream] + uvOff;
    if (uvType == D3DDECLTYPE_FLOAT16_2) {
      uint16_t h[2];
      std::memcpy(h, uv, 4);
      o.texcoord[0] = half_to_float(h[0]); o.texcoord[1] = half_to_float(h[1]);
    } else {
      std::memcpy(o.texcoord, uv, 8);
    }
    o.color = 0xFFFFFFFFu;
    if (colOff >= 0) std::memcpy(&o.color, sbase[colStream] + static_cast<size_t>(i) * sstr[colStream] + colOff, 4);
    if (norOff >= 0) std::memcpy(o.normal, v + norOff, 12);
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
  if (object && norOff >= 0) {                         // the model's own normals, shared vertices
    for (size_t t = 0; t + 2 < src.size(); t += 3) {
      if (src[t] >= k.numV || src[t + 1] >= k.numV || src[t + 2] >= k.numV) continue;
      idx.push_back(src[t]); idx.push_back(src[t + 1]); idx.push_back(src[t + 2]);
    }
    for (auto& v : verts) unit(v.normal);
    out.swap(verts);
  } else if (g_flat && !object) {
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
  g_cfgObjects = geti(L"drivers", 1) != 0;          // drivers and co-drivers drawn by the API every frame
  g_maxBuilds = static_cast<unsigned>((std::max)(1, geti(L"max_builds_per_frame", 64)));
  g_maxVerts = static_cast<unsigned>((std::max)(1000, geti(L"max_vertices_per_frame", 60000)));
  g_hideFrames = static_cast<unsigned>((std::max)(0, geti(L"hide_frames", 30)));
  wchar_t v[16] = {};
  if (GetEnvironmentVariableW(L"RBRRTX_API_WORLD", v, 16)) g_cfgOn = v[0] != L'0';
  g_mode = g_cfgOn ? 0 : 2;                            // enabled = 0: starts off, Alt+W still turns it on
  g_cfgOn = true;
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
       g_mode == 2 && g_avail ? L"off at start (Alt+W turns it on)" : g_avail ? L"ON (retained instances, D3D9 texture import)"
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
  if ((vb && g_dynamic.count(vb)) || (ib && g_dynamic.count(ib))) {   // written by the game: stays D3D9
    if (vb) vb->lpVtbl->Release(vb);
    if (ib) ib->lpVtbl->Release(ib);
    if (decl) decl->lpVtbl->Release(decl);
    if (tex) tex->lpVtbl->Release(tex);
    ++g_passed;
    return false;
  }
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
      // Own picking value per instance, so Alt+X world clicks find this draw's texture. Without it every API
      // instance writes 0 and Remix maps 0 to whichever texture registered last (user 2026-11-06: hashes not
      // selectable). 0x40000000+: D3D9 draw IDs count up from 0 each frame, the runtime's own range is 0x80000000+.
      remixapi_InstanceInfoObjectPickingEXT pk = {};
      pk.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT;
      pk.pNext = &bl;
      pk.objectPickingValue = 0x40000000u | static_cast<uint32_t>(g_meshSerial & 0x3FFFFFFFu);
      remixapi_InstanceInfo ii = {};
      ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      ii.pNext = &pk;
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
      if (g_watched.insert(vb).second | g_watched.insert(ib).second) rbrrtx_watch_buffer_writes(vb, ib);
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

namespace {

// ---- Objects that move (drivers, user 2026-11-12: "make the driver and co-driver fully rendered through the API").
// One API mesh per game draw range in the model's own space (built once, as the static world), drawn every frame the
// game draws it with DrawInstance and the draw's world matrix. Not retained: the retained table goes out at the next
// frame's first BeginScene and would leave a moving object one frame behind the car (0.7 m at 150 km/h). The world
// matrix comes from comp.cpp world_from_c0 (about 1 mm far from the origin; c0 x VP^-1 drifted 0.3-8 m on Noiker).
std::unordered_map<Key, Entry, KeyHash> g_objects;     // Entry.sway reused: double-sided
unsigned g_objDrawn = 0, g_objBuilt = 0, g_objRefused = 0;

void lamps_reset();
void objects_reset() {
  for (auto& kv : g_objects) if (kv.second.mesh && g_destroyMesh) g_destroyMesh(kv.second.mesh);
  g_objects.clear();
  lamps_reset();
}

// ---- Car lamp parts through the Remix API (user 2026-11-13: "the correct play is going through API"). The lamp lenses,
// glass and lamp units the lit-parts code names (litparts.cpp) are API meshes in the car's own space, drawn every frame
// the game draws them with the draw's world matrix and an "off" or "on" look; the D3D9 draw is skipped. Replaces the
// twin textures (a new hash per lit look, a generated mod material that loads seconds later: lamps without texture, the
// glow arriving late, z-fighting with the unlit mesh). Remix binds the material to the mesh, so each look is its own
// mesh over the same vertices, made the first time the look is needed.
struct LampLookKey {
  uint64_t tex; uint32_t emissive; uint32_t glowQ; uint32_t state;
  bool operator==(const LampLookKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct LampLookHash {
  size_t operator()(const LampLookKey& k) const {
    return static_cast<size_t>(k.tex * 0x9E3779B97F4A7C15ull ^ (uint64_t(k.emissive) << 32) ^ (uint64_t(k.glowQ) << 7) ^ k.state);
  }
};
struct LampEntry {
  bool failed = false;
  std::vector<remixapi_HardcodedVertex> verts;
  std::vector<uint32_t> idx;
  std::unordered_map<LampLookKey, remixapi_MeshHandle, LampLookHash> meshes;
};
std::unordered_map<Key, LampEntry, KeyHash> g_lampParts;
std::unordered_map<LampLookKey, Material, LampLookHash> g_lampMats;
unsigned g_lampDrawn = 0, g_lampBuilt = 0, g_lampRefused = 0;

uint32_t blend_factor(DWORD d3d) {                     // D3DBLEND -> VkBlendFactor
  switch (d3d) {
    case D3DBLEND_ZERO: return 0;
    case D3DBLEND_ONE: return 1;
    case D3DBLEND_SRCCOLOR: return 2;
    case D3DBLEND_INVSRCCOLOR: return 3;
    case D3DBLEND_DESTCOLOR: return 4;
    case D3DBLEND_INVDESTCOLOR: return 5;
    case D3DBLEND_SRCALPHA: return 6;
    case D3DBLEND_INVSRCALPHA: return 7;
    case D3DBLEND_DESTALPHA: return 8;
    case D3DBLEND_INVDESTALPHA: return 9;
    default: return 1;
  }
}

remixapi_MaterialHandle lamp_material(IDirect3DBaseTexture9* t, uint64_t hx, const RbrLampLook& look, uint32_t state,
                                      DWORD alphaFunc, DWORD alphaRef, DWORD wrapU, DWORD wrapV, const LampLookKey& mk) {
  auto it = g_lampMats.find(mk);
  if (it != g_lampMats.end()) return it->second.failed ? nullptr : it->second.h;
  Material& m = g_lampMats[mk];
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
  op.roughnessConstant = look.roughness;
  op.heightTexture = L"";
  op.alphaTestType = 7;
  if (alphaFunc >= D3DCMP_NEVER && alphaFunc < D3DCMP_ALWAYS) {
    op.alphaTestType = static_cast<int>(alphaFunc) - 1;
    op.alphaReferenceValue = static_cast<uint8_t>(alphaRef & 0xFF);
  }
  op.useDrawCallAlphaState = 1;                        // the instance's blend state (glass, lenses) decides
  op.enableDlssControlMask = 1;
  op.dlssControlMaskIntensity = 1.0f;
  op.dlssControlMaskToneStrength = 0.3f;
  op.dlssControlMaskStructuralStrength = 0.7f;
  remixapi_MaterialInfo mi = {};
  mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  mi.pNext = &op;
  mi.hash = 0x524252574C000000ull ^ (LampLookHash()(mk) * 0x100000001B3ull);   // "RBRWL"
  if (!mi.hash) mi.hash = 1;
  mi.albedoTexture = path;
  mi.normalTexture = L"";
  mi.tangentTexture = L"";
  mi.emissiveTexture = look.glow > 0.0f ? (look.emissiveFile && look.emissiveFile[0] ? look.emissiveFile : path) : L"";
  mi.emissiveIntensity = look.glow;
  mi.emissiveColorConstant = { 1.0f, 1.0f, 1.0f };
  mi.filterMode = 1;
  mi.wrapModeU = wrap_mode(wrapU);
  mi.wrapModeV = wrap_mode(wrapV);
  (void)state;
  if (g_createMat(&mi, &m.h) != REMIXAPI_ERROR_CODE_SUCCESS || !m.h) { m.h = nullptr; return nullptr; }
  m.failed = false;
  return m.h;
}

void lamps_reset() {
  for (auto& kv : g_lampParts)
    for (auto& mk : kv.second.meshes) if (mk.second && g_destroyMesh) g_destroyMesh(mk.second);
  g_lampParts.clear();
}

}  // namespace

bool rbrrtx_apiworld_object(void* device, const RbrWorldDraw& d, const float world[16]) {
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (!g_cfgObjects || !g_avail || !g_active || !g_ext || !g_ext->drawInstance || !world) return false;
  DWORD blend = FALSE, at = FALSE, func = D3DCMP_ALWAYS, ref = 0, cull = D3DCULL_CCW, wu = D3DTADDRESS_WRAP, wv = D3DTADDRESS_WRAP;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  if (blend) return false;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  if (at) { dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAFUNC, &func); dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &ref); }
  else func = D3DCMP_ALWAYS;
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
  IDirect3DVertexBuffer9* sb[3] = {};                  // streams 1 and 2 the declaration reads (drivers: UV, colour)
  if (decl) {
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    bool uses[3] = {};
    if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, el, &n)))
      for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
        if (el[i].Stream == 1 || el[i].Stream == 2) uses[el[i].Stream] = true;
    for (int st = 1; st <= 2; ++st) {
      if (!uses[st]) continue;
      UINT so = 0, ss = 0;
      dev->lpVtbl->GetStreamSource(dev, st, &sb[st], &so, &ss);
      if (!sb[st]) continue;
      if (st == 1) { k.s1 = sb[1]; k.s1Gen = g_gen[sb[1]]; k.s1Off = so; k.s1Stride = ss; }
      else { k.s2 = sb[2]; k.s2Gen = g_gen[sb[2]]; k.s2Off = so; k.s2Stride = ss; }
    }
  }
  const bool streamsStatic = !(sb[1] && g_dynamic.count(sb[1])) && !(sb[2] && g_dynamic.count(sb[2]));
  auto it = g_objects.find(k);
  if (it == g_objects.end() && vb && ib && decl && tex && stride && !g_dynamic.count(vb) && !g_dynamic.count(ib) &&
      streamsStatic) {
    Entry& e = g_objects[k];
    e.failed = true;
    const uint64_t hx = rbrrtx_texture_hash(tex);
    std::vector<remixapi_HardcodedVertex> verts;
    std::vector<uint32_t> idx;
    remixapi_MaterialHandle mat = nullptr;
    int why = -1;
    if (!hx) why = 0;
    else if (tex->lpVtbl->GetType(tex) != D3DRTYPE_TEXTURE) why = 1;
    else if ((why = build_vertices(k, verts, idx, e.c, e.r, true)) < 0 &&
             !(mat = material_for(dev, tex, hx, k.state & 0xFFF, func, ref, wu, wv)))
      why = 4;
    if (why < 0) {
      remixapi_MeshInfoSurfaceTriangles sf = {};
      sf.vertices_values = verts.data();
      sf.vertices_count = verts.size();
      sf.indices_values = idx.data();
      sf.indices_count = idx.size();
      sf.material = mat;
      remixapi_MeshInfo mi = {};
      mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      mi.hash = 0x524252444F000000ull + ++g_meshSerial;   // "RBRDO" + a counter
      mi.surfaces_values = &sf;
      mi.surfaces_count = 1;
      if (g_createMesh(&mi, &e.mesh) == REMIXAPI_ERROR_CODE_SUCCESS && e.mesh) { e.failed = false; ++g_objBuilt; }
      else { e.mesh = nullptr; why = 5; }
    }
    if (why >= 0 && ++g_objRefused <= 4)
      wlog(L"api objects: draw with texture %016llX (%u vertices) stays D3D9: %s", hx, d.numV, kRefuse[why]);
    if (!e.failed) {
      e.sway = cull == D3DCULL_NONE;
      if (g_watched.insert(vb).second | g_watched.insert(ib).second) rbrrtx_watch_buffer_writes(vb, ib);
      for (int st = 1; st <= 2; ++st)
        if (sb[st] && g_watched.insert(sb[st]).second) rbrrtx_watch_buffer_writes(sb[st], nullptr);
    }
    it = g_objects.find(k);
  }
  for (int st = 1; st <= 2; ++st)
    if (sb[st]) sb[st]->lpVtbl->Release(sb[st]);
  if (vb) vb->lpVtbl->Release(vb);
  if (ib) ib->lpVtbl->Release(ib);
  if (decl) decl->lpVtbl->Release(decl);
  if (tex) tex->lpVtbl->Release(tex);
  if (it == g_objects.end() || it->second.failed || !it->second.mesh) return false;
  remixapi_InstanceInfoBlendEXT bl = {};               // as the static world: texture x vertex colour, alpha = texture
  bl.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  bl.alphaTestCompareOp = 7;
  bl.srcColorBlendFactor = 1; bl.dstColorBlendFactor = 0; bl.colorBlendOp = 0;
  bl.textureColorArg1Source = 1; bl.textureColorArg2Source = 2; bl.textureColorOperation = 3;
  bl.textureAlphaArg1Source = 1; bl.textureAlphaArg2Source = 2; bl.textureAlphaOperation = 1;
  bl.tFactor = 0xFFFFFFFFu;
  bl.srcAlphaBlendFactor = 1; bl.dstAlphaBlendFactor = 0; bl.alphaBlendOp = 0;
  bl.writeMask = 0xF;
  bl.isVertexColorBakedLighting = 1;
  remixapi_InstanceInfo ii = {};
  ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  ii.pNext = &bl;
  ii.mesh = it->second.mesh;
  ii.doubleSided = it->second.sway ? 1 : 0;
  for (int r = 0; r < 3; ++r)                          // D3D row vectors (p x W) -> Remix 3x4, translation in column 3
    for (int c = 0; c < 4; ++c) ii.transform.matrix[r][c] = world[c * 4 + r];
  if (reinterpret_cast<PFN_remixapi_DrawInstance>(g_ext->drawInstance)(&ii) != REMIXAPI_ERROR_CODE_SUCCESS) return false;
  ++g_objDrawn;
  return true;
}


// A car lamp part (litparts.cpp names it): its API mesh in the look asked for, drawn now with the draw's world matrix
// (row vectors, D3D). False = not taken (the caller draws it as D3D9).
bool rbrrtx_apiworld_lamp(void* device, const RbrWorldDraw& d, const float world[16], const RbrLampLook& look) {
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (!g_avail || !g_active || !g_ext || !g_ext->drawInstance || !world || !g_createMat) return false;
  DWORD blend = FALSE, at = FALSE, func = D3DCMP_ALWAYS, ref = 0, cull = D3DCULL_CCW, wu = D3DTADDRESS_WRAP, wv = D3DTADDRESS_WRAP;
  DWORD src = D3DBLEND_ONE, dst = D3DBLEND_ZERO, zw = TRUE;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  if (blend) {
    dev->lpVtbl->GetRenderState(dev, D3DRS_SRCBLEND, &src);
    dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
  }
  dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  if (at) { dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAFUNC, &func); dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &ref); }
  else func = D3DCMP_ALWAYS;
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
  const uint32_t state = (func & 0xF) | ((ref & 0xFF) << 4) | ((cull & 0x3) << 12) | ((wu & 0x7) << 14) | ((wv & 0x7) << 17);
  const uint64_t hx = tex ? rbrrtx_texture_hash(tex) : 0;
  remixapi_MeshHandle mesh = nullptr;
  auto it = g_lampParts.find(k);
  if (it == g_lampParts.end() && vb && ib && decl && tex && stride && hx && !g_dynamic.count(vb) && !g_dynamic.count(ib) &&
      tex->lpVtbl->GetType(tex) == D3DRTYPE_TEXTURE) {
    LampEntry& e = g_lampParts[k];
    float c[3], r;
    const int why = build_vertices(k, e.verts, e.idx, c, r, true);
    e.failed = why >= 0;
    if (e.failed && ++g_lampRefused <= 4) wlog(L"api lamps: lamp draw with texture %016llX (%u vertices) stays D3D9: %s", hx, d.numV, kRefuse[why]);
    if (!e.failed && (g_watched.insert(vb).second | g_watched.insert(ib).second)) rbrrtx_watch_buffer_writes(vb, ib);
    it = g_lampParts.find(k);
  }
  if (it != g_lampParts.end() && !it->second.failed) {
    LampEntry& e = it->second;
    LampLookKey lk = {};
    lk.tex = hx;
    lk.emissive = look.emissiveFile && look.emissiveFile[0] ? static_cast<uint32_t>(std::wcslen(look.emissiveFile) * 31 + look.emissiveFile[std::wcslen(look.emissiveFile) - 5]) : 0u;
    lk.glowQ = static_cast<uint32_t>(std::lround(look.glow * 1000.0f)) ^ (static_cast<uint32_t>(std::lround(look.roughness * 100.0f)) << 24);
    lk.state = state;
    auto mm = e.meshes.find(lk);
    if (mm == e.meshes.end()) {
      remixapi_MeshHandle h = nullptr;
      if (remixapi_MaterialHandle mat = lamp_material(tex, hx, look, state, func, ref, wu, wv, lk)) {
        remixapi_MeshInfoSurfaceTriangles sf = {};
        sf.vertices_values = e.verts.data();
        sf.vertices_count = e.verts.size();
        sf.indices_values = e.idx.data();
        sf.indices_count = e.idx.size();
        sf.material = mat;
        remixapi_MeshInfo mi = {};
        mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        mi.hash = 0x524252574D000000ull + ++g_meshSerial;   // "RBRWM" + a counter
        mi.surfaces_values = &sf;
        mi.surfaces_count = 1;
        if (g_createMesh(&mi, &h) != REMIXAPI_ERROR_CODE_SUCCESS) h = nullptr;
        else ++g_lampBuilt;
      }
      mm = e.meshes.emplace(lk, h).first;
    }
    mesh = mm->second;
  }
  if (vb) vb->lpVtbl->Release(vb);
  if (ib) ib->lpVtbl->Release(ib);
  if (decl) decl->lpVtbl->Release(decl);
  if (tex) tex->lpVtbl->Release(tex);
  if (!mesh) return false;
  remixapi_InstanceInfoBlendEXT bl = {};
  bl.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  bl.alphaTestEnabled = at ? 1 : 0;
  bl.alphaTestReferenceValue = static_cast<uint8_t>(ref & 0xFF);
  bl.alphaTestCompareOp = at ? (func >= D3DCMP_NEVER && func <= D3DCMP_ALWAYS ? func - 1 : 7) : 7;
  bl.alphaBlendEnabled = blend ? 1 : 0;
  bl.srcColorBlendFactor = blend ? blend_factor(src) : 1;
  bl.dstColorBlendFactor = blend ? blend_factor(dst) : 0;
  bl.colorBlendOp = 0;
  bl.textureColorArg1Source = 1; bl.textureColorArg2Source = 0; bl.textureColorOperation = 1;   // texture
  bl.textureAlphaArg1Source = 1;
  if (blend && look.alphaAdd > 0) {                    // see-through glass: its alpha raised so its tint and reflection show
    bl.textureAlphaArg2Source = 3; bl.textureAlphaOperation = 6;   // texture + tFactor
    bl.tFactor = (static_cast<uint32_t>(look.alphaAdd & 0xFF) << 24) | 0x00FFFFFFu;
  } else {
    bl.textureAlphaArg2Source = 0; bl.textureAlphaOperation = 1;
    bl.tFactor = 0xFFFFFFFFu;
  }
  bl.srcAlphaBlendFactor = 1; bl.dstAlphaBlendFactor = 0; bl.alphaBlendOp = 0;
  bl.writeMask = 0xF;
  bl.isVertexColorBakedLighting = 0;
  remixapi_InstanceInfo ii = {};
  ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  ii.pNext = &bl;
  ii.mesh = mesh;
  ii.doubleSided = cull == D3DCULL_NONE ? 1 : 0;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) ii.transform.matrix[r][c] = world[c * 4 + r];
  for (int r = 0; r < 3; ++r) ii.transform.matrix[r][3] += look.nudge[r];
  (void)zw;
  if (reinterpret_cast<PFN_remixapi_DrawInstance>(g_ext->drawInstance)(&ii) != REMIXAPI_ERROR_CODE_SUCCESS) return false;
  ++g_lampDrawn;
  return true;
}

namespace {
void objects_reset_outer() { objects_reset(); }
}  // namespace

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
    if (g_objBuilt || g_objDrawn)
      wlog(L"api objects (drivers): %zu meshes, %.1f drawn per frame, %u built since last line", g_objects.size(),
           static_cast<double>(g_objDrawn) / g_logFrames, g_objBuilt);
    if (g_lampBuilt || g_lampDrawn)
      wlog(L"api lamps: %zu car lamp parts, %.1f drawn per frame through the API, %u looks built since last line", g_lampParts.size(),
           static_cast<double>(g_lampDrawn) / g_logFrames, g_lampBuilt);
    g_objDrawn = g_objBuilt = 0;
    g_lampDrawn = g_lampBuilt = 0;
    g_logFrames = g_taken = g_passed = g_built = g_hidden = g_shown = 0;
    g_sentMB = 0;
    std::memset(g_refused, 0, sizeof(g_refused));
  }
  ++g_frame;
  g_builtFrame = g_vertsFrame = g_drawnWorld = 0;
}

void rbrrtx_apiworld_reset(const wchar_t* why) {
  if (!g_objects.empty()) wlog(L"api objects: %zu meshes released (%s)", g_objects.size(), why);
  objects_reset_outer();
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
  g_watched.clear();
  g_dynamic.clear();
  g_updates.clear();
  g_live = g_visibleCount = 0;
}

// A new buffer at an address: entries of the old one stay until hidden (in view, not drawn), new draws get new keys.
void rbrrtx_apiworld_forget(const void* buffer) {
  if (!buffer) return;
  ++g_gen[buffer];
  g_dynamic.erase(buffer);                             // a new buffer: may be static again
}

// The game locked a buffer for writing (comp.cpp Lock hooks). Called for every write lock of every buffer once any
// is watched, so the common case is one set lookup.
void rbrrtx_apiworld_written(const void* buffer) {
  if (!buffer || !g_watched.erase(buffer)) return;
  g_dynamic.insert(buffer);
  for (auto it = g_objects.begin(); it != g_objects.end();) {
    if (it->first.vb == buffer || it->first.ib == buffer || it->first.s1 == buffer || it->first.s2 == buffer) {
      if (it->second.mesh && g_destroyMesh) g_destroyMesh(it->second.mesh);
      it = g_objects.erase(it);
    } else ++it;
  }
  unsigned n = 0;
  for (auto it = g_entries.begin(); it != g_entries.end();) {
    if (it->first.vb == buffer || it->first.ib == buffer) { destroy_entry(it->second); it = g_entries.erase(it); ++n; }
    else ++it;
  }
  if (++g_dynamicDropped <= 8)
    wlog(L"api world: buffer %p written by the game: %u instance(s) dropped, it stays D3D9", buffer, n);
}

const wchar_t* rbrrtx_apiworld_cycle() {
  g_mode = (g_mode + 1) % 3;
  return g_mode == 0 ? L"all static scenery and foliage" : g_mode == 1 ? L"foliage only" : L"off (all D3D9)";
}
