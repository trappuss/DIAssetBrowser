#include "model/ExportExtras.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <set>
#include <vector>

#include "store/Zzz4.h"

namespace di {

namespace {

// Deps folders are shared by every model written into one output folder, and a
// batch export touches them from more than one item, so the check-then-write is
// serialised. Only the WRITE is guarded — reading and inflating stay parallel.
QMutex g_depsMx;

// The blob as this tool sees it: ZZZ4-wrapped assets are inflated, so what lands
// on disk is what a parser consumes rather than a container nobody else opens.
std::vector<uint8_t> payload(const di::DiAssetStore& store, size_t blob)
{
    if (blob == (size_t)-1) return {};
    std::vector<uint8_t> raw = store.mpk().readAsset(blob);
    if (raw.size() >= 4 && raw[0] == 'Z' && raw[1] == 'Z' && raw[2] == 'Z' &&
        raw[3] == '4') {
        std::vector<uint8_t> flat = di::inflateZzz4(raw.data(), raw.size());
        if (!flat.empty()) return flat;
    }
    return raw;
}

// Write once. The existence test is on SIZE, not existence: a zero-byte corpse
// from a killed run must be rewritten, not treated as done forever.
bool writeOnce(const QString& path, const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) return false;
    QMutexLocker lock(&g_depsMx);
    if (QFileInfo(path).size() > 0) return false;   // another model already wrote it
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(reinterpret_cast<const char*>(bytes.data()),
                   qint64(bytes.size())) == qint64(bytes.size());
}

QString leaf(const QString& name)
{
    const int s = name.lastIndexOf(QLatin1Char('/'));
    return s >= 0 ? name.mid(s + 1) : name;
}

// Sanitise a repository name into a filename. Repository names are path-like
// ("Char/f_barbarian/…"), and the leaf alone is what identifies the asset.
QString fileStem(const QString& name)
{
    QString s = leaf(name);
    static const QString bad = QStringLiteral("\\/:*?\"<>|");
    for (QChar& c : s)
        if (bad.contains(c)) c = QLatin1Char('_');
    return s.trimmed();
}

}   // namespace

int writeRawDeps(const di::DiAssetStore& store, int32_t repoIdx,
                 const QString& assetName, const QString& destDir)
{
    const di::Repository* repo = store.repo();
    if (repoIdx < 0 || destDir.isEmpty() || !repo) return 0;
    if ((size_t)repoIdx >= repo->entries.size()) return 0;
    QDir().mkpath(destDir);
    const QDir dir(destDir);

    int written = 0;
    std::set<size_t> seen;

    // Walk the dependency graph from the model outward. Depth 2 is the whole
    // chain in DI: Model -> {Mesh, Material, SkinSkeleton} -> {Texture2D...}.
    // Naming each file "<entry name>.<its repository type>" makes the folder
    // readable without a manifest, which matters because the archive itself
    // stores these blobs under GUID names.
    const auto emitEntry = [&](size_t idx) {
        if (idx >= repo->entries.size() || seen.count(idx)) return;
        seen.insert(idx);
        const di::RepoEntry& e = repo->entries[idx];
        const size_t blob = store.blobForHash(e.hashHex);
        if (blob == (size_t)-1) return;
        QString stem = fileStem(QString::fromStdString(e.name));
        if (stem.isEmpty())
            stem = QStringLiteral("asset_%1").arg((qulonglong)idx);
        QString ext = QString::fromStdString(repo->typeOf(e)).toLower();
        if (ext.isEmpty()) ext = QStringLiteral("bin");
        if (writeOnce(dir.filePath(stem + QLatin1Char('.') + ext),
                      payload(store, blob)))
            ++written;
    };

    emitEntry((size_t)repoIdx);
    const di::RepoEntry& root = repo->entries[(size_t)repoIdx];
    for (const std::string& h : root.related) {
        auto it = repo->byHash.find(h);
        if (it == repo->byHash.end()) continue;
        emitEntry(it->second);
        // One level further: a Material's textures.
        const di::RepoEntry& child = repo->entries[it->second];
        for (const std::string& h2 : child.related) {
            auto it2 = repo->byHash.find(h2);
            if (it2 != repo->byHash.end()) emitEntry(it2->second);
        }
    }
    (void)assetName;   // naming comes from the repository entries themselves
    return written;
}

int writeLooseTextures(const MeshTextures& tex, const QString& modelName,
                       const QString& destDir)
{
    if (destDir.isEmpty()) return 0;
    QString base = fileStem(modelName);
    if (base.isEmpty()) base = QStringLiteral("model");
    const struct { const QImage* img; const char* suffix; } maps[] = {
        {&tex.diffuse,  "basecolor"},
        {&tex.normal,   "normal"},
        {&tex.mix,      "mix"},
        {&tex.emissive, "emissive"},
    };
    bool made = false;
    int written = 0;
    for (const auto& m : maps) {
        if (m.img->isNull()) continue;
        if (!made) { QDir().mkpath(destDir); made = true; }
        const QString p = QDir(destDir).filePath(
            QStringLiteral("%1_%2.png").arg(base, QLatin1String(m.suffix)));
        if (m.img->save(p, "PNG")) ++written;
    }
    return written;
}

}   // namespace di
