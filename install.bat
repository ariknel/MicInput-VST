@echo off
setlocal EnableDelayedExpansion
title MicInput VST - Install
cd /d "%~dp0"

:: Must run as admin
net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Run as Administrator.
    echo Right-click install.bat ^> "Run as administrator"
    pause & exit /b 1
)

set "VST3_SRC=%~dp0build\MicInputVST_artefacts\Release\VST3\MicInput.vst3"
set "VST3_DST=C:\Program Files\Common Files\VST3\MicInput.vst3"

if not exist "!VST3_SRC!" (
    echo [ERROR] MicInput.vst3 not found at:
    echo   !VST3_SRC!
    echo Run build.bat first.
    pause & exit /b 1
)

echo [..] Installing to !VST3_DST!
xcopy /E /I /Y "!VST3_SRC!" "!VST3_DST!\" >nul
if errorlevel 1 (
    echo [ERROR] Copy failed.
    pause & exit /b 1
)

echo [OK] Installed.
echo.
echo Rescan in Bitwig: Settings ^> Plugins ^> Rescan
echo Then load MicInput on an Audio Track instrument slot.
echo.
pause
