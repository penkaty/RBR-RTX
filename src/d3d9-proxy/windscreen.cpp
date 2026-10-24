// windscreen.cpp - rain drops and snow on the car's windscreen as Remix API meshes (user 2026-10-23, run 4).
//
// RBR draws its windscreen rain/snow (the 2D NParticleSystem2D, WS_* textures, render_windscreen_effects 0x452020)
// into an off-screen texture and shows it on a flat plane in front of the windscreen opening. Under Remix that texture
// never reaches the path-traced glass, so the windscreen stays dry (docs/research/particles-remix.md "Windscreen rain").
//
// Here: a plane on the car where the windscreen is, in the car's model frame (headlights.cpp rbrrtx_car_model_frame):
// fitted to the car's own windscreen mesh (e_window_f, every model of the install; user 2026-10-23 "read each car's
// windscreen plane from its glass mesh"), measured as the game draws it in the first outside view (run 9:
// headlights.cpp glass_calibrate), else placed from the head lamp nodes with particles.ini [windscreen] offsets. Drops live in the plane's own 2D
// coordinates (u across -1..1, v up the glass 0..1) and are drawn every frame as instances of one water lens mesh
// (translucent, index 1.33: the path tracer refracts the road through them). Snow lands as flat white flakes that
// shrink over melt_seconds and end as a small drop. Rain/snow amount = the game's weather (particles.cpp
// rbrrtx_precip_state), more with speed. Above move_above_kmh the air pushes the drops from the centre to the edges
// (faster with speed, a little up the glass); slower, big drops run down. Drops leaving the plane are gone.
// Not yet: wipers (their angle is not known), drops joining.
// The bridge drops the GPU-instancing extension (bridge client remix_api.cpp has no case for it), so every drop is its
// own DrawInstance of the shared mesh; max_drops caps the calls.
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <d3d9.h>
#include "third_party/remix_c.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

#define plog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

struct Cfg {
  bool enabled = true;
  // The plane in the car's model frame, from the middle of the two head lamps: metres back (against the model's
  // forward axis) and up, for the bottom and top edges; widths at the bottom and the top.
  float bottomBack = 1.15f, bottomUp = 0.42f, topBack = 1.83f, topUp = 0.92f, widthBottom = 1.65f, widthTop = 1.20f;   // user, Golf II (run 5)
  float lift = 0.004f;                                 // drops this far outside the plane (m)
  float dropsPerSecond = 40.0f;                        // rain hitting the glass at 100 % intensity, standing
  float speedFactor = 1.0f;                            // x (1 + km/h / 50 x this): more hits at speed
  float dropMin = 0.0015f, dropMax = 0.004f;           // drop radius (m)
  float moveAboveKmh = 50.0f;                          // above this the air pushes the drops outward
  float flowSpeed = 0.25f;                             // m/s on the glass per 50 km/h above move_above_kmh
  float flowUp = 0.3f;                                 // share of the flow that pushes up the glass
  float slideSize = 0.003f, slideSpeed = 0.03f;        // slower: drops this big run down at this speed (m/s)
  float flakesPerSecond = 25.0f;                       // snow on the glass at 100 % intensity, standing
  float flakeMin = 0.004f, flakeMax = 0.009f;          // flake radius (m)
  float meltMin = 6.0f, meltMax = 12.0f;               // seconds until a flake is water
  int maxDrops = 160;
  bool showPlane = false;                              // a faint red plane where the drops sit (to place it)
  int useGlass = 2;                                    // 0 lamp plane, 1 glass plane, 2 lamps when symmetric else glass
  float inset = 0.04f;                                 // glass edges moved in this far (m): rubber, frame
  // Drop shape (user 2026-10-24 run 9: drops looked blue): lens height / radius and refractive index. A flatter or
  // weaker lens bends less of the sky into the drop.
  float dropHeight = 0.35f, dropIor = 1.33f;
} g_cfg;
unsigned g_lensGen = 0;                                // new lens/material hashes when drop_height / drop_ior change

wchar_t g_path[MAX_PATH * 2] = {};
FILETIME g_time = {};
ULONGLONG g_checked = 0;

float ini_f(const wchar_t* key, float d) {
  wchar_t v[64] = {};
  GetPrivateProfileStringW(L"windscreen", key, L"", v, 64, g_path);
  return v[0] ? static_cast<float>(_wtof(v)) : d;
}

void load() {
  Cfg c;
  c.enabled = ini_f(L"enabled", 1.0f) >= 0.5f;
  c.bottomBack = ini_f(L"bottom_back", c.bottomBack);
  c.bottomUp = ini_f(L"bottom_up", c.bottomUp);
  c.topBack = ini_f(L"top_back", c.topBack);
  c.topUp = ini_f(L"top_up", c.topUp);
  c.widthBottom = ini_f(L"width_bottom", c.widthBottom);
  c.widthTop = ini_f(L"width_top", c.widthTop);
  c.lift = ini_f(L"lift", c.lift);
  c.dropsPerSecond = ini_f(L"drops_per_second", c.dropsPerSecond);
  c.speedFactor = ini_f(L"speed_factor", c.speedFactor);
  c.dropMin = ini_f(L"drop_size_min", c.dropMin);
  c.dropMax = (std::max)(c.dropMin, ini_f(L"drop_size_max", c.dropMax));
  c.moveAboveKmh = ini_f(L"move_above_kmh", c.moveAboveKmh);
  c.flowSpeed = ini_f(L"flow_speed", c.flowSpeed);
  c.flowUp = ini_f(L"flow_up", c.flowUp);
  c.slideSize = ini_f(L"slide_size", c.slideSize);
  c.slideSpeed = ini_f(L"slide_speed", c.slideSpeed);
  c.flakesPerSecond = ini_f(L"flakes_per_second", c.flakesPerSecond);
  c.flakeMin = ini_f(L"flake_size_min", c.flakeMin);
  c.flakeMax = (std::max)(c.flakeMin, ini_f(L"flake_size_max", c.flakeMax));
  c.meltMin = ini_f(L"melt_seconds_min", c.meltMin);
  c.meltMax = (std::max)(c.meltMin, ini_f(L"melt_seconds_max", c.meltMax));
  c.maxDrops = static_cast<int>(ini_f(L"max_drops", static_cast<float>(c.maxDrops)));
  if (c.maxDrops < 0) c.maxDrops = 0;
  if (c.maxDrops > 600) c.maxDrops = 600;
  c.showPlane = ini_f(L"show_plane", 0.0f) >= 0.5f;
  c.useGlass = static_cast<int>(ini_f(L"use_glass", 2.0f));
  c.inset = ini_f(L"inset", c.inset);
  c.dropHeight = (std::max)(0.05f, (std::min)(1.0f, ini_f(L"drop_height", c.dropHeight)));
  c.dropIor = (std::max)(1.0f, (std::min)(2.0f, ini_f(L"drop_ior", c.dropIor)));
  if (c.dropHeight != g_cfg.dropHeight || c.dropIor != g_cfg.dropIor) ++g_lensGen;   // make_all builds them again
  g_cfg = c;
  plog(L"windscreen: %s; plane %.2f back %.2f up -> %.2f back %.2f up, width %.2f/%.2f; rain %.0f/s, drops %.1f-%.1f mm, "
       L"move above %.0f km/h at %.2f m/s per 50 km/h; snow %.0f/s, flakes %.1f-%.1f mm, melt %.0f-%.0f s; at most %d%s",
       c.enabled ? L"on" : L"off", c.bottomBack, c.bottomUp, c.topBack, c.topUp, c.widthBottom, c.widthTop,
       c.dropsPerSecond, c.dropMin * 1000, c.dropMax * 1000, c.moveAboveKmh, c.flowSpeed, c.flakesPerSecond,
       c.flakeMin * 1000, c.flakeMax * 1000, c.meltMin, c.meltMax, c.maxDrops, c.showPlane ? L"; plane shown" : L"");
}

void check_cfg() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_checked < 1000) return;
  g_checked = now;
  WIN32_FILE_ATTRIBUTE_DATA a = {};
  if (!GetFileAttributesExW(g_path, GetFileExInfoStandard, &a)) return;
  if (CompareFileTime(&a.ftLastWriteTime, &g_time) == 0) return;
  const bool first = g_time.dwLowDateTime == 0 && g_time.dwHighDateTime == 0;
  g_time = a.ftLastWriteTime;
  if (!first) load();
}

// ---------------------------------------------------------------- simulation (plane coordinates)

struct Drop {
  float u, v;                                          // u across -1..1, v up the glass 0..1
  float r;                                             // radius (m)
  bool flake;                                          // snow not yet melted
  float melt, meltTime;                                // flake: 0..1 of meltTime seconds
  float age;
};
std::vector<Drop> g_drops;
float g_spawnDebt = 0.0f;
LARGE_INTEGER g_lastT = {};
float g_lastCar[3] = {}, g_kmh = 0.0f;
bool g_haveCar = false;

float frand() { return static_cast<float>(rand()) / RAND_MAX; }

void spawn(bool flake) {
  if (static_cast<int>(g_drops.size()) >= g_cfg.maxDrops) g_drops.erase(g_drops.begin());   // the oldest goes
  Drop d = {};
  d.u = frand() * 2.0f - 1.0f;
  d.v = frand();
  d.flake = flake;
  d.r = flake ? g_cfg.flakeMin + frand() * (g_cfg.flakeMax - g_cfg.flakeMin)
              : g_cfg.dropMin + frand() * (g_cfg.dropMax - g_cfg.dropMin);
  d.meltTime = g_cfg.meltMin + frand() * (g_cfg.meltMax - g_cfg.meltMin);
  g_drops.push_back(d);
}

// length = the plane's length up the glass (m); wBottom/wTop = its widths.
void simulate(float dt, int kind, float intensity, float length, float wBottom, float wTop) {
  const float amount = intensity / 100.0f * (1.0f + g_kmh / 50.0f * g_cfg.speedFactor);
  g_spawnDebt += dt * amount * (kind == 2 ? g_cfg.flakesPerSecond : kind == 1 ? g_cfg.dropsPerSecond : 0.0f);
  while (g_spawnDebt >= 1.0f) { spawn(kind == 2); g_spawnDebt -= 1.0f; }
  const float over = g_kmh - g_cfg.moveAboveKmh;
  const float flow = over > 0.0f ? (std::min)(g_cfg.flowSpeed * over / 50.0f, 3.0f) : 0.0f;
  for (Drop& d : g_drops) {
    d.age += dt;
    const float halfW = 0.5f * (wBottom + (wTop - wBottom) * d.v);
    if (d.flake) {                                     // snow: stays, melts, becomes a small drop
      d.melt += dt / d.meltTime;
      if (d.melt >= 1.0f) { d.flake = false; d.r *= 0.45f; }
      if (flow > 0.0f) {                               // packed flakes move slowly in strong wind
        const float x = d.u * halfW, y = (d.v - 0.45f) * length;
        const float l = std::sqrt(x * x + y * y) + 1e-4f;
        d.u += x / l * flow * 0.2f * dt / halfW;
        d.v += (y / l + g_cfg.flowUp) * flow * 0.2f * dt / length;
      }
      continue;
    }
    if (flow > 0.0f) {                                 // the air pushes water from the centre to the edges, and up
      const float x = d.u * halfW, y = (d.v - 0.45f) * length;
      const float l = std::sqrt(x * x + y * y) + 1e-4f;
      d.u += x / l * flow * dt / halfW;
      d.v += (y / l * flow + g_cfg.flowUp * flow) * dt / length;
    } else if (d.r >= g_cfg.slideSize) {               // slow: big drops run down
      d.v -= g_cfg.slideSpeed * dt / length;
    }
  }
  for (size_t i = 0; i < g_drops.size();) {
    const Drop& d = g_drops[i];
    if (d.u < -1.0f || d.u > 1.0f || d.v < 0.0f || d.v > 1.0f || d.age > 90.0f) g_drops.erase(g_drops.begin() + i);
    else ++i;
  }
}

// ---------------------------------------------------------------- Remix side

const remixapi_Interface* g_api = nullptr;
remixapi_MaterialHandle g_water = nullptr, g_snowMat = nullptr, g_planeMat = nullptr;
remixapi_MeshHandle g_lens = nullptr, g_disc = nullptr, g_plane = nullptr;
bool g_failed = false;
wchar_t g_flakeTex[MAX_PATH * 2] = {};

// Water lens: a flattened spherical cap (height 0.35 of the radius) closed by a flat base, local y = out of the glass.
remixapi_MeshHandle make_lens(remixapi_MaterialHandle mat) {
  constexpr int kSeg = 12, kRings = 4;
  static remixapi_HardcodedVertex v[kSeg * kRings + 2 + kSeg] = {};
  static uint32_t idx[kSeg * 6 * kRings + kSeg * 3] = {};
  int nv = 0, ni = 0;
  for (int r = 0; r < kRings; ++r) {                  // ring r at polar angle (r + 1) / (kRings + 1) of 90 degrees
    const float th = 1.5707963f * (1.0f - static_cast<float>(r) / kRings);   // r 0 = the rim
    for (int s = 0; s < kSeg; ++s) {
      const float ph = 6.2831853f * s / kSeg;
      remixapi_HardcodedVertex& o = v[nv++];
      const float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
      o.position[0] = x; o.position[1] = y * g_cfg.dropHeight; o.position[2] = z;
      o.normal[0] = x; o.normal[1] = y; o.normal[2] = z;
      o.texcoord[0] = 0.5f + 0.5f * x; o.texcoord[1] = 0.5f + 0.5f * z;
      o.color = 0xFFFFFFFF;
    }
  }
  const int top = nv;
  v[nv].position[1] = g_cfg.dropHeight; v[nv].normal[1] = 1.0f; v[nv].texcoord[0] = v[nv].texcoord[1] = 0.5f; v[nv].color = 0xFFFFFFFF;
  ++nv;
  for (int r = 0; r + 1 < kRings; ++r)
    for (int s = 0; s < kSeg; ++s) {
      const uint32_t a = r * kSeg + s, b = r * kSeg + (s + 1) % kSeg, c = (r + 1) * kSeg + s, d = (r + 1) * kSeg + (s + 1) % kSeg;
      const uint32_t t[6] = { a, c, b, b, c, d };
      std::memcpy(&idx[ni], t, sizeof(t));
      ni += 6;
    }
  for (int s = 0; s < kSeg; ++s) {                     // cap top
    const uint32_t t[3] = { static_cast<uint32_t>((kRings - 1) * kSeg + s), static_cast<uint32_t>(top),
                            static_cast<uint32_t>((kRings - 1) * kSeg + (s + 1) % kSeg) };
    std::memcpy(&idx[ni], t, sizeof(t));
    ni += 3;
  }
  const int base = nv;                                 // flat base (facing the glass): its own centre vertex
  v[nv].normal[1] = -1.0f; v[nv].texcoord[0] = v[nv].texcoord[1] = 0.5f; v[nv].color = 0xFFFFFFFF;
  ++nv;
  for (int s = 0; s < kSeg; ++s) {
    const uint32_t t[3] = { static_cast<uint32_t>(base), static_cast<uint32_t>(s), static_cast<uint32_t>((s + 1) % kSeg) };
    std::memcpy(&idx[ni], t, sizeof(t));
    ni += 3;
  }
  remixapi_MeshInfoSurfaceTriangles tri = {};
  tri.vertices_values = v;
  tri.vertices_count = nv;
  tri.indices_values = idx;
  tri.indices_count = ni;
  tri.material = mat;
  remixapi_MeshInfo mesh = {};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 0x52425254585F776Cull ^ (static_cast<uint64_t>(g_lensGen) << 40);   // "RBRTX_wl"
  mesh.surfaces_values = &tri;
  mesh.surfaces_count = 1;
  remixapi_MeshHandle h = nullptr;
  return g_api->CreateMesh(&mesh, &h) == REMIXAPI_ERROR_CODE_SUCCESS ? h : nullptr;
}

// Flat unit square in the local x/z plane facing +y (the flake disc via its texture's alpha; the debug plane).
remixapi_MeshHandle make_square(remixapi_MaterialHandle mat, uint64_t hash) {
  static remixapi_HardcodedVertex v[2][4] = {};
  const int k = hash & 1;
  const float q[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
  for (int i = 0; i < 4; ++i) {
    remixapi_HardcodedVertex& o = v[k][i];
    o = {};
    o.position[0] = q[i][0]; o.position[2] = q[i][1];
    o.normal[1] = 1.0f;
    o.texcoord[0] = 0.5f + 0.5f * q[i][0]; o.texcoord[1] = 0.5f + 0.5f * q[i][1];
    o.color = 0xFFFFFFFF;
  }
  static const uint32_t idx[6] = { 0, 2, 1, 0, 3, 2 };
  remixapi_MeshInfoSurfaceTriangles tri = {};
  tri.vertices_values = v[k];
  tri.vertices_count = 4;
  tri.indices_values = idx;
  tri.indices_count = 6;
  tri.material = mat;
  remixapi_MeshInfo mesh = {};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = hash;
  mesh.surfaces_values = &tri;
  mesh.surfaces_count = 1;
  remixapi_MeshHandle h = nullptr;
  return g_api->CreateMesh(&mesh, &h) == REMIXAPI_ERROR_CODE_SUCCESS ? h : nullptr;
}

remixapi_MaterialHandle make_opaque(uint64_t hash, const wchar_t* albedo, remixapi_Float3D colour, float opacity) {
  remixapi_MaterialInfoOpaqueEXT op = {};
  op.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  op.roughnessTexture = L"";
  op.metallicTexture = L"";
  op.heightTexture = L"";
  op.albedoConstant = colour;
  op.opacityConstant = opacity;
  op.roughnessConstant = 0.8f;
  op.useDrawCallAlphaState = 1;                        // alpha from the blend extension at draw time
  remixapi_MaterialInfo mi = {};
  mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  mi.pNext = &op;
  mi.hash = hash;
  mi.albedoTexture = albedo;
  mi.normalTexture = L"";
  mi.tangentTexture = L"";
  mi.emissiveTexture = L"";
  mi.filterMode = 1;
  mi.wrapModeU = mi.wrapModeV = 2;
  remixapi_MaterialHandle h = nullptr;
  return g_api->CreateMaterial(&mi, &h) == REMIXAPI_ERROR_CODE_SUCCESS ? h : nullptr;
}

unsigned g_madeGen = 0;

bool make_all() {
  if (g_lens && g_madeGen != g_lensGen) {              // drop_height / drop_ior changed: a new lens (new hashes: a
    g_api->DestroyMesh(g_lens);                        // destroy and a create of one hash in a frame can lose it)
    g_api->DestroyMaterial(g_water);
    g_lens = nullptr;
    g_water = nullptr;
  }
  if (g_lens && g_disc && g_plane) return true;
  if (g_failed || !g_api) return false;
  g_madeGen = g_lensGen;
  remixapi_MaterialInfoTranslucentEXT tr = {};
  tr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
  tr.transmittanceTexture = L"";
  tr.refractiveIndex = g_cfg.dropIor;                  // water 1.33
  tr.transmittanceColor = { 0.97f, 0.98f, 1.0f };
  tr.transmittanceMeasurementDistance = 0.05f;
  tr.useDiffuseLayer = 0;
  remixapi_MaterialInfo mi = {};
  mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  mi.pNext = &tr;
  mi.hash = 0x52425254585F7777ull ^ (static_cast<uint64_t>(g_lensGen) << 40);   // "RBRTX_ww"
  mi.albedoTexture = L"";
  mi.normalTexture = L"";
  mi.tangentTexture = L"";
  mi.emissiveTexture = L"";
  mi.filterMode = 1;
  if (!g_water && g_api->CreateMaterial(&mi, &g_water) != REMIXAPI_ERROR_CODE_SUCCESS) g_water = nullptr;
  if (g_disc && g_plane) {                             // only the lens was made again
    g_lens = g_water ? make_lens(g_water) : nullptr;
    plog(L"windscreen: drop lens made again: height %.2f x radius, index %.2f", g_cfg.dropHeight, g_cfg.dropIor);
    return g_lens != nullptr;
  }
  const bool haveFlake = GetFileAttributesW(g_flakeTex) != INVALID_FILE_ATTRIBUTES;
  g_snowMat = make_opaque(0x52425254585F7773ull, haveFlake ? g_flakeTex : L"", { 0.95f, 0.96f, 0.98f }, 1.0f);   // "RBRTX_ws"
  g_planeMat = make_opaque(0x52425254585F7770ull, L"", { 1.0f, 0.1f, 0.1f }, 0.25f);                          // "RBRTX_wp"
  g_lens = g_water ? make_lens(g_water) : nullptr;
  g_disc = g_snowMat ? make_square(g_snowMat, 0x52425254585F7764ull) : nullptr;    // "RBRTX_wd"
  g_plane = g_planeMat ? make_square(g_planeMat, 0x52425254585F7765ull) : nullptr; // "RBRTX_we"
  if (!g_lens || !g_disc || !g_plane) {
    plog(L"windscreen: Remix meshes/materials failed (lens %p, flake %p, plane %p): windscreen drops off", g_lens, g_disc, g_plane);
    g_failed = true;
    return false;
  }
  plog(L"windscreen: water lens, flake (%s) and plane made", haveFlake ? g_flakeTex : L"no texture");
  return true;
}

struct V { float x, y, z; };
V operator+(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V operator-(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V operator*(V a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V norm(V a) { const float l = std::sqrt(dot(a, a)); return l > 1e-6f ? a * (1.0f / l) : a; }

// Model frame M (rows x, y, z, position) -> world.
V to_world(const float M[16], V p) {
  return { p.x * M[0] + p.y * M[4] + p.z * M[8] + M[12], p.x * M[1] + p.y * M[5] + p.z * M[9] + M[13],
           p.x * M[2] + p.y * M[6] + p.z * M[10] + M[14] };
}
V dir_world(const float M[16], V d) {
  return { d.x * M[0] + d.y * M[4] + d.z * M[8], d.x * M[1] + d.y * M[5] + d.z * M[9], d.x * M[2] + d.y * M[6] + d.z * M[10] };
}

void blend_alpha(remixapi_InstanceInfoBlendEXT& b) {
  b = {};
  b.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  b.alphaTestCompareOp = 7;
  b.alphaBlendEnabled = 1;
  b.srcColorBlendFactor = 6;                           // src alpha
  b.dstColorBlendFactor = 7;                           // 1 - src alpha
  b.textureColorArg1Source = 1;
  b.textureColorArg2Source = 2;
  b.textureColorOperation = 3;
  b.textureAlphaArg1Source = 1;
  b.textureAlphaArg2Source = 2;
  b.textureAlphaOperation = 3;
  b.srcAlphaBlendFactor = 1;
  b.writeMask = 0xF;
}

// One instance: local x = across (ax), y = out of the glass (ay), z = up the glass (az), each already scaled.
void draw(remixapi_MeshHandle mesh, V pos, V ax, V ay, V az, void* pNext) {
  remixapi_InstanceInfo inst = {};
  inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  inst.pNext = pNext;
  inst.mesh = mesh;
  inst.transform = { { { ax.x, ay.x, az.x, pos.x }, { ax.y, ay.y, az.y, pos.y }, { ax.z, ay.z, az.z, pos.z } } };
  inst.doubleSided = 1;
  g_api->DrawInstance(&inst);
}

ULONGLONG g_lastLog = 0;
int g_lastKind = -1;

// 4x4 row-vector matrices (rows x, y, z, position): out = a * b; rigid inverse.
void mul4(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}
void inverse_rigid(const float* m, float* out) {
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) out[r * 4 + c] = m[c * 4 + r];
  for (int c = 0; c < 3; ++c) out[12 + c] = -(m[12] * out[c] + m[13] * out[4 + c] + m[14] * out[8 + c]);
  out[3] = out[7] = out[11] = 0.0f;
  out[15] = 1.0f;
}

// Cockpit views (car body not drawn): the drops ride on the camera, which RBR fixes to the cockpit. The model root
// relative to the camera (g_rel: M = g_rel * camera) is measured from RBR's car object while the car stands (then the
// object and the drawn car agree) and kept for that view; while it moves, a slow average stands in until a stop.
// Run 7 (user 2026-10-23): with the cockpit's interior drawn by shaders the proxy does not know, no car draw placed
// the drops (4115 frames from RBR's object, 0.1-0.2 m off each frame: the remaining wiggle, drops through the glass).
// Run 10 (user 2026-10-24): the plane moved with every camera change and slid while driving (the slow average followed
// RBR's object, which lags the drawn car while moving). Run 11 (user: "focus on the DRIVING camera", the plane showed
// in the bonnet view and moved between bonnet / internal 2 / internal): drops only in the car's driving camera
// ([Cam_internal] of its ini, recognised by its field of view, camera inside the cabin), one link per car, kept once
// it has 30 samples and never measured again this session: samples from the interior as drawn in the same frame as
// the camera (headlights.cpp cockpit_root_vote, exact while moving); until there are any, RBR's object while the car
// stands (replaced once drawn samples come). No drops before 10 samples.
struct Link { float car[3]; float m[16]; int n; bool drawn, locked; };
std::vector<Link> g_links;
unsigned g_camFrames = 0;

// This frame's plane in the model frame (set by rbrrtx_windscreen_emit, drawn by draw_drops).
struct Plane { V bottom, S, R, N; float length, wB, wT; bool valid; } g_pl = {};
bool g_drawnThisFrame = false;                         // draw_drops ran for this frame's simulation

// Drops on the glass, M = the model root in world space (rows x, y, z, position).
void draw_drops(const float M[16]) {
  if (!g_pl.valid || g_drawnThisFrame || !make_all()) return;
  g_drawnThisFrame = true;
  const Plane& P = g_pl;
  const V Rw = norm(dir_world(M, P.R)), Sw = norm(dir_world(M, P.S)), Nw = norm(dir_world(M, P.N));
  remixapi_InstanceInfoBlendEXT blend;
  blend_alpha(blend);
  unsigned drops = 0, flakes = 0;
  for (const Drop& d : g_drops) {
    const float halfW = 0.5f * (P.wB + (P.wT - P.wB) * d.v);
    const V p = P.bottom + P.S * (d.v * P.length) + P.R * (d.u * halfW) + P.N * g_cfg.lift;
    const V w = to_world(M, p);
    if (d.flake) {
      const float r = d.r * (1.0f - 0.6f * d.melt);    // melting: shrinks
      draw(g_disc, w, Rw * r, Nw * (r * 0.3f), Sw * r, &blend);
      ++flakes;
    } else {
      draw(g_lens, w, Rw * d.r, Nw * d.r, Sw * d.r, nullptr);
      ++drops;
    }
  }
  if (g_cfg.showPlane) {
    const V c = to_world(M, P.bottom + P.S * (P.length * 0.5f) + P.N * 0.002f);
    const float hw = 0.25f * (P.wB + P.wT);
    draw(g_plane, c, Rw * hw, Nw, Sw * (P.length * 0.5f), &blend);
  }
  const ULONGLONG now = GetTickCount64();
  if (now - g_lastLog > 20000 && (drops || flakes)) {
    g_lastLog = now;
    plog(L"windscreen: %u drops, %u flakes on the glass at %.0f km/h (%s); %u cockpit frames drawn",
         drops, flakes, g_kmh, g_kmh > g_cfg.moveAboveKmh ? L"air pushes them outward" : L"big drops run down", g_camFrames);
  }
}

}  // namespace

void rbrrtx_windscreen_init(const remixapi_Interface* api) {
  g_api = api;
  rbrrtx_config_path(L"RBRRTX_PARTICLES", L"particles.ini", g_path, MAX_PATH * 2);
  wcscpy_s(g_flakeTex, g_path);                        // the flake sprite sits next to particles.ini
  if (wchar_t* slash = wcsrchr(g_flakeTex, L'\\')) slash[1] = 0;
  wcscat_s(g_flakeTex, L"snow_flake.dds");
  load();
}

void rbrrtx_windscreen_emit(bool sweeping) {
  if (!g_api || sweeping) { g_pl.valid = false; return; }   // nothing on the glass during a sweep
  check_cfg();
  if (!g_cfg.enabled) return;
  float intensity = 0.0f;
  const int kind = rbrrtx_precip_state(&intensity);
  float M[16], lf[3], rf[3], fwd[3];
  const bool car = rbrrtx_car_model_frame(M, lf, rf, fwd);
  g_pl.valid = false;                                  // run 10 (user): drops only in cockpit views, not from the object
  if (kind != g_lastKind) {
    plog(L"windscreen: %s (%.0f %%)%s", kind == 2 ? L"snow" : kind == 1 ? L"rain" : L"dry", intensity,
         car ? L"" : L"; car model frame not known yet (needs one outside view): no drops until then");
    g_lastKind = kind;
    if (!kind) g_drops.clear();
  }
  if (!car || (!kind && g_drops.empty() && !g_cfg.showPlane)) return;
  // Time step and car speed (km/h) from the model frame's position.
  LARGE_INTEGER t, f;
  QueryPerformanceCounter(&t);
  QueryPerformanceFrequency(&f);
  float dt = g_haveCar ? static_cast<float>(static_cast<double>(t.QuadPart - g_lastT.QuadPart) / f.QuadPart) : 0.0f;
  if (dt > 0.1f) dt = 0.1f;
  if (g_haveCar && dt > 0.0005f) {
    const float dx = M[12] - g_lastCar[0], dy = M[13] - g_lastCar[1], dz = M[14] - g_lastCar[2];
    const float ms = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
    if (ms < 100.0f) g_kmh += (ms * 3.6f - g_kmh) * 0.1f;
  }
  g_lastT = t;
  std::memcpy(g_lastCar, M + 12, sizeof(g_lastCar));
  g_haveCar = true;
  // The plane in the model frame (y = up). From the car's own windscreen glass (headlights.cpp load_windscreen: the
  // e_window_f mesh), its edges moved in by `inset` (rubber and frame); without it, or with use_glass = 0: from the
  // lamps' middle, back against the forward axis (the Golf values tuned in run 5).
  const V F = norm({ fwd[0], 0.0f, fwd[2] }), U = { 0, 1, 0 };
  V R = norm(cross(U, F));
  V bottom, top;
  float wB = g_cfg.widthBottom, wT = g_cfg.widthTop;
  float gb[3], gt[3], gn[3], gwb = 0.0f, gwt = 0.0f;
  // Run 11 (user 2026-10-24): the plane goes with the driving camera's view, so the car's own glass: the interior
  // model's (i_window_f) where it has one, else the exterior's (headlights.cpp load_windscreen). Run 7's "the lamp plane
  // is more accurate on the E30" came from the cockpit anchor (RBR's object) being off, not from the glass. The lamp
  // plane (Golf offsets) only without usable glass, or with use_glass = 0.
  const int source = rbrrtx_car_windscreen_source();
  const bool wantGlass = g_cfg.useGlass >= 1;
  const bool glass = wantGlass && rbrrtx_car_windscreen_glass(gb, gt, gn, &gwb, &gwt);
  if (glass) {
    bottom = { gb[0], gb[1], gb[2] };
    top = { gt[0], gt[1], gt[2] };
    const V S0 = norm(top - bottom);
    bottom = bottom + S0 * g_cfg.inset;
    top = top - S0 * g_cfg.inset;
    wB = (std::max)(0.1f, gwb - 2.0f * g_cfg.inset);
    wT = (std::max)(0.1f, gwt - 2.0f * g_cfg.inset);
  } else {
    const V mid = { (lf[0] + rf[0]) * 0.5f, (lf[1] + rf[1]) * 0.5f, (lf[2] + rf[2]) * 0.5f };
    bottom = mid - F * g_cfg.bottomBack + U * g_cfg.bottomUp;
    top = mid - F * g_cfg.topBack + U * g_cfg.topUp;
    // The lamp plane's widths are the Golf's (ini); a car with usable glass gets its own widths from it (run 8 user:
    // the Civic FN's plane was far too wide). Position stays from the lamps (more accurate on the E30).
    if (rbrrtx_car_windscreen_glass(gb, gt, gn, &gwb, &gwt)) {
      wB = (std::max)(0.1f, gwb - 2.0f * g_cfg.inset);
      wT = (std::max)(0.1f, gwt - 2.0f * g_cfg.inset);
    }
  }
  // Per car by hand (particles.ini [windscreen.<model file name>], e.g. [windscreen.BMW_M3_E30]): up / back / right
  // move the plane (metres, the car's axes), wider / longer add to its width and length. Kept for good: a car fixed
  // once stays fixed.
  const wchar_t* model = rbrrtx_car_model_name();
  const ULONGLONG cfgStamp = (static_cast<ULONGLONG>(g_time.dwHighDateTime) << 32) | g_time.dwLowDateTime;
  static std::wstring offModel;
  static ULONGLONG offStamp = 0;
  static float up = 0.0f, back = 0.0f, right = 0.0f, wider = 0.0f, longer = 0.0f;
  if (offModel != model || offStamp != cfgStamp) {     // read when the car or particles.ini changes, not per frame
    offModel = model;
    offStamp = cfgStamp;
    wchar_t sec[96];
    swprintf_s(sec, L"windscreen.%s", model);
    auto car_f = [&](const wchar_t* key) {
      wchar_t v[64] = {};
      GetPrivateProfileStringW(sec, key, L"", v, 64, g_path);
      return static_cast<float>(_wtof(v));
    };
    up = car_f(L"up"); back = car_f(L"back"); right = car_f(L"right"); wider = car_f(L"wider"); longer = car_f(L"longer");
  }
  const V shift = U * up - F * back + R * right;
  bottom = bottom + shift;
  top = top + shift;
  if (longer != 0.0f) { const V S0 = norm(top - bottom); bottom = bottom - S0 * (longer * 0.5f); top = top + S0 * (longer * 0.5f); }
  wB = (std::max)(0.1f, wB + wider);
  wT = (std::max)(0.1f, wT + wider);
  static int lastSource = -1;
  static float lastLf = 1e9f;
  static ULONGLONG lastCfg = 0;
  const int src = glass ? source : 0;
  if (src != lastSource || lf[0] != lastLf || cfgStamp != lastCfg) {
    lastSource = src;
    lastLf = lf[0];
    lastCfg = cfgStamp;
    plog(L"windscreen: %s: plane from %s%s: bottom (%.2f %.2f %.2f) %.2f wide, top (%.2f %.2f %.2f) %.2f wide (model frame)",
         model, src == 2 ? L"the interior glass (i_window_f)" : src == 1 ? L"the exterior glass (e_window_f)" : L"the lamps + [windscreen] offsets",
         up || back || right || wider || longer ? L" + [windscreen.<car>] offsets" : L"",
         bottom.x, bottom.y, bottom.z, wB, top.x, top.y, top.z, wT);
  }
  const V slope = top - bottom;
  const float length = std::sqrt(dot(slope, slope));
  if (length < 0.05f) return;
  const V S = slope * (1.0f / length);
  V N = norm(cross(R, S));
  if (dot(N, F) < 0.0f) N = N * -1.0f;                 // out of the glass = forward/up
  R = norm(cross(S, N));                               // across, square to the slope
  simulate(dt, kind, intensity, length, wB, wT);
  // Not drawn here: the drops go on the car as this frame draws it (rbrrtx_windscreen_car_drawn, called from the first
  // car draw headlights.cpp can place). RBR's car object differs from the drawn car by 0.1-0.2 m, changing every frame
  // (headlights.cpp track_drift); drops placed from it wiggled on the glass while driving (user 2026-10-23 run 6).
  // A frame that had no such draw shows them from RBR's object at its next emit (see rbrrtx_windscreen_emit's start).
  g_pl = { bottom, S, R, N, length, wB, wT, true };
  g_drawnThisFrame = false;
}

void rbrrtx_windscreen_car_drawn(const float M[16]) {
  (void)M;                                             // run 10 (user): outside views get no drops at all
}

bool driving_camera(float fovY) {
  if (!rbrrtx_camera_in_cabin()) return false;         // bonnet / bumper / outside cameras
  const float F = rbrrtx_car_internal_fov();
  if (F <= 0.0f) return true;                          // the car's ini has no [Cam_internal]: any cabin camera
  // [Cam_internal] FOV 0.85 rad came out as 36.5 deg vertical (16:9), the 1.309 rad cameras as 56.3: the ini value
  // is horizontal for 4:3, scaled 0.727 into the vertical half angle.
  const float want = 2.0f * std::atan(std::tan(F * 0.5f) * 0.727f) * 57.29578f;
  static float logged = -1.0f;
  const bool ok = std::fabs(fovY - want) < 4.0f;
  if (fovY != logged) {
    logged = fovY;
    plog(L"windscreen: cabin camera fov %.1f, driving camera %.1f (car ini %.3f rad): %s", fovY, want, F,
         ok ? L"drops on" : L"no drops in this camera");
  }
  return ok;
}

void rbrrtx_windscreen_camera(const float camToWorld[16], float fovY) {
  if (!g_api || !g_cfg.enabled || !driving_camera(fovY)) return;   // links are kept as they are
  float M[16], lf[3], rf[3], fwd[3], O[16];
  if (!rbrrtx_car_model_frame(M, lf, rf, fwd)) return;
  Link* L = nullptr;
  for (Link& l : g_links)
    if (std::fabs(l.car[0] - lf[0]) + std::fabs(l.car[1] - lf[1]) + std::fabs(l.car[2] - lf[2]) < 1e-3f) { L = &l; break; }
  if (!L) {
    Link l = {};
    std::memcpy(l.car, lf, sizeof(l.car));
    g_links.push_back(l);
    L = &g_links.back();
  }
  float sample[16];
  bool have = false, drawn = false;
  if (rbrrtx_cockpit_link_sample(sample)) {
    have = drawn = true;
  } else if (!L->drawn && g_kmh < 1.0f && rbrrtx_car_object_frame(O)) {   // standing: RBR's object = the drawn car
    float inv[16];
    inverse_rigid(camToWorld, inv);
    mul4(O, inv, sample);
    have = true;
  }
  if (have && drawn && !L->drawn) { L->n = 0; L->locked = false; L->drawn = true; }   // the drawn interior wins once
  if (have && !L->locked && (drawn || !L->drawn)) {
    const float k = 1.0f / static_cast<float>(L->n + 1);   // plain mean of the samples
    for (int i = 0; i < 16; ++i) L->m[i] += (sample[i] - L->m[i]) * (L->n ? k : 1.0f);
    if (++L->n >= 30) {
      L->locked = true;
      plog(L"windscreen: driving camera of %s kept: car root at (%.2f %.2f %.2f) in camera space, from %s",
           rbrrtx_car_model_name(), L->m[12], L->m[13], L->m[14], L->drawn ? L"the interior as drawn" : L"RBR's car object while standing");
    }
  }
  if (L->n < 10) return;
  float W[16];
  mul4(L->m, camToWorld, W);
  for (int r = 0; r < 3; ++r) {                        // keep the axes unit length after the averaging
    const float l = std::sqrt(W[r * 4] * W[r * 4] + W[r * 4 + 1] * W[r * 4 + 1] + W[r * 4 + 2] * W[r * 4 + 2]);
    if (l > 1e-4f) for (int c = 0; c < 3; ++c) W[r * 4 + c] /= l;
  }
  ++g_camFrames;
  draw_drops(W);
}

void rbrrtx_windscreen_destroy() {
  if (!g_api) return;
  if (g_lens) g_api->DestroyMesh(g_lens);
  if (g_disc) g_api->DestroyMesh(g_disc);
  if (g_plane) g_api->DestroyMesh(g_plane);
  if (g_water) g_api->DestroyMaterial(g_water);
  if (g_snowMat) g_api->DestroyMaterial(g_snowMat);
  if (g_planeMat) g_api->DestroyMaterial(g_planeMat);
  g_lens = g_disc = g_plane = nullptr;
  g_water = g_snowMat = g_planeMat = nullptr;
  g_failed = false;
}
