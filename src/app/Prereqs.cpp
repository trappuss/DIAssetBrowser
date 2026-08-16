#include "app/Prereqs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QProcess>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QUrl>

namespace Prereqs {

QString vcRedistUrl()
{
    // aka.ms/vc14/..., NOT aka.ms/vs/17/release/... . The /vs/17/ link is the
    // Visual Studio 2022 *lineage* redistributable; Microsoft's rule is that the
    // installed redistributable must be the SAME OR LATER than the MSVC build
    // tools the binary was compiled with. This project is built with a v14.5x
    // toolset (VS2026), so the VS2022 link can hand back an older CRT than the
    // exe needs and "fix" nothing. The /vc14/ link is Microsoft's documented
    // permanent link to the latest v14 redistributable, which covers everything
    // built by VS2017 through VS2026 and is what their own docs now point at.
    return QStringLiteral("https://aka.ms/vc14/vc_redist.x64.exe");
}

Check checkMsvcRuntime()
{
    Check c;
    c.name = QStringLiteral("Visual C++ runtime");
#ifndef Q_OS_WIN
    c.state  = State::Ok;
    c.detail = QStringLiteral("not applicable on this platform");
    return c;
#else
    // The three the executable actually imports. vcruntime140_1.dll is the one
    // that catches people out: it only exists from VS2019 onward, so a machine
    // with an OLD redistributable installed passes a naive "is the redist
    // there" test and still fails to start.
    static const char* const kNeeded[] = {"vcruntime140.dll", "vcruntime140_1.dll",
                                          "msvcp140.dll"};
    QStringList missing, local;
    const QDir appDir(QCoreApplication::applicationDirPath());
    for (const char* dll : kNeeded) {
        const QString name = QString::fromLatin1(dll);
        if (QFile::exists(appDir.filePath(name))) {   // app-local copy wins
            local << name;
            continue;
        }
        // QLibrary follows the loader's search order, so this answers the real
        // question ("would the loader find it") rather than probing System32.
        QLibrary lib(name);
        if (lib.load()) { lib.unload(); continue; }
        missing << name;
    }
    if (missing.isEmpty()) {
        c.state  = State::Ok;
        c.detail = local.isEmpty()
                       ? QStringLiteral("installed system-wide")
                       : QStringLiteral("bundled beside the app (%1)")
                             .arg(local.join(QStringLiteral(", ")));
        return c;
    }
    c.state   = State::Missing;
    c.fixable = true;
    c.detail  = QStringLiteral("missing: %1").arg(missing.join(QStringLiteral(", ")));
    c.advice  = QStringLiteral(
        "The app cannot start without these. They normally ship inside the "
        "release zip — if they are gone, either the zip was extracted "
        "partially or an antivirus removed them. Download installs them "
        "system-wide from Microsoft, which fixes it permanently.");
    return c;
#endif
}

Check checkOpenGl()
{
    Check c;
    c.name = QStringLiteral("OpenGL 3.3");

    // Probe on an offscreen surface rather than assuming the viewport's context
    // is current — this page can be opened from anywhere, including before any
    // 3D tab has been shown.
    QOpenGLContext ctx;
    ctx.setFormat(QSurfaceFormat::defaultFormat());
    if (!ctx.create()) {
        c.state  = State::Missing;
        c.detail = QStringLiteral("no OpenGL context could be created");
        c.advice = QStringLiteral(
            "The 3D viewport will be blank; every other tab still works. This is "
            "almost always a graphics driver that has not been installed — "
            "Windows' own basic display adapter provides no usable OpenGL. "
            "Install your GPU vendor's driver.");
        return c;
    }
    QOffscreenSurface surf;
    surf.setFormat(ctx.format());
    surf.create();
    if (!ctx.makeCurrent(&surf)) {
        c.state  = State::Unknown;
        c.detail = QStringLiteral("a context exists but could not be made current");
        return c;
    }
    const QSurfaceFormat f = ctx.format();
    QString renderer;
    if (auto* fn = ctx.functions()) {
        const GLubyte* r = fn->glGetString(GL_RENDERER);
        if (r) renderer = QString::fromLatin1(reinterpret_cast<const char*>(r));
    }
    const int maj = f.majorVersion(), min = f.minorVersion();
    ctx.doneCurrent();

    c.detail = QStringLiteral("%1.%2%3").arg(maj).arg(min).arg(
        renderer.isEmpty() ? QString()
                           : QStringLiteral("  ·  %1").arg(renderer));
    if (maj > 3 || (maj == 3 && min >= 3)) {
        c.state = State::Ok;
    } else {
        c.state  = State::Degraded;
        c.advice = QStringLiteral(
            "The viewport needs 3.3 core. Textures, the asset browser and bulk "
            "extraction are unaffected — only the 3D preview and the .glb "
            "export's viewport-driven parts need it. Updating the graphics "
            "driver is the fix; there is no download this app can offer that "
            "would be correct for an unknown card.");
    }
    return c;
}

// ── Fetcher ────────────────────────────────────────────────────────────────

Fetcher::Fetcher(QObject* parent) : QObject(parent) {}

Fetcher::~Fetcher() { cancel(); }

void Fetcher::start(const QString& url, const QString& saveAsName)
{
    cancel();
    if (!m_net) m_net = new QNetworkAccessManager(this);

    // Downloaded into the system temp dir, NOT the portable data folder: this
    // is an installer that is run once and thrown away, and leaving a 25 MB
    // executable inside a folder whose whole promise is "delete it and nothing
    // is left behind" would break that promise.
    const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_path = QDir(tmp).filePath(saveAsName);

    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);   // aka.ms redirects
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("DIAssetBrowser/%1")
                      .arg(QCoreApplication::applicationVersion()));
    m_reply = m_net->get(req);

    connect(m_reply, &QNetworkReply::downloadProgress, this, &Fetcher::progress);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply* r = m_reply;
        m_reply = nullptr;
        if (!r) return;
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            Q_EMIT finished(r->errorString(), QString());
            return;
        }
        QFile f(m_path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            Q_EMIT finished(QStringLiteral("cannot write %1").arg(m_path), QString());
            return;
        }
        const QByteArray body = r->readAll();
        if (f.write(body) != body.size()) {
            f.close();
            QFile::remove(m_path);
            Q_EMIT finished(QStringLiteral("short write to %1").arg(m_path), QString());
            return;
        }
        f.close();
        // An installer that downloaded as an error page would be a 4 KB "file"
        // that Windows then refuses to run with a useless message. Catch it here
        // where we can say what actually happened.
        if (QFileInfo(m_path).size() < 1024 * 512) {
            QFile::remove(m_path);
            Q_EMIT finished(
                QStringLiteral("the download was too small to be the installer "
                               "(%1 bytes) — the link may be blocked by a proxy")
                    .arg(body.size()),
                QString());
            return;
        }
        Q_EMIT finished(QString(), m_path);
    });
}

void Fetcher::cancel()
{
    if (!m_reply) return;
    QNetworkReply* r = m_reply;
    m_reply = nullptr;
    r->abort();
    r->deleteLater();
}

}   // namespace Prereqs
