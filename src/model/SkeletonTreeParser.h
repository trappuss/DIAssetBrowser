#pragma once
// Diablo Immortal .skeleton parser — the authoritative bone hierarchy.
//
// A .skeleton is a Messiah binary property tree (magic C1 59 41 0D after ZZZ4):
//   u32 magic, i32 fileSize, i32 zero
//   u8  dictCount; dictCount x null-terminated key strings; u8 zero
//   u32 dataStartOffset (relative to offset 12), u32 zero
//   varint elementCount
//   elementCount x Descriptor { u8 dictIndex; varint childCount }
//   elementCount x Element    { u8 zero; u8 type; payload }
//     type 0 none · 1 string(nul) · 2/4 u16+u16 · 3 u8 bool · 5 f32
//     type 6 u16 count + u16 unk + count x f32          (measured 2026-08-01)
// The tree is reconstructed BREADTH-FIRST from the child counts (pre-order
// produces garbage — measured). Root/node subtree = the full skeleton:
// nested `node` entries with `identifier` names — including every
// bone_software_* / cloth bone with its true parent. (SoftBoneChains repeats
// the cloth chains; redundant for parenting, so not extracted.)
//
// Format basis: CucFlavius/Zee DiabloImmortal_Skeleton.bt + measurement on
// f_barbarian.skeleton (6,331 elements parse to exact EOF; 413 named nodes).

#include <cstdint>
#include <string>
#include <unordered_map>

namespace di {

// name -> parent name ("" for top-level nodes like "Scene Root").
using BoneParents = std::unordered_map<std::string, std::string>;

// LOCAL (parent-relative) rest transform of a node, as a row-vector 4x3
// affine: rows 0-2 rotation, row 3 translation — the same convention as
// SkinBone::invBind and AnimPose worlds. Stored in the tree as `transform`
// children row0..row3, each a string of three floats. VERIFIED 2026-08-02:
// accumulating world = local @ parentWorld down the tree reproduces the
// SkinSkeleton bind worlds of all 57 f_barbarian_yifu_t07_004 bones at
// 1.6e-6 median / 3.7e-6 max error.
struct BoneLocal {
    float m[12];
};
using BoneLocals = std::unordered_map<std::string, BoneLocal>;

// Parse an INFLATED .skeleton (call through MpkIndex::readAsset) and extract
// the node-identifier hierarchy. Returns false with *err on structural failure.
// locals (optional) receives each named node's local rest transform when the
// node carries a complete row0..row3 `transform` block.
bool parseSkeletonHierarchy(const uint8_t* data, size_t len, BoneParents* out,
                            std::string* err, BoneLocals* locals = nullptr);

// out = a @ b for row-vector 4x3 affines (rows 0-2 rotation, row 3
// translation) — the one convention the whole pipeline uses.
void mul43Row(const float* a, const float* b, float* out);

// Bind-pose WORLD transform of one node, accumulated up the tree
// (world = local @ parentWorld, row-vector 4x3 as above). False when the node
// (or an ancestor) has no local transform. Depth-capped against name cycles.
bool worldOfBone(const BoneParents& parents, const BoneLocals& locals,
                 const std::string& name, float out[12]);

} // namespace di
