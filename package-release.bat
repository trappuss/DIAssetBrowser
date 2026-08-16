@echo off
setlocal enabledelayedexpansion
title Package DIAssetBrowser - portable release folder
cd /d "%~dp0"

:: ============================================================================
::  Builds the portable RELEASE folder:
::    * DIAssetBrowser.exe + the exact Qt6 runtime DLLs and plugins it needs
::      (copied from vcpkg_installed) + README
::    * settings + caches live in a data\ folder beside the exe (no registry, no AppData)
::  Output: dist\DIAssetBrowser\   (+ dist\DIAssetBrowser.zip)
::
::  This uses the normal DYNAMIC Qt from vcpkg (no static Qt build required), so it is
::  the fast path. The folder is fully self-contained: unzip anywhere and run.
:: ============================================================================

taskkill /im DIAssetBrowser.exe /f >nul 2>&1

:: 1. MSVC on PATH (else init vcvars64).
where cl >nul 2>&1
if errorlevel 1 (
    echo [1/5] Initializing Visual Studio 2022 build tools...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" ( echo   ERROR: Visual Studio 2022 with "Desktop development with C++" required. & pause & exit /b 1 )
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo   ERROR: MSVC C++ tools not found. & pause & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" || ( echo   ERROR: vcvars64 failed. & pause & exit /b 1 )
) else ( echo [1/5] MSVC already on PATH. )

:: 2. vcpkg.
if not defined VCPKG_ROOT ( echo   ERROR: VCPKG_ROOT is not set. See README.md ^(Build^). & pause & exit /b 1 )
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ( echo   ERROR: VCPKG_ROOT is not a vcpkg checkout. & pause & exit /b 1 )
"%VCPKG_ROOT%\vcpkg.exe" x-update-baseline --add-initial-baseline >nul 2>&1

:: Each stage streams LIVE to the console AND is captured to build_log.txt via PowerShell
:: Tee-Object (a cmdlet, so %errorlevel% keeps cmake's real exit code). No frozen-looking window.

:: 3. Configure (normal dynamic-Qt release preset).
echo [2/5] Configuring release build...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --preset windows-msvc-release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
if not "%RC%"=="0" (
    REM Select-String, NOT findstr. Tee-Object above writes UTF-16; findstr cannot read it and
    REM produces an EMPTY build_errors.txt, so a failed configure reported no errors at all. That
    REM cost a debugging round trip on a real failure. rebuild.bat and the smoke test already do
    REM this correctly - this path was missed.
    REM
    REM Set-Content, NOT "Tee-Object -Encoding utf8": Tee-Object gained -Encoding in PowerShell 7.
    REM Windows PowerShell 5.1 - which is what "powershell.exe" is - rejects it with "A parameter
    REM cannot be found that matches parameter name 'Encoding'", so build_errors.txt was never
    REM written on this path and the script told the user to read a file that did not exist.
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$e = @(Select-String -Path '%~dp0build_log.txt' -Pattern 'CMake Error','Failed to find','FAILED',': error' | ForEach-Object { $_.Line } | Select-Object -First 40); $e | Write-Host; Set-Content -Path '%~dp0build_errors.txt' -Value $e -Encoding utf8"
    echo   CONFIGURE FAILED ^(see build_errors.txt^). & pause & exit /b 1
)

:: 4. Build.
echo [3/5] Building (LIVE output; the two big files can take a few minutes each)...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
REM Same UTF-16 trap as the configure step above - findstr silently produced an empty file, and the
REM same PowerShell 5.1 trap: Tee-Object has no -Encoding there.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$e = @(Select-String -Path '%~dp0build_log.txt' -Pattern 'error C','error LNK','fatal error',': error','ninja: build stopped' | ForEach-Object { $_.Line } | Select-Object -First 40); $e | Write-Host; Set-Content -Path '%~dp0build_errors.txt' -Value $e -Encoding utf8"
if not "%RC%"=="0" ( echo   BUILD FAILED ^(see build_errors.txt^). & pause & exit /b 1 )

:: 5. Install into the dist folder. The CMake install copies the Qt6 DLLs + the plugin
::    families the app loads next to the exe — by hand, because this vcpkg Qt ships no
::    windeployqt.exe (only the qmake .prf). See the elseif(WIN32) block in CMakeLists.txt.
echo [4/5] Deploying Qt runtime into dist\DIAssetBrowser ...
set "OUT=%~dp0dist\DIAssetBrowser"
if exist "%OUT%" rmdir /s /q "%OUT%"
cmake --install "%~dp0build\release" --prefix "%OUT%" > "%~dp0build_log.txt" 2>&1
set "RC=%errorlevel%"
type "%~dp0build_log.txt"
if not "%RC%"=="0" ( echo   DEPLOY FAILED ^(see build_log.txt^). & pause & exit /b 1 )

:: Verify the deploy actually produced a runnable tree. Without platforms\qwindows.dll the exe
:: dies at startup with "no Qt platform plugin could be initialized" — and that would only be
:: discovered by whoever downloads the release, which is the worst possible place to find it.
if not exist "%OUT%\DIAssetBrowser.exe" (
    echo   [X] No exe in %OUT% - the install step produced nothing.
    pause & exit /b 1
)
if not exist "%OUT%\platforms\qwindows.dll" (
    echo   [X] platforms\qwindows.dll is MISSING from %OUT%.
    echo       The zip would build fine and fail to START on every machine.
    pause & exit /b 1
)
if not exist "%OUT%\Qt6Core.dll" (
    echo   [X] Qt6Core.dll is missing from %OUT% - Qt runtime was not copied.
    pause & exit /b 1
)

:: The MSVC runtime. vcpkg links everything against the DYNAMIC CRT, so the exe
:: needs these three. A developer machine always has them installed system-wide,
:: which is exactly why their absence from the zip went unnoticed: it ran here
:: and died on the first clean machine with "vcruntime140_1.dll was not found",
:: before a single line of our code executed. CMake's
:: InstallRequiredSystemLibraries now copies them; this verifies it did.
set "CRT_OK=1"
for %%D in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll) do (
    if not exist "%OUT%\%%D" ( echo   [X] %%D is MISSING from %OUT%. & set "CRT_OK=0" )
)
if "%CRT_OK%"=="0" (
    echo       The zip would start on THIS machine and fail on any without the
    echo       Visual C++ v14 Redistributable installed. Check that
    echo       InstallRequiredSystemLibraries found the redist in your VS install.
    pause & exit /b 1
)

:: The TLS backend. Settings ^> Prerequisites downloads over https; without this
:: plugin every request fails with "TLS initialization failed" in the RELEASE
:: build while working perfectly from the build tree.
if not exist "%OUT%\tls" (
    echo   [!] tls plugin folder missing - the in-app download will not work.
    echo       Everything else is unaffected; the app is fully usable offline.
)

echo   Deploy verified: exe + Qt6Core.dll + platforms\qwindows.dll + MSVC runtime.
if not exist "%OUT%\DIAssetBrowser.exe" ( echo   ERROR: exe missing in "%OUT%". & pause & exit /b 1 )
if exist "%~dp0RELEASE_README.txt" copy /y "%~dp0RELEASE_README.txt" "%OUT%\README.txt" >nul

:: 6. Zip it — version-stamped from main.cpp's setApplicationVersion, so releases don't overwrite
::    each other and a download's filename says what it is.
echo [5/5] Zipping...
set "APPVER="
for /f tokens^=2^ delims^=^" %%v in ('findstr /c:"setApplicationVersion" "%~dp0src\main.cpp"') do set "APPVER=%%v"
if "%APPVER%"=="" set "APPVER=dev"
set "ZIP=%~dp0dist\DIAssetBrowser_v%APPVER%.zip"
REM ZipFile::CreateFromDirectory, not Compress-Archive. Compress-Archive on Windows PowerShell
REM writes BACKSLASH path separators, which the zip spec says must be forward slashes. Windows
REM Explorer and 7-Zip cope; Linux/macOS unzip does not — it creates single files literally named
REM "DIAssetBrowser\platforms\qwindows.dll" instead of a folder tree. For a GitHub release that
REM anyone can download on any OS, that is a broken archive for some of them.
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; if (Test-Path '%ZIP%') { Remove-Item '%ZIP%' -Force }; [System.IO.Compression.ZipFile]::CreateFromDirectory('%OUT%', '%ZIP%', [System.IO.Compression.CompressionLevel]::Optimal, $true)"
if not exist "%ZIP%" ( echo   [X] Zip was not created. & pause & exit /b 1 )

echo.
echo ============================================================
echo  RELEASE OK.  (v%APPVER%)
echo   folder : %OUT%\   (exe + Qt DLLs/plugins, self-contained)
echo   zip    : %ZIP%
echo  Unzip anywhere and run DIAssetBrowser.exe. Point it at the Diablo
echo  Immortal install (File - Set game folder) on first run.
echo ============================================================
pause
