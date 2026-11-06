@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-RPK.ps1"
if errorlevel 1 (
  echo.
  echo RPK setup did not complete. See the error above.
  pause
  exit /b 1
)
echo.
echo RPK setup finished.
pause
