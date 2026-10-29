// remixplus_api_test.cpp - remixplus_api.cpp against a fake Remix Plus runtime in this exe: the NVIDIA-layout table it
// fills, lights (own hash per create, isDynamic), particle sType 25 -> 26, the opaque material's bigger struct.
// Build (x86 cl, from src\d3d9-proxy):
//   cl /nologo /EHsc /std:c++17 /DUNICODE /D_UNICODE test\remixplus_api_test.cpp remixplus_api.cpp /Fe:test\remixplus_api_test.exe /Fo:obj\
// Run: test\remixplus_api_test.exe
#define REMIX_ALLOW_X86
#include "../common.hpp"
#include "../third_party/remixplus/remix_c.h"
#include <cstddef>
#include <cstdio>
#include <cstring>

#pragma comment(linker, "/EXPORT:remixapi_InitializeLibrary=_fake_init@8")

namespace {

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

remixapi_LightInfo g_light = {};
remixapi_LightInfoSphereEXT g_sphere = {};
remixapi_InstanceInfoParticleSystemEXT g_particle = {};
int g_particleSType = 0;
remixapi_MaterialInfoOpaqueEXT g_opaque = {};
int g_destroyed = 0;

remixapi_ErrorCode REMIXAPI_CALL fake_CreateLight(const remixapi_LightInfo* info, remixapi_LightHandle* out) {
  g_light = *info;
  if (info->pNext) g_sphere = *static_cast<const remixapi_LightInfoSphereEXT*>(info->pNext);
  *out = reinterpret_cast<remixapi_LightHandle>(static_cast<uintptr_t>(0x100));
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
remixapi_ErrorCode REMIXAPI_CALL fake_DestroyLight(remixapi_LightHandle) { ++g_destroyed; return REMIXAPI_ERROR_CODE_SUCCESS; }
remixapi_ErrorCode REMIXAPI_CALL fake_DrawLightInstance(remixapi_LightHandle) { return REMIXAPI_ERROR_CODE_SUCCESS; }
remixapi_ErrorCode REMIXAPI_CALL fake_DrawInstance(const remixapi_InstanceInfo* info) {
  const auto* p = static_cast<const remixapi_InstanceInfoParticleSystemEXT*>(info->pNext);
  g_particleSType = p ? p->sType : 0;
  if (p) g_particle = *p;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
remixapi_ErrorCode REMIXAPI_CALL fake_CreateMaterial(const remixapi_MaterialInfo* info, remixapi_MaterialHandle*) {
  if (info->pNext) g_opaque = *static_cast<const remixapi_MaterialInfoOpaqueEXT*>(info->pNext);
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

}  // namespace

extern "C" remixapi_ErrorCode REMIXAPI_CALL fake_init(const remixapi_InitializeLibraryInfo* info, remixapi_Interface* out) {
  if (info->version != REMIXAPI_VERSION_MAKE(1, 0, 0)) return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
  *out = {};
  out->CreateLight = fake_CreateLight;
  out->DestroyLight = fake_DestroyLight;
  out->DrawLightInstance = fake_DrawLightInstance;
  out->DrawInstance = fake_DrawInstance;
  out->CreateMaterial = fake_CreateMaterial;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

int main() {
  void* table[22] = {};
  CHECK(rbrrtx_remixplus_init(GetModuleHandleW(nullptr), table, sizeof(table)) == 0);
  CHECK(table[7] != nullptr && table[8] == reinterpret_cast<void*>(&fake_DestroyLight));   // NVIDIA slots 7, 8, 9
  CHECK(table[9] == reinterpret_cast<void*>(&fake_DrawLightInstance));
  CHECK(table[5] == nullptr);                                                               // SetupCamera: none

  // NVIDIA LightInfo (no isDynamic / ignoreViewModel) followed by stack garbage.
  struct { remixapi_StructType sType; void* pNext; uint64_t hash; remixapi_Float3D radiance; uint32_t garbage[2]; } nv = {};
  remixapi_LightInfoSphereEXT sphere = {};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = 0.05f;
  nv.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  nv.pNext = &sphere;
  nv.hash = 0x524252'53554E00ULL;
  nv.radiance = { 1.0f, 2.0f, 3.0f };
  nv.garbage[0] = nv.garbage[1] = 0xCCCCCCCC;
  auto createLight = reinterpret_cast<PFN_remixapi_CreateLight>(table[7]);
  remixapi_LightHandle h = nullptr;
  CHECK(createLight(reinterpret_cast<const remixapi_LightInfo*>(&nv), &h) == REMIXAPI_ERROR_CODE_SUCCESS);
  const uint64_t first = g_light.hash;
  CHECK(g_light.isDynamic == 1 && g_light.ignoreViewModel == 0);
  CHECK(g_light.radiance.z == 3.0f && g_sphere.radius == 0.05f);
  CHECK(first != 0 && first != nv.hash);
  createLight(reinterpret_cast<const remixapi_LightInfo*>(&nv), &h);
  CHECK(g_light.hash != first);                        // same caller hash, re-created: a new hash each time

  // Particle system: NVIDIA sType 25 -> Remix Plus 26 (TEXTURE_INFO took 25).
  remixapi_InstanceInfoParticleSystemEXT ps = {};
  ps.sType = static_cast<remixapi_StructType>(25);
  ps.maxNumParticles = 77;
  remixapi_InstanceInfo inst = {};
  inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  inst.pNext = &ps;
  CHECK(reinterpret_cast<PFN_remixapi_DrawInstance>(table[6])(&inst) == REMIXAPI_ERROR_CODE_SUCCESS);
  CHECK(g_particleSType == REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT && g_particleSType == 26);
  CHECK(g_particle.maxNumParticles == 77);
  CHECK(ps.sType == 25);                               // the caller's struct is not changed

  // Opaque material: NVIDIA's shorter struct, the bytes after it are garbage; Remix Plus must see zeros there.
  remixapi_MaterialInfoOpaqueEXT big;
  std::memset(&big, 0xCC, sizeof(big));
  std::memset(&big, 0, offsetof(remixapi_MaterialInfoOpaqueEXT, enableDlssControlMask));
  big.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  big.pNext = nullptr;
  big.roughnessConstant = 0.5f;
  remixapi_MaterialInfo mat = {};
  mat.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  mat.pNext = &big;
  remixapi_MaterialHandle mh = nullptr;
  CHECK(reinterpret_cast<PFN_remixapi_CreateMaterial>(table[1])(&mat, &mh) == REMIXAPI_ERROR_CODE_SUCCESS);
  CHECK(g_opaque.roughnessConstant == 0.5f && g_opaque.enableDlssControlMask == 0 && g_opaque.dlssControlMaskIntensity == 0.0f);
  CHECK(rbrrtx_remixplus_converted() == 2);            // the particle chain and the material chain

  std::printf(g_fail ? "%d check(s) failed\n" : "remixplus_api: all checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
