@echo off
setlocal
cd /d "%~dp0"

call build-lite.bat x64
if errorlevel 1 exit /b 1
copy /b /y "dist\G435BatteryTrayLite-x64.exe" "dist\G435BatteryTrayNative.exe" >nul
if errorlevel 1 (
  echo Close the running widget before replacing G435BatteryTrayNative.exe.
  exit /b 1
)
echo Built dist\G435BatteryTrayNative.exe
