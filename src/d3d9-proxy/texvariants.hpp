// texvariants.hpp - texture variants: one stage object, several textures (implemented in texvariants.cpp).
//
// RBR stages carry a dry, a damp and a wet texture set and load the one of the track condition; the game may also convert
// a texture at load (a dry DXT5 drawn as DXT3). Every entry keyed by a texture hash (F3 emissives, lamps and tags, the
// Alt+X hash lists in rtx.conf / user.conf) then held only for the set it was made on (Sardian Night street lamps, rc1
// test 2026-11-14). The geometry is the same in every set: a draw's vertex positions in the world. So the draws of a
// texture that carries an entry leave their geometry signatures, and a draw with another texture and a known signature
// makes that texture a variant of the first one. Never while both are drawn in one scene (then they are two textures
// of one object, as a multi-pass draw has).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

void rbrrtx_texvar_load();                                   // texture-variants.txt (RBRRTX_TEXVARIANTS), once
void rbrrtx_texvar_frame(int sceneKey);                      // per Present: a new scene (stage / menu), saving when due
uint64_t rbrrtx_texvar_canonical(uint64_t h);                // the texture h stands for (h itself when it is no variant)
// One scenery draw with texture h. watched: h carries an entry of the user. sig(ctx) = the draw's geometry signature,
// called only when needed (0 = not readable).
void rbrrtx_texvar_draw(uint64_t h, bool watched, uint64_t (*sig)(void*), void* ctx);
unsigned rbrrtx_texvar_generation();                         // changes when a variant comes or goes
void rbrrtx_texvar_aliases(std::vector<std::pair<uint64_t, uint64_t>>* out);   // variant, canonical
std::wstring rbrrtx_texvar_status();                         // one line for the log / reports
