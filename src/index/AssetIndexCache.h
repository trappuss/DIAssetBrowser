#pragma once
// Binary snapshot of the AssetIndex's DERIVED metadata (repo mapping, display
// names, type/category facets) keyed on MpkIndex::signature(). Skips the
// slowest startup phase — AssetIndex::build was measured at 1.8-4.1 s on the
// live 677k-entry store (sandbox, 2026-08-01) while mpkinfo parse + repository
// load are ~1.2 s combined and must run regardless (the tabs need the live
// store + repository for reads and dependency resolution).
//
// Not asset bytes (ground rule: never cache those) — only strings and indexes
// derived from the .mpkinfo + repository, both of which are fingerprinted by
// the signature; any patch/repack invalidates the cache and it rebuilds.
//
// File: data/index_v1.cache. Row order == store.mpk().entries() order, so
// entryId / mpkName / size are NOT stored — they come from the live store and
// the row count doubles as a consistency check.

#include <QString>

#include <memory>

#include "index/AssetIndex.h"

namespace AssetIndexCache {

// data/index_v1.cache beside the executable.
QString defaultPath();

// Null on any mismatch (missing file, bad magic/version, signature or row
// count differs) — never an error the user must handle; the caller rebuilds.
std::shared_ptr<AssetIndex> load(std::shared_ptr<di::DiAssetStore> store,
                                 const QString& path);

// Best-effort; logs on failure and returns false.
bool save(const AssetIndex& idx, const QString& path);

} // namespace AssetIndexCache
