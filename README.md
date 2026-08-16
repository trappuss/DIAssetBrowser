# DIAssetBrowser

A Diablo Immortal asset browser and 3D wardrobe studio. Reads your installed
game directly (MPK), decodes textures, and previews or exports models, outfits
and animations. **A native C++17 / Qt6 / OpenGL tool** — no Python, no runtime
to install, no launcher; unzip it and run it.

> Not affiliated with, endorsed by, or associated with Blizzard Entertainment
> or NetEase.

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
## Building from source

```bat
build.bat          :: first build - vcpkg resolves Qt6 + lz4, then runs it
rebuild.bat        :: incremental
package-release.bat:: portable folder + versioned zip in dist\
```

Visual Studio 2022 with "Desktop development with C++", and `VCPKG_ROOT` set to
a vcpkg checkout. The first build fetches Qt and takes a while; every one after
is minutes.

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
## Data and keys

No game assets, decryption keys or extracted content are included here or in
any release. The tool reads the copy of the game you installed yourself, and
nothing else.

DI names assets structurally — `f_barbarian_yifu_t07_004`. To see real in-game
set names, use **File ▸ Names ▸ Export set-key name template**, fill in the
third column, save it as `data\di_item_names.csv`, and **File ▸ Names ▸ Reload
names**.

---
## Credits

Built alongside [D4AssetBrowser](https://github.com/trappuss/D4AssetBrowser),
which it shares its architecture and much of its UI vocabulary with.

Every format claim in this tool was measured against the live game data rather
than assumed — the numbers in *Settings ▸ Information* are the ones the parser
actually produced.

## License

[MIT](LICENSE). Not affiliated with Blizzard Entertainment or NetEase.
