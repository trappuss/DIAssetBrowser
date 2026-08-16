#pragma once
// Diablo Immortal (NetEase Messiah) mesh parser — framework-free (std only).
// Ported from the proven Python reference (diasset/parsing/mesh_parser.py),
// itself verified against real files via TaylorMouse's GriffonStudios layout:
//
//   ZZZ4 container -> LZ4 block, then:
//   0x00  u8   version (the '.' of '.MESSIAH')
//   0x01  7s   'MESSIAH'
//   0x08  u32  type (1 = no mesh data)
//   0x12  u16  submesh count
//   0x14  u32  total vertex count
//   0x18  u32  total index count
//   0x1C  4 x [u16 len][ascii] vertex-stream format names (e.g. P3F_N4B_T2F)
//         40-byte bbox/center/radius block (skipped)
//         submesh table: max(count,1) x [u32 startIndex][u32 idxCount]
//                                       [u32 startVert][u32 vertCount]
//         index buffer — ALWAYS u16 (verified: diablofans loader reads
//         Uint16 unconditionally; a mesh never exceeds 65535 verts)
//         vertex streams, one per named chunk
//
// Component tokens: letter + count + type (F=4 bytes, H=2, B=1). Extracted:
// P*F position, T2F/T2H first UV, W4B weights + I4B bone indices (skinning).
// Everything else is skipped by size. Streams that run short keep what fits.

#include <cstdint>
#include <string>
#include <vector>

namespace di {

struct SubMeshRange {
    uint32_t startIndex = 0;
    uint32_t indexCount = 0;
    uint32_t startVert  = 0;
    uint32_t vertCount  = 0;
};

struct MeshData {
    std::vector<float>    positions;   // 3 per vertex
    std::vector<float>    normals;     // 3 per vertex (empty = none) — decoded
                                       // from the Q4H tangent-frame quaternion:
                                       // n = -sign(w) * (q rotate +Z). Measured
                                       // 2026-08-01 vs geometric normals on two
                                       // golden meshes: mean dot 0.958 / 0.994.
    std::vector<float>    tangents;    // 4 per vertex (empty = none): xyz =
                                       // q rotate +Y (measured vs UV gradients,
                                       // dot 0.98); w = handedness so that
                                       // bitangent = w * cross(normal, tangent)
                                       // (measured agreement 0.999/1.000).
    std::vector<float>    uv0;         // 2 per vertex (empty = none)
    std::vector<uint8_t>  boneIndices; // 4 per vertex (empty = unskinned)
    std::vector<float>    boneWeights; // 4 per vertex (empty = unskinned)
    std::vector<uint32_t> indices;     // empty = draw as point cloud
    std::vector<SubMeshRange> submeshes;
    std::vector<std::string>  streams; // the 4 chunk names, for display
    uint32_t declaredVerts = 0;
    bool     skinned = false;

    size_t vertexCount() const { return positions.size() / 3; }
};

// Parse a mesh blob (ZZZ4-wrapped or already-inflated). Returns false with
// *err set ("no mesh data (type=1)" is common — Model-less placeholder).
bool parseMesh(const uint8_t* data, size_t len, MeshData* out, std::string* err);

// IEEE half -> float (for T2H UV streams).
float halfToFloat(uint16_t h);

} // namespace di
