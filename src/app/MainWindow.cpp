#include "app/MainWindow.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QToolButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>

#include "app/AppPaths.h"
#include "app/ExportHooks.h"
#include "app/ExportNotifier.h"
#include "app/ExportSettings.h"
#include "app/Hotkeys.h"
#include "app/LogConsole.h"
#include "app/SehGuard.h"
#include "app/SettingsDialog.h"
#include "index/AssetIndexCache.h"
#include "index/ItemNames.h"
#include "tabs/AssetsTab.h"
#include "tabs/BulkExtractTab.h"
#include "tabs/ModelsTab.h"
#include "tabs/TexturesTab.h"
#include "tabs/WardrobeTab.h"
#include "tex/BcDecode.h"
#include "util/MenuText.h"

#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const char kMpkDirKey[]  = "app/mpkDir";
const char kMpkDirDflt[] = "G:/G Games/Diablo Immortal/Package/MPK";
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("DIAssetBrowser"));
    resize(1360, 860);

    m_tabs = new QTabWidget(this);
    setCentralWidget(m_tabs);
    m_assetsTab = new AssetsTab(this);
    m_tabs->addTab(m_assetsTab, QStringLiteral("Assets"));
    m_texturesTab = new TexturesTab(this);
    m_tabs->addTab(m_texturesTab, QStringLiteral("Textures"));
    m_modelsTab = new ModelsTab(this);
    m_tabs->addTab(m_modelsTab, QStringLiteral("Models"));
    m_wardrobeTab = new WardrobeTab(this);
    m_tabs->addTab(m_wardrobeTab, QStringLiteral("Wardrobe"));
    m_bulkTab = new BulkExtractTab(this);
    m_tabs->addTab(m_bulkTab, QStringLiteral("Bulk Extract"));

    // UI persistence: window geometry + last active tab (splitters persist
    // themselves via PanelPersist inside each tab).
    {
        QSettings s;
        const QByteArray geo = s.value(QStringLiteral("ui/geometry")).toByteArray();
        if (!geo.isEmpty()) restoreGeometry(geo);
        const int tab = s.value(QStringLiteral("ui/lastTab"), 0).toInt();
        if (tab > 0 && tab < m_tabs->count()) m_tabs->setCurrentIndex(tab);
    }
    connect(m_tabs, &QTabWidget::currentChanged, this, [](int i) {
        QSettings().setValue(QStringLiteral("ui/lastTab"), i);
    });

    // Block-decoder self-check (BC7 tables etc.) — loud in the log if a port
    // regression sneaks in; costs nothing at startup.
    {
        const QString fail = BcDecode::selfTest();
        if (!fail.isEmpty()) qWarning("BcDecode selfTest FAILED: %s", qPrintable(fail));
        else                 qInfo("BcDecode selfTest OK");
    }

    m_status = new QLabel(this);
    statusBar()->addPermanentWidget(m_status, 1);

    // ONE place every tab's export result is reported. Before this each tab
    // said it its own way — a status line here, an info-panel note there — so
    // the same operation read differently depending on where you started it.
    connect(&ExportNotifier::instance(), &ExportNotifier::exported, this,
            [this](const QString& text, const QString& folder, int failed) {
                QString line = text;
                if (failed > 0)
                    line += QStringLiteral("  ·  %1 failed").arg(failed);
                statusBar()->showMessage(line, 12000);
                qInfo("export: %s", qPrintable(line));
                if (folder.isEmpty()) return;
                // "Show in folder" lives on the status bar rather than in a
                // popup: an export finishing must never steal focus from what
                // the user is doing next.
                if (!m_showFolderBtn) {
                    m_showFolderBtn = new QToolButton(this);
                    m_showFolderBtn->setText(QStringLiteral("Show in folder"));
                    m_showFolderBtn->setAutoRaise(true);
                    m_showFolderBtn->setCursor(Qt::PointingHandCursor);
                    statusBar()->addPermanentWidget(m_showFolderBtn);
                }
                m_lastExportFolder = folder;
                m_showFolderBtn->disconnect();
                connect(m_showFolderBtn, &QToolButton::clicked, this, [this] {
                    QDesktopServices::openUrl(
                        QUrl::fromLocalFile(m_lastExportFolder));
                });
                m_showFolderBtn->setVisible(true);
            });
    // Per-tab filter cues; the status bar shows the CURRENT tab's cue next to
    // the load summary (the Assets cue used to show on every tab).
    auto refreshStatus = [this] {
        const QString summary = m_status->property("loadSummary").toString();
        if (summary.isEmpty()) return;   // load in flight / failed: keep that text
        const QString cue = m_tabCues.value(m_tabs->currentWidget());
        m_status->setText(cue.isEmpty() ? summary
                                        : summary + QStringLiteral("   |   ") + cue);
    };
    auto cueFor = [this, refreshStatus](QWidget* tab) {
        return [this, tab, refreshStatus](const QString& t) {
            m_tabCues[tab] = t;
            refreshStatus();
        };
    };
    connect(m_assetsTab,   &AssetsTab::statusText,      this, cueFor(m_assetsTab));
    connect(m_modelsTab,   &ModelsTab::statusText,      this, cueFor(m_modelsTab));
    connect(m_texturesTab, &TexturesTab::statusText,    this, cueFor(m_texturesTab));
    connect(m_wardrobeTab, &WardrobeTab::statusText,    this, cueFor(m_wardrobeTab));
    connect(m_bulkTab,     &BulkExtractTab::statusText, this, cueFor(m_bulkTab));
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [refreshStatus](int) { refreshStatus(); });

    // "Reveal in Models tab" from the Textures tab's ASSOCIATED MODELS panel:
    // switch to Models and select+load the model.
    connect(m_texturesTab, &TexturesTab::revealModelRequested, this,
            [this](int repoIdx) {
                if (!m_modelsTab) return;
                m_tabs->setCurrentWidget(m_modelsTab);
                m_modelsTab->revealRepoIndex(repoIdx);
            });

    buildMenus();

    // Loader thread -> GUI thread handoff (queued: the payload crosses threads).
    connect(this, &MainWindow::indexLoaded, this,
            [this](std::shared_ptr<AssetIndex> idx, int generation, QString error) {
                if (generation != m_generation.load())
                    return;   // stale load: folder changed while it was building
                if (!idx) {
                    const QString failed = QStringLiteral("load FAILED: %1").arg(error);
                    m_status->setProperty("loadSummary", failed);
                    m_status->setText(failed);
                    QMessageBox::warning(this, QStringLiteral("Index load failed"), error);
                    return;
                }
                m_index = idx;
                const QString summary =
                    QStringLiteral("%L1 assets · build %2 · indexed in %L3 ms")
                        .arg(idx->rows.size())
                        .arg(idx->buildVersion.isEmpty() ? QStringLiteral("<unknown>")
                                                         : idx->buildVersion)
                        .arg((qint64)idx->loadMs);
                m_status->setProperty("loadSummary", summary);
                m_status->setText(summary);
                qInfo("index: %s", qPrintable(summary));
                m_assetsTab->setIndex(idx);
                m_modelsTab->setIndex(idx);
                m_texturesTab->setIndex(idx);
                m_wardrobeTab->setIndex(idx);
                m_bulkTab->setIndex(idx);
            },
            Qt::QueuedConnection);

    reload();
}

void MainWindow::closeEvent(QCloseEvent* ev)
{
    QSettings().setValue(QStringLiteral("ui/geometry"), saveGeometry());
    QMainWindow::closeEvent(ev);
}

MainWindow::~MainWindow()
{
    // Joining here guarantees no loader thread emits into (or logs past) a
    // destroyed window — closing mid-load was a use-after-free otherwise. The
    // generation bump makes the result discard itself if it does arrive first.
    ++m_generation;
    const QSet<QThread*> workers = m_workers;   // wait() re-enters via finished
    for (QThread* w : workers)
        w->wait();
}

QString MainWindow::mpkDir() const
{
    return QSettings().value(QLatin1String(kMpkDirKey), QLatin1String(kMpkDirDflt))
        .toString();
}

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("Set &game folder…"), this,
                    &MainWindow::chooseGameFolder);
    file->addAction(QStringLiteral("&Reload index"), this, &MainWindow::reload);
    file->addAction(QStringLiteral("&Settings…"), this,
                    [this] { openSettings(); });
    file->addSeparator();
    file->addAction(QStringLiteral("Toggle &console"), this, [this] {
        if (!m_console) m_console = new ConsoleWindow(this);
        m_console->setVisible(!m_console->isVisible());
    });
    // (Names submenu + Exit are appended to File below, after the Export menu,
    //  so the top bar stays File / Export / Help — matching the D4 browser.)

    // ── Export — adaptive to the current tab (D4 export-hook pattern) ──────
    // The ACTIONS are permanent so Ctrl+E / Ctrl+Shift+E / Ctrl+Shift+I work
    // without ever opening the menu; aboutToShow re-labels and re-enables
    // them for whatever tab is in front, and every handler re-resolves the
    // current tab at trigger time.
    QMenu* exportMenu = menuBar()->addMenu(QStringLiteral("&Export"));
    m_actExport = exportMenu->addAction(QStringLiteral("Export selection…"));
    m_actExport->setShortcut(Hotkeys::seq(QStringLiteral("hotkeys/exportSelection"),
                                          QStringLiteral("Ctrl+E")));
    connect(m_actExport, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExport()) h->exportNow(false);
    });
    m_actExportLast = exportMenu->addAction(QStringLiteral("Export to last folder"));
    m_actExportLast->setShortcut(Hotkeys::seq(QStringLiteral("hotkeys/exportToLast"),
                                              QStringLiteral("Ctrl+Shift+E")));
    connect(m_actExportLast, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExport()) h->exportNow(true);
    });
    m_actExportAnims =
        exportMenu->addAction(QStringLiteral("Export animations only (.glb)…"));
    connect(m_actExportAnims, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportAnims()) h->exportAnimsNow();
    });
    m_actExportAttach =
        exportMenu->addAction(QStringLiteral("Export with attachments (.glb)…"));
    connect(m_actExportAttach, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportWithAttachments()) h->exportWithAttachmentsNow(false);
    });
    exportMenu->addSeparator();
    m_actFramesSel = exportMenu->addAction(QStringLiteral("Export selected frames…"));
    connect(m_actFramesSel, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportFrames()) h->exportFramesNow(false, false);
    });
    m_actFramesAll = exportMenu->addAction(QStringLiteral("Export all frames…"));
    connect(m_actFramesAll, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportFrames()) h->exportFramesNow(true, false);
    });
    m_actFramesAllLast =
        exportMenu->addAction(QStringLiteral("Export all frames to last folder"));
    connect(m_actFramesAllLast, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportFrames()) h->exportFramesNow(true, true);
    });
    m_actExportAll = exportMenu->addAction(QStringLiteral("Export all matching…"));
    connect(m_actExportAll, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportAll()) h->exportAllNow(false);
    });
    m_actExportAllLast =
        exportMenu->addAction(QStringLiteral("Export all matching to last folder"));
    connect(m_actExportAllLast, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportAll()) h->exportAllNow(true);
    });
    exportMenu->addSeparator();
    m_actSaveImage = exportMenu->addAction(QStringLiteral("Save preview image…"));
    m_actSaveImage->setShortcut(Hotkeys::seq(QStringLiteral("hotkeys/saveImage"),
                                             QStringLiteral("Ctrl+Shift+I")));
    connect(m_actSaveImage, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canSaveImage()) h->saveImageNow();
    });
    // Animated GIF (viewport tabs) — D4 keeps these out of the toolbar; they
    // live here in the Export menu (and the right-click menu), gated per tab.
    m_actGifTurntable = exportMenu->addAction(QStringLiteral("Turntable &GIF…"));
    connect(m_actGifTurntable, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportGif()) h->exportGifTurntable();
    });
    m_actGifAnim = exportMenu->addAction(QStringLiteral("Animation &loop GIF…"));
    connect(m_actGifAnim, &QAction::triggered, this, [this] {
        if (ExportHooks* h = currentHooks())
            if (h->canExportAnimGif()) h->exportGifAnim();
    });
    exportMenu->addSeparator();
    connect(exportMenu, &QMenu::aboutToShow, this,
            &MainWindow::refreshExportMenu);
    // A DISABLED action's shortcut does not fire, and enabled-state computed
    // at menu-show time goes stale the moment the user changes a selection —
    // so outside the open menu every action stays enabled and its handler
    // no-ops when the tab can't comply. The menu itself still shows honest
    // enabled states while open.
    connect(exportMenu, &QMenu::aboutToHide, this, [this] {
        for (QAction* a : {m_actExport, m_actExportLast, m_actFramesSel,
                           m_actFramesAll, m_actFramesAllLast, m_actSaveImage,
                           m_actGifTurntable, m_actGifAnim, m_actExportAnims,
                           m_actExportAttach, m_actExportAll, m_actExportAllLast})
            a->setEnabled(true);
    });
    refreshExportMenu();   // sane initial labels for the shortcut tooltips
    for (QAction* a : {m_actExport, m_actExportLast, m_actFramesSel,
                       m_actFramesAll, m_actFramesAllLast, m_actSaveImage,
                       m_actExportAnims, m_actExportAll, m_actExportAllLast})
        a->setEnabled(true);
    // (The animation-mode picker and the attachments toggle are export SETTINGS,
    //  not menu actions — D4 keeps them in the Export-settings dialog, and DI's
    //  SettingsDialog already exposes both. The Export menu stays a flat list of
    //  actions, matching D4.)
    exportMenu->addAction(QStringLiteral("Export &settings…"), this,
                          [this] { openSettings(); });

    // Names tools live under File as a submenu (not a top-level menu) so the
    // menu bar matches D4's File / Export / Help.
    QMenu* tools = file->addMenu(QStringLiteral("&Names"));
    tools->addAction(
        QStringLiteral("&Export cosmetic-set name template…"), this, [this] {
            if (!m_index) {
                QMessageBox::information(this, QStringLiteral("Names"),
                    QStringLiteral("Load an index first."));
                return;
            }
            const QString dflt =
                AppPaths::file(QStringLiteral("di_item_names_template.csv"));
            const QString dest = QFileDialog::getSaveFileName(
                this, QStringLiteral("Export set-key name template"), dflt,
                QStringLiteral("CSV (*.csv)"));
            if (dest.isEmpty()) return;
            const int n = ItemNames::exportTemplate(m_index, dest);
            QMessageBox::information(this, QStringLiteral("Names"),
                n < 0 ? QStringLiteral("Could not write %1").arg(dest)
                      : QStringLiteral("Wrote %1 set keys to %2.\n\nFill the "
                                       "third column with in-game names, save as "
                                       "data\\di_item_names.csv, then Names → "
                                       "Reload.").arg(n).arg(dest));
        });
    tools->addAction(QStringLiteral("&Reload names (data\\di_item_names.csv)"),
                     this, [this] {
            // count only (throwaway table); the rebuild worker loads its own
            const int n = ItemNames::load(
                AppPaths::file(QStringLiteral("di_item_names.csv"))).size();
            m_index.reset();
            QFile::remove(AssetIndexCache::defaultPath());  // force rebuild with new names
            reload();
            QMessageBox::information(this, QStringLiteral("Names"),
                QStringLiteral("Loaded %1 real-name rows; rebuilding index.").arg(n));
        });

    // Exit sits last in File (appended here so the Names submenu precedes it).
    file->addSeparator();
    file->addAction(QStringLiteral("E&xit"), qApp, &QApplication::quit);

    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* shortcuts =
        help->addAction(QStringLiteral("&Shortcuts and mouse gestures…"), this,
                        &MainWindow::showShortcutsHelp);
    shortcuts->setShortcut(QKeySequence(Qt::Key_F1));
    help->addAction(QStringLiteral("&Information (how the tool works)…"), this,
                    [this] { openSettings(); });
    help->addSeparator();
    help->addAction(QStringLiteral("Open &data folder"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::dataDir()));
    });
    help->addAction(QStringLiteral("&Export log…"), this, &MainWindow::exportLog);
    help->addSeparator();
    help->addAction(QStringLiteral("&About"), this, [this] {
        QMessageBox::about(this, QStringLiteral("DIAssetBrowser"),
            QStringLiteral("<b>DIAssetBrowser</b> v%1<br>"
                           "Native Diablo Immortal asset browser.<br>"
                           "Reads the PC client's Package/MPK store directly.<br><br>"
                           "Everything the tool writes lives in <code>data\\</code> "
                           "beside the executable.")
                .arg(QApplication::applicationVersion()));
    });
}

ExportHooks* MainWindow::currentHooks() const
{
    // Every tab implements ExportHooks; dynamic_cast keeps this safe if one
    // ever doesn't.
    return dynamic_cast<ExportHooks*>(m_tabs->currentWidget());
}

void MainWindow::refreshExportMenu()
{
    ExportHooks* h = currentHooks();
    const bool can = h && h->canExport();
    const QString what = h ? h->exportWhat() : QStringLiteral("selection");
    m_actExport->setText(MenuText::exportSetPrompt(what));
    m_actExport->setEnabled(can);
    const QString dir = h ? h->lastExportDir() : QString();
    m_actExportLast->setVisible(!dir.isEmpty());
    m_actExportLast->setText(
        MenuText::exportSetLast(what, MenuText::condensePath(dir)));
    m_actExportLast->setEnabled(can && !dir.isEmpty());

    const bool frames = h && h->canExportFrames();
    m_actFramesSel->setVisible(frames);
    m_actFramesAll->setVisible(frames);
    if (frames) {
        const int sel = h->frameSelCount();
        m_actFramesSel->setText(
            sel > 0 ? QStringLiteral("Export %1 selected frame(s)…").arg(sel)
                    : QStringLiteral("Export selected frames…"));
        m_actFramesSel->setEnabled(sel > 0);
        m_actFramesAll->setText(
            MenuText::prompts(QStringLiteral("Export all %1 frames")
                                  .arg(h->frameCount())));
    }
    const QString fdir = h ? h->framesLastDir() : QString();
    m_actFramesAllLast->setVisible(frames && !fdir.isEmpty());
    if (frames && !fdir.isEmpty())
        m_actFramesAllLast->setText(MenuText::withValue(
            QStringLiteral("Export all frames to last folder"),
            MenuText::condensePath(fdir)));

    m_actSaveImage->setEnabled(h && h->canSaveImage());

    const bool gif = h && h->canExportGif();
    m_actGifTurntable->setVisible(gif);
    m_actGifTurntable->setEnabled(gif);
    m_actGifAnim->setVisible(gif);
    m_actGifAnim->setEnabled(h && h->canExportAnimGif());

    const bool anims = h && h->canExportAnims();
    m_actExportAnims->setVisible(anims);
    m_actExportAnims->setEnabled(anims);

    const bool withAtt = h && h->canExportWithAttachments();
    m_actExportAttach->setVisible(withAtt);   // only when attachments are shown
    m_actExportAttach->setEnabled(withAtt);

    const bool all = h && h->canExportAll();
    m_actExportAll->setVisible(all);
    m_actExportAll->setEnabled(all);
    if (all)
        m_actExportAll->setText(
            QStringLiteral("Export all %1 matching…").arg(h->exportAllCount()));
    const QString adir = h ? h->lastExportDir() : QString();
    m_actExportAllLast->setVisible(all && !adir.isEmpty());
    m_actExportAllLast->setEnabled(all && !adir.isEmpty());
    if (all && !adir.isEmpty())
        m_actExportAllLast->setText(MenuText::withValue(
            QStringLiteral("Export all matching to last folder"),
            MenuText::condensePath(adir)));
}

void MainWindow::showShortcutsHelp()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Shortcuts and mouse gestures"));
    dlg.resize(620, 640);
    auto* lay = new QVBoxLayout(&dlg);
    auto* browser = new QTextBrowser(&dlg);
    browser->setOpenExternalLinks(false);
    browser->setHtml(QStringLiteral(R"HTML(
<h3>Everywhere</h3>
<p><b>Ctrl+E</b> — export what the current tab has selected (the Export menu
shows exactly what that means per tab).<br>
<b>Ctrl+Shift+E</b> — the same export, straight into that tab's last-used
folder, no dialog.<br>
<b>Ctrl+Shift+I</b> — save the preview image (decoded texture, or the 3D
viewport as shown).<br>
<b>Ctrl+C</b> on any list — copy the selected rows (all rows when nothing is
selected) as CSV with headers.<br>
<b>F1</b> — this list.</p>

<h3>Textures</h3>
<p><b>Wheel</b> over the preview — zoom (uncouples from Fit at the current
scale) · <b>drag</b> — pan · <b>double-click</b> — back to Fit ·
<b>Ctrl+wheel</b> over the grid — resize thumbnails.<br>
The pixel line under the preview follows the cursor: coordinates, RGBA and
hex of the pixel, plus the current zoom.<br>
Atlas frames: click a frame to preview just that frame; selection in the
frames table drives "Export frames…" (nothing selected = all frames).</p>

<h3>Models</h3>
<p><b>Wheel</b> on the timeline — step exactly one frame ·
<b>Shift+wheel</b> — playback speed.<br>
<b>Left-drag</b> in the viewport — orbit · <b>middle/right-drag</b> — pan ·
<b>wheel</b> — dolly.<br>
Parts checkboxes hide submeshes; All / None / Invert act on the whole list.
The animation box is searchable — type to filter hundreds of clips.</p>

<h3>Wardrobe</h3>
<p>Click any slot box to open its full list (they are searchable too).
Prev/Next step armor sets with wrap-around; the variant arrows step dye /
awakened looks per slot.</p>

<h3>Bulk Extract</h3>
<p><b>Esc</b> — cancel a running extraction.<br>
<b>Double-click</b> a match — toggle just that row without losing the rest of
the selection.<br>
"Pick items manually" turns the left list into a picker; the queue on the
right is persistent — it survives filter changes, mode switches and
restarts.</p>

<h3>Menus</h3>
<p>Context menus follow one vocabulary: a trailing "…" always means a dialog
opens; "to last folder" actions name the folder they will write to; copy
actions show the value they will copy.</p>
)HTML"));
    lay->addWidget(browser, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
}

// Every Settings entry point goes through here. The dialog writes its keys
// live, and its Cancel and Restore Defaults paths rewrite them again on the way
// out — so the tabs that CACHE one of those keys in a widget have to re-read it
// afterwards, or the viewport and the exporter end up disagreeing about, say,
// whether cloth is baked. Three separate call sites used to open the dialog and
// none of them did this.
void MainWindow::openSettings()
{
    SettingsDialog dlg(this);
    dlg.exec();
    if (m_modelsTab)   m_modelsTab->syncExportSettings();
    if (m_wardrobeTab) m_wardrobeTab->syncExportSettings();
}

void MainWindow::chooseGameFolder()
{
    // Accept either the game root or the MPK folder itself; normalise to MPK.
    QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Pick the Diablo Immortal install (or its Package\\MPK folder)"),
        mpkDir());
    if (dir.isEmpty()) return;
    if (QDir(dir + QStringLiteral("/Package/MPK")).exists())
        dir += QStringLiteral("/Package/MPK");
    QSettings().setValue(QLatin1String(kMpkDirKey), dir);
    qInfo("settings: mpkDir = %s", qPrintable(dir));
    reload();
}

void MainWindow::reload()
{
    const int generation = ++m_generation;
    const QString dir    = mpkDir();
    m_status->setProperty("loadSummary", QString());
    m_status->setText(QStringLiteral("Loading index from %1 …").arg(dir));

    QThread* worker = QThread::create([this, generation, dir] {
        seh::installSehTranslator();
        std::shared_ptr<AssetIndex> idx;
        QString error;
        const bool ok = seh::runGuarded("index-load", [&] {
            // Per-phase timing is permanent instrumentation: the log alone
            // answers where a slow startup went.
            QElapsedTimer total, phase;
            total.start();
            phase.start();
            auto store = std::make_shared<di::DiAssetStore>();
            std::string err;
            if (!store->open(dir.toStdString(), &err)) {
                error = QString::fromStdString(err);
                return;
            }
            const qint64 openMs = phase.restart();
            std::string rerr;
            if (!store->loadRepository(&rerr)) {
                // Non-fatal: the browser still works on physical names alone,
                // but say so plainly instead of pretending nothing happened.
                qWarning("repository: %s (browsing physical names only)", rerr.c_str());
            }
            const qint64 repoMs = phase.restart();

            // Derived-metadata cache, keyed on the pak signature — skips the
            // slowest phase (AssetIndex::build). Asset bytes are never cached.
            const QString cachePath = AssetIndexCache::defaultPath();
            idx = AssetIndexCache::load(store, cachePath);
            const bool cacheHit = (bool)idx;
            qint64 buildMs = phase.elapsed();
            if (!idx) {
                phase.restart();
                idx = AssetIndex::build(store);
                buildMs = phase.elapsed();
                AssetIndexCache::save(*idx, cachePath);   // logs its own ms
            }
            idx->loadMs = (double)total.elapsed();
            qInfo("index: mpk open %lld ms, repository %lld ms, rows %s %lld ms, "
                  "total %lld ms",
                  (long long)openMs, (long long)repoMs,
                  cacheHit ? "from cache in" : "built in", (long long)buildMs,
                  (long long)total.elapsed());
        });
        if (!ok && error.isEmpty())
            error = QStringLiteral("index load crashed (see log)");
        emit indexLoaded(idx, generation, error);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

void MainWindow::exportLog()
{
    const QString suggested =
        QDir::home().filePath(QStringLiteral("dibrowser_log_%1.txt")
                                  .arg(QDateTime::currentDateTime().toString(
                                      QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString dest = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export log"), suggested, QStringLiteral("Text files (*.txt)"));
    if (dest.isEmpty()) return;
    QFile f(dest);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        f.write(LogBuffer::instance().contents().toUtf8());
        f.write("\n");
        qInfo("log exported -> %s", qPrintable(dest));
    } else {
        QMessageBox::warning(this, QStringLiteral("Export log"),
                             QStringLiteral("Cannot write %1").arg(dest));
    }
}
