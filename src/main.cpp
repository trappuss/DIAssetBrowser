// DIAssetBrowser — entry point.
//
// Native C++/Qt6 Diablo Immortal asset browser, forked from the D4 tool's stack:
//   C++17 · Qt 6 Widgets · (OpenGL viewport arrives with the Models tab).
// Reads the PC client's Package/MPK store directly via src/store.
#include <QApplication>
#include <QDateTime>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QMutex>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTextStream>

#include "app/AppLog.h"
#include "app/AppPaths.h"
#include "app/LogConsole.h"
#include "app/MainWindow.h"
#include "app/SehGuard.h"
#include "app/SettingsSchema.h"

namespace {

// Real checkmarks (not the platform's filled blue box) on every toggle — same
// treatment as the D4 tool, with the accent moved from Diablo-red toward the
// Immortal gold/ember palette.
void installCheckmarkStyle(QApplication& app)
{
    const QString png = AppPaths::file(QStringLiteral("checkmark.png"));
    {
        QPixmap pm(28, 28);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(0xe8, 0xa3, 0x3d));   // ember-gold tick
        pen.setWidthF(3.2);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPolyline(QPolygonF{QPointF(6, 15), QPointF(12, 21), QPointF(22, 7)});
        p.end();
        pm.save(png, "PNG");
    }
    QString url = png;
    url.replace(QLatin1Char('\\'), QLatin1Char('/'));
    app.setStyleSheet(QStringLiteral(
        "QCheckBox::indicator, QMenu::indicator, QGroupBox::indicator,"
        "QTreeView::indicator, QTreeWidget::indicator,"
        "QTableView::indicator, QListView::indicator, QListWidget::indicator {"
        " width:15px; height:15px; border:1px solid #6a6a6a;"
        " border-radius:3px; background:#2c2c2c; }"
        "QCheckBox::indicator:hover, QMenu::indicator:hover,"
        "QTreeView::indicator:hover, QTableView::indicator:hover {"
        " border-color:#b08a3c; }"
        "QCheckBox::indicator:checked, QMenu::indicator:checked,"
        "QGroupBox::indicator:checked, QTreeView::indicator:checked,"
        "QTreeWidget::indicator:checked, QTableView::indicator:checked,"
        "QListView::indicator:checked, QListWidget::indicator:checked {"
        " border-color:#a07818; image:url(\"%1\"); }"
        "QCheckBox::indicator:indeterminate, QTreeView::indicator:indeterminate,"
        "QTreeWidget::indicator:indeterminate {"
        " border-color:#a07818; background:#4a3a14; }"
        "QTableView, QTreeView, QListView, QTreeWidget, QListWidget {"
        " selection-background-color:#6a5210; selection-color:#ffffff;"
        " outline:0; }"
        "QTableView::item, QTreeView::item, QListView::item,"
        "QTreeWidget::item, QListWidget::item {"
        " border:0; border-radius:0; }"
        "QTableView::item:selected, QTreeView::item:selected, QListView::item:selected,"
        "QTreeWidget::item:selected, QListWidget::item:selected {"
        " background:#6a5210; color:#ffffff; border:0; border-radius:0; }"
        "QTableView::item:hover, QTreeView::item:hover, QListView::item:hover,"
        "QTreeWidget::item:hover, QListWidget::item:hover {"
        " background:#33301f; }").arg(url));
}

void logHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    // Serialize: Qt routes messages from ANY thread into this one handler.
    static QMutex s_logMutex;
    QMutexLocker s_lock(&s_logMutex);

    const char* lvl = "INFO";
    switch (type) {
        case QtDebugMsg:    lvl = "DBG ";  break;
        case QtWarningMsg:  lvl = "WARN";  break;
        case QtCriticalMsg: lvl = "CRIT";  break;
        case QtFatalMsg:    lvl = "FATAL"; break;
        default:            lvl = "INFO";  break;
    }
    const QString line = QStringLiteral("%1 %2  %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(QLatin1String(lvl), msg);
    applog::writeLine(line);   // honours the "Write log to file" setting + size cap
    fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
    LogBuffer::instance().append(line);
}
} // namespace

int main(int argc, char** argv)
{
    seh::installSehTranslator();

    // Same GL setup as the D4 tool so the texture preview / model viewport that
    // arrive in later milestones inherit a working context configuration.
    QSurfaceFormat fmt;
    fmt.setVersion(4, 5);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSamples(4);
    fmt.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    LogBuffer::instance();   // construct on the GUI thread (queued log delivery)
    QApplication::setOrganizationName("DIAssetBrowser");
    QApplication::setApplicationName("DIAssetBrowser");
    QApplication::setApplicationVersion("2.0.0");

    // Portable: every QSettings() writes to an INI in the beside-exe data/ folder.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, AppPaths::dataDir());

    // Bring an older profile up to this build's key scheme BEFORE anything
    // reads a setting. A no-op for a current or fresh profile.
    SettingsSchema::migrate();

    // Runtime log inside data/ (truncated each launch, self-capped, toggleable
    // via Settings). Path: data\DIAssetBrowser.log.
    applog::initFileLogging();
    qInstallMessageHandler(logHandler);
    qInfo("DIAssetBrowser v%s starting",
          QApplication::applicationVersion().toLatin1().constData());

    installCheckmarkStyle(app);

    // No application icon yet. The generated placeholder was removed and the
    // exe carries Windows' default until a real one exists — a stand-in icon
    // reads as a decision that has been made.
    //
    // To restore: put app_256.png back in res/app.qrc and re-add
    //     const QIcon appIcon(QStringLiteral(":/app_256.png"));
    //     if (!appIcon.isNull()) app.setWindowIcon(appIcon);
    // here. res/app.rc separately gives the .exe FILE its Explorer icon — both
    // mechanisms are needed, neither substitutes for the other.

    MainWindow window;
    window.show();
    return QApplication::exec();
}
