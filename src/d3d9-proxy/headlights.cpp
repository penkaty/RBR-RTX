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
//
// Rear (user 2026-10-23): two red spots at Point_light_brake_lb / _rb (on all 109 models; Point_light_lb is broken on
// some, Fabia R5 evo x = 0), aimed backwards. Tail lights while the headlights are on; brake lights, brighter, whenever
// RBR's brake input (car object +0x860, 0..1) is pressed, headlights on or off. Values after xoxor4d's gta4-rtx
// (game_lights.cpp veh_center_rearlight, comp_settings.hpp): rear intensity scalar 0.6 against 1.2 for the
// headlights (= half), cone 0.6 x the headlight's + 5 degrees, aimed back and 0.2 down (11.3 degrees).
// Backfire (exhaust pop): RBR draws the g_pop particle (Maps\PS_*.ini g_pop_particleclass, sprite g_pop.dds in
// Textures\particles.dat, Remix hash 0x93E4994710A5380B with ParticleMod by Kahoo v5); a draw with that texture
// flashes a short orange point light at the pipe tip (Point_exhaust1, pushed out along Point_exhaust2 -> 1).
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include <d3d9.h>
#include "third_party/remix_c.h"
#include <algorithm>
#include <chrono>
#include <cctype>
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
  // user 2026-10-23: x27 of the first build, +50 %, -20 %, then -15 %
  float radiance[3] = { 11016.0f, 10465.0f, 9364.0f };  // per lamp
  float radius = 0.05f;                                // sphere light radius (m)
  float cone = 36.0f;                                  // half angle (degrees; Remix takes cos of it)
  float softness = 0.15f, focus = 1.0f;
  // Brights (mode 3). Remix's cone only masks the light (shaping factor <= 1, light_shaping.slangh), so a tighter
  // cone does not make the beam brighter: the reach comes from the extra radiance on the axis, the tight cone keeps
  // it off the near road and the verges. Softness is in cosine units: 0.15 would darken a 12 degree cone (1 - cos =
  // 0.022) almost completely, so the brights have their own.
  float brightsRadiance[3] = { 45441.0f, 43169.0f, 38625.0f };   // per high beam (user tuning 2026-10-23, +10 %, then -15 %)
  float brightsCone = 15.0f, brightsSoftness = 0.01f, brightsPitch = -0.5f;
  float brightsInset = 0.25f;                          // metres toward the car's middle from each lamp node
  float brightsForward = 0.15f;                        // extra metres ahead (user 2026-10-23: the Quattro's big front
                                                       // lamps hid the high beams inside the car model)
  float pitch = 0.0f;                                  // beam aimed this many degrees below the car's forward axis (+ = down)
  float forward = 0.08f;                               // lamp moved this far ahead of its node (in front of the lens glass)
  float volumetric = 1.0f;                             // volumetricRadianceScale (beams in fog / dust)
  bool fallbackOn = true;                              // keep Remix's fallback light while the lights are on at night
  V3 fallback = { 0.6f, 0.5f, -1.8f };                 // model offset of the left lamp when the model has none
  // Rear lamps (cone and aim after gta4-rtx: cone 0.6 x + 5; aimed back, 0.2 down). RBR's own lamp sprites are pure
  // red (Cars\*\*_settings.ini Red 255 Green 0 Blue 0). Brightness (user 2026-10-23 tuning, replacing gta4-rtx's
  // half): braking = a headlight's red, tail = 66 % dimmer than that.
  bool rear = true, brakeLights = true;
  float brakeRadiance[3] = { 8262.0f, 165.0f, 83.0f };   // per lamp while braking (a headlight's red, then -25 %)
  float tailRadiance[3] = { 2809.0f, 56.0f, 28.0f };   // per lamp with the headlights on (0.34 x, then -25 %)
  float rearCone = 36.0f * 0.6f + 5.0f, rearSoftness = 0.15f;
  float rearPitch = 11.3f;                             // degrees below straight back (atan 0.2)
  float rearBack = 0.05f;                              // metres behind the lamp node (outside the lens)
  float brakeOn = 0.05f;                               // brake input (0..1) that lights them; off again below half of it
  // Backfire flash
  bool pop = true;
  float popRadiance[3] = { 53460.0f, 24057.0f, 6415.0f };   // orange (flame), point light; the high beams before their -15 % (user 2026-10-23)
  float popRadius = 0.08f, popHold = 0.06f, popOut = 0.12f;   // m, s after the last pop draw, m beyond the pipe tip
  unsigned long long popTexture = 0x93E4994710A5380BULL;
  bool gameLamps = true;                               // L also switches the game's own lit lamp meshes / glow sprites
  bool followDrawn = true;                             // lamps follow the drawn car (track_drift)
  // Head lamps this far ahead of their nodes in outside views (car body drawn: chase, TV, 3rd person) and in the
  // cockpit views (user 2026-10-23: 1.5 m outside made them stop vanishing; they still vanished a few times onboard).
  float outsideForward = 1.5f, cockpitForward = 0.0f;
} g_cfg;

// Per-car tuning (experiment, user 2026-10-23): a section named after the car model file ([BMW_M3_E30] for
// Cars\BMW_M3_E30\BMW_M3_E30.sgc) overrides any [headlights] key for that car.
std::wstring g_carSection;

void ini_str(const wchar_t* key, wchar_t* v, DWORD n, const wchar_t* path) {
  v[0] = 0;
  if (!g_carSection.empty()) GetPrivateProfileStringW(g_carSection.c_str(), key, L"", v, n, path);
  if (!v[0]) GetPrivateProfileStringW(L"headlights", key, L"", v, n, path);
}

float ini_float(const wchar_t* path, const wchar_t* key, float dflt) {
  wchar_t v[64] = {};
  ini_str(key, v, 64, path);
  return v[0] ? static_cast<float>(_wtof(v)) : dflt;
}

void ini_rgb(const wchar_t* path, const wchar_t* key, float out[3]) {
  wchar_t v[96] = {};
  ini_str(key, v, 96, path);
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
    ini_str(L"auto_below_sun", v, 32, path);
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
    s.rear = ini_float(path, L"tail_lights", 1.0f) != 0.0f;
    s.brakeLights = ini_float(path, L"brake_lights", 1.0f) != 0.0f;
    ini_rgb(path, L"brake_radiance", s.brakeRadiance);
    ini_rgb(path, L"tail_radiance", s.tailRadiance);
    s.rearCone = std::clamp(ini_float(path, L"rear_cone", s.rearCone), 1.0f, 89.0f);
    s.rearSoftness = std::clamp(ini_float(path, L"rear_softness", s.rearSoftness), 0.0f, 1.0f);
    s.rearPitch = std::clamp(ini_float(path, L"rear_pitch", s.rearPitch), -30.0f, 60.0f);
    s.rearBack = std::clamp(ini_float(path, L"rear_back", s.rearBack), -1.0f, 1.0f);
    s.brakeOn = std::clamp(ini_float(path, L"brake_on", s.brakeOn), 0.001f, 1.0f);
    s.pop = ini_float(path, L"backfire_light", 1.0f) != 0.0f;
    ini_rgb(path, L"backfire_radiance", s.popRadiance);
    s.popRadius = std::clamp(ini_float(path, L"backfire_radius", s.popRadius), 0.005f, 1.0f);
    s.popHold = std::clamp(ini_float(path, L"backfire_hold", s.popHold), 0.0f, 2.0f);
    s.popOut = std::clamp(ini_float(path, L"backfire_out", s.popOut), -1.0f, 1.0f);
    wchar_t tex[32] = {};
    ini_str(L"backfire_texture", tex, 32, path);
    if (tex[0]) s.popTexture = wcstoull(tex, nullptr, 16);
    s.gameLamps = ini_float(path, L"game_lamps", 1.0f) != 0.0f;
    s.followDrawn = ini_float(path, L"follow_drawn_car", 1.0f) != 0.0f;
    s.outsideForward = std::clamp(ini_float(path, L"outside_view_forward", s.outsideForward), -1.0f, 5.0f);
    s.cockpitForward = std::clamp(ini_float(path, L"cockpit_view_forward", s.cockpitForward), -1.0f, 5.0f);
  }
  g_cfg = s;
  if (!g_carSection.empty()) {
    wchar_t keys[16] = {};
    const bool own = GetPrivateProfileSectionW(g_carSection.c_str(), keys, 16, path) > 0;
    hlog(L"headlights: car section [%s] %s", g_carSection.c_str(), own ? L"found: its keys override [headlights]" : L"not in the ini");
  }
  hlog(L"headlights (%s): %s, auto below sun %.1f deg in mode %d, radiance %.0f/%.0f/%.0f, "
       L"radius %.3f, cone %.1f, softness %.2f, focus %.2f, pitch %.1f, forward %.2f, volumetric %.2f, "
       L"keep fallback light %s; brights radiance %.0f cone %.1f softness %.3f pitch %.1f inset %.2f forward %.2f",
       found ? path : L"defaults, no headlights.ini", s.enabled ? L"on" : L"OFF", s.autoBelow,
       s.autoMode, s.radiance[0], s.radiance[1], s.radiance[2], s.radius, s.cone, s.softness, s.focus, s.pitch, s.forward, s.volumetric,
       s.fallbackOn ? L"yes" : L"no", s.brightsRadiance[0], s.brightsCone, s.brightsSoftness, s.brightsPitch, s.brightsInset, s.brightsForward);
  hlog(L"headlights: tail lights %s (radiance %.0f/%.0f/%.0f), brake lights %s (radiance %.0f/%.0f/%.0f, on at brake %.2f), "
       L"rear cone %.1f softness %.2f pitch %.1f back %.2f; backfire light %s (texture %016llX, radiance %.0f/%.0f/%.0f, "
       L"radius %.2f, hold %.2f s, out %.2f)",
       s.rear ? L"on" : L"off", s.tailRadiance[0], s.tailRadiance[1], s.tailRadiance[2], s.brakeLights ? L"on" : L"off",
       s.brakeRadiance[0], s.brakeRadiance[1], s.brakeRadiance[2], s.brakeOn, s.rearCone, s.rearSoftness, s.rearPitch,
       s.rearBack, s.pop ? L"on" : L"off", s.popTexture, s.popRadiance[0], s.popRadiance[1], s.popRadiance[2], s.popRadius,
       s.popHold, s.popOut);
}

// ---------------------------------------------------------------- car model (.sgc node tree)

struct Node { std::string name; unsigned parent; float R[3][3]; V3 t; };
// Model root -> node local, as a row-vector 4x4 without the last column: p_local = p_root * A (+ row 3).
struct Placed { float A[4][3]; };

struct CarModel {
  std::wstring file;                                   // what was read ("" = nothing)
  bool lamps = false;                                  // Point_light_lf / _rf found
  V3 lf = {}, rf = {};                                 // lamp positions in the root frame
  bool rear = false;                                   // rear lamps found (Point_light_brake_lb/rb, else _lb/_rb)
  V3 lb = {}, rb = {};
  bool exhaust = false;                                // Point_exhaust1 (pipe tip) found
  V3 exhaustTip = {}, exhaustDir = {};                 // tip and its outward direction (from Point_exhaust2)
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
  // Rear lamps, one per side; tail and brake light share it. Per side the brake light node, else the tail node, if it
  // lies at the back and on that side of the headlights' centreline: on ds3_r5 and yaris_rally2 Point_light_brake_lb is
  // the high centre brake light (x 0, y 1.3), on Fabia R5 evo Point_light_lb sits at x 0. Some models are off-centre
  // (208 T16, Fabia S2000, Fiesta R5: both headlights at x < 0), so the side is taken from the centreline, not x = 0.
  // A side with no usable node mirrors the other one.
  if (g_car.lamps) {
    const float c = (g_car.lf.x + g_car.rf.x) * 0.5f, front = (g_car.lf.z + g_car.rf.z) * 0.5f;
    auto usable = [&](const char* name, float side, V3& out) {
      V3 p;
      return lamp(name, p) && (p.z - front) * -g_car.forward.z > 2.0f && (p.x - c) * side > 0.2f && (out = p, true);
    };
    const float sl = g_car.lf.x > c ? 1.0f : -1.0f;    // the side Point_light_lf (and so _lb) is on
    const bool hl = usable("Point_light_brake_lb", sl, g_car.lb) || usable("Point_light_lb", sl, g_car.lb);
    const bool hr = usable("Point_light_brake_rb", -sl, g_car.rb) || usable("Point_light_rb", -sl, g_car.rb);
    if (hl && !hr) g_car.rb = { 2.0f * c - g_car.lb.x, g_car.lb.y, g_car.lb.z };
    if (hr && !hl) g_car.lb = { 2.0f * c - g_car.rb.x, g_car.rb.y, g_car.rb.z };
    g_car.rear = hl || hr;
  }
  V3 ex2 = {};
  if (lamp("Point_exhaust1", g_car.exhaustTip)) {      // the tip; Point_exhaust2 lies further in along the pipe
    g_car.exhaust = true;
    const V3 out = lamp("Point_exhaust2", ex2) ? norm(g_car.exhaustTip - ex2) : V3{ 0, 0, 0 };
    g_car.exhaustDir = len(out) > 0.5f ? out : g_car.forward * -1.0f;
  } else if (lamp("Point_exhaust2", g_car.exhaustTip)) {
    g_car.exhaust = true;
    g_car.exhaustDir = g_car.forward * -1.0f;
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
  hlog(L"headlights: rear lamps %s lb (%.3f %.3f %.3f) rb (%.3f %.3f %.3f), exhaust %s tip (%.3f %.3f %.3f) out (%.2f %.2f %.2f)",
       g_car.rear ? L"found" : L"MISSING", g_car.lb.x, g_car.lb.y, g_car.lb.z, g_car.rb.x, g_car.rb.y, g_car.rb.z,
       g_car.exhaust ? L"found" : L"MISSING", g_car.exhaustTip.x, g_car.exhaustTip.y, g_car.exhaustTip.z,
       g_car.exhaustDir.x, g_car.exhaustDir.y, g_car.exhaustDir.z);
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

// Drawn car vs RBR's car object (user 2026-10-23 07:38 sweep, TV camera, replay): the headlights went dark while the
// car moved in TV / 3rd-person cameras and came back when the replay was paused; tail lights stayed. With the car
// hidden the scene was 25x brighter (sweep step 07), so the lamps sat inside the drawn car: in those cameras the car
// is drawn ahead of RBR's car object (draws and physics from different moments; known camera-dependent 3rd-person
// transform). The lamps now follow the drawn car: g_drift = drawn body root - predicted root, from the body draw of
// the last frame (smoothed; it fades out in views without the body, e.g. the cockpit).
V3 g_drift = {};
unsigned g_driftFrames = 0, g_driftLogged = 0;
unsigned g_bodyFrames = 0;                             // frames in a row with the car body drawn (outside views)
float g_driftMax = 0.0f;

// RBR's pose in render space with the model root at d: rows = car axes (x, y = up, z), row 3 = position.
// drift = add g_drift (the lamps); without it = RBR's object only (offset measurement).
bool physics_pose(float C[16], V3* phys, bool drift = false) {
  float r[20];
  if (!rbrrtx_player_car_raw(r)) return false;
  const V3 ax[3] = { swap_yz(r + 4), swap_yz(r + 10), swap_yz(r + 7) };
  for (const V3& a : ax) if (!(len(a) > 0.5f && len(a) < 2.0f)) return false;   // not a rotation: object not ready
  *phys = swap_yz(r + 16);
  for (int i = 0; i < 3; ++i) { C[i * 4] = ax[i].x; C[i * 4 + 1] = ax[i].y; C[i * 4 + 2] = ax[i].z; C[i * 4 + 3] = 0; }
  const V3 o = *phys + ax[0] * g_off.d.x + ax[1] * g_off.d.y + ax[2] * g_off.d.z +
               (drift && g_cfg.followDrawn ? g_drift : V3{ 0, 0, 0 });
  C[12] = o.x; C[13] = o.y; C[14] = o.z; C[15] = 1;
  return true;
}

// At frame end: where this frame's body draw put the model root, against RBR's object (offset measured only).
void track_drift() {
  float Q[16];
  V3 phys;
  if (!g_off.locked || !physics_pose(Q, &phys)) { g_drift = g_drift * 0.8f; return; }
  const V3 want = row(Q, 3);
  const Group* best = nullptr;
  V3 root = {};
  float C[16];
  for (const Group& g : g_groups) {
    if (g.prims < 500 || (best && g.prims <= best->prims)) continue;
    compose(g_car.body, g.m, C);
    bool axes = true;
    for (int r = 0; r < 3 && axes; ++r) axes = dot(norm(row(C, r)), norm(row(Q, r))) > 0.995f;
    if (!axes || len(row(C, 3) - want) > 2.5f) continue;   // not the body, or the wrong 3rd-person body far off
    best = &g;
    root = row(C, 3);
  }
  if (!best) { g_drift = g_drift * 0.9f; g_bodyFrames = 0; return; }
  ++g_bodyFrames;
  // Slow average: the gap comes from speed and timing and changes slowly, while single frames jitter by several cm
  // (test 2026-10-23 09:00: 0.11 .. 0.23 m with the sign changing; lamps flickered in bonnet and chase cameras).
  // A frame more than 0.5 m off the average is not trusted.
  const V3 sample = root - want;
  if (g_driftFrames > 10 && len(sample - g_drift) > 0.5f) return;
  g_drift = g_drift * 0.9f + sample * 0.1f;
  ++g_driftFrames;
  const float l = len(g_drift);
  if (l > g_driftMax) g_driftMax = l;
  if (l > 0.1f && g_driftLogged < 5 && g_driftFrames % 120 == 0) {
    ++g_driftLogged;
    hlog(L"headlights: drawn car %.2f m from RBR's car object (%.2f %.2f %.2f); lamps follow the drawn car", l, g_drift.x,
         g_drift.y, g_drift.z);
  }
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
  return physics_pose(C, &phys, true);
}

// ---------------------------------------------------------------- state

enum { kOff = 0, kPair = 2, kBrights = 3 };
const remixapi_Interface* g_api = nullptr;
int g_mode = kOff;
bool g_manual = false;                                 // the key was used on this stage: no automatic switching
bool g_dark = false;                                   // sun below auto_below_sun (last frame)
int g_track = -1, g_carSlot = -1;
bool g_emitted = false;
struct Lamp { remixapi_LightHandle h = nullptr; unsigned long long hash; V3 pos{}, dir{}; float cone = 0; float rad[3] = {}; };
// RBRHLCEN / LFT / RGT, RBRHBRLF / BRRF, RBRTLLFT / RGT (rear), RBRBFPOP (backfire)
Lamp g_lamps[8] = { { nullptr, 0x52425248'4C43454EULL }, { nullptr, 0x52425248'4C4C4654ULL }, { nullptr, 0x52425248'4C524754ULL },
                    { nullptr, 0x52425248'42524C46ULL }, { nullptr, 0x52425248'42525246ULL }, { nullptr, 0x52425254'4C4C4654ULL },
                    { nullptr, 0x52425254'4C524754ULL }, { nullptr, 0x52425242'46504F50ULL } };
unsigned g_creates = 0;
bool g_braking = false;                                // brake lights lit (hysteresis on the brake input)
unsigned g_brakeSwitches = 0;
std::chrono::steady_clock::time_point g_lastPop;       // last frame with a backfire draw
bool g_popSeen = false;
unsigned g_popDraws = 0, g_popFlashes = 0;
bool g_popActive = false;

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

// cone >= 180 = a plain point light (no shaping; dir unused). radius 0 = the headlights' radius.
void draw_lamp(Lamp& l, V3 pos, V3 dir, float cone, const float rad[3], float softness, float radius = 0.0f) {
  const bool moved = !l.h || len(pos - l.pos) > 0.005f || dot(dir, l.dir) < 0.99998f || cone != l.cone ||
                     rad[0] != l.rad[0] || rad[1] != l.rad[1] || rad[2] != l.rad[2];
  if (moved) {
    destroy_lamp(l);
    remixapi_LightInfoSphereEXT sphere = {};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = { pos.x, pos.y, pos.z };
    sphere.radius = radius > 0.0f ? radius : g_cfg.radius;
    sphere.shaping_hasvalue = cone < 180.0f;
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
    if (++g_creates <= 6)
      hlog(L"headlights: lamp %016llX at (%.2f %.2f %.2f) aimed (%.3f %.3f %.3f), cone %.1f, radiance %.0f",
           l.hash, pos.x, pos.y, pos.z, dir.x, dir.y, dir.z, cone, rad[0]);
    l.pos = pos; l.dir = dir; l.cone = cone;
    std::memcpy(l.rad, rad, sizeof(l.rad));
  }
  g_api->DrawLightInstance(l.h);
}

// The game's own lamp switch (user 2026-10-23: lit lamps when the lights are on). RBR never switches the headlights
// itself, but HedgeHog3D handles a CSigCarLight signal (0x10068E30, re/kb lights-atmosphere.md section 3): +4 index
// (0 lf, 1 rf), +8 on, +0xC glass on, +0x10 type (1 = headlight). It sets the car's switch boxes light_lf / light_rf
// (and lightglass_*) "on"/"off" - every model mesh with SwitchBox = light_lf and Switch = true shows or hides by its
// Predicate (Cars\<car>\<car>.ini: e_light_lf_lit Predicate = on) - and flips the glow sprite of that lamp
// (<car>_settings.ini [LightN], visible only if its Size > 0, i.e. the skin's "Headlights on" option). The handler's
// object (CHH_World) is taken from the signal connection the game made for it (0x10065E40: list at
// *(*0x101DDFC8) + 0x9CF8, connection +4 next, +0x10 object, +0x14 handler); offsets are from HedgeHog3D.dll's image
// base 0x10000000, the connection with handler == base + 0x68E30 confirms the DLL matches.
void* g_hhWorld = nullptr;
int g_gameLampsSent = -1;                              // last state sent (-1 = none)
unsigned g_gameLampsLog = 0;

void* find_hh_world() {
  HMODULE hh = GetModuleHandleW(L"HedgeHog3D.dll");
  if (!hh) hh = GetModuleHandleW(L"HedgeHog3D_NoSSE.dll");
  if (!hh) return nullptr;
  const uintptr_t base = reinterpret_cast<uintptr_t>(hh), handler = base + 0x68E30;
  __try {
    const uintptr_t disp = *reinterpret_cast<volatile uintptr_t*>(base + 0x1DDFC8);
    const uintptr_t mgr = disp ? *reinterpret_cast<volatile uintptr_t*>(disp) : 0;
    uintptr_t c = mgr ? *reinterpret_cast<volatile uintptr_t*>(mgr + 0x9CF8) : 0;
    for (int i = 0; c && i < 256; ++i) {
      if (*reinterpret_cast<volatile uintptr_t*>(c + 0x14) == handler) return *reinterpret_cast<void* volatile*>(c + 0x10);
      c = *reinterpret_cast<volatile uintptr_t*>(c + 4);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

bool call_car_light(void* world, int type, int index, bool on) {
  using Handler = void(__thiscall*)(void*, int);
  HMODULE hh = GetModuleHandleW(L"HedgeHog3D.dll");
  if (!hh) hh = GetModuleHandleW(L"HedgeHog3D_NoSSE.dll");
  if (!hh) return false;
  int sig[5] = { 0, index, on ? 1 : 0, on ? 1 : 0, type };   // CSigCarLight: (vtable), index, on, glass, type
  __try {
    reinterpret_cast<Handler>(reinterpret_cast<uintptr_t>(hh) + 0x68E30)(world, reinterpret_cast<int>(sig));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  return true;
}

// Each frame on a stage: send the headlight state when it changed, or when the game made a new world (stage, replay).
void sync_game_lamps() {
  if (!g_cfg.gameLamps) return;
  if (g_frameNo % 30 != 0 && g_gameLampsSent == (g_mode != kOff ? 1 : 0)) return;
  void* world = find_hh_world();
  const int want = g_mode != kOff ? 1 : 0;
  if (!world) {
    if (g_gameLampsLog < 2 && g_gameLampsSent != -2) { ++g_gameLampsLog; hlog(L"headlights: game lamp switch not found (HedgeHog3D CSigCarLight connection)"); }
    g_gameLampsSent = -2;
    return;
  }
  if (world == g_hhWorld && want == g_gameLampsSent) return;
  // Headlights (type 1: light_lf, light_rf) and the tail lamps with them (type 3: index + 2 into the handler's suffix
  // table _lf _rf _lb _rb _m, prefix "light": light_lb, light_rb, light_m; HedgeHog3D.dll tables 0x101DA26C /
  // 0x101DA280, read 2026-10-23; type 4 = brake, 5 = reverse are the game's own).
  bool ok = call_car_light(world, 1, 0, want == 1) && call_car_light(world, 1, 1, want == 1);
  for (int i = 0; ok && i < 3; ++i) ok = call_car_light(world, 3, i, want == 1);
  if (g_gameLampsLog < 12) {
    ++g_gameLampsLog;
    hlog(L"headlights: game's own head and tail lamps (lit meshes, glow sprites) %s%s (world %p)", want ? L"ON" : L"off",
         ok ? L"" : L" FAILED", world);
  }
  g_hhWorld = world;
  g_gameLampsSent = want;
  if (!ok) g_cfg.gameLamps = false;                    // the handler faulted: not again until the settings are read
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
  // On a stage always: the offset (until measured) and the drawn car's drift (track_drift) for every lamp, brake and
  // backfire lights included.
  if (!g_cfg.enabled || g_track < 0 || blended || g_groups.size() >= 96) return;   // blended: ghost car, glass
  for (Group& g : g_groups) if (same_matrix(g.m, world)) { g.prims += prims; return; }
  Group g;
  std::memcpy(g.m, world, sizeof(g.m));
  g.prims = prims;
  g_groups.push_back(g);
}

void rbrrtx_headlights_emit() {
  if (g_emitted || !g_api || !g_cfg.enabled || g_track < 0) return;
  // Brake lights: on above brake_on, off below half of it (no flicker from a pedal resting at the threshold).
  float brake = 0.0f;
  const bool brakeRead = rbrrtx_player_car_brake(&brake);
  const bool braking = g_cfg.brakeLights && brakeRead && brake > (g_braking ? g_cfg.brakeOn * 0.5f : g_cfg.brakeOn);
  if (braking != g_braking && ++g_brakeSwitches <= 3)
    hlog(L"headlights: brake lights %s (brake input %.2f)", braking ? L"ON" : L"off", brake);
  g_braking = braking;
  const bool pop = g_cfg.pop && g_popSeen &&
                   std::chrono::duration<float>(std::chrono::steady_clock::now() - g_lastPop).count() <= g_cfg.popHold;
  if (pop && !g_popActive && ++g_popFlashes <= 3) hlog(L"headlights: backfire flash (%u pop draws so far)", g_popDraws);
  g_popActive = pop;
  const bool tail = g_cfg.rear && g_mode != kOff;
  if (g_mode == kOff && !braking && !pop) return;
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
  // User 2026-10-23: while the car body is drawn (3rd-person, chase, TV views) the car itself blocked the head lamps;
  // moving them outside_view_forward (1.5 m) ahead fixed it. Cockpit views (no body drawn) have cockpit_view_forward.
  const bool outside = g_bodyFrames >= 3;
  const float extra = outside ? g_cfg.outsideForward : g_cfg.cockpitForward;
  static int viewWas = -1;
  if (static_cast<int>(outside) != viewWas) {
    viewWas = outside;
    hlog(L"headlights: %s view, head lamps %.2f m ahead", outside ? L"outside (car body drawn)" : L"cockpit",
         g_cfg.forward + extra);
  }
  const V3 ahead = g_car.forward * (g_cfg.forward + extra);
  const V3 wl = to_world(C, lf + ahead), wr = to_world(C, rf + ahead);
  const V3 fwd = norm(row(C, 2) * g_car.forward.z);
  const V3 up = norm(row(C, 1));
  auto aim = [&](float pitchDeg) { const float pr = pitchDeg * 0.0174533f; return norm(fwd * std::cos(pr) - up * std::sin(pr)); };
  if ((tail || braking) && g_car.rear) {               // rear: aimed straight back and rearPitch down
    const V3 back = g_car.forward * -g_cfg.rearBack;
    const float pr = g_cfg.rearPitch * 0.0174533f;
    const V3 rdir = norm(fwd * -std::cos(pr) - up * std::sin(pr));
    const float* rad = braking ? g_cfg.brakeRadiance : g_cfg.tailRadiance;
    draw_lamp(g_lamps[5], to_world(C, g_car.lb + back), rdir, g_cfg.rearCone, rad, g_cfg.rearSoftness);
    draw_lamp(g_lamps[6], to_world(C, g_car.rb + back), rdir, g_cfg.rearCone, rad, g_cfg.rearSoftness);
  }
  if (pop && g_car.exhaust)                            // backfire: a point light just beyond the pipe tip
    draw_lamp(g_lamps[7], to_world(C, g_car.exhaustTip + g_car.exhaustDir * g_cfg.popOut), up, 180.0f, g_cfg.popRadiance,
              0.0f, g_cfg.popRadius);
  if (g_mode == kOff) return;
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

// The car body drawn in recent frames (outside views: chase, TV, 3rd person) = 1, not drawn (cockpit views) = 0,
// -1 = not known yet (car model offset not measured). The dust uses it too (particles.cpp, user 2026-10-23).
int rbrrtx_car_body_visible() {
  if (g_track < 0 || !g_off.locked) return -1;
  return g_bodyFrames >= 3 ? 1 : 0;
}

// The car's model root in render space as the lamps use it (rows: model x, y = up, z, row 3 = position; RBR's pose
// + the measured model offset + drift), the head lamp nodes and the model's forward axis in that frame. For the
// windscreen drops (windscreen.cpp). False while the model or its offset is not known.
bool rbrrtx_car_model_frame(float M[16], float lf[3], float rf[3], float fwd[3]) {
  if (g_track < 0 || !g_car.lamps || !g_off.locked) return false;
  V3 phys;
  if (!physics_pose(M, &phys, true)) return false;
  lf[0] = g_car.lf.x; lf[1] = g_car.lf.y; lf[2] = g_car.lf.z;
  rf[0] = g_car.rf.x; rf[1] = g_car.rf.y; rf[2] = g_car.rf.z;
  fwd[0] = g_car.forward.x; fwd[1] = g_car.forward.y; fwd[2] = g_car.forward.z;
  return true;
}

void rbrrtx_headlights_frame_end(float sunElevation, bool layerOn) {
  ++g_frameNo;
  const bool keyed = rbrrtx_key_pressed(RBRKEY_HEADLIGHTS);
  g_emitted = false;                                     // emitted at the next first BeginScene
  if (layerOn && g_track >= 0) {                         // with every car draw of the frame
    measure_frame();
    track_drift();
  }
  g_groups.clear();
  if (!g_cfg.enabled || !g_api) return;
  RbrStageInfo si;
  const bool stage = in_stage(&si);
  if (!stage || !layerOn) {
    if (g_mode != kOff && !stage) set_mode(kOff, L"left the stage");
    if (!layerOn) destroy_all();
    if (!stage) { g_track = -1; g_manual = false; g_dark = false; g_gameLampsSent = -1; g_hhWorld = nullptr; }
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
    g_braking = g_popSeen = g_popActive = false;
    g_drift = {};
    g_driftMax = 0.0f;
    g_driftFrames = 0;
    g_gameLampsSent = -1;                              // the stage's world starts with the model ini's states
    g_hhWorld = nullptr;
    const std::wstring model = car_model_path(si.carId);
    const size_t slash = model.find_last_of(L"\\/"), dot = model.find_last_of(L'.');
    g_carSection = model.empty() ? L"" : model.substr(slash + 1, dot != std::wstring::npos && dot > slash ? dot - slash - 1 : std::wstring::npos);
    load_settings();                                   // after the car is known: its own section may override
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
  sync_game_lamps();
}

unsigned long long rbrrtx_headlights_pop_texture() {
  return g_cfg.enabled && g_cfg.pop && g_track >= 0 && g_api ? g_cfg.popTexture : 0;
}

void rbrrtx_headlights_pop_drawn() {
  ++g_popDraws;
  g_popSeen = true;
  g_lastPop = std::chrono::steady_clock::now();
}

bool rbrrtx_headlights_keep_fallback() { return g_mode != kOff && g_cfg.enabled && g_cfg.fallbackOn; }

void rbrrtx_headlights_destroy() { destroy_all(); }

void rbrrtx_headlights_status(wchar_t* out, size_t n) {
  swprintf_s(out, n, L"headlights: %s%s, model %s (lamps %s, rear %s, exhaust %s), auto below sun %.1f deg, placed %u / not placed %u frames; "
             L"brake lights %s; backfire draws %u, flashes %u; drawn car %.2f m from RBR's object (max %.2f this stage)",
             mode_name(g_mode), g_manual ? L" (set by key)" : L" (automatic)", g_car.file.empty() ? L"none" : g_car.file.c_str(),
             g_car.lamps ? L"from model" : L"fallback", g_car.rear ? L"yes" : L"no", g_car.exhaust ? L"yes" : L"no",
             g_cfg.autoBelow, g_solved, g_failed, g_braking ? L"ON" : L"off", g_popDraws, g_popFlashes,
             len(g_drift), g_driftMax);
}

// Lit lamp textures made emissive (user 2026-10-23: the lamps should look lit inside). Some cars draw their lamps
// with their own lit texture when the lamp is on (BMW M3 E30: lights_lit.dds, Remix 0xAE5889DDDA655C65, shown by
// e_light_*_lit meshes; most other cars use their shared car atlas, which cannot glow per texture). Before Remix
// starts, every Cars\*\*_shaders.ini texture entry named *_lit*.dds is hashed as Remix does (XXH3_64 of mip 0) and a
// local mod <mods>\rbr_rtx_lit\mod.usda gives that texture hash an emissive material whose emission texture is the
// player's own file (absolute path: nothing is copied or shipped). headlights.ini lit_glow = emissive intensity
// (0 = no mod). The file is rewritten only when its content changes.
// game = game folder with a trailing backslash. Returns the number of textures made emissive.
size_t write_lit_mod_for(const std::wstring& game, const wchar_t* modsDir, float glow) {
  const std::wstring dir = std::wstring(modsDir) + L"\\rbr_rtx_lit", file = dir + L"\\mod.usda";
  std::string body;
  std::unordered_map<unsigned long long, bool> seen;
  WIN32_FIND_DATAW fd = {};
  HANDLE h = glow > 0.0f ? FindFirstFileW((game + L"Cars\\*").c_str(), &fd) : INVALID_HANDLE_VALUE;
  for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileW(h, &fd)) {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
    const std::wstring car = game + L"Cars\\" + fd.cFileName + L"\\";
    WIN32_FIND_DATAW sd;
    HANDLE hs = FindFirstFileW((car + L"*_shaders.ini").c_str(), &sd);
    if (hs == INVALID_HANDLE_VALUE) continue;
    std::ifstream ini(car + sd.cFileName);
    FindClose(hs);
    for (std::string line; std::getline(ini, line);) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string key = line.substr(0, eq), val = line.substr(eq + 1);
      auto trim = [](std::string& t) {
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '"')) t.pop_back();
        size_t a = 0;
        while (a < t.size() && (t[a] == ' ' || t[a] == '\t' || t[a] == '"')) ++a;
        t.erase(0, a);
      };
      trim(key);
      trim(val);
      std::string low = key;
      for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      if (low.size() < 5 || low.compare(low.size() - 4, 4, ".dds") || low.find("_lit") == std::string::npos) continue;
      const std::wstring tex = game + std::wstring(val.begin(), val.end());
      std::ifstream f(tex, std::ios::binary);
      const std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4)) continue;
      unsigned hgt, wid, flags, four, bits;
      std::memcpy(&hgt, d.data() + 12, 4);
      std::memcpy(&wid, d.data() + 16, 4);
      std::memcpy(&flags, d.data() + 80, 4);
      std::memcpy(&four, d.data() + 84, 4);
      std::memcpy(&bits, d.data() + 88, 4);
      size_t size = 0;
      if (flags & 4) {                                 // compressed: DXT1 8 bytes per 4x4 block, DXT2..5 16
        const size_t blk = four == MAKEFOURCC('D', 'X', 'T', '1') ? 8 : 16;
        size = (std::max)(1u, (wid + 3) / 4) * static_cast<size_t>((std::max)(1u, (hgt + 3) / 4)) * blk;
      } else {
        size = static_cast<size_t>(wid) * hgt * (bits / 8);
      }
      if (!size || 128 + size > d.size()) continue;
      const unsigned long long hx = rbrrtx_xxh3(d.data() + 128, size);
      if (seen.count(hx)) continue;
      seen[hx] = true;
      char u8[MAX_PATH * 4] = {};                      // USD asset paths are UTF-8
      WideCharToMultiByte(CP_UTF8, 0, tex.c_str(), -1, u8, sizeof(u8), nullptr, nullptr);
      std::string abs(u8);
      for (char& c : abs) if (c == '\\') c = '/';
      char m[1024];
      sprintf_s(m,
                "        # %s\n"
                "        def Material \"mat_%016llX\"\n"
                "        {\n"
                "            def Shader \"Shader\"\n"
                "            {\n"
                "                uniform token info:implementationSource = \"sourceAsset\"\n"
                "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
                "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n"
                "                bool inputs:enable_emission = 1\n"
                "                asset inputs:emissive_mask_texture = @%s@ (\n"
                "                    colorSpace = \"auto\"\n"
                "                )\n"
                "                float inputs:emissive_intensity = %.3f\n"
                "                token outputs:out\n"
                "            }\n"
                "        }\n",
                abs.c_str(), hx, abs.c_str(), glow);
      body += m;
    }
  }
  if (h != INVALID_HANDLE_VALUE) FindClose(h);
  std::string old;
  {
    std::ifstream in(file, std::ios::binary);
    old.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  if (body.empty()) {
    if (!old.empty()) { DeleteFileW(file.c_str()); RemoveDirectoryW(dir.c_str()); }
    hlog(L"headlights: no lit lamp textures made emissive (lit_glow %.2f)", glow);
    return 0;
  }
  const std::string usda = "#usda 1.0\n(\n    upAxis = \"Y\"\n)\n\n# Written by RBR-RTX (headlights.cpp) at game start from the installed cars; do not edit.\n"
                           "def Xform \"RootNode\"\n{\n    def Scope \"Looks\"\n    {\n" + body + "    }\n}\n";
  if (usda != old) {
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << usda;
  }
  hlog(L"headlights: %zu lit lamp texture(s) made emissive (intensity %.2f) in %s%s", seen.size(), glow, file.c_str(),
       usda != old ? L" (written)" : L"");
  return seen.size();
}

void rbrrtx_headlights_write_lit_mod(const wchar_t* modsDir) {
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_HEADLIGHTS", L"headlights.ini", path, MAX_PATH * 2);
  const float glow = ini_float(path, L"lit_glow", 3.0f);
  wchar_t exe[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t* slash = (n > 0 && n < MAX_PATH) ? wcsrchr(exe, L'\\') : nullptr;
  if (!slash) return;
  slash[1] = 0;
  write_lit_mod_for(exe, modsDir, glow);
}
