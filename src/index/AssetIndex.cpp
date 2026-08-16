#include "index/AssetIndex.h"

#include <QElapsedTimer>
#include <QSet>

#include <unordered_map>

#include "app/AppPaths.h"
#include "index/ItemNames.h"
#include "index/NameTranslator.h"
#include "model/ModelResolve.h"

std::shared_ptr<AssetIndex> AssetIndex::build(std::shared_ptr<di::DiAssetStore> store)
{
    QElapsedTimer timer;
    timer.start();

    auto idx = std::make_shared<AssetIndex>();
    idx->store = store;
    idx->buildVersion = QString::fromLatin1(store->buildVersion().c_str());

    // Load external real-name overrides once per build into a LOCAL table
    // (no shared global — overlapping builds can't race). Missing file = empty.
    const ItemNames::Table itemNames =
        ItemNames::load(AppPaths::file(QStringLiteral("di_item_names.csv")));

    const auto& entries = store->mpk().entries();
    const di::Repository* repo = store->repo();

    // blob entry id -> repository entry index (inverse of the hash bridge).
    std::unordered_map<uint32_t, int32_t> blob2repo;
    if (repo) {
        blob2repo.reserve(repo->entries.size() * 2);
        for (size_t r = 0; r < repo->entries.size(); ++r) {
            const size_t blob = store->blobForHash(repo->entries[r].hashHex);
            if (blob != (size_t)-1)
                blob2repo.emplace((uint32_t)blob, (int32_t)r);
        }
    }

    // Clip-folder index, built once here so the Models tab's animated/static
    // filter is a hash lookup per row instead of findFolderAnims' full-archive
    // scan (which would be 677k scans over 677k rows).
    di::AnimFolderIndex animIdx;
    if (repo) animIdx.build(store->mpk());

    QSet<QString> typeSet;
    idx->rows.reserve((qsizetype)entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const di::MpkEntry& e = entries[i];
        AssetRow row;
        row.entryId = (uint32_t)i;
        row.size    = e.length;
        row.mpkName = QString::fromLatin1(e.name.c_str());
        row.display = row.mpkName;

        auto it = blob2repo.find((uint32_t)i);
        if (it != blob2repo.end() && repo) {
            row.repoIdx = it->second;
            const di::RepoEntry& re = repo->entries[(size_t)it->second];
            row.display = QString::fromLatin1(repo->pathOf(re).c_str());
            const std::string& t = repo->typeOf(re);
            if (!t.empty()) row.type = QString::fromLatin1(t.c_str());
        }
        if (row.type.isEmpty()) {
            // extension fallback (lowercased); "-" when there is none
            const int slash = row.mpkName.lastIndexOf(QLatin1Char('/'));
            const int dot   = row.mpkName.lastIndexOf(QLatin1Char('.'));
            row.type = (dot > slash && dot >= 0)
                           ? row.mpkName.mid(dot + 1).toLower()
                           : QStringLiteral("-");
        }
        // facet keys: first / second segment of the LOGICAL path — repo-resolved
        // rows only. Physical names all start "Package/<2-hex-shard>/", which
        // floods Category with one giant "Package" bucket and Subcategory with
        // 256 hex shards (audit finding).
        if (row.repoIdx >= 0) {
            const int s1 = row.display.indexOf(QLatin1Char('/'));
            if (s1 > 0) {
                row.cat1 = row.display.left(s1);
                const int s2 = row.display.indexOf(QLatin1Char('/'), s1 + 1);
                if (s2 > s1 + 1)
                    row.cat2 = row.display.mid(s1 + 1, s2 - s1 - 1);
            }
        }
        if (repo && row.repoIdx >= 0 &&
            (row.type == QLatin1String("Model") ||
             row.type == QLatin1String("LodModel") ||
             row.type == QLatin1String("Mesh")))
            row.animClips = animIdx.clipCount(*repo, row.repoIdx);
        // A datamined/crowd-sourced real set name (if any) wins over the
        // decoded structural name.
        row.meaning = NameTranslator::translate(row.display, row.type);
        if (const QString real = ItemNames::nameFor(itemNames, row.display);
            !real.isEmpty())
            row.meaning = real;
        {
            const NameTranslator::Facets f =
                NameTranslator::facetsOf(row.display, row.cat1, row.cat2);
            row.cls    = f.cls;
            row.slot   = f.slot;
            row.player = f.player;
        }
        typeSet.insert(row.type);
        idx->typeCounts[row.type]++;
        if (!row.cat1.isEmpty()) idx->catCounts[row.cat1]++;
        if (!row.cat2.isEmpty()) idx->subcatCounts[row.cat2]++;
        if (!row.cls.isEmpty())  idx->classCounts[row.cls]++;
        if (!row.slot.isEmpty()) idx->slotCounts[row.slot]++;
        if (row.player)          idx->playerCount++;
        idx->rows.push_back(std::move(row));
    }

    idx->types = QStringList(typeSet.begin(), typeSet.end());
    idx->types.sort(Qt::CaseInsensitive);
    idx->loadMs = (double)timer.elapsed();
    return idx;
}
