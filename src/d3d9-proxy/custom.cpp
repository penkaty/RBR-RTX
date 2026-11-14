// custom.cpp - user-made lights and emissives, made in the F3 menu (menu_custom.cpp page "Lamps").
//
// Lights: Remix API sphere lights at world positions (placed at the crosshair hit or in front of the camera), kept per stage
// in menu.ini [lights.<stage id>] (the menu = lights.menu): colour, lumens, radius, flicker (flares, fires), at night or always.
// Stage positions are absolute, so a light stays where it was put on that stage.
//
// Emissives: the texture of a prop (or a rectangle of it) made emissive, keyed by the Remix hash of the game texture, so every
// draw with it glows. Window category: rectangles over the panes of a facade texture; the mask gets a random lit fraction and a
// random brightness per pane baked in, so a wall shows a believable mix of lit and dark windows (every building with that
// texture shows the same ones: texture-keyed, no per-instance variation without new assets). Same mechanism as the marked
// lamps' glass (worldlights.cpp): the game's texture is saved next to the mod (stages are encrypted), a 16x16 twin texture
// with a hash of its own is drawn instead of the game's while the emissive is on, and the generated mod (genmod.cpp, section
// "emissive") gives that twin hash the saved texture as albedo plus the mask as emissive mask. Night mode fades in with the
// street lamps in four steps (rbrrtx_world_lights_night_fraction). menu.ini [emissive] / [windows]: one line per emissive.
// A texture can carry several emissives, each with its own colour, strength, mode and light (user 2026-11-09). Remix gives a
// texture one material, so they share its twin and mask: the mask holds each one's colour and relative strength, and when some
// are "at night" and others "always" each on-level has a mask of its own.
// Texture variants (texvariants.cpp): the same object in another texture set (dry / damp / wet) takes the emissives of the
// texture they were made on, with its own saved texture, masks and twin, so it glows in its own colours.
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include "custom.hpp"
#include "texvariants.hpp"
#include <d3d9.h>
#include "third_party/remix_c.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

#define clog2(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

const remixapi_Interface* g_api = nullptr;

// ------------------------------------------------------------------------------------------------ lights
struct CL {
  RbrCustomLight d = {};
  remixapi_LightHandle h = nullptr;
  float sentK = -1.0f;
  float sentPos[3] = {};
};
std::map<int, std::vector<CL>> g_lights;              // stage key (-1 menu) -> lights
int g_stage = -2;
int g_nextId = 1;

std::vector<CL>& cur() { return g_lights[g_stage]; }

void destroy_light(CL& l) {
  if (l.h && g_api && g_api->DestroyLight) g_api->DestroyLight(l.h);
  l.h = nullptr;
  l.sentK = -1.0f;
}

void destroy_stage(int key) {
  const auto it = g_lights.find(key);
  if (it == g_lights.end()) return;
  for (CL& l : it->second) destroy_light(l);
}

// Smooth random dips in 0..1 for flicker (two incommensurate sines and a fast one per light).
float flicker_noise(int id, double t) {
  const double a = std::sin(t * 13.1 + id * 1.7), b = std::sin(t * 5.3 + id * 0.9), c = std::sin(t * 29.0 + id * 3.1);
  return static_cast<float>(std::clamp(0.5 + 0.28 * a * b + 0.22 * c * a, 0.0, 1.0));
}

// ------------------------------------------------------------------------------------------------ emissives
constexpr int kLevels = 4;
constexpr ULONGLONG kSettle = 9000, kHold = 1000;

struct EmitState {
  bool tried = false, ok = false;
  std::wstring dds, mask[kLevels + 1];                 // per on-level; level 0 = daylight, only when "at night" and "always" are mixed
  ULONGLONG readyAt = 0;
  IDirect3DTexture9* twin[kLevels + 1] = {};
  int lvHeld = -1;
  ULONGLONG lowerSince = 0;
  unsigned w = 0, h = 0;
};
std::map<uint64_t, std::vector<RbrEmit>> g_emit;     // texture -> its emissives, oldest first
int g_nextEmit = 1;
std::map<uint64_t, EmitState> g_state;
std::set<uint64_t> g_drawnThisStage;                   // F3 lists: emissive textures drawn since the stage loaded

// The emissives a texture shows: its own, or those of the texture it is a variant of. nullptr = none.
const std::vector<RbrEmit>* emits_for(uint64_t tex) {
  auto it = g_emit.find(tex);
  if (it == g_emit.end()) {
    const uint64_t c = rbrrtx_texvar_canonical(tex);
    if (c != tex) it = g_emit.find(c);
  }
  return it == g_emit.end() || it->second.empty() ? nullptr : &it->second;
}

std::wstring emit_dir() {
  const wchar_t* d = rbrrtx_genmod_dir();
  return d && d[0] ? std::wstring(d) + L"\\emissive" : std::wstring();
}

std::wstring hex16(uint64_t h) {
  wchar_t b[24];
  swprintf_s(b, L"%016llX", static_cast<unsigned long long>(h));
  return b;
}

uint64_t twin_pixels(uint64_t src, int level, uint32_t px[256]) {
  uint64_t st = src ^ (0x52424545'4D495400ULL + static_cast<uint64_t>(level) * 0x1000193ULL);   // "RBEEMIT"
  for (int i = 0; i < 256; ++i) {
    uint64_t z = (st += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    px[i] = static_cast<uint32_t>(z ^ (z >> 31)) | 0xFF000000u;
  }
  return rbrrtx_xxh3(px, 256 * sizeof(uint32_t));
}

float rand01(unsigned seed, unsigned i, unsigned salt) {
  uint64_t z = (static_cast<uint64_t>(seed) << 32) ^ (static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ULL) ^ salt;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  return static_cast<float>(static_cast<double>(z >> 11) / 9007199254740992.0);
}

// Mip 0 of a saved DDS (DXT1, DXT3, DXT5 or 32-bit A8R8G8B8) as 0xAARRGGBB pixels. Returns false for other formats.
bool read_dds_rgba(const std::wstring& path, std::vector<uint32_t>& out, unsigned& w, unsigned& h) {
  FILE* fp = nullptr;
  if (_wfopen_s(&fp, path.c_str(), L"rb") || !fp) return false;
  uint32_t hdr[32] = {};
  const bool okHdr = fread(hdr, sizeof(hdr), 1, fp) == 1 && hdr[0] == 0x20534444;
  if (!okHdr) { fclose(fp); return false; }
  h = hdr[3];
  w = hdr[4];
  const uint32_t fourcc = hdr[21], bits = hdr[22];
  const int kind = fourcc == 0x31545844 ? 1 : fourcc == 0x33545844 ? 3 : fourcc == 0x35545844 ? 5 : (fourcc == 0 && bits == 32) ? 32 : 0;
  if (!kind || w == 0 || h == 0 || w > 8192 || h > 8192) { fclose(fp); return false; }
  const size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  const size_t bytes = kind == 32 ? static_cast<size_t>(w) * h * 4 : bw * bh * (kind == 1 ? 8 : 16);
  std::vector<uint8_t> d(bytes);
  const bool okData = fread(d.data(), 1, bytes, fp) == bytes;
  fclose(fp);
  if (!okData) return false;
  out.assign(static_cast<size_t>(w) * h, 0);
  if (kind == 32) {
    std::memcpy(out.data(), d.data(), bytes);
    return true;
  }
  auto c565 = [](uint16_t c, int ch) {
    const int v = ch == 0 ? (c >> 11) & 31 : ch == 1 ? (c >> 5) & 63 : c & 31;
    return ch == 1 ? (v * 255 + 31) / 63 : (v * 255 + 15) / 31;
  };
  for (size_t by = 0; by < bh; ++by) {
    for (size_t bx = 0; bx < bw; ++bx) {
      const uint8_t* b = d.data() + (by * bw + bx) * (kind == 1 ? 8 : 16);
      const uint8_t* cb = kind == 1 ? b : b + 8;
      const uint16_t c0 = static_cast<uint16_t>(cb[0] | (cb[1] << 8)), c1 = static_cast<uint16_t>(cb[2] | (cb[3] << 8));
      int pal[4][3];
      for (int ch = 0; ch < 3; ++ch) {
        pal[0][ch] = c565(c0, ch);
        pal[1][ch] = c565(c1, ch);
        if (kind != 1 || c0 > c1) {
          pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
          pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
        } else {
          pal[2][ch] = (pal[0][ch] + pal[1][ch]) / 2;
          pal[3][ch] = 0;
        }
      }
      const uint32_t idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | (static_cast<uint32_t>(cb[7]) << 24);
      int alphas[8] = {};
      uint64_t aidx = 0;
      if (kind == 5) {
        alphas[0] = b[0];
        alphas[1] = b[1];
        for (int i = 2; i < 8; ++i)
          alphas[i] = alphas[0] > alphas[1] ? ((8 - i) * alphas[0] + (i - 1) * alphas[1]) / 7
                    : i < 6 ? ((6 - i) * alphas[0] + (i - 1) * alphas[1]) / 5 : (i == 6 ? 0 : 255);
        for (int i = 0; i < 6; ++i) aidx |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
      }
      for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
          const size_t x = bx * 4 + px, y = by * 4 + py;
          if (x >= w || y >= h) continue;
          const int i = py * 4 + px;
          const int ci = (idx >> (2 * i)) & 3;
          int a = 255;
          if (kind == 1) a = (c0 <= c1 && ci == 3) ? 0 : 255;
          else if (kind == 3) a = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
          else a = alphas[(aidx >> (3 * i)) & 7];
          out[y * w + x] = (static_cast<uint32_t>(a) << 24) | (pal[ci][0] << 16) | (pal[ci][1] << 8) | pal[ci][2];
        }
      }
    }
  }
  return true;
}

bool write_rgba_dds(const std::wstring& path, unsigned W, unsigned H, const std::vector<uint32_t>& px) {
  DWORD hdr[32] = {};
  hdr[0] = 0x20534444; hdr[1] = 124;
  hdr[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;
  hdr[3] = H; hdr[4] = W; hdr[5] = W * 4; hdr[7] = 1;
  hdr[19] = 32; hdr[20] = 0x41; hdr[22] = 32;
  hdr[23] = 0xFF0000; hdr[24] = 0xFF00; hdr[25] = 0xFF; hdr[26] = 0xFF000000u;
  hdr[27] = 0x1000;
  FILE* fp = nullptr;
  if (_wfopen_s(&fp, path.c_str(), L"wb") || !fp) return false;
  fwrite(hdr, sizeof(hdr), 1, fp);
  fwrite(px.data(), 4, px.size(), fp);
  fclose(fp);
  return true;
}

// The solid copy of a mask's texture (same name with _solid_ for _mask_): the saved texture with full alpha wherever the
// mask glows. The material's albedo is this copy: with the game's own texture, the draw's alpha test cut glowing parts away
// where the texture's alpha is low (user 2026-11-07: an emissive lamppost lost its top; the alpha there is ~0, the game
// does not use it as opacity).
std::wstring solid_path(const std::wstring& mask) {
  std::wstring s = mask;
  const size_t at = s.rfind(L"_mask_");
  if (at == std::wstring::npos) return std::wstring();
  s.replace(at, 6, L"_solid_");
  const size_t lv = s.rfind(L'L');                     // a level's mask (see mask_name) shares one solid copy
  if (lv != std::wstring::npos && lv > at && s.size() == lv + 6) s.erase(lv, 2);
  return s;
}

bool emits_mixed(const std::vector<RbrEmit>& v) {
  bool night = false, always = false;
  for (const RbrEmit& e : v) (e.mode == 1 ? always : night) = true;
  return night && always;
}

float emits_peak(const std::vector<RbrEmit>& v) {
  float m = 0.0f;
  for (const RbrEmit& e : v) m = (std::max)(m, e.intensity);
  return m;
}

// The emissive mask of a texture's emissives: white where the texture glows (props), or the lit panes with their brightness
// (windows). Named by its content (Remix does not reload a file it has open). Returns the path ("" = failed).
// With the saved texture readable, each glowing texel takes the texture's own colour, weighted by its brightness
// (user 2026-11-06: emissive lamps came out plain white, windows flat): dark parts of a lamp housing or a window frame
// stay dark, bright glass glows in its own colour. Where emissives overlap, the newest wins.
// level -1: one mask for every level. 0..kLevels: the "at night" emissives at that level's share, the "always" ones in full.
std::wstring write_mask(uint64_t hash, const std::vector<RbrEmit>& list, unsigned tw, unsigned th, const std::wstring& texDds, int level) {
  const std::wstring dir = emit_dir();
  if (dir.empty()) return std::wstring();
  const unsigned W = std::clamp(tw / 2, 256u, 2048u), H = std::clamp(th / 2, 128u, 2048u);
  const float peak = emits_peak(list);
  auto share = [&](const RbrEmit& e) {                  // what the material's one intensity leaves to this emissive
    const float rel = peak > 0.0f ? e.intensity / peak : 0.0f;
    return level >= 0 && e.mode == 0 ? rel * static_cast<float>(level) / kLevels : rel;
  };
  uint32_t key = 2166136261u;
  auto mix = [&](uint32_t v) { key ^= v; key *= 16777619u; };
  std::vector<uint32_t> tex;
  unsigned texW = 0, texH = 0;
  const bool textured = read_dds_rgba(texDds, tex, texW, texH);
  mix(W); mix(H); mix(textured ? 3u : 1u);
  mix(4u);                                             // file version: older masks (noise textures, the old brightness curve) are not reused
  for (const RbrEmit& e : list) {
    mix(e.windows); mix(e.seed); mix(static_cast<uint32_t>(e.litPercent)); mix(static_cast<uint32_t>(e.nrects));
    for (float f : e.rgb) mix(static_cast<uint32_t>(std::lround(f * 1023.0f)));   // the colour is in the mask (below)
    mix(static_cast<uint32_t>(std::lround(share(e) * 4096.0f)));
    for (int i = 0; i < e.nrects; ++i) for (float f : e.rects[i]) mix(static_cast<uint32_t>(std::lround(f * 4096.0f)));
  }
  wchar_t tail[40];
  if (level >= 0) swprintf_s(tail, L"_mask_%08XL%d.dds", key, level);
  else swprintf_s(tail, L"_mask_%08X.dds", key);
  const std::wstring path = dir + L"\\" + hex16(hash) + tail;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
      (!textured || GetFileAttributesW(solid_path(path).c_str()) != INVALID_FILE_ATTRIBUTES)) return path;
  std::vector<uint32_t> px(static_cast<size_t>(W) * H, 0xFF000000u);
  std::vector<uint8_t> covered(static_cast<size_t>(W) * H, 0);
  std::vector<uint8_t> part(static_cast<size_t>(W) * H);
  for (const RbrEmit& e : list) {
    std::fill(part.begin(), part.end(), static_cast<uint8_t>(e.nrects == 0 && !e.windows ? 255 : 0));
    for (int i = 0; i < e.nrects; ++i) {
      unsigned v = 255;
      if (e.windows) {
        if (rand01(e.seed, i, 1) * 100.0f >= static_cast<float>(e.litPercent)) continue;   // a dark pane
        v = static_cast<unsigned>(255.0f * (0.55f + 0.45f * rand01(e.seed, i, 2)));
      }
      const float* r = e.rects[i];
      const int x0 = std::clamp(static_cast<int>(std::floor(r[0] * W)), 0, static_cast<int>(W) - 1), x1 = std::clamp(static_cast<int>(std::ceil(r[2] * W)), 1, static_cast<int>(W));
      const int y0 = std::clamp(static_cast<int>(std::floor(r[1] * H)), 0, static_cast<int>(H) - 1), y1 = std::clamp(static_cast<int>(std::ceil(r[3] * H)), 1, static_cast<int>(H));
      for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) part[static_cast<size_t>(y) * W + x] = static_cast<uint8_t>(v);
    }
    const float k0 = share(e);
    for (unsigned y = 0; y < H; ++y) {
      for (unsigned x = 0; x < W; ++x) {
        const size_t at = static_cast<size_t>(y) * W + x;
        if (!part[at]) continue;
        covered[at] = 1;
        const float v = static_cast<float>(part[at]) / 255.0f;
        float c[3] = { v, v, v };
        if (textured) {
          const uint32_t t = tex[static_cast<size_t>(y * texH / H) * texW + x * texW / W];
          const float r = ((t >> 16) & 0xFF) / 255.0f, g = ((t >> 8) & 0xFF) / 255.0f, b = (t & 0xFF) / 255.0f;
          const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
          // Windows: hue from the texture, brightness 0.35 .. 1 by the texel's own brightness. Lamps (user 2026-11-08): only
          // light grey to white glows, as poles and housings are dark or medium grey and the white-ish texels are the lamp.
          const float k = e.windows ? std::clamp((lum - 0.05f) / 0.45f, 0.0f, 1.0f) : std::clamp((lum - 0.5f) / 0.3f, 0.0f, 1.0f);
          const float curve = k * k * (3.0f - 2.0f * k);
          const float level01 = v * (e.windows ? 0.35f + 0.65f * curve : curve);
          const float top = (std::max)({ r, g, b, 0.05f });
          c[0] = r / top * level01; c[1] = g / top * level01; c[2] = b / top * level01;
        }
        // With a mask set, a USD material's emissive colour is the mask's own: Remix ignores emissive_color_constant (only its API
        // materials can ask for a tint). So the chosen colour goes into the mask; white keeps the texture's hue as before.
        auto ch = [&](int i) { return static_cast<uint32_t>(std::lround(std::clamp(c[i] * e.rgb[i] * k0, 0.0f, 1.0f) * 255.0f)); };
        px[at] = 0xFF000000u | (ch(0) << 16) | (ch(1) << 8) | ch(2);
      }
    }
  }
  // The solid copy: it follows the parts, not which texels end up glowing.
  if (textured && texW && texH) {
    std::vector<uint32_t> solid = tex;
    for (unsigned y = 0; y < texH; ++y) {
      for (unsigned x = 0; x < texW; ++x) {
        if (covered[static_cast<size_t>(y * H / texH) * W + x * W / texW]) solid[static_cast<size_t>(y) * texW + x] |= 0xFF000000u;
      }
    }
    write_rgba_dds(solid_path(path), texW, texH, solid);
  }
  if (!write_rgba_dds(path, W, H, px)) return std::wstring();
  return path;
}

// The masks of a texture from its emissives as they are now. False when one could not be written (the old ones stay).
bool rebuild_masks(uint64_t tex, EmitState& st) {
  const std::vector<RbrEmit>* own = emits_for(tex);
  if (!own) return false;
  const std::vector<RbrEmit>& list = *own;
  if (emits_mixed(list)) {
    std::wstring m[kLevels + 1];
    for (int lv = 0; lv <= kLevels; ++lv) {
      m[lv] = write_mask(tex, list, st.w, st.h, st.dds, lv);
      if (m[lv].empty()) return false;
    }
    for (int lv = 0; lv <= kLevels; ++lv) st.mask[lv] = m[lv];
  } else {
    const std::wstring m = write_mask(tex, list, st.w, st.h, st.dds, -1);
    if (m.empty()) return false;
    for (std::wstring& s : st.mask) s = m;
  }
  return true;
}

// Section "emissive" of the generated mod: one material per on-level for every emissive texture.
void publish() {
  std::string body;
  for (const auto& sv : g_state) {                     // own textures and variants: each one saved its own texture
    const std::vector<RbrEmit>* listp = emits_for(sv.first);
    if (!sv.second.ok || !listp) continue;
    const std::pair<const uint64_t, const std::vector<RbrEmit>&> kv(sv.first, *listp);
    const auto s = &sv;
    const bool mixed = emits_mixed(kv.second);
    const float peak = emits_peak(kv.second);
    const RbrEmit& e = kv.second.front();
    const std::wstring solid = solid_path(s->second.mask[kLevels]);
    const bool haveSolid = !solid.empty() && GetFileAttributesW(solid.c_str()) != INVALID_FILE_ATTRIBUTES;
    const std::string dds = rbrrtx_genmod_asset(haveSolid ? solid : s->second.dds);
    for (int lv = mixed ? 0 : 1; lv <= kLevels; ++lv) {
      const std::string mask = rbrrtx_genmod_asset(s->second.mask[lv]);
      uint32_t px[256];
      const uint64_t th = twin_pixels(kv.first, lv, px);
      char num[200];
      // Mixed modes: the level is in each mask, as only the "at night" emissives follow it.
      snprintf(num, sizeof(num), "                color3f inputs:emissive_color_constant = (%.3f, %.3f, %.3f)\n"
                                 "                float inputs:emissive_intensity = %.3f\n", e.rgb[0], e.rgb[1], e.rgb[2], mixed ? peak : peak * lv / kLevels);
      const std::string in =
          "                asset inputs:diffuse_texture = @" + dds + "@ (\n"
          "                    colorSpace = \"auto\"\n"
          "                )\n"
          // Matte: the texture is often an atlas shared with ground or walls, and without a value the whole of it takes
          // Remix's default roughness 0.5 (user 2026-11-13: a shiny grass patch in the menu, twin 34ED98A2444D100D).
          "                float inputs:reflection_roughness_constant = 0.9\n"
          "                float inputs:metallic_constant = 0\n"
          "                bool inputs:enable_emission = 1\n"
          "                asset inputs:emissive_mask_texture = @" + mask + "@ (\n"
          "                    colorSpace = \"auto\"\n"
          "                )\n" + num;
      char c[96];
      snprintf(c, sizeof(c), "%zu emissive(s), texture %016llX, on %d/%d:", kv.second.size(), static_cast<unsigned long long>(kv.first), lv, kLevels);
      body += rbrrtx_genmod_material(th, std::string(c) + " " + dds, in);
    }
  }
  rbrrtx_genmod_set("emissive", body);
}

}  // namespace

// An emission mask from a DDS: its brightness in a near-white tint, so the picture's pattern stays and its colour does not.
bool rbrrtx_dds_neutral_mask(const wchar_t* src, const wchar_t* dst) {
  std::vector<uint32_t> px;
  unsigned w = 0, h = 0;
  if (!read_dds_rgba(src, px, w, h)) return false;
  for (uint32_t& p : px) {
    // Glass pictures are mostly dark (the E30's about 43 of 255): the floor makes sure the whole pane glows, the rest keeps the pattern.
    const float lum = 140.0f + 115.0f / 255.0f * (0.2126f * ((p >> 16) & 0xFF) + 0.7152f * ((p >> 8) & 0xFF) + 0.0722f * (p & 0xFF));
    auto ch = [&](float tint) { return static_cast<uint32_t>(std::lround(std::clamp(lum * tint, 0.0f, 255.0f))); };
    p = 0xFF000000u | (ch(1.0f) << 16) | (ch(0.98f) << 8) | ch(0.94f);
  }
  return write_rgba_dds(dst, w, h, px);
}

// An emission mask of a DDS's bright parts only: a head lamp's reflectors, bulbs and LED strips are painted bright, its housing
// and trim are not, so the lamps glow and the lamp unit around them stays dark (user 2026-11-13, Yaris Rally2: "the headlight
// body is getting lit not the actual lamps inside it").
bool rbrrtx_dds_bright_mask(const wchar_t* src, const wchar_t* dst) {
  std::vector<uint32_t> px;
  unsigned w = 0, h = 0;
  if (!read_dds_rgba(src, px, w, h)) return false;
  for (uint32_t& p : px) {
    const float v = static_cast<float>((std::max)((std::max)((p >> 16) & 0xFF, (p >> 8) & 0xFF), p & 0xFF)) / 255.0f;
    const float t = std::clamp((v - 0.45f) / 0.40f, 0.0f, 1.0f), m = t * t * (3.0f - 2.0f * t) * 255.0f;
    auto ch = [&](float tint) { return static_cast<uint32_t>(std::lround(std::clamp(m * tint, 0.0f, 255.0f))); };
    p = 0xFF000000u | (ch(1.0f) << 16) | (ch(0.96f) << 8) | ch(0.88f);
  }
  return write_rgba_dds(dst, w, h, px);
}

void rbrrtx_custom_init(const remixapi_Interface* api) { g_api = api; }

// ---- lights
int rbrrtx_custom_light_count() { return static_cast<int>(cur().size()); }

bool rbrrtx_custom_light_get(int i, RbrCustomLight* out) {
  if (i < 0 || i >= static_cast<int>(cur().size()) || !out) return false;
  *out = cur()[i].d;
  return true;
}

int rbrrtx_custom_light_add(const float pos[3], const float rgb[3]) {
  CL l;
  l.d.id = g_nextId++;
  std::memcpy(l.d.pos, pos, sizeof(l.d.pos));
  std::memcpy(l.d.rgb, rgb, sizeof(l.d.rgb));
  l.d.lumens = 4000.0f;
  l.d.radius = 0.1f;
  l.d.flicker = 0.0f;
  l.d.mode = 0;
  l.d.dir[1] = -1.0f;
  l.d.softness = 0.15f;
  cur().push_back(l);
  clog2(L"custom light %d added on %S at (%.2f %.2f %.2f)", l.d.id, rbrrtx_custom_stage_name(), pos[0], pos[1], pos[2]);
  return l.d.id;
}

void rbrrtx_custom_light_set(const RbrCustomLight& d) {
  for (CL& l : cur())
    if (l.d.id == d.id) { l.d = d; l.sentK = -1.0f; return; }
}

void rbrrtx_custom_light_remove(int id) {
  auto& v = cur();
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i].d.id == id) {
      destroy_light(v[i]);
      v.erase(v.begin() + i);
      clog2(L"custom light %d removed from %S", id, rbrrtx_custom_stage_name());
      return;
    }
}

const char* rbrrtx_custom_stage_name() {
  static char b[24];
  if (g_stage < 0) snprintf(b, sizeof(b), "lights.menu");
  else snprintf(b, sizeof(b), "lights.%d", g_stage);
  return b;
}

// Per Present: the stage's lights (destroyed when the stage changes), drawn or created.
void rbrrtx_custom_frame(int stageKey, float sunElevation, bool on) {
  if (stageKey != g_stage) {
    destroy_stage(g_stage);
    g_stage = stageKey;
    g_drawnThisStage.clear();
  }
  if (!g_api || !g_api->CreateLight || !g_api->DrawLightInstance) return;
  const float night = rbrrtx_world_lights_night_fraction(sunElevation);
  float unitLux = rbrrtx_numos_lux_per_unit();
  if (unitLux <= 0.0f) unitLux = 6400.0f;
  const double t = GetTickCount64() * 0.001;
  for (CL& l : cur()) {
    const RbrCustomLight& d = l.d;
    const float base = d.mode == 1 ? 1.0f : night;
    if (!on || base <= 0.02f) { if (l.h) destroy_light(l); continue; }
    float k = base;
    if (d.flicker > 0.0f) k *= 1.0f - d.flicker * (1.0f - flicker_noise(d.id, t));
    k = std::round(k * 20.0f) / 20.0f;                   // 5 % steps: a re-create per change, not per frame
    const bool isNew = !l.h;
    const bool posMoved = l.sentPos[0] != d.pos[0] || l.sentPos[1] != d.pos[1] || l.sentPos[2] != d.pos[2];
    if (isNew || posMoved || std::fabs(k - l.sentK) > 0.001f) {
      if (!rbrrtx_remix_plus()) destroy_light(l);
      // A cone's lumens leave through its solid angle, as in worldlights.cpp.
      const float solid = d.cone > 0.0f ? 2.0f * 3.14159265f * (1.0f - std::cos(d.cone * 3.14159265f / 180.0f)) : 4.0f * 3.14159265f;
      const float rad = k * d.lumens / solid / (unitLux * 3.14159265f * d.radius * d.radius);
      remixapi_LightInfoSphereEXT sphere = {};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = { d.pos[0], d.pos[1], d.pos[2] };
      sphere.radius = d.radius;
      sphere.shaping_hasvalue = d.cone > 0.0f;
      sphere.shaping_value.direction = { d.dir[0], d.dir[1], d.dir[2] };
      sphere.shaping_value.coneAngleDegrees = d.cone;
      sphere.shaping_value.coneSoftness = d.softness;
      sphere.shaping_value.focusExponent = 0.0f;
      sphere.volumetricRadianceScale = 1.0f;
      remixapi_LightInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      info.pNext = &sphere;
      info.hash = 0x52425543'4C000000ULL + static_cast<uint64_t>(d.id);   // "RBUCL" + id: one Remix light per custom light
      info.radiance = { d.rgb[0] * rad, d.rgb[1] * rad, d.rgb[2] * rad };
      const remixapi_LightHandle old = l.h;
      if (g_api->CreateLight(&info, &l.h) != REMIXAPI_ERROR_CODE_SUCCESS) { l.h = old; destroy_light(l); continue; }
      std::memcpy(l.sentPos, d.pos, sizeof(l.sentPos));
      l.sentK = k;
    }
    g_api->DrawLightInstance(l.h);
  }
}

// ---- emissives
// The emissive's light (worldlights.cpp lamp kind keyed by its texture), or none.
void push_light(const RbrEmit& e, bool remove = false) {
  // The emissive's rectangles go along: a lamp only on the vertex groups drawn with that part of the texture, not one at
  // the centre of every mesh sharing the texture (run 20261107-051117: a lamppost's light under every barrier post).
  rbrrtx_world_lights_set_emit(e.id, e.tex, e.rgb, remove ? 0.0f : e.lumens, e.cone, e.softness, e.dir, e.aim, e.aimAt, e.mode == 1, e.offset,
                               e.windows ? nullptr : e.rects, e.windows ? 0 : e.nrects, e.anchor);
}

int rbrrtx_emit_count() {
  size_t n = 0;
  for (const auto& kv : g_emit) n += kv.second.size();
  return static_cast<int>(n);
}

bool rbrrtx_emit_get(int i, RbrEmit* out) {
  if (i < 0 || !out) return false;
  for (const auto& kv : g_emit) {
    if (static_cast<size_t>(i) < kv.second.size()) { *out = kv.second[i]; return true; }
    i -= static_cast<int>(kv.second.size());
  }
  return false;
}

namespace {
RbrEmit* find_emit_id(int id) {
  for (auto& kv : g_emit)
    for (RbrEmit& e : kv.second) if (e.id == id) return &e;
  return nullptr;
}
}  // namespace

void rbrrtx_emit_set(const RbrEmit& in) {
  if (!in.tex) return;
  RbrEmit* d = in.id ? find_emit_id(in.id) : nullptr;
  if (!d) {
    g_emit[in.tex].push_back(in);
    d = &g_emit[in.tex].back();
    d->id = in.id ? in.id : g_nextEmit++;
  } else {
    *d = in;
  }
  const RbrEmit e = *d;
  for (auto& sv : g_state)                             // the texture and its variants: the masks follow the edit; the twin
    if (sv.first == e.tex || rbrrtx_texvar_canonical(sv.first) == e.tex)   // comes back after the mod reloads
      if (sv.second.ok && rebuild_masks(sv.first, sv.second)) sv.second.readyAt = GetTickCount64() + kSettle;
  publish();
  push_light(e);
  clog2(L"emissive %d on %016llX set: %s, %s, intensity %.2f, colour %.2f %.2f %.2f, %d rectangle(s)%s", e.id, e.tex, e.windows ? L"windows" : L"prop",
        e.mode ? L"always" : L"at night", e.intensity, e.rgb[0], e.rgb[1], e.rgb[2], e.nrects, e.windows ? L" (lit fraction baked into the mask)" : L"");
  if (e.lumens > 0.0f)
    clog2(L"emissive %d casts light: %.0f lm, %s %.0f deg, %s (%.2f %.2f %.2f)", e.id, e.lumens, e.cone > 0.0f ? L"cone" : L"omni", e.cone,
          e.aim ? L"aimed at" : L"direction", e.aim ? e.aimAt[0] : e.dir[0], e.aim ? e.aimAt[1] : e.dir[1], e.aim ? e.aimAt[2] : e.dir[2]);
}

void rbrrtx_emit_set_light(const RbrEmit& e) {
  RbrEmit* found = find_emit_id(e.id);
  if (!found) return;
  RbrEmit& d = *found;
  d.lumens = e.lumens;
  d.cone = e.cone;
  d.softness = e.softness;
  std::memcpy(d.dir, e.dir, sizeof(d.dir));
  d.aim = e.aim;
  std::memcpy(d.aimAt, e.aimAt, sizeof(d.aimAt));
  std::memcpy(d.offset, e.offset, sizeof(d.offset));   // the list's offset sliders were dropped here before 2026-11-07
  d.anchor = e.anchor;
  push_light(d, d.lumens <= 0.0f);
  clog2(L"emissive %d light: %.0f lm, cone %.0f deg, %s", e.id, d.lumens, d.cone, d.aim ? L"aimed at a point" : L"along a direction");
}

void rbrrtx_emit_remove(int id) {
  const RbrEmit* found = find_emit_id(id);
  if (!found) return;
  const RbrEmit e = *found;
  if (e.lumens > 0.0f) push_light(e, true);
  std::vector<RbrEmit>& list = g_emit[e.tex];
  list.erase(std::remove_if(list.begin(), list.end(), [&](const RbrEmit& x) { return x.id == id; }), list.end());
  if (list.empty()) {
    g_emit.erase(e.tex);
  } else {
    for (auto& sv : g_state)                           // the texture and its variants
      if (sv.first == e.tex || rbrrtx_texvar_canonical(sv.first) == e.tex)
        if (sv.second.ok && rebuild_masks(sv.first, sv.second)) sv.second.readyAt = GetTickCount64() + kSettle;
  }
  publish();
  clog2(L"emissive %d on %016llX removed", id, e.tex);
}

bool rbrrtx_emit_wanted() { return !g_emit.empty(); }
bool rbrrtx_emit_has(unsigned long long tex) { return g_emit.count(tex) != 0; }   // its own emissives (texture variants: an entry)
bool rbrrtx_emit_on_stage(unsigned long long tex) { return g_drawnThisStage.count(tex) != 0 || rbrrtx_texture_on_stage(tex); }

// A draw with an emissive texture: its twin for the lit level (nullptr = draw the game's texture).
void* rbrrtx_emit_twin(void* device, unsigned long long hash, void* baseTexture, float night) {
  const std::vector<RbrEmit>* listp = emits_for(hash);     // a variant: the emissives of its texture, its own state below
  if (!listp) return nullptr;
  g_drawnThisStage.insert(hash);
  const std::vector<RbrEmit>& list = *listp;
  const bool mixed = emits_mixed(list), always = list.front().mode == 1;   // not mixed: all as the first
  if (!mixed && !always && night <= 0.02f) return nullptr;
  EmitState& st = g_state[hash];
  if (!st.tried) {
    st.tried = true;
    const std::wstring dir = emit_dir();
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    st.dds = dir + L"\\" + hex16(hash) + L".dds";
    D3DSURFACE_DESC desc = {};
    IDirect3DTexture9* bt = static_cast<IDirect3DTexture9*>(baseTexture);
    if (bt && SUCCEEDED(bt->lpVtbl->GetLevelDesc(bt, 0, &desc))) { st.w = desc.Width; st.h = desc.Height; }
    const bool have = rbrrtx_dds_is_texture(st.dds.c_str(), hash);   // false too for a noise file of an older build: saved anew
    if (!dir.empty() && st.w && (have || rbrrtx_save_texture_dds_mips(baseTexture, st.dds.c_str()))) {
      st.ok = rebuild_masks(hash, st);
    }
    st.readyAt = GetTickCount64() + kSettle;
    clog2(L"emissive %016llX (%zu of them): texture %s %s%s", hash, list.size(), st.ok ? L"saved" : L"FAILED to save", st.dds.c_str(),
          st.ok ? L"; it glows once the generated mod has reloaded" : L"");
    if (st.ok) publish();
  }
  const ULONGLONG now = GetTickCount64();
  if (!st.ok || now < st.readyAt) return nullptr;
  const int nightLevel = std::clamp(static_cast<int>(std::ceil(night * kLevels - 0.001f)), 1, kLevels);
  const int want = mixed ? (night <= 0.02f ? 0 : nightLevel) : always ? kLevels : nightLevel;
  if (st.lvHeld < 0 || want > st.lvHeld) { st.lvHeld = want; st.lowerSince = 0; }
  else if (want < st.lvHeld) {                         // up at once, down after a second (a sun sweeping through dusk)
    if (!st.lowerSince) st.lowerSince = now;
    else if (now - st.lowerSince >= kHold) { st.lvHeld = want; st.lowerSince = 0; }
  } else {
    st.lowerSince = 0;
  }
  IDirect3DTexture9*& t = st.twin[st.lvHeld];
  if (!t) {
    IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
    uint32_t px[256];
    twin_pixels(hash, st.lvHeld, px);
    if (SUCCEEDED(dev->lpVtbl->CreateTexture(dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) && t) {
      D3DLOCKED_RECT lr = {};
      if (SUCCEEDED(t->lpVtbl->LockRect(t, 0, &lr, nullptr, 0)) && lr.pBits) {
        for (int r = 0; r < 16; ++r) std::memcpy(static_cast<char*>(lr.pBits) + r * lr.Pitch, px + r * 16, 64);
        t->lpVtbl->UnlockRect(t, 0);
      } else {
        t->lpVtbl->Release(t);
        t = nullptr;
      }
    }
  }
  return t;
}

// ---- menu.ini sections
// [lights.<stage>] l<id> = x, y, z, r, g, b, lumens, radius, flicker, mode[, cone, softness, dx, dy, dz]   (older lines: spheres)
// [emissive] / [windows] <hex16>[_<n>] = mode, intensity, r, g, b, litPercent, seed, u0 v0 u1 v1; u0 v0 u1 v1; ...
//   [| light=lumens, cone, softness, dx, dy, dz, aim, ax, ay, az]   (an emissive prop that casts light)
void rbrrtx_custom_ini_set(const char* section, const char* key, const char* value) {
  if (!section || !key || !value) return;
  if (!std::strncmp(section, "lights.", 7)) {
    const int stage = !std::strcmp(section + 7, "menu") ? -1 : std::atoi(section + 7);
    CL l;
    l.d.id = g_nextId++;
    int mode = 0;
    l.d.dir[1] = -1.0f;
    l.d.softness = 0.15f;
    if (sscanf_s(value, " %f , %f , %f , %f , %f , %f , %f , %f , %f , %d , %f , %f , %f , %f , %f", &l.d.pos[0], &l.d.pos[1], &l.d.pos[2], &l.d.rgb[0], &l.d.rgb[1],
                 &l.d.rgb[2], &l.d.lumens, &l.d.radius, &l.d.flicker, &mode, &l.d.cone, &l.d.softness, &l.d.dir[0], &l.d.dir[1], &l.d.dir[2]) < 10) return;
    l.d.mode = mode;
    g_lights[stage].push_back(l);
  } else if (!std::strcmp(section, "emissive") || !std::strcmp(section, "windows")) {
    RbrEmit e = {};
    e.tex = std::strtoull(key, nullptr, 16);
    e.windows = !std::strcmp(section, "windows");
    const char* p = value;
    int n = 0;
    if (!e.tex || sscanf_s(p, " %d , %f , %f , %f , %f , %d , %u , %n", &e.mode, &e.intensity, &e.rgb[0], &e.rgb[1], &e.rgb[2], &e.litPercent, &e.seed, &n) < 7) return;
    p += n;
    while (*p && e.nrects < kEmitMaxRects) {
      float* r = e.rects[e.nrects];
      int used = 0;
      if (sscanf_s(p, " %f %f %f %f ;%n", &r[0], &r[1], &r[2], &r[3], &used) < 4) break;
      ++e.nrects;
      p += used;
    }
    e.softness = 0.2f;
    e.dir[1] = -1.0f;
    if (const char* lp = std::strstr(value, "| light=")) {
      sscanf_s(lp + 8, " %f , %f , %f , %f , %f , %f , %d , %f , %f , %f , %f , %f , %f , %d", &e.lumens, &e.cone, &e.softness, &e.dir[0], &e.dir[1], &e.dir[2], &e.aim,
               &e.aimAt[0], &e.aimAt[1], &e.aimAt[2], &e.offset[0], &e.offset[1], &e.offset[2], &e.anchor);   // offset, anchor: since 2026-11-07, older lines lack them
    }
    e.id = g_nextEmit++;
    g_emit[e.tex].push_back(e);                        // one line per emissive: <hex16>, then <hex16>_2, _3 for the same texture
    g_state[e.tex];
    if (e.lumens > 0.0f) push_light(e);
  }
}

int rbrrtx_custom_ini_dump(void (*emit)(const char* line, void* user), void* user) {
  int items = 0;
  char b[4096];
  for (const auto& st : g_lights) {
    if (st.second.empty()) continue;
    snprintf(b, sizeof(b), st.first < 0 ? "[lights.menu]" : "[lights.%d]", st.first);
    emit(b, user);
    int i = 0;
    for (const CL& l : st.second) {
      const RbrCustomLight& d = l.d;
      char key[32];
      snprintf(key, sizeof(key), "l%d", ++i);
      snprintf(b, sizeof(b), "%s=%.3f, %.3f, %.3f, %.4f, %.4f, %.4f, %.1f, %.3f, %.2f, %d, %.1f, %.2f, %.4f, %.4f, %.4f", key, d.pos[0], d.pos[1], d.pos[2], d.rgb[0], d.rgb[1], d.rgb[2],
               d.lumens, d.radius, d.flicker, d.mode, d.cone, d.softness, d.dir[0], d.dir[1], d.dir[2]);
      emit(b, user);
      ++items;
    }
    emit("", user);
  }
  for (int pass = 0; pass < 2; ++pass) {
    bool head = false;
    for (const auto& kv : g_emit) {
      int nth = 0;
      for (const RbrEmit& e : kv.second) {
      if ((e.windows != 0) != (pass == 1)) continue;
      if (!head) { emit(pass ? "[windows]" : "[emissive]", user); head = true; }
      char key[40];
      if (++nth == 1) snprintf(key, sizeof(key), "%016llX", static_cast<unsigned long long>(kv.first));
      else snprintf(key, sizeof(key), "%016llX_%d", static_cast<unsigned long long>(kv.first), nth);
      int len = snprintf(b, sizeof(b), "%s=%d, %.3f, %.4f, %.4f, %.4f, %d, %u, ", key, e.mode, e.intensity, e.rgb[0], e.rgb[1],
                         e.rgb[2], e.litPercent, e.seed);
      for (int i = 0; i < e.nrects && len < static_cast<int>(sizeof(b)) - 80; ++i)
        len += snprintf(b + len, sizeof(b) - len, "%.5f %.5f %.5f %.5f; ", e.rects[i][0], e.rects[i][1], e.rects[i][2], e.rects[i][3]);
      if (e.lumens > 0.0f && len < static_cast<int>(sizeof(b)) - 160)
        snprintf(b + len, sizeof(b) - len, "| light=%.0f, %.1f, %.2f, %.4f, %.4f, %.4f, %d, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %d", e.lumens, e.cone, e.softness, e.dir[0], e.dir[1],
                 e.dir[2], e.aim, e.aimAt[0], e.aimAt[1], e.aimAt[2], e.offset[0], e.offset[1], e.offset[2], e.anchor);
      emit(b, user);
      ++items;
      }
    }
    if (head) emit("", user);
  }
  return items;
}
