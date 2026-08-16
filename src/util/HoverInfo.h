#pragma once
// Ported from the D4 browser. Read-through layer over the hover-preview
// settings so every tab's popup honours ONE set of knobs, plus the shared
// colour palette for info lines. There is no settings UI for these yet —
// they live in data\DIAssetBrowser.ini under [hover] (edit-settings.bat
// opens it); every knob has a sensible default so nothing needs configuring.
//
//   hover/enabled       master gate                      (default true)
//   hover/delaySec      dwell before the popup shows     (default 0.5, 0..5)
//   hover/imagePreview  include the thumbnail image      (default true)
//   hover/scrollZoom    wheel over the popup resizes it  (default true)
//   hover/previewPx     image edge in pixels             (default 256, 64..1024)
//   hover/colour        colour-code the info lines       (default true)
//   hover/<tab>/<line>  per-line toggles, e.g. hover/tex/meaning = false

#include <QColor>
#include <QSettings>
#include <QString>

namespace HoverInfo {

inline bool enabled()
{
    return QSettings().value(QStringLiteral("hover/enabled"), true).toBool();
}

inline int delayMs()
{
    const double s =
        QSettings().value(QStringLiteral("hover/delaySec"), 0.5).toDouble();
    return (int)(qBound(0.0, s, 5.0) * 1000.0);
}

inline bool imagePreview()
{
    return QSettings().value(QStringLiteral("hover/imagePreview"), true).toBool();
}

inline bool scrollZoom()
{
    return QSettings().value(QStringLiteral("hover/scrollZoom"), true).toBool();
}

inline int previewPx()
{
    return qBound(64,
                  QSettings().value(QStringLiteral("hover/previewPx"), 256).toInt(),
                  1024);
}

inline bool colourCode()
{
    return QSettings().value(QStringLiteral("hover/colour"), true).toBool();
}

// Per-line toggle: on("tex/meaning") reads hover/tex/meaning (default on).
inline bool on(const char* key, bool def = true)
{
    return QSettings()
        .value(QStringLiteral("hover/") + QLatin1String(key), def)
        .toBool();
}

// Shared line palette (D4 vocabulary): name white, file grey, series gold,
// info blue, good green, meta light grey, new ember.
namespace Col {
inline const QColor kName{0xff, 0xff, 0xff};
inline const QColor kFile{0x9a, 0x9a, 0x9a};
inline const QColor kSeries{0xe8, 0xc4, 0x6a};
inline const QColor kInfo{0x7f, 0xb2, 0xe5};
inline const QColor kGood{0x8f, 0xbf, 0x8f};
inline const QColor kMeta{0xb0, 0xb0, 0xb0};
inline const QColor kNew{0xe0, 0x80, 0x3c};
}  // namespace Col

}  // namespace HoverInfo
