#pragma once
// Wardrobe — assemble a character's cosmetic pieces onto one rig and view
// them together. Data-driven from the live index (measured 2026-08-01/02):
//   * playable classes = depth-2 Char/<x> folders with their own .skeleton
//     AND >= 25 non-lod _yifu Models (exactly the 20 class rigs)
//   * pieces are the folder's Model entries, bucketed by the pinyin slot
//     token in their name; "_lod" variants are skipped
//   * every piece of one character lives in the same mesh space — weapons
//     included (measured: wuqi meshes sit on the character's back in
//     character space, rigidly skinned to peijian_tuteng / Bip001 Spine1),
//     so the multi-part GLMeshView just draws all selected pieces and the
//     per-part PosePlayers animate them, weapon and all.
//   * base body pieces (face/neck/eyes/lashes/beard) are the folder's
//     lian/bozi/yanqiu/jiemao/huzi Models — equipped by the Body toggle.
//   * ARMOR SETS are multi-piece groups (measured on f_barbarian sz06_006 /
//     f_monk sz11_007): one MAIN piece per slot (plain "<tok>_<key>", or
//     toukui_all_/toukui_half_, or a jianjia left+right pair) PLUS same-key
//     sub-attachments (yifu_back/parts/fujian/wing, jianjia_parts, tui_parts,
//     toukui_face/maofa/piaodai/beard) and set-keyed pieces in other buckets
//     (bozi neck, wuqi back weapon, bijia_L/R bracers). 741 sub-attachment
//     models used to sit in the Chest combo and alphabetically outrank the
//     real chest in the set picker ("yifu_back_X" < "yifu_X") — they now live
//     in their own Attachment bucket, the set picker ranks mains explicitly,
//     and applying a set auto-equips every matching attachment into a pool of
//     extra parts so outfits come out COMPLETE.
//   * dye/awakened variants ship as sibling Materials named
//     "<model>_<b1|g1|dye0|...>_mat" over the same mesh; the per-slot
//     variant stepper swaps textures via resolveMaterialTextures.
//   * HAND WEAPONS (measured 2026-08-02) are a different animal from the
//     back-worn "<class>_wuqi_*" cosmetics. They live in the shared
//     "Char/item" folder (3,638 models), are modelled in WEAPON space around
//     the origin, and carry a one-bone skin named "<type>_bone". The class rig
//     supplies the placement: its .skeleton tree holds the GENERIC holders
//     "R_weapon" / "L_weapon" (children of the hand bones, animated by every
//     clip) plus a few type-specific "zhushou_<type>" / "fushou_<type>" nodes
//     at the same spot (f_barbarian: danshouchui, danshoujian). Loading picks
//     the type-specific holder when it exists and the generic one otherwise,
//     then renames the weapon's single skin bone to that node — which makes
//     every downstream stage (bind placement, PosePlayer, overlay, glb export)
//     work with no special cases, because the clips animate those very names.

#include <QHash>
#include <QImage>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <memory>
#include <vector>

#include "app/ExportCapture.h"   // shared viewport capture (image / turntable / anim GIF)
#include "app/ExportHooks.h"
#include "gl/GLMeshView.h"
#include "index/AssetIndex.h"
#include "index/ItemNames.h"
#include "gl/ModelThumbRenderer.h"
#include "index/ThumbnailProvider.h"
#include "model/AnimParser.h"
#include "model/AnimPose.h"
#include "model/GlbExporter.h"
#include "model/SkeletonTreeParser.h"
#include "model/SkinSkeletonParser.h"
#include "tabs/PanelBox.h"       // right-column stacking panels (shared with Models)
#include "util/AnimExportScope.h"
#include "util/HoverPreview.h"
#include "util/InfoNotes.h"     // shared status readout (base + bounded notes)
#include "util/TexturePreview.h"   // the shared TEXTURE PREVIEW body (both modes)

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QSplitter;
class QThread;
class QTimer;
class QToolButton;
class QTreeWidget;
class QVBoxLayout;

class WardrobeTab : public QWidget, public ExportHooks {
    Q_OBJECT
public:
    explicit WardrobeTab(QWidget* parent = nullptr);
    ~WardrobeTab() override;   // joins in-flight loaders

    void setIndex(std::shared_ptr<AssetIndex> idx);

    // ExportHooks: Ctrl+E exports the assembled outfit (same as the button).
    bool    canExport() const override;
    QString exportWhat() const override;
    void    exportNow(bool toLastDir) override;
    // ExportHooks: viewport capture. Enabled once anything is equipped; the
    // animation GIF additionally needs a clip loaded.
    bool canSaveImage() const override;
    void saveImageNow() override;
    bool canExportGif() const override;
    bool canExportAnimGif() const override;
    void exportGifTurntable() override;
    void exportGifAnim() override;

    // helmet chest shoulders legs hair back-weapon main-hand off-hand
    // attachment other
    static constexpr int kSlots = 10;
    static constexpr int kMainHand   = 6;
    static constexpr int kOffHand    = 7;
    static constexpr int kAttachSlot = 8;   // manual single-attachment combo
    static constexpr int kBodySlots = 5;    // lian bozi yanqiu jiemao huzi
    static constexpr int kSetAttach = 10;   // pool auto-filled by applySet
                                            // (measured post-dedup max: 8)
    static constexpr int kParts = kSlots + kBodySlots + kSetAttach;

signals:
    void statusText(const QString& text);
    // internal: class-scan worker -> GUI (queued)
    void classScanned(int generation,
                      std::shared_ptr<std::vector<QStringList>> slotNames,
                      std::shared_ptr<std::vector<QList<int>>> slotRepoIdx,
                      std::shared_ptr<QStringList> animNames,
                      std::shared_ptr<QList<quint32>> animIds,
                      std::shared_ptr<QList<int>> bodyRepoIdx,
                      std::shared_ptr<di::BoneParents> hier,
                      std::shared_ptr<di::BoneLocals> locals);
    // internal: piece loader -> GUI (queued). part.mesh null = load failed.
    // seq guards against out-of-order arrivals on rapid slot changes.
    // bindPose is non-empty only for hand slots (hardpoint placement).
    void pieceLoaded(int generation, int slot, int seq, ViewPart part,
                     std::shared_ptr<di::SkinSkeleton> skel, QString note,
                     std::shared_ptr<QStringList> varNames,
                     std::shared_ptr<QList<int>> varRepoIdx,
                     std::shared_ptr<std::vector<float>> bindPose);
    void bulkProgress(int generation, QString message);
    // internal: variant texture loader -> GUI (queued)
    void variantLoaded(int generation, int slot, int seq, MeshTextures tex,
                       QString label);
    void exportDone(QString message);

private:
    void scanClass(const QString& folder);
    void loadPiece(int slot, int repoIdx);
    void refreshView();
    void selectAnim(int comboIdx);
    void animPose(float tMs);   // pose/overlay only (capture path — no transport UI)
    void animTick(float tMs);   // animPose + transport slider/spin/time label
    // Viewport capture, shared with the Models tab through ExportCapture so the
    // same settings produce the same image or GIF from either tab.
    ExportCapture::AnimSource captureAnim();
    QString captureBaseName() const;
    void stopAnim();
    // Which part of a set to equip — the D4 tool's theme scopes. "Everything"
    // is what the set combo has always done; the narrower ones let you keep the
    // armour you are wearing and swap only the weapons, or vice versa.
    enum ThemeScope { ThemeAll, ThemeArmor, ThemeWeapons, ThemeAttach };
    void applySet(int setIdx, ThemeScope scope = ThemeAll);
    // Every piece of the class carrying this set key, across all buckets — a
    // set often ships more matching weapons than the single-choice slot can
    // show. Selecting a row equips it into its own slot.
    void buildMatchList(const QString& key);
    void clearSetAttachments();     // empty the auto-equipped pool
    // combo row (>=1) of the set's MAIN piece for one slot, -1 = none.
    // Ranks candidates so a sub-attachment can never shadow the real piece.
    int  pickMainInSlot(int slot, const QString& key) const;
    void stepSet(int delta);        // prev/next with wrap-around
    void stepVariant(int slot, int delta);
    // The slot row's right-click menu: equip this piece's set at a scope,
    // step variants, export, lock, clear. Replaces the per-row < > Export
    // buttons and the old global "Equip ▾" dropdown.
    void showSlotMenu(int slot, const QPoint& globalPos);
    void exportSlotPiece(int slot);            // one equipped slot -> .glb
    int  setKeyIndexOf(const QString& pieceName) const;   // -1 = no set
    void equipSetAt(int setKeyIdx, ThemeScope scope);
    void applyBodyToggle(bool on);
    bool slotLocked(int slot) const;   // the per-slot pin (set/theme skip it)
    // Hide the character rows that do not match; combo indices stay put, so the
    // index -> m_classFolders mapping every other path relies on is unaffected.
    void applyClassFilter(const QString& text);
    void refreshMatchIcons();   // fill in match-list icons as decodes land
    void initSlotPlayer(int slot);   // bake cloth (if on) + init that slot's player
public:
    // Re-read the export settings this tab caches in a widget, after the
    // Settings dialog has had a chance to change them. See the .cpp.
    void syncExportSettings();

private:
    void onClothToggled();           // re-bake every slot's clip, re-tick
    bool clothOn() const;            // the Cloth-physics checkbox state
    QString currentClipName() const; // the loaded clip's name (not combo text)
    // partRepoIdx: the repo entry behind each part, in order. Drives raw deps,
    // loose textures and the opposite-gender twin. Empty = the equipped outfit.
    void exportGlb(const QString& suggestedBase,
                   std::vector<GlbExporter::Part> parts,
                   const QList<int>& partRepoIdx = {});
    // What a bulk run writes.
    //   BulkAllSets        every armour set of the class, shaped and filtered by
    //                      Settings ▸ Export ▸ Wardrobe (which can also ADD the
    //                      non-armour match pass across every set).
    //   BulkListedMatches  exactly the rows in the Set matches list — this set's
    //                      alternatives, one .glb each. Reads the widget itself,
    //                      so the button's count and the run's size are the same
    //                      number by construction.
    enum BulkMode { BulkAllSets, BulkListedMatches };

    // One unit of bulk work. An OUTFIT unit holds a whole set's pieces; a
    // PER-PIECE unit holds exactly one. Everything downstream — cloth bake,
    // folder layout, raw deps, loose textures, the opposite-gender twin, cancel,
    // progress — treats both identically, which is why the export MODES are
    // expressed as a different unit list rather than a second exporter.
    struct SetJob {
        QString key;                 // the label used in the log and progress
        QString stem;                // the .glb file name (and folder, by-model)
        QList<int> repoIdx;
        QStringList names;
        QList<int> slotOf;
        // The opposite-gender counterpart, resolved and named up front rather
        // than inside a lane: naming goes through the File-names templates and
        // the item-name table, neither of which a raw worker thread should be
        // touching, and doing it once also keeps the lanes doing nothing but
        // geometry. Empty = this unit has no counterpart in the data.
        QList<int>  twinRepoIdx;
        QStringList twinNames;
        QList<int>  twinSlotOf;
        QString     twinStem;
    };
    // What a bulk run would produce. `files` is the number the buttons show:
    // units PLUS the ones that actually resolved an opposite-gender twin, which
    // is not simply "double when both genders is on" — genderless item-folder
    // weapons have no counterpart, and a class the game ships one gender of has
    // none at all.
    struct BulkCounts {
        size_t units = 0, files = 0, twins = 0;
        size_t sets = 0, matches = 0, listed = 0;
        size_t renamed = 0;   // units whose name collided and was suffixed
    };
    // The exact units a run would write, named and de-collided, writing nothing.
    // Shared by exportAllSets() and the buttons' counters so the two cannot
    // disagree about the size of the job.
    std::vector<SetJob> buildBulkUnits(BulkMode mode,
                                       BulkCounts* counts = nullptr) const;
    void exportAllSets(BulkMode mode = BulkAllSets);
    void syncMatchButton();      // label + enabled state from the list's count
    void syncExportButtons();    // every export button's label, from BulkCounts
    void syncOutfitButton();     // just the outfit button (cheap; per equip)
    // The all-sets count depends on the class and the export settings only, so
    // it is computed once and reused until one of those changes.
    void invalidateSetsCount() { m_setsCountsValid = false; }
    BulkCounts m_setsCounts;
    bool       m_setsCountsValid = false;
    // The Set list options as of the last scan, so a Settings close can tell
    // whether a rescan is actually needed. Empty until the first sync.
    QString    m_setListSig;
    // The parts an outfit export writes, honouring export/wardrobeScope (all vs
    // equipped items only) and the viewport's hidden-part state.
    std::vector<GlbExporter::Part> outfitParts(QList<int>* outRepoIdx = nullptr) const;
    // The clips an export embeds, per AnimExportScope. In the Wardrobe the
    // useful source is `base`: armour owns no animation, so the class rig's
    // clips are what drives it.
    QList<QPair<QString, quint32>> outfitAnimRefs(const AnimExportScope& sc) const;
    // Repository index of the same piece on the opposite-gender rig, or -1.
    // DI names cosmetics "<g>_<class>_<slot>_<set>", so the twin is a rename.
    int oppositeGenderPiece(int repoIdx) const;
    // The outfit filename from export/wardrobeNameTemplate.
    QString outfitFileName() const;
    // "Char/f_barbarian" -> ("barbarian", "F"). One spelling for every filename.
    static void splitClassFolder(const QString& folder, QString* cls,
                                 QString* gender);
    // export/wardrobeNameTemplate applied to one outfit. Static and
    // argument-driven so the BULK run can name each set with the same template
    // the single-outfit export uses — it used to hardcode "<class>_<key>".
    static QString outfitStem(const QString& classFolder, const QString& setKey,
                              const QString& setName,
                              const QHash<int, QString>& slotName);
    void applyBindPoses();          // hand slots keep their hardpoint placement
    void requestThumbs(int slot);   // lazy combo icons for one slot
    bool eventFilter(QObject* obj, QEvent* ev) override;

    // ── Right-hand panel column (ported 1:1 from the Models tab) ───────────
    // A 26px strip of checkable glyph toggles beside a vertical QSplitter of
    // PanelBoxes: several panels stack at once, drag to size, ▲▼ reorder, ✕
    // hides, and the open set persists under wardrobe/panels/shown.
    QLabel* addRightPage(const QString& title, QWidget* content);
    void showPanel(int page, bool on);
    void movePanel(int page, int delta);
    void savePanelLayout();
    void restorePanelLayout();
    void ensurePanelVisible(const QString& title);

    // Panel content builders. Each reads only already-loaded state, so they are
    // cheap enough to call on every equip/unequip.
    void buildPanelPages(QWidget* host);   // creates the six panel bodies
    void fillInfoPanel();
    void fillMaterialsPanel();
    void fillTexturePreview(const MeshTextures& tex);
    // Show one texture split into RGB/R/G/B/A instead of the material's PBR
    // channels — driven by picking a texture row rather than a piece row.
    void showTextureChannels(const QImage& img, const QString& name);
    void refreshTexPanelTitle();
    void buildPartsList();       // per-piece submesh tree
    void buildEquipList();       // every live part incl. body + set-attach pool
    void applyPartsMask();       // m_partHidden + m_subHidden -> the viewport
    // Push the hide sets back onto the existing tree rows WITHOUT rebuilding
    // them — safe to call from inside an itemChanged handler, which a clear()
    // would not be.
    void syncVisibilityChecks();
    void setAllParts(int mode);  // 0 none · 1 all · 2 invert
    // Forget a part's visibility overrides — called when a slot is about to hold
    // a different piece, so a hidden slot never hides its replacement.
    void clearPartVisibility(int part);
    void setFocusPart(int part); // drives MATERIALS detail + TEXTURE PREVIEW
    // Human label for a part index: slot name, "Body — face", or "Set piece N".
    QString partLabel(int part) const;

    // Animation transport (Models-tab parity).
    void seekFrames(int delta);
    void updateFrameUi();
    void applySpeed(int comboIdx);
    void syncAnimList();         // mirror the combo selection into the panel list

    QWidget*     m_farW      = nullptr;   // whole right column (» collapses it)
    QSplitter*   m_rstack    = nullptr;   // vertical stack of PanelBoxes
    QVBoxLayout* m_rstripLay = nullptr;   // the toggle strip's layout
    std::vector<PanelBox*>    m_rsections;   // registration order == page id
    QStringList               m_sectKeys;    // per-panel settings key
    std::vector<QToolButton*> m_rpageBtns;   // strip toggles, parallel
    QHash<QString, int>       m_panelIndex;  // title -> page
    bool m_panelRestore = false;             // replaying a saved layout

    // INFO panel
    QWidget* m_infoBox = nullptr;
    QHash<QString, QLabel*> m_infoVals;

    // MATERIALS panel — one row per equipped piece, channels as children
    QWidget*     m_matBox  = nullptr;
    QTreeWidget* m_matList = nullptr;

    // TEXTURE PREVIEW panel — the shared body (material PBR / texture RGBA)
    texprev::Panel m_texPrev;
    QWidget*       m_texPrevBox = nullptr;

    // PARTS panel — piece -> submesh visibility
    QWidget*     m_partsBox  = nullptr;
    QTreeWidget* m_partsList = nullptr;
    bool         m_partsSyncing = false;

    // EQUIPPED panel — every live part, including the body pieces and the
    // auto-filled set-attachment pool that previously had no UI at all.
    QWidget*     m_equipBox  = nullptr;
    QTreeWidget* m_equipList = nullptr;
    bool         m_equipSyncing = false;

    // ANIMATIONS panel — the class's clips as a list (arrow keys autoplay)
    QListWidget* m_animList = nullptr;
    bool         m_animListSyncing = false;

    // Visibility state, merged by applyPartsMask()
    QSet<int>                 m_partHidden;   // whole parts switched off
    QHash<int, QSet<int>>     m_subHidden;    // part -> hidden submesh indices
    int                       m_focusPart = -1;

    // Transport extras
    QPushButton* m_stepBack  = nullptr;
    QPushButton* m_stepFwd   = nullptr;
    QSpinBox*    m_frameSpin = nullptr;
    QLabel*      m_frameMax  = nullptr;
    QLabel*      m_timeLabel = nullptr;
    QComboBox*   m_speedBox  = nullptr;
    QCheckBox*   m_loop      = nullptr;
    float        m_speed     = 1.0f;

    // Viewport N-strip + overlay toggles
    QWidget*     m_vpStrip = nullptr;
    QToolButton* m_camBtn   = nullptr;
    QToolButton* m_lightBtn = nullptr;
    QToolButton* m_clothBtn = nullptr;
    QToolButton* m_shadeMoreBtn = nullptr;
    QComboBox*   m_channelCombo = nullptr;
    QCheckBox*   m_grid       = nullptr;
    QCheckBox*   m_cull       = nullptr;
    QCheckBox*   m_turntable  = nullptr;
    QPushButton* m_frameBtn   = nullptr;

    std::shared_ptr<AssetIndex> m_idx;
    QComboBox* m_classBox = nullptr;
    QComboBox* m_slotBox[kSlots] = {};
    // Per-slot "keep this while I browse sets" pins. See slotLocked().
    QPushButton* m_slotLock[kSlots] = {};
    QLineEdit*   m_classFilter = nullptr;   // search over the character list
    int          m_classFilterFirst = -1;   // first visible row (Enter target)
    QCheckBox* m_bones = nullptr;
    QCheckBox* m_body  = nullptr;      // base body under armor (toggleable)
    QCheckBox* m_clothBox = nullptr;   // soft-body (cloth/tail/hair) physics
    QCheckBox* m_alphaBox = nullptr;   // alpha-tested transparency
    QCheckBox* m_fxBox    = nullptr;   // material emissive glow (arcane/star/fresnel)
    QPushButton* m_exportOutfit = nullptr;
    QPushButton* m_exportSets   = nullptr;   // every armor set -> a folder
    QPushButton* m_exportMatches = nullptr;  // the non-armour match pass alone
    bool m_bulkRunning = false;              // guards bulk-export re-entry
    // Cancel flag for the bulk set export. Written by the progress dialog on
    // the GUI thread, polled by the worker between sets — the same shape the
    // Bulk Extract tab's BatchSink uses.
    std::atomic<bool> m_bulkCancel{false};
    QLabel*    m_info  = nullptr;
    // Base line + bounded, collapsing notes (util/InfoNotes.h). Before this,
    // every message here overwrote the last, so a run that hit twelve problems
    // showed only the twelfth — and a routine status line could erase it.
    infonotes::Panel m_infoText;
    GLMeshView* m_view = nullptr;

    QStringList m_classFolders;                       // parallel to class combo
    std::shared_ptr<std::vector<QList<int>>> m_slotRepoIdx;   // per slot, per combo row-1
    std::shared_ptr<QList<int>> m_bodyRepoIdx;        // kBodySlots entries, -1 absent

    // armor-set cycling: keys like "sz08_001" / "t07_004" appearing in >= 2
    // of the four armor slots (measured: f_barbarian has 148 keys, 133 of
    // them complete 4-slot sets)
    QComboBox*   m_setBox  = nullptr;
    QPushButton* m_setPrev = nullptr;
    QPushButton* m_setNext = nullptr;
    QListWidget* m_matchList = nullptr;
    QLabel*       m_matchLabel = nullptr;   // hidden with the list
    QVBoxLayout*  m_leftCol      = nullptr; // the equipment column's layout
    int           m_matchListRow = -1;      // its index of the match list
    int           m_matchFillRow = -1;      // and of the spacer that replaces it
    HoverPreview* m_matchHover = nullptr;   // same dwell preview as the dropdowns  // every piece carrying the set key
    // Piece names equipped before a class switch, keyed by slot — used to
    // re-equip the same look on the opposite-gender rig.
    QHash<int, QString> m_carryOver;
    QString m_carryOverClass;            // the class those names came from
    QString m_curClassFolder;            // the class currently scanned/loaded
    // Re-equip the remembered look on the newly scanned rig (gender switch).
    void applyCarryOver();
    QStringList  m_setKeys;                           // parallel to combo rows-1

    struct SlotState {
        ViewPart part;                                // mesh null = empty
        std::shared_ptr<di::SkinSkeleton> skel;
        di::PosePlayer player;                        // valid while a clip plays
        std::shared_ptr<di::AnimClip> playClip;       // cloth-baked (or == m_clip)
        QString name;                                 // piece display name
        QStringList varNames;                         // variant labels ([0]=base)
        QList<int>  varRepoIdx;                       // Material repo indices
        int varIdx = 0;
        std::vector<float> bindPose;                  // hand slots: hardpoint
                                                      // placement skin matrices
        int repoIdx = -1;                             // the piece's repository
                                                      // entry (raw-deps export,
                                                      // opposite-gender twin)
    };
    SlotState m_slots[kParts];
    // True between loadPiece() and its pieceLoaded arrival. stepVariant bumps
    // the same sequence counter a load uses, so it has to stand aside — see
    // loadPiece for what happens when it does not.
    bool m_slotLoading[kParts] = {};
    std::shared_ptr<di::BoneParents> m_hier;          // class .skeleton tree
    std::shared_ptr<di::BoneLocals>  m_locals;        // its local rest transforms
    std::unique_ptr<ThumbnailProvider> m_thumbs;      // flat texture combo icons
    // Opt-in 3D-rendered combo icons (settings: wardrobe/view/thumb3d). Mirrors
    // ThumbnailProvider's get/peek/commit/ready surface, so requestThumbs()
    // drives either one through the same code path.
    std::unique_ptr<ModelThumbRenderer> m_thumbs3d;
    bool m_thumb3dActive = false;                     // mode the icons were built in
    int m_thumbSlot = -1;                             // slot whose popup is open
    bool thumbs3dOn() const;                          // the live setting
    void clearSlotIcons();                            // drop icons after a mode flip
    // Dwell popup over a slot dropdown: the piece's name and facts, plus a big
    // preview — a 3D render when 3D icons are on, the flat texture otherwise.
    // The shared core: a (slot, combo row) pair is what BOTH the dropdown rows
    // and the Set matches rows resolve to, so both previews are the same code.
    bool resolvePieceHover(int slot, int comboRow,
                           QList<HoverPreview::Line>* lines, QImage* image);
    bool resolveMatchHover(const QModelIndex& idx,
                           QList<HoverPreview::Line>* lines, QImage* image);
    void showMatchMenu(int slot, int comboRow, const QPoint& globalPos);
    // Toggles the list AND hands the column's vertical stretch to whichever of
    // the list / trailing spacer is doing the filling. See the .cpp.
    void setMatchListVisible(bool on);
    void exportRepoPiece(int repoIdx, const QString& name);   // no slot needed
    bool resolveSlotHover(int slot, const QModelIndex& idx,
                          QList<HoverPreview::Line>* lines, QImage* image);
    HoverPreview* m_slotHover[kSlots] = {};
    // One-entry memo for the hover render. renderPreview() is deliberately
    // uncached (it is a one-off, full-size render), so without this every mouse
    // twitch on the same row re-parses and re-renders the mesh.
    QImage m_hoverImg;
    int    m_hoverRepo = -1;
    ItemNames::Table m_names;                         // real-name overrides
    QString currentClassName() const;                 // "barbarian" from folder
    QString setLabel(const QString& setKey) const;    // "<real> (key)" or key

    // shared animation clip driving every slot's own SkinSkeleton subset
    QComboBox*   m_animBox    = nullptr;
    QPushButton* m_playBtn    = nullptr;
    QSlider*     m_timeSlider = nullptr;
    QTimer*      m_animTimer  = nullptr;
    std::shared_ptr<QList<quint32>> m_animIds;        // parallel to combo rows-1
    std::shared_ptr<di::AnimClip>   m_clip;
    float m_animT = 0.0f;

    std::atomic<int> m_generation{0};
    std::atomic<int> m_slotSeq[kParts] = {};
    QSet<QThread*>   m_workers;
};
