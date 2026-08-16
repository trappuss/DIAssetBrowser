#include "model/ModelResolve.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include <QStringList>

#include "model/MaterialParser.h"

namespace di {

void fillFromMaterial(const di::DiAssetStore& store, const di::RepoEntry& matRef,
                      ResolvedModel* out);

ResolvedModel resolveModelChain(const di::DiAssetStore& store, int32_t repoIdx)
{
    ResolvedModel r;
    const di::Repository* repo = store.repo();
    if (repoIdx < 0 || !repo) {
        r.note = QStringLiteral("no repository entry");
        return r;
    }
    const di::RepoEntry& e = repo->entries[(size_t)repoIdx];
    const std::string&   t = repo->typeOf(e);

    auto blobOf = [&](const std::string& hash) { return store.blobForHash(hash); };
    auto entryOf = [&](const std::string& hash) -> const di::RepoEntry* {
        auto it = repo->byHash.find(hash);
        return it == repo->byHash.end() ? nullptr : &repo->entries[it->second];
    };

    const di::RepoEntry* meshEntry = nullptr;
    const di::RepoEntry* matEntry  = nullptr;
    const di::RepoEntry* skinEntry = nullptr;
    if (t == "Mesh") {
        // A bare Mesh entry has no deps of its own (rels are empty) — its
        // Material and SkinSkeleton hang off the OWNING Model. Resolve the
        // parent (measured: 79.8% of Mesh entries have one) and reuse the
        // Model path, keeping THIS mesh blob.
        const size_t par = store.parentEntryOf((size_t)repoIdx);
        if (par != (size_t)-1) {
            const std::string& pt = repo->typeOf(repo->entries[par]);
            if (pt == "Model" || pt == "LodModel") {
                ResolvedModel r2 = resolveModelChain(store, (int32_t)par);
                r2.meshBlob = blobOf(e.hashHex);   // the row's own mesh
                if (!r2.note.isEmpty())
                    r2.note += QStringLiteral(" (via %1)").arg(
                        QString::fromLatin1(repo->entries[par].name.c_str()));
                return r2;
            }
        }
        meshEntry = &e;
    } else {   // Model / LodModel: walk deps
        for (const std::string& h : e.related) {
            const di::RepoEntry* dep = entryOf(h);
            if (!dep) continue;
            const std::string& dt = repo->typeOf(*dep);
            if (dt == "Mesh" && !meshEntry)             meshEntry = dep;
            if (dt == "Material" && !matEntry)          matEntry  = dep;
            if (dt == "SkinSkeleton" && !skinEntry)     skinEntry = dep;
            if (dt == "Model" && !meshEntry) {
                // LodModel wraps a Model — one more hop
                for (const std::string& h2 : dep->related) {
                    const di::RepoEntry* d2 = entryOf(h2);
                    if (!d2) continue;
                    const std::string& dt2 = repo->typeOf(*d2);
                    if (dt2 == "Mesh" && !meshEntry)         meshEntry = d2;
                    if (dt2 == "Material" && !matEntry)      matEntry  = d2;
                    if (dt2 == "SkinSkeleton" && !skinEntry) skinEntry = d2;
                }
            }
        }
    }
    if (meshEntry) r.meshBlob = blobOf(meshEntry->hashHex);
    if (skinEntry) r.skinBlob = blobOf(skinEntry->hashHex);
    if (!matEntry) {
        if (t == "Mesh") r.note = QStringLiteral("untextured (bare Mesh entry)");
        return r;
    }
    r.matRepo = (int32_t)(matEntry - repo->entries.data());
    fillFromMaterial(store, *matEntry, &r);
    return r;
}

// Shared by resolveModelChain and resolveMaterialTextures: blob-first texture
// bindings with the name-suffix rel fallback.
void fillFromMaterial(const di::DiAssetStore& store, const di::RepoEntry& matRef,
                      ResolvedModel* out)
{
    ResolvedModel& r = *out;
    const di::Repository* repo = store.repo();
    const di::RepoEntry* matEntry = &matRef;
    auto blobOf = [&](const std::string& hash) { return store.blobForHash(hash); };
    auto entryOf = [&](const std::string& hash) -> const di::RepoEntry* {
        auto it = repo->byHash.find(hash);
        return it == repo->byHash.end() ? nullptr : &repo->entries[it->second];
    };

    // Primary: the material blob's own bindings (authoritative).
    const size_t matBlob = blobOf(matEntry->hashHex);
    if (matBlob != (size_t)-1) {
        const std::vector<uint8_t> mraw = store.mpk().read(matBlob);
        di::MaterialMaps maps;
        std::string merr;
        if (!mraw.empty() && di::parseMaterial(mraw.data(), mraw.size(), &maps, &merr)) {
            if (!maps.baseMap.empty())     r.texBlob = blobOf(maps.baseMap);
            if (!maps.normalMap.empty())   r.nrmBlob = blobOf(maps.normalMap);
            if (!maps.mixMap.empty())      r.mixBlob = blobOf(maps.mixMap);
            if (!maps.emissionMap.empty()) r.emiBlob = blobOf(maps.emissionMap);
            // Animated-emissive FX constants ride along to the viewport.
            r.matFx.emissionColor    = maps.emissionColor;
            r.matFx.emissionColorSet = maps.emissionColorSet;
            r.matFx.arcaneColor      = maps.arcaneColor;
            r.matFx.arcaneIntensity  = maps.arcaneIntensity;
            r.matFx.arcaneScroll     = maps.arcaneScroll;
            r.matFx.star1            = maps.star1;
            r.matFx.star2            = maps.star2;
            r.matFx.fresnelColor     = maps.fresnelColor;
            r.matFx.fresnelIntensity = maps.fresnelIntensity;
            r.matFx.fresnelRange     = maps.fresnelRange;
            // Diagnostic (opt-in via DI_DUMP_MAT): one compact line naming the
            // shader + every "t*Map=" key in the blob, to pin a material's
            // texture bindings when investigating. Off by default so routine
            // model loads don't flood the log.
            if (std::getenv("DI_DUMP_MAT")) {
                QStringList keys;
                const char* h = reinterpret_cast<const char*>(mraw.data());
                const size_t hn = mraw.size();
                for (size_t i = 0; i + 4 < hn; ++i) {
                    if (h[i] != 't') continue;
                    size_t j = i + 1;
                    while (j < hn) {
                        const char c = h[j];
                        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9')) ++j; else break;
                    }
                    if (j < hn && h[j] == '=' && j - i >= 4 &&
                        std::memcmp(h + j - 3, "Map", 3) == 0) {
                        const QString k = QString::fromLatin1(h + i, (int)(j - i));
                        if (!keys.contains(k)) keys << k;
                    }
                    i = j;
                }
                qInfo("mat %s shader='%s' maps=[%s]", matEntry->name.c_str(),
                      maps.shader.c_str(), qPrintable(keys.join(QLatin1Char(','))));
            }
            if (r.texBlob != (size_t)-1)
                r.note = QStringLiteral("mat %1%2")
                             .arg(QString::fromLatin1(matEntry->name.c_str()),
                                  maps.shader.empty()
                                      ? QString()
                                      : QStringLiteral(" (%1)").arg(
                                            QString::fromLatin1(maps.shader.c_str())));
        }
    }

    // Fallback: name-suffix convention over the Material's repository rels
    // (fills only what the blob did not bind).
    const di::RepoEntry* texAny = nullptr;
    auto suffixed = [](const di::RepoEntry* e, char c) {
        return e->name.size() > 2 && e->name[e->name.size() - 2] == '_' &&
               e->name.back() == c;
    };
    for (const std::string& h : matEntry->related) {
        const di::RepoEntry* dep = entryOf(h);
        if (!dep || repo->typeOf(*dep) != "Texture2D") continue;
        if (!texAny) texAny = dep;
        if (suffixed(dep, 'd') && r.texBlob == (size_t)-1) {
            r.texBlob = blobOf(dep->hashHex);
            r.note = QStringLiteral("tex %1").arg(QString::fromLatin1(dep->name.c_str()));
        }
        if (suffixed(dep, 'n') && r.nrmBlob == (size_t)-1) r.nrmBlob = blobOf(dep->hashHex);
        if (suffixed(dep, 'm') && r.mixBlob == (size_t)-1) r.mixBlob = blobOf(dep->hashHex);
        if (suffixed(dep, 'e') && r.emiBlob == (size_t)-1) r.emiBlob = blobOf(dep->hashHex);
    }
    if (r.texBlob == (size_t)-1 && texAny) {
        r.texBlob = blobOf(texAny->hashHex);
        r.note = QStringLiteral("tex %1").arg(QString::fromLatin1(texAny->name.c_str()));
    }
    if (r.note.isEmpty())
        r.note = QStringLiteral("untextured");
}

ResolvedModel resolveMaterialTextures(const di::DiAssetStore& store,
                                      int32_t matRepoIdx)
{
    ResolvedModel r;
    const di::Repository* repo = store.repo();
    if (matRepoIdx < 0 || !repo) {
        r.note = QStringLiteral("no repository entry");
        return r;
    }
    const di::RepoEntry& e = repo->entries[(size_t)matRepoIdx];
    if (repo->typeOf(e) != "Material") {
        r.note = QStringLiteral("not a Material entry");
        return r;
    }
    fillFromMaterial(store, e, &r);
    return r;
}

size_t resolveThumbTexture(const di::DiAssetStore& store, int32_t repoIdx,
                           uint32_t entryId, const QString& type)
{
    if (type == QLatin1String("Texture2D"))
        return (size_t)entryId;
    if (repoIdx < 0) return (size_t)-1;
    if (type == QLatin1String("Model") || type == QLatin1String("LodModel") ||
        type == QLatin1String("Mesh"))
        return resolveModelChain(store, repoIdx).texBlob;
    return (size_t)-1;
}

// "<asset>_lod" / "_lod1" / "_lod2" are LOD variants of one asset; any clip or
// skeleton folder named for them is named for the BASE asset. Measured on the
// live archive (di_probe --anim-coverage): without this, every "_lod" row in
// Char/item misses its own per-asset folder.
static std::string stripLodSuffix(const std::string& s)
{
    size_t n = s.size();
    while (n > 0 && s[n - 1] >= '0' && s[n - 1] <= '9') --n;
    if (n >= 4 && s.compare(n - 4, 4, "_lod") == 0) return s.substr(0, n - 4);
    return s;
}

// The clip folder ships as "ani", but 282 clips sit in a misspelled "ain" and 5
// in a capitalised "Ani" (measured; di_probe [12d]). Char/sorceress_f is 100%
// "ain" — the whole family had zero clips until this accepted both spellings.
static bool isAniSegment(const char* p, size_t n)
{
    if (n != 3) return false;
    auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; };
    const char a = lc(p[0]), b = lc(p[1]), c = lc(p[2]);
    return (a == 'a' && b == 'n' && c == 'i') || (a == 'a' && b == 'i' && c == 'n');
}

// THE one place the clip-folder search order lives. Calls probe() with each
// candidate parent folder, in priority order, until probe() returns true:
//   1. "Package/<folder>/<asset>"            per-asset clip folder
//   2. "Package/<folder>/<asset minus _lod>" same, LOD variants share the base
//   3. "Package/<folder>", then each ancestor  the folder walk-up
// findFolderAnims and AnimFolderIndex::clipCount both drive this, so widening
// the rules in one automatically widens the other.
template <class Probe>
static void forEachClipBase(const di::Repository& repo, int32_t repoIdx, Probe probe)
{
    if (repoIdx < 0 || repoIdx >= (int32_t)repo.entries.size()) return;
    const di::RepoEntry& self = repo.entries[(size_t)repoIdx];
    std::string folder = repo.folderOf(self);
    const std::string base = "Package/" + folder + "/";

    if (probe(base + self.name)) return;
    const std::string stripped = stripLodSuffix(self.name);
    if (stripped != self.name && probe(base + stripped)) return;

    while (!folder.empty()) {
        if (probe("Package/" + folder)) return;
        const size_t slash = folder.find_last_of('/');
        if (slash == std::string::npos) break;
        folder.resize(slash);
    }
}

// Append every "<prefix><ani|ain>/<clip>.anim" to out. prefix ends with '/'.
static void collectAnimsUnder(const di::DiAssetStore& store, const std::string& prefix,
                              std::vector<std::pair<std::string, size_t>>* out)
{
    const auto& entries = store.mpk().entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string& n = entries[i].name;
        if (n.size() <= prefix.size() + 9 ||          // "<seg>/x.anim" at minimum
            n.compare(0, prefix.size(), prefix) != 0 ||
            n.compare(n.size() - 5, 5, ".anim") != 0)
            continue;
        const size_t segEnd = n.find('/', prefix.size());
        if (segEnd == std::string::npos) continue;
        if (!isAniSegment(n.c_str() + prefix.size(), segEnd - prefix.size())) continue;
        if (n.find('/', segEnd + 1) != std::string::npos) continue;   // clips are flat
        out->emplace_back(n.substr(segEnd + 1, n.size() - segEnd - 6), i);
    }
}

size_t findSkeletonByConvention(const di::DiAssetStore& store, int32_t repoIdx)
{
    const di::Repository* repo = store.repo();
    if (repoIdx < 0 || !repo) return (size_t)-1;
    const di::RepoEntry& self = repo->entries[(size_t)repoIdx];

    // Per-asset skeleton: "<folder>/<asset>/<asset>.skeleton". Char/item models
    // all share the folder "Char/item", so the folder walk below can never find
    // a skeleton for them; the per-asset probe can (measured: 0% -> 6.5%).
    {
        const std::string base = "Package/" + repo->folderOf(self) + "/";
        const std::string stripped = stripLodSuffix(self.name);
        for (const std::string& nm : {self.name, stripped}) {
            const size_t id = store.mpk().find(base + nm + "/" + nm + ".skeleton");
            if (id != (size_t)-1) return id;
            if (stripped == self.name) break;
        }
    }

    std::string folder = repo->folderOf(self);
    while (!folder.empty()) {
        const size_t slash = folder.find_last_of('/');
        const std::string leaf =
            slash == std::string::npos ? folder : folder.substr(slash + 1);
        const size_t id =
            store.mpk().find("Package/" + folder + "/" + leaf + ".skeleton");
        if (id != (size_t)-1) return id;
        if (slash == std::string::npos) break;
        folder.resize(slash);   // one level up
    }
    return (size_t)-1;
}

std::vector<std::pair<std::string, size_t>>
findFolderAnims(const di::DiAssetStore& store, int32_t repoIdx)
{
    std::vector<std::pair<std::string, size_t>> out;
    const di::Repository* repo = store.repo();
    if (repoIdx < 0 || !repo) return out;

    // The walk is deliberately NOT widened to a recursive search: measured on
    // the live archive, a recursive walk attaches 2,390 unrelated clips to every
    // Char/item row and 37,769 to every Char/nielian row.
    forEachClipBase(*repo, repoIdx, [&](const std::string& base) {
        collectAnimsUnder(store, base + "/", &out);
        return !out.empty();
    });
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::vector<std::pair<std::string, size_t>>
findClassAnims(const di::DiAssetStore& store, const std::string& folder)
{
    std::vector<std::pair<std::string, size_t>> out;
    if (folder.empty()) return out;
    // Clips sit directly under the class folder: "Package/<folder>/{ani|ain}/".
    // collectAnimsUnder handles the ani/ain/Ani spelling + flat-clip rules.
    collectAnimsUnder(store, "Package/" + folder + "/", &out);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::vector<std::pair<std::string, int32_t>>
findSiblingModels(const di::DiAssetStore& store, int32_t repoIdx)
{
    std::vector<std::pair<std::string, int32_t>> out;
    const di::Repository* repo = store.repo();
    if (repoIdx < 0 || !repo || (size_t)repoIdx >= repo->entries.size()) return out;
    const di::RepoEntry& self = repo->entries[(size_t)repoIdx];
    // Folder index, not a full sweep: this runs on EVERY model load to fill the
    // attachments panel, and walking all 551,524 entries for the handful that
    // share a folder was pure overhead on the click path.
    const std::vector<size_t>* sibs = repo->byFolder(self.folderIdx);
    if (!sibs) return out;
    for (size_t i : *sibs) {
        if ((int32_t)i == repoIdx) continue;
        const di::RepoEntry& e = repo->entries[i];
        const std::string& ty = repo->typeOf(e);
        if (ty != "Model" && ty != "LodModel") continue;
        out.emplace_back(e.name, (int32_t)i);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

// Keys are "<exact parent path>/<clip segment lowercased>". Only the clip
// segment is case-folded, because that is exactly what collectAnimsUnder does:
// it compares the parent prefix byte-for-byte and only the "ani"/"ain" segment
// case-insensitively. Folding the whole path here would make the filter claim
// clips for a model whose folder casing does not actually match on disk.
void AnimFolderIndex::build(const di::MpkIndex& mpk)
{
    m_dirs.clear();
    const auto& entries = mpk.entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string& n = entries[i].name;
        if (n.size() <= 5 || n.compare(n.size() - 5, 5, ".anim") != 0) continue;
        const size_t slash = n.find_last_of('/');
        if (slash == std::string::npos) continue;
        const size_t seg = n.find_last_of('/', slash - 1);
        if (seg == std::string::npos) continue;      // clips are never at root
        std::string key = n.substr(0, seg + 1);      // parent, trailing '/'
        for (size_t k = seg + 1; k < slash; ++k) {
            const char c = n[k];
            key += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        m_dirs[key]++;
    }
    m_built = true;
}

uint32_t AnimFolderIndex::clipCount(const di::Repository& repo, int32_t repoIdx) const
{
    uint32_t found = 0;
    forEachClipBase(repo, repoIdx, [&](const std::string& base) {
        for (const char* seg : {"/ani", "/ain"}) {
            auto it = m_dirs.find(base + seg);
            if (it != m_dirs.end()) found += it->second;
        }
        return found != 0;
    });
    return found;
}

} // namespace di
