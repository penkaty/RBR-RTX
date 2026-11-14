# RBR-RTX 0.4.0: technical release notes

**Status: release candidate, 2026-11-14.** Items marked *not yet seen* were built but have not been confirmed in game.

This page is for people who want the detail. For an introduction, see the [main page](../README.md); for what each system is, [How it works](how-it-works.md). The change list is in the [changelog](../CHANGELOG.md).

## What moved
0.3.0 ran on stock RTX Remix 1.5.2. 0.4.0 runs on **Remix Plus**, Kim2091's fork of `dxvk-remix`, with a short patch queue of our own. The source is [penkaty/dxvk-remix](https://github.com/penkaty/dxvk-remix), branch `rbr-main`.

| Layer | What it is |
|---|---|
| `d3d9.dll` (x86) | Our proxy. It rebuilds camera and object transforms from RBR's vertex shader constants, then drives lights, particles, sky and options through the Remix API |
| Bridge | 32-bit game to 64-bit renderer. Remix Plus's bridge forwards the API from 32-bit hosts |
| Runtime (x64) | Remix Plus 2.0.0 plus Kim's `fnv/main` line (1.2.0-rc7) and 44 patches of our own |
| `rbr-rtx\config\` | `rtx.conf` and `user-seed.conf` (copied once to the game folder), weather presets, `atmosphere.ini`, terrain catalog, `start-options.ini` |

## Findings

### The proxy
- **`rtx.conf` was parsed twice per frame.** It cost 9 of 10.6 ms in the main menu. There is now one parsed copy, and `rbrrtx_prof.log` prices each proxy zone.
- **Car draws skip vertex capture.** RBR's exact car matrix can be solved from `c0`, so non-glass car draws go down the fixed-function path (`Alt+Period` to compare). Menu frame time went from 16.57 to 13.83 ms, with captures per frame down from 138 to 35.
- **Remix Plus drops a light re-created under the same id in one frame.** That is why headlights were missing at first. Moving lights now get a new id each time.
- **Saved wetness values were never applied at start.** `rbrrtx_menu_set_callbacks` ran before `load_ini`, saw an empty map and cleared the pending flag. Saved sliders only took effect once one was moved, and until then `rtx.conf` won. *Fix not yet seen.*

### Particles
- **Sky-lit particles were compiled out.** `RESOLVE_SKY_LIT_PARTICLES` is defined nowhere upstream, so particles only got the froxel radiance cache, which holds analytic lights. Under NUMOS that left dust dark. Reported as [RemixProjGroup/dxvk-remix#48](https://github.com/RemixProjGroup/dxvk-remix/issues/48).
- **A size guard removed falling snow.** `kMinimumAuthoredSize = 0.1` assumes centimetres. At `sceneScale` 1 with a game unit of one metre, that is 10 cm, and our flakes are 2 to 5 cm. The guard now only drops degenerate particles (0.001).
- **The screen-size cull counts render pixels.** `minParticleSize` is compared against the render resolution, which DLSS Performance halves. At 1080p that removes a 2.25 cm flake at about 16 m, so a large spawner mostly feeds particles that never draw. It is now the option `rtx.particles.minScreenSize`, set to 1 here, and the snow spawner shrank from 36 m to 26 m with larger flakes. *Not yet seen.*
- **Particles never saw a headlight beam.** Their only local light is the froxel cache times `rtx.volumetrics.volumetricConsumerGain` (0.008 under NUMOS), which is coarse with volumetrics off; a narrow beam in rain did not register. New `rtx.particles.localLightGain` adds the unshadowed light of sphere lights near each particle hit (range and count capped), cone shaping included. The cache also averaged 128 frames (`rtx.volumetrics.maxAccumulationFrames`), so particle light lagged seconds behind; the package sets 16. *Not yet seen.*
- **Overdraw is the main cost of a camera inside dust.** The proxy's particle zone stays under 0.005 ms. New `rtx.particles.maxScreenCoverage` fades a billboard between 1 and 2 screen heights, then drops it.

### Wetness
- **Raindrop size set their density too.** Cells per unit were `max(3.4 * scale - 4.4 * bias, 0.5)`, so a small ring meant the floor: one drop per 4 square units, every 0.6 s. New `rtx.wet.raindropDensity` (default 4) separates them. It scales with the square root of rain intensity and is limited so a ring stays inside the five-cell search. *Not yet seen.*
- **Slider holds outlived the stage.** Moving Global Wetness on one stage held it on the next. Holds on the three stage-driven values now end when a stage starts or ends. *Not yet seen.*
- **Rain map tracing is distance-gated.** `rtx.wet.mapUpdateDistance` traces it again only when the camera has moved a set distance, because static geometry doesn't change frame to frame.

### Ground
RBR's ground shader computes `mix(t0, t1, t1.a * v.a)` per pixel. Stages disagree on which layer holds the picture: on Whaanga it is `t1`.

- **Terrain baking moves textures.** Baked ground is looked up by position in camera-following cascades, while a decal stays on the mesh. Automatic Terrain tags gave grey ground and tags that had to be undone one by one, so they are **off by default**. Hand tags still bake, and the runner's `-Terrain` restores the old behaviour.
- **Second-layer twins now shade as ground.** The twin is what the eye sees on such stages, so it gets Smooth Normals and Wet, sent by the proxy. This came from a player's Alt+X test on Whaanga.
- **Twins need Decal.** Taking it off a road twin brings back patches, and putting it back fixes them.
- **The Alt+X click reached the baked map's hash.** The lookup now remembers the game texture from before the bake.

### Sky and weather
- **NUMOS owns all weather.** The proxy used to set fog distance and aerosol per stage on top of the preset; the two fought, and a saved preset never looked the same twice. Now the proxy only names the weather and sends the sun and the time; the presets carry the fog. `rtx.volumetrics.enable` and `rtx.enableFog` are off.
- NUMOS draws the sun, clouds, fog and aerosol from the game's weather. Ids go most specific first (`stage456_Hazy_LightCloud`, ... `Hazy`, `LightCloud`, `RBR`), and the first bound preset wins. A stage can therefore have its own sky, as Legazpi-Gabiria now does.
- Rain presets use `cloudLayer2` as a low bank instead of a high deck. It is lit and cut by the scene, unlike height fog. *Not yet seen.*
- The main menu drifts between random presets over 120 s through `gameWeatherOutgoing` and `gameWeatherBlend`, with a 10 % chance of bad weather. *Not yet seen.*

### RBRHUD
- **Its windscreen settings are pixel shader work.** `CarGlassInternal.pso` (constant table: `b0` regions only, `c0`): alpha = `a >= threshold ? a : a * opacity`, or `a * opacity` everywhere. Remix never runs it, so F4 did nothing. The proxy reads `rbrhud.ini [windscreen]` whenever it changes, writes the windscreen sheet with its alpha remapped the same way, and draws a twin texture whose generated-mod material holds it with the glass roughness. *Not yet seen.*
- **Gauge glow through the material protocol is back on,** at `rtx.fnvProtocol.glowEmissiveIntensity` 0.25 (1.0 washed the display out) and only on opaque display draws (blended needle layers turned black). *Not yet seen.*
- **Display render-target hashes depend on start order.** Many render targets are made before the car loads; the hash is a creation counter. Live tagging waits for one more probe run that shows whether the counter offset is constant.

### Indirect light response
- **The SHaRC Medium preset refreshes the cache sparsely:** one update path per 12x12 pixels and no primary vertex, set on the Derived layer above `rtx.conf`. The user layer overrides it with 8x8 and the primary vertex. `rtx.di.confidenceHistoryLength` 4 (8) lets the denoiser confidence follow light changes sooner. *Not yet seen.*
- **The NUMOS cloud screen pass is the biggest cost after ray reconstruction:** 4.2 ms at Cloud Resolution Auto, which picks Full at DLSS Performance. Quarter is the setting to try when frame time matters.

### Stability
- **Sweep crash (device lost, `nvlddmkm` event 153).** Two Alt+K sweeps died in the same place: 10 to 12 s after the "opacity micromaps off" step, right as its second `Integrate Indirect` pipeline finished compiling. The stalled frame held an OMM bake of about 507,000 micro-triangles. The cause is not proven, and no Aftermath dump exists yet. The OMM steps are out of the sweep. *Not yet seen.*
- **Key 0 can still lose the device.** It rebuilds every BLAS at once.

### Lamps
- **Twins were the wrong tool for lamps.** A lit lamp part used to be a second texture ("twin") whose emissive material came from a generated mod. Remix reloads a mod asynchronously, so lamps lagged, and brake lamps switched a frame or more late. Lamp parts, car glass and brake discs are now Remix API meshes, one per look (off, on, braking), and the D3D9 draw is skipped.
- **Car UVs are on vertex stream 1.** The first API build drew nothing because it read stream 0.
- **Lamp roles cannot be read from names alone.** Cars share vertex counts between tail and brake lenses (Celica TA64), draw one lamp as several face blocks over one vertex block (Fabia RS Rally2, Hyundai i20 WRC), draw lamp blocks opaque with the glass shader (Yaris Rally2), or leave the bumper lamps out (Porsche 911 SC RS). Roles are decided by model name, then exact vertex count, then place within the part's box. The lamp learning pulse at stage start confirms which parts answer the game's lamp signal.
- **The wheel test compared the wrong thing.** Parts that turn with a wheel are brake discs. Testing `M - B` (part matrix minus body matrix) changes with every turn of the car for a tilted node, so i20 brake lenses were taken for discs. The test is now `M x B^-1`.
- **Emissive colour is ignored when a mask is set.** Remix takes the colour from the mask texture, so the colour is baked into the mask.
- **Lamp glow in the air is the main cost of lamps.** NUMOS computes the airlight of every lamp per pixel in the composite pass: 4 to 5 ms at night in heavy rain with street lamps on a 2080 Ti. Its budget and cutoff options changed nothing measurable. Reported with six ideas as [Kim2091/numos-sky#2](https://github.com/Kim2091/numos-sky/issues/2). Rear lamps are left out of the airlight.

### Materials
- **The game's physics is the best classifier.** Every ground triangle carries a surface id for the tyre model. The proxy reads the same collision data, looks up the id under each ground texture and caches texture to class in `rtx-remix\modsbr_rtx_lit\surfaces.txt`.
- **The terrain baker ignores material constants,** so class materials carry small roughness maps instead of numbers.
- **Road mask.** The same physics data gives the road's outline. It goes to the runtime as a mask, widened 5 m with a soft edge, and the wetness shader lowers the water level across it: puddles on the road, wet ground without puddles beside it (`rtx.wetNoPuddleTextures`).
- **Card normals only on cut-outs.** Applied to every foliage-class texture, they reached car parts and walls. They now apply to cut-out alpha only, and never to a texture you classified otherwise in `rtx.conf`.

### Effects
- **One hook instead of one feature per effect.** `CEmitter_Emit` is the game's single spawn function. Hooking it gives every emitter with its position, velocity and amount; a table maps each effect type to a Remix particle system. Wheel dust and smoke are driven per wheel, rain and snow by one spawner around the camera.
- **Rain cost is rain times lamps, not the drop count.** Halving the drops or shrinking the area gained little; switching lamps off in rain gained more than both.
- **Precipitation needs its own motion vectors.** Without them each flake left copies of itself a frame's fall apart under Ray Reconstruction (`rtx.particles.motionVector*`, `antiGhostCoverage`).
- **The game's windscreen drops are world-space additive quads** and work as particle textures. Their second, subtractive pass cannot be drawn by Remix and is ignored.

### Logging
- **The profiler was always on.** About 8000 timed zones a frame, two `QueryPerformanceCounter` calls each, also when no log folder existed. Zones now time nothing without a log folder.
- **A release install logs on demand.** Key 8 and Alt+K point the log at `rbr-rtx\logs` for the rest of the session. `diagnostics.txt` still logs from the start. The bridge's periodic performance breakdown is off in the package.

## Options added by our runtime patches
| Option | Default | Here | Purpose |
|---|---|---|---|
| `rtx.particles.maxScreenCoverage` | 0 (off) | 1 (from `particles.ini`) | Fade billboards near screen-filling size |
| `rtx.particles.minScreenSize` | 2 | 1 | Screen-size cull, in render pixels |
| `rtx.wet.mapUpdateDistance` | 0 | set in F3 | Rain map trace distance |
| `rtx.wet.raindropDensity` | 4 | 4 | Raindrops per grid axis |
| `rtx.particleSkyAmbientScale` | 1 | 1 | Scale of the restored sky term |
| `rtx.particles.localLightGain` | 0 (off) | 1 | Sphere lights (headlights, lamps) on API particles |
| `rtx.particles.localLightRangeMeters` | 40 | 40 | Lights farther from a particle are skipped |
| `rtx.particles.localLightMaxCount` | 64 | 64 | Most sphere lights looked at per particle hit |
| `rtx.particles.motionVector*`, `rtx.particles.antiGhostCoverage` | off | set by the proxy while rain or snow falls | Motion vectors and anti-ghost mask for precipitation |
| `rtx.wetNoPuddleTextures` | empty | sent by the proxy | Wet surfaces that hold no standing water |
| `rtx.cardNormals.dome`, `domeSpread` | off | see `rtx.conf` | Card normals as a dome per card |

`rtx.numFramesToKeepInstances = 0` was tried against brake lenses lingering behind the car and undone: it did not help, and it stopped rain and snow from showing.

## Performance
Comparison runs against earlier versions are still to come, and this section will carry their numbers. Measured so far, on an RTX 2080 Ti at 1080p: the menu and car figures above, and about 2.8 ms saved by the car path on Gabiria.

## What is not done
- Materials are per class of surface, not per texture. Finer materials come after 0.4.0.
- Lancia Delta tail lamps, car dirt, and the cost of lamp glow in rain at night.
- Bob's Track Builder stages still fail under path tracing.
- See [Known issues](known-issues.md).

## Credits
This release stands on Kim2091's Remix Plus and NUMOS, and on xoxor4d's wetness and CardNormals work. Full list on the [main page](../README.md).
