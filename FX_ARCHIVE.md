# DIAssetBrowser — FX System Archive

> **Status: REMOVED from the tool.** This document preserves the complete
> Diablo Immortal cosmetic-FX (ParticleSystem) feature — research, cracked
> formats, and full source — so it can be restored later if wanted. It was
> pulled from the shipping tool because a faithful real-time reproduction of
> Messiah-engine particle FX proved out of scope for the viewer.
>
> What it did: parsed DI `ParticleSystem` blobs, resolved which effect binds to
> a weapon (via process-dump analysis), simulated the particles on the CPU, and
> rendered them with additive billboards + HDR bloom + twist/mask shaders,
> driven entirely by real game data.

## What was achieved (kept for reference)

- **ParticleSystem blob format** — fully cracked (typed property tree, 497/500
  blobs byte-exact). See the research section below.
- **FX→weapon binding** — worn-cosmetic bindings are NOT in readable assets;
  recovered from a live process dump (`tools/di_fx_dumpscan.py`).
- **Sprite geometry** — the `Patch` control points (posX,posY,U,V) are the real
  sprite quad + UV mapping (the data-driven answer to size/crop).
- **Simulator** — `FxRuntime`: emission, lifetime, init distributions
  (Uniform/Sphere/Cylinder), over-life curves (color/alpha/size), sub-UV
  flipbook, force, looping.
- **Renderer** — offscreen HDR FBO + separable-gaussian bloom (+ float-FBO
  fallback), twist/mask flame shaders, mesh flame cards from real geometry.
- **Offline preview harness** — `tools/fxpreview/` renders an FX from a tree
  dump + exported textures without the C++ tool (for tuning).

## Why it was removed

Reproducing a tuned mobile-game real-time FX pipeline (exact bloom/tonemap,
soft particles, blend modes, sort) 1:1 in a from-scratch OpenGL viewer was not
tractable to a quality bar worth shipping. The data extraction is solid and
reusable; the rendering fidelity was the blocker.

## To restore

1. Re-add `src/model/FxParser.{h,cpp}`, `FxResolve.{h,cpp}`, `FxRuntime.{h,cpp}`
   (full source below) and their three lines in `CMakeLists.txt`.
2. Re-apply the GLMeshView FX renderer, ModelsTab FX panel/worker, and
   WardrobeTab per-slot FX (integration notes below).
3. Tools: `tools/di_fx_dumpscan.py`, `crack-fx-dump.bat`, `run-fx-dump.bat`,
   `tools/fxpreview/`.

---

# Research (from FX_RESEARCH.md)


# Diablo Immortal FX (ParticleSystem) — measured format research

Date: 2026-08-15. Everything below was measured on the live game install
(repository of 551,524 entries; 43,935 ParticleSystem entries; parser verified
**497/500 random PS blobs byte-exact**, remaining 3 hit an unknown tag 0x30 —
skill FX, fail-soft). No guessed fields.

## Where armor FX live

- Repository type 1 = `ParticleSystem` (43,935 entries), folder `EffectCom`.
- **Nothing references ParticleSystems** in the repository — binding is by NAME:
  cosmetic FX are named `<model name>` plus optional suffixes measured on a
  5,000-entry sample (22% have an exact Model-name prefix): `""` (exact),
  `_lod`, `_color_a|b|c[_lod]`, `_aw2|_aw3[_NN][_lod]` (awakened tiers),
  `_l`/`_r` (paired shoulders etc.), `_1`/`_01`…
  The rest are skill/world FX (`fx_*`, `boss_*`, `innate_*`, …).
- PS repo rels: exactly 1 Material + N Mesh (+ occasionally Model). The meshes
  are FX geometry primitives (`Mesh_plane_*`, `Mesh_cylinder_*`,
  `Mesh_spiral_*`, `Mesh_guangshu_*` = light beam) — standard `.MESSIAH`
  meshes the existing MeshParser already reads.

## Blob container

`ZZZ4` + u32 uncompressedSize + raw-LZ4 block (same tolerant decode as
Zzz4.cpp). Decoded payload is a **typed property tree**:

```
file := u16 rootNameLen + rootName + node
node := tag byte + payload
  'p' 0x70: u32 size + raw bytes            (primitive: bool/int/float/vecN/str16/hash16)
  '~' 0x7E: u32 size + raw bytes            (same as p; seen in Patch arrays)
  '$' 0x24: u16 len + chars                 (string; texture GUIDs, parameter values)
  'v' 0x76: u32 count + count*node          (array)
  'c' 0x63: u16 len + className + u32 propCount + propCount*(u16 len + name + node)
  't' 0x74 / 'd' 0x64 / 's' 0x73: u32 propCount + props (anonymous struct / dict)
  'o' 0x6F: u8 present + (node if present)  (optional polymorphic object)
```

Primitive `p` payload interpretation is by length: 1 = bool/u8, 2 = u16 or
counted string (u16 len + chars when it matches), 4 = u32/float, 8/12/16 =
float2/3/4, 16 also = GUID hash (BaseMaterial).

## Semantic model (class census over 500 random systems)

`ParticleSystem { Name, Emitters: v[ParticleEmitter] }`
`ParticleEmitter { Name, Enable, Elements: v[<modules>] }` — element order is
TypeData first, then modules. Key modules (count in 500-system sample):

- TypeData (what is drawn):
  - `ParticleElementTypeDataMesh` (2,210): `MeshPath` = mesh GUID **string**,
    `Effect` (blend/effect mode, u8), `DoubleSided`, `FaceCamera`,
    `FixedRotation`, `YAxisLocked`, `RepeatU/V`, `CenterOffset` float3.
  - `ParticleElementTypeDataSprite` (1,941): billboard; `Facing`, `Scale`
    float2, `Pivot` float2, `Patch` v[6] of float4 (quad patch data).
  - `TypeDataCombo` (379), `TypeDataLight` (119), `TypeDataModel` (79),
    `TypeDataBlade/Beam/Trail/Decal/GPU/VATSeq` (rare).
- `ParticleElementProperty` (4,846): `MaxParticles` (u16 — **2,959 of 4,846
  are 1** → persistent overlay, the armor-FX case), `SortMode`, `Local`,
  `DepthTest/DepthWrite`, element transform `Scale`/`Rotation`/`Translation`
  (float3 each), `FadeDistance`, `WarmUpTime`, and:
  - `MaterialInfo: t { BaseMaterial p[16] (GUID, often 0), Shader str
    ("Particle" | "Effect/Fresnel" | "Effect/Twisted" | ...), Textures d
    { DiffuseTexture/MaskTexture/TwistTexture = GUID strings },
    Parameters d { stringly-typed key=value:
      ModulateColor "(r,g,b,a)", HDRScale, BloomScale,
      DiffuseUVScolling "(su,sv,du,dv)" (tile + scroll rate),
      TwistEnable/TwistStrength/TwistUVScolling, FresnelPower/FresnelAlpha,
      Soft, DepthBias, UVScale, Local, ... }, Flags u32 }`
- Spawn: `TwParticleElementSource`/`ParticleElementSource` (EmissionRate,
  EmissionBurst, ActiveDuration, -1 duration = forever),
  `InitLife/InitEmission/InitSize[/2D/3D]/InitSpin[Rate]/InitRotation[Rate]/
  InitVelocity/InitPosition/InitColor/InitSubUV` with `Distribution*` objects
  (`DistributionUniformFloat {MinValue,MaxValue}`, `UniformVector3`,
  `ConstantFloat/Vector3`, `Cylinder`, `Sphere`, `UniformVector4`).
- Over-life curves: `ColorByLife { ColorCurve.Nodes[ s{Time, Value float4} ] }`,
  `AlphaByLife`, `SizeByLife`, `Size2D/3DByLife`, `SubUVByLife`,
  `EmissionByLife`, `ControlByLife`, `SpinByLife`.
- Motion modifiers: `TwParticleElementStream/Scaler/Jitter/Force/Tracker/
  Orbitor/Magnet`, `Scatter`, `Revive`, `Sustain {Attack,Release}`.

## Rendering plan (data-faithful approximation)

Armor "aura" FX are dominated by MaxParticles=1 persistent elements:
1. **Mesh elements**: load `MeshPath` GUID mesh via existing parser, place with
   the Property transform, texture = DiffuseTexture GUID, color =
   ModulateColor × HDRScale, additive blending, UV scroll from
   DiffuseUVScolling each frame, optional Fresnel rim (Effect/Fresnel).
2. **Sprite elements**: camera-facing quad, size from InitSize/Scale, same
   material path.
3. Loop ColorByLife over a fixed period for persistent systems.

Materials in the PS's repo rels duplicate this data in `.MESSIAH` key=value
text (`Glow_submat`, `Particle`, `TwistTexture=…`) — the tree is the richer
source; the rel Material is a packaging artifact.

## FX BINDING — how effects attach to characters (measured 2026-08-15)

The name-convention and rels heuristics were WRONG as a general binding. The
real mechanism, byte-verified in `.graph` files, is an explicit attach record:

```
Effect := "EffectCom/<ParticleSystem>:<AttachBone>:<StartTime>:<flags8>[:r..][:p..][:s..]"
  e.g.  EffectCom/bq_bk_duominic_05_03_dmq:Bip001 Spine:13.5:01000000:r0.36,3.14,-1.566:p0.691,-0.045,0:s0.7,0.8,0.7
```

- Measured at scale: **10,274 of 21,047 graphs carry these; 48,163 records
  total.** The regex in `tools/di_fx_dumpscan.py` parses them exactly (22/22 on
  bq_bk_duominic_05).
- `<AttachBone>` is a rig bone name ("Bip001 Spine", "HP_hand_right",
  "Scene Root", "WorldOrigin"); `flags8` is an 8-hex mask; the trailing
  `r`/`p`/`s` tokens are the attach transform (euler rot, position, scale).
- Cutscene/timeline graphs also bind effect MODELS to a rig via JSON
  `modelData.models[["Char/.../fx_...",""]]` + a driving `graph` + `skeleton`.
- Graph-bound effects (teleport portals `fx_chuansongmen_*`, skills, NPC auras,
  showcases, back-piece idles) are therefore fully reconstructable from real
  data: parse the graph, load each `EffectCom/<ps>` with the existing FX parser,
  attach to the named bone with the transform.

### What is NOT in the readable assets

A **worn cosmetic's** idle effect is applied from the encrypted client item
table (`Data/data/*.idx`, hash-addressed, not decodable from disk) at equip
time — verified for `f_bloodknight_toukui_all_sz05_007`: zero ParticleSystems
by name/rel, zero of the 21,047 graphs reference `sz05_007` / `toukui_all`, and
no MPK config table binds it. The single `gorefiend` graph hit is an NPC
cutscene (Hera), not this cosmetic. So this binding cannot be read statically.

`tools/di_fx_dumpscan.py` streams a DiabloImmortal process dump, extracts every
attach record, greps every resident effect name, and reports refs co-located
with a cosmetic id.

### Dump result — sz11_008 crusader set (measured 2026-08-15, 3.97 GB dump)

A dump captured with the sz11_008 crusader set visible was scanned. Findings
(all from the real dump; nothing inferred):

- **The set's particle systems ARE resident and confirmed** — the effect that
  exists for this set, by census (`--grep`):
  - `fx_sz11_008_crusader_lianchui` (+ `_lod`) — the **weapon** (lianchui = mace).
  - `fx_sz11_008_Doom_crusader_dun` (+ `_lod`) — the **shield** (dun = shield).
  - `fx_sz11_008_crusader_fazhang` / `fazhangL_wp_bone` — **staff** variant.
  - `fx_sz_11_008_m_crusader_shoubi` (+ `_lod`) — **forearm/arm** effect.
  - Resident weapon hardpoints: `HP_zhushou_lianchui`, `HP_lianchui_bone`,
    `HP_zhushou_fazhang`, `HP_zhushou_danshouchui`.
- **No particle system exists for the helmet / body / shoulders / legs**
  (toukui / yifu / jianjia / tui) of sz11_008 crusader — none appear in the
  60-name `fx_sz11_008*` census nor the 154-name `fx_*crusader*` census. Their
  in-game richness is the **animated material** (emissive / arcane / fresnel),
  which the tool already renders — not a particle system.
- **The serialized `EffectCom/<ps>:<bone>:<time>:…` string is NOT present for
  the worn set.** All 63 strict attach records that landed near the tokens are
  OTHER cosmetics' idle FX (phoenix/malthael/Arthas idles, cru/war/mk skills)
  that merely sit near `sz11_008` in the heap. Zero strict records name any
  `fx_sz11_008_crusader_*` system. So for worn cosmetics the runtime binds via
  handles/pointers, not the text form — a string scan tops out at *residency +
  candidate hardpoint*, which is what we now have. (The text form IS the exact
  binding for graph-bound effects: skills, cutscenes, portals, NPC auras.)

**Faithful reconstruction that follows from this (no guessing):** load each
confirmed weapon/offhand PS in the model's own local space when that weapon
model is viewed (`fx_sz11_008_crusader_lianchui` on the mace, `_Doom_crusader_dun`
on the shield, `_crusader_fazhang` on the staff) — a weapon's own authored
effect in weapon-local space needs no external bone. Armor pieces with no PS get
material FX only. Nothing is attached to a guessed bone.


---

# Integration notes (what was wired into shared files)

- **GLMeshView**: an FX render path — additive billboard/mesh shader with
  flipbook sub-UV + twist + mask; an HDR FBO + 2-pass gaussian bloom + additive
  composite (with a float-FBO completeness fallback that draws FX directly);
  `FxRuntime` advanced per frame; FX anchored at the mesh vertex centroid
  (`m_fxAnchor`, computed in frameMesh) plus a user offset. Sprite quads built
  from each emitter's `Patch`. Public API: setFxParts / setFxSystem /
  setFxVisible / setFx{OffsetX,OffsetY,OffsetZ,Scale,BloomStrength,Exposure,
  EmitterEnabled} + fx getters.
- **ModelsTab**: an "FX" popover on the viewport strip (sliders: Anchor X/Y/Z,
  Scale, Bloom, Exposure; per-emitter enable list; bloom/particle readout); an
  FX overlay checkbox (off by default); a worker thread that resolves the FX on
  model load and emits fxReady(); the confirmed-binding table lived in
  FxResolve.
- **WardrobeTab**: per-slot FX resolve + a union of parts across slots, FX
  checkbox.
- **ModelResolve / MaterialParser / MeshTextures**: material-FX params
  (emissive / arcane / star / fresnel animated emissive) — `MatFxParams`.

---

## `src/model/FxParser.h`
```cpp
#pragma once
// Diablo Immortal ParticleSystem (FX) blob parser — format measured on the live
// game install and documented in FX_RESEARCH.md (verified 497/500 random blobs
// byte-exact). The blob is a ZZZ4-compressed typed property tree; this parser
// walks the tree and extracts the subset the viewer renders:
//
//   ParticleSystem -> Emitters[] -> Elements[]:
//     ParticleElementTypeDataMesh    (MeshPath GUID -> a normal .MESSIAH mesh)
//     ParticleElementTypeDataSprite  (camera-facing billboard quad)
//     ParticleElementProperty        (transform, MaterialInfo: shader name,
//                                     texture GUIDs, stringly parameters)
//     ParticleElementInitSize / ColorByLife / InitLife (visual scale + tint)
//
// Everything else (velocity fields, jitter, trails, GPU particles) is parsed
// structurally (so nothing desyncs) but not extracted — the armor/cosmetic FX
// this exists for are dominated by MaxParticles==1 persistent mesh/sprite
// overlays (measured: 2,959 of 4,846 Property modules in a 500-system sample).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace di {

struct FxMaterialInfo {
    std::string shader;                    // "Particle" | "Effect/Fresnel" | ...
    std::string diffuseGuid;               // 36-char GUID string, zero-GUID = none
    std::string maskGuid;
    std::string twistGuid;
    // Parameters (stringly key=value in the blob; parsed to numbers here)
    std::array<float, 4> modulateColor{1, 1, 1, 1};
    std::array<float, 4> uvScroll{1, 1, 0, 0};   // DiffuseUVScolling: su,sv,du,dv
    float hdrScale   = 1.0f;
    float bloomScale = 0.0f;
    float fresnelPower = 0.0f;             // >0 only when the shader is Fresnel
    float fresnelAlpha = 1.0f;
    bool  twistEnable  = false;
    std::array<float, 2> twistStrength{0, 0};      // TwistStrength (uv distortion)
    std::array<float, 4> twistScroll{1, 1, 0, 0};  // TwistUVScolling: tile.xy, rate.xy
};

struct FxColorKey {
    float time = 0.0f;                     // 0..1 over particle life
    std::array<float, 4> rgba{1, 1, 1, 1};
};

// One scalar key of an over-life curve (Size/Alpha/Emission/Control).
struct FxCurveKey {
    float time = 0.0f;                     // 0..1 over particle life
    float value = 0.0f;
};

// A spawn distribution (Init* modules). Sampled by the runtime; the parser only
// records the measured shape + bounds — no randomness here.
struct FxDist {
    enum Kind { None, ConstFloat, UniformFloat, ConstVec3, UniformVec3,
                Sphere, Cylinder } kind = None;
    bool set = false;
    float fMin = 0, fMax = 0;                     // float distributions
    std::array<float, 3> vMin{0, 0, 0}, vMax{0, 0, 0};   // vec3
    std::array<float, 3> center{0, 0, 0}, axis{0, 0, 0}; // sphere/cylinder
    float rMin = 0, rMax = 0;                     // sphere/cylinder radius
};

struct FxElement {
    enum Kind { None, Mesh, Sprite } kind = None;
    bool        enable = true;
    // TypeDataMesh
    std::string meshGuid;                  // 36-char GUID string
    bool doubleSided = true;
    bool faceCamera  = false;
    // TypeDataSprite
    bool spriteFacing = false;             // Facing: 1 = camera-facing billboard
    std::array<float, 2> spriteScale{1, 1};
    std::array<float, 2> spritePivot{0, 0};
    int  atlasU = 1, atlasV = 1;           // RepeatU/RepeatV = sub-UV flipbook grid
    // Patch = the sprite's actual quad/polygon: N control points, each
    // (posX, posY, U, V). This IS the sprite geometry + texture mapping — the
    // data-driven answer to sprite size, shape, pivot and UV crop.
    std::vector<std::array<float, 4>> patch;
    // Property
    int  maxParticles = 1;
    bool local        = true;
    bool depthTest    = true;
    bool depthWrite   = false;
    std::array<float, 3> scale{1, 1, 1};
    std::array<float, 3> rotation{0, 0, 0};      // radians (measured small values)
    std::array<float, 3> translation{0, 0, 0};
    float depthOffset = 0.0f;
    FxMaterialInfo material;
    // Source (emission)
    float emissionRate     = 0.0f;
    float emissionDuration = -1.0f;              // -1 = forever
    float emissionBurst    = 0.0f;               // particles at t=0
    float activeDuration   = 0.0f;               // >0 with sleepDuration = on/off cycle
    float sleepDuration    = 0.0f;
    bool  timeTriggered    = false;
    // Init distributions
    FxDist life, initSize, initVelocity, initPosition, initRotation, initRotationRate;
    float initSizeMin = 1.0f, initSizeMax = 1.0f;  // legacy static-preview mid-point
    float lifeMin = 0.0f, lifeMax = 0.0f;
    // Over-life curves (empty = not present)
    std::vector<FxColorKey> colorCurve;
    std::vector<FxCurveKey> alphaCurve;
    std::vector<FxCurveKey> sizeCurve;           // uniform SizeByLife
    std::vector<FxCurveKey> sizeCurveX, sizeCurveY;   // Size2D/3DByLife
    std::vector<FxCurveKey> emissionCurve;       // EmissionByLife (gates spawns)
    std::vector<FxCurveKey> controlCurve;        // ControlByLife (fade-in gate)
    // Sub-UV flipbook (SubUVByLife)
    bool subUv        = false;
    int  tilesPerSec  = 0;
    int  tileStartMin = 0, tileStartMax = 0;
    bool subUvLoop    = true;
    // Constant force (TwParticleElementForce)
    std::array<float, 3> force{0, 0, 0};
    bool hasForce = false;
};

struct FxEmitter {
    std::string name;
    bool enable = true;
    std::vector<FxElement> elements;       // usually 1 TypeData merged w/ props
};

struct FxSystem {
    std::string name;
    std::vector<FxEmitter> emitters;
    std::array<float, 3> bias{0, 0, 0};    // System.Property.Bias (whole-FX offset)
};

// Parse a DECOMPRESSED ParticleSystem payload (call zzz4Decode first if the
// blob still carries the ZZZ4 header). Returns false with *err set on a
// structural failure; unknown element classes are skipped, never fatal.
bool parseFxSystem(const uint8_t* data, size_t len, FxSystem* out, std::string* err);

// Debug: walk the same typed tree and append an indented text dump of EVERY
// node (class / property / value preview) to *outText — used to measure emitter
// fields (emission, velocity, size/alpha-over-life, sub-UV) the renderer does
// not yet extract, with no guessing. Returns false on structural failure.
bool dumpFxSystem(const uint8_t* data, size_t len, std::string* outText, std::string* err);

} // namespace di
```

## `src/model/FxParser.cpp`
```cpp
#include "model/FxParser.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

// Tag-tree walker for the Messiah typed-property serialization measured in
// FX_RESEARCH.md. The walker is STRUCTURAL (it can skip any value without
// understanding it), extraction is a set of hooks over (className, propName).

namespace di {
namespace {

struct Cursor {
    const uint8_t* d;
    size_t len;
    size_t off = 0;
    bool fail = false;

    bool need(size_t n) {
        if (off + n > len) { fail = true; return false; }
        return true;
    }
    uint8_t u8() { if (!need(1)) return 0; return d[off++]; }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = (uint16_t)(d[off] | (d[off + 1] << 8));
        off += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
                     ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
        off += 4;
        return v;
    }
    std::string str16() {
        const uint16_t n = u16();
        if (!need(n)) return {};
        std::string s((const char*)d + off, n);
        off += n;
        return s;
    }
};

float f32FromBytes(const uint8_t* p)
{
    float f;
    std::memcpy(&f, p, 4);
    return f;
}

// A primitive payload ('p' tag): raw bytes.
struct Prim {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool  asBool()  const { return n >= 1 && p[0] != 0; }
    int   asInt()   const {
        if (n == 1) return p[0];
        if (n == 2) return p[0] | (p[1] << 8);
        if (n == 4) return (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
        return 0;
    }
    float asFloat() const { return n == 4 ? f32FromBytes(p) : 0.0f; }
    void  floats(float* out, size_t count) const {
        for (size_t i = 0; i < count; ++i)
            out[i] = (i * 4 + 4 <= n) ? f32FromBytes(p + i * 4) : 0.0f;
    }
    // p[2+k] counted string (u16 len + chars) — Name/Shader/MeshPath use this.
    std::string asStr() const {
        if (n < 2) return {};
        const size_t sl = p[0] | (p[1] << 8);
        if (sl + 2 != n) return {};
        return std::string((const char*)p + 2, sl);
    }
};

// Parse "(a,b,c,d)" / "(a,b)" / "a" stringly parameter values (measured forms).
int parseFloatList(const std::string& s, float* out, int maxN)
{
    int n = 0;
    const char* c = s.c_str();
    while (*c && n < maxN) {
        while (*c == '(' || *c == ')' || *c == ',' || *c == ' ') ++c;
        if (!*c) break;
        char* end = nullptr;
        const float v = std::strtof(c, &end);
        if (end == c) break;
        out[n++] = v;
        c = end;
    }
    return n;
}

struct Extract {
    FxSystem* sys = nullptr;
    FxEmitter* emitter = nullptr;    // current emitter being filled
    FxElement* elem = nullptr;       // current element (TypeData or merged)
    int depthInMatInfo = 0;          // inside MaterialInfo struct
    std::string pendingDictKey;
    std::string curInit;             // suffix of the current ParticleElementInit* class
    std::vector<FxCurveKey>* curveTgt = nullptr;   // current scalar over-life curve
    bool curveColor = false;         // current curve is the ColorByLife curve
    // Debug tree dump (null in the normal render path — zero overhead).
    std::string* dump = nullptr;
    int dumpDepth = 0;
};

// Which Init distribution the current ParticleElementInit* class fills.
FxDist* distTargetFor(Extract& ex)
{
    FxElement* e = ex.elem;
    if (!e) return nullptr;
    const std::string& s = ex.curInit;
    if (s == "Life")         return &e->life;
    if (s == "Size")         return &e->initSize;
    if (s == "Velocity")     return &e->initVelocity;
    if (s == "Position")     return &e->initPosition;
    if (s == "Rotation")     return &e->initRotation;
    if (s == "RotationRate") return &e->initRotationRate;
    return nullptr;                  // Emission (intensity) etc. — not simulated
}

// One indented line into the debug dump (no-op unless dumping).
inline void dumpLine(Extract& ex, const std::string& s)
{
    if (!ex.dump) return;
    ex.dump->append((size_t)ex.dumpDepth * 2, ' ');
    ex.dump->append(s);
    ex.dump->push_back('\n');
}

// Compact human preview of a primitive payload, by measured length rules.
std::string primPreview(const Prim& v)
{
    char buf[160];
    if (v.n == 1) { std::snprintf(buf, sizeof buf, "u8=%d/bool=%d", v.p[0], v.p[0] != 0); return buf; }
    if (v.n == 2) {
        const std::string s = v.asStr();
        if (!s.empty()) { std::snprintf(buf, sizeof buf, "str16=\"%s\"", s.c_str()); return buf; }
        std::snprintf(buf, sizeof buf, "u16=%d", v.asInt()); return buf;
    }
    if (v.n == 4) { std::snprintf(buf, sizeof buf, "f32=%.4g (i32=%d)", v.asFloat(), v.asInt()); return buf; }
    if (v.n == 8)  { float f[2]; v.floats(f, 2); std::snprintf(buf, sizeof buf, "f2=(%.4g,%.4g)", f[0], f[1]); return buf; }
    if (v.n == 12) { float f[3]; v.floats(f, 3); std::snprintf(buf, sizeof buf, "f3=(%.4g,%.4g,%.4g)", f[0], f[1], f[2]); return buf; }
    if (v.n == 16) {
        const std::string s = v.asStr();
        if (!s.empty()) { std::snprintf(buf, sizeof buf, "str16=\"%s\"", s.c_str()); return buf; }
        float f[4]; v.floats(f, 4);
        std::snprintf(buf, sizeof buf, "f4=(%.4g,%.4g,%.4g,%.4g)/guid16", f[0], f[1], f[2], f[3]);
        return buf;
    }
    const std::string s = v.asStr();   // odd lengths: Name/Shader/MeshPath etc.
    if (!s.empty()) { std::snprintf(buf, sizeof buf, "str16=\"%s\"", s.c_str()); return buf; }
    std::snprintf(buf, sizeof buf, "raw[%zu]", v.n);
    return buf;
}

// Forward decl — value walker. `cls` is the owning class name, `prop` the
// property (or dict key) that owns this value.
void walkValue(Cursor& c, Extract& ex, const std::string& cls, const std::string& prop);

void applyParam(FxMaterialInfo& m, const std::string& key, const std::string& val)
{
    float f[4];
    if (key == "ModulateColor") {
        if (parseFloatList(val, f, 4) == 4)
            m.modulateColor = {f[0], f[1], f[2], f[3]};
    } else if (key == "DiffuseUVScolling") {
        if (parseFloatList(val, f, 4) == 4)
            m.uvScroll = {f[0], f[1], f[2], f[3]};
    } else if (key == "HDRScale") {
        if (parseFloatList(val, f, 1) == 1) m.hdrScale = f[0];
    } else if (key == "BloomScale") {
        if (parseFloatList(val, f, 1) == 1) m.bloomScale = f[0];
    } else if (key == "FresnelPower") {
        if (parseFloatList(val, f, 1) == 1) m.fresnelPower = f[0];
    } else if (key == "FresnelAlpha") {
        if (parseFloatList(val, f, 1) == 1) m.fresnelAlpha = f[0];
    } else if (key == "TwistEnable") {
        m.twistEnable = (val == "true");
    } else if (key == "TwistStrength") {
        if (parseFloatList(val, f, 2) == 2) m.twistStrength = {f[0], f[1]};
    } else if (key == "TwistUVScolling") {
        if (parseFloatList(val, f, 4) == 4) m.twistScroll = {f[0], f[1], f[2], f[3]};
    }
}

// Hook: a primitive property on a known class.
void applyPrim(Extract& ex, const std::string& cls, const std::string& prop, const Prim& v)
{
    FxElement* e = ex.elem;
    // System-level offset (root ParticleSystem.Property.Bias).
    if (cls == "ParticleSystem" && prop == "Bias" && v.n == 12 && ex.sys) {
        v.floats(ex.sys->bias.data(), 3);
        return;
    }
    if (cls == "ParticleEmitter" && ex.emitter) {
        if (prop == "Name")   ex.emitter->name  = v.asStr();
        if (prop == "Enable") ex.emitter->enable = v.asBool();
        return;
    }
    if (!e) return;

    // Init distributions (routed by the ParticleElementInit* class on the stack).
    if (cls.rfind("Distribution", 0) == 0 && !ex.curInit.empty()) {
        FxDist* d = distTargetFor(ex);
        if (d) {
            if (prop == "MinValue") {
                if (v.n == 4) d->fMin = v.asFloat();
                else if (v.n == 12) v.floats(d->vMin.data(), 3);
            } else if (prop == "MaxValue") {
                if (v.n == 4) d->fMax = v.asFloat();
                else if (v.n == 12) v.floats(d->vMax.data(), 3);
            } else if (prop == "Value") {
                if (v.n == 4) { d->fMin = d->fMax = v.asFloat(); }
                else if (v.n == 12) { v.floats(d->vMin.data(), 3); d->vMax = d->vMin; }
            } else if (prop == "Center")    { v.floats(d->center.data(), 3); }
            else if (prop == "Axis")        { v.floats(d->axis.data(), 3); }
            else if (prop == "MinRadius")   { d->rMin = v.asFloat(); }
            else if (prop == "MaxRadius")   { d->rMax = v.asFloat(); }
        }
        return;
    }

    if (cls == "ParticleElementTypeDataMesh") {
        if (prop == "MeshPath")    e->meshGuid   = v.asStr();
        if (prop == "DoubleSided") e->doubleSided = v.asBool();
        if (prop == "FaceCamera")  e->faceCamera  = v.asBool();
        if (prop == "Enable")      e->enable      = v.asBool();
    } else if (cls == "ParticleElementTypeDataSprite") {
        if (prop == "Scale")  v.floats(e->spriteScale.data(), 2);
        if (prop == "Pivot")  v.floats(e->spritePivot.data(), 2);
        if (prop == "Facing") e->spriteFacing = v.asBool();
        if (prop == "RepeatU") e->atlasU = v.asInt() > 0 ? v.asInt() : 1;
        if (prop == "RepeatV") e->atlasV = v.asInt() > 0 ? v.asInt() : 1;
        if (prop == "Patch" && v.n == 16) {          // one control point (posX,posY,U,V)
            std::array<float, 4> q; v.floats(q.data(), 4);
            e->patch.push_back(q);
        }
        if (prop == "Enable") e->enable = v.asBool();
    } else if (cls == "ParticleElementProperty") {
        if (ex.depthInMatInfo > 0) {
            if (prop == "Shader") e->material.shader = v.asStr();
            return;
        }
        if (prop == "MaxParticles") e->maxParticles = v.asInt();
        if (prop == "Local")        e->local        = v.asBool();
        if (prop == "DepthTest")    e->depthTest    = v.asBool();
        if (prop == "DepthWrite")   e->depthWrite   = v.asBool();
        if (prop == "Scale")        v.floats(e->scale.data(), 3);
        if (prop == "Rotation")     v.floats(e->rotation.data(), 3);
        if (prop == "Translation")  v.floats(e->translation.data(), 3);
        if (prop == "DepthOffset")  e->depthOffset  = v.asFloat();
    } else if (cls == "ParticleElementSource" || cls == "TwParticleElementSource") {
        if (prop == "EmissionRate")     e->emissionRate     = v.asFloat();
        if (prop == "EmissionDuration") e->emissionDuration = v.asFloat();
        if (prop == "EmissionBurst")    e->emissionBurst    = (float)v.asInt();
        if (prop == "ActiveDuration")   e->activeDuration   = v.asFloat();
        if (prop == "SleepDuration")    e->sleepDuration    = v.asFloat();
        if (prop == "TimeTriggered")    e->timeTriggered    = v.asBool();
    } else if (cls == "ParticleElementSubUVByLife") {
        if (prop == "TilesPerSecond") e->tilesPerSec  = v.asInt();
        if (prop == "TileStartMin")   e->tileStartMin = v.asInt();
        if (prop == "TileStartMax")   e->tileStartMax = v.asInt();
        if (prop == "Loop")           e->subUvLoop    = v.asBool();
    } else if (cls == "TwParticleElementForce") {
        if (prop == "Force" && v.n == 12) v.floats(e->force.data(), 3);
    }
}

// Walk one class node ('c' tag already consumed up to the class name).
void walkClass(Cursor& c, Extract& ex)
{
    const std::string cls = c.str16();
    const uint32_t nprops = c.u32();
    if (c.fail) return;

    // Element lifecycle: a TypeData class BEGINS a new element; Property and
    // Init/Curve classes MERGE into the current one (measured ordering:
    // TypeData first in Elements[]).
    const bool isTypeMesh   = cls == "ParticleElementTypeDataMesh";
    const bool isTypeSprite = cls == "ParticleElementTypeDataSprite";
    const bool isOtherType  = !isTypeMesh && !isTypeSprite &&
                              cls.rfind("ParticleElementTypeData", 0) == 0;
    if (ex.emitter && (isTypeMesh || isTypeSprite || isOtherType)) {
        ex.emitter->elements.emplace_back();
        ex.elem = &ex.emitter->elements.back();
        ex.elem->kind = isTypeMesh ? FxElement::Mesh
                                   : isTypeSprite ? FxElement::Sprite
                                                  : FxElement::None;
    }
    if (cls == "ParticleEmitter" && ex.sys) {
        ex.sys->emitters.emplace_back();
        ex.emitter = &ex.sys->emitters.back();
        ex.elem = nullptr;
    }

    // Module markers (restored at the end of this class frame).
    const std::string savedInit = ex.curInit;
    if (cls.rfind("ParticleElementInit", 0) == 0)
        ex.curInit = cls.substr(std::string("ParticleElementInit").size());
    if (ex.elem && cls == "ParticleElementSubUVByLife") ex.elem->subUv   = true;
    if (ex.elem && cls == "TwParticleElementForce")     ex.elem->hasForce = true;
    // Entering a Distribution nested in an Init* module: fix its shape now, so a
    // Sphere/Cylinder with no Min/MaxValue prims still records its kind.
    if (ex.elem && !ex.curInit.empty() && cls.rfind("Distribution", 0) == 0) {
        if (FxDist* d = distTargetFor(ex)) {
            d->set = true;
            if      (cls == "DistributionUniformFloat")   d->kind = FxDist::UniformFloat;
            else if (cls == "DistributionConstantFloat")  d->kind = FxDist::ConstFloat;
            else if (cls == "DistributionUniformVector3") d->kind = FxDist::UniformVec3;
            else if (cls == "DistributionConstantVector3")d->kind = FxDist::ConstVec3;
            else if (cls == "DistributionSphere")         d->kind = FxDist::Sphere;
            else if (cls == "DistributionCylinder")       d->kind = FxDist::Cylinder;
        }
    }

    if (ex.dump) {
        char b[96];
        std::snprintf(b, sizeof b, "class %s {  (%u prop%s)", cls.c_str(),
                      nprops, nprops == 1 ? "" : "s");
        dumpLine(ex, b);
    }
    ++ex.dumpDepth;
    for (uint32_t i = 0; i < nprops && !c.fail; ++i) {
        const std::string prop = c.str16();
        walkValue(c, ex, cls, prop);
    }
    --ex.dumpDepth;
    dumpLine(ex, "}");
    ex.curInit = savedInit;
}

void walkValue(Cursor& c, Extract& ex, const std::string& cls, const std::string& prop)
{
    if (c.fail || !c.need(1)) { c.fail = true; return; }
    const uint8_t tag = c.d[c.off++];
    switch (tag) {
    case 0x70:   // 'p'
    case 0x7e: { // '~'
        const uint32_t n = c.u32();
        if (!c.need(n)) return;
        Prim v{c.d + c.off, n};
        c.off += n;
        if (ex.dump) dumpLine(ex, prop + " = " + primPreview(v));
        applyPrim(ex, cls, prop, v);
        // Over-life curve node values: s-struct Time/Value routed here.
        if (ex.elem && cls == "__curve__") {
            if (ex.curveColor && !ex.elem->colorCurve.empty()) {
                if (prop == "Time" && n == 4)
                    ex.elem->colorCurve.back().time = v.asFloat();
                if (prop == "Value" && n == 16)
                    v.floats(ex.elem->colorCurve.back().rgba.data(), 4);
            } else if (ex.curveTgt && !ex.curveTgt->empty()) {
                if (prop == "Time" && n == 4)  ex.curveTgt->back().time  = v.asFloat();
                if (prop == "Value" && n == 4) ex.curveTgt->back().value = v.asFloat();
            }
        }
        break;
    }
    case 0x24: { // '$' string — texture GUIDs + Parameters values
        const std::string s = c.str16();
        if (ex.dump) dumpLine(ex, prop + " = $\"" + s + "\"");
        if (ex.elem && ex.depthInMatInfo > 0) {
            if (prop == "DiffuseTexture") ex.elem->material.diffuseGuid = s;
            else if (prop == "MaskTexture") ex.elem->material.maskGuid = s;
            else if (prop == "TwistTexture") ex.elem->material.twistGuid = s;
            else applyParam(ex.elem->material, prop, s);
        }
        break;
    }
    case 0x76: { // 'v' array
        const uint32_t n = c.u32();
        const bool isCurveNodes = ex.elem && prop == "Nodes" &&
                                  (ex.curveColor || ex.curveTgt);
        if (ex.dump) {
            char b[64];
            std::snprintf(b, sizeof b, "%s = array[%u] [", prop.c_str(), n);
            dumpLine(ex, b);
        }
        ++ex.dumpDepth;
        for (uint32_t i = 0; i < n && !c.fail; ++i) {
            if (isCurveNodes) {
                if (ex.curveColor) ex.elem->colorCurve.emplace_back();
                else               ex.curveTgt->emplace_back();
            }
            // Array items: classes ('c'), raw blocks ('~'), or s-structs.
            if (!c.need(1)) return;
            const uint8_t t2 = c.d[c.off];
            if (t2 == 0x73 && isCurveNodes) {
                // curve node struct: route Time/Value into the new key
                c.off++;
                const uint32_t np = c.u32();
                for (uint32_t k = 0; k < np && !c.fail; ++k) {
                    const std::string pn = c.str16();
                    walkValue(c, ex, "__curve__", pn);
                }
            } else {
                walkValue(c, ex, cls, prop);
            }
        }
        --ex.dumpDepth;
        dumpLine(ex, "]");
        break;
    }
    case 0x63:   // 'c' class
        walkClass(c, ex);
        break;
    case 0x6f: { // 'o' optional object
        const uint8_t present = c.u8();
        if (present) walkValue(c, ex, cls, prop);
        break;
    }
    case 0x74:   // 't' struct
    case 0x64:   // 'd' dict
    case 0x73: { // 's' anonymous struct
        const uint32_t n = c.u32();
        const bool isMatInfo = prop == "MaterialInfo" || prop == "Textures" ||
                               prop == "Parameters";
        if (isMatInfo) ++ex.depthInMatInfo;
        // A *Curve struct (inside a *ByLife class) owns the Nodes we capture.
        std::vector<FxCurveKey>* savedTgt = ex.curveTgt;
        const bool savedColor = ex.curveColor;
        bool touchedCurve = false;
        if (ex.elem) {
            if (prop == "ColorCurve" && cls == "ParticleElementColorByLife") {
                ex.curveColor = true; ex.curveTgt = nullptr; touchedCurve = true;
            } else if (prop == "AlphaCurve")     { ex.curveTgt = &ex.elem->alphaCurve;    ex.curveColor = false; touchedCurve = true; }
            else if (prop == "SizeCurve")        { ex.curveTgt = &ex.elem->sizeCurve;     ex.curveColor = false; touchedCurve = true; }
            else if (prop == "SizeCurveX")       { ex.curveTgt = &ex.elem->sizeCurveX;    ex.curveColor = false; touchedCurve = true; }
            else if (prop == "SizeCurveY")       { ex.curveTgt = &ex.elem->sizeCurveY;    ex.curveColor = false; touchedCurve = true; }
            else if (prop == "EmissionCurve")    { ex.curveTgt = &ex.elem->emissionCurve; ex.curveColor = false; touchedCurve = true; }
            else if (prop == "ControlCurve")     { ex.curveTgt = &ex.elem->controlCurve;  ex.curveColor = false; touchedCurve = true; }
        }
        if (ex.dump) {
            char b[64];
            std::snprintf(b, sizeof b, "%s = struct{%u} {", prop.c_str(), n);
            dumpLine(ex, b);
        }
        ++ex.dumpDepth;
        for (uint32_t i = 0; i < n && !c.fail; ++i) {
            const std::string pn = c.str16();
            walkValue(c, ex, cls, pn);
        }
        --ex.dumpDepth;
        dumpLine(ex, "}");
        if (isMatInfo) --ex.depthInMatInfo;
        if (touchedCurve) { ex.curveTgt = savedTgt; ex.curveColor = savedColor; }
        break;
    }
    default:
        // Unknown tag (3/500 samples hit 0x30 in skill FX) — structural loss;
        // stop parsing this system rather than misread the rest.
        c.fail = true;
        break;
    }
}

} // namespace

bool parseFxSystem(const uint8_t* data, size_t len, FxSystem* out, std::string* err)
{
    if (!data || len < 8) {
        if (err) *err = "fx: empty blob";
        return false;
    }
    Cursor c{data, len};
    const std::string rootName = c.str16();   // "ParticleSystem"
    if (rootName != "ParticleSystem") {
        if (err) *err = "fx: not a ParticleSystem root (" + rootName + ")";
        return false;
    }
    Extract ex;
    ex.sys = out;
    walkValue(c, ex, std::string(), std::string());
    if (c.fail && out->emitters.empty()) {
        if (err) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "fx: parse failed at offset %zu", c.off);
            *err = buf;
        }
        return false;
    }
    // Drop disabled or empty emitters/elements here so callers see draw-ready data.
    auto& ems = out->emitters;
    for (auto& em : ems) {
        auto& els = em.elements;
        for (size_t i = els.size(); i-- > 0;)
            if (!els[i].enable || els[i].kind == FxElement::None)
                els.erase(els.begin() + (long)i);
    }
    for (size_t i = ems.size(); i-- > 0;)
        if (!ems[i].enable || ems[i].elements.empty())
            ems.erase(ems.begin() + (long)i);
    return true;
}

bool dumpFxSystem(const uint8_t* data, size_t len, std::string* outText, std::string* err)
{
    if (!outText) return false;
    if (!data || len < 8) { if (err) *err = "fx: empty blob"; return false; }
    Cursor c{data, len};
    const std::string rootName = c.str16();
    if (rootName != "ParticleSystem") {
        if (err) *err = "fx: not a ParticleSystem root (" + rootName + ")";
        return false;
    }
    FxSystem scratch;
    Extract ex;
    ex.sys  = &scratch;
    ex.dump = outText;
    outText->append("ParticleSystem \"").append(scratch.name).append("\"\n");
    walkValue(c, ex, std::string(), std::string());
    if (c.fail) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "\n[parse stopped at offset %zu]\n", c.off);
        outText->append(buf);
    }
    return true;
}

} // namespace di
```

## `src/model/FxResolve.h`
```cpp
#pragma once
// Model -> cosmetic FX binding + loading. Binding is by NAME CONVENTION,
// measured on the live repository (FX_RESEARCH.md): a model's persistent FX
// are ParticleSystem entries named exactly like the model, or the model name
// plus a measured suffix family:
//   "", "_lod", "_color_<x>[_lod]", "_aw<N>...[_lod]", "_l"/"_r"[_lod],
//   "_<digits>[_lod]"
// Nothing in the repository points model->PS (verified: zero referrers), so
// the name rule IS the binding — same convention family the tool already uses
// for skeletons (findSkeletonByConvention) and clips (findFolderAnims).
//
// loadModelFx() resolves + parses + decodes everything render-ready on the
// caller's (worker) thread: FX mesh via the standard MESSIAH mesh parser,
// diffuse texture decoded to QImage.

#include <QImage>

#include <memory>
#include <string>
#include <vector>

#include "model/FxParser.h"
#include "model/MeshParser.h"
#include "store/AssetStore.h"

namespace di {

// One draw-ready FX piece (a persistent MaxParticles-style element).
struct FxDrawPart {
    FxElement::Kind kind = FxElement::None;
    std::shared_ptr<MeshData> mesh;      // Mesh kind (null for Sprite)
    QImage diffuse;                      // decoded; may be null (untextured glow)
    // element transform (local/model space)
    std::array<float, 3> scale{1, 1, 1};
    std::array<float, 3> rotation{0, 0, 0};
    std::array<float, 3> translation{0, 0, 0};
    std::array<float, 2> spriteScale{1, 1};
    std::array<float, 4> color{1, 1, 1, 1};   // ModulateColor x curve mid
    std::array<float, 4> uvScroll{1, 1, 0, 0};
    float hdrScale   = 1.0f;
    bool  doubleSided = true;
    bool  faceCamera  = false;
    bool  depthTest   = true;
    QString sourcePs;                    // PS entry name (UI/debug)
    QString emitterName;
};

// Per-emitter GPU-bound resources for the simulated path (index-aligned to
// FxLoadedSystem::system.emitters). Resolved on the worker thread.
struct FxEmitterRes {
    QImage diffuse;                      // decoded DiffuseTexture (may be null)
    QImage mask;                         // decoded MaskTexture (may be null)
    QImage twist;                        // decoded TwistTexture (may be null)
    std::shared_ptr<MeshData> mesh;      // mesh emitters only (null for sprites)
};

// A fully parsed + resource-resolved system, ready to hand to FxRuntime. Owned
// by the result so the runtime can borrow &system safely.
struct FxLoadedSystem {
    FxSystem system;
    std::vector<FxEmitterRes> emitters;
};

struct FxLoadResult {
    std::vector<FxDrawPart> parts;       // legacy static path (name-convention)
    std::shared_ptr<FxLoadedSystem> sim; // simulated path (confirmed weapon FX)
    int systemsFound  = 0;               // PS entries matched by name
    int systemsParsed = 0;
    QString note;                        // human summary for the INFO panel/log
    QString debugTreePath;               // where the confirmed-FX tree dump was written
};

// Find the ParticleSystem repository entries bound to this model by the
// measured name convention. Returns repo indices, exact-name match first.
std::vector<int32_t> findModelFx(const DiAssetStore& store, int32_t modelRepoIdx);

// Find ParticleSystems bound to this model by a DUMP-CONFIRMED binding that the
// name convention cannot reach — weapon/offhand FX are named
// fx_<setid>_<class>_<weapontype>, NOT after the model, so findModelFx misses
// them entirely. Each entry was verified resident in a real DiabloImmortal
// process dump (FX_RESEARCH.md, "Dump result"); a row fires ONLY when a real
// model whose name contains its token is loaded AND the exact PS entry exists,
// so nothing is fabricated. Returns exact-name PS repo indices.
std::vector<int32_t> findConfirmedModelFx(const DiAssetStore& store, int32_t modelRepoIdx);

// Resolve + parse + decode all persistent FX of a model. Heavy (blob reads,
// LZ4, texture decode) — call from a worker thread.
FxLoadResult loadModelFx(const DiAssetStore& store, int32_t modelRepoIdx);

} // namespace di
```

## `src/model/FxResolve.cpp`
```cpp
#include "model/FxResolve.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "store/Zzz4.h"
#include "tex/TextureDecode.h"
#include "tex/TextureParser.h"

namespace di {
namespace {

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Measured suffix rule (FX_RESEARCH.md): after "<model>_", the remainder must
// begin with one of the observed cosmetic-FX suffix families. Deliberately
// tight — a bare prefix match would steal FX from longer-named models.
bool fxSuffixOk(const std::string& rest)
{
    if (rest.empty()) return true;                       // exact name
    if (rest == "lod") return true;
    if (rest.rfind("color_", 0) == 0) return true;       // _color_a[_lod]
    if (rest.rfind("aw", 0) == 0 && rest.size() > 2 &&
        std::isdigit((unsigned char)rest[2]))
        return true;                                     // _aw2 / _aw3_01_lod
    if (rest == "l" || rest == "r" || rest == "l_lod" || rest == "r_lod")
        return true;
    if (std::isdigit((unsigned char)rest[0])) return true;   // _1 / _01[_lod]
    return false;
}

// Strip a 36-char GUID string "8c163c4b-9c9e-..." to 32-hex; empty if zero-GUID
// or malformed.
std::string guidToHex(const std::string& g)
{
    std::string hex;
    hex.reserve(32);
    for (char c : g) {
        if (c == '-') continue;
        if (!std::isxdigit((unsigned char)c)) return {};
        hex += (char)std::tolower((unsigned char)c);
    }
    if (hex.size() != 32) return {};
    if (hex.find_first_not_of('0') == std::string::npos) return {};   // zero GUID
    return hex;
}

QImage decodeTexByHash(const DiAssetStore& store, const std::string& hex)
{
    if (hex.empty()) return {};
    const size_t blob = store.blobForHash(hex);
    if (blob == (size_t)-1) return {};
    const std::vector<uint8_t> raw = store.mpk().readAsset(blob);
    if (raw.empty() || !isTexture2D(raw.data(), raw.size())) return {};
    Texture2D tex;
    std::string err;
    if (!parseTexture2D(raw.data(), raw.size(), &tex, &err)) return {};
    // Pick the LARGEST mip (mips may be listed smallest-first, so a first-match
    // scan grabs a 4x4 sliver); cap at 1024 for memory sanity.
    int pick = -1;
    long bestArea = -1;
    for (int i = 0; i < (int)tex.mips.size(); ++i) {
        const long w = tex.mips[i].width, h = tex.mips[i].height;
        if (w <= 1024 && h <= 1024 && w * h > bestArea) { bestArea = w * h; pick = i; }
    }
    if (pick < 0) pick = 0;
    return TextureDecode::decode(tex, pick).image;
}

// Dump-confirmed model -> ParticleSystem bindings (FX_RESEARCH.md, "Dump result
// - sz11_008 crusader set", measured on a 3.97 GB process dump). Weapon/offhand
// FX are named fx_<setid>_<class>_<weapontype>, so the name convention in
// findModelFx never reaches them. Each row is a REAL PS confirmed resident with
// the set visible; a row only fires when a real model whose name contains
// `modelToken` is loaded and the exact `ps` entry is present in the repository,
// so no unconfirmed effect is ever attached.
struct ConfirmedFx {
    const char* modelToken;   // substring of the loaded model's repo name
    const char* ps;           // exact ParticleSystem entry name to resolve
};
static const ConfirmedFx kConfirmedFx[] = {
    { "lianchui_sz11_008", "fx_sz11_008_crusader_lianchui"  },  // mace  (weapon)
    { "dun_sz11_008",      "fx_sz11_008_Doom_crusader_dun"  },  // shield (offhand)
    { "fazhang_sz11_008",  "fx_sz11_008_crusader_fazhang"   },  // staff (weapon)
    { "shoubi_sz11_008",   "fx_sz_11_008_m_crusader_shoubi" },  // forearm
};

} // namespace

std::vector<int32_t> findConfirmedModelFx(const DiAssetStore& store, int32_t modelRepoIdx)
{
    std::vector<int32_t> out;
    const Repository* repo = store.repo();
    if (!repo || modelRepoIdx < 0 || (size_t)modelRepoIdx >= repo->entries.size())
        return out;
    const std::string base = lower(repo->entries[(size_t)modelRepoIdx].name);
    if (base.empty()) return out;

    for (const ConfirmedFx& cf : kConfirmedFx) {
        if (base.find(cf.modelToken) == std::string::npos) continue;
        const std::string want = lower(cf.ps);
        for (size_t i = 0; i < repo->entries.size(); ++i) {
            const RepoEntry& e = repo->entries[i];
            if (repo->typeOf(e) != "ParticleSystem") continue;
            if (lower(e.name) == want) { out.push_back((int32_t)i); break; }
        }
    }
    return out;
}

std::vector<int32_t> findModelFx(const DiAssetStore& store, int32_t modelRepoIdx)
{
    std::vector<int32_t> out;
    const Repository* repo = store.repo();
    if (!repo || modelRepoIdx < 0 || (size_t)modelRepoIdx >= repo->entries.size())
        return out;
    const std::string base = lower(repo->entries[(size_t)modelRepoIdx].name);
    if (base.empty()) return out;

    const std::string& modelHash = repo->entries[(size_t)modelRepoIdx].hashHex;
    for (size_t i = 0; i < repo->entries.size(); ++i) {
        const RepoEntry& e = repo->entries[i];
        if (repo->typeOf(e) != "ParticleSystem") continue;
        const std::string n = lower(e.name);
        // Binding path 1 (name convention — cosmetic sets, aw tiers).
        if (n.rfind(base, 0) == 0) {
            if (n.size() == base.size()) { out.push_back((int32_t)i); continue; }
            if (n[base.size()] == '_' && fxSuffixOk(n.substr(base.size() + 1))) {
                out.push_back((int32_t)i);
                continue;
            }
        }
        // Binding path 2 (repository rels — measured: 4,800 PS entries list a
        // Model in their dependency hashes; that Model is the piece the FX
        // belongs to, e.g. chuansongmen_succubus -> f_demonhunter_sz08_001_fx_*).
        for (const std::string& h : e.related)
            if (h == modelHash) { out.push_back((int32_t)i); break; }
    }
    // Exact match first, then _lod-less before _lod, then name order — the
    // caller renders the FIRST variant group (base look), not every recolor.
    std::sort(out.begin(), out.end(), [&](int32_t a, int32_t b) {
        const std::string na = lower(repo->entries[(size_t)a].name);
        const std::string nb = lower(repo->entries[(size_t)b].name);
        const int ra = (na == base) ? 0 : 1, rb = (nb == base) ? 0 : 1;
        if (ra != rb) return ra < rb;
        return na < nb;
    });
    return out;
}

// Parse one ParticleSystem repo entry and append its persistent (Mesh/Sprite)
// draw parts to `res`, in the PS's own local/model space. Shared by the
// confirmed-binding path and the name-convention path. When `dumpTree` is set
// (confirmed weapons only), the system's full typed tree is written to
// fx_tree_dump.txt next to the executable so emitter fields the renderer does
// not yet extract can be measured directly — no guessing, no env var needed.
static void appendFxParts(const DiAssetStore& store, const Repository& repo,
                          int32_t idx, FxLoadResult& res, bool dumpTree)
{
    const RepoEntry& e = repo.entries[(size_t)idx];
    const size_t blob = store.blobForHash(e.hashHex);
    if (blob == (size_t)-1) return;
    const std::vector<uint8_t> raw = store.mpk().readAsset(blob);
    if (raw.empty()) return;

    FxSystem sys;
    std::string err;
    if (!parseFxSystem(raw.data(), raw.size(), &sys, &err)) return;
    ++res.systemsParsed;

    if (dumpTree) {
        std::string tree, derr;
        if (dumpFxSystem(raw.data(), raw.size(), &tree, &derr)) {
            const QString hdr =
                QStringLiteral("\n===== %1  (entry: %2, %3 bytes decoded) =====\n")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                         QString::fromStdString(e.name))
                    .arg((qulonglong)raw.size());
            // Try several writable spots so this never silently no-ops; the
            // first that opens wins and its path is reported on the FX line.
            const QStringList dirs = {
                QCoreApplication::applicationDirPath(),
                QDir::currentPath(),
                QDir::homePath(),
                QDir::tempPath(),
            };
            bool wrote = false;
            for (const QString& d : dirs) {
                QFile f(d + "/fx_tree_dump.txt");
                if (f.open(QIODevice::Append | QIODevice::Text)) {
                    f.write(hdr.toUtf8());
                    f.write(tree.data(), (qint64)tree.size());
                    f.close();
                    res.debugTreePath = f.fileName();
                    wrote = true;
                    break;
                }
            }
            if (!wrote)
                res.debugTreePath = QStringLiteral("(dump write failed — no writable dir)");
        }
    }

    for (const FxEmitter& em : sys.emitters) {
        for (const FxElement& el : em.elements) {
            if (el.kind != FxElement::Mesh && el.kind != FxElement::Sprite)
                continue;
            FxDrawPart p;
            p.kind        = el.kind;
            p.scale       = el.scale;
            p.rotation    = el.rotation;
            p.translation = el.translation;
            p.spriteScale = el.spriteScale;
            p.doubleSided = el.doubleSided;
            p.faceCamera  = el.faceCamera || el.kind == FxElement::Sprite;
            p.depthTest   = el.depthTest;
            p.uvScroll    = el.material.uvScroll;
            p.hdrScale    = el.material.hdrScale > 0 ? el.material.hdrScale : 1.0f;
            p.color       = el.material.modulateColor;
            // Fold the ColorByLife mid-life tint in (persistent FX loop it).
            if (!el.colorCurve.empty()) {
                const FxColorKey& k = el.colorCurve[el.colorCurve.size() / 2];
                for (int c = 0; c < 4; ++c) p.color[c] *= k.rgba[c];
            }
            // Sprite size baked from InitSize midpoint.
            if (el.kind == FxElement::Sprite) {
                const float s = (el.initSizeMin + el.initSizeMax) * 0.5f;
                if (s > 0) {
                    p.spriteScale[0] *= s;
                    p.spriteScale[1] *= s;
                }
            }
            p.sourcePs    = QString::fromStdString(e.name);
            p.emitterName = QString::fromStdString(em.name);

            if (el.kind == FxElement::Mesh) {
                const std::string hx = guidToHex(el.meshGuid);
                if (hx.empty()) continue;
                const size_t mb = store.blobForHash(hx);
                if (mb == (size_t)-1) continue;
                const std::vector<uint8_t> mraw = store.mpk().readAsset(mb);
                auto mesh = std::make_shared<MeshData>();
                std::string merr;
                if (mraw.empty() ||
                    !parseMesh(mraw.data(), mraw.size(), mesh.get(), &merr))
                    continue;
                p.mesh = mesh;
            }
            p.diffuse = decodeTexByHash(store, guidToHex(el.material.diffuseGuid));
            res.parts.push_back(std::move(p));
        }
    }
}

// Parse one confirmed ParticleSystem, dump its tree, resolve each emitter's
// diffuse texture + (mesh emitters) FX mesh, and append the emitters to the
// result's simulated system. Emitter i in system.emitters aligns with
// emitters[i] in the resource list.
static void appendFxSim(const DiAssetStore& store, const Repository& repo,
                        int32_t idx, FxLoadResult& res, int& emitterCount)
{
    const RepoEntry& e = repo.entries[(size_t)idx];
    const size_t blob = store.blobForHash(e.hashHex);
    if (blob == (size_t)-1) return;
    const std::vector<uint8_t> raw = store.mpk().readAsset(blob);
    if (raw.empty()) return;

    FxSystem sys;
    std::string err;
    if (!parseFxSystem(raw.data(), raw.size(), &sys, &err)) return;
    ++res.systemsParsed;

    // Tree dump (writes to the first writable dir; path shown on the FX line).
    {
        std::string tree, derr;
        if (dumpFxSystem(raw.data(), raw.size(), &tree, &derr)) {
            const QString hdr =
                QStringLiteral("\n===== %1  (entry: %2, %3 bytes decoded) =====\n")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                         QString::fromStdString(e.name))
                    .arg((qulonglong)raw.size());
            const QStringList dirs = { QCoreApplication::applicationDirPath(),
                                       QDir::currentPath(), QDir::homePath(),
                                       QDir::tempPath() };
            for (const QString& d : dirs) {
                QFile f(d + "/fx_tree_dump.txt");
                if (f.open(QIODevice::Append | QIODevice::Text)) {
                    f.write(hdr.toUtf8());
                    f.write(tree.data(), (qint64)tree.size());
                    f.close();
                    res.debugTreePath = f.fileName();
                    break;
                }
            }
        }
    }

    if (!res.sim) res.sim = std::make_shared<FxLoadedSystem>();
    if (res.sim->system.emitters.empty()) res.sim->system.bias = sys.bias;

    for (const FxEmitter& em : sys.emitters) {
        const FxElement* pe = nullptr;
        for (const FxElement& el : em.elements)
            if (el.kind == FxElement::Mesh || el.kind == FxElement::Sprite) { pe = &el; break; }
        if (!em.enable || !pe) continue;

        FxEmitterRes r;
        r.diffuse = decodeTexByHash(store, guidToHex(pe->material.diffuseGuid));
        r.mask    = decodeTexByHash(store, guidToHex(pe->material.maskGuid));
        r.twist   = decodeTexByHash(store, guidToHex(pe->material.twistGuid));

        // Export the FX textures next to the exe (fx_tex/) so an offline preview
        // renderer can reproduce the look. One-time; overwrites on reload.
        {
            const QString dir = QCoreApplication::applicationDirPath() + "/fx_tex";
            QDir().mkpath(dir);
            const QString base = QStringLiteral("%1/%2_%3")
                .arg(dir)
                .arg(emitterCount, 2, 10, QLatin1Char('0'))
                .arg(QString::fromStdString(em.name.empty() ? "emitter" : em.name));
            if (!r.diffuse.isNull()) r.diffuse.save(base + "_diffuse.png");
            if (!r.mask.isNull())    r.mask.save(base + "_mask.png");
            if (!r.twist.isNull())   r.twist.save(base + "_twist.png");
        }
        if (pe->kind == FxElement::Mesh) {
            const std::string hx = guidToHex(pe->meshGuid);
            if (!hx.empty()) {
                const size_t mb = store.blobForHash(hx);
                if (mb != (size_t)-1) {
                    const std::vector<uint8_t> mraw = store.mpk().readAsset(mb);
                    auto mesh = std::make_shared<MeshData>();
                    std::string merr;
                    if (!mraw.empty() &&
                        parseMesh(mraw.data(), mraw.size(), mesh.get(), &merr))
                        r.mesh = mesh;
                }
            }
        }
        res.sim->system.emitters.push_back(em);
        res.sim->emitters.push_back(std::move(r));
        ++emitterCount;
    }
}

FxLoadResult loadModelFx(const DiAssetStore& store, int32_t modelRepoIdx)
{
    FxLoadResult res;
    const Repository* repo = store.repo();
    if (!repo) return res;

    // Dump-confirmed weapon/offhand bindings take precedence: they are the exact
    // ground-truth PS for that model (verified resident in a process dump), named
    // fx_<setid>_<class>_<weapontype> where the name convention can't see them.
    // When present, render exactly those systems in the model's own local space
    // (a weapon's authored effect needs no external bone) — no variant guessing.
    const std::vector<int32_t> confirmed = findConfirmedModelFx(store, modelRepoIdx);
    const bool usedConfirmed = !confirmed.empty();

    std::vector<int32_t> psIdx = usedConfirmed ? confirmed
                                               : findModelFx(store, modelRepoIdx);
    res.systemsFound = (int)psIdx.size();
    if (psIdx.empty()) return res;

    if (usedConfirmed) {
        // Simulated path: parse each confirmed system, resolve per-emitter
        // textures/meshes, and hand it to the runtime. All fields are measured.
        res.sim = std::make_shared<FxLoadedSystem>();
        int emitterCount = 0;
        for (int32_t idx : psIdx)
            appendFxSim(store, *repo, idx, res, emitterCount);
        res.note = QStringLiteral("%1 confirmed FX system(s), %2 parsed, %3 emitter(s) simulated")
                       .arg(res.systemsFound)
                       .arg(res.systemsParsed)
                       .arg(emitterCount);
        if (!res.debugTreePath.isEmpty())
            res.note += QStringLiteral(" — tree dump: %1").arg(res.debugTreePath);
        if (emitterCount == 0) res.sim.reset();
        return res;
    }

    // Variant-group selection. Groups measured on the live repository: base
    // (exact / _<digits> multi-part), awakened tiers (_aw3 > _aw2 — where the
    // heavy "armor glow" actually lives; many pieces have NO base-look FX at
    // all), then recolors (_color_a first). Try groups in that order and keep
    // the FIRST one that yields systems, so a piece whose only FX is its
    // awakened aura still lights up instead of rendering nothing.
    const std::string base = lower(repo->entries[(size_t)modelRepoIdx].name);
    auto groupOf = [&](int32_t i) -> int {
        const std::string n = lower(repo->entries[(size_t)i].name);
        if (n.size() >= 4 && n.compare(n.size() - 4, 4, "_lod") == 0)
            return -1;                                     // lod dupes: never
        if (n.rfind(base + "_aw3", 0) == 0) return 1;
        if (n.rfind(base + "_aw", 0) == 0) return 2;       // aw2 and others
        if (n.rfind(base + "_color_a", 0) == 0) return 3;
        if (n.rfind(base + "_color_", 0) == 0) return 4;
        return 0;                                          // base look
    };
    std::vector<int32_t> chosen;
    for (int want : {0, 1, 2, 3, 4}) {
        for (int32_t i : psIdx)
            if (groupOf(i) == want) chosen.push_back(i);
        if (!chosen.empty()) break;
    }
    if (chosen.empty())                                    // only _lod entries
        chosen.assign(psIdx.begin(), psIdx.begin() + 1);

    for (int32_t idx : chosen)
        appendFxParts(store, *repo, idx, res, /*dumpTree=*/false);

    res.note = QStringLiteral("%1 FX system(s), %2 parsed, %3 draw part(s)")
                   .arg(res.systemsFound)
                   .arg(res.systemsParsed)
                   .arg(res.parts.size());
    return res;
}

} // namespace di
```

## `src/model/FxRuntime.h`
```cpp
#pragma once
// CPU particle simulator for DI ParticleSystems (FxParser output). Turns the
// measured emitter data (emission, lifetime, init distributions, over-life
// curves, sub-UV, force) into a per-frame list of draw instances. Pure logic:
// no Qt, no GL — so it is unit-testable and the renderer just uploads the
// instance list each frame.
//
// Coordinates are FX-local (System.Bias + element translation already folded
// in). The caller applies the model/attach transform. Nothing here is guessed:
// every field comes from FxParser; unsampled fields fall back to identity.

#include <array>
#include <cstdint>
#include <vector>

#include "model/FxParser.h"

namespace di {

// One particle to draw this frame.
struct FxInstance {
    int   emitter = 0;                 // index into FxSystem::emitters (GPU res lookup)
    bool  mesh = false;                // true: draw emitter mesh; false: billboard
    bool  faceCamera = true;           // billboard faces camera
    std::array<float, 3> pos{0, 0, 0}; // FX-local center
    std::array<float, 3> size{1, 1, 1};// world size (billboard uses xy)
    float roll = 0.0f;                 // z-roll radians (billboard)
    std::array<float, 4> color{1, 1, 1, 1};   // rgba (hdr applied in shader)
    float hdrScale = 1.0f;
    int   atlasU = 1, atlasV = 1, frame = 0;   // sub-UV flipbook cell
    std::array<float, 4> uvScroll{1, 1, 0, 0}; // material DiffuseUVScolling
    float depthOffset = 0.0f;          // render sort bias
};

class FxRuntime {
public:
    // Borrow a parsed system (must outlive the runtime). Resets state.
    void setSystem(const FxSystem* sys);
    void clear();

    // Advance the simulation by dt seconds and rebuild the instance list.
    void advance(float dt);

    const std::vector<FxInstance>& instances() const { return out_; }
    bool active() const { return sys_ && !sys_->emitters.empty(); }

    // Live debug controls (FX panel).
    void setOffsetY(float v) { offsetY_ = v; }
    void setScale(float v)   { scale_ = v > 0.001f ? v : 0.001f; }
    void setEmitterEnabled(int i, bool on) {
        if (i >= 0 && i < (int)enabled_.size()) enabled_[(size_t)i] = on ? 1 : 0;
    }
    float offsetY() const { return offsetY_; }
    float scale()   const { return scale_; }

private:
    struct Particle {
        bool  alive = false;
        float age = 0, life = 1;
        std::array<float, 3> pos{0, 0, 0};
        std::array<float, 3> vel{0, 0, 0};
        float roll = 0, rollRate = 0;
        float baseSize = 1;
        int   frameOffset = 0;
    };
    struct EmState {
        std::vector<Particle> pool;
        float spawnAccum = 0;
        float time = 0;
        bool  bursted = false;
        uint32_t rng = 0x9e3779b9u;
    };

    const FxSystem* sys_ = nullptr;
    std::vector<EmState> em_;
    std::vector<uint8_t> enabled_;      // per-emitter on/off (FX panel)
    std::vector<FxInstance> out_;
    // Placement tuning (read from env in setSystem — lets the viewer dial the
    // standalone anchor without a rebuild):
    //   DI_FX_BIAS=1     apply the authored System.Bias (default OFF — the
    //                    renderer anchors the FX at the model centre instead)
    //   DI_FX_OFFSET_Y=v add v to every particle Y (default 0)
    //   DI_FX_SCALE=v    multiply every particle size by v (default 1)
    bool  applyBias_ = false;
    float offsetY_   = 0.0f;
    float scale_     = 1.0f;
};

} // namespace di
```

## `src/model/FxRuntime.cpp`
```cpp
#include "model/FxRuntime.h"

#include <cmath>
#include <cstdlib>

namespace di {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// xorshift32 — cheap, deterministic per emitter.
inline uint32_t xr(uint32_t& s)
{
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : (s = 0x1234567u);
}
inline float rnd01(uint32_t& s) { return (xr(s) & 0xffffff) / float(0x1000000); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// The one element an emitter draws (first Mesh/Sprite element).
const FxElement* drawElem(const FxEmitter& em)
{
    for (const FxElement& e : em.elements)
        if (e.kind == FxElement::Mesh || e.kind == FxElement::Sprite)
            return &e;
    return nullptr;
}

float sampleFloat(const FxDist& d, uint32_t& s, float fallback)
{
    if (!d.set) return fallback;
    if (d.kind == FxDist::ConstFloat || d.kind == FxDist::UniformFloat)
        return lerp(d.fMin, d.fMax, rnd01(s));
    return fallback;
}

std::array<float, 3> sampleVec(const FxDist& d, uint32_t& s)
{
    if (!d.set) return {0, 0, 0};
    switch (d.kind) {
    case FxDist::ConstVec3:
    case FxDist::UniformVec3:
        return {lerp(d.vMin[0], d.vMax[0], rnd01(s)),
                lerp(d.vMin[1], d.vMax[1], rnd01(s)),
                lerp(d.vMin[2], d.vMax[2], rnd01(s))};
    case FxDist::Sphere: {
        // uniform direction on the unit sphere, scaled by [rMin,rMax], + center
        const float z = 2.0f * rnd01(s) - 1.0f;
        const float phi = 2.0f * kPi * rnd01(s);
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float mag = lerp(d.rMin, d.rMax, rnd01(s));
        return {d.center[0] + r * std::cos(phi) * mag,
                d.center[1] + r * std::sin(phi) * mag,
                d.center[2] + z * mag};
    }
    case FxDist::Cylinder: {
        // ring in the plane, offset along the axis vector
        const float ang = 2.0f * kPi * rnd01(s);
        const float rr = lerp(d.rMin, d.rMax, rnd01(s));
        const float h = rnd01(s);
        return {d.center[0] + std::cos(ang) * rr + d.axis[0] * h,
                d.center[1]                     + d.axis[1] * h,
                d.center[2] + std::sin(ang) * rr + d.axis[2] * h};
    }
    default:
        return {0, 0, 0};
    }
}

// Piecewise-linear evaluation of a scalar over-life curve at t in [0,1].
float evalCurve(const std::vector<FxCurveKey>& c, float t, float fallback)
{
    if (c.empty()) return fallback;
    if (c.size() == 1) return c[0].value;
    if (t <= c.front().time) return c.front().value;
    if (t >= c.back().time)  return c.back().value;
    for (size_t i = 1; i < c.size(); ++i) {
        if (t <= c[i].time) {
            const float span = c[i].time - c[i - 1].time;
            const float f = span > 1e-6f ? (t - c[i - 1].time) / span : 0.0f;
            return lerp(c[i - 1].value, c[i].value, f);
        }
    }
    return c.back().value;
}

std::array<float, 4> evalColor(const std::vector<FxColorKey>& c, float t)
{
    if (c.empty()) return {1, 1, 1, 1};
    if (c.size() == 1) return c[0].rgba;
    if (t <= c.front().time) return c.front().rgba;
    if (t >= c.back().time)  return c.back().rgba;
    for (size_t i = 1; i < c.size(); ++i) {
        if (t <= c[i].time) {
            const float span = c[i].time - c[i - 1].time;
            const float f = span > 1e-6f ? (t - c[i - 1].time) / span : 0.0f;
            std::array<float, 4> o;
            for (int k = 0; k < 4; ++k) o[k] = lerp(c[i - 1].rgba[k], c[i].rgba[k], f);
            return o;
        }
    }
    return c.back().rgba;
}

} // namespace

void FxRuntime::setSystem(const FxSystem* sys)
{
    sys_ = sys;
    clear();
    // Placement tuning from env (see header).
    applyBias_ = std::getenv("DI_FX_BIAS") != nullptr;
    if (const char* o = std::getenv("DI_FX_OFFSET_Y")) offsetY_ = (float)std::atof(o);
    else offsetY_ = 0.0f;
    if (const char* s = std::getenv("DI_FX_SCALE")) {
        const float v = (float)std::atof(s);
        scale_ = v > 0.0f ? v : 1.0f;
    } else scale_ = 1.0f;
    if (!sys_) return;
    em_.resize(sys_->emitters.size());
    enabled_.assign(sys_->emitters.size(), 1);
    for (size_t i = 0; i < em_.size(); ++i)
        em_[i].rng = 0x9e3779b9u ^ (uint32_t)(i * 2654435761u + 1u);
}

void FxRuntime::clear()
{
    for (EmState& e : em_) {
        e.pool.clear();
        e.spawnAccum = 0;
        e.time = 0;
        e.bursted = false;
    }
    out_.clear();
}

void FxRuntime::advance(float dt)
{
    out_.clear();
    if (!sys_) return;
    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;                 // clamp long stalls

    for (size_t ei = 0; ei < sys_->emitters.size(); ++ei) {
        const FxEmitter& em = sys_->emitters[ei];
        const FxElement* pe = drawElem(em);
        if (!em.enable || !pe) continue;
        if (ei < enabled_.size() && !enabled_[ei]) continue;   // FX-panel toggle
        const FxElement& e = *pe;
        EmState& st = em_[ei];
        st.time += dt;

        // Loop the emitter so finite-duration / one-shot-burst effects keep
        // burning (worn-cosmetic FX play continuously in game). Period = the
        // longest of its emission window, active/sleep cycle, or particle life.
        float loopPeriod = 0.0f;
        if (e.emissionDuration > 0.0f) loopPeriod = std::max(loopPeriod, e.emissionDuration);
        if (e.activeDuration > 0.0f && e.sleepDuration > 0.0f)
            loopPeriod = std::max(loopPeriod, e.activeDuration + e.sleepDuration);
        const float maxLife = e.life.set ? std::max(e.life.fMin, e.life.fMax) : 1.0f;
        loopPeriod = std::max(loopPeriod, maxLife);
        if (loopPeriod > 0.0f && st.time >= loopPeriod) {
            st.time -= loopPeriod;
            st.bursted = false;                 // re-fire the burst each loop
        }

        // --- emission gating -------------------------------------------------
        bool emitting = e.emissionRate > 0.0f || e.emissionBurst > 0.0f;
        if (e.emissionDuration >= 0.0f && st.time > e.emissionDuration)
            emitting = false;
        if (e.activeDuration > 0.0f && e.sleepDuration > 0.0f) {
            const float period = e.activeDuration + e.sleepDuration;
            const float phase = std::fmod(st.time, period);
            if (phase >= e.activeDuration) emitting = false;
        }
        const int cap = e.maxParticles > 0 ? e.maxParticles : 1;

        auto aliveCount = [&]() {
            int n = 0; for (const Particle& p : st.pool) if (p.alive) ++n; return n;
        };
        auto spawn = [&]() {
            Particle np;
            np.alive = true;
            np.age = 0;
            np.life = std::max(0.05f, sampleFloat(e.life, st.rng, 1.0f));
            np.pos = sampleVec(e.initPosition, st.rng);
            for (int k = 0; k < 3; ++k) {
                np.pos[k] += e.translation[k];
                if (applyBias_) np.pos[k] += sys_->bias[k];
            }
            np.vel = sampleVec(e.initVelocity, st.rng);
            np.roll = sampleFloat(e.initRotation, st.rng, 0.0f);
            np.rollRate = sampleFloat(e.initRotationRate, st.rng, 0.0f);
            np.baseSize = sampleFloat(e.initSize, st.rng, 1.0f);
            if (e.tileStartMax > e.tileStartMin)
                np.frameOffset = e.tileStartMin +
                    (int)(rnd01(st.rng) * (e.tileStartMax - e.tileStartMin));
            else
                np.frameOffset = e.tileStartMin;
            // reuse a dead slot if possible
            for (Particle& s : st.pool)
                if (!s.alive) { s = np; return; }
            if ((int)st.pool.size() < cap) st.pool.push_back(np);
        };

        // burst at first active frame
        if (!st.bursted && e.emissionBurst > 0.0f) {
            st.bursted = true;
            const int n = (int)e.emissionBurst;
            for (int k = 0; k < n && aliveCount() < cap; ++k) spawn();
        }
        // steady emission
        if (emitting && e.emissionRate > 0.0f) {
            st.spawnAccum += e.emissionRate * dt;
            while (st.spawnAccum >= 1.0f) {
                st.spawnAccum -= 1.0f;
                if (aliveCount() < cap) spawn();
            }
        }
        // keep MaxParticles==1 persistent elements always lit (respawn on death)
        if (cap == 1 && aliveCount() == 0 &&
            (e.emissionRate > 0.0f || e.emissionBurst > 0.0f) &&
            (e.emissionDuration < 0.0f || st.time <= e.emissionDuration))
            spawn();

        // --- advance + emit instances ---------------------------------------
        for (Particle& p : st.pool) {
            if (!p.alive) continue;
            p.age += dt;
            if (p.age >= p.life) { p.alive = false; continue; }
            if (e.hasForce)
                for (int k = 0; k < 3; ++k) p.vel[k] += e.force[k] * dt;
            for (int k = 0; k < 3; ++k) p.pos[k] += p.vel[k] * dt;
            p.roll += p.rollRate * dt * 30.0f;      // rate measured per ~1/30s

            const float t01 = p.age / p.life;

            FxInstance in;
            in.emitter    = (int)ei;
            in.mesh       = e.kind == FxElement::Mesh;
            in.faceCamera = in.mesh ? e.faceCamera : true;
            in.pos        = p.pos;
            in.pos[1]    += offsetY_;
            in.roll       = p.roll;
            in.hdrScale   = e.material.hdrScale > 0 ? e.material.hdrScale : 1.0f;
            in.uvScroll   = e.material.uvScroll;   // authored tile + scroll rate
            in.depthOffset = e.depthOffset;

            // colour = ModulateColor x ColorByLife, alpha also x AlphaByLife
            std::array<float, 4> col = e.material.modulateColor;
            if (!e.colorCurve.empty()) {
                const std::array<float, 4> c = evalColor(e.colorCurve, t01);
                for (int k = 0; k < 4; ++k) col[k] *= c[k];
            }
            col[3] *= evalCurve(e.alphaCurve, t01, 1.0f);
            if (col[3] < 0.0f) col[3] = 0.0f;   // authored negative alpha = off
            in.color = col;

            // size = |elementScale| x spriteScale x baseSize x size-curve
            const float sMul  = evalCurve(e.sizeCurve, t01, 1.0f);
            const float sMulX = e.sizeCurveX.empty() ? sMul : evalCurve(e.sizeCurveX, t01, 1.0f);
            const float sMulY = e.sizeCurveY.empty() ? sMul : evalCurve(e.sizeCurveY, t01, 1.0f);
            const float spx = in.mesh ? 1.0f : e.spriteScale[0];
            const float spy = in.mesh ? 1.0f : e.spriteScale[1];
            in.size = {std::fabs(e.scale[0]) * std::fabs(spx) * p.baseSize * sMulX * scale_,
                       std::fabs(e.scale[1]) * std::fabs(spy) * p.baseSize * sMulY * scale_,
                       std::fabs(e.scale[2]) * p.baseSize * sMul * scale_};

            // sub-UV flipbook cell. ONLY divide the texture into cells when the
            // emitter actually flipbooks (SubUVByLife); otherwise RepeatU/V are
            // not an atlas and the sprite must show the FULL texture (else it
            // renders a heavily-cropped 1/Nth sliver).
            if (e.subUv) {
                in.atlasU = std::max(1, e.atlasU);
                in.atlasV = std::max(1, e.atlasV);
                const int cells = in.atlasU * in.atlasV;
                int f = p.frameOffset;
                if (e.tilesPerSec > 0) f += (int)(p.age * e.tilesPerSec);
                if (cells > 1) f = e.subUvLoop ? (f % cells) : std::min(f, cells - 1);
                else f = 0;
                in.frame = f;
            } else {
                in.atlasU = 1;
                in.atlasV = 1;
                in.frame = 0;
            }
            out_.push_back(in);
        }
    }
}

} // namespace di
```

---

# Key data facts (quick reference)

**Confirmed weapon→FX bindings (from process dump, sz11_008 crusader set):**
```
lianchui_sz11_008  -> fx_sz11_008_crusader_lianchui   (mace)
dun_sz11_008       -> fx_sz11_008_Doom_crusader_dun    (shield)
fazhang_sz11_008   -> fx_sz11_008_crusader_fazhang     (staff)
shoubi_sz11_008    -> fx_sz_11_008_m_crusader_shoubi   (forearm)
```
Worn armor pieces (toukui/yifu/jianjia/tui) have NO particle system — their
in-game richness is material-driven emissive, not particles.

**Sprite Patch** = TypeDataSprite.Patch: array of (posX, posY, U, V) control
points = the sprite's real quad/polygon geometry AND texture mapping. Use it
directly (triangle fan) — it is the data-driven answer to sprite size, shape,
pivot, and UV crop.

**System.Property.Bias** = whole-FX offset from the attach frame (NOT the model
origin) — do not apply it in standalone model-space; anchor at the mesh
centroid instead.
