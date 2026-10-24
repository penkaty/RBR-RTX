# Troubleshooting

## Report a problem
**Whatever the problem, run the Alt+K sweep first.** It is the main source of information for fixing things.

1. Performance or a crash? Send the zip of that stage from `rbr-rtx\logs\reports`. Every stage is recorded on its own;
   press **9** when it happens and that zip is kept and named `-marked`.
2. For anything that looks wrong: stop the car on the spot and leave the camera as it is.
3. Press **Alt+K** and do not touch anything for about 25 seconds. Strange views are normal. At the end the sweep
   presses **Pause** itself for a Remix scene capture, so keep Pause free.
4. A **beep** means it is done. Quit the game and zip `rbr-rtx\logs` (plus the newest `capture_*.usd` from
   `rbr-rtx\rtx-remix\captures` if you can).
5. Send it with stage, car, weather and what looks wrong: [GitHub issues](https://github.com/penkaty/RBR-RTX/issues)
   (a Discord link is coming).

The logs contain your Windows user name and GPU model, nothing else personal. Nothing is sent automatically.

## The game will not start, crashes at start, or looks broken everywhere
- **ReShade or the RBR Lights Plugin installed?** Neither works together with Remix. Remove them (see [Installation](installation.md)).
- **Driver older than 572.18?** Remix refuses to start and shows a driver error. Update to 572.18 or later.
- **RSF Graphics mode is not Normal?** Other modes bring their own `d3d9.dll`.
- **Updated from an older version?** Delete `user.conf` in the RBR folder.

## Black picture with path tracing on, normal picture when you press 0
RBR-RTX cannot recognise the game's shaders, so Remix gets no camera. Usually another mod changes them: ReShade, a
shader pack or a graphics plugin. Remove it and check that `VertexShaders\` holds the game's own files.
`rbr-rtx\logs\rbrrtx_comp.log` then says "WARNING: game shaders not recognised".

## The picture or settings differ from the defaults
Settings saved with **Save** in the Alt+X menu go to `user.conf` in the RBR folder and override the shipped defaults,
also after updates. Delete `user.conf` to return to the defaults.

## Low FPS
- Stage reports and the log show where the time goes; send the report zip.
- 1440p and above cost much more than 1080p: 5120x1440 is 3.6 times the pixels. Lower the DLSS quality mode in Alt+X.
- Try **U** (distance culling) on stages with long views, and **K** to compare scenery modes.
- Run **Alt+P** with the view still and send the log lines it writes.

## High FPS counter but it feels choppy
The counter counts the frames the game sends, not the frames that reach your screen. Try fullscreen instead of
windowed and close other overlays.

## Wrong surfaces in unusual weather or on rarely driven stages
Snow, fog, rain, night and custom stages can show glowing, see-through or missing surfaces. Remix knows textures by
hash and not all are sorted yet. A sweep from exactly there gives us the hashes to fix it.

More: [Known issues](known-issues.md).
