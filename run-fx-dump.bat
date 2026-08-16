@echo off
setlocal enableextensions enabledelayedexpansion
title Run DIAssetBrowser + open FX tree dump
cd /d "%~dp0"

rem ============================================================================
rem  Launches the NEWEST DIAssetBrowser.exe build and, after you close it, opens
rem  the FX tree dump.
rem
rem  The dump now writes AUTOMATICALLY whenever a confirmed-FX model loads
rem  (lianchui / dun / fazhang / shoubi _sz11_008) — no env var required. The
rem  in-app FX status line also prints the exact dump path.
rem
rem  HOW TO USE:
rem    1) Double-click this .bat (keep it in the DIAssetBrowser project folder).
rem    2) In the app: Models tab -> load  lianchui_sz11_008  (FX box checked).
rem    3) Close the app. fx_tree_dump.txt opens automatically. Send it back.
rem ============================================================================

rem --- find the NEWEST DIAssetBrowser.exe under this folder (by modified time)-
set "EXE="
for /f "delims=" %%F in ('dir /b /s /a-d /o-d "DIAssetBrowser.exe" 2^>nul') do (
  if not defined EXE set "EXE=%%F"
)

if not defined EXE (
  echo Could not find DIAssetBrowser.exe under "%~dp0".
  echo Build the app first, or drop this .bat in the same folder as the .exe.
  pause
  exit /b 1
)

echo Newest build: "%EXE%"
for %%F in ("%EXE%") do set "EXEDIR=%%~dpF"
set "DUMP=%EXEDIR%fx_tree_dump.txt"

rem --- clear any old dump so you only get this run's tree --------------------
if exist "%DUMP%" del /q "%DUMP%"

echo.
echo Load  lianchui_sz11_008  in the Models tab (FX box checked), then CLOSE the app.
echo (DI_DUMP_FX is also set, in case an older build needs it.)
echo.

set "DI_DUMP_FX=1"
"%EXE%"

echo.
if exist "%DUMP%" (
  echo Dump written to:
  echo   %DUMP%
  start "" notepad "%DUMP%"
) else (
  echo No fx_tree_dump.txt was produced next to the exe.
  echo  - Make sure you rebuilt after the latest FxResolve.cpp / FxParser.cpp.
  echo  - Make sure you loaded lianchui_sz11_008 with the FX box checked.
  echo  - The app's FX status line shows the exact dump path if it wrote one.
  pause
)
endlocal
