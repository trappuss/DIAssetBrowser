#pragma once
// The storage seam for the DI browser. Everything above this layer knows only:
// "an asset with an id, a name, a type, and some bytes" — the same split the
// D4 tool keeps between its storage and its tabs/decoders.
//
//   storage layer -> asset index -> decoders (mesh / texture) -> tabs + viewport
//
// DiAssetStore = MpkIndex (physical: names -> bytes)
//              + Repository (logical: names/types/dependency hashes)
//              + hash bridge (repository 32-hex hash -> GUID-named blob entry).
// It is framework-free (std only) so headless tools and tests link it without Qt.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "store/MpkIndex.h"
#include "store/Repository.h"

namespace di {

class DiAssetStore {
public:
    // mpkDir = <game>/Package/MPK. Loads the physical index; repository load is
    // separate (loadRepository) so callers can measure the two phases apart.
    bool open(const std::string& mpkDir, std::string* err = nullptr);

    // Decode Package/resource.repository from inside the archive.
    // Returns false (with *err) if absent or structurally invalid.
    bool loadRepository(std::string* err = nullptr);

    const MpkIndex&   mpk()  const { return m_mpk; }
    const Repository* repo() const { return m_repoLoaded ? &m_repo : nullptr; }

    // Update-proofing: a stamp that changes whenever the installed data does.
    // buildVersion() = content of Engine/Config/Built.version inside the MPKs
    // (empty when unreadable — callers must treat that as "unknown", not "ok").
    // signature()    = (name:size:mtime) fingerprint of every pack + index file.
    std::string buildVersion() const;
    std::string signature() const { return m_mpk.signature(); }

    // Hash bridge: 32-hex content hash -> MpkIndex entry id (GUID-named blob),
    // built lazily from GUID-shaped entry names. SIZE_MAX when unresolved.
    size_t blobForHash(const std::string& hashHex) const;
    size_t hashBridgeSize() const;

    // Reverse dependency: repository entry -> the entry that lists it in its
    // rels (first Model wins, then any — two-pass, so a bare Mesh resolves to
    // its owning Model, never to some LodModel that also references it when a
    // Model exists). Measured on the live repository: 67,692 of 84,843 Mesh
    // entries (79.8%) have a Model/LodModel parent — that is how a bare Mesh
    // row borrows its textures + skeleton. SIZE_MAX = no parent.
    size_t parentEntryOf(size_t repoIdx) const;

private:
    void buildHashBridge() const;
    void buildParents() const;

    MpkIndex   m_mpk;
    Repository m_repo;
    bool       m_repoLoaded = false;

    mutable std::mutex m_bridgeMtx;   // lazy build may race across threads
    mutable std::unordered_map<std::string, size_t> m_hash2blob;
    mutable bool m_bridgeBuilt = false;

    mutable std::mutex m_parentMtx;
    mutable std::vector<int32_t> m_parentOf;   // repo idx -> parent repo idx, -1 none
    mutable bool m_parentsBuilt = false;
};

// True when an MPK entry name is a GUID-shaped content blob
// ("Package/9c/9cxxxxxx-....6" -> 32 hex chars once dashes/extension go).
// hexOut (optional) receives the normalized 32-hex hash.
bool guidBlobHash(const std::string& entryName, std::string* hexOut);

} // namespace di
