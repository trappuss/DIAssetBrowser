#include "model/AnimPose.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace di {

namespace {

// q (x,y,z,w) -> 3x3 rotation, laid out as the ROW block of a row-vector
// affine. The decoded quat is a standard column-convention rotation, so the
// row block is its TRANSPOSE (verified: with the cyclic quat decode this
// renders the upright idle figure; untransposed lies on its side).
void quatToRows(const float q[4], float r[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z); r[3] = 2 * (x * y - w * z); r[6] = 2 * (x * z + w * y);
    r[1] = 2 * (x * y + w * z); r[4] = 1 - 2 * (x * x + z * z); r[7] = 2 * (y * z - w * x);
    r[2] = 2 * (x * z - w * y); r[5] = 2 * (y * z + w * x); r[8] = 1 - 2 * (x * x + y * y);
}

// out = a @ b for 4x3 row-vector affines (rows 0-2 rotation, row 3 translation)
void mul43(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = a[r * 3 + 0] * b[0 + c] + a[r * 3 + 1] * b[3 + c] +
                      a[r * 3 + 2] * b[6 + c];
            if (r == 3) s += b[9 + c];
            out[r * 3 + c] = s;
        }
}

// Inverse of a 4x3 row-vector affine (general 3x3 + translation, handles scale).
// out = m^-1 such that mul43(m, out) == identity.
bool invert43(const float* m, float* out)
{
    const float a = m[0], b = m[1], c = m[2];
    const float d = m[3], e = m[4], f = m[5];
    const float g = m[6], h = m[7], i = m[8];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-20f) return false;
    const float id = 1.0f / det;
    out[0] = (e * i - f * h) * id; out[1] = (c * h - b * i) * id; out[2] = (b * f - c * e) * id;
    out[3] = (f * g - d * i) * id; out[4] = (a * i - c * g) * id; out[5] = (c * d - a * f) * id;
    out[6] = (d * h - e * g) * id; out[7] = (b * g - a * h) * id; out[8] = (a * e - b * d) * id;
    // translation: -T * Rinv
    const float tx = m[9], ty = m[10], tz = m[11];
    out[9]  = -(tx * out[0] + ty * out[3] + tz * out[6]);
    out[10] = -(tx * out[1] + ty * out[4] + tz * out[7]);
    out[11] = -(tx * out[2] + ty * out[5] + tz * out[8]);
    return true;
}

} // namespace

void PosePlayer::init(const AnimClip* clip, const SkinSkeleton* skel,
                      const BoneLocals* locals, const BoneParents* parents)
{
    m_clip = clip;
    m_skel = skel;
    m_trackOfBone.clear();
    m_matched = 0;
    m_fallback.clear();   // never carry one piece's placement onto the next
    m_fbAnchor.clear();
    m_fbAccum.clear();
    if (!clip || !skel) return;
    std::unordered_map<std::string, int> byName;
    for (int t = 0; t < (int)clip->tracks.size(); ++t)
        byName.emplace(clip->tracks[t].name, t);   // first wins
    m_trackOfBone.reserve(skel->bones.size());
    std::vector<int> boneOfTrack(clip->tracks.size(), -1);
    for (int bi = 0; bi < (int)skel->bones.size(); ++bi) {
        auto it = byName.find(skel->bones[bi].name);
        const int t = it == byName.end() ? -1 : it->second;
        if (t >= 0) { ++m_matched; if (boneOfTrack[t] < 0) boneOfTrack[t] = bi; }
        m_trackOfBone.push_back(t);
    }
    m_world.assign(clip->tracks.size() * 12, 0.0f);

    // Bind-LOCAL transform per track (from the skin skeleton's inverse-bind).
    // Used to fill channels a clip does not drive: a "held"/absent bone keeps
    // its bind rotation instead of snapping to identity (v3 clips animate only
    // rotation and leave many bones — e.g. the Pelvis — un-keyed; without this
    // the whole subtree below such a bone is mis-oriented). bindWorld = inv(
    // invBind); bindLocal = bindWorld @ invBind(parent). Only tracks that map
    // to a skin bone get one; others fall back to identity.
    m_bindLocal.assign(clip->tracks.size() * 12, 0.0f);
    m_hasBind.assign(clip->tracks.size(), 0);
    for (size_t t = 0; t < clip->tracks.size(); ++t) {
        float* bl = &m_bindLocal[t * 12];
        // 1st choice: authoritative .skeleton local rest transform (by name) —
        // covers every bone, translation + rotation, no inversion needed.
        if (locals) {
            auto it = locals->find(clip->tracks[t].name);
            if (it != locals->end()) {
                std::memcpy(bl, it->second.m, 12 * sizeof(float));
                m_hasBind[t] = 1;
                continue;
            }
        }
        // 2nd choice: derive local rest from the skin skeleton's inverse-bind.
        const int bi = boneOfTrack[t];
        if (bi < 0) continue;
        float bindWorld[12];
        if (!invert43(skel->bones[bi].invBind, bindWorld)) continue;
        const int pt = clip->tracks[t].parent;
        const int pbi = (pt >= 0 && pt < (int)boneOfTrack.size()) ? boneOfTrack[pt] : -1;
        if (pbi >= 0) mul43(bindWorld, skel->bones[pbi].invBind, bl);
        else std::memcpy(bl, bindWorld, 12 * sizeof(float));
        m_hasBind[t] = 1;
    }

    // ── Skinned-pose fallback for UNDRIVEN skin bones (A1). ──────────────────
    // With the .skeleton hierarchy, place a skin bone the clip does not drive at
    // its rest-pose chain composed onto the nearest DRIVEN ancestor's animated
    // world — so a cloth/cape bone rides the body instead of floating at bind.
    // accum = restLocal[bone] @ restLocal[parent] @ ... (up to, excluding, the
    // driven ancestor); world_bone = accum @ world[anchorTrack].
    if (parents && locals) {
        m_fbAnchor.assign(skel->bones.size(), -1);
        m_fbAccum.assign(skel->bones.size() * 12, 0.0f);
        for (int b = 0; b < (int)skel->bones.size(); ++b) {
            if (m_trackOfBone[b] >= 0) continue;   // driven — no fallback needed
            const auto lit0 = locals->find(skel->bones[b].name);
            if (lit0 == locals->end()) continue;   // no rest pose for this bone
            float accum[12];
            std::memcpy(accum, lit0->second.m, 12 * sizeof(float));
            std::string cur = skel->bones[b].name;
            int anchor = -1;
            for (int guard = 0; guard < 256; ++guard) {
                auto pit = parents->find(cur);
                if (pit == parents->end() || pit->second.empty()) break;
                const std::string& pname = pit->second;
                auto tIt = byName.find(pname);
                if (tIt != byName.end()) { anchor = tIt->second; break; }  // driven ancestor
                auto lIt = locals->find(pname);
                if (lIt == locals->end()) break;    // chain breaks — unresolved
                float acc2[12];
                mul43(accum, lIt->second.m, acc2);  // world = local @ parentLocal @ ...
                std::memcpy(accum, acc2, 12 * sizeof(float));
                cur = pname;
            }
            if (anchor >= 0) {
                m_fbAnchor[b] = anchor;
                std::memcpy(&m_fbAccum[(size_t)b * 12], accum, 12 * sizeof(float));
            }
        }
    }
}

void PosePlayer::setFallbackSkin(std::vector<float> skinMats16)
{
    m_fallback = std::move(skinMats16);
}

void PosePlayer::computeWorlds(float tMs, std::vector<float>* out) const
{
    if (!m_clip) return;
    const auto& tracks = m_clip->tracks;
    out->assign(tracks.size() * 12, 0.0f);

    // every track's world transform (parents always precede children —
    // enforced at parse time)
    // Bind-pose fallback is a v3 need (rotation-only clips leave bones un-keyed);
    // v1/v2 carry full pos+rot per track, so keep their exact prior behaviour.
    const bool bindFallback = m_clip->version == 3;
    for (size_t t = 0; t < tracks.size(); ++t) {
        float p[3], q[4], local[12];
        m_clip->sample(t, tMs, p, q);
        const bool haveBind =
            bindFallback && m_hasBind.size() == tracks.size() && m_hasBind[t];
        // Rotation: from the clip if this track is keyed, else the bind pose
        // (a held/undriven bone must keep its rest orientation, not identity).
        if (!tracks[t].rot.empty() || !haveBind) {
            quatToRows(q, local);
        } else {
            const float* bl = &m_bindLocal[t * 12];
            for (int k = 0; k < 9; ++k) local[k] = bl[k];
        }
        // Translation: from the clip if keyed, else the bind-pose local offset.
        if (!tracks[t].pos.empty()) {
            local[9] = p[0]; local[10] = p[1]; local[11] = p[2];
        } else if (haveBind) {
            local[9] = m_bindLocal[t * 12 + 9];
            local[10] = m_bindLocal[t * 12 + 10];
            local[11] = m_bindLocal[t * 12 + 11];
        } else {
            local[9] = p[0]; local[10] = p[1]; local[11] = p[2];
        }
        float* w = &(*out)[t * 12];
        if (tracks[t].parent < 0) {
            for (int i = 0; i < 12; ++i) w[i] = local[i];
        } else {
            mul43(local, &(*out)[(size_t)tracks[t].parent * 12], w);
        }
    }
}

void PosePlayer::evaluate(float tMs, std::vector<float>* skinMats,
                          std::vector<float>* segs, std::vector<float>* joints)
{
    if (!valid()) return;
    const auto& tracks = m_clip->tracks;

    // 1. every track's world transform (see computeWorlds).
    computeWorlds(tMs, &m_world);

    // 2. skin matrices: skin = invBind @ world, emitted as skin^T in
    //    column-major order (= skin rows written sequentially)
    if (skinMats) {
        skinMats->resize(m_skel->bones.size() * 16);
        const bool haveFallback =
            m_fallback.size() >= m_skel->bones.size() * 16;
        for (size_t b = 0; b < m_skel->bones.size(); ++b) {
            float* m = skinMats->data() + b * 16;
            const int t = m_trackOfBone[b];
            // A parked track (the hide-at-infinity trick, see below) counts as
            // "not driven" so hand-held pieces keep their hardpoint placement.
            // ONLY when a fallback exists: without one the alternative is an
            // identity skin matrix, which would drop a parked cosmetic onto
            // the character's feet, and would collapse an entire mesh to
            // undeformed space during a clip with >100 units of root motion
            // (audit finding). No fallback == the old behaviour, exactly.
            bool driven = t >= 0;
            if (driven && haveFallback) {
                const float* w = &m_world[(size_t)t * 12 + 9];
                if (w[0] * w[0] + w[1] * w[1] + w[2] * w[2] > 100.0f * 100.0f)
                    driven = false;
            }
            if (!driven && haveFallback) {
                std::memcpy(m, m_fallback.data() + b * 16, 16 * sizeof(float));
                continue;
            }
            float S[12];
            if (!driven) {
                // Skinned-pose fallback (A1): an undriven skin bone rides its
                // nearest driven ancestor via the rest chain, so cloth follows
                // the body instead of floating. world = accum @ anchorWorld;
                // skin = invBind @ world. Falls back to identity only when the
                // bone has no resolvable driven ancestor.
                const int a = (b < m_fbAnchor.size()) ? m_fbAnchor[b] : -1;
                if (a >= 0) {
                    float world[12];
                    mul43(&m_fbAccum[(size_t)b * 12], &m_world[(size_t)a * 12], world);
                    mul43(m_skel->bones[b].invBind, world, S);
                } else {
                    // bone not animated and unanchored: bind == identity skin
                    S[0] = 1; S[1] = 0; S[2] = 0;
                    S[3] = 0; S[4] = 1; S[5] = 0;
                    S[6] = 0; S[7] = 0; S[8] = 1;
                    S[9] = 0; S[10] = 0; S[11] = 0;
                }
            } else {
                mul43(m_skel->bones[b].invBind, &m_world[(size_t)t * 12], S);
            }
            // GL column-major of skin^T: m[c*4+r] = S[c][r]
            for (int c = 0; c < 4; ++c) {
                m[c * 4 + 0] = S[c * 3 + 0];
                m[c * 4 + 1] = S[c * 3 + 1];
                m[c * 4 + 2] = S[c * 3 + 2];
                m[c * 4 + 3] = (c == 3) ? 1.0f : 0.0f;
            }
        }
    }

    // 3. overlay geometry from track worlds. Clips PARK unused attachment
    //    tracks ~2000 units away (measured: zhushou_/fushou_/HP_*weapon local
    //    translations of 1666-2000 in a_bandageheal — the hide-at-infinity
    //    trick), which drew kilometer-long rays through the viewport. Any
    //    joint beyond kParkRadius is dropped, along with its segments; real
    //    character bones stay within ~3 units.
    constexpr float kParkRadius2 = 100.0f * 100.0f;
    auto parked = [&](size_t t) {
        const float* w = &m_world[t * 12 + 9];
        return w[0]*w[0] + w[1]*w[1] + w[2]*w[2] > kParkRadius2;
    };
    if (joints) {
        joints->clear();
        joints->reserve(tracks.size() * 3);
        for (size_t t = 0; t < tracks.size(); ++t) {
            if (parked(t)) continue;
            joints->push_back(m_world[t * 12 + 9]);
            joints->push_back(m_world[t * 12 + 10]);
            joints->push_back(m_world[t * 12 + 11]);
        }
    }
    if (segs) {
        segs->clear();
        for (size_t t = 0; t < tracks.size(); ++t) {
            const int p = tracks[t].parent;
            if (p < 0 || parked(t) || parked((size_t)p)) continue;
            segs->push_back(m_world[(size_t)p * 12 + 9]);
            segs->push_back(m_world[(size_t)p * 12 + 10]);
            segs->push_back(m_world[(size_t)p * 12 + 11]);
            segs->push_back(m_world[t * 12 + 9]);
            segs->push_back(m_world[t * 12 + 10]);
            segs->push_back(m_world[t * 12 + 11]);
        }
    }
}

} // namespace di
