#pragma once
// Shared soft-body (cloth) glue for the tabs that play + export animation
// (Models, Wardrobe). One place owns the QSettings keys, the "bake this clip if
// cloth is on" helper, and the tuning dialog, so the two tabs can never drift in
// parameters or behaviour (the project's one-setting-one-key / shared-builder
// rule). The physics itself lives in model/ClothSim.{h,cpp}; this is only the
// Qt-facing settings + UI wrapper.

#include <functional>
#include <memory>

#include "model/ClothSim.h"   // di::ClothParams + di::AnimClip/SkinSkeleton/Bone*

class QWidget;

namespace cloth {

// The solver tunables, loaded from QSettings ("cloth/*"). Both the viewport
// player and the export worker read this, so what you see is what you export.
di::ClothParams paramsFromSettings();

// The master on/off (ExportSettings "export/cloth"), shared by both tabs.
bool enabled();
void setEnabled(bool on);

// Return `clip` unchanged, or a cloth-baked copy when `on` and the skeleton
// actually has cloth bones. Safe with any null input (returns `clip`).
std::shared_ptr<di::AnimClip> maybeBake(
    const std::shared_ptr<di::AnimClip>& clip, const di::SkinSkeleton* skel,
    const di::BoneParents* hier, const di::BoneLocals* locals, bool on);

// Modal tuning dialog (gravity / tracking / stiffness / damping / max-stretch /
// body-collision / iterations). Writes the cloth/* keys on OK and calls
// `onApply` so the caller can re-bake and re-tick the live view.
void showTuningDialog(QWidget* parent, const std::function<void()>& onApply);

} // namespace cloth
