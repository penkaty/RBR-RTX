// headlights.cpp - car headlights as Remix API spot lights (user 2026-10-22).
//
// RBR's headlights are glow sprites (Cars\<car>\<car>_settings.ini [LightN]), not light sources
// (re/kb/systems/lights-atmosphere.md section 3). Here they become Remix sphere lights with light shaping (spots):
//   mode 2 = two spots at the model's own lamp nodes Point_light_lf / Point_light_rf
//   mode 3 = brights (user 2026-10-23): mode 2 plus a tight high beam next to each lamp, toward the car's middle
// The headlights key (L) steps off -> 2 -> 3 -> off (mode 1, one centre light, was dropped, user 2026-10-23).
// Automatic (2026-10-30): daytime running lights (mode 2 all day) and the brights below brights_below_sun (4 degrees);
// day_running = 0 is the old switch (lamps on below auto_below_sun in auto_mode). Once the key is used on a stage the
// lights stay as set until the next stage. Settings: rbr-rtx\config\headlights.ini (env RBRRTX_HEADLIGHTS).
//
// Where the lamps are. The car model (Cars\Cars.ini [CarNN] FileName, a .sgc) holds a node tree; each node record
// is the tag 0xC 0x13 0x2F 0x1, u32 size, name\0, u32 id, u32 parent (0xFFFFFFFF = root) and a 3x4 float [R|t]
// (to the parent). A lamp's model position = its transforms chained to the root (all 111 car models of the install
// decoded, 2026-10-22). Car pose C (model root -> world, row vectors) = the car body as drawn in this frame
// (match_body), else RBR's own car rotation and position as they were g_lag earlier plus a per-car offset measured
// from the body's draw (pred_pose / measure_frame; "Pose of the lamps" below). Why: the 2026-10-23 USD capture (Twingo R1) shows the game draws most of the car
// (body, bumpers, bonnet, wings, glass, ~20 meshes) with ONE matrix, the body node's (e_body, else i_body), and only
// moving parts (doors, wheels, wipers, steering wheel) with their own. Bodies are often stored rotated (Twingo: z
// down); the stored 3x3 read as R (column vectors) gives an upright Twingo, transposed an upside-down one.
// History: the first build guessed from unrotated parts only (Twingo lamps aimed straight up, 2026-10-22); a
// per-part vote then placed the car from only 3-4 of ~34 groups (lamps jumped and lagged, 2026-10-23).
// Lights: Remix 1.5.2 keys API lights by hash; re-creating one overwrites it (its RTXDI index resets either way,
// rtx_lights.cpp copyFrom), and through the bridge every CreateLight takes a new handle, so a moved lamp is
// destroyed and created again (as the sun) and drawn every frame (DrawLightInstance is per frame).
// Remix Plus (2026-10-29): re-creating a live hash keeps the light's buffer slot and identity, so there a moved lamp is
// only created again (one light for its life, no destroy). Cost: the bridge server keeps one ~40-byte handle entry per
// update until DestroyLight (bridge util_remixapi.h Handle), about 50 MB an hour with four lamps moving every frame,
// until the bridge forwards UpdateLightDefinition (a stub in Remix Plus 2.0.0's bridge client).
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
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <array>
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
  // Automatic (user 2026-10-30): daytime running lights, the low beams always on, and the brights by themselves with
  // the sun below brightsBelow degrees. day_running = 0: the lamps come on only below autoBelow, in autoMode.
  bool dayRunning = true;
  bool keepLampsAlive = true;                          // lamps off on a stage: the head lamps stay in Remix at zero strength (light count unchanged)
  float brightsBelow = 4.0f;
  float autoBelow = 10.0f;                             // sun elevation (degrees) below which the lights come on; -90 = never
  int autoMode = 2;                                    // mode the automatic switch uses
  bool autoBadWeather = true;                          // day_running = 0: also on in rain, snow or fog, whatever the sun
  // user 2026-10-23: x27 of the first build, +50 %, -20 %, then -15 %
  float radiance[3] = { 11016.0f, 10465.0f, 9364.0f };  // per lamp
  float radius = 0.05f;                                // sphere light radius (m)
  float cone = 36.0f;                                  // half angle (degrees; Remix takes cos of it)
  float softness = 0.15f, focus = 1.0f;
  // Daylight (user 2026-10-30: still too strong in daylight): every lamp but the backfire flash x a factor that goes
  // from 1 with the sun at dayFade[0] degrees to dayIntensity with the sun at dayFade[1] degrees and above.
  float dayIntensity = 1.0f, dayFade[2] = { 0.0f, 20.0f };
  // Brights (mode 3). Remix's cone only masks the light (shaping factor <= 1, light_shaping.slangh), so a tighter
  // cone does not make the beam brighter: the reach comes from the extra radiance on the axis, the tight cone keeps
  // it off the near road and the verges. Softness is in cosine units: 0.15 would darken a 12 degree cone (1 - cos =
  // 0.022) almost completely, so the brights have their own.
  float brightsRadiance[3] = { 45441.0f, 43169.0f, 38625.0f };   // per high beam (user tuning 2026-10-23, +10 %, then -15 %)
  float brightsCone = 15.0f, brightsSoftness = 0.01f, brightsPitch = -0.5f;
  float brightsInset = 0.25f;                          // metres toward the car's middle from each lamp node
  // Four round lamps (BMW M3 E30): the inner pair at brights_inset is lit as low beams too and becomes the brights.
  bool quadLamps = false;
  float brightsForward = 0.15f;                        // extra metres ahead (user 2026-10-23: the Quattro's big front
                                                       // lamps hid the high beams inside the car model)
  float pitch = 0.0f;                                  // beam aimed this many degrees below the car's forward axis (+ = down)
  float forward = 0.08f;                               // lamp moved this far ahead of its node (in front of the lens glass)
  float volumetric = 1.0f;                             // volumetricRadianceScale (beams in fog / dust)
  bool fallbackOn = true;                              // keep Remix's fallback light while the lights are on at night
  float historyFrames = 0.0f;                          // NUMOS froxel history while car lamps are lit (0 = leave NUMOS's)
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
  bool reverseLight = true;                            // two white reversing lamps in reverse gear (user 2026-11-01; was the right rear lamp)
  float reverseRadiance[3] = { 6000.0f, 6000.0f, 6000.0f };
  float brakeOn = 0.05f;                               // brake input (0..1) that lights them; off again below half of it
  // Backfire flash
  bool pop = true;
  float popRadiance[3] = { 53460.0f, 24057.0f, 6415.0f };   // orange (flame), point light; the high beams before their -15 % (user 2026-10-23)
  float popRadius = 0.08f, popHold = 0.06f, popOut = 0.12f;   // m, s after the last pop draw, m beyond the pipe tip
  unsigned long long popTexture = 0x93E4994710A5380BULL;
  // Lamp halo sprite (user 2026-11-02: the glow around a lit lamp is too big and strong and the lamp mesh clips through it):
  // Cars\*\textures\light.dds, a 64x64 white disc with a soft alpha edge that RBR draws additive for every lamp, is drawn
  // as a generated twin with a steeper edge. core = the part of the radius kept as the game has it, scale = how much of the
  // rest is left, falloff = how steeply that fades (exponent).
  bool halo = true;
  unsigned long long haloTexture = 0x660BCAB491C6BCCAULL;
  float haloCore = 0.25f, haloScale = 0.6f, haloFalloff = 2.0f;
  bool gameLamps = true;                               // L also switches the game's own lit lamp meshes / glow sprites
  bool lampPulse = true;                               // lamp_learn_pulse: the game's lamp signal once off / on at a stage's start
  // Pose (rewrite 2026-10-30, see "Pose of the lamps" below): the drawn car's lag behind RBR's car object, seconds;
  // < 0 = measured while driving (headlights.ini pose_lag = auto).
  float poseLag = -1.0f;
  bool poseLog = false;                                // per-frame CSV rbrrtx_lamp_pose.csv in the run's logs
  // Lamp type (lamp_photometry): what the head lamps and brights are, for the log.
  std::wstring lampNote = L"custom (radiance keys)";
  // Extra lamps (RSF carparts: bonnet pods, light bars, bumper lamps): a spot at each extra light node the car's live
  // <car>_settings.ini switches on. extra = 0 off, 1 with the headlights, 2 with the brights only. Each lamp gets
  // extraIntensity x a high beam's radiance in its own [LightN] colour; position lamps (posi, gab) get extraWeak x that.
  int extra = 1;
  float extraIntensity = 0.6f, extraCone = 20.0f, extraWeak = 0.04f;
} g_cfg;
bool g_haloStale = true;                               // load_settings ran: rebuild the halo twin on its next draw

// The player's car slot (headlights_frame_end), for the lamp type by year.
int g_carSlot = -1;

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

// ---------------------------------------------------------------- lamp type and photometry (user 2026-10-30)
// "Lights never brighter than the sun, a realistic scene; the hue easy to set; LED for cars from 2010, xenon 1998-2009,
// halogen before". The head lamps and brights are given as a real lamp is: luminous intensity on the beam axis in
// candela (cd = lux on a surface 1 m away) and a colour temperature in kelvin (or an sRGB colour). Under NUMOS they
// become Remix radiance through NUMOS's own calibration (its sun, sunIlluminance x sunIntensity, is 127,000 lux;
// numos renderer.cpp luxPerUnit), the sun cap keeps every lamp at or below the sun on a surface 1 m away, and the
// radius no longer changes the brightness. On Remix 1.5.2 (no NUMOS) only the colour applies, the brightness stays
// that of the radiance keys.

// Kelvin -> linear Rec.709 / sRGB primaries (Remix's working space), luminance 1. Planckian locus after Kim et al.
// (cubic fits of CIE 1931 x, y, 1667-25000 K), then XYZ -> linear sRGB (IEC 61966-2-1); negative channels clipped.
void kelvin_rgb(float kelvin, float out[3]) {
  const double T = std::clamp(static_cast<double>(kelvin), 1667.0, 25000.0);
  const double x = T <= 4000.0 ? -0.2661239e9 / (T * T * T) - 0.2343589e6 / (T * T) + 0.8776956e3 / T + 0.179910
                               : -3.0258469e9 / (T * T * T) + 2.1070379e6 / (T * T) + 0.2226347e3 / T + 0.240390;
  const double y = T <= 2222.0 ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
                 : T <= 4000.0 ? -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867
                               : 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
  const double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
  double r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
  double g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
  double b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
  r = (std::max)(r, 0.0); g = (std::max)(g, 0.0); b = (std::max)(b, 0.0);
  const double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  out[0] = static_cast<float>(r / l); out[1] = static_cast<float>(g / l); out[2] = static_cast<float>(b / l);
}

// sRGB 0-255 (as a colour picker shows it) -> linear, luminance 1.
void srgb_rgb(const float c[3], float out[3]) {
  double v[3];
  for (int i = 0; i < 3; ++i) {
    const double u = std::clamp(c[i] / 255.0, 0.0, 1.0);
    v[i] = u <= 0.04045 ? u / 12.92 : std::pow((u + 0.055) / 1.055, 2.4);
  }
  const double l = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
  for (int i = 0; i < 3; ++i) out[i] = l > 1e-6 ? static_cast<float>(v[i] / l) : 1.0f;
}

float luminance(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// The car's model year: Cars\Cars.ini [CarNN] RSFCarID -> <game>\rsfdata\cache\cars_data.json "year" (RSF's car
// data: 102 of 103 cars have one). 0 = not known.
int car_year(int carId, int* rsfId) {
  *rsfId = 0;
  if (carId < 0 || carId > 7) return 0;
  wchar_t dir[MAX_PATH] = {};
  DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
  wchar_t* slash = (n > 0 && n < MAX_PATH) ? wcsrchr(dir, L'\\') : nullptr;
  if (!slash) return 0;
  slash[1] = 0;
  const std::wstring base(dir);
  wchar_t sec[8];
  swprintf_s(sec, L"Car%02d", carId);
  *rsfId = static_cast<int>(GetPrivateProfileIntW(sec, L"RSFCarID", 0, (base + L"Cars\\Cars.ini").c_str()));
  if (*rsfId <= 0) return 0;
  static std::unordered_map<int, int> cache;
  auto it = cache.find(*rsfId);
  if (it != cache.end()) return it->second;
  std::ifstream f(base + L"rsfdata\\cache\\cars_data.json", std::ios::binary);
  const std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto value = [&](size_t from, size_t to, const char* key) -> std::string {   // "key" : "value" inside [from, to)
    const std::string k = std::string("\"") + key + "\"";
    size_t p = d.find(k, from);
    if (p == std::string::npos || p >= to) return "";
    p = d.find(':', p + k.size());
    if (p == std::string::npos || p >= to) return "";
    p = d.find('"', p);
    const size_t e = p == std::string::npos ? p : d.find('"', p + 1);
    return e == std::string::npos || e >= to ? "" : d.substr(p + 1, e - p - 1);
  };
  int year = 0;
  for (size_t o = d.find('{'); o != std::string::npos; o = d.find('{', o + 1)) {
    const size_t end = d.find('}', o);
    if (end == std::string::npos) break;
    if (atoi(value(o, end, "car_id").c_str()) == *rsfId) { year = atoi(value(o, end, "year").c_str()); break; }
  }
  cache[*rsfId] = year;
  return year;
}

void lamp_photometry(const wchar_t* path, Settings& s) {
  wchar_t v[64] = {};
  ini_str(L"lamp_type", v, 64, path);
  std::wstring type = v[0] ? v : L"auto";
  for (wchar_t& ch : type) ch = static_cast<wchar_t>(towlower(ch));
  if (type == L"custom") { s.lampNote = L"custom (radiance keys)"; return; }
  int rsf = 0;
  const int year = car_year(g_carSlot, &rsf);
  wchar_t why[96] = L"set in headlights.ini";
  if (type == L"auto") {
    const int led = static_cast<int>(ini_float(path, L"led_from_year", 2010.0f));
    const int xenon = static_cast<int>(ini_float(path, L"xenon_from_year", 1998.0f));
    type = !year ? L"xenon" : year >= led ? L"led" : year >= xenon ? L"xenon" : L"halogen";
    if (year) swprintf_s(why, L"car from %d (RSF car %d)", year, rsf);
    else swprintf_s(why, L"car year not known (RSF car %d): the middle one", rsf);
  }
  // A key of the car's own section wins, then the type's section [lamp.<type>].
  const std::wstring sec = L"lamp." + type;
  auto key = [&](const wchar_t* k, wchar_t* out, DWORD n) {
    out[0] = 0;
    if (!g_carSection.empty()) GetPrivateProfileStringW(g_carSection.c_str(), k, L"", out, n, path);
    if (!out[0]) GetPrivateProfileStringW(sec.c_str(), k, L"", out, n, path);
  };
  float rgb[3] = { 1, 1, 1 };
  wchar_t c[96] = {}, t[32] = {};
  float cr, cg, cb;
  key(L"color", c, 96);
  key(L"colour_temperature", t, 32);
  const float kelvin = t[0] ? static_cast<float>(_wtof(t)) : type == L"led" ? 6500.0f : type == L"halogen" ? 3200.0f : 5000.0f;
  wchar_t colourNote[64];
  if (c[0] && swscanf_s(c, L"%f , %f , %f", &cr, &cg, &cb) == 3) {
    const float in[3] = { cr, cg, cb };
    srgb_rgb(in, rgb);
    swprintf_s(colourNote, L"colour %.0f, %.0f, %.0f", cr, cg, cb);
  } else {
    kelvin_rgb(kelvin, rgb);
    swprintf_s(colourNote, L"%.0f K", kelvin);
  }
  wchar_t iv[32] = {}, bv[32] = {};
  key(L"intensity", iv, 32);
  key(L"brights_intensity", bv, 32);
  float cd = iv[0] ? static_cast<float>(_wtof(iv)) : type == L"led" ? 80000.0f : type == L"halogen" ? 40000.0f : 60000.0f;
  float bcd = bv[0] ? static_cast<float>(_wtof(bv)) : type == L"led" ? 127000.0f : type == L"halogen" ? 90000.0f : 110000.0f;
  const float unitLux = rbrrtx_numos_lux_per_unit();
  wchar_t brightNote[160];
  if (unitLux > 0.0f) {
    // Sun cap: on a surface 1 m from the lamp, no more light than the sun gives (127,000 lux x sun_cap).
    const float cap = (std::max)(0.0f, ini_float(path, L"sun_cap", 1.0f)) * 127000.0f;
    const bool capped = cap > 0.0f && (cd > cap || bcd > cap);
    if (cap > 0.0f) { cd = (std::min)(cd, cap); bcd = (std::min)(bcd, cap); }
    const float area = 3.14159265f * s.radius * s.radius;   // a sphere light's on-axis intensity = radiance x pi r^2
    for (int i = 0; i < 3; ++i) {
      s.radiance[i] = rgb[i] * cd / (unitLux * area);
      s.brightsRadiance[i] = rgb[i] * bcd / (unitLux * area);
    }
    // Rear lamps the same way (user 2026-10-30: "taillights do not seem to be affected by the correct headlight
    // intensity"). Real lamps (UN ECE R7 / R23): a tail lamp 4-12 cd, a brake lamp 60-185 cd, a reversing lamp 80-600 cd;
    // the old radiance keys were about 140 times a real tail lamp (a red pool on the road behind the car).
    auto cdKey = [&](const wchar_t* k, float d) { return (std::max)(0.0f, ini_float(path, k, d)); };
    float red[3] = { 1, 0, 0 }, white[3];
    ini_rgb(path, L"rear_color", red);
    float redLin[3];
    srgb_rgb(red, redLin);
    kelvin_rgb(ini_float(path, L"reverse_colour_temperature", 6500.0f), white);
    const float tail = cdKey(L"tail_intensity", 10.0f), brake = cdKey(L"brake_intensity", 150.0f),
                rev = cdKey(L"reverse_intensity", 300.0f);
    for (int i = 0; i < 3; ++i) {
      s.tailRadiance[i] = redLin[i] * (std::min)(tail, cap > 0.0f ? cap : tail) / (unitLux * area);
      s.brakeRadiance[i] = redLin[i] * (std::min)(brake, cap > 0.0f ? cap : brake) / (unitLux * area);
      s.reverseRadiance[i] = white[i] * (std::min)(rev, cap > 0.0f ? cap : rev) / (unitLux * area);
    }
    swprintf_s(brightNote, L"%.0f cd, brights %.0f cd%s; tail %.0f cd, brake %.0f cd, reverse %.0f cd (NUMOS: 1 unit = %.0f lux)",
               cd, bcd, capped ? L", capped at the sun's light 1 m away" : L"", tail, brake, rev, unitLux);
  } else {                                             // 1.5.2: the colour only, brightness from the radiance keys
    const float l = luminance(s.radiance), bl = luminance(s.brightsRadiance);
    for (int i = 0; i < 3; ++i) { s.radiance[i] = rgb[i] * l; s.brightsRadiance[i] = rgb[i] * bl; }
    swprintf_s(brightNote, L"brightness from the radiance keys (no NUMOS: no candela scale)");
  }
  s.lampNote = type + L" (" + why + L"): " + colourNote + L", " + brightNote;
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
    s.dayRunning = ini_float(path, L"day_running", 1.0f) != 0.0f;
    s.keepLampsAlive = ini_float(path, L"keep_lamps_alive", 1.0f) != 0.0f;
    s.autoBadWeather = ini_float(path, L"auto_bad_weather", 1.0f) != 0.0f;
    ini_str(L"brights_below_sun", v, 32, path);
    if (!_wcsicmp(v, L"none")) s.brightsBelow = -90.0f;
    else if (v[0]) s.brightsBelow = static_cast<float>(_wtof(v));
    ini_rgb(path, L"radiance", s.radiance);
    s.dayIntensity = std::clamp(ini_float(path, L"day_intensity", 1.0f), 0.0f, 1.0f);
    wchar_t fv[64] = {};
    ini_str(L"day_fade", fv, 64, path);
    float f0, f1;
    if (fv[0] && swscanf_s(fv, L"%f , %f", &f0, &f1) == 2 && f1 > f0) { s.dayFade[0] = f0; s.dayFade[1] = f1; }
    ini_rgb(path, L"brights_radiance", s.brightsRadiance);
    s.brightsCone = std::clamp(ini_float(path, L"brights_cone", s.brightsCone), 1.0f, 89.0f);
    s.brightsSoftness = std::clamp(ini_float(path, L"brights_softness", s.brightsSoftness), 0.0f, 1.0f);
    s.brightsPitch = std::clamp(ini_float(path, L"brights_pitch", s.brightsPitch), -30.0f, 30.0f);
    s.brightsInset = std::clamp(ini_float(path, L"brights_inset", s.brightsInset), -1.0f, 1.0f);
    s.quadLamps = ini_float(path, L"quad_lamps", 0.0f) != 0.0f;
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
    s.reverseLight = ini_float(path, L"reverse_light", 1.0f) != 0.0f;
    ini_rgb(path, L"reverse_radiance", s.reverseRadiance);
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
    s.halo = ini_float(path, L"halo", 1.0f) != 0.0f;
    wchar_t ht[32] = {};
    ini_str(L"halo_texture", ht, 32, path);
    if (ht[0]) s.haloTexture = wcstoull(ht, nullptr, 16);
    s.haloCore = std::clamp(ini_float(path, L"halo_core", s.haloCore), 0.0f, 0.95f);
    s.haloScale = std::clamp(ini_float(path, L"halo_scale", s.haloScale), 0.05f, 1.0f);
    s.haloFalloff = std::clamp(ini_float(path, L"halo_falloff", s.haloFalloff), 0.5f, 8.0f);
    s.gameLamps = ini_float(path, L"game_lamps", 1.0f) != 0.0f;
    s.lampPulse = ini_float(path, L"lamp_learn_pulse", 1.0f) != 0.0f;
    s.extra = std::clamp(static_cast<int>(ini_float(path, L"extra_lights", 1.0f)), 0, 2);
    s.extraIntensity = std::clamp(ini_float(path, L"extra_intensity", s.extraIntensity), 0.0f, 10.0f);
    s.extraCone = std::clamp(ini_float(path, L"extra_cone", s.extraCone), 1.0f, 89.0f);
    s.extraWeak = std::clamp(ini_float(path, L"extra_weak", s.extraWeak), 0.0f, 1.0f);
    wchar_t lag[32] = {};
    ini_str(L"pose_lag", lag, 32, path);
    if (lag[0] && _wcsicmp(lag, L"auto")) s.poseLag = std::clamp(static_cast<float>(_wtof(lag)), -0.1f, 0.3f);
    s.poseLog = ini_float(path, L"pose_log", 0.0f) != 0.0f || rbrrtx_env_flag(L"RBRRTX_LAMP_LOG", false);
    // Remix Plus + NUMOS (user 2026-10-29): head, brake, tail and reverse lamps x remixplus_intensity (0.5 = half);
    // the backfire flash is left alone. numos_history_frames: see rbrrtx_headlights_history_frames.
    if (rbrrtx_remix_plus()) {
      const float k = std::clamp(ini_float(path, L"remixplus_intensity", 1.0f), 0.0f, 10.0f);
      for (float* rgb : { s.radiance, s.brightsRadiance, s.brakeRadiance, s.tailRadiance, s.reverseRadiance })
        for (int i = 0; i < 3; ++i) rgb[i] *= k;
      s.historyFrames = std::clamp(ini_float(path, L"numos_history_frames", 0.0f), 0.0f, 255.0f);
    }
    lamp_photometry(path, s);                          // head lamps and brights: type, colour, candela
  }
  g_cfg = s;
  g_haloStale = true;                                  // the twin is made again from the new settings
  hlog(L"headlights: lamps %s", s.lampNote.c_str());
  hlog(L"headlights: lamp halo sprite %016llX %s (core %.2f, scale %.2f, falloff %.1f)", s.haloTexture, s.halo ? L"thinned" : L"as the game draws it",
       s.haloCore, s.haloScale, s.haloFalloff);
  if (!g_carSection.empty()) {
    wchar_t keys[16] = {};
    const bool own = GetPrivateProfileSectionW(g_carSection.c_str(), keys, 16, path) > 0;
    hlog(L"headlights: car section [%s] %s", g_carSection.c_str(), own ? L"found: its keys override [headlights]" : L"not in the ini");
  }
  wchar_t autoNote[96];
  if (s.dayRunning) swprintf_s(autoNote, L"day running lights, brights below sun %.1f deg", s.brightsBelow);
  else swprintf_s(autoNote, L"auto below sun %.1f deg in mode %d, brights below %.1f", s.autoBelow, s.autoMode, s.brightsBelow);
  hlog(L"headlights (%s): %s, %s, radiance %.0f/%.0f/%.0f, "
       L"radius %.3f, cone %.1f, softness %.2f, focus %.2f, pitch %.1f, forward %.2f, volumetric %.2f, "
       L"keep fallback light %s; brights radiance %.0f cone %.1f softness %.3f pitch %.1f inset %.2f forward %.2f",
       found ? path : L"defaults, no headlights.ini", s.enabled ? L"on" : L"OFF", autoNote, s.radiance[0], s.radiance[1], s.radiance[2], s.radius, s.cone, s.softness, s.focus, s.pitch, s.forward, s.volumetric,
       s.fallbackOn ? L"yes" : L"no", s.brightsRadiance[0], s.brightsCone, s.brightsSoftness, s.brightsPitch, s.brightsInset, s.brightsForward);
  hlog(L"headlights: daylight x%.2f with the sun at %.0f deg and above, full strength at %.0f deg and below "
       L"(backfire flash not dimmed)", s.dayIntensity, s.dayFade[1], s.dayFade[0]);
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
  bool reverse = false;                                // Point_light_reverse_lb / _rb found (105-108 of 109 models)
  V3 revl = {}, revr = {};                             // else the rear lamps
  bool exhaust = false;                                // Point_exhaust1 (pipe tip) found
  V3 exhaustTip = {}, exhaustDir = {};                 // tip and its outward direction (from Point_exhaust2)
  V3 forward = { 0, 0, -1 };                           // model forward axis (up = +y)
  Placed body = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 0 } } };   // model root -> body node (e_body, i_body)
  std::string bodyName = "(none: root frame)";
  std::vector<Placed> parts;                           // every drawn node (e_* / i_*), identical frames merged
  bool glass = false;                                  // windscreen plane from the e_window_f mesh (windscreen.cpp)
  V3 glassBottom = {}, glassTop = {}, glassN = {};     // edge middles and outward normal, root frame
  float glassWb = 0.0f, glassWt = 0.0f;                // widths at the bottom and the top edge
  int glassPoints = 0;
  float glassFlat = 0.0f;                              // rms distance from the plane / plane length (curved glass ~0.1)
  bool glassRejected = false;                          // found but implausible (glass_plausible): lamp-based plane
  bool glassMesh = false;                              // windscreen mesh read (plausible or not)
  std::string glassName;                               // i_window_f (interior) or e_window_f
  // The glass meshes' corners in their own node frame (bottom left/right, top left/right), plausible or not:
  // i_window_f, e_window_f, k_windscreen. The cockpit glass search (cockpit_glass_find) puts them through this frame's
  // CarGlassInternal draws. k_windscreen (run 13) is a 4-vertex quad in every model (E30 1.6 x 0.88 m), textured
  // <car>_internal_kwindscreen.dds: RBR's own windscreen plane for the cockpit, the mesh CarGlassInternal draws.
  struct Fit { bool ok = false; V3 local[4] = {}; Placed node = {}; } fits[3];
  int cockpitMesh = -1;                                // which fit the cockpit draw is expected to be
  // Run 16: RBR's own cockpit windscreen quad (k_windscreen mesh record: its 3x4 matrix x its 4 vertices) in the
  // car ini's camera frame, and the driving camera [Cam_internal] (Pos/Target/Up) in that frame: the quad in camera
  // space, from the files alone.
  bool kQuad = false;
  V3 kCam[4] = {};                                     // bottom left, bottom right, top left, top right (camera space)
  Placed glassNode = {};                               // model root -> e_window_f node local (its node chain)
  // Extra lamps switched on in the live <car>_settings.ini (load_extra_lamps), in the root frame.
  struct Extra { std::string name; V3 pos = {}; float rgb[3] = { 1, 1, 1 }; bool weak = false; bool brake = false; };
  std::vector<Extra> extras;
  std::vector<V3> lampPts;                             // vertices of the lamp parts, root frame (lens_front)
  bool hasLens = false;                                // lamp glass / lens / cover meshes of its own: the light is inside them
  std::vector<unsigned> brakeLensVerts;                // vertex counts of the unlit brake lamp lenses (e_light_brake_*, not *_lit)
  std::vector<unsigned> auxVerts;                      // vertex counts of the auxiliary lamp meshes (not their glass)
  std::vector<unsigned> lampGlassVerts;                // vertex counts of the front lamps' glass meshes
  std::vector<unsigned> auxGlassVerts;                 // vertex counts of the auxiliary lamps' glass meshes
} g_car;
constexpr size_t kMaxExtra = 16;

// The value of "key = value" on a line of an ini block ("" if absent).
std::string ini_field(const std::string& block, const char* key) {
  const size_t klen = std::strlen(key);
  size_t at = 0;
  while ((at = block.find(key, at)) != std::string::npos) {
    const bool lineStart = at == 0 || block[at - 1] == '\n';
    size_t e = at + klen;
    while (e < block.size() && (block[e] == ' ' || block[e] == '\t')) ++e;
    if (lineStart && e < block.size() && block[e] == '=') {
      size_t v = e + 1;
      while (v < block.size() && (block[v] == ' ' || block[v] == '\t')) ++v;
      const size_t end = block.find_first_of("\r\n", v);
      std::string out = block.substr(v, end == std::string::npos ? std::string::npos : end - v);
      while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
      return out;
    }
    at = e;
  }
  return std::string();
}

// RBR's [LightN] Id says what switches a lamp: 4 / 5 (6-11 on light bars) light with the headlights, 16-18 with the
// brake, 21 / 22 are position lamps. Only active lamps that are not one of the stock ones (placed elsewhere) count.
template <class NodeFn>
void load_extra_lamps(const std::wstring& modelFile, NodeFn node) {
  const size_t slash = modelFile.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return;
  const std::wstring dir = modelFile.substr(0, slash + 1);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*_settings.ini").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  const std::wstring file = dir + fd.cFileName;
  FindClose(h);
  std::ifstream f(file, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto lower = [](std::string x) {
    for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return x;
  };
  static const char* const kStock[] = { "point_light_lf", "point_light_rf", "point_light_lb", "point_light_rb",
                                        "point_light_brake_lb", "point_light_brake_rb", "point_light_reverse_lb",
                                        "point_light_reverse_rb" };
  size_t at = 0;
  while ((at = text.find('[', at)) != std::string::npos && g_car.extras.size() < kMaxExtra) {
    const size_t next = text.find("\n[", at + 1);
    const std::string block = text.substr(at, next == std::string::npos ? std::string::npos : next + 1 - at);
    at = next == std::string::npos ? text.size() : next + 1;
    const std::string part = ini_field(block, "Car Part"), lpart = lower(part);
    if (part.empty() || lower(ini_field(block, "Active")) != "true") continue;
    if (std::any_of(std::begin(kStock), std::end(kStock), [&](const char* k) { return lpart == k; })) continue;
    const int id = std::atoi(ini_field(block, "Id").c_str());
    CarModel::Extra e;
    e.name = part;
    e.weak = lpart.find("posi") != std::string::npos || lpart.find("gab") != std::string::npos;
    e.brake = id >= 16 && id <= 18;
    if (!e.weak && !e.brake && (id < 4 || id > 11)) continue;
    if (std::any_of(g_car.extras.begin(), g_car.extras.end(), [&](const CarModel::Extra& o) { return o.name == part; })) continue;
    if (!node(part.c_str(), e.pos)) continue;
    const float r = static_cast<float>(std::atoi(ini_field(block, "Red").c_str()));
    const float g = static_cast<float>(std::atoi(ini_field(block, "Green").c_str()));
    const float b = static_cast<float>(std::atoi(ini_field(block, "Blue").c_str()));
    const float m = (std::max)({ r, g, b });
    if (m > 0.0f) { e.rgb[0] = r / m; e.rgb[1] = g / m; e.rgb[2] = b / m; }
    else if (e.brake) { e.rgb[1] = e.rgb[2] = 0.0f; }
    g_car.extras.push_back(e);
  }
  std::wstring names;
  for (const CarModel::Extra& e : g_car.extras)
    names += std::wstring(e.name.begin(), e.name.end()) + (e.brake ? L" (brake) " : e.weak ? L" (position) " : L" ");
  hlog(L"headlights: extra lamps from %s: %zu %s", file.c_str(), g_car.extras.size(), names.empty() ? L"(none switched on)" : names.c_str());
}

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

// Windscreen glass (user 2026-10-23: drops placed from each car's own glass; prototype tools/kb/sgc_glass.py, all 109
// models of the install). Mesh records: tag u32 0xC, 0, k, 1 (k 1 or 3 seen), u32 size, part name\0, then at +136
// u32 vertex count, u32 flags (7 or 23), at +144 count x (position, normal) floats in the part's frame. e_window_f has
// one such record in every model; its chain to the root places it. Kept: vertices whose normal faces up and forward
// (the glass, either side; not the frame edges). The plane: normal = their mean normal (outward = forward/up), across
// = model x in the plane, up the glass = normal x across; bottom/top edge = lowest/highest quarter of the points.
// A windscreen where one belongs: 0.4-2.5 m behind the head lamps and above them, facing up and forward, 0.6-2 m wide
// (Stratos wraparound 0.71), 0.3-1.2 m long, nearly flat. headlights_test over the install's 109 models: 106 pass;
// Clio_R3 (glass 1.1 m below the lamps: its vertices are not in the e_window_f frame) and Escort_mk_2 (1.65 m long)
// use the lamp-based plane.
bool glass_plausible() {
  const V3 lampMid = (g_car.lf + g_car.rf) * 0.5f, c = (g_car.glassBottom + g_car.glassTop) * 0.5f;
  const float back = dot(lampMid - c, g_car.forward), length = len(g_car.glassTop - g_car.glassBottom);
  return g_car.lamps && back > 0.4f && back < 2.5f && c.y > lampMid.y && g_car.glassN.y > 0.3f &&
         dot(g_car.glassN, g_car.forward) > 0.1f && g_car.glassWb > 0.6f && g_car.glassWb < 2.0f && length > 0.3f &&
         length < 1.2f && g_car.glassFlat < 0.2f;
}

bool fit_windscreen(const std::vector<char>& d, const std::unordered_map<unsigned, Node>& nodes,
                    const std::unordered_map<std::string, unsigned>& byName, const char* kKey) {
  const auto it = byName.find(kKey);
  if (it == byName.end()) return false;
  const size_t keyLen = std::strlen(kKey) + 1;          // with its terminating 0
  float R[3][3];
  V3 t;
  total_of(nodes, it->second, false, R, t);
  auto rot = [&](V3 a) { return V3{ R[0][0] * a.x + R[0][1] * a.y + R[0][2] * a.z, R[1][0] * a.x + R[1][1] * a.y + R[1][2] * a.z,
                                    R[2][0] * a.x + R[2][1] * a.y + R[2][2] * a.z }; };
  const V3 ref = norm(V3{ 0, 0.8f, 0 } + g_car.forward * 0.6f);
  const char* p = d.data();
  float bestFlat = 1e9f;
  for (size_t i = 0; i + 20 + keyLen + 144 <= d.size(); ++i) {
    if (p[i] != 0xC) continue;
    uint32_t tag[4];
    std::memcpy(tag, p + i, 16);
    if (tag[0] != 0xC || tag[1] != 0 || tag[2] > 7 || tag[3] != 1 || std::memcmp(p + i + 20, kKey, keyLen)) continue;
    const size_t a = i + 20 + keyLen;
    uint32_t n;
    std::memcpy(&n, p + a + 136, 4);
    if (n < 3 || n > 20000 || a + 144 + static_cast<size_t>(n) * 24 > d.size()) continue;
    std::vector<V3> pts;
    V3 nsum = {};
    for (uint32_t k = 0; k < n; ++k) {
      float v[6];
      std::memcpy(v, p + a + 144 + k * 24, 24);
      const V3 nn = rot({ v[3], v[4], v[5] });
      const float dn = dot(nn, ref);
      if (std::fabs(dn) <= 0.5f * len(nn)) continue;   // frame / edge faces
      pts.push_back(rot({ v[0], v[1], v[2] }) + t);
      nsum = nsum + (dn < 0 ? nn * -1.0f : nn);
    }
    if (pts.size() < 3) continue;
    const V3 N = norm(nsum);
    V3 c = {};
    for (const V3& q : pts) c = c + q;
    c = c * (1.0f / pts.size());
    V3 X = norm(V3{ 1, 0, 0 } - N * N.x);               // across: model x in the plane
    V3 S = norm(V3{ N.y * X.z - N.z * X.y, N.z * X.x - N.x * X.z, N.x * X.y - N.y * X.x });
    if (S.y < 0) S = S * -1.0f;                         // up the glass
    float smin = 1e9f, smax = -1e9f, rms = 0.0f;
    for (const V3& q : pts) {
      const float sv = dot(q - c, S), dv = dot(q - c, N);
      smin = (std::min)(smin, sv); smax = (std::max)(smax, sv); rms += dv * dv;
    }
    const float length = smax - smin;
    if (length < 0.1f) continue;
    const float flat = std::sqrt(rms / pts.size()) / length;
    if (flat >= bestFlat) continue;
    float bx0 = 1e9f, bx1 = -1e9f, tx0 = 1e9f, tx1 = -1e9f;
    for (const V3& q : pts) {
      const float sv = dot(q - c, S), xv = dot(q - c, X);
      if (sv <= smin + 0.25f * length) { bx0 = (std::min)(bx0, xv); bx1 = (std::max)(bx1, xv); }
      if (sv >= smax - 0.25f * length) { tx0 = (std::min)(tx0, xv); tx1 = (std::max)(tx1, xv); }
    }
    if (bx1 <= bx0 || tx1 <= tx0) continue;
    bestFlat = flat;
    g_car.glass = true;
    g_car.glassN = N;
    g_car.glassBottom = c + S * smin + X * (0.5f * (bx0 + bx1));
    g_car.glassTop = c + S * smax + X * (0.5f * (tx0 + tx1));
    g_car.glassWb = bx1 - bx0;
    g_car.glassWt = tx1 - tx0;
    g_car.glassPoints = static_cast<int>(pts.size());
    g_car.glassFlat = flat;
  }
  g_car.glassMesh = g_car.glass && placed_of(R, t, &g_car.glassNode);
  if (g_car.glass && !glass_plausible()) {
    g_car.glass = false;                               // windscreen.cpp falls back to the lamp-based plane
    g_car.glassRejected = true;
  }
  return g_car.glass;
}

// Run 11 (user 2026-10-24: "use the interior model"): the drops are for the driving camera, which looks through the
// interior model's windscreen (i_window_f: C3 R5, Impreza, Civic, ...), so that glass first; e_window_f when the car
// has no usable interior one (E30). Both meshes are kept in their node frames for the cockpit glass search.
void load_windscreen(const std::vector<char>& d, const std::unordered_map<unsigned, Node>& nodes,
                     const std::unordered_map<std::string, unsigned>& byName) {
  const CarModel base = g_car;
  CarModel chosen = base;
  CarModel::Fit fits[3];
  bool haveChosen = false, rejected = false;
  for (int k = 0; k < 3; ++k) {
    const char* name = k == 0 ? "i_window_f" : k == 1 ? "e_window_f" : "k_windscreen";
    g_car = base;
    const bool ok = fit_windscreen(d, nodes, byName, name);
    if (g_car.glassMesh) {                             // corners, root frame -> the mesh's node frame
      const V3 X = norm(V3{ 1, 0, 0 } - g_car.glassN * g_car.glassN.x);
      const V3 pts[4] = { g_car.glassBottom - X * (g_car.glassWb * 0.5f), g_car.glassBottom + X * (g_car.glassWb * 0.5f),
                          g_car.glassTop - X * (g_car.glassWt * 0.5f), g_car.glassTop + X * (g_car.glassWt * 0.5f) };
      const Placed& a = g_car.glassNode;
      for (int i = 0; i < 4; ++i) {
        const V3 p = pts[i];
        fits[k].local[i] = { p.x * a.A[0][0] + p.y * a.A[1][0] + p.z * a.A[2][0] + a.A[3][0],
                             p.x * a.A[0][1] + p.y * a.A[1][1] + p.z * a.A[2][1] + a.A[3][1],
                             p.x * a.A[0][2] + p.y * a.A[1][2] + p.z * a.A[2][2] + a.A[3][2] };
      }
      fits[k].node = g_car.glassNode;
      fits[k].ok = true;
    }
    if (ok && !haveChosen && k < 2) { chosen = g_car; chosen.glassName = name; haveChosen = true; }
    rejected |= g_car.glassRejected;
  }
  g_car = chosen;
  if (!haveChosen) g_car.glassRejected = rejected;
  for (int k = 0; k < 3; ++k) g_car.fits[k] = fits[k];
  g_car.cockpitMesh = fits[2].ok ? 2 : g_car.glassName == "e_window_f" ? 1 : 0;
}

// k_windscreen mesh record (tag 0xC 0 k 1, name): +64 a 3x4 matrix (rows: R | t), +136 vertex count, +144 vertices
// (position, normal). The E30's: t (-0.04 1.04 -2.09), 4 vertices 1.6 x 0.88 m. Cam_internal of the car's ini:
// Pos (0.30 0.99 -1.00), Target, Up. Both in the same frame (the quad lands 0.8-1.2 m in front of that camera, its
// bottom edge ~15 degrees down: the E30's glass bottom on screen ~11). Camera axes: forward = Target - Pos,
// up = Up made square to it, right = up x forward (left-handed, as D3D).
void load_cockpit_quad(const std::vector<char>& d, const std::wstring& file) {
  static const char kKey[] = "k_windscreen";
  const char* p = d.data();
  V3 q[4];
  bool found = false;
  for (size_t i = 0; i + 20 + sizeof(kKey) + 144 + 96 <= d.size() && !found; ++i) {
    if (p[i] != 0xC) continue;
    uint32_t tag[4];
    std::memcpy(tag, p + i, 16);
    if (tag[0] != 0xC || tag[1] != 0 || tag[2] > 7 || tag[3] != 1 || std::memcmp(p + i + 20, kKey, sizeof(kKey))) continue;
    const size_t a = i + 20 + sizeof(kKey);
    uint32_t n;
    std::memcpy(&n, p + a + 136, 4);
    if (n != 4) continue;
    float m[12];
    std::memcpy(m, p + a + 64, 48);
    for (int k = 0; k < 4; ++k) {
      float v[3];
      std::memcpy(v, p + a + 144 + k * 24, 12);
      q[k] = { m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3], m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7],
               m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11] };
    }
    found = true;
  }
  if (!found) { hlog(L"headlights: no k_windscreen quad in %s: no cockpit drops for this car", file.c_str()); return; }
  const size_t dot0 = file.find_last_of(L'.');
  const std::wstring base = dot0 == std::wstring::npos ? file : file.substr(0, dot0);
  float pos[3] = {}, tgt[3] = {}, upv[3] = {};
  bool cam = false;
  for (const wchar_t* sfx : { L"_personal.ini", L".ini" }) {
    wchar_t a[96] = {}, b[96] = {}, c[96] = {};
    const std::wstring ini = base + sfx;
    GetPrivateProfileStringW(L"Cam_internal", L"Pos", L"", a, 96, ini.c_str());
    GetPrivateProfileStringW(L"Cam_internal", L"Target", L"", b, 96, ini.c_str());
    GetPrivateProfileStringW(L"Cam_internal", L"Up", L"", c, 96, ini.c_str());
    if (swscanf_s(a, L"%f %f %f", &pos[0], &pos[1], &pos[2]) == 3 && swscanf_s(b, L"%f %f %f", &tgt[0], &tgt[1], &tgt[2]) == 3 &&
        swscanf_s(c, L"%f %f %f", &upv[0], &upv[1], &upv[2]) == 3) { cam = true; break; }
  }
  if (!cam) { hlog(L"headlights: no [Cam_internal] Pos/Target/Up for %s: no cockpit drops for this car", file.c_str()); return; }
  const V3 P = { pos[0], pos[1], pos[2] };
  const V3 F = norm(V3{ tgt[0], tgt[1], tgt[2] } - P);
  // The ini's Up is not an up vector: (0, 0.05, 1) on every car, nearly along the view, so what is left of it after
  // squaring to the view was noise (run 17: Audi quattro / Fiesta R5 quads mirrored and tilted). World up instead.
  (void)upv;
  V3 U = norm(V3{ 0, 1, 0 } - F * F.y);
  const V3 R = { U.y * F.z - U.z * F.y, U.z * F.x - U.x * F.z, U.x * F.y - U.y * F.x };
  V3 c[4];
  for (int k = 0; k < 4; ++k) { const V3 r = q[k] - P; c[k] = { dot(r, R), dot(r, U), dot(r, F) }; }
  // bottom = the two lower corners (camera up), each pair left then right
  int idx[4] = { 0, 1, 2, 3 };
  std::sort(idx, idx + 4, [&](int x, int y) { return c[x].y < c[y].y; });
  if (c[idx[0]].x > c[idx[1]].x) std::swap(idx[0], idx[1]);
  if (c[idx[2]].x > c[idx[3]].x) std::swap(idx[2], idx[3]);
  for (int k = 0; k < 4; ++k) g_car.kCam[k] = c[idx[k]];
  g_car.kQuad = true;
  const V3 b = (g_car.kCam[0] + g_car.kCam[1]) * 0.5f, t = (g_car.kCam[2] + g_car.kCam[3]) * 0.5f;
  hlog(L"headlights: cockpit windscreen from the files (k_windscreen + [Cam_internal]): bottom %.2f ahead %+.2f up %+.2f "
       L"right, top %.2f ahead %+.2f up %+.2f right, %.2f/%.2f wide", b.z, b.y, b.x, t.z, t.y, t.x,
       len(g_car.kCam[1] - g_car.kCam[0]), len(g_car.kCam[3] - g_car.kCam[2]));
}

bool cockpit_quad(float c[4][3]) {                     // camera space (x right, y up, z ahead)
  if (!g_car.kQuad) return false;
  for (int k = 0; k < 4; ++k) { c[k][0] = g_car.kCam[k].x; c[k][1] = g_car.kCam[k].y; c[k][2] = g_car.kCam[k].z; }
  return true;
}

// The vertices of a car's lamp parts (e_light*, e_lamp*, e_bumperlight*, lens or cover glass) in the root frame, for
// lens_front. Same mesh records as load_windscreen: tag 0xC 0 k 1, size, name\0, +136 count, +144 (position, normal).
struct LampBox { V3 lo, hi; };
std::vector<LampBox> g_lampBoxes;                      // bounds of each lamp part of this car (not the aux lamps), root frame

// A light node that lies outside every lamp part is moved onto the nearest one (within kSnapReach): on some models the
// nodes float well clear of the lenses (yaris_rally2: tail nodes 10 cm, reversing nodes 15-20 cm behind the glass and
// one 13 cm too low; user 2026-11-12: "beams a noticeable distance away from the physical lamps").
constexpr float kSnapReach = 0.40f;
bool snap_to_lamp(V3& pos) {
  float best = kSnapReach;
  V3 to = pos;
  for (const LampBox& b : g_lampBoxes) {
    const V3 c = { std::clamp(pos.x, b.lo.x, b.hi.x), std::clamp(pos.y, b.lo.y, b.hi.y), std::clamp(pos.z, b.lo.z, b.hi.z) };
    const float dist = len(c - pos);
    if (dist < 1e-4f) return false;                    // inside a lamp part: the node is fine
    if (dist < best) { best = dist; to = c; }
  }
  if (best >= kSnapReach) return false;
  pos = to;
  return true;
}

void load_lamp_points(const std::vector<char>& d, const std::unordered_map<unsigned, Node>& nodes,
                      const std::unordered_map<std::string, unsigned>& byName) {
  const char* p = d.data();
  for (size_t i = 0; i + 20 + 64 + 144 <= d.size() && g_car.lampPts.size() < 60000; ++i) {
    if (p[i] != 0xC) continue;
    uint32_t tag[4];
    std::memcpy(tag, p + i, 16);
    if (tag[0] != 0xC || tag[1] != 0 || tag[2] > 7 || tag[3] != 1) continue;
    const char* z = static_cast<const char*>(std::memchr(p + i + 20, 0, 64));
    if (!z || z == p + i + 20) continue;
    std::string name(p + i + 20, z), low = name;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low.compare(0, 2, "e_") || (low.find("light") == std::string::npos && low.find("lamp") == std::string::npos)) continue;
    if (low.find("glass") != std::string::npos || low.find("lens") != std::string::npos || low.find("cover") != std::string::npos) g_car.hasLens = true;
    const auto node = byName.find(name);
    if (node == byName.end()) continue;
    const size_t a = static_cast<size_t>(z - p) + 1;
    uint32_t n = 0;
    if (a + 148 > d.size()) continue;
    std::memcpy(&n, p + a + 136, 4);
    if (n < 3 || n > 20000 || a + 144 + static_cast<size_t>(n) * 24 > d.size()) continue;
    if (!low.compare(0, 13, "e_light_brake") && low.find("_lit") == std::string::npos) g_car.brakeLensVerts.push_back(n);
    if ((low.find("extralight") != std::string::npos || low.find("bumperlight") != std::string::npos || low.find("bumper_light") != std::string::npos ||
         low.find("light_ramp") != std::string::npos) && low.find("glass") == std::string::npos) g_car.auxVerts.push_back(n);
    float R[3][3];
    V3 t;
    total_of(nodes, node->second, false, R, t);
    V3 mid = {};
    LampBox box = { { 1e9f, 1e9f, 1e9f }, { -1e9f, -1e9f, -1e9f } };
    for (uint32_t k = 0; k < n; ++k) {
      float v[3];
      std::memcpy(v, p + a + 144 + k * 24, 12);
      g_car.lampPts.push_back({ R[0][0] * v[0] + R[0][1] * v[1] + R[0][2] * v[2] + t.x,
                                R[1][0] * v[0] + R[1][1] * v[1] + R[1][2] * v[2] + t.y,
                                R[2][0] * v[0] + R[2][1] * v[1] + R[2][2] * v[2] + t.z });
      const V3& w = g_car.lampPts.back();
      mid = mid + w * (1.0f / n);
      box.lo = { (std::min)(box.lo.x, w.x), (std::min)(box.lo.y, w.y), (std::min)(box.lo.z, w.z) };
      box.hi = { (std::max)(box.hi.x, w.x), (std::max)(box.hi.y, w.y), (std::max)(box.hi.z, w.z) };
    }
    const bool auxPart = low.find("extralight") != std::string::npos || low.find("bumperlight") != std::string::npos ||
                         low.find("bumper_light") != std::string::npos || low.find("light_ramp") != std::string::npos;
    if (n >= 16 && !auxPart) g_lampBoxes.push_back(box);   // placeholder parts of a few vertices are not lamps
    if (low.find("glass") != std::string::npos) {      // the flat front that glows (user 2026-11-09)
      const bool aux = low.find("extralight") != std::string::npos || low.find("bumperlight") != std::string::npos ||
                       low.find("bumper_light") != std::string::npos || low.find("light_ramp") != std::string::npos;
      if (aux) g_car.auxGlassVerts.push_back(n);
      else if (dot(mid - g_car.lf, g_car.forward) > -0.5f) g_car.lampGlassVerts.push_back(n);   // near the front lamps' plane
    }
  }
}

// How far ahead of a lamp node the front of its own lamp (reflector, lens, cover glass) is, on the car's forward axis:
// the vertices of the lamp parts within kLensAxis of the lamp's axis. 0 = none found. On most cars the node sits behind
// the glass (measured over the 109 models: E30 4.5 cm, Fabia Rally2 9, Porsche GT3 RS 14-27), so a lamp only `forward`
// ahead of its node lit its own lens from a few centimetres: flat white and opaque (user 2026-11-09).
constexpr float kLensAxis = 0.12f, kLensReach = 0.35f;
struct LensHit { V3 pos; float front; };
std::vector<LensHit> g_lensHits;                       // per lamp position, this car
float lens_front(const V3& pos, const V3& axis) {
  for (const LensHit& h : g_lensHits) if (len(h.pos - pos) < 1e-3f) return h.front;
  float front = 0.0f;
  for (const V3& v : g_car.lampPts) {
    const V3 r = v - pos;
    const float along = dot(r, axis);
    if (along < -0.05f || along > kLensReach || along <= front) continue;
    const V3 side = r - axis * along;
    if (dot(side, side) < kLensAxis * kLensAxis) front = along;
  }
  g_lensHits.push_back({ pos, front });
  return front;
}

// Where a beam starts relative to its node: `wanted` metres ahead, but at least clear of the lamp's glass.
V3 beam_ahead(const V3& node, float wanted) {
  return g_car.forward * (std::max)(wanted, lens_front(node, g_car.forward) + g_cfg.radius + 0.01f);
}

// The same behind the car: a tail lamp inside its lens lit the glass itself (user 2026-11-09: "the taillight's glass gets hazy and opaque").
V3 beam_behind(const V3& node, float wanted) {
  const V3 back = g_car.forward * -1.0f;
  return back * (std::max)(wanted, lens_front(node, back) + g_cfg.radius + 0.01f);
}

void load_car_model(const std::wstring& file) {
  const auto t0 = std::chrono::steady_clock::now();
  g_car = CarModel();
  g_lensHits.clear();
  g_lampBoxes.clear();
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
    // Reversing lamps (user 2026-11-01: most cars light two reverse lenses, e_light_reverse_lb/rb_lit): a white light at
    // each Point_light_reverse_* node; a missing side mirrors the other, none at all = the rear lamps' places.
    const bool vl = usable("Point_light_reverse_lb", sl, g_car.revl), vr = usable("Point_light_reverse_rb", -sl, g_car.revr);
    if (vl && !vr) g_car.revr = { 2.0f * c - g_car.revl.x, g_car.revl.y, g_car.revl.z };
    if (vr && !vl) g_car.revl = { 2.0f * c - g_car.revr.x, g_car.revr.y, g_car.revr.z };
    g_car.reverse = vl || vr;
    if (!g_car.reverse) { g_car.revl = g_car.lb; g_car.revr = g_car.rb; }
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
  load_extra_lamps(file, lamp);
  load_lamp_points(d, nodes, byName);
  if (g_car.lamps) {
    int moved = 0;
    float farthest = 0.0f;
    for (V3* n : { &g_car.lf, &g_car.rf, &g_car.lb, &g_car.rb, &g_car.revl, &g_car.revr }) {
      const V3 was = *n;
      if (snap_to_lamp(*n)) { ++moved; farthest = (std::max)(farthest, len(*n - was)); }
    }
    if (moved) hlog(L"headlights: %d light node(s) lay outside the lamp parts and were moved onto the nearest one (up to %.0f cm)", moved, farthest * 100.0f);
  }
  hlog(L"headlights: lamp glass %.1f / %.1f cm ahead of the lamp nodes (%zu lamp vertices): beams start in front of it",
       lens_front(g_car.lf, g_car.forward) * 100.0f, lens_front(g_car.rf, g_car.forward) * 100.0f, g_car.lampPts.size());
  load_windscreen(d, nodes, byName);
  load_cockpit_quad(d, file);
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
  hlog(L"headlights: windscreen glass %S %s: bottom (%.2f %.2f %.2f) %.2f wide, top (%.2f %.2f %.2f) %.2f wide, normal "
       L"(%.2f %.2f %.2f), %d points, flatness %.3f", g_car.glassName.c_str(), g_car.glass ? L"found" : g_car.glassRejected ? L"REJECTED (not where a windscreen goes): lamp-based plane" : L"NOT found: lamp-based plane",
       g_car.glassBottom.x, g_car.glassBottom.y, g_car.glassBottom.z, g_car.glassWb, g_car.glassTop.x, g_car.glassTop.y,
       g_car.glassTop.z, g_car.glassWt, g_car.glassN.x, g_car.glassN.y, g_car.glassN.z, g_car.glassPoints, g_car.glassFlat);
  hlog(L"headlights: reversing lamps %s l (%.3f %.3f %.3f) r (%.3f %.3f %.3f)", g_car.reverse ? L"found" : L"not in the model: at the rear lamps",
       g_car.revl.x, g_car.revl.y, g_car.revl.z, g_car.revr.x, g_car.revr.y, g_car.revr.z);
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

// The car pose C (model root -> world) from RBR's car movement object (0x8EF660, block from +0x100):
// +0x110 is a 3x3 rotation, 9 floats, rows = the car's axes; +0x140 the car position; physics space = render world
// with Y and Z swapped (decoded 2026-10-23 01:43 from "pose sample" lines: the body draw's rows are
// swap(M0), swap(M2), swap(M1) to 3 decimals in two poses). So C rows = those, and C's origin = position + d * R,
// d = the model root's offset from the physics origin in car axes (E30 standing: (-0.06, -0.24, -0.16) m).
// d is measured per car by a vote of the car draws (measure_frame), only while the car stands still, averaged over 20
// frames, then locked (kept for the session per model file). Until then d = 0 (lamps ~0.3 m off in views without a
// body draw; the stage-load view draws the body, so the lamps are on it from the first frame there).
//
// Pose of the lamps (rewrite 2026-10-30, docs/research/headlights-rewrite-handoff.md): the car AS DRAWN in this frame.
// comp.cpp gives Remix every car draw's world matrix W and hands the same W here (rbrrtx_headlights_car_draw); the
// body draw's W composed with the body node's placement is the model root Remix renders the car at, so lamps placed
// from it are rigid with the car on screen by construction. They are sent from it just before Remix takes the
// frame's lights (our UI trigger, else Present: rbrrtx_headlights_refine); Remix Plus updates a live light in place.
// Views without a body draw (cockpit, bonnet) and Remix 1.5.2 (no in-place update: lamps go out once, at the first
// BeginScene) use RBR's car object as it was g_lag seconds earlier, from a short pose history: the drawn car trails
// the object by a near constant time (logs 2026-10-29: 0.2-1.4 m growing with speed, ~70 ms). g_lag is measured
// from frames with a body draw (the delay that puts the object's past pose onto the drawn root; median of 31).
// Replaced: the 0.9/0.1 average of drawn-minus-object (follow_drawn_car, track_drift), which trailed by ~10 frames and
// wandered at every speed change, and the per-view head lamp offsets that flipped with the view.
unsigned g_frameNo = 0, g_solved = 0, g_failed = 0;
struct Offset { int n = 0; V3 sum = {}; bool locked = false; V3 d = {}; int rejected = 0; };
Offset g_off;
std::unordered_map<std::wstring, V3> g_offsetByModel;  // locked offsets of this session
V3 g_lastPhys = {};
bool g_haveLastPhys = false;
int g_sampleLogged = 0;

V3 swap_yz(const float* v) { return { v[0], v[2], v[1] }; }

double now_s() {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// RBR's car object now: axes (render space, rows x, y = up, z) and physics origin. False while it is not a rotation.
bool read_object(V3 ax[3], V3* pos) {
  float r[20];
  if (!rbrrtx_player_car_raw(r)) return false;
  ax[0] = swap_yz(r + 4); ax[1] = swap_yz(r + 10); ax[2] = swap_yz(r + 7);
  for (int i = 0; i < 3; ++i) if (!(len(ax[i]) > 0.5f && len(ax[i]) < 2.0f)) return false;   // object not ready
  *pos = swap_yz(r + 16);
  return true;
}

// C = axes as rows, origin = pos + d in car axes.
void pose_of(const V3 ax[3], V3 pos, float C[16]) {
  for (int i = 0; i < 3; ++i) { C[i * 4] = ax[i].x; C[i * 4 + 1] = ax[i].y; C[i * 4 + 2] = ax[i].z; C[i * 4 + 3] = 0; }
  const V3 o = pos + ax[0] * g_off.d.x + ax[1] * g_off.d.y + ax[2] * g_off.d.z;
  C[12] = o.x; C[13] = o.y; C[14] = o.z; C[15] = 1;
}

// RBR's object now with the model root at d (offset measurement, cockpit test, windscreen anchor).
bool physics_pose(float C[16], V3* phys) {
  V3 ax[3];
  if (!read_object(ax, phys)) return false;
  pose_of(ax, *phys, C);
  return true;
}

// Pose history: RBR's object once per frame (first BeginScene), with the time.
struct PoseSample { double t; V3 ax[3]; V3 pos; };
constexpr int kHist = 64;
PoseSample g_hist[kHist];
int g_histN = 0, g_histHead = 0;                       // head = newest
double g_frameT = 0.0;                                 // time of this frame's sample

const PoseSample& hist(int age) { return g_hist[(g_histHead - age + kHist) % kHist]; }

void hist_push(double t) {
  PoseSample s;
  if (!read_object(s.ax, &s.pos)) { g_histN = 0; return; }
  s.t = t;
  // A jump (stage restart, replay seek or camera cut to another car moment): the old samples mean nothing now.
  if (g_histN && (t <= hist(0).t || len(s.pos - hist(0).pos) > 25.0f)) g_histN = 0;
  g_histHead = (g_histHead + 1) % kHist;
  g_hist[g_histHead] = s;
  if (g_histN < kHist) ++g_histN;
}

PoseSample lerp(const PoseSample& a, const PoseSample& b, float f) {
  PoseSample s;
  s.t = a.t + (b.t - a.t) * f;
  for (int i = 0; i < 3; ++i) s.ax[i] = norm(a.ax[i] + (b.ax[i] - a.ax[i]) * f);
  s.pos = a.pos + (b.pos - a.pos) * f;
  return s;
}

// The object at time t: between two samples linear, after the newest extrapolated from the last two (at most 0.15 s),
// before the oldest the oldest.
bool hist_at(double t, PoseSample* out) {
  if (!g_histN) return false;
  if (t >= hist(0).t) {
    if (g_histN < 2 || hist(0).t - hist(1).t < 1e-4) { *out = hist(0); return true; }
    const double ahead = (std::min)(t - hist(0).t, 0.15);
    *out = lerp(hist(1), hist(0), static_cast<float>(1.0 + ahead / (hist(0).t - hist(1).t)));
    return true;
  }
  for (int age = 1; age < g_histN; ++age) {
    const PoseSample &o = hist(age), &n = hist(age - 1);
    if (t >= o.t) { *out = lerp(o, n, static_cast<float>((t - o.t) / (n.t - o.t))); return true; }
  }
  *out = hist(g_histN - 1);
  return true;
}

// Speed of the object over the last ~0.1 s (m/s).
float hist_speed() {
  if (g_histN < 2) return 0.0f;
  int age = 1;
  while (age + 1 < g_histN && hist(0).t - hist(age).t < 0.1) ++age;
  const double dt = hist(0).t - hist(age).t;
  return dt > 1e-3 ? len(hist(0).pos - hist(age).pos) / static_cast<float>(dt) : 0.0f;
}

// The drawn car's lag behind the object.
float g_lag = 0.0f;
bool g_lagKnown = false;
float g_lagSamples[31];
int g_lagN = 0, g_lagNext = 0;
float g_lagResid = 0.0f;                               // distance left at the best lag, last sample (m)
unsigned g_lagTaken = 0, g_lagRefused = 0, g_lagLogged = 0;

// The object as the car is drawn: g_lag seconds back (the prediction for views without a body draw).
bool pred_pose(float C[16]) {
  PoseSample s;
  if (g_histN && hist_at(g_frameT - g_lag, &s)) { pose_of(s.ax, s.pos, C); return true; }
  V3 phys;
  return physics_pose(C, &phys);
}

// One lag sample from this frame's drawn body root B: the delay in -0.1 .. 0.3 s whose past object pose puts the
// model root nearest B. Only while driving (> 4 m/s: standing still any delay fits) and with d known.
void lag_sample(const float B[16]) {
  if (!g_off.locked || g_histN < 8 || g_cfg.poseLag >= 0.0f) return;
  const float v = hist_speed();
  if (v < 4.0f) return;
  float best = 1e9f, bestLag = 0.0f;
  for (int i = 0; i <= 200; ++i) {
    const float lag = -0.1f + 0.002f * i;
    PoseSample s;
    if (!hist_at(g_frameT - lag, &s)) return;
    float C[16];
    pose_of(s.ax, s.pos, C);
    const float e = len(row(C, 3) - row(B, 3));
    if (e < best) { best = e; bestLag = lag; }
  }
  g_lagResid = best;
  if (best > 0.3f || bestLag <= -0.099f || bestLag >= 0.299f) { ++g_lagRefused; return; }   // no delay explains it
  g_lagSamples[g_lagNext] = bestLag;
  g_lagNext = (g_lagNext + 1) % 31;
  if (g_lagN < 31) ++g_lagN;
  float sorted[31];
  std::copy(g_lagSamples, g_lagSamples + g_lagN, sorted);
  std::sort(sorted, sorted + g_lagN);
  g_lag = sorted[g_lagN / 2];
  g_lagKnown = g_lagN >= 5;
  if (++g_lagTaken == 5 || g_lagTaken % 1800 == 0)
    hlog(L"headlights: drawn car trails RBR's car object by %.0f ms (median of %d; last sample %.0f ms at %.1f m/s, %.3f m left)",
         g_lag * 1000.0f, g_lagN, bestLag * 1000.0f, v, best);
}

// This frame's body draw: of the car draw groups whose body-node root has the car's axes (within ~10 deg: the drawn
// car may be turned a little against the predicted one) the big ones (>= 35 % of the biggest such group), the one
// nearest where the body is expected: last frame's drawn root moved as the object moved since, else the prediction.
// The wrong 3rd-person body (4.6 m off, often bigger), the bonnet and doors (a part's offset away) lose on distance,
// spinning wheels on the axes.
float g_bodyM[16];                                     // the last matched drawn root
V3 g_bodyObj = {};                                     // the object's position in that frame
unsigned g_bodyFrames = 0;                             // frames in a row with the body matched (outside views)
unsigned g_bodyMatched = 0, g_bodyMissed = 0;
float g_bodyGap = 0.0f, g_bodyGapMax = 0.0f;           // drawn root - object root (no lag), this frame / this stage
unsigned g_bodyHeld = 0;                               // frames in a row the lamps held the last body (no match)
constexpr unsigned kHoldFrames = 10;                   // at most this many, then the prediction
// Last match (pose log): candidates with the car's axes, eligible ones, the pick's triangles, its distance from where
// the body was expected and the runner-up's (0 = none). Two close candidates = the match can alternate between them.
int g_matchCands = 0, g_matchEligible = 0;
unsigned g_matchPrims = 0;
float g_matchD = 0.0f, g_matchD2 = 0.0f;
// This car's body size: the biggest pick that sat on the prediction (run 20261101-052355 Osli: after L toggles and a
// camera switch the 40042-triangle body group was gone for 15 s; 4519-12606-triangle parts, roots up to 2 m apart,
// won in turn, and "near last frame's body" followed them away from the car). A pick must be half of it and near the
// prediction too; without one the lamps take the prediction (cm-accurate with the lag known).
unsigned g_bodyPrimsRef = 0;

bool match_body(float out[16]) {
  float P[16];
  V3 phys;
  if (!physics_pose(P, &phys) || !pred_pose(P)) return false;
  const float v = hist_speed();
  const V3 pred = row(P, 3);
  V3 want = pred;
  float tol = g_lagKnown ? 0.6f + 0.04f * v : (std::min)(1.2f + 0.12f * v, 5.0f);
  const float tolPred = g_lagKnown ? 0.35f + 0.015f * v : tol;   // the prediction bounds the chain below
  if (g_bodyFrames) { want = row(g_bodyM, 3) + (phys - g_bodyObj); tol = 0.4f + 0.02f * v; }
  struct Cand { float C[16]; unsigned prims; float d, dPred; };
  Cand cand[24];
  int n = 0;
  unsigned maxPrims = 0;
  for (const Group& g : g_groups) {
    if (g.prims < 200 || n >= 24) continue;
    Cand& c = cand[n];
    compose(g_car.body, g.m, c.C);
    bool axes = true;
    for (int r = 0; r < 3 && axes; ++r) axes = dot(norm(row(c.C, r)), norm(row(P, r))) > 0.985f;
    if (!axes) continue;
    c.prims = g.prims;
    c.d = len(row(c.C, 3) - want);
    c.dPred = len(row(c.C, 3) - pred);
    maxPrims = (std::max)(maxPrims, g.prims);
    ++n;
  }
  const Cand* best = nullptr;
  g_matchCands = n;
  g_matchEligible = 0;
  g_matchD2 = 0.0f;
  for (int i = 0; i < n; ++i)
    if (cand[i].prims * 100 >= maxPrims * 35 && cand[i].d < tol && cand[i].dPred < tolPred &&
        cand[i].prims * 2 >= g_bodyPrimsRef) {
      ++g_matchEligible;
      if (!best || cand[i].d < best->d) { if (best) g_matchD2 = best->d; best = &cand[i]; }
      else if (!g_matchD2 || cand[i].d < g_matchD2) g_matchD2 = cand[i].d;
    }
  g_matchPrims = best ? best->prims : 0;
  g_matchD = best ? best->d : 0.0f;
  if (!best) return false;
  if (g_lagKnown && best->dPred < 0.15f) g_bodyPrimsRef = (std::max)(g_bodyPrimsRef, best->prims);
  std::memcpy(out, best->C, sizeof(best->C));
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

bool place_frame(float C[16]) { return pred_pose(C); }

// ---------------------------------------------------------------- state

enum { kOff = 0, kPair = 2, kBrights = 3 };
const remixapi_Interface* g_api = nullptr;
int g_mode = kOff;
bool g_manual = false;                                 // the key was used on this stage: no automatic switching
float g_dayK = 1.0f;                                   // daylight dimming of the lamps (day_intensity)
int g_autoWant = -1;                                   // the automatic switch's mode last frame (-1 = none yet)
bool g_badWeather = false;                             // this stage's sky is rain, snow or fog
// The mode the automatic switch wants at this sun height.
// day_running = 0 also turns the brights on below brights_below_sun (user 2026-11-12: a full-night Peklo replay had only
// the two lamps; with day_running = 0 the switch knew only auto_mode).
int auto_mode_for(float sunEl) {
  if (g_cfg.dayRunning) return sunEl < g_cfg.brightsBelow ? kBrights : kPair;
  if (!(sunEl < g_cfg.autoBelow || (g_cfg.autoBadWeather && g_badWeather))) return kOff;
  return sunEl < g_cfg.brightsBelow ? kBrights : g_cfg.autoMode;
}
int g_track = -1;
bool g_emitted = false;                                // first BeginScene of this frame seen (lamp states read)
bool g_refined = false;                                // this frame's drawn body looked for (rbrrtx_headlights_refine)
bool g_sentThisFrame = false;                          // lamps sent this frame
bool g_lampsLit = false;                               // some car lamp is lit this frame
// Lamp states of this frame (read at the first BeginScene), used wherever the lamps are sent from.
bool g_tailOn = false, g_popOn = false;
const wchar_t* g_poseSrc = L"-";                       // where this frame's lamps were placed from (pose log)
FILE* g_poseCsv = nullptr;
unsigned g_lampFrame = 0;                              // last frame a car lamp was sent
struct Lamp { remixapi_LightHandle h = nullptr; unsigned long long hash; V3 pos{}, dir{}; float cone = 0; float rad[3] = {};
              unsigned sent = ~0u; };                  // sent = frame it was last drawn
// RBRHLCEN / LFT / RGT, RBRHBRLF / BRRF, RBRTLLFT / RGT (rear), RBRBFPOP (backfire), RBRRVLFT / RGT (reversing)
Lamp g_lamps[10 + kMaxExtra] = { { nullptr, 0x52425248'4C43454EULL }, { nullptr, 0x52425248'4C4C4654ULL }, { nullptr, 0x52425248'4C524754ULL },
                     { nullptr, 0x52425248'42524C46ULL }, { nullptr, 0x52425248'42525246ULL }, { nullptr, 0x52425254'4C4C4654ULL },
                     { nullptr, 0x52425254'4C524754ULL }, { nullptr, 0x52425242'46504F50ULL }, { nullptr, 0x52425252'564C4654ULL },
                     { nullptr, 0x52425252'56524754ULL } };
Lamp* const g_extraLamps = g_lamps + 10;               // RBRXL00.. (hashes set in rbrrtx_headlights_init)
unsigned g_creates = 0;
bool g_reversing = false;                              // reverse lamp lit (gear 0)
unsigned g_reverseSwitches = 0;
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

// A lamp is only sent when it changes, so one lost while a stage loads stayed lost on the start line until the car moved or
// L re-created it (user 2026-11-08: no beam from the cockpit at first). Sent every frame for a few seconds after any change.
int g_lampResend = 0;

void set_mode(int m, const wchar_t* why) {
  if (m == g_mode) return;
  g_mode = m;
  g_lampResend = 300;
  // Only the lamps the new mode does not use (send_lamps also destroys every lamp it did not draw). Destroying all of
  // them here re-created the low beams under the same hash in the same frame, and on Remix Plus the destroy won: OFF ->
  // BRIGHTS showed the high beams alone until the car moved (run 20261030-072711, Gabiria marks 1-3).
  const bool keep = g_cfg.keepLampsAlive && g_track >= 0;   // send_lamps keeps them at zero strength instead
  if (!keep && m != kBrights && !(g_cfg.quadLamps && m == kPair)) { destroy_lamp(g_lamps[3]); destroy_lamp(g_lamps[4]); }
  if (!keep && m == kOff) { destroy_lamp(g_lamps[1]); destroy_lamp(g_lamps[2]); }
  g_creates = 0;                                       // log the first lamps of each switch
  hlog(L"headlights: %s (%s)", mode_name(m), why);
  wchar_t ev[96];
  swprintf_s(ev, L"headlights %s (%s)", mode_name(m), why);
  rbrrtx_report_event(ev);
}

// cone >= 180 = a plain point light (no shaping; dir unused). radius 0 = the headlights' radius.
void draw_lamp(Lamp& l, V3 pos, V3 dir, float cone, const float radIn[3], float softness, float radius = 0.0f) {
  const float k = &l == &g_lamps[7] ? 1.0f : g_dayK;     // daylight dimming; the backfire flash stays
  const float rad[3] = { radIn[0] * k, radIn[1] * k, radIn[2] * k };
  const bool moved = g_lampResend > 0 || !l.h || len(pos - l.pos) > 0.005f || dot(dir, l.dir) < 0.99998f || cone != l.cone ||
                     rad[0] != l.rad[0] || rad[1] != l.rad[1] || rad[2] != l.rad[2];
  if (moved) {
    // Remix Plus: the same hash again UPDATES the live lamp and keeps its RTXDI/denoiser history (remixplus_api.cpp
    // header), so the lamp is one light for its whole life. 1.5.2 resets that history on any re-create and the
    // bridge keeps a handle per create until DestroyLight, so there it is destroyed first (KB headlights.md section 5).
    if (!rbrrtx_remix_plus()) destroy_lamp(l);
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
    const remixapi_LightHandle old = l.h;
    const remixapi_ErrorCode err = g_api->CreateLight(&info, &l.h);
    if (err != REMIXAPI_ERROR_CODE_SUCCESS) {
      l.h = old;
      destroy_lamp(l);                                 // a failed in-place update must not leave the old lamp lit
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
  l.sent = g_frameNo;
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
// A new world (stage, replay) keeps the game's lamps off for its first kLampHoldCarFrames frames with car draws, then
// switches them on: litparts.cpp learns the lit lenses from that switch (night stages start with the lamps on, and
// "drawn only while on" alone lit the whole car, run 20261030-055723). Our Remix lights are not held.
constexpr unsigned kLampHoldCarFrames = 30;
void* g_holdWorld = nullptr;
unsigned g_worldCarFrames = 0;
// Main menu car (user 2026-10-30: "the car in the main menu has lights when nighttime falls"). Under NUMOS the menu's sun
// runs forward from a stage's time (comp.cpp numos_menu); below auto_below_sun the lamps go on, placed on the car as drawn (menu_root),
// and the game's own lamp meshes switch with them (so its lit lenses glow, litparts.cpp).
bool g_menuCar = false, g_menuDark = false;
unsigned g_menuRootMissed = 0;

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
// The learning pulse (user 2026-11-09: "if addon learns from on/off maybe send some signal to trigger it at the very
// beginning?"): lit parts are learnt from a clean switch of the game's lamp signal (litparts.cpp), and by day or with the lamps
// left alone none ever came (Clio, Zlin, run 20261109-160217). Only the game's own lit meshes and sprites follow it, the Remix
// lamps stay as the player set them. Off, then on for kPulseOn car frames, then the player's state; again at a later start
// (a camera cut refuses a switch) until the car has lamp parts. Works on every stage: the parts are per car and stage.
constexpr unsigned kPulseAt[3] = { 30, 240, 480 }, kPulseOn = 30, kPulseOff = 30;
int pulsed_lamps(int real) {
  const unsigned f = g_worldCarFrames;
  if (g_track < 0 || !g_cfg.lampPulse) return real == 1 && f < kLampHoldCarFrames ? 0 : real;
  if (f < kPulseAt[0]) return 0;
  for (int a = 0; a < 3; ++a) {
    if (a && rbrrtx_litparts_lamps_done()) break;      // judged: more pulses would only blink the lamps
    if (f + kPulseOff >= kPulseAt[a] && f < kPulseAt[a]) return 0;
    if (f >= kPulseAt[a] && f < kPulseAt[a] + kPulseOn) return 1;
  }
  return real;
}

void sync_game_lamps() {
  if (!g_cfg.gameLamps) return;
  const int real = g_mode != kOff ? 1 : 0;
  int want = pulsed_lamps(real);
  if (g_frameNo % 30 != 0 && g_gameLampsSent == want) return;
  void* world = find_hh_world();
  if (!world) {
    if (g_gameLampsLog < 2 && g_gameLampsSent != -2) { ++g_gameLampsLog; hlog(L"headlights: game lamp switch not found (HedgeHog3D CSigCarLight connection)"); }
    g_gameLampsSent = -2;
    return;
  }
  if (world != g_holdWorld) { g_holdWorld = world; g_worldCarFrames = 0; want = pulsed_lamps(real); }
  const bool held = want != real;
  if (world == g_hhWorld && want == g_gameLampsSent) return;
  // Headlights (type 1: light_lf, light_rf) and the tail lamps with them (type 3: index + 2 into the handler's suffix
  // table _lf _rf _lb _rb _m, prefix "light": light_lb, light_rb, light_m; HedgeHog3D.dll tables 0x101DA26C /
  // 0x101DA280, read 2026-10-23; type 4 = brake, 5 = reverse are the game's own).
  bool ok = call_car_light(world, 1, 0, want == 1) && call_car_light(world, 1, 1, want == 1);
  for (int i = 0; ok && i < 3; ++i) ok = call_car_light(world, 3, i, want == 1);
  if (g_gameLampsLog < 12) {
    ++g_gameLampsLog;
    hlog(L"headlights: game's own head and tail lamps (lit meshes, glow sprites) %s%s (world %p)%s", want ? L"ON" : L"off",
         ok ? L"" : L" FAILED", world, held ? L", learning pulse (lit parts learn the switch; the Remix lamps do not follow it)" : L"");
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
  for (size_t i = 0; i < kMaxExtra; ++i) g_extraLamps[i].hash = 0x52425258'4C000000ULL + i;   // "RBRXL" + index
  load_settings();
  if (g_cfg.poseLag >= 0.0f) { g_lag = g_cfg.poseLag; g_lagKnown = true; }
}

// The windscreen drops (windscreen.cpp) go on the car as it is drawn in this frame: the model root from the first big
// car draw whose body-node root lies within 0.35 m of where the body is expected (last frame's drawn body moved as RBR's
// object moved since, else the object g_lag back), axes matching (wheels spin, doors swing: not those).
bool g_rootThisFrame = false, g_predThisFrame = false, g_predOk = false;
float g_pred[16];
float g_rootM[16];                                     // this frame's drawn model root (when g_rootThisFrame)

void drawn_root(const float* world) {
  if (!g_predThisFrame) {
    g_predThisFrame = true;
    V3 phys;
    float O[16];
    g_predOk = pred_pose(g_pred);
    if (g_predOk && g_bodyFrames && physics_pose(O, &phys)) {
      std::memcpy(g_pred, g_bodyM, sizeof(g_pred));
      const V3 w = row(g_bodyM, 3) + (phys - g_bodyObj);
      g_pred[12] = w.x; g_pred[13] = w.y; g_pred[14] = w.z;
    }
  }
  if (!g_predOk) return;
  // Only the body frame (run 7: trying every part's frame let a wrong part win by a few cm, shifting the drops).
  float best[16], C[16];
  float bestErr = 0.35f;
  for (const Placed& pl : { g_car.body }) {
    compose(pl, world, C);
    bool axes = true;
    for (int r = 0; r < 3 && axes; ++r) axes = dot(norm(row(C, r)), norm(row(g_pred, r))) > 0.995f;
    const float err = len(row(C, 3) - row(g_pred, 3));
    if (axes && err < bestErr) { bestErr = err; std::memcpy(best, C, sizeof(best)); }
  }
  if (bestErr >= 0.35f) return;
  g_rootThisFrame = true;
  std::memcpy(g_rootM, best, sizeof(g_rootM));
  rbrrtx_windscreen_car_drawn(best);
}

// ---------------------------------------------------------------- cockpit windscreen
struct WMat { float m[16]; bool blended; };

V3 xf_point(const Placed& a, V3 p) {                   // p * A + A[3] (row vector)
  return { p.x * a.A[0][0] + p.y * a.A[1][0] + p.z * a.A[2][0] + a.A[3][0],
           p.x * a.A[0][1] + p.y * a.A[1][1] + p.z * a.A[2][1] + a.A[3][1],
           p.x * a.A[0][2] + p.y * a.A[1][2] + p.z * a.A[2][2] + a.A[3][2] };
}

// World -> model frame for a model root M (rows x, y, z, position; rows may be scaled).
bool model_of(const float* M, Placed* inv) {
  float R[3][3];
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) R[r][c] = M[c * 4 + r];
  return placed_of(R, row(M, 3), inv);
}

// Cockpit or not from where the camera is (user 2026-10-24 run 9: on Rosciszow the Civic's cockpit counted as an
// outside view: its interior is drawn in the body's frame with enough triangles for the body match, so the drops took
// RBR's object in place of the camera and the dust showed in the cabin). Inside = within the car's width, between
// its lamps and its rear, under the glass top, behind the windscreen plane. Bonnet and roof cameras are outside.
float g_camW[3] = {};
unsigned g_camFrameNo = 0, g_insideFrame = ~0u;
bool g_camSet = false, g_insideCached = false;

bool windscreen_plane(V3* b, V3* t, V3* n) {           // the model's glass, else the lamp plane
  if (g_car.glass) { *b = g_car.glassBottom; *t = g_car.glassTop; *n = g_car.glassN; return true; }
  if (!g_car.lamps) return false;
  const V3 mid = (g_car.lf + g_car.rf) * 0.5f, F = g_car.forward;
  *b = mid - F * 1.15f + V3{ 0, 0.42f, 0 };
  *t = mid - F * 1.83f + V3{ 0, 0.92f, 0 };
  *n = norm(V3{ 0, 0.68f, 0 } + F * 0.5f);
  return true;
}

bool camera_inside() {
  if (g_insideFrame == g_frameNo) return g_insideCached;
  g_insideFrame = g_frameNo;
  g_insideCached = false;
  if (!g_camSet || g_camFrameNo + 2 < g_frameNo || !g_car.lamps || g_track < 0 || !g_off.locked) return false;
  float O[16];
  V3 phys, b, t, n;
  Placed inv;
  if (!physics_pose(O, &phys) || !model_of(O, &inv) || !windscreen_plane(&b, &t, &n)) return false;
  const V3 p = xf_point(inv, { g_camW[0], g_camW[1], g_camW[2] });
  const V3 mid = (g_car.lf + g_car.rf) * 0.5f, F = g_car.forward;
  const float along = dot(p - mid, F);
  if (dot(n, F) < 0.0f) n = n * -1.0f;
  g_insideCached = std::fabs(p.x - mid.x) < 0.85f && along < -0.3f && along > -3.6f && p.y > mid.y - 0.3f &&
                   p.y < (std::max)(b.y, t.y) + 0.2f && dot(p - (b + t) * 0.5f, n) < 0.0f;
  return g_insideCached;
}

float g_camM[16] = {};                                 // this frame's camera to world (rows x, y, z, position)

void rbrrtx_headlights_camera(const float camToWorld[16]) {
  g_camW[0] = camToWorld[12]; g_camW[1] = camToWorld[13]; g_camW[2] = camToWorld[14];
  std::memcpy(g_camM, camToWorld, sizeof(g_camM));
  g_camFrameNo = g_frameNo;
  g_camSet = true;
}

// Cockpit windscreen (run 12, user 2026-10-24: "statically mounted, match the windshield", "different people have
// different camera settings"; run 11 anchors jumped: the interior parts' node chains do not place the drawn interior,
// E30 0.45 m off). RBR draws the windscreen seen from the cockpit with CarGlassInternal.vso. Its world matrix this
// frame x the glass mesh in its node frame (i_window_f, else e_window_f) is the glass on screen, whatever the camera
// or the node chains: no camera link, nothing measured once. Of this frame's CarGlassInternal draws x both meshes,
// the one that puts a windscreen in front of the camera (0.2-2 m ahead, within 0.9 m sideways, -1..0.8 m up, 0.5-2.2 m wide,
// facing it), nearest the view's middle and the last frame's, is taken.
std::vector<WMat> g_glassW;                            // this frame's CarGlassInternal draws
struct CockpitGlass { V3 b, t, n; float wb, wt; V3 camRel; int mesh; } g_cg = {};
unsigned g_cgFrame = ~0u, g_cgFound = 0, g_cgMissed = 0;
bool g_cgLogged = false;

// Run 14 (log 21:37-21:46): k_windscreen through its CarGlassInternal matrix lands 0.97 m under the eye: the quad is a
// flat overlay RBR places ~2 m ahead with its own record matrix (openRBRVR drops it in VR), not the glass. But that
// draw's matrix is the car as drawn in the cockpit: its origin sat where the car root was measured from the camera
// (0.30 right, 0.97 down, 0.25 ahead vs 0.30 / 0.92 / 0.33). So the drops go on the car's glass plane (model frame)
// through this matrix: the drawn car, every frame, fixed to it. Taken: a CarGlassInternal matrix whose axes match RBR's
// car object and whose origin lies within 2 m of the camera; nearest the last one.
float g_cockpitRoot[16];
void cockpit_glass_find() {
  const size_t draws = g_glassW.size();
  if (g_camFrameNo != g_frameNo || !camera_inside() || !draws) { g_glassW.clear(); return; }
  float P[16];
  V3 phys;
  if (!physics_pose(P, &phys)) { g_glassW.clear(); return; }
  const V3 cam = row(g_camM, 3);
  const bool prev = g_cgFrame != ~0u && g_cgFrame + 2 >= g_frameNo;
  float bestScore = 1e9f;
  const WMat* best = nullptr;
  for (const WMat& w : g_glassW) {
    bool axes = true;
    for (int r = 0; r < 3 && axes; ++r) axes = dot(norm(row(w.m, r)), norm(row(P, r))) > 0.95f;
    const float d = len(row(w.m, 3) - cam);
    if (!axes || d > 2.0f) continue;
    const float score = d + (prev ? 3.0f * len(row(w.m, 3) - row(g_cockpitRoot, 3)) : 0.0f);
    if (score < bestScore) { bestScore = score; best = &w; }
  }
  g_glassW.clear();
  if (!best) {
    if (++g_cgMissed == 120) hlog(L"headlights: cockpit: %zu CarGlassInternal draws, none with the car's axes near the camera", draws);
    return;
  }
  std::memcpy(g_cockpitRoot, best->m, sizeof(g_cockpitRoot));
  g_cgFrame = g_frameNo;
  if (++g_cgFound == 1) {
    const V3 o = row(best->m, 3) - cam;
    hlog(L"headlights: cockpit: car placed by the CarGlassInternal draw, %.2f m from the camera; drops on the car's glass", len(o));
  }
}

// The car as drawn from the cockpit in the frame that just ended (rows x, y, z, position), for windscreen.cpp.
bool rbrrtx_cockpit_root(float M[16]) {
  if (g_cgFrame == ~0u || g_cgFrame + 1 != g_frameNo) return false;
  std::memcpy(M, g_cockpitRoot, sizeof(g_cockpitRoot));
  return true;
}

void rbrrtx_cockpit_glass_stats(unsigned* found, unsigned* missed) { *found = g_cgFound; *missed = g_cgMissed; }

bool rbrrtx_camera_in_cabin() { return camera_inside(); }

void rbrrtx_headlights_car_draw(const float* world, unsigned prims, bool blended, bool cockpitGlass) {
  (void)cockpitGlass;                                    // the cockpit glass search fed only the proxy's own windscreen drops (off for good, 2026-11-11)
  if (!g_rootThisFrame && !blended && prims >= 200 && g_track >= 0 && g_off.locked) drawn_root(world);
  // Collected while the lights are on, and on a stage until the model offset is measured, lights off too: the
  // game shows the car in a 3rd-person view whenever a stage loads, where several car parts agree (user 2026-10-23;
  // the cockpit view often shows just one, and the lamps then moved once, ~0.3 m, when the view changed).
  // On a stage always: the offset (until measured) and the drawn body (match_body) for every lamp, brake and backfire
  // lights included.
  if (!g_cfg.enabled || (g_track < 0 && !g_menuCar) || blended || g_groups.size() >= 96) return;   // blended: ghost car, glass
  for (Group& g : g_groups) if (same_matrix(g.m, world)) { g.prims += prims; return; }
  Group g;
  std::memcpy(g.m, world, sizeof(g.m));
  g.prims = prims;
  g_groups.push_back(g);
}

// Every lamp lit this frame at car pose C (model root -> world); lamps not lit this frame are destroyed: Remix Plus
// draws a created light every frame until it is destroyed (persistent auto-instancing, RemixApi-Migration.md
// "Lights"), so a brake lamp only left undrawn stayed lit where it was last placed (2026-10-30; the same for the
// backfire flash and the reverse lamp). On 1.5.2 an undrawn light is dark anyway; destroying it frees its bridge handle.
float g_lastRoot[16] = {};                              // the car's root pose at the last send_lamps
bool g_haveRoot = false;
void send_lamps(const float C[16], const wchar_t* src) {
  g_sentThisFrame = true;
  g_poseSrc = src;
  std::memcpy(g_lastRoot, C, sizeof(g_lastRoot));
  g_haveRoot = true;
  const V3 lf = g_car.lamps ? g_car.lf : g_cfg.fallback;
  const V3 rf = g_car.lamps ? g_car.rf : V3{ -g_cfg.fallback.x, g_cfg.fallback.y, g_cfg.fallback.z };
  // One head lamp offset for every view (forward, in front of the lens): with the lamps on the drawn car the car no
  // longer blocks them (2026-10-23 they sat up to 1.4 m inside the drawn body while moving, which needed 0.5-1.5 m).
  // Each lamp clears its own glass (beam_ahead), or the lens catches the whole beam at a few centimetres (2026-11-09).
  const V3 wl = to_world(C, lf + beam_ahead(lf, g_cfg.forward)), wr = to_world(C, rf + beam_ahead(rf, g_cfg.forward));
  const V3 fwd = norm(row(C, 2) * g_car.forward.z);
  const V3 up = norm(row(C, 1));
  auto aim = [&](float pitchDeg) { const float pr = pitchDeg * 0.0174533f; return norm(fwd * std::cos(pr) - up * std::sin(pr)); };
  if ((g_tailOn || g_braking || g_reversing) && g_car.rear) {         // rear: aimed straight back and rearPitch down
    auto back = [&](const V3& node) { return beam_behind(node, g_cfg.rearBack); };
    const float pr = g_cfg.rearPitch * 0.0174533f;
    const V3 rdir = norm(fwd * -std::cos(pr) - up * std::sin(pr));
    const float* rad = g_braking ? g_cfg.brakeRadiance : g_cfg.tailRadiance;
    if (g_tailOn || g_braking) {
      draw_lamp(g_lamps[5], to_world(C, g_car.lb + back(g_car.lb)), rdir, g_cfg.rearCone, rad, g_cfg.rearSoftness);
      draw_lamp(g_lamps[6], to_world(C, g_car.rb + back(g_car.rb)), rdir, g_cfg.rearCone, rad, g_cfg.rearSoftness);
    }
    if (g_reversing) {                                 // two white reversing lamps, as the game lights both lenses
      draw_lamp(g_lamps[8], to_world(C, g_car.revl + back(g_car.revl)), rdir, g_cfg.rearCone, g_cfg.reverseRadiance, g_cfg.rearSoftness);
      draw_lamp(g_lamps[9], to_world(C, g_car.revr + back(g_car.revr)), rdir, g_cfg.rearCone, g_cfg.reverseRadiance, g_cfg.rearSoftness);
    }
  }
  if (g_popOn && g_car.exhaust)                        // backfire: a point light just beyond the pipe tip
    draw_lamp(g_lamps[7], to_world(C, g_car.exhaustTip + g_car.exhaustDir * g_cfg.popOut), up, 180.0f, g_cfg.popRadiance,
              0.0f, g_cfg.popRadius);
  if (g_mode != kOff) {
    const V3 dir = aim(g_cfg.pitch);
    draw_lamp(g_lamps[1], wl, dir, g_cfg.cone, g_cfg.radiance, g_cfg.softness);
    draw_lamp(g_lamps[2], wr, dir, g_cfg.cone, g_cfg.radiance, g_cfg.softness);
    if (g_mode == kBrights || g_cfg.quadLamps) {       // high beams: each lamp node moved toward the car's middle
      const float in = g_cfg.brightsInset;
      const V3 lfb = lf + V3{ lf.x > rf.x ? -in : in, 0, 0 }, rfb = rf + V3{ lf.x > rf.x ? in : -in, 0, 0 };
      const V3 bl = to_world(C, lfb + beam_ahead(lfb, g_cfg.forward + g_cfg.brightsForward));
      const V3 br = to_world(C, rfb + beam_ahead(rfb, g_cfg.forward + g_cfg.brightsForward));
      const bool high = g_mode == kBrights;            // quad_lamps: the inner pair is a low beam until brights
      const V3 bdir = aim(high ? g_cfg.brightsPitch : g_cfg.pitch);
      draw_lamp(g_lamps[3], bl, bdir, high ? g_cfg.brightsCone : g_cfg.cone, high ? g_cfg.brightsRadiance : g_cfg.radiance,
                high ? g_cfg.brightsSoftness : g_cfg.softness);
      draw_lamp(g_lamps[4], br, bdir, high ? g_cfg.brightsCone : g_cfg.cone, high ? g_cfg.brightsRadiance : g_cfg.radiance,
                high ? g_cfg.brightsSoftness : g_cfg.softness);
    }
  }
  if (g_cfg.keepLampsAlive && g_track >= 0) {         // user 2026-11-08: the scenery flickered with every L press; the light count stays the same now
    static const float kNoLight[3] = { 0.0f, 0.0f, 0.0f };
    const V3 dir = aim(g_cfg.pitch);
    if (g_mode == kOff) {
      draw_lamp(g_lamps[1], wl, dir, g_cfg.cone, kNoLight, g_cfg.softness);
      draw_lamp(g_lamps[2], wr, dir, g_cfg.cone, kNoLight, g_cfg.softness);
    }
    if (g_mode == kOff || (g_mode == kPair && !g_cfg.quadLamps)) {
      draw_lamp(g_lamps[3], wl, dir, g_cfg.cone, kNoLight, g_cfg.softness);
      draw_lamp(g_lamps[4], wr, dir, g_cfg.cone, kNoLight, g_cfg.softness);
    }
  }
  if (g_cfg.extra && g_mode != kOff) {                // extra lamps of the headlight group
    const V3 edir = aim(g_cfg.brightsPitch), wdir = aim(g_cfg.pitch);
    for (size_t i = 0; i < g_car.extras.size(); ++i) {
      const CarModel::Extra& e = g_car.extras[i];
      if (e.brake || (!e.weak && g_cfg.extra == 2 && g_mode != kBrights)) continue;
      const float k = g_cfg.extraIntensity * (e.weak ? g_cfg.extraWeak : 1.0f);
      const float rad[3] = { g_cfg.brightsRadiance[0] * k * e.rgb[0], g_cfg.brightsRadiance[1] * k * e.rgb[1],
                             g_cfg.brightsRadiance[2] * k * e.rgb[2] };
      draw_lamp(g_extraLamps[i], to_world(C, e.pos + beam_ahead(e.pos, g_cfg.forward)), e.weak ? wdir : edir, e.weak ? g_cfg.cone : g_cfg.extraCone, rad,
                e.weak ? g_cfg.softness : g_cfg.brightsSoftness);
    }
  }
  if (g_cfg.extra && g_braking) {                      // extra brake lamps (centre and high brake lights)
    const float pr = g_cfg.rearPitch * 0.0174533f;
    const V3 rdir = norm(fwd * -std::cos(pr) - up * std::sin(pr));
    for (size_t i = 0; i < g_car.extras.size(); ++i) {
      const CarModel::Extra& e = g_car.extras[i];
      if (!e.brake) continue;
      const float rad[3] = { g_cfg.brakeRadiance[0] * e.rgb[0], g_cfg.brakeRadiance[1] * e.rgb[1], g_cfg.brakeRadiance[2] * e.rgb[2] };
      draw_lamp(g_extraLamps[i], to_world(C, e.pos + beam_behind(e.pos, g_cfg.rearBack)), rdir, g_cfg.rearCone, rad, g_cfg.rearSoftness);
    }
  }
  for (Lamp& l : g_lamps) if (l.h && l.sent != g_frameNo) destroy_lamp(l);
}

// Per-frame pose log (headlights.ini pose_log = 1): <logs>\rbrrtx_lamp_pose.csv, one line per frame at the refine
// point. Columns: time, frame, game mode (1 driving, 2 pause, 8 replay, 10 pre-stage), speed, object root, predicted
// root (object g_lag back), drawn body root (empty if none), lag, lag residual, source of the lamps, lamps lit, body
// match: candidates, eligible, pick's triangles, pick's and runner-up's distance from the expected root.
void pose_log(const float* body) {
  if (!g_cfg.poseLog) return;
  if (!g_poseCsv) {
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"RBRRTX_LOG_DIR", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { g_cfg.poseLog = false; return; }
    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, L"%s\\rbrrtx_lamp_pose.csv", dir);
    if (_wfopen_s(&g_poseCsv, path, L"w") || !g_poseCsv) { g_cfg.poseLog = false; return; }
    fprintf(g_poseCsv, "t,frame,mode,speed,obj_x,obj_y,obj_z,pred_x,pred_y,pred_z,body_x,body_y,body_z,lag_ms,lag_resid,source,lit,cands,eligible,prims,d_best,d_second\n");
    hlog(L"headlights: pose log %s", path);
  }
  float O[16], P[16];
  V3 phys;
  if (!physics_pose(O, &phys) || !pred_pose(P)) return;
  RbrStageInfo si;
  const int mode = rbrrtx_stage_info(&si) ? si.gameMode : -1;
  char b[96] = "";
  if (body) sprintf_s(b, "%.4f,%.4f,%.4f", body[12], body[13], body[14]); else strcpy_s(b, ",,");
  fprintf(g_poseCsv, "%.4f,%u,%d,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%s,%.1f,%.3f,%ls,%d,%d,%d,%u,%.3f,%.3f\n", g_frameT, g_frameNo,
          mode, hist_speed(), O[12], O[13], O[14], P[12], P[13], P[14], b, g_lag * 1000.0f, g_lagResid, g_poseSrc,
          g_lampsLit ? 1 : 0, g_matchCands, g_matchEligible, g_matchPrims, g_matchD, g_matchD2);
  if (g_frameNo % 120 == 0) fflush(g_poseCsv);
}

void rbrrtx_headlights_emit() {
  if (g_emitted || !g_api || !g_cfg.enabled || g_track < 0) return;
  g_emitted = true;
  g_frameT = now_s();
  hist_push(g_frameT);
  // Brake lights: on above brake_on, off below half of it (no flicker from a pedal resting at the threshold).
  float brake = 0.0f;
  const bool brakeRead = rbrrtx_player_car_brake(&brake);
  const bool braking = g_cfg.brakeLights && brakeRead && brake > (g_braking ? g_cfg.brakeOn * 0.5f : g_cfg.brakeOn);
  if (braking != g_braking && ++g_brakeSwitches <= 6)
    hlog(L"headlights: brake lights %s (brake input %.2f; lamps from %s, body frames %u, held %u)", braking ? L"ON" : L"off", brake,
         g_poseSrc, g_bodyFrames, g_bodyHeld);
  g_braking = braking;
  int gear = 1;                                        // reverse: two white lamps at Point_light_reverse_lb/rb
  const bool reversing = g_cfg.reverseLight && rbrrtx_player_car_gear(&gear) && gear == 0;
  if (reversing != g_reversing && ++g_reverseSwitches <= 3)
    hlog(L"headlights: reverse light %s (gear %d)", reversing ? L"ON" : L"off", gear);
  g_reversing = reversing;
  {                                                    // game particles are off for good: the g_pop sprite is never drawn (0 flashes in 21 runs, 2026-11-01)
    static unsigned noPop = 0;
    static bool noted = false;
    if (g_cfg.pop && !g_popSeen && !noted && ++noPop == 3600) {
      noted = true;
      hlog(L"headlights: backfire: no flame sprite (particles off), the flash light needs a draw of the g_pop sprite; nothing in %u headlight updates", noPop);
    }
  }
  const bool pop = g_cfg.pop && g_popSeen &&
                   std::chrono::duration<float>(std::chrono::steady_clock::now() - g_lastPop).count() <= g_cfg.popHold;
  if (pop && !g_popActive && ++g_popFlashes <= 3) hlog(L"headlights: backfire flash (%u pop draws so far)", g_popDraws);
  g_popActive = pop;
  g_popOn = pop;
  g_tailOn = g_cfg.rear && g_mode != kOff;
  g_lampsLit = g_mode != kOff || braking || pop || reversing || g_cfg.keepLampsAlive;   // kept lamps are sent at zero strength
  if (!g_lampsLit && !g_cfg.keepLampsAlive) { destroy_all(); return; }
  g_lampFrame = g_frameNo;
  // Remix Plus with the body drawn last frame: wait for this frame's body draw (rbrrtx_headlights_refine, before Remix
  // takes the lights); the lamps stay where last frame's body put them until then. Else send the prediction now.
  if (rbrrtx_remix_plus() && (g_bodyFrames > 0 || (g_bodyHeld && g_bodyHeld < kHoldFrames))) return;
  float C[16];
  if (!place_frame(C)) {
    if (++g_failed <= 5 || g_failed % 600 == 0)
      hlog(L"headlights: car NOT placed this frame (RBR's car object not readable); placed %u, not placed %u frames", g_solved, g_failed);
    return;
  }
  if (++g_solved <= 2 || g_solved % 3600 == 0)
    hlog(L"headlights: car placed from RBR's car object %.0f ms back (model offset %s); placed %u, not placed %u frames",
         g_lag * 1000.0f, g_off.locked ? L"measured" : L"not measured yet, 0", g_solved, g_failed);
  send_lamps(C, L"object");
}

// Before Remix takes this frame's lights (comp.cpp: right before our UI trigger draw, else at Present), once per frame:
// find this frame's drawn body among the car draws so far; it measures the lag, and on Remix Plus the lamps are sent
// again from it (an in-place update of each live light). Views without a body draw: the prediction, if not sent yet.
void rbrrtx_headlights_refine() {
  if (g_refined || !g_emitted || g_track < 0) return;
  g_refined = true;
  float B[16];
  const bool body = !g_car.file.empty() && match_body(B);
  if (body) {
    V3 phys;
    float O[16];
    if (physics_pose(O, &phys)) {
      g_bodyGap = len(row(B, 3) - row(O, 3));
      g_bodyGapMax = (std::max)(g_bodyGapMax, g_bodyGap);
      g_bodyObj = phys;
    }
    std::memcpy(g_bodyM, B, sizeof(g_bodyM));
    lag_sample(B);
    ++g_bodyFrames;
    g_bodyHeld = 0;
    if (++g_bodyMatched == 1 || g_bodyMatched % 3600 == 0)
      hlog(L"headlights: car body drawn %.2f m from RBR's car object; lamps on the drawn body (%u frames matched, %u not)",
           g_bodyGap, g_bodyMatched, g_bodyMissed);
  } else {
    if (g_bodyFrames || g_bodyHeld) ++g_bodyHeld;
    if (g_bodyFrames >= 3 && ++g_bodyMissed <= 3)
      hlog(L"headlights: car body not drawn this frame (after %u frames with it): lamps from RBR's car object %.0f ms back",
           g_bodyFrames, g_lag * 1000.0f);
    g_bodyFrames = 0;
  }
  if (g_lampsLit && g_api && rbrrtx_remix_plus()) {
    float H[16], Q[16];
    V3 phys;
    if (body) send_lamps(B, L"body");
    // A frame without the body right after frames with it (2026-10-30 runs: 1 in 25 frames while driving): the last
    // body moved as the object moved since, turned as the object turned. Switching to the prediction for one frame
    // made the lamps jump by its error (0.2-0.8 m). Up to kHoldFrames, then the prediction (cockpit view).
    else if (g_bodyHeld && g_bodyHeld <= kHoldFrames && physics_pose(Q, &phys) && pred_pose(H)) {
      const V3 o = row(g_bodyM, 3) + (phys - g_bodyObj);
      H[12] = o.x; H[13] = o.y; H[14] = o.z;
      send_lamps(H, L"held");
    } else if (!g_sentThisFrame) {
      float C[16];
      if (place_frame(C)) send_lamps(C, L"object");
    }
  }
  pose_log(body ? B : nullptr);
}

// The car body drawn in recent frames (outside views: chase, TV, 3rd person) = 1, not drawn (cockpit views) = 0,
// -1 = not known yet (car model offset not measured). The dust uses it too (particles.cpp, user 2026-10-23).
int rbrrtx_car_body_visible() {
  if (g_track < 0 || !g_off.locked) return -1;
  if (camera_inside()) return 0;                       // in the cabin, whatever the car draws say
  return g_bodyFrames >= 3 ? 1 : 0;
}

// The car's model root in render space as the lamps use it without a body draw (rows: model x, y = up, z, row 3 =
// position; RBR's pose g_lag back + the measured model offset), the head lamp nodes and the model's forward axis in that frame. For the
// windscreen drops (windscreen.cpp). False while the model or its offset is not known.
bool rbrrtx_car_model_frame(float M[16], float lf[3], float rf[3], float fwd[3]) {
  if (g_track < 0 || !g_car.lamps || !g_off.locked) return false;
  if (!pred_pose(M)) return false;
  lf[0] = g_car.lf.x; lf[1] = g_car.lf.y; lf[2] = g_car.lf.z;
  rf[0] = g_car.rf.x; rf[1] = g_car.rf.y; rf[2] = g_car.rf.z;
  fwd[0] = g_car.forward.x; fwd[1] = g_car.forward.y; fwd[2] = g_car.forward.z;
  return true;
}

// The model root from RBR's car object + the measured offset only (no drift: the drift is only refreshed while the
// body is drawn, so in the cockpit it is stale). For the cockpit windscreen anchor, measured while the car stands.
bool rbrrtx_car_object_frame(float M[16]) {
  if (g_track < 0 || !g_off.locked) return false;
  V3 phys;
  return physics_pose(M, &phys);
}

// The windscreen glass plane in the model frame (load_windscreen: interior glass first); false if not usable.
bool rbrrtx_car_windscreen_glass(float bottom[3], float top[3], float n[3], float* widthBottom, float* widthTop) {
  if (!g_car.glass) return false;
  bottom[0] = g_car.glassBottom.x; bottom[1] = g_car.glassBottom.y; bottom[2] = g_car.glassBottom.z;
  top[0] = g_car.glassTop.x; top[1] = g_car.glassTop.y; top[2] = g_car.glassTop.z;
  n[0] = g_car.glassN.x; n[1] = g_car.glassN.y; n[2] = g_car.glassN.z;
  *widthBottom = g_car.glassWb;
  *widthTop = g_car.glassWt;
  return true;
}

bool rbrrtx_car_cockpit_quad(float c[4][3]) { return cockpit_quad(c); }

int rbrrtx_car_windscreen_source() { return g_car.glass ? (g_car.glassName == "i_window_f" ? 2 : 1) : 0; }

// The car's driving camera field of view (Cars\<car>\<model>_personal.ini, else <model>.ini, [Cam_internal] FOV,
// radians); 0 if not found. windscreen.cpp draws the drops in that camera only (run 11, user).
float rbrrtx_car_internal_fov() {
  static std::wstring forFile;
  static float fov = 0.0f;
  if (forFile != g_car.file) {
    forFile = g_car.file;
    fov = 0.0f;
    const size_t dot = g_car.file.find_last_of(L'.');
    const std::wstring base = dot == std::wstring::npos ? g_car.file : g_car.file.substr(0, dot);
    // The player's setting first (run 17, user): NGPCarMenu's cockpit FOV override (Plugins\NGPCarMenu.ini,
    // CockpitOverrideFOV = 1, CockpitOverrideFOVValue in radians) replaces every car's [Cam_internal] FOV; the test
    // install has 0.85, which is why the Mini (ini 1.1) was drawn at 36.5 deg like the rest.
    const size_t carsDir = base.rfind(L"\\Cars\\");
    const std::wstring ngp = (carsDir == std::wstring::npos ? std::wstring() : base.substr(0, carsDir)) + L"\\Plugins\\NGPCarMenu.ini";
    const wchar_t* from = L"[Cam_internal]";
    wchar_t v[64] = {};
    if (GetPrivateProfileIntW(L"Default", L"CockpitOverrideFOV", 0, ngp.c_str()) == 1 &&
        GetPrivateProfileStringW(L"Default", L"CockpitOverrideFOVValue", L"", v, 64, ngp.c_str()) && (fov = static_cast<float>(_wtof(v))) > 0.0f)
      from = L"NGPCarMenu CockpitOverrideFOVValue";
    else
      for (const wchar_t* sfx : { L"_personal.ini", L".ini" })
        if (GetPrivateProfileStringW(L"Cam_internal", L"FOV", L"", v, 64, (base + sfx).c_str()) && (fov = static_cast<float>(_wtof(v))) > 0.0f)
          break;
    hlog(L"headlights: driving camera FOV %.3f rad (%s; car %s)", fov, from, base.c_str());
  }
  return fov;
}

const wchar_t* rbrrtx_car_model_name() {               // the model file's name without folder and extension
  static std::wstring name;
  const size_t slash = g_car.file.find_last_of(L"\\/"), dot = g_car.file.find_last_of(L'.');
  name = g_car.file.substr(slash == std::wstring::npos ? 0 : slash + 1,
                           dot == std::wstring::npos || dot < slash ? std::wstring::npos : dot - (slash == std::wstring::npos ? 0 : slash + 1));
  return name.c_str();
}

// The menu car's model root from its draws: every draw group (>= 200 triangles), for every drawn part of the model it
// could be, gives a root; the root most groups agree on (within 1 mm) wins, two groups at least (as measure_frame).
bool menu_root(const std::vector<Group>& groups, float out[16]) {
  struct Cand { V3 p; int group; float C[16]; };
  std::vector<Cand> c;
  for (int j = 0; j < static_cast<int>(groups.size()); ++j) {
    if (groups[j].prims < 200) continue;
    for (const Placed& pl : g_car.parts) {
      Cand k;
      compose(pl, groups[j].m, k.C);
      k.p = row(k.C, 3);
      k.group = j;
      c.push_back(k);
    }
  }
  // Correctly placed parts give the same root to float precision; models with node frames a centimetre or two apart
  // (124 Abarth, i20 R5) made 2 cm bins pick a neighbour. So: within 1 mm, swept along x.
  std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.p.x < b.p.x; });
  int bestGroups = 0;
  unsigned bestPrims = 0;
  const Cand* best = nullptr;
  std::vector<int> seen;
  for (size_t i = 0; i < c.size(); ++i) {
    seen.clear();
    unsigned prims = 0;
    size_t a = i;
    while (a > 0 && c[i].p.x - c[a - 1].p.x < 0.001f) --a;
    for (size_t k = a; k < c.size() && c[k].p.x - c[i].p.x < 0.001f; ++k) {
      bool same = len(c[k].p - c[i].p) < 0.001f;     // and the same axes (impreza03: a rotated node at the root's origin)
      for (int r = 0; r < 3 && same; ++r) same = len(row(c[k].C, r) - row(c[i].C, r)) < 0.001f * (1.0f + len(row(c[i].C, r)));
      if (!same || std::find(seen.begin(), seen.end(), c[k].group) != seen.end()) continue;
      seen.push_back(c[k].group);
      prims += groups[c[k].group].prims;
    }
    const int n = static_cast<int>(seen.size());
    if (n > bestGroups || (n == bestGroups && prims > bestPrims)) { bestGroups = n; bestPrims = prims; best = &c[i]; }
  }
  if (bestGroups < 2 || !best) return false;
  std::memcpy(out, best->C, sizeof(best->C));
  // The body as drawn, as on a stage (match_body): draws with one matrix are one group, so the body (one matrix for
  // ~20 meshes) is one vote against every wheel and suspension part, and those sit at the model's rest pose. The menu
  // car's body rides higher than that: the lamps sat low on the Yaris and the Fabia (user 2026-10-30). So: the biggest
  // group whose body-node root is near the vote's root with the same axes.
  unsigned bodyPrims = 0;
  for (const Group& g : groups) {
    if (g.prims < 200 || g.prims <= bodyPrims) continue;
    float C[16];
    compose(g_car.body, g.m, C);
    bool close = len(row(C, 3) - row(best->C, 3)) < 0.3f;
    for (int r = 0; r < 3 && close; ++r) close = dot(norm(row(C, r)), norm(row(best->C, r))) > 0.985f;
    if (!close) continue;
    bodyPrims = g.prims;
    std::memcpy(out, C, sizeof(C));
  }
  return true;
}

void menu_lamps(const RbrStageInfo& si, float sunEl, const std::vector<Group>& groups) {
  if (!g_menuCar) {
    g_menuCar = true;
    g_menuDark = false;
    g_menuRootMissed = 0;
    g_gameLampsSent = -1;
    g_hhWorld = nullptr;
    if (g_mode != kOff) set_mode(kOff, L"menu");
  }
  const std::wstring model = car_model_path(si.carId);
  if (model.empty()) return;
  if (model != g_car.file) {
    const size_t slash = model.find_last_of(L"\\/"), dot = model.find_last_of(L'.');
    g_carSection = model.substr(slash + 1, dot != std::wstring::npos && dot > slash ? dot - slash - 1 : std::wstring::npos);
    g_carSlot = si.carId;
    load_settings();
    load_car_model(model);
    hlog(L"headlights: menu car %s (lamps on below sun %.1f deg)", model.c_str(), g_cfg.autoBelow);
  }
  {
    const float t = std::clamp((sunEl - g_cfg.dayFade[0]) / (g_cfg.dayFade[1] - g_cfg.dayFade[0]), 0.0f, 1.0f);
    g_dayK = std::round((1.0f + (g_cfg.dayIntensity - 1.0f) * t) * 50.0f) / 50.0f;
  }
  // The parked menu car: no day running lights, its lamps on below auto_below_sun (brights below brights_below_sun).
  const bool dark = sunEl < g_cfg.autoBelow;
  const int want = !dark ? kOff : auto_mode_for(sunEl);
  if (dark != g_menuDark || want != g_mode) {
    wchar_t why[64];
    swprintf_s(why, L"menu, sun %.1f deg", sunEl);
    set_mode(want, why);
    g_menuDark = dark;
  }
  sync_game_lamps();                                   // the menu car's own lit lenses and glow sprites
  g_braking = g_reversing = g_popOn = false;
  g_tailOn = g_cfg.rear && g_mode != kOff;
  g_lampsLit = g_mode != kOff;
  if (!g_lampsLit) return;
  float C[16];
  if (!menu_root(groups, C)) {                         // not drawn this frame: the lamps stay where they were
    if (++g_menuRootMissed == 1 || g_menuRootMissed % 3600 == 0)
      hlog(L"headlights: menu car not found among this frame's car draws (%zu groups); %u frames so far", groups.size(), g_menuRootMissed);
    for (Lamp& l : g_lamps) if (l.h) { g_api->DrawLightInstance(l.h); l.sent = g_frameNo; }
    return;
  }
  send_lamps(C, L"menu");
}

void rbrrtx_headlights_frame_end(float sunElevation, bool layerOn) {
  cockpit_glass_find();                                  // cockpit: the windscreen as drawn this frame
  ++g_frameNo;
  // headlights.ini saved while driving (F3 > Headlights > Save, or an editor) applies within a second (user 2026-11-07).
  if (g_frameNo % 60 == 0) {
    static FILETIME s_seen = {};
    wchar_t path[MAX_PATH * 2] = {};
    rbrrtx_config_path(L"RBRRTX_HEADLIGHTS", L"headlights.ini", path, MAX_PATH * 2);
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
      const bool first = s_seen.dwLowDateTime == 0 && s_seen.dwHighDateTime == 0;
      if (!first && CompareFileTime(&fa.ftLastWriteTime, &s_seen) != 0) {
        load_settings();
        hlog(L"headlights: headlights.ini changed, settings read again");
      }
      s_seen = fa.ftLastWriteTime;
    }
  }
  g_rootThisFrame = g_predThisFrame = false;            // windscreen drops: placed from the next frame's draws
  const bool keyed = rbrrtx_key_pressed(RBRKEY_HEADLIGHTS);
  // Remix Plus keeps a created light drawn every frame until it is destroyed: a frame that lit the lamps but sent none (no
  // pose, no body, no object) left them hanging where the last frame put them (user 2026-11-02: the lamps "stay where braking
  // started"). They go out instead, and the log says when and which pose source ran last.
  if (g_lampsLit && g_emitted && !g_sentThisFrame && g_track >= 0) {
    bool live = false;
    for (const Lamp& l : g_lamps) live = live || l.h;
    if (live) {
      destroy_all();
      static unsigned s_stale = 0;
      if (++s_stale <= 10 || s_stale % 600 == 0)
        hlog(L"headlights: a frame lit the lamps but sent none (last pose source %s, body frames %u, held %u, braking %d): lamps removed instead of left hanging (%u times)",
             g_poseSrc, g_bodyFrames, g_bodyHeld, g_braking ? 1 : 0, s_stale);
    }
  }
  g_emitted = g_refined = g_sentThisFrame = false;       // lamp states at the next first BeginScene
  if (layerOn && g_track >= 0) measure_frame();          // with every car draw of the frame
  if (!g_groups.empty()) ++g_worldCarFrames;
  std::vector<Group> groups;
  groups.swap(g_groups);
  if (!g_cfg.enabled || !g_api) return;
  RbrStageInfo si;
  const bool stage = in_stage(&si);
  float menuEl = 0.0f;
  if (!stage && layerOn && rbrrtx_menu_sun_elevation(&menuEl)) { menu_lamps(si, menuEl, groups); return; }
  if (g_menuCar) {                                     // out of the menu: its lamps go, a stage decides afresh
    g_menuCar = g_menuDark = false;
    set_mode(kOff, L"left the menu");
    g_gameLampsSent = -1;
    g_hhWorld = nullptr;
  }
  if (!stage || !layerOn) {
    if (g_mode != kOff && !stage) set_mode(kOff, L"left the stage");
    if (!layerOn) destroy_all();
    if (!stage) { g_track = -1; g_manual = false; g_autoWant = -1; g_gameLampsSent = -1; g_hhWorld = nullptr; }
    return;
  }
  if (si.trackId != g_track || si.carId != g_carSlot) {  // new stage or car: read the model, automatic again
    g_track = si.trackId;
    g_carSlot = si.carId;
    g_manual = false;
    g_lampResend = 300;
    g_autoWant = -1;
    g_solved = g_failed = 0;
    g_off = Offset();
    g_haveLastPhys = false;
    g_braking = g_popSeen = g_popActive = false;
    g_histN = 0;                                       // pose history and body tracking start again (the lag stays:
    g_bodyFrames = 0;                                  // it is the game's timing, not the car's)
    g_bodyHeld = 0;
    g_bodyGapMax = 0.0f;
    g_bodyPrimsRef = 0;
    g_gameLampsSent = -1;                            // the stage's world starts with the model ini's states
    g_hhWorld = nullptr;
    g_holdWorld = nullptr;                             // the world object is reused between stages (08E6A728 all of run 20261109-160217): the pulse starts over here
    g_worldCarFrames = 0;
    const std::wstring model = car_model_path(si.carId);
    const size_t slash = model.find_last_of(L"\\/"), dot = model.find_last_of(L'.');
    g_carSection = model.empty() ? L"" : model.substr(slash + 1, dot != std::wstring::npos && dot > slash ? dot - slash - 1 : std::wstring::npos);
    load_settings();                                   // after the car is known: its own section may override
    if (model != g_car.file) load_car_model(model);
    auto known = g_offsetByModel.find(g_car.file);         // measured earlier this session
    if (known != g_offsetByModel.end()) { g_off.d = known->second; g_off.locked = true; }
    if (g_cfg.poseLag >= 0.0f) { g_lag = g_cfg.poseLag; g_lagKnown = true; }
  }
  if (keyed) {
    g_manual = true;
    set_mode(g_mode == kOff ? kPair : g_mode == kPair ? kBrights : kOff, rbrrtx_key_name(RBRKEY_HEADLIGHTS));
  }
  {                                                    // steps of 0.02: no lamp update every frame
    const float t = std::clamp((sunElevation - g_cfg.dayFade[0]) / (g_cfg.dayFade[1] - g_cfg.dayFade[0]), 0.0f, 1.0f);
    g_dayK = std::round((1.0f + (g_cfg.dayIntensity - 1.0f) * t) * 50.0f) / 50.0f;
    // day_intensity dims the automatic day running lights only: lamps switched on with the key are at full strength
    // (user 2026-11-07: with day_intensity 0 the L key did nothing by day).
    if (g_manual) g_dayK = 1.0f;
  }
  if (g_lampResend > 0) --g_lampResend;
  {
    const wchar_t* sky = rbrrtx_sky_name(si.skyType);
    g_badWeather = (wcsstr(sky, L"Rain") || wcsstr(sky, L"Snow") || wcsstr(sky, L"Fog")) && wcsncmp(sky, L"No", 2) != 0;
  }
  const int want = auto_mode_for(sunElevation);
  if (!g_manual && want != g_autoWant) {
    wchar_t why[96];
    if (g_cfg.dayRunning) swprintf_s(why, L"day running lights; sun %.1f deg, brights below %.0f", sunElevation, g_cfg.brightsBelow);
    else swprintf_s(why, L"sun %.1f deg, lamps below %.0f, brights below %.0f%s", sunElevation, g_cfg.autoBelow, g_cfg.brightsBelow,
                    g_badWeather && g_cfg.autoBadWeather ? L", bad weather" : L"");
    set_mode(want, why);
  }
  g_autoWant = want;
  sync_game_lamps();
}

// ---- lamp halo twin: RBR's light.dds (a white disc, alpha 255 in the middle, 0 at the edge; measured from the DXT3 dump of
// 660BCAB491C6BCCA) redrawn with the same middle and a steeper, smaller edge. Same use as the lit-parts twins: bound for
// the draws of that texture only, Remix sees an ordinary texture (no tag of the original hash is needed).
namespace {

IDirect3DTexture9* g_haloTex = nullptr;

float halo_original(float rho) {                       // the game's alpha at radius rho (1 = the texture's edge)
  static const float t[9] = { 1.0f, 1.0f, 0.933f, 0.83f, 0.63f, 0.40f, 0.165f, 0.03f, 0.0f };
  if (rho >= 1.0f) return 0.0f;
  const float x = rho * 8.0f;
  const int i = static_cast<int>(x);
  return t[i] + (t[i + 1] - t[i]) * (x - static_cast<float>(i));
}

float halo_alpha(float rho) {
  const float a = halo_original(rho);
  if (rho <= g_cfg.haloCore) return a;
  const float t = (rho - g_cfg.haloCore) / (1.0f - g_cfg.haloCore);
  if (t >= g_cfg.haloScale) return 0.0f;
  return a * std::pow(1.0f - t / g_cfg.haloScale, g_cfg.haloFalloff);
}

}  // namespace

unsigned long long rbrrtx_headlights_halo_texture() {
  return g_cfg.enabled && g_cfg.halo ? g_cfg.haloTexture : 0;
}

void* rbrrtx_headlights_halo_twin(void* device) {
  if (!g_haloStale) return g_haloTex;                  // (nullptr if the last build failed: not retried until the settings change)
  if (g_haloTex) { g_haloTex->lpVtbl->Release(g_haloTex); g_haloTex = nullptr; }
  g_haloStale = false;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (FAILED(dev->lpVtbl->CreateTexture(dev, 64, 64, 7, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_haloTex, nullptr)) || !g_haloTex) {
    g_haloTex = nullptr;
    return nullptr;
  }
  for (UINT l = 0; l < 7; ++l) {
    D3DLOCKED_RECT lr = {};
    if (FAILED(g_haloTex->lpVtbl->LockRect(g_haloTex, l, &lr, nullptr, 0)) || !lr.pBits) continue;
    const int n = 64 >> l;
    for (int y = 0; y < n; ++y) {
      uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<char*>(lr.pBits) + y * lr.Pitch);
      for (int x = 0; x < n; ++x) {
        const float dx = (x + 0.5f) / n - 0.5f, dy = (y + 0.5f) / n - 0.5f;
        const float a = std::clamp(halo_alpha(2.0f * std::sqrt(dx * dx + dy * dy)), 0.0f, 1.0f);
        row[x] = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | 0x00FFFAFFu;   // the game's white (255, 250, 255)
      }
    }
    g_haloTex->lpVtbl->UnlockRect(g_haloTex, l);
  }
  hlog(L"headlights: lamp halo twin built (core %.2f, scale %.2f, falloff %.1f)", g_cfg.haloCore, g_cfg.haloScale, g_cfg.haloFalloff);
  return g_haloTex;
}

unsigned long long rbrrtx_headlights_pop_texture() {
  return g_cfg.enabled && g_cfg.pop && g_track >= 0 && g_api ? g_cfg.popTexture : 0;
}

void rbrrtx_headlights_pop_drawn() {
  ++g_popDraws;
  g_popSeen = true;
  g_lastPop = std::chrono::steady_clock::now();
}

// NUMOS's lamp glow in fog is a froxel grid with 32 frames of history reprojected with the camera only: a lamp that
// moves with the car leaves a trail (user 2026-10-29). Lamps sent within the last 90 frames (no flapping with brake
// pulses): the shorter history to ask for, else 0.
int rbrrtx_headlights_history_frames() {
  return g_cfg.historyFrames >= 1.0f && g_lampFrame != 0 && g_frameNo - g_lampFrame < 90 ? static_cast<int>(g_cfg.historyFrames) : 0;
}

bool rbrrtx_headlights_keep_fallback() { return g_mode != kOff && g_cfg.enabled && g_cfg.fallbackOn; }

void rbrrtx_headlights_destroy() {
  destroy_all();
  if (g_poseCsv) fflush(g_poseCsv);
}

void rbrrtx_headlights_status(wchar_t* out, size_t n) {
  swprintf_s(out, n, L"headlights: %s%s, model %s (lamps %s, rear %s, exhaust %s), auto below sun %.1f deg, placed %u / not placed %u frames; "
             L"brake lights %s; backfire draws %u, flashes %u; drawn car %.2f m from RBR's object (max %.2f this stage), drawn %.0f ms "
             L"behind it (%s, %d samples, %u refused), body matched %u / missed %u frames",
             mode_name(g_mode), g_manual ? L" (set by key)" : L" (automatic)", g_car.file.empty() ? L"none" : g_car.file.c_str(),
             g_car.lamps ? L"from model" : L"fallback", g_car.rear ? L"yes" : L"no", g_car.exhaust ? L"yes" : L"no",
             g_cfg.autoBelow, g_solved, g_failed, g_braking ? L"ON" : L"off", g_popDraws, g_popFlashes,
             g_bodyGap, g_bodyGapMax, g_lag * 1000.0f, g_cfg.poseLag >= 0.0f ? L"set in headlights.ini" : g_lagKnown ? L"measured" : L"not measured yet",
             g_lagN, g_lagRefused, g_bodyMatched, g_bodyMissed);
}

// Lit lamp textures made emissive (user 2026-10-23: the lamps should look lit inside). Some cars draw their lamps
// with their own lit texture when the lamp is on (BMW M3 E30: lights_lit.dds, Remix 0xAE5889DDDA655C65, shown by
// e_light_*_lit meshes; most other cars use their shared car atlas, which cannot glow per texture). Before Remix
// starts, every Cars\*\*_shaders.ini texture entry named *_lit*.dds is hashed as Remix does (XXH3_64 of mip 0) and
// the generated mod (genmod.cpp, section "lit") gives that texture hash an emissive material whose emission texture is
// the player's own file (absolute path: nothing is copied or shipped). headlights.ini lit_glow = emissive intensity
// (0 = none). Cars whose lamps are lit parts of the shared atlas: litparts.cpp.
// game = game folder with a trailing backslash. Returns the number of textures made emissive.
size_t write_lit_mod_for(const std::wstring& game, float glow) {
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
      {                                                // the unlit sibling: same name without _lit
        std::wstring lowTex = tex;
        for (wchar_t& lc : lowTex) lc = static_cast<wchar_t>(towlower(lc));
        const size_t at = lowTex.rfind(L"_lit");
        if (at != std::wstring::npos) { std::wstring unlit = tex; unlit.erase(at, 4); rbrrtx_litparts_note_lit_atlas(hx, unlit.c_str()); }
      }
      // No material here any more (user 2026-11-09): this one made the whole lamp glow, whatever litparts.cpp decided per part.
    }
  }
  if (h != INVALID_HANDLE_VALUE) FindClose(h);
  rbrrtx_genmod_set("lit", "");
  hlog(L"headlights: %zu lit lamp texture(s) found (their glow: litparts.cpp)", seen.size());
  return seen.size();
}

void rbrrtx_headlights_write_lit_mod() {
  wchar_t path[MAX_PATH * 2] = {};
  rbrrtx_config_path(L"RBRRTX_HEADLIGHTS", L"headlights.ini", path, MAX_PATH * 2);
  const float glow = ini_float(path, L"lit_glow", 3.0f);
  wchar_t exe[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t* slash = (n > 0 && n < MAX_PATH) ? wcsrchr(exe, L'\\') : nullptr;
  if (!slash) return;
  slash[1] = 0;
  write_lit_mod_for(exe, glow);
}

// For litparts.cpp: the game's own lamp switch as last sent (1 on, 0 off), -1 when not switched by us (game_lamps 0,
// switch not found, not on a stage).
int rbrrtx_headlights_on() { return g_mode != kOff ? 1 : 0; }

bool rbrrtx_headlights_has_lens() { return g_car.hasLens; }
bool rbrrtx_headlights_is_lamp_glass(unsigned numV) {
  return std::find(g_car.lampGlassVerts.begin(), g_car.lampGlassVerts.end(), numV) != g_car.lampGlassVerts.end();
}
bool rbrrtx_headlights_is_aux_glass(unsigned numV) {
  return std::find(g_car.auxGlassVerts.begin(), g_car.auxGlassVerts.end(), numV) != g_car.auxGlassVerts.end();
}
bool rbrrtx_headlights_has_aux_glass() { return !g_car.auxGlassVerts.empty(); }

bool rbrrtx_headlights_is_aux(unsigned numV) {
  return std::find(g_car.auxVerts.begin(), g_car.auxVerts.end(), numV) != g_car.auxVerts.end();
}

bool rbrrtx_headlights_aux_on() {                       // as send_lamps: extra_lights = 2 brings them on with the brights only
  return g_cfg.extra && g_mode != kOff && (g_cfg.extra != 2 || g_mode == kBrights);
}

bool rbrrtx_headlights_forward(float out[3]) {
  if (!g_haveRoot) return false;
  const V3 f = norm(row(g_lastRoot, 2) * g_car.forward.z);
  out[0] = f.x; out[1] = f.y; out[2] = f.z;
  return true;
}

bool rbrrtx_headlights_is_brake_lens(unsigned numV) {
  return std::find(g_car.brakeLensVerts.begin(), g_car.brakeLensVerts.end(), numV) != g_car.brakeLensVerts.end();
}

int rbrrtx_headlights_lamp_end(const float worldPos[3]) {
  if (!g_haveRoot || !g_car.lamps || !g_car.rear) return 0;
  const V3 p{ worldPos[0], worldPos[1], worldPos[2] };
  const float front = (std::min)(len(p - to_world(g_lastRoot, g_car.lf)), len(p - to_world(g_lastRoot, g_car.rf)));
  const float rear = (std::min)(len(p - to_world(g_lastRoot, g_car.lb)), len(p - to_world(g_lastRoot, g_car.rb)));
  return front < rear ? 1 : -1;
}

int rbrrtx_headlights_game_lamps() {
  return g_cfg.enabled && g_cfg.gameLamps && (g_track >= 0 || g_menuCar) && g_hhWorld && g_gameLampsSent >= 0 ? g_gameLampsSent : -1;
}

// The matrix the car's body was drawn with this frame (the car draw group with the most triangles: the body node
// draws ~20 meshes with one matrix), for litparts.cpp to tell lamp lenses (on the body) from brake discs (on the
// wheels). Valid until this frame's headlights_frame_end.
bool rbrrtx_car_body_draw(float m[16]) {
  const Group* best = nullptr;
  for (const Group& g : g_groups) if (!best || g.prims > best->prims) best = &g;
  if (!best || best->prims < 200) return false;
  std::memcpy(m, best->m, sizeof(best->m));
  return true;
}

const wchar_t* rbrrtx_car_model_path() { return g_track >= 0 || g_menuCar ? g_car.file.c_str() : L""; }
