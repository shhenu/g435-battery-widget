@echo off
setlocal
cd /d "%~dp0"
if not exist "G435BatteryTrayNative.exe" (
  echo Extract the complete release ZIP before running diagnostics.
  pause
  exit /b 1
)
"%~dp0G435BatteryTrayNative.exe" --diagnose > "%~dp0G435-diagnostics.txt" 2>&1
start "" notepad.exe "%~dp0G435-diagnostics.txt"
