#pragma once
// App-wide export notifications — ported from D4AssetBrowser.
//
// Any tab calls ExportNotifier::instance().notify(summary, folder) after an
// export finishes; MainWindow listens and shows ONE consistent toast with a
// "Show in folder" action. Before this, each tab reported exports its own way —
// a status-bar line here, an info-panel note there — so the same operation read
// differently depending on where you started it.

#include <QObject>
#include <QString>
#include <QStringList>

class ExportNotifier : public QObject {
    Q_OBJECT
public:
    static ExportNotifier& instance()
    {
        static ExportNotifier n;
        return n;
    }
    // `folder` is the directory to reveal (omit for none). `text` is a short
    // human summary. `failed` > 0 makes the toast report the failures too.
    void notify(const QString& text, const QString& folder = QString(),
                int failed = 0)
    {
        emit exported(text, folder, failed);
    }

    // One line describing what an export's OPTIONS did, for appending to the
    // toast: "  ·  textures embedded, 12 clips, raw sources". Built from the
    // resolved facts of the file that was written, never from settings — a
    // settings-derived line would confidently name options a given path does
    // not honour, which is worse than saying nothing.
    static QString optionsLine(bool embeddedTex, bool looseTex, bool rawDeps,
                               int animCount)
    {
        QStringList parts;
        parts << (embeddedTex ? QStringLiteral("textures embedded")
                              : QStringLiteral("no textures"));
        if (looseTex) parts << QStringLiteral("loose PNGs");
        if (rawDeps)  parts << QStringLiteral("raw sources");
        if (animCount > 0)
            parts << QStringLiteral("%1 clip%2").arg(animCount)
                         .arg(animCount == 1 ? QString() : QStringLiteral("s"));
        return QStringLiteral("  ·  ") + parts.join(QStringLiteral(", "));
    }

signals:
    void exported(const QString& text, const QString& folder, int failed);

private:
    ExportNotifier() = default;
};
