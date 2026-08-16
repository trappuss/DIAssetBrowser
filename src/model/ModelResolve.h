#pragma once
// Repository dependency-chain resolution shared by the Models tab and the
// thumbnail provider. Chain (measured on the live repository, 2026-08-01):
//   Model    -> deps { Mesh, Material, [SkinSkeleton] }  (exactly 1 Mesh +
//                1 Material on every one of the 68,946 Models — measured)
//   LodModel -> wraps a Model (one more hop)
//   Material -> texture GUIDs come from the material BLOB's key=value text
//               (tBaseMap/tNormalMap/tMixMap/tEmissionMap — see
//               MaterialParser.h); the name-suffix convention (_d/_n/_m/_e
//               over the Material's repository rels) remains as fallback when
//               the blob is unreadable.

#include <QString>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "model/MaterialParser.h"
#include "model/MeshTextures.h"
#include "store/AssetStore.h"

struct AssetRow;

namespace di {

// Blob entry ids into store.mpk().entries(); SIZE_MAX = not resolved.
struct ResolvedModel {
    size_t meshBlob = (size_t)-1;
    size_t texBlob  = (size_t)-1;   // diffuse  (tBaseMap  / _d)
    size_t nrmBlob  = (size_t)-1;   // normal   (tNormalMap/ _n)
    size_t mixBlob  = (size_t)-1;   // mix map  (tMixMap   / _m)  R=rough G=metal B=AO
    size_t emiBlob  = (size_t)-1;   // emissive (tEmissionMap/_e)
    size_t skinBlob = (size_t)-1;
    int32_t matRepo = -1;           // repository index of the Material used
                                    // (variant stepping keys off ITS name —
                                    // some pieces share another piece's
                                    // material, e.g. monk bijia -> yifu mat)
    MatFxParams matFx;              // animated-emissive constants from the
                                    // material blob (arcane/star layers)
    QString note;                   // texture provenance, for UI + log
};

// Full chain for the Models tab. repoIdx must be the row's repository index
// (< 0 returns an empty result with a note).
ResolvedModel resolveModelChain(const di::DiAssetStore& store, int32_t repoIdx);

// Cheap variant for thumbnails: only the diffuse blob (Texture2D rows return
// their own blob id). SIZE_MAX when nothing usable.
size_t resolveThumbTexture(const di::DiAssetStore& store, int32_t repoIdx,
                           uint32_t entryId, const QString& type);

// Texture blobs of one specific Material repository entry — used for dye /
// awakened variant stepping (measured: variants ship as sibling Materials
// named "<model>_<b1|b2|g1|g2|dye0|...>_mat" over the SAME mesh). Only the
// tex/nrm/mix/emi blobs and note are filled; mesh/skin stay SIZE_MAX.
ResolvedModel resolveMaterialTextures(const di::DiAssetStore& store,
                                      int32_t matRepoIdx);

// The entry's .skeleton file by path convention, walking UP the folder chain
// ("Char/f_barbarian" -> "Package/Char/f_barbarian/f_barbarian.skeleton";
// sub-folders inherit the rig one level up — verified on the live index).
// Shared by the Models tab and the Bulk .glb pipeline. SIZE_MAX = none.
size_t findSkeletonByConvention(const di::DiAssetStore& store, int32_t repoIdx);

// The entry's playable clips, sorted by stem: {stem, mpk entry id} pairs.
// Looks in the per-asset folder "<folder>/<asset>/{ani,ain}/" first, then walks
// UP the folder chain; the first level with any clips wins. Both spellings of
// the clip folder are accepted and matched case-insensitively (measured: 282
// clips ship under "ain", 5 under "Ani" — Char/sorceress_f is entirely "ain").
std::vector<std::pair<std::string, size_t>>
findFolderAnims(const di::DiAssetStore& store, int32_t repoIdx);

// Every *.anim directly under "Package/<folder>/{ani|ain|Ani}/", sorted by stem.
// Folder-PATH based (the Wardrobe scans a whole class by name, not from a repo
// row), and accepts all three clip-folder spellings the same way findFolderAnims
// does — so Char/sorceress_f (100% "ain") is no longer silently animation-less.
// {stem, mpk entry id} pairs.
std::vector<std::pair<std::string, size_t>>
findClassAnims(const di::DiAssetStore& store, const std::string& folder);

// Sibling model entries in the SAME repository folder as repoIdx (type Model or
// LodModel), excluding repoIdx itself. Returns {name, repoIdx} sorted by name.
// Used by the Models tab's attachments panel to offer the parts that belong to
// one creature (a body + its _weiba tail, _01/_02 halves, ...).
std::vector<std::pair<std::string, int32_t>>
findSiblingModels(const di::DiAssetStore& store, int32_t repoIdx);

// Directory index of every *.anim in the archive, so "does this asset have
// clips?" costs two hash lookups per candidate folder instead of the
// full-archive scan findFolderAnims does. Built once per AssetIndex, on the
// loader thread, for the Models tab's animated/static filter.
//
// It cannot drift from findFolderAnims: both drive the SAME candidate-folder
// sequence (forEachClipBase in ModelResolve.cpp) and differ only in how they
// test a candidate.
class AnimFolderIndex {
public:
    void build(const di::MpkIndex& mpk);
    bool ready() const { return m_built; }

    // Clips findFolderAnims would return for this entry, without listing them.
    uint32_t clipCount(const di::Repository& repo, int32_t repoIdx) const;

private:
    // lowercased directory path -> number of *.anim directly inside it
    std::unordered_map<std::string, uint32_t> m_dirs;
    bool m_built = false;
};

} // namespace di
