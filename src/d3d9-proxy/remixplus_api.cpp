// remixplus_api.cpp - the Remix API on Remix Plus (2.0.0, API 1.0.0) for code written against NVIDIA's header (0.6.4).
//
// Why (2026-10-29): every proxy file includes third_party/remix_c.h (NVIDIA 1.5.2, API 0.6.4). Remix Plus refuses that
// version (remixapi_InitializeLibrary -> 8 INCOMPATIBLE_VERSION, run 20261029-011226: "sun: Remix API unavailable
// (error 8)"), so on Remix Plus the sun, headlights, dust, rain and windscreen were all off. Its table and structs also
// differ (tools/ref/remixplus/docs/RemixApi-Migration.md, section 2):
//  - remixapi_Interface: other slot order (CreateMeshBatched, CreateLightBatched, texture calls inserted);
//  - remixapi_LightInfo: + isDynamic, ignoreViewModel (the 32-bit bridge serializes both, util_remixapi.cpp LightInfoVars);
//  - remixapi_MaterialInfoOpaqueEXT: + four DLSS control-mask fields (read from 1.0.0 callers);
//  - sTypes from 25 on: Remix Plus inserted TEXTURE_INFO = 25, so NVIDIA's PARTICLE_SYSTEM_EXT 25 is its 26 (and
//    GPU_INSTANCING 26 -> 27, CAMERA_MEDIUM 27 -> 28).
// This file alone includes the Remix Plus header (third_party/remixplus/remix_c.h). rbrrtx_remixplus_init initializes the
// library as 1.0.0 and fills the caller's NVIDIA-layout table: calls whose input differs go through a converter here,
// the others are the runtime's own pointers. The rest of the proxy stays on the NVIDIA header, so 1.5.2 still works.
//
// Lights (2026-10-29 evening): a CreateLight of a hash that is live UPDATES that light in place and keeps its buffer
// slot and stable identity, so RTXDI and the denoiser keep its history (rtx_light_manager.cpp addExternalLight; NVIDIA's
// advice too, research/2026-10-22-headlights.md). On Remix Plus the proxy therefore moves a lamp by creating it again
// under the same hash, without DestroyLight (headlights.cpp draw_lamp, comp.cpp update_sun). Hashes pass unchanged.
// Remix Plus drops a create whose hash was destroyed in the same frame (migration guide section 3, Lights): only then
// the create gets another hash (the caller's mixed with a counter), and the caller's later creates follow it.
// rbrrtx_remixplus_frame (each Present) ends a frame. isDynamic = 1 on every light (they move or change).
#define REMIX_ALLOW_X86
#include "common.hpp"
#include "third_party/remixplus/remix_c.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace {

// NVIDIA 0.6.4 layout of remixapi_Interface (third_party/remix_c.h): 22 function pointers, in this order.
struct NvTable {
  void *Shutdown, *CreateMaterial, *DestroyMaterial, *CreateMesh, *DestroyMesh, *SetupCamera, *DrawInstance,
       *CreateLight, *DestroyLight, *DrawLightInstance, *SetConfigVariable, *dxvk_CreateD3D9, *dxvk_RegisterD3D9Device,
       *dxvk_GetExternalSwapchain, *dxvk_GetVkImage, *dxvk_CopyRenderingOutput, *dxvk_SetDefaultOutput,
       *pick_RequestObjectPicking, *pick_HighlightObjects, *Startup, *Present, *SetCameraMediumMaterial;
};
static_assert(sizeof(NvTable) == 22 * sizeof(void*), "NVIDIA remixapi_Interface has 22 slots");

// NVIDIA's remixapi_LightInfo: the Remix Plus struct without its last two fields.
struct NvLightInfo {
  remixapi_StructType sType;
  void* pNext;
  uint64_t hash;
  remixapi_Float3D radiance;
};

// NVIDIA sType numbers that Remix Plus moved (all below 25 are the same).
constexpr int kNvParticleSystem = 25, kNvGpuInstancing = 26, kNvCameraMedium = 27;

remixapi_Interface g_rp = {};
RbrApiExt g_ext = {};
uint32_t g_renames = 0;                                // creates that needed another hash (destroyed this frame)
unsigned g_converted = 0;                              // chains rebuilt (logged once)
std::unordered_map<uint64_t, uint64_t> g_liveHash;     // caller's hash -> hash of its live light in the runtime
std::unordered_map<uintptr_t, uint64_t> g_handleHash;  // latest bridge handle of a caller's light -> caller's hash
std::unordered_map<uint64_t, uintptr_t> g_callerHandle;   // caller's hash -> that latest handle
std::unordered_set<uint64_t> g_destroyedThisFrame;     // runtime hashes destroyed since the last Present

bool in_module(const void* p, HMODULE module) {
  HMODULE m = nullptr;
  return p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 static_cast<LPCWSTR>(p), &m) && m == module;
}

// Size of a chained struct the proxy sends, in NVIDIA's layout, by its NVIDIA sType; 0 = not one the proxy uses.
size_t nv_size(int sType) {
  switch (sType) {
    case REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT: return sizeof(remixapi_MaterialInfoTranslucentEXT);
    case REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT: return offsetof(remixapi_MaterialInfoOpaqueEXT, enableDlssControlMask);
    case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT: return sizeof(remixapi_LightInfoDistantEXT);
    case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT: return sizeof(remixapi_LightInfoSphereEXT);
    case REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT: return sizeof(remixapi_InstanceInfoBlendEXT);
    case kNvParticleSystem: return sizeof(remixapi_InstanceInfoParticleSystemEXT);
    default: return 0;
  }
}

bool needs_copy(int sType) {
  return sType == REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT || sType == kNvParticleSystem ||
         sType == kNvGpuInstancing || sType == kNvCameraMedium;
}

// A pNext chain in NVIDIA's layout, rebuilt in Remix Plus's: nodes up to the last one that differs are copied (bigger
// structs zero-filled, sTypes renumbered), the rest is linked as it is. The copies live in the caller's Chain (stack).
struct Chain {
  alignas(8) unsigned char buf[8][256];
  void* head = nullptr;
};

void* convert_chain(void* nvHead, Chain& c) {
  int last = -1, n = 0;
  for (auto* p = static_cast<const remixapi_MaterialInfoOpaqueEXT*>(nvHead); p && n < 8;
       p = static_cast<const remixapi_MaterialInfoOpaqueEXT*>(p->pNext), ++n)
    if (needs_copy(p->sType)) last = n;
  if (last < 0) return nvHead;
  ++g_converted;
  void** link = &c.head;
  void* src = nvHead;
  for (int i = 0; i <= last && src; ++i) {
    const int sType = *static_cast<const int*>(src);
    const size_t size = nv_size(sType);
    if (!size) break;                                  // unknown struct: linked unchanged from here on
    void* dst = c.buf[i];
    std::memset(dst, 0, sizeof(c.buf[i]));
    std::memcpy(dst, src, size);
    int* t = static_cast<int*>(dst);
    if (sType == kNvParticleSystem) *t = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT;
    else if (sType == kNvGpuInstancing) *t = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
    else if (sType == kNvCameraMedium) *t = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO;
    *link = dst;
    src = static_cast<remixapi_MaterialInfoOpaqueEXT*>(dst)->pNext;   // every struct starts with sType, pNext
    link = &static_cast<remixapi_MaterialInfoOpaqueEXT*>(dst)->pNext;
  }
  *link = src;
  return c.head;
}

remixapi_ErrorCode REMIXAPI_CALL rp_CreateMaterial(const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out) {
  if (!info) return g_rp.CreateMaterial(info, out);
  remixapi_MaterialInfo m = *info;                     // same layout in both headers
  Chain c;
  m.pNext = convert_chain(info->pNext, c);
  return g_rp.CreateMaterial(&m, out);
}

remixapi_ErrorCode REMIXAPI_CALL rp_DrawInstance(const remixapi_InstanceInfo* info) {
  if (!info) return g_rp.DrawInstance(info);
  remixapi_InstanceInfo inst = *info;                  // same layout in both headers
  Chain c;
  inst.pNext = convert_chain(info->pNext, c);
  return g_rp.DrawInstance(&inst);
}

remixapi_ErrorCode REMIXAPI_CALL rp_CreateLight(const remixapi_LightInfo* info, remixapi_LightHandle* out) {
  if (!info) return g_rp.CreateLight(info, out);
  const NvLightInfo* nv = reinterpret_cast<const NvLightInfo*>(info);
  remixapi_LightInfo l = {};
  l.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  Chain c;
  l.pNext = convert_chain(nv->pNext, c);
  const uint64_t caller = nv->hash;
  const auto live = g_liveHash.find(caller);
  uint64_t h = live != g_liveHash.end() ? live->second : caller;   // live: update that light in place
  while (!h || g_destroyedThisFrame.count(h)) h = caller ^ (static_cast<uint64_t>(++g_renames) * 0x9E3779B97F4A7C15ULL);
  l.hash = h;
  l.radiance = nv->radiance;
  l.isDynamic = 1;
  l.ignoreViewModel = 0;
  const auto prev = g_callerHandle.find(caller);
  if (prev != g_callerHandle.end() && g_rp.UpdateLightDefinition) {
    const remixapi_LightHandle hPrev = reinterpret_cast<remixapi_LightHandle>(prev->second);
    if (g_rp.UpdateLightDefinition(hPrev, &l) == REMIXAPI_ERROR_CODE_SUCCESS) {
      if (out) *out = hPrev;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
  }
  const remixapi_ErrorCode err = g_rp.CreateLight(&l, out);
  if (err != REMIXAPI_ERROR_CODE_SUCCESS || !out) return err;
  g_liveHash[caller] = h;
  if (prev != g_callerHandle.end()) g_handleHash.erase(prev->second);
  g_callerHandle[caller] = reinterpret_cast<uintptr_t>(*out);
  g_handleHash[reinterpret_cast<uintptr_t>(*out)] = caller;
  return err;
}

remixapi_ErrorCode REMIXAPI_CALL rp_DestroyLight(remixapi_LightHandle handle) {
  const auto it = g_handleHash.find(reinterpret_cast<uintptr_t>(handle));
  if (it != g_handleHash.end()) {
    const uint64_t caller = it->second;
    const auto live = g_liveHash.find(caller);
    if (live != g_liveHash.end()) { g_destroyedThisFrame.insert(live->second); g_liveHash.erase(live); }
    g_callerHandle.erase(caller);
    g_handleHash.erase(it);
  }
  return g_rp.DestroyLight(handle);
}

}  // namespace

// Fills `nvTable` (NVIDIA's remixapi_Interface, `size` bytes) from Remix Plus. Returns the init error; 0 = success.
int rbrrtx_remixplus_init(HMODULE backend, void* nvTable, size_t size) {
  if (size != sizeof(NvTable)) return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  auto init = reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(backend, "remixapi_InitializeLibrary"));
  if (!init) return REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE;
  remixapi_InitializeLibraryInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  // The header is Kim's (Kim2091/dxvk-remix fnv/main, API 0.1000.1; our runtime builds on it since 2026-11-01). The
  // Remix Plus 2.0.0 release says 1.0.0. Same 39-slot table and the same structs we use (header diff 2026-11-01: only
  // terrain layers, host-setting polls and D3D9 texture import added, all outside the table): either version works here.
  info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
  remixapi_ErrorCode err = init(&info, &g_rp);
  if (err == REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION) {
    info.version = REMIXAPI_VERSION_MAKE(1, 0, 0);
    err = init(&info, &g_rp);
  }
  if (err != REMIXAPI_ERROR_CODE_SUCCESS) return err;
  // Calls beyond NVIDIA's table: standard CreateMesh + imported D3D9 textures (retained/batched extensions are
  // left null because upstream/dev retired them).
  g_ext = {};
  g_ext.importD3D9Texture = GetProcAddress(backend, "remixapi_ImportD3D9Texture");
  g_ext.createMesh = reinterpret_cast<void*>(g_rp.CreateMesh);
  g_ext.setupCamera = reinterpret_cast<void*>(g_rp.SetupCamera);
  g_ext.createMaterial = reinterpret_cast<void*>(g_rp.CreateMaterial);
  g_ext.drawInstance = reinterpret_cast<void*>(g_rp.DrawInstance);
  g_ext.destroyMesh = reinterpret_cast<void*>(g_rp.DestroyMesh);
  g_ext.destroyMaterial = reinterpret_cast<void*>(g_rp.DestroyMaterial);
  g_ext.destroyTexture = reinterpret_cast<void*>(g_rp.DestroyTexture);
  g_ext.setGameValue = reinterpret_cast<void*>(g_rp.SetGameValue);   // particles.cpp: the car kill box (user 2026-11-01)
  g_ext.getGameValue = reinterpret_cast<void*>(g_rp.GetGameValue);
  g_ext.version = info.version;
  // The bridge fills only what it forwards (migration guide section 7); the proxy needs these.
  if (!in_module(reinterpret_cast<const void*>(g_rp.CreateLight), backend) ||
      !in_module(reinterpret_cast<const void*>(g_rp.DestroyLight), backend) ||
      !in_module(reinterpret_cast<const void*>(g_rp.DrawLightInstance), backend))
    return REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE;
  NvTable t = {};
  t.Shutdown = reinterpret_cast<void*>(g_rp.Shutdown);
  t.CreateMaterial = g_rp.CreateMaterial ? reinterpret_cast<void*>(&rp_CreateMaterial) : nullptr;
  t.DestroyMaterial = reinterpret_cast<void*>(g_rp.DestroyMaterial);
  t.CreateMesh = reinterpret_cast<void*>(g_rp.CreateMesh);   // remixapi_MeshInfo and its surfaces: same layout
  t.DestroyMesh = reinterpret_cast<void*>(g_rp.DestroyMesh);
  t.DrawInstance = g_rp.DrawInstance ? reinterpret_cast<void*>(&rp_DrawInstance) : nullptr;
  t.CreateLight = reinterpret_cast<void*>(&rp_CreateLight);
  t.DestroyLight = reinterpret_cast<void*>(&rp_DestroyLight);
  t.DrawLightInstance = reinterpret_cast<void*>(g_rp.DrawLightInstance);
  t.SetConfigVariable = reinterpret_cast<void*>(g_rp.SetConfigVariable);
  // SetupCamera, the dxvk_ and pick_ calls, Startup, Present: null through the bridge, and the proxy calls none of them.
  std::memcpy(nvTable, &t, sizeof(t));
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

const RbrApiExt* rbrrtx_api_ext() { return g_ext.version ? &g_ext : nullptr; }

unsigned rbrrtx_remixplus_converted() { return g_converted; }

void rbrrtx_remixplus_frame() { g_destroyedThisFrame.clear(); }

unsigned rbrrtx_remixplus_renames() { return g_renames; }
