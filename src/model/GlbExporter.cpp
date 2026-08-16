#include "model/GlbExporter.h"

#include <QBuffer>
#include <QDebug>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace {

constexpr int FLOAT_T  = 5126;
constexpr int USHORT_T = 5123;
constexpr int UINT_T   = 5125;
constexpr int ARRAY_BUFFER         = 34962;
constexpr int ELEMENT_ARRAY_BUFFER = 34963;

struct BinBuilder {
    QByteArray blob;
    QJsonArray views;
    QJsonArray accessors;

    void pad4() { while (blob.size() % 4) blob.append('\0'); }

    int addView(const QByteArray& data, int target = 0)
    {
        pad4();
        QJsonObject v;
        v["buffer"]     = 0;
        v["byteOffset"] = blob.size();
        v["byteLength"] = data.size();
        if (target) v["target"] = target;
        blob += data;
        views.append(v);
        return views.size() - 1;
    }

    int addAccessor(const void* data, int elemCount, int compType,
                    const char* type, int compsPerElem, int bytesPerComp,
                    int target = 0, const float* minv = nullptr,
                    const float* maxv = nullptr)
    {
        const QByteArray bytes(reinterpret_cast<const char*>(data),
                               (qsizetype)elemCount * compsPerElem * bytesPerComp);
        const int view = addView(bytes, target);
        QJsonObject a;
        a["bufferView"]    = view;
        a["componentType"] = compType;
        a["count"]         = elemCount;
        a["type"]          = QLatin1String(type);
        if (minv && maxv) {
            QJsonArray mn, mx;
            for (int i = 0; i < compsPerElem; ++i) {
                mn.append((double)minv[i]);
                mx.append((double)maxv[i]);
            }
            a["min"] = mn;
            a["max"] = mx;
        }
        accessors.append(a);
        return accessors.size() - 1;
    }
};

// ---- row-vector 4x3 affine helpers (rows 0-2 rotation, row 3 translation) ----

void identity43(float* m)
{
    m[0] = 1; m[1] = 0; m[2] = 0;
    m[3] = 0; m[4] = 1; m[5] = 0;
    m[6] = 0; m[7] = 0; m[8] = 1;
    m[9] = 0; m[10] = 0; m[11] = 0;
}

// out = a @ b (row-vector composition: apply a, then b)
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

// inverse of a rigid row affine: R' = R^T, t' = -t @ R^T
void inv43(const float* a, float* out)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[c * 3 + r];
    for (int c = 0; c < 3; ++c)
        out[9 + c] = -(a[9] * out[c] + a[10] * out[3 + c] + a[11] * out[6 + c]);
}

// glTF quaternion (x,y,z,w, COLUMN convention) of a row affine's rotation
// block: the column matrix is the row block's transpose, M[r][c] = m[c*3+r].
void rowRotToQuat(const float* m, float q[4])
{
    const auto M = [&](int r, int c) { return m[c * 3 + r]; };
    const float tr = M(0, 0) + M(1, 1) + M(2, 2);
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (M(2, 1) - M(1, 2)) / s;
        q[1] = (M(0, 2) - M(2, 0)) / s;
        q[2] = (M(1, 0) - M(0, 1)) / s;
    } else if (M(0, 0) > M(1, 1) && M(0, 0) > M(2, 2)) {
        const float s = std::sqrt(1.0f + M(0, 0) - M(1, 1) - M(2, 2)) * 2.0f;
        q[0] = 0.25f * s;
        q[1] = (M(0, 1) + M(1, 0)) / s;
        q[2] = (M(0, 2) + M(2, 0)) / s;
        q[3] = (M(2, 1) - M(1, 2)) / s;
    } else if (M(1, 1) > M(2, 2)) {
        const float s = std::sqrt(1.0f + M(1, 1) - M(0, 0) - M(2, 2)) * 2.0f;
        q[0] = (M(0, 1) + M(1, 0)) / s;
        q[1] = 0.25f * s;
        q[2] = (M(1, 2) + M(2, 1)) / s;
        q[3] = (M(0, 2) - M(2, 0)) / s;
    } else {
        const float s = std::sqrt(1.0f + M(2, 2) - M(0, 0) - M(1, 1)) * 2.0f;
        q[0] = (M(0, 2) + M(2, 0)) / s;
        q[1] = (M(1, 2) + M(2, 1)) / s;
        q[2] = 0.25f * s;
        q[3] = (M(1, 0) - M(0, 1)) / s;
    }
    float len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len > 1e-12f)
        for (int i = 0; i < 4; ++i) q[i] /= len;
    else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
}

// column-major glTF MAT4 of a row affine = its rows flattened with 0/0/0/1
// per column (byte-identical to the GL skin-matrix upload layout)
void rowAffineToGltfMat4(const float* a, float* m16)
{
    for (int c = 0; c < 4; ++c) {
        m16[c * 4 + 0] = a[c * 3 + 0];
        m16[c * 4 + 1] = a[c * 3 + 1];
        m16[c * 4 + 2] = a[c * 3 + 2];
        m16[c * 4 + 3] = (c == 3) ? 1.0f : 0.0f;
    }
}

// Standard 3ds Max Biped parent of a Bip bone, restricted to bones actually
// present — ported from the reference (skinskeleton_parser._biped_parent).
// Returns empty when the bone has no present parent (stays under the root).
QString bipedParent(const QString& name, const QSet<QString>& present)
{
    if (!name.startsWith(QLatin1String("Bip001")))
        return {};
    const QString suf = name.mid(6).trimmed();

    // Finger chains: "Bip001 L Finger01" -> "Bip001 L Finger0" -> "Bip001 L Hand"
    static const QRegularExpression fingerRe(
        QStringLiteral("^([LR]) (Finger|Toe)(\\d)(\\d*)$"));
    const QRegularExpressionMatch m = fingerRe.match(suf);
    if (m.hasMatch()) {
        const QString side = m.captured(1), kind = m.captured(2),
                      fid = m.captured(3), seg = m.captured(4);
        QString cand;
        if (!seg.isEmpty())
            cand = QStringLiteral("Bip001 %1 %2%3%4")
                       .arg(side, kind, fid, seg.left(seg.size() - 1));
        else
            cand = QStringLiteral("Bip001 %1 %2")
                       .arg(side, kind == QLatin1String("Finger")
                                      ? QStringLiteral("Hand")
                                      : QStringLiteral("Foot"));
        return present.contains(cand) ? cand : QString();
    }

    // Twist chains — DI skeletons replace the forearm with twist bones
    // (measured: UpperArm (y1.635) > ForeTwist (1.395) > ForeTwist1 (1.283) >
    // Hand (1.171) on f_barbarian_yifu_t07_004). "X Twist<N>" parents to
    // "X Twist<N-1>"; the base twist to the limb bone it subdivides.
    static const QRegularExpression twistRe(
        QStringLiteral("^([LR]) (Fore|UpArm|UpperArm|Thigh|Calf|Horse)Twist(\\d*)$"));
    const QRegularExpressionMatch tw = twistRe.match(suf);
    if (tw.hasMatch()) {
        const QString side = tw.captured(1), kind = tw.captured(2),
                      num = tw.captured(3);
        if (!num.isEmpty()) {
            const int n = num.toInt();
            const QString cand = QStringLiteral("Bip001 %1 %2Twist%3")
                                     .arg(side, kind,
                                          n > 1 ? QString::number(n - 1) : QString());
            if (present.contains(cand)) return cand;
        }
        // base twist -> the limb it subdivides (first present candidate)
        const QStringList limbs =
            kind == QLatin1String("Fore")
                ? QStringList{QStringLiteral("Forearm"), QStringLiteral("UpperArm")}
            : (kind == QLatin1String("Calf") || kind == QLatin1String("Horse"))
                ? QStringList{QStringLiteral("Calf"), QStringLiteral("Thigh")}
            : kind == QLatin1String("Thigh")
                ? QStringList{QStringLiteral("Thigh")}
                : QStringList{QStringLiteral("UpperArm"), QStringLiteral("Clavicle")};
        for (const QString& limb : limbs) {
            const QString cand = QStringLiteral("Bip001 %1 %2").arg(side, limb);
            if (present.contains(cand)) return cand;
        }
        return {};
    }

    // Hand / Foot: prefer the DEEPEST present twist bone of their limb — that
    // is the measured chain end (ForeTwist1 sits at the wrist side).
    static const QRegularExpression handFootRe(QStringLiteral("^([LR]) (Hand|Foot)$"));
    const QRegularExpressionMatch hf = handFootRe.match(suf);
    if (hf.hasMatch()) {
        const QString side  = hf.captured(1);
        const QString twist = hf.captured(2) == QLatin1String("Hand")
                                  ? QStringLiteral("ForeTwist")
                                  : QStringLiteral("CalfTwist");
        for (int k = 9; k >= 0; --k) {
            const QString cand = QStringLiteral("Bip001 %1 %2%3")
                                     .arg(side, twist,
                                          k > 0 ? QString::number(k) : QString());
            if (present.contains(cand)) return cand;
        }
        // fall through to the chain table (Forearm/Calf then walk-up)
    }

    static const QHash<QString, QString> chain = {
        {QStringLiteral("Pelvis"), QString()},
        {QStringLiteral("Spine"), QStringLiteral("Pelvis")},
        {QStringLiteral("Spine1"), QStringLiteral("Spine")},
        {QStringLiteral("Spine2"), QStringLiteral("Spine1")},
        {QStringLiteral("Neck"), QStringLiteral("Spine2")},
        {QStringLiteral("Neck1"), QStringLiteral("Neck")},
        {QStringLiteral("Head"), QStringLiteral("Neck")},
        {QStringLiteral("L Thigh"), QStringLiteral("Pelvis")},
        {QStringLiteral("L Calf"), QStringLiteral("L Thigh")},
        {QStringLiteral("L Foot"), QStringLiteral("L Calf")},
        {QStringLiteral("L Toe0"), QStringLiteral("L Foot")},
        {QStringLiteral("R Thigh"), QStringLiteral("Pelvis")},
        {QStringLiteral("R Calf"), QStringLiteral("R Thigh")},
        {QStringLiteral("R Foot"), QStringLiteral("R Calf")},
        {QStringLiteral("R Toe0"), QStringLiteral("R Foot")},
        {QStringLiteral("L Clavicle"), QStringLiteral("Neck")},
        {QStringLiteral("L UpperArm"), QStringLiteral("L Clavicle")},
        {QStringLiteral("L Forearm"), QStringLiteral("L UpperArm")},
        {QStringLiteral("L Hand"), QStringLiteral("L Forearm")},
        {QStringLiteral("R Clavicle"), QStringLiteral("Neck")},
        {QStringLiteral("R UpperArm"), QStringLiteral("R Clavicle")},
        {QStringLiteral("R Forearm"), QStringLiteral("R UpperArm")},
        {QStringLiteral("R Hand"), QStringLiteral("R Forearm")},
    };
    auto it = chain.find(suf);
    if (it == chain.end()) return {};
    QString next = it.value();
    while (!next.isEmpty()) {                     // walk up to a PRESENT ancestor
        const QString cand = QStringLiteral("Bip001 ") + next;
        if (present.contains(cand)) return cand;
        auto up = chain.find(next);
        next = (up == chain.end()) ? QString() : up.value();
    }
    return {};
}

// Smooth per-vertex normals: accumulate area-weighted face normals.
std::vector<float> smoothNormals(const std::vector<float>& pos,
                                 const std::vector<uint32_t>& idx)
{
    const size_t nv = pos.size() / 3;
    std::vector<float> nrm(nv * 3, 0.0f);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= nv || b >= nv || c >= nv) continue;
        const float* pa = &pos[a * 3];
        const float* pb = &pos[b * 3];
        const float* pc = &pos[c * 3];
        const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                             e1[2] * e2[0] - e1[0] * e2[2],
                             e1[0] * e2[1] - e1[1] * e2[0]};
        for (uint32_t v : {a, b, c})
            for (int k = 0; k < 3; ++k)
                nrm[v * 3 + k] += fn[k];
    }
    for (size_t v = 0; v < nv; ++v) {
        float* n = &nrm[v * 3];
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; }
        else            { n[1] = 1.0f; }
    }
    return nrm;
}

// One rig node in the shared skeleton tree.
struct RigNode {
    QString name;
    int  parent = -1;
    bool haveWorld = false;
    float world[12];          // bind-pose world (row affine), when known
    float local[12];          // rest local TRS source (row affine)
};

} // namespace

namespace GlbExporter {

bool writeGlb(const QString& outPath, const std::vector<Part>& parts,
              const di::BoneParents* hierarchy, const di::BoneLocals* locals,
              const std::vector<AnimExport>& anims, const QString& sceneName,
              QString* err)
{
    // ---- validate input ------------------------------------------------
    std::vector<const Part*> live;
    for (const Part& p : parts)
        if (p.mesh && p.mesh->vertexCount() > 0)
            live.push_back(&p);
    if (live.empty()) {
        if (err) *err = QStringLiteral("nothing to export (no parts with vertices)");
        return false;
    }
    std::vector<const AnimExport*> clips;
    for (const AnimExport& a : anims)
        if (a.clip && !a.clip->tracks.empty())
            clips.push_back(&a);

    // ---- 1. collect the shared rig node set ----------------------------
    std::vector<RigNode> rig;
    QHash<QString, int> nodeOf;
    const auto addNode = [&](const QString& nm) -> int {
        auto it = nodeOf.find(nm);
        if (it != nodeOf.end()) return it.value();
        RigNode n;
        n.name = nm;
        identity43(n.local);
        identity43(n.world);
        rig.push_back(n);
        nodeOf.insert(nm, (int)rig.size() - 1);
        return (int)rig.size() - 1;
    };
    for (const Part* p : live)
        if (p->skel)
            for (const di::SkinBone& b : p->skel->bones)
                addNode(QString::fromStdString(b.name));
    for (const AnimExport* a : clips)
        for (const di::AnimTrack& t : a->clip->tracks)
            addNode(QString::fromLatin1(t.name.c_str()));

    // centroid-fallback pseudo bones for skinned parts without a skel get
    // part-scoped names so two such parts never collide
    for (const Part* p : live) {
        const di::MeshData& m = *p->mesh;
        if (!p->skel && m.skinned && m.boneIndices.size() >= m.vertexCount() * 4) {
            uint8_t maxBone = 0;
            for (size_t i = 0; i < m.vertexCount() * 4; ++i)
                maxBone = std::max(maxBone, m.boneIndices[i]);
            for (int j = 0; j <= (int)maxBone; ++j)
                addNode(QStringLiteral("%1.bone_%2").arg(p->name).arg(j));
        }
    }

    // ---- 2. parenting ---------------------------------------------------
    // clips are authoritative for animated bones (a clip may skip intermediate
    // hierarchy bones; parenting an animated track through a skipped rest bone
    // would compose a DIFFERENT world than the engine's track@parentTrack).
    QHash<QString, QString> clipParent;
    for (const AnimExport* a : clips) {
        const auto& tracks = a->clip->tracks;
        for (const di::AnimTrack& t : tracks) {
            const QString nm = QString::fromLatin1(t.name.c_str());
            if (clipParent.contains(nm)) continue;   // first clip wins
            clipParent.insert(nm, t.parent >= 0 && t.parent < (int)tracks.size()
                                      ? QString::fromLatin1(
                                            tracks[(size_t)t.parent].name.c_str())
                                      : QString());
        }
    }
    QSet<QString> present;
    for (const RigNode& n : rig) present.insert(n.name);
    int fromClip = 0, fromTree = 0, fromHeuristic = 0;
    for (RigNode& n : rig) {
        auto cp = clipParent.find(n.name);
        if (cp != clipParent.end()) {
            const int pj = cp.value().isEmpty() ? -1 : nodeOf.value(cp.value(), -1);
            n.parent = pj;
            if (pj >= 0) ++fromClip;
            continue;
        }
        if (hierarchy) {
            // walk up to the nearest ancestor present in the node set
            std::string cur = n.name.toStdString();
            for (int hop = 0; hop < 256; ++hop) {
                auto it = hierarchy->find(cur);
                if (it == hierarchy->end() || it->second.empty()) break;
                cur = it->second;
                const int pj = nodeOf.value(QString::fromStdString(cur), -1);
                if (pj >= 0 && &rig[(size_t)pj] != &n) {
                    n.parent = pj;
                    ++fromTree;
                    break;
                }
            }
        }
        if (n.parent < 0) {
            const QString p = bipedParent(n.name, present);
            if (!p.isEmpty()) {
                const int pj = nodeOf.value(p, -1);
                if (pj >= 0) {
                    n.parent = pj;
                    ++fromHeuristic;
                }
            }
        }
    }
    // cycle guard (duplicate names / degenerate data): break at the child
    for (int j = 0; j < (int)rig.size(); ++j) {
        int a = rig[j].parent, steps = 0;
        while (a >= 0 && steps++ <= (int)rig.size()) {
            if (a == j) { rig[j].parent = -1; break; }
            a = rig[a].parent;
        }
    }
    qInfo("glb: %zu rig nodes — parents %d clip / %d tree / %d heuristic / %d root",
          rig.size(), fromClip, fromTree, fromHeuristic,
          (int)rig.size() - fromClip - fromTree - fromHeuristic);

    // ---- 3. bind worlds --------------------------------------------------
    // Preference: .skeleton tree > skin inverse-bind > primary clip t=0.
    //
    // The TREE leads because it is the rig's own rest pose and it agrees with
    // the skins where both exist (measured: 1.6e-6 median / 3.7e-6 max over
    // the 57 f_barbarian_yifu_t07_004 bones). Skin-first would break parts
    // authored OUTSIDE character space: an in-hand weapon's one-bone skin is
    // an identity bind in WEAPON space, so inverting it would drag the rig's
    // hand-holder node to the origin and take every other part hanging off
    // that node with it.
    int wSkin = 0, wTree = 0, wClip = 0;
    if (locals && hierarchy) {
        // accumulate the FULL tree once (world = local @ parentWorld); the
        // depth cap guards against parent cycles from duplicate node names
        // (BoneParents is first-wins by name) — recursion without it could
        // blow the worker stack, which no SEH guard recovers from
        std::unordered_map<std::string, std::array<float, 12>> treeWorld;
        std::function<const float*(const std::string&, int)> tw =
            [&](const std::string& nm, int depth) -> const float* {
            if (depth > 256) return nullptr;
            auto hit = treeWorld.find(nm);
            if (hit != treeWorld.end()) return hit->second.data();
            auto lit = locals->find(nm);
            if (lit == locals->end()) return nullptr;
            std::array<float, 12> w;
            auto pit = hierarchy->find(nm);
            const float* pw = (pit != hierarchy->end() && !pit->second.empty())
                                  ? tw(pit->second, depth + 1)
                                  : nullptr;
            if (pw) mul43(lit->second.m, pw, w.data());
            else    std::memcpy(w.data(), lit->second.m, sizeof(w));
            return treeWorld.emplace(nm, w).first->second.data();
        };
        for (RigNode& n : rig) {
            if (n.haveWorld) continue;
            const float* w = tw(n.name.toStdString(), 0);
            if (!w) continue;
            std::memcpy(n.world, w, 12 * sizeof(float));
            n.haveWorld = true;
            ++wTree;
        }
    }
    for (const Part* p : live) {
        if (!p->skel) continue;
        for (const di::SkinBone& b : p->skel->bones) {
            const int j = nodeOf.value(QString::fromStdString(b.name), -1);
            if (j < 0 || rig[j].haveWorld) continue;
            inv43(b.invBind, rig[j].world);          // IB @ W = I  =>  W = IB^-1
            rig[j].haveWorld = true;
            ++wSkin;
        }
    }
    if (!clips.empty()) {
        // compose the primary clip at t=0 along its own track parents
        const di::AnimClip& c0 = *clips[0]->clip;
        std::vector<float> world(c0.tracks.size() * 12);
        for (size_t t = 0; t < c0.tracks.size(); ++t) {
            float p[3], q[4], local[12];
            c0.sample(t, 0.0f, p, q);
            // local rotation block = transpose of the column matrix of q
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            local[0] = 1 - 2 * (y * y + z * z); local[3] = 2 * (x * y - w * z); local[6] = 2 * (x * z + w * y);
            local[1] = 2 * (x * y + w * z); local[4] = 1 - 2 * (x * x + z * z); local[7] = 2 * (y * z - w * x);
            local[2] = 2 * (x * z - w * y); local[5] = 2 * (y * z + w * x); local[8] = 1 - 2 * (x * x + y * y);
            local[9] = p[0]; local[10] = p[1]; local[11] = p[2];
            if (c0.tracks[t].parent < 0)
                std::memcpy(&world[t * 12], local, 12 * sizeof(float));
            else
                mul43(local, &world[(size_t)c0.tracks[t].parent * 12], &world[t * 12]);
        }
        for (size_t t = 0; t < c0.tracks.size(); ++t) {
            const int j = nodeOf.value(QString::fromLatin1(c0.tracks[t].name.c_str()), -1);
            if (j < 0 || rig[j].haveWorld) continue;
            std::memcpy(rig[j].world, &world[t * 12], 12 * sizeof(float));
            rig[j].haveWorld = true;
            ++wClip;
        }
    }
    // centroid fallback worlds for pseudo bones
    for (const Part* p : live) {
        const di::MeshData& m = *p->mesh;
        if (p->skel || !m.skinned || m.boneIndices.size() < m.vertexCount() * 4 ||
            m.boneWeights.size() < m.vertexCount() * 4)
            continue;
        uint8_t maxBone = 0;
        for (size_t i = 0; i < m.vertexCount() * 4; ++i)
            maxBone = std::max(maxBone, m.boneIndices[i]);
        const int nB = (int)maxBone + 1;
        std::vector<double> acc((size_t)nB * 3, 0.0);
        std::vector<double> wsum(nB, 0.0);
        for (size_t v = 0; v < m.vertexCount(); ++v)
            for (int k = 0; k < 4; ++k) {
                const int j = m.boneIndices[v * 4 + k];
                const double w = m.boneWeights[v * 4 + k];
                for (int c = 0; c < 3; ++c)
                    acc[(size_t)j * 3 + c] += (double)m.positions[v * 3 + c] * w;
                wsum[j] += w;
            }
        for (int j = 0; j < nB; ++j) {
            const int nj = nodeOf.value(
                QStringLiteral("%1.bone_%2").arg(p->name).arg(j), -1);
            // haveWorld: two same-named skel-less parts share pseudo nodes —
            // first part wins, matching the skin/tree preference loops
            if (nj < 0 || rig[nj].haveWorld || wsum[j] <= 1e-6) continue;
            identity43(rig[nj].world);
            for (int c = 0; c < 3; ++c)
                rig[nj].world[9 + c] = (float)(acc[(size_t)j * 3 + c] / wsum[j]);
            rig[nj].haveWorld = true;
        }
    }

    {   // logged AFTER the centroid pass so "unknown" is the real count
        int unknown = 0;
        for (const RigNode& n : rig) if (!n.haveWorld) ++unknown;
        qInfo("glb: bind worlds %d tree / %d skin / %d clip / %d centroid / "
              "%d unknown", wTree, wSkin, wClip,
              (int)rig.size() - wTree - wSkin - wClip - unknown, unknown);
    }

    // ---- 4. rest locals: local = W(self) @ inv(W(parent)) ---------------
    for (RigNode& n : rig) {
        if (!n.haveWorld) continue;                  // stays identity
        if (n.parent >= 0 && rig[n.parent].haveWorld) {
            float ip[12];
            inv43(rig[n.parent].world, ip);
            mul43(n.world, ip, n.local);
        } else {
            std::memcpy(n.local, n.world, 12 * sizeof(float));
        }
    }

    // ---- 5. emit the glTF ------------------------------------------------
    BinBuilder buf;
    QJsonArray nodes, meshes, materials, skinsArr;
    QJsonArray images, texturesArr;
    QJsonArray sceneNodes;

    const auto addTexture = [&](const QImage& img) -> int {
        if (img.isNull()) return -1;
        QByteArray png;
        QBuffer pngBuf(&png);
        pngBuf.open(QIODevice::WriteOnly);
        img.save(&pngBuf, "PNG");
        const int iv = buf.addView(png);
        images.append(QJsonObject{{"bufferView", iv}, {"mimeType", "image/png"}});
        texturesArr.append(QJsonObject{{"sampler", 0}, {"source", images.size() - 1}});
        return texturesArr.size() - 1;
    };

    // mesh nodes come first (index = part order), armature root next, then
    // rig nodes at armatureRoot + 1 + rigIndex
    const int nParts = (int)live.size();
    const int armatureRoot = nParts;
    const int firstRig = armatureRoot + 1;

    for (int pi = 0; pi < nParts; ++pi) {
        const Part& p = *live[pi];
        const di::MeshData& m = *p.mesh;
        const size_t nv = m.vertexCount();

        std::vector<uint32_t> idx = m.indices;
        if (idx.size() < 3) {
            idx.resize(nv / 3 * 3);
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = (uint32_t)i;
        }
        idx.resize(idx.size() / 3 * 3);

        float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
        for (size_t v = 0; v < nv; ++v)
            for (int c = 0; c < 3; ++c) {
                mn[c] = std::min(mn[c], m.positions[v * 3 + c]);
                mx[c] = std::max(mx[c], m.positions[v * 3 + c]);
            }
        const int aPos = buf.addAccessor(m.positions.data(), (int)nv, FLOAT_T,
                                         "VEC3", 3, 4, ARRAY_BUFFER, mn, mx);
        const std::vector<float> nrm =
            m.normals.size() >= nv * 3
                ? std::vector<float>(m.normals.begin(), m.normals.begin() + nv * 3)
                : smoothNormals(m.positions, idx);
        const int aNrm = buf.addAccessor(nrm.data(), (int)nv, FLOAT_T, "VEC3", 3, 4,
                                         ARRAY_BUFFER);
        std::vector<float> uv(nv * 2, 0.0f);
        if (m.uv0.size() >= nv * 2)
            std::copy(m.uv0.begin(), m.uv0.begin() + nv * 2, uv.begin());
        const int aUv  = buf.addAccessor(uv.data(), (int)nv, FLOAT_T, "VEC2", 2, 4,
                                         ARRAY_BUFFER);
        const int aIdx = buf.addAccessor(idx.data(), (int)idx.size(), UINT_T,
                                         "SCALAR", 1, 4, ELEMENT_ARRAY_BUFFER);

        QJsonObject attrs;
        attrs["POSITION"]   = aPos;
        attrs["NORMAL"]     = aNrm;
        attrs["TEXCOORD_0"] = aUv;
        if (m.tangents.size() >= nv * 4)
            attrs["TANGENT"] = buf.addAccessor(m.tangents.data(), (int)nv, FLOAT_T,
                                               "VEC4", 4, 4, ARRAY_BUFFER);

        const bool haveSkin = m.skinned && m.boneIndices.size() >= nv * 4 &&
                              m.boneWeights.size() >= nv * 4;
        int skinIndex = -1;
        if (haveSkin) {
            uint8_t maxBone = 0;
            for (size_t i = 0; i < nv * 4; ++i)
                maxBone = std::max(maxBone, m.boneIndices[i]);
            int nBones = (int)maxBone + 1;
            // uBones-bleed parity: a mesh referencing bones past its skin list
            // would index out of joints[] — clamp and say so
            if (p.skel && nBones > (int)p.skel->bones.size()) {
                qWarning("glb: %s references bone %d but skin has %zu — clamping",
                         qPrintable(p.name), nBones - 1, p.skel->bones.size());
                nBones = (int)p.skel->bones.size();
            }

            std::vector<uint16_t> joints(nv * 4);
            std::vector<float>    weights(nv * 4);
            for (size_t v = 0; v < nv; ++v) {
                float ws = 0.0f;
                for (int k = 0; k < 4; ++k) ws += m.boneWeights[v * 4 + k];
                const float inv = ws > 0.0f ? 1.0f / ws : 0.0f;
                for (int k = 0; k < 4; ++k) {
                    joints[v * 4 + k] = std::min<uint16_t>(
                        m.boneIndices[v * 4 + k], (uint16_t)(nBones - 1));
                    weights[v * 4 + k] = m.boneWeights[v * 4 + k] * inv;
                }
            }
            attrs["JOINTS_0"]  = buf.addAccessor(joints.data(), (int)nv, USHORT_T,
                                                 "VEC4", 4, 2, ARRAY_BUFFER);
            attrs["WEIGHTS_0"] = buf.addAccessor(weights.data(), (int)nv, FLOAT_T,
                                                 "VEC4", 4, 4, ARRAY_BUFFER);

            QJsonArray jointNodes;
            std::vector<float> ibms((size_t)nBones * 16, 0.0f);
            for (int j = 0; j < nBones; ++j) {
                int nj = -1;
                float ibmRow[12];
                if (p.skel) {
                    const di::SkinBone& b = p.skel->bones[(size_t)j];
                    nj = nodeOf.value(QString::fromStdString(b.name), -1);
                    std::memcpy(ibmRow, b.invBind, sizeof(ibmRow));
                } else {
                    nj = nodeOf.value(
                        QStringLiteral("%1.bone_%2").arg(p.name).arg(j), -1);
                    identity43(ibmRow);
                    if (nj >= 0 && rig[nj].haveWorld)
                        for (int c = 0; c < 3; ++c)
                            ibmRow[9 + c] = -rig[nj].world[9 + c];
                }
                jointNodes.append(firstRig + (nj >= 0 ? nj : 0));
                rowAffineToGltfMat4(ibmRow, &ibms[(size_t)j * 16]);
            }
            const int aIbm =
                buf.addAccessor(ibms.data(), nBones, FLOAT_T, "MAT4", 16, 4);
            QJsonObject skin;
            skin["inverseBindMatrices"] = aIbm;
            skin["joints"]   = jointNodes;
            skin["skeleton"] = armatureRoot;
            skinsArr.append(skin);
            skinIndex = skinsArr.size() - 1;
        }

        // per-part material
        QJsonObject mat;
        mat["name"] = p.name + QStringLiteral("_mat");
        mat["doubleSided"] = true;
        QJsonObject pbr;
        pbr["metallicFactor"]  = 0.0;
        pbr["roughnessFactor"] = 0.9;
        const int tDiff = addTexture(p.textures.diffuse);
        const int tNrm  = addTexture(p.textures.normal);
        const int tEmi  = addTexture(p.textures.emissive);
        // Mix map (measured: R=roughness G=metallic B=AO) -> one glTF ORM
        // image: occlusion reads R, metallicRoughness reads G(rough)/B(metal).
        // Swizzle: outR=inB(AO) outG=inR(rough) outB=inG(metal).
        int tOrm = -1;
        if (!p.textures.mix.isNull()) {
            QImage orm = p.textures.mix.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < orm.height(); ++y) {
                uchar* line = orm.scanLine(y);
                for (int x = 0; x < orm.width(); ++x) {
                    uchar* px = line + x * 4;
                    const uchar r = px[0], g = px[1], b = px[2];
                    px[0] = b;
                    px[1] = r;
                    px[2] = g;
                    px[3] = 255;
                }
            }
            tOrm = addTexture(orm);
        }
        if (tDiff >= 0) pbr["baseColorTexture"] = QJsonObject{{"index", tDiff}};
        else            pbr["baseColorFactor"] = QJsonArray{0.8, 0.76, 0.7, 1.0};
        // Alpha cutout: when the diffuse carries a real (varying) alpha channel
        // — hair cards, lashes, cloth fringes — flag the glTF material MASK so
        // the transparency survives into Blender/other tools, mirroring the
        // viewport's Alpha toggle. Opaque diffuses (flat alpha) stay OPAQUE.
        if (tDiff >= 0 && !p.textures.diffuse.isNull()) {
            const QImage da =
                p.textures.diffuse.convertToFormat(QImage::Format_RGBA8888);
            int amin = 255, amax = 0;
            const int sy = std::max(1, da.height() / 64);
            const int sx = std::max(1, da.width() / 64);
            for (int y = 0; y < da.height(); y += sy) {
                const uchar* L = da.constScanLine(y);
                for (int x = 0; x < da.width(); x += sx) {
                    const int av = L[x * 4 + 3];
                    amin = std::min(amin, av);
                    amax = std::max(amax, av);
                }
            }
            if (amax - amin >= 8) {   // genuine transparency, not a flat channel
                mat["alphaMode"]   = QStringLiteral("MASK");
                mat["alphaCutoff"] = 0.5;
            }
        }
        if (tOrm >= 0) {
            pbr["metallicRoughnessTexture"] = QJsonObject{{"index", tOrm}};
            pbr["metallicFactor"]  = 1.0;
            pbr["roughnessFactor"] = 1.0;
            mat["occlusionTexture"] = QJsonObject{{"index", tOrm}};
        }
        if (tNrm >= 0) mat["normalTexture"] = QJsonObject{{"index", tNrm}};
        if (tEmi >= 0) {
            mat["emissiveTexture"] = QJsonObject{{"index", tEmi}};
            mat["emissiveFactor"]  = QJsonArray{1.0, 1.0, 1.0};
        }
        mat["pbrMetallicRoughness"] = pbr;
        materials.append(mat);

        meshes.append(QJsonObject{
            {"name", p.name},
            {"primitives",
             QJsonArray{QJsonObject{{"attributes", attrs},
                                    {"indices", aIdx},
                                    {"material", materials.size() - 1}}}}});
        QJsonObject meshNode;
        meshNode["name"] = p.name;
        meshNode["mesh"] = meshes.size() - 1;
        if (skinIndex >= 0) meshNode["skin"] = skinIndex;
        nodes.append(meshNode);
        sceneNodes.append(nodes.size() - 1);
    }

    // armature + rig nodes
    {
        QJsonArray rootChildren;
        std::vector<QJsonArray> childLists(rig.size());
        for (int j = 0; j < (int)rig.size(); ++j) {
            if (rig[j].parent < 0) rootChildren.append(firstRig + j);
            else                   childLists[(size_t)rig[j].parent].append(firstRig + j);
        }
        QJsonObject armature;
        armature["name"] = QStringLiteral("Armature");
        armature["children"] = rootChildren;
        nodes.append(armature);
        sceneNodes.append(armatureRoot);
        for (int j = 0; j < (int)rig.size(); ++j) {
            QJsonObject bn;
            bn["name"] = rig[j].name;
            float q[4];
            rowRotToQuat(rig[j].local, q);
            bn["translation"] = QJsonArray{(double)rig[j].local[9],
                                           (double)rig[j].local[10],
                                           (double)rig[j].local[11]};
            bn["rotation"] = QJsonArray{(double)q[0], (double)q[1], (double)q[2],
                                        (double)q[3]};
            if (!childLists[(size_t)j].isEmpty()) bn["children"] = childLists[(size_t)j];
            nodes.append(bn);
        }
    }

    // ---- 6. animations ---------------------------------------------------
    QJsonArray animsArr;
    for (const AnimExport* a : clips) {
        const di::AnimClip& c = *a->clip;
        QJsonArray samplers, channels;
        int skipped = 0;
        for (const di::AnimTrack& t : c.tracks) {
            const int nj = nodeOf.value(QString::fromLatin1(t.name.c_str()), -1);
            if (nj < 0) { ++skipped; continue; }
            const int target = firstRig + nj;
            if (!t.pos.empty()) {
                std::vector<float> times(t.pos.size()), vals(t.pos.size() * 3);
                for (size_t i = 0; i < t.pos.size(); ++i) {
                    times[i] = (float)t.pos[i].timeMs / 1000.0f;
                    for (int cc = 0; cc < 3; ++cc)
                        vals[i * 3 + cc] = t.pos[i].p[cc];
                }
                const float tmn = times.front(), tmx = times.back();
                const int aT = buf.addAccessor(times.data(), (int)times.size(),
                                               FLOAT_T, "SCALAR", 1, 4, 0, &tmn, &tmx);
                const int aV = buf.addAccessor(vals.data(), (int)times.size(),
                                               FLOAT_T, "VEC3", 3, 4);
                samplers.append(QJsonObject{{"input", aT},
                                            {"output", aV},
                                            {"interpolation", "LINEAR"}});
                channels.append(QJsonObject{
                    {"sampler", samplers.size() - 1},
                    {"target", QJsonObject{{"node", target},
                                           {"path", "translation"}}}});
            }
            if (!t.rot.empty()) {
                std::vector<float> times(t.rot.size()), vals(t.rot.size() * 4);
                for (size_t i = 0; i < t.rot.size(); ++i) {
                    times[i] = (float)t.rot[i].timeMs / 1000.0f;
                    for (int cc = 0; cc < 4; ++cc)
                        vals[i * 4 + cc] = t.rot[i].q[cc];
                }
                const float tmn = times.front(), tmx = times.back();
                const int aT = buf.addAccessor(times.data(), (int)times.size(),
                                               FLOAT_T, "SCALAR", 1, 4, 0, &tmn, &tmx);
                const int aV = buf.addAccessor(vals.data(), (int)times.size(),
                                               FLOAT_T, "VEC4", 4, 4);
                samplers.append(QJsonObject{{"input", aT},
                                            {"output", aV},
                                            {"interpolation", "LINEAR"}});
                channels.append(QJsonObject{
                    {"sampler", samplers.size() - 1},
                    {"target", QJsonObject{{"node", target},
                                           {"path", "rotation"}}}});
            }
        }
        if (channels.isEmpty()) {
            qWarning("glb: clip %s produced no channels — dropped",
                     qPrintable(a->name));
            continue;
        }
        if (skipped)
            qInfo("glb: clip %s: %d tracks had no rig node", qPrintable(a->name),
                  skipped);
        animsArr.append(QJsonObject{{"name", a->name},
                                    {"samplers", samplers},
                                    {"channels", channels}});
    }

    // ---- 7. assemble + write --------------------------------------------
    QJsonObject gltf;
    gltf["asset"] = QJsonObject{{"version", "2.0"},
                                {"generator", "DIAssetBrowser rigged glTF v2"}};
    gltf["scene"]  = 0;
    gltf["scenes"] = QJsonArray{QJsonObject{{"nodes", sceneNodes},
                                            {"name", sceneName}}};
    gltf["nodes"]     = nodes;
    gltf["meshes"]    = meshes;
    gltf["materials"] = materials;
    if (!skinsArr.isEmpty()) gltf["skins"] = skinsArr;
    if (!animsArr.isEmpty()) gltf["animations"] = animsArr;
    if (!images.isEmpty()) {
        gltf["images"]   = images;
        gltf["samplers"] = QJsonArray{QJsonObject{{"magFilter", 9729},
                                                  {"minFilter", 9987},
                                                  {"wrapS", 10497},
                                                  {"wrapT", 10497}}};
        gltf["textures"] = texturesArr;
    }
    gltf["accessors"]   = buf.accessors;
    gltf["bufferViews"] = buf.views;

    buf.pad4();
    gltf["buffers"] = QJsonArray{QJsonObject{{"byteLength", buf.blob.size()}}};

    QByteArray json = QJsonDocument(gltf).toJson(QJsonDocument::Compact);
    while (json.size() % 4) json.append(' ');
    QByteArray bin = buf.blob;
    while (bin.size() % 4) bin.append('\0');

    // QSaveFile, not QFile: a .glb is written to a temporary next to the target
    // and renamed into place only on commit(). A disk-full or a dropped network
    // share therefore leaves the PREVIOUS export intact instead of replacing it
    // with a truncated file that still reports success.
    QSaveFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("cannot open %1 for write").arg(outPath);
        return false;
    }
    bool wrote = true;
    const auto writeU32 = [&f, &wrote](quint32 v) {
        wrote = wrote && f.write(reinterpret_cast<const char*>(&v), 4) == 4;
    };
    writeU32(0x46546C67);                                   // 'glTF'
    writeU32(2);
    writeU32(quint32(12 + 8 + json.size() + 8 + bin.size()));
    writeU32(quint32(json.size()));
    writeU32(0x4E4F534A);                                   // 'JSON'
    wrote = wrote && f.write(json) == json.size();
    writeU32(quint32(bin.size()));
    writeU32(0x004E4942);                                   // 'BIN'
    wrote = wrote && f.write(bin) == bin.size();
    if (!wrote || !f.commit()) {
        // commit() rolls the temporary back, so nothing was overwritten.
        if (err)
            *err = QStringLiteral("write failed for %1 (%2)")
                       .arg(outPath,
                            f.errorString().isEmpty()
                                ? QStringLiteral("disk full or path unwritable")
                                : f.errorString());
        return false;
    }
    qInfo("glb: wrote %s — %d parts, %zu rig nodes, %d anims, %lld bytes",
          qPrintable(outPath), nParts, rig.size(), (int)animsArr.size(),
          (long long)(12 + 8 + json.size() + 8 + bin.size()));
    return true;
}

bool writeGlb(const QString& outPath, const di::MeshData& mesh,
              const MeshTextures& textures, const di::SkinSkeleton* skel,
              const di::BoneParents* hierarchy,
              const QString& meshName, QString* err)
{
    Part p;
    // non-owning aliases: the caller keeps mesh/skel alive across this call
    p.mesh = std::shared_ptr<const di::MeshData>(&mesh, [](const di::MeshData*) {});
    p.textures = textures;
    if (skel)
        p.skel = std::shared_ptr<const di::SkinSkeleton>(
            skel, [](const di::SkinSkeleton*) {});
    p.name = meshName;
    return writeGlb(outPath, {p}, hierarchy, nullptr, {}, meshName, err);
}

} // namespace GlbExporter
