#pragma once
// Auto-written runtime log sink: data/DIAssetBrowser.log, truncated on each
// launch and SELF-CAPPED during a session so it can never bloat. Toggleable
// (Settings ▸ "Write log to file"); persisted under QSettings "log/fileEnabled".
// The global qInstallMessageHandler formats each line and hands it to
// writeLine(); the console window + Help ▸ Export log are unaffected by the
// toggle (they read the in-memory LogBuffer).

#include <QString>

namespace applog {

// Absolute path of the log file (data/DIAssetBrowser.log beside the exe).
QString logPath();

// Current setting (default true). Persisted as QSettings "log/fileEnabled".
bool fileLogging();

// Open+truncate (on) or close (off) the file sink and persist the choice. Safe
// on the GUI thread; takes effect immediately.
void setFileLogging(bool on);

// Called once at startup to honour the saved setting.
void initFileLogging();

// Append one already-formatted line when file logging is on. Thread-safe. Self-
// truncates past a size cap (a long/bulk session wraps instead of growing).
void writeLine(const QString& line);

} // namespace applog
