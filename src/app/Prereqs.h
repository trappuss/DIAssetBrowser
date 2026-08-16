#pragma once
// ── Runtime prerequisites: check, and fetch what is missing ────────────────
// The release zip is meant to be self-contained: unzip anywhere, run, leave no
// trace. Two things can still stop it starting on a machine that is not the one
// it was built on, and neither says anything useful when it happens:
//
//   MSVC RUNTIME   vcpkg builds against the dynamic CRT, so the exe needs
//                  vcruntime140.dll / vcruntime140_1.dll / msvcp140.dll. The
//                  build now copies them beside the exe, so this is normally
//                  already satisfied — but a zip extracted with "only the .exe",
//                  an antivirus that quarantined a DLL, or a hand-assembled
//                  folder all reproduce the original failure, which Windows
//                  reports as a bare "the code execution cannot proceed"
//                  dialog before main() ever runs.
//
//   OPENGL 3.3     the viewport needs a 3.3 core context. A machine without one
//                  (a very old GPU, a stripped VM, Remote Desktop with hardware
//                  acceleration off) shows an empty viewport and nothing else.
//
// This page reports both in plain language and, for the runtime, offers to
// download Microsoft's official redistributable and run it.
//
// WHAT IS DELIBERATELY NOT HERE: a GPU-driver download. There is no vendor-
// neutral URL that is correct for an unknown card, and guessing at one would be
// worse than saying plainly what is wrong and letting the user handle it. The
// OpenGL row diagnoses; it does not pretend it can fix.

#include <QObject>
#include <QString>

// Forward-declared at GLOBAL scope. Declaring them inside the namespace (or
// inline as "class QNetworkReply* m_reply") invents Prereqs::QNetworkReply,
// which then refuses to convert from the real one.
class QNetworkAccessManager;
class QNetworkReply;

namespace Prereqs {

enum class State {
    Ok,          // present and sufficient
    Missing,     // absent, and the app needs it
    Degraded,    // present but below what the viewport wants
    Unknown,     // could not be determined (checked too early, or no context)
};

struct Check {
    QString name;       // "Visual C++ runtime"
    State   state = State::Unknown;
    QString detail;     // what was found, in the user's words
    QString advice;     // what to do about it
    bool    fixable = false;   // true when this page can act on it
};

// The MSVC runtime DLLs the executable links against. Checked by attempting to
// resolve them the way the loader would — app-local directory first, then the
// system search path — so a bundled copy counts as present.
Check checkMsvcRuntime();

// The OpenGL version actually obtained. Must be called AFTER a context has
// existed at least once (i.e. after the main window has shown), or it returns
// Unknown rather than guessing.
Check checkOpenGl();

// Microsoft's permanent link for the current x64 redistributable. Documented by
// Microsoft as a stable redirect, which is why it is used instead of pinning a
// version-specific URL that goes stale on the next servicing release.
QString vcRedistUrl();

// Downloads `url` to a temporary file and runs it, reporting progress through
// the callbacks. Returns immediately; everything happens on the Qt event loop.
//
// `onDone` receives an empty error string on success. The installer is launched
// detached — it is a UAC-elevating Microsoft installer and this app must not
// sit blocked waiting on it.
class Fetcher : public QObject {
    Q_OBJECT
public:
    explicit Fetcher(QObject* parent = nullptr);
    ~Fetcher() override;
    void start(const QString& url, const QString& saveAsName);
    void cancel();

signals:
    void progress(qint64 received, qint64 total);   // total < 0 = unknown
    void finished(QString error, QString savedPath);

private:
    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply*         m_reply = nullptr;
    QString                m_path;
};

}   // namespace Prereqs
