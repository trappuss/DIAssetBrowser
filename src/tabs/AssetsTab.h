#pragma once
// Assets tab — the whole-index browser: search box + type filter + 677k-row
// table + a details pane (physical location, repository identity/dependencies,
// raw header hexdump). The Textures/Models/Bulk tabs come later; this tab is
// the proof that the index and read path work end to end in the UI.

#include <QWidget>

#include <memory>

#include "app/ExportHooks.h"
#include "index/AssetIndex.h"

class AssetListModel;
class FilterBar;
class QLabel;
class QPlainTextEdit;
class QTableView;

class AssetsTab : public QWidget, public ExportHooks {
    Q_OBJECT
public:
    explicit AssetsTab(QWidget* parent = nullptr);

    void setIndex(std::shared_ptr<AssetIndex> idx);

    // ExportHooks: Ctrl+E saves the current row's exact stored bytes.
    bool    canExport() const override;
    QString exportWhat() const override;         // "raw asset"
    void    exportNow(bool toLastDir) override;
    QString lastExportDir() const override;

signals:
    void statusText(const QString& text);   // filtered/total cue for the status bar

private:
    void applyFilter();
    void showDetails(int modelRow);
    void updateStatus();
    void showRowMenu(const QPoint& viewportPos);

    std::shared_ptr<AssetIndex> m_idx;
    AssetListModel* m_model   = nullptr;
    QTableView*     m_table   = nullptr;
    FilterBar*      m_filter  = nullptr;
    QLabel*         m_header  = nullptr;
    QPlainTextEdit* m_details = nullptr;
    int             m_curRow  = -1;   // model row the details pane shows
};
