# Installation

## Before you start
- NVIDIA RTX GPU and driver **572.18 or newer** (the minimum RTX Remix 1.5.2 itself checks for; older drivers get a Remix error dialog at start).
- **ReShade and the RBR Lights Plugin must not be installed.** Remix does not run properly with either one. Remove
  them first: for ReShade also delete its Vulkan layer and shader folders, for the plugin remove it from the game's `Plugins\` folder.
- RSF launcher: **Graphics mode = Normal**. The Vulkan, dgVoodoo and VR modes bring their own `d3d9.dll`, which
  would replace ours.

## Install
1. Download the latest [release](https://github.com/penkaty/RBR-RTX/releases). `.7z` opens with [7-Zip](https://www.7-zip.org/); a `.zip` is the fallback.
2. Unpack it **into your RBR folder**, next to `RichardBurnsRally_SSE.exe`. This adds `d3d9.dll` and an `rbr-rtx\` folder.
   Alternatively run `rbr-rtx\install.ps1 -Game "<your RBR folder>"` from the unpacked folder in PowerShell.
3. RSF launcher / in-game options:
   - **Cubic env maps: off.** RBR-RTX path traces car reflections itself; the game's cube map pass only costs time.
   - **Render car shadows: off.** Remix traces the real shadow; the game's blob shadow is not needed.
   - **Particles: off.** Remix uses its own particle renderer.
   - Normal or windowed "Fullscreen mode" in the launcher is recommended: the stage sweep screenshots come out black in exclusive fullscreen.
4. Start the game as usual. The first start compiles shaders and takes longer. **Alt+X** opens the Remix menu.

## Updating
Quit the game and unpack the new version over the old files. Then **delete `user.conf` in the RBR folder**, and an
edited `rbr-rtx\config\keys.ini`: settings saved from the Remix menu live in `user.conf` and override the new
defaults, and keys have moved between versions. The first start afterwards recompiles shaders.

## Uninstall
Run `rbr-rtx\uninstall.cmd`. It removes `d3d9.dll` (only if it is still ours), the files Remix created (`user.conf`,
logs) and the `rbr-rtx\` folder, including `rtx-remix\mods` and `rtx-remix\captures` in it. Copy out anything you want to
keep first, for example `config\sun-saved.ini`. Files that were there before stay. No game file was ever changed.
