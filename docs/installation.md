# Installation

## Before you start
- NVIDIA RTX GPU with driver 610.47 or newer (the minimum of the RTX Remix runtime that Remix Plus is built on).
  AMD and Intel GPUs with Vulkan ray tracing (Radeon RX 6000, Intel Arc and newer) are experimental and not tested by us;
  DLSS and Ray Reconstruction are NVIDIA only, so choose FSR or XeSS in the Alt+X menu.
- **ReShade and the RBR Lights Plugin must not be installed.** Remix does not run properly with either one. Remove
  them first: for ReShade also delete its Vulkan layer and shader folders, for the plugin remove it from the game's `Plugins\` folder.
- RSF launcher: **Graphics mode = Normal**. The Vulkan, dgVoodoo and VR modes bring their own `d3d9.dll`, which
  would replace ours.

## Install
1. Download the latest [release](https://github.com/penkaty/RBR-RTX/releases). `.7z` opens with [7-Zip](https://www.7-zip.org/); the `.zip` is the same content for Windows' own unpacker.
2. Unpack it **into your RBR folder**, next to `RichardBurnsRally_SSE.exe`. This adds `d3d9.dll` and an `rbr-rtx\` folder.
3. Open the `rbr-rtx` folder in your RBR folder and double-click **`install.bat`** (or run `install.ps1` in PowerShell).
   It checks that no other `d3d9.dll` is in the way and notes which settings files were there before, so the uninstaller keeps them.
   Unpacked somewhere else instead? Run `rbr-rtx\install.ps1 -Game "<your RBR folder>"` from there: it copies the files over.
4. RSF launcher / in-game options:
   - **Particles: on** (any quality) if you want raindrops on the windscreen. Dust, smoke, rain and snow work either way.
   - **Cubic env maps** and **Render car shadows:** either setting. RBR-RTX skips both passes and path traces the real thing.
   - Windowed or borderless "Fullscreen mode" is recommended: the stage sweep screenshots come out black in exclusive fullscreen.
   - **Windscreen: Default/Enabled.**
5. Start the game as usual. The first start compiles shaders and takes longer. **F3** opens the RBR-RTX menu, **Alt+X** the Remix menu.

On the first start the mod puts two settings files into the RBR folder: `rtx.conf` and `user.conf`. They are yours from then on: what you save in Alt+X goes there, and no update overwrites them.

## Updating
1. Quit the game and unpack the new version over the old files.
2. **Delete `rtx.conf` and `user.conf` in the RBR folder.** They hold the old version's defaults and would override the new ones. If you tagged textures in Alt+X yourself, keep a copy of `rtx.conf` to look them up.
3. Delete an edited `rbr-rtx\config\keys.ini` if you had one: keys have moved between versions.

Your own F3 work is in `rbr-rtx\config\menu.ini` and your saved suns in `rbr-rtx\config\sun-saved.ini`. Unpacking a new version replaces both with the shipped ones, so copy them out first if you want to keep them.

The first start afterwards recompiles shaders.

## Uninstall
Double-click `rbr-rtx\uninstall.cmd`. It removes `d3d9.dll` (only if it is still ours), the files Remix created (`rtx.conf`, `user.conf`,
logs) and the `rbr-rtx\` folder, including `rtx-remix\mods` and `rtx-remix\captures` in it. Copy out anything you want to
keep first, for example `config\menu.ini` and `config\sun-saved.ini`. Files that were there before stay. No game file was ever changed.
