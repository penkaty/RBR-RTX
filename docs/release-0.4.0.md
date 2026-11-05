# RBR-RTX 0.4.0: technical release notes

**Status: being prepared, not released.** Items marked *not yet seen* were built but have not been confirmed in game.

This page is for people who want the detail. For an introduction, see the [main page](../README.md). The change list is in the [changelog](../CHANGELOG.md).

## What moved
0.3.0 ran on stock RTX Remix 1.5.2. 0.4.0 runs on **Remix Plus**, Kim2091's fork of `dxvk-remix`, with a short patch queue of our own. The source is [penkaty/dxvk-remix](https://github.com/penkaty/dxvk-remix), branch `rbr-main`.

| Layer | What it is |
|---|---|
| `d3d9.dll` (x86) | Our proxy. It rebuilds camera and object transforms from RBR's vertex shader constants, then drives lights, particles, sky and options through the Remix API |
| Bridge | 32-bit game to 64-bit renderer. Remix Plus's bridge forwards the API from 32-bit hosts |
| Runtime (x64) | Remix Plus 2.0.0 plus Kim's `fnv/main` line and our patches |
| `rbr-rtx\config-remixplus\` | Its own `rtx.conf`, weather presets, `atmosphere.ini` and terrain catalog |

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

`rtx.numFramesToKeepInstances = 0` was tried against brake lenses lingering behind the car and undone: it did not help, and it stopped rain and snow from showing.

## Performance
Comparison runs against earlier versions are still to come, and this section will carry their numbers. Measured so far, on an RTX 2080 Ti at 1080p: the menu and car figures above, and about 2.8 ms saved by the car path on Gabiria.

## What is not done
- Material response without PBR: every unmodded texture shares one legacy material, so surfaces can look too shiny or flat. Per-class roughness and specular are next.
- Bob's Track Builder stages still fail under path tracing.
- See [Known issues](known-issues.md).

## Credits
This release stands on Kim2091's Remix Plus and NUMOS, and on xoxor4d's wetness and CardNormals work. Full list on the [main page](../README.md).
