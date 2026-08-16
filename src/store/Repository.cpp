#include "store/Repository.h"

#include <cstring>

#include "store/Zzz4.h"

namespace di {

static const std::string kEmpty;

static uint32_t rdU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rdU16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static std::string toHex(const uint8_t* p, size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s[2 * i]     = digits[p[i] >> 4];
        s[2 * i + 1] = digits[p[i] & 0xF];
    }
    return s;
}

static std::vector<std::string> splitSemis(const uint8_t* p, size_t n)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || p[i] == ';') {
            if (i > start)
                out.emplace_back(reinterpret_cast<const char*>(p + start), i - start);
            start = i + 1;
        }
    }
    return out;
}

const std::string& Repository::typeOf(const RepoEntry& e) const
{
    if ((size_t)e.typeIdx < types.size())     // 0-based (measured; see header)
        return types[e.typeIdx];
    return kEmpty;
}

const std::string& Repository::folderOf(const RepoEntry& e) const
{
    if ((size_t)e.folderIdx < folders.size()) // 0-based (measured; see header)
        return folders[e.folderIdx];
    return kEmpty;
}

std::string Repository::pathOf(const RepoEntry& e) const
{
    const std::string& f = folderOf(e);
    const std::string& t = typeOf(e);
    std::string s;
    if (!f.empty()) { s += f; s += '/'; }
    s += e.name;
    if (!t.empty()) { s += '.'; s += t; }
    return s;
}

bool parseRepository(const uint8_t* data, size_t len, Repository* out, std::string* err)
{
    if (len < 12 || std::memcmp(data, "CCCC", 4) != 0 || std::memcmp(data + 4, "ZZZ4", 4) != 0) {
        if (err) *err = "not a resource.repository (missing CCCC/ZZZ4 frame)";
        return false;
    }
    std::vector<uint8_t> raw = inflateZzz4(data + 4, len - 4);
    const uint32_t usize = rdU32(data + 8);
    if (raw.size() != usize) {
        if (err) *err = "repository inflate size " + std::to_string(raw.size())
                        + " != declared " + std::to_string(usize);
        return false;
    }

    const uint8_t* p = raw.data();
    const size_t   n = raw.size();
    size_t o = 0;
    out->bytesTotal = n;

    if (n < 14) { if (err) *err = "repository body too small"; return false; }
    o += 4;                                          // i32 one (==1) — not enforced
    out->declaredCount = rdU32(p + o); o += 4;       // u32 (measured; see header)
    o += 2;                                          // u16 unk (0)

    const uint16_t nTypes = rdU16(p + o); o += 2;
    if (o + nTypes > n) { if (err) *err = "type string block truncated"; return false; }
    out->types = splitSemis(p + o, nTypes); o += nTypes;

    uint32_t nFold = rdU16(p + o); o += 2;
    if (nFold == 0xFFFF) {
        if (o + 4 > n) { if (err) *err = "folder length truncated"; return false; }
        nFold = rdU32(p + o); o += 4;
    }
    if (o + nFold > n) { if (err) *err = "folder string block truncated"; return false; }
    out->folders = splitSemis(p + o, nFold); o += nFold;

    out->entries.reserve(out->declaredCount);
    for (uint32_t i = 0; i < out->declaredCount; ++i) {
        if (o + 23 > n) break;
        RepoEntry e;
        e.unk1 = rdU16(p + o);
        e.unk2 = rdU16(p + o + 2);
        o += 5;                                      // u16 u16 u8
        e.hashHex = toHex(p + o, 16); o += 16;
        const uint16_t nl = rdU16(p + o); o += 2;
        if (o + nl + 6 > n) break;
        e.name.assign(reinterpret_cast<const char*>(p + o), nl); o += nl;
        e.folderIdx = rdU16(p + o);
        e.typeIdx   = rdU16(p + o + 2);
        const uint16_t hc = rdU16(p + o + 4);
        o += 6;
        if (o + (size_t)hc * 16 > n) break;
        e.related.reserve(hc);
        for (uint16_t k = 0; k < hc; ++k)
            e.related.push_back(toHex(p + o + (size_t)k * 16, 16));
        o += (size_t)hc * 16;
        out->entries.push_back(std::move(e));
    }
    out->bytesConsumed = o;

    out->byHash.reserve(out->entries.size());
    for (size_t i = 0; i < out->entries.size(); ++i)
        out->byHash.emplace(out->entries[i].hashHex, i);
    return true;
}

// Built on first use, never at parse time: the map costs roughly 30 MB on the
// live repository and most sessions never ask for a name lookup.
void Repository::buildNameIndex() const
{
    std::lock_guard<std::mutex> lock(m_nameMtx);
    if (m_nameBuilt.load(std::memory_order_relaxed)) return;
    m_byName.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        m_byName[entries[i].name].push_back(i);
    // RELEASE pairs with the acquire in byName(): everything written above is
    // visible to any thread that observes the flag set.
    m_nameBuilt.store(true, std::memory_order_release);
}

const std::vector<size_t>* Repository::byName(const std::string& name) const
{
    if (!m_nameBuilt.load(std::memory_order_acquire)) buildNameIndex();
    auto it = m_byName.find(name);
    return it == m_byName.end() ? nullptr : &it->second;
}

size_t Repository::findByNameType(const std::string& name,
                                  const std::string& type) const
{
    const std::vector<size_t>* v = byName(name);
    if (!v) return (size_t)-1;
    for (size_t idx : *v)
        if (typeOf(entries[idx]) == type) return idx;
    return (size_t)-1;
}

void Repository::buildFolderIndex() const
{
    std::lock_guard<std::mutex> lock(m_folderMtx);
    if (m_folderBuilt.load(std::memory_order_relaxed)) return;
    for (size_t i = 0; i < entries.size(); ++i)
        m_byFolder[entries[i].folderIdx].push_back(i);
    m_folderBuilt.store(true, std::memory_order_release);
}

const std::vector<size_t>* Repository::byFolder(uint16_t folderIdx) const
{
    if (!m_folderBuilt.load(std::memory_order_acquire)) buildFolderIndex();
    auto it = m_byFolder.find(folderIdx);
    return it == m_byFolder.end() ? nullptr : &it->second;
}

int Repository::folderIndexOf(const std::string& folder) const
{
    for (size_t i = 0; i < folders.size(); ++i)
        if (folders[i] == folder) return (int)i;
    return -1;
}

} // namespace di
