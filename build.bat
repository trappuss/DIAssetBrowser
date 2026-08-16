@echo off
setlocal enabledelayedexpansion
title Build DIAssetBrowser
cd /d "%~dp0"

:: Close any running instance so the linker can overwrite the .exe (avoids LNK1104).
taskkill /im DIAssetBrowser.exe /f >nul 2>&1

echo ============================================================
echo  DIAssetBrowser - full build
echo  (first build installs qtbase via vcpkg - the D4 tool's
echo   binary cache should make that minutes, not hours)
echo ============================================================
echo.

:: 1. MSVC on PATH; if not, run vcvars64.
where cl >nul 2>&1
if errorlevel 1 (
    echo [1/4] Initializing Visual Studio 2022 build tools...
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
    echo [1/4] MSVC already on PATH.
)

:: 2. vcpkg.
if not defined VCPKG_ROOT (
    echo   ERROR: VCPKG_ROOT is not set ^(same one-time setup as the D4 tool^).
    pause & exit /b 1
)

:: 3. Pin the vcpkg baseline to the local checkout (manifest mode requirement).
echo [2/4] Pinning vcpkg baseline...
"%VCPKG_ROOT%\vcpkg.exe" x-update-baseline --add-initial-baseline
if errorlevel 1 echo   WARNING: baseline pin failed - continuing with the committed one.

:: 4. Configure + build, LIVE output AND captured to build_log.txt.
echo [3/4] Configuring...
cmake --preset windows-msvc-release
if errorlevel 1 ( echo CONFIGURE FAILED & pause & exit /b 1 )

echo [4/4] Building (live output below)...
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
findstr /i /c:"error C" /c:": error" /c:"error LNK" /c:"fatal error" /c:"FAILED" /c:"ninja: build stopped" "%~dp0build_log.txt" > "%~dp0build_errors.txt"
if not "%RC%"=="0" (
    echo.
    echo BUILD FAILED - full output in build_log.txt, errors in build_errors.txt
    pause & exit /b 1
)

echo.
echo Done. Deploying + launching...
call "%~dp0run.bat"
