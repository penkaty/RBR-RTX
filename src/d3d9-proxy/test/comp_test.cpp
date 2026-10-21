// comp_test (x86): checks the comp layer's matrix reconstruction outside the game.
// Needs next to the exe: d3d9.dll (our proxy) and VertexShaders\ copied from the game.
// Creates a device through the proxy (RBRRTX_COMP=1 must be set), draws once with RBR's RLDoubleTexture shader
// (world-space terrain) and once with CarPaint1 (object with world matrix in c8), each with constants built from
// known World/View/Projection matrices exactly as RBR uploads them, then reads back World/View/Projection with
// GetTransform. Exit code 0 when all three match within 1e-3. A third frame repeats the terrain draw with the
// shader's comment blocks stripped and a moved camera (shader recognition without comments). A fourth frame draws
// with a BTB-style shader (HLSL compiled at run time like rbr_rx's D3DX effects: worldviewproj / worldMatrix found by
// name in the constant table, column_major registers) and a moved camera. Needs d3dcompiler_47.dll (Windows has it).
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <d3d9.h>
#include <d3dcommon.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

static D3DMATRIX mul(const D3DMATRIX& a, const D3DMATRIX& b) {
  D3DMATRIX r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
  return r;
}
static void norm(float* v) { float l = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); v[0]/=l; v[1]/=l; v[2]/=l; }
static void cross(const float* a, const float* b, float* r) { r[0]=a[1]*b[2]-a[2]*b[1]; r[1]=a[2]*b[0]-a[0]*b[2]; r[2]=a[0]*b[1]-a[1]*b[0]; }
static float dot(const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

static D3DMATRIX look_at_lh(const float* eye, const float* at, const float* up) {   // D3DXMatrixLookAtLH
  float z[3] = { at[0]-eye[0], at[1]-eye[1], at[2]-eye[2] }; norm(z);
  float x[3]; cross(up, z, x); norm(x);
  float y[3]; cross(z, x, y);
  D3DMATRIX m = {};
  m._11 = x[0]; m._12 = y[0]; m._13 = z[0];
  m._21 = x[1]; m._22 = y[1]; m._23 = z[1];
  m._31 = x[2]; m._32 = y[2]; m._33 = z[2];
  m._41 = -dot(x, eye); m._42 = -dot(y, eye); m._43 = -dot(z, eye); m._44 = 1;
  return m;
}
static D3DMATRIX perspective_lh(float fovY, float aspect, float zn, float zf) {      // D3DXMatrixPerspectiveFovLH
  const float ys = 1.0f / std::tan(fovY / 2), xs = ys / aspect;
  D3DMATRIX m = {};
  m._11 = xs; m._22 = ys; m._33 = zf / (zf - zn); m._34 = 1; m._43 = -zn * zf / (zf - zn);
  return m;
}
// RBR uploads a D3D matrix M as shader rows: c[k][i] = M.m[i][k]
static D3DMATRIX translation(float x, float y, float z) {
  D3DMATRIX m = {}; m._11 = m._22 = m._33 = m._44 = 1; m._41 = x; m._42 = y; m._43 = z; return m;
}
static void to_rows(const D3DMATRIX& m, float out[4][4]) {
  for (int k = 0; k < 4; ++k) for (int i = 0; i < 4; ++i) out[k][i] = m.m[i][k];
}
static float max_diff(const D3DMATRIX& a, const D3DMATRIX& b) {
  float d = 0;
  for (int i = 0; i < 16; ++i) d = (std::max)(d, std::fabs((&a.m[0][0])[i] - (&b.m[0][0])[i]));
  return d;
}
static std::vector<char> read_file(const wchar_t* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main() {
  wchar_t dir[MAX_PATH];
  GetModuleFileNameW(nullptr, dir, MAX_PATH);
  *(wcsrchr(dir, L'\\') + 1) = 0;
  wchar_t path[MAX_PATH];
  swprintf_s(path, L"%sd3d9.dll", dir);
  SetEnvironmentVariableW(L"RBRRTX_BTB", L"1");     // BTB transforms are parked (off) in the game; test them anyway
  HMODULE proxy = LoadLibraryW(path);
  if (!proxy) { printf("cannot load proxy\n"); return 2; }
  auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(proxy, "Direct3DCreate9"));
  IDirect3D9* d3d = create(D3D_SDK_VERSION);
  if (!d3d) { printf("Direct3DCreate9 failed\n"); return 2; }

  WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"comp_test";
  RegisterClassW(&wc);
  HWND wnd = CreateWindowW(L"comp_test", L"comp_test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
  D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = wnd;
  IDirect3DDevice9* dev = nullptr;
  HRESULT hr = d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr)) { printf("CreateDevice failed 0x%08lX\n", hr); return 2; }

  IDirect3DVertexShader9 *rl = nullptr, *car = nullptr;
  swprintf_s(path, L"%sVertexShaders\\fog\\RLDoubleTexture.vso", dir);
  std::vector<char> rlCode = read_file(path);
  swprintf_s(path, L"%sVertexShaders\\fog\\CarPaint1.vso", dir);
  std::vector<char> carCode = read_file(path);
  if (rlCode.empty() || carCode.empty()) { printf("shader files missing\n"); return 2; }
  dev->lpVtbl->CreateVertexShader(dev, reinterpret_cast<const DWORD*>(rlCode.data()), &rl);
  dev->lpVtbl->CreateVertexShader(dev, reinterpret_cast<const DWORD*>(carCode.data()), &car);
  // Frame 2: the same terrain shader with its comment blocks stripped (as some testers' hooks hand it to the device);
  // the proxy must still recognise it, or View/Projection stay at frame 1's camera.
  std::vector<DWORD> rlStrippedCode;
  {
    const DWORD* t = reinterpret_cast<const DWORD*>(rlCode.data());
    const size_t n = rlCode.size() / 4;
    rlStrippedCode.push_back(t[0]);
    for (size_t i = 1; i < n; ++i) {
      if ((t[i] & 0xFFFF) == 0xFFFE) { i += (t[i] >> 16) & 0x7FFF; continue; }
      rlStrippedCode.push_back(t[i]);
      if (t[i] == 0x0000FFFF) break;
    }
  }
  IDirect3DVertexShader9* rlStripped = nullptr;
  dev->lpVtbl->CreateVertexShader(dev, rlStrippedCode.data(), &rlStripped);
  // Frame 3: BTB_Diffuse.fx's VShader (RX_CONTENT\COMMON\FX), registers pinned away from RBR's c0/c8.
  static const char kBtbVs[] =
    "float4x4 worldviewproj : register(c4); float4x4 viewproj; float4x4 worldMatrix : register(c12);\n"
    "struct VS_OUT { float4 pos : POSITION; float3 N : TEXCOORD0; float2 uv : TEXCOORD1; float4 vpos : TEXCOORD2; };\n"
    "VS_OUT VShader(float3 pos : POSITION, float3 N : NORMAL, float2 uv0 : TEXCOORD0) {\n"
    "  VS_OUT o = (VS_OUT)0; o.pos = mul(float4(pos, 1), worldviewproj); o.N = normalize(mul(N, (float3x4)worldMatrix));\n"
    "  o.uv = uv0; o.vpos = mul(float4(pos, 1), worldMatrix); return o; }\n";
  typedef HRESULT(WINAPI* CompileFn)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT,
                                     UINT, ID3DBlob**, ID3DBlob**);
  HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
  CompileFn compile = dc ? reinterpret_cast<CompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
  ID3DBlob *btbBlob = nullptr, *btbErr = nullptr;
  if (!compile || FAILED(compile(kBtbVs, sizeof(kBtbVs) - 1, "btb", nullptr, nullptr, "VShader", "vs_2_0", 0, 0, &btbBlob, &btbErr))) {
    printf("BTB test shader did not compile%s%s\n", btbErr ? ": " : "", btbErr ? static_cast<const char*>(btbErr->lpVtbl->GetBufferPointer(btbErr)) : "");
    return 2;
  }
  IDirect3DVertexShader9* btb = nullptr;
  dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(btbBlob->lpVtbl->GetBufferPointer(btbBlob)), &btb);
  // Frame 4: BTB_Diffuse.fx's VShaderInstanced (world per copy in TEXCOORD1-4 of stream 1), viewproj in c8.
  static const char kBtbInstVs[] =
    "float4x4 viewproj : register(c8);\n"
    "float4 VShaderInstanced(float3 pos : POSITION, float4 row1 : TEXCOORD1, float4 row2 : TEXCOORD2,\n"
    "                        float4 row3 : TEXCOORD3, float4 row4 : TEXCOORD4) : POSITION {\n"
    "  float4x4 mWorld = float4x4(row1, row2, row3, row4); return mul(mul(mWorld, float4(pos, 1)), viewproj); }\n";
  ID3DBlob* instBlob = nullptr;
  if (FAILED(compile(kBtbInstVs, sizeof(kBtbInstVs) - 1, "btbi", nullptr, nullptr, "VShaderInstanced", "vs_2_0", 0, 0, &instBlob, &btbErr))) {
    printf("BTB instanced test shader did not compile\n");
    return 2;
  }
  IDirect3DVertexShader9* btbInst = nullptr;
  dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(instBlob->lpVtbl->GetBufferPointer(instBlob)), &btbInst);

  D3DVERTEXELEMENT9 el[] = {
    { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL, 0 },
    { 0, 24, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, { 0, 28, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 36, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 1 }, D3DDECL_END() };
  IDirect3DVertexDeclaration9* decl = nullptr;
  dev->lpVtbl->CreateVertexDeclaration(dev, el, &decl);
  struct V { float p[3], n[3]; DWORD c; float t0[2], t1[2]; } tri[3] = {
    { {0,0,0}, {0,1,0}, 0xffffffff, {0,0}, {0,0} }, { {1,0,0}, {0,1,0}, 0xffffffff, {1,0}, {1,0} },
    { {0,1,0}, {0,1,0}, 0xffffffff, {0,1}, {0,1} } };

  float eye[3] = { 120, 35, -260 }; const float at[3] = { 100, 30, -200 }, up[3] = { 0, 1, 0 };
  D3DMATRIX view = look_at_lh(eye, at, up);
  const D3DMATRIX proj = perspective_lh(1.05f, 16.0f / 9.0f, 0.5f, 6000.0f);
  D3DMATRIX world = {};                       // car: rotated 30 degrees about Y, placed near the camera target
  world._11 = std::cos(0.52f); world._13 = -std::sin(0.52f); world._22 = 1; world._31 = std::sin(0.52f); world._33 = std::cos(0.52f);
  world._41 = 101; world._42 = 29.5f; world._43 = -198; world._44 = 1;
  float rows[4][4];

  int failures = 0;
  for (int frame = 0; frame < 5; ++frame) {
    if (frame == 4) {                         // BTB instanced: 2 copies, the proxy splits the draw per copy
      D3DVERTEXELEMENT9 ie[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 1, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1 },
        { 1, 16, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 2 }, { 1, 32, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 3 },
        { 1, 48, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 4 }, D3DDECL_END() };
      IDirect3DVertexDeclaration9* idecl = nullptr;
      dev->lpVtbl->CreateVertexDeclaration(dev, ie, &idecl);
      IDirect3DVertexBuffer9 *geo = nullptr, *inst = nullptr;
      IDirect3DIndexBuffer9* ib = nullptr;
      dev->lpVtbl->CreateVertexBuffer(dev, 36, 0, 0, D3DPOOL_MANAGED, &geo, nullptr);
      dev->lpVtbl->CreateVertexBuffer(dev, 128, 0, 0, D3DPOOL_MANAGED, &inst, nullptr);
      dev->lpVtbl->CreateIndexBuffer(dev, 6, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
      void* p = nullptr;
      const float pos[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
      geo->lpVtbl->Lock(geo, 0, 0, &p, 0); memcpy(p, pos, sizeof(pos)); geo->lpVtbl->Unlock(geo);
      const WORD idx[3] = { 0, 1, 2 };
      ib->lpVtbl->Lock(ib, 0, 0, &p, 0); memcpy(p, idx, sizeof(idx)); ib->lpVtbl->Unlock(ib);
      D3DMATRIX world2 = world; world2._41 += 5; world2._43 -= 3;
      float copies[2][4][4];                  // float4x4(row1..row4) * pos: row k = column k of the D3D matrix
      const D3DMATRIX toRel = translation(-eye[0], -eye[1], -eye[2]);
      for (int c = 0; c < 2; ++c) {
        const D3DMATRIX wr = mul(c ? world2 : world, toRel);
        for (int k = 0; k < 4; ++k) for (int j = 0; j < 4; ++j) copies[c][k][j] = wr.m[j][k];
      }
      inst->lpVtbl->Lock(inst, 0, 0, &p, 0); memcpy(p, copies, sizeof(copies)); inst->lpVtbl->Unlock(inst);
      dev->lpVtbl->BeginScene(dev);
      dev->lpVtbl->SetVertexDeclaration(dev, idecl);
      dev->lpVtbl->SetVertexShader(dev, btbInst);
      float t[4][4];
      const D3DMATRIX vp = mul(mul(translation(eye[0], eye[1], eye[2]), view), proj);   // relative viewproj
      for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) t[i][k] = vp.m[k][i];
      dev->lpVtbl->SetVertexShaderConstantF(dev, 8, &t[0][0], 4);
      dev->lpVtbl->SetStreamSource(dev, 0, geo, 0, 12);
      dev->lpVtbl->SetStreamSource(dev, 1, inst, 0, 64);
      dev->lpVtbl->SetStreamSourceFreq(dev, 0, D3DSTREAMSOURCE_INDEXEDDATA | 2);
      dev->lpVtbl->SetStreamSourceFreq(dev, 1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
      dev->lpVtbl->SetIndices(dev, ib);
      const HRESULT dr = dev->lpVtbl->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
      dev->lpVtbl->EndScene(dev);
      D3DMATRIX gw, gv, gp;
      UINT f0 = 0, f1 = 0, off = 99, stride = 0;
      IDirect3DVertexBuffer9* s1 = nullptr;
      dev->lpVtbl->GetTransform(dev, D3DTS_WORLD, &gw);
      dev->lpVtbl->GetTransform(dev, D3DTS_VIEW, &gv);
      dev->lpVtbl->GetTransform(dev, D3DTS_PROJECTION, &gp);
      dev->lpVtbl->GetStreamSourceFreq(dev, 0, &f0);
      dev->lpVtbl->GetStreamSourceFreq(dev, 1, &f1);
      dev->lpVtbl->GetStreamSource(dev, 1, &s1, &off, &stride);
      const bool restored = f0 == (D3DSTREAMSOURCE_INDEXEDDATA | 2) && f1 == (D3DSTREAMSOURCE_INSTANCEDATA | 1) && s1 == inst && off == 0 && stride == 64;
      // one merged draw in BTB space: Remix world = BTB space -> world = T(eye)
      const float ew = max_diff(gw, translation(eye[0], eye[1], eye[2])), ev = max_diff(gv, view), ep = max_diff(gp, proj);
      printf("frame 4 (BTB instanced, 2 relative copies merged): max error world %.6f  view %.6f  projection %.6f, streams %s, draw 0x%08lX\n",
             ew, ev, ep, restored ? "restored" : "NOT restored", dr);
      if (ew > 1e-3f || ev > 1e-3f || ep > 1e-3f || !restored || FAILED(dr)) ++failures;
      if (s1) s1->lpVtbl->Release(s1);
      dev->lpVtbl->SetStreamSourceFreq(dev, 0, 1);
      dev->lpVtbl->SetStreamSourceFreq(dev, 1, 1);
      dev->lpVtbl->SetStreamSource(dev, 1, nullptr, 0, 0);
      geo->lpVtbl->Release(geo); inst->lpVtbl->Release(inst); ib->lpVtbl->Release(ib); idecl->lpVtbl->Release(idecl);
      dev->lpVtbl->Present(dev, nullptr, nullptr, nullptr, nullptr);
      continue;
    }
    const bool stripped = frame == 2;
    if (stripped) { eye[0] += 40; eye[2] -= 30; view = look_at_lh(eye, at, up); }
    if (frame == 3) {                         // BTB: only an effect draw, D3DX uploads column_major = transposed
      eye[0] -= 70; eye[1] += 12; view = look_at_lh(eye, at, up);
      auto put = [&](UINT reg, const D3DMATRIX& m) {
        float t[4][4];
        for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) t[i][k] = m.m[k][i];
        dev->lpVtbl->SetVertexShaderConstantF(dev, reg, &t[0][0], 4);
      };
      dev->lpVtbl->BeginScene(dev);
      dev->lpVtbl->SetVertexDeclaration(dev, decl);
      dev->lpVtbl->SetVertexShader(dev, btb);
      // rbr_rx draws camera-relative: BTB space = world - eye; c50 (RBR) holds the absolute camera
      const D3DMATRIX wRel = mul(world, translation(-eye[0], -eye[1], -eye[2])), vRel = mul(translation(eye[0], eye[1], eye[2]), view);
      const float cam[4] = { eye[0], eye[1], eye[2], 1 }; dev->lpVtbl->SetVertexShaderConstantF(dev, 50, cam, 1);
      put(4, mul(mul(wRel, vRel), proj));
      put(12, wRel);
      dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
      dev->lpVtbl->EndScene(dev);
      D3DMATRIX gw, gv, gp;
      dev->lpVtbl->GetTransform(dev, D3DTS_WORLD, &gw);
      dev->lpVtbl->GetTransform(dev, D3DTS_VIEW, &gv);
      dev->lpVtbl->GetTransform(dev, D3DTS_PROJECTION, &gp);
      const float ew = max_diff(gw, world), ev = max_diff(gv, view), ep = max_diff(gp, proj);
      printf("frame 3 (BTB effect, camera-relative): max error world %.6f  view %.6f  projection %.6f\n", ew, ev, ep);
      if (ew > 1e-3f || ev > 1e-3f || ep > 1e-3f) ++failures;
      dev->lpVtbl->Present(dev, nullptr, nullptr, nullptr, nullptr);
      continue;
    }
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->SetVertexDeclaration(dev, decl);
    // c20-c23 deliberately off by a few units, like RBR's (the layer must not depend on it)
    const float eyeOff[3] = { eye[0] + 3, eye[1] - 1, eye[2] + 4 };
    to_rows(look_at_lh(eyeOff, at, up), rows); dev->lpVtbl->SetVertexShaderConstantF(dev, 20, &rows[0][0], 4);
    const float sun[4] = { 0.3f, -0.8f, 0.52f, 0 }; dev->lpVtbl->SetVertexShaderConstantF(dev, 41, sun, 1);
    const float cam[4] = { eye[0], eye[1], eye[2], 1 }; dev->lpVtbl->SetVertexShaderConstantF(dev, 50, cam, 1);
    // terrain: vertices in world space, c0-c3 = View * Projection
    dev->lpVtbl->SetVertexShader(dev, stripped ? rlStripped : rl);
    to_rows(mul(view, proj), rows); dev->lpVtbl->SetVertexShaderConstantF(dev, 0, &rows[0][0], 4);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
    // car: c0-c3 = World * View * Projection, c8-c11 = World
    if (!stripped) {
    dev->lpVtbl->SetVertexShader(dev, car);
    to_rows(mul(mul(world, view), proj), rows); dev->lpVtbl->SetVertexShaderConstantF(dev, 0, &rows[0][0], 4);
    to_rows(world, rows); dev->lpVtbl->SetVertexShaderConstantF(dev, 8, &rows[0][0], 4);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
    }
    dev->lpVtbl->EndScene(dev);

    D3DMATRIX gw, gv, gp;
    dev->lpVtbl->GetTransform(dev, D3DTS_WORLD, &gw);
    dev->lpVtbl->GetTransform(dev, D3DTS_VIEW, &gv);
    dev->lpVtbl->GetTransform(dev, D3DTS_PROJECTION, &gp);
    const float ew = stripped ? 0.0f : max_diff(gw, world), ev = max_diff(gv, view), ep = max_diff(gp, proj);
    printf("frame %d: max error world %.6f  view %.6f  projection %.6f\n", frame, ew, ev, ep);
    if (ew > 1e-3f || ev > 1e-3f || ep > 1e-3f) ++failures;
    dev->lpVtbl->Present(dev, nullptr, nullptr, nullptr, nullptr);
  }
  decl->lpVtbl->Release(decl); rl->lpVtbl->Release(rl); rlStripped->lpVtbl->Release(rlStripped); car->lpVtbl->Release(car); btb->lpVtbl->Release(btb); btbInst->lpVtbl->Release(btbInst);
  dev->lpVtbl->Release(dev); d3d->lpVtbl->Release(d3d);
  printf(failures ? "FAIL\n" : "PASS\n");
  return failures ? 1 : 0;
}
