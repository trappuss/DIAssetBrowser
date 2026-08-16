# DIAssetBrowser scripts — categorized reference

Every `.bat`/helper in the project, grouped by purpose. After running
`organize.bat`, the **Active** scripts stay at the repo root and the
**Archived** ones move to `_to_delete\` (safe to delete once you're sure).

## Build & run (root — used constantly)
| Script | What it does |
|---|---|
| `build.bat` | One-time full configure + dependency build (run this first on a fresh checkout). |
| `rebuild.bat` | Incremental build, then deploy + launch. The everyday command. |
| `run.bat` | Deploy the built exe and launch it (no compile). |
| `backup-src.bat` | Snapshots `src\` into `.Backups\` before a build (recovery insurance). Called by `rebuild.bat`. |
| `verify-src.py` | Pre-build source sanity checks. Called by `rebuild.bat` (non-blocking). |

## Release
| Script | What it does |
|---|---|
| `package-release.bat` | Builds and packages a distributable release into `dist\`. |
| `Release Smoke Test.bat` | Post-package smoke test of the release build. |

## Utilities
| Script | What it does |
|---|---|
| `edit-settings.bat` | Opens the app's settings file for hand-editing. |

## Diagnostics (root — still useful)
| Script | What it does |
|---|---|
| `verify-anim.bat` | Runs the real parser over **every** `.anim` in the archive and audits v3 pose quality. Writes `anim_verify_report.txt`. This is the animation coverage check. |
| `probe-anim-all.bat` | Runs every animation probe mode (coverage / forensics / skin-map / verify) in one pass. Heavier; writes several reports. |

## Archived — obsolete or superseded (moved to `_to_delete\`)
| Script | Why it's archived |
|---|---|
| `crack-anim-dump.bat`, `crack-anim-dump2.bat` | Carved the v3 decoder out of the process dump. The v3 codec is fully cracked and implemented — no longer needed. |
| `build-probe.bat` | Superseded by `verify-anim.bat` / `probe-anim-all.bat`. |
| `probe-anim-coverage.bat`, `probe-anim-graph.bat`, `probe-anim-tags.bat` | All folded into `probe-anim-all.bat`. |
| `probe-tex-formats.bat`, `probe-guid-blobs.bat`, `capture-names.bat` | One-off investigations (texture formats, GUID blobs, string capture) that are done. |

## Also archived (large / stale data, `_to_delete\`)
- `DiabloImmortal.DMP`, `DiabloImmortalold.DMP` — the 6.5 GB process dumps used to crack v3 (the decompiled functions are preserved in `docs\DI_ANIM_V3_DECOMPILED.txt`).
- `capture_strings.txt`, `di_probe_names.txt`, `name_candidates.csv`, `codex.har` — large scratch outputs from the name-decoding work.
- Old logs and superseded reports (`dibrowser_log_*`, `anim_*_report.txt` from earlier runs, `di_probe_report.txt`, `di_tex_formats.txt`).

## Research notes (moved to `docs\`)
`DI_ANIM_V3_NOTES.md` (the definitive v3 codec spec), `DI_ANIM_V3_DECOMPILED.txt`
(decompiled decoder functions), `ANIM_HANDOFF.md`, `DI_AUDIT.md`,
`CAPTURE_NAMES_GUIDE.md`, `LOCALIZATION_FINDINGS.md`.
