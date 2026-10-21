# Builds src\d3d9-proxy\bin\d3d9.dll (x86, release) with the Visual Studio 2022 C++ tools.
#   .\src\d3d9-proxy\build.ps1          -> bin\d3d9.dll + exports listing
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x86/x64 tools not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$bin = Join-Path $here 'bin'; $obj = Join-Path $here 'obj'
New-Item -ItemType Directory -Force $bin, $obj | Out-Null

$cl = "cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE " +
      "/Fo`"$obj\\`" `"$here\d3d9_proxy.cpp`" `"$here\exception_trace.cpp`" `"$here\comp.cpp`" `"$here\harvest.cpp`" `"$here\alphacut.cpp`" `"$here\keys.cpp`" `"$here\game_state.cpp`" /LD /link /DEF:`"$here\d3d9_proxy.def`" " +
      "/OUT:`"$bin\d3d9.dll`" /IMPLIB:`"$obj\d3d9.lib`" kernel32.lib user32.lib gdi32.lib ole32.lib windowscodecs.lib"
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"   # vcvarsall calls vswhere
$ErrorActionPreference = 'Continue'   # tool chatter on stderr must not abort the build
cmd /c "`"$vcvars`" x86 >nul && $cl && dumpbin /nologo /exports `"$bin\d3d9.dll`"" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
$ErrorActionPreference = 'Stop'
Get-Item "$bin\d3d9.dll" | Select-Object FullName, Length, LastWriteTime
