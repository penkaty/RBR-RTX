<#
.SYNOPSIS
  Removes RBR-RTX from the game folder it sits in (<game>\rbr-rtx\uninstall.ps1) and restores the original game.

.DESCRIPTION
  Deletes our d3d9.dll (only if it is still ours: same SHA-256 as in files.txt), the files RTX Remix created in
  the game folder while RBR-RTX was installed (user.conf with the Alt+X settings, logs, an empty rtx-remix\
  folder from earlier builds), and finally the whole rbr-rtx\ folder, including its rtx-remix\mods
  and \captures. Files that were there before the install (preexisting.txt) and Remix mods you added under
  rtx-remix\ stay.
#>
param([switch]$Yes)
$ErrorActionPreference = 'Stop'
$addon = $PSScriptRoot
$Game = Split-Path -Parent $addon
$exe = 'RichardBurnsRally_SSE.exe'
if (-not (Test-Path (Join-Path $Game $exe))) { throw "$Game has no $exe; run this from <game>\rbr-rtx\." }
if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw 'Close the game first.' }
if (-not $Yes -and (Read-Host "Remove RBR-RTX from $Game? (y/N)") -ne 'y') { 'Cancelled.'; return }

$manifest = Get-Content (Join-Path $addon 'files.txt')
$ourHash = ($manifest[0] -split 'sha256 ')[1].Trim()
$keep = @(); $pre = Join-Path $addon 'preexisting.txt'
if (Test-Path $pre) { $keep = @(Get-Content $pre | Where-Object { $_ -and -not $_.StartsWith('#') }) }
$removed = 0

$dll = Join-Path $Game 'd3d9.dll'
if (Test-Path $dll) {
  if ((Get-FileHash $dll).Hash -eq $ourHash) { Remove-Item $dll -Force; $removed++ }
  else { Write-Warning 'd3d9.dll in the game folder is not the RBR-RTX one (RSF or another mod replaced it); left alone.' }
}
$remixPatterns = 'user.conf', 'rtx.conf', 'd3d9.log', '*_d3d9.log', '*bridge*.log', 'NvRemix*.log', 'remix-*.log',
                 'metrics*', 'nrc_session_log.txt', '*.dxvk-cache'
Get-ChildItem $Game -File -Force | Where-Object { $n = $_.Name; ($keep -notcontains $n) -and ($remixPatterns | Where-Object { $n -like $_ }) } |
  ForEach-Object { Remove-Item $_.FullName -Force; $removed++ }
$mods = Join-Path $Game 'rtx-remix'
if ((Test-Path $mods) -and ($keep -notcontains 'rtx-remix')) {
  if (Get-ChildItem $mods -Recurse -File -Force) { Write-Warning "$mods has files (Remix mods?); left alone." }
  else { Remove-Item $mods -Recurse -Force }
}
Set-Location $Game
Remove-Item $addon -Recurse -Force
"RBR-RTX removed from $Game ($removed file(s) in the game folder plus rbr-rtx\). The game is back to its original setup."
