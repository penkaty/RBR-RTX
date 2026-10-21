// Alpha-card cut-outs (docs/research/adr-001-alphacards.md, tiers 2-3): an alpha-tested foliage draw whose stage-0
// texture has an outline file is drawn from our own copy of its cards, clipped in UV space to that outline.
//   cut  (tier 2, file mode 0): the outline follows the alpha test; drawn with alpha test OFF, so Remix's rays never
//        run the any-hit alpha test on those cards;
//   hull (tier 3, file mode 1): a coarse outline holding every passing texel; alpha test stays ON, only the empty
//        card area is gone (fewer any-hit calls, same look).
// Outlines come from tools\alphacut\pick.py: <dir>\<texture hash>.act ("ACT1", u32 mode, u32 aref, u32 n, then n
// triangles as float32 u,v x 3). <dir> = RBRRTX_ALPHACUT_DIR, else <proxy dir>\rbr-rtx\alphacut.
//
// Per card triangle (UV is affine across a triangle, so this is exact): the triangle is clipped against every outline
// triangle under its UV box (grid lookup), each piece fanned into triangles, every new vertex interpolated from the
// card's three vertices (all attributes, barycentric in UV space). Cards that miss the outline vanish. A card whose
// UVs span more than one texture repeat, or that would need more than max_per_tri pieces, is kept as it is; then the
// draw keeps its alpha test even with a cut outline. Same for vertex colour alpha below 255 (the game fades cards).
// The result is cached per draw range (buffers, generations, ranges, texture) and built once, on first sight; a new
// buffer at a known address (CreateVertexBuffer/CreateIndexBuffer -> rbrrtx_alphacut_forget) invalidates entries.
#define CINTERFACE
#include "common.hpp"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

#define alog(...) rbrrtx_log_to(L"rbrrtx_alphacut.log", __VA_ARGS__)

struct Outline {
  int mode = 0, aref = 0;
  std::vector<float> t;                                // 6 floats per triangle, counter-clockwise in (u, v)
  static constexpr int G = 64;                         // grid cells per side over [0,1]^2
  std::vector<uint32_t> cellStart, cellTris;           // triangles per cell (CSR)
};
std::unordered_map<unsigned long long, std::unique_ptr<Outline>> g_outlines;   // texture hash -> outline (null = none)
wchar_t g_dir[MAX_PATH] = {};

struct Key {
  unsigned long long tex;
  const void *vb, *ib, *decl;
  unsigned vbGen, ibGen, offset, stride, minIdx, numV, startIdx, prims;
  int base, aref;
  uint32_t pad;
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    const auto* p = reinterpret_cast<const unsigned char*>(&k);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(Key); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return static_cast<size_t>(h ^ (h >> 32));
  }
};
struct KeyEq { bool operator()(const Key& a, const Key& b) const { return !std::memcmp(&a, &b, sizeof(Key)); } };
struct Mesh {
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  UINT verts = 0, tris = 0, stride = 0;
  bool failed = false, needsTest = false;
};
std::unordered_map<Key, Mesh, KeyHash, KeyEq> g_meshes;
std::unordered_map<const void*, unsigned> g_gen;       // bumped when the game creates a buffer at an address

unsigned g_maxPerTri = 256, g_maxTris = 400000;
struct Stats { unsigned cut = 0, hull = 0, built = 0, failed = 0, noOutline = 0; double buildMs = 0; } g_s;
unsigned g_noHash = 0;

void load_dir() {
  if (g_dir[0]) return;
  const DWORD n = GetEnvironmentVariableW(L"RBRRTX_ALPHACUT_DIR", g_dir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return;
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&load_dir), &self);
  wchar_t mod[MAX_PATH] = {};
  const DWORD len = GetModuleFileNameW(self, mod, MAX_PATH);
  wchar_t* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(mod, L'\\') : nullptr;
  if (slash) *slash = 0;
  swprintf_s(g_dir, L"%s\\rbr-rtx\\alphacut", mod);
}

const Outline* outline_for(unsigned long long hash) {
  auto it = g_outlines.find(hash);
  if (it != g_outlines.end()) return it->second.get();
  std::unique_ptr<Outline>& slot = g_outlines[hash];
  load_dir();
  wchar_t path[MAX_PATH * 2];
  swprintf_s(path, L"%s\\%016llX.act", g_dir, hash);
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"rb") || !f) return nullptr;
  char magic[4] = {};
  uint32_t hdr[3] = {};
  auto o = std::make_unique<Outline>();
  bool valid = false;
  if (fread(magic, 1, 4, f) == 4 && !std::memcmp(magic, "ACT1", 4) && fread(hdr, 4, 3, f) == 3 && hdr[2] < 4000000) {
    o->mode = static_cast<int>(hdr[0]);
    o->aref = static_cast<int>(hdr[1]);
    o->t.resize(static_cast<size_t>(hdr[2]) * 6);
    valid = fread(o->t.data(), sizeof(float), o->t.size(), f) == o->t.size();
  }
  fclose(f);
  if (!valid) { alog(L"%016llX: bad outline file %s", hash, path); return nullptr; }
  // Counter-clockwise (inside = left of every edge), degenerate ones dropped, then the grid.
  std::vector<float> kept;
  for (size_t i = 0; i + 6 <= o->t.size(); i += 6) {
    float* p = &o->t[i];
    const float a = (p[2] - p[0]) * (p[5] - p[1]) - (p[3] - p[1]) * (p[4] - p[0]);
    if (std::fabs(a) < 1e-12f) continue;
    if (a < 0) { std::swap(p[2], p[4]); std::swap(p[3], p[5]); }
    kept.insert(kept.end(), p, p + 6);
  }
  o->t.swap(kept);
  const int G = Outline::G;
  const size_t n = o->t.size() / 6;
  std::vector<uint32_t> count(G * G + 1, 0);
  auto cells = [&](size_t i, int& c0, int& c1, int& r0, int& r1) {
    const float* p = &o->t[i * 6];
    const float lu = (std::min)({p[0], p[2], p[4]}), hu = (std::max)({p[0], p[2], p[4]});
    const float lv = (std::min)({p[1], p[3], p[5]}), hv = (std::max)({p[1], p[3], p[5]});
    c0 = std::clamp(static_cast<int>(lu * G), 0, G - 1); c1 = std::clamp(static_cast<int>(hu * G), 0, G - 1);
    r0 = std::clamp(static_cast<int>(lv * G), 0, G - 1); r1 = std::clamp(static_cast<int>(hv * G), 0, G - 1);
  };
  for (size_t i = 0; i < n; ++i) {
    int c0, c1, r0, r1;
    cells(i, c0, c1, r0, r1);
    for (int r = r0; r <= r1; ++r) for (int c = c0; c <= c1; ++c) ++count[r * G + c + 1];
  }
  for (int k = 1; k <= G * G; ++k) count[k] += count[k - 1];
  o->cellStart = count;
  o->cellTris.resize(count[G * G]);
  for (size_t i = 0; i < n; ++i) {
    int c0, c1, r0, r1;
    cells(i, c0, c1, r0, r1);
    for (int r = r0; r <= r1; ++r) for (int c = c0; c <= c1; ++c) o->cellTris[count[r * G + c]++] = static_cast<uint32_t>(i);
  }
  alog(L"%016llX: outline %s, ALPHAREF %d, %zu triangles", hash, o->mode == 0 ? L"cut" : L"hull", o->aref, n);
  slot = std::move(o);
  return slot.get();
}

struct P { double u, v; };

// Card polygon (in/out, <= 12 points) clipped by one outline triangle (counter-clockwise), Sutherland-Hodgman.
int clip(const P* card, int n, const float* t, P* out) {
  P a[12], b[12];
  std::copy(card, card + n, a);
  for (int e = 0; e < 3 && n >= 3; ++e) {
    const double x0 = t[e * 2], y0 = t[e * 2 + 1], x1 = t[(e + 1) % 3 * 2], y1 = t[(e + 1) % 3 * 2 + 1];
    auto side = [&](const P& p) { return (x1 - x0) * (p.v - y0) - (y1 - y0) * (p.u - x0); };
    int m = 0;
    for (int i = 0; i < n && m < 11; ++i) {
      const P& p = a[i];
      const P& q = a[(i + 1) % n];
      const double sp = side(p), sq = side(q);
      if (sp >= 0) b[m++] = p;
      if ((sp >= 0) != (sq >= 0)) {
        const double k = sp / (sp - sq);
        b[m++] = { p.u + k * (q.u - p.u), p.v + k * (q.v - p.v) };
      }
    }
    n = m;
    std::copy(b, b + m, a);
  }
  if (n < 3) return 0;
  std::copy(a, a + n, out);
  return n;
}

struct Elem { unsigned off; BYTE type; };

// out = w0*v0 + w1*v1 + w2*v2 for every element; bytes no element covers (and integer indices) from the vertex
// with the largest weight.
void lerp_vertex(const uint8_t* const v[3], const double w[3], const std::vector<Elem>& el, UINT stride, uint8_t* out) {
  const int dom = w[0] >= w[1] && w[0] >= w[2] ? 0 : w[1] >= w[2] ? 1 : 2;
  std::memcpy(out, v[dom], stride);
  for (const Elem& e : el) {
    int nf = 0, nb = 0, ns = 0, nus = 0;
    switch (e.type) {
      case D3DDECLTYPE_FLOAT1: nf = 1; break;
      case D3DDECLTYPE_FLOAT2: nf = 2; break;
      case D3DDECLTYPE_FLOAT3: nf = 3; break;
      case D3DDECLTYPE_FLOAT4: nf = 4; break;
      case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4N: nb = 4; break;
      case D3DDECLTYPE_SHORT2: case D3DDECLTYPE_SHORT2N: ns = 2; break;
      case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: ns = 4; break;
      case D3DDECLTYPE_USHORT2N: nus = 2; break;
      case D3DDECLTYPE_USHORT4N: nus = 4; break;
      default: break;                                  // UBYTE4 (indices), FLOAT16, DEC3N...: dominant vertex
    }
    for (int k = 0; k < nf; ++k) {
      float f[3];
      for (int j = 0; j < 3; ++j) std::memcpy(&f[j], v[j] + e.off + 4 * k, 4);
      const float r = static_cast<float>(w[0] * f[0] + w[1] * f[1] + w[2] * f[2]);
      std::memcpy(out + e.off + 4 * k, &r, 4);
    }
    for (int k = 0; k < nb; ++k)
      out[e.off + k] = static_cast<uint8_t>(std::clamp(std::lround(w[0] * v[0][e.off + k] + w[1] * v[1][e.off + k] + w[2] * v[2][e.off + k]), 0L, 255L));
    for (int k = 0; k < ns; ++k) {
      int16_t s[3];
      for (int j = 0; j < 3; ++j) std::memcpy(&s[j], v[j] + e.off + 2 * k, 2);
      const int16_t r = static_cast<int16_t>(std::clamp(std::lround(w[0] * s[0] + w[1] * s[1] + w[2] * s[2]), -32768L, 32767L));
      std::memcpy(out + e.off + 2 * k, &r, 2);
    }
    for (int k = 0; k < nus; ++k) {
      uint16_t s[3];
      for (int j = 0; j < 3; ++j) std::memcpy(&s[j], v[j] + e.off + 2 * k, 2);
      const uint16_t r = static_cast<uint16_t>(std::clamp(std::lround(w[0] * s[0] + w[1] * s[1] + w[2] * s[2]), 0L, 65535L));
      std::memcpy(out + e.off + 2 * k, &r, 2);
    }
  }
}

// Builds the clipped copy of the current draw. False = leave the draw alone (logged).
bool build(IDirect3DDevice9* dev, const Key& k, const Outline& ol, Mesh& m) {
  LARGE_INTEGER t0, t1, qpf;
  QueryPerformanceCounter(&t0);
  // Vertex layout: stream 0 only, POSITION float, TEXCOORD0 float2+.
  std::vector<Elem> el;
  int uvOff = -1, colOff = -1;
  bool posOk = false;
  {
    IDirect3DVertexDeclaration9* decl = static_cast<IDirect3DVertexDeclaration9*>(const_cast<void*>(k.decl));
    D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (!decl || FAILED(decl->lpVtbl->GetDeclaration(decl, e, &n))) { alog(L"%016llX: no vertex declaration", k.tex); return false; }
    for (UINT i = 0; i < n && e[i].Stream != 0xFF; ++i) {
      if (e[i].Stream != 0) { alog(L"%016llX: vertex stream %u used, not handled", k.tex, e[i].Stream); return false; }
      el.push_back({ e[i].Offset, e[i].Type });
      if (e[i].Usage == D3DDECLUSAGE_POSITION && e[i].UsageIndex == 0)
        posOk = e[i].Type == D3DDECLTYPE_FLOAT3 || e[i].Type == D3DDECLTYPE_FLOAT4;
      if (e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex == 0 &&
          (e[i].Type == D3DDECLTYPE_FLOAT2 || e[i].Type == D3DDECLTYPE_FLOAT3 || e[i].Type == D3DDECLTYPE_FLOAT4)) uvOff = e[i].Offset;
      if (e[i].Usage == D3DDECLUSAGE_COLOR && e[i].UsageIndex == 0 && e[i].Type == D3DDECLTYPE_D3DCOLOR) colOff = e[i].Offset;
    }
  }
  if (!posOk || uvOff < 0 || static_cast<unsigned>(uvOff) + 8 > k.stride) { alog(L"%016llX: no float position / texcoord0", k.tex); return false; }
  // Source indices and vertices (READONLY locks: the Remix bridge client keeps a copy, see comp.cpp cull_entry).
  IDirect3DVertexBuffer9* vb = static_cast<IDirect3DVertexBuffer9*>(const_cast<void*>(k.vb));
  IDirect3DIndexBuffer9* ib = static_cast<IDirect3DIndexBuffer9*>(const_cast<void*>(k.ib));
  D3DVERTEXBUFFER_DESC vd = {};
  D3DINDEXBUFFER_DESC id = {};
  if (FAILED(vb->lpVtbl->GetDesc(vb, &vd)) || FAILED(ib->lpVtbl->GetDesc(ib, &id))) return false;
  if (vd.Usage & D3DUSAGE_DYNAMIC) { alog(L"%016llX: dynamic vertex buffer, not cached", k.tex); return false; }
  const UINT isz = id.Format == D3DFMT_INDEX32 ? 4 : 2;
  const UINT iStart = k.startIdx * isz, iBytes = k.prims * 3 * isz;
  const UINT vStart = k.offset + (static_cast<UINT>(k.base) + k.minIdx) * k.stride, vBytes = k.numV * k.stride;
  if (iStart + iBytes > id.Size || vStart + vBytes > vd.Size) { alog(L"%016llX: draw range outside its buffers", k.tex); return false; }
  std::vector<uint32_t> idx(static_cast<size_t>(k.prims) * 3);
  std::vector<uint8_t> src(vBytes);
  void* p = nullptr;
  if (FAILED(ib->lpVtbl->Lock(ib, iStart, iBytes, &p, D3DLOCK_READONLY)) || !p) { alog(L"%016llX: index buffer not readable", k.tex); return false; }
  for (size_t i = 0; i < idx.size(); ++i)
    idx[i] = isz == 4 ? static_cast<const uint32_t*>(p)[i] : static_cast<const uint16_t*>(p)[i];
  ib->lpVtbl->Unlock(ib);
  if (FAILED(vb->lpVtbl->Lock(vb, vStart, vBytes, &p, D3DLOCK_READONLY)) || !p) { alog(L"%016llX: vertex buffer not readable", k.tex); return false; }
  std::memcpy(src.data(), p, vBytes);
  vb->lpVtbl->Unlock(vb);

  const UINT stride = k.stride;
  std::vector<uint8_t> outV;
  std::vector<uint32_t> outI;
  unsigned dropped = 0, whole = 0, cut = 0, repeat = 0, capped = 0;
  bool fade = false;
  std::vector<uint32_t> stamp(ol.t.size() / 6, 0);
  std::unordered_map<uint64_t, uint32_t> dedupe;
  const int G = Outline::G;
  auto emit_whole = [&](const uint8_t* const v[3]) {
    const uint32_t base = static_cast<uint32_t>(outV.size() / stride);
    for (int j = 0; j < 3; ++j) { outV.insert(outV.end(), v[j], v[j] + stride); outI.push_back(base + j); }
    ++whole;
  };
  for (UINT tri = 0; tri < k.prims; ++tri) {
    const uint8_t* v[3];
    bool ok = true;
    for (int j = 0; j < 3; ++j) {
      const uint32_t i = idx[tri * 3 + j];
      if (i < k.minIdx || i - k.minIdx >= k.numV) { ok = false; break; }
      v[j] = &src[static_cast<size_t>(i - k.minIdx) * stride];
    }
    if (!ok) { alog(L"%016llX: index outside the draw's vertex range", k.tex); return false; }
    if (colOff >= 0 && !fade) for (int j = 0; j < 3; ++j) if (v[j][colOff + 3] < 255) fade = true;
    P c[3];
    for (int j = 0; j < 3; ++j) {
      float uv[2];
      std::memcpy(uv, v[j] + uvOff, 8);
      c[j] = { uv[0], uv[1] };
    }
    const double lu = (std::min)({c[0].u, c[1].u, c[2].u}), hu = (std::max)({c[0].u, c[1].u, c[2].u});
    const double lv = (std::min)({c[0].v, c[1].v, c[2].v}), hv = (std::max)({c[0].v, c[1].v, c[2].v});
    if (!std::isfinite(lu + hu + lv + hv)) { emit_whole(v); ++repeat; continue; }
    // One texture repeat: shift into [0,1]. Wider cards stay whole.
    const double fu = std::floor(lu), fv = std::floor(lv);
    if (hu - fu > 1.0 + 1e-5 || hv - fv > 1.0 + 1e-5) { emit_whole(v); ++repeat; continue; }
    P l[3];
    for (int j = 0; j < 3; ++j) l[j] = { c[j].u - fu, c[j].v - fv };
    const double det = (l[1].u - l[0].u) * (l[2].v - l[0].v) - (l[1].v - l[0].v) * (l[2].u - l[0].u);
    if (std::fabs(det) < 1e-14) { emit_whole(v); --whole; continue; }   // no UV area: nothing to clip, nothing to test
    const size_t vMark = outV.size(), iMark = outI.size();
    dedupe.clear();
    unsigned pieces = 0;
    const int c0 = std::clamp(static_cast<int>((lu - fu) * G), 0, G - 1), c1 = std::clamp(static_cast<int>((hu - fu) * G), 0, G - 1);
    const int r0 = std::clamp(static_cast<int>((lv - fv) * G), 0, G - 1), r1 = std::clamp(static_cast<int>((hv - fv) * G), 0, G - 1);
    for (int r = r0; r <= r1 && pieces <= g_maxPerTri; ++r)
      for (int cc = c0; cc <= c1 && pieces <= g_maxPerTri; ++cc) {
        const int cell = r * G + cc;
        for (uint32_t s = ol.cellStart[cell]; s < ol.cellStart[cell + 1] && pieces <= g_maxPerTri; ++s) {
          const uint32_t ot = ol.cellTris[s];
          if (stamp[ot] == tri + 1) continue;
          stamp[ot] = tri + 1;
          P poly[12];
          const int n = clip(l, 3, &ol.t[ot * 6], poly);
          if (n < 3) continue;
          uint32_t ids[12];
          for (int q = 0; q < n; ++q) {
            const double w1 = ((poly[q].u - l[0].u) * (l[2].v - l[0].v) - (poly[q].v - l[0].v) * (l[2].u - l[0].u)) / det;
            const double w2 = ((l[1].u - l[0].u) * (poly[q].v - l[0].v) - (l[1].v - l[0].v) * (poly[q].u - l[0].u)) / det;
            const double w[3] = { 1.0 - w1 - w2, w1, w2 };
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(std::llround(w1 * 1e7))) << 32) |
                                 static_cast<uint32_t>(std::llround(w2 * 1e7));
            auto it = dedupe.find(key);
            if (it != dedupe.end()) { ids[q] = it->second; continue; }
            const uint32_t id2 = static_cast<uint32_t>(outV.size() / stride);
            outV.resize(outV.size() + stride);
            lerp_vertex(v, w, el, stride, &outV[static_cast<size_t>(id2) * stride]);
            dedupe.emplace(key, id2);
            ids[q] = id2;
          }
          for (int q = 1; q + 1 < n; ++q) {
            if (ids[0] == ids[q] || ids[q] == ids[q + 1] || ids[0] == ids[q + 1]) continue;
            outI.push_back(ids[0]); outI.push_back(ids[q]); outI.push_back(ids[q + 1]);
            ++pieces;
          }
        }
      }
    if (pieces > g_maxPerTri) {                        // too finely cut: the card as it was
      outV.resize(vMark);
      outI.resize(iMark);
      emit_whole(v);
      ++capped;
      continue;
    }
    // Winding: clipping keeps the card's vertex order, so the fans face the way the card did.
    if (pieces == 0) ++dropped; else ++cut;
    if (outI.size() / 3 > g_maxTris) { alog(L"%016llX: more than %u triangles, left as cards", k.tex, g_maxTris); return false; }
  }
  m.tris = static_cast<UINT>(outI.size() / 3);
  m.verts = static_cast<UINT>(outV.size() / stride);
  m.stride = stride;
  m.needsTest = ol.mode != 0 || fade || whole > 0;
  if (m.tris) {
    const bool i32 = m.verts > 65535;
    if (FAILED(dev->lpVtbl->CreateVertexBuffer(dev, static_cast<UINT>(outV.size()), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m.vb, nullptr)) ||
        FAILED(dev->lpVtbl->CreateIndexBuffer(dev, static_cast<UINT>(outI.size()) * (i32 ? 4 : 2), D3DUSAGE_WRITEONLY,
                                              i32 ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_MANAGED, &m.ib, nullptr))) {
      alog(L"%016llX: could not create buffers (%u vertices, %u triangles)", k.tex, m.verts, m.tris);
      if (m.vb) { m.vb->lpVtbl->Release(m.vb); m.vb = nullptr; }
      return false;
    }
    if (SUCCEEDED(m.vb->lpVtbl->Lock(m.vb, 0, 0, &p, 0)) && p) { std::memcpy(p, outV.data(), outV.size()); m.vb->lpVtbl->Unlock(m.vb); }
    if (SUCCEEDED(m.ib->lpVtbl->Lock(m.ib, 0, 0, &p, 0)) && p) {
      if (i32) std::memcpy(p, outI.data(), outI.size() * 4);
      else for (size_t i = 0; i < outI.size(); ++i) static_cast<uint16_t*>(p)[i] = static_cast<uint16_t>(outI[i]);
      m.ib->lpVtbl->Unlock(m.ib);
    }
  }
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&qpf);
  const double ms = 1000.0 * (t1.QuadPart - t0.QuadPart) / qpf.QuadPart;
  g_s.buildMs += ms;
  alog(L"%016llX %s: %u card triangles -> %u triangles, %u vertices (%u cut, %u dropped, %u kept whole: %u over a repeat, "
       L"%u over %u pieces); alpha test %s%s; %.1f ms",
       k.tex, ol.mode == 0 ? L"cut" : L"hull", k.prims, m.tris, m.verts, cut, dropped, whole, repeat, capped, g_maxPerTri,
       m.needsTest ? L"kept" : L"OFF", fade ? L" (vertex alpha fades)" : L"", ms);
  return true;
}

}  // namespace

bool rbrrtx_alphacut_find(void* device, int base, unsigned minIdx, unsigned numV, unsigned startIdx, unsigned prims,
                          RbrAlphaCut* out) {
  IDirect3DDevice9* dev = static_cast<IDirect3DDevice9*>(device);
  if (!dev || !out || base < 0 || !prims) return false;
  DWORD at = 0, blend = 0, func = 0, ref = 0;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHATESTENABLE, &at);
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
  if (!at || blend) return false;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAFUNC, &func);
  if (func != D3DCMP_GREATEREQUAL && func != D3DCMP_GREATER) return false;
  dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHAREF, &ref);
  IDirect3DBaseTexture9* bt = nullptr;
  dev->lpVtbl->GetTexture(dev, 0, &bt);
  if (!bt) return false;
  const unsigned long long hx = rbrrtx_texture_hash(bt);
  bt->lpVtbl->Release(bt);
  if (!hx) { ++g_noHash; return false; }
  const Outline* ol = outline_for(hx);
  // An outline traced at a lower ALPHAREF holds everything a higher one passes (hull use); cut only at its own.
  if (!ol || ol->aref > static_cast<int>(ref & 0xFF)) { ++g_s.noOutline; return false; }
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  IDirect3DVertexDeclaration9* decl = nullptr;
  UINT offset = 0, stride = 0;
  dev->lpVtbl->GetStreamSource(dev, 0, &vb, &offset, &stride);
  dev->lpVtbl->GetIndices(dev, &ib);
  dev->lpVtbl->GetVertexDeclaration(dev, &decl);
  Key k{};
  k.tex = hx; k.vb = vb; k.ib = ib; k.decl = decl;
  k.vbGen = g_gen[vb]; k.ibGen = g_gen[ib];
  k.offset = offset; k.stride = stride; k.minIdx = minIdx; k.numV = numV; k.startIdx = startIdx; k.prims = prims;
  k.base = base; k.aref = static_cast<int>(ref & 0xFF);
  auto it = g_meshes.find(k);
  if (it == g_meshes.end()) {
    Mesh m;
    m.failed = !vb || !ib || !decl || stride == 0 || !build(dev, k, *ol, m);
    if (m.failed) ++g_s.failed; else ++g_s.built;
    it = g_meshes.emplace(k, m).first;
  }
  if (vb) vb->lpVtbl->Release(vb);
  if (ib) ib->lpVtbl->Release(ib);
  if (decl) decl->lpVtbl->Release(decl);
  const Mesh& m = it->second;
  if (m.failed) return false;
  out->vb = m.vb; out->ib = m.ib; out->stride = m.stride; out->verts = m.verts; out->tris = m.tris;
  out->alphaTestOff = !m.needsTest && ol->aref == static_cast<int>(ref & 0xFF);
  ++(out->alphaTestOff ? g_s.cut : g_s.hull);
  return true;
}

void rbrrtx_alphacut_forget(const void* buffer) { if (buffer) ++g_gen[buffer]; }

void rbrrtx_alphacut_init() {
  wchar_t v[32];
  if (GetEnvironmentVariableW(L"RBRRTX_ALPHACUT_MAX_PER_TRI", v, 32)) g_maxPerTri = static_cast<unsigned>((std::max)(1, _wtoi(v)));
  if (GetEnvironmentVariableW(L"RBRRTX_ALPHACUT_MAX_TRIS", v, 32)) g_maxTris = static_cast<unsigned>((std::max)(1000, _wtoi(v)));
  load_dir();
  alog(L"alpha-card cut-outs: outlines from %s, at most %u pieces per card triangle, %u triangles per draw", g_dir, g_maxPerTri, g_maxTris);
}

// Every few hundred frames: free the copies of draws whose source buffer was recreated (stage change).
void rbrrtx_alphacut_frame(unsigned frame) {
  if (frame % 300) return;
  for (auto it = g_meshes.begin(); it != g_meshes.end();) {
    const Key& k = it->first;
    if (g_gen[k.vb] == k.vbGen && g_gen[k.ib] == k.ibGen) { ++it; continue; }
    if (it->second.vb) it->second.vb->lpVtbl->Release(it->second.vb);
    if (it->second.ib) it->second.ib->lpVtbl->Release(it->second.ib);
    it = g_meshes.erase(it);
  }
}

void rbrrtx_alphacut_summary(wchar_t* out, size_t n, unsigned frames) {
  UINT tris = 0;
  size_t live = 0;
  for (const auto& kv : g_meshes) if (!kv.second.failed) { ++live; tris += kv.second.tris; }
  const double f = frames ? frames : 1;
  swprintf_s(out, n, L"per frame %.0f draws cut (alpha test off), %.0f clipped with alpha test kept; since last line %u "
             L"draw copies built (%.0f ms), %u left as cards; %zu copies held (%u triangles); %u alpha-tested draws "
             L"without an outline, %u with an unknown texture",
             g_s.cut / f, g_s.hull / f, g_s.built, g_s.buildMs, g_s.failed, live, tris, g_s.noOutline, g_noHash);
  g_s = {};
  g_noHash = 0;
}
