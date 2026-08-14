@echo off
setlocal
cd /d "%~dp0"
echo Building inkMOD 1.1.4 Stable (release)...
pio run -e release -t clean
if errorlevel 1 goto :fail
set INKMOD_RELEASE_VERSION=1.1.4
pio run -e release -t upload
if errorlevel 1 goto :fail
echo.
echo Done. Firmware is in .pio\build\release\
pause
exit /b 0
:fail
echo.
echo BUILD FAILED.
pause
exit /b 1
