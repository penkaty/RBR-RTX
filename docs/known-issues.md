# Known issues

Last updated for 0.4.0. Found something that is not here? See [Troubleshooting](troubleshooting.md) for how to report it.

## Stages and surfaces

| Issue | Details | Workaround |
|---|---|---|
| **Bob's Track Builder stages** (e.g. Ruuhimaki) | The stage is missing or floats in the sky under path tracing | Press **0** to switch ray tracing off there |
| **Some RSF stages render nothing** (e.g. Toivonvaara, Toikkala, Antaramanana) | They draw with shaders RBR-RTX does not know yet | Press **0** there; send a sweep |
| **Unusual weather, rare stages** | Snow, fog, rain, night and custom stages can show glowing, see-through or missing surfaces (texture hashes not sorted yet) | Tag it in the F3 **Textures** page, or send a sweep from that spot |
| **Surfaces of one kind all look alike** | Materials are per kind of surface (gravel, tarmac, rock...), not per texture | More detailed materials are planned after 0.4.0 |
| **Incorrect texture blending or missing detail** (sea, coast, cliffs, road markings) | The second ground texture is drawn as a thin layer | **Alt+Y** cycles decal / bake (experiment) / off, applied at the next stage load. The decal layer can turn road markings into black holes and holds up to about 1 GB of the 4 GB a 32-bit game has. |
| **Patches on the road after untagging Decal** | A second-layer texture needs its Decal flag | Tag it Decal again in **F3** or **Alt+X** |
| **A stage is darker or brighter than it should be** | The weather preset does not fit that stage | Tell us the stage and weather; presets can be tuned in **Alt+X** (Sky, Weather) |
| **Shadows do not match the baked ones** | The stage's sun direction is missing or wrong | [Fixing the sun](fixing-the-sun.md) |

## Cars and lamps

| Issue | Details | Workaround |
|---|---|---|
| **Lancia Delta HF 4WD tail lamps disappear** | Its rear lamp is four overlapping parts per side | None yet. A **9** mark and **8** while they are gone helps. |
| **Glowing lamp lenses on some cars** | Some cars have no separate lit lens meshes (Ford Focus WRC): the beams and rear lights work, the lenses do not glow | None yet |
| **Main menu car lamps** | Some cars show different lamps in the menu than on a stage | Cosmetic, stages are right |
| **Ford Escort Mk2 low beams** | The beams start low and towards the centre of the car, from the model's lamp points | None yet |
| **Tail lamps smear behind the car after braking** | The lit lens can leave a short trail | Cosmetic |
| **Car dirt does not show** | The dirt layer made body and tyres vanish, so it is off | None yet |
| **Car not rendering properly** | Some use shaders RBR-RTX does not handle yet | Send a sweep with that car |

## Weather and particles

| Issue | Details | Workaround |
|---|---|---|
| **No raindrops on the windscreen** | They are the game's own effect now | Set **Particles** on in the RSF launcher |
| **Night rain with many street lamps is slow** | The glow of each lamp in wet air is costly | Lower the DLSS quality mode; a faster path is being worked on with the NUMOS author |
| **Key 0 in rain or snow** | Switching ray tracing back on can cost a short stutter, and rarely a GPU hang | Press it once and give it a moment |
| **Rain takes a while to return after restarting a replay** | The rain rebuilds slowly | Give it a few seconds |

## Menu and tools

| Issue | Details | Workaround |
|---|---|---|
| **Street lamps dim when looking at the ground** | Lamps placed in F3 can look darker with the camera pointing down | Cosmetic |
| **F3 picker hits hidden sprites** | A glow sprite ignored in **Alt+X** can still block the F3 crosshair until the game restarts | Restart the game |
| **Unsaved F3 Wetness values are lost on exit** | Only Save writes them | Press **Save** |
| **Alt+0 set to 1 bounce** | Faster, but on some forest stages the first rows of trees get a black background | Press **Alt+0** until it is back at the default |
| **Shadows behind the car on forest stages** | Trees far behind the camera can drop out of path tracing when distance culling (**Alt+U**) is on | Leave it off (default) |
| **Stutter while the game window is not focused** | The runtime trims its work in the background | Keep the game focused |

## Other

| Issue | Details | Workaround |
|---|---|---|
| **Hang on first load of a new stage** | Very rare | Restart the game |
| **Discord streaming costs a lot of frame rate** | Capture and encoding share the GPU with path tracing | Stream at a lower resolution and frame rate, and turn off your own stream preview |
