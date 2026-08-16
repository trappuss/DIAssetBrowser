#pragma once
// Blender-style outliner model for the Models tab — ported from the D4 tool's
// ModelOutlinerModel and retrofitted to DI's AssetListModel.
//
// Wraps the flat AssetListModel (the 677k-class browse rows, already narrowed
// to the Models tab's animated/static filter) as a TREE without touching it —
// AssetListModel is shared by four tabs, so all tree behaviour lives here. The
// top level mirrors the source 1:1: same row numbers, same columns, data /
// flags / headers forwarded verbatim, sort() delegated. That 1:1 mapping is
// what lets you scroll the ordinary browse list in the Outliner view and have
// exactly ONE row — the currently loaded model — sprout an inline subtree
// (Skeleton -> bones, Parts, Attachments, Animations) beneath it.
//
// The host row is keyed by AssetRow::entryId, so when the source resets
// (filter / sort) the subtree relocates to wherever that model's row moved and
// survives. Part nodes are checkable and carry a right-aligned visibility eye
// plus an export-include arrow, exactly like the D4 outliner.

#include <QAbstractItemModel>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QStyledItemDelegate>
#include <QVector>

class AssetListModel;
struct AssetRow;

class AssetOutlinerModel : public QAbstractItemModel {
    Q_OBJECT
public:
    // Group    = a header row (Skeleton / Parts / Material / Attachments / …)
    // Bone     = a rig joint (tan bone glyph)
    // Part     = a submesh leaf (checkable eye + export arrow; ref = submesh index)
    // Material = the resolved material node (shaded-sphere glyph)
    // Texture  = a texture-channel leaf (checkerboard glyph; aux = full name)
    // Attach   = a loaded sibling model (expands into its own Parts / Material)
    // Anim     = a clip leaf (double-click to play; aux = clip display name)
    enum Kind { Group, Bone, Part, Material, Texture, Attach, Anim };

    // DI's name column (0) is the wide, stretched column that already carries
    // thumbnails + labels — the natural home for the indented tree. That is
    // also QTreeView's default tree position, so no setTreePosition() needed.
    static constexpr int kTreeCol     = 0;
    static constexpr int ExportRole   = Qt::UserRole + 41;   // Part rows: bool include-in-export
    static constexpr int AnimNameRole = Qt::UserRole + 42;   // Anim leaf: clip name to play

    struct Node {
        Kind           kind;
        QString        text;
        int            ref = -1;             // Part: submesh index · Attach: repo index
        int            sub = -1;             // Attachment Part only: owning attachment's repo
                                             // index (>=0). Primary parts keep -1.
        QString        aux;                  // Part: tri/slot tooltip · Anim: clip display name ·
                                             // Texture: full asset name · Attach: display name
        QPixmap        icon;                 // per-node thumbnail (Texture channels); falls back
                                             // to the drawn kind glyph when null
        bool           checkable = false;    // Part + Attach (the eye)
        Qt::CheckState check = Qt::Checked;
        bool           exportOn = true;      // Part only (the arrow)
        Node*          parent = nullptr;
        QVector<Node*> kids;
        Node(Kind k, const QString& t, int r = -1) : kind(k), text(t), ref(r) {}
        ~Node() { qDeleteAll(kids); }
        Node* add(Node* n) { n->parent = this; kids.append(n); return n; }
    };

    explicit AssetOutlinerModel(AssetListModel* src, QObject* parent = nullptr);
    ~AssetOutlinerModel() override;

    // ── Subtree management (ModelsTab drives these) ──
    void  setSubtree(quint32 entryId, Node* root);   // takes ownership; root's kids show under the host row
    void  clearSubtree();
    Node* node(const QModelIndex& ix) const;         // nullptr for a top-level browse row
    QModelIndex hostIndex() const;                   // the loaded model's row (invalid if filtered out)

    void  setPartCheck(int prim, bool on);           // reflect the parts panel; silent
    void  partChecks(QHash<int, bool>& out) const;   // submesh -> eye state
    void  togglePartExport(const QModelIndex& ix);   // arrow click
    void  partExportFlags(QHash<int, bool>& out) const;
    // Fixed top-level row height (tracks the thumbnail/icon size). Without this
    // a QTreeView with uniformRowHeights caches the FIRST row's height while its
    // async thumbnail is still missing, so every row stays too short and clips
    // the image until a relayout — reported as "cropped until you switch views".
    void  setRowHeight(int h);
    static QPixmap kindIcon(Kind kind);              // drawn Blender-style glyph (no assets)

    // ── QAbstractItemModel ──
    QModelIndex   index(int row, int col, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex   parent(const QModelIndex& child) const override;
    int           rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int           columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant      data(const QModelIndex& ix, int role = Qt::DisplayRole) const override;
    bool          setData(const QModelIndex& ix, const QVariant& v, int role) override;
    Qt::ItemFlags flags(const QModelIndex& ix) const override;
    QVariant      headerData(int s, Qt::Orientation o, int role) const override;
    void          sort(int col, Qt::SortOrder order) override;

signals:
    void partCheckChanged();   // a part eye toggled by the user -> recompute GL visibility
    void attachCheckChanged(int repoIdx, bool on);   // an attachment eye toggled -> load/unload
    void attachPartToggled(int repoIdx, int submesh, bool on);   // one attachment submesh eye

private:
    void relocateHost();       // source rows changed -> find the host row again by entryId

    AssetListModel* m_src;
    Node*   m_root    = nullptr;   // owned container; its kids are the visible child rows
    qint64  m_hostId  = -1;        // AssetRow::entryId of the loaded model (-1 = none)
    int     m_hostRow = -1;        // -1 = host filtered out / no subtree
    int     m_rowH    = 0;         // fixed top-level row height (0 = view default)
};

// Delegate that paints Part visibility as a right-aligned Blender EYE (open =
// visible, closed lid = hidden) plus an export-include arrow, instead of the
// stock left-edge checkbox. Applies only to rows carrying a CheckStateRole
// (the outliner's Part nodes); every other row paints normally.
class AssetOutlinerDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    bool editorEvent(QEvent* ev, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;
    bool helpEvent(QHelpEvent* ev, QAbstractItemView* view, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;
private:
    static QRect eyeRect(const QRect& rowRect);
};
