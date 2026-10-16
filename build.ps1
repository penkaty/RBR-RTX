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
  .\build.ps1 -Remix D:\downloads\remix-1.5.2
#>
param([string]$Remix = '', [switch]$SkipProxy, [switch]$Diagnostics)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$dist = Join-Path $here 'dist\RBR-RTX'
if (-not $Remix) {
  $Remix = Get-ChildItem (Join-Path $here '..\remix\runtime') -Directory -ErrorAction SilentlyContinue |
           Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Remix -or -not (Test-Path (Join-Path $Remix '.trex\NvRemixBridge.exe'))) {
  throw 'RTX Remix runtime not found. Pass -Remix <extracted Remix release folder with d3d9.dll and .trex\>.'
}
if (-not $SkipProxy) { & (Join-Path $here 'src\d3d9-proxy\build.ps1') | Out-Null }
$proxy = Join-Path $here 'src\d3d9-proxy\bin\d3d9.dll'
if (-not (Test-Path $proxy)) { throw "Missing $proxy" }

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
$remixDst = Join-Path $dist 'rbr-rtx\remix'
New-Item -ItemType Directory -Force $remixDst | Out-Null
Copy-Item $proxy $dist
Get-ChildItem $Remix -Force | Where-Object { $_.Name -notin 'NvRemixLauncher32.exe', 'd3d8to9.dll', 'ThirdPartyLicenses-d3d8to9.txt' } |
  Copy-Item -Destination $remixDst -Recurse -Force
Get-ChildItem (Join-Path $here 'package') -Force | Copy-Item -Destination $dist -Recurse -Force
# Dev runs write into the runtime folder (crash dumps, logs, ReShade injected into NvRemixBridge.exe): never ship those.
$leftovers = '*.dmp', '*.log', 'ReShade*', 'nrc_session_log.txt', 'user.conf', 'imgui.ini'
Get-ChildItem $remixDst -Recurse -Force -File | Where-Object { $n = $_.Name; $leftovers | Where-Object { $n -like $_ } } |
  ForEach-Object { "left out (dev-run leftover): $($_.FullName.Substring($remixDst.Length + 1))"; Remove-Item $_.FullName -Force }
Get-ChildItem $remixDst -Recurse -Force -Directory -Filter 'reshade-shaders' |
  ForEach-Object { "left out (dev-run leftover): $($_.FullName.Substring($remixDst.Length + 1))\"; Remove-Item $_.FullName -Recurse -Force }

if ($Diagnostics) {
  @('RBR-RTX diagnostics mode (test build).',
    'While this file exists, RBR-RTX writes its own log, Ctrl+Scroll Lock frame dumps and crash dumps to rbr-rtx\logs,',
    'next to RTX Remix''s logs. To report a problem: zip the rbr-rtx\logs folder and send it. The logs contain your',
    'Windows user name in file paths and your GPU/driver; nothing is sent anywhere automatically.',
    'Delete this file to stop the extra logging.') | Set-Content -Encoding UTF8 (Join-Path $dist 'rbr-rtx\diagnostics.txt')
}
$rev = (git -C $here describe --always --dirty 2>$null)
if ($Diagnostics) { $rev += ' (diagnostics)' }
@("RBR-RTX $rev, built $(Get-Date -Format 'yyyy-MM-dd HH:mm')", "RTX Remix runtime: $(Split-Path $Remix -Leaf)") |
  Set-Content -Encoding UTF8 (Join-Path $dist 'rbr-rtx\version.txt')
# files.txt: every file the add-on brings, plus the proxy's hash so uninstall never deletes another d3d9.dll.
$base = (Resolve-Path $dist).Path.TrimEnd('\')
$list = Get-ChildItem $dist -Recurse -Force -File | ForEach-Object { $_.FullName.Substring($base.Length + 1) }
@("# RBR-RTX files (relative to the game folder). d3d9.dll sha256 $((Get-FileHash $proxy).Hash)") + $list + 'rbr-rtx\files.txt' |
  Set-Content -Encoding UTF8 (Join-Path $dist 'rbr-rtx\files.txt')
$sum = (Get-ChildItem $dist -Recurse -Force -File | Measure-Object Length -Sum)
"dist\RBR-RTX: $($sum.Count) files, $([math]::Round($sum.Sum / 1MB, 1)) MB  (Remix from $Remix)"
