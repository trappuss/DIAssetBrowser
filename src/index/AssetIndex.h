#pragma once
// AssetIndex — the loaded store plus the per-entry UI metadata derived from it.
// Built once on a background thread (MainWindow::reload), then handed to the
// tabs as an immutable shared snapshot. Rebuilt wholesale on game-folder change
// or reload; nothing mutates it in place.

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <memory>

#include "store/AssetStore.h"

// One browsable row. `entryId` indexes DiAssetStore::mpk().entries().
// GUID-named blobs that resolve through the repository get the logical
// repository path as display name; everything else shows its MPK path.
struct AssetRow {
    QString  display;        // what the list shows / search matches
    QString  mpkName;        // physical name inside the archive
    QString  meaning;        // English gloss of the pinyin leaf name (may be
                             // empty; searched and shown in its own column)
    QString  type;           // repository type, or lowercase extension, or "-"
    QString  cat1;           // first path segment of display  (facet)
    QString  cat2;           // second path segment of display (facet)
    uint32_t size    = 0;
    int32_t  repoIdx = -1;   // index into repo().entries, -1 = not catalogued
    uint32_t entryId = 0;
    uint32_t animClips = 0;  // clips findFolderAnims would return; 0 = static.
                             // Only computed for Model/LodModel/Mesh rows —
                             // nothing else has a rig to play them on.
    QString  cls;            // player class facet ("Barbarian"); "" = none
    QString  slot;           // cosmetic slot facet ("Chest");    "" = none
    bool     player = false; // player-wearable/holdable (see NameTranslator)
};

struct AssetIndex {
    std::shared_ptr<di::DiAssetStore> store;
    QVector<AssetRow> rows;          // one per MPK entry (677k-class)
    QStringList       types;         // distinct type strings, sorted (for the filter combo)
    // Facet values with row counts, for the funnel popup (sorted by count desc
    // when listed there).
    QHash<QString, int> typeCounts;
    QHash<QString, int> catCounts;
    QHash<QString, int> subcatCounts;
    QHash<QString, int> classCounts;   // player class facet
    QHash<QString, int> slotCounts;    // cosmetic slot facet
    int                 playerCount = 0;
    QString           buildVersion;  // Engine/Config/Built.version ("" = unknown)
    double            loadMs = 0.0;  // measured; shown in the status bar

    // Build everything from an opened store. Runs on the loader thread.
    static std::shared_ptr<AssetIndex> build(std::shared_ptr<di::DiAssetStore> store);
};
