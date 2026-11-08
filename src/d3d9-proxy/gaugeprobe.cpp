// gaugeprobe.cpp - RBRHUD gauge / display probe (dev tool, off by default: RBRRTX_GAUGE_PROBE=1, launcher -GaugeProbe).
// Nothing here changes what the game draws; it only watches and writes <RBRRTX_LOG_DIR>\rbrrtx_gauge_probe.log.
//
// Question 1 (docs/research/terrain-library-and-gauge-glow.md section A): RBRHUD draws the cockpit gauges and displays
// into a render-target copy of the car's interior texture and lights them in the dark with its own pixel shader
// (CarTextureNoDirtEmMask: s0 = that texture, s15 = a per-car emissive mask, b1 = "dynamic emissive elements on";
// colour = lit + tex * mask.a + (b1 ? tex * mask.rgb : 0)). Remix reads s0 only. To hand the masks to Remix as an
// emissive material, the display texture needs a Remix hash we know before Remix starts. A render target's Remix
// hash is XXH3(extent) seeded with the number of render targets created before it (dxvk-remix d3d9_common_texture.cpp
// :347-353; checked 2026-10-29: the E30 needle layer F4BC05C699777512 is a 2048x512 render target created as the 38th).
// So the probe counts render-target creations the way Remix does and logs the hash each one would get, for the
// counter = our 0-based count + RBRRTX_GAUGE_RT_OFFSET (Remix also counts back buffers and anything the game does not
// show us; RBRRTX_GAUGE_FIND=<hash,...> scans the counter for a hash read in Remix's own UI and prints the offset).
//
// Question 2 (user report, Fabia RS Rally2): displays vanish after driving a while. For every tracked texture the probe
// logs when car draws stop sampling it or RBRHUD stops rendering into it (and when they resume), with gear and speed,
// every window (2 s) which shader / textures / b1 the gauge draws used and whether our compat layer skipped them, and a
// full state dump when the player presses the stage-report mark key (9).
#define CINTERFACE
#define XXH_INLINE_ALL
#include "common.hpp"
#include "third_party/xxhash.h"
#include <d3d9.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

bool rbrrtx_gauge_probe_on = false;

namespace {


constexpr ULONGLONG kWindowMs = 2000;
constexpr unsigned kStopFrames = 90;                  // not sampled / rendered for this many frames = stopped
constexpr unsigned kMaxTexLines = 600;
constexpr unsigned kMaxLines = 30000;

struct Tex {
  UINT w = 0, h = 0, levels = 0;
  DWORD usage = 0;
  D3DFORMAT fmt = D3DFMT_UNKNOWN;
  D3DPOOL pool = D3DPOOL_DEFAULT;
  bool rt = false;
  unsigned seq = 0;                                   // creation order among tracked textures
  unsigned rtSeq = 0;                                 // 1-based count among render-target creations (0 = not a render target)
  unsigned long long pred = 0;                        // Remix hash if its counter = rtSeq - 1 + offset
  unsigned albedo = 0, rtDraws = 0, mask = 0;         // this window: sampled on s0, rendered into, sampled on s15
  unsigned long long albedoTotal = 0, rtTotal = 0, maskTotal = 0;
  unsigned lastAlbedoFrame = 0, lastRtFrame = 0;
  bool albedoStopped = false, rtStopped = false;
  unsigned updates = 0;                               // UpdateTexture copies into it
};

struct Combo {
  std::wstring vs;
  void* s0 = nullptr;
  void* s15 = nullptr;
  int b1 = -1;
  bool primary = true;
  unsigned draws = 0, skipped = 0;
  unsigned long long total = 0;
  unsigned firstFrame = 0, lastFrame = 0;
  const wchar_t* why = L"";
};

std::mutex g_mx;
std::unordered_map<void*, Tex> g_tex;                 // tracked textures by pointer
std::unordered_map<void*, void*> g_surfTex;           // render-target surface -> container texture (nullptr = none)
std::unordered_map<void*, std::wstring> g_vsName;     // vertex shader object -> what it is
std::unordered_map<unsigned long long, std::wstring> g_hudVs;   // bytecode hash -> RBRHUD shader file
std::unordered_map<unsigned long long, Combo> g_combos;
std::unordered_map<void*, std::string> g_maskInfo;    // texture seen on s15 -> description (logged once)
Combo* g_last = nullptr;                              // the combo of the draw in progress (for skip notes)

IDirect3DDevice9* g_dev = nullptr;
decltype(IDirect3DDevice9Vtbl::SetTexture) g_setTexture = nullptr;
decltype(IDirect3DDevice9Vtbl::SetPixelShaderConstantB) g_setPsB = nullptr;
decltype(IDirect3DDevice9Vtbl::CreateRenderTarget) g_createRt = nullptr;

void* g_s0 = nullptr;
void* g_s15 = nullptr;
BOOL g_b[16] = {};
bool g_bSeen[16] = {};
void* g_curRtTex = nullptr;                           // texture behind the render target now bound (index 0)
unsigned g_frame = 0, g_rtSeq = 0, g_texSeq = 0, g_lines = 0, g_texLines = 0, g_marks = 0, g_bLines = 0;
unsigned g_offset = 0;
ULONGLONG g_t0 = 0, g_nextWindow = 0;
std::vector<unsigned long long> g_find;
bool g_hadGaugeDraws = false;
unsigned g_lastGaugeFrame = 0;
int g_stageKey = -2;
unsigned g_vsOther = 0;

template <class F>
void patch(F* slot, F hook, F& orig) {
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(F), PAGE_EXECUTE_READWRITE, &old)) return;
  if (*slot != hook) { orig = *slot; *slot = hook; }
  VirtualProtect(slot, sizeof(F), old, &old);
}

// ---- same shader identification as comp.cpp (FNV-1a over the bytecode up to the end token)
size_t bytecode_size(const DWORD* p, size_t max_dwords) {
  for (size_t i = 1; i < max_dwords; ++i) {
    const DWORD t = p[i];
    if (t == 0x0000FFFF) return (i + 1) * sizeof(DWORD);
    if ((t & 0xFFFF) == 0xFFFE) i += (t >> 16) & 0x7FFF;
  }
  return 0;
}

unsigned long long fnv1a(const void* data, size_t size) {
  unsigned long long h = 1469598103934665603ULL;
  const unsigned char* b = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

// Hash of the instruction tokens only (comment blocks left out), as comp.cpp's code_hash.
unsigned long long code_hash(const DWORD* p, size_t max_dwords) {
  unsigned long long h = 1469598103934665603ULL;
  auto mix = [&h](DWORD t) { for (int k = 0; k < 4; ++k) { h ^= (t >> (8 * k)) & 0xFF; h *= 1099511628211ULL; } };
  mix(p[0]);
  for (size_t i = 1; i < max_dwords; ++i) {
    const DWORD t = p[i];
    if ((t & 0xFFFF) == 0xFFFE) { i += (t >> 16) & 0x7FFF; continue; }
    mix(t);
    if (t == 0x0000FFFF) break;
  }
  return h;
}

// ---- game state (RBRCarInfo at [0x165FC68]: +0x0C speed, +0x10 rpm, +0x170 gear; NGPCarMenu RBRAPI.h)
bool read_car_info(float* speed, float* rpm, int* gear) {
  __try {
    const uintptr_t car = *reinterpret_cast<volatile uintptr_t*>(0x0165FC68);
    if (!car) return false;
    *speed = *reinterpret_cast<volatile float*>(car + 0x0C);
    *rpm = *reinterpret_cast<volatile float*>(car + 0x10);
    *gear = *reinterpret_cast<volatile int*>(car + 0x170);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  return true;
}

std::wstring state_text() {
  float speed = 0, rpm = 0;
  int gear = -1;
  const bool ok = read_car_info(&speed, &rpm, &gear);
  wchar_t s[192];
  if (ok) swprintf_s(s, L"gear %d speed %.1f rpm %.0f cabin %d", gear, speed, rpm, rbrrtx_camera_in_cabin() ? 1 : 0);
  else swprintf_s(s, L"car state unreadable, cabin %d", rbrrtx_camera_in_cabin() ? 1 : 0);
  return s;
}

const wchar_t* fmt_name(D3DFORMAT f) {
  switch (f) {
    case D3DFMT_A8R8G8B8: return L"A8R8G8B8";
    case D3DFMT_X8R8G8B8: return L"X8R8G8B8";
    case D3DFMT_A4R4G4B4: return L"A4R4G4B4";
    case D3DFMT_R5G6B5: return L"R5G6B5";
    case D3DFMT_A16B16G16R16F: return L"A16B16G16R16F";
    case D3DFMT_DXT1: return L"DXT1";
    case D3DFMT_DXT3: return L"DXT3";
    case D3DFMT_DXT5: return L"DXT5";
    default: return L"other";
  }
}

const wchar_t* pool_name(D3DPOOL p) {
  switch (p) {
    case D3DPOOL_DEFAULT: return L"DEFAULT";
    case D3DPOOL_MANAGED: return L"MANAGED";
    case D3DPOOL_SYSTEMMEM: return L"SYSTEMMEM";
    default: return L"?";
  }
}

// Remix's render-target hash: XXH3(extent {w, h, 1}) used as the seed of XXH3(counter).
unsigned long long predict(UINT w, UINT h, unsigned counter) {
  const uint32_t ext[3] = { w, h, 1u };
  const XXH64_hash_t base = XXH3_64bits(ext, sizeof(ext));
  return XXH3_64bits_withSeed(&counter, sizeof(counter), base);
}

std::wstring tex_text(const Tex& t, void* p) {
  wchar_t s[300];
  if (t.rt)
    swprintf_s(s, L"tex#%u %p %ux%u %s %s usage 0x%lx rt#%u hash(counter %u) %016llX", t.seq, p, t.w, t.h, fmt_name(t.fmt),
               pool_name(t.pool), t.usage, t.rtSeq, t.rtSeq - 1 + g_offset, t.pred);
  else
    swprintf_s(s, L"tex#%u %p %ux%u %s %s usage 0x%lx (not a render target)", t.seq, p, t.w, t.h, fmt_name(t.fmt),
               pool_name(t.pool), t.usage);
  return s;
}

std::wstring ptr_text(void* p) {
  if (!p) return L"-";
  auto it = g_tex.find(p);
  wchar_t s[64];
  if (it != g_tex.end()) swprintf_s(s, L"tex#%u", it->second.seq);
  else swprintf_s(s, L"%p", p);
  return s;
}

bool room() { return g_lines < kMaxLines; }

// One log line, capped at kMaxLines per run.
void gl(const wchar_t* fmt, ...) {
  if (g_lines > kMaxLines) return;
  wchar_t buf[3000];
  if (g_lines == kMaxLines) {
    ++g_lines;
    rbrrtx_log_to(L"rbrrtx_gauge_probe.log", L"(line limit %u reached; further output suppressed)", kMaxLines);
    return;
  }
  ++g_lines;
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
  va_end(args);
  rbrrtx_log_to(L"rbrrtx_gauge_probe.log", L"%s", buf);
}

// ---- hooks
HRESULT STDMETHODCALLTYPE hk_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
  if (rbrrtx_menu_is_drawing()) return g_setTexture(dev, stage, tex);   // the F3 menu's own draws
  if (stage == 0) g_s0 = tex;
  else if (stage == 15) g_s15 = tex;
  return g_setTexture(dev, stage, tex);
}

HRESULT STDMETHODCALLTYPE hk_SetPsB(IDirect3DDevice9* dev, UINT start, const BOOL* data, UINT count) {
  if (data)
    for (UINT i = 0; i < count && start + i < 16; ++i) {
      const UINT r = start + i;
      const BOOL v = data[i] ? TRUE : FALSE;
      if ((!g_bSeen[r] || g_b[r] != v) && g_bLines < 300 && room()) {
        ++g_bLines;
        gl(L"b%u = %d (was %s) at frame %u, %s", r, v, g_bSeen[r] ? (g_b[r] ? L"1" : L"0") : L"unset", g_frame,
             state_text().c_str());
      }
      g_bSeen[r] = true;
      g_b[r] = v;
    }
  return g_setPsB(dev, start, data, count);
}

void note_rt(const wchar_t* api, void* obj, UINT w, UINT h, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, Tex* out) {
  Tex t;
  t.w = w; t.h = h; t.usage = usage; t.fmt = fmt; t.pool = pool; t.rt = true;
  t.rtSeq = ++g_rtSeq;
  t.seq = ++g_texSeq;
  t.pred = predict(w, h, t.rtSeq - 1 + g_offset);
  if (out) *out = t;
  if (g_texLines < kMaxTexLines && room()) {
    ++g_texLines;
    gl(L"%s: %s", api, tex_text(t, obj).c_str());
  }
  if (!g_find.empty()) {                              // which counter gives a hash read in Remix's UI?
    for (unsigned c = 0; c < 32768; ++c) {
      const unsigned long long h64 = predict(w, h, c);
      for (unsigned long long want : g_find)
        if (h64 == want) {
          gl(L"FIND hit: %016llX = %ux%u render target with Remix counter %u; this is our render target #%u, so "
               L"RBRRTX_GAUGE_RT_OFFSET = %d", want, w, h, c, t.rtSeq, static_cast<int>(c) - static_cast<int>(t.rtSeq - 1));
        }
    }
  }
}

HRESULT STDMETHODCALLTYPE hk_CreateRenderTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                DWORD q, BOOL lockable, IDirect3DSurface9** out, HANDLE* share) {
  const HRESULT hr = g_createRt(dev, w, h, fmt, ms, q, lockable, out, share);
  if (SUCCEEDED(hr) && out && *out) {
    std::lock_guard<std::mutex> g(g_mx);
    Tex t;
    note_rt(L"CreateRenderTarget (surface)", *out, w, h, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &t);
  }
  return hr;
}

void load_hud_shaders() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const fs::path dir = fs::path(exe).parent_path() / L"Plugins" / L"RBRHUD" / L"Shaders";
  std::error_code ec;
  unsigned n = 0;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file() || _wcsicmp(it->path().extension().c_str(), L".vso") != 0) continue;
    std::ifstream f(it->path(), std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 8) continue;
    size_t size = bytecode_size(reinterpret_cast<const DWORD*>(data.data()), data.size() / 4);
    if (!size) size = data.size();
    const std::wstring name = L"RBRHUD:" + fs::relative(it->path(), dir).wstring();
    const unsigned long long h1 = fnv1a(data.data(), size), h2 = code_hash(reinterpret_cast<const DWORD*>(data.data()), data.size() / 4);
    g_hudVs[h1] = name;
    g_hudVs[h2] = name;
    ++n;
    gl(L"RBRHUD vertex shader file %s: %zu bytes, hash %016llx", name.c_str(), size, h1);
  }
  gl(L"%u RBRHUD vertex shader files under %s", n, dir.c_str());
}

void parse_find(const wchar_t* text) {
  const wchar_t* p = text;
  while (*p) {
    while (*p == L',' || *p == L' ' || *p == L';') ++p;
    wchar_t* end = nullptr;
    const unsigned long long v = _wcstoui64(p, &end, 16);
    if (end == p) break;
    if (v) g_find.push_back(v);
    p = end;
  }
}

void print_texture_state(const wchar_t* tag) {
  for (auto& kv : g_tex) {
    const Tex& t = kv.second;
    gl(L"%s %s: sampled on s0 %llu draws (last frame %u), rendered into %llu draws (last frame %u), on s15 %llu, "
         L"UpdateTexture copies %u",
         tag, tex_text(t, kv.first).c_str(), t.albedoTotal, t.lastAlbedoFrame, t.rtTotal, t.lastRtFrame, t.maskTotal, t.updates);
  }
}

void flush_window() {
  RbrStageInfo si = {};
  if (rbrrtx_stage_info(&si)) {
    const int key = si.trackId * 1000 + si.carId;
    if (key != g_stageKey) {
      g_stageKey = key;
      gl(L"stage %d \"%s\" car slot %d (%s), game mode %d, weather %d time %d", si.trackId, si.name, si.carId,
           rbrrtx_car_model_name() ? rbrrtx_car_model_name() : L"?", si.gameMode, si.weatherType, si.timeOfDay);
    }
  }
  bool any = false;
  for (auto& kv : g_combos) {
    Combo& c = kv.second;
    if (c.draws || c.skipped) {
      any = true;
      if (room()) {
        gl(L"  window: vs %s  s0 %s  s15 %s  b1 %d  %s: %u draws, %u skipped by compat%s%s", c.vs.c_str(), ptr_text(c.s0).c_str(),
             ptr_text(c.s15).c_str(), c.b1, c.primary ? L"main target" : L"off-screen", c.draws, c.skipped,
             c.skipped ? L" (" : L"", c.skipped ? c.why : L"");
      }
      c.draws = c.skipped = 0;
    }
  }
  if (any) { g_hadGaugeDraws = true; g_lastGaugeFrame = g_frame; }
  else if (g_hadGaugeDraws && g_frame - g_lastGaugeFrame > kStopFrames) {
    g_hadGaugeDraws = false;
    gl(L"ALL gauge-related draws stopped at frame ~%u (%s)", g_lastGaugeFrame, state_text().c_str());
  }
  for (auto& kv : g_tex) {
    Tex& t = kv.second;
    if (room() && (t.albedo || t.rtDraws || t.mask)) {
      gl(L"  window: %s: sampled s0 %u, rendered into %u, on s15 %u", tex_text(t, kv.first).c_str(), t.albedo, t.rtDraws, t.mask);
    }
    t.albedo = t.rtDraws = t.mask = 0;
    if (t.albedoTotal && !t.albedoStopped && g_frame - t.lastAlbedoFrame > kStopFrames) {
      t.albedoStopped = true;
      gl(L"STOPPED: %s is no longer sampled by any draw (last frame %u, now %u); %s", tex_text(t, kv.first).c_str(),
           t.lastAlbedoFrame, g_frame, state_text().c_str());
    }
    if (t.rtTotal && !t.rtStopped && g_frame - t.lastRtFrame > kStopFrames) {
      t.rtStopped = true;
      gl(L"STOPPED: nothing renders into %s any more (last frame %u, now %u); %s", tex_text(t, kv.first).c_str(),
           t.lastRtFrame, g_frame, state_text().c_str());
    }
  }
}

}  // namespace

void rbrrtx_gauge_probe_init(void* device) {
  if (!device || rbrrtx_gauge_probe_on) return;
  g_dev = static_cast<IDirect3DDevice9*>(device);
  wchar_t v[512];
  if (GetEnvironmentVariableW(L"RBRRTX_GAUGE_RT_OFFSET", v, 16)) g_offset = static_cast<unsigned>(_wtoi(v));
  if (GetEnvironmentVariableW(L"RBRRTX_GAUGE_FIND", v, 512)) parse_find(v);
  g_t0 = GetTickCount64();
  g_nextWindow = g_t0 + kWindowMs;
  gl(L"gauge probe on. Remix render-target hash = XXH3_64bits_withSeed(counter u32, seed = XXH3_64bits({w, h, 1} u32 x3)); "
       L"counter = our 0-based render-target count + RBRRTX_GAUGE_RT_OFFSET (now %u); find hashes: %zu", g_offset, g_find.size());
  load_hud_shaders();
  IDirect3DDevice9Vtbl* vt = g_dev->lpVtbl;
  patch(&vt->SetTexture, &hk_SetTexture, g_setTexture);
  patch(&vt->SetPixelShaderConstantB, &hk_SetPsB, g_setPsB);
  patch(&vt->CreateRenderTarget, &hk_CreateRenderTarget, g_createRt);
  rbrrtx_gauge_probe_on = true;
}

void rbrrtx_gauge_probe_texture(void* tex, unsigned w, unsigned h, unsigned levels, DWORD usage, int format, int poolId) {
  const D3DFORMAT fmt = static_cast<D3DFORMAT>(format);
  const D3DPOOL pool = static_cast<D3DPOOL>(poolId);
  const bool rt = (usage & D3DUSAGE_RENDERTARGET) != 0;
  const bool big = pool == D3DPOOL_DEFAULT && fmt == D3DFMT_A8R8G8B8 && w >= 256 && h >= 256;   // RBRHUD's copies are 2048x2048 DEFAULT
  std::lock_guard<std::mutex> g(g_mx);
  g_surfTex.clear();                                  // an address may now belong to another object
  Tex t;
  if (rt) {
    note_rt(L"CreateTexture (render target)", tex, w, h, usage, fmt, pool, &t);
  } else if (big) {
    t.seq = ++g_texSeq;
  } else {
    return;
  }
  t.w = w; t.h = h; t.levels = levels; t.usage = usage; t.fmt = fmt; t.pool = pool; t.rt = rt;
  if (!rt && g_texLines < kMaxTexLines && room()) {
    ++g_texLines;
    gl(L"CreateTexture (large DEFAULT, not a render target): %s", tex_text(t, tex).c_str());
  }
  g_tex[tex] = t;                                     // replaces an older texture at the same address
}

void rbrrtx_gauge_probe_update(void* src, void* dst) {
  std::lock_guard<std::mutex> g(g_mx);
  auto it = g_tex.find(dst);
  if (it == g_tex.end()) return;
  ++it->second.updates;
  if (it->second.updates <= 3 && room()) {
    gl(L"UpdateTexture into %s from %p (copy %u)", tex_text(it->second, dst).c_str(), src, it->second.updates);
  }
}

void rbrrtx_gauge_probe_vs(void* vs, const void* bytecode, const wchar_t* gameName) {
  std::lock_guard<std::mutex> g(g_mx);
  const DWORD* fn = static_cast<const DWORD*>(bytecode);
  const size_t size = fn ? bytecode_size(fn, 1u << 16) : 0;
  auto h = size ? g_hudVs.find(fnv1a(fn, size)) : g_hudVs.end();
  if (h == g_hudVs.end() && size) h = g_hudVs.find(code_hash(fn, 1u << 16));
  if (h != g_hudVs.end()) g_vsName[vs] = h->second;
  else if (gameName && *gameName) g_vsName[vs] = std::wstring(L"game:") + gameName;
  else { g_vsName[vs] = L"(other)"; ++g_vsOther; }
}

void rbrrtx_gauge_probe_rt(void* surface) {
  std::lock_guard<std::mutex> g(g_mx);
  if (!surface) { g_curRtTex = nullptr; return; }
  auto it = g_surfTex.find(surface);
  if (it != g_surfTex.end()) { g_curRtTex = it->second; return; }
  void* container = nullptr;
  IDirect3DSurface9* s = static_cast<IDirect3DSurface9*>(surface);
  if (SUCCEEDED(s->lpVtbl->GetContainer(s, IID_IDirect3DTexture9, &container)) && container)
    static_cast<IUnknown*>(container)->lpVtbl->Release(static_cast<IUnknown*>(container));   // pointer only
  else container = nullptr;
  g_surfTex[surface] = container;
  g_curRtTex = container;
}

void rbrrtx_gauge_probe_draw(void* vs, bool primaryRT) {
  std::lock_guard<std::mutex> g(g_mx);
  g_last = nullptr;
  std::wstring vsName;
  auto n = g_vsName.find(vs);
  if (n != g_vsName.end()) vsName = n->second;
  const bool hudVs = vsName.rfind(L"RBRHUD:", 0) == 0;
  Tex* albedo = nullptr;
  if (g_s0) {
    auto it = g_tex.find(g_s0);
    if (it != g_tex.end()) albedo = &it->second;
  }
  if (g_curRtTex) {                                   // RBRHUD (or the game) drawing into a tracked texture
    auto it = g_tex.find(g_curRtTex);
    if (it != g_tex.end()) {
      Tex& t = it->second;
      ++t.rtDraws;
      ++t.rtTotal;
      t.lastRtFrame = g_frame;
      if (t.rtStopped) {
        t.rtStopped = false;
        gl(L"RESUMED: draws render into %s again (frame %u); %s", tex_text(t, g_curRtTex).c_str(), g_frame, state_text().c_str());
      }
    }
  }
  if (!(hudVs || albedo || g_s15)) return;
  if (g_s15) {
    auto m = g_tex.find(g_s15);
    if (m != g_tex.end()) { ++m->second.mask; ++m->second.maskTotal; }
    if (!g_maskInfo.count(g_s15)) {                   // first time this texture is on s15: what it is
      IDirect3DTexture9* t = static_cast<IDirect3DTexture9*>(g_s15);
      D3DSURFACE_DESC d = {};
      const bool ok = SUCCEEDED(t->lpVtbl->GetLevelDesc(t, 0, &d));
      const unsigned long long hash = rbrrtx_texture_hash(g_s15);
      char info[200];
      sprintf_s(info, "%ux%u fmt %d pool %d, Remix hash %016llX", ok ? d.Width : 0, ok ? d.Height : 0, ok ? static_cast<int>(d.Format) : 0,
                ok ? static_cast<int>(d.Pool) : 0, hash);
      g_maskInfo[g_s15] = info;
      gl(L"s15 gets a new texture %p: %S  (vs %s, b1 %d) - an emissive mask if its hash matches a *_em_mask.dds", g_s15, info,
           vsName.c_str(), g_b[1]);
    }
  }
  if (albedo) {
    ++albedo->albedo;
    ++albedo->albedoTotal;
    albedo->lastAlbedoFrame = g_frame;
    if (albedo->albedoStopped) {
      albedo->albedoStopped = false;
      gl(L"RESUMED: %s is sampled again (frame %u); %s", tex_text(*albedo, g_s0).c_str(), g_frame, state_text().c_str());
    }
  }
  unsigned long long key = 1469598103934665603ULL;
  auto mixp = [&key](const void* p, size_t n2) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n2; ++i) { key ^= b[i]; key *= 1099511628211ULL; }
  };
  const int b1 = g_bSeen[1] ? g_b[1] : -1;
  mixp(&vs, sizeof(vs));
  mixp(&g_s0, sizeof(g_s0));
  mixp(&g_s15, sizeof(g_s15));
  mixp(&b1, sizeof(b1));
  mixp(&primaryRT, sizeof(primaryRT));
  Combo& c = g_combos[key];
  if (c.total == 0) {
    c.vs = vsName.empty() ? (vs ? L"(unnamed VS)" : L"(fixed-function)") : vsName;
    c.s0 = g_s0;
    c.s15 = g_s15;
    c.b1 = b1;
    c.primary = primaryRT;
    c.firstFrame = g_frame;
    gl(L"NEW draw combination at frame %u: vs %s  s0 %s  s15 %s  b1 %d  %s; %s", g_frame, c.vs.c_str(), ptr_text(g_s0).c_str(),
         ptr_text(g_s15).c_str(), b1, primaryRT ? L"main target" : L"off-screen", state_text().c_str());
  }
  ++c.draws;
  ++c.total;
  c.lastFrame = g_frame;
  g_last = &c;
}

void rbrrtx_gauge_probe_skip(const wchar_t* why) {
  if (g_last) { ++g_last->skipped; g_last->why = why; }
}

void rbrrtx_gauge_probe_event(const wchar_t* text) {
  std::lock_guard<std::mutex> g(g_mx);
  gl(L"EVENT frame %u: %s; %s", g_frame, text, state_text().c_str());
}

void rbrrtx_gauge_probe_mark() {
  std::lock_guard<std::mutex> g(g_mx);
  ++g_marks;
  gl(L"MARK %u (key 9) at frame %u, +%.1f s: %s. State of everything tracked:", g_marks, g_frame,
       (GetTickCount64() - g_t0) / 1000.0, state_text().c_str());
  print_texture_state(L"  ");
  for (auto& kv : g_combos) {
    const Combo& c = kv.second;
    gl(L"  combo vs %s s0 %s s15 %s b1 %d %s: %llu draws in total, first frame %u, last frame %u (now %u), %s",
         c.vs.c_str(), ptr_text(c.s0).c_str(), ptr_text(c.s15).c_str(), c.b1, c.primary ? L"main" : L"off-screen", c.total,
         c.firstFrame, c.lastFrame, g_frame, c.lastFrame + kStopFrames < g_frame ? L"NOT DRAWN lately" : L"still drawn");
  }
  gl(L"  current: s0 %s s15 %s b1 %d (%s)", ptr_text(g_s0).c_str(), ptr_text(g_s15).c_str(), g_bSeen[1] ? g_b[1] : -1,
       g_bSeen[1] ? L"seen" : L"never set");
}

void rbrrtx_gauge_probe_frame() {
  std::lock_guard<std::mutex> g(g_mx);
  ++g_frame;
  const ULONGLONG now = GetTickCount64();
  if (now < g_nextWindow) return;
  g_nextWindow = now + kWindowMs;
  flush_window();
}
