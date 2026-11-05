// rbrhud.cpp - RBRHUD's F4 windscreen settings under Remix (user 2026-11-05: "its menu at F4 allows the user to change
// many things including windshield opacity and even just its banner using a simple transparency limit"; top fix
// before 0.4.0). Plan: docs/research/car-and-hud-investigation-2026-11-05.md "RBRHUD done properly".
//
// RBRHUD draws the cockpit windscreen (the game's CarGlassInternal.vso) with its own pixel shader
// Plugins\RBRHUD\Shaders\car\ps\general\CarGlassInternal.pso (constant table: b0 b_transparent_regions_only,
// c0 constant0, s0 tex0). Disassembled 2026-11-05:
//   colour = tex.rgb * v1.rgb
//   b0 on:  alpha = tex.a >= threshold ? tex.a : tex.a * opacity     (c0.z = -threshold, c0.w = opacity)
//   b0 off: alpha = tex.a * opacity
// Remix never runs that shader, so the F4 values did nothing. rbrhud.ini [windscreen] opacity, transparentregionsonly,
// transparencythreshold hold them (read at start and again whenever the file changes). While they change anything,
// the windscreen draw gets a twin texture: a 16x16 picture whose Remix hash is known (as the marked lamps' glass,
// worldlights.cpp), and the generated mod (genmod.cpp, section "windscreen") gives that hash the windscreen sheet with
// its alpha remapped as above, plus the glass roughness of tools\pbr\make_car_glass_mod.py. The sheet is saved from the
// game's memory once per car (<mods>\rbr_rtx_lit\windscreen\<hash>.dds); DXT3 (every RBR windscreen sheet), DXT5 and
// A8R8G8B8 are remapped, other formats are left alone. With opacity 1 (RBRHUD's default) nothing is changed.
#define CINTERFACE
#include "common.hpp"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#define hlog(...) rbrrtx_log_to(L"rbrrtx_comp.log", __VA_ARGS__)

namespace {

struct Settings {
  float opacity = 1.0f, threshold = 1.0f;
  bool regionsOnly = true;
  bool found = false;
};

struct Twin {
  bool tried = false, ok = false;
  std::wstring dds;                                    // the remapped sheet
  ULONGLONG readyAt = 0;
  IDirect3DTexture9* tex = nullptr;
  uint64_t twinHash = 0;
};

constexpr ULONGLONG kSettle = 9000;                    // ms for the generated mod to be written and reloaded
Settings g_set;
std::wstring g_ini;
FILETIME g_iniTime = {};
ULONGLONG g_nextCheck = 0;
bool g_hud = false;                                    // RBRHUD.dll is loaded
std::unordered_map<uint64_t, Twin> g_twins;            // key: source hash x settings

std::wstring hex16(uint64_t h) {
  wchar_t b[24];
  swprintf_s(b, L"%016llX", static_cast<unsigned long long>(h));
  return b;
}

std::wstring ws_dir() {
  const wchar_t* d = rbrrtx_genmod_dir();
  return d && d[0] ? std::wstring(d) + L"\\windscreen" : std::wstring();
}

float ini_f(const wchar_t* key, float dflt) {
  wchar_t v[64] = {};
  GetPrivateProfileStringW(L"windscreen", key, L"", v, 64, g_ini.c_str());
  return v[0] ? static_cast<float>(_wtof(v)) : dflt;
}

// rbrhud.ini next to RBRHUD.dll (Plugins\RBRHUD\rbrhud.ini), read again when its time stamp moves.
void poll_ini() {
  const ULONGLONG now = GetTickCount64();
  if (now < g_nextCheck) return;
  g_nextCheck = now + 2000;
  if (g_ini.empty()) {
    HMODULE m = GetModuleHandleW(L"RBRHUD.dll");
    g_hud = m != nullptr;
    if (!m) return;
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(m, p, MAX_PATH * 2);
    if (wchar_t* s = wcsrchr(p, L'\\')) s[1] = 0;
    g_ini = std::wstring(p) + L"RBRHUD\\rbrhud.ini";
  }
  WIN32_FILE_ATTRIBUTE_DATA fa = {};
  if (!GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &fa)) { g_set.found = false; return; }
  if (g_set.found && CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) == 0) return;
  g_iniTime = fa.ftLastWriteTime;
  Settings s;
  s.found = true;
  s.opacity = std::clamp(ini_f(L"opacity", 1.0f), 0.0f, 1.0f);
  s.threshold = std::clamp(ini_f(L"transparencythreshold", 1.0f), 0.0f, 1.0f);
  s.regionsOnly = ini_f(L"transparentregionsonly", 1.0f) != 0.0f;
  if (!g_set.found || s.opacity != g_set.opacity || s.threshold != g_set.threshold || s.regionsOnly != g_set.regionsOnly)
    hlog(L"rbrhud: windscreen opacity %.2f, %s (threshold %.2f) from %s%s", s.opacity,
         s.regionsOnly ? L"only below the threshold" : L"everywhere", s.threshold, g_ini.c_str(),
         s.opacity >= 0.999f ? L" - nothing to change" : L" - the cockpit windscreen gets a remapped twin");
  g_set = s;
}

uint8_t remap(uint8_t a) {
  const float f = a / 255.0f;
  const float o = (!g_set.regionsOnly || f < g_set.threshold) ? f * g_set.opacity : f;
  return static_cast<uint8_t>(std::lround(std::clamp(o, 0.0f, 1.0f) * 255.0f));
}

// Rewrites the alpha of a saved DDS (whole mip chain) into `out`. DXT3: the 4-bit alphas; DXT5: the two alpha
// endpoints (kept in the order that selects the 8-value mode); A8R8G8B8: the alpha bytes.
bool write_remapped(const std::wstring& in, const std::wstring& out) {
  FILE* fp = nullptr;
  if (_wfopen_s(&fp, in.c_str(), L"rb") || !fp) return false;
  std::vector<uint8_t> b;
  fseek(fp, 0, SEEK_END);
  const long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (n > 128) { b.resize(static_cast<size_t>(n)); fread(b.data(), 1, b.size(), fp); }
  fclose(fp);
  if (b.size() <= 128 || std::memcmp(b.data(), "DDS ", 4)) return false;
  uint32_t pfFlags = 0, fourcc = 0, bits = 0, amask = 0;
  std::memcpy(&pfFlags, &b[80], 4);
  std::memcpy(&fourcc, &b[84], 4);
  std::memcpy(&bits, &b[88], 4);
  std::memcpy(&amask, &b[104], 4);
  uint8_t* p = b.data() + 128;
  const size_t len = b.size() - 128;
  if ((pfFlags & 4) && fourcc == 0x33545844u) {                       // DXT3
    for (size_t o = 0; o + 16 <= len; o += 16)
      for (int i = 0; i < 8; ++i) {
        const uint8_t lo = p[o + i] & 0xF, hi = p[o + i] >> 4;
        const uint8_t nl = static_cast<uint8_t>((remap(static_cast<uint8_t>(lo * 17)) + 8) / 17);
        const uint8_t nh = static_cast<uint8_t>((remap(static_cast<uint8_t>(hi * 17)) + 8) / 17);
        p[o + i] = static_cast<uint8_t>((nh << 4) | nl);
      }
  } else if ((pfFlags & 4) && fourcc == 0x35545844u) {                // DXT5
    for (size_t o = 0; o + 16 <= len; o += 16) {
      const uint8_t a0 = p[o], a1 = p[o + 1];
      uint8_t n0 = remap(a0), n1 = remap(a1);
      if (a0 > a1 && n0 <= n1) {                                       // keep the 8-value mode
        if (n1 < 255) n0 = static_cast<uint8_t>(n1 + 1); else { n1 = 254; n0 = 255; }
      }
      p[o] = n0;
      p[o + 1] = n1;
    }
  } else if (!(pfFlags & 4) && bits == 32 && amask == 0xFF000000u) {   // A8R8G8B8
    for (size_t o = 3; o < len; o += 4) p[o] = remap(p[o]);
  } else {
    return false;
  }
  if (_wfopen_s(&fp, out.c_str(), L"wb") || !fp) return false;
  fwrite(b.data(), 1, b.size(), fp);
  fclose(fp);
  return true;
}

uint64_t settings_key() {
  const uint32_t o = static_cast<uint32_t>(std::lround(g_set.opacity * 1000.0f));
  const uint32_t t = static_cast<uint32_t>(std::lround(g_set.threshold * 1000.0f));
  return (static_cast<uint64_t>(o) << 32) ^ (static_cast<uint64_t>(t) << 8) ^ (g_set.regionsOnly ? 1u : 0u);
}

// The twin's mip 0 (16x16 A8R8G8B8) from the source hash and the settings: its Remix hash is known before Remix sees it.
uint64_t twin_pixels(uint64_t src, uint64_t key, uint32_t px[256]) {
  uint64_t st = src ^ (0x5242525753435200ULL + key * 0x100000001B3ULL);   // "RBRWSCR"
  for (int i = 0; i < 256; ++i) {
    uint64_t z = (st += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    px[i] = static_cast<uint32_t>(z ^ (z >> 31)) | 0xFF000000u;
  }
  return rbrrtx_xxh3(px, 256 * sizeof(uint32_t));
}

// Section "windscreen" of the generated mod: one material per remapped sheet.
void publish() {
  std::string body;
  for (const auto& kv : g_twins) {
    if (!kv.second.ok) continue;
    const std::string in =
        "                asset inputs:diffuse_texture = @" + rbrrtx_genmod_asset(kv.second.dds) + "@ (\n"
        "                    colorSpace = \"auto\"\n"
        "                )\n"
        "                float inputs:reflection_roughness_constant = 0.01\n"
        "                float inputs:metallic_constant = 0\n";
    body += rbrrtx_genmod_material(kv.second.twinHash, "RBRHUD windscreen opacity: " + rbrrtx_genmod_asset(kv.second.dds), in);
  }
  rbrrtx_genmod_set("windscreen", body);
}

}  // namespace

// The cockpit windscreen draw (CarGlassInternal.vso) with its texture: the twin to draw instead (nullptr = the game's).
void* rbrrtx_rbrhud_windscreen_twin(void* device, void* baseTexture, unsigned long long srcHash) {
  poll_ini();
  if (!g_hud || !g_set.found || g_set.opacity >= 0.999f || !baseTexture || !srcHash) return nullptr;
  const uint64_t key = settings_key();
  Twin& t = g_twins[srcHash ^ (key * 0x9E3779B97F4A7C15ULL)];
  if (!t.tried) {
    t.tried = true;
    const std::wstring dir = ws_dir();
    if (!dir.empty()) {
      CreateDirectoryW(dir.c_str(), nullptr);
      const std::wstring src = dir + L"\\" + hex16(srcHash) + L".dds";
      wchar_t tail[48];
      swprintf_s(tail, L"_o%03d_t%03d_%c.dds", static_cast<int>(std::lround(g_set.opacity * 100.0f)),
                 static_cast<int>(std::lround(g_set.threshold * 100.0f)), g_set.regionsOnly ? L'r' : L'a');
      t.dds = dir + L"\\" + hex16(srcHash) + tail;
      const bool haveSrc = GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES ||
                           rbrrtx_save_texture_dds_mips(baseTexture, src.c_str());
      t.ok = haveSrc && (GetFileAttributesW(t.dds.c_str()) != INVALID_FILE_ATTRIBUTES || write_remapped(src, t.dds));
      uint32_t px[256];
      t.twinHash = twin_pixels(srcHash, key, px);
    }
    t.readyAt = GetTickCount64() + kSettle;
    hlog(L"rbrhud: windscreen %016llX: %s %s", srcHash, t.ok ? L"remapped sheet" : L"FAILED (not saved, or a format without alpha)",
         t.dds.c_str());
    if (t.ok) publish();
  }
  if (!t.ok || GetTickCount64() < t.readyAt) return nullptr;
  if (!t.tex) {
    IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
    uint32_t px[256];
    twin_pixels(srcHash, key, px);
    if (SUCCEEDED(dev->lpVtbl->CreateTexture(dev, 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex, nullptr)) && t.tex) {
      D3DLOCKED_RECT lr = {};
      if (SUCCEEDED(t.tex->lpVtbl->LockRect(t.tex, 0, &lr, nullptr, 0)) && lr.pBits) {
        for (int r = 0; r < 16; ++r) std::memcpy(static_cast<char*>(lr.pBits) + r * lr.Pitch, px + r * 16, 64);
        t.tex->lpVtbl->UnlockRect(t.tex, 0);
      } else {
        t.tex->lpVtbl->Release(t.tex);
        t.tex = nullptr;
      }
    }
  }
  return t.tex;
}
