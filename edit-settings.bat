@echo off
:: NOTE: File > Settings now has in-app Hotkeys and Hover tabs - use those
:: instead of editing by hand. This .bat remains for advanced/other keys.
:: Open the tool's settings file (data\DIAssetBrowser.ini beside the exe) in
:: Notepad. Hover-preview knobs live under [hover] and have no UI yet:
::   enabled=true        master gate
::   delaySec=0.5        dwell before the popup shows (0..5)
::   imagePreview=true   include the thumbnail image
::   scrollZoom=true     wheel over the popup resizes it
::   previewPx=256       image size (64..1024)
::   colour=true         colour-coded info lines
:: Per-line toggles, e.g.:  tex\meaning=false   mdl\path=false
:: Export hotkeys live under [hotkeys]: exportSelection / exportToLast /
:: saveImage (Qt key-sequence text, e.g. Ctrl+E).
:: The app reads these keys live on the next hover / menu open - no restart
:: needed for hover knobs; hotkey changes apply at next launch.
setlocal
set "INI=%~dp0build\release\data\DIAssetBrowser.ini"
if not exist "%INI%" (
    echo Settings file not found yet: %INI%
    echo Run the app once first - it creates the file on first launch.
    pause & exit /b 1
)
start notepad "%INI%"
