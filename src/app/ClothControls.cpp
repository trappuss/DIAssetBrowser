#include "app/ClothControls.h"

#include "app/ExportSettings.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>

namespace cloth {

di::ClothParams paramsFromSettings()
{
    QSettings s;
    di::ClothParams p;   // struct defaults are the fallback for every key
    p.gravity      = s.value(QStringLiteral("cloth/gravity"),    p.gravity).toFloat();
    p.boneTracking = s.value(QStringLiteral("cloth/tracking"),   p.boneTracking).toFloat();
    p.stiffness    = s.value(QStringLiteral("cloth/stiffness"),  p.stiffness).toFloat();
    p.damping      = s.value(QStringLiteral("cloth/damping"),    p.damping).toFloat();
    p.iterations   = s.value(QStringLiteral("cloth/iterations"), p.iterations).toInt();
    p.maxStretch   = s.value(QStringLiteral("cloth/maxStretch"), p.maxStretch).toFloat();
    p.bodyRadius   = s.value(QStringLiteral("cloth/bodyRadius"), p.bodyRadius).toFloat();
    return p;
}

bool enabled()          { return ExportSettings::clothPhysics(); }
void setEnabled(bool on){ ExportSettings::setClothPhysics(on); }

std::shared_ptr<di::AnimClip> maybeBake(
    const std::shared_ptr<di::AnimClip>& clip, const di::SkinSkeleton* skel,
    const di::BoneParents* hier, const di::BoneLocals* locals, bool on)
{
    if (!clip || !skel || !hier || !locals || !on) return clip;
    if (!di::hasClothBones(*skel)) return clip;
    return di::bakeCloth(*clip, *skel, *hier, *locals, paramsFromSettings());
}

void showTuningDialog(QWidget* parent, const std::function<void()>& onApply)
{
    di::ClothParams def;   // struct defaults, for the Reset button
    const di::ClothParams cur = paramsFromSettings();

    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Cloth physics"));
    auto* form = new QFormLayout(&dlg);

    auto mkD = [&](double lo, double hi, double step, double val, int dec) {
        auto* sb = new QDoubleSpinBox(&dlg);
        sb->setRange(lo, hi); sb->setSingleStep(step); sb->setDecimals(dec);
        sb->setValue(val);
        return sb;
    };
    // Ranges chosen so no value can destabilise the solver (all bake-safe).
    auto* grav  = mkD(0.0, 20.0, 0.5,  cur.gravity, 2);
    auto* trk   = mkD(0.0,  1.0, 0.05, cur.boneTracking, 2);
    auto* stif  = mkD(0.0,  1.0, 0.05, cur.stiffness, 2);
    auto* damp  = mkD(0.0, 0.99, 0.01, cur.damping, 2);
    auto* strch = mkD(1.0,  1.5, 0.01, cur.maxStretch, 2);
    auto* body  = mkD(0.0,  2.0, 0.05, cur.bodyRadius, 2);
    auto* iters = new QSpinBox(&dlg);
    iters->setRange(1, 32); iters->setValue(cur.iterations);

    trk->setToolTip(QStringLiteral("How tightly cloth hugs the animated pose. "
        "1 = no visible sway (same as off); lower = swings more freely."));
    grav->setToolTip(QStringLiteral("Downward pull (world units/s²)."));
    stif->setToolTip(QStringLiteral("Per-frame spring back toward the rest pose."));
    damp->setToolTip(QStringLiteral("Velocity retained per frame (higher = more, longer swing)."));
    strch->setToolTip(QStringLiteral("Max bone-length change; keeps cloth from stretching."));
    body->setToolTip(QStringLiteral("Body collision: pushes cloth off core body bones "
        "so it doesn't clip through. 0 = off. Radius scales with each bone's length; "
        "raise until cloth clears the body, lower if it stands too far off."));
    iters->setToolTip(QStringLiteral("Constraint relaxation passes (higher = stiffer, costlier)."));

    form->addRow(QStringLiteral("Bone tracking"), trk);
    form->addRow(QStringLiteral("Gravity"),       grav);
    form->addRow(QStringLiteral("Stiffness"),     stif);
    form->addRow(QStringLiteral("Damping"),       damp);
    form->addRow(QStringLiteral("Max stretch"),   strch);
    form->addRow(QStringLiteral("Body collision"),body);
    form->addRow(QStringLiteral("Iterations"),    iters);

    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
        QDialogButtonBox::RestoreDefaults, &dlg);
    form->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(bb->button(QDialogButtonBox::RestoreDefaults),
                     &QPushButton::clicked, &dlg, [&] {
        grav->setValue(def.gravity);   trk->setValue(def.boneTracking);
        stif->setValue(def.stiffness); damp->setValue(def.damping);
        strch->setValue(def.maxStretch); iters->setValue(def.iterations);
        body->setValue(def.bodyRadius);
    });

    if (dlg.exec() != QDialog::Accepted) return;
    QSettings w;
    w.setValue(QStringLiteral("cloth/gravity"),    grav->value());
    w.setValue(QStringLiteral("cloth/tracking"),   trk->value());
    w.setValue(QStringLiteral("cloth/stiffness"),  stif->value());
    w.setValue(QStringLiteral("cloth/damping"),    damp->value());
    w.setValue(QStringLiteral("cloth/maxStretch"), strch->value());
    w.setValue(QStringLiteral("cloth/bodyRadius"), body->value());
    w.setValue(QStringLiteral("cloth/iterations"), iters->value());
    if (onApply) onApply();
}

} // namespace cloth
