@echo off
setlocal EnableDelayedExpansion
title MicInput VST - Build
cd /d "%~dp0"

echo.
echo ============================================================
echo  MicInput VST - Build
echo ============================================================
echo.

:: ── Find vswhere ─────────────────────────────────────────────────────────────
set VS86=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe
set VS64=C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe

set VSWHERE=
if exist "%VS86%" set VSWHERE=%VS86%
if exist "%VS64%" if "%VSWHERE%"=="" set VSWHERE=%VS64%

if "%VSWHERE%"=="" (
    echo [ERROR] vswhere not found. Install Visual Studio 2022 Build Tools.
    pause & exit /b 1
)
echo [OK] Found vswhere.

:: ── Find vcvars64 ─────────────────────────────────────────────────────────────
set TMPOUT=%TEMP%\micinput_vcvars.txt
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "VC\Auxiliary\Build\vcvars64.bat" > "%TMPOUT%" 2>nul

set VCVARS=
for /f "usebackq tokens=*" %%P in ("%TMPOUT%") do (
    if "!VCVARS!"=="" set "VCVARS=%%P"
)
del "%TMPOUT%" >nul 2>&1

if "!VCVARS!"=="" (
    echo [ERROR] MSVC not found. Install "Desktop development with C++" workload.
    pause & exit /b 1
)
call "!VCVARS!" >nul 2>&1
echo [OK] MSVC x64 ready.

:: ── Check CMake ───────────────────────────────────────────────────────────────
where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake not found. Download from https://cmake.org/download/
    pause & exit /b 1
)
echo [OK] CMake found.

:: ── Check Git ─────────────────────────────────────────────────────────────────
where git >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Git not found. Download from https://git-scm.com/
    pause & exit /b 1
)
echo [OK] Git found.

:: ── Check for local dependency overrides ──────────────────────────────────────
:: If you have slow/unreliable internet, pre-download deps manually:
::
::   JUCE:
::     git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git deps\juce
::     (or download zip from https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.4.zip
::      and extract to deps\juce)
::
::   whisper.cpp:
::     git clone --depth 1 --branch v1.7.4 https://github.com/ggerganov/whisper.cpp.git deps\whisper
::
:: If those folders exist, CMake will use them instead of downloading.

set LOCAL_JUCE=
set LOCAL_WHISPER=
if exist "%~dp0deps\juce\CMakeLists.txt"   set LOCAL_JUCE=%~dp0deps\juce
if exist "%~dp0deps\whisper\CMakeLists.txt" set LOCAL_WHISPER=%~dp0deps\whisper

set CMAKE_EXTRA=
if not "!LOCAL_JUCE!"=="" (
    echo [OK] Using local JUCE at: !LOCAL_JUCE!
    set CMAKE_EXTRA=!CMAKE_EXTRA! -DFETCHCONTENT_SOURCE_DIR_JUCE="!LOCAL_JUCE!"
) else (
    echo [..] JUCE: will download from GitHub ^(first run only^)
)
if not "!LOCAL_WHISPER!"=="" (
    echo [OK] Using local whisper.cpp at: !LOCAL_WHISPER!
    set CMAKE_EXTRA=!CMAKE_EXTRA! -DFETCHCONTENT_SOURCE_DIR_WHISPER="!LOCAL_WHISPER!"
) else (
    echo [..] whisper.cpp: will download from GitHub ^(first run only^)
)
echo.

:: ── Clean broken build if deps failed to download ─────────────────────────────
:: If build exists but deps stamps are incomplete, nuke and retry.
if exist build (
    if not exist "build\_deps\juce-src\CMakeLists.txt" (
        echo [!!] Incomplete JUCE download detected - cleaning build dir...
        rmdir /s /q build
        echo [OK] Build dir cleaned.
    )
    if not exist "build\_deps\whisper-src\CMakeLists.txt" (
        echo [!!] Incomplete whisper.cpp download detected - cleaning build dir...
        rmdir /s /q build
        echo [OK] Build dir cleaned.
    )
)

if not exist build mkdir build

:: ── Configure ─────────────────────────────────────────────────────────────────
echo [1/2] Configuring...
echo       First run downloads JUCE (~120MB) and whisper.cpp (~30MB).
echo       If this fails, see the deps\ instructions above.
echo.

where ninja >nul 2>&1
if not errorlevel 1 (
    echo [OK] Using Ninja generator
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release !CMAKE_EXTRA!
) else (
    echo [OK] Using NMake generator
    cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release !CMAKE_EXTRA!
)

if errorlevel 1 (
    echo.
    echo [ERROR] CMake configure failed.
    echo.
    echo  Most common causes:
    echo    1. No internet / GitHub unreachable
    echo       Fix: pre-download deps as described above
    echo    2. Partial clone from previous failed run
    echo       Fix: rmdir /s /q build  then retry
    echo    3. Git not installed
    echo       Fix: https://git-scm.com/
    echo.
    pause & exit /b 1
)
echo [OK] Configured.
echo.

:: ── Build ─────────────────────────────────────────────────────────────────────
echo [2/2] Building...

cmake --build build --config Release --parallel
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed. Read errors above.
    pause & exit /b 1
)

echo.
echo ============================================================
echo  BUILD SUCCESSFUL
echo ============================================================
echo.

for /r "%~dp0build" %%F in (MicInput.vst3) do (
    echo VST3: %%F
)

echo.
echo Next steps:
echo   1. Run install.bat as Administrator
echo   2. Bitwig: Settings - Plugins - Rescan
echo   3. Load MicInput on an Instrument Track
echo   4. Select mic, arm track, record
echo.
pause
