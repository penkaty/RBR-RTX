@cd /d "%~dp0" & powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %* & pause & exit /b
