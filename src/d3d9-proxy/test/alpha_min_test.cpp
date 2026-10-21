// Test of alpha_min.hpp: prints "<file>\t<min alpha>" for each mip-0 DDS given (harvest output, remix\harvest\*.dds),
// to compare with a Pillow decode (tools\alphacut). Build (x86 cl):
//   cl /nologo /EHsc /std:c++17 test\alpha_min_test.cpp /Fe:test\alpha_min_test.exe
#include "../alpha_min.hpp"
#include <cstdio>
#include <vector>

int wmain(int argc, wchar_t** argv) {
  for (int a = 1; a < argc; ++a) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, argv[a], L"rb") || !f) { wprintf(L"%s\tcannot open\n", argv[a]); continue; }
    std::vector<unsigned char> b;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    if (b.size() < 128) { wprintf(L"%s\ttoo short\n", argv[a]); continue; }
    const DWORD* h = reinterpret_cast<const DWORD*>(b.data());
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    if (h[20] & 0x4) fmt = static_cast<D3DFORMAT>(h[21]);           // FOURCC = D3DFMT_DXTn
    else if (h[22] == 32) fmt = h[26] ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    else if (h[22] == 16) fmt = h[26] == 0x8000 ? D3DFMT_A1R5G5B5 : h[26] == 0xF000 ? D3DFMT_A4R4G4B4 : h[26] == 0xFF00 ? D3DFMT_A8L8 : D3DFMT_R5G6B5;
    else if (h[22] == 8) fmt = h[26] ? D3DFMT_A8 : D3DFMT_L8;
    wprintf(L"%s\t%d\n", argv[a], rbrrtx_min_alpha(fmt, b.data() + 128, b.size() - 128));
  }
  return 0;
}
