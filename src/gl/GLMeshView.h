#pragma once
// Mesh viewport — orbit camera, headlight shading with authored Q4H normals,
// tangent-space normal mapping, mix-map PBR-lite (R=roughness G=metallic
// B=AO — measured), emissive, GPU skinning, skeleton overlay, wireframe and
// point-cloud fallback.
//
// MULTI-PART: the view renders N parts (mesh + textures each) in one shared
// space — every DI piece of one character sits in the same mesh space, so the
// Wardrobe tab just hands all selected pieces over. The Models tab uses the
// single-part wrapper. Each part can carry its own skinning pose (its bone
// indices reference its OWN SkinSkeleton subset).

#include <QColor>
#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QString>

class QTimer;

#include <memory>
#include <vector>

#include "model/MeshParser.h"
#include "model/MeshTextures.h"

struct ViewPart {
    std::shared_ptr<di::MeshData> mesh;
    MeshTextures textures;
};

// 3.3 core functions: everything this viewport touches (VAOs, glPolygonMode)
// with the widest driver compatibility; the app requests a 4.5 context, which
// is backward-compatible with the 3.3 entry points.
class GLMeshView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit GLMeshView(QWidget* parent = nullptr);
    ~GLMeshView() override;

    // GUI thread only. Replaces ALL parts; textures may be null.
    void setParts(std::vector<ViewPart> parts);
    // Replace ONE part (grows the part list if needed) — only that part's GPU
    // data re-uploads, so a 7-slot wardrobe change stays O(1) not O(n).
    // A default-constructed ViewPart empties the slot.
    void setPart(size_t index, ViewPart part);
    // Single-part convenience (the Models tab).
    void setMesh(std::shared_ptr<di::MeshData> mesh, MeshTextures textures);
    void clearMesh();
    void setWireframe(bool on);
    // Blender-style shading mode driven by the D4 toolbar's shading balls:
    // 0 wireframe · 1 flat · 2 shaded (legacy headlight) · 3 rendered (PBR).
    void setShadingMode(int mode);
    // Render one raw material channel instead of the lit result (D4's Channel
    // dropdown): 0 Shaded · 1 Base Color · 2 Normal · 3 Roughness · 4 Metallic
    // · 5 AO · 6 Emissive.
    void setViewChannel(int ch) { m_viewChannel = ch; update(); }
    // Orthographic projection toggle (D4's clickable gizmo / double-tap). When
    // on, the perspective frustum is swapped for a parallel one sized to match
    // the current orbit distance so the switch doesn't jump the framing.
    void setOrthographic(bool on)
    {
        if (m_ortho == on) return;
        m_ortho = on;
        update();
        Q_EMIT cameraChanged();
    }
    bool orthographic() const { return m_ortho; }

    // ── Camera (D4 Camera-panel parity) ────────────────────────────────────
    // Vertical field of view in degrees. The ortho half-height tracks it too, so
    // toggling projection never jumps the framing.
    void  setFov(float deg);
    float fov() const { return m_fov; }
    // Set both orbit angles at once (the Yaw/Pitch rows and the angle presets).
    // Pitch is clamped to the same gimbal guard the mouse drag uses.
    void  setOrbitAngles(float yawRad, float pitchRad);
    // The complete camera, for presets and "remember camera on relaunch".
    struct CamState {
        float yaw = 0.6f, pitch = 0.35f, dist = 3.0f, fov = 45.0f;
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        bool  ortho = false;
        bool  valid = false;
    };
    CamState cameraState() const;
    void     setCameraState(const CamState& s);
    // Which tab's settings namespace this viewport belongs to ("models" /
    // "wardrobe"). The orientation gizmo's double-click persists the projection
    // it just toggled, and without this it wrote one hard-coded key from every
    // viewport — so toggling ortho on the Wardrobe silently changed the Models
    // tab's stored projection. CameraPanel uses the same prefix for the
    // checkbox that mirrors this state.
    void    setSettingsPrefix(const QString& p) { m_settingsPrefix = p; }
    QString orthoSettingsKey() const
    {
        return m_settingsPrefix + QStringLiteral("/view/ortho");
    }

    // ── Capture (drives ExportCapture) ─────────────────────────────────────
    // While capturing, the animated-emissive shader clock reads setCaptureTime()
    // instead of the wall clock. Without this a GIF samples the glow at whatever
    // irregular real intervals the encode happened to take, so the same model
    // exports differently every run and the loop point visibly tears.
    void setCaptureMode(bool on);
    void setCaptureTime(float seconds) { m_captureTime = seconds; }
    bool capturing() const { return m_capturing; }
    // Alpha-0 clear with the background suppressed — a genuinely transparent grab.
    void setTransparentClear(bool on) { m_transparentClear = on; update(); }
    // Alpha-0 clear with the background still drawn: RGB is the normal render
    // while alpha is pure model coverage, so a crop can find the subject without
    // a second pass. Stripped back to opaque once the crop box is known.
    void setCoverageAlpha(bool on) { m_coverageAlpha = on; update(); }
    // Render one frame into an offscreen buffer `factor` times the widget size.
    // Genuinely re-renders (not an upscale); returns a null image if the driver
    // refuses every factor, so callers must fall back to grabFramebuffer().
    QImage grabSupersampled(int factor);

    // RAII capture mode — guarantees the flag is cleared on every exit path,
    // including an early return or a thrown exception.
    struct CaptureScope {
        GLMeshView* v;
        explicit CaptureScope(GLMeshView* w) : v(w) { if (v) v->setCaptureMode(true); }
        ~CaptureScope() { if (v) v->setCaptureMode(false); }
        CaptureScope(const CaptureScope&) = delete;
        CaptureScope& operator=(const CaptureScope&) = delete;
    };

    // Per-submesh visibility for one part (D4 parts-panel port): one flag per
    // SubMeshRange, empty = everything visible. Cleared by setParts/setPart.
    // Hidden ranges are simply not drawn — geometry stays uploaded, so
    // toggling is free.
    void setPartSubmeshMask(size_t part, std::vector<uint8_t> visible);

    // Viewport options (D4 Graphics/Camera panel ports).
    void setShowGrid(bool on);          // ground grid at the model's base
    void setBackfaceCull(bool on);      // default off: DI cloth is double-sided
    void setAlpha(bool on);             // alpha-tested transparency (hair/cloth)
    void setBackground(const QColor& c);
    void setTurntable(bool on);         // auto-rotate the orbit yaw
    void setTurntableSpeed(float degPerSec);
    void  setYaw(float yaw);            // set orbit yaw directly (turntable GIF capture)
    float yaw() const { return m_yaw; }
    // Camera basis accessors — the axis-gizmo overlay reads these each repaint.
    float camYaw() const   { return m_yaw; }
    float camPitch() const { return m_pitch; }
    // Glide the orbit to look down a world axis (keeps centre + zoom). Driven by
    // the clickable orientation gizmo's axis balls.
    void  orbitToAxis(float yawRad, float pitchRad);
    void reframe();                     // re-fit camera to the current parts
    // Auto-frame on geometry change (default on). When off, a newly-uploaded
    // model keeps the current camera instead of re-fitting — except the very
    // first model ever shown, which always frames so it isn't off-screen.
    void setAutoFrame(bool on) { m_autoFrame = on; }
    // Frame the camera to a single submesh of a part (rotation kept). Used by
    // the Models tab's "Frame part on select". Out-of-range args are ignored.
    void frameToSubmesh(size_t part, size_t submesh);

    // Ray-pick the front-most visible submesh of part 0 under a widget-space
    // point (Möller–Trumbore against the CPU mesh, same proj/view as paintGL).
    // Returns the submesh index, or -1 on a miss. Honors the visibility mask.
    int pickSubmesh(const QPoint& posPx) const;
    // Same pick, but across EVERY part (the Wardrobe's pieces are parts 1..N as
    // much as part 0). Returns the submesh index and writes the winning part to
    // *outPart; both -1 on a miss.
    int pickAnyPart(const QPoint& posPx, int* outPart) const;
    // Frame the camera on one whole part (all of its submeshes), rotation kept.
    void framePart(size_t part);

signals:
    // Emitted when the user double-clicks a submesh in the viewport (D4's
    // partFocused) — the Models tab selects that part in the PARTS panel.
    // Only fires for part 0, which is the Models tab's model.
    void submeshPicked(int submesh);
    // Part-aware twin of the above, for multi-part scenes (the Wardrobe).
    void partPicked(int part, int submesh);
    // The orbit/zoom/projection changed by something the Camera panel should
    // follow: a drag, a wheel, an axis snap, a preset, a reframe.
    // Deliberately NOT emitted by the turntable's per-frame yaw drift (that
    // runs inside paintGL and would flicker the readouts 30x a second) nor by
    // setYaw() during a GIF capture, which walks a full revolution.
    void cameraChanged();

public:

    // ── Material emissive glow toggle (the "FX" checkbox) ──────────────────
    // Shows/hides the model material's authored animated emissive layer. The
    // separate particle-effect feature was removed (see FX_ARCHIVE.md).
    void setFxVisible(bool on);
    bool fxVisible() const { return m_fxVisible; }

    // Skeleton overlay: `segments` is parent->child line pairs (6 floats per
    // segment), `joints` all joint positions (3 floats each), both in mesh
    // space. Cleared by setParts/setMesh/clearMesh — call after.
    void setSkeleton(std::vector<float> segments, std::vector<float> joints);
    void setShowSkeleton(bool on);

    // GPU skinning pose for one part: 16 floats per skin bone, upload-ready
    // column-major (PosePlayer::evaluate output). Empty = back to bind pose.
    // Bone order must match that part's boneIndices (its SkinSkeleton order).
    static constexpr int kMaxBones = 128;
    void setPartPose(size_t part, std::vector<float> skinMats);
    void setPose(std::vector<float> skinMats) { setPartPose(0, std::move(skinMats)); }
    void clearPose();   // all parts

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;

private:
    struct GpuPart {
        unsigned vao = 0, vbo = 0, ibo = 0;
        unsigned texD = 0, texN = 0, texM = 0, texE = 0;
        int  indexCount = 0, vertexCount = 0;
        int  maxBoneIdx = -1;   // highest bone index the mesh references
        bool textured = false, hasNormals = false;
        bool hasNrmMap = false, hasMix = false, hasEmi = false;
    };
    // Shared ray-pick over parts [partFrom, partTo); *outPart (when non-null)
    // receives the winning part index. Backs both public pick entry points.
    int  pickIn(const QPoint& posPx, size_t partFrom, size_t partTo,
                int* outPart) const;
    void uploadPending();
    void uploadOnePart(size_t i);   // m_srcParts[i] -> m_parts[i]
    void deletePart(struct GpuPart& p);
    void deleteParts();
    void frameMesh();

    // CPU-authoritative content (survives context loss)
    std::vector<ViewPart> m_srcParts;
    std::vector<std::vector<float>> m_poses;   // parallel to m_srcParts
    std::vector<std::vector<uint8_t>> m_submeshMask;   // parallel; empty = all
    std::vector<size_t> m_dirtyParts;          // incremental setPart updates
    bool m_hasPending = false;                 // full rebuild

    // GPU state (owned by this widget's context)
    std::vector<GpuPart> m_parts;
    unsigned m_prog = 0, m_progFlat = 0;
    bool m_glOk = false;
    bool m_wireframe  = false;
    int  m_shadeMode  = 3;   // 0 wire · 1 flat · 2 shaded · 3 rendered (PBR default)
    bool m_poseWarned = false;

    // skeleton overlay (CPU copies retained for context-loss re-upload)
    std::vector<float> m_skelSegs;
    std::vector<float> m_skelJoints;
    bool     m_skelDirty  = false;
    bool     m_showSkel   = false;
    unsigned m_skelVao = 0, m_skelVbo = 0;
    int      m_skelSegVerts = 0, m_skelJointVerts = 0;

    // ── Material emissive/glow animation clock ──────────────────────────────
    // Drives repaints while a material's authored emissive layer animates
    // (arcane scroll / star sparkle). NOT the removed particle-FX feature.
    void updateFxTimer();
    bool     m_fxVisible = true;    // material emissive glow (the "FX" toggle)
    QTimer*  m_fxTimer   = nullptr; // repaint driver for the emissive scroll
    qint64   m_fxT0      = 0;       // ms epoch for the emissive clock

    // ground grid (XZ plane at the model's base)
    bool     m_showGrid = false;
    bool     m_gridDirty = true;
    float    m_gridY = 0.0f;
    unsigned m_gridVao = 0, m_gridVbo = 0;
    int      m_gridVerts = 0, m_gridAxisVerts = 0;

    // options
    bool   m_backfaceCull = false;
    bool   m_alpha        = false;   // alpha-tested transparency toggle
    // Camera-control inversion (loaded from QSettings on each drag start). The
    // orbit DEFAULT is flipped from the D4 tool because DI users reported it
    // reversed; these let each axis be flipped back. Keys: view/invOrbitX,
    // view/invOrbitY, view/invPan.
    bool   m_invOrbitX = false;
    bool   m_invOrbitY = false;
    bool   m_invPan    = false;
    QColor m_bg{26, 26, 28};
    QTimer* m_spinTimer = nullptr;   // turntable
    float   m_spinDegPerSec = 30.0f;

    // camera
    float m_yaw = 0.6f, m_pitch = 0.35f, m_dist = 3.0f;
    float m_fov = 45.0f;              // vertical FOV, degrees
    QVector3D m_center;
    float     m_radius = 1.0f;

    // capture state
    bool     m_capturing        = false;
    float    m_captureTime      = 0.0f;   // deterministic FX clock, seconds
    bool     m_transparentClear = false;
    bool     m_coverageAlpha    = false;
    unsigned m_captureFbo       = 0;      // non-zero while rendering offscreen
    int      m_fbW = 0, m_fbH = 0;        // offscreen target size (0 = the widget)
    bool     m_spinWasActive    = false;  // turntable state to restore after capture
    QPoint    m_lastPos;
    int       m_viewChannel = 0;      // raw-channel view (0 = lit result)
    bool      m_ortho      = false;    // orthographic projection (gizmo toggle)
    QString   m_settingsPrefix = QStringLiteral("models");   // see setSettingsPrefix
    bool      m_autoFrame  = true;    // re-fit camera when geometry changes
    bool      m_framedOnce = false;   // the first model always frames regardless

    // Clickable orientation gizmo (D4 AxisGizmoOverlay port). A plain child
    // widget pinned top-right — kept as QWidget* because its concrete type lives
    // in the .cpp anonymous namespace. Created lazily in initializeGL, sized in
    // resizeGL.
    QWidget*  m_gizmo = nullptr;
    void positionGizmo();
};
