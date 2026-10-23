// headlights.cpp - car headlights as Remix API spot lights (user 2026-10-22).
//
// RBR's headlights are glow sprites (Cars\<car>\<car>_settings.ini [LightN]), not light sources
// (re/kb/systems/lights-atmosphere.md section 3). Here they become Remix sphere lights with light shaping (spots):
//   mode 2 = two spots at the model's own lamp nodes Point_light_lf / Point_light_rf
//   mode 3 = brights (user 2026-10-23): mode 2 plus a tight high beam next to each lamp, toward the car's middle
// The headlights key (L) steps off -> 2 -> 3 -> off (mode 1, one centre light, was dropped, user 2026-10-23).
// Automatic: when the sun shown drops below auto_below_sun (10 degrees) the lights come on in auto_mode, above it they go off again; once the key is used on a stage the
// lights stay as set until the next stage. Settings: rbr-rtx\config\headlights.ini (env RBRRTX_HEADLIGHTS).
//
// Where the lamps are. The car model (Cars\Cars.ini [CarNN] FileName, a .sgc) holds a node tree; each node record
// is the tag 0xC 0x13 0x2F 0x1, u32 size, name\0, u32 id, u32 parent (0xFFFFFFFF = root) and a 3x4 float [R|t]
// (to the parent). A lamp's model position = its transforms chained to the root (all 111 car models of the install
// decoded, 2026-10-22). Car pose C (model root -> world, row vectors) = RBR's own car rotation and position plus a
// per-car offset measured from the body's draw (physics_pose / measure_frame). Why: the 2026-10-23 USD capture (Twingo R1) shows the game draws most of the car
// (body, bumpers, bonnet, wings, glass, ~20 meshes) with ONE matrix, the body node's (e_body, else i_body), and only
// moving parts (doors, wheels, wipers, steering wheel) with their own. Bodies are often stored rotated (Twingo: z
// down); the stored 3x3 read as R (column vectors) gives an upright Twingo, transposed an upside-down one.
// History: the first build guessed from unrotated parts only (Twingo lamps aimed straight up, 2026-10-22); a
// per-part vote then placed the car from only 3-4 of ~34 groups (lamps jumped and lagged, 2026-10-23).
// Lights: Remix 1.5.2 keys API lights by hash; re-creating one overwrites it (its RTXDI index resets either way,
// rtx_lights.cpp copyFrom), and through the bridge every CreateLight takes a new handle, so a moved lamp is
// destroyed and created again (as the sun) and drawn every frame (DrawLightInstance is per frame).
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <d3d9.h>
#include "third_party/remix_c.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

#define hlog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(V3 a) { return std::sqrt(dot(a, a)); }
V3 norm(V3 a) { const float l = len(a); return l > 1e-6f ? a * (1.0f / l) : V3{ 0, 0, 0 }; }

// ---------------------------------------------------------------- settings (headlights.ini)

struct Settings {
  bool enabled = true;
  float autoBelow = 10.0f;                             // sun elevation (degrees) below which the lights come on; -90 = never
  int autoMode = 2;                                    // mode the automatic switch uses
  // user 2026-10-23: x27 of the first build, +50 %, then -20 %
  float radiance[3] = { 12960.0f, 12312.0f, 11016.0f }; // per lamp
  float radius = 0.05f;                                // sphere light radius (m)
  float cone = 36.0f;                                  // half angle (degrees; Remix takes cos of it)
  float softness = 0.15f, focus = 1.0f;
  // Brights (mode 3). Remix's cone only masks the light (shaping factor <= 1, light_shaping.slangh), so a tighter
  // cone does not make the beam brighter: the reach comes from the extra radiance on the axis, the tight cone keeps
  // it off the near road and the verges. Softness is in cosine units: 0.15 would darken a 12 degree cone (1 - cos =
  // 0.022) almost completely, so the brights have their own.
  float brightsRadiance[3] = { 53460.0f, 50787.0f, 45441.0f };   // per high beam (user tuning 2026-10-23, then +10 %)
  float brightsCone = 15.0f, brightsSoftness = 0.01f, brightsPitch = -0.5f;
  float brightsInset = 0.25f;                          // metres toward the car's middle from each lamp node
  float brightsForward = 0.15f;                        // extra metres ahead (user 2026-10-23: the Quattro's big front
                                                       // lamps hid the high beams inside the car model)
  float pitch = 0.0f;                                  // beam aimed this many degrees below the car's forward axis (+ = down)
  float forward = 0.08f;                               // lamp moved this far ahead of its node (in front of the lens glass)
  float volumetric = 1.0f;                             // volumetricRadianceScale (beams in fog / dust)
  bool fallbackOn = true;                              // keep Remix's fallback light while the lights are on at night
  V3 fallback = { 0.6f, 0.5f, -1.8f };                 // model offset of the left lamp when the model has none
} g_cfg;

float ini_float(const wchar_t* path, const wchar_t* key, float dflt) {
  wchar_t v[64] = {};
  GetPrivateProfileStringW(L"headlights", key, L"", v, 64, path);
  return v[0] ? static_cast<float>(_wtof(v)) : dflt;
}

void ini_rgb(const wchar_t* path, const wchar_t* key, float out[3]) {
  wchar_t v[96] = {};
  GetPrivateProfileStringW(L"headlights", key, L"", v, 96, path);
  float r, g, b;
  if (v[0] && swscanf_s(v, L"%f , %f , %f", &r, &g, &b) == 3) { out[0] = r; out[1] = g; out[2] = b; }
}

void load_settings() {
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_HEADLIGHTS", L"headlights.ini", path, MAX_PATH * 2);
  const bool found = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
  Settings s;
  if (found) {
    s.enabled = ini_float(path, L"enable", 1.0f) != 0.0f;
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"headlights", L"auto_below_sun", L"", v, 32, path);
    if (!_wcsicmp(v, L"none")) s.autoBelow = -90.0f;
    else if (v[0]) s.autoBelow = static_cast<float>(_wtof(v));
    s.autoMode = std::clamp(static_cast<int>(ini_float(path, L"auto_mode", 2.0f)), 2, 3);   // 1 (centre light) is gone
    ini_rgb(path, L"radiance", s.radiance);
    ini_rgb(path, L"brights_radiance", s.brightsRadiance);
    s.brightsCone = std::clamp(ini_float(path, L"brights_cone", s.brightsCone), 1.0f, 89.0f);
    s.brightsSoftness = std::clamp(ini_float(path, L"brights_softness", s.brightsSoftness), 0.0f, 1.0f);
    s.brightsPitch = std::clamp(ini_float(path, L"brights_pitch", s.brightsPitch), -30.0f, 30.0f);
    s.brightsInset = std::clamp(ini_float(path, L"brights_inset", s.brightsInset), -1.0f, 1.0f);
    s.brightsForward = std::clamp(ini_float(path, L"brights_forward", s.brightsForward), -1.0f, 1.0f);
    s.radius = std::clamp(ini_float(path, L"radius", s.radius), 0.005f, 1.0f);
    s.cone = std::clamp(ini_float(path, L"cone", s.cone), 1.0f, 89.0f);
    s.softness = std::clamp(ini_float(path, L"softness", s.softness), 0.0f, 1.0f);
    s.focus = std::clamp(ini_float(path, L"focus", s.focus), 0.0f, 100.0f);
    s.pitch = std::clamp(ini_float(path, L"pitch", s.pitch), -30.0f, 30.0f);
    s.forward = std::clamp(ini_float(path, L"forward", s.forward), -1.0f, 1.0f);
    s.volumetric = std::clamp(ini_float(path, L"volumetric", s.volumetric), 0.0f, 100.0f);
    s.fallbackOn = ini_float(path, L"keep_fallback_light", 1.0f) != 0.0f;
  }
  g_cfg = s;
  hlog(L"headlights (%s): %s, auto below sun %.1f deg in mode %d, radiance %.0f/%.0f/%.0f, "
       L"radius %.3f, cone %.1f, softness %.2f, focus %.2f, pitch %.1f, forward %.2f, volumetric %.2f, "
       L"keep fallback light %s; brights radiance %.0f cone %.1f softness %.3f pitch %.1f inset %.2f forward %.2f",
       found ? path : L"defaults, no headlights.ini", s.enabled ? L"on" : L"OFF", s.autoBelow,
       s.autoMode, s.radiance[0], s.radiance[1], s.radiance[2], s.radius, s.cone, s.softness, s.focus, s.pitch, s.forward, s.volumetric,
       s.fallbackOn ? L"yes" : L"no", s.brightsRadiance[0], s.brightsCone, s.brightsSoftness, s.brightsPitch, s.brightsInset, s.brightsForward);
}

// ---------------------------------------------------------------- car model (.sgc node tree)

struct Node { std::string name; unsigned parent; float R[3][3]; V3 t; };
// Model root -> node local, as a row-vector 4x4 without the last column: p_local = p_root * A (+ row 3).
struct Placed { float A[4][3]; };

struct CarModel {
  std::wstring file;                                   // what was read ("" = nothing)
  bool lamps = false;                                  // Point_light_lf / _rf found
  V3 lf = {}, rf = {};                                 // lamp positions in the root frame
  V3 forward = { 0, 0, -1 };                           // model forward axis (up = +y)
  Placed body = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 0 } } };   // model root -> body node (e_body, i_body)
  std::string bodyName = "(none: root frame)";
  std::vector<Placed> parts;                           // every drawn node (e_* / i_*), identical frames merged
} g_car;

// Model root -> node local (row vectors) from the node's chain to the root: p_local = R^-1 (p_root - t). A real
// inverse: some nodes are scaled (fiesta_r2 e_body: x 1.048).
bool placed_of(const float R[3][3], V3 t, Placed* pl) {
  const float det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                    R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
  if (std::fabs(det) < 1e-6f) return false;
  float inv[3][3];
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
      inv[r][c] = (R[r1][c1] * R[r2][c2] - R[r1][c2] * R[r2][c1]) / det;   // cofactor transpose
    }
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) pl->A[r][c] = inv[c][r];
  const float tt[3] = { t.x, t.y, t.z };
  for (int c = 0; c < 3; ++c) pl->A[3][c] = -(tt[0] * pl->A[0][c] + tt[1] * pl->A[1][c] + tt[2] * pl->A[2][c]);
  return true;
}

// Chain node -> root: p_root = R p + t (column vectors). transposed = read the stored 3x3 as R^T.
void total_of(const std::unordered_map<unsigned, Node>& nodes, unsigned id, bool transposed, float R[3][3], V3& t) {
  float acc[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
  float tv[3] = { 0, 0, 0 };
  for (int depth = 0; depth < 64; ++depth) {
    auto it = nodes.find(id);
    if (it == nodes.end()) break;
    const Node& n = it->second;
    float Rn[3][3];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) Rn[r][c] = transposed ? n.R[c][r] : n.R[r][c];
    float nt[3], nR[3][3];
    for (int r = 0; r < 3; ++r) {                      // this node applied after the chain so far: p' = Rn p + tn
      nt[r] = Rn[r][0] * tv[0] + Rn[r][1] * tv[1] + Rn[r][2] * tv[2];
      for (int c = 0; c < 3; ++c) nR[r][c] = Rn[r][0] * acc[0][c] + Rn[r][1] * acc[1][c] + Rn[r][2] * acc[2][c];
    }
    tv[0] = nt[0] + n.t.x; tv[1] = nt[1] + n.t.y; tv[2] = nt[2] + n.t.z;
    std::memcpy(acc, nR, sizeof(acc));
    id = n.parent;
  }
  std::memcpy(R, acc, sizeof(acc));
  t = { tv[0], tv[1], tv[2] };
}

void load_car_model(const std::wstring& file) {
  const auto t0 = std::chrono::steady_clock::now();
  g_car = CarModel();
  g_car.file = file;
  std::ifstream f(file, std::ios::binary);
  const std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 64) { hlog(L"headlights: car model %s not readable; fallback lamp positions", file.c_str()); return; }
  static const unsigned kTag[4] = { 0xC, 0x13, 0x2F, 0x1 };
  std::unordered_map<unsigned, Node> nodes;
  std::unordered_map<std::string, unsigned> byName;
  const char* p = d.data();
  for (size_t i = 0; i + 16 + 4 + 2 + 8 + 48 <= d.size(); ++i) {
    if (std::memcmp(p + i, kTag, 16) != 0) continue;
    unsigned size;
    std::memcpy(&size, p + i + 16, 4);
    const size_t name0 = i + 20;
    const char* z = static_cast<const char*>(std::memchr(p + name0, 0, (std::min)(static_cast<size_t>(200), d.size() - name0)));
    if (size < 0x40 || size >= 0x200 || !z || z == p + name0) continue;
    const size_t after = static_cast<size_t>(z - p) + 1;
    if (after + 8 + 48 > d.size()) break;
    unsigned id, parent;
    float m[12];
    std::memcpy(&id, p + after, 4);
    std::memcpy(&parent, p + after + 4, 4);
    std::memcpy(m, p + after + 8, 48);
    if (nodes.count(id)) continue;                     // first record per id = the node table
    Node n;
    n.name.assign(p + name0, z);
    n.parent = parent;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) n.R[r][c] = m[r * 4 + c];
    n.t = { m[3], m[7], m[11] };
    byName.emplace(n.name, id);
    nodes.emplace(id, std::move(n));
  }
  auto lamp = [&](const char* name, V3& out) {
    auto it = byName.find(name);
    if (it == byName.end()) return false;
    float R[3][3];
    total_of(nodes, it->second, false, R, out);        // translations do not depend on the reading
    return true;
  };
  V3 lb = {}, rb = {};
  g_car.lamps = lamp("Point_light_lf", g_car.lf) && lamp("Point_light_rf", g_car.rf);
  if (lamp("Point_light_lb", lb) && lamp("Point_light_rb", rb) && g_car.lamps) {
    const float front = (g_car.lf.z + g_car.rf.z) * 0.5f, back = (lb.z + rb.z) * 0.5f;
    if (std::fabs(front - back) > 0.5f) g_car.forward = { 0, 0, front < back ? -1.0f : 1.0f };
  }
  for (const char* name : { "e_body", "i_body" }) {   // the frame most of the car is drawn in
    auto it = byName.find(name);
    if (it == byName.end()) continue;
    float R[3][3];
    V3 t;
    total_of(nodes, it->second, false, R, t);
    if (!placed_of(R, t, &g_car.body)) continue;
    g_car.bodyName = name;
    break;
  }
  for (const auto& [id, n] : nodes) {                  // every drawn part, for the offset vote (measure_frame)
    if (n.name.size() < 3 || (n.name.compare(0, 2, "e_") && n.name.compare(0, 2, "i_"))) continue;
    float R[3][3];
    V3 t;
    total_of(nodes, id, false, R, t);
    Placed pl;
    if (!placed_of(R, t, &pl)) continue;
    bool dup = false;
    for (const Placed& o : g_car.parts) {
      float dev = 0.0f;
      for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) dev = (std::max)(dev, std::fabs(o.A[r][c] - pl.A[r][c]));
      if (dev < 1e-3f) { dup = true; break; }
    }
    if (!dup) g_car.parts.push_back(pl);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  hlog(L"headlights: car model %s: %zu nodes (%zu distinct drawn parts), body frame %S, lamps %s lf (%.3f %.3f %.3f) rf (%.3f %.3f %.3f), "
       L"forward model %+.0f z, %.1f ms", file.c_str(), nodes.size(), g_car.parts.size(), g_car.bodyName.c_str(), g_car.lamps ? L"found" : L"MISSING",
       g_car.lf.x, g_car.lf.y, g_car.lf.z, g_car.rf.x, g_car.rf.y, g_car.rf.z, g_car.forward.z, ms);
}

// Cars\Cars.ini [CarNN] FileName for the car slot (0..7), next to the game exe.
std::wstring car_model_path(int carId) {
  if (carId < 0 || carId > 7) return L"";
  wchar_t dir[MAX_PATH] = {};
  DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
  wchar_t* slash = (n > 0 && n < MAX_PATH) ? wcsrchr(dir, L'\\') : nullptr;
  if (!slash) return L"";
  slash[1] = 0;
  const std::wstring base(dir);
  wchar_t sec[8], file[MAX_PATH] = {};
  swprintf_s(sec, L"Car%02d", carId);
  GetPrivateProfileStringW(sec, L"FileName", L"", file, MAX_PATH, (base + L"Cars\\Cars.ini").c_str());
  return file[0] ? base + file : L"";
}

// ---------------------------------------------------------------- per frame: car draws -> car frame

struct Group { float m[16]; unsigned prims; };
std::vector<Group> g_groups;                           // this frame's car draw matrices (identical ones merged)

// One part frame: axes within 1e-3, translation within 5 mm. Parts sharing the body frame get their matrices computed
// one by one, and at world coordinates of a few thousand metres a float resolves only ~0.25 mm: a 0.1 mm test split
// the body into ~40 groups once the car physics ran (2026-10-23 test, E30), so the "biggest" group changed per frame.
bool same_matrix(const float* a, const float* b) {
  for (int i = 0; i < 12; ++i) if (std::fabs(a[i] - b[i]) > 1e-3f) return false;
  for (int i = 12; i < 15; ++i) if (std::fabs(a[i] - b[i]) > 0.005f) return false;
  return true;
}

V3 row(const float* m, int r) { return { m[r * 4], m[r * 4 + 1], m[r * 4 + 2] }; }
V3 to_world(const float* m, V3 d) { return row(m, 0) * d.x + row(m, 1) * d.y + row(m, 2) * d.z + row(m, 3); }

// C = A * W (A: model root -> node local, W: node local -> world), row vectors.
void compose(const Placed& a, const float* W, float C[16]) {
  for (int r = 0; r < 4; ++r) {
    const float x = a.A[r][0], y = a.A[r][1], z = a.A[r][2], w = r == 3 ? 1.0f : 0.0f;
    for (int c = 0; c < 4; ++c) C[r * 4 + c] = x * W[c] + y * W[4 + c] + z * W[8 + c] + w * W[12 + c];
  }
}

// The car pose C (model root -> world) comes from RBR's car movement object (0x8EF660, block from +0x100):
// +0x110 is a 3x3 rotation, 9 floats, rows = the car's axes; +0x140 the car position; physics space = render world
// with Y and Z swapped (decoded 2026-10-23 01:43 from "pose sample" lines: the body draw's rows are
// swap(M0), swap(M2), swap(M1) to 3 decimals in two poses). So C rows = those, and C's origin = position + d * R,
// d = the model root's offset from the physics origin in car axes (E30 standing: (-0.06, -0.24, -0.16) m).
// d is measured per car by a vote of the car draws (measure_frame), only while the car stands still (draws and physics come from slightly different moments: the moving car's
// draw trailed the physics by ~0.37 m), averaged over 20 frames, then locked (kept for the session per model file).
// Until then d = 0 (lamps ~0.3 m off). Car draws are not needed for the lamps at all: every camera works, and the
// lamps go out at the first BeginScene with the sun (one fixed point per frame; the UI trigger did not come every
// frame, so lamps were sometimes sent after Remix had taken the frame's lights: periodic flashing).
unsigned g_frameNo = 0, g_solved = 0, g_failed = 0;
struct Offset { int n = 0; V3 sum = {}; bool locked = false; V3 d = {}; int rejected = 0; };
Offset g_off;
std::unordered_map<std::wstring, V3> g_offsetByModel;  // locked offsets of this session
V3 g_lastPhys = {};
bool g_haveLastPhys = false;
int g_sampleLogged = 0;

V3 swap_yz(const float* v) { return { v[0], v[2], v[1] }; }

// RBR's pose in render space with the model root at d: rows = car axes (x, y = up, z), row 3 = position.
bool physics_pose(float C[16], V3* phys) {
  float r[20];
  if (!rbrrtx_player_car_raw(r)) return false;
  const V3 ax[3] = { swap_yz(r + 4), swap_yz(r + 10), swap_yz(r + 7) };
  for (const V3& a : ax) if (!(len(a) > 0.5f && len(a) < 2.0f)) return false;   // not a rotation: object not ready
  *phys = swap_yz(r + 16);
  for (int i = 0; i < 3; ++i) { C[i * 4] = ax[i].x; C[i * 4 + 1] = ax[i].y; C[i * 4 + 2] = ax[i].z; C[i * 4 + 3] = 0; }
  const V3 o = *phys + ax[0] * g_off.d.x + ax[1] * g_off.d.y + ax[2] * g_off.d.z;
  C[12] = o.x; C[13] = o.y; C[14] = o.z; C[15] = 1;
  return true;
}

// At frame end, with all of this frame's car draws: one offset sample while the car stands still.
void measure_frame() {
  float Q[16];
  V3 phys;
  if (!physics_pose(Q, &phys)) { g_haveLastPhys = false; return; }
  const bool still = g_haveLastPhys && len(phys - g_lastPhys) < 0.002f;
  g_lastPhys = phys;
  g_haveLastPhys = true;
  if (!still) return;
  if (g_off.locked) return;
  // Vote: every car draw group (>= 200 triangles), for every drawn part of the model it could be, gives the model
  // root's offset d from RBR's car centre (in car axes) if its axes then match RBR's. The right offset gets a vote
  // from every correctly placed part, a part taken for the wrong node only its own: so one big off-centre part (the
  // Evo IX bonnet, 0.725 m off at its hinge: one lamp inside the car, one outside) or a centred one (Quattro: a
  // 2800-triangle part, lamps 0.83 m ahead of the car) can no longer decide it (2026-10-23 tests). Plausible offsets
  // only: within 1.5 m, RBR's car centre within 0.25 m of the centreline between the lamps. Bins of 2 cm.
  struct Vote { long long key; int group; V3 d; };
  std::vector<Vote> votes;
  const float centre = (g_car.lf.x + g_car.rf.x) * 0.5f;
  float C[16];
  for (int j = 0; j < static_cast<int>(g_groups.size()); ++j) {
    const Group& g = g_groups[j];
    if (g.prims < 200) continue;
    for (const Placed& pl : g_car.parts) {
      compose(pl, g.m, C);
      bool axes = true;
      for (int r = 0; r < 3 && axes; ++r) axes = dot(norm(row(C, r)), norm(row(Q, r))) > 0.999f;
      if (!axes) continue;
      const V3 e = row(C, 3) - phys;                    // root offset in render space -> car axes
      const V3 dv = { dot(e, norm(row(Q, 0))), dot(e, norm(row(Q, 1))), dot(e, norm(row(Q, 2))) };
      if (len(dv) > 1.5f || std::fabs(-dv.x - centre) > 0.25f) continue;
      const long long key = (static_cast<long long>(std::lround(dv.x * 50.0f) + 1000) << 40) |
                            (static_cast<long long>(std::lround(dv.y * 50.0f) + 1000) << 20) | (std::lround(dv.z * 50.0f) + 1000);
      votes.push_back({ key, j, dv });
    }
  }
  std::sort(votes.begin(), votes.end(), [](const Vote& a, const Vote& b) { return a.key != b.key ? a.key < b.key : a.group < b.group; });
  int bestGroups = 0;
  V3 d = {};
  unsigned bestPrims = 0;
  for (size_t i = 0; i < votes.size();) {
    size_t e = i;
    int groups = 0;
    V3 sum = {};
    unsigned prims = 0;
    for (; e < votes.size() && votes[e].key == votes[i].key; ++e) {
      if (e == i || votes[e].group != votes[e - 1].group) { ++groups; prims += g_groups[votes[e].group].prims; }
      sum = sum + votes[e].d;
    }
    if (groups > bestGroups || (groups == bestGroups && prims > bestPrims)) {
      bestGroups = groups;
      bestPrims = prims;
      d = sum * (1.0f / static_cast<float>(e - i));
    }
    i = e;
  }
  if (bestGroups < 2) {                                // one part alone does not decide it
    if (++g_off.rejected <= 3 && !votes.empty())
      hlog(L"headlights: offset not measured this frame: no two car draws agree (%zu votes)", votes.size());
    return;
  }
  g_off.sum = g_off.sum + d;
  if (++g_off.n >= 20) {
    const bool again = g_off.locked;
    g_off.d = g_off.sum * (1.0f / g_off.n);
    g_off.locked = true;
    g_off.n = 0;
    g_off.sum = {};
    g_offsetByModel[g_car.file] = g_off.d;
    hlog(L"headlights: model root offset from RBR's car position %s: (%.3f %.3f %.3f) m in car axes (last frame: %d car draws agreed, %u triangles)",
         again ? L"measured again" : L"measured", g_off.d.x, g_off.d.y, g_off.d.z, bestGroups, bestPrims);
  }
}

bool place_frame(float C[16]) {
  V3 phys;
  return physics_pose(C, &phys);
}

// ---------------------------------------------------------------- state

enum { kOff = 0, kPair = 2, kBrights = 3 };
const remixapi_Interface* g_api = nullptr;
int g_mode = kOff;
bool g_manual = false;                                 // the key was used on this stage: no automatic switching
bool g_dark = false;                                   // sun below auto_below_sun (last frame)
int g_track = -1, g_carSlot = -1;
bool g_emitted = false;
struct Lamp { remixapi_LightHandle h = nullptr; unsigned long long hash; V3 pos{}, dir{}; float cone = 0; bool drawn = false; };
Lamp g_lamps[5] = { { nullptr, 0x52425248'4C43454EULL }, { nullptr, 0x52425248'4C4C4654ULL }, { nullptr, 0x52425248'4C524754ULL },
                    { nullptr, 0x52425248'42524C46ULL }, { nullptr, 0x52425248'42525246ULL } };   // RBRHLCEN / LFT / RGT, RBRHBRLF / BRRF
unsigned g_creates = 0;

void destroy_lamp(Lamp& l) {
  if (l.h && g_api && g_api->DestroyLight) g_api->DestroyLight(l.h);
  l.h = nullptr;
}

void destroy_all() { for (Lamp& l : g_lamps) destroy_lamp(l); }

const wchar_t* mode_name(int m) {
  return m == kPair ? L"ON (two lamps)" : m == kBrights ? L"BRIGHTS (four lamps)" : L"OFF";
}

void set_mode(int m, const wchar_t* why) {
  if (m == g_mode) return;
  g_mode = m;
  destroy_all();                                       // the other mode's lamps must not stay drawn
  g_creates = 0;                                       // log the first lamps of each switch
  hlog(L"headlights: %s (%s)", mode_name(m), why);
  wchar_t ev[96];
  swprintf_s(ev, L"headlights %s (%s)", mode_name(m), why);
  rbrrtx_report_event(ev);
}

void draw_lamp(Lamp& l, V3 pos, V3 dir, float cone, const float rad[3], float softness) {
  const bool moved = !l.h || len(pos - l.pos) > 0.005f || dot(dir, l.dir) < 0.99998f || cone != l.cone;
  if (moved) {
    destroy_lamp(l);
    remixapi_LightInfoSphereEXT sphere = {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = { pos.x, pos.y, pos.z };
    sphere.radius = g_cfg.radius;
    sphere.shaping_hasvalue = TRUE;
    sphere.shaping_value.direction = { dir.x, dir.y, dir.z };
    sphere.shaping_value.coneAngleDegrees = cone;
    sphere.shaping_value.coneSoftness = softness;
    sphere.shaping_value.focusExponent = g_cfg.focus;
    sphere.volumetricRadianceScale = g_cfg.volumetric;
    remixapi_LightInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = l.hash;
    info.radiance = { rad[0], rad[1], rad[2] };
    const remixapi_ErrorCode err = g_api->CreateLight(&info, &l.h);
    if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
      l.h = nullptr;
      if (++g_creates <= 20) hlog(L"headlights: CreateLight failed, error %d", static_cast<int>(err));
      return;
    }
    if (++g_creates <= 4)
      hlog(L"headlights: lamp %016llX at (%.2f %.2f %.2f) aimed (%.3f %.3f %.3f), cone %.1f, radiance %.0f",
           l.hash, pos.x, pos.y, pos.z, dir.x, dir.y, dir.z, cone, rad[0]);
    l.pos = pos; l.dir = dir; l.cone = cone;
  }
  g_api->DrawLightInstance(l.h);
}

bool in_stage(RbrStageInfo* si) {
  return rbrrtx_stage_info(si) && si->trackId > 0 &&
         (si->gameMode == 1 || si->gameMode == 2 || si->gameMode == 8 || si->gameMode == 10);
}

}  // namespace

void rbrrtx_headlights_init(const remixapi_Interface* api) {
  g_api = api;
  load_settings();
}

void rbrrtx_headlights_car_draw(const float* world, unsigned prims, bool blended) {
  // Collected while the lights are on, and on a stage until the model offset is measured, lights off too: the
  // game shows the car in a 3rd-person view whenever a stage loads, where several car parts agree (user 2026-10-23;
  // the cockpit view often shows just one, and the lamps then moved once, ~0.3 m, when the view changed).
  const bool measuring = g_track >= 0 && !g_off.locked;
  if (!g_cfg.enabled || (g_mode == kOff && !measuring) || blended || g_groups.size() >= 96) return;   // blended: ghost car, glass
  for (Group& g : g_groups) if (same_matrix(g.m, world)) { g.prims += prims; return; }
  Group g;
  std::memcpy(g.m, world, sizeof(g.m));
  g.prims = prims;
  g_groups.push_back(g);
}

void rbrrtx_headlights_emit() {
  if (g_emitted || !g_api || g_mode == kOff) return;
  g_emitted = true;
  float C[16];
  if (!place_frame(C)) {
    if (++g_failed <= 5 || g_failed % 600 == 0)
      hlog(L"headlights: car NOT placed this frame (RBR's car object not readable); placed %u, not placed %u frames", g_solved, g_failed);
    return;
  }
  if (++g_solved <= 2 || g_solved % 3600 == 0)
    hlog(L"headlights: car placed from RBR's car object (model offset %s); placed %u, not placed %u frames",
         g_off.locked ? L"measured" : L"not measured yet, 0", g_solved, g_failed);
  const V3 lf = g_car.lamps ? g_car.lf : g_cfg.fallback;
  const V3 rf = g_car.lamps ? g_car.rf : V3{ -g_cfg.fallback.x, g_cfg.fallback.y, g_cfg.fallback.z };
  const V3 ahead = g_car.forward * g_cfg.forward;
  const V3 wl = to_world(C, lf + ahead), wr = to_world(C, rf + ahead);
  const V3 fwd = norm(row(C, 2) * g_car.forward.z);
  const V3 up = norm(row(C, 1));
  auto aim = [&](float pitchDeg) { const float pr = pitchDeg * 0.0174533f; return norm(fwd * std::cos(pr) - up * std::sin(pr)); };
  const V3 dir = aim(g_cfg.pitch);
  draw_lamp(g_lamps[1], wl, dir, g_cfg.cone, g_cfg.radiance, g_cfg.softness);
  draw_lamp(g_lamps[2], wr, dir, g_cfg.cone, g_cfg.radiance, g_cfg.softness);
  if (g_mode == kBrights) {                            // high beams: each lamp node moved toward the car's middle
    const float in = g_cfg.brightsInset;
    const V3 bahead = ahead + g_car.forward * g_cfg.brightsForward;
    const V3 bl = to_world(C, lf + bahead + V3{ lf.x > rf.x ? -in : in, 0, 0 });
    const V3 br = to_world(C, rf + bahead + V3{ lf.x > rf.x ? in : -in, 0, 0 });
    const V3 bdir = aim(g_cfg.brightsPitch);
    draw_lamp(g_lamps[3], bl, bdir, g_cfg.brightsCone, g_cfg.brightsRadiance, g_cfg.brightsSoftness);
    draw_lamp(g_lamps[4], br, bdir, g_cfg.brightsCone, g_cfg.brightsRadiance, g_cfg.brightsSoftness);
  }
}

void rbrrtx_headlights_frame_end(float sunElevation, bool layerOn) {
  ++g_frameNo;
  const bool keyed = rbrrtx_key_pressed(RBRKEY_HEADLIGHTS);
  g_emitted = false;                                     // emitted at the next first BeginScene
  if (layerOn && (g_mode != kOff || (g_track >= 0 && !g_off.locked))) measure_frame();   // with every car draw of the frame
  g_groups.clear();
  if (!g_cfg.enabled || !g_api) return;
  RbrStageInfo si;
  const bool stage = in_stage(&si);
  if (!stage || !layerOn) {
    if (g_mode != kOff && !stage) set_mode(kOff, L"left the stage");
    if (!layerOn) destroy_all();
    if (!stage) { g_track = -1; g_manual = false; g_dark = false; }
    return;
  }
  if (si.trackId != g_track || si.carId != g_carSlot) {  // new stage or car: read the model, automatic again
    g_track = si.trackId;
    g_carSlot = si.carId;
    g_manual = false;
    g_dark = false;
    g_solved = g_failed = 0;
    g_off = Offset();
    g_haveLastPhys = false;
    load_settings();
    const std::wstring model = car_model_path(si.carId);
    if (model != g_car.file) load_car_model(model);
    auto known = g_offsetByModel.find(g_car.file);         // measured earlier this session
    if (known != g_offsetByModel.end()) { g_off.d = known->second; g_off.locked = true; }
  }
  if (keyed) {
    g_manual = true;
    set_mode(g_mode == kOff ? kPair : g_mode == kPair ? kBrights : kOff, rbrrtx_key_name(RBRKEY_HEADLIGHTS));
  }
  const bool dark = sunElevation < g_cfg.autoBelow;
  if (!g_manual && dark != g_dark) {
    wchar_t why[64];
    swprintf_s(why, L"sun %.1f deg, %s %.0f", sunElevation, dark ? L"below" : L"above", g_cfg.autoBelow);
    set_mode(dark ? g_cfg.autoMode : kOff, why);
  }
  g_dark = dark;
}

bool rbrrtx_headlights_keep_fallback() { return g_mode != kOff && g_cfg.enabled && g_cfg.fallbackOn; }

void rbrrtx_headlights_destroy() { destroy_all(); }

void rbrrtx_headlights_status(wchar_t* out, size_t n) {
  swprintf_s(out, n, L"headlights: %s%s, model %s (lamps %s), auto below sun %.1f deg, placed %u / not placed %u frames",
             mode_name(g_mode), g_manual ? L" (set by key)" : L" (automatic)", g_car.file.empty() ? L"none" : g_car.file.c_str(),
             g_car.lamps ? L"from model" : L"fallback", g_cfg.autoBelow, g_solved, g_failed);
}
