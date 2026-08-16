@echo off
setlocal
cd /d "%~dp0"
:: Best-effort src snapshot into .Backups\src_<date>_<time>. Never blocks a build.
set "TS=%date:~-4%%date:~4,2%%date:~7,2%_%time:~0,2%%time:~3,2%%time:~6,2%"
set "TS=%TS: =0%"
set "DEST=.Backups\src_%TS%"
if not exist ".Backups" mkdir ".Backups" >nul 2>&1
xcopy /e /i /q /y "src" "%DEST%\src" >nul 2>&1
exit /b 0
