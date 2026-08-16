# Diablo Immortal FX (ParticleSystem) — what we learned

This is a reader-facing summary of how Diablo Immortal's cosmetic visual effects
are built, for anyone studying the game's data. The FX *feature* was removed
from the asset browser (a faithful real-time reproduction wasn't worth shipping),
but the findings below are solid and reusable. Full technical archive +
source: `FX_ARCHIVE.md`.

## Where FX live

- The repository type `ParticleSystem` (folder `EffectCom`) holds ~43,900
  effects. Each is a `ZZZ4`-compressed **typed property tree** (same container
  as meshes/materials).
- A `ParticleSystem` is `Emitters[] → Elements[]`. Each emitter draws one
  **TypeData** (Mesh card, Sprite billboard, Light, Model…) plus modules that
  configure it: a **Source** (emission rate/duration/burst), **Init** modules
  (lifetime, size, velocity, position, rotation as distributions), **over-life
  curves** (color, alpha, size), a **SubUV** flipbook, and forces.

## How an effect knows where to attach

- Skills, cutscenes, portals, and NPC auras bind effects through readable
  `.graph` files: `EffectCom/<ParticleSystem>:<AttachBone>:<StartTime>:<flags>`
  (48,163 such records across ~10,000 graphs).
- **Worn cosmetics are different.** Which effect a piece of gear uses is applied
  from the encrypted client item table at equip time — it is *not* in any
  readable asset. It can only be recovered by scanning a live process memory
  dump for the effect names resident when the cosmetic is equipped.
- Practical upshot: for the sz11_008 crusader set, the weapon/offhand pieces
  (mace `lianchui`, shield `dun`, staff `fazhang`, forearm `shoubi`) each have a
  particle effect; the armor pieces (helmet/body/shoulders/legs) have **none** —
  their in-game richness comes from the animated *material* (emissive), not
  particles.

## The two things worth remembering

1. **Sprite `Patch`** — every sprite carries a `Patch`: an array of control
   points, each `(posX, posY, U, V)`. That IS the sprite's real geometry *and*
   its texture mapping. If you render a sprite, build its quad from the Patch —
   it answers size, shape, pivot, and exactly which region of the texture to
   show (no guessing, no per-effect tuning).

2. **`System.Property.Bias`** is an offset from the *attach frame* (the hand
   hardpoint the item clips into), not from the model's own origin. Applied in
   standalone model space it overshoots; anchor effects to the model geometry
   instead.

## Container / format quick reference

```
blob := "ZZZ4" + u32 uncompressedSize + raw-LZ4 block
tree tags: p=primitive(len-typed), $=string, v=array, c=class,
           t/d/s=struct, o=optional
```
Primitive length rules: 1=bool/u8, 2=u16 or counted-string, 4=u32/float,
8/12/16=floatN, 16 also = GUID. Parser verified byte-exact on 497/500 blobs.

## Tools left behind (in `tools/`, and `FX_ARCHIVE.md`)

- `di_fx_dumpscan.py` — scans a DiabloImmortal process dump for the FX-attach
  records / resident effect names (with a drag-drop `.bat`).
- `fxpreview/` — a standalone Python renderer that plays an effect from a tree
  dump + exported textures, for studying/tuning without the C++ tool.
