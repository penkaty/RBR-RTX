# Particles, rain, snow and weather

In 0.3 the game's own particles are switched off and RBR-RTX makes them with the Remix API instead: real particles
that the path tracer lights and shadows, for less cost than the game's stacked sprites.

| Effect | What it does | Key |
|---|---|---|
| Dust and tyre smoke | Dust from each wheel, coloured by the ground under it; tyre smoke on tarmac after a slide | **7** on/off |
| Rain | Streaks falling around the camera; light and heavy rain differ in drop size, speed and amount | **Alt+7**: Remix / the game's own |
| Snow | Flakes falling around the camera, with a car-sized gap so none start inside the car | **Alt+7** |
| Snow wake | Snow the car stirs up from its roof and sides at speed (outside views only) | |
| Windscreen drops | Water drops and melting snow on the windscreen, cockpit camera only. Above 40 km/h the air pushes them to the edges. | **I** hides the glass |

The amount follows the game's weather (light or heavy rain/snow). Fog and haze follow the stage's own weather settings.

## Configuration files

All in `rbr-rtx\config\`. Open them with Notepad. **Save the file while driving and the change applies within a
second**: no restart needed. Lines starting with `;` are explanations.

### `particles.ini`: dust, smoke, rain, snow, windscreen
| Section | Controls |
|---|---|
| `[dust]` | Wheel dust: how many per wheel, size, life, colour, how high it rises, drag, wind swirl |
| `[smoke]` | Tarmac tyre smoke |
| `[rain_light]`, `[rain_heavy]` | Rain for light / heavy weather: `per_wheel_rate` (drops per second), `start_size_*` (length), `rise_speed` (fall speed), `spawner_size` (the box around the camera they fall in) |
| `[snow_light]`, `[snow_heavy]` | Snow, same keys as rain |
| `[snow_wake]` | Snow stirred up by the car |
| `[windscreen]` | Drops on the glass: `drops_per_second`, `drop_size_min/max`, `flakes_per_second`, `melt_seconds_min/max`, `max_drops`, `drop_style` (1 glossy, 0 refracting) |
| `[windscreen.<car>]` | Per-car fine tuning of the windscreen position (below) |
| `[precipitation]` | From which weather strength the heavy presets are used |

Tips:
- More particles cost frame time. If rain or snow stutters, lower `per_wheel_rate` and `max_particles` first.
- `collision = 1` makes particles stop on surfaces they hit. It is off for snow because it caused stutter.

### Windscreen position per car
The drops sit on the game's own cockpit windscreen plane for each car, read from the car's files, so most cars need
nothing. The plane is tied to the driving camera with your field of view (NGPCarMenu's cockpit FOV override, else the
car's `[Cam_internal]` FOV). If a car's drops sit off the glass:
1. Set `show_plane = 1` in `[windscreen]`: a faint red plane shows where the drops go.
2. Find the car's name in `rbr-rtx\logs\rbrrtx_comp.log` (line `windscreen: <name>: plane from ...`).
3. Add or edit its section, in metres, and save while sitting in the cockpit:
   ```ini
   [windscreen.BMW_M3_E30]
   up = 0        ; along the glass, + up
   back = 0      ; + toward the driver
   right = 0     ; + right
   wider = 0     ; added to the width
   longer = 0    ; added to the length
   ```
4. Set `show_plane = 0` again when it fits. Please send us the numbers so the next version has them for everyone.

### `atmosphere.ini`: haze, fog, sky light
Each stage's weather (sky type, clouds, time of day) and country set Remix's volumetric haze, sky light and sun.
- Rain, snow and fog take their haze from the stage's own fog distances (`stage_fog`), so bad weather looks as closed
  in as in the original game. `density` makes any section thicker (2) or thinner (0.5).
- Sections apply in order: `[region.*]`, `[sky.<Sky>]`, `[clouds.<Clouds>]`, `[time.<Time>]`, `[sky.<Sky>.<Time>]`,
  `[stage.<id>]`, `[stage.<id>.<Sky>]`. The log line `atmosphere: stage ...` shows what a stage got.
- When a stage's weather has a description that names the weather or time of day ("Rainy night", "Noche Despejada"),
  that wins over the game's names.

### `headlights.ini`
Lamp brightness, beam angles, when they switch on by themselves, tail, brake and reverse lights. Per-car sections
named after the car's model file override any key for that car.

## Reporting a particle problem
Press **9** at the moment it looks wrong (the stage report keeps that moment), or **Alt+K** for a full sweep, and say
which stage, car and weather it was. See [Troubleshooting](troubleshooting.md).
