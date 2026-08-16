#pragma once
// ── Which animations an export embeds ──────────────────────────────────────
// Ported from D4AssetBrowser's AnimExportScope and retrofitted to DI, whose
// clip ownership works differently: D4 resolves clips through snoAppearance,
// while DI finds them by FOLDER — a model's clips live in its own folder's
// /ani/, walking up (di::collectAnimsUnder), and a class rig's clips are found
// by di::findClassAnims.
//
// THREE sources, because they answer different questions:
//
//   original   clips found near the model itself (its folder's /ani/, walking
//              up). For a class body this is the playable set. For a single
//              armour piece it is usually EMPTY — armour carries no clips of
//              its own, which is exactly why `base` exists.
//   previewed  the single clip playing in the viewport right now. Loaded model
//              only; a batch item that is not loaded has no previewed clip.
//   base       the CLASS RIG's clips, which a piece inherits. This is the one
//              that matters in the Wardrobe: an outfit is a pile of armour
//              pieces, none of which own a clip, all of which are driven by the
//              class's animations. Deduped against `original` by name, so a
//              clip the model already owns is never embedded twice.
//
// Deliberately NOT a source: anything guessed by skeleton similarity. A guess is
// a viewing aid; it does not belong in a file you ship.
//
// The two CLIP FILTERS below apply to original and base only. A clip you are
// previewing is an explicit choice and is never filtered out.

#include <QSettings>
#include <QString>
#include <QStringList>

struct AnimExportScope {
    bool includeAnim = false;   // master switch (export/includeAnim)
    bool original    = true;
    bool previewed   = true;
    bool base        = false;

    int         maxFrames = 0;   // 0 = no limit
    QStringList exclude;         // case-insensitive "contains" filters

    static AnimExportScope load()
    {
        QSettings s;
        AnimExportScope sc;
        sc.includeAnim = s.value(QStringLiteral("export/includeAnim"), false).toBool();
        sc.original    = s.value(QStringLiteral("export/animOriginal"), true).toBool();
        sc.previewed   = s.value(QStringLiteral("export/animPreviewed"), true).toBool();
        sc.base        = s.value(QStringLiteral("export/animBase"), false).toBool();
        sc.maxFrames   = qBound(0, s.value(QStringLiteral("export/animMaxFrames"), 0).toInt(), 5000);
        const QString ex = s.value(QStringLiteral("export/animExclude")).toString();
        for (const QString& part : ex.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString t = part.trimmed();
            if (!t.isEmpty()) sc.exclude << t;
        }
        // The legacy export/animMode enum is NOT handled here: it is a schema
        // change, and app/SettingsSchema.h migrates it once at startup rather
        // than re-checking (and writing settings) on every export, from
        // whatever thread happens to be exporting.
        return sc;
    }

    // True when this clip should be embedded. `frames` <= 0 means unknown, which
    // never trips the length filter — a filter must not drop what it cannot
    // measure. `explicitPick` marks the previewed clip, which is never filtered.
    bool accepts(const QString& name, int frames, bool explicitPick = false) const
    {
        if (explicitPick) return true;
        if (maxFrames > 0 && frames > maxFrames) return false;
        for (const QString& e : exclude)
            if (name.contains(e, Qt::CaseInsensitive)) return false;
        return true;
    }

    // Any data-derived source on? (previewed is GUI state, not a data source.)
    bool anyDataSource() const { return original || base; }

    QString describe() const
    {
        if (!includeAnim) return QStringLiteral("none");
        QStringList p;
        if (original)  p << QStringLiteral("original");
        if (previewed) p << QStringLiteral("previewed");
        if (base)      p << QStringLiteral("base");
        return p.isEmpty() ? QStringLiteral("nothing selected") : p.join(QLatin1Char('+'));
    }
};
