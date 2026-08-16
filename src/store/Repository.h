#pragma once
// Diablo Immortal resource.repository parser — the game's master asset catalog:
// every logical asset's real path, name, type, 16-byte content hash, and the
// hashes of the assets it depends on (a Model's related hashes are its Mesh /
// Material / Texture / SkinSkeleton parts).
//
// Container: 'CCCC' + 'ZZZ4' + u32 uncompressedSize + one LZ4 block.
// Decoded layout (verified vs the CucFlavius/Zee 010 template by the Python
// reference parser, diasset/parsing/repository_parser.py):
//
//   i32  one (==1)
//   u32  fileCount                     <-- u32, NOT the template's u16 + skip 4:
//   u16  unk (0)                           measured 2026-08-01 on the live file —
//                                          u16 parse left 47.7 MB unconsumed; u32
//                                          parse consumed all 50,211,754 bytes
//                                          exactly (551,524 entries, 0 leftover).
//   u16  sizeOfFileTypeStrings;  char fileTypes[...]  (';'-separated)
//   u16  sizeOfFolderStrings (u32 if ==0xFFFF); char folders[...] (';'-separated)
//   fileCount x FILE:
//     u16 unk1, u16 unk2, u8 unk3
//     char fileHash[16]
//     u16 nameLen; char name[nameLen]
//     u16 folderIndex, u16 typeIndex   — both 0-BASED, not the template's
//         1-based-with-0-none. Measured 2026-08-01: with 1-based indexing a
//         "Texture2D" entry's blob inflated to a ParticleSystem, "*_mat"
//         entries typed Mesh, and every Texture2D mapped to a tiny no-ext
//         blob. 0-based puts all 182,399 Texture2D on the .6/.1 blobs
//         (02 02 02 01 magic, byte-verified) and Mesh on .MESSIAH blobs.
//         Out-of-range index = untyped/unfoldered.
//     u16 relatedCount; relatedCount x char relatedHash[16]

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace di {

struct RepoEntry {
    std::string hashHex;                 // 32 lowercase hex chars
    std::string name;
    uint16_t    folderIdx = 0;           // 0-based; out-of-range = none
    uint16_t    typeIdx   = 0;           // 0-based; out-of-range = none
    uint16_t    unk1 = 0, unk2 = 0;
    std::vector<std::string> related;    // dependency hashes, 32-hex each
};

struct Repository {
    std::vector<std::string> types;
    std::vector<std::string> folders;
    std::vector<RepoEntry>   entries;
    std::unordered_map<std::string, size_t> byHash;   // hashHex -> entry index

    // parse diagnostics (printed by the probe; never guessed at)
    uint32_t declaredCount = 0;
    size_t   bytesConsumed = 0;
    size_t   bytesTotal    = 0;

    const std::string& typeOf(const RepoEntry& e) const;
    const std::string& folderOf(const RepoEntry& e) const;
    std::string pathOf(const RepoEntry& e) const;     // folder/name.type

    // ── name -> entry index, built lazily ──────────────────────────────────
    // The repository holds 551,524 entries and shipped with only byHash, so
    // every "find the entry called X" was a full linear scan. Several ran per
    // item inside loops (the opposite-gender twin lookup, the hand-weapon and
    // body-piece scans, the reverse material walk), which is O(items x 551k)
    // string compares on the GUI thread.
    //
    // Names are NOT unique — a name can repeat across folders and types — so
    // this maps to a LIST of indices and callers filter by type/folder. That is
    // exactly what the scans it replaces were already doing inline.
    //
    // Lazy + mutex-guarded, mirroring DiAssetStore's hash bridge: the map costs
    // ~30 MB and most sessions never need it.
    const std::vector<size_t>* byName(const std::string& name) const;
    // First entry with this name AND type, or SIZE_MAX. The common case.
    size_t findByNameType(const std::string& name, const std::string& type) const;

    // ── folderIdx -> entry indices, built lazily ───────────────────────────
    // The hot scans in this codebase turn out to be by FOLDER, not by name:
    // findSiblingModels runs on EVERY model load (the attachments panel) and
    // walked all 551,524 entries to find the handful sharing a folder, and the
    // Wardrobe's class scan does the same per class. Both become a hash lookup
    // plus a walk of that folder's own entries.
    const std::vector<size_t>* byFolder(uint16_t folderIdx) const;
    // Index of a folder by name, or -1. Lets a caller resolve "Char/item" once
    // and then use the fast path.
    int folderIndexOf(const std::string& folder) const;

private:
    void buildNameIndex() const;
    void buildFolderIndex() const;
    mutable std::mutex m_nameMtx;
    mutable std::unordered_map<std::string, std::vector<size_t>> m_byName;
    // ATOMIC, not a plain bool: byName() double-checks this outside m_nameMtx,
    // so a plain read gives no happens-before edge with the 551k-entry map
    // construction — a thread that saw `true` could index a table whose buckets
    // were not yet visible to it. Newly reachable from several threads at once
    // now that the bulk export's lanes resolve opposite-gender twins by name.
    mutable std::atomic<bool> m_nameBuilt{false};
    mutable std::mutex m_folderMtx;
    mutable std::unordered_map<uint16_t, std::vector<size_t>> m_byFolder;
    mutable std::atomic<bool> m_folderBuilt{false};   // same reason as m_nameBuilt
};

// Parse from the raw file bytes (still CCCC/ZZZ4-framed). Returns false with
// *err set on structural failure; partial trailing entries are dropped and
// reflected in bytesConsumed < bytesTotal.
bool parseRepository(const uint8_t* data, size_t len, Repository* out, std::string* err);

} // namespace di
