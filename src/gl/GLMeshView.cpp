#include "gl/GLMeshView.h"

#include <QColor>
#include <QEnterEvent>
#include <QFont>
#include <QLineF>
#include <QMouseEvent>
#include <QOpenGLFramebufferObject>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRectF>
#include <QSettings>
#include <QDateTime>
#include <QTimer>
#include <QVector3D>
#include <QVector4D>
#include <QVector>
#include <QWheelEvent>
#include <QWidget>
#include <QtMath>

#include <algorithm>
#include <cstdlib>

#include "app/SehGuard.h"

namespace {

// Blender-style axis-orientation gizmo, ported 1:1 from D4AssetBrowser Native
// (GLModelWidget.cpp AxisGizmoOverlay): a small ball in the viewport's top-right
// that projects the world axes with the SAME yaw/pitch math paintGL uses.
// Clicking an axis end glides the camera to that axis view (centre + zoom kept);
// double-clicking the body toggles orthographic projection. A plain child widget
// — no GL state interplay, and it naturally receives the clicks so they never
// reach the orbit-drag handler. No Q_OBJECT (no signals/slots) → no moc needed.
class AxisGizmoOverlay : public QWidget {
public:
    explicit AxisGizmoOverlay(GLMeshView* host) : QWidget(host), m_host(host) {
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Orbit to an axis view (click a ball) · double-click: ortho"));
        setMouseTracking(true);
    }

protected:
    struct End { QPointF p; float toward; int axis; bool pos; int idx; };   // toward > 0 = faces the camera;
                                                                            // idx = canonical id (axis*2+neg)

    QVector<End> ends() const
    {
        // Camera basis — mirrors paintGL: dir = (cp·sy, sp, cp·cy), looking along
        // -dir with world-up (0,1,0).
        const float cp = std::cos(m_host->camPitch()), sp = std::sin(m_host->camPitch());
        const float cy = std::cos(m_host->camYaw()),   sy = std::sin(m_host->camYaw());
        const QVector3D dir(cp * sy, sp, cp * cy);
        const QVector3D f = -dir;                                        // camera forward
        QVector3D r = QVector3D::crossProduct(f, QVector3D(0, 1, 0));
        if (r.lengthSquared() < 1e-6f) r = QVector3D(1, 0, 0);           // looking straight up/down
        r.normalize();
        const QVector3D u = QVector3D::crossProduct(r, f);
        const QPointF c(width() / 2.0, height() / 2.0);
        const float R = qMin(width(), height()) / 2.0f - 9.0f;
        QVector<End> out;
        static const QVector3D ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int a = 0; a < 3; ++a)
            for (int s = 0; s < 2; ++s) {
                const QVector3D w = s == 0 ? ax[a] : -ax[a];
                out << End{c + QPointF(QVector3D::dotProduct(w, r) * R,
                                       -QVector3D::dotProduct(w, u) * R),
                           -QVector3D::dotProduct(w, f), a, s == 0, a * 2 + s};
            }
        return out;
    }

    // Nearest axis end within grab range, or -1. Canonical idx (axis*2+neg).
    int endAt(const QPointF& p) const
    {
        const QVector<End> e = ends();
        int best = -1; double bd = 1e9;
        for (const End& n : e) {
            const double d = QLineF(p, n.p).length();
            if (d < bd) { bd = d; best = n.idx; }
        }
        return bd <= 9.5 ? best : -1;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        // Blender: the gizmo is quiet until the cursor comes to it — semi-transparent
        // at rest, fully lit (with its backdrop disc) on hover.
        if (!m_hover) g.setOpacity(0.35);
        g.setPen(Qt::NoPen);
        if (m_hover) {
            g.setBrush(QColor(255, 255, 255, 26));   // backdrop disc only while engaged
            g.drawEllipse(rect().adjusted(1, 1, -1, -1));
        }
        static const QColor col[3] = {QColor(226, 84, 77),     // X — Blender red
                                      QColor(118, 183, 66),    // Y — Blender green
                                      QColor(74, 132, 222)};   // Z — Blender blue
        QVector<End> e = ends();
        std::sort(e.begin(), e.end(), [](const End& a, const End& b) { return a.toward < b.toward; });
        const QPointF c(width() / 2.0, height() / 2.0);
        for (const End& n : e) {
            const bool hot = m_hover && n.idx == m_hotEnd;   // the ball under the cursor
            if (n.pos) { g.setPen(QPen(col[n.axis], 1.6)); g.drawLine(c, n.p); }
            g.setPen(Qt::NoPen);
            if (n.pos) {
                const qreal rad = hot ? 8.0 : 6.5;
                g.setBrush(hot ? col[n.axis].lighter(125) : col[n.axis]);
                g.drawEllipse(n.p, rad, rad);
                if (hot) {   // white ring — unmistakably "this one"
                    g.setPen(QPen(QColor(255, 255, 255, 220), 1.3));
                    g.setBrush(Qt::NoBrush);
                    g.drawEllipse(n.p, rad, rad);
                    g.setPen(Qt::NoPen);
                }
                g.setPen(QColor(25, 25, 25));
                QFont f = g.font(); f.setPointSizeF(7.5); f.setBold(true); g.setFont(f);
                g.drawText(QRectF(n.p.x() - rad, n.p.y() - rad, rad * 2, rad * 2), Qt::AlignCenter,
                           QString(QLatin1Char('X' + n.axis)));
            } else if (hot) {   // hovered negative end: fills and labels itself (−X / −Y / −Z)
                g.setBrush(col[n.axis].lighter(115));
                g.drawEllipse(n.p, 7.5, 7.5);
                g.setPen(QPen(QColor(255, 255, 255, 220), 1.3));
                g.setBrush(Qt::NoBrush);
                g.drawEllipse(n.p, 7.5, 7.5);
                g.setPen(QColor(25, 25, 25));
                QFont f = g.font(); f.setPointSizeF(6.5); f.setBold(true); g.setFont(f);
                g.drawText(QRectF(n.p.x() - 7.5, n.p.y() - 7.5, 15, 15), Qt::AlignCenter,
                           QStringLiteral("−") + QString(QLatin1Char('X' + n.axis)));
            } else {   // negative end at rest: hollow, dimmer, no label (Blender's look)
                g.setBrush(col[n.axis].darker(230));
                g.drawEllipse(n.p, 5.0, 5.0);
                g.setPen(QPen(col[n.axis].darker(140), 1.2));
                g.setBrush(Qt::NoBrush);
                g.drawEllipse(n.p, 5.0, 5.0);
            }
        }
    }

    void enterEvent(QEnterEvent*) override { m_hover = true;  update(); }
    void leaveEvent(QEvent*) override      { m_hover = false; m_hotEnd = -1; update(); }

    void mouseMoveEvent(QMouseEvent* ev) override
    {
        const int hot = endAt(ev->position());
        if (hot != m_hotEnd) { m_hotEnd = hot; update(); }
    }

    void mousePressEvent(QMouseEvent* ev) override
    {
        const QPointF p = ev->position();
        const QVector<End> e = ends();
        int best = -1; double bd = 1e9;
        for (int i = 0; i < e.size(); ++i) {
            const double d = QLineF(p, e[i].p).length();
            if (d < bd) { bd = d; best = i; }
        }
        if (best < 0 || bd > 9.5) return;   // not on a ball — swallow the click
        // Look FROM that axis end toward the centre: camera dir = the clicked axis.
        constexpr float kPi = 3.14159265f, kTop = 1.55f;
        float yaw = m_host->camYaw(), pitch = 0.0f;
        const bool pos = e[best].pos;
        switch (e[best].axis) {
        case 0: yaw = pos ? kPi / 2 : -kPi / 2; break;
        case 1: pitch = pos ? kTop : -kTop;     break;   // top/bottom keep the current yaw
        case 2: yaw = pos ? 0.0f : kPi;         break;
        }
        m_host->orbitToAxis(yaw, pitch);
        ev->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* ev) override
    {
        // Blender: the gizmo doubles as the ortho/perspective switch. Double-click
        // the ball's BODY (not an axis end — those orbit) to toggle projection.
        const QVector<End> e = ends();
        for (const End& n : e)
            if (QLineF(ev->position(), n.p).length() <= 9.5) { ev->accept(); return; }
        m_host->setOrthographic(!m_host->orthographic());
        QSettings().setValue(m_host->orthoSettingsKey(), m_host->orthographic());
        setToolTip(m_host->orthographic()
                       ? QStringLiteral("Orthographic — double-click for perspective")
                       : QStringLiteral("Orbit to an axis view (click a ball) · double-click: ortho"));
        ev->accept();
    }

private:
    GLMeshView* m_host;
    bool m_hover  = false;   // cursor is over the gizmo → full opacity + backdrop disc
    int  m_hotEnd = -1;      // canonical idx of the hovered axis ball, or -1
};

const char* kVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec4 aTan;   // xyz tangent, w bitangent handedness
layout(location=3) in vec2 aUv;
layout(location=4) in vec4 aJoints;
layout(location=5) in vec4 aWeights;
uniform mat4 uMvp;
uniform mat4 uMv;
uniform int  uSkinned;
uniform mat4 uBones[128];
out vec3 vViewPos;
out vec3 vViewNrm;
out vec4 vViewTan;
out vec2 vUv;
void main() {
    vec4 sp = vec4(aPos, 1.0);
    vec3 sn = aNrm;
    vec3 st = aTan.xyz;
    if (uSkinned == 1) {
        mat4 M = aWeights.x * uBones[int(aJoints.x)] +
                 aWeights.y * uBones[int(aJoints.y)] +
                 aWeights.z * uBones[int(aJoints.z)] +
                 aWeights.w * uBones[int(aJoints.w)];
        sp = M * sp;
        mat3 R = mat3(M);
        sn = R * sn;
        st = R * st;
    }
    gl_Position = uMvp * sp;
    vViewPos    = (uMv * sp).xyz;
    vViewNrm    = mat3(uMv) * sn;
    vViewTan    = vec4(mat3(uMv) * st, aTan.w);
    vUv         = aUv;
    gl_PointSize = 2.0;
}
)";

const char* kFrag = R"(#version 330 core
in vec3 vViewPos;
in vec3 vViewNrm;
in vec4 vViewTan;
in vec2 vUv;
uniform sampler2D uTex;
uniform sampler2D uNrm;
uniform sampler2D uMix;
uniform sampler2D uEmi;
uniform int  uTextured;
uniform int  uNormalMap;
uniform int  uMixMap;
uniform int  uEmissive;
uniform int  uShading;   // 0 points (none) · 1 flat derivative · 2 authored normals
uniform int  uViewChannel; // 0 Shaded · 1 BaseColor · 2 Normal · 3 Rough · 4 Metal · 5 AO · 6 Emissive
uniform int  uAlpha;     // 0 opaque · 1 use the diffuse alpha (cutout + blend)
uniform int  uPbr;       // 1 = Cook-Torrance studio rig · 0 = legacy headlight
uniform float uExposure; // pre-tonemap exposure (PBR path)
// Animated-emissive material FX (DI PBR_Monster constants — measured; see
// MeshTextures.h). All layers are masked by the emissive map, so a material
// without one renders exactly as before.
uniform vec3  uEmiColor;   // cEmmsionColor tint ((1,1,1) = neutral)
uniform vec4  uArcane;     // cArcaneColor.rgb, cArcaneIntensity (0 = off)
uniform vec4  uArcaneTf;   // cArcaneScrolling: tile.xy, scroll-rate.xy
uniform vec4  uStarA;      // _StarUV1.xy, _StarSpeed1.xy ((0,0) uv = off)
uniform vec4  uStarB;      // _StarUV2.xy, _StarSpeed2.xy
uniform vec4  uFresnelFx;  // cFresnelColor.rgb, cFresnelIntensity (0 = off)
uniform float uFresnelPow; // cFresnelRange
uniform float uTime;       // seconds (0 when nothing animates)

// The full emissive stack: base emissive x authored tint, plus the arcane
// scrolling layer (a second emissive sample flowing across the glow regions),
// plus the star sparkle (two independently-scrolled samples multiplied — the
// classic twinkle construction the _Star* constants describe). Everything is
// masked to the emissive map's own lit regions.
vec3 fxEmissive(vec2 uv) {
    vec3 em = texture(uEmi, uv).rgb;
    vec3 outc = em * uEmiColor;
    float mask = max(em.r, max(em.g, em.b));
    if (uArcane.a > 0.0001 && mask > 0.001) {
        vec3 arc = texture(uEmi, uv * uArcaneTf.xy + uArcaneTf.zw * uTime).rgb;
        outc += arc * uArcane.rgb * uArcane.a * mask;
    }
    if (uStarA.x != 0.0 && mask > 0.001) {
        float s1 = dot(texture(uEmi, uv * uStarA.xy + uStarA.zw * uTime).rgb,
                       vec3(0.333));
        float s2 = dot(texture(uEmi, uv * uStarB.xy + uStarB.zw * uTime).rgb,
                       vec3(0.333));
        outc += uArcane.rgb * (s1 * s2 * 2.0) * mask;
    }
    return outc;
}

const float PI = 3.14159265359;
float distGGX(float NoH, float a){ float a2=a*a; float d=NoH*NoH*(a2-1.0)+1.0; return a2/max(PI*d*d,1e-7); }
float gSchlick(float x, float k){ return x/(x*(1.0-k)+k); }
float gSmith(float NoV, float NoL, float r){ float k=(r+1.0); k=k*k/8.0; return gSchlick(NoV,k)*gSchlick(NoL,k); }
vec3 fres(float c, vec3 F0){ return F0 + (1.0-F0)*pow(clamp(1.0-c,0.0,1.0),5.0); }
// One directional light, energy-conserving Cook-Torrance (ported from the D4
// tool). N, V, Ldir in VIEW space; returns diffuse+specular for this light.
vec3 shadeLight(vec3 N, vec3 V, vec3 Ldir, vec3 lcol, vec3 albedo, vec3 F0,
                float rough, float metal){
    vec3 L = normalize(Ldir);
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NoV=max(dot(N,V),1e-4), NoH=max(dot(N,H),0.0), VoH=max(dot(V,H),0.0);
    float D = distGGX(NoH, rough*rough);
    float G = gSmith(NoV, NoL, rough);
    vec3  F = fres(VoH, F0);
    vec3 spec = (D*G*F) / max(4.0*NoV*NoL, 1e-4);
    vec3 kd = (vec3(1.0) - F) * (1.0 - clamp(metal, 0.0, 1.0));
    return (kd*albedo/PI + spec) * lcol * NoL;
}

out vec4 frag;
void main() {
    vec4 texel = uTextured == 1 ? texture(uTex, vUv) : vec4(0.62, 0.60, 0.57, 1.0);
    vec3 base = texel.rgb;
    float alpha = texel.a;
    vec3 col  = base;
    bool toneMapped = false;
    if (uShading == 1) {
        vec3 n = normalize(cross(dFdx(vViewPos), dFdy(vViewPos)));
        col = base * (0.25 + 0.75 * max(dot(n, normalize(-vViewPos)), 0.0));
    } else if (uShading == 2) {
        vec3 N = normalize(vViewNrm);
        if (uNormalMap == 1) {
            vec3 t = normalize(vViewTan.xyz - N * dot(N, vViewTan.xyz));
            vec3 b = cross(N, t) * vViewTan.w;
            vec3 nts = texture(uNrm, vUv).rgb * 2.0 - 1.0;
            N = normalize(t * nts.x + b * nts.y + N * nts.z);
        }
        vec3 V = normalize(-vViewPos);
        // Two-sided lighting: flip only genuine BACK faces (triangle winding),
        // never a front face whose normal-mapped detail happens to graze past
        // the viewer.
        //
        // This used to be "if (dot(N, V) < 0.0) N = -N;", which is view- AND
        // detail-dependent: at a glancing angle the perturbed normal of a
        // front-facing texel tips just past 90 degrees, gets flipped away from
        // every light, and the texel goes black. Whole edges darken as the
        // camera orbits, and the band moves with the camera — the giveaway that
        // the test was reading the view direction rather than the geometry.
        // gl_FrontFacing answers the question actually being asked ("is this the
        // back of the surface?") and is independent of both.
        //
        // Applied before BOTH shading paths, not just PBR. The legacy headlight
        // below keeps its abs(dot(N,V)) on purpose: it mirrors shading across
        // the silhouette rather than darkening, so it never showed this bug,
        // and swapping it for max(...,0) could introduce black faces on models
        // with unreliable authored normals — the opposite of the fix.
        if (!gl_FrontFacing) N = -N;
        if (uPbr == 1) {
            float rough = 0.5, metal = 0.0, ao = 1.0;
            if (uMixMap == 1) {   // measured: R=roughness G=metallic B=AO
                vec3 m = texture(uMix, vUv).rgb;
                rough = clamp(m.r, 0.05, 1.0); metal = m.g; ao = m.b;
            }
            vec3 albedo = pow(base, vec3(2.2));                 // sRGB -> linear
            vec3 F0 = mix(vec3(0.04), albedo, clamp(metal, 0.0, 1.0));
            // Studio 3-light rig in VIEW space, so the subject stays lit as the
            // camera orbits (key upper-left, warm fill lower-right, cool back rim).
            vec3 lit = vec3(0.0);
            lit += shadeLight(N, V, vec3(-0.4, 0.6, 0.7), vec3(1.00,0.98,0.94)*2.3, albedo, F0, rough, metal);
            lit += shadeLight(N, V, vec3( 0.7,-0.1, 0.5), vec3(0.42,0.47,0.58)*0.8, albedo, F0, rough, metal);
            lit += shadeLight(N, V, vec3( 0.1, 0.5,-0.9), vec3(0.68,0.76,0.95)*1.1, albedo, F0, rough, metal);
            // Hemisphere ambient (sky/ground) × AO.
            float hemi = 0.5 + 0.5 * N.y;
            vec3 amb = mix(vec3(0.15,0.14,0.13), vec3(0.34,0.37,0.42), hemi) * albedo;
            col = (lit + amb) * ao;
            if (uEmissive == 1) col += pow(fxEmissive(vUv), vec3(2.2));
            // Authored fresnel rim (cFresnel* — the red edge glow on FX shells).
            if (uFresnelFx.a > 0.0001)
                col += uFresnelFx.rgb * uFresnelFx.a *
                       pow(1.0 - max(dot(N, V), 0.0), max(uFresnelPow, 0.1));
            col *= uExposure;                                                     // exposure
            col = clamp((col*(2.51*col+0.03))/(col*(2.43*col+0.59)+0.14),0.0,1.0);// ACES filmic
            col = pow(col, vec3(1.0/2.2));                                        // linear -> sRGB
            toneMapped = true;
        } else {
            // Legacy headlight (fallback when PBR is off).
            float ndv = abs(dot(N, V));
            if (uMixMap == 1) {
                vec3 m = texture(uMix, vUv).rgb;
                float rough = clamp(m.r, 0.04, 1.0), metal = m.g, ao = m.b;
                float gloss = (1.0 - rough) * (1.0 - rough);
                float spec  = pow(ndv, mix(8.0, 96.0, gloss)) *
                              mix(0.08, 0.9, metal) * (0.3 + 0.7 * gloss);
                vec3 diff    = base * (1.0 - 0.6 * metal);
                vec3 specCol = mix(vec3(1.0), base, metal);
                col = (diff * (0.25 + 0.75 * ndv) + specCol * spec) * ao;
            } else {
                col = base * (0.25 + 0.75 * ndv);
            }
            if (uFresnelFx.a > 0.0001)
                col += uFresnelFx.rgb * uFresnelFx.a *
                       pow(1.0 - ndv, max(uFresnelPow, 0.1));
        }
    }
    if (uEmissive == 1 && !toneMapped)
        col += fxEmissive(vUv);
    if (uViewChannel > 0) {   // raw single-channel material inspection
        if (uViewChannel == 1)      col = base;
        else if (uViewChannel == 2) col = (uNormalMap == 1) ? texture(uNrm, vUv).rgb
                                                            : normalize(vViewNrm) * 0.5 + 0.5;
        else if (uViewChannel == 3) col = vec3(uMixMap == 1 ? texture(uMix, vUv).r : 0.5);
        else if (uViewChannel == 4) col = vec3(uMixMap == 1 ? texture(uMix, vUv).g : 0.0);
        else if (uViewChannel == 5) col = vec3(uMixMap == 1 ? texture(uMix, vUv).b : 1.0);
        else                        col = (uEmissive == 1) ? texture(uEmi, vUv).rgb : vec3(0.0);
        frag = vec4(col, 1.0);
        return;
    }
    if (uAlpha == 1) {
        // Alpha-tested cutout: drop the see-through fragments (hair cards,
        // lashes, cloth fringes, foliage) so they don't read as solid quads.
        // The surviving edge band still blends when GL_BLEND is on, softening
        // the cut. Depth stays authoritative — no per-triangle sort needed.
        if (alpha < 0.5) discard;
        frag = vec4(col, alpha);
    } else {
        frag = vec4(col, 1.0);
    }
}
)";

// Skeleton overlay: constant-colour lines/points, no lighting.
const char* kFlatVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uMvp;
void main() {
    gl_Position  = uMvp * vec4(aPos, 1.0);
    gl_PointSize = 6.0;
}
)";

const char* kFlatFrag = R"(#version 330 core
uniform vec4 uColor;
out vec4 frag;
void main() { frag = uColor; }
)";

} // namespace

GLMeshView::GLMeshView(QWidget* parent) : QOpenGLWidget(parent) {}

GLMeshView::~GLMeshView()
{
    makeCurrent();
    deleteParts();
    if (m_gridVbo) glDeleteBuffers(1, &m_gridVbo);
    if (m_gridVao) glDeleteVertexArrays(1, &m_gridVao);
    if (m_skelVbo) glDeleteBuffers(1, &m_skelVbo);
    if (m_skelVao) glDeleteVertexArrays(1, &m_skelVao);
    if (m_prog)     glDeleteProgram(m_prog);
    if (m_progFlat) glDeleteProgram(m_progFlat);
    doneCurrent();
}

// ── Material emissive glow (the "FX" checkbox) ─────────────────────────────
// The particle-effect feature (ParticleSystem port + bloom) was removed; see
// FX_ARCHIVE.md. What remains is the model material's own authored animated
// emissive layer (arcane scroll / star sparkle / fresnel rim), rendered by the
// main shader's fxEmissive(). This toggle just shows/hides it.
void GLMeshView::setFxVisible(bool on)
{
    if (m_fxVisible == on) return;
    m_fxVisible = on;
    updateFxTimer();
    update();
}

// The repaint clock runs only while a visible material's emissive stack
// animates (arcane scroll / star sparkle). Idle otherwise.
void GLMeshView::updateFxTimer()
{
    bool need = false;
    if (m_fxVisible)
        for (const ViewPart& p : m_srcParts)
            if (!p.textures.emissive.isNull() && p.textures.fx.animated()) {
                need = true;
                break;
            }
    if (need && !m_fxTimer) {
        m_fxTimer = new QTimer(this);
        m_fxTimer->setInterval(33);            // ~30 fps scroll animation
        connect(m_fxTimer, &QTimer::timeout, this,
                qOverload<>(&GLMeshView::update));
    }
    if (m_fxTimer) {
        if (need && !m_fxTimer->isActive()) m_fxTimer->start();
        if (!need && m_fxTimer->isActive()) m_fxTimer->stop();
    }
}

void GLMeshView::deletePart(GpuPart& p)
{
    if (p.vbo)  glDeleteBuffers(1, &p.vbo);
    if (p.ibo)  glDeleteBuffers(1, &p.ibo);
    if (p.texD) glDeleteTextures(1, &p.texD);
    if (p.texN) glDeleteTextures(1, &p.texN);
    if (p.texM) glDeleteTextures(1, &p.texM);
    if (p.texE) glDeleteTextures(1, &p.texE);
    if (p.vao)  glDeleteVertexArrays(1, &p.vao);
    p = GpuPart();
}

void GLMeshView::deleteParts()
{
    for (GpuPart& p : m_parts)
        deletePart(p);
    m_parts.clear();
}

void GLMeshView::setParts(std::vector<ViewPart> parts)
{
    m_srcParts = std::move(parts);
    m_poses.assign(m_srcParts.size(), {});
    m_submeshMask.assign(m_srcParts.size(), {});
    m_dirtyParts.clear();
    m_hasPending = true;
    m_skelSegs.clear();      // stale bones over new geometry would mislead;
    m_skelJoints.clear();    // callers re-send after setParts
    m_skelDirty = true;
    updateFxTimer();         // new materials may (not) animate
    update();
}

void GLMeshView::setPart(size_t index, ViewPart part)
{
    if (index >= m_srcParts.size()) {
        m_srcParts.resize(index + 1);
        m_poses.resize(index + 1);
        m_submeshMask.resize(index + 1);
        m_hasPending = true;   // structural change: full rebuild keeps
                               // m_parts aligned with m_srcParts
    }
    m_srcParts[index] = std::move(part);
    m_poses[index].clear();
    m_submeshMask[index].clear();
    if (!m_hasPending)
        m_dirtyParts.push_back(index);
    updateFxTimer();         // the new piece's material may (not) animate
    update();
}

void GLMeshView::setMesh(std::shared_ptr<di::MeshData> mesh, MeshTextures textures)
{
    std::vector<ViewPart> p;
    if (mesh) p.push_back({std::move(mesh), std::move(textures)});
    setParts(std::move(p));
}

void GLMeshView::clearMesh()
{
    setParts({});
}

void GLMeshView::setWireframe(bool on)
{
    m_wireframe = on;
    update();
}

void GLMeshView::setShadingMode(int mode)
{
    m_shadeMode = mode;
    m_wireframe = (mode == 0);   // ball 0 is the wireframe sphere
    update();
}

void GLMeshView::setYaw(float yaw)
{
    m_yaw = yaw;
    update();
}

void GLMeshView::setPartSubmeshMask(size_t part, std::vector<uint8_t> visible)
{
    if (part >= m_srcParts.size()) return;
    if (part >= m_submeshMask.size()) m_submeshMask.resize(m_srcParts.size());
    m_submeshMask[part] = std::move(visible);
    update();
}

void GLMeshView::setShowGrid(bool on)
{
    m_showGrid = on;
    update();
}

void GLMeshView::setBackfaceCull(bool on)
{
    m_backfaceCull = on;
    update();
}

void GLMeshView::setAlpha(bool on)
{
    m_alpha = on;
    update();
}

void GLMeshView::setBackground(const QColor& c)
{
    m_bg = c;
    update();
}

void GLMeshView::setTurntable(bool on)
{
    if (on) {
        if (!m_spinTimer) {
            m_spinTimer = new QTimer(this);
            m_spinTimer->setInterval(33);
            connect(m_spinTimer, &QTimer::timeout, this, [this] {
                m_yaw += qDegreesToRadians(m_spinDegPerSec) * 0.033f;
                update();
            });
        }
        m_spinTimer->start();
    } else if (m_spinTimer) {
        m_spinTimer->stop();
    }
}

void GLMeshView::setTurntableSpeed(float degPerSec)
{
    m_spinDegPerSec = degPerSec;
}

void GLMeshView::reframe()
{
    frameMesh();
    m_framedOnce = true;   // a manual frame counts, so auto-off keeps this view
    update();
    Q_EMIT cameraChanged();
}

void GLMeshView::setSkeleton(std::vector<float> segments, std::vector<float> joints)
{
    m_skelSegs   = std::move(segments);
    m_skelJoints = std::move(joints);
    m_skelDirty  = true;
    update();
}

void GLMeshView::setShowSkeleton(bool on)
{
    m_showSkel = on;
    update();
}

void GLMeshView::setPartPose(size_t part, std::vector<float> skinMats)
{
    if (part >= m_poses.size()) return;
    if (skinMats.size() > (size_t)kMaxBones * 16) {
        if (!m_poseWarned) {
            qWarning("GLMeshView: pose has %zu bones, clamped to %d",
                     skinMats.size() / 16, kMaxBones);
            m_poseWarned = true;
        }
        skinMats.resize((size_t)kMaxBones * 16);
    }
    m_poses[part] = std::move(skinMats);
    update();
}

void GLMeshView::clearPose()
{
    for (auto& p : m_poses) p.clear();
    update();
}

void GLMeshView::initializeGL()
{
    // Context (re)creation: any previously-held GPU ids belong to a dead
    // context. Zero them and re-arm the upload from the retained CPU copies,
    // or the view wedges blank after a driver reset / screen migration.
    for (GpuPart& p : m_parts) p = GpuPart();
    m_parts.clear();
    m_prog = m_progFlat = m_skelVao = m_skelVbo = 0;
    m_gridVao = m_gridVbo = 0;
    m_gridVerts = m_gridAxisVerts = 0;
    m_skelSegVerts = m_skelJointVerts = 0;
    m_hasPending = !m_srcParts.empty();
    m_skelDirty  = true;
    m_gridDirty  = true;

    // Orientation gizmo (a plain child widget, unaffected by GL context loss) —
    // create once, keep across re-inits. Restore the persisted ortho state here.
    if (!m_gizmo) {
        // The persisted projection is NOT restored here. It used to be, from a
        // hard-coded "models/ortho" — and since initializeGL runs on the first
        // paint, i.e. AFTER the tab constructor, it silently overwrote whatever
        // CameraPanel::applyStartupState had just set. The viewport then drew
        // perspective while the panel's checkbox read "Orthographic", and
        // unticking did nothing because setOrthographic saw no change. The
        // camera panel owns startup state; this only builds the gizmo.
        m_gizmo  = new AxisGizmoOverlay(this);
        positionGizmo();
        m_gizmo->show();
        m_gizmo->raise();
    }

    m_glOk = initializeOpenGLFunctions();
    if (!m_glOk) {
        qWarning("GLMeshView: OpenGL 3.3 core functions unavailable - viewport disabled");
        return;   // paintGL checks m_glOk before touching any GL entry point
    }
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_PROGRAM_POINT_SIZE);

    auto buildProgram = [&](const char* vsSrc, const char* fsSrc) -> GLuint {
        auto compile = [&](GLenum type, const char* src) -> GLuint {
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &src, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[1024];
                glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
                qWarning("GLMeshView: shader compile failed: %s", log);
            }
            return sh;
        };
        GLuint prog = glCreateProgram();
        const GLuint vs = compile(GL_VERTEX_SHADER, vsSrc);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc);
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            qWarning("GLMeshView: program link failed: %s", log);
        }
        glDeleteShader(vs);
        glDeleteShader(fs);
        return prog;
    };
    m_prog     = buildProgram(kVert, kFrag);
    m_progFlat = buildProgram(kFlatVert, kFlatFrag);
    glGenVertexArrays(1, &m_skelVao);
    glGenVertexArrays(1, &m_gridVao);
}

// Builds m_parts[i] from m_srcParts[i] IN PLACE — every GL id lands in the
// vector the moment it exists, so a mid-upload crash cannot leak resources.
void GLMeshView::uploadOnePart(size_t i)
{
    deletePart(m_parts[i]);
    bool ok = seh::runGuarded("gl-upload", [&] {
        ViewPart& src = m_srcParts[i];
        GpuPart& gp = m_parts[i];
        {
                const std::shared_ptr<di::MeshData>& mesh = src.mesh;
                if (!mesh || mesh->positions.empty())
                    return;
                const size_t nv = mesh->vertexCount();
                // interleave pos(3f)+normal(3f)+tangent(4f)+uv(2f)+joints(4f)+
                // weights(4f), stride 80
                std::vector<float> vb(nv * 20, 0.0f);
                const bool hasUv  = mesh->uv0.size() >= nv * 2;
                const bool hasNrm = mesh->normals.size() >= nv * 3;
                const bool hasTan = mesh->tangents.size() >= nv * 4;
                const bool hasSkin = mesh->boneIndices.size() >= nv * 4 &&
                                     mesh->boneWeights.size() >= nv * 4;
                gp.hasNormals = hasNrm;
                for (size_t v = 0; v < nv; ++v) {
                    vb[v * 20]     = mesh->positions[v * 3];
                    vb[v * 20 + 1] = mesh->positions[v * 3 + 1];
                    vb[v * 20 + 2] = mesh->positions[v * 3 + 2];
                    if (hasNrm) {
                        vb[v * 20 + 3] = mesh->normals[v * 3];
                        vb[v * 20 + 4] = mesh->normals[v * 3 + 1];
                        vb[v * 20 + 5] = mesh->normals[v * 3 + 2];
                    }
                    if (hasTan) {
                        vb[v * 20 + 6] = mesh->tangents[v * 4];
                        vb[v * 20 + 7] = mesh->tangents[v * 4 + 1];
                        vb[v * 20 + 8] = mesh->tangents[v * 4 + 2];
                        vb[v * 20 + 9] = mesh->tangents[v * 4 + 3];
                    }
                    if (hasUv) {
                        vb[v * 20 + 10] = mesh->uv0[v * 2];
                        vb[v * 20 + 11] = mesh->uv0[v * 2 + 1];
                    }
                    if (hasSkin) {
                        for (int k = 0; k < 4; ++k) {
                            const uint8_t bi = mesh->boneIndices[v * 4 + k];
                            vb[v * 20 + 12 + k] = (float)bi;
                            vb[v * 20 + 16 + k] = mesh->boneWeights[v * 4 + k];
                            if (mesh->boneWeights[v * 4 + k] > 0.0f &&
                                (int)bi > gp.maxBoneIdx)
                                gp.maxBoneIdx = bi;
                        }
                    }
                }
                glGenVertexArrays(1, &gp.vao);
                glBindVertexArray(gp.vao);
                glGenBuffers(1, &gp.vbo);
                glBindBuffer(GL_ARRAY_BUFFER, gp.vbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vb.size() * 4), vb.data(),
                             GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 80, (void*)0);
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 80, (void*)12);
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 80, (void*)24);
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 80, (void*)40);
                glEnableVertexAttribArray(4);
                glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 80, (void*)48);
                glEnableVertexAttribArray(5);
                glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, 80, (void*)64);

                if (!mesh->indices.empty()) {
                    glGenBuffers(1, &gp.ibo);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gp.ibo);
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                                 (GLsizeiptr)(mesh->indices.size() * 4),
                                 mesh->indices.data(), GL_STATIC_DRAW);
                    gp.indexCount = (int)mesh->indices.size();
                }
                gp.vertexCount = (int)nv;
                glBindVertexArray(0);

                auto uploadTex = [&](const QImage& src2, unsigned* id) {
                    const QImage img = src2.convertToFormat(QImage::Format_RGBA8888);
                    glGenTextures(1, id);
                    glBindTexture(GL_TEXTURE_2D, *id);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(),
                                 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
                    glGenerateMipmap(GL_TEXTURE_2D);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                    GL_LINEAR_MIPMAP_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                };
                if (hasUv && !src.textures.diffuse.isNull()) {
                    uploadTex(src.textures.diffuse, &gp.texD);
                    gp.textured = true;
                    // Diagnostic (opt-in via DI_DUMP_MAT): does the diffuse carry
                    // a usable opacity (alpha) channel? Off by default so it never
                    // bloats the log; set the env var when investigating.
                    if (std::getenv("DI_DUMP_MAT")) {
                    const QImage a =
                        src.textures.diffuse.convertToFormat(QImage::Format_RGBA8888);
                    int amin = 255, amax = 0;
                    const int sy = std::max(1, a.height() / 64);
                    const int sx = std::max(1, a.width() / 64);
                    for (int y = 0; y < a.height(); y += sy) {
                        const uchar* L = a.constScanLine(y);
                        for (int x = 0; x < a.width(); x += sx) {
                            const int av = L[x * 4 + 3];
                            amin = std::min(amin, av);
                            amax = std::max(amax, av);
                        }
                    }
                    qInfo("GLpart %zu diffuse alpha range [%d..%d]%s", i, amin, amax,
                          (amax - amin < 8) ? " (flat/opaque - no cutout in diffuse)"
                                            : " (has transparency)");
                    }   // DI_DUMP_MAT
                }
                if (hasTan && hasUv && !src.textures.normal.isNull()) {
                    uploadTex(src.textures.normal, &gp.texN);
                    gp.hasNrmMap = true;
                }
                if (hasUv && !src.textures.mix.isNull()) {
                    uploadTex(src.textures.mix, &gp.texM);
                    gp.hasMix = true;
                }
                if (hasUv && !src.textures.emissive.isNull()) {
                    uploadTex(src.textures.emissive, &gp.texE);
                    gp.hasEmi = true;
                }
                // trim CPU copies not needed for context-loss re-upload
                if (!gp.textured)  src.textures.diffuse  = QImage();
                if (!gp.hasNrmMap) src.textures.normal   = QImage();
                if (!gp.hasMix)    src.textures.mix      = QImage();
                if (!gp.hasEmi)    src.textures.emissive = QImage();
        }
    });
    if (!ok) {
        qWarning("GLMeshView: part %zu upload crashed; part cleared", i);
        deletePart(m_parts[i]);
    }
}

void GLMeshView::uploadPending()
{
    if (m_hasPending) {
        m_hasPending = false;
        m_dirtyParts.clear();
        deleteParts();
        m_parts.resize(m_srcParts.size());
        for (size_t i = 0; i < m_srcParts.size(); ++i)
            uploadOnePart(i);
        if (m_autoFrame || !m_framedOnce) { frameMesh(); m_framedOnce = true; }
    } else if (!m_dirtyParts.empty()) {
        for (size_t i : m_dirtyParts)
            if (i < m_parts.size())
                uploadOnePart(i);
        m_dirtyParts.clear();
        if (m_autoFrame || !m_framedOnce) { frameMesh(); m_framedOnce = true; }
    }

    // Skeleton re-upload only when it will actually be drawn — animTick sends
    // fresh segments at 30 Hz, and a delete+create+upload per frame for an
    // invisible overlay is pure churn. m_skelDirty stays set while hidden, so
    // enabling the overlay uploads the latest data on the next paint.
    if (m_skelDirty && m_showSkel) {
        m_skelDirty = false;
        if (m_skelVbo) { glDeleteBuffers(1, &m_skelVbo); m_skelVbo = 0; }
        m_skelSegVerts   = (int)(m_skelSegs.size() / 3);
        m_skelJointVerts = (int)(m_skelJoints.size() / 3);
        if (m_skelSegVerts + m_skelJointVerts > 0) {
            std::vector<float> all;
            all.reserve(m_skelSegs.size() + m_skelJoints.size());
            all.insert(all.end(), m_skelSegs.begin(), m_skelSegs.end());
            all.insert(all.end(), m_skelJoints.begin(), m_skelJoints.end());
            glBindVertexArray(m_skelVao);
            glGenBuffers(1, &m_skelVbo);
            glBindBuffer(GL_ARRAY_BUFFER, m_skelVbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(all.size() * 4), all.data(),
                         GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void*)0);
            glBindVertexArray(0);
        }
    }

    if (m_gridDirty) {
        m_gridDirty = false;
        if (m_gridVbo) { glDeleteBuffers(1, &m_gridVbo); m_gridVbo = 0; }
        m_gridVerts = m_gridAxisVerts = 0;
        // 10x10 cells around the model centre on its base plane; cell size
        // scales with the model so the grid always reads as "the floor".
        const float step = std::max(0.05f, m_radius * 0.4f);
        const float ext  = step * 5.0f;
        const float cx = m_center.x(), cz = m_center.z(), y = m_gridY;
        std::vector<float> v;
        for (int i = -5; i <= 5; ++i) {
            if (i == 0) continue;   // centre lines drawn as tinted axes below
            const float o = i * step;
            v.insert(v.end(), {cx - ext, y, cz + o, cx + ext, y, cz + o});
            v.insert(v.end(), {cx + o, y, cz - ext, cx + o, y, cz + ext});
        }
        m_gridVerts = (int)(v.size() / 3);
        // centre lines last, drawn in a second colour (X/Z world axes)
        v.insert(v.end(), {cx - ext, y, cz, cx + ext, y, cz});
        v.insert(v.end(), {cx, y, cz - ext, cx, y, cz + ext});
        m_gridAxisVerts = (int)(v.size() / 3) - m_gridVerts;
        glBindVertexArray(m_gridVao);
        glGenBuffers(1, &m_gridVbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * 4), v.data(),
                     GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void*)0);
        glBindVertexArray(0);
    }
}

void GLMeshView::frameMesh()
{
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    bool any = false;
    for (const ViewPart& part : m_srcParts) {
        if (!part.mesh) continue;
        const size_t nv = part.mesh->vertexCount();
        if (!nv) continue;
        any = true;
        for (size_t v = 0; v < nv; ++v)
            for (int c = 0; c < 3; ++c) {
                const float x = part.mesh->positions[v * 3 + c];
                mn[c] = std::min(mn[c], x);
                mx[c] = std::max(mx[c], x);
            }
    }
    if (!any) return;
    m_center = QVector3D((mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f,
                         (mn[2] + mx[2]) * 0.5f);
    const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    m_radius = std::max(0.05f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    m_dist   = m_radius * 2.6f;
    m_gridY  = mn[1];       // ground grid sits at the model's base
    m_gridDirty = true;
}

// ── Camera (D4 Camera-panel parity) ────────────────────────────────────────

void GLMeshView::setFov(float deg)
{
    const float f = std::clamp(deg, 10.0f, 100.0f);
    if (qFuzzyCompare(f, m_fov)) return;
    m_fov = f;
    update();
    Q_EMIT cameraChanged();
}

void GLMeshView::setOrbitAngles(float yawRad, float pitchRad)
{
    m_yaw   = yawRad;
    m_pitch = std::clamp(pitchRad, -1.55f, 1.55f);   // same guard as the drag
    update();
    Q_EMIT cameraChanged();
}

GLMeshView::CamState GLMeshView::cameraState() const
{
    CamState s;
    s.yaw = m_yaw;  s.pitch = m_pitch;  s.dist = m_dist;  s.fov = m_fov;
    s.cx = m_center.x();  s.cy = m_center.y();  s.cz = m_center.z();
    s.ortho = m_ortho;
    s.valid = true;
    return s;
}

void GLMeshView::setCameraState(const CamState& s)
{
    if (!s.valid) return;
    m_yaw   = s.yaw;
    m_pitch = std::clamp(s.pitch, -1.55f, 1.55f);
    m_dist  = std::max(0.001f, s.dist);
    m_fov   = std::clamp(s.fov, 10.0f, 100.0f);
    m_center = QVector3D(s.cx, s.cy, s.cz);
    m_ortho = s.ortho;
    // A restored camera is the user's framing — never let an auto-frame on the
    // next load throw it away.
    m_framedOnce = true;
    update();
    Q_EMIT cameraChanged();
}

// ── Capture ────────────────────────────────────────────────────────────────

// Capture mode freezes every wall-clock-driven source of change so that N calls
// to grabFramebuffer() produce N frames that differ ONLY by what the caller
// stepped. Here that is the emissive shader clock (see paintGL) and the
// turntable timer, which would otherwise keep rewriting the yaw between the
// exporter's own setYaw calls.
void GLMeshView::setCaptureMode(bool on)
{
    if (m_capturing == on) return;
    m_capturing = on;
    if (on) {
        m_spinWasActive = m_spinTimer && m_spinTimer->isActive();
        if (m_spinTimer) m_spinTimer->stop();
    } else {
        m_captureTime = 0.0f;
        if (m_spinWasActive && m_spinTimer) m_spinTimer->start();
        m_spinWasActive = false;
        update();
    }
}

QImage GLMeshView::grabSupersampled(int factor)
{
    if (!m_glOk || factor < 2) return {};
    makeCurrent();
    const qreal dpr = devicePixelRatioF();
    const int baseW = std::max(1, int(width() * dpr));
    const int baseH = std::max(1, int(height() * dpr));

    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    if (maxTex <= 0) maxTex = 8192;
    while (factor > 1 &&
           (baseW * factor > maxTex || baseH * factor > maxTex))
        --factor;

    QImage out;
    // The driver may still refuse the allocation; step the factor down until one
    // is accepted rather than failing the whole export.
    for (; factor >= 2; --factor) {
        const int W = baseW * factor, H = baseH * factor;
        QOpenGLFramebufferObjectFormat fmt;
        fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        QOpenGLFramebufferObject fbo(W, H, fmt);
        if (!fbo.isValid()) continue;
        m_captureFbo = fbo.handle();
        m_fbW = W;  m_fbH = H;
        fbo.bind();
        glViewport(0, 0, W, H);
        paintGL();                 // aspect comes from width()/height(), unchanged
        glFinish();
        glGetError();              // swallow anything the pass left latched
        fbo.release();
        out = fbo.toImage();
        m_captureFbo = 0;
        m_fbW = m_fbH = 0;
        glViewport(0, 0, baseW, baseH);
        break;
    }
    doneCurrent();
    update();
    if (!out.isNull()) out.setDevicePixelRatio(1.0);
    return out;
}

void GLMeshView::frameToSubmesh(size_t part, size_t submesh)
{
    if (part >= m_srcParts.size()) return;
    const std::shared_ptr<di::MeshData>& mesh = m_srcParts[part].mesh;
    if (!mesh || submesh >= mesh->submeshes.size()) return;
    const di::SubMeshRange& sr = mesh->submeshes[submesh];
    const size_t nv = mesh->vertexCount();
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    bool any = false;
    auto acc = [&](size_t v) {
        if (v >= nv) return;
        for (int c = 0; c < 3; ++c) {
            const float x = mesh->positions[v * 3 + c];
            mn[c] = std::min(mn[c], x);
            mx[c] = std::max(mx[c], x);
        }
        any = true;
    };
    // Prefer the submesh's index range (exact triangles); fall back to its
    // declared vertex span for point-cloud meshes with no index buffer.
    if (!mesh->indices.empty() && sr.indexCount > 0) {
        const size_t end =
            std::min((size_t)sr.startIndex + sr.indexCount, mesh->indices.size());
        for (size_t k = sr.startIndex; k < end; ++k)
            acc(mesh->indices[k]);
    } else if (sr.vertCount > 0) {
        const size_t end = std::min((size_t)sr.startVert + sr.vertCount, nv);
        for (size_t v = sr.startVert; v < end; ++v)
            acc(v);
    }
    if (!any) return;
    m_center = QVector3D((mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f,
                         (mn[2] + mx[2]) * 0.5f);
    const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    m_radius = std::max(0.05f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    m_dist   = m_radius * 2.6f;
    m_framedOnce = true;
    update();
}

void GLMeshView::paintGL()
{
    if (!m_glOk) return;   // 3.3 init failed: never call unresolved GL pointers
    uploadPending();
    // Alpha 0 for a transparent grab (background suppressed) and for a coverage
    // grab (background still drawn, but alpha carries model coverage so a crop
    // can locate the subject in one render).
    glClearColor(m_bg.redF(), m_bg.greenF(), m_bg.blueF(),
                 (m_transparentClear || m_coverageAlpha) ? 0.0f : 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // Depth state must be re-established every frame, not just in initializeGL:
    // the QPainter orientation-gizmo overlay at the end of paintGL disables
    // GL_DEPTH_TEST (and can leave depth writes off) when its paint engine tears
    // down, and never restores it. Relying on the one-time init enable therefore
    // makes every frame AFTER the first render depth-less — the model looks fine
    // until the first camera move repaints it, then turns "see-through" (front
    // and back faces both drawn, no occlusion). Backface culling can't mask it
    // because a closed mesh still shows its far interior faces. (Regression fix.)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    if (!m_prog || m_parts.empty()) return;
    if (m_backfaceCull) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }

    QMatrix4x4 proj;
    const float aspect =
        width() > 0 ? float(width()) / float(height() ? height() : 1) : 1.0f;
    const float nearP = m_radius * 0.01f, farP = m_radius * 40.0f + m_dist;
    if (m_ortho) {
        // tan(fov/2) — the ortho box tracks the FOV so toggling projection
        // never jumps the framing.
        const float h = m_dist * std::tan(m_fov * 0.5f * 3.14159265f / 180.0f);
        proj.ortho(-h * aspect, h * aspect, -h, h, nearP, farP);
    } else {
        proj.perspective(m_fov, aspect, nearP, farP);
    }
    QMatrix4x4 view;
    const float cx = m_center.x() + m_dist * std::cos(m_pitch) * std::sin(m_yaw);
    const float cy = m_center.y() + m_dist * std::sin(m_pitch);
    const float cz = m_center.z() + m_dist * std::cos(m_pitch) * std::cos(m_yaw);
    view.lookAt(QVector3D(cx, cy, cz), m_center, QVector3D(0, 1, 0));

    glUseProgram(m_prog);
    const QMatrix4x4 mvp = proj * view;
    glUniformMatrix4fv(glGetUniformLocation(m_prog, "uMvp"), 1, GL_FALSE, mvp.constData());
    glUniformMatrix4fv(glGetUniformLocation(m_prog, "uMv"), 1, GL_FALSE, view.constData());
    glUniform1i(glGetUniformLocation(m_prog, "uTex"), 0);
    glUniform1i(glGetUniformLocation(m_prog, "uNrm"), 1);
    glUniform1i(glGetUniformLocation(m_prog, "uEmi"), 2);
    glUniform1i(glGetUniformLocation(m_prog, "uMix"), 3);
    // Alpha transparency (opt-in): cutout discard in the shader + standard
    // over-blend for the soft edge band. Depth write stays on so the opaque
    // core occludes correctly without a per-triangle sort.
    glUniform1i(glGetUniformLocation(m_prog, "uAlpha"),
                (m_alpha && !m_wireframe) ? 1 : 0);
    if (m_alpha && !m_wireframe) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    // Enhanced (PBR) lighting + exposure, read live from Settings so tuning
    // shows on the next repaint. Wireframe forces the flat legacy look.
    {
        QSettings s;
        // Shading balls drive the lighting: ball 3 (rendered) = PBR, others flat/legacy.
        const bool pbr = !m_wireframe && m_shadeMode >= 3;
        const float exposure =
            s.value(QStringLiteral("view/exposure"), 1.0).toFloat();
        glUniform1i(glGetUniformLocation(m_prog, "uPbr"), pbr ? 1 : 0);
        glUniform1f(glGetUniformLocation(m_prog, "uExposure"), exposure);
        glUniform1i(glGetUniformLocation(m_prog, "uViewChannel"),
                    m_wireframe ? 0 : m_viewChannel);
    }

    bool drewAnything = false;
    for (size_t i = 0; i < m_parts.size(); ++i) {
        const GpuPart& p = m_parts[i];
        if (!p.vbo || p.vertexCount == 0) continue;
        drewAnything = true;
        glUniform1i(glGetUniformLocation(m_prog, "uTextured"),
                    (p.textured && !m_wireframe) ? 1 : 0);
        glUniform1i(glGetUniformLocation(m_prog, "uNormalMap"),
                    (p.hasNrmMap && !m_wireframe) ? 1 : 0);
        glUniform1i(glGetUniformLocation(m_prog, "uMixMap"),
                    (p.hasMix && !m_wireframe) ? 1 : 0);
        glUniform1i(glGetUniformLocation(m_prog, "uEmissive"),
                    (p.hasEmi && !m_wireframe) ? 1 : 0);
        // Animated-emissive material FX (arcane / star layers). Gated by the
        // FX toggle; neutral values when off or unauthored → legacy output.
        {
            const MatFxParams& fx = i < m_srcParts.size()
                                        ? m_srcParts[i].textures.fx
                                        : MatFxParams{};
            const bool on = m_fxVisible && p.hasEmi;
            const float ei0 = on && fx.emissionColorSet ? fx.emissionColor[0] : 1.0f;
            const float ei1 = on && fx.emissionColorSet ? fx.emissionColor[1] : 1.0f;
            const float ei2 = on && fx.emissionColorSet ? fx.emissionColor[2] : 1.0f;
            glUniform3f(glGetUniformLocation(m_prog, "uEmiColor"), ei0, ei1, ei2);
            glUniform4f(glGetUniformLocation(m_prog, "uArcane"),
                        fx.arcaneColor[0], fx.arcaneColor[1], fx.arcaneColor[2],
                        on && fx.arcaneOn() ? fx.arcaneIntensity : 0.0f);
            glUniform4f(glGetUniformLocation(m_prog, "uArcaneTf"),
                        fx.arcaneScroll[0], fx.arcaneScroll[1],
                        fx.arcaneScroll[2], fx.arcaneScroll[3]);
            glUniform4f(glGetUniformLocation(m_prog, "uStarA"),
                        on && fx.starOn() ? fx.star1[0] : 0.0f, fx.star1[1],
                        fx.star1[2], fx.star1[3]);
            glUniform4f(glGetUniformLocation(m_prog, "uStarB"),
                        fx.star2[0], fx.star2[1], fx.star2[2], fx.star2[3]);
            // Fresnel is independent of the emissive map (the Gorefiend FX
            // shell has rim glow but no tEmissionMap) — gate on FX visibility.
            glUniform4f(glGetUniformLocation(m_prog, "uFresnelFx"),
                        fx.fresnelColor[0], fx.fresnelColor[1],
                        fx.fresnelColor[2],
                        m_fxVisible ? fx.fresnelIntensity : 0.0f);
            glUniform1f(glGetUniformLocation(m_prog, "uFresnelPow"),
                        fx.fresnelRange);
            if (m_fxT0 == 0) m_fxT0 = QDateTime::currentMSecsSinceEpoch();
            // While capturing, the emissive clock is driven by frame index, not
            // by the wall clock: an export must be reproducible and its last
            // frame must hand back to its first, which a real-time clock (whose
            // advance depends on how long the encode took) cannot do.
            const float tSec =
                m_capturing
                    ? m_captureTime
                    : float(QDateTime::currentMSecsSinceEpoch() - m_fxT0) / 1000.0f;
            glUniform1f(glGetUniformLocation(m_prog, "uTime"), tSec);
        }
        static const std::vector<float> kNoPose;
        const std::vector<float>& pose = i < m_poses.size() ? m_poses[i] : kNoPose;
        // A pose shorter than the mesh's bone range would make the shader read
        // ANOTHER part's leftover uBones — render such a part unposed instead.
        bool posed = !pose.empty();
        if (posed && (size_t)(p.maxBoneIdx + 1) * 16 > pose.size()) {
            if (!m_poseWarned) {
                qWarning("GLMeshView: part %zu pose has %zu bones but mesh "
                         "references bone %d - rendering unposed",
                         i, pose.size() / 16, p.maxBoneIdx);
                m_poseWarned = true;
            }
            posed = false;
        }
        glUniform1i(glGetUniformLocation(m_prog, "uSkinned"), posed ? 1 : 0);
        if (posed)
            glUniformMatrix4fv(glGetUniformLocation(m_prog, "uBones"),
                               (GLsizei)(pose.size() / 16), GL_FALSE, pose.data());

        if (p.texD) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, p.texD); }
        if (p.texN) { glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, p.texN); }
        if (p.texE) { glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, p.texE); }
        if (p.texM) { glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, p.texM); }
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(p.vao);
        // Submesh visibility (parts panel): with a mask present, draw only the
        // visible index ranges; otherwise one call for the whole part.
        const std::vector<uint8_t>* mask =
            i < m_submeshMask.size() && !m_submeshMask[i].empty()
                ? &m_submeshMask[i]
                : nullptr;
        const std::vector<di::SubMeshRange>* subsPtr =
            i < m_srcParts.size() && m_srcParts[i].mesh
                ? &m_srcParts[i].mesh->submeshes
                : nullptr;
        // An all-zero mask means "hide this whole part" — honoured even when the
        // mesh carries no submesh table (a one-piece cosmetic) and for the
        // point-cloud path, neither of which the per-range branch can express.
        const bool hideAll =
            mask && !mask->empty() &&
            std::none_of(mask->begin(), mask->end(),
                         [](uint8_t v) { return v != 0; });
        if (hideAll) {
            // nothing drawn for this part
        } else if (p.indexCount > 0) {
            glUniform1i(glGetUniformLocation(m_prog, "uShading"),
                        m_shadeMode == 1 ? 1 : (p.hasNormals ? 2 : 1));   // ball 1 = flat
            glPolygonMode(GL_FRONT_AND_BACK, m_wireframe ? GL_LINE : GL_FILL);
            if (mask && subsPtr && mask->size() == subsPtr->size() &&
                !subsPtr->empty()) {
                for (size_t s2 = 0; s2 < subsPtr->size(); ++s2) {
                    if (!(*mask)[s2]) continue;
                    const di::SubMeshRange& r = (*subsPtr)[s2];
                    if (r.indexCount == 0 ||
                        (size_t)r.startIndex + r.indexCount >
                            (size_t)p.indexCount)
                        continue;
                    glDrawElements(GL_TRIANGLES, (GLsizei)r.indexCount,
                                   GL_UNSIGNED_INT,
                                   (void*)((size_t)r.startIndex * 4));
                }
            } else {
                glDrawElements(GL_TRIANGLES, p.indexCount, GL_UNSIGNED_INT,
                               nullptr);
            }
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        } else {
            glUniform1i(glGetUniformLocation(m_prog, "uShading"), 0);
            glDrawArrays(GL_POINTS, 0, p.vertexCount);
        }
        glBindVertexArray(0);
    }
    (void)drewAnything;
    glDisable(GL_BLEND);       // overlays are opaque constant-colour lines
    glDisable(GL_CULL_FACE);   // overlays are lines; never cull them

    // Ground grid: depth-tested so the model sits ON it, drawn after parts.
    if (m_showGrid && m_progFlat && m_gridVbo && m_gridVerts > 0) {
        glUseProgram(m_progFlat);
        glUniformMatrix4fv(glGetUniformLocation(m_progFlat, "uMvp"), 1, GL_FALSE,
                           mvp.constData());
        glBindVertexArray(m_gridVao);
        glUniform4f(glGetUniformLocation(m_progFlat, "uColor"),
                    0.30f, 0.30f, 0.32f, 1.0f);
        glDrawArrays(GL_LINES, 0, m_gridVerts);
        if (m_gridAxisVerts > 0) {
            // world X/Z centre lines, tinted so orientation reads at a glance
            glUniform4f(glGetUniformLocation(m_progFlat, "uColor"),
                        0.55f, 0.30f, 0.30f, 1.0f);
            glDrawArrays(GL_LINES, m_gridVerts, 2);
            glUniform4f(glGetUniformLocation(m_progFlat, "uColor"),
                        0.30f, 0.38f, 0.58f, 1.0f);
            glDrawArrays(GL_LINES, m_gridVerts + 2, 2);
        }
        glBindVertexArray(0);
    }

    // Skeleton overlay: drawn last with depth testing off so bones inside the
    // mesh stay visible.
    if (m_showSkel && m_progFlat && m_skelVbo &&
        (m_skelSegVerts > 0 || m_skelJointVerts > 0)) {
        glDisable(GL_DEPTH_TEST);
        glUseProgram(m_progFlat);
        glUniformMatrix4fv(glGetUniformLocation(m_progFlat, "uMvp"), 1, GL_FALSE,
                           mvp.constData());
        glBindVertexArray(m_skelVao);
        if (m_skelSegVerts > 0) {
            glUniform4f(glGetUniformLocation(m_progFlat, "uColor"),
                        1.0f, 0.78f, 0.20f, 1.0f);
            glDrawArrays(GL_LINES, 0, m_skelSegVerts);
        }
        if (m_skelJointVerts > 0) {
            glUniform4f(glGetUniformLocation(m_progFlat, "uColor"),
                        1.0f, 0.42f, 0.10f, 1.0f);
            glDrawArrays(GL_POINTS, m_skelSegVerts, m_skelJointVerts);
        }
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }

    // The orientation gizmo is now a separate child widget (AxisGizmoOverlay,
    // ported from D4) painted on top — it repaints itself, so paintGL leaves it
    // alone. Keep the gizmo current with the camera every frame.
    if (m_gizmo) m_gizmo->update();
}

void GLMeshView::resizeGL(int, int) { positionGizmo(); }

// Pin the gizmo to the top-right corner (D4 geometry: 88×88, 8px inset).
void GLMeshView::positionGizmo()
{
    if (!m_gizmo) return;
    m_gizmo->setGeometry(width() - 96, 8, 88, 88);
    m_gizmo->raise();
}

// Snap the orbit to look down a world axis. DI's viewport has no camera
// animation path, so this sets the angles directly (D4 animates via
// frameRegion; the end state is identical).
void GLMeshView::orbitToAxis(float yawRad, float pitchRad)
{
    m_yaw   = yawRad;
    m_pitch = std::clamp(pitchRad, -1.55f, 1.55f);
    update();
    Q_EMIT cameraChanged();
}

void GLMeshView::mousePressEvent(QMouseEvent* ev)
{
    m_lastPos = ev->pos();
    // Pick up the invert-axis preferences at the start of each drag (cheap, and
    // means a Settings change applies without restarting).
    QSettings s;
    m_invOrbitX = s.value(QStringLiteral("view/invOrbitX"), false).toBool();
    m_invOrbitY = s.value(QStringLiteral("view/invOrbitY"), false).toBool();
    m_invPan    = s.value(QStringLiteral("view/invPan"),    false).toBool();
}

// Double-click a part in the viewport → focus it (D4's partFocused). Frame the
// picked submesh when "frame part on pick" is on, and notify the Models tab so it
// can select the matching PARTS row. Clicks that land on the gizmo child widget
// never reach here — that widget handles its own double-click (ortho toggle).
void GLMeshView::mouseDoubleClickEvent(QMouseEvent* ev)
{
    // One ray-cast serves both signals: part-aware listeners (the Wardrobe, whose
    // pieces are parts 1..N as much as part 0) get partPicked, while the Models
    // tab keeps the part-0-only submeshPicked its PARTS panel is indexed by.
    int part = -1;
    const int sub = pickAnyPart(ev->pos(), &part);
    if (sub >= 0 && part >= 0) {
        if (QSettings()
                .value(QStringLiteral("models/framePartOnPick"), false)
                .toBool())
            frameToSubmesh((size_t)part, (size_t)sub);
        emit partPicked(part, sub);
        if (part == 0) emit submeshPicked(sub);
    }
    ev->accept();
}

// Ray-pick the front-most visible submesh across a range of parts. Reconstructs
// the same projection/view paintGL uses, unprojects the pixel into a world ray
// and runs Möller–Trumbore against the CPU mesh triangles, grouped by submesh
// range. partTo is exclusive; *outPart (when given) receives the winning part.
//
// The range exists because the two tabs want different things: the Models tab
// picks within its single model (part 0, attachments excluded — its PARTS panel
// only lists the primary's submeshes), while the Wardrobe picks across every
// equipped piece.
int GLMeshView::pickIn(const QPoint& posPx, size_t partFrom, size_t partTo,
                       int* outPart) const
{
    if (outPart) *outPart = -1;
    partTo = std::min(partTo, m_srcParts.size());
    if (partFrom >= partTo || width() <= 0 || height() <= 0) return -1;

    const float aspect = float(width()) / float(height() ? height() : 1);
    const float nearP = m_radius * 0.01f, farP = m_radius * 40.0f + m_dist;
    QMatrix4x4 proj;
    if (m_ortho) {
        const float h = m_dist * std::tan(m_fov * 0.5f * 3.14159265f / 180.0f);
        proj.ortho(-h * aspect, h * aspect, -h, h, nearP, farP);
    } else {
        proj.perspective(m_fov, aspect, nearP, farP);
    }
    const float cx = m_center.x() + m_dist * std::cos(m_pitch) * std::sin(m_yaw);
    const float cy = m_center.y() + m_dist * std::sin(m_pitch);
    const float cz = m_center.z() + m_dist * std::cos(m_pitch) * std::cos(m_yaw);
    QMatrix4x4 view;
    view.lookAt(QVector3D(cx, cy, cz), m_center, QVector3D(0, 1, 0));
    bool ok = false;
    const QMatrix4x4 invVP = (proj * view).inverted(&ok);
    if (!ok) return -1;

    const float nx = 2.0f * float(posPx.x()) / float(width()) - 1.0f;
    const float ny = 1.0f - 2.0f * float(posPx.y()) / float(height());
    const QVector4D pn = invVP * QVector4D(nx, ny, -1.0f, 1.0f);
    const QVector4D pf = invVP * QVector4D(nx, ny, 1.0f, 1.0f);
    if (qFuzzyIsNull(pn.w()) || qFuzzyIsNull(pf.w())) return -1;
    const QVector3D ro = pn.toVector3D() / pn.w();
    const QVector3D rd = (pf.toVector3D() / pf.w() - ro).normalized();

    float best = 1e30f;
    int   bestSub = -1;
    for (size_t pi = partFrom; pi < partTo; ++pi) {
        if (!m_srcParts[pi].mesh) continue;
        const di::MeshData& m = *m_srcParts[pi].mesh;
        if (m.positions.empty() || m.indices.empty() || m.submeshes.empty())
            continue;
        const size_t nv = m.positions.size() / 3;
        const std::vector<uint8_t>* mask =
            (pi < m_submeshMask.size() && !m_submeshMask[pi].empty())
                ? &m_submeshMask[pi]
                : nullptr;
        for (size_t si = 0; si < m.submeshes.size(); ++si) {
            if (mask && si < mask->size() && !(*mask)[si]) continue;   // hidden
            const di::SubMeshRange& r = m.submeshes[si];
            const size_t end =
                std::min((size_t)r.startIndex + r.indexCount, m.indices.size());
            for (size_t k = r.startIndex; k + 2 < end; k += 3) {
                const uint32_t i0 = m.indices[k], i1 = m.indices[k + 1],
                               i2 = m.indices[k + 2];
                if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
                const QVector3D v0(m.positions[i0 * 3], m.positions[i0 * 3 + 1],
                                   m.positions[i0 * 3 + 2]);
                const QVector3D v1(m.positions[i1 * 3], m.positions[i1 * 3 + 1],
                                   m.positions[i1 * 3 + 2]);
                const QVector3D v2(m.positions[i2 * 3], m.positions[i2 * 3 + 1],
                                   m.positions[i2 * 3 + 2]);
                const QVector3D e1 = v1 - v0, e2 = v2 - v0;
                const QVector3D pv = QVector3D::crossProduct(rd, e2);
                const float det = QVector3D::dotProduct(e1, pv);
                if (std::fabs(det) < 1e-8f) continue;   // ray parallel
                const float inv = 1.0f / det;
                const QVector3D tv = ro - v0;
                const float u = QVector3D::dotProduct(tv, pv) * inv;
                if (u < 0.0f || u > 1.0f) continue;
                const QVector3D qv = QVector3D::crossProduct(tv, e1);
                const float vparam = QVector3D::dotProduct(rd, qv) * inv;
                if (vparam < 0.0f || u + vparam > 1.0f) continue;
                const float t = QVector3D::dotProduct(e2, qv) * inv;
                if (t > 1e-4f && t < best) {
                    best = t;
                    bestSub = (int)si;
                    if (outPart) *outPart = (int)pi;
                }
            }
        }
    }
    return bestSub;
}

int GLMeshView::pickSubmesh(const QPoint& posPx) const
{
    return pickIn(posPx, 0, 1, nullptr);   // part 0 only (the Models tab's model)
}

int GLMeshView::pickAnyPart(const QPoint& posPx, int* outPart) const
{
    return pickIn(posPx, 0, m_srcParts.size(), outPart);
}

// Frame the camera on one whole part (every submesh), rotation kept. Used by the
// Wardrobe's "Frame this piece" — frameToSubmesh only covers a single range and
// does nothing at all for a mesh with no submesh table.
void GLMeshView::framePart(size_t part)
{
    if (part >= m_srcParts.size()) return;
    const std::shared_ptr<di::MeshData>& mesh = m_srcParts[part].mesh;
    if (!mesh) return;
    const size_t nv = mesh->vertexCount();
    if (!nv) return;
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    for (size_t v = 0; v < nv; ++v)
        for (int c = 0; c < 3; ++c) {
            const float x = mesh->positions[v * 3 + c];
            mn[c] = std::min(mn[c], x);
            mx[c] = std::max(mx[c], x);
        }
    m_center = QVector3D((mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f,
                         (mn[2] + mx[2]) * 0.5f);
    const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    m_radius = std::max(0.02f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    m_dist   = m_radius * 2.6f;
    update();
}

void GLMeshView::mouseMoveEvent(QMouseEvent* ev)
{
    const QPoint delta = ev->pos() - m_lastPos;
    m_lastPos = ev->pos();
    if (ev->buttons() & Qt::LeftButton) {
        // Orbit. DEFAULT is flipped from the D4 tool (DI users reported the
        // rotate reversed); the invert toggles put either axis back.
        const float ox = m_invOrbitX ? -1.0f : 1.0f;
        const float oy = m_invOrbitY ? -1.0f : 1.0f;
        m_yaw   += ox * delta.x() * 0.01f;
        m_pitch -= oy * delta.y() * 0.01f;
        m_pitch  = std::clamp(m_pitch, -1.55f, 1.55f);
        update();
        Q_EMIT cameraChanged();
    } else if (ev->buttons() & (Qt::MiddleButton | Qt::RightButton)) {
        // Pan the orbit centre in the camera plane. Matches the D4 tool exactly:
        // the old code used cross(up, fwd) with a +right term, which panned the
        // HORIZONTAL axis backwards (drag-right moved the view left). Using the
        // eye->centre view direction with `-right` fixes the inversion.
        const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
        const float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
        const QVector3D viewDir = -QVector3D(cp * sy, sp, cp * cy);   // eye -> centre
        QVector3D right = QVector3D::crossProduct(viewDir, QVector3D(0, 1, 0));
        if (right.lengthSquared() < 1e-8f) right = QVector3D(1, 0, 0);
        right.normalize();
        const QVector3D camUp = QVector3D::crossProduct(right, viewDir).normalized();
        const float s = m_dist * 0.0015f;
        const float pf = m_invPan ? -1.0f : 1.0f;
        m_center -= right * (delta.x() * s) * pf;
        m_center += camUp * (delta.y() * s) * pf;
        update();
        Q_EMIT cameraChanged();
    }
}

void GLMeshView::wheelEvent(QWheelEvent* ev)
{
    const float steps = ev->angleDelta().y() / 120.0f;
    m_dist *= std::pow(0.88f, steps);
    m_dist  = std::clamp(m_dist, m_radius * 0.15f, m_radius * 30.0f);
    update();
    Q_EMIT cameraChanged();
}
