# DIAssetBrowser

A Diablo Immortal asset browser and 3D wardrobe studio. Reads your installed
game directly (MPK), decodes textures, and previews or exports models, outfits
and animations. **A native C++17 / Qt6 / OpenGL tool** — no Python, no runtime
to install, no launcher; unzip it and run it.

> Not affiliated with, endorsed by, or associated with Blizzard Entertainment
> or NetEase. For personal use with a copy of Diablo Immortal that you own.
> **No game assets and no decryption keys are included in this repository.**

---
## Quick start

1. Download the release zip and unzip it anywhere — a USB stick is fine.
2. Run `DIAssetBrowser.exe`.
3. **File ▸ Set game folder…** and pick your Diablo Immortal install (or its
   `Package\MPK` folder).
4. Wait a few seconds for the index — the status bar shows the asset count when
   it is ready. It is cached, so later launches are instant.
5. Start on **Wardrobe** if you want outfits, **Models** if you want a specific
   asset.

Windows 10/11 64-bit. A GPU with OpenGL 3.3 for the 3D viewport — everything
else works without one. The Visual C++ runtime ships inside the zip, so there
is nothing to install; *Settings ▸ Maintenance ▸ Prerequisites* checks both and
can fetch the runtime if it is ever missing.

Windows SmartScreen will warn about an unsigned executable from a new
publisher. That is expected for an unsigned hobby build — "More info" then "Run
anyway", or check the file yourself first.

Version history: **[CHANGELOG.md](CHANGELOG.md)**.

---
## Tabs

| Tab | What it does |
|---|---|
| **Assets** | Every one of the ~677,000 catalogue entries. Search, filter by type, inspect the raw record. |
| **Textures** | Decode any texture — BC7 and the rest — with per-channel isolation, mip picker, atlas frames and PNG export. |
| **Models** | A live 3D viewport with the real bone hierarchy, animation playback, per-submesh visibility, attachments and `.glb` export. |
| **Wardrobe** | Assemble a character: pick a class, equip a set or individual pieces, dye variants, play the class animations, export the outfit. |
| **Bulk Extract** | Point it at a filter and let it write thousands of files — raw original bytes, decoded PNGs, or rigged `.glb`. |

Each is covered below.

---
## Features by tab

### Wardrobe

**Sets.** Pick a class and a set; the picker ranks the real garment above its
sub-attachments and auto-equips every matching piece, so outfits come out
complete. Awakened tiers (`_aw1`–`_aw3`) are their own entries, toggleable in
*Settings ▸ Interface*.

**Set matches.** A set often ships more candidates than the slots can show —
a dozen main-hand weapons is normal. The matches list shows every one with a 3D
thumbnail, click to equip, right-click to export without equipping.

**Per-slot control.** Right-click any slot to equip that piece's set at a
scope (everything / armour / weapons / attachments), step its dye variants,
export it alone, or lock it so the set picker leaves it be.

**Gender switching** re-equips the same pieces on the other rig, and the
**equip theme** menu lets you take a set's weapons without losing the armour
you already assembled.

### Models

**Transport bar** — step, play, scrub, frame spinner, speed, loop. Wheel on the
timeline moves exactly one frame; Shift+wheel changes speed.

**Parts panel** with live triangle counts, and attachments: sibling models in
the same folder, each with its own skeleton and its own clip.

**Cloth physics** for cape, tail and hair bones — simulated in the viewport and
baked into the export, so the file matches what you were looking at.

### Textures

Channel isolation (R/G/B/A), alpha checkerboard, mip picker, fit / wheel-zoom /
drag-pan, and a live pixel inspector reading coordinates and RGBA under the
cursor. **Texture atlases** are read from the sibling Cocos plist, so frames
come with real names and real rectangles rather than guessed ones.

### Bulk Extract

Queue models, textures or raw blobs, run it in parallel, pause and resume, and
skip what you already have. Writes a manifest so a second run only does what is
new.

---
## Panels

Every 3D tab has a strip of toggles down the right edge. Panels stack, drag to
size, reorder with ▲▼, and the layout persists.

| Tab | Panels |
|---|---|
| **Models** | INFO · MATERIALS · TEXTURE PREVIEW · PARTS · ATTACHMENTS · ANIMATIONS |
| **Wardrobe** | INFO · MATERIALS · TEXTURE PREVIEW · PARTS · EQUIPPED · ANIMATIONS |
| **Textures** | ASSOCIATED MODELS · MIPMAPS · TEXFRAMES |

- **TEXTURE PREVIEW** shows a material's PBR channels, or one texture split into
  its raw RGBA — DI packs unrelated maps per channel, and a packed channel is
  invisible until you look at it alone.
- **ASSOCIATED MODELS** inverts the whole repository to answer "what uses this
  texture", with Reveal in Models tab.

---
## Exporting

**Models and outfits** export as `.glb` with the real bone hierarchy, the
skinning, and whichever animations you ask for — the clip playing, every clip
the model owns, or the class rig's clips that armour inherits.

**Everything is a setting.** *Settings ▸ Export* covers embedded or loose
textures, raw game blobs beside the file, folder layout, file-name templates,
the opposite-gender counterpart, and whether to overwrite.

**Bulk set export.** One `.glb` per set, or per piece, optionally adding every
non-armour piece carrying a set key. The buttons carry the file count, so
`Export all sets (84)…` is what you will actually get. Runs in parallel, can be
cancelled, and writes a report of everything it did.

**Images and GIFs** from the viewport — still, turntable, or the animation loop
— honouring transparent background, crop-to-model, palette and size budget.

Shortcuts: `Ctrl+E` export selection, `Ctrl+Shift+E` to the last folder,
`Ctrl+Shift+A` animations only, `Ctrl+Shift+I` save image. Rebindable in
*Settings ▸ Hotkeys*.

---
## Honest limitations

**Cosmetic particle effects are not rendered or exported.** DI keeps them in the
`ParticleSystem` type (folder `EffectCom`, ~43,900 entries), and that container
is byte-level cracked — but *which* effect a worn cosmetic uses is applied from
the encrypted client item table at equip time and appears in no readable asset.
A real-time re-simulation was built and then removed: without that binding table
it could not be made faithful, and a not-quite-right fire is worse than none.
What the FX toggle does show is the material's own authored animated emissive
layer, which is read straight from the material constants and is always correct.
The full research is preserved in [FX_ARCHIVE.md](FX_ARCHIVE.md) and summarised
in *Settings ▸ Information*.

**The Models and Textures tabs ignore their file-name templates.** *Settings ▸
Export ▸ File names* has rows for `Models (.glb)` and `Textures`; today only the
Wardrobe's exports read them. The other two tabs use their own naming, so
editing those two rows changes nothing.

**Asset names are structural, not the ones you see in game.** DI ships
`f_barbarian_yifu_t07_004`, not "Warcry of the Ancients" — see *Data and names*
below for how to supply the real ones.

**Windows only.** The build assumes MSVC (the SEH guard, the resource compiler
and the packaging all depend on it).

---
## Portable — everything lives in `data\`

The tool writes nothing outside the folder it runs from. No registry keys, no
`%AppData%`.

| | |
|---|---|
| `data\DIAssetBrowser.ini` | every setting |
| `data\DIAssetBrowser.log` | runtime log, truncated each launch |
| `data\index_v1.cache` | the asset index, so later launches are instant |
| `data\di_item_names.csv` | optional: real cosmetic names |

Delete the folder and nothing of the tool is left behind. Copy it to another
machine and it remembers everything.

---
## Data and names

No game assets, decryption keys or extracted content are included here or in
any release. The tool reads the copy of the game you installed yourself, and
nothing else. The one thing it ever downloads is Microsoft's Visual C++
redistributable, from *Settings ▸ Maintenance ▸ Prerequisites*, and only if you
press the button — everything else works with no network at all.

DI names assets structurally — `f_barbarian_yifu_t07_004`. To see real in-game
set names, use **File ▸ Names ▸ Export set-key name template**, fill in the
third column, save it as `data\di_item_names.csv`, and **File ▸ Names ▸ Reload
names**. `di_item_names_template.csv` in this repo is a starting point, and
[docs/CAPTURE_NAMES_GUIDE.md](docs/CAPTURE_NAMES_GUIDE.md) covers recovering
names from a client capture.

---
## What's in this repository

```
src/store/    framework-free storage seam (std + lz4, no Qt)
              Zzz4 · MpkIndex · Repository · AssetStore
src/index/    catalogue, list/outliner models, name translation, thumbnails
src/model/    mesh, material, skeleton and animation parsers · cloth solver
              · glTF exporter
src/tex/      BC/ASTC decode, DI pixel formats, Cocos atlas plists
src/gl/       OpenGL viewport, thumbnail renderer, GIF encoder
src/tabs/     Assets · Textures · Models · Wardrobe · Bulk Extract
src/app/      main window, settings, export plumbing, prerequisites, SEH guard
src/util/     shared UI and export helpers (filters, name templates, panels)
tools/        console provers and research scripts
docs/         format research and findings
res/          Windows resource + version info
```

Header-only files sit beside their implementations; anything in `src/store` is
deliberately Qt-free so the console provers can link it.

### Build and release scripts

| | |
|---|---|
| `build.bat` | first / full build — vcpkg resolves qtbase + lz4, then launches |
| `rebuild.bat` | incremental edit→test loop, distilling errors to `build_errors.txt` |
| `run.bat` | launch the build-tree exe with the Qt DLLs and plugins staged beside it |
| `package-release.bat` | portable folder + versioned zip in `dist\`, with deploy verification |
| `Release Smoke Test.bat` | builds the release, then tests it unzipped into a clean folder **outside** the repo — the way a downloader actually gets it |
| `verify-src.py` | pre-compile source check: brace balance, header-only includes, format args, Qt macro names (`emit`/`slots` as identifiers), duplicate lambdas |
| `backup-src.bat` | best-effort `src` snapshot into `.Backups\` |
| `organize.bat` | tidies the root — archives one-off scripts into `_to_delete\`, research notes into `docs\` |
| `edit-settings.bat` | opens `data\DIAssetBrowser.ini` for the few keys with no UI |
| `github.bat` | menu-driven sync + commit + push of this repo folder |

### Provers and research tooling

Every format claim in this tool was measured rather than assumed, and these are
what did the measuring.

| | |
|---|---|
| `tools/di_probe.cpp` | the console prover. Compiles the app's **real** `AnimParser`, so a verdict on a format fix comes from the code that ships, not a reimplementation that could drift. |
| `tools/di_dumpcarve.cpp` | minidump carver — pulls the packed decoder out of a live-process `.DMP`. |
| `probe-anim-all.bat` | every animation probe mode (coverage, forensics, tags, graph) in one `di_probe` run. |
| `verify-anim.bat` | runs the real parser over every `.anim` in the archive — ~37,769 clips — then audits each v3 clip for pose quality. Writes `anim_verify_report.txt`. |
| `crack-fx-dump.bat` · `run-fx-dump.bat` · `tools/di_fx_dumpscan.py` | FX research: scans a process dump for `EffectCom` attach records to recover which effect a worn piece binds to. |
| `tools/di_names_from_capture.py` · `dn.py` | recovers real cosmetic names from a client capture. |

### Format research

| | |
|---|---|
| [docs/DI_AUDIT.md](docs/DI_AUDIT.md) | the audit that set this project's discipline |
| [DI_ANIM_V3_NOTES.md](DI_ANIM_V3_NOTES.md) · [docs/DI_ANIM_V3_NOTES.md](docs/DI_ANIM_V3_NOTES.md) · [docs/DI_ANIM_V3_DECOMPILED.txt](docs/DI_ANIM_V3_DECOMPILED.txt) | the v3 animation container, byte by byte |
| [docs/ANIM_HANDOFF.md](docs/ANIM_HANDOFF.md) | animation resolution: which clips a model may use, and why |
| [FX_ARCHIVE.md](FX_ARCHIVE.md) · [FX_RESEARCH.md](FX_RESEARCH.md) · [docs/FX_NOTES.md](docs/FX_NOTES.md) | the particle-effect work, kept for anyone who later gets the item table |
| [docs/LOCALIZATION_FINDINGS.md](docs/LOCALIZATION_FINDINGS.md) | where the client keeps display strings |
| [docs/CAPTURE_NAMES_GUIDE.md](docs/CAPTURE_NAMES_GUIDE.md) | recovering real cosmetic names |
| [docs/BAT_GUIDE.md](docs/BAT_GUIDE.md) | what each script in the root is for |
| [D4_MIGRATION_PLAN.md](D4_MIGRATION_PLAN.md) · [README-DEV.md](README-DEV.md) | parity plan against D4AssetBrowser, and dev notes |

---
## Building from source

```bat
build.bat          :: first build - vcpkg resolves Qt6 + lz4, then runs it
rebuild.bat        :: incremental
package-release.bat:: portable folder + versioned zip in dist\
```

**Requirements:** Visual Studio 2022 or 2026 with "Desktop development with
C++", CMake 3.21+, and `VCPKG_ROOT` pointing at a vcpkg checkout. The first
build fetches Qt and takes a while; every one after is minutes.

**Dependencies**, all resolved by vcpkg from [`vcpkg.json`](vcpkg.json):

- `qtbase` with `widgets`, `opengl`, `gui`, `png`, `jpeg`, `network`
- `lz4`

`network` is there for exactly one thing — *Settings ▸ Maintenance ▸
Prerequisites* fetching the Visual C++ redistributable. Drop the feature and
CMake will not configure, because `Qt6::Network` is on the link line.

The release build bundles the MSVC runtime beside the exe via
`InstallRequiredSystemLibraries`, and `package-release.bat` **fails the build**
if those DLLs are not in the output — the zip is meant to run on a machine that
has never had a compiler on it.

---
## Keeping up with the project

- **[Issues](https://github.com/trappuss/DIAssetBrowser/issues)** — bugs and
  requests. A model that fails to load or exports wrong is worth reporting;
  attach `data\DIAssetBrowser.log`.
- **[Releases](https://github.com/trappuss/DIAssetBrowser/releases)** — watch
  the repo to be told about new builds.

**After a game patch:** the index is keyed to the game build and rebuilds itself
when it notices the store changed. If something looks stale, delete
`data\index_v1.cache` and relaunch.

---
## Credits

Built alongside [D4AssetBrowser](https://github.com/trappuss/D4AssetBrowser),
which it shares its architecture and much of its UI vocabulary with.

Every format claim in this tool was measured against the live game data rather
than assumed — the numbers in *Settings ▸ Information* are the ones the parser
actually produced.

## License

[MIT](LICENSE). Not affiliated with Blizzard Entertainment or NetEase.
