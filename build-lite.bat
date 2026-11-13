@echo off
setlocal
cd /d "%~dp0"

set "ARCH=%~1"
if not defined ARCH set "ARCH=x64"
if not "%ARCH%"=="x64" if not "%ARCH%"=="x86" (
  echo Usage: build-lite.bat [x64^|x86]
  exit /b 1
)
call build-env.bat %ARCH%
if errorlevel 1 exit /b 1
if not exist dist mkdir dist
if not exist build mkdir build

cl /nologo /TC /utf-8 /W4 /O1 /Oi /GS /guard:cf /Gy /Gw /Zl /MT ^
  /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0602 ^
  /Fo:build\G435BatteryTrayLite-%ARCH%.obj native\G435BatteryTray.c ^
  /Fe:dist\G435BatteryTrayLite-%ARCH%.exe ^
  /link /NODEFAULTLIB /ENTRY:G435Entry /SUBSYSTEM:WINDOWS ^
  /INCREMENTAL:NO /OPT:REF,ICF /DYNAMICBASE /NXCOMPAT /GUARD:CF ^
  /STACK:131072,4096 kernel32.lib user32.lib gdi32.lib libcmt.lib
if errorlevel 1 exit /b 1
echo Built dist\G435BatteryTrayLite-%ARCH%.exe
