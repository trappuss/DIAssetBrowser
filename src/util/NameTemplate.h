#pragma once
// Export filename templates — ported from D4AssetBrowser's NameTemplate and
// retrofitted to DI's identifiers (DI has no SNO; assets are named entries in
// resource.repository, addressed by repo index / MPK entry id).
//
// Placeholders, case-insensitive:
//   {{FileName}}  the asset's repository name ("f_barbarian_yifu_t07_004")
//   {{Id}}        the repository index
//   {{Meaning}}   the decoded cosmetic name, when one decodes
// Outfit templates additionally take:
//   {{Class}} {{Gender}} {{Set}} {{Date}} and the per-slot names
//   {{Helmet}} {{Chest}} {{Shoulders}} {{Legs}} {{Hair}} {{Main}} {{Off}}
//
// Configured in Settings ▸ Export ▸ File names. Defaults reproduce the names the
// tool used before templates existed, so an existing workflow is unchanged.

#include <QDate>
#include <QHash>
#include <QRegularExpression>
#include <QSettings>
#include <QString>

namespace NameTemplate {

// Substitute, sanitise, and never return empty.
//
// A placeholder this kind of export has no value for is REMOVED, along with the
// separator it orphans. Templates are shared across export kinds that do not
// all carry the same fields — {{Set}} means nothing to a single model,
// {{Gender}} means nothing to a shared item-folder weapon — and leaving the
// unresolved ones in produced file names like
// "{{Gender}}_m_warlock_toukui_beard_t06_009".
//
// The cost of this is real and is handled elsewhere: stripping fields makes two
// different assets more likely to render the SAME name, so every caller that
// writes more than one file per run claims its names through a uniqueness pass
// (and export/overwrite decides what happens when the name already exists on
// disk). Silently overwriting was never acceptable; printing "{{Gender}}" into a
// filename to avoid it was not either.
inline QString applyMap(QString tpl, const QHash<QString, QString>& vals)
{
    for (auto it = vals.constBegin(); it != vals.constEnd(); ++it) {
        // Case-insensitive: {{class}} and {{Class}} are the same placeholder.
        const QRegularExpression re(
            QStringLiteral("\\{\\{\\s*%1\\s*\\}\\}").arg(QRegularExpression::escape(it.key())),
            QRegularExpression::CaseInsensitiveOption);
        tpl.replace(re, it.value());
    }
    // Anything still in {{...}} had no value for this export kind: drop it and
    // let the separator collapse below tidy up after it.
    static const QRegularExpression leftover(QStringLiteral("\\{\\{[^}]*\\}\\}"));
    tpl.remove(leftover);
    // Empty fields leave gaps; collapse the separators they orphan so a missing
    // slot does not turn into "chest__legs" or a leading underscore.
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    tpl.replace(bad, QStringLiteral("_"));
    tpl = tpl.simplified();
    static const QRegularExpression dupUs(QStringLiteral("_{2,}"));
    tpl.replace(dupUs, QStringLiteral("_"));
    static const QRegularExpression edgeUs(QStringLiteral("^[_\\-\\s]+|[_\\-\\s]+$"));
    tpl.remove(edgeUs);
    return tpl;
}

inline QString apply(const QString& tpl, const QString& fileName, int id,
                     const QString& meaning = QString())
{
    QHash<QString, QString> v;
    v.insert(QStringLiteral("FileName"), fileName);
    v.insert(QStringLiteral("Id"), QString::number(id));
    v.insert(QStringLiteral("Meaning"), meaning);
    const QString out = applyMap(tpl, v);
    return out.isEmpty() ? fileName : out;   // never produce an empty file name
}

inline QString kModelDefault()   { return QStringLiteral("{{FileName}}"); }
inline QString kTextureDefault() { return QStringLiteral("{{FileName}}"); }
inline QString kOutfitDefault()  { return QStringLiteral("{{Class}}_{{Gender}}_{{Set}}"); }

inline QString model(const QString& fileName, int id, const QString& meaning = QString())
{
    return apply(QSettings()
                     .value(QStringLiteral("export/nameModel"), kModelDefault())
                     .toString(),
                 fileName, id, meaning);
}
inline QString texture(const QString& fileName, int id)
{
    return apply(QSettings()
                     .value(QStringLiteral("export/nameTexture"), kTextureDefault())
                     .toString(),
                 fileName, id);
}

// Outfits carry their own placeholder set; `vals` is filled by the Wardrobe tab.
inline QString outfit(const QHash<QString, QString>& vals, const QString& fallback)
{
    QHash<QString, QString> v = vals;
    if (!v.contains(QStringLiteral("Date")))
        v.insert(QStringLiteral("Date"), QDate::currentDate().toString(Qt::ISODate));
    const QString out = applyMap(
        QSettings()
            .value(QStringLiteral("export/wardrobeNameTemplate"), kOutfitDefault())
            .toString(),
        v);
    return out.isEmpty() ? fallback : out;
}

}   // namespace NameTemplate
