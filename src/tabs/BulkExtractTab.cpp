#include "tabs/BulkExtractTab.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QHash>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <thread>

#include "app/ExportReport.h"
#include "app/ExportSettings.h"
#include "app/SehGuard.h"
#include "index/AssetListModel.h"
#include "model/AnimParser.h"
#include "model/GlbExporter.h"
#include "model/MeshParser.h"
#include "model/ModelResolve.h"
#include "model/SkeletonTreeParser.h"
#include "model/SkinSkeletonParser.h"
#include "tex/TextureDecode.h"
#include "tex/TextureParser.h"
#include "util/FilterBar.h"
#include "util/HintBar.h"
#include "util/MenuText.h"
#include "util/PanelPersist.h"

namespace {

const char kModeKey[]     = "bulk/mode";
const char kOutDirKey[]   = "bulk/outDir";
const char kOldDestKey[]  = "bulk/destDir";   // pre-port key, read as fallback
const char kOrganizeKey[] = "bulk/organize";
const char kOnlyNewKey[]  = "bulk/onlyNew";
const char kManualKey[]   = "bulk/manual";
const char kParallelKey[] = "bulk/parallel";
const char kReportKey[]   = "bulk/report";
const char kAnimsKey[]    = "bulk/embedAnims";
const char* kQueueKeys[3] = {"bulk/queueGlb", "bulk/queueTex", "bulk/queueRaw"};

constexpr int kListCap = 5000;   // Matches widget rows (queries can be 600k)

// Make a repository/MPK path safe as a relative file path on Windows.
QString sanitizeRelPath(QString p)
{
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    static const QString bad = QStringLiteral(":*?\"<>|");
    for (const QChar c : bad)
        p.remove(c);
    while (p.startsWith(QLatin1Char('/')))
        p.remove(0, 1);
    p.replace(QStringLiteral(".."), QStringLiteral("__"));   // no escaping the root
    return p;
}

QString flatName(const QString& display)
{
    // The WHOLE path mangled into one token — leaf names alone collide across
    // folders (measured: 'diffuse'-class leaves repeat), the path never does.
    QString f = sanitizeRelPath(display);
    f.replace(QLatin1Char('/'), QLatin1Char('_'));
    return f;
}

QString csvField(const QString& s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) ||
        s.contains(QLatin1Char('\n'))) {
        QString q = s;
        q.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + q + QLatin1Char('"');
    }
    return s;
}

} // namespace

BulkExtractTab::BulkExtractTab(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    if (QWidget* hint = makeHintBar(
            this,
            QStringLiteral("Tip: the queue is persistent — pick items with the "
                           "filters set one way, change them, keep picking · "
                           "double-click toggles a row without losing the "
                           "selection · Esc cancels a run"),
            "hints/bulk"))
        lay->addWidget(hint);

    // ── row 1: mode · filter · presets ─────────────────────────────────────
    auto* row1 = new QHBoxLayout();
    m_modeBox = new QComboBox(this);
    m_modeBox->addItem(QStringLiteral("Models (.glb)"));
    m_modeBox->addItem(QStringLiteral("Textures (PNG)"));
    m_modeBox->addItem(QStringLiteral("Raw (original bytes)"));
    m_modeBox->setToolTip(QStringLiteral(
        "What each match becomes on disk.\n"
        "Models: full resolve -> parse -> rigged .glb per item.\n"
        "Textures: decoded largest mip as PNG.\n"
        "Raw: the exact stored bytes, untouched — any asset type."));
    row1->addWidget(m_modeBox);
    m_filter = new FilterBar(this);
    row1->addWidget(m_filter, 1);
    m_preset = new QComboBox(this);
    m_preset->setMinimumWidth(140);
    m_preset->setToolTip(QStringLiteral("Saved query presets"));
    row1->addWidget(m_preset);
    m_presetSave = new QPushButton(QStringLiteral("Save…"), this);
    m_presetSave->setToolTip(QStringLiteral(
        "Save the current mode, search, facets, layout and folder as a preset"));
    row1->addWidget(m_presetSave);
    m_presetDel = new QPushButton(QStringLiteral("Delete"), this);
    m_presetDel->setToolTip(QStringLiteral("Delete the selected preset"));
    row1->addWidget(m_presetDel);
    lay->addLayout(row1);

    // ── row 2: manual pick · options · parallel · count ────────────────────
    auto* row2 = new QHBoxLayout();
    m_manual = new QCheckBox(QStringLiteral("Pick items manually"), this);
    m_manual->setToolTip(QStringLiteral(
        "Off: every match is extracted.\n"
        "On: select matches on the left to build the extract queue on the "
        "right (click, Ctrl/Shift-click, double-click to toggle, Ctrl+A for "
        "all). The queue survives filter changes and restarts."));
    row2->addWidget(m_manual);
    m_selAll = new QPushButton(QStringLiteral("Select all"), this);
    m_selAll->setFixedWidth(88);
    row2->addWidget(m_selAll);
    m_selNone = new QPushButton(QStringLiteral("Select none"), this);
    m_selNone->setFixedWidth(94);
    m_selNone->setToolTip(QStringLiteral("Clears the WHOLE queue, not just "
                                         "the visible rows"));
    row2->addWidget(m_selNone);
    auto* sep = new QLabel(QStringLiteral("|"), this);
    sep->setStyleSheet(QStringLiteral("color:#4a4a4a;"));
    row2->addWidget(sep);
    auto* embedAnims = new QCheckBox(QStringLiteral("Embed all animations"), this);
    embedAnims->setToolTip(QStringLiteral(
        "Models mode: parse every clip in the model's folder and embed them "
        "in each .glb. Slower and much larger files."));
    embedAnims->setChecked(QSettings().value(QLatin1String(kAnimsKey), false).toBool());
    connect(embedAnims, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QLatin1String(kAnimsKey), on);
    });
    row2->addWidget(embedAnims);
    m_report = new QCheckBox(QStringLiteral("Write report"), this);
    m_report->setToolTip(QStringLiteral(
        "After each run, write _bulk_report.csv (name · id · status · reason "
        "· size) into the output folder."));
    m_report->setChecked(QSettings().value(QLatin1String(kReportKey), false).toBool());
    connect(m_report, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QLatin1String(kReportKey), on);
    });
    row2->addWidget(m_report);
    row2->addWidget(new QLabel(QStringLiteral("Parallel:"), this));
    m_parallel = new QComboBox(this);
    m_parallel->addItem(QStringLiteral("Auto"), -1);
    for (int n : {1, 2, 4, 8, 16})
        m_parallel->addItem(QString::number(n), n);
    m_parallel->setToolTip(QStringLiteral(
        "Concurrent workers. Auto = CPU core count. Pak reads are serialized "
        "per file, so N workers stream N paks. Output is byte-identical to a "
        "single-threaded run."));
    {
        const int saved = QSettings().value(QLatin1String(kParallelKey), -1).toInt();
        const int at = m_parallel->findData(saved);
        m_parallel->setCurrentIndex(at < 0 ? 0 : at);
    }
    connect(m_parallel, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                QSettings().setValue(QLatin1String(kParallelKey),
                                     m_parallel->currentData().toInt());
            });
    row2->addWidget(m_parallel);
    row2->addStretch(1);
    m_count = new QLabel(this);
    m_count->setStyleSheet(QStringLiteral("color:#d8a23a;font-weight:bold;"));
    row2->addWidget(m_count);
    lay->addLayout(row2);

    // ── row 3: destination · organize · overwrite policy ───────────────────
    auto* row3 = new QHBoxLayout();
    row3->addWidget(new QLabel(QStringLiteral("To:"), this));
    m_outDir = new QLineEdit(this);
    m_outDir->setReadOnly(true);
    m_outDir->setPlaceholderText(
        QStringLiteral("no folder yet — use \"Extract to…\""));
    m_outDir->setToolTip(QStringLiteral(
        "The last folder you extracted to. Change it with \"Extract to…\"."));
    {
        QSettings s;
        QString d = s.value(QLatin1String(kOutDirKey)).toString();
        if (d.isEmpty()) d = s.value(QLatin1String(kOldDestKey)).toString();
        m_outDir->setText(d);
    }
    row3->addWidget(m_outDir, 1);
    m_organize = new QComboBox(this);
    m_organize->addItem(QStringLiteral("Flat"));                    // 0
    m_organize->addItem(QStringLiteral("Subfolders by folder"));    // 1
    m_organize->addItem(QStringLiteral("Subfolders by type"));      // 2
    m_organize->setToolTip(QStringLiteral(
        "Output layout. Flat mangles the whole repository path into one "
        "unique file name; by folder mirrors the repository tree; by type "
        "groups by asset type."));
    m_organize->setCurrentIndex(
        qBound(0, QSettings().value(QLatin1String(kOrganizeKey), 0).toInt(), 2));
    connect(m_organize, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) {
                QSettings().setValue(QLatin1String(kOrganizeKey), i);
                updateMatches();   // present-markers key off the output path
            });
    row3->addWidget(m_organize);
    m_onlyNew = new QRadioButton(QStringLiteral("Only new"), this);
    m_onlyNew->setToolTip(QStringLiteral(
        "Reads the folder's _bulk_manifest.json + existing files and extracts "
        "only what is missing — re-running after a patch extracts just the "
        "new items."));
    m_overwrite = new QRadioButton(QStringLiteral("Overwrite"), this);
    m_overwrite->setToolTip(
        QStringLiteral("Re-extract everything that matches, overwriting."));
    const bool onlyNew = QSettings().value(QLatin1String(kOnlyNewKey), true).toBool();
    m_onlyNew->setChecked(onlyNew);
    m_overwrite->setChecked(!onlyNew);
    connect(m_onlyNew, &QRadioButton::toggled, this, [this](bool on) {
        QSettings().setValue(QLatin1String(kOnlyNewKey), on);
        updateMatches();
    });
    row3->addWidget(m_onlyNew);
    row3->addWidget(m_overwrite);
    lay->addLayout(row3);

    // ── row 4: extract buttons ─────────────────────────────────────────────
    auto* row4 = new QHBoxLayout();
    m_extract = new QPushButton(QStringLiteral("Extract"), this);
    m_extract->setStyleSheet(QStringLiteral("font-weight:bold;"));
    m_extract->setToolTip(QStringLiteral(
        "Extract the work set (queue in manual mode, every match otherwise) "
        "to the folder above."));
    connect(m_extract, &QPushButton::clicked, this, [this] { doExtract(false); });
    row4->addWidget(m_extract, 1);
    m_extractTo = new QPushButton(QStringLiteral("Extract to…"), this);
    m_extractTo->setToolTip(QStringLiteral(
        "Pick a folder, then extract there (and remember it as the last "
        "folder)."));
    connect(m_extractTo, &QPushButton::clicked, this, [this] { doExtract(true); });
    row4->addWidget(m_extractTo);
    row4->addStretch(1);
    m_openBtn = new QPushButton(QStringLiteral("Open folder"), this);
    m_openBtn->setToolTip(
        QStringLiteral("Open the current output folder in your file manager."));
    connect(m_openBtn, &QPushButton::clicked, this, [this] {
        const QString dir = m_outDir->text();
        if (!dir.isEmpty() && QDir(dir).exists())
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    row4->addWidget(m_openBtn);
    m_copyBtn = new QPushButton(QStringLiteral("Copy list"), this);
    m_copyBtn->setToolTip(QStringLiteral(
        "Copy the matched names to the clipboard (one per line)."));
    connect(m_copyBtn, &QPushButton::clicked, this, [this] {
        const QList<Job> matches = computeMatches();
        QStringList names;
        names.reserve(matches.size());
        for (const Job& j : matches) names << j.name;
        QApplication::clipboard()->setText(names.join(QLatin1Char('\n')));
        emit statusText(QStringLiteral("Copied %1 name(s) to the clipboard.")
                            .arg(names.size()));
    });
    row4->addWidget(m_copyBtn);
    lay->addLayout(row4);

    m_lastRun = new QLabel(this);
    m_lastRun->setStyleSheet(QStringLiteral("color:#8fbf8f;"));
    m_lastRun->setWordWrap(true);
    m_lastRun->hide();
    lay->addWidget(m_lastRun);

    // ── matches | queue ────────────────────────────────────────────────────
    auto* split = new QSplitter(Qt::Horizontal, this);
    auto* leftW = new QWidget(split);
    auto* leftL = new QVBoxLayout(leftW);
    leftL->setContentsMargins(0, 0, 0, 0);
    m_listLbl = new QLabel(QStringLiteral("Matches (0)"), this);
    m_listLbl->setAlignment(Qt::AlignCenter);
    leftL->addWidget(m_listLbl);
    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);   // manual flips it
    m_list->viewport()->installEventFilter(this);   // double-click toggle
    leftL->addWidget(m_list, 1);
    split->addWidget(leftW);
    auto* rightW = new QWidget(split);
    auto* rightL = new QVBoxLayout(rightW);
    rightL->setContentsMargins(0, 0, 0, 0);
    m_queueLbl = new QLabel(QStringLiteral("Queue (0)"), this);
    m_queueLbl->setAlignment(Qt::AlignCenter);
    rightL->addWidget(m_queueLbl);
    m_queue = new QListWidget(this);
    m_queue->setUniformItemSizes(true);
    m_queue->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queue->setToolTip(QStringLiteral(
        "Items queued for extraction. Double-click (or right-click) to "
        "remove one."));
    rightL->addWidget(m_queue, 1);
    split->addWidget(rightW);
    split->setSizes({10000, 10000});   // perfectly centred start
    PanelPersist::bind(split, QStringLiteral("panels/bulkSplit"));
    lay->addWidget(split, 1);

    // ── run bar + console ──────────────────────────────────────────────────
    auto* runRow = new QHBoxLayout();
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    m_progress->setFormat(QStringLiteral("%v / %m"));
    m_progress->hide();
    runRow->addWidget(m_progress, 1);
    m_pauseBtn = new QPushButton(QStringLiteral("Pause"), this);
    m_pauseBtn->hide();
    runRow->addWidget(m_pauseBtn);
    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancelBtn->hide();
    runRow->addWidget(m_cancelBtn);
    lay->addLayout(runRow);
    m_console = new QPlainTextEdit(this);
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(5000);   // ring buffer: long sessions can't balloon it
    m_console->setMinimumHeight(120);
    m_console->setPlaceholderText(QStringLiteral(
        "Run output appears here — every failure with its reason."));
    m_console->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:#141414;color:#c8c8c8;"
        "font-family:Consolas,'Courier New',monospace;font-size:11px;}"));
    lay->addWidget(m_console);

    connect(m_cancelBtn, &QPushButton::clicked, this, [this] {
        if (m_runCancel) *m_runCancel = true;
        m_cancelBtn->setText(QStringLiteral("Canceling…"));
        m_cancelBtn->setEnabled(false);
        logLine(QStringLiteral("Cancel requested — stopping after the current "
                               "item…"));
    });
    // Esc cancels a run — WidgetWithChildrenShortcut so it steals nothing
    // when this tab is not focused.
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, [this] {
        if (m_running && m_cancelBtn->isEnabled()) m_cancelBtn->click();
    });
    connect(m_pauseBtn, &QPushButton::clicked, this, [this] {
        if (!m_runPause) return;
        const bool pausing = !m_runPause->load();
        *m_runPause = pausing;
        m_pauseBtn->setText(pausing ? QStringLiteral("Resume")
                                    : QStringLiteral("Pause"));
        if (pausing) {
            m_pauseT0 = QDateTime::currentMSecsSinceEpoch();
            logLine(QStringLiteral("Paused — finishing the current item, then "
                                   "holding."));
        } else {
            m_pausedMs += QDateTime::currentMSecsSinceEpoch() - m_pauseT0;
            logLine(QStringLiteral("Resumed."));
        }
    });

    m_model = new AssetListModel(this);   // headless: filtering engine only
    connect(m_model, &QAbstractItemModel::modelReset, this,
            [this] { updateMatches(); });
    connect(m_filter, &FilterBar::changed, this, &BulkExtractTab::applyFilter);

    connect(m_modeBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int m) { setMode(m); });
    m_manual->setChecked(QSettings().value(QLatin1String(kManualKey), false).toBool());
    connect(m_manual, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QLatin1String(kManualKey), on);
        m_list->setSelectionMode(on ? QAbstractItemView::ExtendedSelection
                                    : QAbstractItemView::NoSelection);
        m_selAll->setEnabled(on);
        m_selNone->setEnabled(on);
        m_queue->setEnabled(on);
        m_queueLbl->setEnabled(on);
        if (on) reflectQueueInList();
        else m_list->clearSelection();
        syncExtractButtons();
    });
    m_list->setSelectionMode(m_manual->isChecked()
                                 ? QAbstractItemView::ExtendedSelection
                                 : QAbstractItemView::NoSelection);
    m_selAll->setEnabled(m_manual->isChecked());
    m_selNone->setEnabled(m_manual->isChecked());
    m_queue->setEnabled(m_manual->isChecked());
    connect(m_selAll, &QPushButton::clicked, this, [this] {
        if (!m_manual->isChecked()) return;
        m_list->selectAll();   // selectionChanged syncs the queue
    });
    connect(m_selNone, &QPushButton::clicked, this, [this] {
        m_queued.clear();      // the WHOLE queue, not just visible rows
        saveQueue();
        rebuildQueueWidget();
        reflectQueueInList();
        syncExtractButtons();
    });
    connect(m_list, &QListWidget::itemSelectionChanged, this,
            [this] { syncQueueFromSelection(); });
    connect(m_queue, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* it) {
                m_queued.remove(it->data(Qt::UserRole).toString());
                saveQueue();
                rebuildQueueWidget();
                reflectQueueInList();
                syncExtractButtons();
            });
    m_queue->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_queue, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& p) {
                QMenu menu(this);
                QListWidgetItem* it = m_queue->itemAt(p);
                if (it) {
                    const QString name = it->data(Qt::UserRole).toString();
                    menu.addAction(QStringLiteral("Remove from queue"), this,
                                   [this, name] {
                                       m_queued.remove(name);
                                       saveQueue();
                                       rebuildQueueWidget();
                                       reflectQueueInList();
                                       syncExtractButtons();
                                   });
                }
                menu.addAction(QStringLiteral("Clear queue"), this, [this] {
                    m_queued.clear();
                    saveQueue();
                    rebuildQueueWidget();
                    reflectQueueInList();
                    syncExtractButtons();
                });
                menu.exec(m_queue->viewport()->mapToGlobal(p));
            });

    // presets
    connect(m_presetSave, &QPushButton::clicked, this, [this] { savePreset(); });
    connect(m_presetDel, &QPushButton::clicked, this, [this] { deletePreset(); });
    connect(m_preset, qOverload<int>(&QComboBox::activated), this, [this](int) {
        const QString name = m_preset->currentData().toString();
        if (!name.isEmpty()) loadPreset(name);
    });

    // worker -> GUI
    connect(this, &BulkExtractTab::runLog, this,
            [this](int generation, QString line) {
                if (generation != m_generation.load()) return;
                logLine(line);
            },
            Qt::QueuedConnection);
    connect(this, &BulkExtractTab::runProgress, this,
            [this](int generation, int done, int total, int failed) {
                if (generation != m_generation.load()) return;
                m_progress->setMaximum(total);
                m_progress->setValue(done);
                // Rate + ETA once there is signal, from ACTIVE time only —
                // a coffee-length pause must not poison the estimate.
                qint64 paused = m_pausedMs;
                if (m_runPause && m_runPause->load())
                    paused += QDateTime::currentMSecsSinceEpoch() - m_pauseT0;
                const double activeSec =
                    (QDateTime::currentMSecsSinceEpoch() - m_runStartMs - paused) /
                    1000.0;
                if (done >= 5 && activeSec > 2.0) {
                    const double rate = done / activeSec;
                    const qint64 left =
                        rate > 0 ? qint64((total - done) / rate) : 0;
                    const QString leftTxt =
                        left >= 3600
                            ? QStringLiteral("%1h %2m").arg(left / 3600).arg((left % 3600) / 60)
                            : left >= 60
                                  ? QStringLiteral("%1m %2s").arg(left / 60).arg(left % 60)
                                  : QStringLiteral("%1s").arg(left);
                    m_progress->setFormat(
                        QStringLiteral("%v / %m  ·  %1/s  ·  ~%2 left  ·  %3 failed")
                            .arg(rate < 10 ? QString::number(rate, 'f', 1)
                                           : QString::number(qRound(rate)))
                            .arg(leftTxt)
                            .arg(failed));
                }
            },
            Qt::QueuedConnection);
    connect(this, &BulkExtractTab::runFinished, this,
            [this](int generation, int written, int skipped, int failed,
                   qint64 bytes, qint64 elapsedMs, bool canceled) {
                if (generation != m_generation.load()) return;
                m_running = false;
                m_extract->setEnabled(true);
                m_extractTo->setEnabled(true);
                m_modeBox->setEnabled(true);
                m_pauseBtn->hide();
                m_cancelBtn->hide();
                m_progress->setValue(m_progress->maximum());
                QString summary =
                    QStringLiteral("Last run: %L1 new item(s), %L2 skipped, %3 "
                                   "failed, %4, %L5 ms → %6")
                        .arg(written)
                        .arg(skipped)
                        .arg(failed)
                        .arg(QLocale::c().formattedDataSize(bytes, 1))
                        .arg(elapsedMs)
                        .arg(m_outDir->text());
                if (failed)
                    summary += QStringLiteral("   ·   see _bulk_failed.txt");
                if (canceled) summary += QStringLiteral("   ·   CANCELED");
                m_lastRun->setText(summary);
                m_lastRun->show();
                logLine(QStringLiteral("── done in %1 s.  %2")
                            .arg(elapsedMs / 1000.0, 0, 'f', 1)
                            .arg(summary));
                qInfo("bulk: %s", qPrintable(summary));
                loadFolderManifest();
                updateMatches();
                syncExtractButtons();
            },
            Qt::QueuedConnection);

    // restore mode LAST (its handler touches queue + count widgets)
    m_prevMode = qBound(0, QSettings().value(QLatin1String(kModeKey), 2).toInt(), 2);
    m_modeBox->blockSignals(true);
    m_modeBox->setCurrentIndex(m_prevMode);
    m_modeBox->blockSignals(false);
    loadQueueForMode();
    refreshPresets();
    loadFolderManifest();
    syncExtractButtons();
}

BulkExtractTab::~BulkExtractTab()
{
    ++m_generation;
    if (m_runCancel) *m_runCancel = true;
    if (m_runPause)  *m_runPause  = false;   // never leave a worker parked
    const QSet<QThread*> workers = m_workers;
    for (QThread* w : workers)
        w->wait();
}

// Double-click additive toggle (D4 QoL, ported verbatim in spirit): in
// ExtendedSelection the first press of a double-click has already cleared the
// multi-selection, so snapshot it on the press and restore it around the
// toggle.
bool BulkExtractTab::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_list->viewport() && m_manual->isChecked()) {
        if (ev->type() == QEvent::MouseButtonPress) {
            static qint64 lastPress = 0;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastPress > QApplication::doubleClickInterval()) {
                m_preClickSel.clear();
                for (const QModelIndex& mi :
                     m_list->selectionModel()->selectedIndexes())
                    m_preClickSel.append(mi.row());
            }
            lastPress = now;
        } else if (ev->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(ev);
            QListWidgetItem* it = m_list->itemAt(me->pos());
            if (it) {
                const int row = m_list->row(it);
                QSignalBlocker block(m_list->selectionModel());
                m_list->clearSelection();
                for (int r : m_preClickSel)
                    if (QListWidgetItem* pi = m_list->item(r))
                        pi->setSelected(true);
                it->setSelected(!m_preClickSel.contains(row));
                block.unblock();
                syncQueueFromSelection();
                return true;   // swallow: the view must not re-select on top
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void BulkExtractTab::setIndex(std::shared_ptr<AssetIndex> idx)
{
    ++m_generation;                            // detach the UI from any old run
    if (m_runCancel) *m_runCancel = true;      // and cancel it for real
    if (m_runPause)  *m_runPause  = false;
    m_idx = std::move(idx);
    m_filter->setIndex(m_idx);
    m_running = false;
    m_extract->setEnabled(true);
    m_extractTo->setEnabled(true);
    m_modeBox->setEnabled(true);
    m_pauseBtn->hide();
    m_cancelBtn->hide();
    // Same regression class as ModelsTab: the model gets the index HERE.
    // Its reset signal triggers updateMatches().
    m_model->setIndex(m_idx, m_filter->spec());
}

void BulkExtractTab::applyFilter()
{
    m_model->setFilters(m_filter->spec());   // modelReset -> updateMatches()
}

int BulkExtractTab::mode() const { return m_modeBox->currentIndex(); }

QSet<QString> BulkExtractTab::modeTypes() const
{
    switch (mode()) {
    case 0:
        return {QStringLiteral("Model"), QStringLiteral("LodModel"),
                QStringLiteral("Mesh")};
    case 1:
        return {QStringLiteral("Texture2D")};
    default:
        return {};   // raw: anything
    }
}

void BulkExtractTab::setMode(int m)
{
    saveQueue();   // outgoing mode keeps its queue
    m_prevMode = m;
    QSettings().setValue(QLatin1String(kModeKey), m);
    loadQueueForMode();
    updateMatches();
    syncExtractButtons();
}

QList<BulkExtractTab::Job> BulkExtractTab::computeMatches() const
{
    QList<Job> out;
    if (!m_idx) return out;
    const QSet<QString> types = modeTypes();
    out.reserve(m_model->visibleCount());
    for (int row = 0; row < m_model->visibleCount(); ++row) {
        const AssetRow* r = m_model->rowAt(row);
        if (!r) continue;
        if (!types.isEmpty() && !types.contains(r->type)) continue;
        out.append({(int)r->entryId, r->repoIdx, r->display, r->type});
    }
    return out;
}

void BulkExtractTab::updateMatches()
{
    const QList<Job> matches = computeMatches();
    const int curMode = mode();
    const int organize = m_organize->currentIndex();
    auto isPresent = [&](const Job& j) {
        return m_folderDone.contains(
            outputRelPath(j.name, j.type, curMode, organize));
    };
    int present = 0;
    for (const Job& j : matches)
        if (isPresent(j)) ++present;
    static const char* nouns[3] = {"model", "texture", "asset"};
    QString label = QStringLiteral("%L1 %2(s) match")
                        .arg(matches.size())
                        .arg(QLatin1String(nouns[mode()]));
    if (present > 0)
        label += QStringLiteral("   ·   %L1 new · %L2 already in folder")
                     .arg(matches.size() - present)
                     .arg(present);
    m_count->setText(label);
    m_listLbl->setText(QStringLiteral("Matches (%L1)").arg(matches.size()));

    m_syncingQueue = true;
    QSignalBlocker block(m_list->selectionModel());
    m_list->clear();
    const int cap = qMin((int)matches.size(), kListCap);
    for (int i = 0; i < cap; ++i) {
        const Job& j = matches[i];
        const bool done = isPresent(j);
        auto* it = new QListWidgetItem(
            done ? QStringLiteral("[done] %1").arg(j.name) : j.name);
        it->setData(Qt::UserRole, j.name);
        it->setForeground(done ? QColor(120, 130, 120) : QColor(215, 215, 215));
        if (done)
            it->setToolTip(m_onlyNew->isChecked()
                               ? QStringLiteral("Already in this folder — will "
                                                "be skipped")
                               : QStringLiteral("Already in this folder — will "
                                                "be overwritten"));
        m_list->addItem(it);
    }
    if ((int)matches.size() > cap) {
        auto* more = new QListWidgetItem(
            QStringLiteral("…and %L1 more (extract still covers them all)")
                .arg(matches.size() - cap));
        more->setFlags(Qt::NoItemFlags);
        m_list->addItem(more);
    }
    block.unblock();
    m_syncingQueue = false;
    if (m_manual->isChecked()) reflectQueueInList();
    syncExtractButtons((int)matches.size());   // no second full recompute
}

// Reconcile ONLY the visible rows into the queue — items queued under another
// filter (or past the list cap) are left alone. That is what makes the queue
// a store instead of a mirror.
void BulkExtractTab::syncQueueFromSelection()
{
    if (m_syncingQueue || !m_manual->isChecked()) return;
    bool changed = false;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        const QString name = it->data(Qt::UserRole).toString();
        if (name.isEmpty()) continue;   // the "…and N more" row
        const bool sel = it->isSelected();
        if (sel && !m_queued.contains(name)) {
            m_queued.insert(name);
            changed = true;
        } else if (!sel && m_queued.contains(name)) {
            m_queued.remove(name);
            changed = true;
        }
    }
    if (changed) {
        saveQueue();
        rebuildQueueWidget();
        syncExtractButtons();
    }
}

void BulkExtractTab::rebuildQueueWidget()
{
    m_queue->clear();
    QStringList names(m_queued.begin(), m_queued.end());
    names.sort(Qt::CaseInsensitive);
    for (const QString& n : names) {
        auto* it = new QListWidgetItem(n);
        it->setData(Qt::UserRole, n);
        m_queue->addItem(it);
    }
    m_queueLbl->setText(QStringLiteral("Queue (%L1)").arg(m_queued.size()));
}

void BulkExtractTab::reflectQueueInList()
{
    if (!m_manual->isChecked()) return;
    m_syncingQueue = true;
    QSignalBlocker block(m_list->selectionModel());
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        const QString name = it->data(Qt::UserRole).toString();
        if (!name.isEmpty()) it->setSelected(m_queued.contains(name));
    }
    block.unblock();
    m_syncingQueue = false;
}

void BulkExtractTab::saveQueue()
{
    QStringList names(m_queued.begin(), m_queued.end());
    QSettings().setValue(QLatin1String(kQueueKeys[m_prevMode]), names);
}

void BulkExtractTab::loadQueueForMode()
{
    m_queued.clear();
    const QStringList names =
        QSettings().value(QLatin1String(kQueueKeys[mode()])).toStringList();
    for (const QString& n : names)
        if (!n.isEmpty()) m_queued.insert(n);
    rebuildQueueWidget();
    reflectQueueInList();
}

QList<BulkExtractTab::Job> BulkExtractTab::workSet() const
{
    if (!m_manual->isChecked()) return computeMatches();
    // Resolve queued NAMES against the current index (names survive patches;
    // entry ids do not). One pass over the rows; missing names are dropped
    // and reported by doExtract.
    QList<Job> out;
    if (!m_idx || m_queued.isEmpty()) return out;
    const QSet<QString> types = modeTypes();
    for (const AssetRow& r : m_idx->rows) {
        if (!m_queued.contains(r.display)) continue;
        if (!types.isEmpty() && !types.contains(r.type)) continue;
        out.append({(int)r.entryId, r.repoIdx, r.display, r.type});
    }
    return out;
}

void BulkExtractTab::syncExtractButtons(int knownMatchCount)
{
    const int n = m_manual->isChecked()
                      ? (int)m_queued.size()
                      : (knownMatchCount >= 0 ? knownMatchCount
                                              : (int)computeMatches().size());
    static const char* nouns[3] = {"model", "texture", "asset"};
    if (n <= 0) {
        m_extract->setText(QStringLiteral("Extract"));
    } else if (m_manual->isChecked()) {
        m_extract->setText(QStringLiteral("Extract %L1 queued").arg(n));
    } else {
        m_extract->setText(QStringLiteral("Extract %L1 %2(s)")
                               .arg(n)
                               .arg(QLatin1String(nouns[mode()])));
    }
    m_extract->setEnabled(n > 0 && !m_running);
    m_extractTo->setEnabled(n > 0 && !m_running);
}

// The ONE output-path rule. glb/png replace the extension; raw keeps the
// original leaf untouched. Bulk .glb names do NOT carry the single-export
// "_rigged" suffix — the incremental ledger needs the name computable before
// the mesh is parsed.
QString BulkExtractTab::outputRelPath(const QString& name, const QString& type,
                                      int mode, int organize)
{
    // glb/png outputs mangle embedded dots ("body.Mesh" -> "body_Mesh.glb" —
    // a stray ".Mesh" component confuses importers); raw keeps the original
    // leaf byte-for-byte. Applied identically in every layout so the same
    // asset always has ONE computable name per (mode, organize).
    const bool convert = mode != 2;
    const QString ext = mode == 0 ? QStringLiteral(".glb")
                       : mode == 1 ? QStringLiteral(".png")
                                   : QString();
    QString rel;
    switch (organize) {
    case 1: {   // subfolders by (repository) folder — tree mirror
        QString dirPart = name;
        QString leaf = name;
        const int slash = name.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0) {
            dirPart = sanitizeRelPath(name.left(slash));
            leaf    = name.mid(slash + 1);
        } else {
            dirPart.clear();
        }
        if (convert) leaf.replace(QLatin1Char('.'), QLatin1Char('_'));
        leaf = sanitizeRelPath(leaf);
        rel = dirPart.isEmpty() ? leaf : dirPart + QLatin1Char('/') + leaf;
        break;
    }
    case 2: {   // subfolders by type
        QString flat = flatName(name);
        if (convert) flat.replace(QLatin1Char('.'), QLatin1Char('_'));
        rel = sanitizeRelPath(type) + QLatin1Char('/') + flat;
        break;
    }
    default: {  // flat: whole path mangled into one unique token
        rel = flatName(name);
        if (convert) rel.replace(QLatin1Char('.'), QLatin1Char('_'));
        break;
    }
    }
    return rel + ext;
}

void BulkExtractTab::loadFolderManifest()
{
    // Keyed by the OUTPUT rel path ("file"), not the asset name: presence is a
    // property of the file the current (mode, layout) would write. Keying by
    // name made a layout switch skip everything as "already present" while
    // writing nothing (review finding).
    m_folderDone.clear();
    const QString dir = m_outDir->text();
    if (dir.isEmpty()) return;
    QFile f(QDir(dir).filePath(QStringLiteral("_bulk_manifest.json")));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    for (const QJsonValue& v : doc.array()) {
        const QString file = v.toObject().value(QStringLiteral("file")).toString();
        if (!file.isEmpty()) m_folderDone.insert(file);
    }
}

void BulkExtractTab::refreshPresets()
{
    m_preset->blockSignals(true);
    m_preset->clear();
    m_preset->addItem(QStringLiteral("(load preset…)"), QString());
    QSettings s;
    s.beginGroup(QStringLiteral("bulk/presets"));
    QStringList names = s.childGroups();
    s.endGroup();
    names.sort(Qt::CaseInsensitive);
    for (const QString& n : names)
        m_preset->addItem(n, n);
    m_preset->setCurrentIndex(0);
    m_preset->blockSignals(false);
}

void BulkExtractTab::savePreset()
{
    const QString name = QInputDialog::getText(
                             this, QStringLiteral("Save preset"),
                             QStringLiteral("Preset name:"))
                             .trimmed();
    if (name.isEmpty()) return;
    const FilterSpec spec = m_filter->spec();
    QSettings s;
    s.beginGroup(QStringLiteral("bulk/presets/") + name);
    s.setValue(QStringLiteral("mode"), mode());
    s.setValue(QStringLiteral("text"), spec.text);
    s.setValue(QStringLiteral("types"), QStringList(spec.types.begin(), spec.types.end()));
    s.setValue(QStringLiteral("cats"), QStringList(spec.cats.begin(), spec.cats.end()));
    s.setValue(QStringLiteral("subs"), QStringList(spec.subcats.begin(), spec.subcats.end()));
    s.setValue(QStringLiteral("organize"), m_organize->currentIndex());
    s.setValue(QStringLiteral("onlyNew"), m_onlyNew->isChecked());
    s.setValue(QStringLiteral("outDir"), m_outDir->text());
    s.endGroup();
    refreshPresets();
    const int at = m_preset->findData(name);
    if (at >= 0) m_preset->setCurrentIndex(at);
}

void BulkExtractTab::deletePreset()
{
    const QString name = m_preset->currentData().toString();
    if (name.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("Delete preset"),
                              QStringLiteral("Delete preset \"%1\"?").arg(name)) !=
        QMessageBox::Yes)
        return;
    QSettings().remove(QStringLiteral("bulk/presets/") + name);
    refreshPresets();
}

void BulkExtractTab::loadPreset(const QString& name)
{
    QSettings s;
    s.beginGroup(QStringLiteral("bulk/presets/") + name);
    if (s.childKeys().isEmpty()) {
        s.endGroup();
        return;
    }
    const int m = qBound(0, s.value(QStringLiteral("mode")).toInt(), 2);
    FilterSpec spec;
    spec.text = s.value(QStringLiteral("text")).toString();
    const QStringList t = s.value(QStringLiteral("types")).toStringList();
    const QStringList c = s.value(QStringLiteral("cats")).toStringList();
    const QStringList u = s.value(QStringLiteral("subs")).toStringList();
    spec.types   = QSet<QString>(t.begin(), t.end());
    spec.cats    = QSet<QString>(c.begin(), c.end());
    spec.subcats = QSet<QString>(u.begin(), u.end());
    const int organize = qBound(0, s.value(QStringLiteral("organize")).toInt(), 2);
    const bool onlyNew = s.value(QStringLiteral("onlyNew"), true).toBool();
    const QString outDir = s.value(QStringLiteral("outDir")).toString();
    s.endGroup();

    m_modeBox->setCurrentIndex(m);            // setMode runs via the signal
    m_organize->setCurrentIndex(organize);
    m_onlyNew->setChecked(onlyNew);
    m_overwrite->setChecked(!onlyNew);
    if (!outDir.isEmpty()) {
        m_outDir->setText(outDir);
        QSettings().setValue(QLatin1String(kOutDirKey), outDir);
        loadFolderManifest();
    }
    m_filter->applySpec(spec);                // one changed() -> updateMatches
}

void BulkExtractTab::logLine(const QString& s)
{
    m_console->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                 s));
    m_console->verticalScrollBar()->setValue(
        m_console->verticalScrollBar()->maximum());
}

void BulkExtractTab::doExtract(bool promptDir, QList<Job> explicitJobs)
{
    if (m_running || !m_idx || !m_idx->store) return;
    QList<Job> jobs = explicitJobs.isEmpty() ? workSet() : std::move(explicitJobs);
    if (m_manual->isChecked() && explicitJobs.isEmpty() &&
        jobs.size() < (int)m_queued.size()) {
        logLine(QStringLiteral("%1 queued name(s) are not in this index (or "
                               "not this mode's type) — skipped.")
                    .arg((int)m_queued.size() - jobs.size()));
    }
    if (jobs.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Bulk extract"),
                                 QStringLiteral("Nothing to extract — widen "
                                                "the filters or queue some "
                                                "items."));
        return;
    }

    QString dir = m_outDir->text().trimmed();
    if (promptDir || dir.isEmpty()) {
        dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Extract to folder"),
            dir.isEmpty() ? QDir::homePath() : dir);
        if (dir.isEmpty()) return;
        m_outDir->setText(dir);
        QSettings().setValue(QLatin1String(kOutDirKey), dir);
        loadFolderManifest();
    }
    if (!QDir().mkpath(dir)) {
        logLine(QStringLiteral("Cannot create destination %1").arg(dir));
        return;
    }

    // Size guard: quote REAL source bytes before a big run.
    if (jobs.size() > 500) {
        qint64 src = 0;
        const auto& entries = m_idx->store->mpk().entries();
        for (const Job& j : jobs)
            if (j.entryId >= 0 && (size_t)j.entryId < entries.size())
                src += entries[(size_t)j.entryId].length;
        const double gb = src / (1024.0 * 1024.0 * 1024.0);
        if (QMessageBox::question(
                this, QStringLiteral("Bulk extract"),
                QStringLiteral("This will extract up to %L1 item(s) (~%2 GB "
                               "source data) to:\n%3\n\nContinue?")
                    .arg(jobs.size())
                    .arg(gb, 0, 'f', gb < 1.0 ? 2 : 1)
                    .arg(dir)) != QMessageBox::Yes)
            return;
    }

    const int generation = ++m_generation;
    auto cancel = std::make_shared<std::atomic<bool>>(false);   // per-run tokens
    auto pause  = std::make_shared<std::atomic<bool>>(false);
    m_runCancel = cancel;
    m_runPause  = pause;
    m_running = true;
    m_extract->setEnabled(false);
    m_extractTo->setEnabled(false);
    m_modeBox->setEnabled(false);   // a mode switch mid-run would desync it all
    m_progress->setRange(0, (int)jobs.size());
    m_progress->setValue(0);
    m_progress->setFormat(QStringLiteral("%v / %m"));
    m_progress->show();
    m_pauseBtn->setText(QStringLiteral("Pause"));
    m_pauseBtn->show();
    m_cancelBtn->setText(QStringLiteral("Cancel"));
    m_cancelBtn->setEnabled(true);
    m_cancelBtn->show();
    m_runStartMs = QDateTime::currentMSecsSinceEpoch();
    m_pausedMs = 0;

    const int  runMode  = mode();
    const int  organize = m_organize->currentIndex();
    const bool onlyNew  = m_onlyNew->isChecked();
    const bool report   = m_report->isChecked();
    const bool embedAnims =
        QSettings().value(QLatin1String(kAnimsKey), false).toBool();
    int par = m_parallel->currentData().toInt();
    if (par <= 0) par = QThread::idealThreadCount();
    par = qBound(1, par, 16);
    const QSet<QString> doneSet = m_folderDone;   // snapshot for the skip check

    static const char* modeNames[3] = {"Models (.glb)", "Textures (PNG)", "Raw"};
    logLine(QStringLiteral("── %1 run: %L2 item(s) → %3")
                .arg(QLatin1String(modeNames[runMode]))
                .arg(jobs.size())
                .arg(dir));
    logLine(QStringLiteral("   existing: %1 · layout: %2 · %3 worker(s)")
                .arg(onlyNew ? QStringLiteral("only new")
                             : QStringLiteral("overwrite"))
                .arg(m_organize->currentText().toLower())
                .arg(par));
    qInfo("bulk: starting %d items -> %s (mode=%d organize=%d par=%d onlyNew=%d)",
          (int)jobs.size(), qPrintable(dir), runMode, organize, par,
          (int)onlyNew);

    // Successes only get console lines on runs small enough to read; failures
    // ALWAYS log. (The 5000-block ring would silently eat them anyway — this
    // keeps the signal queue sane on 100k-item runs.)
    const bool verbose = jobs.size() <= 2000;

    auto store = m_idx->store;
    QThread* worker = QThread::create([this, generation, store, jobs, dir,
                                       runMode, organize, onlyNew, report,
                                       embedAnims, par, doneSet, cancel, pause,
                                       verbose] {
        seh::installSehTranslator();
        QElapsedTimer timer;
        timer.start();
        std::atomic<int> next{0}, done{0}, failed{0}, skipped{0};
        std::atomic<qint64> bytes{0};
        std::atomic<qint64> lastEmit{0};
        QMutex mtx;   // failures + manifest additions
        QStringList failures;
        struct ManifestAdd { QString name, file; };
        std::vector<ManifestAdd> added;
        QHash<QString, QString> statusByName;   // for the report

        // Cancel poll doubles as the PAUSE gate: workers sleep between items,
        // so pausing is instant and free, and Cancel breaks the hold.
        // The shared human-readable report (app/ExportReport.h), on top of this
        // tab's own machine-readable CSV — the CSV exists to be diffed between
        // runs, this one exists to be read when something went wrong.
        std::shared_ptr<ExportReport> xreport;
        if (ExportReport::enabled()) {
            xreport = std::make_shared<ExportReport>(
                QStringLiteral("Bulk extract"), dir);
            xreport->context(QStringLiteral("mode: %1")
                                 .arg(runMode == 2 ? QStringLiteral("raw")
                                      : runMode == 1 ? QStringLiteral("textures")
                                                     : QStringLiteral("models")));
            xreport->context(QStringLiteral("%1 job(s), %2 worker(s)")
                                 .arg(jobs.size()).arg(par));
            xreport->context(QStringLiteral("only new: %1 · organize: %2")
                                 .arg(onlyNew ? QStringLiteral("yes") : QStringLiteral("no"))
                                 .arg(organize ? QStringLiteral("yes") : QStringLiteral("no")));
        }

        auto canceled = [&]() -> bool {
            while (pause->load() && !cancel->load())
                QThread::msleep(100);
            return cancel->load() || generation != m_generation.load();
        };

        // Output paths handed out but not yet on disk. uniquePath() is
        // check-then-write and this pool is parallel, so without a reservation
        // two workers can both resolve to the same "_2" name and the second
        // write eats the first.
        QMutex pathMtx;
        QSet<QString> takenPaths;
        auto reservePath = [&](const QString& want) {
            QMutexLocker lock(&pathMtx);
            QString out = ExportSettings::uniquePath(want);
            while (takenPaths.contains(out.toLower())) {
                const QFileInfo fi(out);
                out = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() +
                      QStringLiteral("_x%1").arg(takenPaths.size()) +
                      (fi.suffix().isEmpty() ? QString()
                                             : QLatin1Char('.') + fi.suffix());
            }
            takenPaths.insert(out.toLower());
            return out;
        };

        auto processOne = [&](const Job& j) {
            const QString rel = outputRelPath(j.name, j.type, runMode, organize);
            const QString wantPath = QDir(dir).filePath(rel);
            if (onlyNew && (doneSet.contains(rel) || QFile::exists(wantPath))) {
                ++skipped;
                if (xreport)
                    xreport->skipped(rel, QStringLiteral("already extracted"));
                QMutexLocker lock(&mtx);
                statusByName.insert(j.name, QStringLiteral("skipped"));
                return;
            }
            // "Only new" already returned above, so this only bites when the
            // user wants everything re-extracted but not clobbered.
            const QString outPath = reservePath(wantPath);
            QString why;
            seh::HardwareFault fault;
            const bool guarded = seh::runGuarded("bulk-item", [&] {
                QDir().mkpath(QFileInfo(outPath).absolutePath());
                if (runMode == 2) {
                    // RAW: the exact stored bytes, untouched — no inflate.
                    const std::vector<uint8_t> raw =
                        store->mpk().read((size_t)j.entryId);
                    if (raw.empty()) {
                        why = QStringLiteral("read returned 0 bytes");
                        return;
                    }
                    QFile f(outPath);
                    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        why = QStringLiteral("cannot open for write");
                        return;
                    }
                    if (f.write(reinterpret_cast<const char*>(raw.data()),
                                (qint64)raw.size()) != (qint64)raw.size()) {
                        why = QStringLiteral("short write");
                        return;
                    }
                    bytes += (qint64)raw.size();
                } else if (runMode == 1) {
                    const std::vector<uint8_t> raw =
                        store->mpk().readAsset((size_t)j.entryId);
                    di::Texture2D tex;
                    std::string terr;
                    if (!di::isTexture2D(raw.data(), raw.size()) ||
                        !di::parseTexture2D(raw.data(), raw.size(), &tex, &terr)) {
                        why = QStringLiteral("texture parse failed: %1")
                                  .arg(QString::fromStdString(terr));
                        return;
                    }
                    TextureDecode::Result res = TextureDecode::decode(tex);
                    if (res.image.isNull()) {
                        why = QStringLiteral("decode failed: %1").arg(res.error);
                        return;
                    }
                    if (!res.image.save(outPath, "PNG")) {
                        why = QStringLiteral("PNG write failed");
                        return;
                    }
                    bytes += QFileInfo(outPath).size();
                } else {
                    // MODELS: resolve -> parse -> rigged .glb, fresh per item.
                    const di::ResolvedModel res =
                        di::resolveModelChain(*store, j.repoIdx);
                    if (res.meshBlob == (size_t)-1) {
                        why = QStringLiteral("no Mesh dependency resolved (%1)")
                                  .arg(res.note);
                        return;
                    }
                    const std::vector<uint8_t> raw =
                        store->mpk().readAsset(res.meshBlob);
                    auto mesh = std::make_shared<di::MeshData>();
                    std::string err;
                    if (!di::parseMesh(raw.data(), raw.size(), mesh.get(), &err)) {
                        why = QStringLiteral("mesh parse failed: %1")
                                  .arg(QString::fromStdString(err));
                        return;
                    }
                    std::shared_ptr<di::SkinSkeleton> skel;
                    if (res.skinBlob != (size_t)-1) {
                        const std::vector<uint8_t> sraw =
                            store->mpk().readAsset(res.skinBlob);
                        auto sk = std::make_shared<di::SkinSkeleton>();
                        std::string serr;
                        if (di::parseSkinSkeleton(sraw.data(), sraw.size(),
                                                  sk.get(), &serr))
                            skel = sk;
                    }
                    std::shared_ptr<di::BoneParents> hier;
                    std::shared_ptr<di::BoneLocals> locals;
                    const size_t skelFile =
                        di::findSkeletonByConvention(*store, j.repoIdx);
                    if (skelFile != (size_t)-1) {
                        const std::vector<uint8_t> hraw =
                            store->mpk().readAsset(skelFile);
                        auto h = std::make_shared<di::BoneParents>();
                        auto l = std::make_shared<di::BoneLocals>();
                        std::string herr;
                        if (di::parseSkeletonHierarchy(hraw.data(), hraw.size(),
                                                       h.get(), &herr, l.get())) {
                            hier = h;
                            locals = l;
                        }
                    }
                    auto decodeTex = [&](size_t blob) -> QImage {
                        if (blob == (size_t)-1) return {};
                        const std::vector<uint8_t> traw =
                            store->mpk().readAsset(blob);
                        di::Texture2D tex;
                        std::string terr;
                        if (!di::isTexture2D(traw.data(), traw.size()) ||
                            !di::parseTexture2D(traw.data(), traw.size(), &tex,
                                                &terr))
                            return {};
                        return TextureDecode::decode(tex).image;
                    };
                    MeshTextures textures;
                    textures.diffuse  = decodeTex(res.texBlob);
                    textures.normal   = decodeTex(res.nrmBlob);
                    textures.mix      = decodeTex(res.mixBlob);
                    textures.emissive = decodeTex(res.emiBlob);

                    std::vector<GlbExporter::AnimExport> anims;
                    if (embedAnims && skel) {
                        const auto clips = di::findFolderAnims(*store, j.repoIdx);
                        for (const auto& [stem, id] : clips) {
                            const std::vector<uint8_t> araw =
                                store->mpk().readAsset(id);
                            auto clip = std::make_shared<di::AnimClip>();
                            std::string aerr;
                            if (!araw.empty() &&
                                di::parseAnim(araw.data(), araw.size(),
                                              clip.get(), &aerr))
                                anims.push_back(
                                    {QString::fromStdString(stem), clip});
                        }
                    }

                    GlbExporter::Part part;
                    part.mesh     = mesh;
                    part.skel     = skel;
                    part.textures = textures;
                    part.name     = j.name;
                    QString gerr;
                    if (!GlbExporter::writeGlb(outPath, {part}, hier.get(),
                                               locals.get(), anims, j.name,
                                               &gerr)) {
                        why = QStringLiteral("glb write failed: %1").arg(gerr);
                        return;
                    }
                    bytes += QFileInfo(outPath).size();
                }
            }, &fault);
            if (!guarded)
                why = QStringLiteral("CRASHED (%1) — skipped, run continues")
                          .arg(fault.what);
            if (why.isEmpty()) {
                ++done;
                if (xreport) xreport->wrote(rel);
                QMutexLocker lock(&mtx);
                added.push_back({j.name, rel});
                statusByName.insert(j.name, QStringLiteral("ok"));
                if (verbose)
                    emit runLog(generation, QStringLiteral("  ok  %1").arg(rel));
            } else {
                ++failed;
                if (xreport) xreport->failed(rel, why);
                QMutexLocker lock(&mtx);
                failures << QStringLiteral("%1 — %2").arg(j.name, why);
                statusByName.insert(j.name, why);
                emit runLog(generation,
                            QStringLiteral("  FAIL %1 — %2").arg(j.name, why));
            }
        };

        // Worker pool over an atomic index (D4 pattern): reads + decode +
        // writes all run in parallel; output is order-independent.
        std::vector<std::thread> pool;
        pool.reserve((size_t)par);
        for (int w = 0; w < par; ++w) {
            pool.emplace_back([&] {
                seh::installSehTranslator();
                for (;;) {
                    if (canceled()) return;
                    const int i = next.fetch_add(1);
                    if (i >= (int)jobs.size()) return;
                    processOne(jobs[(int)i]);
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    qint64 last = lastEmit.load();
                    if (now - last > 150 &&
                        lastEmit.compare_exchange_strong(last, now))
                        emit runProgress(generation,
                                         done.load() + failed.load() +
                                             skipped.load(),
                                         (int)jobs.size(), failed.load());
                }
            });
        }
        for (std::thread& t : pool) t.join();
        emit runProgress(generation, done.load() + failed.load() + skipped.load(),
                         (int)jobs.size(), failed.load());

        // Incremental ledger: merge this run's additions into the existing
        // manifest (never rewrite history — cancelled runs keep their part).
        {
            const QString mpath =
                QDir(dir).filePath(QStringLiteral("_bulk_manifest.json"));
            QJsonArray arr;
            QSet<QString> have;
            {
                QFile f(mpath);
                if (f.open(QIODevice::ReadOnly)) {
                    arr = QJsonDocument::fromJson(f.readAll()).array();
                    for (const QJsonValue& v : arr)
                        have.insert(
                            v.toObject().value(QStringLiteral("file")).toString());
                }
            }
            const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
            for (const ManifestAdd& a : added) {
                if (have.contains(a.file)) continue;
                QJsonObject o;
                o.insert(QStringLiteral("name"), a.name);
                o.insert(QStringLiteral("file"), a.file);
                o.insert(QStringLiteral("date"), now);
                arr.append(o);
            }
            QSaveFile f(mpath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
                f.commit();
            }
        }
        if (!failures.isEmpty()) {
            QFile f(QDir(dir).filePath(QStringLiteral("_bulk_failed.txt")));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&f);
                for (const QString& line : failures) out << line << '\n';
            }
        }
        if (report) {
            QFile f(QDir(dir).filePath(QStringLiteral("_bulk_report.csv")));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&f);
                out << "name,entryId,status,file\n";
                for (const Job& j : jobs) {
                    const QString st =
                        statusByName.value(j.name, QStringLiteral("missing"));
                    out << csvField(j.name) << ',' << j.entryId << ','
                        << csvField(st) << ','
                        << csvField(outputRelPath(j.name, j.type, runMode,
                                                  organize))
                        << '\n';
                }
            }
            emit runLog(generation,
                        QStringLiteral("── report: _bulk_report.csv written"));
        }
        if (xreport) {
            const QString rp = xreport->write(cancel->load());
            if (!rp.isEmpty())
                emit runLog(generation, QStringLiteral("── report: %1")
                                            .arg(QFileInfo(rp).fileName()));
        }
        emit runFinished(generation, done.load(), skipped.load(), failed.load(),
                         bytes.load(), timer.elapsed(), cancel->load());
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

// ── ExportHooks: the adaptive Export menu + Ctrl+E family ──────────────────

bool BulkExtractTab::canExport() const
{
    if (m_running || !m_idx) return false;
    return m_manual->isChecked() ? !m_queued.isEmpty()
                                 : m_model->visibleCount() > 0;
}

QString BulkExtractTab::exportWhat() const
{
    static const char* nouns[3] = {"model", "texture", "asset"};
    if (m_manual->isChecked()) {
        const int n = (int)m_queued.size();
        return QStringLiteral("%L1 queued item%2")
            .arg(n)
            .arg(n == 1 ? QString() : QStringLiteral("s"));
    }
    const int n = (int)computeMatches().size();
    return QStringLiteral("%L1 %2%3")
        .arg(n)
        .arg(QLatin1String(nouns[mode()]))
        .arg(n == 1 ? QString() : QStringLiteral("s"));
}

void BulkExtractTab::exportNow(bool toLastDir)
{
    if (!canExport()) return;
    // toLastDir = run straight into the remembered output folder; otherwise
    // prompt (and remember) — the same two paths as the tab's own buttons.
    doExtract(!toLastDir);
}

QString BulkExtractTab::lastExportDir() const
{
    const QString d = m_outDir->text();
    return !d.isEmpty() && QDir(d).exists() ? d : QString();
}
