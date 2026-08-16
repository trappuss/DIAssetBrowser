#pragma once
// One vocabulary for every context menu in the tool — ported from the D4 browser's
// MenuText, retrofitted to DI identifiers (no SNOs here: an asset is identified by
// its repository display path, its decoded MEANING, and its physical MPK path).
//
// The D4 tool learned this the hard way: the same clipboard action had drifted to
// "Copy SNO id" / "Copy SNO" / "Copy source SNO ID" across tabs, and — worst of the
// set — "Save image" saved silently to the last folder while "Save image…" opened a
// file dialog: one ellipsis apart, opposite behaviours.
//
// Rules, so additions stay consistent:
//   * Sentence case. "Export part", not "Export Part".
//   * A trailing "…" means AND ONLY MEANS "this opens a dialog". Never decorative.
//   * An action that writes somewhere remembered says where:
//     "… to last folder (…/DI/exports)" via condensePath().
//   * Copy actions carry their value in parentheses via withValue(), so you can read
//     what you are about to copy without invoking it.
// Anything user-visible in a context menu belongs here rather than inline at the
// call site.

#include <QDir>
#include <QLocale>
#include <QString>
#include <QStringList>

namespace MenuText {

inline const QString kCopyName    = QStringLiteral("Copy name");      // display path
inline const QString kCopyMeaning = QStringLiteral("Copy meaning");   // decoded gloss / true name
inline const QString kCopyMpkPath = QStringLiteral("Copy MPK path");  // physical entry name

// No ellipsis baked in: these get suffixes appended ("Export model (1,234 tris)"),
// and an ellipsis stranded mid-label reads as a typo. Wrap the FINISHED string in
// prompts() instead.
inline const QString kExportModel     = QStringLiteral("Export model");
inline const QString kExportModelLast = QStringLiteral("Export model to last folder");

inline const QString kCopyImage     = QStringLiteral("Copy image");
inline const QString kSaveImage     = QStringLiteral("Save image…");               // prompts
inline const QString kSaveImageLast = QStringLiteral("Save image to last folder"); // silent

// "C:/Users/me/Documents/DI/exports" -> "…/DI/exports". A full path makes the menu
// unreadable; the last two components are what distinguishes one folder from another.
inline QString condensePath(const QString& path)
{
    if (path.isEmpty()) return {};
    const QString clean = QDir::fromNativeSeparators(QDir::cleanPath(path));
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() <= 2) return QDir::toNativeSeparators(clean);
    return QStringLiteral("…/%1/%2").arg(parts[parts.size() - 2], parts.last());
}

inline QString withCount(const QString& label, int n)
{
    return n > 0 ? QStringLiteral("%1 (%2 tris)").arg(label, QLocale().toString(n)) : label;
}

inline QString withValue(const QString& label, const QString& value)
{
    if (value.isEmpty()) return label;
    const QString v = value.size() > 40 ? value.left(37) + QStringLiteral("…") : value;
    return QStringLiteral("%1 (%2)").arg(label, v);
}

// The ONLY way an ellipsis gets onto a label. Applied last, to the finished string.
inline QString prompts(const QString& label) { return label + QStringLiteral("…"); }

// List menus act on a COUNTED set of things — "3 textures", "12 models".
inline QString exportSetPrompt(const QString& what)
{
    return prompts(QStringLiteral("Export %1").arg(what));
}
inline QString exportSetLast(const QString& what, const QString& dir)
{
    return withValue(QStringLiteral("Export %1 to last folder").arg(what), dir);
}

}  // namespace MenuText
