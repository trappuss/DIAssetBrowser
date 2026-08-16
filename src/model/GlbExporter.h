#pragma once
// Rigged .glb writer, v2: multi-part outfits, real inverse-bind matrices,
// rest-pose TRS from the .skeleton tree / skin binds, and glTF animation
// channels straight from decoded CHAR::ANIM clips.
//
// Conventions (all measured on the live data, see di-browser-audit.md):
//   * engine affines are ROW-vector 4x3 (rows 0-2 rotation, row 3 translation);
//     world = local @ parentWorld; skin = invBind @ world.
//   * a glTF matrix is the TRANSPOSE of the row affine, stored column-major —
//     which is byte-for-byte the row affine's rows flattened with 0/0/0/1
//     appended per column (same layout the GL uploader uses).
//   * decoded 40-bit anim quats are COLUMN-convention rotations, so they go
//     into glTF node rotations DIRECTLY; rest rotations come from transposing
//     the row-affine rotation block.
//
// Output: glTF 2.0 binary — one mesh+primitive per part, per-part materials
// (base colour / normal / emissive / ORM swizzle), per-part skins over one
// shared node tree, optional animations (translation+rotation samplers).

#include <QImage>
#include <QString>

#include <memory>
#include <vector>

#include "model/AnimParser.h"
#include "model/MeshTextures.h"
#include "model/MeshParser.h"
#include "model/SkeletonTreeParser.h"
#include "model/SkinSkeletonParser.h"

namespace GlbExporter {

struct Part {
    std::shared_ptr<const di::MeshData> mesh;         // required
    MeshTextures textures;
    std::shared_ptr<const di::SkinSkeleton> skel;     // null = centroid fallback
    QString name;
};

struct AnimExport {
    QString name;
    std::shared_ptr<const di::AnimClip> clip;
};

// Multi-part writer. hierarchy/locals may be null (Biped heuristics and
// skin-derived rest poses still apply). anims empty = static export.
// Returns false with *err set on failure.
bool writeGlb(const QString& outPath, const std::vector<Part>& parts,
              const di::BoneParents* hierarchy, const di::BoneLocals* locals,
              const std::vector<AnimExport>& anims, const QString& sceneName,
              QString* err);

// Legacy single-part convenience wrapper (static, no locals).
bool writeGlb(const QString& outPath, const di::MeshData& mesh,
              const MeshTextures& textures, const di::SkinSkeleton* skel,
              const di::BoneParents* hierarchy,
              const QString& meshName, QString* err);

} // namespace GlbExporter
