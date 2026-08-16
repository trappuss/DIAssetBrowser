#pragma once
// ── Run report for the multi-file exports ───────────────────────────────────
// A bulk run writes hundreds or thousands of files across several worker lanes.
// When it finishes, the status line has room for four numbers — and "3 failed"
// out of 900 is not something you can act on. This collects, per run: what was
// written, what was skipped and why, what failed and with what error, and the
// settings the run actually used, then drops a plain-text report next to the
// output.
//
// It is written to from EXPORT LANES, so every method takes the lock. The cost
// is a mutex per file written, against a file write — immaterial.
//
// Deliberately plain text rather than CSV: this is meant to be opened and read.
// (The Bulk Extract tab keeps its machine-readable _bulk_report.csv as well —
// that one exists to be diffed between runs, which is a different job.)

#include <QMutex>
#include <QString>
#include <QStringList>

class ExportReport {
public:
    // `title` heads the file ("Export all sets", "Bulk extract"). `outDir` is
    // where the report lands, normally the run's own output folder.
    ExportReport(QString title, QString outDir);

    // Was the report requested at all? Reading the setting here keeps the
    // callers free of "if (reportOn)" around every single call.
    static bool enabled();

    // Context printed above the results: the settings the run used, the class,
    // the plan. Call before the run starts.
    void context(const QString& line);

    void wrote(const QString& path, const QString& detail = QString());
    void failed(const QString& path, const QString& why);
    void skipped(const QString& path, const QString& why);
    // Anything that is not per-file: a rig that would not load, a name
    // collision that was suffixed, a cancelled run.
    void note(const QString& line);

    int wroteCount() const;
    int failedCount() const;

    // Write the file and return its full path, or an empty string if it was
    // disabled, had nothing to say, or could not be written. Safe to call once
    // the lanes have joined.
    QString write(bool canceled = false);

private:
    mutable QMutex m_mtx;
    QString m_title;
    QString m_dir;
    QStringList m_context, m_wrote, m_failed, m_skipped, m_notes;
    int m_nWrote = 0, m_nFailed = 0, m_nSkipped = 0;
};
