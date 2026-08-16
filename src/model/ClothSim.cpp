#include "model/ClothSim.h"

#include "model/AnimPose.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace di {
namespace {

// ---- small math on ROW-vector 4x3 affines and column 3x3 rotations --------
// A 4x3 "row affine" m[0..11]: m[0..8] the 3x3 block in ROW-vector convention
// (p' = p @ block, so it is the TRANSPOSE of the column rotation), m[9..11] the
// translation. A "col rot" R[0..8] is r[row*3+col] with v' = R @ v.

void mul43(const float* a, const float* b, float* out)   // out = a @ b (row)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = a[r * 3 + 0] * b[0 + c] + a[r * 3 + 1] * b[3 + c] +
                      a[r * 3 + 2] * b[6 + c];
            if (r == 3) s += b[9 + c];
            out[r * 3 + c] = s;
        }
}

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
    const float tx = m[9], ty = m[10], tz = m[11];
    out[9]  = -(tx * out[0] + ty * out[3] + tz * out[6]);
    out[10] = -(tx * out[1] + ty * out[4] + tz * out[7]);
    out[11] = -(tx * out[2] + ty * out[5] + tz * out[8]);
    return true;
}

void m3_mul(const float* A, const float* B, float* out)   // out = A @ B (col)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = A[r * 3 + 0] * B[0 * 3 + c] +
                             A[r * 3 + 1] * B[1 * 3 + c] +
                             A[r * 3 + 2] * B[2 * 3 + c];
}

void m3_mulvec(const float* R, const float* v, float* out)   // out = R @ v
{
    out[0] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
    out[1] = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
    out[2] = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
}

// col rot = transpose of a row affine's 3x3 block
void rowblock_to_colrot(const float* m12, float* R9)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) R9[i * 3 + j] = m12[j * 3 + i];
}

// write a col rot back into the 3x3 block of a row affine (transpose again)
void colrot_to_rowblock(const float* R9, float* m12)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m12[i * 3 + j] = R9[j * 3 + i];
}

// col rot -> quaternion (x,y,z,w), the COLUMN-convention quat glTF/AnimParser use
void colrot_to_quat(const float* R, float q[4])
{
    const float t = R[0] + R[4] + R[8];
    if (t > 0.0f) {
        float s = std::sqrt(t + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (R[7] - R[5]) / s;
        q[1] = (R[2] - R[6]) / s;
        q[2] = (R[3] - R[1]) / s;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        float s = std::sqrt(1.0f + R[0] - R[4] - R[8]) * 2.0f;
        q[3] = (R[7] - R[5]) / s;
        q[0] = 0.25f * s;
        q[1] = (R[1] + R[3]) / s;
        q[2] = (R[2] + R[6]) / s;
    } else if (R[4] > R[8]) {
        float s = std::sqrt(1.0f + R[4] - R[0] - R[8]) * 2.0f;
        q[3] = (R[2] - R[6]) / s;
        q[0] = (R[1] + R[3]) / s;
        q[1] = 0.25f * s;
        q[2] = (R[5] + R[7]) / s;
    } else {
        float s = std::sqrt(1.0f + R[8] - R[0] - R[4]) * 2.0f;
        q[3] = (R[3] - R[1]) / s;
        q[0] = (R[2] + R[6]) / s;
        q[1] = (R[5] + R[7]) / s;
        q[2] = 0.25f * s;
    }
    float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 1e-8f) { q[0]/=n; q[1]/=n; q[2]/=n; q[3]/=n; } else { q[0]=q[1]=q[2]=0; q[3]=1; }
}

// Shortest-arc col rotation taking unit vector a -> unit vector b.
void rot_between(const float* a, const float* b, float* R)
{
    const float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    if (d > 0.99999f) {                       // already aligned
        R[0]=1;R[1]=0;R[2]=0; R[3]=0;R[4]=1;R[5]=0; R[6]=0;R[7]=0;R[8]=1; return;
    }
    float ax, ay, az;
    if (d < -0.99999f) {                       // opposite: 180 about any perp
        // pick the axis least aligned with a
        float t[3] = {1,0,0};
        if (std::fabs(a[0]) > 0.9f) { t[0]=0; t[1]=1; }
        ax = a[1]*t[2]-a[2]*t[1]; ay = a[2]*t[0]-a[0]*t[2]; az = a[0]*t[1]-a[1]*t[0];
    } else {
        ax = a[1]*b[2]-a[2]*b[1]; ay = a[2]*b[0]-a[0]*b[2]; az = a[0]*b[1]-a[1]*b[0];
    }
    float qw = 1.0f + d;                       // (axis, 1+cos) then normalize
    if (d < -0.99999f) qw = 0.0f;
    float qx = ax, qy = ay, qz = az;
    float n = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
    if (n < 1e-12f) { R[0]=1;R[1]=0;R[2]=0;R[3]=0;R[4]=1;R[5]=0;R[6]=0;R[7]=0;R[8]=1; return; }
    qx/=n; qy/=n; qz/=n; qw/=n;
    R[0] = 1-2*(qy*qy+qz*qz); R[1] = 2*(qx*qy-qw*qz); R[2] = 2*(qx*qz+qw*qy);
    R[3] = 2*(qx*qy+qw*qz);   R[4] = 1-2*(qx*qx+qz*qz); R[5] = 2*(qy*qz-qw*qx);
    R[6] = 2*(qx*qz-qw*qy);   R[7] = 2*(qy*qz+qw*qx);   R[8] = 1-2*(qx*qx+qy*qy);
}

std::string lower(const std::string& n)
{
    std::string s; s.reserve(n.size());
    for (char c : n) s.push_back((char)std::tolower((unsigned char)c));
    return s;
}

bool nameIsCloth(const std::string& n)
{
    const std::string s = lower(n);
    return s.find("software") != std::string::npos ||
           s.find("cloth")    != std::string::npos;
}

// Hair/fur/tassel-like trim wants tight pose tracking (the D4 "hair treatment"),
// even when the chain is long — it should hug the head/shoulder, not swing like a
// rope. Everything else is treated structurally (long chain => free pendulum).
bool nameIsHairLike(const std::string& n)
{
    const std::string s = lower(n);
    return s.find("hair")   != std::string::npos ||
           s.find("fur")    != std::string::npos ||
           s.find("tassel") != std::string::npos ||
           s.find("fringe") != std::string::npos ||
           s.find("feather")!= std::string::npos;
}

// One simulated bone.
struct CBone {
    std::string name;
    float rest[12];        // effective LOCAL rest transform relative to its parent
    float restLen = 0.0f;  // |rest translation|
    int   parentCloth = -1;  // index into the CBone vector, or -1 (root)
    int   parentTrack = -1;  // augmented-track index of the parent (>=0)
    int   augIdx = -1;       // this bone's augmented-track index
    int   baseTrack = -1;    // existing base track to overwrite, or -1 (append)
    float trkScale = 1.0f;   // per-bone multiplier on boneTracking (A5: chain vs hair)
    // sim state (world)
    float pos[3]  = {0,0,0};
    float prev[3] = {0,0,0};
};

} // namespace

bool hasClothBones(const SkinSkeleton& skel)
{
    for (const auto& b : skel.bones)
        if (nameIsCloth(b.name)) return true;
    return false;
}

std::shared_ptr<AnimClip> bakeCloth(const AnimClip& base,
                                    const SkinSkeleton& skel,
                                    const BoneParents& parents,
                                    const BoneLocals& locals,
                                    const ClothParams& P)
{
    auto out = std::make_shared<AnimClip>(base);   // unchanged copy is the default
    if (base.durationMs == 0 || base.tracks.empty()) return out;

    // Which clip tracks exist, by name, and their index.
    std::unordered_map<std::string, int> trackByName;
    trackByName.reserve(base.tracks.size() * 2);
    for (int i = 0; i < (int)base.tracks.size(); ++i)
        trackByName.emplace(base.tracks[i].name, i);

    // Collect cloth bones that are actually skinned (only those deform a mesh).
    // Dedup by name; require a rest transform in the .skeleton.
    std::vector<CBone> cb;
    std::unordered_map<std::string, int> cbByName;
    for (const auto& sb : skel.bones) {
        if (!nameIsCloth(sb.name)) continue;
        if (cbByName.count(sb.name)) continue;
        auto lit = locals.find(sb.name);
        if (lit == locals.end()) continue;         // no rest pose -> can't sim
        CBone c;
        c.name = sb.name;
        cbByName.emplace(sb.name, (int)cb.size());
        cb.push_back(std::move(c));
    }
    if (cb.empty()) return out;

    const int baseCount = (int)base.tracks.size();

    // Resolve each cloth bone's rest transform + parent. The rest transform is
    // the bone's LOCAL pose relative to its DIRECT .skeleton parent, and that
    // parent must be either another simulated cloth bone or an existing clip
    // track (an animated body bone = the anchor). We deliberately do NOT fold
    // non-track connector bones into a distant anchor: the glTF exporter applies
    // each baked key relative to the node's tree parent (the direct parent), so
    // anchoring anywhere else would make the export disagree with the viewport.
    // A cloth bone whose direct parent is neither is left at bind (status quo).
    for (int i = 0; i < (int)cb.size(); ++i) {
        CBone& c = cb[i];
        std::memcpy(c.rest, locals.at(c.name).m, 12 * sizeof(float));
        std::string pname;
        {
            auto pit = parents.find(c.name);
            pname = pit == parents.end() ? std::string() : pit->second;
        }
        auto cbit = cbByName.find(pname);
        auto tit  = trackByName.find(pname);
        if (cbit != cbByName.end())        c.parentCloth = cbit->second;
        else if (tit != trackByName.end()) c.parentTrack = tit->second;
        else                               c.parentCloth = -2;   // un-anchorable
        c.restLen = std::sqrt(c.rest[9]*c.rest[9] + c.rest[10]*c.rest[10] + c.rest[11]*c.rest[11]);
        c.baseTrack = (trackByName.count(c.name) ? trackByName[c.name] : -1);
    }

    // Drop un-anchorable bones and anything whose cloth-parent was dropped;
    // repeat to convergence so orphaned chain tails go too.
    std::vector<char> keep(cb.size(), 1);
    for (int i = 0; i < (int)cb.size(); ++i)
        if (cb[i].parentCloth == -2) keep[i] = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < (int)cb.size(); ++i)
            if (keep[i] && cb[i].parentCloth >= 0 && !keep[cb[i].parentCloth]) {
                keep[i] = 0; changed = true;
            }
    }

    // Topological order (parents before children): a cloth bone's cloth-parent
    // has a lower vector index only by luck, so sort by chain depth.
    std::vector<int> depth(cb.size(), 0);
    for (int i = 0; i < (int)cb.size(); ++i) {
        int d = 0, p = cb[i].parentCloth, g = 0;
        while (p >= 0 && g++ < 256) { ++d; p = cb[p].parentCloth; }
        depth[i] = d;
    }
    std::vector<int> order;
    order.reserve(cb.size());
    for (int i = 0; i < (int)cb.size(); ++i) if (keep[i]) order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b){ return depth[a] < depth[b]; });
    if (order.empty()) return out;

    // ── A5: chain-vs-hair tracking. ─────────────────────────────────────────
    // Chain deviation is CUMULATIVE along the joints (measured: freeing only the
    // tip gives LESS tip travel than a uniformly-freer chain, because every joint
    // bends a little and the bends add up toward the tip). So the pendulum-vs-hug
    // distinction is per-CHAIN, not per-bone: a LONG free chain (tail/rope,
    // depth >= 3) gets its whole length run at reduced tracking so it swings as a
    // unit; SHORT trim (depth 1-2) and anything hair/fur/feather-named keeps full
    // tracking so it hugs the head/shoulder instead of flailing.
    {
        std::vector<int> rootOf(cb.size(), -1);
        for (int i = 0; i < (int)cb.size(); ++i) {
            int r = i, g = 0;
            while (cb[r].parentCloth >= 0 && g++ < 256) r = cb[r].parentCloth;
            rootOf[i] = r;
        }
        std::vector<int> maxDepthOfRoot(cb.size(), 0);
        for (int i = 0; i < (int)cb.size(); ++i)
            maxDepthOfRoot[rootOf[i]] = std::max(maxDepthOfRoot[rootOf[i]], depth[i]);
        for (int i = 0; i < (int)cb.size(); ++i) {
            const int md = maxDepthOfRoot[rootOf[i]];
            const bool hair = nameIsHairLike(cb[i].name) || nameIsHairLike(cb[rootOf[i]].name);
            // Free pendulum: 0.4x tracking across the whole chain (more swing);
            // hair / short trim: 1.0x (hug the pose).
            cb[i].trkScale = (md >= 3 && !hair) ? 0.4f : 1.0f;
        }
    }

    // ── A7: env-gated diagnostics (DI_DUMP_CLOTH). Instrument-first: report what
    // the solver actually classified so tuning cites numbers, not guesses. ─────
    const bool dump = std::getenv("DI_DUMP_CLOTH") != nullptr;
    if (dump) {
        int roots = 0, chained = 0, free = 0, hairish = 0, maxDepth = 0;
        for (int idx : order) {
            if (cb[idx].parentCloth < 0) ++roots; else ++chained;
            if (cb[idx].trkScale < 1.0f) ++free; else ++hairish;
            if (depth[idx] > maxDepth) maxDepth = depth[idx];
        }
        std::fprintf(stderr,
            "[DI_DUMP_CLOTH] clip dur=%ums tracks=%d | cloth bones: with-rest=%d "
            "simulated=%d dropped=%d | roots=%d chained=%d | free-pendulum=%d "
            "hair/short=%d | maxChainDepth=%d | tracking=%.2f grav=%.2f stiff=%.2f\n",
            base.durationMs, (int)base.tracks.size(), (int)cb.size(),
            (int)order.size(), (int)cb.size() - (int)order.size(), roots, chained,
            free, hairish, maxDepth, P.boneTracking, P.gravity, P.stiffness);
    }

    // Assign augmented-track indices. Case B (bone already a base track) keeps
    // its slot and is overwritten; Case A appends after the base tracks.
    int appendCursor = baseCount;
    for (int idx : order) {
        CBone& c = cb[idx];
        if (c.baseTrack >= 0) c.augIdx = c.baseTrack;
        else                  c.augIdx = appendCursor++;
        // parentTrack for a cloth-parented bone is the parent's augmented slot
        if (c.parentCloth >= 0) c.parentTrack = cb[c.parentCloth].augIdx;
    }

    // Safety: the whole pipeline (PosePlayer, GlbExporter) assumes a track's
    // parent index precedes it. Overwritten (Case B) bones keep a low base
    // index, so a rare mixed chain (bone keyed but its cloth parent not) could
    // put a parent after its child. If that ever happens, don't emit a broken
    // clip — fall back to the unchanged copy.
    for (int idx : order) {
        const CBone& c = cb[idx];
        if (c.parentTrack < 0 || c.parentTrack >= c.augIdx) return out;
    }

    // ---- frame timeline: sample at ~60 Hz for a smooth solve/export ----------
    const float step = 1000.0f / 60.0f;
    int nFrames = (int)std::floor((float)base.durationMs / step) + 1;
    nFrames = std::max(2, std::min(nFrames, 4096));
    const float dt = step / 1000.0f;

    // Base-track worlds come from the SAME evaluator the viewport uses, so the
    // baked cloth attaches exactly where the body draws.
    PosePlayer player;
    player.init(&base, &skel, &locals);

    // Per-bone key streams we will fill, then splice into the clip.
    std::vector<std::vector<AnimPosKey>> posKeys(order.size());
    std::vector<std::vector<AnimRotKey>> rotKeys(order.size());

    // Combined per-frame world array indexed by augmented-track index.
    const int augCount = appendCursor;
    std::vector<float> augWorld((size_t)augCount * 12, 0.0f);
    std::vector<float> baseWorld;
    const float gravWorld[3] = {0.0f, -P.gravity, 0.0f};
    float dumpWorstDrift = 0.0f;   // A7: worst |sim - skinned pose| seen while baking
    int   dumpWorstBone  = -1;

    // ── A6 body-collision colliders (opt-in). Spheres on core body base bones,
    // radius = bodyRadius × that bone's rest length. Center is read per frame
    // from baseWorld; disabled entirely when bodyRadius <= 0. ─────────────────
    std::vector<int>   colTrack;   // base-track index of each collider bone
    std::vector<float> colR;       // sphere radius (rest-scaled)
    if (P.bodyRadius > 0.0f) {
        auto isBody = [](const std::string& n) {
            const std::string s = lower(n);
            const char* keys[] = { "spine","pelvis","hip","chest","neck","head",
                                   "thigh","calf","clavicle","shoulder","upperarm",
                                   "forearm","torso","waist","leg" };
            for (const char* k : keys) if (s.find(k) != std::string::npos) return true;
            return false;
        };
        for (int t = 0; t < baseCount; ++t) {
            const std::string& nm = base.tracks[t].name;
            if (nameIsCloth(nm) || !isBody(nm)) continue;
            auto lit = locals.find(nm);
            float restLen = 0.0f;
            if (lit != locals.end()) {
                const float* m = lit->second.m;
                restLen = std::sqrt(m[9]*m[9] + m[10]*m[10] + m[11]*m[11]);
            }
            if (restLen < 1e-5f) continue;   // no length to scale a radius from
            colTrack.push_back(t);
            colR.push_back(P.bodyRadius * restLen);
        }
    }
    std::vector<float> colC(colTrack.size() * 3, 0.0f);   // per-frame centers

    // One solve step for the whole cloth set at a given time. `settleOnly`
    // freezes the timeline at frame 0 to relax the initial pose.
    auto parentWorldOf = [&](const CBone& c) -> const float* {
        return &augWorld[(size_t)c.parentTrack * 12];
    };
    // Push a point out of any body-collision sphere it is inside (A6).
    auto collide = [&](float* p) {
        for (size_t ci = 0; ci < colTrack.size(); ++ci) {
            const float* C = &colC[ci * 3];
            const float R = colR[ci];
            float d[3] = { p[0]-C[0], p[1]-C[1], p[2]-C[2] };
            float len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (len < R && len > 1e-6f) {
                const float s = R / len;
                p[0] = C[0] + d[0]*s; p[1] = C[1] + d[1]*s; p[2] = C[2] + d[2]*s;
            }
        }
    };
    auto solveFrame = [&](float tMs, bool record) {
        player.computeWorlds(tMs, &baseWorld);
        // seed augWorld with the animated base tracks
        for (int t = 0; t < baseCount; ++t)
            std::memcpy(&augWorld[(size_t)t * 12], &baseWorld[(size_t)t * 12], 12 * sizeof(float));
        // per-frame collider centers from the animated body bones
        for (size_t ci = 0; ci < colTrack.size(); ++ci)
            std::memcpy(&colC[ci * 3], &baseWorld[(size_t)colTrack[ci] * 12 + 9], 3 * sizeof(float));

        for (int s = 0; s < (int)order.size(); ++s) {
            CBone& c = cb[order[s]];
            const float* pW = parentWorldOf(c);          // parent world (row 4x3)
            const float parentPos[3] = { pW[9], pW[10], pW[11] };

            // rigid (no-physics) target: rest @ parentWorld
            float rigidW[12];
            mul43(c.rest, pW, rigidW);
            const float target[3] = { rigidW[9], rigidW[10], rigidW[11] };

            // Verlet integrate the bone origin in world space.
            float np[3];
            for (int k = 0; k < 3; ++k) {
                float vel = (c.pos[k] - c.prev[k]) * P.damping;
                np[k] = c.pos[k] + vel * P.inertia + gravWorld[k] * dt * dt;
            }
            for (int k = 0; k < 3; ++k) c.prev[k] = c.pos[k];
            // pull toward the rigid target (keeps it attached; kills the float)
            for (int k = 0; k < 3; ++k)
                c.pos[k] = np[k] + (target[k] - np[k]) * P.stiffness;

            // distance constraint to the parent (bone length within maxStretch),
            // interleaved with body collision so the collider has the final say.
            for (int it = 0; it < P.iterations; ++it) {
                float d[3] = { c.pos[0]-parentPos[0], c.pos[1]-parentPos[1], c.pos[2]-parentPos[2] };
                float len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                if (len > 1e-6f && c.restLen > 1e-6f) {
                    float lo = c.restLen * (2.0f - P.maxStretch);
                    float hi = c.restLen * P.maxStretch;
                    float clamped = std::max(lo, std::min(hi, len));
                    float f = clamped / len;
                    for (int k = 0; k < 3; ++k) c.pos[k] = parentPos[k] + d[k] * f;
                }
                if (!colTrack.empty()) collide(c.pos);
            }
            // NaN / blow-up guard: snap back to rigid target.
            if (!std::isfinite(c.pos[0]) || !std::isfinite(c.pos[1]) || !std::isfinite(c.pos[2])) {
                for (int k = 0; k < 3; ++k) { c.pos[k] = target[k]; c.prev[k] = target[k]; }
            }

            // BONE TRACKING (D4 flBoneTrackingFactor): the emitted pose is a
            // blend of the free sim toward the rigid/skinned target. The raw sim
            // state (c.pos/c.prev) keeps evolving for velocity continuity; only
            // the OUTPUT (bp) is pulled toward the pose. trk=1 => hug the skinned
            // pose exactly (identical to physics-off), trk=0 => full sim.
            float trk = P.boneTracking * c.trkScale;   // A5 per-bone taper
            trk = trk < 0 ? 0.0f : (trk > 1 ? 1.0f : trk);
            float bp[3];
            for (int k = 0; k < 3; ++k) bp[k] = c.pos[k] + (target[k] - c.pos[k]) * trk;
            // The skinned-pose target can sit inside the body (cloth resting on
            // it), so the tracking blend can re-penetrate — collide the OUTPUT.
            if (!colTrack.empty()) collide(bp);

            // ---- reconstruct this bone's world transform from the position ----
            float parentRotCol[9];   rowblock_to_colrot(pW, parentRotCol);
            float restRotCol[9];     rowblock_to_colrot(c.rest, restRotCol);
            float rigidRotCol[9];    m3_mul(parentRotCol, restRotCol, rigidRotCol);

            float W[12];
            if (c.restLen > 1e-6f) {
                // rest aim (world) = parentRot @ normalize(rest translation)
                float ra[3] = { c.rest[9]/c.restLen, c.rest[10]/c.restLen, c.rest[11]/c.restLen };
                float restAim[3]; m3_mulvec(parentRotCol, ra, restAim);
                float actual[3] = { bp[0]-parentPos[0], bp[1]-parentPos[1], bp[2]-parentPos[2] };
                float al = std::sqrt(actual[0]*actual[0]+actual[1]*actual[1]+actual[2]*actual[2]);
                float Wrot[9];
                if (al > 1e-6f) {
                    actual[0]/=al; actual[1]/=al; actual[2]/=al;
                    float Rd[9]; rot_between(restAim, actual, Rd);
                    m3_mul(Rd, rigidRotCol, Wrot);       // bend the joint at the parent
                } else {
                    std::memcpy(Wrot, rigidRotCol, sizeof(Wrot));
                }
                colrot_to_rowblock(Wrot, W);
            } else {
                colrot_to_rowblock(rigidRotCol, W);
            }
            W[9] = bp[0]; W[10] = bp[1]; W[11] = bp[2];
            std::memcpy(&augWorld[(size_t)c.augIdx * 12], W, 12 * sizeof(float));

            if (record && dump) {   // A7: track worst drift from the skinned pose
                const float dd[3] = { bp[0]-target[0], bp[1]-target[1], bp[2]-target[2] };
                const float dm = std::sqrt(dd[0]*dd[0]+dd[1]*dd[1]+dd[2]*dd[2]);
                if (dm > dumpWorstDrift) { dumpWorstDrift = dm; dumpWorstBone = order[s]; }
            }
            if (record) {
                // local (relative to parent track) = W @ inv(parentWorld)
                float invP[12];
                float local[12];
                if (invert43(pW, invP)) mul43(W, invP, local);
                else std::memcpy(local, c.rest, 12 * sizeof(float));
                float colr[9]; rowblock_to_colrot(local, colr);
                float q[4];    colrot_to_quat(colr, q);
                uint16_t tm = (uint16_t)std::min(65535.0f, tMs);
                AnimPosKey pk; pk.timeMs = tm;
                pk.p[0] = local[9]; pk.p[1] = local[10]; pk.p[2] = local[11];
                AnimRotKey rk; rk.timeMs = tm;
                rk.q[0] = q[0]; rk.q[1] = q[1]; rk.q[2] = q[2]; rk.q[3] = q[3];
                posKeys[s].push_back(pk);
                rotKeys[s].push_back(rk);
            }
        }
    };

    // Seed positions at the frame-0 rigid pose, then settle so nothing snaps.
    player.computeWorlds(0.0f, &baseWorld);
    for (int t = 0; t < baseCount; ++t)
        std::memcpy(&augWorld[(size_t)t * 12], &baseWorld[(size_t)t * 12], 12 * sizeof(float));
    for (int s = 0; s < (int)order.size(); ++s) {
        CBone& c = cb[order[s]];
        const float* pW = parentWorldOf(c);
        float rigidW[12]; mul43(c.rest, pW, rigidW);
        for (int k = 0; k < 3; ++k) { c.pos[k] = rigidW[9+k]; c.prev[k] = rigidW[9+k]; }
        std::memcpy(&augWorld[(size_t)c.augIdx * 12], rigidW, 12 * sizeof(float));
    }
    for (int it = 0; it < std::max(0, P.settle); ++it) solveFrame(0.0f, false);

    // Bake every frame.
    for (int f = 0; f < nFrames; ++f) {
        float tMs = std::min((float)base.durationMs, f * step);
        solveFrame(tMs, true);
    }
    if (dump) {
        const char* wn = (dumpWorstBone >= 0) ? cb[dumpWorstBone].name.c_str() : "(none)";
        std::fprintf(stderr,
            "[DI_DUMP_CLOTH] baked %d frames @ %d Hz | worst drift from skinned "
            "pose = %.4f on '%s'\n", nFrames, 60, dumpWorstDrift, wn);
    }

    // ---- splice the baked tracks into the output clip ----
    // Grow the track list for appended (Case A) bones first, so indices are
    // stable, then fill both appended and overwritten (Case B) tracks.
    out->tracks.resize(augCount);
    for (int s = 0; s < (int)order.size(); ++s) {
        CBone& c = cb[order[s]];
        AnimTrack& tr = out->tracks[c.augIdx];
        tr.name   = c.name;
        tr.parent = (int16_t)c.parentTrack;
        tr.pos    = std::move(posKeys[s]);
        tr.rot    = std::move(rotKeys[s]);
    }
    return out;
}

} // namespace di
