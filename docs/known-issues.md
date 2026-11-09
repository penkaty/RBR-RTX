# Known issues

Last updated for 0.4.0. Found something that is not here? Send an Alt+K sweep, see [Troubleshooting](troubleshooting.md).

| Issue | Details | Workaround |
|---|---|---|
| **Bob's Track Builder stages** (e.g. Ruuhimaki) | The stage is missing or floats in the sky under path tracing. Three attempts gave the same broken picture. | Press **0** to switch ray tracing off there |
| **Unusual weather, rare stages** | Snow, fog, rain, night and custom stages can show glowing, see-through or missing surfaces (texture hashes not sorted yet) | Send a sweep from that spot |
| **Key P set to 1 bounce** | Faster, but on some heavily forested stages the first two rows of trees get a black background behind them | Press **P** again (2 bounces, the default) |
| **Some RSF stages render nothing** (e.g. Toivonvaara, Toikkala, Antaramanana) | They draw with shaders RBR-RTX does not know yet | Press **0** there; send a sweep |
| **Windscreen drops off the glass on a car** | Most cars sit right from the car's own files; a few may not | Tune it per car, see [Particles](particles-and-weather.md) |
| **Hang on first load of a new stage** | Very rare, not reproduced since | Restart the game |
| **Car not rendering properly** | Some use shaders RBR-RTX does not handle yet | Send a sweep with that car |
| **Incorrect texture blending/missing texture detail** (sea, coast, cliffs, road markings) | The decal layer blends the second texture by default | **Alt+Y** cycles decal / bake (experiment) / off, applied at the next stage load. Only the decal layer can turn road markings into black holes and holds up to about 1 GB of memory out of 4GB that RBR supports (32bit); set it BEFORE STAGE LOAD |
| **Key 0 in rain or snow** | Switching ray tracing back on can cost a short stutter while the weather particles restart | Press it once and give it a moment |
| **Glowing lamp lenses on some cars** | Some cars have no separate lit lens meshes (Ford Focus WRC): the beams and rear lights work, the lenses do not glow | None yet |
| **Main menu car lamps** | Some cars show different lamps in the menu than on a stage (Ford Escort, BMW, Skoda WRC) | Cosmetic, stages are right |
| **Ford Escort Mk2 low beams** | The beams start low and towards the centre of the car, from the model's lamp points | None yet |
| **Tail lamps smear behind the car after braking** | The lit lens can leave a short trail | Cosmetic |
| **Street lamps dim when looking at the ground** | Lamps placed in F3 can look darker with the camera pointing down | Cosmetic |
| **F3 picker hits hidden sprites** | A glow sprite ignored in **Alt+X** can still block the F3 crosshair until the game restarts | Restart the game |
| **Rain takes a while to return after restarting a replay** | The rain rebuilds slowly | Give it a few seconds |
| **Surfaces too shiny or flat without PBR** | Every texture without a PBR set shares one material | Being worked on |
| **Patches on the road after untagging Decal** | A second-layer texture needs its Decal flag | Tag it Decal again in **F3** or **Alt+X** |
| **Discord streaming costs a lot of frame rate** | Capture and encoding share the GPU with path tracing | Stream at a lower resolution and frame rate, and turn off your own stream preview |
| **Cubic env maps on** | The 3rd-person car looks wrong | Turn them off in the launcher |
| **Headlights not illuminated although they cast light** | Some cars do not apply the original headlight flare properly | Send an **Alt+K** sweep with that car |
| **Shadows behind the car on forest stages** | Trees far behind the camera can drop out of path tracing when distance culling (**U**) is on | Leave **U** off (default) |
