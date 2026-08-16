@echo off
setlocal enabledelayedexpansion
title DIAssetBrowser - GitHub
cd /d "%~dp0"

:: ============================================================================
::  github.bat - everything this repo needs, from a menu. Double-click it.
::
::  This file lives INSIDE the repo folder (_github). The working folder - the
::  one you actually edit and build in - is its parent. "Update GitHub" will:
::
::    1. re-copy the shipping files from the working folder into this one,
::       MIRRORING src/res/tools/docs so files you DELETED upstream also
::       disappear here (a plain copy would leave zombies in the repo forever)
::    2. show you exactly what changed and wait for you to say yes
::    3. refuse to continue if anything huge got staged
::    4. commit with the message you type
::    5. rebase onto origin/main, then push
::
::  You can also pass the message straight in and skip the menu:
::      github.bat  fixed the normals at glancing angles
::
::  WHAT IS NEVER COPIED: the game minidump, build\, dist\, data\, the _probe /
::  _golden / _fxwork / _to_delete scratch folders, backups, and the generated
::  reports. This script copies from an ALLOWLIST - it names what to bring
::  across rather than copying everything and trying to exclude the bad parts.
::  A 4 GB DiabloImmortal.DMP sits one directory up; an exclusion list that ever
::  gets a pattern wrong pushes it to GitHub, and an allowlist cannot.
:: ============================================================================

set "REPO=%~dp0"
set "WORK=%~dp0.."
set "ORIGIN=https://github.com/trappuss/DIAssetBrowser.git"
set "WEBURL=https://github.com/trappuss/DIAssetBrowser"

call :preflight
if errorlevel 1 exit /b 1

:: A message on the command line means "just do it" - no menu.
if not "%*"=="" (
    set "MSG=%*"
    call :do_update
    pause
    exit /b 0
)

:: ============================================================================
:menu
cls
echo ============================================================
echo   DIAssetBrowser  -  GitHub
echo   %WEBURL%
echo ============================================================
call :show_state
echo.
echo   1   Update GitHub        sync, commit and push  ^(the usual one^)
echo   2   See what changed     no writes, nothing is sent
echo   3   Sync only            refresh this folder, do not commit
echo   4   Pull from GitHub     take changes made on the website
echo   5   Open the repo in your browser
echo   6   Git identity         name and email on your commits
echo   0   Exit
echo.
set "OPT="
set /p "OPT=  Choose: "

if "%OPT%"=="1" ( call :do_update  & call :hold & goto :menu )
if "%OPT%"=="2" ( call :do_status  & call :hold & goto :menu )
if "%OPT%"=="3" ( call :do_sync    & call :hold & goto :menu )
if "%OPT%"=="4" ( call :do_pull    & call :hold & goto :menu )
if "%OPT%"=="5" ( start "" "%WEBURL%" & goto :menu )
if "%OPT%"=="6" ( call :do_identity & call :hold & goto :menu )
if "%OPT%"=="0" exit /b 0
if "%OPT%"==""  goto :menu
echo   "%OPT%" is not one of the options.
call :hold
goto :menu

:: ============================================================================
:hold
echo.
echo   ---- press a key to go back to the menu ----
pause >nul
exit /b 0

:: ============================================================================
:: Everything that must be true before any menu option can work. Each failure
:: says what to do about it rather than letting git produce the error later.
:preflight
where git >nul 2>&1
if errorlevel 1 (
    echo.
    echo   git is not installed, or not on PATH.
    echo   Get it from https://git-scm.com/download/win, then run this again.
    echo.
    pause
    exit /b 1
)

if not exist "%REPO%.git" (
    echo.
    echo   This folder is not a git repository yet.
    set "YN="
    set /p "YN=  Set it up now, pointing at %ORIGIN% ? [Y/n] "
    if /i "!YN!"=="n" exit /b 1
    git init -b main || ( echo   git init failed. & pause & exit /b 1 )
    echo   Created, on branch main.
)

git remote get-url origin >nul 2>&1
if errorlevel 1 (
    git remote add origin "%ORIGIN%" || ( echo   Could not add the remote. & pause & exit /b 1 )
    echo   Remote origin set to %ORIGIN%.
)

:: Committing with no identity fails with a wall of text that never plainly
:: says "set your name", so ask for it here instead.
set "GITNAME="
for /f "delims=" %%N in ('git config user.name 2^>nul') do set "GITNAME=%%N"
if not defined GITNAME (
    echo.
    echo   git does not know who you are yet.
    call :do_identity
)
exit /b 0

:: ============================================================================
:show_state
set "BR=?"
for /f "delims=" %%B in ('git rev-parse --abbrev-ref HEAD 2^>nul') do set "BR=%%B"
set "DIRTY=0"
for /f %%C in ('git status --porcelain 2^>nul ^| find /c /v ""') do set "DIRTY=%%C"
set "WHO="
for /f "delims=" %%N in ('git config user.name 2^>nul') do set "WHO=%%N"
echo.
echo   branch  : !BR!
if "!DIRTY!"=="0" (
    echo   changes : none since the last commit
) else (
    echo   changes : !DIRTY! changed since the last commit
)
echo   commits : as !WHO!
exit /b 0

:: ============================================================================
:: Re-copy the shipping files from the working folder. Guarded: if the parent
:: does not look like the project (someone cloned this repo on its own), skip
:: the copy entirely rather than mangling the checkout.
:do_sync
echo.
if not exist "%WORK%\CMakeLists.txt" goto :sync_skip
if not exist "%WORK%\src\main.cpp"   goto :sync_skip
echo   Syncing from the working folder...

for %%D in (src res tools docs) do (
    if exist "%WORK%\%%D\" (
        robocopy "%WORK%\%%D" "%REPO%%%D" /MIR /NJH /NJS /NDL /NP >nul
        REM Robocopy uses its exit code as a BITMASK: 0-7 are all success
        REM (1 = files copied, 2 = extras removed). Only 8 and up are failures,
        REM so "if errorlevel 1" would call every successful sync an error.
        if !ERRORLEVEL! GEQ 8 (
            echo   [X] robocopy failed on %%D ^(code !ERRORLEVEL!^).
            exit /b 1
        )
    ) else (
        echo   [!] %%D\ is not in the working folder - left as it is here.
    )
)

:: Loose root files, named one at a time. Add to this list when the project
:: gains a root file that should ship.
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
            echo   [X] could not copy %%F. & exit /b 1
        )
    ) else (
        echo   [!] %%F is not in the working folder - left as it is here.
    )
)
if exist "%WORK%\Release Smoke Test.bat" copy /y "%WORK%\Release Smoke Test.bat" "%REPO%Release Smoke Test.bat" >nul

:: .gitattributes and this script are authored HERE - nothing copies over them.
echo   Sync done.
exit /b 0

:sync_skip
echo   The parent folder is not the working tree, so there is nothing to sync.
echo   Using the files in this folder as they are.
exit /b 0

:: ============================================================================
:do_status
echo.
call :do_sync
echo.
echo   ---- files that differ from the last commit ----
git add -A >nul 2>&1
git status --short
echo.
echo   ---- summary ----
git diff --cached --stat
echo.
echo   Nothing has been committed or sent. To undo the staging: git reset
exit /b 0

:: ============================================================================
:do_update
echo.
call :do_sync
if errorlevel 1 exit /b 1

echo.
echo   Staging...
git add -A || ( echo   [X] git add failed. & exit /b 1 )

git diff --cached --quiet
if not errorlevel 1 (
    echo   Nothing has changed since the last commit.
    echo   Checking whether the last push completed...
    goto :push
)

echo.
echo   ---- what will be committed ----
git status --short
echo.
git diff --cached --stat
echo.

:: The size guard. GitHub warns over 50 MB and hard-rejects over 100 MB, and by
:: the time it rejects, the object is already in your history - getting rid of
:: it then means rewriting history. Catch it while it is only staged.
set "TOOBIG="
for /f "usebackq delims=" %%F in (`git diff --cached --name-only --diff-filter=d`) do (
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
    echo   Stopping. Nothing was committed or sent; those files are staged only.
    echo   Un-stage everything with:  git reset
    exit /b 1
)

if not defined MSG (
    echo   Describe the change in one line. Blank cancels.
    set /p "MSG=  message: "
)
if not defined MSG ( echo   Cancelled - nothing committed. & exit /b 1 )

echo.
set "YN="
set /p "YN=  Commit and push this? [Y/n] "
if /i "!YN!"=="n" ( set "MSG=" & echo   Cancelled - nothing committed. & exit /b 1 )

echo.
echo   Committing...
git commit -m "!MSG!" || ( set "MSG=" & echo   [X] commit failed. & exit /b 1 )
set "MSG="

:push
echo.
echo   Fetching origin...
git fetch origin
if errorlevel 1 (
    echo   [X] Could not reach GitHub. Check your connection, or your saved
    echo       credentials if it asked for a login and failed.
    exit /b 1
)

:: Replay local commits on top of whatever is on the remote. This is what turns
:: "! [rejected] (fetch first)" into a clean push. If the remote changed the
:: same lines, the rebase STOPS - it never silently picks a winner.
git rev-parse --verify origin/main >nul 2>&1
if not errorlevel 1 (
    echo   Replaying your commits on top of GitHub's...
    git rebase origin/main
    if errorlevel 1 call :resolve_conflict
    if errorlevel 1 exit /b 1
)

echo   Pushing...
git push -u origin main
if errorlevel 1 (
    echo.
    echo   [X] Push failed. If it says "rejected / fetch first" again, something
    echo       landed on GitHub while this was running - just choose 1 again.
    exit /b 1
)

echo.
echo   ============================================================
echo    PUSHED
for /f "delims=" %%C in ('git log -1 --oneline') do echo     %%C
echo   ============================================================
exit /b 0

:: ============================================================================
:: A rebase stopped on a conflict. Walk through it instead of printing git
:: commands and leaving the repo half-rebased.
:resolve_conflict
echo.
echo   ------------------------------------------------------------
echo    STOPPED - GitHub and this folder both changed the same lines
echo   ------------------------------------------------------------
echo   Files needing a decision:
for /f "usebackq delims=" %%F in (`git diff --name-only --diff-filter=U`) do echo     %%F
echo.

:conflict_menu
echo   1   Open those files so I can fix them
echo   2   I have fixed them - carry on
echo   3   Give up and put everything back the way it was
echo.
set "CO="
set /p "CO=  Choose: "

if "%CO%"=="1" (
    for /f "usebackq delims=" %%F in (`git diff --name-only --diff-filter=U`) do (
        set "FP=%%F"
        set "FP=!FP:/=\!"
        start "" notepad "!FP!"
    )
    echo.
    echo   Each file has the two versions marked with rows of angle brackets.
    echo   Keep the text you want, delete the marker lines, save, then choose 2.
    echo.
    goto :conflict_menu
)

if "%CO%"=="2" (
    git add -A
    git rebase --continue
    if errorlevel 1 (
        echo.
        echo   Still conflicted - there is more than one commit to work through.
        echo.
        goto :conflict_menu
    )
    echo   Resolved.
    exit /b 0
)

if "%CO%"=="3" (
    git rebase --abort
    echo   Put back. Your commit is still here, it just has not been pushed.
    exit /b 1
)

echo   "%CO%" is not one of the options.
goto :conflict_menu

:: ============================================================================
:: Take changes made on the website (editing the README in the browser, say)
:: without touching anything local that is not committed.
:do_pull
echo.
git diff --quiet
if errorlevel 1 (
    echo   You have uncommitted changes here. Commit them first ^(option 1^),
    echo   otherwise a pull could collide with them.
    exit /b 1
)
echo   Fetching...
git fetch origin || ( echo   [X] Could not reach GitHub. & exit /b 1 )
git rebase origin/main
if errorlevel 1 call :resolve_conflict
if errorlevel 1 exit /b 1
echo.
echo   Up to date with GitHub:
for /f "delims=" %%C in ('git log -1 --oneline') do echo     %%C
echo.
echo   NOTE: this updates the repo folder only. If you pulled a change to a
echo         source file, copy it back into the working folder by hand - the
echo         sync in option 1 goes the other way and would overwrite it.
exit /b 0

:: ============================================================================
:do_identity
echo.
echo   Every commit carries a name and an email, and both are PUBLIC on GitHub.
echo   If you would rather not publish your real address, GitHub gives you a
echo   no-reply one under Settings - Emails - "Keep my email addresses private".
echo.
set "CURN=" & set "CURE="
for /f "delims=" %%N in ('git config user.name 2^>nul')  do set "CURN=%%N"
for /f "delims=" %%E in ('git config user.email 2^>nul') do set "CURE=%%E"
if defined CURN echo   current name  : !CURN!
if defined CURE echo   current email : !CURE!
echo.
set "NN="
set /p "NN=  Name  (blank keeps it): "
if defined NN git config --global user.name "!NN!"
set "NE="
set /p "NE=  Email (blank keeps it): "
if defined NE git config --global user.email "!NE!"
echo.
echo   Saved.
exit /b 0
