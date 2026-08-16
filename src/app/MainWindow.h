#pragma once
// MainWindow — tab host, menus, status bar, and the background index load.
//
// Load shape (the D4 background-index pattern, simplified to one index):
// a detached QThread builds MpkIndex + Repository + AssetIndex off the GUI
// thread, posts the result back via a queued signal, and carries a generation
// counter so a stale load (game folder changed mid-build) discards itself.

#include <QHash>
#include <QMainWindow>
#include <QSet>

#include <atomic>
#include <memory>

#include "index/AssetIndex.h"

class AssetsTab;
class BulkExtractTab;
class ConsoleWindow;
class ModelsTab;
class QAction;
class QLabel;
class QToolButton;
class QTabWidget;
class TexturesTab;
class WardrobeTab;
struct ExportHooks;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;      // joins any in-flight loader thread (UAF guard)

    QString mpkDir() const;      // current setting (default: the G: install)

protected:
    void closeEvent(QCloseEvent* ev) override;   // saves window geometry

signals:
    // internal: loader thread -> GUI thread (queued)
    void indexLoaded(std::shared_ptr<AssetIndex> idx, int generation, QString error);

private:
    void buildMenus();
    void openSettings();   // the single Settings entry point (see the .cpp)
    void chooseGameFolder();
    void reload();               // kick a background load of mpkDir()
    void exportLog();
    // Adaptive Export menu (D4 export-hook pattern): the CURRENT tab is asked
    // what it can export, and the persistent actions relabel/enable to match.
    ExportHooks* currentHooks() const;
    void refreshExportMenu();
    void showShortcutsHelp();

    QTabWidget*  m_tabs        = nullptr;
    AssetsTab*   m_assetsTab   = nullptr;
    ModelsTab*      m_modelsTab   = nullptr;
    TexturesTab*    m_texturesTab = nullptr;
    WardrobeTab*    m_wardrobeTab = nullptr;
    BulkExtractTab* m_bulkTab     = nullptr;
    QHash<QWidget*, QString> m_tabCues;   // per-tab status cue; current tab shown
    QLabel*     m_status    = nullptr;
    // Export toast: a status-bar "Show in folder" action, created on the first
    // export that reports a folder.
    QToolButton* m_showFolderBtn = nullptr;
    QString      m_lastExportFolder;
    ConsoleWindow* m_console = nullptr;

    // Export menu actions — PERSISTENT (created once) so their shortcuts stay
    // registered app-wide; aboutToShow only relabels/enables them, and their
    // handlers resolve the current tab at trigger time.
    QAction* m_actExport        = nullptr;   // Ctrl+E
    QAction* m_actExportLast    = nullptr;   // Ctrl+Shift+E
    QAction* m_actFramesSel     = nullptr;
    QAction* m_actFramesAll     = nullptr;
    QAction* m_actFramesAllLast = nullptr;
    QAction* m_actSaveImage     = nullptr;   // Ctrl+Shift+I
    QAction* m_actGifTurntable  = nullptr;   // viewport tabs only
    QAction* m_actGifAnim       = nullptr;   // when a clip is loaded
    QAction* m_actExportAnims   = nullptr;   // export loaded model + all its clips
    QAction* m_actExportAttach  = nullptr;   // export loaded model + shown attachments
    QAction* m_actExportAll     = nullptr;   // export every filtered row
    QAction* m_actExportAllLast = nullptr;

    std::shared_ptr<AssetIndex> m_index;
    std::atomic<int> m_generation{0};
    QSet<QThread*>   m_workers;   // live loader threads; joined in the destructor
};
