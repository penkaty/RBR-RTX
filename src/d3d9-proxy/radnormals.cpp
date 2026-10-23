// Radial foliage normals (prototype, docs/research/foliage-normals.md): RBR's foliage cards carry no vertex normals, so
// Remix shades every card with its flat triangle normal (paper-cutout look). For a fixed-function foliage draw this
// builds a FLOAT3 normal per vertex pointing out of the card's clump (Polycount "bent normals", SpeedTree
// "puffiness"), in a buffer numbered like the draw's stream 0; comp.cpp binds it as an extra stream with a NORMAL
// element. Positions and indices are untouched (same triangles, same geometry hash for texture/material mods).
//
// Per draw range, once (READONLY locks, like alphacut.cpp): cards = triangles joined by shared vertices; clumps =
// cards whose centres lie within `clump_radius` of a clump centre (two passes: greedy leaders, then nearest centre).
// Normal = normalize(mix * (p - centre) + (1 - mix) * card normal turned outward) + up_bias * up. A clump of one card
// (a flag, a lone grass sprite) keeps its card normal. RL* positions are world space, Y up.
// Remix flips a shading normal that points away from the ray's side of the card (surface_interaction.slangh:381), so
// outward normals hold for cards seen from outside the clump, both faces; rays from inside get the flipped normal.
// Remix's geometry generation hash does not include normals: changed settings show after the next stage load.
#define CINTERFACE
#include "common.hpp"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

#define nlog(...) rbrrtx_log_to(L"rbrrtx_foliage_normals.log", __VA_ARGS__)

struct Cfg {
  bool enabled = false;
  float clumpRadius = 2.5f;                            // metres: card centres this close to a clump centre join it
  float mix = 1.0f;                                    // 1 = fully radial, 0 = card normals (as without this)
  float upBias = 0.0f;                                 // added along +Y before normalising (brighter tops)
  float centreDrop = 0.0f;                             // clump centre moved down by this x clump height (tilts up)
  bool debug = false;                                  // every normal +X: shows whether Remix uses the stream at all
} g_cfg;
wchar_t g_cfgPath[MAX_PATH * 2] = {};

// One normal buffer per game vertex buffer (+ stream offset and stride), entry i = vertex i of that stream.
struct BufKey {
  const void* vb;
  unsigned gen, offset, stride;
  bool operator==(const BufKey& o) const { return vb == o.vb && gen == o.gen && offset == o.offset && stride == o.stride; }
};
struct BufKeyHash {
  size_t operator()(const BufKey& k) const {
    return std::hash<const void*>()(k.vb) ^ (static_cast<size_t>(k.gen) * 0x9E3779B9u) ^ (k.offset << 7) ^ (k.stride << 19);
  }
};
std::unordered_map<BufKey, IDirect3DVertexBuffer9*, BufKeyHash> g_bufs;   // nullptr = failed
// Draw ranges already filled (or failed): hash of buffers, generations and range -> ok.
std::unordered_map<uint64_t, bool> g_ranges;
std::unordered_map<const void*, unsigned> g_gen;       // bumped when the game creates a buffer at an address

struct Stats { unsigned bound = 0, built = 0, failed = 0, clumps = 0, cards = 0; double buildMs = 0; } g_s;

float cfg_float(const wchar_t* key, float dflt, float lo, float hi) {
  wchar_t v[32];
  GetPrivateProfileStringW(L"radial_normals", key, L"", v, 32, g_cfgPath);
  if (!v[0]) return dflt;
  return std::clamp(static_cast<float>(_wtof(v)), lo, hi);
}

void release_all() {
  for (auto& kv : g_bufs) if (kv.second) kv.second->lpVtbl->Release(kv.second);
  g_bufs.clear();
  g_ranges.clear();
}

uint64_t range_hash(const void* vb, const void* ib, unsigned vbGen, unsigned ibGen, unsigned offset, unsigned stride,
                    int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims) {
  const uint64_t v[] = { reinterpret_cast<uintptr_t>(vb), reinterpret_cast<uintptr_t>(ib), vbGen, ibGen, offset, stride,
                         static_cast<uint64_t>(static_cast<uint32_t>(base)), minIdx, numV, startIdx, prims };
  uint64_t h = 1469598103934665603ull;
  for (uint64_t x : v) h = (h ^ x) * 1099511628211ull;
  return h;
}

int find(std::vector<uint32_t>& p, uint32_t x) {
  while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
  return static_cast<int>(x);
}

struct V3 { float x, y, z; };
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 norm(V3 a, V3 fallback) {
  const float l = std::sqrt(dot(a, a));
  return l > 1e-6f ? a * (1.0f / l) : fallback;
}

// Fills the normals of one draw range into nb. False = leave the draw without normals (logged).
bool build(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib, IDirect3DVertexDeclaration9* decl, IDirect3DVertexBuffer9* nb,
           unsigned offset, unsigned stride, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims) {
  LARGE_INTEGER t0, t1, qpf;
  QueryPerformanceCounter(&t0);
  int posOff = -1;
  {
    D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (FAILED(decl->lpVtbl->GetDeclaration(decl, e, &n))) return false;
    for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i)
      if (e[i].Stream == 0 && e[i].Usage == D3DDECLUSAGE_POSITION && e[i].UsageIndex == 0 &&
          (e[i].Type == D3DDECLTYPE_FLOAT3 || e[i].Type == D3DDECLTYPE_FLOAT4)) posOff = e[i].Offset;
  }
  if (posOff < 0 || static_cast<unsigned>(posOff) + 12 > stride) { nlog(L"no float position in stream 0"); return false; }
  D3DINDEXBUFFER_DESC id = {};
  D3DVERTEXBUFFER_DESC vd = {};
  if (FAILED(ib->lpVtbl->GetDesc(ib, &id)) || FAILED(vb->lpVtbl->GetDesc(vb, &vd))) return false;
  const UINT isz = id.Format == D3DFMT_INDEX32 ? 4 : 2;
  const UINT iStart = startIdx * isz, iBytes = prims * 3 * isz;
  const UINT first = static_cast<UINT>(base) + minIdx;  // first vertex of the range, in stream entries
  const UINT vStart = offset + first * stride, vBytes = numV * stride;
  if (iStart + iBytes > id.Size || vStart + vBytes > vd.Size) { nlog(L"draw range outside its buffers"); return false; }
  std::vector<uint32_t> idx(static_cast<size_t>(prims) * 3);
  void* p = nullptr;
  if (FAILED(ib->lpVtbl->Lock(ib, iStart, iBytes, &p, D3DLOCK_READONLY)) || !p) { nlog(L"index buffer not readable"); return false; }
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = isz == 4 ? static_cast<const uint32_t*>(p)[i] : static_cast<const uint16_t*>(p)[i];
  ib->lpVtbl->Unlock(ib);
  std::vector<V3> pos(numV);
  if (FAILED(vb->lpVtbl->Lock(vb, vStart, vBytes, &p, D3DLOCK_READONLY)) || !p) { nlog(L"vertex buffer not readable"); return false; }
  for (UINT i = 0; i < numV; ++i) std::memcpy(&pos[i], static_cast<const uint8_t*>(p) + static_cast<size_t>(i) * stride + posOff, 12);
  vb->lpVtbl->Unlock(vb);
  for (uint32_t& i : idx) {
    if (i < minIdx || i - minIdx >= numV) { nlog(L"index outside the draw's vertex range"); return false; }
    i -= minIdx;
  }

  // Cards: vertices joined through triangles.
  std::vector<uint32_t> parent(numV);
  for (UINT i = 0; i < numV; ++i) parent[i] = i;
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const int a = find(parent, idx[t]), b = find(parent, idx[t + 1]), c = find(parent, idx[t + 2]);
    parent[b] = a;
    parent[find(parent, c)] = a;
  }
  std::vector<int> cardOf(numV, -1);
  std::vector<V3> cardC, cardN;                        // centre (vertex mean), area-weighted normal
  std::vector<unsigned> cardCount;
  std::vector<char> used(numV, 0);
  for (uint32_t i : idx) used[i] = 1;
  for (UINT i = 0; i < numV; ++i) {
    if (!used[i]) continue;
    const int r = find(parent, i);
    if (cardOf[r] < 0) { cardOf[r] = static_cast<int>(cardC.size()); cardC.push_back({}); cardN.push_back({}); cardCount.push_back(0); }
    const int c = cardOf[r];
    cardOf[i] = c;
    cardC[c] = cardC[c] + pos[i];
    ++cardCount[c];
  }
  const size_t nCards = cardC.size();
  for (size_t c = 0; c < nCards; ++c) cardC[c] = cardC[c] * (1.0f / cardCount[c]);
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const V3 a = pos[idx[t]], b = pos[idx[t + 1]], c = pos[idx[t + 2]];
    const int k = cardOf[idx[t]];
    cardN[k] = cardN[k] + cross(b - a, c - a);
  }

  // Clumps: pass 1 greedy leaders on a grid of clump_radius cells, pass 2 every card to the nearest clump centre.
  const float R = g_cfg.clumpRadius, R2 = R * R;
  auto cell = [&](V3 q) {
    const int64_t x = static_cast<int64_t>(std::floor(q.x / R)), y = static_cast<int64_t>(std::floor(q.y / R)),
                  z = static_cast<int64_t>(std::floor(q.z / R));
    return std::make_tuple(x, y, z);
  };
  auto key = [](int64_t x, int64_t y, int64_t z) {
    return (static_cast<uint64_t>(x & 0x1FFFFF) << 42) | (static_cast<uint64_t>(y & 0x1FFFFF) << 21) | static_cast<uint64_t>(z & 0x1FFFFF);
  };
  std::vector<V3> centre;
  std::unordered_multimap<uint64_t, int> grid;
  auto nearest = [&](V3 q, float maxD2) {
    auto [cx, cy, cz] = cell(q);
    int best = -1;
    float bd = maxD2;
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz) {
      auto rg = grid.equal_range(key(cx + dx, cy + dy, cz + dz));
      for (auto it = rg.first; it != rg.second; ++it) {
        const V3 d = centre[it->second] - q;
        const float d2 = dot(d, d);
        if (d2 < bd) { bd = d2; best = it->second; }
      }
    }
    return best;
  };
  std::vector<int> clumpOf(nCards);
  for (size_t c = 0; c < nCards; ++c) {
    int k = nearest(cardC[c], R2);
    if (k < 0) {
      k = static_cast<int>(centre.size());
      centre.push_back(cardC[c]);
      auto [x, y, z] = cell(cardC[c]);
      grid.emplace(key(x, y, z), k);
    }
    clumpOf[c] = k;
  }
  const size_t nClumps = centre.size();
  auto recentre = [&](std::vector<V3>& out, std::vector<unsigned>& members, std::vector<float>& lo, std::vector<float>& hi) {
    out.assign(nClumps, {});
    members.assign(nClumps, 0);
    lo.assign(nClumps, 1e30f);
    hi.assign(nClumps, -1e30f);
    for (size_t c = 0; c < nCards; ++c) {
      const int k = clumpOf[c];
      out[k] = out[k] + cardC[c];
      ++members[k];
      lo[k] = (std::min)(lo[k], cardC[c].y);
      hi[k] = (std::max)(hi[k], cardC[c].y);
    }
    for (size_t k = 0; k < nClumps; ++k) if (members[k]) out[k] = out[k] * (1.0f / members[k]);
  };
  std::vector<V3> mean;
  std::vector<unsigned> members;
  std::vector<float> lo, hi;
  recentre(mean, members, lo, hi);
  grid.clear();
  for (size_t k = 0; k < nClumps; ++k) {
    centre[k] = mean[k];
    auto [x, y, z] = cell(centre[k]);
    grid.emplace(key(x, y, z), static_cast<int>(k));
  }
  for (size_t c = 0; c < nCards; ++c) {
    const int k = nearest(cardC[c], 2.25f * R2);
    if (k >= 0) clumpOf[c] = k;
  }
  recentre(mean, members, lo, hi);

  // Normals, written to entries first .. first + numV - 1 of the normal buffer.
  const V3 up = { 0.0f, 1.0f, 0.0f };
  std::vector<V3> out(numV, up);
  for (UINT i = 0; i < numV; ++i) {
    const int c = cardOf[i];
    if (c < 0) continue;
    const int k = clumpOf[c];
    V3 fn = norm(cardN[c], up);
    V3 n = fn;
    if (members[k] > 1) {
      V3 ctr = mean[k];
      ctr.y -= g_cfg.centreDrop * (hi[k] - lo[k]);
      const V3 cardOut = cardC[c] - ctr;
      if (dot(fn, cardOut) < 0) fn = fn * -1.0f;
      const V3 radial = norm(pos[i] - ctr, norm(cardOut, fn));
      n = norm(radial * g_cfg.mix + fn * (1.0f - g_cfg.mix), fn);
    }
    out[i] = g_cfg.debug ? V3{ 1.0f, 0.0f, 0.0f } : norm(n + up * g_cfg.upBias, n);
  }
  if (FAILED(nb->lpVtbl->Lock(nb, first * 12, numV * 12, &p, 0)) || !p) { nlog(L"normal buffer not writable"); return false; }
  std::memcpy(p, out.data(), static_cast<size_t>(numV) * 12);
  nb->lpVtbl->Unlock(nb);

  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&qpf);
  const double ms = 1000.0 * (t1.QuadPart - t0.QuadPart) / qpf.QuadPart;
  unsigned lone = 0;
  for (size_t k = 0; k < nClumps; ++k) if (members[k] == 1) ++lone;
  g_s.buildMs += ms;
  g_s.clumps += static_cast<unsigned>(nClumps);
  g_s.cards += static_cast<unsigned>(nCards);
  nlog(L"vb %p entries %u..%u: %u triangles, %zu cards, %zu clumps (%u of one card, kept flat); %.1f ms", vb, first,
       first + numV - 1, prims, nCards, nClumps, lone, ms);
  return true;
}

}  // namespace

void rbrrtx_radnorm_load() {
  if (!g_cfgPath[0]) rbrrtx_config_path(L"RBRRTX_FOLIAGE_NORMALS", L"foliage-normals.ini", g_cfgPath, MAX_PATH * 2);
  Cfg c;
  wchar_t v[8];
  GetPrivateProfileStringW(L"radial_normals", L"enabled", L"0", v, 8, g_cfgPath);
  c.enabled = _wtoi(v) != 0;
  if (GetEnvironmentVariableW(L"RBRRTX_RADNORM", v, 8)) c.enabled = _wtoi(v) != 0;
  c.clumpRadius = cfg_float(L"clump_radius", c.clumpRadius, 0.2f, 50.0f);
  c.mix = cfg_float(L"mix", c.mix, 0.0f, 1.0f);
  c.upBias = cfg_float(L"up_bias", c.upBias, 0.0f, 5.0f);
  c.centreDrop = cfg_float(L"centre_drop", c.centreDrop, 0.0f, 2.0f);
  c.debug = cfg_float(L"debug_normals", 0.0f, 0.0f, 1.0f) != 0.0f;
  const bool changed = c.clumpRadius != g_cfg.clumpRadius || c.mix != g_cfg.mix || c.upBias != g_cfg.upBias ||
                       c.centreDrop != g_cfg.centreDrop || c.debug != g_cfg.debug;
  g_cfg = c;
  if (changed) release_all();                          // rebuilt with the new values on next sight
  nlog(L"radial foliage normals %s (%s): clump_radius %.2f m, mix %.2f, up_bias %.2f, centre_drop %.2f%s",
       g_cfg.enabled ? L"ON" : L"off", g_cfgPath, g_cfg.clumpRadius, g_cfg.mix, g_cfg.upBias, g_cfg.centreDrop,
       g_cfg.debug ? L"; DEBUG: every normal +X" : L"");
}

bool rbrrtx_radnorm_enabled() { return g_cfg.enabled; }

bool rbrrtx_radnorm_toggle() {
  const bool was = g_cfg.enabled;
  rbrrtx_radnorm_load();                               // picks up edited values (shown after the next stage load)
  g_cfg.enabled = !was;
  return g_cfg.enabled;
}

void* rbrrtx_radnorm_find(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims) {
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (!g_cfg.enabled || !dev || base < 0 || !prims || !numV) return nullptr;
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  IDirect3DVertexDeclaration9* decl = nullptr;
  UINT offset = 0, stride = 0;
  dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride);
  dev->lpVtbl->GetIndices(dev, &ib);
  dev->lpVtbl->GetVertexDeclaration(dev, &decl);
  IDirect3DVertexBuffer9* nb = nullptr;
  if (vb && ib && decl && stride) {
    const BufKey bk{ vb, g_gen[vb], offset, stride };
    auto bit = g_bufs.find(bk);
    if (bit == g_bufs.end()) {
      D3DVERTEXBUFFER_DESC vd = {};
      IDirect3DVertexBuffer9* created = nullptr;
      if (SUCCEEDED(vb->lpVtbl->GetDesc(vb, &vd)) && !(vd.Usage & D3DUSAGE_DYNAMIC) && vd.Size > offset) {
        const UINT entries = (vd.Size - offset) / stride;
        if (SUCCEEDED(dev->lpVtbl->CreateVertexBuffer(dev, entries * 12, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &created, nullptr))) {
          void* p = nullptr;
          if (SUCCEEDED(created->lpVtbl->Lock(created, 0, 0, &p, 0)) && p) {
            for (UINT i = 0; i < entries; ++i) { static const float up[3] = { 0, 1, 0 }; std::memcpy(static_cast<uint8_t*>(p) + i * 12, up, 12); }
            created->lpVtbl->Unlock(created);
          }
        }
      }
      if (!created) nlog(L"vb %p: no normal buffer (dynamic or not created)", vb);
      bit = g_bufs.emplace(bk, created).first;
    }
    if (bit->second) {
      const uint64_t rk = range_hash(vb, ib, g_gen[vb], g_gen[ib], offset, stride, base, minIdx, numV, startIdx, prims);
      auto rit = g_ranges.find(rk);
      if (rit == g_ranges.end()) {
        const bool ok = build(vb, ib, decl, bit->second, offset, stride, base, minIdx, numV, startIdx, prims);
        if (ok) ++g_s.built; else ++g_s.failed;
        rit = g_ranges.emplace(rk, ok).first;
      }
      if (rit->second) nb = bit->second;
    }
  }
  if (vb) vb->lpVtbl->Release(vb);
  if (ib) ib->lpVtbl->Release(ib);
  if (decl) decl->lpVtbl->Release(decl);
  if (nb) ++g_s.bound;
  return nb;
}

void rbrrtx_radnorm_forget(const void* buffer) { if (buffer) ++g_gen[buffer]; }

// Every few hundred frames: free normal buffers of game buffers that were recreated (stage change).
void rbrrtx_radnorm_frame(unsigned frame) {
  if (frame % 300) return;
  for (auto it = g_bufs.begin(); it != g_bufs.end();) {
    if (g_gen[it->first.vb] == it->first.gen) { ++it; continue; }
    if (it->second) it->second->lpVtbl->Release(it->second);
    it = g_bufs.erase(it);
  }
  if (g_ranges.size() > 20000) g_ranges.clear();       // stale ranges just rebuild on sight
}

void rbrrtx_radnorm_summary(wchar_t* out, size_t n, unsigned frames) {
  const double f = frames ? frames : 1;
  swprintf_s(out, n, L"%s; per frame %.0f draws with normals; since last line %u ranges built (%u cards in %u clumps, "
             L"%.0f ms), %u left flat; %zu normal buffers held",
             g_cfg.enabled ? L"ON" : L"off", g_s.bound / f, g_s.built, g_s.cards, g_s.clumps, g_s.buildMs, g_s.failed,
             g_bufs.size());
  g_s = {};
}
