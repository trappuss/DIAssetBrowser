#pragma once
// Export options shared by the Settings menu and the exporting tabs
// (Models, Wardrobe). Persisted via QSettings.

#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QString>

namespace ExportSettings {

// What animation data a .glb export carries.
enum AnimMode {
    AnimNone    = 0,   // bind/rest pose only (smallest file)
    AnimCurrent = 1,   // the clip currently selected in the tab
    AnimAll     = 2,   // every clip tied to the model's folder
};

inline AnimMode animMode()
{
    const int v =
        QSettings().value(QStringLiteral("export/animMode"), 0).toInt();
    return v == 1 ? AnimCurrent : (v == 2 ? AnimAll : AnimNone);
}

inline void setAnimMode(AnimMode m)
{
    QSettings().setValue(QStringLiteral("export/animMode"), (int)m);
}

inline QString animModeName(AnimMode m)
{
    switch (m) {
        case AnimCurrent: return QStringLiteral("currently playing animation");
        case AnimAll:     return QStringLiteral("all animations");
        default:          return QStringLiteral("no animations");
    }
}

// Whether a Models-tab .glb export also bakes in the attachments the user has
// loaded (sibling parts — a body's tail, etc.), each with its own skeleton and
// its matching animation. Persisted; default on.
inline bool includeAttachments()
{
    return QSettings().value(QStringLiteral("export/attachments"), true).toBool();
}

inline void setIncludeAttachments(bool on)
{
    QSettings().setValue(QStringLiteral("export/attachments"), on);
}

// Whether soft-body (cloth / tail / hair) bones are physically simulated in
// playback AND baked into exports. When off, those bones hold their clip/bind
// pose exactly as before. Persisted; default on. Same key backs the viewport
// toggle and the export path so what you see is what you get.
inline bool clothPhysics()
{
    return QSettings().value(QStringLiteral("export/cloth"), true).toBool();
}

inline void setClothPhysics(bool on)
{
    QSettings().setValue(QStringLiteral("export/cloth"), on);
}

// ── Texture and raw-source options (ported from D4AssetBrowser) ────────────
// Embedded and loose are NOT alternatives. Loose is purely additive: the .glb
// keeps its embedded copies and stays one self-contained file. Rewriting the
// glTF to reference external URIs would change what a .glb IS and break every
// caller that assumes it is standalone.

// Embed the decoded material textures in the .glb's binary chunk. Default on.
inline bool includeTextures()
{
    return QSettings().value(QStringLiteral("export/includeTex"), true).toBool();
}

// Also write the exported maps as PNGs into a "textures" folder beside the .glb.
inline bool looseTextures()
{
    return QSettings().value(QStringLiteral("export/looseTextures"), false).toBool();
}

// Also write the RAW game blobs the model came from — its mesh, material,
// skeleton and every texture — into a "deps" folder, exactly as they are stored
// in the MPK (after ZZZ4 inflation). One deps folder per output folder, shared
// by every model written beside it, so a texture ten pieces reference is stored
// once rather than ten times.
inline bool withRawDeps()
{
    return QSettings().value(QStringLiteral("export/withDeps"), false).toBool();
}

// Export the opposite-gender twin of whatever is exported, as a second file.
inline bool bothGenders()
{
    return QSettings().value(QStringLiteral("export/bothGenders"), false).toBool();
}

// Honour the viewport's hidden parts. Off (the default) means the export matches
// what you see; on exports every submesh regardless.
inline bool exportHiddenParts()
{
    return QSettings().value(QStringLiteral("export/hiddenParts"), false).toBool();
}

// What an exported outfit contains.
enum WardrobeScope {
    WardrobeAll   = 0,   // everything shown in the preview
    WardrobeItems = 1,   // the equipped gear only (no base body/face/hair)
};

inline WardrobeScope wardrobeScope()
{
    return QSettings().value(QStringLiteral("export/wardrobeScope"), 0).toInt() == 1
               ? WardrobeItems
               : WardrobeAll;
}

// Raise a desktop/status notification when an export finishes. Default on.
inline bool notifyOnFinish()
{
    return QSettings().value(QStringLiteral("export/osNotify"), true).toBool();
}

// Replace a file that is already there. Default ON, which is what every export
// in this tool has always done — turning it OFF makes a run add to a folder
// instead of rewriting it, which is what you want when the file-name template
// cannot distinguish two assets (see NameTemplate: an unresolved placeholder is
// dropped, so more things share a name than used to).
inline bool overwriteExisting()
{
    return QSettings().value(QStringLiteral("export/overwrite"), true).toBool();
}

// A path that will not clobber anything, honouring overwriteExisting(). Returns
// `path` unchanged when overwriting is allowed or nothing is in the way;
// otherwise inserts "_2", "_3" ... before the extension.
//
// This is best-effort against a race (another process could create the file
// between the check and the write) and deliberately not more: the alternative
// is an exclusive-create dance in every writer, and the failure it would guard
// against is a user running two exports into one folder at once.
inline QString uniquePath(const QString& path)
{
    if (overwriteExisting() || !QFile::exists(path)) return path;
    const QFileInfo fi(path);
    const QString dir  = fi.absolutePath();
    const QString base = fi.completeBaseName();
    const QString ext  = fi.suffix().isEmpty() ? QString()
                                               : QLatin1Char('.') + fi.suffix();
    for (int n = 2; n < 10000; ++n) {
        const QString cand =
            dir + QLatin1Char('/') + base + QStringLiteral("_%1").arg(n) + ext;
        if (!QFile::exists(cand)) return cand;
    }
    return path;   // 10k of one name: give up and let the write decide
}

} // namespace ExportSettings
