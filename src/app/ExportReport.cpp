#include "app/ExportReport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSettings>
#include <QTextStream>

ExportReport::ExportReport(QString title, QString outDir)
    : m_title(std::move(title)), m_dir(std::move(outDir))
{
}

bool ExportReport::enabled()
{
    return QSettings().value(QStringLiteral("export/writeReport"), false).toBool();
}

void ExportReport::context(const QString& line)
{
    QMutexLocker lock(&m_mtx);
    m_context << line;
}

void ExportReport::wrote(const QString& path, const QString& detail)
{
    QMutexLocker lock(&m_mtx);
    ++m_nWrote;
    m_wrote << (detail.isEmpty()
                    ? QFileInfo(path).fileName()
                    : QStringLiteral("%1    %2").arg(QFileInfo(path).fileName(), detail));
}

void ExportReport::failed(const QString& path, const QString& why)
{
    QMutexLocker lock(&m_mtx);
    ++m_nFailed;
    m_failed << QStringLiteral("%1    %2").arg(QFileInfo(path).fileName(), why);
}

void ExportReport::skipped(const QString& path, const QString& why)
{
    QMutexLocker lock(&m_mtx);
    ++m_nSkipped;
    m_skipped << QStringLiteral("%1    %2").arg(QFileInfo(path).fileName(), why);
}

void ExportReport::note(const QString& line)
{
    QMutexLocker lock(&m_mtx);
    m_notes << line;
}

int ExportReport::wroteCount() const
{
    QMutexLocker lock(&m_mtx);
    return m_nWrote;
}

int ExportReport::failedCount() const
{
    QMutexLocker lock(&m_mtx);
    return m_nFailed;
}

QString ExportReport::write(bool canceled)
{
    QMutexLocker lock(&m_mtx);
    if (m_dir.isEmpty()) return {};
    if (m_wrote.isEmpty() && m_failed.isEmpty() && m_skipped.isEmpty() &&
        m_notes.isEmpty())
        return {};

    // Timestamped, so a second run into the same folder does not silently
    // replace the evidence from the first — which is the one file in an export
    // folder that must never be overwritten.
    const QString name =
        QStringLiteral("_export_report_%1.txt")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QDir(m_dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("export report: cannot write %s", qPrintable(path));
        return {};
    }
    QTextStream out(&f);
    out << m_title << '\n';
    out << QDateTime::currentDateTime().toString(Qt::ISODate) << '\n';
    out << QStringLiteral("output: ") << m_dir << '\n';
    if (canceled) out << "RUN WAS CANCELLED - this is a partial result\n";
    out << '\n';

    if (!m_context.isEmpty()) {
        out << "settings used\n";
        for (const QString& l : m_context) out << "  " << l << '\n';
        out << '\n';
    }
    out << QStringLiteral("written %1    failed %2    skipped %3\n\n")
               .arg(m_nWrote).arg(m_nFailed).arg(m_nSkipped);

    // Failures first: they are the reason anyone opens this file.
    if (!m_failed.isEmpty()) {
        out << QStringLiteral("FAILED (%1)\n").arg(m_nFailed);
        for (const QString& l : m_failed) out << "  " << l << '\n';
        out << '\n';
    }
    if (!m_skipped.isEmpty()) {
        out << QStringLiteral("skipped (%1)\n").arg(m_nSkipped);
        for (const QString& l : m_skipped) out << "  " << l << '\n';
        out << '\n';
    }
    if (!m_notes.isEmpty()) {
        out << "notes\n";
        for (const QString& l : m_notes) out << "  " << l << '\n';
        out << '\n';
    }
    if (!m_wrote.isEmpty()) {
        out << QStringLiteral("written (%1)\n").arg(m_nWrote);
        QStringList sorted = m_wrote;   // lanes finish out of order
        sorted.sort(Qt::CaseInsensitive);
        for (const QString& l : sorted) out << "  " << l << '\n';
    }
    out.flush();
    f.close();
    return path;
}
