#include "store/AssetStore.h"

namespace di {

bool DiAssetStore::open(const std::string& mpkDir, std::string* err)
{
    return m_mpk.open(mpkDir, err);
}

bool DiAssetStore::loadRepository(std::string* err)
{
    size_t id = m_mpk.find("Package/resource.repository");
    if (id == (size_t)-1) id = m_mpk.find("resource.repository");
    if (id == (size_t)-1) {
        if (err) *err = "resource.repository not present in the MPK index";
        return false;
    }
    const std::vector<uint8_t> raw = m_mpk.read(id);   // parser strips CCCC/ZZZ4 itself
    if (raw.empty()) {
        if (err) *err = "resource.repository read returned 0 bytes";
        return false;
    }
    m_repoLoaded = parseRepository(raw.data(), raw.size(), &m_repo, err);
    return m_repoLoaded;
}

std::string DiAssetStore::buildVersion() const
{
    const size_t id = m_mpk.find("Engine/Config/Built.version");
    if (id == (size_t)-1) return {};
    std::vector<uint8_t> b = m_mpk.readAsset(id);
    // trim to printable single line
    std::string s;
    for (uint8_t c : b) {
        if (c == '\r' || c == '\n') break;
        if (c >= 0x20 && c < 0x7F) s += (char)c;
    }
    return s;
}

bool guidBlobHash(const std::string& entryName, std::string* hexOut)
{
    const size_t slash = entryName.find_last_of('/');
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot   = entryName.find('.', start);
    const size_t end   = dot == std::string::npos ? entryName.size() : dot;

    std::string hex;
    hex.reserve(32);
    for (size_t i = start; i < end; ++i) {
        const char c = entryName[i];
        if (c == '-') continue;
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        const bool isHexUp = (c >= 'A' && c <= 'F');
        if (!isHex && !isHexUp) return false;
        hex += isHexUp ? (char)(c - 'A' + 'a') : c;
        if (hex.size() > 32) return false;
    }
    if (hex.size() != 32) return false;
    if (hexOut) *hexOut = std::move(hex);
    return true;
}

void DiAssetStore::buildHashBridge() const
{
    std::lock_guard<std::mutex> lock(m_bridgeMtx);
    if (m_bridgeBuilt) return;
    m_bridgeBuilt = true;
    const auto& es = m_mpk.entries();
    std::string hex;
    for (size_t i = 0; i < es.size(); ++i) {
        if (guidBlobHash(es[i].name, &hex))
            m_hash2blob.emplace(hex, i);   // first writer wins (matches reference)
    }
}

size_t DiAssetStore::blobForHash(const std::string& hashHex) const
{
    buildHashBridge();
    auto it = m_hash2blob.find(hashHex);
    return it == m_hash2blob.end() ? (size_t)-1 : it->second;
}

size_t DiAssetStore::hashBridgeSize() const
{
    buildHashBridge();
    return m_hash2blob.size();
}

void DiAssetStore::buildParents() const
{
    std::lock_guard<std::mutex> lock(m_parentMtx);
    if (m_parentsBuilt) return;
    m_parentsBuilt = true;
    if (!m_repoLoaded) return;
    const auto& entries = m_repo.entries;
    m_parentOf.assign(entries.size(), -1);
    // pass 1: Model parents claim their children first; pass 2: everything
    // else (LodModel, Material, ...) fills the remainder.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < entries.size(); ++i) {
            const bool isModel = m_repo.typeOf(entries[i]) == "Model";
            if ((pass == 0) != isModel) continue;
            for (const std::string& rel : entries[i].related) {
                auto it = m_repo.byHash.find(rel);
                if (it != m_repo.byHash.end() && m_parentOf[it->second] < 0)
                    m_parentOf[it->second] = (int32_t)i;
            }
        }
    }
}

size_t DiAssetStore::parentEntryOf(size_t repoIdx) const
{
    buildParents();
    if (repoIdx >= m_parentOf.size()) return (size_t)-1;
    const int32_t p = m_parentOf[repoIdx];
    return p < 0 ? (size_t)-1 : (size_t)p;
}

} // namespace di
