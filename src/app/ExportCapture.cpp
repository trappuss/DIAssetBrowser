#include "app/ExportCapture.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QProgressDialog>
#include <QSettings>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <vector>

#include "gl/GLMeshView.h"
#include "gl/GifEncoder.h"

namespace {

const char kDirKey[] = "export/imageDir";

bool transparentEnabled()
{
    return QSettings().value(QStringLiteral("export/transparentBg"), false).toBool();
}
bool cropEnabled()
{
    return QSettings().value(QStringLiteral("export/gifCropToModel"), false).toBool();
}

// One grab with the requested alpha behaviour. The flags are set around the
// call because the render happens inside grabFramebuffer(), not just the read.
QImage captureFrame(GLMeshView* view, bool wantTransparent, bool wantCoverage)
{
    if (!view) return {};
    if (wantTransparent) view->setTransparentClear(true);
    if (wantCoverage)    view->setCoverageAlpha(true);
    QImage img = view->grabFramebuffer();
    if (wantTransparent) view->setTransparentClear(false);
    if (wantCoverage)    view->setCoverageAlpha(false);
    if (img.isNull()) return {};
    return img.convertToFormat(QImage::Format_RGBA8888);
}

// Append one frame to the GIF buffer. The FIRST frame fixes the output size;
// every later frame is resampled to match, because the encoder requires every
// frame to be exactly width*height*4 bytes.
bool pushFrame(QImage f, std::vector<std::vector<uint8_t>>& buf, int& gw, int& gh,
               int scalePct)
{
    if (f.isNull()) return false;
    if (gw == 0) {
        int w = f.width(), h = f.height();
        if (scalePct > 0 && scalePct < 100) {
            w = std::max(16, w * scalePct / 100);
            h = std::max(16, h * scalePct / 100);
        }
        // Even dimensions keep every GIF viewer and video converter happy.
        gw = std::max(2, w & ~1);
        gh = std::max(2, h & ~1);
    }
    if (f.width() != gw || f.height() != gh)
        f = f.scaled(gw, gh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (f.format() != QImage::Format_RGBA8888)
        f = f.convertToFormat(QImage::Format_RGBA8888);
    std::vector<uint8_t> px((size_t)gw * gh * 4);
    for (int y = 0; y < gh; ++y)
        std::memcpy(px.data() + (size_t)y * gw * 4, f.constScanLine(y),
                    (size_t)gw * 4);
    buf.push_back(std::move(px));
    return true;
}

// Trim every frame to the union of the drawn area. A per-frame box would make
// the subject swim, so the box is computed once across the whole sequence.
// Threshold is 8 rather than 0: anti-aliased edges and faint glow would
// otherwise drag the box back out to the frame border.
bool cropFramesToModel(std::vector<std::vector<uint8_t>>& frames, int& w, int& h,
                       bool keepAlpha)
{
    if (frames.empty() || w <= 0 || h <= 0) return false;
    constexpr int kMinAlpha = 8, kPad = 4;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (const std::vector<uint8_t>& f : frames) {
        if (f.size() != (size_t)w * h * 4) return false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (f[((size_t)y * w + x) * 4 + 3] >= kMinAlpha) {
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
    }
    if (x1 < 0) return false;                      // nothing was drawn
    x0 = std::max(0, x0 - kPad);  y0 = std::max(0, y0 - kPad);
    x1 = std::min(w - 1, x1 + kPad);  y1 = std::min(h - 1, y1 + kPad);
    int nw = std::max(2, (x1 - x0 + 1) & ~1);
    int nh = std::max(2, (y1 - y0 + 1) & ~1);
    if (nw == w && nh == h && keepAlpha) return false;   // nothing to do
    for (std::vector<uint8_t>& f : frames) {
        std::vector<uint8_t> out((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y) {
            const uint8_t* src = f.data() + ((size_t)(y0 + y) * w + x0) * 4;
            uint8_t* dst = out.data() + (size_t)y * nw * 4;
            std::memcpy(dst, src, (size_t)nw * 4);
            // Coverage alpha has served its purpose once the box is known.
            if (!keepAlpha)
                for (int x = 0; x < nw; ++x) dst[(size_t)x * 4 + 3] = 255;
        }
        f.swap(out);
    }
    w = nw; h = nh;
    return true;
}

void downscaleFrames(std::vector<std::vector<uint8_t>>& frames, int& w, int& h,
                     int nw, int nh)
{
    if (nw <= 0 || nh <= 0 || (nw == w && nh == h)) return;
    nw = std::max(2, nw & ~1);
    nh = std::max(2, nh & ~1);
    for (std::vector<uint8_t>& f : frames) {
        QImage img((const uchar*)f.data(), w, h, w * 4, QImage::Format_RGBA8888);
        QImage sm = img.scaled(nw, nh, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_RGBA8888);
        std::vector<uint8_t> out((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y)
            std::memcpy(out.data() + (size_t)y * nw * 4, sm.constScanLine(y),
                        (size_t)nw * 4);
        f.swap(out);
    }
    w = nw; h = nh;
}

// Encode, and when the size budget is on, re-encode (never re-render) until the
// result fits. Order matters: palette first, then dither, then resolution —
// and the SMALLEST encode seen is what ships, because a later pass can overshoot.
bool encodeWithBudget(const QString& path, std::vector<std::vector<uint8_t>>& buf,
                      int gw, int gh, int delayCs, bool loop, int transThresh,
                      int maxColors, QString* why)
{
    QSettings s;
    const bool optimize = s.value(QStringLiteral("export/gifOptimize"), false).toBool();
    const qint64 targetBytes =
        qint64(qBound(1, s.value(QStringLiteral("export/gifTargetMB"), 10).toInt(), 200))
        * 1024 * 1024;
    const bool wantDither = s.value(QStringLiteral("export/gifDither"), true).toBool();

    std::vector<uint8_t> bytes, best;
    auto attempt = [&](int colors, bool dither) -> qint64 {
        if (!GifEncoder::encodeToBuffer(bytes, buf, gw, gh, delayCs, loop,
                                        transThresh, colors, dither))
            return -1;
        if (best.empty() || bytes.size() < best.size()) best = bytes;
        return qint64(bytes.size());
    };

    int colors = maxColors;
    qint64 sz = attempt(colors, wantDither);
    if (sz < 0) {
        if (why) *why = QStringLiteral("encoder rejected the frames");
        return false;
    }
    if (optimize && sz > targetBytes) {
        while (sz > targetBytes && colors > 32) {
            colors = std::max(32, colors * 3 / 4);
            const qint64 n = attempt(colors, wantDither);
            if (n < 0) break;
            sz = n;
        }
        bool ditherNow = wantDither;
        if (sz > targetBytes && wantDither) {
            const qint64 n = attempt(colors, false);
            if (n > 0) { sz = n; ditherNow = false; }
        }
        for (int pass = 0; pass < 5 && sz > targetBytes; ++pass) {
            const double ratio = double(targetBytes) / double(sz);
            const double k = qBound(0.35, std::sqrt(ratio) * 0.93, 0.92);
            const int nw = std::max(96, int(gw * k));
            const int nh = std::max(96, int(gh * k));
            if (nw >= gw && nh >= gh) break;         // at the floor
            downscaleFrames(buf, gw, gh, nw, nh);
            const qint64 n = attempt(colors, ditherNow);
            if (n < 0) break;
            sz = n;
        }
        if (sz > targetBytes && why)
            *why = QStringLiteral("target size not reachable — wrote the smallest encode");
    }
    if (best.empty()) {
        if (why) *why = QStringLiteral("encoder produced no data");
        return false;
    }
    if (!GifEncoder::writeBuffer(path.toStdString(), best)) {
        if (why) *why = QStringLiteral("could not write the file");
        return false;
    }
    return true;
}

}   // namespace

namespace ExportCapture {

QString imageFormat()
{
    const QString f = QSettings()
                          .value(QStringLiteral("export/imageFormat"),
                                 QStringLiteral("png"))
                          .toString()
                          .toLower();
    return (f == QLatin1String("jpg") || f == QLatin1String("webp"))
               ? f
               : QStringLiteral("png");
}

bool saveImage(GLMeshView* view, const QString& path)
{
    if (!view) return false;
    GLMeshView::CaptureScope capture(view);

    // The container comes from the PATH, not the setting — the user can always
    // override the default extension in the dialog.
    const QString ext = QFileInfo(path).suffix().toLower();
    const bool alphaOk = (ext != QLatin1String("jpg") && ext != QLatin1String("jpeg"));
    const bool wantT = transparentEnabled() && alphaOk;
    const bool wantCrop = cropEnabled();
    const bool coverage = wantCrop && !wantT;

    QSettings s;
    const int pct = qBound(25, s.value(QStringLiteral("export/imageScale"), 100).toInt(), 400);

    QImage img;
    if (pct > 100) {
        const int factor = qBound(2, (pct + 99) / 100, 4);
        if (wantT)    view->setTransparentClear(true);
        if (coverage) view->setCoverageAlpha(true);
        img = view->grabSupersampled(factor);
        if (wantT)    view->setTransparentClear(false);
        if (coverage) view->setCoverageAlpha(false);
        if (!img.isNull() && img.format() != QImage::Format_RGBA8888)
            img = img.convertToFormat(QImage::Format_RGBA8888);
        if (img.isNull())                       // driver refused — ship 1x
            img = captureFrame(view, wantT, coverage);
        // Resample to the REQUESTED percentage, not the whole supersample factor.
        const int tw = std::max(1, int(qint64(view->width()) * pct / 100));
        if (!img.isNull() && img.width() != tw)
            img = img.scaled(tw, std::max(1, img.height() * tw / img.width()),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else {
        img = captureFrame(view, wantT, coverage);
        if (!img.isNull() && pct < 100)
            img = img.scaled(std::max(1, img.width() * pct / 100),
                             std::max(1, img.height() * pct / 100),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (img.isNull()) return false;

    if (wantCrop) {
        int cw = img.width(), ch = img.height();
        std::vector<std::vector<uint8_t>> one;
        int gw = 0, gh = 0;
        if (pushFrame(img, one, gw, gh, 100)) {
            cw = gw; ch = gh;
            if (cropFramesToModel(one, cw, ch, /*keepAlpha=*/wantT)) {
                img = QImage((const uchar*)one[0].data(), cw, ch, cw * 4,
                             QImage::Format_RGBA8888)
                          .copy();               // the vector dies at scope end
            } else if (coverage) {
                // Crop found nothing (or nothing to trim). The coverage grab left
                // the background at alpha 0, which the user did not ask for —
                // put it back before writing.
                img = img.convertToFormat(QImage::Format_RGBA8888);
                for (int y = 0; y < img.height(); ++y) {
                    uchar* sl = img.scanLine(y);
                    for (int x = 0; x < img.width(); ++x) sl[x * 4 + 3] = 255;
                }
            }
        }
    } else if (coverage) {
        img = img.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < img.height(); ++y) {
            uchar* sl = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) sl[x * 4 + 3] = 255;
        }
    }

    if (!alphaOk) img = img.convertToFormat(QImage::Format_RGB888);
    const int q = qBound(1, s.value(QStringLiteral("export/imageQuality"), 92).toInt(), 100);
    const bool lossy = (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") ||
                        ext == QLatin1String("webp"));
    return img.save(path, nullptr, lossy ? q : -1);
}

bool turntableGif(GLMeshView* view, const QString& path, const AnimSource& anim,
                  const ProgressFn& progress)
{
    if (!view) return false;
    GLMeshView::CaptureScope capture(view);   // before any early-out

    QSettings s;
    const bool wantT = transparentEnabled();
    int frames = qBound(8, s.value(QStringLiteral("export/gifTurntableFrames"), 48).toInt(), 240);
    const int fps = qBound(1, s.value(QStringLiteral("export/gifFps"), 25).toInt(), 60);
    const int delayCs = std::max(2, 100 / fps);
    const int scalePct = qBound(25, s.value(QStringLiteral("export/gifScale"), 100).toInt(), 100);
    const int maxCols = qBound(16, s.value(QStringLiteral("export/gifMaxColors"), 256).toInt(), 256);
    const bool cropToModel = cropEnabled() && !wantT;

    const float startYaw = view->yaw();
    const int clipN = (anim.frameCount > 0 && anim.seek) ? anim.frameCount : 0;

    // Sync the clip to the revolution when one is playing: round the requested
    // frame count to a whole number of clip cycles so the GIF's wrap is seamless
    // in pose as well as in camera angle.
    bool authoredRate = false;
    if (clipN > 0) {
        const int loops = std::max(1, int(std::lround(double(frames) / double(clipN))));
        if (loops * clipN <= 240) { frames = std::max(8, loops * clipN); authoredRate = true; }
    }
    auto clipFrameFor = [&](int i) {
        return authoredRate ? (i % clipN) : int(qint64(i) * clipN / frames);
    };

    std::vector<std::vector<uint8_t>> buf;
    int gw = 0, gh = 0;
    auto restore = [&] {
        view->setYaw(startYaw);
        if (clipN > 0) anim.seek(anim.savedFrame);
    };

    for (int i = 0; i < frames; ++i) {
        // i/frames (not frames-1): the last step stops one short of a full turn,
        // so the loop has no duplicated frame.
        view->setYaw(startYaw + float(i) / float(frames) * 2.0f * float(M_PI));
        if (clipN > 0) anim.seek(clipFrameFor(i));
        view->setCaptureTime(float(i) * float(delayCs) * 0.01f);
        pushFrame(captureFrame(view, wantT, cropToModel), buf, gw, gh, scalePct);
        if (progress && !progress(i + 1, frames)) { restore(); return false; }
    }
    restore();
    if (buf.empty() || gw == 0) return false;
    if (cropEnabled()) cropFramesToModel(buf, gw, gh, /*keepAlpha=*/wantT);
    return encodeWithBudget(path, buf, gw, gh, delayCs, /*loop=*/true,
                            wantT ? 128 : -1, maxCols, nullptr);
}

bool animLoopGif(GLMeshView* view, const QString& path, const AnimSource& anim,
                 const ProgressFn& progress)
{
    if (!view || anim.frameCount <= 0 || !anim.seek) return false;
    GLMeshView::CaptureScope capture(view);

    QSettings s;
    const bool wantT = transparentEnabled();
    const int n = anim.frameCount;
    const float fr = anim.fps > 1.0f ? anim.fps : 30.0f;
    // The clip's own rate, not export/gifFps — an animation loop must play at the
    // speed it was authored at.
    const int delayCs = qBound(2, int(std::lround(100.0 / double(fr))), 100);
    const int scalePct = qBound(25, s.value(QStringLiteral("export/gifScale"), 100).toInt(), 100);
    const int maxCols = qBound(16, s.value(QStringLiteral("export/gifMaxColors"), 256).toInt(), 256);
    const bool cropToModel = cropEnabled() && !wantT;

    std::vector<std::vector<uint8_t>> buf;
    int gw = 0, gh = 0;
    for (int f = 0; f < n; ++f) {
        anim.seek(f);
        view->setCaptureTime(float(f) * float(delayCs) * 0.01f);
        pushFrame(captureFrame(view, wantT, cropToModel), buf, gw, gh, scalePct);
        if (progress && !progress(f + 1, n)) { anim.seek(anim.savedFrame); return false; }
    }
    anim.seek(anim.savedFrame);
    if (buf.empty() || gw == 0) return false;
    if (cropEnabled()) cropFramesToModel(buf, gw, gh, /*keepAlpha=*/wantT);
    return encodeWithBudget(path, buf, gw, gh, delayCs, /*loop=*/true,
                            wantT ? 128 : -1, maxCols, nullptr);
}

// ── Full flows ─────────────────────────────────────────────────────────────

namespace {

QString askPath(QWidget* parent, const QString& title, const QString& baseName,
                const QString& ext, const QString& filter)
{
    QSettings s;
    QString base = baseName.trimmed();
    if (base.isEmpty()) base = QStringLiteral("viewport");
    const QString dir = s.value(QLatin1String(kDirKey)).toString();
    const QString seed = (dir.isEmpty() ? base : dir + QLatin1Char('/') + base) +
                         QLatin1Char('.') + ext;
    QString path = QFileDialog::getSaveFileName(parent, title, seed, filter);
    if (path.isEmpty()) return {};
    // Qt appends the filter's suffix on Windows but not everywhere; do it here so
    // the file is openable on every platform.
    if (QFileInfo(path).suffix().isEmpty()) path += QLatin1Char('.') + ext;
    s.setValue(QLatin1String(kDirKey), QFileInfo(path).absolutePath());
    return path;
}

// A cancellable progress dialog wired to the capture's ProgressFn.
struct Progress {
    QProgressDialog dlg;
    explicit Progress(QWidget* parent, const QString& label, int total)
        : dlg(label, QStringLiteral("Cancel"), 0, total, parent)
    {
        dlg.setWindowModality(Qt::WindowModal);
        dlg.setMinimumDuration(400);   // instant captures never flash a dialog
        dlg.setAutoClose(false);
        dlg.setAutoReset(false);
        dlg.setValue(0);
    }
    ExportCapture::ProgressFn fn()
    {
        return [this](int done, int total) {
            if (dlg.maximum() != total) dlg.setMaximum(total);
            dlg.setValue(done);
            QApplication::processEvents();
            return !dlg.wasCanceled();
        };
    }
};

}   // namespace

bool runSaveImage(QWidget* parent, GLMeshView* view, const QString& baseName,
                  QString* outMsg)
{
    QString msg;
    if (!view) { if (outMsg) *outMsg = QStringLiteral("no viewport"); return false; }
    const QString ext = imageFormat();
    const QString filter =
        ext == QLatin1String("jpg")  ? QStringLiteral("JPEG image (*.jpg)")
      : ext == QLatin1String("webp") ? QStringLiteral("WebP image (*.webp)")
                                     : QStringLiteral("PNG image (*.png)");
    const QString path = askPath(parent, QStringLiteral("Save preview image"),
                                 baseName, ext, filter);
    if (path.isEmpty()) { if (outMsg) outMsg->clear(); return false; }
    const bool ok = saveImage(view, path);
    msg = ok ? QStringLiteral("Saved image → %1").arg(QFileInfo(path).fileName())
             : QStringLiteral("image save FAILED (%1)").arg(QFileInfo(path).fileName());
    if (outMsg) *outMsg = msg;
    return ok;
}

bool runTurntableGif(QWidget* parent, GLMeshView* view, const QString& baseName,
                     const AnimSource& anim, QString* outMsg)
{
    if (!view) { if (outMsg) *outMsg = QStringLiteral("no viewport"); return false; }
    const QString path = askPath(parent, QStringLiteral("Save turntable GIF"),
                                 baseName, QStringLiteral("gif"),
                                 QStringLiteral("GIF animation (*.gif)"));
    if (path.isEmpty()) { if (outMsg) outMsg->clear(); return false; }
    const int total = qBound(8,
        QSettings().value(QStringLiteral("export/gifTurntableFrames"), 48).toInt(), 240);
    Progress p(parent, QStringLiteral("Rendering turntable GIF…"), total);
    const bool ok = turntableGif(view, path, anim, p.fn());
    p.dlg.close();
    if (outMsg)
        *outMsg = ok ? QStringLiteral("Saved turntable GIF → %1")
                           .arg(QFileInfo(path).fileName())
                     : (p.dlg.wasCanceled()
                            ? QStringLiteral("turntable GIF cancelled")
                            : QStringLiteral("turntable GIF FAILED"));
    return ok;
}

bool runAnimLoopGif(QWidget* parent, GLMeshView* view, const QString& baseName,
                    const AnimSource& anim, QString* outMsg)
{
    if (!view) { if (outMsg) *outMsg = QStringLiteral("no viewport"); return false; }
    if (anim.frameCount <= 0 || !anim.seek) {
        if (outMsg) *outMsg = QStringLiteral("no animation is playing");
        return false;
    }
    const QString path = askPath(parent, QStringLiteral("Save animation GIF"),
                                 baseName + QStringLiteral("_anim"),
                                 QStringLiteral("gif"),
                                 QStringLiteral("GIF animation (*.gif)"));
    if (path.isEmpty()) { if (outMsg) outMsg->clear(); return false; }
    Progress p(parent, QStringLiteral("Rendering animation GIF…"), anim.frameCount);
    const bool ok = animLoopGif(view, path, anim, p.fn());
    p.dlg.close();
    if (outMsg)
        *outMsg = ok ? QStringLiteral("Saved animation GIF → %1")
                           .arg(QFileInfo(path).fileName())
                     : (p.dlg.wasCanceled()
                            ? QStringLiteral("animation GIF cancelled")
                            : QStringLiteral("animation GIF FAILED"));
    return ok;
}

}   // namespace ExportCapture
