// comp_test (x86): checks the comp layer's matrix reconstruction outside the game.
// Needs next to the exe: d3d9.dll (our proxy) and VertexShaders\ copied from the game.
// Creates a device through the proxy (RBRRTX_COMP=1 must be set), draws once with RBR's RLDoubleTexture shader
// (world-space terrain) and once with CarPaint1 (object with world matrix in c8), each with constants built from
// known World/View/Projection matrices exactly as RBR uploads them, then reads back World/View/Projection with
// GetTransform. Exit code 0 when all three match within 1e-3. A third frame repeats the terrain draw with the
// shader's comment blocks stripped and a moved camera (shader recognition without comments).
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <d3d9.h>
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
  for (int frame = 0; frame < 3; ++frame) {
    const bool stripped = frame == 2;
    if (stripped) { eye[0] += 40; eye[2] -= 30; view = look_at_lh(eye, at, up); }
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
  decl->lpVtbl->Release(decl); rl->lpVtbl->Release(rl); rlStripped->lpVtbl->Release(rlStripped); car->lpVtbl->Release(car);
  dev->lpVtbl->Release(dev); d3d->lpVtbl->Release(d3d);
  printf(failures ? "FAIL\n" : "PASS\n");
  return failures ? 1 : 0;
}
