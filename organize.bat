@echo off
setlocal enabledelayedexpansion
title Organize DIAssetBrowser working directory
cd /d "%~dp0"

rem Tidies the repo root: archives obsolete one-off scripts and large dead-weight
rem files into _to_delete\ (reversible - nothing is deleted), and moves research
rem notes into docs\. Build/release scripts and config stay at the root where
rem they belong. Review _to_delete\ afterwards and delete it when satisfied.

echo Organizing DIAssetBrowser working directory...
echo.

if not exist "_to_delete\" mkdir "_to_delete"
if not exist "docs\" mkdir "docs"

echo [1/3] Archiving obsolete .bat files (v3 cracking is done; probes superseded)...
for %%F in (crack-anim-dump.bat crack-anim-dump2.bat build-probe.bat ^
    probe-anim-coverage.bat probe-anim-graph.bat probe-anim-tags.bat ^
    probe-tex-formats.bat probe-guid-blobs.bat capture-names.bat) do (
    if exist "%%F" ( move /Y "%%F" "_to_delete\" >nul && echo    archived %%F )
)

echo [2/3] Archiving large dumps + stale scratch/report files (~6.9 GB)...
for %%F in (DiabloImmortal.DMP DiabloImmortalold.DMP capture_strings.txt ^
    di_probe_names.txt codex.har name_candidates.csv ^
    dibrowser_log_20260801_114453.txt dibrowser_log_20260801_114507.txt ^
    di_probe_report.txt di_tex_formats.txt ^
    anim_coverage_report.txt anim_full_report.txt anim_graph_report.txt ^
    anim_tag_report.txt) do (
    if exist "%%F" ( move /Y "%%F" "_to_delete\" >nul && echo    archived %%F )
)

echo [3/3] Moving research notes to docs\...
for %%F in (DI_ANIM_V3_NOTES.md DI_ANIM_V3_DECOMPILED.txt ANIM_HANDOFF.md ^
    DI_AUDIT.md CAPTURE_NAMES_GUIDE.md LOCALIZATION_FINDINGS.md BAT_GUIDE.md) do (
    if exist "%%F" ( move /Y "%%F" "docs\" >nul && echo    moved %%F )
)

echo.
echo Done. The root now holds only the active build/release scripts, config,
echo and the source/build/tools folders.
echo.
echo   - Archived clutter is in _to_delete\  (delete that folder to reclaim ~6.9 GB)
echo   - Research notes are in docs\
echo   - Every .bat is categorized in docs\BAT_GUIDE.md
echo.
echo Nothing was permanently deleted; move anything back if you need it.
pause
