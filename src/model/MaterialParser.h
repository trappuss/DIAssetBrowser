#pragma once
// Material blob -> texture-map bindings. Format measured on the live golden
// (f_barbarian_yifu_t07_004_mat, 2026-08-01):
//
//   ZZZ4 -> '.MESSIAH' tag, small header, then one record per shader variant
//   (golden: PBR_Monster / PBR_Skin / PBR_Monster), each record carrying a
//   newline-separated key=value TEXT block:
//     tBaseMap=11d9765c-9a0a-4b34-b6a6-c14b703c3d36    <- texture GUIDs
//     tNormalMap=..., tMixMap=..., tEmissionMap=...
//     cMetallic=0, cRoughnessLow=0, cRoughnessHigh=1   <- scalar constants
//   All three golden records bind the SAME texture GUIDs, and every GUID
//   matched the Material's repository rels exactly. So this parser scans the
//   whole inflated body for the known t* keys and takes the first non-null
//   GUID per key — framing-free, robust to record-header fields we have not
//   fully mapped. The all-zero GUID means "not bound" and is skipped.
//
// _m channel semantics (measured 2026-08-01 on wuqi/metal vs yifu/cloth):
//   G is BINARY 0|255 in both (metallic mask), R is a smooth mid-range spread
//   (roughness), B darkens only in weapon crevices and is flat 255 on the flat
//   garment (ambient occlusion). => R=roughness G=metallic B=AO.

#include <array>
#include <cstdint>
#include <string>

namespace di {

struct MaterialMaps {
    // 32-char lowercase hex (dashes stripped) — feed to blobForHash. Empty =
    // the material binds no such map.
    std::string baseMap;      // tBaseMap      (_d diffuse)
    std::string normalMap;    // tNormalMap    (_n tangent-space)
    std::string mixMap;       // tMixMap       (_m R=rough G=metal B=AO)
    std::string emissionMap;  // tEmissionMap / tEmmsiveMap (_e)
    std::string shader;       // first record's shader name, for the log

    // Animated-emissive FX constants (measured on live PBR_Monster cosmetics;
    // 81% of a 400-material sample carry an active arcane layer). Defaults
    // mean "layer off" so unparsed/absent keys change nothing.
    std::array<float, 3> emissionColor{1, 1, 1};    // cEmmsionColor (sic)
    bool                 emissionColorSet = false;
    std::array<float, 3> arcaneColor{0, 0, 0};      // cArcaneColor
    float                arcaneIntensity = 0.0f;    // cArcaneIntensity
    std::array<float, 4> arcaneScroll{1, 1, 0, 0};  // cArcaneScrolling
    std::array<float, 4> star1{0, 0, 0, 0};         // _StarUV1 + _StarSpeed1
    std::array<float, 4> star2{0, 0, 0, 0};         // _StarUV2 + _StarSpeed2
    std::array<float, 3> fresnelColor{0, 0, 0};     // cFresnelColor
    float                fresnelIntensity = 0.0f;   // cFresnelIntensity
    float                fresnelRange     = 2.0f;   // cFresnelRange
};

// Parse a Material blob (ZZZ4-wrapped or already inflated). Returns false with
// *err set when the blob is not a MESSIAH material; a material with zero bound
// maps still returns true.
bool parseMaterial(const uint8_t* data, size_t len, MaterialMaps* out,
                   std::string* err);

} // namespace di
