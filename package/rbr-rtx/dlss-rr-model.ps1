<#
.SYNOPSIS
  Switches the bundled NVIDIA DLSS Ray Reconstruction DLL in rbr-rtx\remix\.trex\ between Transformer and CNN-compatible builds.

.DESCRIPTION
  Remix Plus defaults to nvngx_dlssd.dll 310.9.x (Transformer presets only).
  CNN Ray Reconstruction presets (A, B, C) are faster than Transformer on RTX 20/30 and laptop GPUs,
  but require an nvngx_dlssd.dll older than 310.4 (such as 310.2.1.0 bundled from RTX Remix 1.5.2).

  Double-click dlss-rr-model.bat for an interactive prompt, or pass -Model transformer / cnn / status.

.EXAMPLE
  .\rbr-rtx\dlss-rr-model.ps1 -Model cnn
  .\rbr-rtx\dlss-rr-model.ps1 -Model transformer
  .\rbr-rtx\dlss-rr-model.ps1 -Model status
#>
param(
  [ValidateSet('', 'transformer', 'cnn', 'status')]
  [string]$Model = ''
)
$ErrorActionPreference = 'Stop'

$trex = Join-Path $PSScriptRoot 'remix\.trex'
$active = Join-Path $trex 'nvngx_dlssd.dll'
$store = Join-Path $trex 'dlss-rr'
$dllTransformer = Join-Path $store 'nvngx_dlssd_transformer.dll'
$dllCnn = Join-Path $store 'nvngx_dlssd_cnn.dll'

function Get-DllInfo([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return $null }
  $v = (Get-Item -LiteralPath $path).VersionInfo
  $ver = '{0}.{1}.{2}.{3}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart
  $cnnOk = ($v.FileMajorPart -lt 310) -or ($v.FileMajorPart -eq 310 -and $v.FileMinorPart -lt 4)
  [pscustomobject]@{
    Path    = $path
    Version = $ver
    CnnOk   = $cnnOk
    Label   = if ($cnnOk) { 'CNN presets A-C + Transformer' } else { 'Transformer only (presets D/E, requires < 310.4 for CNN)' }
    Hash    = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
  }
}

function Show-Status {
  $cur = Get-DllInfo $active
  if (-not $cur) { Write-Warning "Active DLL missing: $active" }
  else { "Active:      nvngx_dlssd.dll  v$($cur.Version)  [$($cur.Label)]" }
  $tr = Get-DllInfo $dllTransformer
  $cn = Get-DllInfo $dllCnn
  if ($tr) { "Transformer: v$($tr.Version)  ($dllTransformer)" }
  if ($cn) { "CNN (<310.4): v$($cn.Version)  ($dllCnn)" }
}

if (-not (Test-Path -LiteralPath $trex)) { throw "Runtime .trex folder missing: $trex" }
Show-Status
if ($Model -eq 'status') { return }

if (-not $Model) {
  Write-Host ''
  Write-Host 'Choose DLSS Ray Reconstruction model DLL:'
  Write-Host '  [1] Transformer (310.9.x, default) - best quality, RTX 40/50 recommended'
  Write-Host '  [2] CNN-compatible (<310.4)        - unlocks faster CNN presets A/B/C on RTX 20/30 & laptop GPUs'
  Write-Host '  [Q] Cancel'
  $pick = (Read-Host 'Selection (1/2/Q)').Trim().ToUpperInvariant()
  if ($pick -eq '1') { $Model = 'transformer' }
  elseif ($pick -eq '2') { $Model = 'cnn' }
  else { 'Cancelled.'; return }
}

foreach ($proc in 'RichardBurnsRally_SSE', 'NvRemixBridge') {
  if (Get-Process $proc -ErrorAction SilentlyContinue) {
    throw "Close the game ($proc is running) before switching nvngx_dlssd.dll."
  }
}

$src = if ($Model -eq 'cnn') { $dllCnn } else { $dllTransformer }
$srcInfo = Get-DllInfo $src
if (-not $srcInfo) {
  throw "Bundled $Model DLL not found: $src"
}

Copy-Item -LiteralPath $src -Destination $active -Force
$dstInfo = Get-DllInfo $active
if (-not $dstInfo -or $dstInfo.Hash -ne $srcInfo.Hash) {
  throw "Copy verification failed for $active"
}

Write-Host ''
"Switched active nvngx_dlssd.dll to v$($dstInfo.Version) [$($dstInfo.Label)]"
if ($Model -eq 'cnn') {
  'In-game (Alt+X -> Rendering -> DLSS Ray Reconstruction): pick Preset A, B, or C for lower GPU cost on RTX 20/30 / laptop GPUs.'
} else {
  'In-game (Alt+X -> Rendering -> DLSS Ray Reconstruction): pick Default, D, or E for Transformer quality.'
}
