// Test of litparts.cpp's learning outside the game (2026-10-30, run 20261030-055723: ~100 body parts became lamp parts
// and the whole car glowed). Feeds judge_frame the parts each frame draws and the signal states, as rbrrtx_litparts_draw
// and rbrrtx_litparts_frame do, and checks what is learnt:
//  - a lamp switch with the same view: the lit lens is a lamps part, the body is not, the unlit lens is nothing;
//  - a brake press with the lamps on: the lit brake lens is a brake part, the unlit brake lens is a tail lamp;
//  - a lamp switch during a camera change (other LODs on each side): refused, nothing learnt;
//  - the run's case: lamps off in one view, on in another, for thousands of frames: nothing learnt;
//  - a lit part later drawn with every signal off: dropped.
//   x86 cl test\litparts_test.cpp -> litparts_test.exe
#include "../litparts.cpp"
#include <cstdarg>
#include <cstdio>

void rbrrtx_log_to(const wchar_t*, const wchar_t* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vfwprintf(stdout, fmt, a);
  va_end(a);
  fputwc(L'\n', stdout);
}
void rbrrtx_config_path(const wchar_t*, const wchar_t* file, wchar_t* out, size_t size) { swprintf_s(out, size, L"none\\%s", file); }
std::string rbrrtx_genmod_asset(const std::wstring&) { return ""; }
const wchar_t* rbrrtx_genmod_dir() { return L""; }
void rbrrtx_genmod_set(const char*, const std::string&) {}
std::string rbrrtx_genmod_material(unsigned long long, const std::string&, const std::string&) { return ""; }
unsigned long long rbrrtx_texture_hash(void*) { return 0; }
unsigned long long rbrrtx_xxh3(const void*, size_t) { return 0; }
bool rbrrtx_player_car_brake(float*) { return false; }
float rbrrtx_sun_shown_elevation() { return 30.0f; }
bool rbrrtx_player_car_raw(float[20]) { return false; }
int rbrrtx_headlights_lamp_end(const float*) { return 0; }
bool rbrrtx_headlights_has_lens() { return false; }
bool rbrrtx_headlights_is_aux(unsigned) { return false; }
bool rbrrtx_headlights_aux_on() { return false; }
bool rbrrtx_headlights_is_brake_lens(unsigned) { return false; }
bool rbrrtx_headlights_forward(float*) { return false; }
const float (*rbrrtx_headlights_front_rects(int* count))[4] { *count = 0; return nullptr; }
bool rbrrtx_dds_keep_rects(const wchar_t*, const wchar_t*, const float (*)[4], int) { return false; }
bool rbrrtx_car_body_draw(float*) { return false; }
bool rbrrtx_player_car_gear(int*) { return false; }

namespace {

// One frame: these parts drawn (key, triangles), then judged with these states (brake, lamps, reverse).
void frame(const std::vector<std::pair<uint64_t, unsigned>>& drawn, int brake, int lamps, int reverse = -1) {
  for (const auto& d : drawn) {
    Part& p = g_parts[d.first];
    p.lastFrame = g_frame;
    p.prims = d.second;
    g_drawn.push_back(d.first);
    ++g_carDrawsThisFrame;
  }
  const int st[kSignals] = { brake, lamps, reverse };
  judge_frame(st);
  ++g_frame;
  g_carDrawsThisFrame = 0;
}

std::vector<std::pair<uint64_t, unsigned>> body(uint64_t first, int n) {   // n body parts, 300-3800 triangles
  std::vector<std::pair<uint64_t, unsigned>> v;
  for (int i = 0; i < n; ++i) v.push_back({ first + i, 300u + 70u * i });
  return v;
}

std::vector<std::pair<uint64_t, unsigned>> with(std::vector<std::pair<uint64_t, unsigned>> v,
                                                std::initializer_list<std::pair<uint64_t, unsigned>> more) {
  v.insert(v.end(), more);
  return v;
}

unsigned mask(uint64_t k) { auto it = g_parts.find(k); return it == g_parts.end() ? 0 : it->second.mask; }
bool tail(uint64_t k) { auto it = g_parts.find(k); return it != g_parts.end() && it->second.tail; }
unsigned litCount() { unsigned n = 0; for (const auto& kv : g_parts) n += kv.second.mask ? 1 : 0; return n; }

int g_fails = 0;
void check(bool ok, const wchar_t* what) {
  wprintf(L"%s: %s\n", what, ok ? L"ok" : L"FAIL");
  if (!ok) ++g_fails;
}

}  // namespace

int main() {
  const uint64_t headUnlit = 1000, headLit = 1001, brakeUnlit = 2000, brakeLit = 2001;
  const auto car = body(1, 50);

  // 1. Lamps switched on, same view: off 30 frames, 12 unsettled, on 30.
  for (int f = 0; f < 30; ++f) frame(with(car, { { headUnlit, 220 }, { brakeUnlit, 220 } }), -1, -1);
  for (int f = 0; f < 12; ++f) frame(with(car, { { headLit, 220 }, { brakeUnlit, 220 } }), -1, 0);
  for (int f = 0; f < 30; ++f) frame(with(car, { { headLit, 220 }, { brakeUnlit, 220 } }), -1, 1);
  check(mask(headLit) == (1u << kLamps), L"lamp switch: the lit head lens is a lamps part");
  check(litCount() == 1, L"lamp switch: nothing else is lit (body, unlit lens)");

  // 2. Brake pressed with the lamps on: the lit brake lens replaces the unlit one.
  for (int f = 0; f < 12; ++f) frame(with(car, { { headLit, 220 }, { brakeLit, 220 } }), 0, 1);
  for (int f = 0; f < 20; ++f) frame(with(car, { { headLit, 220 }, { brakeLit, 220 } }), 1, 1);
  check(mask(brakeLit) == (1u << kBrake), L"brake with the lamps on: the lit brake lens is a brake part");
  check(tail(brakeUnlit), L"brake with the lamps on: the unlit brake lens glows as a tail lamp");
  check(mask(headLit) == (1u << kLamps), L"brake with the lamps on: the head lens stays a lamps part only");
  for (int f = 0; f < 12; ++f) frame(with(car, { { headLit, 220 }, { brakeUnlit, 220 } }), 0, 1);
  for (int f = 0; f < 20; ++f) frame(with(car, { { headLit, 220 }, { brakeUnlit, 220 } }), -1, 1);
  check(litCount() == 2 && tail(brakeUnlit), L"brake released: still the same two lit parts and the tail lens");

  // 3. New car (forget), lamps switched while the camera cuts: other LOD parts on each side.
  forget_car();
  const auto nearView = body(100, 50), farView = body(500, 20);
  for (int f = 0; f < 30; ++f) frame(with(nearView, { { headUnlit, 220 } }), -1, -1);
  for (int f = 0; f < 12; ++f) frame(with(farView, { { headLit, 120 } }), -1, 0);
  for (int f = 0; f < 30; ++f) frame(with(farView, { { headLit, 120 } }), -1, 1);
  check(litCount() == 0, L"switch during a camera cut: refused, nothing learnt");

  // 4. The run's case: the lamps on in the nearView view for thousands of frames, off for 230 in the farView view.
  forget_car();
  for (int f = 0; f < 2800; ++f) frame(with(nearView, { { headLit, 220 } }), -1, 1);
  for (int f = 0; f < 12; ++f) frame(farView, -1, 0);
  for (int f = 0; f < 230; ++f) frame(farView, -1, -1);
  for (int f = 0; f < 12; ++f) frame(with(nearView, { { headLit, 220 } }), -1, 0);
  for (int f = 0; f < 600; ++f) frame(with(nearView, { { headLit, 220 } }), -1, 1);
  check(litCount() == 0, L"lamps on in one view, off in another (run 20261030-055723): no body part lit");

  // 5. Learnt, then drawn with every signal off: dropped.
  forget_car();
  for (int f = 0; f < 30; ++f) frame(with(car, { { headUnlit, 220 } }), -1, -1);
  for (int f = 0; f < 12; ++f) frame(with(car, { { headLit, 220 } }), -1, 0);
  for (int f = 0; f < 30; ++f) frame(with(car, { { headLit, 220 } }), -1, 1);
  const bool learnt = mask(headLit) != 0;
  for (int f = 0; f < 12; ++f) frame(with(car, { { headLit, 220 } }), -1, 0);
  for (int f = 0; f < 5; ++f) frame(with(car, { { headLit, 220 } }), -1, -1);
  check(learnt && mask(headLit) == 0 && g_parts[headLit].never, L"a lit part drawn with every signal off: dropped for good");

  wprintf(L"%s\n", g_fails ? L"FAIL" : L"PASS");
  return g_fails ? 1 : 0;
}
