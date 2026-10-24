# Known issues

Last updated for 0.3. Found something that is not here? Send an Alt+K sweep, see [Troubleshooting](troubleshooting.md).

| Issue | Details | Workaround |
|---|---|---|
| **Bob's Track Builder stages** (e.g. Ruuhimaki) | The stage is missing or floats in the sky under path tracing. Three attempts gave the same broken picture. | Press **0** to switch ray tracing off there |
| **Unusual weather, rare stages** | Snow, fog, rain, night and custom stages can show glowing, see-through or missing surfaces (texture hashes not sorted yet) | Send a sweep from that spot |
| **Key P set to 1 bounce** | Faster, but on some heavily forested stages the first two rows of trees get a black background behind them | Press **P** again (2 bounces, the default) |
| **Some RSF stages render nothing** (e.g. Toivonvaara, Toikkala, Antaramanana) | They draw with shaders RBR-RTX does not know yet | Press **0** there; send a sweep |
| **Windscreen drops off the glass on a car** | Most cars sit right from the car's own files; a few may not | Tune it per car, see [Particles](particles-and-weather.md) |
| **Hang on first load of a new stage** | Very rare, not reproduced since | Restart the game |
| **Modern RSF cars** | Some use shaders RBR-RTX does not handle yet | Send a sweep with that car |
| **Ground with two textures** (sea, coast, cliffs, road markings) | Only the first texture shows by default | **Alt+Y** draws the second; it can turn road markings into black holes and holds up to about 1 GB of memory |
| **Cubic env maps on** | The 3rd-person car looks wrong | Turn them off in the launcher |
| **Car lights** | Other cars' lights are not done; rear flares are untested | |
| **Shadows behind the car on forest stages** | Trees far behind the camera can drop out of path tracing when distance culling (**U**) is on | Leave **U** off (default) |
