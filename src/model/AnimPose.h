#pragma once
// Pose evaluation: AnimClip sampled at a time -> GPU skinning matrices +
// skeleton-overlay geometry. All math is the ROW-vector convention the data
// itself uses (verified: invBind @ bindWorld == I on all 57 golden skin
// bones): world = local @ parentWorld, skin = invBind @ world. The output
// matrices are laid out so glUniformMatrix4fv(transpose=GL_FALSE) yields the
// standard column-vector matrix (skin^T) the shader multiplies with.

#include <cstdint>
#include <string>
#include <vector>

#include "model/AnimParser.h"
#include "model/SkinSkeletonParser.h"
#include "model/SkeletonTreeParser.h"

namespace di {

class PosePlayer {
public:
    // Both pointers must outlive the player. Bone->track matching is by name;
    // skin bones absent from the clip stay at bind pose (identity skin matrix,
    // or the fallback below when one is set).
    //
    // `locals` (optional) is the authoritative .skeleton rest pose (parent-
    // relative local transforms by name). v3 clips animate only rotation and
    // carry no translation, so the player takes each bone's rest translation
    // (and any un-keyed bone's rest rotation) from here. Without it, v3 bones
    // fall back to the skin skeleton's inverse-bind, or identity.
    //
    // `parents` (optional) is the .skeleton bone hierarchy. With it, a skin bone
    // the clip does NOT drive (every cloth `bone_software_*` bone in a v3 clip)
    // is placed at its SKINNED POSE — its rest-pose chain composed onto the
    // nearest DRIVEN ancestor's animated world — instead of snapping to an
    // identity skin matrix (which leaves the vertex at bind-world, i.e. the
    // piece floats while the body moves). This is the D4 "cloth tracks the
    // bone-skinned pose" rule: a cape with no physics still moves with the spine.
    void init(const AnimClip* clip, const SkinSkeleton* skel,
              const BoneLocals* locals = nullptr,
              const BoneParents* parents = nullptr);

    // Skin matrices (16 floats per skin bone, same layout as evaluate's
    // output) used for any bone the clip does not drive. Two cases need this,
    // both measured on real data:
    //   * a mesh authored OUTSIDE character space — an in-hand weapon from
    //     Char/item lives in weapon space and only lands on the body through
    //     its hardpoint transform, so "not animated" must mean "stay at the
    //     hardpoint", not "identity".
    //   * a track the clip PARKS at 1666-2000 units to hide the attachment;
    //     a wardrobe viewer wants to keep showing the piece, so parked tracks
    //     fall back too (kParkRadius, same rule the overlay uses).
    // Empty (default) restores the plain identity behaviour.
    void setFallbackSkin(std::vector<float> skinMats16);

    bool   valid() const { return m_clip && m_skel; }
    float  durationMs() const { return m_clip ? (float)m_clip->durationMs : 0.0f; }
    int    matchedBones() const { return m_matched; }

    // skinMats: 16 floats per skin bone (upload-ready, see header note).
    // segs/joints: overlay lines (parented tracks) and points, world space.
    void evaluate(float tMs, std::vector<float>* skinMats,
                  std::vector<float>* segs, std::vector<float>* joints);

    // Per-track WORLD transform at time tMs (12 floats/track, row-vector 4x3 —
    // the same array evaluate() builds in step 1, bind-fallback included).
    // Exposed so the cloth solver anchors its chains on exactly the world the
    // viewport draws. `out` is resized to tracks.size()*12.
    void computeWorlds(float tMs, std::vector<float>* out) const;

    int trackCount() const { return m_clip ? (int)m_clip->tracks.size() : 0; }

private:
    const AnimClip*     m_clip = nullptr;
    const SkinSkeleton* m_skel = nullptr;
    std::vector<int>    m_trackOfBone;   // skin bone -> clip track, -1 = none
    int                 m_matched = 0;
    std::vector<float>  m_world;         // scratch: 12 floats (4x3 rows) per track
    std::vector<float>  m_fallback;      // optional 16 floats per skin bone
    std::vector<float>  m_bindLocal;     // 12 floats/track: bind-pose local xform
    std::vector<char>   m_hasBind;       // per track: bindLocal is valid
    // Skinned-pose fallback for UNDRIVEN skin bones (see init's `parents` note):
    // per skin bone, the driven track its rest-chain hangs off (-1 = none), and
    // the accumulated rest-local transform from the bone up to that anchor. When
    // set, evaluate places the bone at m_fbAccum @ world[m_fbAnchor] instead of
    // an identity skin matrix.
    std::vector<int>    m_fbAnchor;      // per skin bone: anchor track, -1 = none
    std::vector<float>  m_fbAccum;       // per skin bone: 12 floats (row 4x3)
};

} // namespace di
