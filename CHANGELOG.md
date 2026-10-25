# Changelog

Each entry says what changed and why it was done that way. Newest first. Measurements are frame times on an
RTX 2080 Ti at 1080p with RTX Remix 1.5.2, same view, repeated runs agreeing within 0.1 ms.

## 0.3.0 (2026-10-25)

### Particles, rain, snow and weather on the Remix API
The game's particles are stacked flat sprites that path tracing renders slowly and wrongly lit. In 0.3 the game makes
none; RBR-RTX creates real Remix particles instead. Details and settings: [Particles, rain, snow and weather](docs/particles-and-weather.md).
- **Dust and tyre smoke** (key `7`): dust per wheel coloured by the ground under it, tyre smoke on tarmac after a slide.
- **Rain and snow** (`Alt+7` compares with the game's own): light and heavy presets, falling around the camera, ahead
  of it as fast as you drive (at speed the rain used to fall behind the car); snow keeps a car-sized gap so no flake
  starts inside the car, and follows TV cameras far from the car. Snow kicked up by the car in outside views.
- **Windscreen drops and melting snow** in the cockpit: drops run down when slow and are pushed to the edges above
  40 km/h; snow lands and melts into drops. Why it took a while: the drops have to sit exactly on the glass you see,
  and every live measurement (the car's physics position, the drawn interior, the cockpit glass draw) was off by
  centimetres to half a metre on some cars. The working answer reads the game's own cockpit windscreen plane and the
  driving camera from each car's files, with your field of view, and was checked on all 109 car models.
- **Bad weather looks closed in again:** rain, snow and fog take their haze from each stage's own fog distances (they
  looked almost clear before). Stage weather descriptions that name the weather or time ("Rainy night") are used.
  `atmosphere.ini` is read again when saved, for tuning while driving.
- **Fixed:** a stutter every 7-9 s (the log file was opened and closed per line); snow vanishing after editing
  `particles.ini` while driving.


### Car lights
RBR has no light sources: headlights, tail lights and brake lights are glow sprites painted on the car. Path tracing
needs real lights, so the mod now creates them through the Remix API and attaches them to the car.

- **Headlights (key `L`):** off, two lamps, brights (four lamps, two extra tight high beams). They switch on by
  themselves when the sun is below 12 degrees and off again above it; pressing `L` on a stage overrides this until the
  next stage. Night weathers hold the sun between -5 and +5 degrees, so the lights come on there too.
  Settings: `rbr-rtx\config\headlights.ini`.
- **Tail lights, brake lights and backfire flash:** tail lights come on with the headlights, brake lights light up
  whenever you brake, a backfire flashes an orange light at the exhaust and the flame glows. In reverse gear the right rear lamp turns white (one lamp, as on real rally cars).
- **The game's own lamp flares** switch with `L`, and lit lamp textures glow, so the original skins keep their look.

Why it is built this way:
- *Lamp positions come from RBR's own car object*, plus a per-car offset measured at stage load. Earlier
  attempts that placed lamps from the car's draw calls failed on rotated bodies and wrong 3rd-person transforms. The
  car object is smooth in every camera, and held on every car tried.
- *Tail and brake lights share one lamp per side* (two lights, not four), because noise in Remix's light sampling
  grows with the number of lights. The brake light position exists on all 109 car models; the tail one is broken on some.
- *Head lamps sit a little ahead of the car* (0.50 m in outside views, 0.40 m in the cockpit, both tuned in game).
  The car's own body blocked the light and the beams dropped out; moving the lamp ahead stopped it.

### Picture
- **Key `P`: secondary ray bounces 2 (default) or 1.** 1 is faster; on some heavily forested stages it leaves a black
  background behind the first two rows of trees, so 2 stays the default.
- **Remix fallback light is off** by default (it was a faint night light that fought the new headlights).

### Documentation
- New main page with requirements and the sun fix up front, new `docs/` pages (installation, keybinds, particles and
  weather, fixing the sun, troubleshooting, known issues, settings for strong PCs, contributing) and this changelog.
- Community files: code of conduct, contributing guide, security policy, issue and pull request templates.

## 0.2.5 (2026-10-22)

### Faster: up to about 24 % more FPS on forest stages
| Stage / view | 0.2 | 0.2.5 |
|---|---:|---:|
| Legazpi-Gabiria 2004, start line | 19.8 ms (50 fps) | 16.0 ms (62 fps) |
| Chirdonhead | 13.96 ms (72 fps) | 13.0 ms (77 fps) |

- **Opacity micromaps off.** They are meant to speed up see-through foliage but gave no gain on RBR's alpha-tested
  foliage (Legazpi was 1 ms slower with them).
- **No foliage sway by default.** Swaying trees are captured by Remix again every frame (about 1 million triangles
  on Legazpi). Static foliage costs 0.2 to 2.3 ms less, and the smudged trails behind swaying trees are gone.
- **V-Sync off by default**, and a slower free camera in the Remix menu (20 m/s instead of 200).
- **Retuned `rtx.conf`:** 27 values (exposure, material defaults, sky, bloom, tone mapping) promoted from settings
  tested in game.
- *Tried and not shipped:* cutting foliage cards to the leaf outline. It looked right but was slower on swaying
  foliage (+2.2 ms on Legazpi), so it stays an experimental key (`Alt+O`).

### Sun and weather
- **The sun follows each stage's own sun direction 1:1** (before it was capped at 60 degrees). Why: the stage's baked
  shadows are painted for that direction, and a capped sun put the path-traced shadows elsewhere. Only a missing
  or overhead sun gets a fallback, which follows the time of day and the country.
- **Fix the sun yourself:** `/` debug sun, `[ ] ; '` to move it, `\` to keep it for that stage and time of day
  (`rbr-rtx\config\sun-saved.ini`). Why: 169 of the 224 stages with several times of day use a different sun per time,
  and some do not match their own bake. Nothing in the files reveals which, so players have to show us.
- **Stage suns fixed from player input:** Carvalho de Rei 2008 (reverse, morning), Santa Lucia (morning, noon),
  Olocau Gatova (morning), Gestel I (morning), La Atalaya II (noon), Whaanga Coast Sprint (morning). Rosciszow-Walim
  and Walim-Rosciszow got a weaker sun (x0.23): they are baked with deliberately soft shadows and a full sharp sun
  was harsher than the bake.
- **Main-menu sun** turned to azimuth 28.0, only while no stage is loaded.

### Fixed
- **DirectorsCut menu text** and plugin HUDs (PaceNote and others) in replays: they were blank under ray tracing.
  Cause was our own proxy (stale vertex format, state blocks, draw order of the UI), not Remix.
- **Start clock** counts down again (it froze) and is lit as a screen, so Ray Reconstruction smears the fast digits less.
- **Car dirt** shows with ray tracing on. Dirt is pixel-shader only, so it is redrawn as a Remix decal on the paint.
- **Shader recognition** also works on builds without comment blocks and logs a warning when shaders are unknown.

### Easier to report problems
- **Stage reports:** every stage you drive is zipped locally (frame rate and stutter per second, stage, car, PC,
  settings). Key `9` marks a problem and keeps that zip. Nothing is sent anywhere.
- **Keys moved to the number row** because several clashed with the game's own keys:

| Function | 0.2 | 0.2.5 |
|---|---|---|
| Ray tracing on/off | K | **0** |
| Frame dump | P | **8** |
| Sky | N | **6** |
| Distance culling | M | **U** |
| Faster scenery mode | Alt+J | **K** |
| Sun brighter / dimmer | Alt+H / Alt+N | **Alt+N / Alt+M** |
| Performance test | Alt+T | **Alt+P** |

### Experimental
- **Ground blending (`Alt+Y`, off):** stages that mix two ground textures (sea and coast, cliffs, road markings)
  show only the first under path tracing. `Alt+Y` draws the second too (Whaanga Coast Sprint's sea is grass without
  it). It is off because road markings can turn into black mirror-like holes and it holds up to about 1 GB of memory.

### Known issues
- Bob's Track Builder stages (e.g. Ruuhimaki) do not work with path tracing; press `0` there. Three attempts gave
  the same broken picture.
- Fog weather is too weak; the first load of a new stage can very rarely hang.

## 0.2 (2026-10-17)

First version for outside testers.

- **Camera and objects for Remix.** RBR draws everything with vertex shaders, so Remix sees draws without a place in
  the world. The proxy reads the game's shader constants, rebuilds camera, world matrices and the sun, and hands them
  to Remix. The Remix runtime is started by the proxy's `d3d9.dll`, so no game file is replaced.
- **RBR's sky meshes are dropped and Remix draws the sky.** The sky dome and clouds surround the stage, and once ray
  traced they blocked all light (proved by switching them on and off: lit stage versus dark).
- **Ray Reconstruction on, Neural Radiance Cache for indirect light.** Gabiria interior: 40.7 fps with the cache against
  36.9 with ReSTIR GI, and less smearing on grass. Tuned for 60 fps at 1080p on an RTX 2080 Ti (23 fps before tuning).
  Everything can be raised in the Remix menu, see [settings for strong PCs](docs/remix-settings.md).
- **Faster scenery.** Static terrain, rocks and buildings are drawn fixed-function, because Remix re-runs shader
  draws every frame and so never keeps them in its cached acceleration structure. Same textures and pixel shaders, so
  texture hashes and the PBR mod still match.
- **Stage atmosphere.** RBR's sky type, clouds, time of day and the stage's country set haze, sky light and sun strength
  (`rbr-rtx\config\atmosphere.ini`).
- **Car shadow blob hidden.** RBR's baked car shadow acted like a dark mirror under the car; Remix traces the real shadow.
- **Debug sun and Alt+K stage sweep** for judging light and for bug reports.
- **Hotkeys on plain keys** in `keys.ini`, because compact keyboards lack Scroll Lock and Pause.
- **Default settings seeded** into `user.conf` on first start, so the Remix menu does not show settings as unsaved.
- **Optional PBR materials** for the original Warthog stages live in the separate RBR-RTX-base-mod.
