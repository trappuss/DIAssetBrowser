# Migrating D4AssetBrowser features into the DI browser (physics-first)

Prepared 2026-08-12 after a full read of the D4 cloth solver (`gl/GLModelWidget.cpp`
~2200–3000, `ClothParams` in `.h`), payload (`model/ModelGeometry.h` `ClothSim`),
tuning resolution, and the three D4 skills (codebase / gamedata / debugging).

## The key data-model difference (decides what is portable)

D4 ships an authored **NvCloth cage** per garment: `bindVerts`, `invMasses`,
`constraintIdx/Len`, `ptFollowerIndices` (particle→bone), `ptDriverInfluences`
(bone→particle), `kinRoots`/`attachLen` tethers, plane + capsule colliders, and a
`.clt.json` tuning block (gravity, `flBoneTrackingFactor`, per-class stiffness,
drag, wind). Its solver simulates the low-poly cage and skins the render mesh to it.

**DI has none of this.** DI's `MeshData` carries no cage; DI cloth is entirely
**skeletal** — the mesh is skinned to `bone_software_*` bones that the engine drives
at runtime. So the D4 *cage machinery* cannot be ported (no data to feed it). What
IS portable is D4's governing **philosophy**, which its own header states outright:

> "The cloth mostly tracks the authored bone-skinned pose; physics is a light
> correction."

That is exactly what the user asked for: *if a cape is supposed to have physics but
doesn't, it should just move with the spine* — i.e. fall back to the bone-skinned
pose, never float.

## Root cause of the DI float (found)

In DI's `PosePlayer::evaluate`, a skin bone with no animation track (every
`bone_software_*` bone in a v3 clip) gets an **identity skin matrix** → its vertices
stay at bind-world while the body moves → the piece floats and stretches. D4 instead
places such a bone at its **skinned pose**: the rest-pose chain composed onto its
nearest *driven* ancestor, so it rides the spine. The glTF export already does this
(nodes are built from the full `BoneParents` tree), so the fix is viewport-side.

---

## Task list

### A. Physics (the user's request) — implement first

- **A1. Associated-bone skinned-pose fallback in `PosePlayer`.** Undriven skin bones
  follow their nearest driven ancestor through the rest-pose chain instead of
  snapping to identity/bind-world. This alone makes a physics-less cape move with the
  spine (viewport parity with export). The single highest-value change. *(started)*

- **A2. Cloth sim as a correction on the skinned pose.** Refactor `ClothSim` to blend
  each simulated bone between its skinned-pose target and the free-Verlet result by a
  `boneTracking` factor (D4 `flBoneTrackingFactor`): high = hugs the pose, low = swings
  freely. Today the sim uses a fixed stiffness; this makes "mostly tracks pose, physics
  is a light correction" the explicit model and prevents over-swing.

- **A3. Cloth parameter panel + persistence.** Replace the single checkbox with a small
  panel mirroring D4's (trimmed to DI's skeletal model): gravity, bone-tracking,
  stiffness, damping, iterations, max-distance (leash), react-to-rotation force. One
  QSettings key per param; export reads the same keys so WYSIWYG holds.

- **A4. React-to-rotation inertia (`userSpin`).** ~~Port D4's `spinAccel`.~~
  **DROPPED — architecturally incompatible.** D4 simulates cloth LIVE every frame, so
  it can inject camera-spin forces in real time. DI's solver is a deterministic
  per-clip BAKE (the design that guarantees viewport == export byte-for-byte). A live
  camera force can't be baked without either (a) re-baking on every mouse move, or
  (b) abandoning export parity — both worse than the feature is worth. Kept in the
  record for transparency; not implemented.

- **A5. Chain-vs-hair classification.** Free-swinging chains (tails, flail links,
  long hair) get pendulum treatment (hold rest length, low tracking); short trim / fur
  gets tight pose-hugging. Classify by chain depth + name, set per-bone defaults.

- **A6. Simple body collision.** *SHIPPED — opt-in + tunable.* Spheres are placed on
  core body bones (spine/pelvis/hip/chest/neck/head/thigh/… by name) with radius =
  `bodyRadius × that bone's rest length` — DATA-RELATIVE, scaling with the rig instead
  of a hard-coded world guess. Cloth particles are pushed out of any sphere during the
  solve AND on the emitted output (the skinned-pose target can itself sit inside the
  body). OFF by default (`bodyRadius = 0`), exposed as "Body collision" in the Cloth…
  dialog so the user does the visual tuning the sandbox can't. Verified: a chain that
  penetrates a reference sphere by 0.30 units is pushed to exactly the surface (0.00).
  The principled concern (D4 needed footage for its 0.52) is respected by making it
  opt-in and rig-relative rather than a fixed default.

- **A7. Env-gated cloth diagnostics (`DI_DUMP_CLOTH`).** Per-bone anchor resolution,
  chain assignment, and worst drift vs the skinned pose — the project's instrument-first
  discipline, so future tuning cites numbers.

### B. General features (contextually appropriate, after physics)

- **B1. Shared viewport part context-menu vocabulary** (D4 `ViewportPartMenu`) for
  consistent right-click actions across the parts/attachments lists.
- **B2. Export-scope material renumbering** — verify DI's exporter renumbers materials
  for subset exports (D4's lesson: subsets fall off the material list and export
  untextured otherwise).
- **B3. Hardpoint export as glTF empties** — DI has `HP_*` bones; emit them as named
  empties so weapon/prop sockets survive into other tools.
- **B4. Per-model cloth-tuning memory** — remember cloth params per rig, like D4's
  per-piece tuning.

Physics items A1–A5 + A7 are the core of this request and will be implemented and
numerically verified; A6 and the B items follow.
