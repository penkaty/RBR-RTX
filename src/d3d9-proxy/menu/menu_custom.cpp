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

RbrEmit* find_emit(unsigned long long tex, RbrEmit* store);

void page_lights(void*) {
  ImGui::TextWrapped("Sphere lights at world positions, kept per stage (%s in menu.ini). Aim the middle of the screen at a surface "
                     "and add one at the hit point (nudged back toward you), or place one in front of the camera. For crowd lights, "
                     "flares, a lit doorway.", rbrrtx_custom_stage_name());
  rbrrtx_menu_pick pick = {};
  rbrrtx_menu_probe(&pick, 1);
  float cam[3], fwd[3];
  const bool haveCam = rbrrtx_menu_camera(cam, fwd) != 0;
  ImGui::SliderFloat("distance in front of the camera (m)", &g_front_dist, 1.0f, 40.0f, "%.1f");
  static const char* const kName[3] = { "warm lamp", "flare (flicker, always)", "crowd / camp light" };
  for (int p = 0; p < 3; ++p) {
    ImGui::PushID(p);
    ImGui::BeginDisabled(!pick.found);
    if (ImGui::Button("Add at crosshair")) {
      float pos[3] = { pick.point[0], pick.point[1], pick.point[2] };
      if (haveCam) {                                      // nudge 0.3 m toward the camera, off the surface
        float d[3] = { cam[0] - pos[0], cam[1] - pos[1], cam[2] - pos[2] };
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 1e-3f) for (int k = 0; k < 3; ++k) pos[k] += d[k] / len * 0.3f;
      }
      add_preset(pos, p);
    }
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
  if (!pick.found) ImGui::TextDisabled("no surface under the crosshair (needs a world draw; foliage included)");
  ImGui::SeparatorText("Lights of this stage");
  int remove = -1;
  RbrCustomLight l = {};
  for (int i = 0, n = rbrrtx_custom_light_count(); i < n; ++i) {
    if (!rbrrtx_custom_light_get(i, &l)) break;
    ImGui::PushID(l.id);
    char head[96];
    std::snprintf(head, sizeof(head), "light %d  (%.1f, %.1f, %.1f)  %.0f lm%s", l.id, l.pos[0], l.pos[1], l.pos[2], l.lumens, l.mode ? " always" : "");
    if (ImGui::CollapsingHeader(head)) {
      bool ch = false;
      ch |= ImGui::ColorEdit3("colour", l.rgb, ImGuiColorEditFlags_Float);
      static float kelvin = 3000.0f;
      if (ImGui::SliderFloat("or temperature (K)", &kelvin, 1500.0f, 9000.0f, "%.0f")) { kelvin_rgb(kelvin, l.rgb); ch = true; }
      ch |= ImGui::SliderFloat("lumens", &l.lumens, 10.0f, 200000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
      ch |= ImGui::SliderFloat("radius (m)", &l.radius, 0.02f, 2.0f, "%.2f");
      ch |= ImGui::SliderFloat("flicker", &l.flicker, 0.0f, 1.0f, "%.2f");
      ch |= ImGui::RadioButton("at night", &l.mode, 0);
      ImGui::SameLine();
      ch |= ImGui::RadioButton("always", &l.mode, 1);
      ch |= ImGui::DragFloat3("position", l.pos, 0.05f);
      if (ImGui::Button("Move to crosshair") && pick.found) { for (int k = 0; k < 3; ++k) l.pos[k] = pick.point[k]; ch = true; }
      ImGui::SameLine();
      if (ImGui::Button("Delete")) remove = l.id;
      // The lamp's own mesh glows too: aim at the lamp head (its glass, or the whole mesh) and press.
      ImGui::BeginDisabled(!pick.found || !pick.hash[0]);
      if (ImGui::Button("Make the part at the crosshair glow in this colour")) {
        RbrEmit e = {};
        if (!find_emit(pick.hash[0], &e) || e.windows) { e = {}; e.intensity = 6.0f; e.dir[1] = -1.0f; e.softness = 0.2f; }
        e.tex = pick.hash[0];
        e.windows = 0;
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

RbrEmit* find_emit(unsigned long long tex, RbrEmit* store) {
  for (int i = 0, n = rbrrtx_emit_count(); i < n; ++i)
    if (rbrrtx_emit_get(i, store) && store->tex == tex) return store;
  return nullptr;
}

// Pitch (degrees below horizontal, 90 = straight down) and yaw (degrees around y, 0 = +z) to a world direction (y up).
void pitch_yaw_dir(float pitch, float yaw, float out[3]) {
  const float p = pitch * 3.14159265f / 180.0f, y = yaw * 3.14159265f / 180.0f;
  out[0] = std::cos(p) * std::sin(y);
  out[1] = -std::sin(p);
  out[2] = std::cos(p) * std::cos(y);
}

// ---- Emissive (props) ------------------------------------------------------------------------------------------------
void page_emissive(void*) {
  ImGui::TextWrapped("Make the texture of a prop glow. Aim the middle of the screen at it; the yellow box is the part of the texture "
                     "around the hit point. Whole texture or just that part, a colour, an intensity, at night (fading in with the street "
                     "lamps) or always. Every draw with that texture glows. Window facades: use the Windows page.");
  static bool freeze = false, island_only = true;
  static float rgb[3] = { 1.0f, 0.75f, 0.45f }, intensity = 6.0f, kelvin = 3000.0f;
  static int mode = 0;
  // Light the prop casts (user 2026-11-05: stadium floodlights); every mesh with the texture gets one at its centre.
  static bool cast = false;
  static float lumens = 40000.0f, cone = 30.0f, softness = 0.2f, pitch = 90.0f, yaw = 0.0f;
  ImGui::Checkbox("Freeze (keep the last hit)", &freeze);
  rbrrtx_menu_pick pick = {};
  rbrrtx_menu_probe(&pick, freeze ? 0 : 1);
  if (!pick.found) { ImGui::TextDisabled("nothing under the crosshair"); }
  else {
    ImGui::Text("%s, %s, %.1f m", pick.kind, pick.shader, pick.distance);
    draw_thumb(pick, ImGui::GetTextLineHeight() * 8.0f, {});
    if (pick.hash[0]) ImGui::Text("texture 0x%016llX", pick.hash[0]);
    ImGui::Checkbox("only the highlighted part", &island_only);
    ImGui::ColorEdit3("colour", rgb, ImGuiColorEditFlags_Float);
    if (ImGui::SliderFloat("or temperature (K)", &kelvin, 1500.0f, 9000.0f, "%.0f")) kelvin_rgb(kelvin, rgb);
    ImGui::SliderFloat("intensity", &intensity, 0.2f, 60.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::RadioButton("at night", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("always", &mode, 1);
    ImGui::Checkbox("also cast light (a lamp at every mesh with this texture)", &cast);
    if (cast) {
      ImGui::SliderFloat("lumens", &lumens, 1000.0f, 400000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderFloat("cone half angle (0 = every way)", &cone, 0.0f, 90.0f, "%.0f deg");
      ImGui::SliderFloat("cone softness", &softness, 0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("pitch below horizontal", &pitch, -90.0f, 90.0f, "%.0f deg");
      ImGui::SliderFloat("yaw", &yaw, 0.0f, 360.0f, "%.0f deg");
      ImGui::TextDisabled("Or aim every lamp of it at a point: make it emissive, look at the point, press Aim here in the list.");
    }
    ImGui::BeginDisabled(!pick.hash[0]);
    if (ImGui::Button("Make emissive")) {
      RbrEmit store = {};
      RbrEmit e = {};
      if (find_emit(pick.hash[0], &store) && !store.windows) e = store;     // another part of the same texture: more rectangles
      e.tex = pick.hash[0];
      e.windows = 0;
      e.mode = mode;
      e.intensity = intensity;
      std::memcpy(e.rgb, rgb, sizeof(e.rgb));
      if (island_only && pick.has_island) {
        if (e.nrects < kEmitMaxRects) std::memcpy(e.rects[e.nrects++], pick.uv_rect, sizeof(float) * 4);
      } else {
        e.nrects = 0;                                    // the whole texture
      }
      e.lumens = cast ? lumens : 0.0f;
      e.cone = cone;
      e.softness = softness;
      pitch_yaw_dir(pitch, yaw, e.dir);
      rbrrtx_emit_set(e);
      status("emissive set (Save keeps it); it glows once the generated mod has reloaded, about 10 s");
    }
    ImGui::EndDisabled();
  }
  ImGui::SeparatorText("Emissive props");
  int remove = -1;
  RbrEmit e = {};
  for (int i = 0, n = rbrrtx_emit_count(); i < n; ++i) {
    if (!rbrrtx_emit_get(i, &e) || e.windows) continue;
    ImGui::PushID(i);
    ImGui::Text("0x%016llX  %s  intensity %.1f  %d part(s)", e.tex, e.mode ? "always" : "night", e.intensity, e.nrects);
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) remove = static_cast<int>(i);
    if (e.lumens > 0.0f) {
      // Edits apply on release: each one reloads the lamps (and the drag would otherwise do it every frame).
      bool edit = false;
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
      ImGui::SliderFloat("lm", &e.lumens, 1000.0f, 400000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
      edit |= ImGui::IsItemDeactivatedAfterEdit();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
      ImGui::SliderFloat("cone", &e.cone, 0.0f, 90.0f, "%.0f deg");
      edit |= ImGui::IsItemDeactivatedAfterEdit();
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
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves every lamp of this texture by this much (world axes, y = up), e.g. out of the lamp head so the mesh does not block it.");
      if (e.aim) ImGui::TextDisabled("  aimed at (%.1f %.1f %.1f)", e.aimAt[0], e.aimAt[1], e.aimAt[2]);
      if (edit) {
        rbrrtx_emit_set_light(e);
        status("light changed (Save keeps it)");
      }
    } else if (ImGui::SmallButton("Cast light")) {
      e.lumens = lumens;
      e.cone = cone;
      e.softness = softness;
      pitch_yaw_dir(pitch, yaw, e.dir);
      rbrrtx_emit_set_light(e);
    }
    ImGui::PopID();
    if (remove == i) { rbrrtx_emit_remove(e.tex); break; }
  }
}

// ---- Windows ----------------------------------------------------------------------------------------------------------
void page_windows(void*) {
  ImGui::TextWrapped("Light the windows of a wall. Aim at the facade, tick Freeze, then drag rectangles over the panes in the thumbnail "
                     "(cyan) or press Auto to find the dark / glassy cells inside the yellow island. Apply bakes a random lit fraction "
                     "and brightness per pane into a night-only emissive mask with a warm glow. The mask belongs to the texture: every "
                     "building with it shows the same lit windows.");
  static bool freeze = false;
  static unsigned long long s_tex = 0;
  static std::vector<std::array<float, 4>> rects;
  static float kelvin = 3000.0f, intensity = 5.0f;
  static int lit = 40, mode = 0;
  static unsigned seed = 12345;
  static ImVec2 drag0;
  static bool dragging = false;
  ImGui::Checkbox("Freeze (keep the last hit)", &freeze);
  rbrrtx_menu_pick pick = {};
  rbrrtx_menu_probe(&pick, freeze ? 0 : 1);
  if (!pick.found || !pick.hash[0]) { ImGui::TextDisabled("nothing under the crosshair"); }
  else {
    if (pick.hash[0] != s_tex) {                         // another texture: the working set starts from what was applied for it
      s_tex = pick.hash[0];
      rects.clear();
      RbrEmit store = {};
      if (find_emit(s_tex, &store) && store.windows) {
        for (int i = 0; i < store.nrects; ++i) rects.push_back({ store.rects[i][0], store.rects[i][1], store.rects[i][2], store.rects[i][3] });
        lit = store.litPercent; kelvin = 3000.0f; intensity = store.intensity; mode = store.mode; seed = store.seed;
      }
    }
    ImGui::Text("%s, %s, %.1f m, texture 0x%016llX", pick.kind, pick.shader, pick.distance, pick.hash[0]);
    const Thumb t = draw_thumb(pick, ImGui::GetTextLineHeight() * 20.0f, rects);
    if (freeze && pick.texture[0] && ImGui::IsItemHovered()) {          // drag a rectangle over a pane
      const ImVec2 m = ImGui::GetIO().MousePos;
      const auto uv = [&](ImVec2 p) { return ImVec2(std::clamp((p.x - t.min.x) / (t.max.x - t.min.x), 0.0f, 1.0f), std::clamp((p.y - t.min.y) / (t.max.y - t.min.y), 0.0f, 1.0f)); };
      if (ImGui::IsMouseClicked(0)) { drag0 = uv(m); dragging = true; }
      if (dragging && ImGui::IsMouseReleased(0)) {
        const ImVec2 a = drag0, b = uv(m);
        dragging = false;
        const float x0 = (std::min)(a.x, b.x), x1 = (std::max)(a.x, b.x), y0 = (std::min)(a.y, b.y), y1 = (std::max)(a.y, b.y);
        if (x1 - x0 > 0.0005f && y1 - y0 > 0.0005f && rects.size() < kEmitMaxRects) rects.push_back({ x0, y0, x1, y1 });
      }
      if (dragging) {
        const ImVec2 a = drag0, b = uv(m);
        ImGui::GetWindowDrawList()->AddRect(ImVec2(t.min.x + (t.max.x - t.min.x) * (std::min)(a.x, b.x), t.min.y + (t.max.y - t.min.y) * (std::min)(a.y, b.y)),
                                            ImVec2(t.min.x + (t.max.x - t.min.x) * (std::max)(a.x, b.x), t.min.y + (t.max.y - t.min.y) * (std::max)(a.y, b.y)),
                                            IM_COL32(255, 255, 255, 255));
      }
    } else if (!ImGui::IsMouseDown(0)) {
      dragging = false;
    }
    if (!freeze) ImGui::TextDisabled("tick Freeze to draw rectangles");
    ImGui::Text("%zu pane(s)", rects.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Undo last") && !rects.empty()) rects.pop_back();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) rects.clear();
    ImGui::BeginDisabled(!pick.has_island || !pick.texture[0]);
    if (ImGui::Button("Auto (dark cells in the island)")) {
      float out[kEmitMaxRects][4];
      const int n = rbrrtx_emit_auto_windows(pick.hash[0], pick.texture[0], pick.uv_rect, out, kEmitMaxRects);
      for (int i = 0; i < n && rects.size() < kEmitMaxRects; ++i) rects.push_back({ out[i][0], out[i][1], out[i][2], out[i][3] });
      char b[100];
      std::snprintf(b, sizeof(b), "auto: %d pane(s) found", n);
      status(b);
    }
    ImGui::EndDisabled();
    ImGui::SliderInt("lit fraction (%)", &lit, 0, 100);
    ImGui::SameLine();
    if (ImGui::SmallButton("Re-roll")) seed = seed * 1664525u + 1013904223u;
    ImGui::SliderFloat("warmth (K)", &kelvin, 2000.0f, 4500.0f, "%.0f");
    ImGui::SliderFloat("intensity", &intensity, 0.5f, 40.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::RadioButton("at night", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("always", &mode, 1);
    ImGui::BeginDisabled(rects.empty());
    if (ImGui::Button("Apply windows")) {
      RbrEmit e = {};
      e.tex = pick.hash[0];
      e.windows = 1;
      e.mode = mode;
      e.intensity = intensity;
      kelvin_rgb(kelvin, e.rgb);
      e.litPercent = lit;
      e.seed = seed;
      e.nrects = static_cast<int>((std::min)(rects.size(), static_cast<size_t>(kEmitMaxRects)));
      for (int i = 0; i < e.nrects; ++i) std::memcpy(e.rects[i], rects[i].data(), sizeof(float) * 4);
      rbrrtx_emit_set(e);
      status("windows applied (Save keeps them); they glow after the generated mod has reloaded, about 10 s");
    }
    ImGui::EndDisabled();
  }
  ImGui::SeparatorText("Window textures");
  RbrEmit e = {};
  for (int i = 0, n = rbrrtx_emit_count(); i < n; ++i) {
    if (!rbrrtx_emit_get(i, &e) || !e.windows) continue;
    ImGui::PushID(i);
    ImGui::Text("0x%016llX  %d pane(s), %d %% lit, %s", e.tex, e.nrects, e.litPercent, e.mode ? "always" : "night");
    ImGui::SameLine();
    const bool rm = ImGui::SmallButton("Remove");
    ImGui::PopID();
    if (rm) { rbrrtx_emit_remove(e.tex); s_tex = 0; break; }
  }
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
  rbrrtx_menu_add_page("Lights", page_lights, nullptr);
  rbrrtx_menu_add_page("Emissive", page_emissive, nullptr);
  rbrrtx_menu_add_page("Windows", page_windows, nullptr);
}
