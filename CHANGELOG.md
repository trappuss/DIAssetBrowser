## 2.0

A full rework of the Wardrobe and the export pipeline, brought to parity with
D4AssetBrowser. First public release.

### Fixed
- **The 3D viewport turned edges almost black at glancing angles.** Two-sided lighting flipped the normal on `dot(N,V) < 0`, which is view-dependent — a front face whose normal-mapped detail grazed past the camera got flipped away from every light. It now flips on triangle winding.
- **"Export all sets" ignored the File names page.** It hardcoded `<class>_<key>` and never read the outfit or model templates.
- **"Export all sets" ignored "also export the matching opposite-gender item".** The setting was read into the exporter and then never acted on.
- **Opposite-gender outfits were both named for the same gender.** `barbarian_F_custom_F` beside `barbarian_F_custom_M`. The twin is now named from its own class, so `{{Gender}}` tells the pair apart with no suffix.
- **Genderless weapons got a `_F` tag with no `_M` anywhere.** Hand weapons live in a shared folder and have no counterpart; unpaired files keep their plain name.
- **A single-piece export wrote the whole outfit's raw dependencies, loose textures and a complete outfit twin** beside it.
- **Switching sets left the previous set's hair, weapon or attachments on the character.** Now toggleable in *Settings ▸ Export ▸ Wardrobe*.
- **"Weapons only" replaced the attachments too,** leaving two sets' worth on at once.
- **The Wardrobe's status line showed only the last message.** Twelve problems in a run reported one; it now keeps a bounded, de-duplicated list.
- **Main-hand and off-hand icons never appeared.** All rows were requested at once and evicted each other from a 512-entry queue.
- **Awakened sets could not be equipped at all.** `_aw1`–`_aw3` pieces folded onto the base set key, so no set entry ever put a whole awakened outfit on.
- **Settings had no way back.** Every control writes live and there was no undo; Cancel, Escape and the window X now all revert.
- Cloth physics could disagree between the viewport and the exporter after a settings change.
- The camera's projection reset itself on the first frame, and the orientation gizmo wrote one tab's setting from both.

### Added
- **Export set matches** — every alternative of the selected set as its own `.glb`, straight from the list beside it.
- **Export all sets options** — one file per set or per piece, exclude weapons, and optionally add every non-armour piece carrying a set key.
- **File counters on every export button.** `Export all sets (84)…` counts what the run will actually write, opposite-gender twins included — and only for the pieces that really have one.
- **Run reports** — a timestamped `_export_report_<date>.txt` beside the output listing every file written, everything that failed and why, and the settings used. *Settings ▸ Export ▸ All exports*.
- **Overwrite toggle.** Off writes `name_2.glb` instead of replacing, so a second run adds to a folder rather than rewriting it.
- **Parallel set export** with a shared texture cache, a cancel that finishes the files in flight, and a per-set crash guard — one bad asset costs that outfit, not the rest of the run.
- **Awakening levels in the Set list,** with per-tier toggles in *Settings ▸ Interface*.
- **Right-click everywhere in the Wardrobe** — equip a piece's set at a scope, step dye variants, export a piece, the outfit, or every set. The per-slot arrow and Export buttons are gone.
- **Per-slot lock** so the set picker leaves a piece you have chosen alone.
- **Character search,** and a Set matches list with 3D thumbnails, hover previews and right-click export.
- **Shared camera panel** on both 3D tabs: field of view, orthographic, axis snapping, numeric yaw/pitch, three presets and remember-on-relaunch.
- **Image and GIF capture** honouring every export setting — format, scale, transparent background, crop to model, palette, dither and a size budget.
- Texture preview showing a material's PBR channels or a texture's raw RGBA split, with hover-zoom, on all three tabs.
- Export prompts now open in the folder you last used.
- Settings gained Restore Defaults and a versioned schema, so an older profile is migrated rather than silently reset.

### Portability
- **The release zip now includes the Visual C++ runtime.** It was missing, so the tool started on a machine with the redistributable installed and failed on every machine without one, before any of its own code ran.
- **Settings ▸ Maintenance ▸ Prerequisites** checks the runtime and the OpenGL version, and can download Microsoft's installer. It is the only thing the app ever downloads; everything else works offline.
- Settings, caches and logs still live in `data\` beside the exe. No registry, no `%AppData%`.

### Build tooling
- `package-release.bat` — now fails the build if the MSVC runtime is missing from the output, the same way it already guarded `qwindows.dll`.
