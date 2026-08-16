@echo off
setlocal enabledelayedexpansion
title DIAssetBrowser - update GitHub main
cd /d "%~dp0"

:: ============================================================================
::  gitupdate.bat - re-stage from the working folder, commit, push to main.
::
::  This file lives INSIDE the repo folder (_github). The working folder - the
::  one you actually edit and build in - is its parent. Every run:
::
::    1. re-copies the shipping files from the working folder into this one,
::       MIRRORING src/res/tools/docs so files you DELETED upstream also
::       disappear here (a plain copy would leave zombies in the repo forever)
::    2. refuses to continue if anything huge got staged
::    3. commits with your message
::    4. rebases onto origin/main, then pushes
::
::  Usage:   gitupdate.bat  fixed the normals at glancing angles
::           gitupdate.bat                  (prompts for the message)
::
::  WHAT IS NEVER COPIED: the game minidump, build\, dist\, data\, the _probe /
::  _golden / _fxwork / _to_delete scratch folders, backups, and the generated
::  reports. This script uses an ALLOWLIST - it names what to bring across
::  rather than copying everything and trying to exclude the bad parts. A
::  4 GB DiabloImmortal.DMP sits one directory up; an exclusion list that ever
::  gets a pattern wrong pushes it to GitHub, and an allowlist cannot.
:: ============================================================================

set "REPO=%~dp0"
set "WORK=%~dp0.."
set "ORIGIN=https://github.com/trappuss/DIAssetBrowser.git"

:: --- git present? ----------------------------------------------------------
where git >nul 2>&1
if errorlevel 1 (
    echo   ERROR: git is not on PATH. Install Git for Windows and reopen this window.
    pause & exit /b 1
)

:: --- repo initialised? -----------------------------------------------------
if not exist "%REPO%.git" (
    echo [init] No .git here yet - creating the repository on branch main...
    git init -b main || ( echo   ERROR: git init failed. & pause & exit /b 1 )
)
git remote get-url origin >nul 2>&1
if errorlevel 1 (
    echo [init] Adding remote origin - %ORIGIN%
    git remote add origin "%ORIGIN%" || ( echo   ERROR: could not add remote. & pause & exit /b 1 )
)

:: Committing with no identity configured fails with a wall of text that does
:: not obviously say "set your name". Say it here instead.
for /f "delims=" %%N in ('git config user.name 2^>nul') do set "GITNAME=%%N"
if not defined GITNAME (
    echo   ERROR: git has no identity configured. Run these once, then re-run:
    echo         git config --global user.name  "your name"
    echo         git config --global user.email "your@email"
    echo   NOTE: whatever email you set becomes PUBLIC on every commit. GitHub
    echo         gives you a no-reply address under Settings ^> Emails if you
    echo         would rather not publish your real one.
    pause & exit /b 1
)

:: --- 1. re-stage from the working folder ------------------------------------
:: Guarded: if the parent does not look like the project (someone cloned this
:: repo standalone), skip the sync entirely and just commit whatever is here.
if exist "%WORK%\CMakeLists.txt" if exist "%WORK%\src\main.cpp" goto :do_sync
echo [1/4] Parent folder is not the working tree - skipping sync, using files as they are.
goto :after_sync

:do_sync
echo [1/4] Syncing from the working folder...

:: Mirrored directories. /MIR makes the repo copy match exactly, including
:: removals. /NJH /NJS /NDL /NP keep the log to "what changed".
for %%D in (src res tools docs) do (
    if exist "%WORK%\%%D\" (
        robocopy "%WORK%\%%D" "%REPO%%%D" /MIR /NJH /NJS /NDL /NP >nul
        REM Robocopy uses exit codes as a BITMASK: 0-7 are success ^(1 = files
        REM were copied, 2 = extras removed^). Only 8 and above are failures.
        REM "if errorlevel 1" would call every successful sync an error.
        if !ERRORLEVEL! GEQ 8 (
            echo   ERROR: robocopy failed on %%D ^(code !ERRORLEVEL!^).
            pause & exit /b 1
        )
    ) else (
        echo   [!] %%D\ is not in the working folder - left as-is in the repo.
    )
)

:: Loose root files, named one by one. Add to this list when the repo gains a
:: root file that is authored in the working folder.
for %%F in (
    .gitignore
    CHANGELOG.md CMakeLists.txt CMakePresets.json vcpkg.json
    README.md README-DEV.md LICENSE RELEASE_README.txt
    D4_MIGRATION_PLAN.md DI_ANIM_V3_NOTES.md FX_ARCHIVE.md FX_RESEARCH.md
    di_item_names_template.csv verify-src.py dn.py
    build.bat rebuild.bat run.bat package-release.bat backup-src.bat
    edit-settings.bat organize.bat probe-anim-all.bat verify-anim.bat
    crack-fx-dump.bat run-fx-dump.bat
) do (
    if exist "%WORK%\%%F" (
        copy /y "%WORK%\%%F" "%REPO%%%F" >nul || (
            echo   ERROR: could not copy %%F. & pause & exit /b 1
        )
    ) else (
        echo   [!] %%F is not in the working folder - left as-is in the repo.
    )
)
:: Space in the name, so it cannot ride in the for-loop above.
if exist "%WORK%\Release Smoke Test.bat" copy /y "%WORK%\Release Smoke Test.bat" "%REPO%Release Smoke Test.bat" >nul

:: .gitattributes is authored HERE, not upstream - nothing copies over it.

:after_sync

:: --- 2. stage and sanity-check ---------------------------------------------
echo.
echo [2/4] Staging...
git add -A || ( echo   ERROR: git add failed. & pause & exit /b 1 )

git diff --cached --quiet
if not errorlevel 1 (
    echo   Nothing changed since the last commit.
    echo   Trying the push anyway in case a previous one did not complete.
    goto :push
)

echo.
git status --short
echo.

:: The size guard. GitHub warns over 50 MB and hard-rejects over 100 MB, and a
:: rejected push after the object is already committed means rewriting history
:: to get rid of it. Catch it while it is still only staged.
set "TOOBIG="
for /f "usebackq delims=" %%F in (`git diff --cached --name-only --diff-filter^=d`) do (
    REM git prints forward slashes; cmd's IF EXIST wants backslashes.
    set "FP=%%F"
    set "FP=!FP:/=\!"
    if exist "!FP!" (
        for %%S in ("!FP!") do if %%~zS GTR 52428800 (
            echo   [X] %%F is %%~zS bytes - too large for GitHub.
            set "TOOBIG=1"
        )
    )
)
if defined TOOBIG (
    echo.
    echo   Refusing to commit. Nothing has been committed or pushed; the files
    echo   above are staged only. Un-stage with:  git reset
    pause & exit /b 1
)

:: --- 3. commit --------------------------------------------------------------
set "MSG=%*"
if not defined MSG (
    echo Commit message ^(blank cancels^):
    set /p "MSG=  > "
)
if not defined MSG ( echo   Cancelled - nothing committed. & pause & exit /b 1 )

echo.
echo [3/4] Committing...
git commit -m "!MSG!" || ( echo   ERROR: commit failed. & pause & exit /b 1 )

:: --- 4. rebase then push ----------------------------------------------------
:push
echo.
echo [4/4] Fetching origin...
git fetch origin
if errorlevel 1 (
    echo   ERROR: could not reach origin. Check your connection / credentials.
    pause & exit /b 1
)

:: Replay local commits on top of whatever is on the remote. This is what turns
:: the "! [rejected] (fetch first)" push into a clean one. If the remote has a
:: commit touching the same file differently, the rebase STOPS and says so -
:: it does not silently pick a winner.
git rev-parse --verify origin/main >nul 2>&1
if not errorlevel 1 (
    echo       Rebasing onto origin/main...
    git rebase origin/main
    if errorlevel 1 (
        echo.
        echo   REBASE STOPPED - the remote changed the same lines you did.
        echo   Fix the conflicted files, then:   git add . ^&^& git rebase --continue
        echo   Or back out completely with:      git rebase --abort
        pause & exit /b 1
    )
)

echo       Pushing to main...
git push -u origin main
if errorlevel 1 (
    echo.
    echo   PUSH FAILED. If it says "rejected / fetch first" again, something
    echo   landed on the remote during this run - just re-run this script.
    pause & exit /b 1
)

echo.
echo ============================================================
echo  PUSHED to %ORIGIN%
for /f "delims=" %%C in ('git log -1 --oneline') do echo   %%C
echo ============================================================
pause
