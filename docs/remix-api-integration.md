# RBR-RTX: how the Remix API is driven

A technical write-up of how RBR-RTX turns Richard Burns Rally (2004, 32-bit, Direct3D 9, vertex-shader only) into
something RTX Remix can path trace, and how it creates what the game never had through the Remix API: particles,
lights, lamp glass, emissives and materials.

It is written for runtime developers (Remix Plus in particular) and for people porting another game. Every claim
points at the source in `src/d3d9-proxy/`. Runtime patches live on the `rbr-main` branch of
[penkaty/dxvk-remix](https://github.com/penkaty/dxvk-remix). Section 9 lists the patches worth taking upstream.

The work stands on Kim2091's [Remix Plus](https://github.com/RemixProjGroup/dxvk-remix) runtime and bridge (the
32-bit API forwarding made all of this possible) and [NUMOS](https://github.com/Kim2091/numos-sky) sky. The retained
API-instance approach follows his New Vegas RTX work, and the wetness and card normals are ports of xoxor4d's work.

## Contents
1. [The stack](#1-the-stack)
2. [Spaces, units and float precision](#2-spaces-units-and-float-precision)
3. [Talking to the runtime from a 32-bit host](#3-talking-to-the-runtime-from-a-32-bit-host)
4. [Particles and weather](#4-particles-and-weather)
5. [Car lamps](#5-car-lamps)
6. [World lamps and emissives from the F3 menu](#6-world-lamps-and-emissives-from-the-f3-menu)
7. [The material system](#7-the-material-system)
8. [Performance: what was measured](#8-performance-what-was-measured)
9. [Upstream candidates](#9-upstream-candidates)
10. [Checklist for another game](#10-checklist-for-another-game)

---

## 1. The stack

```
RichardBurnsRally_SSE.exe  (x86, D3D9, every draw through a vertex shader)
  -> d3d9.dll  RBR-RTX proxy (x86, C++)
       - rebuilds view/projection/world from VS constants, re-issues draws as fixed-function
       - hooks game functions (particles, lamp signal, camera signal, collision queries)
       - drives the Remix API: lights, meshes, particle systems, config variables, game values
  -> Remix bridge client (x86)  ->  shared memory  ->  NvRemixBridge.exe (x64)
  -> Remix Plus runtime (x64) + 44 patches of ours  ->  Vulkan path tracer, NUMOS, DLSS-RR
```

**Why a proxy at all:** Remix wants fixed-function transforms. RBR sends every matrix as anonymous VS constants, so
to Remix nothing has a camera or a position. The proxy identifies each `.vso` shader by its bytecode, reads the
constant registers it uses (`c0` = W·V·P for most object shaders, `c8..c11` = W where the shader has it), builds V and P
for the frame, and hands Remix `SetTransform` calls before each draw (`comp.cpp`, sections "matrices" and "per draw").

**Remix API surface used:** `CreateLight` / `DrawLightInstance` (sun, car lamps, world lamps, user lights),
`CreateMaterial` / `CreateMesh` / `DrawInstance` (lamp lenses, glass, discs, drivers, particle emitters),
`InstanceInfoParticleSystemEXT` + `InstanceInfoBlendEXT` (every particle effect), `SetConfigVariable` (texture lists,
per-weather settings), and Remix Plus's `SetGameValue` / `GetGameValue` (a side channel for things the struct API
cannot carry).

---

## 2. Spaces, units and float precision

### Spaces
- **Render space** is what Remix sees: D3D left-handed, **Y up**.
- **Physics space** is what game objects hold (car pose, emitter positions, camera signal, collision): **Z up**. The
  conversion is a Y/Z swap and is applied everywhere game data meets the API (`effects.cpp` header).
- **Units:** `rtx.sceneScale = 1`, one game unit is one metre, so every Remix value documented "in centimetres" is
  read as metres here. This matters for hard-coded guards in the runtime (section 4.6).

### Float precision on 30 km stages
RBR stages are long; the camera can be 3 to 5 km from the map origin. Two things broke there:

1. **`W = c0 · (VP)⁻¹` is ill-conditioned.** RBR's projection has `P._33 ≈ 1.0001`, so inverting VP throws away
   the depth column's precision. The derived world matrix drifted **0.3 to 8 m** on Noiker (about 2.7 km out),
   0.2 to 2 m on Legazpi, 2.9 m on Peklo: drivers slid along the view ray, car lamps jumped around the car.
2. **The naïve check of `c8` against the derived W** refused the exact matrix exactly where the derived one was worst.

What we do instead (`comp.cpp` `world_from_c0`, the per-draw block around it):
- All matrix algebra in **double** (`DMat`, `mul_d`).
- **Solve W from the x, y and w columns of `c0 = W·VP` only** (a 3×3 system per row, Cramer's rule in double), never
  touching the near-degenerate z column. Error: **about 1 mm** at the same distances (offline test: 0.008 m worst case
  against 2.8 m for the inverse at 3 km).
- **Verify `c8` forward, not backward:** push the model's origin and its three unit axes through `c8·VP` and compare
  them with `c0` in NDC. Close to the camera plane (bonnet and cockpit views), compare in clip space relative to its
  magnitude, because `x/w` magnifies float rounding there. If `c8` passes, it wins.
- For **non-RBR vertex shaders** (other plugins draw the cockpit interior) `world_from_c0` is taken only when the
  forward product reproduces `c0`'s z column too, so HUD shaders whose `c0` is not W·VP are left alone.

### Retained instances vs per-frame instances
The static-world path (`apiworld.cpp`) follows the New Vegas approach: every scenery draw becomes one API mesh built
once from the draw's own VB/IB range, a material on the game texture through `ImportD3D9Texture` (no copy), one
retained instance replayed by `DrawRetainedInstances`, and the D3D9 draw is no longer sent. It works, but:
- RBR at 1080p is **GPU bound**: Legazpi went 37.8 to 36.9 fps with it (no gain), and draws whose vertices change
  (clocks, running marshals, flags) froze without VB change detection. **It ships off** (`materials.ini [api_world]`,
  Alt+W to compare).
- **Moving objects cannot be retained.** The retained table is flushed at the next frame's first `BeginScene`, which
  leaves the object one frame behind (0.7 m at 150 km/h). Drivers, co-drivers and car lamp parts are therefore API
  meshes in **model space**, drawn every frame with `DrawInstance` and the precise `world_from_c0` matrix.

---

## 3. Talking to the runtime from a 32-bit host

### API version shim (`remixplus_api.cpp`)
The proxy was written against NVIDIA's `remix_c.h` (API 0.6.4). Remix Plus (API 1.0.0) refuses that version
(`INCOMPATIBLE_VERSION`), and its ABI differs: other `remixapi_Interface` slot order, extra `LightInfo` fields
(`isDynamic`, `ignoreViewModel`), extra DLSS mask fields in `MaterialInfoOpaqueEXT`, and **sTypes shifted from 25 on**
(`TEXTURE_INFO` was inserted at 25, so `PARTICLE_SYSTEM_EXT` is 26 there). One file includes the Remix Plus header,
initialises as 1.0.0 and fills an NVIDIA-layout table with converters; the rest of the proxy is unchanged and still
runs on Remix 1.5.2.

### Light identity
- On Remix Plus, `CreateLight` with a **hash that is live updates that light in place** and keeps its buffer slot,
  so RTXDI reservoirs and the denoiser keep their history. Moving lamps are therefore re-created under the same hash
  every frame and never destroyed. All lights get `isDynamic = 1`.
- **A create whose hash was destroyed in the same frame is dropped.** That is why headlights first went missing.
  The shim mixes a counter into the hash only in that case.
- The 2.0.0 bridge client stubs `UpdateLightDefinition`, so every re-create leaves a ~40-byte handle entry on the
  bridge server until `DestroyLight`: about 50 MB an hour with four lamps moving every frame (section 9).

### Config layers: the host must never write the user's file
Stock `SetConfigVariable` writes into the User Settings layer, so pressing Save in Alt+X persisted per-stage values
the proxy had set, and a host's texture list replaced the user's own tags. Our patch (runtime `6e5922182`):
- **texture lists** go to a layer **below `rtx.conf`**. A host list replaces the host's previous list; a `-0x…`
  entry is an explicit removal in the `rtx.conf` layer.
- **everything else** goes to a layer just **below `user.conf`**, which Save never writes.
- `override:` / `unoverride:` key prefixes (`de2427eb1`) write a non-saved **Host Overrides** layer *above*
  `user.conf`, for the few values the host must win (settings sweeps, `start-options.ini`).

### Game values as a capability channel
Some features need data the struct API has no field for. We use Remix Plus's `SetGameValue` / `GetGameValue`:
- **Handshake:** a patched runtime sets `__rbrrtx.killbox.v` on its first simulation frame (`1` = kill box,
  `2` = also camera-relative trails). The proxy polls it (20 tries, 3 s apart) and only then sets the matching bits,
  so an unpatched runtime never misreads them.
- **Per-frame payloads** as strings, e.g. `__rbrrtx.carbox = "1 cx cy cz ax 0 az halfL halfW halfH"`, and the wet
  road mask.
- **Spare bits in `collisionMode`** (a `uint8_t`) carry particle flags: `0x80` spawn shelter, `0x40` car kill box,
  `0x20` trails relative to the camera. This was a stop-gap; section 9 proposes real fields.

---

## 4. Particles and weather

### 4.1 Principle
The game's particles are camera-facing sprite stacks: slow to path trace, unlit, and invisible to RR's motion
vectors. RBR-RTX **lets the game decide when and where an effect fires, and lets Remix decide what it looks like**.
Every game emitter is cut before it allocates anything, and a Remix GPU particle system runs at the place it fired
(`effects.cpp`). Key 0 (raster mode) gives the game's own particles back for A/B.

### 4.2 Game side: three hooks

| Hook | Address | What it gives us |
|---|---|---|
| `CEmitter_CreateParticle` | `0x5D9580` (fastcall, `ecx` = emitter) | **Every** particle the game creates, from any route: tire signals, auto-rate emitters, collisions, exhaust. Name inline at `+0`, spawn position at `+0x40` (physics space). |
| `CEmitter_Emit` | `0x5DC100` (thiscall: `arg1, pos, vel, amount`) | Wheel dust and tyre smoke requests. Called with `amount` for the dry wheel emitters; we zero `amount` and note the wheel instead. |
| Tire signal | vtable slot `0x74DC58` (thunk `0x5E3C70`) | Each wheel's contact point (midpoint of two points in the signal) and its surface id per frame, settled over 250 ms. |
| Rain camera signal | vtable slot `0x74D1B0` (thunk `0x5DA8B0`) | The rain emitters' camera position and forward vector, and the emitter object itself (connection at `-0x4C`). |

**Cutting at creation (`create_hook`).** For an emitter whose type has a row in our table we return a **dummy slot**
instead of a pooled particle: zeroed 64 bytes with the class pointer at `+0x20` copied from the emitter's holder,
so the callers' writes land harmlessly. Nothing is simulated or drawn by the game. Before that we note the **site**:
up to 6 per effect type, merged within `merge` metres, alive for `hold_ms`. Emitters without a row stay the game's
and are logged once, so new stage content shows up in the log instead of disappearing.

Rows are keyed by the emitter's **name without the stage maker's prefix** and classified once per emitter pointer:
gravel, wet spray, grass, debris (+ smoke), splash (+ smoke), water off the car, exhaust, backfire pop, fire (+ smoke),
engine steam, sparks, glass, snow wall (+ smoke), stage smoke. Engine damage emitters live in car space (position 0);
they are moved to the bonnet, measured from the car model's lamp nodes.

**Rain and snow.** RBR's `CEmitterRain` keeps a fixed pool (rain 1000 + 20, snow 200 + 10 + 100) wrapped in a box
around the camera; shown drops = pool × weather intensity. We find the emitters through their own camera signal
(the connection sits at emitter `+0x4C`), set their **pool size at `+0x30` to 0** once while replaced (restored on
Alt+7), and skip draws of their textures. Pools are touched once per change only: touching a live-looking emitter
every frame hit freed memory when a stage was left. Weather intensity comes from the weather object at `[0x7C36A8]`
(`+0x2C + +0x34 × +0x30`, percent).

### 4.3 Remix side: one emitter mesh, one descriptor per effect

Each effect is:
- a **material** (`MaterialInfoOpaqueEXT`, `useDrawCallAlphaState = 1`, our sprite as albedo; for sparks, fire and
  the backfire pop the same texture as emissive with `emissiveIntensity`),
- a **mesh**: a 0.8 m quad facing +Y (particles leave along the normal),
- drawn every frame per live site with `DrawInstance`, its `pNext` chain being
  `InstanceInfoBlendEXT` (src-alpha / one-minus-src-alpha, texture × vertex colour) → `InstanceInfoParticleSystemEXT`.

Details that mattered:
- **The descriptor is the system's identity.** The runtime keys a particle system on the descriptor. We build each
  descriptor once and cache it with its `AnimatedFloat` curve storage (`DescStore`). It is rebuilt only when
  `particles.ini` is re-read or a runtime capability appears. No per-frame arrays, no per-frame hashing on our side,
  no system re-created by accident.
- **Surface-tinted dust shares systems.** Wheel dust takes the colour of the ground under each wheel, read once per
  new surface id. The tinted colour is **quantised to 1/32 per channel**, so near-identical surfaces map to the same
  descriptor and the same system.
- **Hashes** are ASCII tags (`"RBRTX_f0"` + row) XOR an epoch in the high bits, so a settings reload makes new
  meshes and materials instead of colliding with live ones.
- **Per-wheel logic** (`draw_emitter`): dust only while the car moves (hysteresis in km/h) and the game asked for dust
  at that wheel within 300 ms. Tyre smoke only on dry tarmac (surface ids read from `Physics\physics.lsp`), after a slide
  of `slide_seconds`, never in rain. Snow surfaces get their own lighter dust. Wheel points come from RBR's tire
  signal, never guessed.

### 4.4 Precipitation: a graded disk around the camera

One spawner per weather step (`[rain_light]`, `[rain_heavy]`, `[snow_light]`, `[snow_heavy]`; heavy above
`heavy_from` % intensity). Light and heavy differ in drop size and fall speed, not only in count.

**Spawn density by tessellation.** Remix picks a spawn triangle **uniformly per triangle, whatever its area**. So we
control density with mesh topology (`disk_mesh`): a centre fan, then 2 m rings whose triangle count is
`area × density(r)`, with density 1 up to `r0` and `r0/r` beyond. Sectors beyond an ellipse (far ahead, narrower to the
sides, short behind) are left out. The spawn rate and cap are scaled by the disk's density-weighted area relative to the
tuned square, so the density near the camera stays what was tuned. The mesh faces down at `above` metres and is placed
`ahead + ahead_per_kmh × speed` along the camera's **level** heading, rotated with it.

**Far layer through Remix's own dust.** Past the near disk, more API particles would be wasted (they fall below one
pixel). Instead the proxy configures `rtx.dust.*` (GPU points over the view frustum between `minSpawnDistance` and
`maxSpawnDistance`, path-traced radiance, depth culled) from the same preset: fall speed, turbulence, life, opacity,
point size = the drop's metre size projected at the start distance. It is sent **only when the weather step changes**,
and the band is **cut at the stage's fog end** (from the stage's `TrackSettings` ini): nothing is spawned where
nothing can be seen.

**Runtime-side precipitation features** (each a patch, section 9):
- **Motion vectors for particles**: the flakes' pixels take their own motion vector from depth and the preset's fall
  velocity (`rtx.particles.motionVector*`), plus RR's anti-ghost mask for non-emissive particles
  (`rtx.particles.antiGhostCoverage`). Without both, every flake left a trail of copies one frame's fall apart under RR.
- **Particle objects live one frame** without a draw. A particle system's draw carries new geometry each frame
  (its hash mixes count and generation), so with `numFramesToKeepInstances > 1` the old frames' objects stayed in the
  BVH: a stack of copies per flake.
- **Trails and velocity alignment relative to the moving camera** (`0x20`): streaks look right from a car at 150 km/h.
- **Car kill box** (`0x40` + `__rbrrtx.carbox`): an oriented box around the player car, updated every frame, kills
  particles inside it. Kim's spawn shelter probe runs once at spawn against last frame's world, so a moving car drove
  into drops aimed past it.
- **Spawn shelter** (`0x80`): exposes the runtime's existing `traceSpawnOcclusion` through the API: spawn only under
  open sky, die on the first opaque surface. No rain under bridges or roofs.
- Per-preset values (`antiGhost`, `motionVectorVelocity = (0, -fall, 0)`, `maxScreenCoverage`) are sent when the
  weather step changes and reset to the runtime's defaults without precipitation, so dry-stage dust is unaffected.

### 4.5 Lighting the particles
- **Sky light was compiled out.** `RESOLVE_SKY_LIT_PARTICLES` is defined nowhere upstream, so API particles only saw the
  froxel radiance cache, which holds analytic lights only. Under NUMOS, dust was black. Restored, with
  `rtx.particleSkyAmbientScale` (reported as RemixProjGroup/dxvk-remix#48).
- **Local lights did not reach them.** The cache is multiplied by `rtx.volumetrics.volumetricConsumerGain` (0.008
  under NUMOS) and is coarse with volumetrics off, so a headlight beam never showed in rain. New
  `rtx.particles.localLightGain`: the particle hit adds the **unshadowed** contribution of sphere lights within
  `localLightRangeMeters`, at most `localLightMaxCount`, light shaping (cone) included. Off by default.
- `rtx.volumetrics.maxAccumulationFrames` 128 → 16, so particle light follows moving lamps within a fraction of a second.

### 4.6 Size and screen guards
- `kMinimumAuthoredSize = 0.1` assumes centimetres. At `sceneScale` 1 it removed every snowflake (2 to 5 cm). It now
  drops only degenerate sizes (0.001). A real fix would scale it by `sceneScale`.
- `minParticleSize` is compared in **render** pixels, which DLSS Performance halves: at 1080p a 2.25 cm flake vanished
  at 16 m. Exposed as `rtx.particles.minScreenSize` (1 here).
- `rtx.particles.maxScreenCoverage` fades a billboard between 1 and 2 screen heights and then drops it. A camera inside
  dust is an overdraw problem, not a count problem.

---

## 5. Car lamps

RBR's headlights are glow sprites; nothing lights anything. Lamps are created per car without per-car tuning
(`headlights.cpp`, `litparts.cpp`, `apiworld.cpp` "car lamp parts").

### 5.1 Where the lamps are: the model's node tree
The car model (`.sgc`, from `Cars\Cars.ini`) holds a node tree: a tag `0C 13 2F 01`, size, name, id, parent and a
3×4 `[R|t]` to the parent. Chaining to the root gives each `Point_light_lf/rf`, `Point_light_brake_lb/rb` and
`Point_exhaust1/2` in model space (all 111 car models of a full install decode).

**Car pose:** the game draws most of the body (~20 meshes) with **one** matrix, the body node's. Matching that draw
gives an exact pose for the frame; otherwise the game's own rotation and position, compensated by a per-car offset
measured from the body draw, are used. The matrix itself is the precise one from section 2.

### 5.2 Photometry
- Lamps are specified like real lamps: **luminous intensity in candela** on the beam axis and **colour temperature in
  kelvin** (Planckian locus, Kim et al. cubic fit, XYZ → linear Rec.709, normalised to luminance 1).
- The lamp type follows the car's year: LED from 2010, xenon 1998 to 2009, halogen before.
- Candela becomes Remix radiance through **NUMOS's own calibration** (its sun = 127,000 lux, `luxPerUnit`). A **sun cap**
  keeps every lamp at or below the sun on a surface 1 m away, and the sphere radius no longer changes brightness.
- Sphere lights with light shaping: low beams (36° half angle, softness 0.15), brights as a tight second spot toward the
  car's centre line, tail lamps, brake lamps from the brake input (car `+0x860`), reversing lamp in R, and a short
  orange point at the exhaust tip whenever the game draws its backfire sprite (detected by texture hash).
- Automatic: daytime running lights, brights below 4° of sun elevation, on in rain, snow or fog. Rear lamps are
  excluded from NUMOS's in-air glow (their cost, section 8).

### 5.3 Which mesh is which lamp: learning from the game's own switch
Lit lamp parts are separate meshes the game swaps in (`e_light_lf_lit`, `e_light_brake_lb_lit`,
`e_brakedisc_lf_lit`, ...) and texture from the car's shared atlas, so a material keyed on that texture would light
the whole car.

1. **Fingerprint every car draw** (buffers + index range) and count it per signal: lamps, brake, reverse.
2. **At a clean switch of one signal** (others steady, same camera view on both sides), a fingerprint that appears with
   it is that signal's lit part; one that vanishes with the brake is the unlit brake lens.
3. **Learning pulse:** by day nobody switches the lamps, so nothing would be learnt. At stage start the proxy toggles
   the **game's** lamp signal off then on (car frames 30, 240, 480; 30 frames each) until the parts are judged. The
   Remix lamps do not follow the pulse; only the game's own meshes do.
4. **Roles** come from the model's node names, then exact vertex count, then position within the part's bounding box.
   Cars share vertex counts between tail and brake lenses, draw one lamp as several face blocks, or draw lamp glass
   opaque. **Wheel-attached parts (brake discs)** are found with `M × B⁻¹` (part matrix relative to the body), which is
   constant for body parts. The earlier `M − B` test changed with the car's own rotation and misread tilted nodes.
5. Results are cached per car (`litparts.txt`), so a known car is right from its first frame next time.

### 5.4 Lamp glass as API meshes, not texture twins
The first version gave a lit part a **twin texture** (16×16, content derived from (atlas hash, signal), so its XXH3
hash is known in advance) and a generated USD mod gave that hash an emissive material. Remix loads mods
asynchronously, so lamps came on seconds late, brake lamps lagged by frames, and the twin z-fought the unlit mesh.

Now every lamp part, the car glass and the brake discs are **API meshes in model space**, **one mesh per look**
(off, on, braking) over the same vertices, because Remix binds the material to the mesh. They are drawn every frame
the game draws the part, with that draw's world matrix and blend state (`D3DBLEND` → `VkBlendFactor`), and **the D3D9
draw is skipped**. Learnt the hard way:
- **Car UVs are on vertex stream 1.** The first build read stream 0 and drew nothing.
- **With an emissive mask set, Remix ignores the emissive colour constant**, so the colour is baked into the mask.

---

## 6. World lamps and emissives from the F3 menu

### 6.1 The menu itself
F3 is **Dear ImGui drawn through the game's own D3D9 device just before `Present`**, fed by a subclass of the game
window (`comp.cpp` "F3 menu glue", `menu/`). Alt+X (Remix's menu) can be open at the same time. Closed, it costs one
bool test per draw; open, 0.09 to 0.44 ms. It saves to its own `menu.ini` and **never writes `user.conf`**: everything it
sends goes through the host config layers of section 3.

### 6.2 Picking: a CPU ray against the frame's draws
"Aim with the crosshair" is a ray from the camera through the screen centre, cast in the next primary frame against
the triangles of **every world draw** (stream-0 position + `TEXCOORD0`, lists/strips/fans, half-float UVs decoded, car
excluded, foliage optionally skipped). The nearest hit returns the shader, distance, both texture hashes (`t0`, `t1`),
the share of up-facing area (used to warn before tagging a wall as Terrain), and the **UV island**: the triangles
connected to the hit one (welded by position, within 2.5 m), whose UV rectangle identifies "this lamp head" inside a
texture atlas. The rectangle can be edited by dragging in the texture preview.

### 6.3 Lamps from a texture rectangle (`worldlights.cpp`)
A marked lamp kind is keyed by **(texture hash, UV rectangles)**, so it applies to every copy on every stage that uses
the texture. Each frame, for each draw with that texture, the vertices whose `TEXCOORD0` lies in a rectangle are
grouped within 1.5 m; each group's top, centre or bottom (bulbs under glass) plus an offset places a **sphere light**.
Groups within 1 m share a light. Presets carry lumens and colour temperature (sodium, LED, floodlight, stadium, coloured
spot, point light), cone and softness, and **aim** (at a point or straight down). Lights fade in as the displayed sun
drops below `on_below_sun`, and only the `max_lights` nearest the camera are kept, through the same create-under-same-hash
path as the car lamps.

### 6.4 Emissive glass and lit windows: twins plus a generated mod
For the glowing part itself the proxy still uses the twin-texture route (`custom.cpp`, `worldlights.cpp`, `genmod.cpp`):
1. The first time the texture is drawn, its **mip chain is saved from GPU memory** to a `.dds` beside the mod (stages
   are encrypted; there is no file to point at).
2. A 256×256 **emissive mask**, white on the marked rectangles, holds each emissive's colour and relative strength.
   For **windows**, every pane gets a random lit/unlit state and brightness baked into the mask, so a facade shows a
   believable mix.
3. While the emissive is on, the draw is bound to a **16×16 twin** whose hash is known in advance; the generated mod
   gives that hash the saved texture as albedo plus the mask. Night fades in over a few discrete levels, each its own twin.
4. **One mod folder per generator** (`rbr_rtx_lit`, `rbr_rtx_emissive`, `rbr_rtx_litparts`, ...), each rewritten only when
   its own text changes and at most every few seconds, because a USD reload is a hitch and reloading a shared file made
   unrelated glass flicker.

**Texture variants:** a damp or wet track loads repainted texture sets with other hashes. The proxy learns dry/damp/wet
pairs from **geometry signatures** (the same object drawn with another texture) and lets the variant inherit the
emissives, lamps and tags, each with its own saved texture and twin (`texvariants.cpp`).

### 6.5 Free lights and texture tags
- **Free lights** are sphere lights at absolute stage positions (at the crosshair hit or in front of the camera),
  kept per stage, with colour, lumens, radius and flicker.
- **Texture tags** (Terrain, Decal, Water, Ignore) toggle a hash in the menu's own set; the merged list goes to
  `rtx.terrainTextures` / `rtx.decalTextures` / `rtx.animatedWaterTextures` / `rtx.ignoreTextures` through
  `SetConfigVariable`, at most every 20 frames, in the host layer below `rtx.conf`. Untagging something `rtx.conf`
  tags writes an explicit `-0x…` removal.

---

## 7. The material system

### 7.1 Classifying ground with the game's physics (`surfmap.cpp`)
Every collision triangle in RBR carries a surface id for the tyre model (a 16×16 grid of ids per material map in the
stage's `.mat`; `Tarmac_*`, `Gravel_*`, `Grass_*`, `Snow*`, `Rock_*` from `physics.lsp`). For each ground texture the
proxy takes a few points on its up-facing triangles and runs the game's own **segment query**
(`CCollisionWorld_SegmentQuery 0x4FD760`, a 2 m vertical segment) to read the surface id under them. The most frequent
id is the texture's class; it is **road** when enough of its points are tarmac or gravel. It works on encrypted stages,
without driving, and the verdicts are cached (`surfaces.txt`), so a stage seen once is right from its first frame.

It feeds:
- **Class materials**: a generated mod gives each probed texture its class's roughness, metallic and a shared detail
  normal and roughness map. The terrain baker ignores material constants, so classes carry small roughness maps
  instead of numbers. Textures the physics never touches (rock, wood, metal, buildings, fences, signs, foliage) are
  classed from a catalogue of their names.
- **Wetness:** `rtx.wetTextures` (road) and `rtx.wetNoPuddleTextures` (runtime patch: wet, never standing water), so
  verges darken without puddles.
- **Road mask:** the same physics data gives the road's outline. It goes to the runtime as a mask, widened 5 m with a
  soft edge, and the wetness shader lowers the water level across it.

### 7.2 Other material sources
- **Car paint:** the alpha of a car's `*_external.dds` is the game's environment-map strength, not opacity. The proxy
  writes a roughness map from it (alpha 255 = glossy) into the generated mod, keyed on the texture hash; the F3 page sets
  the glossy/matte roughness and metallic.
- **Card normals** (xoxor4d's port) apply only to alpha-cut textures and never to a texture classified otherwise.
- **Game wet sets:** damp and wet tracks load textures at about 0.78× and 0.60× the dry brightness, so the runtime's own
  wet darkening is scaled down by the set that is loaded.

---

## 8. Performance: what was measured

RTX 2080 Ti, 1080p, DLSS-RR. The proxy has its own profiler (`prof.cpp`, `rbrrtx_prof.log`), which prices every zone
per frame on both the game and render threads.

| Change | Before | After |
|---|---|---|
| One parsed copy of `rtx.conf`, refreshed on file time (a lux helper parsed the whole file 3× per call, twice per frame) | 10.6 ms game-thread CPU, 9.0 of it parsing | about 1.6 ms |
| 5,800-entry terrain catalogue moved out of `rtx.conf` | 377 KB | 163 KB |
| Car draws skip Remix vertex capture (world matrix solved from `c0`, fixed-function path); main menu | 16.57 ms frame (p50), 138 captures, 5.52 ms render-thread CPU | 13.83 ms, 35 captures, 2.55 ms (GPU passes unchanged) |
| Same, Zlin outside view | 20.42 ms | 17.65 ms |
| Same, Gabiria | | about 2.8 ms saved |
| Profiler zones time nothing without a log folder (8,000 zones × 2 `QueryPerformanceCounter` per frame) | always on | 0 in a release install |
| Game emitters cut at creation (dummy slot), rain pools set to 0 | game simulation + sprite draws | none; the proxy's particle zone stays under 0.005 ms |
| Particle descriptors cached, colours quantised | descriptor per frame | rebuilt only on settings change |
| Far precipitation via `rtx.dust` GPU points, cut at fog end | | no API particles below one pixel |
| Lamp glass as API meshes instead of twin texture + mod reload | lamps seconds late, reload hitches | same frame |
| Rain map traced only after the camera moved `rtx.wet.mapUpdateDistance` | every frame | on movement |
| F3 menu closed | | one bool per draw |

**What did not pay off, and what costs the most:**
- **Static world through retained API instances:** no gain, GPU bound (37.8 vs 36.9 fps). Off.
- **The car is the biggest single cost:** hiding it saves about 6 ms (Legazpi 20 → 14 ms, an E30 15.5 → 7.8 ms).
- **Rain at night costs rain × lamps, not the drop count.** Halving the drops or shrinking the disk gained little;
  switching the lamps off gained more than both. The term is NUMOS's per-pixel airlight of every lamp in the composite
  pass: 4 to 5 ms in heavy rain with street lamps. Its budget and cutoff options measured 0.17 to 0.24 ms, so they were
  reverted. Reported with six ideas as Kim2091/numos-sky#2; rear lamps are left out of it.
- **NUMOS cloud screen pass:** 4.2 ms at Cloud Resolution Auto (which picks Full at DLSS Performance); Quarter is the
  lever.
- **SHaRC Medium** updates one path per 12×12 pixels without the primary vertex; we use 8×8 with it, and
  `rtx.di.confidenceHistoryLength` 4, so indirect light follows lamps switching sooner (quality, not speed).
- `numFramesToKeepInstances = 0` against lingering brake lenses did not help and hid rain and snow.

---

## 9. Upstream candidates

Our runtime branch sits on Kim's `fnv/main` line. Each item below is one self-contained PR against Remix Plus,
rebased onto its current main, with the option documented and **off or unchanged by default** where it changes
behaviour. RBR-only commits (welcome-window buttons, RBR texture-grid tweaks) stay on our branch.

### Tier 1: bug fixes any API host hits
| PR | Commits on `rbr-main` | Why generic |
|---|---|---|
| Restore sky light for API particles (`RESOLVE_SKY_LIT_PARTICLES`) + `rtx.particleSkyAmbientScale` | `9b25c6e43`, `63b6319c1` | Every API particle under NUMOS or a sky-lit scene is dark. Issue #48 |
| Particle objects live at most one frame without a draw; drop spawns older than a frame | `6eda0cb14`, `82ebf25c2`, `e7efe9026` | Ghost copies with `numFramesToKeepInstances > 1`; stale instance indices when ray tracing is toggled |
| Minimum authored particle size only drops degenerate sizes (or scale by `sceneScale`) | `0d98a4610` | Breaks every game whose unit is not a centimetre |
| Host `SetConfigVariable` values in non-saved layers; texture lists below `rtx.conf` with `-0x` removals; `override:` layer | `6e5922182`, `de2427eb1` | Any host that sets options per level currently pollutes `user.conf` on Save |
| Stability: ray tracing waits for the TLAS build; keep a dropped BLAS's micromaps until in-flight frames finish; skip present without a swap chain | `245a2adf6`, `786cbf8df`, `7f31c7cc3` | Generic device-lost and crash paths |
| x86 bridge client links with current MSVC (`dllexport` of `remix_c.h` entry points); packman link warnings under `/WX` | `1c56e9a60`, `e87fbbcba`, `72488fc2f` | Build fixes |

### Tier 2: features with a clear generic use
| PR | Commits | Note |
|---|---|---|
| Particle local lights: `rtx.particles.localLightGain`, range, max count | `e04562d5d` (+ range part of `314a53ce5`) | Headlights in rain, torches in smoke. Off by default |
| Particle motion vectors and anti-ghost mask for non-emissive particles | `0c3e0510a`, `3b8a82550` | Any falling precipitation under RR |
| `rtx.particles.minScreenSize` and `maxScreenCoverage` | parts of `68e0d83ab`, `c85b1ffa9` | Render-pixel cull under DLSS; overdraw near the lens |
| API draws merge their own material into USD replacements (as D3D9's `mergeLegacyMaterial`) | `175749913` | A mod can replace an API mesh's material without losing the host's textures |
| Mesh materials: a `material:binding` on a preserved `mesh_<hash>` replaces that draw's material | `175bdf51f`, `7a2ed40f6` | Per-mesh materials where textures are shared (atlases). `documentation/MeshMaterials.md` |
| Inactive window keeps the ray-traced world | part of `314a53ce5` | Alt-tab no longer drops the scene |
| Alt+X texture grid thumbnails scoped by texture hash | `477422e2e` | Generic UI fix |

### Tier 3: needs an API design first
The behaviour is generic, but our transport is a stop-gap and should not go upstream as is.
- **Particle flags in `collisionMode` bits** (`0x80` shelter, `0x40` kill box, `0x20` camera-relative trails;
  `7e059fe11`, `00866b896`, `986a2b209`): propose real fields in `InstanceInfoParticleSystemEXT` (or a chained
  `ParticleSystemExtrasEXT`) plus a **kill-volume** struct (oriented box, per frame) instead of a game-value string.
- **Capability query**: a `GetCapabilities`/feature-string call instead of polling a game value.
- **Bridge forwards `UpdateLightDefinition`**: the bridge-side handle growth of section 3 disappears for any host that
  moves lights.
- **Accept a light create after a same-frame destroy** (or document the rule in `RemixApi.md`): it costs every new
  integrator an afternoon.
- **Emissive material by texture hash through the API** (a runtime material override keyed on a D3D9 texture hash
  plus a mask texture): would replace our twin-texture + generated-mod route for world emissives and its reload hitches.
- **Lamps & Materials tab** in Alt+X (lights attached to picked game meshes, texture-anchored lamps, saved as a
  hot-reloading mod; `5a8e9ef0b`, `d1c9a9f81`, `e089931a7`; `documentation/LightAuthoring.md`): useful to any
  remaster, but it is a larger UI feature and Kim should decide whether it fits.
- **Wetness options** (`rtx.wetNoPuddleTextures`, road mask, `rtx.wet.raindropDensity`, `mapUpdateDistance`) only make
  sense upstream together with the wetness port itself, which is xoxor4d's work; offer them to whoever upstreams it.

### Not for upstream
RBR's welcome-window buttons, anything keyed on RBR texture hashes, and our sweep tooling.

---

## 10. Checklist for another game

1. **Find the game's spawn function, not its draw call.** One hook at particle creation covered every effect; a dummy
   slot cuts the game's cost without patching its logic.
2. **Keep the game's coordinates far from float trouble:** solve world matrices from the well-conditioned columns in
   double; verify an exact matrix forward in NDC, never backward through an inverse.
3. **Never let the host write the user's config**: host layers below `rtx.conf` / `user.conf`.
4. **One descriptor = one particle system.** Build it once, cache it, quantise anything continuous.
5. **Spawn density follows triangle count, not area.** Shape it with the mesh.
6. **Lights:** same hash = same light (RTXDI history); never destroy and re-create in one frame; real units (cd, K, lm)
   calibrated against the sky's sun.
7. **Anything that must change in the same frame goes through API meshes**; generated USD mods are for what can wait.
8. **Let the game tell you what things are:** its lamp signal (pulsed if needed), its physics surface ids, its weather
   object. Names and hashes come last.
9. **Profile CPU and GPU separately** before deciding where the time goes.
