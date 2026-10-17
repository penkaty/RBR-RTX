# RBR-RTX

RTX Remix path tracing for **Richard Burns Rally** (RallySimFans), as an **add-on on top of an unchanged game
folder**. It adds a `d3d9.dll` and an `rbr-rtx\` folder, never replaces game files, works with the RSF plugin and
online racing, and uninstalls back to the original game.

Early test release. It runs well on many stages, but expect rough edges. Your reports are what makes it better
(see **Reporting a problem** below).

## What you get

- The game's scene path traced by NVIDIA RTX Remix: sun shadows, reflections, indirect light and sky light.
- RBR's own sun direction, sky and time of day drive the Remix sun and sky.
- Weather and region: RBR's sky type (crisp, hazy, rain, snow, fog), cloud cover, time of day and the stage's
  country set the Remix haze, sky light and sun strength.
- Performance work for RBR's scenery: static scenery and far trees are handed to Remix in a cheaper form, and
  far geometry outside the view is left out.
- Built-in tools for testers: a one-key stage sweep (Alt+K) that collects everything needed for a bug report, and
  a ray tracing on/off key (K) to compare against the original image.

## Not in yet: dynamic lights

There are no dynamic lights yet: headlights, brake lights and night driving still look like the original game.
RBR doesn't hand light sources to the renderer. It draws headlights as textures and lighting is baked into the
scenery, so there is nothing Remix can pick up as a "light". Each light has to be created separately through the
Remix API and tied to the car, which is the next big step. This release first got the base right: camera and
objects in the right place for path tracing, the sun matched to the game's sun, the sky, and weather/region
atmosphere.

## Requirements

- NVIDIA RTX graphics card with a recent driver.
- RBR with RallySimFans (RSF), **Graphics mode = Normal** in the RSF launcher (Vulkan/dgVoodoo/VR modes use their
  own `d3d9.dll` and won't work with RBR-RTX).
- Windowed or borderless mode is recommended (needed for the sweep's screenshots).

## Install

1. Unpack the release archive **into your RBR folder** (the one with `RichardBurnsRally_SSE.exe`). You should
   now see `d3d9.dll` and an `rbr-rtx` folder there. `.7z` archives open with [7-Zip](https://www.7-zip.org/).
2. Optional (makes uninstall cleaner): in PowerShell in the RBR folder run `.\rbr-rtx\install.ps1`. If Windows
   blocks the script because it came from the internet, skip this step.
3. Recommended RSF launcher settings: **Cubic env maps off**, **Render car shadows off** (RBR-RTX path traces
   reflections and shadows itself), **Particles high** (Low shows no dust under RBR-RTX).
4. Start the game through the RSF launcher as usual. The first start takes longer while shaders compile.
   **Alt+X** opens the RTX Remix menu (e.g. DLSS mode on very wide screens).

Had an older test build? Delete `user.conf` in the RBR folder first: it holds old Alt+X settings that would
override the new defaults.

## Uninstall

Double-click `rbr-rtx\uninstall.cmd`. It removes `d3d9.dll` (only if it's still the RBR-RTX one) and the
`rbr-rtx` folder, including saved Remix captures (copy out anything you want to keep). Your game is back to how
it was.

## Keys

You don't need any of them to drive: the defaults are the best-looking, fastest setup. Change them in
`rbr-rtx\config\keys.ini` if they clash with your controls. The full list is in `rbr-rtx\README.txt`.

| key | what |
|---|---|
| **Alt+K** | **stage sweep** for bug reports (see below) |
| **K** | ray tracing on/off: compare with the original image |
| O / L / N | RBR-RTX layer / Remix sun / sky on/off (find out what causes a problem) |
| J / U / I | foliage mode / dust amount / windscreen |
| Alt+X | RTX Remix menu |

## Reporting a problem

**Whatever the problem, run the Alt+K sweep first.** It's the main source of information for fixing things.

1. Go to the stage and the spot where the problem shows. Stop the car and leave the camera where you see it.
2. Press **Alt+K** and don't touch anything for about 25 seconds. The picture goes through strange views (parts
   disappearing, false colours): that's normal. At the end the sweep presses **Pause** itself for a Remix scene
   capture (don't bind Pause to anything else), and the game may stall for a few seconds.
3. A **beep** means it's done and you can drive on.
4. Quit the game and zip these:
   - `rbr-rtx\logs\` (the newest `sweep-<date-time>` folder is inside, plus the logs)
   - the newest `capture_*.usd` from `rbr-rtx\rtx-remix\captures\` (optional, it can be large)
5. Send it with a short description (stage, car, weather, what looks wrong, roughly the FPS):
   - [GitHub issues](https://github.com/penkaty/RBR-RTX/issues), or
   - the RBR-RTX Discord (link coming soon).

The logs contain your Windows user name in file paths and your GPU/driver model, nothing else personal. Nothing is
sent anywhere automatically.

## Known issues

- **Unusual weather and rarely driven stages** (snow, heavy fog, rain, night, custom stages) may show wrong-looking
  surfaces: glowing, see-through, too shiny or missing parts. Remix recognises game textures by a hash, and many
  of these haven't been sorted yet (sky, decals, particles, glass, ...). A sweep from exactly that stage and
  weather gives us the hashes to fix it, so logs help a lot here.
- No dynamic lights yet (see above).
- Trees, dust and smoke cost a lot of FPS.
- DirectorsCut text in the interior view can be blank.
- With Cubic env maps on, the 3rd-person car looks wrong.
- Some modern RSF cars use shaders RBR-RTX doesn't know yet: parts may look wrong. Please report them with a sweep.

## For developers

| path | what |
|---|---|
| `src/d3d9-proxy/` | x86 `d3d9.dll` proxy + comp layer (camera/World from RBR's shader constants, Remix API sun, RBR sky drawn as Remix sky, stage sweep, weather atmosphere) |
| `package/` | game-folder mirror: the tracked parts of the install (`rbr-rtx/config/`, `rbr-rtx/remix/.trex/bridge.conf`, install/uninstall scripts, README.txt) |
| `build.ps1` | builds the proxy and assembles `dist/RBR-RTX/` = package + proxy + NVIDIA's Remix runtime |

```powershell
.\build.ps1 -Remix <extracted RTX Remix 1.5.2 release>
.\dist\RBR-RTX\rbr-rtx\install.ps1 -Game <RBR folder>
<RBR folder>\rbr-rtx\uninstall.cmd
```

Needs Visual Studio 2022 with the C++ x86 tools. The installed proxy finds Remix in `<game>\rbr-rtx\remix\` and
points DXVK's config/log/cache paths into `rbr-rtx\`; env `RBRRTX_*` variables override for development.

RTX Remix is NVIDIA's (licence in `rbr-rtx\remix\LICENSE.txt`). Richard Burns Rally and RallySimFans belong to
their owners; RBR-RTX contains none of their files.
