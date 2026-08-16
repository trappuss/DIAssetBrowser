#pragma once
// Diablo Immortal .SkinSkeleton parser — the per-mesh skin binding: the bones a
// mesh is weighted to (W4B_I4B indices reference THIS list), each with its 4x3
// world->bone inverse-bind matrix. Ported from the Python reference
// (diasset/parsing/skinskeleton_parser.py):
//
//   .MESSIAH magic (type 1)
//   0x12  u16 bone count
//   0x14  per bone: [u8 nameLen][12 x f32 = 4x3 matrix][ascii name]
//
// Bind-pose joint position in MESH space is -R @ t (NOT -R^T @ t) — verified in
// the reference against weighted-centroid joints (residual 0.047, scale 1.00).

#include <cstdint>
#include <string>
#include <vector>

namespace di {

struct SkinBone {
    std::string name;
    float invBind[12];   // 4x3: rows 0-2 rotation, row 3 translation
    float world[3];      // bind-pose joint position in mesh space (-R @ t)
};

struct SkinSkeleton {
    std::vector<SkinBone> bones;
};

// Parse an inflated .SkinSkeleton blob (call through MpkIndex::readAsset).
bool parseSkinSkeleton(const uint8_t* data, size_t len, SkinSkeleton* out,
                       std::string* err);

} // namespace di
