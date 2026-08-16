#pragma once
// ── Shared Camera popover, ported from D4AssetBrowser's Camera panel ────────
// Both 3D tabs (Models, Wardrobe) used to build their own Camera dropdown by
// hand, and the two had already drifted: the Wardrobe had an Orthographic
// toggle the Models tab lacked, the Models tab had a Reset that did the same
// thing as its own Frame button, and NEITHER had field of view, view-angle
// snapping, numeric yaw/pitch, camera presets or remember-on-relaunch. This
// builds the one panel both of them show, so a camera control added here
// appears on every 3D tab at once and cannot diverge again.
//
// Everything persists per tab under a caller-supplied prefix ("models" /
// "wardrobe"), because the two tabs frame very different subjects and sharing
// one camera between them would be worse, not better. The single deliberate
// exception is "models/framePartOnPick", which GLMeshView reads live at pick
// time and is therefore global by construction — both panels write that same
// key, and the checkbox is kept in sync so the two never disagree on screen.
//
// A note on the view-angle buttons: they are labelled by WORLD AXIS (+X, -X,
// Top, Bottom, +Z, -Z), not Front/Back/Left/Right. DI's mesh format records no
// authored facing direction, so which axis a given character's face points down
// is a per-asset accident — naming a button "Front" would be a guess. The axis
// labels match the viewport's orientation gizmo exactly, and Top/Bottom are
// unambiguous because Y is the up vector the camera is built on.

#include <QString>

class GLMeshView;
class QCheckBox;
class QPushButton;
class QVBoxLayout;

namespace CameraPanel {

// The two widgets a tab still needs to hold on to after the panel is built:
// the turntable toggle (a GIF capture suspends it and puts it back) and the
// Frame button (tabs enable/disable it with the rest of their model controls).
struct Widgets {
    QCheckBox*   turntable = nullptr;
    QPushButton* frameBtn  = nullptr;
};

// Fill `lay` with the whole panel. Widgets parent to lay->parentWidget().
// `prefix` scopes the QSettings keys; `autoFrameLabel` is the only per-tab
// wording ("Auto-frame model on load" vs "Auto-frame on equip"), because the
// two tabs re-frame on genuinely different events.
Widgets build(QVBoxLayout* lay, GLMeshView* view, const QString& prefix,
              const QString& autoFrameLabel);

// Apply the persisted field of view / projection to a freshly-built view, and
// restore the remembered camera when that option is on. Call once, after the
// view exists — before the first model, so an auto-frame can still override a
// camera the user never asked to keep.
void applyStartupState(GLMeshView* view, const QString& prefix);

// Store the live camera under <prefix>/cam/last when the remember option is on.
// Call from the tab's destructor; a no-op otherwise.
void rememberOnExit(GLMeshView* view, const QString& prefix);

}   // namespace CameraPanel
