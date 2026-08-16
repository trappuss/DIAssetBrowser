#include "util/CameraPanel.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStringList>
#include <QVBoxLayout>
#include <QtMath>

#include "gl/GLMeshView.h"
#include "tabs/BrowserTab.h"   // kPushBtnQss — the shared button skin

namespace {

constexpr float kPi  = 3.14159265f;
constexpr float kTop = 1.55f;      // the same gimbal guard the orbit drag uses
constexpr int   kPresets = 3;

QString key(const QString& prefix, const char* leaf)
{
    return prefix + QLatin1Char('/') + QLatin1String(leaf);
}

// A camera as a flat comma-joined record. Deliberately not QVariant/QDataStream:
// this lands in the user's registry / ini, where a readable row survives a
// settings-schema change and can be hand-edited or hand-deleted.
QString packCam(const GLMeshView::CamState& s)
{
    return QStringList{QString::number(s.yaw,   'g', 7),
                       QString::number(s.pitch, 'g', 7),
                       QString::number(s.dist,  'g', 7),
                       QString::number(s.fov,   'g', 7),
                       QString::number(s.cx,    'g', 7),
                       QString::number(s.cy,    'g', 7),
                       QString::number(s.cz,    'g', 7),
                       QString::number(s.ortho ? 1 : 0)}
        .join(QLatin1Char(','));
}

GLMeshView::CamState unpackCam(const QString& text)
{
    GLMeshView::CamState s;
    const QStringList f = text.split(QLatin1Char(','));
    if (f.size() != 8) return s;            // stays invalid -> setCameraState ignores it
    bool ok = true, all = true;
    auto num = [&](int i) { const float v = f[i].toFloat(&ok); all = all && ok; return v; };
    const float yaw = num(0), pitch = num(1), dist = num(2), fov = num(3);
    const float cx = num(4), cy = num(5), cz = num(6);
    const int ortho = f[7].toInt(&ok);
    all = all && ok;
    if (!all) return s;
    s.yaw = yaw;  s.pitch = pitch;  s.dist = dist;  s.fov = fov;
    s.cx = cx;  s.cy = cy;  s.cz = cz;
    s.ortho = (ortho != 0);
    s.valid = true;
    return s;
}

}   // namespace

namespace CameraPanel {

Widgets build(QVBoxLayout* lay, GLMeshView* view, const QString& prefix,
              const QString& autoFrameLabel)
{
    Widgets w;
    if (!lay || !view) return w;
    QWidget* host = lay->parentWidget();
    QSettings settings;

    auto sectionLabel = [&](const QString& text) {
        auto* l = new QLabel(text, host);
        l->setStyleSheet(QStringLiteral("color:#9a8f78;"));
        lay->addWidget(l);
    };
    auto skin = [](QPushButton* b) {
        b->setStyleSheet(QLatin1String(kPushBtnQss));
        return b;
    };

    // ── Lens ───────────────────────────────────────────────────────────────
    sectionLabel(QStringLiteral("Lens"));
    auto* fovRow = new QHBoxLayout();
    fovRow->addWidget(new QLabel(QStringLiteral("Field of view"), host));
    auto* fovSpin = new QDoubleSpinBox(host);
    fovSpin->setRange(10.0, 100.0);           // GLMeshView clamps to the same range
    fovSpin->setDecimals(0);
    fovSpin->setSuffix(QStringLiteral("°"));
    fovSpin->setFixedWidth(64);
    fovRow->addStretch(1);
    fovRow->addWidget(fovSpin);
    lay->addLayout(fovRow);
    auto* fovSlider = new QSlider(Qt::Horizontal, host);
    fovSlider->setRange(10, 100);
    fovSlider->setToolTip(QStringLiteral(
        "Vertical field of view. Also sizes the orthographic frame, so switching "
        "projection never jumps the framing."));
    lay->addWidget(fovSlider);
    {
        const double f = qBound(10.0, settings.value(key(prefix, "view/fov"), 45.0).toDouble(), 100.0);
        QSignalBlocker b1(fovSpin), b2(fovSlider);
        fovSpin->setValue(f);
        fovSlider->setValue(qRound(f));
    }
    QObject::connect(fovSpin, &QDoubleSpinBox::valueChanged, view,
                     [view, fovSlider, prefix](double v) {
                         QSignalBlocker b(fovSlider);
                         fovSlider->setValue(qRound(v));
                         QSettings().setValue(key(prefix, "view/fov"), v);
                         view->setFov(float(v));
                     });
    QObject::connect(fovSlider, &QSlider::valueChanged, view,
                     [fovSpin](int v) { fovSpin->setValue(double(v)); });

    auto* orthoChk = new QCheckBox(QStringLiteral("Orthographic projection"), host);
    orthoChk->setToolTip(QStringLiteral(
        "Parallel projection — double-clicking the orientation gizmo does the same."));
    orthoChk->setChecked(settings.value(key(prefix, "view/ortho"), false).toBool());
    QObject::connect(orthoChk, &QCheckBox::toggled, view, [view, prefix](bool on) {
        QSettings().setValue(key(prefix, "view/ortho"), on);
        view->setOrthographic(on);
    });
    lay->addWidget(orthoChk);

    // ── View angles ────────────────────────────────────────────────────────
    // Labelled by world axis, matching the orientation gizmo. See the header for
    // why these are not called Front/Back/Left/Right.
    sectionLabel(QStringLiteral("View angle"));
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(4);
    struct Angle { const char* label; float yaw; float pitch; bool keepYaw; };
    static const Angle kAngles[] = {
        {"+X",     kPi / 2,  0.0f,  false},
        {"-X",    -kPi / 2,  0.0f,  false},
        {"+Z",     0.0f,     0.0f,  false},
        {"-Z",     kPi,      0.0f,  false},
        {"Top",    0.0f,     kTop,  true},    // Y is the up vector: unambiguous
        {"Bottom", 0.0f,    -kTop,  true},
        {"Iso",    0.6f,     0.35f, false},   // the viewport's own default 3/4 view
    };
    for (int i = 0; i < int(sizeof(kAngles) / sizeof(kAngles[0])); ++i) {
        const Angle a = kAngles[i];
        auto* b = skin(new QPushButton(QLatin1String(a.label), host));
        b->setFixedHeight(22);
        QObject::connect(b, &QPushButton::clicked, view, [view, a] {
            // Top/Bottom keep the current yaw, exactly like the gizmo's Y balls:
            // spinning the model on the way to a top-down view is disorienting.
            view->setOrbitAngles(a.keepYaw ? view->camYaw() : a.yaw, a.pitch);
        });
        grid->addWidget(b, i / 4, i % 4);
    }
    lay->addLayout(grid);

    // ── Numeric orbit ──────────────────────────────────────────────────────
    auto* yawSpin = new QDoubleSpinBox(host);
    auto* pitchSpin = new QDoubleSpinBox(host);
    yawSpin->setRange(-360.0, 360.0);
    yawSpin->setWrapping(true);
    // The gimbal guard in degrees. It must be at least as wide as the view's
    // own clamp (kTop rad = 88.808 deg, exactly what Top/Bottom request): a
    // narrower spin box silently clamped the readout to 88.0 while the camera
    // sat at 88.81, so the box lied about where the camera was.
    pitchSpin->setRange(-89.0, 89.0);
    for (QDoubleSpinBox* sp : {yawSpin, pitchSpin}) {
        sp->setDecimals(1);
        sp->setSingleStep(5.0);
        sp->setSuffix(QStringLiteral("°"));
        sp->setFixedWidth(76);
    }
    auto* angRow = new QHBoxLayout();
    angRow->addWidget(new QLabel(QStringLiteral("Yaw"), host));
    angRow->addWidget(yawSpin);
    angRow->addSpacing(6);
    angRow->addWidget(new QLabel(QStringLiteral("Pitch"), host));
    angRow->addWidget(pitchSpin);
    angRow->addStretch(1);
    lay->addLayout(angRow);

    // Each box drives ONLY its own axis, taking the other from the LIVE camera.
    // Pushing both would resurrect a stale reading: the turntable moves yaw
    // without emitting cameraChanged (deliberately — see the signal's comment),
    // so after ten seconds of spinning, one click on the Pitch arrow used to
    // snap the model back to the yaw it had when the spin started.
    QObject::connect(yawSpin, &QDoubleSpinBox::valueChanged, view,
                     [view](double deg) {
                         view->setOrbitAngles(float(qDegreesToRadians(deg)),
                                              view->camPitch());
                     });
    QObject::connect(pitchSpin, &QDoubleSpinBox::valueChanged, view,
                     [view](double deg) {
                         view->setOrbitAngles(view->camYaw(),
                                              float(qDegreesToRadians(deg)));
                     });
    // Follow the viewport: a drag, a wheel, a gizmo click, a preset or a reframe
    // all land back here. Blocked while writing so the sync can't feed itself.
    QObject::connect(view, &GLMeshView::cameraChanged, yawSpin,
                     [view, yawSpin, pitchSpin, fovSpin, fovSlider, orthoChk] {
                         const GLMeshView::CamState c = view->cameraState();
                         QSignalBlocker b1(yawSpin), b2(pitchSpin), b3(fovSpin),
                             b4(fovSlider), b5(orthoChk);
                         double yawDeg = qRadiansToDegrees(double(c.yaw));
                         yawDeg = std::fmod(yawDeg, 360.0);
                         yawSpin->setValue(yawDeg);
                         pitchSpin->setValue(qRadiansToDegrees(double(c.pitch)));
                         fovSpin->setValue(double(c.fov));
                         fovSlider->setValue(qRound(double(c.fov)));
                         orthoChk->setChecked(c.ortho);
                     });

    // ── Turntable ──────────────────────────────────────────────────────────
    sectionLabel(QStringLiteral("Turntable"));
    w.turntable = new QCheckBox(QStringLiteral("Turntable (auto-rotate)"), host);
    w.turntable->setChecked(settings.value(key(prefix, "view/turntable"), false).toBool());
    // The tab owns the toggled connection (it also drives the GIF capture
    // suspend), so this only persists the state.
    QObject::connect(w.turntable, &QCheckBox::toggled, view, [prefix](bool on) {
        QSettings().setValue(key(prefix, "view/turntable"), on);
    });
    lay->addWidget(w.turntable);
    auto* spd = new QSlider(Qt::Horizontal, host);
    spd->setRange(5, 180);
    spd->setToolTip(QStringLiteral("Degrees per second"));
    spd->setValue(qBound(5, settings.value(key(prefix, "view/turntableSpeed"), 30).toInt(), 180));
    QObject::connect(spd, &QSlider::valueChanged, view, [view, prefix](int v) {
        QSettings().setValue(key(prefix, "view/turntableSpeed"), v);
        view->setTurntableSpeed(float(v));
    });
    lay->addWidget(spd);

    // ── Framing ────────────────────────────────────────────────────────────
    sectionLabel(QStringLiteral("Framing"));
    w.frameBtn = skin(new QPushButton(QStringLiteral("Frame model"), host));
    w.frameBtn->setToolTip(QStringLiteral("Re-fit the camera to what is loaded"));
    QObject::connect(w.frameBtn, &QPushButton::clicked, view, [view] { view->reframe(); });
    lay->addWidget(w.frameBtn);
    auto* resetBtn = skin(new QPushButton(QStringLiteral("Reset view"), host));
    resetBtn->setToolTip(QStringLiteral(
        "Back to the default 3/4 angle, perspective, 45°, re-fitted."));
    QObject::connect(resetBtn, &QPushButton::clicked, view,
                     [view, orthoChk, fovSpin, prefix] {
                         // A real reset, unlike the old per-tab button that just
                         // called reframe() and left a 12-degree ortho camera alone.
                         view->setOrthographic(false);
                         view->setFov(45.0f);
                         view->setOrbitAngles(0.6f, 0.35f);
                         view->reframe();
                         QSettings s;
                         s.setValue(key(prefix, "view/ortho"), false);
                         s.setValue(key(prefix, "view/fov"), 45.0);
                         QSignalBlocker b1(orthoChk), b2(fovSpin);
                         orthoChk->setChecked(false);
                         fovSpin->setValue(45.0);
                     });
    lay->addWidget(resetBtn);

    auto* autoFrameChk = new QCheckBox(autoFrameLabel, host);
    autoFrameChk->setToolTip(QStringLiteral(
        "Re-fit the camera when the geometry changes. Off keeps your framing."));
    autoFrameChk->setChecked(settings.value(key(prefix, "autoFrame"), true).toBool());
    QObject::connect(autoFrameChk, &QCheckBox::toggled, view, [view, prefix](bool on) {
        QSettings().setValue(key(prefix, "autoFrame"), on);
        view->setAutoFrame(on);
    });
    lay->addWidget(autoFrameChk);

    // Global by construction: GLMeshView reads this key live when a pick lands,
    // so there is only one setting no matter which tab's checkbox writes it.
    auto* framePickChk = new QCheckBox(QStringLiteral("Frame part on select"), host);
    framePickChk->setToolTip(QStringLiteral(
        "Double-clicking a part in the viewport also zooms to it. Shared by every 3D tab."));
    framePickChk->setChecked(
        settings.value(QStringLiteral("models/framePartOnPick"), false).toBool());
    QObject::connect(framePickChk, &QCheckBox::toggled, view, [](bool on) {
        QSettings().setValue(QStringLiteral("models/framePartOnPick"), on);
    });
    lay->addWidget(framePickChk);

    // ── Presets ────────────────────────────────────────────────────────────
    sectionLabel(QStringLiteral("Camera presets"));
    for (int i = 1; i <= kPresets; ++i) {
        const QString slotKey = QStringLiteral("cam/preset%1").arg(i);
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(QStringLiteral("%1").arg(i), host));
        auto* saveBtn = skin(new QPushButton(QStringLiteral("Save"), host));
        auto* loadBtn = skin(new QPushButton(QStringLiteral("Recall"), host));
        saveBtn->setFixedHeight(22);
        loadBtn->setFixedHeight(22);
        const QString full = prefix + QLatin1Char('/') + slotKey;
        loadBtn->setEnabled(!settings.value(full).toString().isEmpty());
        saveBtn->setToolTip(QStringLiteral("Store the current angle, zoom, centre, "
                                           "field of view and projection here"));
        QObject::connect(saveBtn, &QPushButton::clicked, view,
                         [view, full, loadBtn] {
                             QSettings().setValue(full, packCam(view->cameraState()));
                             loadBtn->setEnabled(true);
                         });
        QObject::connect(loadBtn, &QPushButton::clicked, view, [view, full] {
            const GLMeshView::CamState c = unpackCam(QSettings().value(full).toString());
            if (c.valid) view->setCameraState(c);   // invalid = nothing stored; ignored
        });
        row->addWidget(saveBtn);
        row->addWidget(loadBtn);
        row->addStretch(1);
        lay->addLayout(row);
    }

    auto* rememberChk = new QCheckBox(QStringLiteral("Remember camera on relaunch"), host);
    rememberChk->setToolTip(QStringLiteral(
        "Store this tab's camera when the app closes and restore it next time. "
        "Auto-framing still re-fits once a model actually loads — turn that off "
        "above to keep the restored framing."));
    rememberChk->setChecked(settings.value(key(prefix, "cam/remember"), false).toBool());
    QObject::connect(rememberChk, &QCheckBox::toggled, view, [prefix](bool on) {
        QSettings s;
        s.setValue(key(prefix, "cam/remember"), on);
        if (!on) s.remove(key(prefix, "cam/last"));   // don't keep a camera nobody wants
    });
    lay->addWidget(rememberChk);

    return w;
}

void applyStartupState(GLMeshView* view, const QString& prefix)
{
    if (!view) return;
    QSettings s;
    view->setFov(float(qBound(10.0, s.value(key(prefix, "view/fov"), 45.0).toDouble(), 100.0)));
    view->setOrthographic(s.value(key(prefix, "view/ortho"), false).toBool());
    if (!s.value(key(prefix, "cam/remember"), false).toBool()) return;
    const GLMeshView::CamState c = unpackCam(s.value(key(prefix, "cam/last")).toString());
    if (c.valid) view->setCameraState(c);
}

void rememberOnExit(GLMeshView* view, const QString& prefix)
{
    if (!view) return;
    QSettings s;
    if (!s.value(key(prefix, "cam/remember"), false).toBool()) return;
    s.setValue(key(prefix, "cam/last"), packCam(view->cameraState()));
}

}   // namespace CameraPanel
