# Keybinds

Keys work only while the game window is active. A plain key works only with Ctrl and Alt up; keys written as Alt+x need Alt. All letter keys are Alt+letter except **L**, so typing or driving binds on letters never toggle anything.

You need none of these keys to drive: the defaults are the best-looking, fastest setup.

Change any key in `rbr-rtx\config\keys.ini` and restart the game; write `none` to switch one off.

## The ones worth remembering

| Key | Action |
|---|---|
| **F3** | **In-game menu:** wetness, texture tags, lamps, car paint, headlights, atmosphere. See [The F3 menu](f3-menu.md). |
| **Alt+X** | Remix's own menu |
| **L** | **Headlights:** off, two lamps, brights. They come on by themselves below 12 degrees of sun and in rain, snow or fog; pressing L overrides that until the next stage (`headlights.ini`). |
| **0** | **Ray tracing on/off:** the original picture of the same scene, to compare |

## The three logging tools

| Key | Action |
|---|---|
| **9** | **Mark a problem while driving:** beep, screenshot, frame times around that moment. The stage's report zip in `rbr-rtx\logs\reports` is kept and named `-marked`. In the main menu it starts a short menu report. |
| **8** | **Frame dump:** everything drawn in the next frame goes to `rbr-rtx\logs\rbrrtx_comp.log`, with what the mod did to each draw and each car lamp part. Aim the camera at the problem first. |
| **Alt+K** | **Stage sweep:** stop the car, leave the camera, wait for the beep (about a minute). Output in `rbr-rtx\logs\sweep-<date-time>\`. It presses Pause itself at the end for a Remix scene capture, so keep Pause free. Pressing it again aborts. |

A release install writes no log file until you press **8** or **Alt+K**; from then on it logs until you quit. Stage reports for key 9 are always recorded (`report.ini` switches them off).

## Sun

| Key | Action |
|---|---|
| `/` | Debug sun on/off. It starts at the game's sun and switches off at the next stage. |
| `[` / `]` | Turn the debug sun left / right (hold) |
| `;` / `'` | Lower / raise the debug sun (hold). Below the horizon it becomes twilight and night. |
| `\` | Keep the sun now shown for this stage and time of day (beep), saved to `rbr-rtx\config\sun-saved.ini`. See [Fixing the sun](fixing-the-sun.md). |
| Alt+M / Alt+N | Sun dimmer / brighter, 10 % per press |
| Alt+L | Sun on/off |
| 6 | Sky on/off |

## Comparing and tuning

Press twice to go off and back.

| Key | Action |
|---|---|
| 7 | Dust and tyre smoke on/off |
| Alt+7 | Rain and snow: Remix particles or the game's own |
| Alt+I | Cockpit windscreen glass on/off |
| Alt+J | Foliage: fuller cut-outs (default), with soft-edge pass, game original |
| Alt+Comma | Scenery: default (static, no sway), old slower mode, static only (everything sways) |
| Alt+Period | Car drawn the fast way (default) or the old way |
| Alt+W | World through the Remix API: all, foliage only, off |
| Alt+O | RBR-RTX camera/objects/sun layer on/off (path tracing itself stays on) |
| Alt+0 | Ray bounces: default, large, 1 secondary bounce. **1 is faster**, but on some forest stages the first rows of trees get a black background. |
| Alt+U | Distance culling on/off (off by default, `culling.ini`) |
| Alt+Y | Ground second layer (sea, cliffs, road edges): decal (default), bake (experiment), off. A long beep means it applies at the next stage load. |
| Alt+9 | Experimental: foliage cards cut to the leaf outline |
| Alt+P | Performance test: keep the view still about a minute; frame time per setting goes to `rbr-rtx\logs\rbrrtx_comp.log` |
| Alt+PageUp | Old lamp marker: aim at a street lamp's head and press. The F3 **Lamps** page does the same with more control. |

Two more actions exist without a key by default (`opaque_alpha`, `foliage_normals`); give them one in `keys.ini` to compare.
