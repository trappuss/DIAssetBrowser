#pragma once
// Textures tab — every repository Texture2D (182k-class), decoded to an RGBA
// preview. Ported up to the D4 browser's Textures tab feature set (2026-08-07),
// retrofitted to DI's data model:
//   * list OR thumbnail-grid browsing over one shared selection (Ctrl+wheel
//     resizes grid tiles), async thumbnails, Ctrl+C CSV copy
//   * preview: R/G/B/A channel isolation, alpha checkerboard toggle, mip
//     picker, Fit / wheel-zoom / drag-pan / double-click-refit, live pixel
//     inspector (coords + RGBA + hex under the cursor)
//   * TEXTURE ATLASES — DI's equivalent of D4 "texframes". The descriptor is
//     the sibling Cocos plist (measured: Package/UIScript/<stem>.plist pairs
//     by stem with 13,533 ui Texture2Ds); frames list with names + rects,
//     click to preview one frame, export selected/all frames (trim option).
//     Unlike D4's 2D_table.dat the plist carries real rectangles — no
//     segmentation heuristics.
//   * context menu on list and grid (clicked-row-wins-unless-selected rule):
//     export to last folder (named), export…, copy image / save image…,
//     copy name / meaning / MPK path with their values shown
//   * exports remember their folders separately: textures (tex/lastDir),
//     atlas frames (tex/framesLastDir), single images (tex/imageDir)
// Decode runs on a worker thread (generation-counted), so a slow BC7 4k atlas
// never stalls the UI.

#include <QImage>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "app/ExportHooks.h"
#include "index/AssetIndex.h"
#include "tex/AtlasPlist.h"
#include "tex/TextureParser.h"
#include "util/HoverPreview.h"
#include "util/TexturePreview.h"   // shared CHANNELS strip (all three tabs)

class AssetListModel;
class FilterBar;
class ThumbnailProvider;
class PanelBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QListView;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QTableView;
class QThread;
class QTimer;
class QTreeWidget;
class QListWidget;
class QToolButton;
class QVBoxLayout;

class TexturesTab : public QWidget, public ExportHooks {
    Q_OBJECT
public:
    explicit TexturesTab(QWidget* parent = nullptr);
    ~TexturesTab() override;   // joins any in-flight decode worker

    void setIndex(std::shared_ptr<AssetIndex> idx);

    bool eventFilter(QObject* obj, QEvent* ev) override;

    // Texture -> model reverse index (D4's "ASSOCIATED MODELS"). DI absolutely
    // HAS these links: every Model resolves to one Material whose blob names the
    // texture GUIDs (see ModelResolve.h). We invert that over the whole
    // repository once, off-thread, keyed by mpk texture-blob id — the same id
    // space a texture row carries as its entryId.
    struct AssocIndex {
        std::unordered_map<size_t, std::vector<qint32>> models;  // blob -> models
        std::unordered_map<size_t, std::vector<qint32>> mats;    // blob -> materials
    };

    // ExportHooks (the adaptive Export menu / Ctrl+E family)
    bool    canExport() const override;
    QString exportWhat() const override;         // "N texture(s)"
    void    exportNow(bool toLastDir) override;  // selection, else current row
    QString lastExportDir() const override;
    bool    canExportFrames() const override;
    int     frameCount() const override;
    int     frameSelCount() const override;
    void    exportFramesNow(bool all, bool toLastDir) override;
    QString framesLastDir() const override;
    bool    canSaveImage() const override;       // decoded image on screen
    void    saveImageNow() override;
    bool    canExportAll() const override;        // any rows pass the filter
    int     exportAllCount() const override;
    void    exportAllNow(bool toLastDir) override;

signals:
    void statusText(const QString& text);
    // ASSOCIATED MODELS "Reveal in Models tab" -> MainWindow switches tabs and
    // calls ModelsTab::revealRepoIndex. repoIdx is a repository index.
    void revealModelRequested(int repoIdx);
    // internal: decode worker -> GUI (queued)
    void decoded(int generation, QImage image, QString info, QString warn,
                 std::shared_ptr<di::Texture2D> tex,
                 std::shared_ptr<AtlasPlist::Sheet> atlas, QString atlasNote);
    // internal: batch-export worker -> GUI (queued)
    void batchProgress(int generation, QString message);
    void batchDone(int generation, QString message);
    // internal: hover-preview decoder -> GUI (queued)
    void hoverDecoded(int seq, int entryId, QImage image);
    // internal: associated-models index builder -> GUI (queued)
    void assocProgress(int generation, int percent);
    void assocReady(int generation, std::shared_ptr<AssocIndex> index);

private:
    struct Job { int entryId; QString name; };

    void applyFilter();
    void buildAssocIndex();   // spawn the repo-wide texture->model scan
    void populateAssoc();     // fill the ASSOCIATED MODELS panel for the pick
    // D4-style right-column panel strip (toggle · reorder · hide · persist).
    PanelBox* addRightPage(const QString& title, QWidget* content);
    void      showPanel(int page, bool on);
    void      movePanel(int page, int delta);
    void      savePanelLayout();
    void      restorePanelLayout();
    void startDecode(int modelRow, int mipIndex);   // mip -1 = largest
    void refreshView();                             // channels/zoom recompose
    void setGridView(bool on);                      // list <-> thumbnail grid
    void setGridPx(int px);                         // tile size (Ctrl+wheel)
    void showListMenu(QWidget* view, const QPoint& viewportPos);
    // Rows a context action targets: the CLICKED row wins unless it is part of
    // the current selection — the file-manager rule (D4 audit fix).
    QList<int> menuTargetRows(int clickedRow) const;
    void exportRows(const QList<Job>& jobs, const QString& dir);
    void exportSelected(bool toLastDir);   // selected rows -> PNGs in a folder
    void exportShownView();                // save the R/G/B/A-composed view as shown
    // all=false: selected frames (none selected = all). toLastDir reuses
    // tex/framesLastDir without a prompt.
    void exportFrames(bool all, bool toLastDir);
    QImage composeChannels(const QImage& src, bool checkerboard) const;
    QImage composed() const;              // preview compose (checker optional)
    QImage composedForExport() const;     // no checkerboard, null when 0 chans
    QImage croppedFrame(int idx, bool trim) const;   // un-rotates, optional trim
    void updatePixelInspector(const QPoint& canvasPos);
    // Hover popup content for a list/grid row; kicks the async decode when
    // the real pixels aren't in yet.
    bool resolveHover(const QModelIndex& idx, QList<HoverPreview::Line>* lines,
                      QImage* image);

    std::shared_ptr<AssetIndex> m_idx;
    AssetListModel* m_model  = nullptr;
    QTableView*     m_table  = nullptr;
    QListView*      m_grid   = nullptr;    // thumbnail grid over the same model
    QStackedWidget* m_stack  = nullptr;    // 0 = table, 1 = grid
    QPushButton*    m_gridBtn = nullptr;   // checkable "Grid"
    int             m_gridPx  = 96;        // tile edge, Ctrl+wheel 48..192
    FilterBar*      m_filter = nullptr;
    std::unique_ptr<ThumbnailProvider> m_thumbs;

    // hover previews (one engine per view, same resolver)
    HoverPreview* m_hoverList = nullptr;
    HoverPreview* m_hoverGrid = nullptr;
    QImage m_hoverImg;                 // decoded-at-preview-size cache (1 entry)
    int    m_hoverEntry = -1;
    std::atomic<int> m_hoverSeq{0};

    QLabel*      m_info    = nullptr;
    QLabel*      m_pixel   = nullptr;      // pixel inspector line (monospace)
    QScrollArea* m_scroll  = nullptr;
    QLabel*      m_canvas  = nullptr;
    QCheckBox*   m_chR     = nullptr;
    QCheckBox*   m_chG     = nullptr;
    QCheckBox*   m_chB     = nullptr;
    QCheckBox*   m_chA     = nullptr;
    QCheckBox*   m_fit     = nullptr;
    QPushButton* m_alphaBg = nullptr;      // checkable checkerboard toggle
    QComboBox*   m_mipBox  = nullptr;
    // CHANNELS strip — 6 grayscale thumbnails (RGBA · R · G · B · A · LUMA) under
    // the preview, matching D4's per-channel preview.
    // The shared body (util/TexturePreview.h) in TEXTURE mode: RGB · R · G · B ·
    // A · LUMA, with per-tile copy/save, hover-zoom and a pixel readout.
    texprev::Panel m_texPrev;
    // (Texture / selection / shown-view export are menu-only — no toolbar buttons.)
    bool m_batchRunning = false;            // re-entry guard

    // D4-layout scaffolding: 3-column split (left list · middle preview · right
    // PanelBox stack) mirroring the D4 Textures tab.
    QSplitter*   m_mainSplit    = nullptr;   // left | middle | right
    QSplitter*   m_rstack       = nullptr;   // right column: stacked PanelBoxes
    // panel toggle strip (ASSOCIATED MODELS · MIPMAPS; TEXFRAMES stays data-driven)
    QVBoxLayout* m_rstripLay    = nullptr;
    std::vector<PanelBox*>    m_rsections;
    std::vector<QToolButton*> m_rpageBtns;
    QStringList  m_sectKeys;
    bool         m_panelRestore = false;
    PanelBox*    m_framesPanel   = nullptr;   // TEXFRAMES panel (shown when atlas)
    QListWidget* m_mipList       = nullptr;   // MIPMAPS panel (click to preview a mip)
    PanelBox*    m_assocPanel    = nullptr;   // ASSOCIATED MODELS panel
    QListWidget* m_assocList     = nullptr;   // models that reference the texture
    std::shared_ptr<AssocIndex> m_assoc;      // built once per setIndex (nullptr = building)
    // Separate generation: m_generation bumps on every decode; sharing it would
    // abort the long index build the moment the user clicks another row (the
    // same lesson as m_batchGen). Only setIndex bumps this.
    std::atomic<int> m_assocGen{0};

    // atlas frames panel (hidden unless the texture has a descriptor)
    QWidget*     m_atlasBox     = nullptr;
    QCheckBox*   m_trim         = nullptr;   // crop exports to alpha bounds
    QPushButton* m_exportFrames = nullptr;
    QTreeWidget* m_frames       = nullptr;
    std::shared_ptr<AtlasPlist::Sheet> m_atlas;
    int m_frameFocus = -1;                 // preview shows just this frame

    QImage  m_image;                       // decoded RGBA of the current pick
    QImage  m_inspectSrc;                  // what the preview shows (full/frame)
    QString m_currentName;
    std::shared_ptr<di::Texture2D> m_tex;  // parsed container (for mip picker)
    int     m_currentRowEntry = -1;        // entryId of the shown asset
    QTimer* m_selDebounce = nullptr;       // arrow-keying must not queue N reads
    int     m_pendingRow  = -1;
    QPixmap m_composedPm;                  // channel-composed cache
    int     m_composedMask = -1;           // channel+checker mask of the cache
    double  m_zoom      = 1.0;             // manual zoom (Fit unchecked)
    double  m_lastScale = 1.0;             // scale the preview was last drawn at
    bool    m_panning   = false;
    QPoint  m_panStart;                    // cursor at pan start (global)
    QPoint  m_panScroll;                   // scrollbar values at pan start
    std::atomic<int> m_generation{0};
    // Batch exports get their OWN counter: m_generation bumps on every decode
    // (selection, mip pick), and sharing it aborted a 500-file batch the
    // moment the user clicked another row (review finding). Only setIndex
    // bumps this one.
    std::atomic<int> m_batchGen{0};
    QSet<QThread*>   m_workers;
};
