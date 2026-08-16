#pragma once
// ── Shared viewport capture: still image, turntable GIF, animation-loop GIF ──
// Ported from D4AssetBrowser's ExportCapture, and deliberately the ONLY capture
// code in the app: the Models tab and the Wardrobe tab both call these, so a
// screenshot or a GIF comes out identical whichever tab produced it, and every
// option below is read from one set of QSettings keys rather than from
// per-tab constants.
//
// Settings (the same keys D4 uses, so the two tools agree):
//   export/transparentBg      bool  false  alpha-0 background where the container allows it
//   export/imageFormat        str   "png"  png | jpg | webp (the save dialog's default)
//   export/imageScale         int   100    25..400 %; above 100 the scene is genuinely
//                                          re-rendered larger, never upscaled
//   export/imageQuality       int   92     1..100, applied to jpg/webp only
//   export/gifFps             int   25     1..60 (turntable only — an animation loop
//                                          uses the clip's own rate)
//   export/gifTurntableFrames int   48     8..240 steps in one revolution
//   export/gifScale           int   100    25..100 % of the viewport
//   export/gifMaxColors       int   256    16..256 palette entries
//   export/gifDither          bool  true   ordered (position-only) dither
//   export/gifOptimize        bool  false  re-encode until the file fits gifTargetMB
//   export/gifTargetMB        int   10     1..200
//   export/gifCropToModel     bool  false  trim to the subject's bounds (images and GIFs)
//   export/imageDir           str   —      last folder used, shared by all three
//
// DI bakes cloth into the played clip up front (cloth::maybeBake), so a pose is
// a pure function of its frame index. That is why there is no warm-up lap here:
// unlike D4, seeking to frame N twice gives the same result both times.

#include <QString>

#include <functional>

class GLMeshView;
class QWidget;

namespace ExportCapture {

// Called with (framesDone, framesTotal) after each rendered frame. Return false
// to cancel: the capture restores the viewport and writes nothing.
using ProgressFn = std::function<bool(int done, int total)>;

// How a caller lets the capture scrub its animation. The tabs own playback (the
// pose players live there, not in the viewport), so they hand over a seek
// function instead of the capture reaching into them.
struct AnimSource {
    int   frameCount = 0;        // 0 = nothing playing; the turntable then just orbits
    float fps        = 30.0f;    // the clip's authored rate
    int   savedFrame = 0;        // restored when the capture finishes or is cancelled
    std::function<void(int frame)> seek;   // pose the scene at this frame index
};

// The configured still-image container ("png" / "jpg" / "webp"). The save dialog
// takes its default extension and filter from here so the setting and the dialog
// cannot disagree; the actual container still comes from the chosen path.
QString imageFormat();

// ── Low-level: capture to a path the caller already chose ──────────────────
bool saveImage(GLMeshView* view, const QString& path);
bool turntableGif(GLMeshView* view, const QString& path, const AnimSource& anim = {},
                  const ProgressFn& progress = {});
bool animLoopGif(GLMeshView* view, const QString& path, const AnimSource& anim,
                 const ProgressFn& progress = {});

// ── Full flows: save dialog + progress dialog + result message ─────────────
// These are what the tabs call. Sharing them is what makes the two tabs behave
// identically — including the remembered folder, the cancellable progress and
// the wording of the success/failure line. *outMsg is always set.
// `baseName` seeds the filename (extension is added here).
bool runSaveImage(QWidget* parent, GLMeshView* view, const QString& baseName,
                  QString* outMsg);
bool runTurntableGif(QWidget* parent, GLMeshView* view, const QString& baseName,
                     const AnimSource& anim, QString* outMsg);
bool runAnimLoopGif(QWidget* parent, GLMeshView* view, const QString& baseName,
                    const AnimSource& anim, QString* outMsg);

}   // namespace ExportCapture
