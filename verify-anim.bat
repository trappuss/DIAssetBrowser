@echo off
setlocal enabledelayedexpansion
title Verify animations - full archive sweep (real parser + v3 pose audit)
cd /d "%~dp0"

rem ONE run. Builds di_probe and runs the app's REAL parser over EVERY .anim in
rem the archive, then audits every v3 clip for pose quality (non-unit
rem quaternions, extreme positions). Output: anim_verify_report.txt.
rem
rem This is the coverage check for the v3 decoder: it turns "works on serrat"
rem into a pass/fail across all ~37,769 clips, with the v3 edge cases
rem (3-channel clips, base/range-skipped clips, raw/float key types) flagged by
rem folder so anything that misbehaves is easy to find.

set "LOG=%~dp0build_log.txt"
set "REPORT=%~dp0anim_verify_report.txt"
set "MPK=G:\G Games\Diablo Immortal\Package\MPK"

where cl >nul 2>&1
if errorlevel 1 (
    echo [1/3] Initializing Visual Studio 2022 build tools...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" ( echo   ERROR: Visual Studio 2022 not found. & pause & exit /b 1 )
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo   ERROR: MSVC C++ tools not found. & pause & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
    if errorlevel 1 ( echo   ERROR: vcvars64 failed. & pause & exit /b 1 )
) else (
    echo [1/3] MSVC already on PATH.
)

if not defined VCPKG_ROOT (
    echo   ERROR: VCPKG_ROOT is not set ^(same setup as the other tools^).
    pause & exit /b 1
)

echo [2/3] Configure + build di_probe...
cmake --preset windows-msvc-release >"%LOG%" 2>&1
if errorlevel 1 ( echo CONFIGURE FAILED - see build_log.txt & findstr /i /c:"error" "%LOG%" & pause & exit /b 1 )
cmake --build --preset release --target di_probe >>"%LOG%" 2>&1
if errorlevel 1 ( echo BUILD FAILED - see build_log.txt & findstr /i /c:"error" "%LOG%" & pause & exit /b 1 )

if not exist "%MPK%" (
    echo   ERROR: MPK folder not found: %MPK%
    echo   Edit the MPK line near the top of this .bat if the game moved.
    pause & exit /b 1
)

echo [3/3] Running the real parser over every .anim (a couple of minutes)...
echo.
build\release\di_probe.exe "%MPK%" --anim-verify >"%REPORT%" 2>&1
if errorlevel 1 ( echo   VERIFY FAILED - see anim_verify_report.txt & pause & exit /b 1 )

for %%F in ("%REPORT%") do echo Report: anim_verify_report.txt  (%%~zF bytes)
echo Send anim_verify_report.txt back.
pause
