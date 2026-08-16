#pragma once
// Diablo Immortal PC (Battle.net) .mpk archive index + lazy reader.
//
// Package/MPK holds Engine.mpk / Resources.mpk .. ResourcesN.mpk plus one
// .mpkinfo index per base. Index format (verified byte-exact by the Python
// reference reader, diasset/ingest/mpk.py):
//
//   u32  version (==1)
//   u32  fileCount
//   fileCount x:
//     u16  nameLength
//     char name[nameLength]     (latin1 path, e.g. "Package/Char/.../x.skeleton"
//                                or GUID blob "Package/9c/<uuid>.6")
//     u32  fileOffset           (into the target .mpk)
//     u32  fileLength           (0 = directory placeholder; skipped)
//     u32  pakField             (pakField / 2 = index of the .mpk)
//   byte hash[16]               (trailing; ignored)
//
// Blobs are stored RAW (no container compression/encryption); individual assets
// may themselves be ZZZ4/LZ4 — readAsset() inflates those.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace di {

struct MpkEntry {
    std::string name;
    uint32_t    pak    = 0;   // index of the .mpk (pakField / 2)
    uint32_t    offset = 0;
    uint32_t    length = 0;
    uint16_t    baseId = 0;   // index into MpkIndex::bases() ("Engine", "Resources")
};

class MpkIndex {
public:
    ~MpkIndex();
    MpkIndex() = default;
    MpkIndex(const MpkIndex&) = delete;
    MpkIndex& operator=(const MpkIndex&) = delete;

    // Parse every .mpkinfo in mpkDir. Returns false (with *err set) when the
    // directory has no .mpkinfo or an index fails structural checks.
    bool open(const std::string& mpkDir, std::string* err = nullptr);

    const std::string&              dir()     const { return m_dir; }
    const std::vector<MpkEntry>&    entries() const { return m_entries; }
    const std::vector<std::string>& bases()   const { return m_bases; }

    // Placeholder (length==0) records skipped during parse; name collisions seen.
    size_t placeholdersSkipped() const { return m_placeholders; }
    size_t nameCollisions()      const { return m_collisions; }

    // Name lookup (first writer wins on collision, matching the reference reader).
    // Returns entry id or SIZE_MAX.
    size_t find(const std::string& name) const;

    // Raw bytes straight from the .mpk (no decompression). Empty on I/O failure.
    // Thread-safe: the seek+read pair is serialized PER PAK FILE, so a decode
    // worker streaming a large blob from one pak does not stall a GUI header
    // probe against a different pak. Same-pak readers still queue.
    std::vector<uint8_t> read(size_t id) const;
    // First n bytes only (cheap probe).
    std::vector<uint8_t> readHeader(size_t id, size_t n) const;
    // ZZZ4 / CCCC+ZZZ4 blobs inflated; everything else returned as-is.
    std::vector<uint8_t> readAsset(size_t id) const;

    // "<base>.mpk" for pak 0, "<base><pak>.mpk" otherwise (file name only).
    std::string pakFileName(const MpkEntry& e) const;

    // Fingerprint of every *.mpk / *.mpkinfo (name:size:mtime). A repack changes
    // offsets sometimes without touching .mpkinfo size — so the packs are
    // included; any cache keyed on this dies with the patch that made it stale.
    std::string signature() const;

private:
    struct PakHandle {
        std::FILE* f = nullptr;
        std::mutex mtx;          // serializes the seek+read pair on this pak
    };

    bool parseInfo(const std::string& infoPath, uint16_t baseId, std::string* err);
    PakHandle* handle(const MpkEntry& e) const;

    std::string                             m_dir;
    std::vector<MpkEntry>                   m_entries;
    std::vector<std::string>                m_bases;
    std::unordered_map<std::string, size_t> m_byName;
    size_t                                  m_placeholders = 0;
    size_t                                  m_collisions   = 0;
    mutable std::mutex                      m_mapMtx;   // guards m_handles only
    mutable std::unordered_map<std::string, std::unique_ptr<PakHandle>> m_handles;
};

} // namespace di
