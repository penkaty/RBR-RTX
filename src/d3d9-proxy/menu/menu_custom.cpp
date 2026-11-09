// menu_custom.cpp - F3 pages "Lights", "Emissive" and "Windows" (custom.cpp holds the data and the rendering side).
//
// Lights: Remix sphere lights at the crosshair hit point or in front of the camera, with colour, lumens, radius, flicker and
// night / always; kept per stage in menu.ini (Save). Emissive: the picked draw's texture (whole or the highlighted island)
// glows with a colour and intensity. Windows: rectangles over the panes of a facade texture, drawn by dragging in the
// thumbnail or found automatically from the luminance of the island; the mask gets a random lit fraction baked in.
#include "menu.hpp"
#include "../custom.hpp"
#include "../common.hpp"
#include <cctype>
#include <string>

#pragma warning(push, 0)
#include "../third_party/imgui/imgui.h"
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

// Tanner Helland's fit of the Planckian locus, linear, brightest channel 1 (as worldlights.cpp kelvin_rgb).
void kelvin_rgb(float k, float out[3]) {
  const float t = std::clamp(k, 1000.0f, 40000.0f) / 100.0f;
  const float r = t <= 66 ? 255 : 329.7f * std::pow(t - 60, -0.1332f);
  const float g = t <= 66 ? 99.47f * std::log(t) - 161.1f : 288.1f * std::pow(t - 60, -0.0755f);
  const float b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5f * std::log(t - 10) - 305.0f;
  const float c[3] = { r, g, b };
  for (int i = 0; i < 3; ++i) {
    const float s = std::clamp(c[i], 0.0f, 255.0f) / 255.0f;
    out[i] = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
  }
  const float m = (std::max)(out[0], (std::max)(out[1], out[2]));
  for (int i = 0; i < 3; ++i) out[i] /= m;
}

void status(const char* t) { rbrrtx_menu_set_status(t); }

// ---- Lamp presets (the old Alt+X lamp creator's, rtx_light_authoring.cpp, user 2026-11-07: "presets in F3") ------------
// Sodium lamps orange, LED / metal halide white. Lumens as the sphere/spot light's output; cone 0 = every way.
// User 2026-11-08 (saved 14:04): strong sodium = 125000 lm, cone 50, softness 0.15 (softer dims the lamp), glow 0.2 (the glow
// only has to look right and not clip; the lumens light the road). The others scaled from it. kelvin 0 = the rgb below.
// User 2026-11-09: sodium 3200 K, the LED and white lamps 750 K above their first values.
// window: glow only, with the window brightness curve (user 2026-11-09: this replaces the Windows page).
struct LampPreset { const char* name; float kelvin; float rgb[3]; float lumens, cone, softness, intensity; bool window; };
constexpr LampPreset kLampPresets[] = {
  { "Streetlamp (sodium)",         3200.0f, { 1.0f, 1.0f, 1.0f }, 50000.0f,  50.0f, 0.15f, 0.2f },
  { "Strong streetlamp (sodium)",  3200.0f, { 1.0f, 1.0f, 1.0f }, 125000.0f, 50.0f, 0.15f, 0.2f },
  { "Modern streetlamp (LED)",     4750.0f, { 1.0f, 1.0f, 1.0f }, 62000.0f,  50.0f, 0.15f, 0.2f },
  { "Modern strong streetlamp",    5750.0f, { 1.0f, 1.0f, 1.0f }, 156000.0f, 50.0f, 0.15f, 0.2f },
  { "Floodlight",                  4750.0f, { 1.0f, 1.0f, 1.0f }, 250000.0f, 35.0f, 0.15f, 0.3f },
  { "Stadium light",               6350.0f, { 1.0f, 1.0f, 1.0f }, 400000.0f, 40.0f, 0.15f, 0.4f },
  { "Coloured spot",               0.0f,    { 1.0f, 0.2f, 0.1f }, 125000.0f, 25.0f, 0.10f, 0.2f },
  { "Point light (every way)",     2700.0f, { 1.0f, 1.0f, 1.0f }, 31000.0f,  0.0f,  0.0f,  0.2f },
  { "Windows (glow only)",         3000.0f, { 1.0f, 1.0f, 1.0f }, 0.0f,      0.0f,  0.15f, 0.3f, true },
};

float nearest_kelvin(const float rgb[3]) {             // a saved lamp's colour on the temperature slider
  float best = 3000.0f, bestErr = 1e9f;
  for (float k = 1500.0f; k <= 9000.0f; k += 50.0f) {
    float c[3];
    kelvin_rgb(k, c);
    const float err = (c[0] - rgb[0]) * (c[0] - rgb[0]) + (c[1] - rgb[1]) * (c[1] - rgb[1]) + (c[2] - rgb[2]) * (c[2] - rgb[2]);
    if (err < bestErr) { bestErr = err; best = k; }
  }
  return best;
}

// Pitch (degrees below horizontal, 90 = straight down) and yaw (degrees around y, 0 = +z) to a world direction (y up).
void pitch_yaw_dir(float pitch, float yaw, float out[3]) {
  const float p = pitch * 3.14159265f / 180.0f, y = yaw * 3.14159265f / 180.0f;
  out[0] = std::cos(p) * std::sin(y);
  out[1] = -std::sin(p);
  out[2] = std::cos(p) * std::cos(y);
}

// ---- Lights --------------------------------------------------------------------------------------------------------
float g_front_dist = 4.0f;

void add_light(const float pos[3], const float rgb[3], float lumens, float radius, float flicker, int mode) {
  const int id = rbrrtx_custom_light_add(pos, rgb);
  RbrCustomLight l = {};
  for (int i = 0, n = rbrrtx_custom_light_count(); i < n; ++i)
    if (rbrrtx_custom_light_get(i, &l) && l.id == id) {
      l.lumens = lumens; l.radius = radius; l.flicker = flicker; l.mode = mode;
      rbrrtx_custom_light_set(l);
      break;
    }
  char b[120];
  std::snprintf(b, sizeof(b), "light %d added at %.2f %.2f %.2f (Save keeps it for this stage)", id, pos[0], pos[1], pos[2]);
  status(b);
}

// Preset: 0 warm lamp, 1 flare, 2 crowd / camp light.
void add_preset(const float pos[3], int preset) {
  float rgb[3];
  if (preset == 1) { rgb[0] = 1.0f; rgb[1] = 0.16f; rgb[2] = 0.07f; add_light(pos, rgb, 7000.0f, 0.18f, 0.7f, 1); return; }
  kelvin_rgb(preset == 2 ? 2700.0f : 3000.0f, rgb);
  add_light(pos, rgb, preset == 2 ? 1800.0f : 4000.0f, preset == 2 ? 0.25f : 0.1f, preset == 2 ? 0.15f : 0.0f, 0);
}

void page_lights(void*) {
  ImGui::TextWrapped("Lights at world positions, kept per stage (%s in menu.ini). Aim the middle of the screen at a surface "
                     "and add one at the hit point (nudged back toward you), or place one at the camera. Spheres light every way, "
                     "cones point somewhere: use cones for lamps that share a texture, one per lamp.", rbrrtx_custom_stage_name());
  rbrrtx_menu_pick pick = {};
  rbrrtx_menu_probe(&pick, 1);
  float cam[3], fwd[3];
  const bool haveCam = rbrrtx_menu_camera(cam, fwd) != 0;
  auto off_surface = [&](float pos[3]) {                // nudge 0.3 m toward the camera, off the surface
    pos[0] = pick.point[0]; pos[1] = pick.point[1]; pos[2] = pick.point[2];
    if (!haveCam) return;
    float d[3] = { cam[0] - pos[0], cam[1] - pos[1], cam[2] - pos[2] };
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > 1e-3f) for (int k = 0; k < 3; ++k) pos[k] += d[k] / len * 0.3f;
  };
  ImGui::SliderFloat("distance in front of the camera (m)", &g_front_dist, 1.0f, 40.0f, "%.1f");
  static const char* const kName[3] = { "warm lamp", "flare (flicker, always)", "crowd / camp light" };
  for (int p = 0; p < 3; ++p) {
    ImGui::PushID(p);
    ImGui::BeginDisabled(!pick.found);
    if (ImGui::Button("Add at crosshair")) { float pos[3]; off_surface(pos); add_preset(pos, p); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveCam);
    if (ImGui::Button("Add in front")) {
      const float pos[3] = { cam[0] + fwd[0] * g_front_dist, cam[1] + fwd[1] * g_front_dist, cam[2] + fwd[2] * g_front_dist };
      add_preset(pos, p);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(kName[p]);
    ImGui::PopID();
  }
  ImGui::SeparatorText("Cone light");
  static int conePreset = 1;
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
  if (ImGui::BeginCombo("lamp type", kLampPresets[conePreset].name)) {
    for (int p = 0; p < static_cast<int>(std::size(kLampPresets)); ++p)
      if (!kLampPresets[p].window && ImGui::Selectable(kLampPresets[p].name, p == conePreset)) conePreset = p;
    ImGui::EndCombo();
  }
  auto add_cone = [&](const float pos[3]) {
    const LampPreset& lp = kLampPresets[conePreset];
    float rgb[3];
    if (lp.kelvin > 0.0f) kelvin_rgb(lp.kelvin, rgb); else std::memcpy(rgb, lp.rgb, sizeof(rgb));
    const int id = rbrrtx_custom_light_add(pos, rgb);
    RbrCustomLight n = {};
    for (int i = 0, c = rbrrtx_custom_light_count(); i < c; ++i)
      if (rbrrtx_custom_light_get(i, &n) && n.id == id) {
        n.lumens = lp.lumens; n.radius = 0.1f; n.cone = lp.cone; n.softness = lp.softness;
        n.dir[0] = 0.0f; n.dir[1] = -1.0f; n.dir[2] = 0.0f;
        rbrrtx_custom_light_set(n);
        break;
      }
    status("cone light added, pointing straight down (Save keeps it for this stage)");
  };
  ImGui::BeginDisabled(!pick.found);
  if (ImGui::Button("Add cone at crosshair")) { float pos[3]; off_surface(pos); add_cone(pos); }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!haveCam);
  if (ImGui::Button("Add cone at camera")) add_cone(cam);
  ImGui::EndDisabled();
  if (!pick.found) ImGui::TextDisabled("no surface under the crosshair (needs a world draw; foliage included)");
  ImGui::SeparatorText("Lights of this stage");
  int remove = -1, duplicate = -1;
  RbrCustomLight l = {};
  static std::map<int, float> s_kelvin;                  // per light: the temperature slider of one light never moved another's
  for (int i = 0, n = rbrrtx_custom_light_count(); i < n; ++i) {
    if (!rbrrtx_custom_light_get(i, &l)) break;
    ImGui::PushID(l.id);
    char head[128];
    // "###" keeps one ID for the header: the text changes with every slider, and a new ID closed the section (user 2026-11-09).
    std::snprintf(head, sizeof(head), "%s %d  (%.1f, %.1f, %.1f)  %.0f lm%s###light", l.cone > 0.0f ? "cone" : "light", l.id, l.pos[0], l.pos[1], l.pos[2], l.lumens, l.mode ? " always" : "");
    if (ImGui::CollapsingHeader(head)) {
      bool ch = false;
      ch |= ImGui::ColorEdit3("colour", l.rgb, ImGuiColorEditFlags_Float);
      auto kIt = s_kelvin.find(l.id);
      if (kIt == s_kelvin.end()) kIt = s_kelvin.emplace(l.id, nearest_kelvin(l.rgb)).first;
      if (ImGui::SliderFloat("or temperature (K)", &kIt->second, 1500.0f, 9000.0f, "%.0f")) { kelvin_rgb(kIt->second, l.rgb); ch = true; }
      ch |= ImGui::SliderFloat("lumens", &l.lumens, 10.0f, 400000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
      ch |= ImGui::SliderFloat("radius (m)", &l.radius, 0.02f, 2.0f, "%.2f");
      ch |= ImGui::SliderFloat("cone half angle (0 = every way)", &l.cone, 0.0f, 90.0f, "%.0f deg");
      if (l.cone > 0.0f) {
        ch |= ImGui::SliderFloat("cone softness", &l.softness, 0.0f, 1.0f, "%.2f");
        ImGui::BeginDisabled(!pick.found);
        if (ImGui::Button("Aim at crosshair")) {
          const float d[3] = { pick.point[0] - l.pos[0], pick.point[1] - l.pos[1], pick.point[2] - l.pos[2] };
          const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
          if (len > 1e-3f) { for (int k = 0; k < 3; ++k) l.dir[k] = d[k] / len; ch = true; }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!haveCam);
        if (ImGui::Button("Aim where the camera looks")) { std::memcpy(l.dir, fwd, sizeof(l.dir)); ch = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Straight down")) { l.dir[0] = 0.0f; l.dir[1] = -1.0f; l.dir[2] = 0.0f; ch = true; }
      }
      ch |= ImGui::SliderFloat("flicker", &l.flicker, 0.0f, 1.0f, "%.2f");
      ch |= ImGui::RadioButton("at night", &l.mode, 0);
      ImGui::SameLine();
      ch |= ImGui::RadioButton("always", &l.mode, 1);
      ch |= ImGui::DragFloat3("position", l.pos, 0.05f);
      ImGui::BeginDisabled(!pick.found);
      if (ImGui::Button("Move to crosshair")) { off_surface(l.pos); ch = true; }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!haveCam);
      if (ImGui::Button("Place at camera position")) { std::memcpy(l.pos, cam, sizeof(l.pos)); ch = true; }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Duplicate")) duplicate = l.id;
      ImGui::SameLine();
      if (ImGui::Button("Delete")) remove = l.id;
      // The lamp's own mesh glows too: aim at the lamp head (its glass, or the whole mesh) and press.
      ImGui::BeginDisabled(!pick.found || !pick.hash[0]);
      if (ImGui::Button("Make the part at the crosshair glow in this colour")) {
        RbrEmit e = {};                                 // a new emissive with its own settings, also on a texture that has some
        e.intensity = 0.2f;
        e.dir[1] = -1.0f;
        e.softness = 0.2f;
        e.tex = pick.hash[0];
        e.mode = l.mode;
        std::memcpy(e.rgb, l.rgb, sizeof(e.rgb));
        if (pick.has_island && e.nrects < kEmitMaxRects) std::memcpy(e.rects[e.nrects++], pick.uv_rect, sizeof(float) * 4);
        rbrrtx_emit_set(e);
        status("the part glows once the generated mod has reloaded (about 10 s); Emissive props lists it, Save keeps it");
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The highlighted part (or the whole texture when nothing is highlighted) gets an emissive in the light's colour. The light itself stays this one.");
      if (ch) rbrrtx_custom_light_set(l);
    }
    ImGui::PopID();
  }
  if (duplicate >= 0) {
    RbrCustomLight src = {};
    for (int i = 0, n = rbrrtx_custom_light_count(); i < n; ++i)
      if (rbrrtx_custom_light_get(i, &src) && src.id == duplicate) {
        const int id = rbrrtx_custom_light_add(src.pos, src.rgb);
        src.id = id;
        rbrrtx_custom_light_set(src);
        status("light duplicated in the same place: move it with Move to crosshair or Place at camera position");
        break;
      }
  }
  if (remove >= 0) rbrrtx_custom_light_remove(remove);
  if (!rbrrtx_custom_light_count()) ImGui::TextDisabled("none on this stage");
}

// ---- thumbnail helper: the picked base texture with the island (yellow) and given rectangles (cyan) ----------------------
struct Thumb { ImVec2 min, max; };

Thumb draw_thumb(const rbrrtx_menu_pick& pick, float side, const std::vector<std::array<float, 4>>& rects) {
  Thumb t = {};
  if (!pick.texture[0]) { ImGui::Dummy(ImVec2(side, side)); return t; }
  ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(pick.texture[0])), ImVec2(side, side));
  t.min = ImGui::GetItemRectMin();
  t.max = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto box = [&](const float r[4], ImU32 fill, ImU32 line) {
    const ImVec2 p0(t.min.x + (t.max.x - t.min.x) * r[0], t.min.y + (t.max.y - t.min.y) * r[1]);
    const ImVec2 p1(t.min.x + (t.max.x - t.min.x) * r[2], t.min.y + (t.max.y - t.min.y) * r[3]);
    if (fill) dl->AddRectFilled(p0, p1, fill);
    dl->AddRect(p0, p1, line, 0.0f, 0, 1.5f);
  };
  if (pick.has_island) box(pick.uv_rect, IM_COL32(255, 220, 0, 40), IM_COL32(255, 220, 0, 255));
  for (const auto& r : rects) box(r.data(), IM_COL32(0, 220, 255, 70), IM_COL32(0, 220, 255, 255));
  return t;
}

// Editing a box on the thumbnail just drawn (user 2026-11-09): drag inside it to move it, drag an edge or corner to resize it, drag
// outside it to draw a new one. Call straight after draw_thumb. Returns true when the box changed.
struct BoxEdit { int mode = 0; float grab[2] = {}, start[4] = {}; };
bool box_edit(const Thumb& th, float r[4], BoxEdit& s) {
  if (th.max.x <= th.min.x) return false;
  const float W = th.max.x - th.min.x, H = th.max.y - th.min.y;
  const ImVec2 m = ImGui::GetIO().MousePos;
  const float u = std::clamp((m.x - th.min.x) / W, 0.0f, 1.0f), v = std::clamp((m.y - th.min.y) / H, 0.0f, 1.0f);
  const float gx = 6.0f / W, gy = 6.0f / H;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    const bool inX = u >= r[0] - gx && u <= r[2] + gx, inY = v >= r[1] - gy && v <= r[3] + gy;
    s.mode = (inY && std::fabs(u - r[0]) < gx ? 1 : 0) | (inY && std::fabs(u - r[2]) < gx ? 2 : 0) |
             (inX && std::fabs(v - r[1]) < gy ? 4 : 0) | (inX && std::fabs(v - r[3]) < gy ? 8 : 0);
    if (!s.mode) s.mode = inX && inY ? 16 : 32;
    s.grab[0] = u; s.grab[1] = v;
    std::memcpy(s.start, r, sizeof(s.start));
  }
  if (!s.mode) return false;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { s.mode = 0; return false; }
  const float du = u - s.grab[0], dv = v - s.grab[1], minW = 2.0f / W, minH = 2.0f / H;
  if (s.mode == 32) {
    r[0] = (std::min)(s.grab[0], u); r[1] = (std::min)(s.grab[1], v);
    r[2] = (std::max)(s.grab[0], u); r[3] = (std::max)(s.grab[1], v);
  } else if (s.mode == 16) {
    const float w = s.start[2] - s.start[0], h = s.start[3] - s.start[1];
    r[0] = std::clamp(s.start[0] + du, 0.0f, 1.0f - w); r[1] = std::clamp(s.start[1] + dv, 0.0f, 1.0f - h);
    r[2] = r[0] + w; r[3] = r[1] + h;
  } else {
    if (s.mode & 1) r[0] = std::clamp(s.start[0] + du, 0.0f, s.start[2] - minW);
    if (s.mode & 2) r[2] = std::clamp(s.start[2] + du, s.start[0] + minW, 1.0f);
    if (s.mode & 4) r[1] = std::clamp(s.start[1] + dv, 0.0f, s.start[3] - minH);
    if (s.mode & 8) r[3] = std::clamp(s.start[3] + dv, s.start[1] + minH, 1.0f);
  }
  return true;
}

// Left, top, width and height of a box as sliders. Returns true when one moved.
bool box_sliders(float r[4]) {
  float x = r[0], y = r[1], w = r[2] - r[0], h = r[3] - r[1];
  bool ch = false;
  ImGui::PushItemWidth(ImGui::GetFontSize() * 11.0f);
  ch |= ImGui::SliderFloat("left", &x, 0.0f, 1.0f, "%.3f");
  ch |= ImGui::SliderFloat("top", &y, 0.0f, 1.0f, "%.3f");
  ch |= ImGui::SliderFloat("width", &w, 0.001f, 1.0f, "%.3f");
  ch |= ImGui::SliderFloat("height", &h, 0.001f, 1.0f, "%.3f");
  ImGui::PopItemWidth();
  if (ch) {
    x = std::clamp(x, 0.0f, 0.999f); y = std::clamp(y, 0.0f, 0.999f);
    r[0] = x; r[1] = y; r[2] = (std::min)(x + w, 1.0f); r[3] = (std::min)(y + h, 1.0f);
  }
  return ch;
}

// ---- Emissive (props) ------------------------------------------------------------------------------------------------
void page_emissive(void*) {
  ImGui::TextWrapped("Aim the middle of the screen at a lamp head; the yellow box is the part of the texture around the hit point. "
                     "With \"only the highlighted part\" the glow AND the lamps go only where that part of the texture is drawn, so "
                     "other objects sharing the texture stay dark. Every lamp head drawn with that part gets one lamp at its centre "
                     "(move it with the offset in the list). Every press adds a new emissive with its own settings, also on a texture that already has one. "
                     "Lit windows: the Windows preset, with the box over the panes.");
  static bool freeze = false, island_only = true;
  static float rgb[3] = { 1.0f, 1.0f, 1.0f }, intensity = 0.2f, kelvin = 3200.0f;
  static const int rgbFromKelvin = (kelvin_rgb(kelvin, rgb), 0);
  (void)rgbFromKelvin;
  static int mode = 0, preset = 0;
  // Light the prop casts (user 2026-11-05: stadium floodlights): a lamp at every place the picked part is drawn.
  static bool cast = false;                            // user 2026-11-09: lights are placed one by one, a part can be drawn on many meshes
  static float lumens = 50000.0f, cone = 50.0f, softness = 0.15f, pitch = 90.0f, yaw = 0.0f;
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
  if (ImGui::BeginCombo("preset", kLampPresets[preset].name)) {
    for (int p = 0; p < static_cast<int>(std::size(kLampPresets)); ++p)
      if (ImGui::Selectable(kLampPresets[p].name, p == preset)) {
        preset = p;
        const LampPreset& lp = kLampPresets[p];
        if (lp.kelvin > 0.0f) { kelvin = lp.kelvin; kelvin_rgb(kelvin, rgb); }
        else std::memcpy(rgb, lp.rgb, sizeof(rgb));
        if (lp.window) cast = false;
        else { lumens = lp.lumens; cone = lp.cone; softness = lp.softness; }
        intensity = lp.intensity;
      }
    ImGui::EndCombo();
  }
  ImGui::Checkbox("Freeze (keep the last hit)", &freeze);
  rbrrtx_menu_pick pick = {};
  rbrrtx_menu_probe(&pick, freeze ? 0 : 1);
  if (!pick.found) { ImGui::TextDisabled("nothing under the crosshair"); }
  else {
    ImGui::Text("%s, %s, %.1f m", pick.kind, pick.shader, pick.distance);
    // The part (cyan): starts as the island around the hit (yellow), which on an atlas can be far bigger than the lamp
    // head (run 20261107-055329: a seventh of the atlas, lamps mid-pole and on ground posts). Drag on the picture to draw
    // the part yourself, or fine-tune it with the numbers.
    static float part[4] = { 0.0f, 0.0f, 1.0f, 1.0f }, lastIsland[4] = {};
    static unsigned long long partTex = 0;
    static BoxEdit box;
    static bool manual = false;                        // the box was moved by hand: aiming elsewhere does not take it away
    // Only a new texture used to start the box again, so a second lamp on the same texture got the first lamp's box
    // (user 2026-11-09: "one overwrites the other").
    const bool newIsland = pick.has_island && std::memcmp(lastIsland, pick.uv_rect, sizeof(lastIsland)) != 0;
    if (!box.mode && (pick.hash[0] != partTex || (newIsland && !manual))) {
      partTex = pick.hash[0];
      manual = false;
      if (pick.has_island) std::memcpy(part, pick.uv_rect, sizeof(part));
      else { part[0] = part[1] = 0.0f; part[2] = part[3] = 1.0f; }
    }
    if (pick.has_island) std::memcpy(lastIsland, pick.uv_rect, sizeof(lastIsland));
    const Thumb th = draw_thumb(pick, ImGui::GetTextLineHeight() * 14.0f, { { part[0], part[1], part[2], part[3] } });
    if (box_edit(th, part, box)) manual = true;
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (box_sliders(part)) manual = true;
    if (ImGui::SmallButton("Reset to island") && pick.has_island) { std::memcpy(part, pick.uv_rect, sizeof(part)); manual = false; }
    ImGui::EndGroup();
    ImGui::TextDisabled("Cyan box: drag inside to move, drag an edge to resize, drag outside to draw a new one. Yellow = the island the crosshair hit.");
    if (pick.hash[0]) ImGui::Text("texture 0x%016llX", pick.hash[0]);
    ImGui::Checkbox("only the part (cyan)", &island_only);
    static int anchor = 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    ImGui::Combo("lamp sits at the part's", &anchor, "top\0centre\0bottom\0");
    if (ImGui::SliderFloat("temperature (K)", &kelvin, 1500.0f, 9000.0f, "%.0f")) kelvin_rgb(kelvin, rgb);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sodium about 3200 K, LED 4750-5750 K.");
    if (ImGui::TreeNode("colour instead of temperature")) {
      ImGui::ColorEdit3("colour", rgb, ImGuiColorEditFlags_Float);
      ImGui::TreePop();
    }
    ImGui::SliderFloat("glow intensity", &intensity, 0.02f, 60.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::RadioButton("at night", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("always", &mode, 1);
    ImGui::Checkbox(island_only ? "also cast light (a lamp wherever this part is drawn)" : "also cast light (a lamp at EVERY mesh with this texture)", &cast);
    if (cast) {
      ImGui::SliderFloat("lumens", &lumens, 1000.0f, 400000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderFloat("cone half angle (0 = every way)", &cone, 0.0f, 90.0f, "%.0f deg");
      ImGui::SliderFloat("cone softness", &softness, 0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("pitch below horizontal", &pitch, -90.0f, 90.0f, "%.0f deg");
      ImGui::SliderFloat("yaw", &yaw, 0.0f, 360.0f, "%.0f deg");
      ImGui::TextDisabled("Or aim every lamp of it at a point: make it emissive, look at the point, press Aim here in the list.");
    }
    ImGui::BeginDisabled(!pick.hash[0]);
    if (ImGui::Button(cast ? "Make this a lamp" : "Make emissive")) {
      RbrEmit e = {};                                  // always a new one (user 2026-11-09: each emissive has its own settings)
      e.tex = pick.hash[0];
      e.windows = kLampPresets[preset].window ? 1 : 0;
      e.litPercent = 100;
      e.seed = 12345;
      e.mode = mode;
      e.intensity = intensity;
      std::memcpy(e.rgb, rgb, sizeof(e.rgb));
      e.anchor = anchor;
      if (island_only && part[2] > part[0] && part[3] > part[1]) {
        std::memcpy(e.rects[e.nrects++], part, sizeof(float) * 4);
      }                                                // no box: the whole texture
      e.lumens = cast ? lumens : 0.0f;
      e.cone = cone;
      e.softness = softness;
      pitch_yaw_dir(pitch, yaw, e.dir);
      rbrrtx_emit_set(e);
      manual = false;                                  // the next lamp starts from its own island
      status("emissive set (Save keeps it); it glows once the generated mod has reloaded, about 10 s");
    }
    ImGui::EndDisabled();
  }
  ImGui::SeparatorText("Emissive props");
  int remove = -1;
  RbrEmit e = {};
  // rbrrtx_emit_get refills e every frame, which threw a drag away (user 2026-11-08: the sliders went back to the value
  // the lamp was made with). The row being dragged keeps its own copy until the release applies it.
  static RbrEmit s_drag = {};
  static int s_dragId = 0;
  ImGui::TextDisabled("Only the lamps on this stage are listed.");
  for (int i = 0, n = rbrrtx_emit_count(); i < n; ++i) {
    if (!rbrrtx_emit_get(i, &e) || !rbrrtx_emit_on_stage(e.tex)) continue;
    if (e.id == s_dragId) e = s_drag;
    ImGui::PushID(e.id);
    ImGui::Text("#%d  0x%016llX  %s%s  intensity %.2f  %d part(s)", e.id, e.tex, e.windows ? "window, " : "", e.mode ? "always" : "night", e.intensity, e.nrects);
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) remove = e.id;
    if (e.nrects > 1) {                                // one part of the texture at a time
      for (int r = 0; r < e.nrects; ++r) {
        ImGui::PushID(1000 + r);
        ImGui::SameLine();
        char label[16];
        std::snprintf(label, sizeof(label), "x %d", r + 1);
        if (ImGui::SmallButton(label)) {
          RbrEmit cut = e;
          for (int k = r + 1; k < cut.nrects; ++k) std::memcpy(cut.rects[k - 1], cut.rects[k], sizeof(float) * 4);
          --cut.nrects;
          rbrrtx_emit_set(cut);
          status("that part no longer glows after the generated mod has reloaded (about 10 s)");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove part %d (u %.3f..%.3f, v %.3f..%.3f)", r + 1, e.rects[r][0], e.rects[r][2], e.rects[r][1], e.rects[r][3]);
        ImGui::PopID();
      }
    }
    // Edits apply on release: each one reloads the lamps (and the drag would otherwise do it every frame). The glow's
    // values change the material, so it comes back after the generated mod has reloaded, about 10 s.
    bool edit = false, held = false, glowEdit = false;
    if (ImGui::TreeNode("glow")) {                     // what "Make this a lamp" set (user 2026-11-08: all of it editable)
      static std::map<int, float> s_kelvin;
      auto kIt = s_kelvin.find(e.id);
      if (kIt == s_kelvin.end()) kIt = s_kelvin.emplace(e.id, nearest_kelvin(e.rgb)).first;
      if (ImGui::SliderFloat("temperature (K)", &kIt->second, 1500.0f, 9000.0f, "%.0f")) kelvin_rgb(kIt->second, e.rgb);
      glowEdit |= ImGui::IsItemDeactivatedAfterEdit();
      held |= ImGui::IsItemActive();
      if (ImGui::TreeNode("colour instead of temperature")) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        ImGui::ColorEdit3("colour", e.rgb, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoPicker);
        glowEdit |= ImGui::IsItemDeactivatedAfterEdit();
        held |= ImGui::IsItemActive();
        ImGui::TreePop();
      }
      ImGui::SliderFloat("glow intensity", &e.intensity, 0.02f, 60.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
      glowEdit |= ImGui::IsItemDeactivatedAfterEdit();
      held |= ImGui::IsItemActive();
      glowEdit |= ImGui::RadioButton("at night", &e.mode, 0);
      ImGui::SameLine();
      glowEdit |= ImGui::RadioButton("always", &e.mode, 1);
      ImGui::TreePop();
    }
    if (e.lumens > 0.0f) {
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
      ImGui::SliderFloat("lm", &e.lumens, 1000.0f, 400000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
      edit |= ImGui::IsItemDeactivatedAfterEdit();
      held |= ImGui::IsItemActive();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
      ImGui::SliderFloat("cone", &e.cone, 0.0f, 90.0f, "%.0f deg");
      edit |= ImGui::IsItemDeactivatedAfterEdit();
      held |= ImGui::IsItemActive();
      ImGui::SameLine();
      ImGui::BeginDisabled(!pick.found);
      if (ImGui::SmallButton("Aim here")) {             // every lamp of it points at what the crosshair hits now
        e.aim = 1;
        std::memcpy(e.aimAt, pick.point, sizeof(e.aimAt));
        edit = true;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::SmallButton("Down")) { e.aim = 0; e.dir[0] = 0.0f; e.dir[1] = -1.0f; e.dir[2] = 0.0f; edit = true; }
      ImGui::SameLine();
      if (ImGui::SmallButton("No light")) { e.lumens = 0.0f; edit = true; }
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
      ImGui::DragFloat3("light offset x / y / z (m)", e.offset, 0.02f, -20.0f, 20.0f, "%.2f");
      edit |= ImGui::IsItemDeactivatedAfterEdit();
      held |= ImGui::IsItemActive();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves every lamp of this texture by this much (world axes, y = up), e.g. out of the lamp head so the mesh does not block it.");
      if (e.aim) ImGui::TextDisabled("  aimed at (%.1f %.1f %.1f)", e.aimAt[0], e.aimAt[1], e.aimAt[2]);
      if (ImGui::TreeNode("lamp")) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        edit |= ImGui::Combo("lamp sits at the part's", &e.anchor, "top\0centre\0bottom\0");
        ImGui::SliderFloat("cone softness", &e.softness, 0.0f, 1.0f, "%.2f");
        edit |= ImGui::IsItemDeactivatedAfterEdit();
        held |= ImGui::IsItemActive();
        float rowPitch = std::asin(std::clamp(-e.dir[1], -1.0f, 1.0f)) * 180.0f / 3.14159265f;
        float rowYaw = std::atan2(e.dir[0], e.dir[2]) * 180.0f / 3.14159265f;
        if (rowYaw < 0.0f) rowYaw += 360.0f;
        bool aimed = ImGui::SliderFloat("pitch below horizontal", &rowPitch, -90.0f, 90.0f, "%.0f deg");
        edit |= ImGui::IsItemDeactivatedAfterEdit();
        held |= ImGui::IsItemActive();
        aimed |= ImGui::SliderFloat("yaw", &rowYaw, 0.0f, 360.0f, "%.0f deg");
        edit |= ImGui::IsItemDeactivatedAfterEdit();
        held |= ImGui::IsItemActive();
        if (aimed) { e.aim = 0; pitch_yaw_dir(rowPitch, rowYaw, e.dir); }
        ImGui::TreePop();
      }
    } else if (ImGui::SmallButton("Cast light")) {
      e.lumens = lumens;
      e.cone = cone;
      e.softness = softness;
      pitch_yaw_dir(pitch, yaw, e.dir);
      rbrrtx_emit_set_light(e);
    }
    if (held) { s_drag = e; s_dragId = e.id; }
    else if (s_dragId == e.id) s_dragId = 0;
    if (glowEdit) {
      rbrrtx_emit_set(e);
      status("glow changed (Save keeps it); it returns once the generated mod has reloaded, about 10 s");
    } else if (edit) {
      rbrrtx_emit_set_light(e);
      status("light changed (Save keeps it)");
    }
    ImGui::PopID();
    if (remove == e.id) { rbrrtx_emit_remove(e.id); break; }
  }
}

// ---- Lamps: the two pages above in one ------------------------------------------------------------------------------
void page_lamps(void*) {
  if (ImGui::CollapsingHeader("Lamp heads: glow + light from a part of a texture", ImGuiTreeNodeFlags_DefaultOpen)) page_emissive(nullptr);
  if (ImGui::CollapsingHeader("Free lights at a point (flares, crowd, doorway)")) page_lights(nullptr);
}

// ---- ini editor pages: Atmosphere (atmosphere.ini), Headlights (headlights.ini) --------------------------------------
// Every "key = value" line of the file as an input field, grouped by [section]; the comment lines right above a key are its
// tooltip. Save rewrites ONLY the changed values in place (comments, order, spacing and line endings stay; keys are not
// added or removed; a one-time copy <file>.f3bak is made before the first save of a run).
struct IniLine {
  std::string text, prefix, key, val, help, section;
  bool is_section = false, is_kv = false, dirty = false;
};
struct IniDoc {
  const wchar_t* env;
  const wchar_t* file;
  const char* name;
  const char* after_save;
  std::wstring path;
  std::vector<IniLine> lines;
  bool loaded = false, crlf = false, bom = false, backed_up = false;
  char filter[64] = "";
  std::string note;
};

std::string ini_trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void ini_load(IniDoc& d) {
  d.loaded = true;
  d.lines.clear();
  d.note.clear();
  wchar_t p[MAX_PATH * 2] = {};
  rbrrtx_config_path(d.env, d.file, p, MAX_PATH * 2);
  d.path = p;
  FILE* f = nullptr;
  if (_wfopen_s(&f, p, L"rb") != 0 || !f) { d.note = "file not found"; return; }
  std::string all;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) all.append(buf, n);
  std::fclose(f);
  d.bom = all.size() >= 3 && (unsigned char)all[0] == 0xEF && (unsigned char)all[1] == 0xBB && (unsigned char)all[2] == 0xBF;
  if (d.bom) all.erase(0, 3);
  d.crlf = all.find("\r\n") != std::string::npos;
  std::string section, help;
  size_t pos = 0;
  while (pos <= all.size()) {
    size_t e = all.find('\n', pos);
    if (e == std::string::npos) e = all.size();
    std::string t = all.substr(pos, e - pos);
    if (!t.empty() && t.back() == '\r') t.pop_back();
    IniLine l;
    l.text = t;
    const std::string tt = ini_trim(t);
    if (tt.empty()) help.clear();
    else if (tt[0] == ';' || tt[0] == '#') { help += (help.empty() ? "" : "\n") + ini_trim(tt.substr(1)); }
    else if (tt[0] == '[' && tt.find(']') != std::string::npos) { l.is_section = true; section = tt.substr(1, tt.find(']') - 1); help.clear(); }
    else {
      const size_t eq = t.find('=');
      if (eq != std::string::npos) {
        size_t v = eq + 1;
        while (v < t.size() && (t[v] == ' ' || t[v] == '\t')) ++v;
        l.is_kv = true; l.prefix = t.substr(0, v); l.key = ini_trim(t.substr(0, eq)); l.val = ini_trim(t.substr(v));
        l.help = help; l.section = section;
      }
      help.clear();
    }
    d.lines.push_back(std::move(l));
    if (e >= all.size()) break;
    pos = e + 1;
  }
}

bool ini_save(IniDoc& d) {
  std::wstring bak = d.path + L".f3bak";
  if (!d.backed_up) { CopyFileW(d.path.c_str(), bak.c_str(), FALSE); d.backed_up = true; }
  std::string out;
  if (d.bom) out += "\xEF\xBB\xBF";
  const char* eol = d.crlf ? "\r\n" : "\n";
  for (size_t i = 0; i < d.lines.size(); ++i) {
    const IniLine& l = d.lines[i];
    out += l.is_kv && l.dirty ? l.prefix + l.val : l.text;
    if (i + 1 < d.lines.size()) out += eol;
  }
  const std::wstring tmp = d.path + L".f3tmp";
  FILE* f = nullptr;
  if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  if (!ok || !MoveFileExW(tmp.c_str(), d.path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
  for (IniLine& l : d.lines) if (l.dirty) { l.text = l.prefix + l.val; l.dirty = false; }
  return true;
}

// "stage.458" or "stage.458.HeavyRain" -> "stage.458  Rosciszow-Walim" from the folder Maps\458-Rosciszow-Walim. Cached.
const std::string& section_label(const std::string& section) {
  static std::map<std::string, std::string> cache;
  const auto it = cache.find(section);
  if (it != cache.end()) return it->second;
  std::string label = section;
  if (section.rfind("stage.", 0) == 0) {
    const std::wstring id = std::to_wstring(std::atoi(section.c_str() + 6));
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((L"Maps\\" + id + L"-*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      FindClose(h);
      label += "  ";
      for (const wchar_t* c = fd.cFileName + id.size() + 1; *c; ++c) label += *c < 128 ? static_cast<char>(*c) : '?';
    }
  }
  return cache[section] = label;
}

// scope: 0 every section, 1 all but [stage.*], 2 only [stage.*].
void page_ini(IniDoc& d, int scope = 0) {
  if (!d.loaded) ini_load(d);
  ImGui::TextWrapped("%s", d.after_save);
  ImGui::TextDisabled("%ls", d.path.c_str());
  if (ImGui::Button("Reload from file")) ini_load(d);
  ImGui::SameLine();
  int dirty = 0;
  for (const IniLine& l : d.lines) dirty += l.dirty ? 1 : 0;
  if (ImGui::Button("Save to file")) {
    if (!dirty) d.note = "nothing changed";
    else { char b[160]; const bool ok = ini_save(d); std::snprintf(b, sizeof(b), ok ? "saved %d value(s)" : "COULD NOT WRITE the file (%d value(s) kept here)", dirty); d.note = b; }
  }
  ImGui::SameLine();
  ImGui::Text("%d unsaved   %s", dirty, d.note.c_str());
  ImGui::SetNextItemWidth(220);
  ImGui::InputText("Filter (key or section)", d.filter, sizeof(d.filter));
  ImGui::Separator();
  ImGui::BeginChild("ini_scroll", ImVec2(0, 0), false);
  std::string low(d.filter);
  for (char& c : low) c = (char)std::tolower((unsigned char)c);
  const bool filtering = !low.empty();
  auto matches = [&](const IniLine& l) {
    const bool stage = l.section.rfind("stage.", 0) == 0;
    if ((scope == 1 && stage) || (scope == 2 && !stage)) return false;
    if (!filtering) return true;
    std::string k = l.key + " " + section_label(l.section);
    for (char& c : k) c = (char)std::tolower((unsigned char)c);
    return k.find(low) != std::string::npos;
  };
  bool open = false, any = false;
  std::string cur = "\x01";
  for (size_t i = 0; i < d.lines.size(); ++i) {
    IniLine& l = d.lines[i];
    if (!l.is_kv) continue;
    if (l.section != cur) {
      if (any && open) ImGui::TreePop();
      cur = l.section; any = false; open = false;
    }
    if (!matches(l)) continue;
    if (!any) {
      any = true;
      if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
      open = ImGui::TreeNode(cur.empty() ? "(top)" : section_label(cur).c_str());
    }
    if (!open) continue;
    ImGui::PushID((int)i);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (l.val.size() >= 1000) {
      ImGui::LabelText(l.key.c_str(), "(long value: edit the file)");
    } else {
      char b[1024];
      std::snprintf(b, sizeof(b), "%s", l.val.c_str());
      // The first edit sets dirty inside InputText: pop only what was pushed (user 2026-11-07 crash, ImGui assert).
      const bool tinted = l.dirty;
      if (tinted) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
      if (ImGui::InputText(l.key.c_str(), b, sizeof(b))) { l.val = b; l.dirty = true; }
      if (tinted) ImGui::PopStyleColor();
    }
    if (!l.help.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", l.help.c_str());
    ImGui::PopID();
  }
  if (any && open) ImGui::TreePop();
  ImGui::EndChild();
}

IniDoc g_atmo_doc = { L"RBRRTX_ATMOSPHERE", L"atmosphere.ini", "Atmosphere",
  "atmosphere.ini: stage weather, haze, sun, NUMOS. Hover a key for its comment. Save: the proxy re-reads the file within about a second, so a change shows while you drive." };
IniDoc g_head_doc = { L"RBRRTX_HEADLIGHTS", L"headlights.ini", "Headlights",
  "headlights.ini: car lamps. Hover a key for its comment. Save writes the file; the proxy reads it at the next stage start (restart the stage to see a change)." };
void page_atmo(void*) { page_ini(g_atmo_doc, 1); }
void page_atmo_stage(void*) { page_ini(g_atmo_doc, 2); }
void page_head(void*) { page_ini(g_head_doc); }

}  // namespace

void rbrrtx_carpaint_register_page();

extern "C" void rbrrtx_menu_register_custom_pages(void) {
  rbrrtx_carpaint_register_page();
  rbrrtx_menu_add_page("Atmosphere", page_atmo, nullptr);
  rbrrtx_menu_add_page("Per stage", page_atmo_stage, nullptr);
  rbrrtx_menu_add_page("Headlights", page_head, nullptr);
  // One page for lamps (user 2026-11-07: "combine lights and emissives into one in F3"): lamp heads (emissive part +
  // its lamps, with presets) first, free-standing lights below.
  rbrrtx_menu_add_page("Lamps", page_lamps, nullptr);
}
