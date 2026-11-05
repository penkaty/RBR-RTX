# Builds src\d3d9-proxy\bin\d3d9.dll (x86, release) with the Visual Studio 2022 C++ tools.
#   .\src\d3d9-proxy\build.ps1          -> bin\d3d9.dll + exports listing
#   .\src\d3d9-proxy\build.ps1 -OutDir bin-f3   -> bin-f3\d3d9.dll instead (relative to this folder; the default bin\ stays untouched,
#                                                  e.g. while a test runs from it)
param([string]$OutDir = 'bin')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x86/x64 tools not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$bin = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $here $OutDir }; $obj = Join-Path $here 'obj'
New-Item -ItemType Directory -Force $bin, $obj, "$obj\imgui" | Out-Null

# Dear ImGui 1.91.7 (third_party\imgui, MIT) for the F3 menu: compiled apart at /W0 (its own warnings are not ours), linked in below.
$imgui = "$here\third_party\imgui"
$imguiObjs = 'imgui','imgui_draw','imgui_tables','imgui_widgets','imgui_impl_dx9','imgui_impl_win32' | ForEach-Object { "`"$obj\imgui\$_.obj`"" }
$clImgui = "cl /nologo /c /O2 /MT /W0 /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$imgui`" /Fo`"$obj\imgui\\`" " +
           "`"$imgui\imgui.cpp`" `"$imgui\imgui_draw.cpp`" `"$imgui\imgui_tables.cpp`" `"$imgui\imgui_widgets.cpp`" " +
           "`"$imgui\backends\imgui_impl_dx9.cpp`" `"$imgui\backends\imgui_impl_win32.cpp`""

$cl = "cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE " +
      "/Fo`"$obj\\`" `"$here\d3d9_proxy.cpp`" `"$here\exception_trace.cpp`" `"$here\prof.cpp`" `"$here\comp.cpp`" `"$here\harvest.cpp`" `"$here\alphacut.cpp`" `"$here\keys.cpp`" `"$here\game_state.cpp`" `"$here\report.cpp`" `"$here\headlights.cpp`" `"$here\particles.cpp`" `"$here\windscreen.cpp`" `"$here\weather.cpp`" `"$here\radnormals.cpp`" `"$here\remixplus_api.cpp`" `"$here\gaugeprobe.cpp`" `"$here\genmod.cpp`" `"$here\litparts.cpp`" `"$here\foliage.cpp`" `"$here\worldlights.cpp`" `"$here\apiworld.cpp`" `"$here\hudglow.cpp`" `"$here\rbrhud.cpp`" `"$here\wetness.cpp`" `"$here\dinput_block.cpp`" `"$here\custom.cpp`" `"$here\carpaint.cpp`" `"$here\menu\menu.cpp`" `"$here\menu\menu_custom.cpp`" /I `"$imgui`" /LD /link /DEF:`"$here\d3d9_proxy.def`" " +
      "/OUT:`"$bin\d3d9.dll`" /MAP:`"$obj\d3d9.map`" /IMPLIB:`"$obj\d3d9.lib`" $($imguiObjs -join ' ') kernel32.lib user32.lib gdi32.lib ole32.lib windowscodecs.lib version.lib advapi32.lib dwmapi.lib"
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"   # vcvarsall calls vswhere
$ErrorActionPreference = 'Continue'   # tool chatter on stderr must not abort the build
cmd /c "`"$vcvars`" x86 >nul && $clImgui && $cl && dumpbin /nologo /exports `"$bin\d3d9.dll`"" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
$ErrorActionPreference = 'Stop'
Get-Item "$bin\d3d9.dll" | Select-Object FullName, Length, LastWriteTime
