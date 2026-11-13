@echo off
setlocal
cd /d "%~dp0"
call build-env.bat x64
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /TC /utf-8 /W4 /O1 /MT /Fo:build\g435_protocol_tests.obj ^
  tests\g435_protocol_tests.c /Fe:build\g435_protocol_tests.exe
if errorlevel 1 exit /b 1
build\g435_protocol_tests.exe
exit /b %errorlevel%
