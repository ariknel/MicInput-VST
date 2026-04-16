@echo off
REM ─────────────────────────────────────────────────────────────────────────────
REM setup_deps.bat — manually clone JUCE and whisper.cpp into the build folder
REM Run this ONCE before build.bat if your internet is unreliable.
REM After this succeeds, build.bat will find the deps and skip cloning.
REM ─────────────────────────────────────────────────────────────────────────────

set BUILD_DIR=%~dp0build
set DEPS_DIR=%BUILD_DIR%\_deps

echo Creating deps directory...
if not exist "%DEPS_DIR%" mkdir "%DEPS_DIR%"

REM ── Clone JUCE 8.0.4 ─────────────────────────────────────────────────────────
if exist "%DEPS_DIR%\juce-src\CMakeLists.txt" (
    echo JUCE already present, skipping.
) else (
    echo Cloning JUCE 8.0.4 (shallow)...
    cd /D "%DEPS_DIR%"
    git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git juce-src
    if errorlevel 1 (
        echo JUCE clone failed. Check your internet connection and try again.
        pause
        exit /b 1
    )
    echo JUCE cloned OK.
)

REM ── Clone whisper.cpp v1.7.4 ──────────────────────────────────────────────────
if exist "%DEPS_DIR%\whisper-src\CMakeLists.txt" (
    echo whisper.cpp already present, skipping.
) else (
    echo Cloning whisper.cpp v1.7.4 (shallow)...
    cd /D "%DEPS_DIR%"
    git clone --depth 1 --branch v1.7.4 https://github.com/ggerganov/whisper.cpp.git whisper-src
    if errorlevel 1 (
        echo whisper.cpp clone failed. Check your internet connection and try again.
        pause
        exit /b 1
    )
    echo whisper.cpp cloned OK.
)

echo.
echo Dependencies ready. Run build.bat to compile.
pause
