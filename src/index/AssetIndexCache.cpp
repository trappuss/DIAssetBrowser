#include "index/AssetIndexCache.h"

#include "app/AppPaths.h"
#include "index/ItemNames.h"

#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QSaveFile>

#include <cstring>

#include "app/AppPaths.h"

namespace AssetIndexCache {

namespace {

// '02': adds the per-row meaning gloss. Bumping the magic makes an old file a
// clean miss (rebuild), never a misparse.
// '03': adds the per-row clip count (AssetRow::animClips), which backs the
// Models tab's animated/static filter. A '02' file has no such field, so every
// row would read back as static and the filter would silently show nothing —
// exactly the stale-cache failure CacheVersioning.h warns about.
// '04': adds the class / slot / player facets. Same reasoning as '03' — a '03'
// file would load with every facet empty and the new filters would match
// nothing at all.
constexpr char kMagic[8] = {'D', 'I', 'I', 'D', 'X', 'C', '0', '4'};

// little-endian writers/readers over a growing QByteArray / raw pointer
void wU16(QByteArray& b, uint16_t v) { b.append((char)(v & 0xFF)); b.append((char)(v >> 8)); }
void wU32(QByteArray& b, uint32_t v)
{
    b.append((char)(v & 0xFF)); b.append((char)((v >> 8) & 0xFF));
    b.append((char)((v >> 16) & 0xFF)); b.append((char)(v >> 24));
}
void wStr(QByteArray& b, const QString& s)   // u16 len + utf8 (truncates >64k — never hit)
{
    const QByteArray u = s.toUtf8();
    const uint16_t n = (uint16_t)qMin<qsizetype>(u.size(), 65535);
    wU16(b, n);
    b.append(u.constData(), n);
}

struct Reader {
    const uint8_t* p;
    size_t n, o = 0;
    bool bad = false;
    uint16_t u16() { if (o + 2 > n) { bad = true; return 0; }
                     uint16_t v = (uint16_t)(p[o] | (p[o+1] << 8)); o += 2; return v; }
    uint32_t u32() { if (o + 4 > n) { bad = true; return 0; }
                     uint32_t v = (uint32_t)p[o] | ((uint32_t)p[o+1] << 8) |
                                  ((uint32_t)p[o+2] << 16) | ((uint32_t)p[o+3] << 24);
                     o += 4; return v; }
    int32_t  i32() { return (int32_t)u32(); }
    uint8_t  u8()  { if (o + 1 > n) { bad = true; return 0; } return p[o++]; }
    QString  str() { const uint16_t ln = u16();
                     if (bad || o + ln > n) { bad = true; return {}; }
                     QString s = QString::fromUtf8((const char*)p + o, ln); o += ln; return s; }
};

} // namespace

QString defaultPath()
{
    return AppPaths::file(QStringLiteral("index_v1.cache"));
}

bool save(const AssetIndex& idx, const QString& path)
{
    if (!idx.store) return false;
    QElapsedTimer t;
    t.start();

    // string tables
    QStringList types, cats, subcats, clss, slotTab;
    QHash<QString, uint16_t> typeIdx, catIdx, subIdx, clsIdx, slotIdx;
    auto intern = [](const QString& s, QStringList& list,
                     QHash<QString, uint16_t>& map) -> uint16_t {
        auto it = map.find(s);
        if (it != map.end()) return it.value();
        const uint16_t id = (uint16_t)list.size();
        map.insert(s, id);
        list.append(s);
        return id;
    };

    QByteArray body;
    body.reserve(idx.rows.size() * 12);
    for (const AssetRow& r : idx.rows) {
        wU32(body, (uint32_t)r.repoIdx);   // -1 round-trips through u32
        wU16(body, intern(r.type, types, typeIdx));
        wU16(body, r.cat1.isEmpty() ? 0xFFFF : intern(r.cat1, cats, catIdx));
        wU16(body, r.cat2.isEmpty() ? 0xFFFF : intern(r.cat2, subcats, subIdx));
        wU16(body, r.cls.isEmpty() ? 0xFFFF : intern(r.cls, clss, clsIdx));
        wU16(body, r.slot.isEmpty() ? 0xFFFF : intern(r.slot, slotTab, slotIdx));
        body.append((char)(r.player ? 1 : 0));
        wU32(body, r.animClips);
        if (r.repoIdx >= 0)
            wStr(body, r.display);         // else display == mpkName (derived)
        wStr(body, r.meaning);
    }
    if (types.size() > 0xFFFE || cats.size() > 0xFFFE || subcats.size() > 0xFFFE ||
        clss.size() > 0xFFFE || slotTab.size() > 0xFFFE) {
        qWarning("index cache: string table overflow — not saving");
        return false;
    }

    QByteArray head;
    head.append(kMagic, 8);
    const std::string sig = idx.store->mpk().signature() +
        "|names:" + ItemNames::fingerprint(AppPaths::file(QStringLiteral("di_item_names.csv")));
    wU32(head, (uint32_t)sig.size());
    head.append(sig.data(), (qsizetype)sig.size());
    wStr(head, idx.buildVersion);
    auto wTable = [&](const QStringList& l) {
        wU32(head, (uint32_t)l.size());
        for (const QString& s : l) wStr(head, s);
    };
    wTable(types);
    wTable(cats);
    wTable(subcats);
    wTable(clss);
    wTable(slotTab);
    wU32(head, (uint32_t)idx.rows.size());

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("index cache: cannot open %s for write", qPrintable(path));
        return false;
    }
    f.write(head);
    f.write(body);
    if (!f.commit()) {
        qWarning("index cache: write failed for %s", qPrintable(path));
        return false;
    }
    qInfo("index cache: saved %lld rows, %lld KB in %lld ms -> %s",
          (long long)idx.rows.size(), (long long)((head.size() + body.size()) / 1024),
          (long long)t.elapsed(), qPrintable(path));
    return true;
}

std::shared_ptr<AssetIndex> load(std::shared_ptr<di::DiAssetStore> store,
                                 const QString& path)
{
    if (!store) return nullptr;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    const QByteArray blob = f.readAll();
    if (blob.size() < 8 || std::memcmp(blob.constData(), kMagic, 8) != 0) {
        qInfo("index cache: bad magic — rebuilding");
        return nullptr;
    }

    Reader rd{(const uint8_t*)blob.constData(), (size_t)blob.size(), 8};
    const uint32_t sigLen = rd.u32();
    if (rd.bad || rd.o + sigLen > rd.n) return nullptr;
    const std::string cachedSig((const char*)rd.p + rd.o, sigLen);
    rd.o += sigLen;
    const std::string liveSig = store->mpk().signature() +
        "|names:" + ItemNames::fingerprint(AppPaths::file(QStringLiteral("di_item_names.csv")));
    if (cachedSig != liveSig) {
        qInfo("index cache: signature mismatch (game updated?) — rebuilding");
        return nullptr;
    }
    const QString buildVersion = rd.str();

    auto rdTable = [&](QStringList* out) {
        const uint32_t n = rd.u32();
        if (rd.bad || n > 0xFFFF) { rd.bad = true; return; }
        out->reserve((int)n);
        for (uint32_t i = 0; i < n && !rd.bad; ++i) out->append(rd.str());
    };
    QStringList types, cats, subcats;
    rdTable(&types);
    rdTable(&cats);
    rdTable(&subcats);
    QStringList clss, slotTab;
    rdTable(&clss);
    rdTable(&slotTab);
    const uint32_t rowCount = rd.u32();
    const auto& entries = store->mpk().entries();
    if (rd.bad || rowCount != entries.size()) {
        qInfo("index cache: row count mismatch — rebuilding");
        return nullptr;
    }

    auto idx = std::make_shared<AssetIndex>();
    idx->store = store;
    idx->buildVersion = buildVersion;
    idx->rows.reserve((qsizetype)rowCount);
    // repoIdx range guard: consumers index repo->entries[] with it directly, so
    // a corrupt-but-well-framed cache must not smuggle an OOB index through.
    const di::Repository* repo = store->repo();
    const int32_t repoMax = repo ? (int32_t)repo->entries.size() : 0;
    std::vector<int> typeN(types.size(), 0), catN(cats.size(), 0),
        subN(subcats.size(), 0), clsN(clss.size(), 0), slotN(slotTab.size(), 0);
    for (uint32_t i = 0; i < rowCount; ++i) {
        AssetRow row;
        row.entryId = i;
        row.size    = entries[i].length;
        row.mpkName = QString::fromLatin1(entries[i].name.c_str());
        row.repoIdx = rd.i32();
        if (row.repoIdx < -1 || row.repoIdx >= repoMax) {
            if (row.repoIdx >= 0 && !repo) {
                row.repoIdx = -1;   // repository failed to load this run; the
                                    // display/type strings still apply
            } else {
                qInfo("index cache: repoIdx out of range at row %u — rebuilding", i);
                return nullptr;
            }
        }
        const uint16_t ti = rd.u16();
        const uint16_t c1 = rd.u16();
        const uint16_t c2 = rd.u16();
        if (rd.bad || (int)ti >= types.size() ||
            (c1 != 0xFFFF && (int)c1 >= cats.size()) ||
            (c2 != 0xFFFF && (int)c2 >= subcats.size())) {
            qInfo("index cache: corrupt row %u — rebuilding", i);
            return nullptr;
        }
        row.type = types[ti];
        if (c1 != 0xFFFF) row.cat1 = cats[c1];
        if (c2 != 0xFFFF) row.cat2 = subcats[c2];
        const uint16_t ci = rd.u16();
        const uint16_t si = rd.u16();
        row.player = rd.u8() != 0;
        if (rd.bad || (ci != 0xFFFF && (int)ci >= clss.size()) ||
            (si != 0xFFFF && (int)si >= slotTab.size())) {
            qInfo("index cache: corrupt facet at row %u — rebuilding", i);
            return nullptr;
        }
        if (ci != 0xFFFF) row.cls  = clss[ci];
        if (si != 0xFFFF) row.slot = slotTab[si];
        row.animClips = rd.u32();
        row.display = row.repoIdx >= 0 ? rd.str() : row.mpkName;
        row.meaning = rd.str();
        if (rd.bad) {
            qInfo("index cache: truncated at row %u — rebuilding", i);
            return nullptr;
        }
        typeN[ti]++;
        if (c1 != 0xFFFF) catN[c1]++;
        if (c2 != 0xFFFF) subN[c2]++;
        if (ci != 0xFFFF) clsN[ci]++;
        if (si != 0xFFFF) slotN[si]++;
        if (row.player) idx->playerCount++;
        idx->rows.push_back(std::move(row));
    }

    for (int i = 0; i < types.size(); ++i)   idx->typeCounts.insert(types[i], typeN[i]);
    for (int i = 0; i < cats.size(); ++i)    idx->catCounts.insert(cats[i], catN[i]);
    for (int i = 0; i < subcats.size(); ++i) idx->subcatCounts.insert(subcats[i], subN[i]);
    for (int i = 0; i < clss.size(); ++i)    idx->classCounts.insert(clss[i], clsN[i]);
    for (int i = 0; i < slotTab.size(); ++i) idx->slotCounts.insert(slotTab[i], slotN[i]);
    idx->types = types;
    idx->types.sort(Qt::CaseInsensitive);
    return idx;
}

} // namespace AssetIndexCache
