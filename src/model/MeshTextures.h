#pragma once
// Decoded material textures shared by the viewport and the glb exporter.
// Any image may be null.

#include <QImage>

#include <array>

// Animated material-FX parameters, measured on live PBR_Monster materials
// (FX_RESEARCH.md addendum; golden: f_bloodknight_toukui_all_sz05_007_mat).
// These — not ParticleSystems — carry the bulk of DI's armor glow: a 400-sample
// scan measured 326 cosmetic materials (81%) with an ACTIVE arcane-scroll layer
// and 286 with star-sparkle layers. All layers are masked by the emissive map,
// so materials without an emissive stay exactly as before.
struct MatFxParams {
    std::array<float, 3> emissionColor{1, 1, 1};   // cEmmsionColor (sic) tint
    std::array<float, 3> arcaneColor{0, 0, 0};     // cArcaneColor
    float                arcaneIntensity = 0.0f;   // cArcaneIntensity
    std::array<float, 4> arcaneScroll{1, 1, 0, 0}; // cArcaneScrolling su,sv,du,dv
    std::array<float, 4> star1{0, 0, 0, 0};        // _StarUV1.xy, _StarSpeed1.xy
    std::array<float, 4> star2{0, 0, 0, 0};        // _StarUV2.xy, _StarSpeed2.xy
    std::array<float, 3> fresnelColor{0, 0, 0};    // cFresnelColor rim tint
    float fresnelIntensity = 0.0f;                 // cFresnelIntensity (0 = off)
    float fresnelRange     = 2.0f;                 // cFresnelRange (rim power)
    bool  emissionColorSet = false;                // cEmmsionColor present+nonzero

    bool arcaneOn() const
    {
        return arcaneIntensity > 0.0001f &&
               (arcaneColor[0] + arcaneColor[1] + arcaneColor[2]) > 0.0001f;
    }
    bool starOn() const
    {
        return (star1[2] != 0.0f || star1[3] != 0.0f) && star1[0] != 0.0f;
    }
    // Anything time-varying? (drives the viewport's repaint timer)
    bool animated() const
    {
        return (arcaneOn() &&
                (arcaneScroll[2] != 0.0f || arcaneScroll[3] != 0.0f)) ||
               starOn();
    }
};

struct MeshTextures {
    QImage diffuse;
    QImage normal;     // tangent-space RGB (measured: means ~128/128/245)
    QImage mix;        // R=roughness G=metallic B=AO (measured — MaterialParser.h)
    QImage emissive;
    MatFxParams fx;    // animated emissive layers (see above)
};
