@echo off
setlocal enabledelayedexpansion
title Rebuild DIAssetBrowser (fast)
cd /d "%~dp0"

:: Close any running instance so the linker can overwrite the .exe (avoids LNK1104).
taskkill /im DIAssetBrowser.exe /f >nul 2>&1

:: Snapshot the source into .Backups before building (recovery insurance).
call "%~dp0backup-src.bat"

:: Pre-build source checks (verify-src.py). Non-blocking: a checker false
:: positive must never stop a build. NOTE plain `python` on Windows is usually
:: the Microsoft Store alias (prints an advert, returns success) - prefer `py`.
set "PYEXE="
py -3 -c "import sys" >nul 2>&1 && set "PYEXE=py -3"
if not defined PYEXE python -c "import sys" >nul 2>&1 && set "PYEXE=python"
if defined PYEXE (
    %PYEXE% "%~dp0verify-src.py" --quiet
    if errorlevel 1 (
        echo.
        echo  ^>^> verify-src found problems ^(listed above^). Building anyway - Ctrl+C to stop.
        echo.
    )
)

where cl >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo ERROR: Visual Studio 2022 C++ tools not found. & pause & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

if not exist "build\release\CMakeCache.txt" (
    echo No build yet - run build.bat first ^(it does the one-time dependency build^).
    pause & exit /b 1
)

:: Incremental build with LIVE output, captured to build_log.txt.
echo Building (LIVE output below)...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"

findstr /i /c:"error C" /c:": error" /c:"error LNK" /c:"fatal error" /c:"FAILED" /c:"ninja: build stopped" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
if not "%RC%"=="0" (
    echo.
    echo BUILD FAILED - full output above and in build_log.txt ^(errors in build_errors.txt^)
    pause & exit /b 1
)

echo.
echo Done. Deploying + launching...
call "%~dp0run.bat"
