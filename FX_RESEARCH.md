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
