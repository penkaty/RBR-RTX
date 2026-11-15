# Troubleshooting

## Report a problem

Three keys collect what we need. Nothing is sent automatically.

| The problem | Do this |
|---|---|
| **A stutter, a slow stage, a crash** | Press **9** the moment it happens (after a crash: nothing to press). Send that stage's zip from `rbr-rtx\logs\reports`. A marked one has `-marked` in its name, one left by a crash has `-unfinished`. |
| **One thing is wrong: a lamp, a texture, a missing part** | Aim the camera at it, press **9**, then **8**. Send the `-marked` zip and `rbr-rtx\logs\rbrrtx_comp.log`. |
| **The whole picture is wrong, or you want us to see why it is slow** | Run the sweep, below. |

### The Alt+K sweep
1. Stop the car on the spot and leave the camera as it is.
2. Press **Alt+K** and do not touch anything for about a minute. Strange views are normal: it switches parts off one by one. At the end it presses **Pause** itself for a Remix scene capture, so keep Pause free.
3. A **beep** means it is done. Quit the game and zip `rbr-rtx\logs` (plus the newest `capture_*.usd` from `rbr-rtx\rtx-remix\captures` if you can).

Run the game windowed or borderless for a sweep: the screenshots come out black in exclusive fullscreen.

### Sending it
Send it with stage, car, weather and what looks wrong: [GitHub issues](https://github.com/penkaty/RBR-RTX/issues).

The logs contain your Windows user name and GPU model, nothing else personal.

### What is logged, and when
A normal install writes no log file while you just drive. Pressing **8** or **Alt+K** switches the log on until you quit. Stage reports (one zip per stage, the last 20 kept) are always recorded so that key 9 has something to mark; `rbr-rtx\config\report.ini` switches them off.

If we ask you for a full log from the start of the game: create an empty file `rbr-rtx\diagnostics.txt`, play, send `rbr-rtx\logs`, and delete the file again.

## The game will not start, crashes at start, or looks broken everywhere
- **ReShade or the RBR Lights Plugin installed?** Neither works together with Remix. Remove them (see [Installation](installation.md)).
- **NVIDIA driver older than 610.47?** Remix refuses to start and shows a driver error. Update it.
- **AMD or Intel GPU?** Support is experimental. Choose FSR or XeSS in the Alt+X menu (DLSS is NVIDIA only) and send us a report.
- **RSF Graphics mode is not Normal?** Other modes bring their own `d3d9.dll`.
- **Updated from an older version?** Delete `rtx.conf` and `user.conf` in the RBR folder.

## Black picture with path tracing on, normal picture when you press 0
RBR-RTX cannot recognise the game's shaders, so Remix gets no camera. Usually another mod changes them: ReShade, a shader pack or a graphics plugin. Remove it and check that `VertexShaders\` holds the game's own files. The stage report then says "WARNING: game shaders not recognised".

## The picture or settings differ from the defaults
Settings saved in the Alt+X menu go to `rtx.conf` and `user.conf` in the RBR folder and stay there, also after updates. Delete both to return to the shipped defaults. Values saved in the F3 menu are in `rbr-rtx\config\menu.ini`.

## A setting in Alt+X jumps back
A few options are sent by the mod at every start because Remix resets them. They are listed in `rbr-rtx\config\start-options.ini`; delete a line there to get that option back in Alt+X.

## Low FPS
- 1440p and above cost much more than 1080p: 5120x1440 is 3.6 times the pixels. Lower the DLSS quality mode in Alt+X.
- Night in rain with street lamps is the heaviest case: the glow of every lamp in the wet air costs frame time.
- Run **Alt+K** on the slow spot and send it. The sweep measures what each part of the picture costs there.

## High FPS counter but it feels choppy
The counter counts the frames the game sends, not the frames that reach your screen. Try fullscreen instead of windowed and close other overlays.

## No raindrops on the windscreen
The drops are the game's own effect. Set **Particles** to on in the RSF launcher.

## Wrong surfaces in unusual weather or on rarely driven stages
Snow, fog, rain, night and custom stages can show glowing, see-through or missing surfaces. Remix knows textures by hash and not all are sorted yet. You can fix many yourself in the F3 **Textures** page, see [The F3 menu](f3-menu.md); a sweep from the spot lets us fix it for everyone.

More: [Known issues](known-issues.md).
