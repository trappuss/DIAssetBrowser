#pragma once
// Bulk Extract — D4-browser-class batch extractor, retrofitted to DI (2026-08-07).
//
//   * three output modes: Models (.glb) — full resolve->parse->export per item;
//     Textures (PNG) — decoded largest mip; Raw — exact stored bytes untouched
//   * the shared FilterBar is the query (search grammar + type/category
//     facets); the mode intersects it with the types it can produce
//   * live Matches list (capped at 5,000 rows) with already-present markers,
//     and a PERSISTENT per-mode manual-pick Queue: it survives filter tweaks,
//     mode switches and app restarts (stored by NAME — DI entry ids are index
//     positions and do not survive a patch; names do)
//   * incremental ledger _bulk_manifest.json + Only new / Overwrite radios —
//     re-running after a patch extracts just what is missing
//   * organize combo: Flat (path-mangled unique names) / Subfolders by folder
//     (repository tree mirror) / Subfolders by type
//   * parallel workers (Auto/1..16; MpkIndex reads are per-pak serialized, so
//     N workers stream N paks), Pause / Resume (workers hold between items —
//     instant and free), Cancel + Esc, rate + ETA that EXCLUDES paused time
//   * timestamped run console (ring-buffered), _bulk_failed.txt with verbatim
//     reasons, optional _bulk_report.csv, query presets, Open folder / Copy
//     list, a >500-item size guard that quotes real source bytes
//
// One SEH guard per item: a malformed blob after a patch costs one file, not
// the run.

#include <QSet>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <memory>

#include "app/ExportHooks.h"
#include "index/AssetIndex.h"

class AssetListModel;
class FilterBar;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QThread;

class BulkExtractTab : public QWidget, public ExportHooks {
    Q_OBJECT
public:
    explicit BulkExtractTab(QWidget* parent = nullptr);
    ~BulkExtractTab() override;

    void setIndex(std::shared_ptr<AssetIndex> idx);

    // ExportHooks: Ctrl+E prompts a folder then runs the extract;
    // Ctrl+Shift+E runs straight into the remembered output folder.
    bool    canExport() const override;
    QString exportWhat() const override;
    void    exportNow(bool toLastDir) override;
    QString lastExportDir() const override;

signals:
    void statusText(const QString& text);
    // internal: worker -> GUI (queued)
    void runProgress(int generation, int done, int total, int failed);
    void runLog(int generation, QString line);
    void runFinished(int generation, int written, int skipped, int failed,
                     qint64 bytes, qint64 elapsedMs, bool canceled);

private:
    struct Job {
        int     entryId = -1;
        int     repoIdx = -1;
        QString name;    // display path (the stable identity)
        QString type;
    };

    bool eventFilter(QObject* obj, QEvent* ev) override;
    void applyFilter();
    int  mode() const;                       // 0 glb · 1 png · 2 raw
    void setMode(int m);
    QSet<QString> modeTypes() const;         // empty = all types allowed
    QList<Job> computeMatches() const;       // filtered set ∩ mode types
    void updateMatches();                    // rebuild list + count label
    void syncQueueFromSelection();           // manual mode: visible rows only
    void rebuildQueueWidget();
    void reflectQueueInList();
    void saveQueue();
    void loadQueueForMode();
    QList<Job> workSet() const;              // queue (manual) or all matches
    void syncExtractButtons(int knownMatchCount = -1);
    void doExtract(bool promptDir, QList<Job> explicitJobs = {});
    void loadFolderManifest();
    // The ONE place an output path is computed — the writer and the "only new"
    // skip check both call it, so they can never drift (D4 ledger lesson).
    static QString outputRelPath(const QString& name, const QString& type,
                                 int mode, int organize);
    void refreshPresets();
    void savePreset();
    void deletePreset();
    void loadPreset(const QString& name);
    void logLine(const QString& s);          // timestamped, scroll-pinned

    std::shared_ptr<AssetIndex> m_idx;
    AssetListModel* m_model  = nullptr;      // filtered set (headless: no view)
    FilterBar*      m_filter = nullptr;

    QComboBox*    m_modeBox  = nullptr;
    QComboBox*    m_preset   = nullptr;
    QPushButton*  m_presetSave = nullptr;
    QPushButton*  m_presetDel  = nullptr;
    QCheckBox*    m_manual   = nullptr;
    QPushButton*  m_selAll   = nullptr;
    QPushButton*  m_selNone  = nullptr;
    QComboBox*    m_parallel = nullptr;
    QCheckBox*    m_report   = nullptr;
    QLabel*       m_count    = nullptr;
    QLineEdit*    m_outDir   = nullptr;      // read-only; set via "Extract to…"
    QComboBox*    m_organize = nullptr;
    QRadioButton* m_onlyNew  = nullptr;
    QRadioButton* m_overwrite = nullptr;
    QPushButton*  m_extract   = nullptr;
    QPushButton*  m_extractTo = nullptr;
    QPushButton*  m_openBtn   = nullptr;
    QPushButton*  m_copyBtn   = nullptr;
    QLabel*       m_lastRun   = nullptr;
    QListWidget*  m_list      = nullptr;     // Matches
    QLabel*       m_listLbl   = nullptr;
    QListWidget*  m_queue     = nullptr;     // Queue
    QLabel*       m_queueLbl  = nullptr;
    QProgressBar* m_progress  = nullptr;
    QPushButton*  m_pauseBtn  = nullptr;
    QPushButton*  m_cancelBtn = nullptr;
    QPlainTextEdit* m_console = nullptr;

    // queue state (per mode, persisted by NAME)
    QSet<QString> m_queued;
    int  m_prevMode = 0;
    bool m_syncingQueue = false;
    QList<int> m_preClickSel;   // double-click additive toggle snapshot

    // destination-folder knowledge (from _bulk_manifest.json), keyed by the
    // OUTPUT rel path — presence follows the current mode + layout
    QSet<QString> m_folderDone;

    // run state
    bool m_running = false;
    std::shared_ptr<std::atomic<bool>> m_runCancel;   // per-run token (audit)
    std::shared_ptr<std::atomic<bool>> m_runPause;
    qint64 m_runStartMs = 0;   // for ETA; paused spans subtracted
    qint64 m_pausedMs   = 0;
    qint64 m_pauseT0    = 0;

    std::atomic<int> m_generation{0};
    QSet<QThread*>   m_workers;
};
