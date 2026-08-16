#pragma once
// External real-name overrides — the d4data-style bridge.
//
// WHY THIS EXISTS (measured 2026-08-02, no guessing): the shipped PC client
// carries NO localized item/cosmetic name text. Verified this session — game
// text is absent from Package/MPK (only assets/UI/audio/fonts/shaders; no
// localization, string-table, or gamedata tree), the Battle.net CASC store is
// a 556 KB launcher bootstrap (data.000) not game data, and LocalData holds
// only shader caches. Cosmetic marketing names ("Dishonored Legionnaire") live
// on the server and are delivered at runtime. So the asset grammar
// (NameTranslator::cosmeticName) gives the precise STRUCTURAL true name, and
// this layer lets a real MARKETING name — datamined or crowd-sourced later —
// be dropped in and take priority, keyed on the stable join the client DOES
// expose: the set key (e.g. "t07_004"), optionally narrowed by class.
//
// File: data/di_item_names.csv beside the executable. One row per mapping:
//   setKey,class,name
//   t07_004,,Dishonored Legionnaire        # blank class = all classes
//   sz08_001,barbarian,Warlord's Fury      # class-specific override
// Lines starting with '#' and blank lines are ignored. There are only ~165
// distinct set keys game-wide (measured), so this file is small and finite.
// ExportTemplate() writes every current key with its structural name so the
// list stays correct across patches.

#include <QHash>
#include <QString>

#include <memory>

#include "index/AssetIndex.h"

namespace ItemNames {

// Loaded override table. STATELESS by design: each AssetIndex build owns its
// own Table on its worker thread, so there is no shared mutable global to race
// across overlapping builds (audit finding).
struct Table {
    // key = "<setkey>" (all classes) or "<setkey>|<class>" (class-specific)
    QHash<QString, QString> byKey;
    bool empty() const { return byKey.isEmpty(); }
    int  size()  const { return byKey.size(); }
};

// Load overrides from `csvPath` (missing file = empty Table, not an error).
Table load(const QString& csvPath);

// A short fingerprint (size+mtime) of the overrides file, folded into the
// index-cache signature so editing di_item_names.csv rebuilds the cache and
// re-applies the names. "none" when the file is absent.
std::string fingerprint(const QString& csvPath);

// Real marketing name for a cosmetic display name, or empty when none is on
// file. The set key + class are derived from `display`; a class-specific row
// wins over a blank-class row for the same key.
QString nameFor(const Table& table, const QString& display);

// Real marketing name for a set key directly (e.g. "sz08_007"), optionally
// narrowed by class (e.g. "barbarian", lowercased); empty when none on file.
// Used by the Wardrobe set picker, where the key is already known.
QString nameForSetKey(const Table& table, const QString& setKey,
                      const QString& className = QString());

// Write a CSV template of every distinct cosmetic set key currently in the
// store, each with its structural name as a comment and an empty name cell to
// fill in. Returns rows written, or -1 on write failure.
int exportTemplate(const std::shared_ptr<AssetIndex>& idx, const QString& csvPath);

} // namespace ItemNames
