# RBR-RTX

RTX Remix path tracing for Richard Burns Rally (RallySimFans), built as an **add-on on top of an unchanged
game folder**: it adds `d3d9.dll` and an `rbr-rtx\` folder, never replaces game files, and uninstalls back to
the original setup. Development, reverse-engineering notes and playtests live in the separate RBR-RTX-dev repo.

## Layout

| path | what |
|---|---|
| `src/d3d9-proxy/` | x86 `d3d9.dll` proxy + comp layer (camera/World from RBR's shader constants, Remix API sun, RBR sky drawn as Remix sky) |
| `package/` | game-folder mirror: the tracked parts of the install (`rbr-rtx/config/`, `rbr-rtx/remix/.trex/bridge.conf`, install/uninstall scripts, README.txt) |
| `build.ps1` | builds the proxy and assembles `dist/RBR-RTX/` = package + proxy + NVIDIA's Remix runtime |

## Build and install

```powershell
.\build.ps1 -Remix <extracted RTX Remix 1.5.2 release>
.\dist\RBR-RTX\rbr-rtx\install.ps1 -Game <RBR folder>
<RBR folder>\rbr-rtx\uninstall.cmd
```

Needs Visual Studio 2022 with the C++ x86 tools. The installed proxy finds Remix in `<game>\rbr-rtx\remix\` and
points DXVK's config/log/cache paths into `rbr-rtx\`; env `RBRRTX_*` variables override for development.
