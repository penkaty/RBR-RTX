// Test of headlights.cpp outside the game: reads car models (.sgc) and checks the lamp placement maths.
//   x86 cl test\headlights_test.cpp -> headlights_test.exe <game folder>\Cars   (every Cars\*\*.sgc)
// Per car: the body is drawn with W = body-to-root * C (C = a rotated, moved car), as the game does (USD capture
// 2026-10-23: most of the car shares the body node's matrix), plus doors / spinning wheels as smaller groups with
// other matrices, three more model parts in their own frames, a bigger, wrongly placed 3rd-person body 4.6 m ahead and an off-centre bonnet bigger than the body; RBR's car object (+0x100 block as the
// game lays it out) is given. The model offset must lock after 20 still frames, and a car that drove on and turned,
// with no car draws at all, must be placed from RBR's car object alone: lamps within 1 cm of lamp * C, forward axis right, upright.
// Prints each car's lamps (model frame) and body frame.
#include "../headlights.cpp"
#include <cstdarg>
#include <cstdio>
#include <filesystem>

void rbrrtx_log_to(const wchar_t*, const wchar_t* fmt, ...) {
  if (!GetEnvironmentVariableW(L"HL_VERBOSE", nullptr, 0)) return;
  va_list a;
  va_start(a, fmt);
  vwprintf(fmt, a);
  va_end(a);
  wprintf(L"\n");
}
void rbrrtx_config_path(const wchar_t*, const wchar_t* file, wchar_t* out, size_t size) { swprintf_s(out, size, L"none\\%s", file); }
bool rbrrtx_key_pressed(int) { return false; }
const wchar_t* rbrrtx_key_name(int) { return L"L"; }
void rbrrtx_report_event(const wchar_t*) {}
bool rbrrtx_stage_info(RbrStageInfo*) { return false; }
float g_testRaw[20] = {};                              // RBR's car block (+0x100); position at [16..18], Y/Z swapped
bool g_testPhysOn = false;
bool rbrrtx_player_car_brake(float*) { return false; }
bool rbrrtx_player_car_gear(int*) { return false; }
float g_wsRoot[16] = {};                               // the model root the windscreen drops were last placed with
int g_wsCalls = 0;
void rbrrtx_windscreen_car_drawn(const float M[16]) { std::memcpy(g_wsRoot, M, sizeof(g_wsRoot)); ++g_wsCalls; }
#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"
unsigned long long rbrrtx_xxh3(const void* data, size_t size) { return XXH3_64bits(data, size); }
bool rbrrtx_player_car_raw(float out[20]) {
  if (!g_testPhysOn) return false;
  std::memcpy(out, g_testRaw, sizeof(g_testRaw));
  return true;
}

namespace {
void mul(const float* a, const float* b, float* out) {   // row-vector 4x4: out = a * b
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}
// Node local -> model root (inverse of Placed::A, general 3x3 inverse: some bodies are scaled).
void local_to_root(const Placed& p, float out[16]) {
  float A[16] = {};
  for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) A[r * 4 + c] = p.A[r][c];
  A[15] = 1;
  const float* m = A;
  const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
  std::memset(out, 0, sizeof(float) * 16);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
      out[r * 4 + c] = (m[r1 * 4 + c1] * m[r2 * 4 + c2] - m[r1 * 4 + c2] * m[r2 * 4 + c1]) / det;
    }
  for (int c = 0; c < 3; ++c) out[12 + c] = -(A[12] * out[c] + A[13] * out[4 + c] + A[14] * out[8 + c]);
  out[15] = 1;
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { wprintf(L"usage: headlights_test <game>\\Cars\n"); return 2; }
  // Car turned 37 deg around up, pitched 4 deg, somewhere on a stage.
  const float a = 37.0f * 0.0174533f, b = 4.0f * 0.0174533f;
  float C[16] = {};
  const V3 x = { std::cos(a), 0, -std::sin(a) }, y0 = { 0, 1, 0 }, z0 = { std::sin(a), 0, std::cos(a) };
  const V3 y = y0 * std::cos(b) + z0 * std::sin(b), z = z0 * std::cos(b) - y0 * std::sin(b);
  const V3 rows[3] = { x, y, z };
  for (int r = 0; r < 3; ++r) { C[r * 4] = rows[r].x; C[r * 4 + 1] = rows[r].y; C[r * 4 + 2] = rows[r].z; }
  C[12] = 1234.5f; C[13] = 87.25f; C[14] = -4321.0f; C[15] = 1;
  int interiorGlass = 0;
  int cars = 0, fails = 0, noLamps = 0, noBody = 0, noRear = 0, noExhaust = 0, noGlass = 0, glassFallback = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(argv[1])) {
    if (_wcsicmp(e.path().extension().c_str(), L".sgc") != 0) continue;
    ++cars;
    load_car_model(e.path().wstring());
    if (!g_car.lamps) { ++noLamps; wprintf(L"%-30s no Point_light_lf/rf\n", e.path().filename().c_str()); continue; }
    // RBR's car object as the game lays it out (+0x100 block): +0x110 3x3 rows = swap(axis0), swap(axis2),
    // swap(axis1); +0x140 position (Y/Z swapped); the physics origin sits at d = (0.1, -0.2, 0.25) from the model root.
    const V3 dTrue = { -(g_car.lf.x + g_car.rf.x) * 0.5f - 0.05f, -0.2f, 0.25f };   // RBR's centre near the centreline
    auto physics_at = [&](const float* Cc) {
      const V3 ax[3] = { row(Cc, 0), row(Cc, 1), row(Cc, 2) };
      const V3 o = row(Cc, 3) - (ax[0] * dTrue.x + ax[1] * dTrue.y + ax[2] * dTrue.z);
      const V3 rows3[3] = { ax[0], ax[2], ax[1] };
      for (int r = 0; r < 3; ++r) { g_testRaw[4 + r * 3] = rows3[r].x; g_testRaw[5 + r * 3] = rows3[r].z; g_testRaw[6 + r * 3] = rows3[r].y; }
      g_testRaw[16] = o.x; g_testRaw[17] = o.z; g_testRaw[18] = o.y;
    };
    g_off = Offset();
    g_haveLastPhys = false;
    g_testPhysOn = true;
    float L[16];
    local_to_root(g_car.body, L);
    // 21 still frames: the body drawn right and the 3rd-person body drawn wrong (bigger, 4.6 m ahead), plus wheels.
    for (int f = 0; f < 21; ++f) {
      g_groups.clear();
      physics_at(C);
      Group body;
      mul(L, C, body.m);
      body.prims = 9000;
      g_groups.push_back(body);
      float wrongC[16];
      std::memcpy(wrongC, C, sizeof(wrongC));
      wrongC[12] += 4.6f * C[8]; wrongC[13] += 1.0f; wrongC[14] += 4.6f * C[10];
      Group wrong;
      mul(L, wrongC, wrong.m);
      wrong.prims = 16000;
      g_groups.push_back(wrong);
      // Three more parts of the model drawn with their own node frames (bumpers, bonnet, ...), as the game does.
      for (size_t k = 0, added = 0; k < g_car.parts.size() && added < 3; ++k) {
        float Lp[16];
        local_to_root(g_car.parts[k], Lp);
        Group part;
        mul(Lp, C, part.m);
        if (same_matrix(part.m, body.m)) continue;
        part.prims = 400 + static_cast<unsigned>(k);
        g_groups.push_back(part);
        ++added;
      }
      // Evo IX style: the bonnet drawn on its own, 0.725 m off-centre at its hinge, bigger than the body draw.
      Group bonnet = body;
      const V3 off = norm(row(C, 0)) * 0.725f;
      bonnet.m[12] += off.x; bonnet.m[13] += off.y; bonnet.m[14] += off.z;
      bonnet.prims = 12000;
      g_groups.push_back(bonnet);
      for (int w = 0; w < 4; ++w) {
        Group gr;
        const float s = 0.7f * w + 0.3f + 0.1f * f;
        const float spin[16] = { 1, 0, 0, 0, 0, std::cos(s), std::sin(s), 0, 0, -std::sin(s), std::cos(s), 0, 0.8f, 0.3f, 1.3f - w, 1 };
        mul(spin, C, gr.m);
        gr.prims = 1200 + w;
        g_groups.push_back(gr);
      }
      measure_frame();
    }
    // The car drives on (20 m, turned 50 deg), nothing of it drawn: the lamps from RBR's car object alone.
    float C2[16];
    const float t = 0.8727f;
    const float turn[16] = { std::cos(t), 0, -std::sin(t), 0, 0, 1, 0, 0, std::sin(t), 0, std::cos(t), 0, 20, 0.5f, -7, 1 };
    mul(turn, C, C2);
    physics_at(C2);
    g_groups.clear();
    float P[16];
    const bool placed = place_frame(P);
    const V3 want = to_world(C2, g_car.lf);
    const float err = len(to_world(P, g_car.lf) - want), dirErr = len(norm(row(P, 2)) - norm(row(C2, 2)));
    // TV camera while moving (2026-10-23 sweep): the car drawn 0.4 m ahead of RBR's object (plus the wrong 3rd-person
    // body): after a second (slow average) the lamps sit in the drawn car.
    float C3[16];
    std::memcpy(C3, C2, sizeof(C3));
    const V3 ahead = norm(row(C2, 2)) * (0.4f * g_car.forward.z);
    C3[12] += ahead.x; C3[13] += ahead.y; C3[14] += ahead.z;
    for (int f = 0; f < 60; ++f) {
      g_groups.clear();
      Group body3;
      mul(L, C3, body3.m);
      body3.prims = 9000;
      g_groups.push_back(body3);
      float wrong3[16];
      std::memcpy(wrong3, C3, sizeof(wrong3));
      wrong3[12] += 4.6f * C3[8]; wrong3[14] += 4.6f * C3[10];
      Group w3;
      mul(L, wrong3, w3.m);
      w3.prims = 16000;
      g_groups.push_back(w3);
      track_drift();
    }
    float P3[16];
    place_frame(P3);
    const float driftErr = len(to_world(P3, g_car.lf) - to_world(C3, g_car.lf));
    // Windscreen drops (user run 6: they wiggled while driving): placed on the car as drawn this frame. The wrong
    // 3rd-person body comes first and must be passed over; the drawn body gives C3 exactly.
    g_rootThisFrame = g_predThisFrame = false;
    g_wsCalls = 0;
    g_track = 1;                                       // on a stage (the game sets it when a stage loads)
    {
      float wrong3[16], body3[16];
      std::memcpy(wrong3, C3, sizeof(wrong3));
      wrong3[12] += 4.6f * C3[8]; wrong3[14] += 4.6f * C3[10];
      float Lw[16];
      mul(L, wrong3, Lw);
      mul(L, C3, body3);
      rbrrtx_headlights_car_draw(Lw, 16000, false, false);
      rbrrtx_headlights_car_draw(body3, 9000, false, false);
    }
    float wsErr = g_wsCalls == 1 ? 0.0f : 1.0f;
    for (int i = 0; i < 16; ++i) wsErr = (std::max)(wsErr, std::fabs(g_wsRoot[i] - C3[i]));
    if (wsErr > 0.01f) {
      ++noGlass;
      wprintf(L"%-30s windscreen root from the drawn car off by %.3f (%d calls) BAD\n", e.path().filename().c_str(), wsErr, g_wsCalls);
    }
    // Cockpit from the camera (run 9): 0.45 m behind the middle of the glass and 0.15 m lower = in the cabin; 0.5 m in
    // front of the glass bottom, 0.2 m up = a bonnet camera, outside. RBR's object is C2 (no drift).
    physics_at(C2);
    V3 pb, pt, pn;
    int camBad = 0;
    if (windscreen_plane(&pb, &pt, &pn)) {
      if (dot(pn, g_car.forward) < 0.0f) pn = pn * -1.0f;
      const V3 eye = (pb + pt) * 0.5f - pn * 0.45f - V3{ 0, 0.15f, 0 };
      const V3 bonnet = pb + g_car.forward * 0.5f + V3{ 0, 0.2f, 0 };
      for (int k = 0; k < 2; ++k) {
        const V3 w = to_world(C2, k == 0 ? eye : bonnet);
        float cam[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, w.x, w.y, w.z, 1 };
        rbrrtx_headlights_camera(cam);
        g_insideFrame = ~0u;
        if (camera_inside() != (k == 0)) ++camBad;
      }
    }
    if (camBad) {
      ++noGlass;
      wprintf(L"%-30s camera in the cabin test wrong (%d of 2) BAD\n", e.path().filename().c_str(), camBad);
    }
    // Cockpit glass (run 12): camera in the cabin of the car drawn at C3; the chosen glass mesh drawn by
    // CarGlassInternal with its node matrix: the plane found equals the model's glass plane put at C3.
    int linkBad = 0;
    if (g_car.glass && windscreen_plane(&pb, &pt, &pn)) {
      if (dot(pn, g_car.forward) < 0.0f) pn = pn * -1.0f;
      const V3 eyeM = (pb + pt) * 0.5f - g_car.forward * 0.75f - V3{ 0, 0.1f, 0 };   // a driver's eye
      const V3 eye = to_world(C3, eyeM), ahead = norm(to_world(C3, eyeM + g_car.forward) - eye);
      const V3 upW = norm(row(C3, 1)), rightW = norm(V3{ upW.y * ahead.z - upW.z * ahead.y, upW.z * ahead.x - upW.x * ahead.z,
                                                          upW.x * ahead.y - upW.y * ahead.x });
      float cam[16] = { rightW.x, rightW.y, rightW.z, 0, upW.x, upW.y, upW.z, 0, ahead.x, ahead.y, ahead.z, 0, eye.x, eye.y, eye.z, 1 };
      physics_at(C3);
      rbrrtx_headlights_camera(cam);
      g_insideFrame = ~0u;
      float Lg[16], glassW[16];
      local_to_root(g_car.glassNode, Lg);
      mul(Lg, C3, glassW);
      g_glassW.clear();
      rbrrtx_headlights_car_draw(glassW, 40, true, true);
      cockpit_glass_find();
      ++g_frameNo;
      float gb[3], gt[3], gn[3], wb, wt;
      if (!rbrrtx_cockpit_glass(gb, gt, gn, &wb, &wt)) linkBad = 1;
      else if (len(V3{ gb[0], gb[1], gb[2] } - to_world(C3, g_car.glassBottom)) > 0.01f ||
               len(V3{ gt[0], gt[1], gt[2] } - to_world(C3, g_car.glassTop)) > 0.01f) linkBad = 2;
      physics_at(C2);
    }
    if (linkBad) {
      ++noGlass;
      wprintf(L"%-30s cockpit glass %s BAD\n", e.path().filename().c_str(),
              linkBad == 1 ? L"not found" : L"wrong");
    }
    g_bodyFrames = 0;
    g_track = -1;
    g_groups.clear();
    g_drift = {};
    // Rear lamps: one each side, behind the headlights; exhaust tip behind the car's middle, unit direction.
    const float cx = (g_car.lf.x + g_car.rf.x) * 0.5f;   // some models are off-centre: sides from the centreline
    const bool rearOk = g_car.rear && (g_car.lb.x - cx) * (g_car.rb.x - cx) < 0.0f && (g_car.lb.x - cx) * (g_car.lf.x - cx) > 0.0f &&
                        std::fabs(g_car.lb.y - g_car.rb.y) < 0.3f && dot(g_car.lb - g_car.lf, g_car.forward) < -2.0f &&
                        dot(g_car.rb - g_car.rf, g_car.forward) < -2.0f;
    const bool exhaustOk = g_car.exhaust && dot(g_car.exhaustTip, g_car.forward) < 0.0f && std::fabs(len(g_car.exhaustDir) - 1.0f) < 1e-3f;
    if (!rearOk) ++noRear;
    if (!exhaustOk) ++noExhaust;
    // Windscreen glass (windscreen.cpp): found, behind the head lamps, above them, facing forward and up, 0.9-2 m wide,
    // 0.3-1.2 m long, nearly flat.
    const V3 lampMid = (g_car.lf + g_car.rf) * 0.5f, gc = (g_car.glassBottom + g_car.glassTop) * 0.5f;
    const float glassBack = dot(lampMid - gc, g_car.forward), glassLen = len(g_car.glassTop - g_car.glassBottom);
    const bool glassOk = g_car.glass || g_car.glassRejected;   // a plane from the glass, or knowingly the lamp-based one
    if (g_car.glassRejected) ++glassFallback;
    if (g_car.glassName == "i_window_f") ++interiorGlass;
    if (!glassOk) ++noGlass;
    wprintf(L"%-30s glass %s: %.2f m behind the lamps, %.2f up, %.2f/%.2f wide, %.2f long, normal (%.2f %.2f %.2f), flat %.3f\n",
            e.path().filename().c_str(), g_car.glass ? L"ok" : g_car.glassRejected ? L"rejected (lamp plane)" : L"BAD", glassBack, gc.y - lampMid.y, g_car.glassWb, g_car.glassWt,
            glassLen, g_car.glassN.x, g_car.glassN.y, g_car.glassN.z, g_car.glassFlat);
    const bool ok = g_off.locked && placed && err < 0.01f && dirErr < 0.01f && rearOk && driftErr < 0.02f;
    if (!ok) ++fails;
    if (g_car.bodyName[0] == '(') ++noBody;
    wprintf(L"%-30s lf (%5.2f %5.2f %5.2f) rf (%5.2f %5.2f %5.2f) fwd %+.0fz | body %-18S err %.4f drift %.4f | rear (%5.2f %5.2f %5.2f) "
            L"(%5.2f %5.2f %5.2f)%s | exhaust (%5.2f %5.2f %5.2f) out (%5.2f %5.2f %5.2f)%s %s\n",
            e.path().filename().c_str(), g_car.lf.x, g_car.lf.y, g_car.lf.z, g_car.rf.x, g_car.rf.y, g_car.rf.z,
            g_car.forward.z, g_car.bodyName.c_str(), err, driftErr, g_car.lb.x, g_car.lb.y, g_car.lb.z, g_car.rb.x, g_car.rb.y, g_car.rb.z,
            rearOk ? L"" : L" BAD", g_car.exhaustTip.x, g_car.exhaustTip.y, g_car.exhaustTip.z, g_car.exhaustDir.x,
            g_car.exhaustDir.y, g_car.exhaustDir.z, exhaustOk ? L"" : L" BAD", ok ? L"ok" : L"FAIL");
  }
  // Lit lamp textures: the E30's lights_lit.dds must come out as Remix's 0xAE5889DDDA655C65 (harvest, 2026-10-23).
  {
    const std::filesystem::path game = std::filesystem::path(argv[1]).parent_path();
    const std::filesystem::path mods = std::filesystem::temp_directory_path() / L"rbrrtx_lit_test";
    std::filesystem::create_directories(mods);
    const size_t lit = write_lit_mod_for(game.wstring() + L"\\", mods.c_str(), 3.0f);
    std::ifstream in(mods / L"rbr_rtx_lit" / L"mod.usda");
    const std::string usda((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const bool litOk = lit >= 1 && usda.find("mat_AE5889DDDA655C65") != std::string::npos;
    wprintf(L"lit lamp textures: %zu, E30 lights_lit hash %s\n", lit, litOk ? L"ok" : L"FAIL");
    if (!litOk) ++fails;
  }
  wprintf(L"%d cars use the interior glass (i_window_f)\n", interiorGlass);
  wprintf(L"%d cars, %d without lamps, %d without e_body/i_body, %d bad rear lamps, %d bad exhaust, %d bad windscreen glass (%d lamp-plane fallbacks), %d FAIL\n",
          cars, noLamps, noBody, noRear, noExhaust, noGlass, glassFallback, fails);
  return cars > 0 && fails == 0 ? 0 : 1;
}
