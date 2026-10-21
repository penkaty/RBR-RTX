// alphacut_test (x86): runs alphacut.cpp's clipper on synthetic cards with a real outline file, on the system
// d3d9 (hidden window, no proxy, no Remix), and checks the copy it builds:
//   - every output triangle lies inside the outline (its centroid is in an outline triangle), in the card's UV area;
//   - the output's UV area per card = the outline's area inside that card (exact clipping, nothing lost or doubled);
//   - positions and colours follow the card's UV -> attribute map (barycentric interpolation);
//   - a card over two texture repeats stays whole, one shifted by a whole repeat is clipped like the unshifted one.
// Build (x86 cl, from src\d3d9-proxy):
//   cl /nologo /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE test\alphacut_test.cpp alphacut.cpp /Fe:test\alphacut_test.exe /Fo:obj\ d3d9.lib user32.lib
// Run: test\alphacut_test.exe <dir with .act files> <texture hash> [aref]   (exit code 0 = all checks passed)
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../common.hpp"

static unsigned long long g_hash = 0;
void rbrrtx_log_to(const wchar_t*, const wchar_t* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vwprintf(fmt, a);
  va_end(a);
  wprintf(L"\n");
}
unsigned long long rbrrtx_texture_hash(void*) { return g_hash; }

struct V { float x, y, z; DWORD col; float u, v; };

static std::vector<float> read_act(const wchar_t* path, int& mode, int& aref) {
  std::vector<float> t;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"rb") || !f) return t;
  char m[4];
  unsigned h[3];
  if (fread(m, 1, 4, f) == 4 && fread(h, 4, 3, f) == 3) {
    mode = static_cast<int>(h[0]); aref = static_cast<int>(h[1]);
    t.resize(static_cast<size_t>(h[2]) * 6);
    if (fread(t.data(), 4, t.size(), f) != t.size()) t.clear();
  }
  fclose(f);
  return t;
}

static bool in_tri(const float* t, double u, double v) {
  auto s = [&](int a, int b) { return (t[b*2] - t[a*2]) * (v - t[a*2+1]) - (t[b*2+1] - t[a*2+1]) * (u - t[a*2]); };
  const double a = s(0, 1), b = s(1, 2), c = s(2, 0);
  return (a >= -1e-9 && b >= -1e-9 && c >= -1e-9) || (a <= 1e-9 && b <= 1e-9 && c <= 1e-9);
}

// Area of the outline inside the axis-aligned box [u0,u1]x[v0,v1] (each triangle clipped, Sutherland-Hodgman).
static double outline_area_in(const std::vector<float>& t, double u0, double v0, double u1, double v1) {
  double sum = 0;
  for (size_t i = 0; i + 6 <= t.size(); i += 6) {
    std::vector<double> p = { t[i], t[i+1], t[i+2], t[i+3], t[i+4], t[i+5] };
    auto clipax = [&](int ax, double val, bool below) {
      std::vector<double> o;
      const size_t n = p.size() / 2;
      for (size_t k = 0; k < n; ++k) {
        const double* a = &p[k*2]; const double* b = &p[((k+1)%n)*2];
        const bool ia = below ? a[ax] <= val : a[ax] >= val, ib = below ? b[ax] <= val : b[ax] >= val;
        if (ia) { o.push_back(a[0]); o.push_back(a[1]); }
        if (ia != ib) { const double s = (val - a[ax]) / (b[ax] - a[ax]); o.push_back(a[0] + s*(b[0]-a[0])); o.push_back(a[1] + s*(b[1]-a[1])); }
      }
      p.swap(o);
    };
    clipax(0, u0, false); clipax(0, u1, true); clipax(1, v0, false); clipax(1, v1, true);
    const size_t n = p.size() / 2;
    double a = 0;
    for (size_t k = 0; k < n; ++k) a += p[k*2] * p[((k+1)%n)*2+1] - p[((k+1)%n)*2] * p[k*2+1];
    sum += std::fabs(a) / 2;
  }
  return sum;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) { wprintf(L"usage: alphacut_test <act dir> <hash> [aref]\n"); return 2; }
  SetEnvironmentVariableW(L"RBRRTX_ALPHACUT_DIR", argv[1]);
  g_hash = wcstoull(argv[2], nullptr, 16);
  const DWORD aref = argc > 3 ? _wtoi(argv[3]) : 96;
  wchar_t path[MAX_PATH * 2];
  swprintf_s(path, L"%s\\%016llX.act", argv[1], g_hash);
  int mode = -1, fileRef = 0;
  const std::vector<float> ol = read_act(path, mode, fileRef);
  if (ol.empty()) { wprintf(L"cannot read %s\n", path); return 2; }

  HWND wnd = CreateWindowW(L"STATIC", L"alphacut_test", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd; pp.BackBufferFormat = D3DFMT_UNKNOWN;
  IDirect3DDevice9* dev = nullptr;
  if (!d3d || FAILED(d3d->lpVtbl->CreateDevice(d3d, 0, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
    wprintf(L"no d3d9 device\n"); return 2;
  }
  rbrrtx_alphacut_init();

  // Cards: quads with position = (u, v) mapped affinely (x = 10u + 3, y = -7v + 1, z = 2u + 5v), colour from u, v.
  struct Card { double u0, v0, u1, v1; };
  std::vector<Card> cards = { {0, 0, 1, 1}, {0.25, 0.25, 0.5, 0.5}, {0.5, 0, 1, 0.5}, {0, 0.5, 0.5, 1}, {1.25, 0.25, 1.5, 0.5},
                              {0.75, 0.75, 1.25, 1.0} };
  for (int i = 0; i < 6; ++i) for (int j = 0; j < 6; ++j) cards.push_back({ i / 6.0, j / 6.0, (i + 1) / 6.0, (j + 1) / 6.0 });
  auto attr = [](double u, double v, V& o) {
    o.x = static_cast<float>(10 * u + 3); o.y = static_cast<float>(-7 * v + 1); o.z = static_cast<float>(2 * u + 5 * v);
    const int r = static_cast<int>(std::lround(100 * u)), g = static_cast<int>(std::lround(100 * v));   // affine in u, v (u, v < 2.5)
    o.col = 0xFF000000u | (r << 16) | (g << 8) | 0x40;
    o.u = static_cast<float>(u); o.v = static_cast<float>(v);
  };
  std::vector<V> verts;
  std::vector<uint16_t> idx;
  for (const Card& c : cards) {
    const uint16_t b = static_cast<uint16_t>(verts.size());
    V q[4];
    attr(c.u0, c.v0, q[0]); attr(c.u1, c.v0, q[1]); attr(c.u1, c.v1, q[2]); attr(c.u0, c.v1, q[3]);
    for (V& x : q) x.z += 100.0f * static_cast<float>(&c - cards.data());
    verts.insert(verts.end(), q, q + 4);
    const uint16_t t[6] = { b, uint16_t(b + 1), uint16_t(b + 2), b, uint16_t(b + 2), uint16_t(b + 3) };
    idx.insert(idx.end(), t, t + 6);
  }
  // Leading padding vertices so base vertex, minIdx and offsets are exercised.
  const UINT pad = 5, offset = 2 * sizeof(V);
  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  dev->lpVtbl->CreateVertexBuffer(dev, static_cast<UINT>((verts.size() + pad + 2) * sizeof(V)), 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
  dev->lpVtbl->CreateIndexBuffer(dev, static_cast<UINT>((idx.size() + 9) * 2), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
  void* p = nullptr;
  vb->lpVtbl->Lock(vb, 0, 0, &p, 0);
  std::memset(p, 0x7F, (verts.size() + pad + 2) * sizeof(V));
  std::memcpy(static_cast<char*>(p) + offset + pad * sizeof(V), verts.data(), verts.size() * sizeof(V));
  vb->lpVtbl->Unlock(vb);
  ib->lpVtbl->Lock(ib, 0, 0, &p, 0);
  std::memset(p, 0, (idx.size() + 9) * 2);
  for (size_t i = 0; i < idx.size(); ++i) static_cast<uint16_t*>(p)[9 + i] = static_cast<uint16_t>(idx[i] + 3);   // minIdx 3
  ib->lpVtbl->Unlock(ib);
  const D3DVERTEXELEMENT9 el[] = { {0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0}, {0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0},
                                   {0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END() };
  IDirect3DVertexDeclaration9* decl = nullptr;
  dev->lpVtbl->CreateVertexDeclaration(dev, el, &decl);
  IDirect3DTexture9* tex = nullptr;
  dev->lpVtbl->CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
  dev->lpVtbl->SetVertexDeclaration(dev, decl);
  dev->lpVtbl->SetStreamSource(dev, 0, vb, offset, sizeof(V));
  dev->lpVtbl->SetIndices(dev, ib);
  dev->lpVtbl->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(tex));
  dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, TRUE);
  dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
  dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAREF, aref);

  RbrAlphaCut ac = {};
  const INT base = static_cast<INT>(pad) - 3;
  if (!rbrrtx_alphacut_find(dev, base, 3, static_cast<UINT>(verts.size()), 9, static_cast<UINT>(idx.size() / 3), &ac)) {
    wprintf(L"FAIL: no cut copy\n"); return 1;
  }
  wprintf(L"copy: %u triangles, %u vertices, alpha test %s (file mode %d, aref %d)\n", ac.tris, ac.verts, ac.alphaTestOff ? L"off" : L"kept", mode, fileRef);
  RbrAlphaCut again = {};
  if (!rbrrtx_alphacut_find(dev, base, 3, static_cast<UINT>(verts.size()), 9, static_cast<UINT>(idx.size() / 3), &again) || again.vb != ac.vb) {
    wprintf(L"FAIL: second lookup did not hit the cache\n"); return 1;
  }
  // Read the copy back (the test's own MANAGED buffers are readable).
  std::vector<V> ov(ac.verts);
  std::vector<uint32_t> oi(static_cast<size_t>(ac.tris) * 3);
  IDirect3DVertexBuffer9* cvb = static_cast<IDirect3DVertexBuffer9*>(ac.vb);
  IDirect3DIndexBuffer9* cib = static_cast<IDirect3DIndexBuffer9*>(ac.ib);
  D3DINDEXBUFFER_DESC id = {};
  cib->lpVtbl->GetDesc(cib, &id);
  cvb->lpVtbl->Lock(cvb, 0, 0, &p, D3DLOCK_READONLY);
  std::memcpy(ov.data(), p, ov.size() * sizeof(V));
  cvb->lpVtbl->Unlock(cvb);
  cib->lpVtbl->Lock(cib, 0, 0, &p, D3DLOCK_READONLY);
  for (size_t i = 0; i < oi.size(); ++i) oi[i] = id.Format == D3DFMT_INDEX32 ? static_cast<uint32_t*>(p)[i] : static_cast<uint16_t*>(p)[i];
  cib->lpVtbl->Unlock(cib);

  // Every card's z carries 100 * its index, so each output vertex names its card.
  int fails = 0;
  double maxAttr = 0;
  std::vector<double> got(cards.size(), 0.0);
  std::vector<int> whole(cards.size(), 0);               // card triangles kept as they were (repeat or piece cap)
  for (size_t t = 0; t < ac.tris; ++t) {
    const V* q[3] = { &ov[oi[t*3]], &ov[oi[t*3+1]], &ov[oi[t*3+2]] };
    int card = -1;
    for (const V* x : q) {
      V ref;
      attr(x->u, x->v, ref);
      const int k = static_cast<int>(std::lround((x->z - ref.z) / 100.0));
      if (card < 0) card = k;
      if (k != card || k < 0 || k >= static_cast<int>(cards.size())) { if (fails++ < 10) wprintf(L"FAIL: triangle %zu mixes cards\n", t); card = -1; break; }
      maxAttr = (std::max)({ maxAttr, std::fabs(static_cast<double>(ref.x) - x->x), std::fabs(static_cast<double>(ref.y) - x->y),
                             std::fabs(ref.z + 100.0 * k - x->z) });
      const int dr = std::abs(static_cast<int>((ref.col >> 16) & 255) - static_cast<int>((x->col >> 16) & 255));
      if (dr > 1) { if (fails++ < 10) wprintf(L"FAIL: colour %08lX vs %08lX at (%g, %g)\n", x->col, ref.col, x->u, x->v); }
    }
    if (card < 0) continue;
    const Card& cd = cards[card];
    const double area = ((q[1]->u - q[0]->u) * (q[2]->v - q[0]->v) - (q[1]->v - q[0]->v) * (q[2]->u - q[0]->u)) / 2;
    if (area < -1e-9 && fails++ < 10) wprintf(L"FAIL: triangle %zu flipped (UV area %g)\n", t, area);
    got[card] += std::fabs(area);
    auto corner = [&](const V* x) { return (std::fabs(x->u - cd.u0) < 1e-6 || std::fabs(x->u - cd.u1) < 1e-6) &&
                                           (std::fabs(x->v - cd.v0) < 1e-6 || std::fabs(x->v - cd.v1) < 1e-6); };
    if (corner(q[0]) && corner(q[1]) && corner(q[2]) && std::fabs(area - (cd.u1 - cd.u0) * (cd.v1 - cd.v0) / 2) < 1e-7) {
      ++whole[card];
      continue;
    }
    const double cu = (q[0]->u + q[1]->u + q[2]->u) / 3, cv = (q[0]->v + q[1]->v + q[2]->v) / 3;
    if (cu < cd.u0 - 1e-5 || cu > cd.u1 + 1e-5 || cv < cd.v0 - 1e-5 || cv > cd.v1 + 1e-5) { if (fails++ < 10) wprintf(L"FAIL: triangle %zu outside its card\n", t); }
    const double fu = std::floor(cd.u0), fv = std::floor(cd.v0);
    bool inside = false;
    for (size_t i = 0; i + 6 <= ol.size() && !inside; i += 6) inside = in_tri(&ol[i], cu - fu, cv - fv);
    if (!inside && fails++ < 10) wprintf(L"FAIL: triangle %zu centroid (%g, %g) outside the outline\n", t, cu, cv);
  }
  double worst = 0;
  int wholeCards = 0;
  for (size_t k = 0; k < cards.size(); ++k) {
    const Card& cd = cards[k];
    const bool repeat = std::floor(cd.u1 - 1e-9) != std::floor(cd.u0);
    if (repeat && whole[k] != 2 && fails++ < 40) wprintf(L"FAIL: card %zu over two repeats not kept whole\n", k);
    if (whole[k]) { ++wholeCards; continue; }           // area of a partly whole card: not checked
    const double fu = std::floor(cd.u0), fv = std::floor(cd.v0);
    const double want = outline_area_in(ol, cd.u0 - fu, cd.v0 - fv, cd.u1 - fu, cd.v1 - fv);
    const double err = std::fabs(got[k] - want) / (std::max)(want, 1e-6);
    if (want > 1e-6) worst = (std::max)(worst, err);
    if (err > 1e-3 && want > 1e-6 && fails++ < 40) wprintf(L"FAIL: card %zu UV area %.6f, outline inside %.6f\n", k, got[k], want);
  }
  wprintf(L"checks: %zu cards (%d with triangles kept whole), worst area error %.2e, worst position error %.2e, %d failures\n", cards.size(), wholeCards, worst, maxAttr, fails);
  if (maxAttr > 1e-3) { wprintf(L"FAIL: attribute interpolation\n"); ++fails; }
  return fails ? 1 : 0;
}
