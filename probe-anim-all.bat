@echo off
setlocal enabledelayedexpansion
title Probe animation - everything, one run
cd /d "%~dp0"

rem ONE run, one report. Supersedes probe-anim-coverage.bat, probe-anim-graph.bat
rem and probe-anim-tags.bat - this runs every animation probe mode in a single
rem di_probe invocation so nothing needs a second pass:
rem
rem   --anim-coverage   where .anim / .skeleton files sit vs the models that
rem                     need them, per folder, for each resolver strategy
rem   --anim-forensics  every .anim header walked: container versions, rot-key
rem                     tag census, solved key stride per tag, whether those
rem                     bytes obey the v2 40-bit quaternion law, which bones and
rem                     track slots carry each tag, raw key dumps WITH a
rem                     known-good 0x32 reference from the same file to diff
rem                     against, and the v3 files whose name table disagrees
rem                     with their header
rem   --skin-map        every repository type; clips reachable via the bound
rem                     SkinSkeleton; the most-shared skeletons
rem   --anim-assets     the Animation-typed repository entries and the unread
rem                     .graph / .mont / .motion / .human families
rem   --anim-verify     runs the app's REAL parser over all 37,769 clips:
rem                     per-folder ok%%, failure reasons, and one 64-byte header
rem                     dump per failure family (cracks the next layout without
rem                     another run)
rem   --asset <name>    full graph trace of one asset (deps, parent, skeleton),
rem                     now with a per-clip REAL-parser verdict
rem
rem Output: anim_full_report.txt (everything) plus anim_coverage_report.txt and
rem anim_tag_report.txt written by their own modes. Send anim_full_report.txt.
rem
rem Args: [assetSubstring] [maxAnimFiles]
rem   probe-anim-all.bat                        - default asset, ALL 37,769 clips
rem   probe-anim-all.bat boss_knight_ma         - trace a different asset
rem   probe-anim-all.bat boss_knight_ma 4000    - and cap the clip scan

set "LOG=%~dp0build_log.txt"
set "REPORT=%~dp0anim_full_report.txt"
set "MPK=G:\G Games\Diablo Immortal\Package\MPK"
set "ASSET=%~1"
set "CAP=%~2"
if "%ASSET%"=="" set "ASSET=battlepet_serrat"

where cl >nul 2>&1
if errorlevel 1 (
    echo [1/3] Initializing Visual Studio 2022 build tools...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo   ERROR: Visual Studio 2022 not found.
        pause & exit /b 1
    )
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH (
        echo   ERROR: MSVC C++ tools not found in your VS install.
        pause & exit /b 1
    )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
    if errorlevel 1 ( echo   ERROR: vcvars64 failed. & pause & exit /b 1 )
) else (
    echo [1/3] MSVC already on PATH.
)

if not defined VCPKG_ROOT (
    echo   ERROR: VCPKG_ROOT is not set ^(same setup as the D4 tool^).
    pause & exit /b 1
)

echo [2/3] Configure + build di_probe...
cmake --preset windows-msvc-release >"%LOG%" 2>&1
if errorlevel 1 (
    echo CONFIGURE FAILED - see build_log.txt
    findstr /i /c:"error" "%LOG%"
    pause & exit /b 1
)

cmake --build --preset release --target di_probe >>"%LOG%" 2>&1
if errorlevel 1 (
    echo BUILD FAILED - see build_log.txt
    findstr /i /c:"error" "%LOG%"
    pause & exit /b 1
)

if not exist "%MPK%" (
    echo   ERROR: MPK folder not found:
    echo     %MPK%
    echo   Edit the MPK line near the top of this .bat if the game moved.
    pause & exit /b 1
)

echo [3/3] Build OK - running every anim probe mode in one pass.
echo       This inflates all 37,769 clips; expect a couple of minutes.
echo.
build\release\di_probe.exe "%MPK%" --anim-coverage "%~dp0anim_coverage_report.txt" --anim-forensics %CAP% --anim-verify --skin-map --anim-assets --asset "%ASSET%" >"%REPORT%" 2>&1
if errorlevel 1 (
    echo   PROBE FAILED - see anim_full_report.txt
    pause & exit /b 1
)

for %%F in ("%REPORT%") do echo Report: anim_full_report.txt  (%%~zF bytes)
echo Send anim_full_report.txt back - it has everything.
pause
