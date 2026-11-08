# Changelog

Each entry says what changed and why it was done that way. Newest first. Measurements are frame times on an
RTX 2080 Ti at 1080p with RTX Remix 1.5.2, same view, repeated runs agreeing within 0.1 ms.

## 0.4.0 (being prepared, not released)

The big change: RBR-RTX now runs on **Remix Plus** instead of RTX Remix 1.5.2. The reasoning and findings are in the [technical notes](docs/release-0.4.0.md). Items marked *not yet seen* were built but not confirmed in game.

### What you will see
- A new sky: real clouds, fog and haze that follow each stage's weather and time of day.
- Wet roads with puddles, ripples and raindrop rings on damp and wet stages.
- Rain and snow as real particles, lit by the sun, the sky and your headlights.
- Dust that takes the colour of the ground, and tarmac smoke under a slide.
- Street lamps at night, which you can place and tune yourself in the **F3** menu: temperature, brightness, cone and position.
- Headlights by car year, brake and reversing lights, hot brake discs and backfire flashes.
- Trees and distant ground no longer blink out, and the picture is sharper, with clearer road textures.
- Key **0** now compares against the original picture with the game's own rain and snow.

### Licence
From here on RBR-RTX is under the PolyForm Noncommercial License 1.0.0: free to use, change and share, never to be sold. Versions up to 0.3.0 stay under MIT.

### Weather and sky
- **NUMOS clouds, fog and haze follow the game's weather,** with presets per sky type. A stage can have its own preset; Legazpi-Gabiria does, with more cloud. Hazy skies carry more cloud too.
- **Main menu weather** changes slowly between random skies, with a 10 % chance of fog or rain. *Not yet seen.*
- **Heavy rain has lightning, and rain has low cloud banks and thicker fog.** *Not yet seen.*

### Roads
- **Wet roads and puddles** from the track condition and the rain. Saved F3 wetness values now apply from the start, and holds on the stage-driven values end with the stage. *Not yet seen.*
- **Raindrops can be dense and small at once.** New Raindrop Density slider, on by default at 16 times the old density. *Not yet seen.*
- **Automatic Terrain tags are off.** This removes the grey ground and moving textures. Textures you tag by hand keep the flag.
- **The second ground layer shades as ground:** smooth normals and wetness on every twin.

### Particles
- **Dust** has larger puffs and is less grey on grey roads, and the emission rate is lower than before. **Snow surfaces** throw their own, smaller snow dust.
- **Falling snow draws at all now.** The runtime dropped particles under 10 cm. Flakes are larger and packed closer to the camera. *Not yet seen.*
- **Tyre smoke only on dry tarmac,** while moving and not in rain.
- **Particles fade before they fill the screen,** the worst case for frame rate.
- **Sky light reaches particles,** so dust and smoke are no longer dark under an overcast sky.
- **Headlights and lamps light the rain.** Particles used to take light only from a coarse cache, so a beam never showed in the drops at night. *Not yet seen.*
- **Particle light follows changes faster:** that cache now averages 16 frames instead of 128.

### Lights
- **Brights reach further:** 1.5 times the candela, which is about 1.2 times the distance. *Not yet seen.*
- **Headlights by car year** in candela and kelvin, on the car as drawn. Lit lenses, hot brake discs, street lamps from textures or marked in game.
- **Brake lens left behind the car:** still open. The trial fix was undone; it hid rain and snow.
- **Emissive props can cast light.** In the F3 Emissive page, "also cast light" puts a lamp on every mesh with that texture: a cone in a direction you choose, or aimed at a point (stadium floodlights, street lamps). lights.ini kinds take the same cone keys. *Not yet seen.*

### RBRHUD
- **F4 windscreen opacity and banner threshold work under Remix.** RBRHUD's shader does this in the game, and Remix never runs it; RBR-RTX now applies the same rule to the windscreen texture. *Not yet seen.*
- **Gauge displays glow again,** dimmer than the first try, and the needle layers of analogue gauges are left out (they turned black). *Not yet seen.*

### Lighting response
- **The indirect light cache reacts faster** to a light switching or moving: with the Medium SHaRC preset it refreshed one path per 12x12 pixels; now 8x8, with the visible surface added. *Not yet seen.*

### Faster
- `rtx.conf` is read once, not twice per frame (9 of 10.6 ms in the menu).
- Car draws skip vertex capture (menu 16.57 to 13.83 ms).
- A state block created every frame (by a plugin) is reused instead of re-created through the bridge: 1.3 to 12 ms a frame down to 0.06 ms.
- Comparison runs against earlier versions are still to come.

### Stability
- **Alt+K sweep:** the opacity micromap steps, which came right before both sweep crashes, are gone. *Not yet seen.*
- Saves swap in whole, so a crash cannot empty `menu.ini`.

### F3 in-game menu
- **A menu inside the game window** (key `F3`, `menu` in `keys.ini`; Dear ImGui drawn by the add-on, Alt+X Remix is unchanged).
  **Wetness:** every `rtx.wet.*` slider (they left Remix's own Alt+X panel, which stays as close to Remix Plus as it was
  built); a value you move wins over the automatic rain driver. **Textures:** aim the middle of the screen, the draw under
  it shows its two texture hashes, shader and current Remix categories; Terrain / Decal / Water / Ignore tag it (toggle) and
  apply live through the Remix API, saved in `config\menu.ini` (applied on top of `rtx.conf` at the next start, never
  written to Remix's `user.conf`). **Info:** stage, weather, ground mode and baker, every key. It draws only while open.

### Remix Plus support
- **The Remix API works on Remix Plus.** Remix Plus refused the API version RBR-RTX was built with, so the sun,
  headlights, dust, rain, snow and windscreen drops were all off there. RBR-RTX now talks to it in its own API version
  (1.0.0) and converts what differs. Lights that move get a new id every time they are re-created, because Remix Plus
  drops a light re-created under the id it had in the same frame (that is why the headlights did not show).
  Remix 1.5.2 is used exactly as before.
- **NUMOS sky follows the game's weather.** With Remix Plus's NUMOS sky (`rtx.skyMode = 1`), the stage's sun (with all
  RBR-RTX's per-stage fixes), its weather and its own fog distances drive NUMOS's sun, clouds and fog. Weather presets
  for every RBR sky and cloud type are in `config-remixplus\weather`. NUMOS lights the scene itself, so RBR-RTX's own
  sun light and Remix's global volumetric haze are off there.
- **Separate settings.** Everything for Remix Plus lives in `rbr-rtx\config-remixplus\`; the Remix 1.5.2 settings in
  `config\` are not read or changed by it.

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
