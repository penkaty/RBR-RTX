// custom.cpp - user-made lights and emissives, made in the F3 menu (menu_custom.cpp pages "Lights" and "Windows").
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
// street lamps in four steps (rbrrtx_world_lights_night_fraction). menu.ini [emissive] / [windows]: one line per texture.
#define CINTERFACE
#define REMIX_ALLOW_X86
#include "common.hpp"
#include "custom.hpp"
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
  std::wstring dds, mask;
  ULONGLONG readyAt = 0;
  IDirect3DTexture9* twin[kLevels] = {};
  int lvHeld = 0;
  ULONGLONG lowerSince = 0;
  unsigned w = 0, h = 0;
};
std::map<uint64_t, RbrEmit> g_emit;
std::map<uint64_t, EmitState> g_state;
std::set<uint64_t> g_drawnThisStage;                   // F3 lists: emissive textures drawn since the stage loaded

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
  return s;
}

// The emissive mask: white where the texture glows (props), or the lit panes with their brightness (windows). Named by its
// content (Remix does not reload a file it has open). Returns the path ("" = failed).
// With the saved texture readable, each glowing texel takes the texture's own colour, weighted by its brightness
// (user 2026-11-06: emissive lamps came out plain white, windows flat): dark parts of a lamp housing or a window frame
// stay dark, bright glass glows in its own colour.
std::wstring write_mask(uint64_t hash, const RbrEmit& e, unsigned tw, unsigned th, const std::wstring& texDds) {
  const std::wstring dir = emit_dir();
  if (dir.empty()) return std::wstring();
  const unsigned W = std::clamp(tw / 2, 256u, 2048u), H = std::clamp(th / 2, 128u, 2048u);
  uint32_t key = 2166136261u;
  auto mix = [&](uint32_t v) { key ^= v; key *= 16777619u; };
  std::vector<uint32_t> tex;
  unsigned texW = 0, texH = 0;
  const bool textured = read_dds_rgba(texDds, tex, texW, texH);
  mix(W); mix(H); mix(e.windows); mix(e.seed); mix(static_cast<uint32_t>(e.litPercent)); mix(static_cast<uint32_t>(e.nrects));
  mix(textured ? 3u : 1u);
  for (float f : e.rgb) mix(static_cast<uint32_t>(std::lround(f * 1023.0f)));   // the colour is in the mask (below)
  mix(3u);                                             // file version: older masks (noise textures, the old brightness curve) are not reused
  for (int i = 0; i < e.nrects; ++i) for (float f : e.rects[i]) mix(static_cast<uint32_t>(std::lround(f * 4096.0f)));
  wchar_t tail[40];
  swprintf_s(tail, L"_mask_%08X.dds", key);
  const std::wstring path = dir + L"\\" + hex16(hash) + tail;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
      (!textured || GetFileAttributesW(solid_path(path).c_str()) != INVALID_FILE_ATTRIBUTES)) return path;
  std::vector<uint32_t> px(static_cast<size_t>(W) * H, e.nrects == 0 && !e.windows ? 0xFFFFFFFFu : 0xFF000000u);
  for (int i = 0; i < e.nrects; ++i) {
    unsigned v = 255;
    if (e.windows) {
      if (rand01(e.seed, i, 1) * 100.0f >= static_cast<float>(e.litPercent)) continue;   // a dark pane
      v = static_cast<unsigned>(255.0f * (0.55f + 0.45f * rand01(e.seed, i, 2)));
    }
    const float* r = e.rects[i];
    const int x0 = std::clamp(static_cast<int>(std::floor(r[0] * W)), 0, static_cast<int>(W) - 1), x1 = std::clamp(static_cast<int>(std::ceil(r[2] * W)), 1, static_cast<int>(W));
    const int y0 = std::clamp(static_cast<int>(std::floor(r[1] * H)), 0, static_cast<int>(H) - 1), y1 = std::clamp(static_cast<int>(std::ceil(r[3] * H)), 1, static_cast<int>(H));
    const uint32_t c = 0xFF000000u | (v << 16) | (v << 8) | v;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) px[static_cast<size_t>(y) * W + x] = c;
  }
  // The solid copy first: it follows the part, not which texels end up glowing.
  if (textured && texW && texH) {
    std::vector<uint32_t> solid = tex;
    for (unsigned y = 0; y < texH; ++y) {
      for (unsigned x = 0; x < texW; ++x) {
        if (px[static_cast<size_t>(y * H / texH) * W + x * W / texW] & 0x00FFFFFFu) solid[static_cast<size_t>(y) * texW + x] |= 0xFF000000u;
      }
    }
    write_rgba_dds(solid_path(path), texW, texH, solid);
  }
  if (textured) {
    for (unsigned y = 0; y < H; ++y) {
      for (unsigned x = 0; x < W; ++x) {
        uint32_t& m = px[static_cast<size_t>(y) * W + x];
        const float v = static_cast<float>(m & 0xFF) / 255.0f;
        if (v <= 0.0f) continue;
        const uint32_t t = tex[static_cast<size_t>(y * texH / H) * texW + x * texW / W];
        const float r = ((t >> 16) & 0xFF) / 255.0f, g = ((t >> 8) & 0xFF) / 255.0f, b = (t & 0xFF) / 255.0f;
        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        // Windows: hue from the texture, brightness 0.35 .. 1 by the texel's own brightness. Lamps (user 2026-11-08): only
        // light grey to white glows, as poles and housings are dark or medium grey and the white-ish texels are the lamp.
        const float k = e.windows ? std::clamp((lum - 0.05f) / 0.45f, 0.0f, 1.0f) : std::clamp((lum - 0.5f) / 0.3f, 0.0f, 1.0f);
        const float curve = k * k * (3.0f - 2.0f * k);
        const float level = v * (e.windows ? 0.35f + 0.65f * curve : curve);
        const float peak = (std::max)({ r, g, b, 0.05f });
        auto ch = [&](float c) { return static_cast<uint32_t>(std::lround(std::clamp(c / peak * level, 0.0f, 1.0f) * 255.0f)); };
        m = 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
      }
    }
  }
  // With a mask set, a USD material's emissive colour is the mask's own: Remix ignores emissive_color_constant (only its API
  // materials can ask for a tint). So the chosen colour goes into the mask; white keeps the texture's hue as before.
  for (uint32_t& m : px) {
    auto tint = [&](int shift, float c) {
      return static_cast<uint32_t>(std::lround(std::clamp(static_cast<float>((m >> shift) & 0xFF) * c, 0.0f, 255.0f))) << shift;
    };
    m = 0xFF000000u | tint(16, e.rgb[0]) | tint(8, e.rgb[1]) | tint(0, e.rgb[2]);
  }
  if (!write_rgba_dds(path, W, H, px)) return std::wstring();
  return path;
}

// Section "emissive" of the generated mod: one material per on-level for every saved emissive texture.
void publish() {
  std::string body;
  for (const auto& kv : g_emit) {
    const auto s = g_state.find(kv.first);
    if (s == g_state.end() || !s->second.ok) continue;
    const RbrEmit& e = kv.second;
    const std::wstring solid = solid_path(s->second.mask);
    const bool haveSolid = !solid.empty() && GetFileAttributesW(solid.c_str()) != INVALID_FILE_ATTRIBUTES;
    const std::string dds = rbrrtx_genmod_asset(haveSolid ? solid : s->second.dds), mask = rbrrtx_genmod_asset(s->second.mask);
    for (int lv = 1; lv <= kLevels; ++lv) {
      uint32_t px[256];
      const uint64_t th = twin_pixels(kv.first, lv, px);
      char num[200];
      snprintf(num, sizeof(num), "                color3f inputs:emissive_color_constant = (%.3f, %.3f, %.3f)\n"
                                 "                float inputs:emissive_intensity = %.3f\n", e.rgb[0], e.rgb[1], e.rgb[2], e.intensity * lv / kLevels);
      const std::string in =
          "                asset inputs:diffuse_texture = @" + dds + "@ (\n"
          "                    colorSpace = \"auto\"\n"
          "                )\n"
          "                bool inputs:enable_emission = 1\n"
          "                asset inputs:emissive_mask_texture = @" + mask + "@ (\n"
          "                    colorSpace = \"auto\"\n"
          "                )\n" + num;
      char c[96];
      snprintf(c, sizeof(c), "%s emissive, texture %016llX, on %d/%d:", e.windows ? "window" : "prop", static_cast<unsigned long long>(kv.first), lv, kLevels);
      body += rbrrtx_genmod_material(th, std::string(c) + " " + dds, in);
    }
  }
  rbrrtx_genmod_set("emissive", body);
}

// ---- window panes by luminance: a luminance value per 4x4 block of the saved DXT texture
bool dds_luma(const std::wstring& path, int* bw, int* bh, std::vector<uint8_t>& luma) {
  FILE* fp = nullptr;
  if (_wfopen_s(&fp, path.c_str(), L"rb") || !fp) return false;
  DWORD hdr[32] = {};
  if (fread(hdr, sizeof(hdr), 1, fp) != 1 || hdr[0] != 0x20534444) { fclose(fp); return false; }
  const int w = static_cast<int>(hdr[4]), h = static_cast<int>(hdr[3]);
  const DWORD fcc = hdr[21];
  const int blockBytes = fcc == 0x31545844 ? 8 : (fcc == 0x33545844 || fcc == 0x35545844) ? 16 : 0;   // DXT1 / DXT3 / DXT5
  if (!blockBytes || w < 4 || h < 4 || w > 16384 || h > 16384) { fclose(fp); return false; }
  *bw = (w + 3) / 4;
  *bh = (h + 3) / 4;
  luma.assign(static_cast<size_t>(*bw) * *bh, 0);
  std::vector<uint8_t> row(static_cast<size_t>(*bw) * blockBytes);
  for (int by = 0; by < *bh; ++by) {
    if (fread(row.data(), 1, row.size(), fp) != row.size()) { fclose(fp); return false; }
    for (int bx = 0; bx < *bw; ++bx) {
      const uint8_t* c = row.data() + static_cast<size_t>(bx) * blockBytes + (blockBytes == 16 ? 8 : 0);
      float l = 0;
      for (int k = 0; k < 2; ++k) {
        const unsigned v = c[k * 2] | (c[k * 2 + 1] << 8);
        const float r = ((v >> 11) & 31) / 31.0f, g = ((v >> 5) & 63) / 63.0f, b = (v & 31) / 31.0f;
        l += 0.299f * r + 0.587f * g + 0.114f * b;
      }
      luma[static_cast<size_t>(by) * *bw + bx] = static_cast<uint8_t>(std::clamp(l * 0.5f * 255.0f, 0.0f, 255.0f));
    }
  }
  fclose(fp);
  return true;
}

}  // namespace

// ================================================================================================ public

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
      const float rad = k * d.lumens / (4.0f * 3.14159265f) / (unitLux * 3.14159265f * d.radius * d.radius);
      remixapi_LightInfoSphereEXT sphere = {};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = { d.pos[0], d.pos[1], d.pos[2] };
      sphere.radius = d.radius;
      sphere.shaping_hasvalue = false;
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
  rbrrtx_world_lights_set_emit(e.tex, e.rgb, remove ? 0.0f : e.lumens, e.cone, e.softness, e.dir, e.aim, e.aimAt, e.mode == 1, e.offset,
                               e.windows ? nullptr : e.rects, e.windows ? 0 : e.nrects, e.anchor);
}

int rbrrtx_emit_count() { return static_cast<int>(g_emit.size()); }

bool rbrrtx_emit_get(int i, RbrEmit* out) {
  if (i < 0 || i >= static_cast<int>(g_emit.size()) || !out) return false;
  auto it = g_emit.begin();
  std::advance(it, i);
  *out = it->second;
  return true;
}

void rbrrtx_emit_set(const RbrEmit& e) {
  if (!e.tex) return;
  g_emit[e.tex] = e;
  EmitState& st = g_state[e.tex];
  if (st.ok) {                                         // the mask follows the edit; the twin comes back after the mod reloads
    const std::wstring m = write_mask(e.tex, e, st.w, st.h, st.dds);
    if (!m.empty()) { st.mask = m; st.readyAt = GetTickCount64() + kSettle; }
  }
  publish();
  push_light(e);
  clog2(L"emissive %016llX set: %s, %s, intensity %.2f, colour %.2f %.2f %.2f, %d rectangle(s)%s", e.tex, e.windows ? L"windows" : L"prop",
        e.mode ? L"always" : L"at night", e.intensity, e.rgb[0], e.rgb[1], e.rgb[2], e.nrects, e.windows ? L" (lit fraction baked into the mask)" : L"");
  if (e.lumens > 0.0f)
    clog2(L"emissive %016llX casts light: %.0f lm, %s %.0f deg, %s (%.2f %.2f %.2f)", e.tex, e.lumens, e.cone > 0.0f ? L"cone" : L"omni", e.cone,
          e.aim ? L"aimed at" : L"direction", e.aim ? e.aimAt[0] : e.dir[0], e.aim ? e.aimAt[1] : e.dir[1], e.aim ? e.aimAt[2] : e.dir[2]);
}

void rbrrtx_emit_set_light(const RbrEmit& e) {
  const auto it = g_emit.find(e.tex);
  if (it == g_emit.end()) return;
  RbrEmit& d = it->second;
  d.lumens = e.lumens;
  d.cone = e.cone;
  d.softness = e.softness;
  std::memcpy(d.dir, e.dir, sizeof(d.dir));
  d.aim = e.aim;
  std::memcpy(d.aimAt, e.aimAt, sizeof(d.aimAt));
  std::memcpy(d.offset, e.offset, sizeof(d.offset));   // the list's offset sliders were dropped here before 2026-11-07
  d.anchor = e.anchor;
  push_light(d, d.lumens <= 0.0f);
  clog2(L"emissive %016llX light: %.0f lm, cone %.0f deg, %s", e.tex, d.lumens, d.cone, d.aim ? L"aimed at a point" : L"along a direction");
}

void rbrrtx_emit_remove(unsigned long long tex) {
  const auto it = g_emit.find(tex);
  if (it != g_emit.end() && it->second.lumens > 0.0f) push_light(it->second, true);
  g_emit.erase(tex);
  publish();
  clog2(L"emissive %016llX removed", tex);
}

bool rbrrtx_emit_wanted() { return !g_emit.empty(); }
bool rbrrtx_emit_on_stage(unsigned long long tex) { return g_drawnThisStage.count(tex) != 0 || rbrrtx_texture_on_stage(tex); }

// A draw with an emissive texture: its twin for the lit level (nullptr = draw the game's texture).
void* rbrrtx_emit_twin(void* device, unsigned long long hash, void* baseTexture, float night) {
  const auto it = g_emit.find(hash);
  if (it == g_emit.end()) return nullptr;
  g_drawnThisStage.insert(hash);
  const RbrEmit& e = it->second;
  if (e.mode == 0 && night <= 0.02f) return nullptr;
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
      st.mask = write_mask(hash, e, st.w, st.h, st.dds);
      st.ok = !st.mask.empty();
    }
    st.readyAt = GetTickCount64() + kSettle;
    clog2(L"emissive %016llX (%s): texture %s %s%s", hash, e.windows ? L"windows" : L"prop", st.ok ? L"saved" : L"FAILED to save", st.dds.c_str(),
          st.ok ? L"; it glows once the generated mod has reloaded" : L"");
    if (st.ok) publish();
  }
  const ULONGLONG now = GetTickCount64();
  if (!st.ok || now < st.readyAt) return nullptr;
  const int want = e.mode == 1 ? kLevels : std::clamp(static_cast<int>(std::ceil(night * kLevels - 0.001f)), 1, kLevels);
  if (st.lvHeld == 0 || want > st.lvHeld) { st.lvHeld = want; st.lowerSince = 0; }
  else if (want < st.lvHeld) {                         // up at once, down after a second (a sun sweeping through dusk)
    if (!st.lowerSince) st.lowerSince = now;
    else if (now - st.lowerSince >= kHold) { st.lvHeld = want; st.lowerSince = 0; }
  } else {
    st.lowerSince = 0;
  }
  IDirect3DTexture9*& t = st.twin[st.lvHeld - 1];
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

// Window panes: dark / glassy rectangular cells inside the island, by the luminance of the saved texture's 4x4 blocks.
int rbrrtx_emit_auto_windows(unsigned long long tex, void* texture, const float island[4], float out[][4], int max) {
  const std::wstring dir = emit_dir();
  if (dir.empty() || !texture) return 0;
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring dds = dir + L"\\" + hex16(tex) + L".dds";
  if (!rbrrtx_dds_is_texture(dds.c_str(), tex) && !rbrrtx_save_texture_dds_mips(texture, dds.c_str())) {
    clog2(L"auto windows %016llX: could not save the texture", tex);
    return 0;
  }
  int bw = 0, bh = 0;
  std::vector<uint8_t> L;
  if (!dds_luma(dds, &bw, &bh, L)) { clog2(L"auto windows %016llX: not a DXT1/3/5 texture, no luminance map", tex); return 0; }
  const int x0 = std::clamp(static_cast<int>(island[0] * bw), 0, bw - 1), x1 = std::clamp(static_cast<int>(std::ceil(island[2] * bw)), x0 + 1, bw);
  const int y0 = std::clamp(static_cast<int>(island[1] * bh), 0, bh - 1), y1 = std::clamp(static_cast<int>(std::ceil(island[3] * bh)), y0 + 1, bh);
  const int rw = x1 - x0, rh = y1 - y0;
  if (rw < 4 || rh < 4) return 0;
  double mean = 0, var = 0;
  for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) mean += L[static_cast<size_t>(y) * bw + x];
  mean /= static_cast<double>(rw) * rh;
  for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { const double d = L[static_cast<size_t>(y) * bw + x] - mean; var += d * d; }
  const double sd = std::sqrt(var / (static_cast<double>(rw) * rh)), thr = mean - 0.4 * sd;
  std::vector<char> dark(static_cast<size_t>(rw) * rh), seen(static_cast<size_t>(rw) * rh, 0);
  for (int y = 0; y < rh; ++y) for (int x = 0; x < rw; ++x) dark[static_cast<size_t>(y) * rw + x] = L[static_cast<size_t>(y0 + y) * bw + x0 + x] < thr;
  int n = 0;
  std::vector<int> stack;
  for (int sy = 0; sy < rh && n < max; ++sy)
    for (int sx = 0; sx < rw && n < max; ++sx) {
      const size_t si = static_cast<size_t>(sy) * rw + sx;
      if (!dark[si] || seen[si]) continue;
      int mnx = sx, mxx = sx, mny = sy, mxy = sy, cells = 0;
      stack.assign(1, static_cast<int>(si));
      seen[si] = 1;
      while (!stack.empty()) {
        const int c = stack.back();
        stack.pop_back();
        const int cx = c % rw, cy = c / rw;
        ++cells;
        mnx = (std::min)(mnx, cx); mxx = (std::max)(mxx, cx); mny = (std::min)(mny, cy); mxy = (std::max)(mxy, cy);
        const int nx[4] = { cx - 1, cx + 1, cx, cx }, ny[4] = { cy, cy, cy - 1, cy + 1 };
        for (int k = 0; k < 4; ++k) {
          if (nx[k] < 0 || ny[k] < 0 || nx[k] >= rw || ny[k] >= rh) continue;
          const size_t ni = static_cast<size_t>(ny[k]) * rw + nx[k];
          if (dark[ni] && !seen[ni]) { seen[ni] = 1; stack.push_back(static_cast<int>(ni)); }
        }
      }
      const int cw = mxx - mnx + 1, ch = mxy - mny + 1;
      const double fill = static_cast<double>(cells) / (static_cast<double>(cw) * ch), aspect = static_cast<double>((std::max)(cw, ch)) / (std::min)(cw, ch);
      if (cw < 2 || ch < 2 || fill < 0.55 || aspect > 5.0 || static_cast<double>(cw) * ch > 0.4 * rw * rh) continue;
      out[n][0] = static_cast<float>(x0 + mnx) / bw;
      out[n][1] = static_cast<float>(y0 + mny) / bh;
      out[n][2] = static_cast<float>(x0 + mxx + 1) / bw;
      out[n][3] = static_cast<float>(y0 + mxy + 1) / bh;
      ++n;
    }
  clog2(L"auto windows %016llX: %d pane(s) found in the island (%dx%d blocks, dark below luminance %.0f of mean %.0f)", tex, n, rw, rh, thr, mean);
  return n;
}

// ---- menu.ini sections
// [lights.<stage>] l<id> = x, y, z, r, g, b, lumens, radius, flicker, mode
// [emissive] / [windows] <hex16> = mode, intensity, r, g, b, litPercent, seed, u0 v0 u1 v1; u0 v0 u1 v1; ...
//   [| light=lumens, cone, softness, dx, dy, dz, aim, ax, ay, az]   (an emissive prop that casts light)
void rbrrtx_custom_ini_set(const char* section, const char* key, const char* value) {
  if (!section || !key || !value) return;
  if (!std::strncmp(section, "lights.", 7)) {
    const int stage = !std::strcmp(section + 7, "menu") ? -1 : std::atoi(section + 7);
    CL l;
    l.d.id = g_nextId++;
    int mode = 0;
    if (sscanf_s(value, " %f , %f , %f , %f , %f , %f , %f , %f , %f , %d", &l.d.pos[0], &l.d.pos[1], &l.d.pos[2], &l.d.rgb[0], &l.d.rgb[1],
                 &l.d.rgb[2], &l.d.lumens, &l.d.radius, &l.d.flicker, &mode) < 10) return;
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
    g_emit[e.tex] = e;
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
      snprintf(b, sizeof(b), "%s=%.3f, %.3f, %.3f, %.4f, %.4f, %.4f, %.1f, %.3f, %.2f, %d", key, d.pos[0], d.pos[1], d.pos[2], d.rgb[0], d.rgb[1], d.rgb[2],
               d.lumens, d.radius, d.flicker, d.mode);
      emit(b, user);
      ++items;
    }
    emit("", user);
  }
  for (int pass = 0; pass < 2; ++pass) {
    bool head = false;
    for (const auto& kv : g_emit) {
      const RbrEmit& e = kv.second;
      if ((e.windows != 0) != (pass == 1)) continue;
      if (!head) { emit(pass ? "[windows]" : "[emissive]", user); head = true; }
      int len = snprintf(b, sizeof(b), "%016llX=%d, %.3f, %.4f, %.4f, %.4f, %d, %u, ", static_cast<unsigned long long>(kv.first), e.mode, e.intensity, e.rgb[0], e.rgb[1],
                         e.rgb[2], e.litPercent, e.seed);
      for (int i = 0; i < e.nrects && len < static_cast<int>(sizeof(b)) - 80; ++i)
        len += snprintf(b + len, sizeof(b) - len, "%.5f %.5f %.5f %.5f; ", e.rects[i][0], e.rects[i][1], e.rects[i][2], e.rects[i][3]);
      if (e.lumens > 0.0f && len < static_cast<int>(sizeof(b)) - 160)
        snprintf(b + len, sizeof(b) - len, "| light=%.0f, %.1f, %.2f, %.4f, %.4f, %.4f, %d, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %d", e.lumens, e.cone, e.softness, e.dir[0], e.dir[1],
                 e.dir[2], e.aim, e.aimAt[0], e.aimAt[1], e.aimAt[2], e.offset[0], e.offset[1], e.offset[2], e.anchor);
      emit(b, user);
      ++items;
    }
    if (head) emit("", user);
  }
  return items;
}
