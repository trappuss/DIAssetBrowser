#include "store/MpkIndex.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

#include "store/Zzz4.h"

namespace fs = std::filesystem;

namespace di {

static uint32_t rdU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rdU16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static bool readWholeFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)sz);
    const size_t got = sz ? std::fread(out.data(), 1, (size_t)sz, f) : 0;
    std::fclose(f);
    return got == (size_t)sz;
}

MpkIndex::~MpkIndex()
{
    for (auto& kv : m_handles)
        if (kv.second && kv.second->f) std::fclose(kv.second->f);
}

bool MpkIndex::open(const std::string& mpkDir, std::string* err)
{
    m_dir = mpkDir;

    std::vector<std::string> infos;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(fs::u8path(mpkDir), ec)) {
        if (!de.is_regular_file()) continue;
        std::string fn = de.path().filename().string();
        if (fn.size() > 8) {
            std::string ext = fn.substr(fn.size() - 8);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".mpkinfo") infos.push_back(fn);
        }
    }
    if (ec) {
        if (err) *err = "cannot list " + mpkDir + ": " + ec.message();
        return false;
    }
    if (infos.empty()) {
        if (err) *err = "no .mpkinfo files in " + mpkDir;
        return false;
    }
    std::sort(infos.begin(), infos.end());

    for (const std::string& fn : infos) {
        const std::string base = fn.substr(0, fn.size() - 8);
        m_bases.push_back(base);
        const uint16_t baseId = (uint16_t)(m_bases.size() - 1);
        if (!parseInfo(mpkDir + "/" + fn, baseId, err))
            return false;
    }
    return true;
}

bool MpkIndex::parseInfo(const std::string& infoPath, uint16_t baseId, std::string* err)
{
    std::vector<uint8_t> data;
    if (!readWholeFile(infoPath, data)) {
        if (err) *err = "cannot read " + infoPath;
        return false;
    }
    if (data.size() < 8) {
        if (err) *err = infoPath + ": too small (" + std::to_string(data.size()) + " bytes)";
        return false;
    }
    const uint32_t version = rdU32(data.data());
    if (version != 1) {
        if (err) *err = infoPath + ": unexpected version " + std::to_string(version)
                        + " (format doc says 1) - refusing to guess";
        return false;
    }
    const uint32_t count = rdU32(data.data() + 4);
    const size_t   n     = data.size();
    size_t         off   = 8;

    m_entries.reserve(m_entries.size() + count);
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 2 > n) break;
        const uint16_t nl = rdU16(data.data() + off);
        off += 2;
        if (off + nl + 12 > n) break;
        std::string name(reinterpret_cast<const char*>(data.data() + off), nl);
        off += nl;
        const uint32_t fo  = rdU32(data.data() + off);
        const uint32_t fl  = rdU32(data.data() + off + 4);
        const uint32_t pkf = rdU32(data.data() + off + 8);
        off += 12;
        if (fl == 0) {                       // directory placeholder
            ++m_placeholders;
            continue;
        }
        MpkEntry e;
        e.name   = std::move(name);
        e.pak    = pkf / 2;
        e.offset = fo;
        e.length = fl;
        e.baseId = baseId;
        const size_t id = m_entries.size();
        m_entries.push_back(std::move(e));
        if (!m_byName.emplace(m_entries.back().name, id).second)
            ++m_collisions;                  // first writer wins (matches reference)
    }
    return true;
}

size_t MpkIndex::find(const std::string& name) const
{
    auto it = m_byName.find(name);
    return it == m_byName.end() ? (size_t)-1 : it->second;
}

std::string MpkIndex::pakFileName(const MpkEntry& e) const
{
    const std::string& base = m_bases[e.baseId];
    return e.pak == 0 ? base + ".mpk" : base + std::to_string(e.pak) + ".mpk";
}

MpkIndex::PakHandle* MpkIndex::handle(const MpkEntry& e) const
{
    const std::string fn = pakFileName(e);
    std::lock_guard<std::mutex> lock(m_mapMtx);
    auto it = m_handles.find(fn);
    if (it == m_handles.end()) {
        auto h = std::make_unique<PakHandle>();
        h->f   = std::fopen((m_dir + "/" + fn).c_str(), "rb");
        it = m_handles.emplace(fn, std::move(h)).first;   // cache nullptr f too
    }
    return it->second.get();
}

std::vector<uint8_t> MpkIndex::readHeader(size_t id, size_t nbytes) const
{
    if (id >= m_entries.size()) return {};
    const MpkEntry& e = m_entries[id];
    PakHandle* h = handle(e);
    if (!h || !h->f) return {};
    std::lock_guard<std::mutex> lock(h->mtx);    // per-pak seek/read pair
    std::FILE* f = h->f;
#if defined(_MSC_VER)
    if (_fseeki64(f, (long long)e.offset, SEEK_SET) != 0) return {};
#else
    if (std::fseek(f, (long)e.offset, SEEK_SET) != 0) return {};
#endif
    const size_t want = nbytes < e.length ? nbytes : (size_t)e.length;
    std::vector<uint8_t> out(want);
    const size_t got = std::fread(out.data(), 1, want, f);
    out.resize(got);
    return out;
}

std::vector<uint8_t> MpkIndex::read(size_t id) const
{
    if (id >= m_entries.size()) return {};
    return readHeader(id, m_entries[id].length);
}

std::vector<uint8_t> MpkIndex::readAsset(size_t id) const
{
    std::vector<uint8_t> raw = read(id);
    if (raw.size() >= 8 && isZzz4(raw.data(), raw.size()))
        return inflateZzz4(raw.data(), raw.size());
    if (raw.size() >= 12 && std::memcmp(raw.data(), "CCCC", 4) == 0 &&
        isZzz4(raw.data() + 4, raw.size() - 4))
        return inflateZzz4(raw.data() + 4, raw.size() - 4);
    return raw;
}

std::string MpkIndex::signature() const
{
    std::vector<std::string> parts;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(fs::u8path(m_dir), ec)) {
        if (!de.is_regular_file()) continue;
        std::string fn = de.path().filename().string();
        std::string lower = fn;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        const bool isPak  = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".mpk") == 0;
        const bool isInfo = lower.size() > 8 && lower.compare(lower.size() - 8, 8, ".mpkinfo") == 0;
        if (!isPak && !isInfo) continue;
        std::error_code ec2;
        const auto sz = de.file_size(ec2);
        const auto mt = fs::last_write_time(de.path(), ec2).time_since_epoch().count();
        parts.push_back(fn + ":" + std::to_string((unsigned long long)sz) + ":"
                        + std::to_string((long long)mt));
    }
    std::sort(parts.begin(), parts.end());
    std::string sig;
    for (const std::string& p : parts) {
        sig += p;
        sig += '|';
    }
    return sig;
}

} // namespace di
