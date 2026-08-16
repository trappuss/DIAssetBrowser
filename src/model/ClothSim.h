#pragma once
// Soft-body (cloth / tail / hair) physics for CHAR::ANIM playback and export.
//
// THE PROBLEM (measured on real DI rigs): a character's cape, tail, skirt or
// hair strands are skinned to `bone_software_*` bones that the engine drives
// with a RUNTIME cloth solver — the .anim clip carries no motion for them (v3
// clips animate rotation only and leave these bones un-keyed; v2 clips hold
// them at bind). So in a plain player they sit at bind pose while the body
// moves underneath: the piece floats in place and the mesh between a moving
// body bone and a static cloth bone stretches. This module reproduces the
// engine's secondary motion with a Verlet / position-based-dynamics pass.
//
// APPROACH — bake, don't stream. The simulation is a deterministic pre-pass
// over the whole clip that returns an AUGMENTED AnimClip: a copy of the input
// with one extra (or replaced) track per cloth bone, carrying a per-frame
// translation+rotation key baked from the solve. Nothing downstream changes —
// PosePlayer plays the augmented clip and the GLB exporter writes its cloth
// tracks as ordinary glTF channels, so the viewport and the export are
// bit-identical by construction (toggle off == the original clip).
//
// Conventions match the rest of the pipeline: ROW-vector 4x3 affines, Y-up
// world (the ground grid sits on the XZ plane at min-Y, so gravity is -Y),
// world = local @ parentWorld, and a track rotation key is a COLUMN-convention
// quaternion (AnimPose transposes it; the glTF writer takes it directly).

#include <memory>
#include <string>
#include <vector>

#include "model/AnimParser.h"
#include "model/SkeletonTreeParser.h"
#include "model/SkinSkeletonParser.h"

namespace di {

// Tunables. Defaults favour a stable "follows the body with gentle secondary
// sway" look over dramatic swishing — the sim is anchored strongly to the
// kinematic (rigidly-attached) pose so it can never diverge or explode, then
// gravity + inertia add lag on top.
struct ClothParams {
    float gravity      = 2.5f;   // world units / s^2 along -Y
    float stiffness    = 0.45f;  // [0..1] per-frame spring pull toward the rigid pose
    float damping      = 0.90f;  // [0..1] velocity retained frame to frame
    int   iterations   = 8;      // distance-constraint relaxation passes
    float maxStretch   = 1.03f;  // hard cap on bone-length change (both ways)
    int   settle       = 12;     // pre-roll solves on frame 0 so it starts relaxed
    float inertia      = 1.0f;   // [0..1] how much body acceleration whips the tips
    // D4 flBoneTrackingFactor: the FINAL blend of the emitted pose toward the
    // skinned (rigid) pose. 1 = hug the pose exactly (no visible sway — same as
    // physics off), 0 = full free simulation. The engine keeps this high
    // (~0.45) because "cloth mostly tracks the bone-skinned pose; physics is a
    // light correction." This is the knob that decides how much the sim shows.
    float boneTracking = 0.45f;  // [0..1] blend emitted pose toward skinned pose
    // A6 body collision (OPT-IN). Spheres are placed on core body bones with a
    // radius = bodyRadius × that bone's rest length (DATA-RELATIVE — scales with
    // the rig, not a hard-coded world guess). Cloth particles are pushed out of
    // any sphere they penetrate during the solve. 0 disables it entirely (the
    // safe default): DI ships no authored collision volumes, so the exact radius
    // is a visual judgement the user tunes against the render.
    float bodyRadius   = 0.0f;   // 0 = off; try ~0.5 and tune to the body
};

// True when the skin skeleton contains any bone this module would simulate.
bool hasClothBones(const SkinSkeleton& skel);

// Bake cloth physics into a new clip. Returns a copy of `base` with cloth
// tracks added/replaced. On any structural problem (no cloth bones, missing
// hierarchy) it returns a shared copy of `base` unchanged — never null.
// `skel` supplies which bones are cloth (they must be skinned to matter);
// `parents`/`locals` supply the chain topology and rest transforms.
std::shared_ptr<AnimClip> bakeCloth(const AnimClip& base,
                                    const SkinSkeleton& skel,
                                    const BoneParents& parents,
                                    const BoneLocals& locals,
                                    const ClothParams& params = {});

} // namespace di
