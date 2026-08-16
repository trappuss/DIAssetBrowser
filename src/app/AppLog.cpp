#include "app/AppLog.h"

#include "app/AppPaths.h"

#include <QFile>
#include <QMutex>
#include <QSettings>
#include <QTextStream>

namespace {
QFile  g_file;
QMutex g_mtx;
bool   g_on = false;
// The file truncates on launch; within a session it self-wraps past this so a
// heavy run (bulk export, verbose diagnostics) can't grow it without bound.
constexpr qint64 kCapBytes = 4 * 1024 * 1024;   // 4 MB

// caller must hold g_mtx
void openLocked()
{
    if (g_file.isOpen()) return;
    g_file.setFileName(applog::logPath());
    g_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    g_on = g_file.isOpen();
}
} // namespace

namespace applog {

QString logPath()
{
    return AppPaths::file(QStringLiteral("DIAssetBrowser.log"));
}

bool fileLogging()
{
    return QSettings().value(QStringLiteral("log/fileEnabled"), true).toBool();
}

void initFileLogging()
{
    QMutexLocker lk(&g_mtx);
    if (fileLogging()) openLocked();
}

void setFileLogging(bool on)
{
    QSettings().setValue(QStringLiteral("log/fileEnabled"), on);
    QMutexLocker lk(&g_mtx);
    if (on) {
        openLocked();
    } else if (g_file.isOpen()) {
        g_file.close();
        g_on = false;
    }
}

void writeLine(const QString& line)
{
    QMutexLocker lk(&g_mtx);
    if (!g_on || !g_file.isOpen()) return;
    if (g_file.size() > kCapBytes) {          // wrap in place — stays bounded
        g_file.resize(0);
        g_file.seek(0);
        QTextStream(&g_file) << QStringLiteral("[log wrapped — size cap reached]\n");
    }
    QTextStream(&g_file) << line << '\n';
    g_file.flush();
}

} // namespace applog
