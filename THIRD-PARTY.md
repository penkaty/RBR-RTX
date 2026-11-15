# Third-party parts

The [LICENSE](LICENSE) covers RBR-RTX's own files only. These parts keep their own licences and are not covered by it:

| Part | Where | Licence |
|---|---|---|
| NVIDIA RTX Remix runtime and bridge, Remix Plus (RemixProjGroup, Kim2091), and the libraries they ship, including NVIDIA DLSS and other RTX SDK components | `rbr-rtx\remix` in a release | Their own: `LICENSE.txt` and `ThirdPartyLicenses.txt` in that folder. Zlib and MIT for the runtime source, the NVIDIA RTX SDK licence for the SDK components |
| NUMOS sky (Kim2091), built into the runtime | `rbr-rtx\remix` in a release; the weather presets in `rbr-rtx\config\weather` are settings for it | Apache-2.0, [Kim2091/numos-sky](https://github.com/Kim2091/numos-sky) |
| Wetness and CardNormals (xoxor4d), ported into the runtime | `rbr-rtx\remix` in a release | As the runtime source: [penkaty/dxvk-remix](https://github.com/penkaty/dxvk-remix) |
| Remix API header | `src/d3d9-proxy/third_party/remix_c.h`, `third_party/remixplus/` | NVIDIA, MIT |
| xxHash (Yann Collet) | `src/d3d9-proxy/third_party/xxhash.h` | BSD 2-Clause |
| Dear ImGui (Omar Cornut), in versions with the F3 menu | `src/d3d9-proxy/third_party/imgui/` | MIT |

Richard Burns Rally belongs to its owners. RBR-RTX contains none of its files. RBR-RTX is not affiliated with or endorsed by NVIDIA.
