@echo off
setlocal enabledelayedexpansion
title Crack DI FX binding - drop a .dmp on this file
cd /d "%~dp0"

rem ============================================================================
rem  DRAG A DiabloImmortal .DMP ONTO THIS FILE.
rem
rem  It scans the process dump for the game's FX-attach records
rem    EffectCom/<ParticleSystem>:<AttachBone>:<StartTime>:<flags>:<transform>
rem  (byte-verified format, FX_RESEARCH.md) and reports everything tied to the
rem  sz11_008 crusader set that was loaded last:
rem    f_crusader_toukui_all / yifu / jianjia / tui _sz11_008  + weapon lianchui.
rem
rem  Capture the dump with the set VISIBLE on the character (Wardrobe preview or
rem  worn in-world) so the effect strings are resident.
rem
rem  Output: fx_dump_report.txt  (opens automatically). Send that back.
rem ============================================================================

set "DMP=%~1"
if "%DMP%"=="" (
    echo Drag a .dmp file onto this .bat, or pass the path as the first argument.
    echo   example:  crack-fx-dump.bat  C:\dumps\DiabloImmortal.dmp
    pause
    exit /b 1
)
if not exist "%DMP%" (
    echo File not found: "%DMP%"
    pause
    exit /b 1
)

rem --- find a Python launcher (py -> python -> python3) --------------------
set "PY="
where py       >nul 2>nul && set "PY=py -3"
if not defined PY ( where python  >nul 2>nul && set "PY=python" )
if not defined PY ( where python3 >nul 2>nul && set "PY=python3" )
if not defined PY (
    echo Python was not found on PATH. Install Python 3 ^(python.org^) and re-run,
    echo or run this by hand:
    echo   python tools\di_fx_dumpscan.py "%DMP%" --near sz11_008 ...
    pause
    exit /b 1
)

set "OUT=%~dp0fx_dump_report.txt"
set "SCAN=%~dp0tools\di_fx_dumpscan.py"

echo Scanning "%DMP%" ...
echo A live percentage + MB/s + ETA prints below as it streams the dump.
echo (a ~4 GB dump is a couple of minutes on the v2 scanner.)
echo.

rem  stdout (the report) -> file ; stderr (the progress bar) -> this console.
rem  Do NOT redirect stderr to nul or you lose the progress readout.
%PY% "%SCAN%" "%DMP%" ^
  --near sz11_008 ^
  --near lianchui ^
  --near toukui_all ^
  --near jianjia ^
  --near f_crusader ^
  --grep "fx_sz11_008[A-Za-z0-9_]*" ^
  --grep "[A-Za-z0-9_]*lianchui[A-Za-z0-9_]*" ^
  --grep "fx_[A-Za-z0-9_]*crusader[A-Za-z0-9_]*" ^
  --grep "fx_sz11_008_Doom[A-Za-z0-9_]*" > "%OUT%"

echo.
echo Done. Report written to:
echo   %OUT%
echo.
start "" notepad "%OUT%"
endlocal
