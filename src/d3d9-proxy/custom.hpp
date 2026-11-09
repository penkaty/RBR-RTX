// custom.hpp - user-made lights and emissives (F3 pages "Lights" and "Windows"), implemented in custom.cpp.
// Persisted by the menu in menu.ini: [lights.<stage id>] (menu = lights.menu), [emissive], [windows].
#pragma once
#include <cstddef>

constexpr int kEmitMaxRects = 64;

// A Remix API sphere light at a world position, kept per stage. mode 0 = at night (sun below the lamps' threshold, fading in),
// 1 = always. flicker 0..1: how deep the random dips go (flares, fires).
struct RbrCustomLight {
  int id;
  float pos[3];
  float rgb[3];                      // linear colour
  float lumens, radius, flicker;
  int mode;
  float cone, softness;              // cone half angle in degrees, 0 = lit every way (a sphere); edge softness 0..1
  float dir[3];                      // where a cone points
};

// An emissive texture (keyed by the Remix hash of the game texture, so every draw with it glows): the whole texture or
// rectangles of it (u0, v0, u1, v1). windows = 1: the rectangles are window panes, each lit by chance (litPercent) with a
// random brightness baked into the mask. mode 0 = at night (fading in with the lamps), 1 = always.
struct RbrEmit {
  unsigned long long tex;
  int windows;
  int mode;
  float rgb[3];
  float intensity;
  int litPercent;
  unsigned seed;
  int nrects;
  float rects[kEmitMaxRects][4];
  // Cast light (user 2026-11-05, stadium floodlights): every mesh drawn with the texture also gets a Remix light at its
  // centre (worldlights.cpp), in the emissive's colour and mode. lumens 0 = none. cone > 0 = a spot of that half angle
  // (degrees) along dir, or toward aimAt when aim = 1; cone 0 = lit every way.
  float lumens;
  float cone, softness;
  float dir[3];
  int aim;
  float aimAt[3];
  float offset[3];                                     // metres added to every lamp of the texture (F3 sliders)
  int anchor;                                          // where in the picked part the lamp sits: 0 top, 1 centre, 2 bottom
};

int rbrrtx_custom_light_count();                       // lights of the current stage
bool rbrrtx_custom_light_get(int i, RbrCustomLight* out);
int rbrrtx_custom_light_add(const float pos[3], const float rgb[3]);   // returns the new id
void rbrrtx_custom_light_set(const RbrCustomLight& l);
void rbrrtx_custom_light_remove(int id);
const char* rbrrtx_custom_stage_name();                // "lights.478" / "lights.menu"

int rbrrtx_emit_count();
bool rbrrtx_emit_get(int i, RbrEmit* out);
void rbrrtx_emit_set(const RbrEmit& e);                // add or replace by texture
void rbrrtx_emit_remove(unsigned long long tex);
void rbrrtx_emit_set_light(const RbrEmit& e);          // only the light fields of an existing emissive (no mod rewrite)
// Window panes found automatically inside `island` (u0, v0, u1, v1) of the texture: dark / glassy rectangular cells by luminance.
// `texture` is the game's IDirect3DTexture9 (its mip chain is saved next to the mod first). Returns the number written to out.
int rbrrtx_emit_auto_windows(unsigned long long tex, void* texture, const float island[4], float out[][4], int max);
