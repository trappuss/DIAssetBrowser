# Diablo Immortal — where item/cosmetic names live (measured)

Research pass 2026-08-02. Goal: true, data-driven item names, no guessing.
This documents exactly what was checked so we never re-chase a phantom
"encrypted names file."

## TL;DR

The PC client **does not ship localized item or cosmetic name text on disk.**
Those names ("Dishonored Legionnaire", "Warlord's Fury") are **server-side**,
delivered to the running client at runtime. This is the honest, measured
reason the tool has only pinyin asset names.

Two consequences, both now built into the tool:

1. **Structural true names** — the asset name grammar is itself game data and
   decodes, with zero guessing, to a precise identity:
   `f_barbarian_yifu_t07_004_aw3` → *"Barbarian (F) — Chest · set t07_004
   (Awakened III)"*. 99.2% of the 16,671 cosmetic models decode; unglossed
   tokens are shown in `[brackets]`, never invented. (`NameTranslator::cosmeticName`)

2. **External real-name override** — a small `data/di_item_names.csv` maps the
   set key (e.g. `t07_004`) to the in-game marketing name and takes priority
   when present. There are only **165 distinct set keys game-wide**, so this is
   a finite, fillable list — the d4data-style bridge for datamined/crowd-sourced
   names. (`ItemNames`, and **Names → Export cosmetic-set name template…**)

## What was checked, and what was found

| Source | Result |
|---|---|
| `Package/MPK/Resources*` (677k entries) | Assets, UI (Cocos `.csb`/`.plist`), sounds, Storyline graphs, fonts, shaders. **No** localization / string-table / gamedata tree. No `.xml` catalogs, no `String*` tables. |
| `Engine/Content/Localization/cat.xml` | The Messiah **editor's** category labels (Animation/Audio/Character…), en + zh. Not game item text. |
| Battle.net CASC `Data/` (product `anbs`) | `config`/`data`/`indices`/`ecache`. `Data/data` is a single **556 KB** `data.000` — a launcher bootstrap, not the 83 GB of game content. The MPKs are delivered by the NetEase updater, not stored in CASC. |
| `LocalData/` | `Cache2` is a compiled-shader cache; `Icon/` is empty; `Config` is an encrypted/high-entropy blob. No gamedata cache. |
| Mobile updater CDN (`g67ena.update.easebar.com`, live) | `mbdl` returns current CDN config. The patch-list → index → repository path (used by CucFlavius/DIDT and fangfang1984/messiah-tools) yields only the **asset repository we already parse** — no separate strings/gamedata bundle was found. |
| Public datamining tools | DIDT is a CDN *downloader* for the asset repository; messiah-tools covers MPK/texture only. **Neither parses item names** — this is genuinely unsolved territory, because the names aren't in the client. |

## The asset-name grammar (measured across all 20 class folders)

```
[m_|f_]<class>_<slot>[_<subtype>...]_<setfamily><n>[_<itemnum>][_<variant>...]
```

- **gender**: `m_` / `f_` / none
- **class**: barbarian, monk, crusader, necromancer, wizard/sorceress,
  demonhunter, druid, tempest, warlock, bloodknight
- **slot**: yifu=Chest, toukui=Helmet, jianjia=Shoulders, tui=Legs,
  toufa=Hair, bijia=Bracers, wuqi=Weapon, … (full table in NameTranslator)
- **subtype**: all=full, half, parts, back, left, right, fujian=accessory, …
- **set key**: `sz##`, `t##`, `s##`, `ty##` + item number — the game's real
  grouping id, kept verbatim (no invented meaning for the family letters)
- **variant** (measured vocabulary, near-equal counts confirm structure):
  `aw1/aw2/aw3` = Awakened tiers I/II/III (1206/1204/1144),
  `b1/b2` + `g1/g2` = recolor tints (sibling materials over the same mesh),
  `ext/int` = outer/inner garment forms, `fx` = effects.

## How to get real marketing names in (tonight or later)

1. **Names → Export cosmetic-set name template…** writes every current set key
   with its structural name as a comment and an empty cell:
   ```
   # Barbarian (F) — Chest · set t07_004
   t07_004,,
   ```
2. Fill the third column with the in-game name (from the game's Codex/【图鉴】,
   the shop, or a community list). Blank class = applies to all classes; add a
   class (`t07_004,barbarian,Warlord's Fury`) to override just one.
3. Save as `data/di_item_names.csv` and use **Names → Reload**. The Meaning
   column now shows the real names; the index cache keys on the file's
   fingerprint so edits always take effect.

## The one remaining frontier

The high-entropy undashed-GUID MPK blobs measured in an earlier session are the
only unread candidate for on-disk gamedata. Reading them needs raw byte-range
access to the 838 MB `Resources*.mpk` files (not available this session), and
even decoded they would be keyed by numeric IDs whose *text* is the server's —
so they are unlikely to yield names without the server data. The realistic path
to marketing names remains the external override file above.
