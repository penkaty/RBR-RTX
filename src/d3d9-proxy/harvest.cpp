// Texture harvest (dev tool, off unless RBRRTX_HARVEST=<folder>): records every texture the game uploads with its
// RTX Remix hash, so materials can be made for stages whose .rbz archives are encrypted (RSF) and cannot be read
// offline (tools\pbr\harvest.py reads the ZIP ones). Remix's hash is XXH3_64 of the raw mip-0 bytes
// (dxvk-remix d3d9_common_texture.cpp SetupForRtxFrom); third_party/xxhash.h is the same xxHash 0.8.0 Remix uses.
//
// Hooks: IDirect3DDevice9::CreateTexture, then (on the shared vtables) IDirect3DTexture9::LockRect/UnlockRect and
// IDirect3DSurface9::LockRect/UnlockRect. A whole, writable lock of mip 0 is hashed at unlock, before the data goes to
// the backend. Each new hash is written once as <folder>\<HASH>.dds (mip 0 only) plus a line in <folder>\harvest.tsv
// with format, size, pool, the loaded stage id (RBRMapSettings.trackID at 0x1660804, NGPCarMenu RBRAPI) and the game
// mode. Hashes already in harvest.tsv are not dumped again. Nothing here changes what the game or Remix see.
//
// RBR fills SYSTEMMEM textures and draws DEFAULT-pool copies made by IDirect3DDevice9::UpdateTexture (re/kb
// materials-textures), so UpdateTexture is hooked too: the copy inherits the source's hash and min alpha, which is
// also how Remix names it (d3d9_device.cpp UpdateTexture -> SetupForRtxFrom(src)).
#define CINTERFACE
#include "common.hpp"
#include "alpha_min.hpp"
#include <d3d9.h>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#define XXH_INLINE_ALL
#include "third_party/xxhash.h"

namespace {

#define hlog(...) rbrrtx_log_to(L"rbrrtx_harvest.log", __VA_ARGS__)

struct {
  decltype(IDirect3DDevice9Vtbl::CreateTexture) CreateTexture;
  decltype(IDirect3DDevice9Vtbl::UpdateTexture) UpdateTexture;
  decltype(IDirect3DTexture9Vtbl::LockRect) TexLock;
  decltype(IDirect3DTexture9Vtbl::UnlockRect) TexUnlock;
  decltype(IDirect3DSurface9Vtbl::LockRect) SurfLock;
  decltype(IDirect3DSurface9Vtbl::UnlockRect) SurfUnlock;
} ho{};

struct Lock { void* bits; INT pitch; };
std::mutex g_mx;
std::unordered_map<void*, Lock> g_texLocks, g_surfLocks;   // texture (level 0) / surface -> current whole lock
std::unordered_set<unsigned long long> g_seen;
std::unordered_map<void*, unsigned long long> g_texHash;   // texture object -> hash of its mip 0 (this run)
std::unordered_map<void*, int> g_texMinAlpha;              // texture object -> smallest alpha of its mip 0 (-1 unknown)
std::unordered_set<std::wstring> g_usage;                  // "shader|hash" pairs already in usage.tsv
FILE* g_usageTsv = nullptr;
std::wstring g_dir;
FILE* g_tsv = nullptr;
unsigned g_seq = 0, g_new = 0, g_dup = 0, g_skipped = 0;
unsigned g_updates = 0, g_updatesKnown = 0;               // UpdateTexture calls / ones whose source had a hash

template <class F>
void patch(F* slot, F hook, F& orig) {
  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(F), PAGE_EXECUTE_READWRITE, &old)) return;
  if (*slot != hook) { orig = *slot; *slot = hook; }
  VirtualProtect(slot, sizeof(F), old, &old);
}

// Game state, read defensively (fixed addresses in RichardBurnsRally_SSE.exe).
int read_int(uintptr_t addr) {
  __try { return *reinterpret_cast<volatile int*>(addr); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int game_mode() {
  __try {
    const uintptr_t cfg = *reinterpret_cast<volatile uintptr_t*>(0x007EAC48);
    return cfg ? *reinterpret_cast<volatile int*>(cfg + 0x728) : -1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Tight row size and row count of mip 0, 0 if the format is not handled.
bool layout(D3DFORMAT f, UINT w, UINT h, UINT& rowBytes, UINT& rows, const char*& name) {
  UINT block = 0, bpp = 0;
  switch (f) {
    case D3DFMT_DXT1: block = 8; name = "DXT1"; break;
    case D3DFMT_DXT2: block = 16; name = "DXT2"; break;
    case D3DFMT_DXT3: block = 16; name = "DXT3"; break;
    case D3DFMT_DXT4: block = 16; name = "DXT4"; break;
    case D3DFMT_DXT5: block = 16; name = "DXT5"; break;
    case D3DFMT_A8R8G8B8: bpp = 4; name = "A8R8G8B8"; break;
    case D3DFMT_X8R8G8B8: bpp = 4; name = "X8R8G8B8"; break;
    case D3DFMT_R5G6B5: bpp = 2; name = "R5G6B5"; break;
    case D3DFMT_A1R5G5B5: bpp = 2; name = "A1R5G5B5"; break;
    case D3DFMT_A4R4G4B4: bpp = 2; name = "A4R4G4B4"; break;
    case D3DFMT_A8L8: bpp = 2; name = "A8L8"; break;
    case D3DFMT_L8: bpp = 1; name = "L8"; break;
    case D3DFMT_A8: bpp = 1; name = "A8"; break;
    default: return false;
  }
  if (block) { rowBytes = ((w + 3) / 4) * block; rows = (h + 3) / 4; }
  else { rowBytes = w * bpp; rows = h; }
  return true;
}

void write_dds(const wchar_t* path, D3DFORMAT f, UINT w, UINT h, const std::string& data) {
  DWORD hdr[32] = {};
  hdr[0] = 0x20534444;                               // "DDS "
  hdr[1] = 124;
  hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000;                 // caps | height | width | pixelformat
  hdr[3] = h; hdr[4] = w; hdr[7] = 1;                // one mip level
  hdr[19] = 32;                                      // pixel format size
  if (f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5) {
    hdr[2] |= 0x80000; hdr[5] = static_cast<DWORD>(data.size());
    hdr[20] = 0x4; hdr[21] = static_cast<DWORD>(f);  // FOURCC (D3DFMT_DXTn values are the FOURCC codes)
  } else {
    hdr[2] |= 0x8;
    struct M { D3DFORMAT f; DWORD flags, bits, r, g, b, a; };
    static const M masks[] = {
      { D3DFMT_A8R8G8B8, 0x41, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000 }, { D3DFMT_X8R8G8B8, 0x40, 32, 0xFF0000, 0xFF00, 0xFF, 0 },
      { D3DFMT_R5G6B5, 0x40, 16, 0xF800, 0x7E0, 0x1F, 0 }, { D3DFMT_A1R5G5B5, 0x41, 16, 0x7C00, 0x3E0, 0x1F, 0x8000 },
      { D3DFMT_A4R4G4B4, 0x41, 16, 0xF00, 0xF0, 0xF, 0xF000 }, { D3DFMT_A8L8, 0x20001, 16, 0xFF, 0, 0, 0xFF00 },
      { D3DFMT_L8, 0x20000, 8, 0xFF, 0, 0, 0 }, { D3DFMT_A8, 0x2, 8, 0, 0, 0, 0xFF } };
    for (const M& m : masks)
      if (m.f == f) { hdr[20] = m.flags; hdr[22] = m.bits; hdr[23] = m.r; hdr[24] = m.g; hdr[25] = m.b; hdr[26] = m.a; }
    hdr[5] = w * (hdr[22] / 8);
  }
  hdr[27] = 0x1000;                                  // DDSCAPS_TEXTURE
  FILE* fp = nullptr;
  if (_wfopen_s(&fp, path, L"wb") || !fp) return;
  fwrite(hdr, sizeof(hdr), 1, fp);
  fwrite(data.data(), 1, data.size(), fp);
  fclose(fp);
}

// Hashes mip 0 of a texture whose level-0 data is mapped at `lk` (caller holds g_mx).
void record(IDirect3DTexture9* tex, const D3DSURFACE_DESC& d, const Lock& lk) {
  UINT rowBytes = 0, rows = 0;
  const char* fmt = "?";
  if (g_dir.empty() && (d.Usage & D3DUSAGE_DYNAMIC)) return;   // hash only: no per-frame cost for streamed textures
  if (!lk.bits || !layout(d.Format, d.Width, d.Height, rowBytes, rows, fmt) || lk.pitch < static_cast<INT>(rowBytes)) {
    ++g_skipped;
    return;
  }
  std::string data(static_cast<size_t>(rowBytes) * rows, '\0');
  for (UINT r = 0; r < rows; ++r)
    memcpy(&data[static_cast<size_t>(r) * rowBytes], static_cast<const char*>(lk.bits) + static_cast<size_t>(r) * lk.pitch, rowBytes);
  const unsigned long long hx = XXH3_64bits(data.data(), data.size());
  if (tex) {
    g_texHash[tex] = hx;
    g_texMinAlpha[tex] = rbrrtx_min_alpha(d.Format, reinterpret_cast<const unsigned char*>(data.data()), data.size());
  }
  if (g_dir.empty()) return;                         // hash only (stage sweep), no harvest folder
  ++g_seq;
  if (!g_seen.insert(hx).second) { ++g_dup; return; }
  ++g_new;
  wchar_t path[MAX_PATH * 2];
  swprintf_s(path, L"%s\\%016llX.dds", g_dir.c_str(), hx);
  write_dds(path, d.Format, d.Width, d.Height, data);
  if (g_tsv) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_tsv, "%016llX\t%s\t%u\t%u\t%lu\t%lu\t%lu\t%d\t%d\t%u\t%02u:%02u:%02u\n", hx, fmt, d.Width, d.Height,
            tex ? tex->lpVtbl->GetLevelCount(tex) : 0, static_cast<unsigned long>(d.Pool), d.Usage,
            read_int(0x01660804), game_mode(), g_seq, t.wHour, t.wMinute, t.wSecond);
    fflush(g_tsv);
  }
  if (g_new % 250 == 0) hlog(L"%u new textures (%u repeats, %u skipped), stage id %d", g_new, g_dup, g_skipped, read_int(0x01660804));
}

bool whole_write_lock(const RECT* rect, DWORD flags) { return !rect && !(flags & D3DLOCK_READONLY); }

HRESULT STDMETHODCALLTYPE hk_TexLock(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags) {
  const HRESULT hr = ho.TexLock(t, level, lr, rect, flags);
  if (SUCCEEDED(hr) && level == 0 && lr && whole_write_lock(rect, flags)) {
    std::lock_guard<std::mutex> g(g_mx);
    g_texLocks[t] = { lr->pBits, lr->Pitch };
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_TexUnlock(IDirect3DTexture9* t, UINT level) {
  if (level == 0) {
    std::lock_guard<std::mutex> g(g_mx);
    auto it = g_texLocks.find(t);
    if (it != g_texLocks.end()) {
      D3DSURFACE_DESC d = {};
      if (SUCCEEDED(t->lpVtbl->GetLevelDesc(t, 0, &d))) record(t, d, it->second);
      g_texLocks.erase(it);
    }
  }
  return ho.TexUnlock(t, level);
}

HRESULT STDMETHODCALLTYPE hk_SurfLock(IDirect3DSurface9* s, D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags) {
  const HRESULT hr = ho.SurfLock(s, lr, rect, flags);
  if (SUCCEEDED(hr) && lr && whole_write_lock(rect, flags)) {
    std::lock_guard<std::mutex> g(g_mx);
    g_surfLocks[s] = { lr->pBits, lr->Pitch };
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_SurfUnlock(IDirect3DSurface9* s) {
  Lock lk = {};
  bool had = false;
  {
    std::lock_guard<std::mutex> g(g_mx);
    auto it = g_surfLocks.find(s);
    if (it != g_surfLocks.end()) { lk = it->second; had = true; g_surfLocks.erase(it); }
  }
  if (had) {                                         // only mip 0 of a texture counts
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(s->lpVtbl->GetContainer(s, IID_IDirect3DTexture9, reinterpret_cast<void**>(&tex))) && tex) {
      IDirect3DSurface9* s0 = nullptr;
      if (SUCCEEDED(tex->lpVtbl->GetSurfaceLevel(tex, 0, &s0)) && s0) {
        if (s0 == s) {
          D3DSURFACE_DESC d = {};
          std::lock_guard<std::mutex> g(g_mx);
          if (SUCCEEDED(s->lpVtbl->GetDesc(s, &d))) record(tex, d, lk);
        }
        s0->lpVtbl->Release(s0);
      }
      tex->lpVtbl->Release(tex);
    }
  }
  return ho.SurfUnlock(s);
}

HRESULT STDMETHODCALLTYPE hk_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT hgt, UINT levels, DWORD usage, D3DFORMAT fmt,
                                           D3DPOOL pool, IDirect3DTexture9** out, HANDLE* share) {
  const HRESULT hr = ho.CreateTexture(dev, w, hgt, levels, usage, fmt, pool, out, share);
  if (SUCCEEDED(hr) && out && *out) {
    IDirect3DTexture9* t = *out;
    {
      // A new object may sit at the address of a released one: forget that one's hash and min alpha until this
      // texture's own mip 0 is recorded (otherwise a see-through texture could inherit "opaque" and lose its test).
      std::lock_guard<std::mutex> g(g_mx);
      g_texHash.erase(t);
      g_texMinAlpha.erase(t);
    }
    patch(&t->lpVtbl->LockRect, &hk_TexLock, ho.TexLock);
    patch(&t->lpVtbl->UnlockRect, &hk_TexUnlock, ho.TexUnlock);
    IDirect3DSurface9* s0 = nullptr;
    if (!ho.SurfLock && SUCCEEDED(t->lpVtbl->GetSurfaceLevel(t, 0, &s0)) && s0) {
      patch(&s0->lpVtbl->LockRect, &hk_SurfLock, ho.SurfLock);
      patch(&s0->lpVtbl->UnlockRect, &hk_SurfUnlock, ho.SurfUnlock);
      s0->lpVtbl->Release(s0);
    }
  }
  return hr;
}

// SYSTEMMEM -> DEFAULT copy: the destination is what draws bind. Only whole-texture copies count (a dirty-rect update
// of part of mip 0 would make the destination a mix; RBR copies whole textures).
HRESULT STDMETHODCALLTYPE hk_UpdateTexture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) {
  const HRESULT hr = ho.UpdateTexture(dev, src, dst);
  if (SUCCEEDED(hr) && src && dst && src != dst) {
    std::lock_guard<std::mutex> g(g_mx);
    ++g_updates;
    auto h = g_texHash.find(src);
    auto a = g_texMinAlpha.find(src);
    if (h != g_texHash.end()) { g_texHash[dst] = h->second; ++g_updatesKnown; } else g_texHash.erase(dst);
    if (a != g_texMinAlpha.end()) g_texMinAlpha[dst] = a->second; else g_texMinAlpha.erase(dst);
    if (g_updates == 1 || g_updates % 500 == 0)
      hlog(L"UpdateTexture: %u copies, %u with a known source hash", g_updates, g_updatesKnown);
  }
  return hr;
}

}  // namespace

// Usage: which shader draws which texture (stage 0), with the blend/alpha state, once per (shader, hash, state).
// Gives a class to textures without names (encrypted stages: foliage = RLSingleTextureSway, particles = Particle.vso).
void rbrrtx_harvest_note_draw(void* device, const wchar_t* shader) {
  if (!g_usageTsv || !device) return;
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  IDirect3DBaseTexture9* bt = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &bt);
  if (!bt) return;
  unsigned long long hx = 0;
  {
    std::lock_guard<std::mutex> g(g_mx);
    auto it = g_texHash.find(bt);
    if (it != g_texHash.end()) hx = it->second;
  }
  bt->lpVtbl->Release(bt);
  if (!hx) return;
  DWORD at = 0, blend = 0, zw = 0, ref = 0;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zw);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &ref);
  wchar_t key[160];
  swprintf_s(key, L"%s|%016llX|%lu%lu%lu", shader, hx, at, blend, zw);
  std::lock_guard<std::mutex> g(g_mx);
  if (!g_usage.insert(key).second) return;
  fwprintf(g_usageTsv, L"%016llX\t%s\t%lu\t%lu\t%lu\t%lu\t%d\n", hx, shader, at, ref, blend, zw, read_int(0x01660804));
  fflush(g_usageTsv);
}

unsigned long long rbrrtx_texture_hash(void* base_texture) {
  if (!base_texture) return 0;
  std::lock_guard<std::mutex> g(g_mx);
  auto it = g_texHash.find(base_texture);
  return it != g_texHash.end() ? it->second : 0;
}

int rbrrtx_texture_min_alpha(void* base_texture) {
  if (!base_texture) return -1;
  std::lock_guard<std::mutex> g(g_mx);
  auto it = g_texMinAlpha.find(base_texture);
  return it != g_texMinAlpha.end() ? it->second : -1;
}

void rbrrtx_harvest_install(void* device) {
  wchar_t dir[MAX_PATH] = {};
  if (!device) return;
  if (!GetEnvironmentVariableW(L"RBRRTX_HARVEST", dir, MAX_PATH) || !dir[0] || !wcscmp(dir, L"0")) {
    // Hash only: the stage sweep (Alt+K) lists each draw's textures by their Remix hash. One XXH3 per texture upload
    // (stage load); nothing written. RBRRTX_TEXHASH=0 = no hooks at all.
    if (!rbrrtx_env_flag(L"RBRRTX_TEXHASH", true)) return;
    IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
    patch(&dev->lpVtbl->CreateTexture, &hk_CreateTexture, ho.CreateTexture);
    patch(&dev->lpVtbl->UpdateTexture, &hk_UpdateTexture, ho.UpdateTexture);
    return;
  }
  g_dir = dir;
  CreateDirectoryW(dir, nullptr);
  const std::wstring tsv = g_dir + L"\\harvest.tsv";
  FILE* in = nullptr;
  if (!_wfopen_s(&in, tsv.c_str(), L"r") && in) {   // earlier runs: do not dump those hashes again
    char line[512];
    while (fgets(line, sizeof(line), in)) {
      unsigned long long hx = 0;
      if (sscanf_s(line, "%llx", &hx) == 1) g_seen.insert(hx);
    }
    fclose(in);
  }
  const bool fresh = g_seen.empty();
  if (_wfopen_s(&g_tsv, tsv.c_str(), L"a") || !g_tsv) g_tsv = nullptr;
  if (g_tsv && fresh) fprintf(g_tsv, "hash\tformat\twidth\theight\tlevels\tpool\tusage\tstage_id\tgame_mode\tseq\ttime\n");
  const std::wstring usage = g_dir + L"\\usage.tsv";
  const bool newUsage = GetFileAttributesW(usage.c_str()) == INVALID_FILE_ATTRIBUTES;
  if (_wfopen_s(&g_usageTsv, usage.c_str(), L"a") || !g_usageTsv) g_usageTsv = nullptr;
  if (g_usageTsv && newUsage) fwprintf(g_usageTsv, L"hash\tshader\talphatest\talpharef\tblend\tzwrite\tstage_id\n");
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  patch(&dev->lpVtbl->CreateTexture, &hk_CreateTexture, ho.CreateTexture);
  patch(&dev->lpVtbl->UpdateTexture, &hk_UpdateTexture, ho.UpdateTexture);
  hlog(L"harvest on: %s (%zu hashes from earlier runs)", dir, g_seen.size());
}
