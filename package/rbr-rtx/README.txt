RBR-RTX - RTX Remix path tracing for Richard Burns Rally (RSF), as a removable add-on

What it adds to the game folder (nothing else is changed):
  d3d9.dll        RBR-RTX proxy; loads RTX Remix from rbr-rtx\remix\ and feeds it the camera, objects and sun
  rbr-rtx\        Remix runtime, configs, logs, these scripts; Remix mods in rbr-rtx\rtx-remix\mods, Alt+X
                  captures in rbr-rtx\rtx-remix\captures (the folder name the RTX Remix Toolkit expects)
                  rtx-remix\mods\rbr_rtx_materials = RBR-RTX's own hand-tuned materials (so far: the mirrors
                  of the BMW M3 E30 GrpA reflect for real, the exhaust backfire flame glows)

Install
  1. RSF launcher: Graphics mode = Normal (Vulkan/dgVoodoo/VR modes use their own d3d9.dll).
  2. From the unpacked RBR-RTX folder, in PowerShell:  .\rbr-rtx\install.ps1 -Game "<your RBR folder>"
     (or copy d3d9.dll and rbr-rtx\ into the game folder by hand).
  3. Start the game as usual. Alt+X opens the Remix menu.

Recommended game settings (RSF launcher / in-game options)
  Cubic env maps: off/Low, Render car shadows: off. RBR-RTX path traces car reflections and shadows; the game's
  reflection cube map pass and blob shadow only cost time (RBR-RTX skips the cube map draws anyway).
  Particles: off (Remix uses its own particle renderer; the game's own particles are never drawn).

Weather: RBR's sky type (crisp, hazy, rain, snow, fog), cloud cover, time of day and the stage's country set the
  Remix haze, sky light and sun automatically (rbr-rtx\config\atmosphere.ini; enable = 0 there turns it off).

Keys (in game; change them in rbr-rtx\config\keys.ini if they clash with your controls)
  Only while the game window is active and Ctrl/Alt are not held. Alt+X = RTX Remix menu.
  You need none of them to drive: the defaults are the best-looking, fastest setup. They are for comparing,
  tuning and reporting problems.

  Something looks wrong, or you want to report a problem:
  9        mark a problem while driving: press it the moment something goes wrong (stutter, wrong picture,
           anything). Beep, screenshot, the frame times around that moment. Every stage is recorded anyway (see
           "Reporting a problem"); a mark names that stage's zip -marked.
  Alt+K    stage sweep: press it on the stage where the problem is, keep the car and camera still and wait about
           25 seconds until the beep (screenshots taken apart layer by layer, Remix debug views, every texture with
           its Remix hash, a Remix scene capture). Result: rbr-rtx\logs\sweep-<date-time>\; zip that folder and send it.
           Run the game windowed or borderless, or the screenshots come out black. Alt+K again aborts.
  8        record everything drawn in the next frame into rbr-rtx\logs (test builds with diagnostics.txt)

  Is it RBR-RTX or Remix? Switch one part off and back (press again):
  0        ray tracing on/off: the original raster image of the same scene
  O        RBR-RTX camera/objects/sun layer on/off (path tracing itself stays on)
  Alt+L    Remix sun on/off
  6       sky: Remix sky (default) <-> no sky
  J        foliage: fuller cut-outs (default) -> with soft-edge pass -> game original
  I        cockpit windscreen on (default) / off
  K        faster scenery: default (static scenery, trees/grass/flags do not sway) -> off (old, slower) -> static
           only (everything sways)
  U        distance culling on/off: far geometry outside the view left out of path tracing (rbr-rtx\config\culling.ini)
  Alt+Y    ground second layer on/off (off by default): stages that mix two ground textures (sea and coast, cliffs,
           road markings) get the second one too. Turn it on before loading a stage. Known issue: road markings
           and damp patches can show as black, mirror-like holes, and it uses a lot of memory on big stages.

  Driving:
  L        headlights: OFF -> ON (two lights at the car's own headlights) -> BRIGHTS (two more, tight high beams
           that reach further; four lights) -> OFF.
           They switch on by themselves when the sun is lower than 10 degrees (and off again above it) until you
           press L on that stage. Brightness, beam width, automatic switch:
           rbr-rtx\config\headlights.ini. Night weathers (the stage's weather says "night") get a sun between
           -5 and +5 degrees (atmosphere.ini night_sun), so the headlights come on there by themselves.
           Tail lights (red, at the car's own rear lamps) are on with the headlights; brake lights light up
           whenever you brake, headlights on or off. A backfire flashes a short orange light at the exhaust.
           All in headlights.ini (tail_lights, brake_lights, backfire_light).

  Tuning:
  7       dust and tyre smoke (Remix particles) on/off
  Alt+N / Alt+M  sun brighter / dimmer (10 % per press; the log shows the value to keep)
  /        debug sun: manual sun on/off (starts at the game's sun); while on, hold ; / ' to lower / raise it and
           [ / ] to turn it around. Below the horizon it becomes twilight and night (down to -18 degrees).
  \        keep the sun now shown for this stage and time of day (see "Fixing the sun on a stage")
  P        secondary ray bounces 2 <-> 1 (rtx.secondaryRayMaxInteractions): 1 is faster, 2 reflects further
  Alt+P    performance test (rbr-rtx\config\experiments.ini): keep the view still ~1 minute; frame times per
           setting in rbr-rtx\logs\rbrrtx_comp.log

Reporting a problem (please do!)
  Stage reports: every stage you drive is recorded and zipped into rbr-rtx\logs\reports\report-<date-time>-<stage>.zip
  (frame rate and stutter per second, stage and car, hotkeys used, warnings, your PC: Windows, CPU, GPU and driver,
  Remix version, other plugins, your RBR-RTX settings). Nothing is sent anywhere: you choose what to share. The last
  20 are kept; zips you marked with 9 are never deleted. A stage left by a crash is zipped at the next start
  (-unfinished). Driving costs nothing measurable. Off: rbr-rtx\config\report.ini, enabled = 0.
  Performance or a crash: send the zip of that stage (the -marked one if you pressed 9).
  Something looks wrong:
  Whatever the problem: run the Alt+K sweep right where it shows (it presses Pause itself at the end for a Remix
  scene capture; keep Pause free), wait for the beep, quit, then zip rbr-rtx\logs (and the newest capture_*.usd
  from rbr-rtx\rtx-remix\captures if you can) and send it with stage, car, weather and what looks wrong:
  https://github.com/penkaty/RBR-RTX/issues or the RBR-RTX Discord (link coming soon).
  Unusual weather (snow, fog, rain, night) and rarely driven stages can show wrong surfaces because Remix knows
  textures by hash and not all are sorted yet: a sweep from exactly there gives us the hashes to fix it.
  Dynamic lights so far: headlights, high beams, tail and brake lights, the backfire flash (all built through
  the Remix API: RBR draws its own lights as textures). Other cars' lights and reversing lights are not done yet.

Fixing the sun on a stage
  Some stages need their sun position fixed: the Remix sun comes from another direction than the stage's baked
  shadows. Press / (debug sun), move it with [ ] ; ' until the light matches, then press \ (beep). That sun is kept
  for this stage and time of day, also after / and next time, in rbr-rtx\config\sun-saved.ini. Please send that
  file in so the fixes go in for everyone; delete a line there to go back. Uninstall removes it: copy it out first.

Test builds: rbr-rtx\diagnostics.txt switches on extra logging (RBR-RTX log, 8 frame dump, crash dumps) into
  rbr-rtx\logs. To report a problem, zip rbr-rtx\logs and send it. Delete diagnostics.txt to stop it.

Uninstall
  Run rbr-rtx\uninstall.cmd. It removes d3d9.dll (only if it is still the RBR-RTX one), the files Remix
  created (user.conf with your Alt+X settings, logs) and rbr-rtx\, including rtx-remix\mods and \captures in it
  (copy out anything you want to keep). Files that were there before stay.

Requires an RTX GPU. RTX Remix is NVIDIA's (licence in rbr-rtx\remix\LICENSE.txt).
