// Smallest alpha a texture's mip 0 decodes to, for the opaque cut-out check (comp.cpp opaque_cutout): an
// alpha-tested draw whose texture never goes below ALPHAREF passes every pixel, so the test can be dropped.
// Bilinear filtering only blends between texels, so it never goes below this either.
#pragma once
#include <d3d9.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>

// `d` = tightly packed mip-0 data of `size` bytes (harvest.cpp layout()). 0..255, or -1 if the format is not handled.
// DXT padding texels of textures smaller than a block count too (can only make the result lower, i.e. safer).
inline int rbrrtx_min_alpha(D3DFORMAT f, const unsigned char* d, size_t size) {
  int m = 255;
  switch (f) {
    case D3DFMT_DXT1:                                   // 3-colour blocks (c0 <= c1): index 3 = transparent
      for (size_t i = 0; i + 8 <= size; i += 8) {
        const unsigned c0 = d[i] | d[i + 1] << 8, c1 = d[i + 2] | d[i + 3] << 8;
        if (c0 > c1) continue;
        const uint32_t idx = d[i + 4] | d[i + 5] << 8 | d[i + 6] << 16 | static_cast<uint32_t>(d[i + 7]) << 24;
        for (int t = 0; t < 16; ++t) if (((idx >> (2 * t)) & 3) == 3) return 0;
      }
      return 255;
    case D3DFMT_DXT2: case D3DFMT_DXT3:                 // explicit 4-bit alpha
      for (size_t i = 0; i + 16 <= size; i += 16)
        for (int b = 0; b < 8; ++b) {
          m = (std::min)(m, (std::min)(d[i + b] & 15, d[i + b] >> 4) * 17);
          if (!m) return 0;
        }
      return m;
    case D3DFMT_DXT4: case D3DFMT_DXT5: {               // two endpoints + 3-bit indices into an 8-entry palette
      for (size_t i = 0; i + 16 <= size; i += 16) {
        const int a0 = d[i], a1 = d[i + 1];
        int pal[8] = {a0, a1};
        if (a0 > a1) for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
        else { for (int k = 1; k < 5; ++k) pal[k + 1] = ((5 - k) * a0 + k * a1) / 5; pal[6] = 0; pal[7] = 255; }
        uint64_t bits = 0;
        for (int b = 0; b < 6; ++b) bits |= static_cast<uint64_t>(d[i + 2 + b]) << (8 * b);
        for (int t = 0; t < 16; ++t) m = (std::min)(m, pal[(bits >> (3 * t)) & 7]);
        if (!m) return 0;
      }
      return m;
    }
    case D3DFMT_A8R8G8B8:
      for (size_t i = 3; i < size; i += 4) { m = (std::min)(m, static_cast<int>(d[i])); if (!m) return 0; }
      return m;
    case D3DFMT_A8L8:
      for (size_t i = 1; i < size; i += 2) { m = (std::min)(m, static_cast<int>(d[i])); if (!m) return 0; }
      return m;
    case D3DFMT_A8:
      for (size_t i = 0; i < size; ++i) { m = (std::min)(m, static_cast<int>(d[i])); if (!m) return 0; }
      return m;
    case D3DFMT_A1R5G5B5:
      for (size_t i = 1; i < size; i += 2) if (!(d[i] & 0x80)) return 0;
      return 255;
    case D3DFMT_A4R4G4B4:
      for (size_t i = 1; i < size; i += 2) { m = (std::min)(m, (d[i] >> 4) * 17); if (!m) return 0; }
      return m;
    case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_L8:
      return 255;
    default:
      return -1;
  }
}
