RBR-RTX - RTX Remix path tracing for Richard Burns Rally (RSF), as a removable add-on

What it adds to the game folder (nothing else is changed):
  d3d9.dll        RBR-RTX proxy; loads RTX Remix from rbr-rtx\remix\ and feeds it the camera, objects and sun
  rbr-rtx\        Remix runtime, configs, logs, these scripts; Remix mods in rbr-rtx\rtx-remix\mods, Alt+X
                  captures in rbr-rtx\rtx-remix\captures (the folder name the RTX Remix Toolkit expects)
                  rtx-remix\mods\rbr_rtx_materials = RBR-RTX's own hand-tuned materials (so far: the mirrors
                  of the BMW M3 E30 GrpA reflect for real)

Install
  1. RSF launcher: Graphics mode = Normal (Vulkan/dgVoodoo/VR modes use their own d3d9.dll).
  2. From the unpacked RBR-RTX folder, in PowerShell:  .\rbr-rtx\install.ps1 -Game "<your RBR folder>"
     (or copy d3d9.dll and rbr-rtx\ into the game folder by hand).
  3. Start the game as usual. Alt+X opens the Remix menu.

Recommended game settings (RSF launcher / in-game options)
  Cubic env maps: off/Low, Render car shadows: off. RBR-RTX path traces car reflections and shadows; the game's
  reflection cube map pass and blob shadow only cost time (RBR-RTX skips the cube map draws anyway).
  Particles: dust and smoke are path traced, so they cost FPS. Particles off (launcher) is the cheapest, as many
  players race anyway; Alt+Pause (below) switches dust live.
  (Particle quality Low currently shows no dust at all under RBR-RTX.)

Keys (in game)
  Scroll Lock   RBR-RTX camera/objects/sun layer on/off
  Shift+Scroll Lock   Remix sun only on/off
  Shift+Pause   cockpit windscreen on (default) / off
  Pause         sky: Remix sky (default) <-> no sky (for comparison)
  Alt+Scroll Lock   foliage: fuller cut-outs (default) -> with soft-edge pass -> game original (for comparison)
  Alt+Pause     gravel dust: all (default) -> none -> quarter -> half (quarter/half shorten the trail for now)
  Alt+Home      debug sun: manual sun on/off (starts at the game's sun); while on, hold Alt+Page Up / Page Down to
                raise / lower it and Alt+Insert / End to turn it around. Only the lighting moves, not the sky.
  Ctrl+Scroll Lock   test builds: record everything drawn in the next frame into rbr-rtx\logs (for bug reports)

Test builds: rbr-rtx\diagnostics.txt switches on extra logging (RBR-RTX log, Ctrl+Scroll Lock frame dump, crash
  dumps) into rbr-rtx\logs. To report a problem, zip rbr-rtx\logs and send it. Delete diagnostics.txt to stop it.

Uninstall
  Run rbr-rtx\uninstall.cmd. It removes d3d9.dll (only if it is still the RBR-RTX one), the files Remix
  created (user.conf with your Alt+X settings, logs) and rbr-rtx\, including rtx-remix\mods and \captures in it
  (copy out anything you want to keep). Files that were there before stay.

Requires an RTX GPU. RTX Remix is NVIDIA's (licence in rbr-rtx\remix\LICENSE.txt).
