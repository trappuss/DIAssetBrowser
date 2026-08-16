#pragma once
// How a multi-model export lays its files out on disk — ONE implementation,
// shared by every DI path that writes more than one model. Ported from
// D4AssetBrowser's ExportLayout.
//
// D4 groups by AppearanceMeta tag ("Class", "Type"). DI has no tag index, but it
// has something better for the same job: every asset already carries its
// repository FOLDER and TYPE, which are exactly the two axes people group by.
// So the modes are Flat / by Type / by Folder / by Model.
//
// The layout picks the GROUP FOLDER only. What appears inside each group — the
// models plus textures\ and deps\ for whichever options are on — is identical in
// every mode, which is what makes Flat simply "one group, at the root" rather
// than a special case.
//
// The rule is the ENTRY POINT, not the item count: single-model paths pass
// applyLayout=false, because a global "by Folder" silently turning Ctrl+E into
// Char/f_barbarian/foo.glb is a surprise the caller cannot undo. Batch paths
// pass true even for one row — exporting one more barbarian into a folder you
// already grouped belongs with the rest, not loose at the top.

#include <QDir>
#include <QMap>
#include <QPair>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

#include "util/NameTemplate.h"

namespace ExportLayout {

// Stored in export/folderLayout as a STABLE STRING, never the combo index:
// inserting a mode would reorder every saved value.
inline QString kFlat()   { return QString(); }
inline QString kType()   { return QStringLiteral("_type"); }
inline QString kFolder() { return QStringLiteral("_folder"); }
inline QString kModel()  { return QStringLiteral("_model"); }

inline bool isKnown(const QString& mode)
{
    return mode.isEmpty() || mode == kType() || mode == kFolder() || mode == kModel();
}

// The mode in force. An unrecognised value — a newer build's mode, a hand-edited
// INI — reads as Flat: failing to where the user pointed beats inventing a
// folder they never asked for.
inline QString mode()
{
    const QString m = QSettings().value(QStringLiteral("export/folderLayout")).toString();
    return isKnown(m) ? m : kFlat();
}

// One item to export: the repo index, its asset name, its repository type and
// folder. Type/folder are captured by the caller (which already has the entry in
// hand) so grouping never has to re-walk the repository per item.
struct Item {
    int     idx = -1;
    QString name;
    QString type;      // "Model" / "Mesh" / …
    QString folder;    // "Char/f_barbarian" / "Char/item" / …
};

// Sanitise one path segment. NameTemplate strips separators but knows nothing
// about directory rules: a trailing dot or a reserved device name makes mkpath
// fail, and every write into that folder then fails with nothing in the log.
inline QString safeSegment(QString stem, const QString& fallback)
{
    stem = stem.trimmed();
    while (stem.endsWith(QLatin1Char('.'))) stem.chop(1);
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    if (kReserved.contains(stem, Qt::CaseInsensitive)) stem.prepend(QLatin1Char('_'));
    if (stem.isEmpty()) stem = fallback;
    return stem.isEmpty() ? QStringLiteral("misc") : stem;
}

struct Group {
    QString       folder;   // empty = the destination itself (Flat)
    QVector<Item> items;
};

// Split `items` into destination groups. Flat returns exactly one group with an
// empty folder, so callers treat every mode uniformly.
inline QVector<Group> group(const QString& m, const QVector<Item>& items)
{
    if (m.isEmpty() || !isKnown(m) || items.isEmpty())
        return {Group{QString(), items}};

    // QMap, not QHash: folders come out in a stable order, so the log and the
    // progress read the same way on every run over the same selection.
    QMap<QString, QVector<Item>> by;
    const QString tpl = QSettings()
                            .value(QStringLiteral("export/nameModel"),
                                   NameTemplate::kModelDefault())
                            .toString();
    for (const Item& it : items) {
        QString key;
        if (m == kModel()) {
            // The .glb's own stem, so folder and file always agree.
            key = safeSegment(NameTemplate::apply(tpl, it.name, it.idx), it.name);
        } else if (m == kType()) {
            key = safeSegment(it.type, QStringLiteral("misc"));
        } else {   // kFolder — the repository folder, flattened to one segment
            QString f = it.folder;
            f.replace(QLatin1Char('/'), QLatin1Char('_'));
            key = safeSegment(f, QStringLiteral("misc"));
        }
        by[key].append(it);
    }
    QVector<Group> out;
    out.reserve(by.size());
    for (auto g = by.constBegin(); g != by.constEnd(); ++g)
        out.append(Group{g.key(), g.value()});
    return out;
}

inline QString folderFor(const QString& root, const Group& g)
{
    return g.folder.isEmpty() ? root : QDir(root).filePath(g.folder);
}

}   // namespace ExportLayout
