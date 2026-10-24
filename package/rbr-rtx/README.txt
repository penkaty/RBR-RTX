RBR-RTX - RTX Remix path tracing for Richard Burns Rally (RSF), as a removable add-on
Full documentation, changelog and updates: https://github.com/penkaty/RBR-RTX

REQUIREMENTS (read this first)
  - NVIDIA RTX GPU with driver 572.18 OR NEWER (the minimum RTX Remix 1.5.2
    itself checks for). Older drivers are not supported.
  - RSF launcher: Graphics mode = Normal (Vulkan/dgVoodoo/VR modes use their own d3d9.dll).
  - ReShade and the RBR Lights Plugin must NOT be installed. With either one in the game, Remix does not run
    properly. Remove them first (for ReShade also its Vulkan layer and shader folders). RBR-RTX has its own
    headlights (key L).

What it adds to the game folder (nothing else is changed):
  d3d9.dll        RBR-RTX proxy; loads RTX Remix from rbr-rtx\remix\ and feeds it the camera, objects and sun
  rbr-rtx\        Remix runtime, configs, logs, these scripts; Remix mods in rbr-rtx\rtx-remix\mods, Alt+X
                  captures in rbr-rtx\rtx-remix\captures (the folder name the RTX Remix Toolkit expects)
                  rtx-remix\mods\rbr_rtx_materials = RBR-RTX's own hand-tuned materials (so far: the mirrors
                  of the BMW M3 E30 GrpA reflect for real, the exhaust backfire flame glows)

Install
  1. Remove ReShade and the RBR Lights Plugin if you have them. RSF launcher: Graphics mode = Normal.
  2. From the unpacked RBR-RTX folder, in PowerShell:  .\rbr-rtx\install.ps1 -Game "<your RBR folder>"
     (or copy d3d9.dll and rbr-rtx\ into the game folder by hand).
  3. Start the game as usual. The first start compiles shaders and takes longer. Alt+X opens the Remix menu.
  Updating from 0.2 or older: unpack over the old files, then delete user.conf in the RBR folder (and an edited
  rbr-rtx\config\keys.ini): old saved settings override the new defaults.

Recommended game settings (RSF launcher / in-game options)
  Cubic env maps: off, Render car shadows: off. RBR-RTX path traces car reflections and shadows; the game's
  reflection cube map pass and blob shadow only cost time (RBR-RTX skips the cube map draws anyway).
  Particles: off (Remix uses its own particle renderer).
  Normal or windowed "Fullscreen mode" in the launcher is recommended (stage sweep screenshots are black in
  exclusive fullscreen).

Weather: RBR's sky type (crisp, hazy, rain, snow, fog), cloud cover, time of day and the stage's country set the
  Remix haze, sky light and sun automatically (rbr-rtx\config\atmosphere.ini; enable = 0 there turns it off).

HELP WANTED: SKY, SUN AND WEATHER
  So far I have focused on much needed performance fixes and on cutting the unused vertex calls and draws. Still
  ahead is an ungodly amount of work that can only be done in a reasonable time with the help of people willing to
  test it. RBR bakes each stage's light into the scenery, so the path-traced sun has to be made to agree with the
  bake, stage by stage and time of day by time of day. Nothing in the game files tells us when it does not, so only a
  player looking at the stage can tell. From easiest to hardest:

  1. Sky textures that block the sun (easiest). Some stages use a skybox that literally blocks the sun. Pick that
     texture with the Remix dev tools (Alt+X) and give it the "sky texture" flag. Send me the rtx.skyBoxTextures
     line from user.conf in the RBR folder once you have saved it (RabocsiRing 2021 was fixed this way).

  2. Sun position. Stages differ in how they bake their light. RBR-RTX reads the sun direction left in the stage
     files and places the Remix sun to match. Some stages lack it entirely or use a straight-up 90 degree sun. For
     those the sun position is automated from the chosen weather, an approximate time of day and the stage's
     location. This is where manual input is needed: every stage and time of day you set right is fixed for all
     players in the next release. To set the sun position in under 20 seconds:
       a. Press / (debug sun).
       b. Hold [ and ] to turn it around and ; and ' to lower / raise it until the light matches the baked shadows.
       c. Press \ (Windows beep). That sun is kept for this stage and time of day, now and next time you drive it.
       d. Send rbr-rtx\config\sun-saved.ini to https://github.com/penkaty/RBR-RTX/issues. Delete a line there to go
          back to the stage's own sun. Uninstall removes it: copy it out first.

  3. Sun intensity and the weather look (hardest). Even when the sun position fits the bake and the theme of the
     stage, its intensity has to be regulated properly. There are two ways: dim and brighten the sun, which works
     today with Alt+N / Alt+M (10 % per press; the log shows the value to keep), and enlarge it so shadows get
     softer, which is not in yet. The goal is a full volumetric atmospheric weather system that matches the
     original raster look (heavy fog, distant haze, overcast, clear day). In many cases the weather name alone will
     not give enough information, so manual input will be needed here too.

Keys (in game; change them in rbr-rtx\config\keys.ini if they clash with your controls)
  Only while the game window is active and Ctrl/Alt are not held. Alt+X = RTX Remix menu.
  You need none of them to drive: the defaults are the best-looking, fastest setup. The four marked * are the ones
  worth remembering.

* Alt+K    stage sweep: press it on the stage where the problem is, keep the car and camera still and wait about
           25 seconds until the beep (screenshots taken apart layer by layer, Remix debug views, every texture with
           its Remix hash, a Remix scene capture). Result: rbr-rtx\logs\sweep-<date-time>\; zip that folder and send it.
           Run the game windowed or borderless, or the screenshots come out black. Alt+K again aborts.
* 0        ray tracing on/off: the original raster image of the same scene
  6        sky: Remix sky (default) <-> no sky
  7        dust and tyre smoke (Remix particles) on/off
  8        record everything drawn in the next frame into rbr-rtx\logs (test builds with diagnostics.txt)
* 9        mark a problem while driving: press it the moment something goes wrong (stutter, wrong picture,
           anything). Beep, screenshot, the frame times around that moment. Every stage is recorded anyway (see
           "Reporting a problem"); a mark names that stage's zip -marked.
  I        cockpit windscreen glass on (default) / off
  J        foliage: fuller cut-outs (default) -> with soft-edge pass -> game original
  K        faster scenery: default (static scenery, trees/grass/flags do not sway) -> off (old, slower) -> static
           only (everything sways)
* L        headlights: OFF -> ON (two lights at the car's own headlights) -> BRIGHTS (two more, tight high beams
           that reach further; four lights) -> OFF.
           They switch on by themselves when the sun is lower than 12 degrees (and off again above it) until you
           press L on that stage. Brightness, beam width, automatic switch:
           rbr-rtx\config\headlights.ini. Night weathers (the stage's weather says "night") get a sun between
           -5 and +5 degrees (atmosphere.ini night_sun), so the headlights come on there by themselves.
           Tail lights (red, at the car's own rear lamps) are on with the headlights; brake lights light up
           whenever you brake, headlights on or off. A backfire flashes a short orange light at the exhaust.
           In reverse gear the right rear lamp turns white (reverse_light).
  O        RBR-RTX camera/objects/sun layer on/off (path tracing itself stays on)
  P        secondary ray bounces 2 (default) <-> 1: 1 is faster, but on some heavily forested stages the first two
           rows of trees then get a black background behind them. Press P again if you see it.
  U        distance culling on/off (off by default): far geometry outside the view left out of path tracing
           (rbr-rtx\config\culling.ini)
  /        debug sun: manual sun on/off (starts at the game's sun); while on, hold ; / ' to lower / raise it and
           [ / ] to turn it around. Below the horizon it becomes twilight and night (down to -18 degrees).
  \        keep the sun now shown for this stage and time of day (see "Help wanted: sky, sun and weather")
  Alt+7    rain and snow: Remix particles (default) <-> the game's own
  Alt+L    Remix sun on/off
  Alt+M / Alt+N  sun dimmer / brighter (10 % per press; the log shows the value to keep)
  Alt+O    experimental: foliage cards cut to the leaf outline
  Alt+P    performance test (rbr-rtx\config\experiments.ini): keep the view still ~1 minute; frame times per
           setting in rbr-rtx\logs\rbrrtx_comp.log
  Alt+Y    ground second layer on/off (off by default): stages that mix two ground textures (sea and coast, cliffs,
           road markings) get the second one too. Turn it on before loading a stage. Known issue: road markings
           and damp patches can show as black, mirror-like holes, and it uses a lot of memory on big stages.

Particles, rain, snow, windscreen drops and fog
  All tunable in rbr-rtx\config\particles.ini (dust, smoke, rain, snow, windscreen) and atmosphere.ini (haze, fog).
  Open them in Notepad and save while driving: the change applies within a second. Every key is explained in the
  file itself. Windscreen drops off the glass on a car: set show_plane = 1 in [windscreen], then tune that car's
  [windscreen.<car>] section (up / back / right / wider / longer, metres) and send us the numbers.

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
  https://github.com/penkaty/RBR-RTX/issues (a Discord link is coming).
  Unusual weather (snow, fog, rain, night) and rarely driven stages can show wrong surfaces because Remix knows
  textures by hash and not all are sorted yet: a sweep from exactly there gives us the hashes to fix it.
  Dynamic lights so far: headlights, high beams, tail and brake lights, the backfire flash (all built through
  the Remix API: RBR draws its own lights as textures). Other cars' lights are not done yet.

Black picture with path tracing on, but fine when you press 0? RBR-RTX cannot recognise the game's shaders. Another
  mod changes them (ReShade, a shader pack, a graphics plugin). Remove it and check that VertexShaders\ holds the
  game's own files. rbr-rtx\logs\rbrrtx_comp.log then says "WARNING: game shaders not recognised".

Test builds: rbr-rtx\diagnostics.txt switches on extra logging (RBR-RTX log, 8 frame dump, crash dumps) into
  rbr-rtx\logs. To report a problem, zip rbr-rtx\logs and send it. Delete diagnostics.txt to stop it.

Uninstall
  Run rbr-rtx\uninstall.cmd. It removes d3d9.dll (only if it is still the RBR-RTX one), the files Remix
  created (user.conf with your Alt+X settings, logs) and rbr-rtx\, including rtx-remix\mods and \captures in it
  (copy out anything you want to keep). Files that were there before stay.

Requires an RTX GPU. RTX Remix is NVIDIA's (licence in rbr-rtx\remix\LICENSE.txt).
