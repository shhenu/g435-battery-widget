@echo off
rem Called by build scripts; intentionally keeps the compiler environment in the caller.
set "ARCH=%~1"
if not defined ARCH set "ARCH=x64"
if not "%ARCH%"=="x64" if not "%ARCH%"=="x86" exit /b 1
set "VSROOT="
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%VSINSTALLDIR%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VSROOT if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
)
if not defined VSROOT set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" (
  echo Install Visual Studio C++ Build Tools with a Windows SDK.
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul
exit /b %errorlevel%
