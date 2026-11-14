RBR-RTX 0.4.0 - path tracing for Richard Burns Rally (RSF) through Remix Plus, as a removable add-on
Full documentation, changelog and updates: https://github.com/penkaty/RBR-RTX

REQUIREMENTS (read this first)
  - Windows 10 or 11, NVIDIA RTX GPU with driver 572.18 OR NEWER. Older drivers are not supported.
    About 60 fps on a clear day, 35 to 50 fps at night in rain (RTX 2080 Ti, 1080p, default settings).
  - RSF launcher: Graphics mode = Normal (Vulkan/dgVoodoo/VR modes use their own d3d9.dll).
  - ReShade and the RBR Lights Plugin must NOT be installed. With either one in the game, Remix does not run
    properly. Remove them first (for ReShade also its Vulkan layer and shader folders). RBR-RTX has its own
    headlights (key L).

What it adds to the game folder (no game file is changed):
  d3d9.dll        RBR-RTX; loads the Remix Plus runtime from rbr-rtx\remix\ and feeds it the camera, objects,
                  sun, weather, lamps and particles
  rbr-rtx\        runtime, settings (config\), logs, these scripts; Remix mods in rbr-rtx\rtx-remix\mods
  rtx.conf, user.conf   created in the game folder at the first start, from rbr-rtx\config. They are yours from
                  then on: Alt+X saves go there and no update overwrites them.

Install
  1. Remove ReShade and the RBR Lights Plugin if you have them. RSF launcher: Graphics mode = Normal.
  2. Unpack into the game folder, next to RichardBurnsRally_SSE.exe (d3d9.dll and rbr-rtx\ land there).
  3. Open the rbr-rtx folder in the game folder and double-click install.bat (or run install.ps1 in PowerShell).
     Unpacked somewhere else? From there in PowerShell:  .\rbr-rtx\install.ps1 -Game "<your RBR folder>"
  4. Start the game as usual. The first start compiles shaders and takes longer.
  Updating from an older version: unpack over the old files, then delete rtx.conf and user.conf in the RBR folder
  (and an edited rbr-rtx\config\keys.ini): old saved settings override the new defaults. Copy your own
  rbr-rtx\config\menu.ini and sun-saved.ini out first if you want to keep them.

Game settings (RSF launcher / in-game options)
  Particles: on (any quality) if you want raindrops on the windscreen; dust, smoke, rain and snow work either way.
  Cubic env maps, Render car shadows: either setting; RBR-RTX skips both passes and path traces the real thing.
  Windowed or borderless is recommended (stage sweep screenshots are black in exclusive fullscreen).

The menus
  F3      RBR-RTX's own menu. Stop the car first: keyboard and mouse go to the menu while it is open.
            Info         stage, weather, time, every key
            Wetness      wet-road sliders: puddles, ripples, raindrops, darkening
            Textures     aim the middle of the screen at a surface and tag it Terrain / Decal / Water / Ignore
            Car paint    how shiny and how metallic car paint is
            Atmosphere, Per stage   weather and sun rules, for all stages or the one you are on
            Headlights   lamp brightness, colour, beam, automatic switching
            Lamps        aim at a street lamp, floodlight or window and make it glow and cast light; free lights
            Debug        switch single parts of the mod on and off to compare
            Save         writes your values to rbr-rtx\config\menu.ini
  Alt+X   Remix's own menu (graphics settings, texture flags, weather presets under Sky > Weather)

Keys (change them in rbr-rtx\config\keys.ini if they clash with your controls)
  Only while the game window is active. Letter keys are all Alt+<letter> except L. You need none of them to drive.
  L        headlights: OFF -> ON (two lamps) -> BRIGHTS -> OFF. They come on by themselves below 12 degrees of sun
           and in rain, snow or fog, until you press L on that stage. Settings: rbr-rtx\config\headlights.ini.
  0        ray tracing on/off: the original picture of the same scene
  /        debug sun on/off; while on, hold [ ] to turn it and ; ' to lower / raise it
  \        keep the sun now shown for this stage and time of day (rbr-rtx\config\sun-saved.ini)
  6        sky on/off                        7        dust and tyre smoke on/off
  Alt+7    rain and snow: Remix particles <-> the game's own
  Alt+0    ray bounces: default -> large -> 1 secondary bounce (faster; can blacken tree backgrounds)
  Alt+Y    ground second layer: decal (default) -> bake (experiment) -> off, applied at the next stage load
  The rest (comparison switches) are listed in the F3 Info page and on the website.

Reporting a problem (please do!) - three keys, nothing is sent anywhere
  9        mark a problem while driving: beep, screenshot, the frame times around that moment. Every stage is
           recorded into rbr-rtx\logs\reports\report-<date-time>-<stage>.zip (the last 20 are kept); a mark names
           that zip -marked and it is never deleted. Works in the main menu too. A stage left by a crash is zipped
           at the next start (-unfinished). Off: rbr-rtx\config\report.ini, enabled = 0.
  8        frame dump: aim the camera at what is wrong and press it. Everything drawn in the next frame goes to
           rbr-rtx\logs\rbrrtx_comp.log.
  Alt+K    stage sweep: stop the car, leave the camera, wait for the beep (about a minute; strange views are
           normal, and it presses Pause itself for a Remix scene capture). Result: rbr-rtx\logs\sweep-<date-time>\.
           Alt+K again aborts.
  Which one: a stutter, slow stage or crash -> the stage zip (9). One wrong thing on screen -> 9, then 8. The
  whole picture wrong or slow -> Alt+K. Then zip rbr-rtx\logs and send it with stage, car, weather and what
  looks wrong: https://github.com/penkaty/RBR-RTX/issues
  While you just drive, only the small stage report is kept. 8 or Alt+K switch the detailed log on until you quit.
  For a full log from the start of the game, create an empty file rbr-rtx\diagnostics.txt (delete it afterwards).
  The logs contain your Windows user name in file paths and your GPU model, nothing else personal.

Black picture with path tracing on, but fine when you press 0? RBR-RTX cannot recognise the game's shaders. Another
  mod changes them (ReShade, a shader pack, a graphics plugin). Remove it and check that VertexShaders\ holds the
  game's own files.

Uninstall
  Double-click rbr-rtx\uninstall.cmd. It removes d3d9.dll (only if it is still the RBR-RTX one), the files Remix created
  (rtx.conf, user.conf, logs) and rbr-rtx\, including rtx-remix\mods and \captures in it (copy out anything you
  want to keep, e.g. config\menu.ini). Files that were there before stay.

Credits: Remix Plus and the NUMOS sky by Kim2091; RTX Remix by NVIDIA; wetness and card normals by xoxor4d. Full
list on the website. Runtime licences: rbr-rtx\remix\LICENSE.txt.

Support the project (it stays free): https://www.patreon.com/cw/penkaty or https://ko-fi.com/penkaty

RBR-RTX is free. It may be used, changed and shared, but never sold: PolyForm Noncommercial 1.0.0, see rbr-rtx\LICENSE.
If you paid for this mod, you were scammed. Third-party parts keep their own licences: rbr-rtx\THIRD-PARTY.md.
