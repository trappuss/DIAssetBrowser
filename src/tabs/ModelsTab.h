#pragma once
// Models tab — repository Model/Mesh/LodModel entries with a live 3D preview.
// Resolution chain (measured on the live repository, 2026-08-01):
//   Model    -> deps { Mesh, Material, [SkinSkeleton] }
//   Material -> deps { Texture2D _d/_e/_m/_n }
// so a Model preview parses its Mesh dep and textures with the Material's
// "_d" Texture2D. A bare Mesh entry previews untextured. Parse + decode run
// on a generation-counted worker; upload happens on the GUI thread.
//
// Ported up to the D4 browser's Models tab feature set (2026-08-07),
// retrofitted to DI:
//   * Blender-style transport bar: step back / play-pause / step forward,
//     frame spinner + "/ N", time readout, speed combo, Loop toggle; wheel on
//     the timeline scrubs exactly ONE frame, Shift+wheel changes speed
//   * async clip loading (the old selectAnim read+parsed on the GUI thread)
//   * Parts panel: one checkbox per submesh with live tri counts, All / None /
//     Invert — visibility drives per-range draws in the viewport
//   * viewport: ground grid with tinted world axes, backface culling toggle,
//     turntable, Frame (re-fit) button, Save image… (framebuffer grab)
//   * multi-select list + batch .glb export with per-item crash guard and
//     verbatim failure reasons; context menu via the shared MenuText
//     vocabulary incl. "Export … to last folder (…/dir)"

#include <QImage>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <memory>

#include "app/ExportCapture.h"   // shared viewport capture (image / turntable / anim GIF)
#include "app/ExportHooks.h"
#include "gl/GLMeshView.h"
#include "index/AssetIndex.h"
#include "model/AnimParser.h"
#include "model/AnimPose.h"
#include "model/MeshParser.h"
#include "model/SkeletonTreeParser.h"
#include "model/SkinSkeletonParser.h"
#include "util/FilterSpec.h"
#include "util/TexturePreview.h"   // shared TEXTURE PREVIEW body (PBR / RGBA modes)
#include "util/HoverPreview.h"
#include "util/InfoNotes.h"   // shared status readout (base + bounded notes)

// One playable .anim found near the model (same folder's /ani/, walking up).
struct AnimRef {
    QString  display;    // file stem, e.g. "a_idle"
    quint32  entryId = 0;
};
using AnimListPtr = std::shared_ptr<std::vector<AnimRef>>;

class AssetListModel;
class FilterBar;
class GLMeshView;
class ThumbnailProvider;
class ModelThumbRenderer;
class QComboBox;
class QListWidget;
class QPushButton;
class QCheckBox;
class QToolButton;
class QLabel;
class QSlider;
class QSpinBox;
class QTableView;
class QThread;
class QTimer;
class QTreeView;
class QTreeWidget;
class QListView;
class QStackedWidget;
class QAbstractItemView;
class QSplitter;
class QVBoxLayout;
class PanelBox;
class AssetOutlinerModel;

class ModelsTab : public QWidget, public ExportHooks {
    Q_OBJECT
public:
    explicit ModelsTab(QWidget* parent = nullptr);
    ~ModelsTab() override;

    void setIndex(std::shared_ptr<AssetIndex> idx);

    // Select + load the model at this repository index, dropping any active
    // filter that hides it. Used by the Textures tab's "Reveal in Models tab".
    void revealRepoIndex(int repoIdx);

    // ExportHooks (the adaptive Export menu / Ctrl+E family)
    bool    canExport() const override;
    QString exportWhat() const override;         // "N model(s)"
    void    exportNow(bool toLastDir) override;
    QString lastExportDir() const override;
    bool    canSaveImage() const override;       // viewport has a mesh
    void    saveImageNow() override;
    bool    canExportGif() const override;        // mesh loaded → turntable GIF
    bool    canExportAnimGif() const override;    // a clip is loaded
    void    exportGifTurntable() override;
    void    exportGifAnim() override;
    bool    canExportAll() const override;        // any rows pass the filter
    int     exportAllCount() const override;
    void    exportAllNow(bool toLastDir) override;
    bool    canExportAnims() const override;      // loaded model has clips
    void    exportAnimsNow() override;
    bool    canExportWithAttachments() const override;   // attachments loaded
    void    exportWithAttachmentsNow(bool toLastDir) override;

signals:
    void statusText(const QString& text);
    // internal: worker -> GUI (queued)
    void meshReady(int generation, std::shared_ptr<di::MeshData> mesh,
                   MeshTextures textures, QString info,
                   std::shared_ptr<di::SkinSkeleton> skel,
                   std::shared_ptr<di::BoneParents> hierarchy,
                   std::shared_ptr<di::BoneLocals> locals, QString displayName,
                   AnimListPtr anims);
    // internal: clip loader -> GUI (queued). seq guards rapid re-picks.
    void animReady(int generation, int seq, std::shared_ptr<di::AnimClip> clip,
                   QString name, QString err);
    // internal: batch exporter -> GUI (queued; guarded by m_batchGen)
    void batchProgress(int generation, QString message);
    void batchDone(int generation, QString message);
    void exportDone(int generation, QString message);
    // internal: hover-preview decoder -> GUI (queued)
    void hoverDecoded(int seq, int entryId, QImage image);

private:
    struct Job { int repoIdx; QString name; };

    bool eventFilter(QObject* obj, QEvent* ev) override;
    void applyFilter();
    void setInfoBase(const QString& html);   // replaces the panel, clears notes
    void addInfoNote(const QString& html);   // bounded + de-duplicated
    void renderInfo();
    void markClipUnsupported(const QString& name);
    // FilterBar's spec plus this tab's animated/static constraint — one place,
    // so the initial setIndex and every later refilter can't disagree.
    FilterSpec currentSpec() const;
    void startLoad(int modelRow);
    void selectAnim(int comboIdx);   // spawns the async clip loader
    void animPose(float tMs);        // evaluate + push pose/overlay ONLY (no UI)
    void animTick(float tMs);        // animPose + transport slider/spin/time label
    void stopAnim(bool restoreBind); // timer off, pose cleared
    void seekFrames(int delta);      // transport step (pauses playback)
    void updateFrameUi(float tMs);   // frame spin + time label, signals blocked
    void applySpeed(int comboIdx);
    void buildPartsList();           // one row per submesh of the loaded mesh
    // D4-style right-column panels: a strip of checkable toggles beside a
    // vertical splitter. Register a panel (starts hidden), toggle it from the
    // strip, ▲▼ reorder among the visible ones, ✕ hides it, layout persists.
    QLabel* addRightPage(const QString& title, QWidget* content);
    void    showPanel(int page, bool on);
    void    movePanel(int page, int delta);
    void    savePanelLayout();
    void    restorePanelLayout();
    // Inject the loaded model's structure subtree. reveal=true recenters on the
    // host (initial load / view switch); reveal=false keeps the current scroll
    // position + expansion (in-place toggles like attachment on/off).
    void buildOutliner(bool reveal = false);
    void syncOutlinerParts();        // push parts-panel eye states into the outliner
    void repaintBrowseViews();       // refresh list/grid/outliner (thumbnail arrivals)
    void applyThumbSize();           // push m_thumbPx into every browse view
    void bumpThumbSize(int notches); // Ctrl+wheel zoom step (persisted)
    void applyPartsMask();           // checkboxes -> GLMeshView submesh mask
    // Selected rows that works for the grid as well as the list (see the .cpp).
    QModelIndexList selectedRowIndexes() const;
    void buildAttachList();          // sibling models in the same folder
    void onAttachToggled();          // check/uncheck -> add/remove a group
    void rebuildParts();             // push primary + attachments to the view
    void syncAttachAnims();          // attachments follow the primary clip name
    void showAttachMenu(const QPoint& pos);   // per-attachment clip override
    // Return `clip` unchanged, or a copy with soft-body (cloth) physics baked
    // in when the toggle is on and the skeleton has cloth bones. Used for both
    // the viewport player and (independently, in the export worker) the GLB.
    std::shared_ptr<di::AnimClip> maybeCloth(
        const std::shared_ptr<di::AnimClip>& clip, const di::SkinSkeleton* skel,
        const di::BoneParents* hier, const di::BoneLocals* locals) const;
public:
    // Re-read the export settings this tab caches in a widget, after the
    // Settings dialog has had a chance to change them. See the .cpp.
    void syncExportSettings();

private:
    void onClothToggled();           // re-bake primary + attachments, re-tick
    void showClothDialog();          // modal soft-body solver tuning
    struct Attachment;               // defined below (needs MeshTextures etc.)
    static bool loadAttachmentModel(di::DiAssetStore* store, int32_t repoIdx,
                                    Attachment& a);   // synchronous sibling load
    // Viewport capture. All three go through ExportCapture so the Models tab and
    // the Wardrobe tab produce byte-identical output from identical settings.
    void saveViewImage();            // still image (png/jpg/webp per settings)
    void exportTurntableGif();       // 360 degree spin -> animated GIF
    void exportAnimGif();            // current clip's frames -> looping GIF
    // Hand the capture a way to scrub this tab's clip. frameCount==0 when
    // nothing is playing, which the turntable treats as "just orbit".
    ExportCapture::AnimSource captureAnim();
    QString captureBaseName() const;   // loaded model's stem, or "viewport"
    void showListMenu(const QPoint& viewportPos, QAbstractItemView* view = nullptr);
    QList<int> menuTargetRows(int clickedRow) const;   // clicked-wins rule
    void exportCurrentGlb(bool toLastDir);
    void exportPartGlb(int submesh);        // one submesh as its own .glb (PARTS menu)
    void exportAttachmentGlb(int repoIdx);  // one loaded attachment as its own .glb
    // Outliner node context menus (parts + attachments — toggle / frame / export).
    void showOutlinerPartMenu(int submesh, const QPoint& gp);
    void showOutlinerAttachMenu(int repoIdx, bool loaded, const QPoint& gp);
    void setAttachmentLoaded(int repoIdx, bool on);   // toggle one sibling by repo idx
    void setAllAttachments(bool on);                  // enable / disable every sibling
    void applyAttachMask(int repoIdx);                // push one attachment's submesh mask to GL
    void exportAttachPartGlb(int repoIdx, int submesh);   // one attachment submesh as .glb
    void showOutlinerAttachPartMenu(int repoIdx, int submesh, const QPoint& gp);
    // Per-attachment hidden-submesh sets (keyed by repo index, survives rebuilds).
    QHash<int, QSet<int>> m_attachPartHidden;
    void exportBatch(const QList<Job>& jobs, const QString& dir);
    bool resolveHover(const QModelIndex& idx, QList<HoverPreview::Line>* lines,
                      QImage* image);

    std::shared_ptr<AssetIndex> m_idx;
    AssetListModel* m_model  = nullptr;
    QTableView*     m_table  = nullptr;
    FilterBar*      m_filter = nullptr;
    QComboBox*      m_animFilter = nullptr;   // All / Animated only / Static only
    QComboBox*      m_whoFilter  = nullptr;   // Everything / Player / Non-player
    infonotes::Panel m_infoText;             // base line + bounded, collapsing notes
    QTimer*         m_selDebounce = nullptr;
    int             m_pendingRow = -1;
    // Anim picker debounce: scrolling the combo fires currentIndexChanged per
    // step; without this each step spawned a clip-decode worker (a thread storm
    // that could exhaust memory). Collapse rapid changes to the final pick.
    QTimer*         m_animDebounce = nullptr;
    int             m_pendingAnimIdx = -1;

    GLMeshView*  m_view = nullptr;
    // Viewport N-strip (D4): Camera / Lighting popover icons on the viewport's
    // right edge, plus a side-panel collapse arrow.
    QWidget*     m_vpStrip = nullptr;
    QToolButton* m_camBtn  = nullptr;
    QToolButton* m_lightBtn = nullptr;
    QToolButton* m_clothBtn = nullptr;   // cloth-physics popover on the N-strip
    QWidget*     m_farW    = nullptr;   // the right panel column (» collapses it)
    QLabel*      m_info = nullptr;
    // right-column panel management (D4 strip + splitter)
    QSplitter*   m_rstack     = nullptr;   // vertical splitter of PanelBoxes
    QVBoxLayout* m_rstripLay  = nullptr;   // the toggle-button strip's layout
    std::vector<PanelBox*>    m_rsections; // panels, registration order
    std::vector<QToolButton*> m_rpageBtns; // strip toggles, parallel to sections
    QStringList  m_sectKeys;               // persistence keys, parallel
    bool         m_panelRestore = false;   // suppress saves while replaying
    QLabel*      m_loading = nullptr;   // viewport corner badge during loads
    // Overlays-dropdown checkboxes (D4 groups these instead of toolbar buttons)
    QCheckBox*   m_bones = nullptr;     // skeleton overlay toggle
    QCheckBox*   m_grid  = nullptr;     // ground grid
    QCheckBox*   m_cull  = nullptr;     // backface culling
    QCheckBox*   m_alphaBox = nullptr;  // alpha-tested transparency
    QCheckBox*   m_turntable = nullptr; // auto-rotate (Camera dropdown)
    QCheckBox*   m_clothBox   = nullptr;   // soft-body (cloth/tail/hair) physics
    QCheckBox*   m_fxBox      = nullptr;   // material emissive glow (arcane/star/fresnel)
    QPushButton* m_frameBtn  = nullptr; // re-fit camera (Camera dropdown)
    // (Save image / GIF / Export .glb are menu-only — Export menu + right-click.)
    QCheckBox*   m_thumb3dBox = nullptr;   // 3D-rendered browse thumbnails
    int          m_thumbPx = 40;           // base browse icon edge (Ctrl+wheel zoom)
    std::unique_ptr<ThumbnailProvider>  m_thumbs;
    std::unique_ptr<ModelThumbRenderer> m_thumbs3d;

    // hover preview over the list
    HoverPreview* m_hover = nullptr;
    QImage m_hoverImg;
    int    m_hoverEntry = -1;
    std::atomic<int> m_hoverSeq{0};

    // TEXTURE PREVIEW panel — the shared body. Selecting a MATERIAL shows the
    // PBR channels it feeds the shader; selecting a TEXTURE shows that one image
    // split into RGB/R/G/B/A.
    texprev::Panel m_texPrev;
    QWidget* m_texPrevBox   = nullptr;
    QLabel*  m_texTilePopup = nullptr;   // hover-zoom
    void fillTexturePreview(const MeshTextures& tex);
    // Show one texture's raw channels (the outliner's texture leaves and the
    // MATERIALS channel rows route here).
    void showTextureChannels(const QImage& img, const QString& name);
    void refreshTexPanelTitle();
    // Route an outliner texture leaf (`tile` = PBR tile index) to the panel by
    // switching it into RGBA mode over that channel's SOURCE texture.
    void showTextureInPreview(int tile);
    // The source image behind a PBR tile index (0/4 base colour, 1/2 mix, ...).
    const QImage* sourceForTile(int tile) const;
    // Context menu for an outliner texture leaf: export / copy the channel image.
    void showTextureNodeMenu(int tile, const QString& name, const QPoint& gp);
    // Bring a right-column panel up by its registration title (checks its strip
    // toggle, which shows + persists it).
    void ensurePanelVisible(const QString& title);
    QHash<QString, int> m_panelIndex;    // panel title -> page index (for the above)

    // INFO panel (D4 DATA/INFO page port): label/value rows of file + model
    // facts, selectable for copy. Filled by fillInfoPanel() on each load.
    QWidget*              m_infoBox = nullptr;
    QHash<QString, QLabel*> m_infoVals;
    void fillInfoPanel();

    // MATERIALS panel (D4 MATERIALS/SHADING port): the resolved material name and
    // a row per bound channel with its source-texture name and dimensions.
    QWidget*     m_matBox  = nullptr;
    QTreeWidget* m_matList = nullptr;
    void fillMaterialsPanel();
    QString currentMaterialName() const;   // shared dep-walk for INFO + MATERIALS

    // Parts panel (submesh visibility) — a D4-style table: PART · TRIS · MATERIAL
    QWidget*     m_partsBox  = nullptr;
    QTreeWidget* m_partsList = nullptr;
    bool m_partsSyncing = false;        // guard: programmatic check changes

    // Browse-area view modes: the asset list (List), a thumbnail Grid over the
    // same model + selection, and the Outliner (the loaded model's structure —
    // skeleton bones, parts, attachments, animations; double-click an anim to
    // play it). One QStackedWidget switched by a List/Grid/Outliner button row.
    QListView*      m_gridView     = nullptr;   // thumbnail grid browse view
    QStackedWidget* m_browseStack  = nullptr;
    // Outliner view: a QTreeView over a wrapping model that presents the flat
    // browse list at top level and sprouts the loaded model's structure subtree
    // (skeleton / parts / attachments / animations) inline beneath its row.
    QTreeView*         m_outlineView  = nullptr;
    AssetOutlinerModel* m_outlineModel = nullptr;

    // Attachments panel: sibling models in the same folder that belong together
    // (body + tail + parts). Each checked one renders as an extra GL part with
    // its own skeleton + player, auto-playing the clip whose name matches the
    // primary model's selected animation.
    struct Attachment {
        int32_t name_repoIdx = -1;
        QString name;
        std::shared_ptr<di::MeshData>     mesh;
        std::shared_ptr<di::SkinSkeleton> skel;
        std::shared_ptr<di::BoneLocals>   locals;
        std::shared_ptr<di::BoneParents>  hier;      // for export node tree
        MeshTextures                      textures;
        di::PosePlayer                    player;
        std::shared_ptr<di::AnimClip>     clip;       // raw decoded clip
        std::shared_ptr<di::AnimClip>     playClip;   // clip actually played/exported (cloth-baked or == clip)
        AnimListPtr                       anims;
        QString                           clipName;   // loaded clip name, if any
        QString                           overrideClip;  // "" = match primary
    };
    QWidget*      m_attachBox  = nullptr;
    QListWidget*  m_attachList = nullptr;
    bool          m_attachSyncing = false;

    int32_t       m_curRepoIdx = -1;    // repo index of the primary model
    AssetRow      m_curRow;             // the loaded row (for the INFO panel)
    QString       m_curAnimName;        // primary's playing clip name ("" = bind)
    std::vector<Attachment> m_attach;   // GL parts 1..N (primary is part 0)
    // Per-model memory of which siblings were checked + any clip overrides, so
    // switching away and back restores the scene. Keyed by primary repoIdx.
    QHash<qint32, QHash<qint32, QString>> m_attachMemory;  // model -> {sibling -> override}

    // last successfully loaded model (what Export .glb writes)
    std::shared_ptr<di::MeshData>     m_lastMesh;
    std::shared_ptr<di::SkinSkeleton> m_lastSkel;
    std::shared_ptr<di::BoneParents>  m_lastHier;
    std::shared_ptr<di::BoneLocals>   m_lastLocals;
    MeshTextures m_lastTextures;
    QString m_lastName;

    // animation playback (clip fps is 30 — measured frame-block format)
    QListWidget* m_animList  = nullptr;   // ANIMATIONS panel (D4): clip list, select to play
    bool m_animListSyncing = false;       // guard: programmatic current-item changes
    QComboBox*   m_animBox   = nullptr;
    QComboBox*   m_channelCombo = nullptr;   // render-channel picker (in the shadeMore menu)
    QToolButton* m_shadeMoreBtn = nullptr;   // ⌄/◆ channel dropdown button (D4 shadeMore)
    QPushButton* m_stepBack  = nullptr;
    QPushButton* m_playBtn   = nullptr;
    QPushButton* m_stepFwd   = nullptr;
    QSlider*     m_timeSlider = nullptr;
    QSpinBox*    m_frameSpin  = nullptr;
    QLabel*      m_frameMax   = nullptr;   // "/ N"
    QLabel*      m_timeLabel  = nullptr;   // "0.00 / 0.00 s"
    QComboBox*   m_speedBox   = nullptr;
    QCheckBox*   m_loop       = nullptr;
    float        m_speed      = 1.0f;
    AnimListPtr  m_anims;
    std::shared_ptr<di::AnimClip> m_clip;       // raw decoded primary clip
    std::shared_ptr<di::AnimClip> m_playClip;   // cloth-baked (or == m_clip) played clip
    di::PosePlayer m_player;
    QTimer* m_animTimer = nullptr;
    float   m_animT   = 0.0f;
    // bind-pose overlay copies, restored when playback stops
    std::vector<float> m_bindSegs, m_bindJoints;

    bool m_batchRunning = false;
    std::atomic<int> m_generation{0};
    // Batch exports get their OWN counter: m_generation bumps on every
    // preview load, and sharing it aborted a 300-model batch the moment the
    // user clicked another row (review finding). Only setIndex bumps this.
    std::atomic<int> m_batchGen{0};
    std::atomic<int> m_animSeq{0};
    QSet<QThread*>   m_workers;
};
