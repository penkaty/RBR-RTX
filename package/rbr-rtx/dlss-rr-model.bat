@cd /d "%~dp0" & powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dlss-rr-model.ps1" %* & pause & exit /b
