# Particles, rain, snow and weather

The game's own particles are stacks of flat sprites that path tracing draws slowly and lights wrongly. RBR-RTX replaces them: wherever the game fires a particle emitter, a real Remix particle system runs instead, lit and shadowed by the path tracer.

| Effect | What it does | Key |
|---|---|---|
| Dust and tyre smoke | Dust from each wheel in the colour of the ground under it; smoke on dry tarmac while sliding | **7** on/off |
| Snow dust | Smaller, whiter dust on snow surfaces | **7** |
| Other game effects | Splashes, engine steam and fire, debris: each at the place the game fires it | |
| Rain | Streaks falling in a disk around the camera, denser near you, turning with the view | **Alt+7**: Remix / the game's own |
| Snow | Flakes, the same way | **Alt+7** |
| Windscreen drops | The game's own drops, drawn as particles. Needs **Particles on** in the RSF launcher. | **Alt+I** hides the glass |

Rain and snow are lit by the sun, the sky and nearby lamps, so a headlight beam shows in them. They end where the stage's fog ends.

## Weather: clouds, fog, haze

The sky and the weather are drawn by **NUMOS**, see [How it works](how-it-works.md). RBR-RTX only names the stage's weather; a preset per RBR weather carries the look.

| RBR weather | Preset in `rbr-rtx\config\weather\` |
|---|---|
| Clear, hazy | `rbr-clear.conf`, `rbr-hazy.conf` |
| Part, light, heavy cloud | `rbr-partcloud.conf`, `rbr-lightcloud.conf`, `rbr-heavycloud.conf` |
| Light, heavy fog | `rbr-lightfog.conf`, `rbr-heavyfog.conf` |
| Light, heavy rain | `rbr-lightrain.conf`, `rbr-heavyrain.conf` |
| Light, heavy snow | `rbr-lightsnow.conf`, `rbr-heavysnow.conf` |

**To change how a weather looks:** drive a stage in that weather, open **Alt+X**, go to the developer menu, **Sky**, **Weather**, change the preset and save it. It applies to every stage with that weather.

**To change one stage's sun:** use the F3 **Per stage** page. It edits that stage's section in `atmosphere.ini`.

## Configuration files

All in `rbr-rtx\config\`. Open them with Notepad. **Save the file while driving and the change applies within a second.** Lines starting with `;` are explanations, and every key is explained in the file.

### `particles.ini`
| Section | Controls |
|---|---|
| `[dust]`, `[snow_dust]` | Wheel dust: amount per wheel, size, life, colour, how high it rises, drag |
| `[smoke]` | Tarmac tyre smoke |
| `[rain_light]`, `[rain_heavy]` | Rain for light / heavy weather: amount, drop length, fall speed, the area around the camera |
| `[snow_light]`, `[snow_heavy]` | Snow, same keys as rain |
| `[precipitation]` | From which weather strength the heavy presets are used |
| `[windscreen]` | RBR-RTX's own windscreen drops. Off by default (`enabled = 0`): they cost more than the game's. |
| `[killbox]` | Keeps rain and snow out of the car |

Tips:
- More particles cost frame time. If rain or snow stutters, lower the rate and the particle cap first.
- Rain in front of lamps is the costly case at night, not the number of drops.

### `atmosphere.ini`
Rules outside the weather presets: the night and dusk sun, the moon, the main menu's drifting weather, regions, times of day, and `[stage.<id>]` sections for single stages. The F3 **Atmosphere** and **Per stage** pages edit this file. The log line `atmosphere: stage ...` shows what a stage got.

### `headlights.ini`
Lamp brightness, colour, beam angles, when they switch on by themselves, tail, brake and reverse lights. Per-car sections named after the car's model file override any key for that car. The F3 **Headlights** page edits this file.

## Reporting a particle or weather problem
Press **9** at the moment it looks wrong, or **Alt+K** for a full sweep, and say which stage, car and weather it was. See [Troubleshooting](troubleshooting.md).
