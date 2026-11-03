// menu_custom.cpp - F3 pages "Lights", "Emissive" and "Windows" (custom.cpp holds the data and the rendering side).
//
// Lights: Remix sphere lights at the crosshair hit point or in front of the camera, with colour, lumens, radius, flicker and
// night / always; kept per stage in menu.ini (Save). Emissive: the picked draw's texture (whole or the highlighted island)
// glows with a colour and intensity. Windows: rectangles over the panes of a facade texture, drawn by dragging in the
// thumbnail or found automatically from the luminance of the island; the mask gets a random lit fraction baked in.
#include "menu.hpp"
#include "../custom.hpp"

#pragma warning(push, 0)
#include "../third_party/imgui/imgui.h"
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// ---- Emissive (props) ------------------------------------------------------------------------------------------------
void page_emissive(void*) {
  ImGui::TextWrapped("Make the texture of a prop glow. Aim the middle of the screen at it; the yellow box is the part of the texture "
                     "around the hit point. Whole texture or just that part, a colour, an intensity, at night (fading in with the street "
                     "lamps) or always. Every draw with that texture glows. Window facades: use the Windows page.");
  static bool freeze = false, island_only = true;
  static float rgb[3] = { 1.0f, 0.75f, 0.45f }, intensity = 6.0f, kelvin = 3000.0f;
  static int mode = 0;
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

}  // namespace

void rbrrtx_carpaint_register_page();

extern "C" void rbrrtx_menu_register_custom_pages(void) {
  rbrrtx_carpaint_register_page();
  rbrrtx_menu_add_page("Lights", page_lights, nullptr);
  rbrrtx_menu_add_page("Emissive", page_emissive, nullptr);
  rbrrtx_menu_add_page("Windows", page_windows, nullptr);
}
