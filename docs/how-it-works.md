# How it works

What each system in RBR-RTX is, what it does for the picture and where its settings are. For the engineering detail see the [0.4.0 technical notes](release-0.4.0.md).

## The chain

| Step | What happens |
|---|---|
| 1. The game | RBR draws a frame through DirectX 9, as it always has |
| 2. `d3d9.dll` (RBR-RTX) | Reads what the game draws, works out where everything is in 3D, and adds what the game never had: lights, particles, materials, weather |
| 3. The bridge | Carries everything from the 32-bit game to a 64-bit renderer |
| 4. Remix Plus | Path traces the scene and shows it |

RBR places everything with vertex shaders, so a renderer watching from outside sees draws without a position. RBR-RTX reads the game's shader constants and rebuilds the camera and every object's place from them. That is the base everything else stands on, and it has been there since 0.2.

## Remix Plus

[Remix Plus](https://github.com/RemixProjGroup/dxvk-remix) is the community fork of NVIDIA's RTX Remix runtime, led by Kim2091. RBR-RTX 0.3.0 ran on stock Remix 1.5.2; 0.4.0 runs on Remix Plus with a short list of our own patches.

Why it mattered: on stock Remix a 32-bit game could barely use the **Remix API**, the interface a mod uses to add lights, meshes and particles. Remix Plus's bridge forwards it. Nearly every system below needs that.

The runtime's source is [penkaty/dxvk-remix](https://github.com/penkaty/dxvk-remix).

## NUMOS: sky, sun, clouds, fog

[NUMOS](https://github.com/Kim2091/numos-sky) is Kim2091's physically based sky, built into Remix Plus. It replaces the game's painted sky dome completely.

**What it draws:** the atmosphere (why the sky is blue and the sunset red), the sun and the moon, volumetric clouds in two layers, height fog, haze, and lightning.

**What makes it different from a skybox:** NUMOS *lights the stage*. The sun's colour and strength come from how much air and cloud its light passes through. An overcast sky dims and softens the sun by itself; fog swallows distant trees and glows around lamps. Nothing is painted.

**What RBR-RTX tells it:**

| RBR-RTX sends | From |
|---|---|
| Where the sun is | The stage's own sun direction, your saved suns, and the per-stage fixes |
| The time of day | The game |
| The name of the weather | RBR's sky type and cloud type, with the stage number |

**Weather presets.** For each RBR weather there is a NUMOS preset in `rbr-rtx\config\weather\`: `rbr-clear`, `rbr-hazy`, `rbr-partcloud`, `rbr-lightcloud`, `rbr-heavycloud`, `rbr-lightfog`, `rbr-heavyfog`, `rbr-lightrain`, `rbr-heavyrain`, `rbr-lightsnow`, `rbr-heavysnow`. A preset holds the clouds, the fog and the haze. RBR-RTX asks for the most specific name first (this stage with this sky and cloud type) and falls back to the general one, so a single stage can have its own sky.

**Changing the weather look:** open **Alt+X**, go to the developer menu, **Sky**, **Weather**, change the preset and save it. For a stage whose sun is in the wrong place, use the F3 **Per stage** page or [fix the sun](fixing-the-sun.md) with the keys.

NUMOS is the only fog in the mod. Remix's own volumetric fog is off.

## The Remix API: lights, meshes, particles

RBR has no light sources. Headlights are glow sprites, street lamps are painted textures, and nothing lights anything. Path tracing needs the real thing, so RBR-RTX creates it:

| Created through the API | Used for |
|---|---|
| **Lights** | Headlights, brights, tail, brake and reversing lamps, backfire flash, street lamps, floodlights, your own lights |
| **Meshes with materials** | Lamp lenses that glow, car glass, brake discs that glow when hot, the crew |
| **Particle systems** | Every effect, below |

## Car lamps

Lamps are not tuned car by car. When a stage starts, RBR-RTX pulses the game's own lamp signal once and watches which parts of the car model respond. That tells it which parts are head lamps, tail lamps, brake lamps and the reversing lamp, and where they are. Lamp glass and painted-on lamps are told apart by their meshes.

- Lamp type follows the car's year: brightness in candela and colour in kelvin, from old tungsten to LED.
- **L** cycles off, two lamps, brights. They come on by themselves when the sun is low and in rain, snow or fog.
- Settings: `rbr-rtx\config\headlights.ini`, or the F3 **Headlights** page.

## World lamps and windows

Street lamps and lit windows are textures the stage paints. In the F3 **Lamps** page you point at one and say what it is; every copy on the stage then glows and casts light from dusk to dawn. See [The F3 menu](f3-menu.md).

## Effects: particles

The game's particles are stacks of flat sprites, which path tracing draws slowly and lights wrongly. RBR-RTX hooks the game's particle emitters and starts a Remix particle system wherever the game fires one: dust per wheel in the colour of the ground, tyre smoke, splashes, engine steam, debris.

Rain and snow are one spawner around the camera. Particles are lit by the sun, the sky and nearby lamps, so a headlight beam shows in the rain.

Settings: `rbr-rtx\config\particles.ini`, see [Particles, rain, snow and weather](particles-and-weather.md).

## Wetness

Wet-surface shading by [xoxor4d](https://github.com/xoxor4d), ported into our runtime. It darkens wet ground, fills hollows with puddles, and draws ripples and raindrop rings.

RBR-RTX drives it from the stage: the track condition (dry, damp, wet) sets how wet the ground is, and the weather sets the rain. Two things are specific to RBR:

- **Puddles stay on the road.** The game's physics knows which ground is road. RBR-RTX builds a mask from it, so verges and grass get wet without standing water.
- **Rain shadow.** A top-down map of the stage keeps ground under bridges and roofs free of raindrops.

Settings: the F3 **Wetness** page.

## The material system

Without it, every texture shares one material and gravel shines like tarmac.

- **Ground from the physics.** The game's physics knows the surface under every wheel: gravel, tarmac, snow, grass and more. RBR-RTX reads the same data for each ground texture and remembers the answer.
- **Class materials.** Each class has its own roughness and surface detail. Other textures are classed by what they are: rock, wood, metal, building, fence, sign, foliage.
- **Generated while you play.** Lit lamp parts, foliage, car paint and gauge displays get their materials written as small Remix mods in `rbr-rtx\rtx-remix\mods\rbr_rtx_*`. They build up over the stages you drive, which is why a stage can look more finished the second time.

These are per-class values, not hand-made textures for every surface. More detailed materials are planned after 0.4.0.

## Ground blending and foliage

- **Second ground layer.** Many stages blend two textures on the ground: sea and coast, cliffs, road edges. RBR-RTX draws the second one as a thin layer on top. **Alt+Y** or the F3 **Debug** page changes how.
- **Terrain baker.** Remix can blend ground textures tagged Terrain into one map. It is used only for textures tagged by hand.
- **Card normals.** Trees are flat cards. xoxor4d's card normals shade them as if they were round, on cut-out textures only.

## Light transport

| Part | Job |
|---|---|
| **RTXDI** | Chooses which lights matter for each pixel |
| **SHaRC** | A cache of bounce light, so one bounce looks like several |
| **DLSS Ray Reconstruction** | Turns the noisy result into a clean picture and upscales it |

Defaults aim at 60 fps at 1080p on an RTX 2080 Ti. To raise them see [Remix settings for strong PCs](remix-settings.md).

## Settings: who owns what

| File | Owner | Notes |
|---|---|---|
| `rtx.conf` in the RBR folder | You, through Alt+X | Created once from the shipped defaults, never rewritten by the mod |
| `user.conf` in the RBR folder | You, through Alt+X | Your graphics settings |
| `rbr-rtx\config\menu.ini` | The F3 menu | Wetness, tags, lamps |
| `rbr-rtx\config\*.ini` | You, in Notepad or F3 | Keys, particles, headlights, atmosphere |
| `rbr-rtx\config\start-options.ini` | The mod | A few options Remix resets at every start; listed there, they cannot be changed in Alt+X |

## Logging

A release install writes no developer log while you drive; it only keeps a small report per stage. Three keys collect data when you want it: **9** marks a moment in the stage report, **8** records one frame, **Alt+K** runs the stage sweep. See [Troubleshooting](troubleshooting.md).
