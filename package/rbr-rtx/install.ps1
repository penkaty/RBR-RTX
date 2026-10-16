<#
.SYNOPSIS
  Installs RBR-RTX on top of a Richard Burns Rally folder: adds d3d9.dll and the rbr-rtx\ folder, nothing else.

.DESCRIPTION
  Run it from the unpacked RBR-RTX folder with -Game, or from <game>\rbr-rtx\ after copying the files by hand
  (then it only checks the folder and records what was there before). Game files are never changed or replaced:
  if the game folder already has another d3d9.dll (RSF's Vulkan/DXVK, dgVoodoo or VR mode), it stops.
  Remove everything again with <game>\rbr-rtx\uninstall.cmd (or uninstall.ps1).

.EXAMPLE
  .\rbr-rtx\install.ps1 -Game 'C:\Richard Burns Rally'
#>
param([string]$Game = '')
$ErrorActionPreference = 'Stop'
$src = Split-Path -Parent $PSScriptRoot
$exe = 'RichardBurnsRally_SSE.exe'
if (-not $Game) {
  if (Test-Path (Join-Path $src $exe)) { $Game = $src } else { throw "Pass -Game <your RBR folder> (the one with $exe)." }
}
$Game = (Resolve-Path $Game).Path.TrimEnd('\')
if (-not (Test-Path (Join-Path $Game $exe))) { throw "$Game has no $exe." }
if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw 'Close the game first.' }

function Get-OurHash([string]$manifest) { ((Get-Content $manifest -TotalCount 1) -split 'sha256 ')[1].Trim() }
$ours = @(Get-OurHash (Join-Path $PSScriptRoot 'files.txt'))
$installed = Join-Path $Game 'rbr-rtx\files.txt'
if (Test-Path $installed) { $ours += Get-OurHash $installed }   # updating an earlier RBR-RTX build
$dll = Join-Path $Game 'd3d9.dll'
if ((Test-Path $dll) -and $ours -notcontains (Get-FileHash $dll).Hash) {
  $v = (Get-Item $dll).VersionInfo
  throw ("The game folder already has another d3d9.dll ({0} {1}), probably from RSF's Vulkan/DXVK, dgVoodoo or VR mode. " +
         "Set the RSF launcher's Graphics mode to Normal (it removes that file) and run the install again.") -f $v.ProductName, $v.FileVersion
}

# Files Remix may create in the game folder; the ones already there now are kept by uninstall.
$remixPatterns = 'user.conf', 'rtx.conf', 'd3d9.log', '*_d3d9.log', '*bridge*.log', 'NvRemix*.log', 'remix-*.log',
                 'metrics*', 'nrc_session_log.txt', '*.dxvk-cache'
$pre = Join-Path $Game 'rbr-rtx\preexisting.txt'
$before = @(Get-ChildItem $Game -File -Force | Where-Object { $n = $_.Name; $remixPatterns | Where-Object { $n -like $_ } } |
            ForEach-Object Name)
if (Test-Path (Join-Path $Game 'rtx-remix')) { $before += 'rtx-remix' }

if ($src -ne $Game) {
  Copy-Item (Join-Path $src 'd3d9.dll') $dll -Force
  New-Item -ItemType Directory -Force (Join-Path $Game 'rbr-rtx') | Out-Null
  Get-ChildItem $PSScriptRoot -Force | Copy-Item -Destination (Join-Path $Game 'rbr-rtx') -Recurse -Force
}
if (-not (Test-Path $pre)) {   # keep the record from the first install when updating
  @('# Remix-type files that were in the game folder before RBR-RTX was installed; uninstall keeps them.') + $before |
    Set-Content -Encoding UTF8 $pre
}
"RBR-RTX installed in $Game ($(Get-Content (Join-Path $PSScriptRoot 'version.txt') -TotalCount 1))."
'Start the game as usual (RSF launcher, Graphics mode Normal). Alt+X opens the RTX Remix menu.'
"Remove it with $Game\rbr-rtx\uninstall.cmd"
