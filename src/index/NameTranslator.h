#pragma once
// Pinyin filename gloss — the practical "in-game name" layer for this client.
//
// Why a dictionary and not the real localized item names: the shipped PC
// client carries NO plaintext name tables. Confirmed again 2026-08-02 with a
// full disk sweep — item/cosmetic marketing names are SERVER-SIDE, delivered
// at runtime, not on disk:
//   * Package/MPK has no localization / string-table / gamedata tree at all —
//     only Char, UIScript (Cocos .csb/.plist), Sounds, Storyline graphs,
//     Fonts and shaders. Engine/Content/Localization/cat.xml is the Messiah
//     EDITOR's category labels (Animation/Audio/…), not game text.
//   * The Battle.net CASC store (Data/, build-product "anbs") is a launcher
//     bootstrap: Data/data holds one 556 KB data.000, not the 83 GB of game
//     content — the MPKs are delivered by the NetEase updater, not CASC.
//   * LocalData/ is only a shader cache (Cache2) + an empty Icon dir.
//   * The mobile updater CDN (g67ena.update.easebar.com) is live but, like
//     DIDT/messiah-tools, serves only the ASSET repository we already parse.
//   (The undashed-GUID high-entropy MPK blobs measured in a prior session
//   remain the one unread frontier — reading them needs raw byte access we did
//   not have this session; even if decoded they are keyed by numeric IDs whose
//   text is the server's.)
//
// So the strongest DATA-DRIVEN name the client itself supports is the decoded
// asset grammar (cosmeticName below), and a real MARKETING name — datamined or
// crowd-sourced — is dropped in via ItemNames (data/di_item_names.csv), keyed
// on the set key the grammar extracts. There are only ~165 distinct set keys
// game-wide (measured), so that override file is small and finite.
//
// What the filenames DO carry is a consistent Chinese-pinyin vocabulary
// (yifu/toukui/jianjia/...). This maps every measured high-frequency token
// (top ~600 across all 551,524 repository names) to English, so the browser
// shows "chest armor" instead of "yifu" and search matches either language.
// Unknown tokens pass through unchanged — a gloss never guesses.

#include <QString>

namespace NameTranslator {

// English gloss of a display name ("Char/f_barbarian/f_barbarian_yifu_t07_004"
// -> "F barbarian chest armor t07 004"). Empty when no token translated —
// callers can skip storing/showing anything. `type` refines single-letter
// texture suffixes (_d/_n/_m/_e) for Texture2D rows.
QString translate(const QString& display, const QString& type);

// STRUCTURED true name of a player cosmetic model, decoded from the asset
// grammar with ZERO guessing. Measured 2026-08-02 across all 16,671 cosmetic
// class Models (99.2% decode): a name is
//   [m_|f_]<class>_<slot>[_<subtype>...]_<setfamily><n>[_<itemnum>][_<variant>...]
//   e.g. "f_barbarian_yifu_t07_004_aw3" -> "Barbarian (F) — Chest · set
//        t07_004 (Awakened III)".
// The set family+number (t07_004) is the game's real grouping KEY, kept
// verbatim. Variant codes are the measured vocabulary: aw1/2/3 = Awakened
// tiers, b1/b2 & g1/g2 = recolor tints (sibling materials, same mesh),
// ext/int = outer/inner garment forms, fx = effects. Any token NOT in the
// measured vocabulary is shown in [brackets] rather than guessed.
// Returns empty for non-cosmetic names. `setKeyOut` (optional) receives the
// bare set key (e.g. "t07_004") so callers can look up an external real name.
QString cosmeticName(const QString& display, QString* setKeyOut = nullptr);

// Friendly class name for a class folder leaf: "f_barbarian" -> "Barbarian (F)",
// "monk" -> "Monk". Empty when the leaf is not a recognised playable class.
QString classDisplay(const QString& folderLeaf);

// Facet keys for the browse filters, decoded from the SAME measured vocabulary
// cosmeticName uses (kCosClasses + slotGloss) — never a second, drifting copy.
struct Facets {
    QString cls;           // gender-neutral class: "Barbarian", "Crusader", …
    QString slot;          // "Chest", "Helmet", "Weapon", … ("" = not a cosmetic)
    bool    player = false;
};

// cat1/cat2 are the row's first two logical path segments. `player` is true for
// anything a player character can wear or hold: a recognised class folder
// (Char/monk, Char/f_barbarian), the shared gear folder (Char/item), the
// hair/face folders (Char/nielian, Char/dl_*), or any asset whose NAME decodes
// to a class — which is how Char/item weapons get classed.
Facets facetsOf(const QString& display, const QString& cat1, const QString& cat2);

} // namespace NameTranslator
