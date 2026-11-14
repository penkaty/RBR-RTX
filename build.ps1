<#
.SYNOPSIS
  Builds the proxy and assembles dist\RBR-RTX\: exactly the files RBR-RTX adds on top of a game folder.

.DESCRIPTION
  dist\RBR-RTX\
    d3d9.dll                 our proxy (src\d3d9-proxy)
    rbr-rtx\remix\           RTX Remix runtime (bridge d3d9.dll + .trex\), with our .trex\bridge.conf
    rbr-rtx\config\          dxvk.conf + rtx.conf (from package\)
    rbr-rtx\install.ps1, uninstall.ps1, uninstall.cmd, README.txt, files.txt (what uninstall removes)
  Remix is not stored in git; it is copied from the extracted NVIDIA release (-Remix), by default the newest
  folder in ..\remix\runtime\ of the dev checkout. NvRemixLauncher32.exe and d3d8to9.dll are left out (unused).

  -Diagnostics (test builds for testers): adds rbr-rtx\diagnostics.txt, which makes the proxy write its log,
  Ctrl+Scroll Lock frame dumps and crash dumps to rbr-rtx\logs. Release builds leave it out (nothing is logged).

.EXAMPLE
  .\build.ps1
  .\build.ps1 -Diagnostics
  .\build.ps1 -Pack                    # also dist\RBR-RTX-<Version>.7z and .zip
  .\build.ps1 -Remix D:\downloads\remix-1.5.2
#>
param([string]$Remix = '', [switch]$SkipProxy, [switch]$Diagnostics, [string]$Version = '0.4.0', [switch]$Pack)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$dist = Join-Path $here 'dist\RBR-RTX'
if (-not $Remix) { $Remix = Join-Path $here '..\remix\runtime\RemixPlus-mat\game folder' }   # 0.4.0: Remix Plus, the runtime the dev runs use (rbr-remix.ps1 -Version mat)
if (-not $Remix -or -not (Test-Path (Join-Path $Remix '.trex\NvRemixBridge.exe'))) {
  throw 'Runtime not found. Pass -Remix <runtime folder with d3d9.dll and .trex\>.'
}
if (-not $SkipProxy) { & (Join-Path $here 'src\d3d9-proxy\build.ps1') | Out-Null }
$proxy = Join-Path $here 'src\d3d9-proxy\bin\d3d9.dll'
if (-not (Test-Path $proxy)) { throw "Missing $proxy" }

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
$remixDst = Join-Path $dist 'rbr-rtx\remix'
New-Item -ItemType Directory -Force $remixDst | Out-Null
Copy-Item $proxy $dist
Get-ChildItem $Remix -Force | Where-Object { $_.Name -notin 'NvRemixLauncher32.exe', 'NvRemixLauncher32.pdb', 'd3d8to9.dll', 'ThirdPartyLicenses-d3d8to9.txt', '.trex_arm64', 'artifacts_readme.txt' } |
  Copy-Item -Destination $remixDst -Recurse -Force
Get-ChildItem (Join-Path $here 'package') -Force | Copy-Item -Destination $dist -Recurse -Force
# Backup copies the F3 menu and editors leave beside a config file are not part of the add-on.
Get-ChildItem $dist -Recurse -Force -File | Where-Object { $_.Name -like '*.f3bak' -or $_.Name -like '*.bak' -or $_.Name -like '*~' } |
  ForEach-Object { "left out (backup copy): $($_.Name)"; Remove-Item $_.FullName -Force }
Copy-Item (Join-Path $here 'LICENSE'), (Join-Path $here 'THIRD-PARTY.md') (Join-Path $dist 'rbr-rtx') -Force
# 0.4.0: the installed proxy reads rbr-rtx\config; the Remix Plus settings replace the 1.5.2 ones of the same name.
$cfg = Join-Path $dist 'rbr-rtx\config'
$plus = Join-Path $dist 'rbr-rtx\config-remixplus'
foreach ($n in 'pbr-material.conf', 'legacy-material-no-pbr.conf', 'material-classes.ini', 'bridge.conf') { Remove-Item (Join-Path $plus $n) -Force }   # dev only; the runtime brings its bridge.conf
Get-ChildItem $plus -Force | Copy-Item -Destination $cfg -Recurse -Force
Remove-Item $plus -Recurse -Force
$rtxConf = Join-Path $cfg 'rtx.conf'
$conf = [IO.File]::ReadAllText($rtxConf) -replace '(?m)^rtx\.weather\.presetDirectory[ \t]*=[^\r\n]*', 'rtx.weather.presetDirectory = rbr-rtx/config/weather'
if ($conf -notmatch '(?m)^rtx\.weather\.presetDirectory = rbr-rtx/config/weather') { throw 'rtx.conf has no weather preset folder' }
if ($conf -match '[A-Za-z]:[\\/](RichardRust|rbr-dev|Users)') { throw 'rtx.conf still has a path of this PC' }
[IO.File]::WriteAllText($rtxConf, $conf, (New-Object Text.UTF8Encoding $false))
# The release uses the user's own F3 lamps, texture tags and kept suns (user 2026-11-08): what dev runs load is what players get.
$devCfg = Join-Path $here '..\remix\config'
Copy-Item (Join-Path $devCfg 'menu.ini') (Join-Path $cfg 'menu.ini') -Force
Copy-Item (Join-Path $devCfg 'sun-saved-remixplus.ini') (Join-Path $cfg 'sun-saved.ini') -Force
if (-not (Test-Path (Join-Path $dist 'rbr-rtx\rtx-remix\mods\rbr_car_glass\mod.usda'))) { throw 'package has no rbr_car_glass mod: car glass would render without its materials' }
# Release = dev (user 2026-11-08; dev runs load both since then, remix\logs\<run>\LOADED.txt): the class materials
# (a_rbrrtx_classmat: one roughness / normal set per surface class, not generated PBR) and the user's own lamps and mesh
# materials from the Alt+X Lamps panel (authored_lights, taken from the dev mods folder like menu.ini) ship.
$modsDst = Join-Path $dist 'rbr-rtx\rtx-remix\mods'
if (-not (Test-Path (Join-Path $modsDst 'a_rbrrtx_classmat\mod.usda'))) { throw 'package has no a_rbrrtx_classmat mod: surfaces would fall back to the one legacy material' }
$tmp = Join-Path $modsDst 'a_rbrrtx_classmat\textures\_tmp'
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
$devLamps = Join-Path $here '..\remix\rtx-remix\mods\authored_lights'
foreach ($n in 'mod.usda', 'library.usda') {
  if (Test-Path (Join-Path $devLamps $n)) { Copy-Item (Join-Path $devLamps $n) (Join-Path $modsDst "authored_lights\$n") -Force }
}
# Bridge: no periodic performance breakdown in the bridge logs (dev runs keep it; the bridge's short 30 s line stays).
$bridgeConf = Join-Path $remixDst '.trex\bridge.conf'
if (Test-Path $bridgeConf) {
  $b = [IO.File]::ReadAllText($bridgeConf) -replace '(?m)^perfLogIntervalFrames[ \t]*=[^\r\n]*', 'perfLogIntervalFrames = 0'
  [IO.File]::WriteAllText($bridgeConf, $b, (New-Object Text.UTF8Encoding $false))
}
# Dev runs write into the runtime folder (crash dumps, logs, ReShade injected into NvRemixBridge.exe): never ship those.
$leftovers = '*.pdb', '*.dmp', '*.log', 'ReShade*', 'nrc_session_log.txt', 'user.conf', 'imgui.ini'
Get-ChildItem $remixDst -Recurse -Force -File | Where-Object { $n = $_.Name; $leftovers | Where-Object { $n -like $_ } } |
  ForEach-Object { "left out (dev-run leftover): $($_.FullName.Substring($remixDst.Length + 1))"; Remove-Item $_.FullName -Force }
Get-ChildItem $remixDst -Recurse -Force -Directory -Filter 'reshade-shaders' |
  ForEach-Object { "left out (dev-run leftover): $($_.FullName.Substring($remixDst.Length + 1))\"; Remove-Item $_.FullName -Recurse -Force }

if ($Diagnostics) {
  @('RBR-RTX diagnostics mode (test build).',
    'While this file exists, RBR-RTX writes its own log from the start of the game, plus crash dumps, to rbr-rtx\logs,',
    'next to RTX Remix''s logs. To report a problem: zip the rbr-rtx\logs folder and send it. The logs contain your',
    'Windows user name in file paths and your GPU/driver; nothing is sent anywhere automatically.',
    'Delete this file to stop the extra logging.') | Set-Content -Encoding UTF8 (Join-Path $dist 'rbr-rtx\diagnostics.txt')
}
$rev = (git -C $here describe --always --dirty 2>$null)
if ($Diagnostics) { $rev += ' (diagnostics)' }
@("RBR-RTX $Version ($rev), built $(Get-Date -Format 'yyyy-MM-dd HH:mm')", "RTX Remix runtime: $(Split-Path (Split-Path $Remix -Parent) -Leaf)") |
  Set-Content -Encoding UTF8 (Join-Path $dist 'rbr-rtx\version.txt')
# files.txt: every file the add-on brings, plus the proxy's hash so uninstall never deletes another d3d9.dll.
$base = (Resolve-Path $dist).Path.TrimEnd('\')
$list = Get-ChildItem $dist -Recurse -Force -File | ForEach-Object { $_.FullName.Substring($base.Length + 1) }
@("# RBR-RTX files (relative to the game folder). d3d9.dll sha256 $((Get-FileHash $proxy).Hash)") + $list + 'rbr-rtx\files.txt' |
  Set-Content -Encoding UTF8 (Join-Path $dist 'rbr-rtx\files.txt')
$sum = (Get-ChildItem $dist -Recurse -Force -File | Measure-Object Length -Sum)
"dist\RBR-RTX: $($sum.Count) files, $([math]::Round($sum.Sum / 1MB, 1)) MB  (Remix from $Remix)"

# -Pack: the two release archives, both with d3d9.dll and rbr-rtx\ at the top (unpack into the game folder).
if ($Pack) {
  $7z = (Get-Command 7z -ErrorAction SilentlyContinue).Source
  if (-not $7z) { $7z = Join-Path $env:ProgramFiles '7-Zip\7z.exe' }
  if (-not (Test-Path $7z)) { throw '7-Zip not found (7z on PATH or in Program Files).' }
  $name = "RBR-RTX-$Version$(if ($Diagnostics) { '-diagnostics' })"
  foreach ($ext in '7z', 'zip') {
    $out = Join-Path $here "dist\$name.$ext"
    if (Test-Path $out) { Remove-Item $out -Force }
    $opts = if ($ext -eq '7z') { '-t7z', '-mx=9', '-ms=on' } else { '-tzip', '-mx=7' }
    Push-Location $dist
    try {
      & $7z a @opts $out '*' | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "7z failed ($LASTEXITCODE) on $out" }
    } finally { Pop-Location }
    "$name.$ext  $([math]::Round((Get-Item $out).Length / 1MB, 1)) MB  sha256 $((Get-FileHash $out).Hash)"
  }
}
