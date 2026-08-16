# Diablo Immortal Asset Browser — audit report (2026-08-01)

Audit only. No code written. Sources: the archived Python project, its logs/exports, and the live game install.

## 1. The archived DI project

`DiabloImmoralAssetBrowser\DiabloImmortal_AssetBrowser.Claude` — a **Python** tool (PySide6 + NumPy + PyOpenGL), ~11,000 lines, layered core/ingest/parsing/render/ui. Last run 2026-06-24.

**It reads the PC client, not just the mobile dump.** Two ingest paths exist:

- Legacy: DIDT-extracted Android tree (medium quality) — superseded.
- Current: `Package\MPK` of the Battle.net install — full quality. Entry points: `diasset/ingest/mpk.py` (archive reader) → `diasset/ingest/catalog.py` (name↔hash bridge) → `diasset/parsing/*`.

**Format knowledge, with the project's own confidence tags:**

| Format | File | Status |
|---|---|---|
| `.mpkinfo` index (version, fileCount, name/offset/length/pak per entry) | `ingest/mpk.py` | verified byte-exact against real Engine/Resources.mpkinfo |
| `.mpk` blobs stored **raw** — no container compression or encryption | `ingest/mpk.py` | verified (app.ico keeps ICO header, PhysX keeps "META") |
| `resource.repository` — master catalog: every asset's real path, name, type, 16-byte hash, and dependency hashes (Model → Mesh/Material/Texture/SkinSkeleton) | `parsing/repository_parser.py` | verified byte-exact vs the CucFlavius/Zee 010 template |
| Mesh: ZZZ4/LZ4 → `.MESSIAH` (NetEase Messiah engine), named vertex streams (`P3F_N4B_T2F` etc.), submesh table | `parsing/mesh_parser.py` | verified against real files (via TaylorMouse GriffonStudios layout) |
| `Texture2D`: 40-byte header, EPixelFormat enum (ASTC 4x4…12x12, ETC1/2), per-slice NNNN raw / ZZZ4 LZ4 mips | `parsing/texture_parser.py` | verified byte-exact vs 010 template + golden samples |
| `.SkinSkeleton`: bone names + 4x3 inverse-bind matrices | `parsing/skinskeleton_parser.py` | documented, and rigged exports exist (below) |
| ASTC decode: astc_encoder → texture2ddecoder → astc_decomp → `astcenc` CLI fallback chain | `parsing/astc.py` | working (texture2ddecoder installed on your box) |

**Proof it runs (printed numbers, not claims):**

- `mpk_asset_baseline_2026-06-18.txt`: **677,307** named assets indexed from the PC MPKs.
- `PC_exports\`: 29 `.glb` exports, including `*_RIGGED.glb` — mesh + skinning decode reached export at least once.
- `previews_new_2026-06-18\`: 5 rendered PNG previews — texture decode + GL render worked.
- `logs/pc_browser.log`: clean startups on 06-21 and 06-24 (numpy 2.4.4, PySide6 6.11.1, lz4, texture2ddecoder present).

**Known broken / stubbed:**

- The PC browser **currently crashes at startup**: circular import `pc_browser_panel` ↔ `pc_builder_panel` (`_DARK`, `_check_png_path`, `_ThumbWorker`). That is the last recorded state.
- `resolver.py` matches textures to meshes by *name convention* (`_d`/`_n`/`_m`/`_e` suffixes) — written for the DIDT tree, where the GUID table was missing. On PC the repository's dependency hashes are the correct route; both exist.
- Animation (`.anim`) and material parsers exist but nothing in the archive proves fidelity end-to-end.
- The builder/wardrobe half (character assembly, cloth demo) is out of scope for the new tool per the brief — ignore it.

## 2. The game install

`G:\G Games\Diablo Immortal` — **PC Battle.net client**, build **5.0.0.3665303** (from `.product.db`).

| Area | Contents |
|---|---|
| `Package\MPK` | **The content store.** 83 GB, 113 files: `Engine.mpk(+info)`, `Resources.mpk` … `Resources105.mpk` + `Resources.mpkinfo` (41.5 MB). |
| `Package\Movie`, `Package\Cursors` | Plain `.mp4`/`.srt` cinematics, `.cur` cursors — no decoding needed |
| `Data\`, `.battle.net\` | Battle.net download/patch cache (CASC-style `data/indices`) — not the asset source |
| `Engine\Binaries\Win64`, `LocalData\` | Game exe and per-user state — irrelevant |

Magic-byte spot checks, consistent with the archive's format docs:

- `Resources.mpkinfo` starts `01 00 00 00 | 66 8E 0A 00` → version 1, **691,814 entries** (677k after dropping length-0 directory placeholders).
- `Resources.mpk` byte 0 is a blob: `ZZZ4 … .MESSIAH` — raw storage confirmed, first entry is an LZ4-wrapped mesh.
- `Engine.mpk` byte 0 is readable ASCII (a config text file) — plain, unencrypted.

## 3. Known / inferred / unknown

**Known** (measured here or verified by the archive): PC client + build stamp; MPK/mpkinfo layout; raw unencrypted blob storage; `resource.repository` as full catalog with names, types and dependency graph; Messiah mesh layout; Texture2D/ASTC layout; 677k-asset index; working mesh→glb (incl. rigged) and texture→PNG at least for character assets.

**Inferred** (plausible, unmeasured): repository dependency hashes resolve Model→parts for *all* types, not just characters; texture formats in the PC build are overwhelmingly ASTC (the enum covers ETC too); `.mpkinfo` mtime + `.product.db` build stamp make a cheap update-detection signature.

**Unknown — the real output of this audit:**

1. Type distribution across the 677k assets (how many Mesh / Texture2D / Material / anim / other) — one script run against the index will answer it.
2. Whether every Texture2D in the PC build decodes with the existing parser (only character samples proven; PC may use formats the mip loop hasn't seen — the multi-frame-shape trap applies).
3. Material parse fidelity → correct PBR channel assignment in the viewport.
4. Animation decode quality (parsers exist, no verified output).
5. Whether any MPK region is locked/encrypted — nothing seen so far says yes, but only spot-checked.
6. How stable `pakField // 2` mapping is across all 106 Resources paks (verified for the samples the archive tested).

## 4. Section-9 answers now resolved from data

- **Client**: PC Battle.net install. No emulator involved. Mobile/DIDT path exists in the archive but is obsolete — the PC MPKs are the single source of truth.
- **Community data project**: nothing like d4data. References: CucFlavius/DIDT + 010 templates, TaylorMouse MaxScripts, MPKExtractor. Format knowledge is already ported into the archive's parsers.
- **Encryption**: none observed in the PC MPKs (unlike D4's TACT keys). Tentatively a non-issue.
- **Separate app vs second mode**: separate, as the brief recommends — nothing D4-format-specific survives; the seam interface (`enumerate / read-by-id / build-stamp`) is exactly what `MpkArchive + PcCatalog` already model.

## 5. Consequence for the port

The storage layer the brief says to design first already has a proven shape: port `mpk.py` + `repository_parser.py` + `catalog.py` to C++ behind the D4 tool's asset-index interface, and the entire upper stack (viewport, filters, list model, bulk extractor, glTF exporter) plugs in. The Python parsers are the spec; the C# tools and 010 templates in `.Resources&Research` are the tie-breakers when an offset is ambiguous.

Suggested first printed number once building starts: C++ index count == 677,307-class number from the same `.mpkinfo` set, diffed against `mpk_asset_baseline_2026-06-18.txt`.
