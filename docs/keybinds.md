# Keybinds

Keys work only while the game window is active and Ctrl/Alt are not held (keys written as Alt+x need Alt). **Alt+X**
is Remix's own menu. You need none of these keys to drive: the defaults are the best-looking, fastest setup. The four
in bold are the ones worth remembering.

Change any key in `rbr-rtx\config\keys.ini` (restart the game); write `none` to switch one off.

| Key | Action |
|---|---|
| **Alt+K** | **Stage sweep for bug reports:** stop the car, leave the camera, wait about 25 seconds for the beep. Output in `rbr-rtx\logs\sweep-<date-time>\`. It presses Pause itself at the end for a Remix scene capture, so keep Pause free. Pressing it again aborts. |
| **0** | **Ray tracing on/off:** the original picture of the same scene, to compare |
| 6 | Sky: Remix sky or no sky |
| 7 | Dust and tyre smoke (Remix particles) on/off |
| 8 | Record everything drawn in the next frame (test builds with `diagnostics.txt`) |
| **9** | **Mark a problem while driving:** beep, screenshot, frame times around that moment. The stage's report zip is kept and named `-marked`. |
| I | Cockpit windscreen glass on/off |
| J | Foliage: fuller cut-outs, with soft-edge pass, game original |
| K | Faster scenery: default (static scenery, no sway), old slower mode, static only (everything sways) |
| **L** | **Headlights:** off, two lamps, brights (four lamps). They also switch on by themselves when the sun is low (`headlights.ini`). |
| O | RBR-RTX camera/objects/sun layer on/off (path tracing itself stays on) |
| P | Secondary ray bounces 2 (default) / 1. **1 is faster**, but on some heavily forested stages the first two rows of trees can get a black background behind them. Switch back with P if you see it. |
| U | Distance culling on/off: far geometry outside the view is left out of path tracing (`culling.ini`) |
| `/` | Debug sun on/off. It starts at the game's sun. |
| `[` / `]` | Turn the debug sun left / right (hold) |
| `;` / `'` | Lower / raise the debug sun (hold). Below the horizon it becomes twilight and night. |
| `\` | Keep the sun now shown for this stage and time of day (beep), saved to `rbr-rtx\config\sun-saved.ini` |
| Alt+7 | Rain and snow: Remix particles or the game's own, to compare |
| Alt+L | Remix sun on/off |
| Alt+M / Alt+N | Sun dimmer / brighter, 10 % per press |
| Alt+O | Experimental: foliage cards cut to the leaf outline |
| Alt+P | Performance test: keep the view still about a minute; frame time per setting goes to `rbr-rtx\logs\rbrrtx_comp.log` |
| Alt+Y | Ground second layer (sea, cliffs, road markings) on/off, off by default. Turn it on before loading a stage. |

Two more actions exist without a key by default (`opaque_alpha`, `foliage_normals`); give them one in `keys.ini` to
compare.
