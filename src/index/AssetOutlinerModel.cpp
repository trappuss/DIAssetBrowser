#include "index/AssetOutlinerModel.h"
#include "index/AssetListModel.h"
#include "index/AssetIndex.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSize>
#include <QStyle>
#include <QToolTip>

AssetOutlinerModel::AssetOutlinerModel(AssetListModel* src, QObject* parent)
    : QAbstractItemModel(parent), m_src(src)
{
    // Forward the source's change signals with 1:1 row mapping. AssetListModel
    // mutates via begin/endResetModel (rebuild/sort/refilter) and dataChanged
    // (async thumbnails); layout* are forwarded defensively.
    connect(m_src, &QAbstractItemModel::modelAboutToBeReset, this, [this] { beginResetModel(); });
    connect(m_src, &QAbstractItemModel::modelReset, this, [this] {
        relocateHost();   // filter/sort moved (or hid) the loaded model's row — the subtree follows
        endResetModel();
    });
    connect(m_src, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& tl, const QModelIndex& br, const QList<int>& roles) {
                if (!tl.isValid() || !br.isValid()) return;
                emit dataChanged(createIndex(tl.row(), tl.column()),
                                 createIndex(br.row(), br.column()), roles);
            });
    connect(m_src, &QAbstractItemModel::layoutAboutToBeChanged, this,
            [this] { emit layoutAboutToBeChanged(); });
    connect(m_src, &QAbstractItemModel::layoutChanged, this, [this] {
        relocateHost();
        emit layoutChanged();
    });
    connect(m_src, &QAbstractItemModel::headerDataChanged, this,
            &QAbstractItemModel::headerDataChanged);
}

AssetOutlinerModel::~AssetOutlinerModel() { delete m_root; }

AssetOutlinerModel::Node* AssetOutlinerModel::node(const QModelIndex& ix) const
{
    return static_cast<Node*>(ix.internalPointer());   // nullptr = top-level browse row
}

void AssetOutlinerModel::relocateHost()
{
    m_hostRow = -1;
    if (m_hostId < 0 || !m_src) return;
    const int n = m_src->rowCount();
    for (int r = 0; r < n; ++r)
        if (const AssetRow* e = m_src->rowAt(r))
            if ((qint64)e->entryId == m_hostId) { m_hostRow = r; return; }
}

void AssetOutlinerModel::setSubtree(quint32 entryId, Node* root)
{
    clearSubtree();
    m_hostId = (qint64)entryId;
    relocateHost();
    const int n = root ? root->kids.size() : 0;
    if (m_hostRow >= 0 && n > 0) {
        // Proper row insertion (not a reset) so the view keeps its selection —
        // the row just clicked to load this model must stay selected.
        beginInsertRows(createIndex(m_hostRow, 0, nullptr), 0, n - 1);
        m_root = root;
        endInsertRows();
    } else {
        m_root = root;   // host filtered out right now; appears when relocateHost finds it again
    }
}

void AssetOutlinerModel::clearSubtree()
{
    if (m_root) {
        const int n = m_root->kids.size();
        if (m_hostRow >= 0 && n > 0) {
            beginRemoveRows(createIndex(m_hostRow, 0, nullptr), 0, n - 1);
            Node* old = m_root;
            m_root = nullptr;
            delete old;
            endRemoveRows();
        } else {
            delete m_root;
            m_root = nullptr;
        }
    }
    m_hostId  = -1;
    m_hostRow = -1;
}

QModelIndex AssetOutlinerModel::hostIndex() const
{
    return m_hostRow >= 0 ? createIndex(m_hostRow, 0, nullptr) : QModelIndex();
}

void AssetOutlinerModel::setRowHeight(int h)
{
    if (h == m_rowH) return;
    m_rowH = h;
    // uniformRowHeights caches the first row's hint — force a re-measure so the
    // new height takes effect immediately (also on a live zoom).
    emit layoutChanged();
}

// ── Part helpers (the tree mirrors the parts panel's visibility) ────────────────────────────

void AssetOutlinerModel::partChecks(QHash<int, bool>& out) const
{
    if (!m_root) return;
    // Primary parts only (sub < 0); attachment parts (sub >= 0) drive their own
    // ViewPart mask, not the primary parts panel.
    for (const Node* g : m_root->kids)
        for (const Node* k : g->kids)
            if (k->kind == Part && k->ref >= 0 && k->sub < 0)
                out[k->ref] = (k->check == Qt::Checked);
}

void AssetOutlinerModel::partExportFlags(QHash<int, bool>& out) const
{
    if (!m_root) return;
    for (const Node* g : m_root->kids)
        for (const Node* k : g->kids)
            if (k->kind == Part && k->ref >= 0 && k->sub < 0) out[k->ref] = k->exportOn;
}

void AssetOutlinerModel::setPartCheck(int prim, bool on)
{
    if (!m_root || m_hostRow < 0) return;
    const QModelIndex host = createIndex(m_hostRow, 0, nullptr);
    for (int gi = 0; gi < m_root->kids.size(); ++gi) {
        Node* g = m_root->kids[gi];
        const QModelIndex gIx = index(gi, 0, host);
        for (int i = 0; i < g->kids.size(); ++i) {
            Node* k = g->kids[i];
            if (k->kind != Part || k->ref != prim) continue;
            const Qt::CheckState want = on ? Qt::Checked : Qt::Unchecked;
            if (k->check != want) {
                k->check = want;
                const QModelIndex ix = index(i, kTreeCol, gIx);
                emit dataChanged(ix, ix, {Qt::CheckStateRole});   // silent — caller recomputes
            }
            return;
        }
    }
}

void AssetOutlinerModel::togglePartExport(const QModelIndex& ix)
{
    Node* n = node(ix);
    if (!n || n->kind != Part) return;
    n->exportOn = !n->exportOn;
    const QModelIndex cell = ix.siblingAtColumn(kTreeCol);
    emit dataChanged(cell, cell, {ExportRole});   // repaint the arrow
}

// ── Blender-style type glyphs, drawn once per kind — no assets, theme-proof ─────────────────

QPixmap AssetOutlinerModel::kindIcon(Kind kind)
{
    static QHash<int, QPixmap> cache;
    auto it = cache.find(int(kind));
    if (it != cache.end()) return it.value();

    constexpr int S = 14;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    switch (kind) {
    case Group: {      // small open folder
        const QColor c(190, 185, 150);
        p.setPen(QPen(c, 1.3));
        p.setBrush(c.darker(220));
        p.drawRoundedRect(QRectF(1.5, 4.0, 11.0, 8.0), 1.5, 1.5);
        p.setPen(Qt::NoPen);
        p.setBrush(c.darker(180));
        p.drawPolygon(QPolygonF({{1.5, 4.0}, {5.5, 4.0}, {7.0, 6.0}, {1.5, 6.0}}));
        break;
    }
    case Bone: {       // tan bone: two knobs + shaft
        const QColor c(200, 190, 150);
        p.setPen(QPen(c, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4.5, 9.5), QPointF(9.5, 4.5));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(3.5, 10.5), 2.1, 2.1);
        p.drawEllipse(QPointF(10.5, 3.5), 2.1, 2.1);
        break;
    }
    case Part: {       // mesh orange, triangle outline with vertex dots
        const QColor c(255, 160, 70);
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF({{7.0, 2.5}, {12.0, 11.5}, {2.0, 11.5}}));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (const QPointF& v : {QPointF(7.0, 2.5), QPointF(12.0, 11.5), QPointF(2.0, 11.5)})
            p.drawEllipse(v, 1.5, 1.5);
        break;
    }
    case Material: {   // shaded sphere
        QRadialGradient g(QPointF(5.5, 5.0), 8.0);
        g.setColorAt(0.0, QColor(250, 170, 150));
        g.setColorAt(1.0, QColor(160, 70, 60));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(QPointF(7.0, 7.0), 5.2, 5.2);
        break;
    }
    case Texture: {    // 2x2 checkerboard
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(210, 210, 210));
        p.drawRect(2, 2, 5, 5);   p.drawRect(7, 7, 5, 5);
        p.setBrush(QColor(110, 110, 110));
        p.drawRect(7, 2, 5, 5);   p.drawRect(2, 7, 5, 5);
        break;
    }
    case Attach: {     // linked squares (a joined sibling)
        const QColor c(150, 200, 210);
        p.setPen(QPen(c, 1.3));
        p.setBrush(c.darker(240));
        p.drawRect(QRectF(2.0, 5.5, 5.0, 5.0));
        p.drawRect(QRectF(7.0, 3.0, 5.0, 5.0));
        break;
    }
    case Anim: {       // green play triangle
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(120, 220, 140));
        p.drawPolygon(QPolygonF({{5.0, 4.0}, {5.0, 10.0}, {10.5, 7.0}}));
        break;
    }
    }
    p.end();
    cache.insert(int(kind), pm);
    return pm;
}

// ── QAbstractItemModel ─────────────────────────────────────────────────────────────────────

QModelIndex AssetOutlinerModel::index(int row, int col, const QModelIndex& parent) const
{
    if (row < 0 || col < 0 || col >= columnCount()) return {};
    if (!parent.isValid()) {
        if (!m_src || row >= m_src->rowCount()) return {};
        return createIndex(row, col, nullptr);
    }
    const Node* pn = node(parent);
    if (!pn) {   // parent is a top-level row — only the host row has children
        if (parent.row() != m_hostRow || !m_root) return {};
        pn = m_root;
    }
    if (row >= pn->kids.size()) return {};
    return createIndex(row, col, pn->kids[row]);
}

QModelIndex AssetOutlinerModel::parent(const QModelIndex& child) const
{
    Node* n = node(child);
    if (!n) return {};
    Node* p = n->parent;
    if (!p || p == m_root)
        return m_hostRow >= 0 ? createIndex(m_hostRow, 0, nullptr) : QModelIndex();
    Node* gp = p->parent ? p->parent : m_root;
    return createIndex(gp->kids.indexOf(p), 0, p);
}

int AssetOutlinerModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid()) return m_src ? m_src->rowCount() : 0;
    if (parent.column() != 0) return 0;             // Qt convention: children hang off column 0
    const Node* n = node(parent);
    if (!n) return (parent.row() == m_hostRow && m_root) ? m_root->kids.size() : 0;
    return n->kids.size();
}

int AssetOutlinerModel::columnCount(const QModelIndex&) const
{
    return m_src ? m_src->columnCount() : 1;
}

QVariant AssetOutlinerModel::data(const QModelIndex& ix, int role) const
{
    if (!ix.isValid() || !m_src) return {};
    const Node* n = node(ix);
    if (!n) {   // browse row → forward verbatim (but pin a stable row height)
        if (role == Qt::SizeHintRole && m_rowH > 0) return QSize(0, m_rowH);
        return m_src->data(m_src->index(ix.row(), ix.column()), role);
    }
    if (ix.column() != kTreeCol) return {};   // node rows populate only the tree column
    switch (role) {
    case Qt::DisplayRole:
        return n->text;
    case Qt::SizeHintRole:
        // Compact tree: subtree rows are short (the flat browse rows keep m_rowH,
        // handled above). Texture leaves are a touch taller to seat the thumbnail.
        return QSize(0, (n->kind == Texture && !n->icon.isNull()) ? 30 : 18);
    case Qt::DecorationRole:
        return n->icon.isNull() ? kindIcon(n->kind) : n->icon;
    case Qt::CheckStateRole:
        if (n->checkable) return n->check;
        break;
    case ExportRole:
        if (n->kind == Part) return n->exportOn;
        break;
    case AnimNameRole:
        if (n->kind == Anim) return n->aux;
        break;
    case Qt::ForegroundRole:
        if (n->kind == Group)    return QColor(200, 190, 150);
        if (n->kind == Bone)     return QColor(190, 180, 150);
        if (n->kind == Material) return QColor(215, 170, 120);
        if (n->kind == Anim)     return QColor(150, 210, 165);
        break;
    case Qt::ToolTipRole:
        if (n->kind == Part)
            return n->text
                   + (n->aux.isEmpty() ? QString() : QStringLiteral("\n") + n->aux)
                   + QStringLiteral("\nEye = show/hide \u00B7 arrow = include in export");
        if (n->kind == Attach)
            return n->text
                   + QStringLiteral("\nEye = load/unload \u00B7 right-click to export");
        if (n->kind == Texture && !n->aux.isEmpty())
            return n->aux + QStringLiteral("\nClick to preview \u00B7 right-click to export");
        if (n->kind == Anim)
            return n->text + QStringLiteral("\nSelect to play");
        return n->text;
    default:
        break;
    }
    return {};
}

bool AssetOutlinerModel::setData(const QModelIndex& ix, const QVariant& v, int role)
{
    Node* n = node(ix);
    if (!n || role != Qt::CheckStateRole || !n->checkable) return false;
    n->check = static_cast<Qt::CheckState>(v.toInt());
    emit dataChanged(ix, ix, {Qt::CheckStateRole});
    // Attachment eye → load/unload the sibling (ref = repo index); attachment
    // part eye → toggle that submesh's visibility (sub = repo index, ref =
    // submesh); primary part eye → recompute GL visibility. Distinct signals.
    const bool on = n->check == Qt::Checked;
    if (n->kind == Attach)
        emit attachCheckChanged(n->ref, on);
    else if (n->kind == Part && n->sub >= 0)
        emit attachPartToggled(n->sub, n->ref, on);
    else
        emit partCheckChanged();
    return true;
}

Qt::ItemFlags AssetOutlinerModel::flags(const QModelIndex& ix) const
{
    if (!ix.isValid() || !m_src) return Qt::NoItemFlags;
    const Node* n = node(ix);
    if (!n) {
        Qt::ItemFlags f = m_src->flags(m_src->index(ix.row(), ix.column()));
        // CRITICAL: QAbstractTableModel::flags() bakes ItemNeverHasChildren into every valid
        // index (tables can't nest). Forwarded as-is it makes QTreeView skip hasChildren()
        // entirely — no expander, no subtree, ever. Strip it; rowCount() says who has children.
        f &= ~Qt::ItemNeverHasChildren;
        return f;
    }
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (n->checkable && ix.column() == kTreeCol) f |= Qt::ItemIsUserCheckable;
    return f;
}

QVariant AssetOutlinerModel::headerData(int s, Qt::Orientation o, int role) const
{
    return m_src ? m_src->headerData(s, o, role) : QVariant{};
}

void AssetOutlinerModel::sort(int col, Qt::SortOrder order)
{
    if (m_src) m_src->sort(col, order);   // source resets; our reset forwarding + relocateHost do the rest
}

// ── AssetOutlinerDelegate: Blender-style right-aligned eye + export arrow ────────────────────

QRect AssetOutlinerDelegate::eyeRect(const QRect& rowRect)
{
    constexpr int W = 22;
    return QRect(rowRect.right() - W, rowRect.top(), W, rowRect.height());
}

static QRect outlinerExportRect(const QRect& rowRect)
{
    constexpr int W = 22;
    return QRect(rowRect.right() - 2 * W, rowRect.top(), W, rowRect.height());
}

static void outlinerPaintExport(QPainter* p, const QRect& r, bool on)
{
    const QPointF c = QRectF(r).center();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QColor col = on ? QColor(215, 210, 195) : QColor(95, 90, 82);
    p->setPen(QPen(col, 1.4, Qt::SolidLine, Qt::RoundCap));
    p->setBrush(Qt::NoBrush);
    p->drawPolyline(QPolygonF({{c.x() - 5.5, c.y() + 1.0}, {c.x() - 5.5, c.y() + 5.0},
                               {c.x() + 5.5, c.y() + 5.0}, {c.x() + 5.5, c.y() + 1.0}}));
    p->drawLine(QPointF(c.x(), c.y() + 2.0), QPointF(c.x(), c.y() - 5.5));
    p->drawLine(QPointF(c.x(), c.y() - 5.5), QPointF(c.x() - 3.0, c.y() - 2.5));
    p->drawLine(QPointF(c.x(), c.y() - 5.5), QPointF(c.x() + 3.0, c.y() - 2.5));
    if (!on) p->drawLine(QPointF(c.x() - 6.5, c.y() + 5.5), QPointF(c.x() + 6.5, c.y() - 5.5));
    p->restore();
}

void AssetOutlinerDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const
{
    const QVariant check = index.data(Qt::CheckStateRole);
    if (!check.isValid()) {   // ordinary row — stock painting (browse rows, groups, bones, anims)
        QStyledItemDelegate::paint(p, option, index);
        return;
    }
    const QVariant exp = index.data(AssetOutlinerModel::ExportRole);
    const int reserve = exp.isValid() ? 44 : 22;
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;   // suppress stock left-edge box
    opt.rect.setRight(opt.rect.right() - reserve);
    const QWidget* w = option.widget;
    (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);

    if (exp.isValid())
        outlinerPaintExport(p, outlinerExportRect(option.rect), exp.toBool());

    const bool on = check.toInt() == Qt::Checked;
    const QRectF r = eyeRect(option.rect);
    const QPointF c = r.center();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QColor col = on ? QColor(215, 210, 195) : QColor(105, 100, 90);
    p->setPen(QPen(col, 1.3));
    p->setBrush(Qt::NoBrush);
    QPainterPath lid;
    lid.moveTo(c.x() - 6.0, c.y());
    lid.quadTo(c.x(), c.y() - 5.0, c.x() + 6.0, c.y());
    if (on) {
        QPainterPath bottom;
        bottom.moveTo(c.x() - 6.0, c.y());
        bottom.quadTo(c.x(), c.y() + 5.0, c.x() + 6.0, c.y());
        p->drawPath(lid);
        p->drawPath(bottom);
        p->setBrush(col);
        p->drawEllipse(c, 1.9, 1.9);
    } else {
        p->drawPath(lid);
        p->drawLine(QPointF(c.x() - 2.0, c.y() + 0.5), QPointF(c.x() - 3.5, c.y() + 3.0));
        p->drawLine(QPointF(c.x() + 0.0, c.y() + 1.0), QPointF(c.x() + 0.0, c.y() + 3.5));
        p->drawLine(QPointF(c.x() + 2.0, c.y() + 0.5), QPointF(c.x() + 3.5, c.y() + 3.0));
    }
    p->restore();
}

bool AssetOutlinerDelegate::editorEvent(QEvent* ev, QAbstractItemModel* model,
                                        const QStyleOptionViewItem& option, const QModelIndex& index)
{
    if (index.data(Qt::CheckStateRole).isValid()) {
        const QEvent::Type t = ev->type();
        if (t == QEvent::MouseButtonRelease || t == QEvent::MouseButtonPress
            || t == QEvent::MouseButtonDblClick) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            const QPoint pt = me->position().toPoint();
            if (eyeRect(option.rect).contains(pt)) {
                if (t == QEvent::MouseButtonRelease) {
                    const bool on = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
                    model->setData(index, on ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
                }
                return true;   // consume all eye-area clicks (incl. dbl-click → no expand toggle)
            }
            if (index.data(AssetOutlinerModel::ExportRole).isValid()
                && outlinerExportRect(option.rect).contains(pt)) {
                if (t == QEvent::MouseButtonRelease)
                    if (auto* om = qobject_cast<AssetOutlinerModel*>(model))
                        om->togglePartExport(index);
                return true;
            }
            return false;      // outside the icons: plain selection, NOT the stock left-edge toggle
        }
    }
    return QStyledItemDelegate::editorEvent(ev, model, option, index);
}

bool AssetOutlinerDelegate::helpEvent(QHelpEvent* ev, QAbstractItemView* view,
                                      const QStyleOptionViewItem& option, const QModelIndex& index)
{
    if (index.data(Qt::CheckStateRole).isValid()) {
        if (index.data(AssetOutlinerModel::ExportRole).isValid()
            && outlinerExportRect(option.rect).contains(ev->pos())) {
            QToolTip::showText(ev->globalPos(),
                               QStringLiteral("Include in export — a part is exported when it is "
                                              "visible AND this arrow is on"), view);
            return true;
        }
        if (eyeRect(option.rect).contains(ev->pos())) {
            QToolTip::showText(ev->globalPos(),
                               QStringLiteral("Show / hide in the viewport"), view);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(ev, view, option, index);
}
