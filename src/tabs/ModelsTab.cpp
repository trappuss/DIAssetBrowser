#include "tabs/ModelsTab.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QIcon>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QTreeWidget>
#include <QColor>
#include <QCursor>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSet>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QFrame>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QToolButton>
#include <QWidgetAction>
#include <QTextStream>
#include <QThread>
#include <QTreeView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "app/ClothControls.h"
#include "app/ExportSettings.h"
#include "util/AnimExportScope.h"
#include "app/SehGuard.h"
#include "gl/GLMeshView.h"
#include "gl/ModelThumbRenderer.h"
#include "index/AssetListModel.h"
#include "index/AssetOutlinerModel.h"
#include "index/NameTranslator.h"
#include "tabs/PanelBox.h"
#include "util/CameraPanel.h"   // shared Camera popover (both 3D tabs)
#include "tabs/ViewGlyphs.h"
#include "index/ThumbnailProvider.h"
#include "model/ClothSim.h"
#include "model/GlbExporter.h"
#include "model/ModelResolve.h"
#include "store/AssetStore.h"
#include "tex/TextureDecode.h"
#include "tex/TextureParser.h"
#include "util/CsvCopy.h"
#include "util/FilterBar.h"
#include "util/HintBar.h"
#include "util/HoverInfo.h"
#include "util/MenuText.h"
#include "util/PanelPersist.h"

#include <QLocale>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Clips are 30 fps frame blocks (measured CHAR::ANIM format) — one transport
// "frame" is exactly this many milliseconds.
// The info panel keeps at most this many notes; older ones drop off.
constexpr float kFrameMs = 1000.0f / 30.0f;

// Compact, uniform grid tile: thumbnail centered above a short leaf label. Cell
// size tracks the view's iconSize (so Ctrl+wheel zoom stays dense), the label is
// the file stem only (the full path bloated cells to 3 columns) middle-elided so
// both the family and the variant suffix stay readable, and the thumbnail is
// scaled to fit — never cropped.
class GridTileDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static int iconOf(const QStyleOptionViewItem& o) {
        const int h = o.decorationSize.height();
        return h > 0 ? h : 80;
    }

    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex&) const override {
        const int ic = iconOf(o);
        return QSize(ic + 10, ic + 20);   // pad + one label line
    }

    void paint(QPainter* p, const QStyleOptionViewItem& o,
               const QModelIndex& idx) const override {
        const bool sel = o.state & QStyle::State_Selected;
        if (sel) p->fillRect(o.rect, o.palette.highlight());

        const int ic = iconOf(o);
        const QRect r = o.rect;
        const int top = r.top() + 3;
        const QRect iconBox(r.left() + (r.width() - ic) / 2, top, ic, ic);
        const QVariant dec = idx.data(Qt::DecorationRole);
        if (dec.canConvert<QPixmap>()) {
            const QPixmap pm = dec.value<QPixmap>();
            if (!pm.isNull()) {
                QPixmap s = pm.scaled(ic, ic, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                p->drawPixmap(iconBox.left() + (ic - s.width()) / 2,
                              iconBox.top() + (ic - s.height()) / 2, s);
            }
        }

        QString name = idx.data(Qt::DisplayRole).toString();
        const int slash = name.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0) name = name.mid(slash + 1);
        if (name.endsWith(QLatin1String(".Model"), Qt::CaseInsensitive)) name.chop(6);

        const QRect textRect(r.left() + 1, iconBox.bottom() + 1,
                             r.width() - 2, r.bottom() - iconBox.bottom() - 1);
        p->setPen(sel ? o.palette.highlightedText().color() : o.palette.text().color());
        const QString elided =
            o.fontMetrics.elidedText(name, Qt::ElideMiddle, textRect.width());
        p->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, elided);
    }
};

const char kExportDirKey[] = "models/lastExportDir";

QString exportBaseName(QString name)
{
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) name = name.mid(slash + 1);
    name.replace(QLatin1Char('.'), QLatin1Char('_'));
    return name;
}

// TEXTURE PREVIEW tile channel names (index 0..5), for the RGBA readout label.
const char* kTexChanName(int i)
{
    static const char* const n[6] = {"COLOR",  "ROUGHNESS", "METAL",
                                     "NORMAL", "ALPHA",     "EMISSIVE"};
    return (i >= 0 && i < 6) ? n[i] : "";
}

// Extract one submesh into a standalone, self-contained MeshData: the indices in
// [startIndex, startIndex+indexCount) are compacted to a fresh 0-based vertex set
// (only the vertices this submesh actually references), with every present vertex
// stream carried across. Used by "Export this part". Skinning (bone indices +
// weights) is preserved and still references the full skeleton, so the exporter
// can rig the part exactly as the viewport does. Returns null on a bad index.
std::shared_ptr<di::MeshData> extractSubmesh(const di::MeshData& m, size_t si)
{
    if (si >= m.submeshes.size() || m.indices.empty()) return nullptr;
    const di::SubMeshRange& r = m.submeshes[si];
    const size_t nv = m.positions.size() / 3;
    const bool hasN  = m.normals.size()     == nv * 3;
    const bool hasT  = m.tangents.size()    == nv * 4;
    const bool hasUv = m.uv0.size()         == nv * 2;
    const bool hasBI = m.boneIndices.size() == nv * 4;
    const bool hasBW = m.boneWeights.size() == nv * 4;

    auto out = std::make_shared<di::MeshData>();
    out->skinned = m.skinned;
    out->streams = m.streams;
    std::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(r.vertCount ? r.vertCount : 1024);
    const size_t iEnd = std::min((size_t)r.startIndex + r.indexCount, m.indices.size());
    out->indices.reserve(iEnd > r.startIndex ? iEnd - r.startIndex : 0);
    for (size_t k = r.startIndex; k < iEnd; ++k) {
        const uint32_t gi = m.indices[k];
        if (gi >= nv) continue;   // guard against a stray index
        uint32_t ni;
        auto it = remap.find(gi);
        if (it == remap.end()) {
            ni = (uint32_t)(out->positions.size() / 3);
            remap.emplace(gi, ni);
            out->positions.insert(out->positions.end(),
                                  m.positions.begin() + gi * 3,
                                  m.positions.begin() + gi * 3 + 3);
            if (hasN)  out->normals.insert(out->normals.end(),
                                           m.normals.begin() + gi * 3,
                                           m.normals.begin() + gi * 3 + 3);
            if (hasT)  out->tangents.insert(out->tangents.end(),
                                            m.tangents.begin() + gi * 4,
                                            m.tangents.begin() + gi * 4 + 4);
            if (hasUv) out->uv0.insert(out->uv0.end(),
                                       m.uv0.begin() + gi * 2,
                                       m.uv0.begin() + gi * 2 + 2);
            if (hasBI) out->boneIndices.insert(out->boneIndices.end(),
                                               m.boneIndices.begin() + gi * 4,
                                               m.boneIndices.begin() + gi * 4 + 4);
            if (hasBW) out->boneWeights.insert(out->boneWeights.end(),
                                               m.boneWeights.begin() + gi * 4,
                                               m.boneWeights.begin() + gi * 4 + 4);
        } else {
            ni = it->second;
        }
        out->indices.push_back(ni);
    }
    di::SubMeshRange nr;
    nr.startIndex = 0;
    nr.indexCount = (uint32_t)out->indices.size();
    nr.startVert  = 0;
    nr.vertCount  = (uint32_t)(out->positions.size() / 3);
    out->submeshes.push_back(nr);
    return out;
}

// Keep only the submeshes flagged in `keep`, compacting to a fresh vertex set —
// the multi-range twin of extractSubmesh above. Each kept submesh stays its own
// range, so the exporter still writes one primitive per part and materials keep
// lining up by index. Returns null when nothing (or everything) is hidden, which
// lets the caller fall through to exporting the mesh as-is.
std::shared_ptr<di::MeshData> extractVisibleSubmeshes(const di::MeshData& m,
                                                      const std::vector<uint8_t>& keep)
{
    if (m.indices.empty() || m.submeshes.empty()) return nullptr;
    if (keep.size() != m.submeshes.size()) return nullptr;
    size_t kept = 0;
    for (uint8_t k : keep) kept += k ? 1 : 0;
    if (kept == 0 || kept == keep.size()) return nullptr;   // nothing to do

    const size_t nv = m.positions.size() / 3;
    const bool hasN  = m.normals.size()     == nv * 3;
    const bool hasT  = m.tangents.size()    == nv * 4;
    const bool hasUv = m.uv0.size()         == nv * 2;
    const bool hasBI = m.boneIndices.size() == nv * 4;
    const bool hasBW = m.boneWeights.size() == nv * 4;

    auto out = std::make_shared<di::MeshData>();
    out->skinned = m.skinned;
    out->streams = m.streams;
    std::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(nv / 2 + 1);
    for (size_t si = 0; si < m.submeshes.size(); ++si) {
        if (!keep[si]) continue;
        const di::SubMeshRange& r = m.submeshes[si];
        const uint32_t first = (uint32_t)out->indices.size();
        const size_t iEnd =
            std::min((size_t)r.startIndex + r.indexCount, m.indices.size());
        for (size_t k = r.startIndex; k < iEnd; ++k) {
            const uint32_t gi = m.indices[k];
            if (gi >= nv) continue;
            uint32_t ni;
            auto it = remap.find(gi);
            if (it == remap.end()) {
                ni = (uint32_t)(out->positions.size() / 3);
                remap.emplace(gi, ni);
                out->positions.insert(out->positions.end(),
                                      m.positions.begin() + gi * 3,
                                      m.positions.begin() + gi * 3 + 3);
                if (hasN)  out->normals.insert(out->normals.end(),
                                               m.normals.begin() + gi * 3,
                                               m.normals.begin() + gi * 3 + 3);
                if (hasT)  out->tangents.insert(out->tangents.end(),
                                                m.tangents.begin() + gi * 4,
                                                m.tangents.begin() + gi * 4 + 4);
                if (hasUv) out->uv0.insert(out->uv0.end(),
                                           m.uv0.begin() + gi * 2,
                                           m.uv0.begin() + gi * 2 + 2);
                if (hasBI) out->boneIndices.insert(out->boneIndices.end(),
                                                   m.boneIndices.begin() + gi * 4,
                                                   m.boneIndices.begin() + gi * 4 + 4);
                if (hasBW) out->boneWeights.insert(out->boneWeights.end(),
                                                   m.boneWeights.begin() + gi * 4,
                                                   m.boneWeights.begin() + gi * 4 + 4);
            } else {
                ni = it->second;
            }
            out->indices.push_back(ni);
        }
        di::SubMeshRange nr;
        nr.startIndex = first;
        nr.indexCount = (uint32_t)out->indices.size() - first;
        nr.startVert  = 0;
        nr.vertCount  = (uint32_t)(out->positions.size() / 3);
        out->submeshes.push_back(nr);
    }
    return out->indices.empty() ? nullptr : out;
}

// Batch outputs mangle the WHOLE display path — leaf names alone collide
// across folders (measured on the live repository), and a collision in a
// batch silently overwrites file A with file B while counting both.
QString flatExportName(QString display)
{
    display.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (display.startsWith(QLatin1Char('/')))
        display.remove(0, 1);
    display.replace(QLatin1Char('/'), QLatin1Char('_'));
    display.replace(QLatin1Char('.'), QLatin1Char('_'));
    static const QString bad = QStringLiteral(":*?\"<>|");
    for (const QChar c : bad)
        display.remove(c);
    return display;
}

} // namespace

ModelsTab::ModelsTab(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);

    if (QWidget* hint = makeHintBar(
            this,
            QStringLiteral("Tip: right-click rows for batch export and copy "
                           "actions · wheel on the timeline steps one frame, "
                           "Shift+wheel changes speed · Parts checkboxes hide "
                           "submeshes"),
            "hints/models"))
        lay->addWidget(hint);

    auto* split = new QSplitter(Qt::Horizontal, this);
    lay->addWidget(split, 1);

    auto* leftW = new QWidget(split);
    auto* left  = new QVBoxLayout(leftW);
    left->setContentsMargins(0, 0, 0, 0);
    m_filter = new FilterBar(this, {QStringLiteral("Model"), QStringLiteral("Mesh"),
                                    QStringLiteral("LodModel")},
                             QString(), /*cosmeticFacets=*/true);
    m_filter->setPersistKey(QStringLiteral("models"));
    left->addWidget(m_filter);

    // Show (player/non-player) and Animation (animated/static) facets live
    // INSIDE the filter funnel popup (D4 consolidates all filtering into one
    // menu) rather than as a separate header row. Rows carry a precomputed clip
    // count (AnimFolderIndex), so these refilter at facet speed.
    {
        auto* facets = new QWidget(this);
        auto* fl = new QVBoxLayout(facets);
        fl->setContentsMargins(2, 4, 2, 2);
        fl->setSpacing(3);
        fl->addWidget(new QLabel(QStringLiteral("Show:"), facets));
        m_whoFilter = new QComboBox(facets);
        m_whoFilter->addItem(QStringLiteral("Everything"), FilterSpec::WhoAny);
        m_whoFilter->addItem(QStringLiteral("Player only"), FilterSpec::PlayerOnly);
        m_whoFilter->addItem(QStringLiteral("Non-player only"), FilterSpec::NonPlayerOnly);
        m_whoFilter->setToolTip(QStringLiteral(
            "Player = anything a character can wear or hold: the class folders,\n"
            "the shared gear folder (Char/item), hair and faces, plus any asset\n"
            "whose name decodes to a class."));
        fl->addWidget(m_whoFilter);
        fl->addWidget(new QLabel(QStringLiteral("Animation:"), facets));
        m_animFilter = new QComboBox(facets);
        m_animFilter->addItem(QStringLiteral("All models"), FilterSpec::AnimAny);
        m_animFilter->addItem(QStringLiteral("Animated only"), FilterSpec::AnimOnly);
        m_animFilter->addItem(QStringLiteral("Static only"), FilterSpec::StaticOnly);
        m_animFilter->setToolTip(
            QStringLiteral("Animated = the model resolves at least one playable "
                           ".anim clip.\nMeasured on the archive: all clips ship "
                           "under Char/, so\nWorld, Proxy and EffectModel rows are "
                           "always static."));
        fl->addWidget(m_animFilter);
        // restore last session's choice before any signal is connected
        QSettings st;
        m_whoFilter->setCurrentIndex(
            qBound(0, st.value(QStringLiteral("models/whoFilter")).toInt(), 2));
        m_animFilter->setCurrentIndex(
            qBound(0, st.value(QStringLiteral("models/animFilter")).toInt(), 2));
        m_filter->addPopupSection(facets);   // fold into the filter funnel menu
    }

    m_model = new AssetListModel(this);
    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(AssetListModel::ColName, Qt::AscendingOrder);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    // multi-select feeds the batch .glb export; the preview still follows the
    // current (lead) row
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(AssetListModel::ColName,
                                                      QHeaderView::Stretch);
    m_table->setShowGrid(false);
    m_table->setColumnHidden(AssetListModel::ColType, true);
    // Context menu + Ctrl+C CSV. Policy is set BEFORE CsvCopy::install so its
    // fallback menu stands down and ours wins (documented CsvCopy trap).
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this,
            [this](const QPoint& pos) { showListMenu(pos); });
    CsvCopy::install(m_table);

    // Grid view: the same model + selection as the list, laid out as a
    // thumbnail grid (ColName carries both the thumbnail and the label).
    m_gridView = new QListView(this);
    m_gridView->setModel(m_model);
    m_gridView->setModelColumn(AssetListModel::ColName);
    m_gridView->setViewMode(QListView::IconMode);
    m_gridView->setResizeMode(QListView::Adjust);
    m_gridView->setMovement(QListView::Static);
    m_gridView->setUniformItemSizes(true);
    m_gridView->setWordWrap(false);
    m_gridView->setSpacing(2);
    m_gridView->setItemDelegate(new GridTileDelegate(m_gridView));
    m_gridView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_gridView->setSelectionModel(m_table->selectionModel());   // share → one preview path
    m_gridView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_gridView, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) { showListMenu(pos, m_gridView); });

    // Outliner view: a QTreeView over a wrapping model. Top level mirrors the
    // browse list 1:1 (scroll + select exactly like List); the loaded model's
    // row sprouts its structure subtree inline. Blender-style delegate paints
    // the per-part visibility eye + export arrow.
    m_outlineModel = new AssetOutlinerModel(m_model, this);
    m_outlineView  = new QTreeView(this);
    m_outlineView->setModel(m_outlineModel);
    m_outlineView->setHeaderHidden(true);
    m_outlineView->setMouseTracking(true);   // for texture-node hover previews
    // Non-uniform heights so the subtree rows can be COMPACT (18px) while the
    // flat browse rows keep their taller thumbnail height (D4's approach). Uniform
    // heights forced every tree node to thumbnail height — the "bulky" tree.
    m_outlineView->setUniformRowHeights(false);
    m_outlineView->setIndentation(14);   // tighter than the 20px default
    m_outlineView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_outlineView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_outlineView->setItemDelegate(new AssetOutlinerDelegate(m_outlineView));
    m_outlineView->setExpandsOnDoubleClick(true);
    for (int c = 1; c < m_model->columnCount(); ++c)   // keep only the name/tree column
        m_outlineView->setColumnHidden(c, true);
    m_outlineView->header()->setStretchLastSection(true);
    m_outlineView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_outlineView, &QTreeView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                // Node-aware menus (texture / part / attachment); browse rows fall
                // through to the standard list menu.
                const QModelIndex ix = m_outlineView->indexAt(pos);
                const QPoint gp = m_outlineView->viewport()->mapToGlobal(pos);
                if (const auto* n = m_outlineModel->node(ix)) {
                    if (n->kind == AssetOutlinerModel::Texture && n->ref >= 0) {
                        showTextureNodeMenu(n->ref,
                                            n->aux.isEmpty() ? n->text : n->aux, gp);
                        return;
                    }
                    if (n->kind == AssetOutlinerModel::Part && n->ref >= 0) {
                        if (n->sub >= 0)   // attachment submesh
                            showOutlinerAttachPartMenu(n->sub, n->ref, gp);
                        else
                            showOutlinerPartMenu(n->ref, gp);
                        return;
                    }
                    if (n->kind == AssetOutlinerModel::Attach && n->ref >= 0) {
                        showOutlinerAttachMenu(n->ref,
                                               n->check == Qt::Checked, gp);
                        return;
                    }
                    // The "Attachments" group header → enable/disable all.
                    if (n->kind == AssetOutlinerModel::Group &&
                        n->text.startsWith(QStringLiteral("Attachments"))) {
                        QMenu m(this);
                        m.addAction(QStringLiteral("Enable all attachments"), this,
                                    [this] { setAllAttachments(true); });
                        m.addAction(QStringLiteral("Disable all attachments"), this,
                                    [this] { setAllAttachments(false); });
                        m.exec(gp);
                        return;
                    }
                }
                showListMenu(pos, m_outlineView);
            });
    // Selecting a TOP-LEVEL row (a browse row, parent invalid) loads that model,
    // exactly like the List view; selecting a child node does not reload.
    connect(m_outlineView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                if (cur.isValid() && !cur.parent().isValid()) {
                    m_pendingRow = cur.row();
                    m_selDebounce->start();
                    return;
                }
                // A subtree node became current (arrow keys / click / drag).
                if (const auto* n = m_outlineModel->node(cur)) {
                    // Animation leaf → autoplay it, like the ANIMATIONS list.
                    const QString clip =
                        cur.data(AssetOutlinerModel::AnimNameRole).toString();
                    if (!clip.isEmpty() && m_animBox) {
                        const int idx = m_animBox->findText(clip);
                        if (idx >= 0 && idx != m_animBox->currentIndex())
                            m_animBox->setCurrentIndex(idx);
                    }
                    // Texture leaf → that one image split into RGB/R/G/B/A.
                    else if (n->kind == AssetOutlinerModel::Texture && n->ref >= 0)
                        showTextureInPreview(n->ref);
                    // Material node → back to the PBR channels it feeds the
                    // shader, so moving up the tree returns the material view.
                    else if (n->kind == AssetOutlinerModel::Material)
                        fillTexturePreview(m_lastTextures);
                }
            });
    // Double-click: an animation leaf plays; an attachment toggles load on/off.
    connect(m_outlineView, &QTreeView::doubleClicked, this, [this](const QModelIndex& ix) {
        const QString clip = ix.data(AssetOutlinerModel::AnimNameRole).toString();
        if (!clip.isEmpty() && m_animBox) {
            const int idx = m_animBox->findText(clip);
            if (idx >= 0) m_animBox->setCurrentIndex(idx);   // triggers playback
            return;
        }
        if (const auto* n = m_outlineModel->node(ix))
            if (n->kind == AssetOutlinerModel::Attach && n->ref >= 0)
                setAttachmentLoaded(n->ref, n->check != Qt::Checked);
    });
    // Remember which top-level groups are open (when the option is on), so a new
    // model re-opens the same ones (Animations/Materials/…). Persisted by first
    // word, applied on reveal in buildOutliner.
    auto persistOpenGroup = [this](const QModelIndex& ix, bool open) {
        if (!QSettings()
                 .value(QStringLiteral("models/outliner/rememberOpen"), false)
                 .toBool())
            return;
        const auto* n = m_outlineModel->node(ix);
        if (!n || n->kind != AssetOutlinerModel::Group ||
            ix.parent() != m_outlineModel->hostIndex())
            return;
        const QString word = n->text.section(QLatin1Char(' '), 0, 0);
        QSettings s;
        QStringList set =
            s.value(QStringLiteral("models/outliner/openGroups")).toStringList();
        if (open) { if (!set.contains(word)) set << word; }
        else set.removeAll(word);
        s.setValue(QStringLiteral("models/outliner/openGroups"), set);
    };
    connect(m_outlineView, &QTreeView::expanded, this,
            [persistOpenGroup](const QModelIndex& ix) { persistOpenGroup(ix, true); });
    connect(m_outlineView, &QTreeView::collapsed, this,
            [persistOpenGroup](const QModelIndex& ix) { persistOpenGroup(ix, false); });
    // Hover a texture leaf → large preview popup (reuses the TEXTURE PREVIEW
    // hover-zoom label), so you can inspect a channel without opening the panel.
    connect(m_outlineView, &QAbstractItemView::entered, this,
            [this](const QModelIndex& ix) {
        const auto* n = m_outlineModel->node(ix);
        if (n && n->kind == AssetOutlinerModel::Texture && n->ref >= 0 &&
            n->ref < texprev::Panel::kTiles &&
            sourceForTile(n->ref) && !sourceForTile(n->ref)->isNull()) {
            if (!m_texTilePopup) {
                m_texTilePopup = new QLabel(this, Qt::ToolTip);
                // Never let the hover preview eat a mouse click — a lingering
                // Qt::ToolTip window can otherwise swallow the first click meant
                // for something else (e.g. the Filters funnel).
                m_texTilePopup->setAttribute(Qt::WA_TransparentForMouseEvents);
                m_texTilePopup->setAttribute(Qt::WA_ShowWithoutActivating);
                m_texTilePopup->setStyleSheet(QStringLiteral(
                    "QLabel{border:1px solid #666;background:#111;}"));
            }
            m_texTilePopup->setPixmap(QPixmap::fromImage(sourceForTile(n->ref)->scaled(
                220, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            m_texTilePopup->adjustSize();
            m_texTilePopup->move(QCursor::pos() + QPoint(16, 12));
            m_texTilePopup->show();
        } else if (m_texTilePopup) {
            m_texTilePopup->hide();
        }
    });
    // A part eye toggled in the tree → mirror into the parts panel (the single
    // source of truth) which re-applies the GL submesh mask.
    connect(m_outlineModel, &AssetOutlinerModel::partCheckChanged, this, [this] {
        if (!m_partsList) return;
        QHash<int, bool> checks;
        m_outlineModel->partChecks(checks);
        m_partsSyncing = true;
        for (auto it = checks.constBegin(); it != checks.constEnd(); ++it)
            if (it.key() >= 0 && it.key() < m_partsList->topLevelItemCount())
                m_partsList->topLevelItem(it.key())->setCheckState(
                    0, it.value() ? Qt::Checked : Qt::Unchecked);
        m_partsSyncing = false;
        applyPartsMask();
    });
    // An attachment eye toggled in the outliner → flip the matching ATTACHMENTS
    // list row, which loads/unloads the sibling through the one existing path.
    connect(m_outlineModel, &AssetOutlinerModel::attachCheckChanged, this,
            [this](int repoIdx, bool on) { setAttachmentLoaded(repoIdx, on); });
    // An attachment submesh eye → hide/show that submesh of the attachment's
    // ViewPart (kept in m_attachPartHidden so it survives outliner rebuilds).
    connect(m_outlineModel, &AssetOutlinerModel::attachPartToggled, this,
            [this](int repoIdx, int submesh, bool on) {
        QSet<int>& hidden = m_attachPartHidden[repoIdx];
        if (on) hidden.remove(submesh);
        else    hidden.insert(submesh);
        applyAttachMask(repoIdx);
    });

    // View switcher over one stacked browse area.
    m_browseStack = new QStackedWidget(this);
    m_browseStack->addWidget(m_table);        // 0 List
    m_browseStack->addWidget(m_gridView);     // 1 Grid
    m_browseStack->addWidget(m_outlineView);  // 2 Outliner
    auto* viewRow = new QHBoxLayout;
    viewRow->setContentsMargins(0, 0, 0, 0);
    viewRow->setSpacing(4);
    viewRow->addWidget(new QLabel(QStringLiteral("View:"), this));
    // One dropdown for the browse style (List / Grid / Outliner), D4-style —
    // a glyph + label button with an exclusive menu, not a row of toggles.
    const char* vlabels[3] = {"List", "Grid", "Outliner"};
    const int startView = qBound(0,
        QSettings().value(QStringLiteral("models/browseView"), 0).toInt(), 2);
    auto* viewBtn = new QToolButton(this);
    viewBtn->setPopupMode(QToolButton::InstantPopup);
    viewBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    viewBtn->setStyleSheet(QLatin1String(kToolBtnQss));
    auto* viewMenu = new QMenu(viewBtn);
    auto* vGroup = new QActionGroup(viewMenu);
    vGroup->setExclusive(true);
    for (int i = 0; i < 3; ++i) {
        QAction* a = viewMenu->addAction(QIcon(viewModeGlyph(i)),
                                         QLatin1String(vlabels[i]));
        a->setCheckable(true);
        a->setData(i);
        vGroup->addAction(a);
    }
    // Outliner options (D4-style): auto-expand + per-kind "show" gates. Each is a
    // checkable action writing a QSettings key that buildOutliner reads, so
    // toggling one rebuilds the tree with that section shown/hidden.
    {
        viewMenu->addSeparator();
        auto* outMenu = viewMenu->addMenu(QStringLiteral("Outliner"));
        QSettings s;
        auto addToggle = [this, outMenu, &s](const QString& label, const char* key,
                                             bool def) {
            QAction* a = outMenu->addAction(label);
            a->setCheckable(true);
            const QString k = QStringLiteral("models/outliner/") + QLatin1String(key);
            a->setChecked(s.value(k, def).toBool());
            connect(a, &QAction::toggled, this, [this, k](bool on) {
                QSettings().setValue(k, on);
                buildOutliner();   // in-place: keep scroll/expansion
            });
        };
        addToggle(QStringLiteral("Auto-expand groups"), "autoExpand", true);
        addToggle(QStringLiteral("Remember opened groups"), "rememberOpen", false);
        outMenu->addSeparator();
        addToggle(QStringLiteral("Show skeleton"),    "show_skeleton",   true);
        addToggle(QStringLiteral("Show parts"),       "show_parts",      true);
        addToggle(QStringLiteral("Show materials"),   "show_materials",  true);
        addToggle(QStringLiteral("Show attachments"), "show_attachments",true);
        addToggle(QStringLiteral("Show animations"),  "show_animations", true);
    }
    // 3D thumbnails toggle — lives in the View dropdown (D4 keeps display options
    // together there) rather than as a separate checkbox on the toolbar.
    {
        viewMenu->addSeparator();
        QAction* a = viewMenu->addAction(QStringLiteral("3D thumbnails"));
        a->setCheckable(true);
        a->setToolTip(QStringLiteral(
            "Replace the flat texture icons with small 3D renders of each model."));
        a->setChecked(
            QSettings().value(QStringLiteral("models/view/thumb3d"), false).toBool());
        connect(a, &QAction::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("models/view/thumb3d"), on);
            if (m_model) m_model->setUse3DThumbs(on);
            repaintBrowseViews();
        });
    }
    viewBtn->setMenu(viewMenu);
    auto applyViewLabel = [viewBtn, vlabels](int id) {
        viewBtn->setIcon(QIcon(viewModeGlyph(id)));
        viewBtn->setText(QLatin1String(vlabels[id]));
    };
    connect(vGroup, &QActionGroup::triggered, this,
            [this, applyViewLabel](QAction* a) {
        const int id = a->data().toInt();
        m_browseStack->setCurrentIndex(id);
        applyViewLabel(id);
        QSettings().setValue(QStringLiteral("models/browseView"), id);
        if (id == 2 && m_outlineView && m_outlineModel) {
            // Reveal the loaded model's subtree when switching to the Outliner
            // (the load may have happened in List).
            const QModelIndex host = m_outlineModel->hostIndex();
            if (host.isValid()) {
                m_outlineView->expand(host);
                m_outlineView->setCurrentIndex(host);
                m_outlineView->scrollTo(host, QAbstractItemView::PositionAtCenter);
            }
        }
    });
    if (auto* a = vGroup->actions().value(startView)) a->setChecked(true);
    applyViewLabel(startView);
    m_browseStack->setCurrentIndex(startView);
    viewRow->addWidget(viewBtn);
    viewRow->addStretch(1);
    // (The 3D-thumbnails toggle now lives inside the View dropdown above.)
    left->addLayout(viewRow);
    left->addWidget(m_browseStack, 1);

    // Ctrl+wheel over any browse view zooms the icon / row size. Filter the
    // viewports (that is where wheel events land) and consume only Ctrl+wheel.
    m_thumbPx = qBound(24,
        QSettings().value(QStringLiteral("models/view/thumbPx"), 40).toInt(), 160);
    m_table->viewport()->installEventFilter(this);
    m_gridView->viewport()->installEventFilter(this);
    m_outlineView->viewport()->installEventFilter(this);
    applyThumbSize();

    split->addWidget(leftW);

    auto* rightW = new QWidget(split);
    auto* right  = new QVBoxLayout(rightW);
    right->setContentsMargins(0, 0, 0, 0);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_infoText.attach(m_info);
    // (m_info is placed at the top of the RIGHT panel column, D4-style — see below.)

    QSettings settings;
    auto* controls = new QHBoxLayout();
    // Shading balls (Blender / D4-style): wireframe · flat · shaded · rendered.
    {
        const int mode = qBound(0,
            settings.value(QStringLiteral("models/view/shadeMode"), 3).toInt(), 3);
        auto* sg = new QButtonGroup(this);
        sg->setExclusive(true);
        const char* tips[4] = {"Wireframe", "Flat (solid)", "Shaded", "Rendered (PBR)"};
        for (int i = 0; i < 4; ++i) {
            auto* b = new QToolButton(this);
            b->setCheckable(true);
            b->setAutoRaise(true);
            b->setIcon(QIcon(shadeBallGlyph(i)));
            b->setIconSize(QSize(20, 20));
            b->setToolTip(QLatin1String(tips[i]));
            b->setStyleSheet(QLatin1String(kIconBtnQss));
            sg->addButton(b, i);
            controls->addWidget(b);
        }
        if (auto* b = sg->button(mode)) b->setChecked(true);
        connect(sg, &QButtonGroup::idClicked, this, [this](int id) {
            QSettings().setValue(QStringLiteral("models/view/shadeMode"), id);
            m_view->setShadingMode(id);
        });
    }
    // Channel dropdown — D4's "shadeMore": a slim icon + arrow button (⌄ default,
    // ◆ when a raw channel is active) whose menu holds the channel picker. Sits
    // beside the shading balls instead of a wide combo box; wheel over it cycles.
    {
        auto* chan = new QComboBox(this);
        m_channelCombo = chan;
        chan->addItems({QStringLiteral("Shaded"), QStringLiteral("Base Color"),
                        QStringLiteral("Normal"), QStringLiteral("Roughness"),
                        QStringLiteral("Metallic"), QStringLiteral("AO"),
                        QStringLiteral("Emissive")});
        chan->setCurrentIndex(qBound(0,
            settings.value(QStringLiteral("models/view/channel"), 0).toInt(), 6));

        auto* shadeMore = new QToolButton(this);
        m_shadeMoreBtn = shadeMore;
        shadeMore->setText(QStringLiteral("⌄"));   // ⌄
        shadeMore->setPopupMode(QToolButton::InstantPopup);
        shadeMore->setCursor(Qt::PointingHandCursor);
        shadeMore->setStyleSheet(QLatin1String(kIconBtnQss));
        shadeMore->installEventFilter(this);            // wheel → cycle channels
        auto* sm = new QMenu(shadeMore);
        {
            auto* row = new QWidget(sm);
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(10, 4, 10, 4);
            rl->setSpacing(6);
            rl->addWidget(new QLabel(QStringLiteral("Channel"), row));
            rl->addWidget(chan, 1);
            auto* wa = new QWidgetAction(sm);
            wa->setDefaultWidget(row);
            sm->addAction(wa);
        }
        shadeMore->setMenu(sm);

        auto syncChannelBtn = [this] {
            if (!m_shadeMoreBtn || !m_channelCombo) return;
            const int i = m_channelCombo->currentIndex();
            m_shadeMoreBtn->setText(i == 0 ? QStringLiteral("⌄")
                                           : QStringLiteral("◆"));   // ⌄ / ◆
            m_shadeMoreBtn->setToolTip(
                QStringLiteral("Channel: %1\nScroll here to flip channels · "
                               "click for the list")
                    .arg(m_channelCombo->currentText()));
        };
        connect(chan, &QComboBox::currentIndexChanged, this,
                [this, syncChannelBtn](int i) {
            QSettings().setValue(QStringLiteral("models/view/channel"), i);
            m_view->setViewChannel(i);
            syncChannelBtn();
        });
        syncChannelBtn();
        controls->addWidget(shadeMore);
    }
    // A dropdown button that toggles a Qt::Popup frame. Returns the button so
    // the caller decides where it lives (toolbar or the viewport N-strip); the
    // popup clamps onto the screen so a right-edge strip button opens leftward.
    auto makeDrop = [this](const QString& text, QVBoxLayout*& outLay) -> QToolButton* {
        auto* btn = new QToolButton(this);
        btn->setText(text);
        btn->setStyleSheet(QLatin1String(kToolBtnQss));
        auto* panel = new QFrame(this, Qt::Popup);
        panel->setStyleSheet(QLatin1String(kPanelQss));
        outLay = new QVBoxLayout(panel);
        outLay->setContentsMargins(8, 8, 8, 8);
        connect(btn, &QToolButton::clicked, this, [btn, panel] {
            if (panel->isVisible()) { panel->hide(); return; }
            panel->adjustSize();
            QPoint pos = btn->mapToGlobal(QPoint(0, btn->height()));
            if (QScreen* s = btn->screen()) {
                if (pos.x() + panel->width() > s->geometry().right())
                    pos.setX(btn->mapToGlobal(QPoint(0, 0)).x() - panel->width());
                if (pos.y() + panel->height() > s->geometry().bottom())
                    pos.setY(btn->mapToGlobal(QPoint(0, 0)).y() - panel->height());
            }
            panel->move(pos);
            panel->show();
        });
        return btn;
    };
    auto persistChk = [](QCheckBox* box, const char* key) {
        QObject::connect(box, &QCheckBox::toggled, box,
                         [key](bool on) { QSettings().setValue(QLatin1String(key), on); });
    };

    // ── Overlays ▾ — grid / skeleton / culling / alpha (D4 groups these in a
    //    dropdown; they are NOT individual toolbar buttons) ─────────────────
    {
        QVBoxLayout* ovl = nullptr;
        auto* ovBtn = makeDrop(QStringLiteral("Overlays ▾"), ovl);
        ovBtn->setIcon(QIcon(overlayGlyph()));   // D4 overlays double-circle icon
        ovBtn->setIconSize(QSize(18, 18));
        ovBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        controls->addWidget(ovBtn);
        m_bones = new QCheckBox(QStringLiteral("Skeleton (bones)"), ovl->parentWidget());
        m_grid  = new QCheckBox(QStringLiteral("Ground grid"), ovl->parentWidget());
        m_cull  = new QCheckBox(QStringLiteral("Cull backfaces"), ovl->parentWidget());
        m_alphaBox = new QCheckBox(QStringLiteral("Alpha (cutout transparency)"),
                                   ovl->parentWidget());
        m_alphaBox->setToolTip(QStringLiteral(
            "Cut out see-through texture areas (hair, lashes, cloth fringes) "
            "instead of drawing them as solid quads."));
        m_bones->setChecked(settings.value(QStringLiteral("models/view/bones"), false).toBool());
        m_grid->setChecked(settings.value(QStringLiteral("models/view/grid"), true).toBool());
        m_cull->setChecked(settings.value(QStringLiteral("models/view/cull"), false).toBool());
        m_alphaBox->setChecked(settings.value(QStringLiteral("models/view/alpha"), false).toBool());
        persistChk(m_bones, "models/view/bones");
        persistChk(m_grid,  "models/view/grid");
        persistChk(m_cull,  "models/view/cull");
        persistChk(m_alphaBox, "models/view/alpha");
        // Material emissive glow — the model's own authored animated emissive
        // layer (arcane scroll / star sparkle / fresnel rim). This is normal
        // material rendering; the separate particle-effect experiment was
        // removed (see FX_ARCHIVE.md).
        m_fxBox = new QCheckBox(QStringLiteral("FX (emissive glow)"),
                                ovl->parentWidget());
        m_fxBox->setToolTip(QStringLiteral(
            "Show the material's authored animated emissive layer "
            "(arcane scroll, star sparkle, fresnel rim glow)."));
        m_fxBox->setChecked(
            settings.value(QStringLiteral("models/view/fx"), true).toBool());
        connect(m_fxBox, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("models/view/fx"), on);
            m_view->setFxVisible(on);
        });
        for (QCheckBox* c : {m_bones, m_grid, m_cull, m_alphaBox, m_fxBox})
            ovl->addWidget(c);
    }

    // Cloth physics — its popover button lives on the viewport N-strip (added
    // after m_view). Shares ExportSettings::clothPhysics so viewport + .glb agree.
    {
        QVBoxLayout* clv = nullptr;
        m_clothBtn = makeDrop(QStringLiteral("Cloth"), clv);
        QWidget* cw = clv->parentWidget();
        m_clothBox = new QCheckBox(QStringLiteral("Cloth physics"), cw);
        m_clothBox->setToolTip(QStringLiteral(
            "Simulate soft-body bones (cape / tail / hair) so they follow the body "
            "and sway, instead of floating at bind pose. Also baked into exports."));
        m_clothBox->setChecked(ExportSettings::clothPhysics());
        connect(m_clothBox, &QCheckBox::toggled, this, [this](bool on) {
            ExportSettings::setClothPhysics(on);
            onClothToggled();
        });
        clv->addWidget(m_clothBox);
        auto* clothCfg = new QPushButton(QStringLiteral("Tune…"), cw);
        clothCfg->setToolTip(QStringLiteral("Tune the soft-body solver (gravity, "
            "bone-tracking, stiffness, damping…). Applies to playback and export."));
        clothCfg->setStyleSheet(QLatin1String(kPushBtnQss));
        connect(clothCfg, &QPushButton::clicked, this, &ModelsTab::showClothDialog);
        clv->addWidget(clothCfg);
    }

    // ── Lighting — exposure. Button lives on the viewport N-strip too. ───────
    {
        QVBoxLayout* ll = nullptr;
        m_lightBtn = makeDrop(QStringLiteral("Lighting"), ll);
        QWidget* lp = ll->parentWidget();
        ll->addWidget(new QLabel(QStringLiteral("Exposure"), lp));
        auto* expo = new QSlider(Qt::Horizontal, lp);
        expo->setRange(20, 400);
        expo->setValue(qBound(20,
            int(settings.value(QStringLiteral("view/exposure"), 1.0).toFloat() * 100.0f), 400));
        connect(expo, &QSlider::valueChanged, this, [this](int v) {
            QSettings().setValue(QStringLiteral("view/exposure"), v / 100.0);
            m_view->update();
        });
        ll->addWidget(expo);
    }

    controls->addStretch(1);
    right->addLayout(controls);

    // Parts panel: hidden until a mesh with >1 submesh loads.
    m_partsBox = new QWidget(this);
    auto* pbox = new QVBoxLayout(m_partsBox);
    pbox->setContentsMargins(0, 2, 0, 2);
    auto* phdr = new QHBoxLayout();
    phdr->addStretch(1);   // title comes from the PARTS PanelBox header
    auto* pAll = new QPushButton(QStringLiteral("All"), this);
    auto* pNone = new QPushButton(QStringLiteral("None"), this);
    auto* pInv = new QPushButton(QStringLiteral("Invert"), this);
    for (QPushButton* b : {pAll, pNone, pInv}) {
        b->setFixedWidth(56);
        b->setStyleSheet(QLatin1String(kPushBtnQss));
        phdr->addWidget(b);
    }
    pbox->addLayout(phdr);
    m_partsList = new QTreeWidget(this);
    m_partsList->setMaximumHeight(120);
    m_partsList->setColumnCount(3);
    m_partsList->setHeaderLabels({QStringLiteral("PART"), QStringLiteral("TRIS"),
                                  QStringLiteral("MATERIAL")});
    m_partsList->setRootIsDecorated(false);
    m_partsList->setUniformRowHeights(true);
    m_partsList->setAlternatingRowColors(true);
    m_partsList->header()->setStretchLastSection(true);
    m_partsList->setColumnWidth(0, 120);
    m_partsList->setColumnWidth(1, 54);
    m_partsList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_partsList, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& p) {
                QMenu menu(this);
                if (QTreeWidgetItem* it = m_partsList->itemAt(p)) {
                    const int soloRow = m_partsList->indexOfTopLevelItem(it);
                    const bool vis = it->checkState(0) == Qt::Checked;
                    menu.addAction(QStringLiteral("Solo this part"), this,
                                   [this, soloRow] {
                        m_partsSyncing = true;
                        for (int i = 0; i < m_partsList->topLevelItemCount(); ++i)
                            m_partsList->topLevelItem(i)->setCheckState(0,
                                i == soloRow ? Qt::Checked : Qt::Unchecked);
                        m_partsSyncing = false;
                        applyPartsMask();
                    });
                    menu.addAction(vis ? QStringLiteral("Hide this part")
                                       : QStringLiteral("Show this part"),
                                   this, [this, soloRow, vis] {
                        if (QTreeWidgetItem* t =
                                m_partsList->topLevelItem(soloRow))
                            t->setCheckState(0, vis ? Qt::Unchecked : Qt::Checked);
                        applyPartsMask();
                    });
                    menu.addAction(QStringLiteral("Frame this part"), this,
                                   [this, soloRow] {
                        m_view->frameToSubmesh(0, (size_t)soloRow);
                    });
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("Export this part…"), this,
                                   [this, soloRow] { exportPartGlb(soloRow); });
                    menu.addSeparator();
                }
                menu.addAction(QStringLiteral("Show all parts"), this, [this] {
                    m_partsSyncing = true;
                    for (int i = 0; i < m_partsList->topLevelItemCount(); ++i)
                        m_partsList->topLevelItem(i)->setCheckState(0, Qt::Checked);
                    m_partsSyncing = false;
                    applyPartsMask();
                });
                menu.addAction(QStringLiteral("Invert visibility"), this, [this] {
                    m_partsSyncing = true;
                    for (int i = 0; i < m_partsList->topLevelItemCount(); ++i) {
                        QTreeWidgetItem* t = m_partsList->topLevelItem(i);
                        t->setCheckState(0, t->checkState(0) == Qt::Checked
                                                ? Qt::Unchecked : Qt::Checked);
                    }
                    m_partsSyncing = false;
                    applyPartsMask();
                });
                menu.exec(m_partsList->viewport()->mapToGlobal(p));
            });
    connect(m_partsList, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* it, int) {
                if (!it || !QSettings()
                                .value(QStringLiteral("models/framePartOnPick"),
                                       false)
                                .toBool())
                    return;
                m_view->frameToSubmesh(0,
                    (size_t)m_partsList->indexOfTopLevelItem(it));
            });
    pbox->addWidget(m_partsList);
    m_partsBox->setVisible(false);
    // (m_partsBox is hosted by the PARTS PanelBox in the right column — see below.)

    // Attachments panel: sibling models that belong with this one. Checking one
    // renders it alongside, auto-playing its clip matching the primary's anim.
    m_attachBox = new QWidget(this);
    auto* abox = new QVBoxLayout(m_attachBox);
    abox->setContentsMargins(0, 2, 0, 2);
    auto* ahdr = new QHBoxLayout();
    ahdr->addStretch(1);   // title comes from the ATTACHMENTS PanelBox header
    auto* aAll  = new QPushButton(QStringLiteral("All"), this);
    auto* aNone = new QPushButton(QStringLiteral("None"), this);
    for (QPushButton* b : {aAll, aNone}) { b->setFixedWidth(56); b->setStyleSheet(QLatin1String(kPushBtnQss)); ahdr->addWidget(b); }
    abox->addLayout(ahdr);
    m_attachList = new QListWidget(this);
    m_attachList->setMaximumHeight(110);
    m_attachList->setContextMenuPolicy(Qt::CustomContextMenu);
    abox->addWidget(m_attachList);
    m_attachBox->setVisible(false);
    // (m_attachBox is hosted by the ATTACHMENTS PanelBox in the right column.)

    connect(m_attachList, &QListWidget::itemChanged, this, [this] {
        if (!m_attachSyncing) onAttachToggled();
    });
    connect(m_attachList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& p) { showAttachMenu(p); });
    auto setAllAttach = [this](bool on) {
        m_attachSyncing = true;
        for (int i = 0; i < m_attachList->count(); ++i)
            m_attachList->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        m_attachSyncing = false;
        onAttachToggled();
    };
    connect(aAll,  &QPushButton::clicked, this, [setAllAttach] { setAllAttach(true); });
    connect(aNone, &QPushButton::clicked, this, [setAllAttach] { setAllAttach(false); });
    auto setAllParts = [this](int mode) {   // 0 none, 1 all, 2 invert
        m_partsSyncing = true;
        for (int i = 0; i < m_partsList->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = m_partsList->topLevelItem(i);
            const bool cur = it->checkState(0) == Qt::Checked;
            const bool nv  = mode == 2 ? !cur : mode == 1;
            it->setCheckState(0, nv ? Qt::Checked : Qt::Unchecked);
        }
        m_partsSyncing = false;
        applyPartsMask();
    };
    connect(pAll,  &QPushButton::clicked, this, [setAllParts] { setAllParts(1); });
    connect(pNone, &QPushButton::clicked, this, [setAllParts] { setAllParts(0); });
    connect(pInv,  &QPushButton::clicked, this, [setAllParts] { setAllParts(2); });
    connect(m_partsList, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem*, int) {
        if (!m_partsSyncing) applyPartsMask();
    });

    // Transport bar.
    auto* animRow = new QHBoxLayout();
    animRow->addWidget(new QLabel(QStringLiteral("Anim:"), this));
    m_animBox = new QComboBox(this);
    m_animBox->setEnabled(false);
    m_animBox->setMinimumWidth(160);
    // doubles as a search box: a character folder ships hundreds of clips
    m_animBox->setEditable(true);
    m_animBox->setInsertPolicy(QComboBox::NoInsert);
    if (QCompleter* c = m_animBox->completer()) {
        c->setCompletionMode(QCompleter::PopupCompletion);
        c->setFilterMode(Qt::MatchContains);
        c->setCaseSensitivity(Qt::CaseInsensitive);
    }
    // click-anywhere-to-open (see WardrobeTab::eventFilter for the rationale:
    // an editable combo body otherwise only places a text cursor)
    if (m_animBox->lineEdit())
        m_animBox->lineEdit()->installEventFilter(this);
    animRow->addWidget(m_animBox, 1);
    m_stepBack = new QPushButton(QStringLiteral("<"), this);
    m_stepBack->setFixedWidth(26);
    m_stepBack->setToolTip(QStringLiteral("Step one frame back (pauses)"));
    m_stepBack->setEnabled(false);
    animRow->addWidget(m_stepBack);
    m_playBtn = new QPushButton(QStringLiteral("Pause"), this);
    m_playBtn->setFixedWidth(52);
    m_playBtn->setEnabled(false);
    animRow->addWidget(m_playBtn);
    m_stepFwd = new QPushButton(QStringLiteral(">"), this);
    m_stepFwd->setFixedWidth(26);
    m_stepFwd->setToolTip(QStringLiteral("Step one frame forward (pauses)"));
    m_stepFwd->setEnabled(false);
    animRow->addWidget(m_stepFwd);
    m_timeSlider = new QSlider(Qt::Horizontal, this);
    m_timeSlider->setEnabled(false);
    m_timeSlider->installEventFilter(this);   // wheel = one frame
    animRow->addWidget(m_timeSlider, 2);
    m_frameSpin = new QSpinBox(this);
    m_frameSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_frameSpin->setAlignment(Qt::AlignRight);
    m_frameSpin->setFixedWidth(52);
    m_frameSpin->setToolTip(QStringLiteral("Current frame — type to jump"));
    m_frameSpin->setEnabled(false);
    animRow->addWidget(m_frameSpin);
    m_frameMax = new QLabel(QStringLiteral("/ 0"), this);
    animRow->addWidget(m_frameMax);
    m_timeLabel = new QLabel(QStringLiteral("0.00 / 0.00 s"), this);
    m_timeLabel->setStyleSheet(QStringLiteral("color:#9a9a9a;"));
    animRow->addWidget(m_timeLabel);
    m_speedBox = new QComboBox(this);
    for (const auto& [label, mult] :
         {std::pair<QString, float>{QStringLiteral("0.25x"), 0.25f},
          {QStringLiteral("0.5x"), 0.5f},
          {QStringLiteral("1x"), 1.0f},
          {QStringLiteral("1.5x"), 1.5f},
          {QStringLiteral("2x"), 2.0f}})
        m_speedBox->addItem(label, mult);
    m_speedBox->setCurrentIndex(2);
    m_speedBox->setToolTip(QStringLiteral(
        "Playback speed (Shift+wheel on the timeline also changes it)"));
    animRow->addWidget(m_speedBox);
    m_loop = new QCheckBox(QStringLiteral("Loop"), this);
    m_loop->setChecked(true);
    animRow->addWidget(m_loop);
    m_view = new GLMeshView(this);
    m_view->setMinimumWidth(420);
    // Double-click a part in the viewport → select it in the PARTS panel (D4's
    // partFocused). The viewport already frames it when framePartOnPick is set.
    connect(m_view, &GLMeshView::submeshPicked, this, [this](int sub) {
        if (!m_partsList || sub < 0 ||
            sub >= m_partsList->topLevelItemCount())
            return;
        m_partsList->setCurrentItem(m_partsList->topLevelItem(sub));
        m_partsList->scrollToItem(m_partsList->topLevelItem(sub));
    });
    right->addWidget(m_view, 1);        // viewport fills the MIDDLE column
    // ── Camera — the shared panel (util/CameraPanel.h). Every control in it was
    //    hand-built per tab before, and the two copies had already drifted; both
    //    tabs now show exactly the same panel. Built HERE, after m_view exists,
    //    because CameraPanel reads the live camera to seed its readouts. Its
    //    popover button is reparented onto the viewport N-strip just below.
    {
        QVBoxLayout* cl2 = nullptr;
        // The gizmo persists the projection it toggles under this prefix,
        // so it can only ever write THIS tab's key.
        m_view->setSettingsPrefix(QStringLiteral("models"));
        m_camBtn = makeDrop(QStringLiteral("Camera"), cl2);
        const CameraPanel::Widgets cw = CameraPanel::build(
            cl2, m_view, QStringLiteral("models"),
            QStringLiteral("Auto-frame model on load"));
        m_turntable = cw.turntable;
        m_frameBtn  = cw.frameBtn;
    }
    CameraPanel::applyStartupState(m_view, QStringLiteral("models"));

    right->addLayout(animRow);          // transport BELOW the viewport (D4 layout)
    split->addWidget(rightW);           // middle column

    // ── Viewport N-strip (D4): Camera / Lighting popover icons on the
    //    viewport's right edge, plus a » arrow that collapses the side panels.
    m_vpStrip = new QWidget(m_view);
    m_vpStrip->setStyleSheet(QStringLiteral(
        "background:rgba(28,28,30,190);border-radius:4px;"));
    auto* sv = new QVBoxLayout(m_vpStrip);
    sv->setContentsMargins(2, 3, 2, 3);
    sv->setSpacing(3);
    auto* sideArrow = new QToolButton(m_vpStrip);
    sideArrow->setText(QStringLiteral("»"));
    sideArrow->setCheckable(true);
    sideArrow->setToolTip(QStringLiteral(
        "Hide the side panels (this arrow brings them back)"));
    sideArrow->setCursor(Qt::PointingHandCursor);
    sideArrow->setFixedSize(26, 18);
    sideArrow->setStyleSheet(QLatin1String(kArrowBtnQss));
    connect(sideArrow, &QToolButton::toggled, this, [this, sideArrow](bool on) {
        if (m_farW) m_farW->setVisible(!on);
        sideArrow->setText(on ? QStringLiteral("«") : QStringLiteral("»"));
    });
    sv->addWidget(sideArrow);
    {   // reparent the Camera / Lighting popover buttons onto the strip as icons
        const struct { QToolButton* b; QPixmap g; } stripBtns[] = {
            {m_camBtn, cameraGlyph()}, {m_lightBtn, lightGlyph()},
            {m_clothBtn, clothGlyph()}};
        for (const auto& e : stripBtns) {
            if (!e.b) continue;
            e.b->setToolTip(e.b->text() + QStringLiteral(" settings"));
            e.b->setText(QString());
            e.b->setIcon(QIcon(e.g));
            e.b->setIconSize(QSize(16, 16));
            e.b->setFixedSize(26, 26);
            e.b->setParent(m_vpStrip);
            sv->addWidget(e.b);
        }
    }
    m_vpStrip->adjustSize();
    m_view->installEventFilter(this);   // keep the strip pinned to the edge

    // ── RIGHT: D4-style panel strip + PanelBox splitter ───────────────────
    // A narrow vertical strip of checkable toggles sits beside a vertical
    // splitter of PanelBoxes. Toggling a strip button shows/hides its panel,
    // ▲▼ reorder among the visible ones, ✕ hides, and the open set persists.
    auto* farW = new QWidget(split);
    m_farW = farW;                      // the » arrow collapses this column
    auto* far  = new QVBoxLayout(farW);
    far->setContentsMargins(0, 0, 0, 0);
    far->setSpacing(3);
    far->addWidget(m_info);             // model info header on top of the column
    // Below the info: [ icon strip | panel splitter ] — strip on the LEFT, D4-style.
    auto* panelRow = new QHBoxLayout();
    panelRow->setContentsMargins(0, 0, 0, 0);
    panelRow->setSpacing(3);
    auto* stripW = new QWidget(farW);
    stripW->setFixedWidth(26);
    stripW->setStyleSheet(QStringLiteral("background:#232323;border-radius:4px;"));
    m_rstripLay = new QVBoxLayout(stripW);
    m_rstripLay->setContentsMargins(3, 4, 3, 4);
    m_rstripLay->setSpacing(4);
    m_rstripLay->addStretch(1);         // toggles insert above this trailing stretch
    panelRow->addWidget(stripW);
    m_rstack = new QSplitter(Qt::Vertical, farW);
    m_rstack->setChildrenCollapsible(false);
    m_rstack->setHandleWidth(4);
    panelRow->addWidget(m_rstack, 1);
    far->addLayout(panelRow, 1);
    split->addWidget(farW);

    // ANIMATIONS panel content (D4): the loaded model's clips; click to play.
    m_animList = new QListWidget(farW);
    m_animList->setAlternatingRowColors(true);
    m_animList->setToolTip(QStringLiteral(
        "Select a clip to play it — arrow keys and click-drag autoplay as you move"));
    // currentItemChanged (not itemClicked) so ARROW KEYS and CLICK-DRAG through the
    // list autoplay each clip as the selection lands on it, matching D4.
    connect(m_animList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* cur, QListWidgetItem*) {
        if (!cur || !m_animBox || m_animListSyncing) return;
        const int idx = m_animBox->findText(cur->text());
        if (idx >= 0 && idx != m_animBox->currentIndex())
            m_animBox->setCurrentIndex(idx);   // triggers playback
    });

    // TEXTURE PREVIEW panel — shared body (see util/TexturePreview.h). It shows
    // the material's PBR channels by default and one texture's raw RGBA split
    // when a texture row is selected.
    m_texPrevBox = m_texPrev.build(farW, this);
    for (int i = 0; i < texprev::Panel::kTiles; ++i) {
        QLabel* t = m_texPrev.tile[i];
        if (!t) continue;
        connect(t, &QWidget::customContextMenuRequested, this,
                [this, i](const QPoint& p) {
                    if (m_texPrev.full[i].isNull() || !m_texPrev.tile[i]) return;
                    QMenu menu(this);
                    menu.addAction(QStringLiteral("Copy image"), this, [this, i] {
                        QGuiApplication::clipboard()->setImage(m_texPrev.full[i]);
                    });
                    menu.addAction(QStringLiteral("Save image…"), this, [this, i] {
                        const QString f = QFileDialog::getSaveFileName(
                            this, QStringLiteral("Save channel"),
                            QStringLiteral("%1.png")
                                .arg(QString::fromLatin1(m_texPrev.channelName(i))
                                         .toLower()),
                            QStringLiteral("PNG (*.png)"));
                        if (!f.isEmpty()) m_texPrev.full[i].save(f);
                    });
                    menu.exec(m_texPrev.tile[i]->mapToGlobal(p));
                });
    }

    // INFO panel (D4 DATA/INFO page): compact label/value form, selectable text,
    // values clip rather than widen the column. Populated by fillInfoPanel().
    m_infoBox = new QWidget(farW);
    {
        auto* form = new QFormLayout(m_infoBox);
        form->setContentsMargins(2, 2, 2, 2);
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(3);
        form->setLabelAlignment(Qt::AlignLeft);
        for (const QString& key :
             {QStringLiteral("Name"), QStringLiteral("Type"),
              QStringLiteral("Category"), QStringLiteral("MPK"),
              QStringLiteral("Entry ID"), QStringLiteral("Vertices"),
              QStringLiteral("Triangles"), QStringLiteral("Parts"),
              QStringLiteral("Bones"), QStringLiteral("Skinned"),
              QStringLiteral("Animations"), QStringLiteral("Material"),
              QStringLiteral("Textures")}) {
            auto* v = new QLabel(QStringLiteral("—"), m_infoBox);
            v->setTextInteractionFlags(Qt::TextSelectableByMouse);
            v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            v->setMinimumWidth(1);
            m_infoVals.insert(key, v);
            auto* k = new QLabel(key + QStringLiteral(":"), m_infoBox);
            k->setStyleSheet(QStringLiteral("color:#9a8f78;"));
            form->addRow(k, v);
        }
    }

    // MATERIALS panel (D4 MATERIALS/SHADING): the model's material and the
    // texture bound to each channel. Populated by fillMaterialsPanel().
    m_matBox = new QWidget(farW);
    {
        auto* mv = new QVBoxLayout(m_matBox);
        mv->setContentsMargins(0, 2, 0, 2);
        mv->setSpacing(2);
        m_matList = new QTreeWidget(m_matBox);
        m_matList->setColumnCount(3);
        m_matList->setHeaderLabels({QStringLiteral("CHANNEL"), QStringLiteral("TEXTURE"),
                                    QStringLiteral("SIZE")});
        m_matList->setRootIsDecorated(false);
        m_matList->setAlternatingRowColors(true);
        m_matList->setUniformRowHeights(true);
        m_matList->header()->setStretchLastSection(false);
        m_matList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_matList->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_matList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        // Which row you pick decides what the TEXTURE PREVIEW shows: the
        // material header gives the PBR channels the shader consumes, a channel
        // row gives that one texture split into RGB/R/G/B/A.
        connect(m_matList, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                    if (!cur) return;
                    const int tile = cur->data(0, Qt::UserRole).toInt();
                    if (tile < 0) { fillTexturePreview(m_lastTextures); return; }
                    const QImage* src = sourceForTile(tile);
                    if (!src || src->isNull()) return;
                    showTextureChannels(*src, cur->text(0));
                });
        mv->addWidget(m_matList);
    }

    addRightPage(QStringLiteral("INFO"), m_infoBox);
    addRightPage(QStringLiteral("MATERIALS"), m_matBox);
    addRightPage(QStringLiteral("TEXTURE PREVIEW"), m_texPrevBox);
    addRightPage(QStringLiteral("PARTS"), m_partsBox);
    addRightPage(QStringLiteral("ATTACHMENTS"), m_attachBox);
    addRightPage(QStringLiteral("ANIMATIONS"), m_animList);
    restorePanelLayout();               // bring up the saved (or default-all) set

    split->setStretchFactor(0, 2);   // list
    split->setStretchFactor(1, 4);   // viewport
    split->setStretchFactor(2, 2);   // panels
    split->setSizes({320, 700, 340});   // sane first-run widths (all three visible)
    // NOTE: fresh key — the old "panels/modelsSplit" state had only 2 sections and
    // starved the new 3rd (panel) column to zero width.
    PanelPersist::bind(split, QStringLiteral("panels/modelsSplit3"));

    // small corner badge over the viewport while a load is in flight
    m_loading = new QLabel(QStringLiteral(" loading… "), m_view);
    m_loading->setStyleSheet(QStringLiteral(
        "background: rgba(20,20,22,180); color: #ddd; border-radius: 3px;"));
    m_loading->move(8, 8);
    m_loading->hide();

    connect(m_bones, &QAbstractButton::toggled, this,
            [this](bool on) { m_view->setShowSkeleton(on); });
    connect(m_grid, &QAbstractButton::toggled, this,
            [this](bool on) { m_view->setShowGrid(on); });
    connect(m_cull, &QAbstractButton::toggled, this,
            [this](bool on) { m_view->setBackfaceCull(on); });
    connect(m_alphaBox, &QAbstractButton::toggled, this,
            [this](bool on) { m_view->setAlpha(on); });
    connect(m_turntable, &QAbstractButton::toggled, this,
            [this](bool on) { m_view->setTurntable(on); });
    // push the restored states into the fresh view
    m_view->setShadingMode(qBound(0,
        settings.value(QStringLiteral("models/view/shadeMode"), 3).toInt(), 3));
    m_view->setViewChannel(qBound(0,
        settings.value(QStringLiteral("models/view/channel"), 0).toInt(), 6));
    m_view->setAutoFrame(
        settings.value(QStringLiteral("models/autoFrame"), true).toBool());
    m_view->setShowSkeleton(m_bones->isChecked());
    m_view->setShowGrid(m_grid->isChecked());
    m_view->setBackfaceCull(m_cull->isChecked());
    m_view->setAlpha(m_alphaBox->isChecked());
    m_view->setFxVisible(m_fxBox->isChecked());
    m_view->setTurntable(m_turntable->isChecked());
    m_view->setTurntableSpeed(
        settings.value(QStringLiteral("models/view/turntableSpeed"), 30).toInt());

    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(33);
    connect(m_animTimer, &QTimer::timeout, this, [this] {
        if (!m_player.valid()) return;
        m_animT += 33.0f * m_speed;
        if (m_animT > m_player.durationMs()) {
            if (m_loop->isChecked()) {
                m_animT = 0.0f;
            } else {
                m_animT = m_player.durationMs();
                m_animTimer->stop();
                m_playBtn->setText(QStringLiteral("Play"));
            }
        }
        animTick(m_animT);
    });
    // Debounced: a mouse-wheel scroll over the combo fires this once per item;
    // loading a clip per step spawned a worker storm (near-freeze on big v3
    // anim lists). Collapse to the final selection after a short settle.
    m_animDebounce = new QTimer(this);
    m_animDebounce->setSingleShot(true);
    m_animDebounce->setInterval(160);
    connect(m_animDebounce, &QTimer::timeout, this, [this] {
        if (m_pendingAnimIdx >= 0) selectAnim(m_pendingAnimIdx);
    });
    connect(m_animBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                m_pendingAnimIdx = idx;
                if (idx <= 0) {         // "(bind pose)" — stop immediately
                    m_animDebounce->stop();
                    selectAnim(idx);
                } else {
                    m_animDebounce->start();
                }
            });
    connect(m_playBtn, &QPushButton::clicked, this, [this] {
        if (!m_player.valid()) return;
        if (m_animTimer->isActive()) {
            m_animTimer->stop();
            m_playBtn->setText(QStringLiteral("Play"));
        } else {
            // Play at the end restarts (D4 transport convention)
            if (m_animT >= m_player.durationMs()) m_animT = 0.0f;
            m_animTimer->start();
            m_playBtn->setText(QStringLiteral("Pause"));
        }
    });
    connect(m_stepBack, &QPushButton::clicked, this, [this] { seekFrames(-1); });
    connect(m_stepFwd,  &QPushButton::clicked, this, [this] { seekFrames(+1); });
    connect(m_timeSlider, &QSlider::sliderMoved, this, [this](int v) {
        if (!m_player.valid()) return;
        m_animT = (float)v;
        animTick(m_animT);
    });
    connect(m_frameSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int f) {
                if (!m_player.valid() || m_frameSpin->signalsBlocked()) return;
                m_animT = qBound(0.0f, f * kFrameMs, m_player.durationMs());
                animTick(m_animT);
            });
    connect(m_speedBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) { applySpeed(i); });

    connect(m_filter, &FilterBar::changed, this, &ModelsTab::applyFilter);
    connect(m_animFilter, &QComboBox::currentIndexChanged, this,
            &ModelsTab::applyFilter);
    connect(m_whoFilter, &QComboBox::currentIndexChanged, this,
            &ModelsTab::applyFilter);

    m_selDebounce = new QTimer(this);
    m_selDebounce->setSingleShot(true);
    m_selDebounce->setInterval(150);
    connect(m_selDebounce, &QTimer::timeout, this, [this] {
        if (m_pendingRow >= 0) startLoad(m_pendingRow);
    });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                if (cur.isValid()) {
                    m_pendingRow = cur.row();
                    m_selDebounce->start();
                }
            });

    connect(this, &ModelsTab::meshReady, this,
            [this](int generation, std::shared_ptr<di::MeshData> mesh,
                   MeshTextures textures, QString info,
                   std::shared_ptr<di::SkinSkeleton> skel,
                   std::shared_ptr<di::BoneParents> hierarchy,
                   std::shared_ptr<di::BoneLocals> locals, QString displayName,
                   AnimListPtr anims) {
                if (generation != m_generation.load()) return;
                stopAnim(false);   // pose/clip belong to the previous mesh
                setInfoBase(info);
                m_lastMesh     = mesh;
                m_lastSkel     = skel;
                m_lastHier     = hierarchy;
                m_lastLocals   = locals;
                m_lastTextures = textures;
                m_lastName     = displayName;
                m_loading->hide();
                fillTexturePreview(textures);          // TEXTURE PREVIEW tiles
                if (mesh) {
                    m_view->setMesh(mesh, textures);   // resets to a single part
                    m_attach.clear();                  // drop the old model's parts
                    buildPartsList();
                    buildAttachList();
                    // Skeleton overlay: joint positions from the SkinSkeleton
                    // (mesh space), parent links from the .skeleton tree —
                    // walking up past bones absent from this mesh's skin set,
                    // same rule the exporter uses.
                    m_bindSegs.clear();
                    m_bindJoints.clear();
                    if (skel && !skel->bones.empty()) {
                        std::vector<float> segs, joints;
                        QHash<QString, int> byName;
                        joints.reserve(skel->bones.size() * 3);
                        for (int j = 0; j < (int)skel->bones.size(); ++j) {
                            const di::SkinBone& b = skel->bones[j];
                            const QString nm = QString::fromStdString(b.name);
                            if (!byName.contains(nm)) byName[nm] = j;
                            joints.push_back(b.world[0]);
                            joints.push_back(b.world[1]);
                            joints.push_back(b.world[2]);
                        }
                        for (int j = 0; j < (int)skel->bones.size(); ++j) {
                            int pj = -1;
                            if (hierarchy) {
                                std::string cur = skel->bones[j].name;
                                for (int hop = 0; hop < 256; ++hop) {   // cycle guard
                                    auto it = hierarchy->find(cur);
                                    if (it == hierarchy->end() || it->second.empty())
                                        break;
                                    cur = it->second;
                                    const int c = byName.value(
                                        QString::fromStdString(cur), -1);
                                    if (c >= 0 && c != j) { pj = c; break; }
                                }
                            }
                            if (pj < 0) continue;
                            const di::SkinBone& b = skel->bones[j];
                            const di::SkinBone& p = skel->bones[pj];
                            segs.insert(segs.end(), {p.world[0], p.world[1], p.world[2],
                                                     b.world[0], b.world[1], b.world[2]});
                        }
                        m_bindSegs   = segs;      // restored when playback stops
                        m_bindJoints = joints;
                        m_view->setSkeleton(std::move(segs), std::move(joints));
                    }
                    // animation picker
                    m_anims = anims;
                    m_animBox->blockSignals(true);
                    m_animBox->clear();
                    m_animBox->addItem(QStringLiteral("(bind pose)"));
                    if (skel && anims)
                        for (const AnimRef& a : *anims)
                            m_animBox->addItem(a.display);
                    m_animBox->blockSignals(false);
                    if (m_animList) {
                        m_animList->clear();
                        if (skel && anims)
                            for (const AnimRef& a : *anims)
                                m_animList->addItem(a.display);
                    }
                    const bool canAnim =
                        skel && anims && !anims->empty() && mesh->skinned;
                    m_animBox->setEnabled(canAnim);
                    if (canAnim)
                        qInfo("anim: %zu clips available for %s", anims->size(),
                              qPrintable(displayName));
                    buildOutliner(/*reveal=*/true);   // skeleton/parts/attachments/anims tree
                } else {
                    m_view->clearMesh();
                    m_partsBox->setVisible(false);
                    m_partsList->clear();
                    buildOutliner();   // clears the subtree (flat list remains)
                    m_animBox->blockSignals(true);
                    m_animBox->clear();
                    m_animBox->blockSignals(false);
                    m_animBox->setEnabled(false);
                    if (m_animList) m_animList->clear();
                    m_anims.reset();
                }
                // INFO + MATERIALS reflect the final state (anims now current).
                fillInfoPanel();
                fillMaterialsPanel();
            },
            Qt::QueuedConnection);

    // clip loader -> GUI
    connect(this, &ModelsTab::animReady, this,
            [this](int generation, int seq, std::shared_ptr<di::AnimClip> clip,
                   QString name, QString err) {
                if (generation != m_generation.load() ||
                    seq != m_animSeq.load())
                    return;   // stale pick or new model — drop silently
                if (!clip) {
                    addInfoNote(QStringLiteral("%1 cannot play: %2")
                                    .arg(name.toHtmlEscaped(), err.toHtmlEscaped()));
                    markClipUnsupported(name);
                    qWarning("anim: %s parse failed (%s)", qPrintable(name),
                             qPrintable(err));
                    return;
                }
                m_clip = clip;
                m_playClip = maybeCloth(m_clip, m_lastSkel.get(), m_lastHier.get(),
                                        m_lastLocals.get());
                m_player.init(m_playClip.get(), m_lastSkel.get(), m_lastLocals.get(),
                      m_lastHier.get());
                m_curAnimName = name;      // attachments match by clip name
                syncAttachAnims();
                qInfo("anim: %s -> %zu tracks, %u ms, %u key array(s)%s, "
                      "%d/%zu bones matched",
                      qPrintable(name), m_clip->tracks.size(), m_clip->durationMs,
                      (unsigned)m_clip->keyArrays,
                      m_clip->mixedArrays ? " (per-track mix)" : "",
                      m_player.matchedBones(),
                      m_lastSkel ? m_lastSkel->bones.size() : 0);
                m_timeSlider->setRange(0, (int)m_clip->durationMs);
                m_timeSlider->setEnabled(true);
                const int frames = (int)(m_clip->durationMs / kFrameMs);
                m_frameSpin->blockSignals(true);
                m_frameSpin->setRange(0, frames);
                m_frameSpin->setValue(0);
                m_frameSpin->blockSignals(false);
                m_frameSpin->setEnabled(true);
                m_frameMax->setText(QStringLiteral("/ %1").arg(frames));
                m_playBtn->setEnabled(true);
                m_stepBack->setEnabled(true);
                m_stepFwd->setEnabled(true);
                m_playBtn->setText(QStringLiteral("Pause"));
                m_animT = 0.0f;
                animTick(0.0f);
                m_animTimer->start();
            },
            Qt::QueuedConnection);

    connect(this, &ModelsTab::exportDone, this,
            [this](int generation, QString message) {
                qInfo("glb: %s", qPrintable(message));
                // A stale append would assert something false about the model
                // the info panel NOW describes (review finding).
                if (generation != m_generation.load()) return;
                addInfoNote(message.toHtmlEscaped());
            },
            Qt::QueuedConnection);
    connect(this, &ModelsTab::batchProgress, this,
            [this](int generation, QString message) {
                if (generation != m_batchGen.load()) return;
                emit statusText(message);
            },
            Qt::QueuedConnection);
    connect(this, &ModelsTab::batchDone, this,
            [this](int generation, QString message) {
                m_batchRunning = false;
                if (generation == m_batchGen.load()) addInfoNote(message.toHtmlEscaped());
                qInfo("glb batch: %s", qPrintable(message));
            },
            Qt::QueuedConnection);

    // Hover preview over the list: identity + decoded diffuse via the same
    // Model -> Material -> tBaseMap chain the thumbnails use.
    m_hover = new HoverPreview(
        m_table,
        [this](const QModelIndex& idx, QList<HoverPreview::Line>* lines,
               QImage* img) { return resolveHover(idx, lines, img); },
        this);
    connect(this, &ModelsTab::hoverDecoded, this,
            [this](int seq, int entryId, QImage image) {
                if (seq != m_hoverSeq.load()) return;
                m_hoverImg   = image;
                m_hoverEntry = entryId;
                m_hover->refresh();
            },
            Qt::QueuedConnection);

    buildOutliner();   // show the empty-state hint until a model loads
}

bool ModelsTab::resolveHover(const QModelIndex& idx,
                             QList<HoverPreview::Line>* lines, QImage* image)
{
    const AssetRow* r = m_model->rowAt(idx.row());
    if (!r || !m_idx || !m_idx->store) return false;

    QString leaf = r->display;
    const int slash = leaf.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) leaf = leaf.mid(slash + 1);
    lines->append({leaf, HoverInfo::Col::kName});
    if (HoverInfo::on("mdl/meaning") && !r->meaning.isEmpty())
        lines->append({r->meaning, HoverInfo::Col::kSeries});
    if (HoverInfo::on("mdl/path"))
        lines->append({r->display, HoverInfo::Col::kFile});
    if (HoverInfo::on("mdl/info"))
        lines->append({QStringLiteral("%1 · %2 · %3")
                           .arg(r->type,
                                QLocale::c().formattedDataSize(r->size, 1),
                                QString::fromStdString(
                                    m_idx->store->mpk().pakFileName(
                                        m_idx->store->mpk()
                                            .entries()[r->entryId]))),
                       HoverInfo::Col::kMeta});
    if (m_lastMesh && m_lastName == r->display && HoverInfo::on("mdl/loaded"))
        lines->append({QStringLiteral("loaded in the viewport"),
                       HoverInfo::Col::kGood});

    if (!HoverInfo::imagePreview()) return true;
    const int entryId = (int)r->entryId;
    if (m_hoverEntry == entryId) {
        // Null after a decode means it FAILED — never re-kick for this row.
        if (!m_hoverImg.isNull()) *image = m_hoverImg;
        return true;
    }
    // When 3D thumbnails are active, the hover popup shows a larger 3D render
    // (synchronous — a hover is a single deliberate dwell). Falls through to the
    // flat texture preview if the model can't be rendered.
    if (m_model && m_model->use3DThumbs() && m_thumbs3d) {
        QImage img;
        if (m_thumbs3d->renderPreview(*r, 256, &img) && !img.isNull()) {
            m_hoverImg   = img;
            m_hoverEntry = entryId;
            *image = img;
            return true;
        }
    }
    if (m_thumbs) {
        QPixmap pm;
        if (m_thumbs->peek(r->entryId, &pm)) *image = pm.toImage();
    }
    const int seq = ++m_hoverSeq;
    auto store = m_idx->store;
    const int32_t repoIdx = r->repoIdx;
    const QString type = r->type;
    QThread* worker =
        QThread::create([this, seq, store, entryId, repoIdx, type] {
            seh::installSehTranslator();
            QImage img;
            seh::runGuarded("hover-decode", [&] {
                const size_t blob = di::resolveThumbTexture(
                    *store, repoIdx, (uint32_t)entryId, type);
                if (blob == (size_t)-1) return;
                const std::vector<uint8_t> raw = store->mpk().readAsset(blob);
                di::Texture2D tex;
                std::string err;
                if (!di::isTexture2D(raw.data(), raw.size()) ||
                    !di::parseTexture2D(raw.data(), raw.size(), &tex, &err))
                    return;
                const int want = HoverInfo::previewPx();
                int best = -1, bestArea = -1, largest = -1, largestArea = -1;
                for (int i = 0; i < (int)tex.mips.size(); ++i) {
                    const int area = tex.mips[i].width * tex.mips[i].height;
                    if (area > largestArea) { largestArea = area; largest = i; }
                    if (tex.mips[i].width >= want &&
                        tex.mips[i].height >= want &&
                        (best < 0 || area < bestArea)) {
                        best = i;
                        bestArea = area;
                    }
                }
                const int pick = best >= 0 ? best : largest;
                if (pick >= 0) img = TextureDecode::decode(tex, pick).image;
            });
            emit hoverDecoded(seq, entryId, img);
        });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
    return true;
}

bool ModelsTab::eventFilter(QObject* obj, QEvent* ev)
{
    // Keep the viewport N-strip pinned to the viewport's right edge (below the
    // axis gizmo) as the viewport resizes.
    if (obj == m_view && m_vpStrip &&
        (ev->type() == QEvent::Resize || ev->type() == QEvent::Show)) {
        m_vpStrip->adjustSize();
        // y=104 clears the 88px axis gizmo pinned at the top-right (y 8..96).
        m_vpStrip->move(m_view->width() - m_vpStrip->width() - 6, 104);
        m_vpStrip->raise();
    }
    // TEXTURE PREVIEW tile: the shared panel owns BOTH the value-under-cursor
    // readout and the hover-zoom, so Enter has to reach it. This tab used to
    // keep its own 320px popup for these tiles — a second copy of what the
    // Textures tab also had, which is exactly the duplication the shared panel
    // exists to end. (m_texTilePopup still serves the OUTLINER hover below.)
    if (ev->type() == QEvent::Enter || ev->type() == QEvent::Leave ||
        ev->type() == QEvent::MouseMove)
        m_texPrev.hover(obj, ev);
    if (ev->type() == QEvent::MouseButtonRelease && m_animBox &&
        m_animBox->lineEdit() && obj == m_animBox->lineEdit() &&
        m_animBox->isEnabled() && !m_animBox->view()->isVisible()) {
        m_animBox->lineEdit()->selectAll();
        m_animBox->showPopup();
    }
    // Cursor left the outliner → drop any texture hover preview.
    if (ev->type() == QEvent::Leave && m_outlineView &&
        obj == m_outlineView->viewport() && m_texTilePopup)
        m_texTilePopup->hide();
    // Wheel over the ⌄/◆ channel button cycles channels in place (D4 shadeMore).
    if (ev->type() == QEvent::Wheel && obj == m_shadeMoreBtn && m_channelCombo) {
        auto* we = static_cast<QWheelEvent*>(ev);
        const int dir = we->angleDelta().y() > 0 ? -1 : 1;
        const int n = m_channelCombo->count();
        m_channelCombo->setCurrentIndex(
            (m_channelCombo->currentIndex() + dir + n) % n);
        return true;
    }
    // Ctrl+wheel over a browse view zooms the icon / row size.
    if (ev->type() == QEvent::Wheel && m_table && m_gridView && m_outlineView &&
        (obj == m_table->viewport() || obj == m_gridView->viewport() ||
         obj == m_outlineView->viewport())) {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (we->modifiers() & Qt::ControlModifier) {
            bumpThumbSize(we->angleDelta().y() > 0 ? 1 : -1);
            return true;   // consume — don't also scroll
        }
    }
    // Timeline wheel: exactly ONE frame per notch (the stock QSlider wheel
    // jumps by wheelScrollLines); Shift+wheel steps the playback speed.
    if (obj == m_timeSlider && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        const int dir = we->angleDelta().y() > 0 ? 1 : -1;
        if (we->modifiers() & Qt::ShiftModifier) {
            const int i = qBound(0, m_speedBox->currentIndex() + dir,
                                 m_speedBox->count() - 1);
            m_speedBox->setCurrentIndex(i);   // applySpeed runs via the signal
            emit statusText(QStringLiteral("Speed: %1")
                                .arg(m_speedBox->currentText()));
        } else if (m_player.valid()) {
            seekFrames(dir);
        }
        return true;   // consume — never fall through to the stock jump
    }
    return QWidget::eventFilter(obj, ev);
}

ModelsTab::~ModelsTab()
{
    // Before the workers are joined: the camera is pure GUI state and the view
    // is still alive here.
    CameraPanel::rememberOnExit(m_view, QStringLiteral("models"));
    // BOTH counters: the batch exporter aborts on m_batchGen (it deliberately
    // ignores m_generation so a preview click can't kill a 300-model run), so
    // bumping only m_generation made closing the window block until the entire
    // batch had finished writing.
    ++m_generation;
    ++m_batchGen;
    const QSet<QThread*> workers = m_workers;
    for (QThread* w : workers)
        w->wait();
}

void ModelsTab::setIndex(std::shared_ptr<AssetIndex> idx)
{
    ++m_generation;              // in-flight loads reference the old store
    ++m_batchGen;                // batch exports die with the store, not a click
    ++m_hoverSeq;                // and so do hover decodes
    m_hoverImg = QImage();
    m_hoverEntry = -1;
    if (m_hover) m_hover->hidePopup();
    m_idx = std::move(idx);
    m_pendingRow = -1;
    m_infoText.clear();
    m_loading->hide();
    stopAnim(false);
    m_anims.reset();
    m_animBox->blockSignals(true);
    m_animBox->clear();
    m_animBox->blockSignals(false);
    m_animBox->setEnabled(false);
    m_view->clearMesh();
    m_partsBox->setVisible(false);
    m_partsList->clear();
    m_lastMesh.reset();
    m_lastSkel.reset();
    m_lastHier.reset();
    m_lastLocals.reset();
    m_lastTextures = MeshTextures();
    m_anims.reset();
    m_curRepoIdx = -1;
    m_curRow = AssetRow();
    fillTexturePreview(MeshTextures());   // clear the TEXTURE PREVIEW tiles
    fillInfoPanel();                      // clear INFO
    fillMaterialsPanel();                 // clear MATERIALS
    m_lastName.clear();
    m_filter->setIndex(m_idx);
    // Thumbnail providers are per-store; drop the model's pointers BEFORE the
    // old providers join their workers in the destructor.
    m_model->setThumbnailProvider(nullptr);
    m_model->setModelThumbs(nullptr);
    m_thumbs.reset();
    m_thumbs3d.reset();
    if (m_idx && m_idx->store) {
        // Decode/render at 128px so zoomed icons (Ctrl+wheel) stay crisp; both
        // caches stay in a fixed memory band (entry count scales with 1/area).
        constexpr int kThumbDecodePx = 128;
        m_thumbs = std::make_unique<ThumbnailProvider>(m_idx->store, kThumbDecodePx);
        connect(m_thumbs.get(), &ThumbnailProvider::ready, this, [this] {
            // ready() is queued cross-thread; an event posted just before a
            // provider swap can still arrive after it — guard the pointer.
            if (!m_thumbs) return;
            m_thumbs->commit();
            repaintBrowseViews();
        });
        m_model->setThumbnailProvider(m_thumbs.get());

        // 3D-rendered thumbnails (opt-in). The renderer parses on worker
        // threads and renders on the GUI thread during commit().
        m_thumbs3d = std::make_unique<ModelThumbRenderer>(
            m_idx->store, kThumbDecodePx);
        connect(m_thumbs3d.get(), &ModelThumbRenderer::ready, this, [this] {
            if (!m_thumbs3d) return;
            m_thumbs3d->commit();
            repaintBrowseViews();
        });
        m_model->setModelThumbs(m_thumbs3d.get());
        m_model->setUse3DThumbs(
            QSettings().value(QStringLiteral("models/view/thumb3d"), false).toBool());
    }
    // Hand the index to the model HERE — applyFilter only re-filters, so this
    // is the one place the model learns about a new index (regression fix:
    // relying on applyFilter for it left the tab permanently empty).
    m_model->setIndex(m_idx, currentSpec());
    emit statusText(QStringLiteral("%L1 model entries").arg(m_model->visibleCount()));
}

void ModelsTab::stopAnim(bool restoreBind)
{
    ++m_animSeq;   // poison any clip load still in flight
    m_animTimer->stop();
    m_playBtn->setText(QStringLiteral("Play"));
    m_playBtn->setEnabled(false);
    m_stepBack->setEnabled(false);
    m_stepFwd->setEnabled(false);
    m_timeSlider->setEnabled(false);
    m_frameSpin->setEnabled(false);
    m_frameSpin->blockSignals(true);
    m_frameSpin->setValue(0);
    m_frameSpin->blockSignals(false);
    m_frameMax->setText(QStringLiteral("/ 0"));
    m_timeLabel->setText(QStringLiteral("0.00 / 0.00 s"));
    m_player.init(nullptr, nullptr);
    m_clip.reset();
    m_animT = 0.0f;
    m_view->clearPose();
    m_curAnimName.clear();      // attachments fall back to their rest pose
    syncAttachAnims();
    if (restoreBind)
        m_view->setSkeleton(std::vector<float>(m_bindSegs),
                            std::vector<float>(m_bindJoints));
}

// Async clip pick: the old version read + parsed the .anim ON the GUI thread —
// the one synchronous read path left in the tab (audit finding). The worker is
// seq-guarded so rapid combo picks only apply the last one.
void ModelsTab::selectAnim(int comboIdx)
{
    stopAnim(comboIdx <= 0);
    if (comboIdx <= 0 || !m_anims || !m_lastSkel ||
        comboIdx > (int)m_anims->size() || !m_idx)
        return;
    const AnimRef ref = (*m_anims)[(size_t)comboIdx - 1];
    const int generation = m_generation.load();
    const int seq = m_animSeq.load();
    auto store = m_idx->store;
    QThread* worker = QThread::create([this, generation, seq, store, ref] {
        seh::installSehTranslator();
        std::shared_ptr<di::AnimClip> clip;
        QString err;
        seh::runGuarded("anim-load", [&] {
            const std::vector<uint8_t> raw =
                store->mpk().readAsset((size_t)ref.entryId);
            auto c = std::make_shared<di::AnimClip>();
            std::string aerr;
            if (!raw.empty() &&
                di::parseAnim(raw.data(), raw.size(), c.get(), &aerr))
                clip = c;
            else
                err = QString::fromStdString(aerr);
        });
        if (!clip && err.isEmpty())
            err = QStringLiteral("clip load crashed (see log)");
        emit animReady(generation, seq, clip, ref.display, err);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

void ModelsTab::seekFrames(int delta)
{
    if (!m_player.valid()) return;
    if (m_animTimer->isActive()) {
        m_animTimer->stop();
        m_playBtn->setText(QStringLiteral("Play"));
    }
    m_animT = qBound(0.0f, m_animT + delta * kFrameMs, m_player.durationMs());
    animTick(m_animT);
}

void ModelsTab::updateFrameUi(float tMs)
{
    const float dur = m_player.valid() ? m_player.durationMs() : 0.0f;
    m_frameSpin->blockSignals(true);
    m_frameSpin->setValue(qRound(tMs / kFrameMs));
    m_frameSpin->blockSignals(false);
    m_timeLabel->setText(QStringLiteral("%1 / %2 s")
                             .arg(tMs / 1000.0, 0, 'f', 2)
                             .arg(dur / 1000.0, 0, 'f', 2));
}

void ModelsTab::applySpeed(int comboIdx)
{
    m_speed = m_speedBox->itemData(comboIdx).toFloat();
    if (m_speed <= 0.0f) m_speed = 1.0f;
}

// Pose-only tick. Split out of animTick because a capture scrubs through every
// frame of the clip and must NOT drag the transport slider/spinner along with
// it — those are the user's playhead, and they get restored by the capture's
// final seek(savedFrame) anyway.
void ModelsTab::animPose(float tMs)
{
    if (!m_player.valid()) return;
    std::vector<float> mats, segs, joints;
    m_player.evaluate(tMs, &mats, &segs, &joints);
    m_view->setPose(std::move(mats));
    // Attachments share the clock; each samples its own clip and contributes its
    // bones to the skeleton overlay.
    for (size_t k = 0; k < m_attach.size(); ++k) {
        Attachment& a = m_attach[k];
        if (!a.player.valid()) continue;   // no matching clip -> stays at bind
        std::vector<float> m2, s2, j2;
        a.player.evaluate(tMs, &m2, &s2, &j2);
        m_view->setPartPose(k + 1, std::move(m2));
        segs.insert(segs.end(), s2.begin(), s2.end());
        joints.insert(joints.end(), j2.begin(), j2.end());
    }
    m_view->setSkeleton(std::move(segs), std::move(joints));
}

void ModelsTab::animTick(float tMs)
{
    if (!m_player.valid()) return;
    animPose(tMs);
    m_timeSlider->blockSignals(true);
    m_timeSlider->setValue((int)tMs);
    m_timeSlider->blockSignals(false);
    updateFrameUi(tMs);
}

void ModelsTab::buildPartsList()
{
    m_partsSyncing = true;
    m_partsList->clear();
    const bool show = m_lastMesh && m_lastMesh->submeshes.size() > 1;
    if (show) {
        // The model's single material name — cheap in-memory dep-walk (no blob
        // read); every submesh in DI shares it.
        QString matName;
        if (m_idx && m_idx->store && m_curRepoIdx >= 0) {
            const di::Repository* repo = m_idx->store->repo();
            if (repo && m_curRepoIdx < (int)repo->entries.size())
                for (const std::string& h :
                     repo->entries[(size_t)m_curRepoIdx].related) {
                    auto bit = repo->byHash.find(h);
                    if (bit != repo->byHash.end() &&
                        repo->typeOf(repo->entries[bit->second]) == "Material") {
                        matName = QString::fromStdString(
                            repo->entries[bit->second].name);
                        break;
                    }
                }
        }
        for (size_t i = 0; i < m_lastMesh->submeshes.size(); ++i) {
            const di::SubMeshRange& r = m_lastMesh->submeshes[i];
            auto* it = new QTreeWidgetItem(m_partsList);
            it->setText(0, QStringLiteral("part %1").arg(i));
            it->setText(1, QString::number(r.indexCount / 3));
            it->setText(2, matName);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(0, Qt::Checked);
        }
    }
    m_partsBox->setVisible(show);
    m_partsSyncing = false;
    // fresh mesh starts fully visible; no mask push needed (empty = all)
}

// Fill the TEXTURE PREVIEW tiles from the loaded material's textures. Tiles:
// COLOR · ROUGHNESS · METAL · NORMAL · ALPHA · EMISSIVE. Roughness/metal/alpha
// are single-channel greyscale extracts (mix = R rough, G metal, B AO).
void ModelsTab::fillTexturePreview(const MeshTextures& tex)
{
    m_texPrev.showMaterial(tex, currentMaterialName());
    if (m_texPrevBox) m_texPrevBox->setVisible(true);
    refreshTexPanelTitle();
}

// The image a PBR tile is derived FROM. Roughness and metal are both the mix
// map; alpha is the base colour's fourth channel — so several tiles share one
// source texture, which is exactly why the RGBA split is worth having.
const QImage* ModelsTab::sourceForTile(int tile) const
{
    switch (tile) {
        case 0: case 4: return &m_lastTextures.diffuse;
        case 1: case 2: return &m_lastTextures.mix;
        case 3:         return &m_lastTextures.normal;
        case 5:         return &m_lastTextures.emissive;
        default:        return nullptr;
    }
}

void ModelsTab::showTextureChannels(const QImage& img, const QString& name)
{
    if (img.isNull()) return;
    ensurePanelVisible(QStringLiteral("TEXTURE PREVIEW"));
    m_texPrev.showTexture(img, name);
    if (m_texPrevBox) m_texPrevBox->setVisible(true);
    refreshTexPanelTitle();
}

void ModelsTab::refreshTexPanelTitle()
{
    const int page = m_panelIndex.value(QStringLiteral("TEXTURE PREVIEW"), -1);
    if (page < 0 || page >= (int)m_rsections.size()) return;
    if (QLabel* l = m_rsections[(size_t)page]->label) l->setText(m_texPrev.title());
}

// The loaded model's single material name — cheap in-memory dep-walk (no blob
// read); every submesh in DI shares it. Empty when uncatalogued or none found.
QString ModelsTab::currentMaterialName() const
{
    if (!m_idx || !m_idx->store || m_curRepoIdx < 0) return {};
    const di::Repository* repo = m_idx->store->repo();
    if (!repo || m_curRepoIdx >= (int)repo->entries.size()) return {};
    for (const std::string& h : repo->entries[(size_t)m_curRepoIdx].related) {
        auto bit = repo->byHash.find(h);
        if (bit != repo->byHash.end() &&
            repo->typeOf(repo->entries[bit->second]) == "Material")
            return QString::fromStdString(repo->entries[bit->second].name);
    }
    return {};
}

// Fill the INFO panel (D4 DATA/INFO page) from the loaded row + mesh + textures.
// Every value is real game data — nothing invented; missing facts show "—".
void ModelsTab::fillInfoPanel()
{
    auto set = [this](const QString& k, const QString& v) {
        if (QLabel* l = m_infoVals.value(k))
            l->setText(v.isEmpty() ? QStringLiteral("—") : v);
    };
    if (m_curRepoIdx < 0 && !m_lastMesh) {
        for (QLabel* l : m_infoVals) l->setText(QStringLiteral("—"));
        return;
    }
    set(QStringLiteral("Name"),
        m_lastName.isEmpty() ? m_curRow.display : m_lastName);
    set(QStringLiteral("Type"), m_curRow.type);
    set(QStringLiteral("Category"),
        m_curRow.cat2.isEmpty() ? m_curRow.cat1
                                : m_curRow.cat1 + QStringLiteral(" / ") + m_curRow.cat2);
    set(QStringLiteral("MPK"), m_curRow.mpkName);
    set(QStringLiteral("Entry ID"),
        m_curRow.entryId ? QString::number(m_curRow.entryId) : QString());
    if (m_lastMesh) {
        set(QStringLiteral("Vertices"),
            QLocale().toString((qulonglong)m_lastMesh->vertexCount()));
        set(QStringLiteral("Triangles"),
            QLocale().toString((qulonglong)(m_lastMesh->indices.size() / 3)));
        set(QStringLiteral("Parts"),
            QString::number(qMax<size_t>(1, m_lastMesh->submeshes.size())));
        set(QStringLiteral("Skinned"),
            m_lastMesh->skinned ? QStringLiteral("yes") : QStringLiteral("no"));
    } else {
        set(QStringLiteral("Vertices"), QString());
        set(QStringLiteral("Triangles"), QString());
        set(QStringLiteral("Parts"), QString());
        set(QStringLiteral("Skinned"), QString());
    }
    set(QStringLiteral("Bones"),
        m_lastSkel && !m_lastSkel->bones.empty()
            ? QString::number(m_lastSkel->bones.size())
            : QString());
    set(QStringLiteral("Animations"),
        m_anims && !m_anims->empty() ? QString::number(m_anims->size()) : QString());
    set(QStringLiteral("Material"), currentMaterialName());
    QStringList chans;
    if (!m_lastTextures.diffuse.isNull())  chans << QStringLiteral("color");
    if (!m_lastTextures.normal.isNull())   chans << QStringLiteral("normal");
    if (!m_lastTextures.mix.isNull())      chans << QStringLiteral("mix");
    if (!m_lastTextures.emissive.isNull()) chans << QStringLiteral("emissive");
    set(QStringLiteral("Textures"), chans.join(QStringLiteral(", ")));
}

// Fill the MATERIALS panel (D4 MATERIALS/SHADING): the resolved material name as
// the top row, then one row per bound channel with its source-texture name and
// pixel size. Channel→texture names come from the same repository dep-walk the
// outliner uses; a channel with no bound blob is simply omitted.
void ModelsTab::fillMaterialsPanel()
{
    if (!m_matList) return;
    m_matList->clear();
    const QString matName = currentMaterialName();
    if (m_matBox)
        m_matBox->setVisible(m_lastMesh != nullptr || !matName.isEmpty());
    if (!matName.isEmpty()) {
        auto* head = new QTreeWidgetItem(m_matList);
        head->setFirstColumnSpanned(true);
        head->setText(0, QStringLiteral("Material — %1").arg(matName));
        head->setData(0, Qt::UserRole, -1);   // -1 = the material itself
        QFont f = head->font(0);
        f.setBold(true);
        head->setFont(0, f);
    }
    // UserRole carries the PBR tile the row's texture backs, so selecting the
    // row can show that texture's own channels.
    auto add = [this](const QString& chan, const QImage& img, int tile) {
        auto* it = new QTreeWidgetItem(m_matList);
        it->setText(0, chan);
        it->setData(0, Qt::UserRole, tile);
        if (img.isNull()) {
            it->setText(1, QStringLiteral("—"));
        } else {
            it->setText(1, QStringLiteral("bound"));
            it->setText(2, QStringLiteral("%1×%2").arg(img.width()).arg(img.height()));
        }
    };
    add(QStringLiteral("Base Color"), m_lastTextures.diffuse, 0);
    add(QStringLiteral("Normal"),     m_lastTextures.normal, 3);
    add(QStringLiteral("Rough/Metal/AO"), m_lastTextures.mix, 1);
    add(QStringLiteral("Emissive"),   m_lastTextures.emissive, 5);
}

// Bring a right-column panel up by its registration title (e.g. "TEXTURE
// PREVIEW"). Checking the strip toggle runs showPanel and persists the layout.
void ModelsTab::ensurePanelVisible(const QString& title)
{
    const int page = m_panelIndex.value(title, -1);
    if (page < 0 || page >= (int)m_rpageBtns.size()) return;
    if (!m_rpageBtns[page]->isChecked())
        m_rpageBtns[page]->setChecked(true);   // → showPanel(page, true)
}

// Route an outliner texture leaf to the TEXTURE PREVIEW panel: reveal the panel,
// flash the matching tile's zoom popup, and mark it as the "hot" tile so a
// glance lands on it. Deliberately does NOT touch the viewport's PBR channel.
void ModelsTab::showTextureInPreview(int tile)
{
    if (tile < 0 || tile >= texprev::Panel::kTiles) return;
    const QImage* src = sourceForTile(tile);
    if (!src || src->isNull()) return;
    // Selecting a TEXTURE means "show me what is stored in this file", so the
    // panel switches to the raw RGBA split rather than highlighting a PBR tile.
    QString name = QLatin1String(texprev::Panel::materialName(tile));
    showTextureChannels(*src, name);
}

// Context menu for an outliner texture leaf — export or copy the channel image
// shown in the TEXTURE PREVIEW panel, plus a shortcut to reveal it there.
void ModelsTab::showTextureNodeMenu(int tile, const QString& name, const QPoint& gp)
{
    if (tile < 0 || tile >= texprev::Panel::kTiles) return;
    // Export/copy the texture ITSELF, not the derived grey channel a PBR tile
    // happens to be showing — this node IS the texture.
    const QImage* src = sourceForTile(tile);
    const bool have = src && !src->isNull();
    QMenu menu(this);
    menu.addAction(QStringLiteral("Show channels (RGBA)"), this,
                   [this, tile] { showTextureInPreview(tile); });
    QAction* exp = menu.addAction(QStringLiteral("Export texture…"), this,
                                  [this, src, name, tile] {
        const QString base = name.isEmpty()
                                 ? QString::fromLatin1(
                                       texprev::Panel::materialName(tile))
                                 : name;
        const QString f = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export texture"),
            base + QStringLiteral(".png"), QStringLiteral("PNG (*.png)"));
        if (!f.isEmpty() && src) src->save(f);
    });
    exp->setEnabled(have);
    QAction* cpy = menu.addAction(QStringLiteral("Copy image"), this, [src] {
        if (src) QGuiApplication::clipboard()->setImage(*src);
    });
    cpy->setEnabled(have);
    menu.exec(gp);
}

namespace {
using OutNode = AssetOutlinerModel::Node;
// Recursively add a bone and its children as outliner Nodes. Depth-capped
// against name cycles; children sorted for stable display.
void addBoneNode(OutNode* parent, const std::string& name,
                 const std::unordered_map<std::string, std::vector<std::string>>& kids,
                 int depth)
{
    OutNode* it = parent->add(
        new OutNode(AssetOutlinerModel::Bone, QString::fromStdString(name)));
    if (depth >= 48) return;
    auto kit = kids.find(name);
    if (kit == kids.end()) return;
    std::vector<std::string> sorted = kit->second;
    std::sort(sorted.begin(), sorted.end());
    for (const std::string& c : sorted) addBoneNode(it, c, kids, depth + 1);
}

// Resolve a model/attachment's material + per-channel textures into outliner
// nodes under `parent`. Every name comes straight from the repository (blob ->
// content hash -> repo entry name); a channel is listed only when the material
// actually references its blob, so nothing here is invented.
void addMaterialNodes(OutNode* parent, const di::DiAssetStore& store,
                      int32_t repoIdx, const MeshTextures& tex)
{
    const di::Repository* repo = store.repo();
    if (repoIdx < 0 || !repo) return;
    const di::ResolvedModel r = di::resolveModelChain(store, repoIdx);

    QString matName;
    if (r.matRepo >= 0 && (size_t)r.matRepo < repo->entries.size())
        matName = QString::fromStdString(repo->entries[(size_t)r.matRepo].name);

    // Hierarchy (D4-style, per request): Materials · N → material name → textures.
    // DI resolves a single shared material per model, so N is 1.
    OutNode* matsRoot = parent->add(new OutNode(
        AssetOutlinerModel::Group, QStringLiteral("Materials · 1")));
    OutNode* matRoot = matsRoot->add(new OutNode(
        AssetOutlinerModel::Material,
        matName.isEmpty() ? QStringLiteral("Material") : matName));

    auto friendly = [&](size_t blob) -> QString {
        if (blob == (size_t)-1 || blob >= store.mpk().entries().size()) return {};
        const std::string& bn = store.mpk().entries()[blob].name;
        std::string hex;
        if (di::guidBlobHash(bn, &hex)) {
            auto it = repo->byHash.find(hex);
            if (it != repo->byHash.end())
                return QString::fromStdString(repo->entries[it->second].name);
        }
        const QString n = QString::fromStdString(bn);   // fallback: blob file stem
        const int s = n.lastIndexOf(QLatin1Char('/'));
        return s >= 0 ? n.mid(s + 1) : n;
    };

    // tile = index into the TEXTURE PREVIEW panel's 6 channel tiles
    // (0 COLOR · 1 ROUGHNESS · 2 METAL · 3 NORMAL · 4 ALPHA · 5 EMISSIVE); the
    // node carries it in `ref` so selecting the leaf routes to that tile.
    struct Chan { const char* role; size_t blob; const QImage* img; int tile; };
    const Chan chans[4] = {
        {"diffuse",  r.texBlob, &tex.diffuse,  0},
        {"normal",   r.nrmBlob, &tex.normal,   3},
        {"mix",      r.mixBlob, &tex.mix,      1},
        {"emissive", r.emiBlob, &tex.emissive, 5},
    };
    int listed = 0;
    for (const Chan& c : chans) {
        if (c.blob == (size_t)-1) continue;
        const QString nm = friendly(c.blob);
        QString label = QLatin1String(c.role);
        if (c.img && !c.img->isNull())
            label += QStringLiteral(" · %1×%2").arg(c.img->width()).arg(c.img->height());
        OutNode* tn = matRoot->add(
            new OutNode(AssetOutlinerModel::Texture, label, c.tile));
        // aux = friendly texture name (used by the export/preview menu + tooltip).
        tn->aux = nm.isEmpty() ? QString::fromLatin1(c.role) : nm;
        // Inline thumbnail (D4-style texture preview in the tree).
        if (c.img && !c.img->isNull())
            tn->icon = QPixmap::fromImage(c.img->scaled(
                24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        ++listed;
    }
    if (listed == 0)
        matRoot->add(new OutNode(AssetOutlinerModel::Texture,
                                 QStringLiteral("(no textures resolved)")));
}
} // namespace

// Build the loaded model's structure as an outliner subtree and inject it under
// its browse row. Clears the subtree when nothing is loaded — the flat browse
// list is what shows then, so the Outliner view is always usable as a list.
void ModelsTab::buildOutliner(bool reveal)
{
    if (!m_outlineModel) return;

    // Capture the current scroll offset + which top-level groups are open, so an
    // in-place rebuild (attachment toggle, part change) doesn't yank the view or
    // collapse what the user had expanded. Matched back by the group's first word.
    const int savedScroll =
        m_outlineView ? m_outlineView->verticalScrollBar()->value() : 0;
    QSet<QString> wasOpen;
    if (m_outlineView) {
        const QModelIndex oldHost = m_outlineModel->hostIndex();
        if (oldHost.isValid())
            for (int g = 0; g < m_outlineModel->rowCount(oldHost); ++g) {
                const QModelIndex gi = m_outlineModel->index(g, 0, oldHost);
                if (m_outlineView->isExpanded(gi))
                    wasOpen.insert(gi.data(Qt::DisplayRole).toString().section(
                        QLatin1Char(' '), 0, 0));
            }
    }

    // Per-kind "show" gates from the View ▸ Outliner menu (default all on).
    QSettings oset;
    auto showKind = [&oset](const char* k) {
        return oset.value(QStringLiteral("models/outliner/") + QLatin1String(k), true)
            .toBool();
    };
    const bool hasSkel = m_lastHier && !m_lastHier->empty() && showKind("show_skeleton");
    const bool hasParts = m_lastMesh && !m_lastMesh->submeshes.empty()
                          && showKind("show_parts");
    const bool showMats = showKind("show_materials");
    // Available attachments = every sibling the ATTACHMENTS panel lists, whether
    // or not it is currently loaded (the panel is populated before this runs).
    const bool hasAttach = m_attachList && m_attachList->count() > 0
                           && showKind("show_attachments");
    const bool hasAnims = m_anims && !m_anims->empty() && showKind("show_animations");
    const bool hasMats = showMats && m_lastMesh;   // material node needs a loaded model
    if (!(hasSkel || hasParts || hasMats || hasAttach || hasAnims) || m_curRepoIdx < 0) {
        m_outlineModel->clearSubtree();
        return;
    }

    // entryId keys the host row so the subtree survives filter/sort. Look it up
    // from the primary model's repo index via the current browse rows.
    quint32 hostEntry = 0;
    bool haveEntry = false;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        if (const AssetRow* e = m_model->rowAt(r))
            if (e->repoIdx == m_curRepoIdx) { hostEntry = e->entryId; haveEntry = true; break; }
    }
    if (!haveEntry) { m_outlineModel->clearSubtree(); return; }

    auto* root = new OutNode(AssetOutlinerModel::Group, QString());

    // Skeleton bone hierarchy (from the .skeleton tree).
    if (hasSkel) {
        std::unordered_map<std::string, std::vector<std::string>> kids;
        std::vector<std::string> roots;
        for (const auto& kv : *m_lastHier) {
            if (kv.second.empty() || !m_lastHier->count(kv.second))
                roots.push_back(kv.first);          // top-level or orphaned parent
            else
                kids[kv.second].push_back(kv.first);
        }
        std::sort(roots.begin(), roots.end());
        OutNode* skelRoot = root->add(new OutNode(
            AssetOutlinerModel::Group,
            QStringLiteral("Skeleton \u2014 %1 bones").arg(m_lastHier->size())));
        for (const std::string& r : roots) addBoneNode(skelRoot, r, kids, 0);
    }

    // Parts (submeshes) — each a checkable eye mirroring the parts panel.
    if (hasParts) {
        OutNode* partsRoot = root->add(new OutNode(
            AssetOutlinerModel::Group,
            QStringLiteral("Parts \u2014 %1").arg(m_lastMesh->submeshes.size())));
        // Eyes only when the parts panel actually drives a mask (>1 submesh);
        // a single-submesh model lists its one part for info, no toggle.
        const bool eyes = m_partsList && m_partsList->topLevelItemCount() ==
                          (int)m_lastMesh->submeshes.size();
        for (size_t i = 0; i < m_lastMesh->submeshes.size(); ++i) {
            OutNode* pn = partsRoot->add(new OutNode(
                AssetOutlinerModel::Part,
                QStringLiteral("part %1 \u00B7 %L2 tris")
                    .arg(i).arg(m_lastMesh->submeshes[i].indexCount / 3),
                (int)i));
            pn->checkable = eyes;
            pn->check = (eyes && m_partsList->topLevelItem((int)i)->checkState(0) == Qt::Checked)
                            || !eyes
                        ? Qt::Checked : Qt::Unchecked;
        }
    }

    // Material + textures of the primary model (names resolved from the repo).
    if (showMats && m_idx && m_idx->store && m_curRepoIdx >= 0)
        addMaterialNodes(root, *m_idx->store, m_curRepoIdx, m_lastTextures);

    // Attachments \u2014 mirrors the ATTACHMENTS panel: every available sibling is
    // listed; a loaded one expands into its own Parts + Materials, an unloaded one
    // is marked "(available)" so you can see what can be attached before loading.
    if (hasAttach) {
        OutNode* aRoot = root->add(new OutNode(
            AssetOutlinerModel::Group,
            QStringLiteral("Attachments \u2014 %1").arg(m_attachList->count())));
        for (int li = 0; li < m_attachList->count(); ++li) {
            QListWidgetItem* item = m_attachList->item(li);
            const bool loaded = item->checkState() == Qt::Checked;
            const int32_t repoIdx = item->data(Qt::UserRole).toInt();
            // ref = repo index so the eye (checkable) can load/unload the sibling
            // and the context menu can export it; check mirrors the ATTACHMENTS list.
            OutNode* an = aRoot->add(new OutNode(
                AssetOutlinerModel::Attach, item->text(), (int)repoIdx));
            an->aux = item->text();
            an->checkable = true;
            an->check = loaded ? Qt::Checked : Qt::Unchecked;
            if (!loaded) continue;
            // Find the loaded copy and expand its structure.
            for (const Attachment& a : m_attach) {
                if (a.name_repoIdx != repoIdx) continue;
                if (a.mesh && !a.mesh->submeshes.empty()) {
                    OutNode* pr = an->add(new OutNode(
                        AssetOutlinerModel::Group,
                        QStringLiteral("Parts \u2014 %1").arg(a.mesh->submeshes.size())));
                    const QSet<int>& hidden = m_attachPartHidden.value(repoIdx);
                    for (size_t i = 0; i < a.mesh->submeshes.size(); ++i) {
                        // ref = submesh, sub = attachment repo index \u2192 the eye
                        // toggles that submesh of the attachment's ViewPart, and
                        // right-click exports it. exportOn drives the arrow.
                        OutNode* pn = pr->add(new OutNode(
                            AssetOutlinerModel::Part,
                            QStringLiteral("part %1 \u00b7 %L2 tris")
                                .arg(i).arg(a.mesh->submeshes[i].indexCount / 3),
                            (int)i));
                        pn->sub = (int)repoIdx;
                        pn->checkable = true;
                        pn->check = hidden.contains((int)i) ? Qt::Unchecked
                                                            : Qt::Checked;
                    }
                }
                if (m_idx && m_idx->store && a.name_repoIdx >= 0)
                    addMaterialNodes(an, *m_idx->store, a.name_repoIdx, a.textures);
                break;
            }
        }
    }

    // Animations (double-click a leaf to play it; aux carries the clip name).
    if (hasAnims) {
        OutNode* animRoot = root->add(new OutNode(
            AssetOutlinerModel::Group,
            QStringLiteral("Animations \u2014 %1").arg(m_anims->size())));
        for (const AnimRef& a : *m_anims) {
            OutNode* an = animRoot->add(new OutNode(AssetOutlinerModel::Anim, a.display));
            an->aux = a.display;
        }
    }

    m_outlineModel->setSubtree(hostEntry, root);

    // Restore the tree's on-screen state. On a reveal (initial load / view
    // switch) expand the useful groups fresh and recenter; otherwise re-open
    // exactly what was open and pin the scroll offset so nothing jumps.
    if (m_outlineView) {
        const QModelIndex host = m_outlineModel->hostIndex();
        if (host.isValid()) {
            m_outlineView->expand(host);
            QSettings os;
            const bool autoExpand =
                os.value(QStringLiteral("models/outliner/autoExpand"), true).toBool();
            const bool rememberOpen =
                os.value(QStringLiteral("models/outliner/rememberOpen"), false).toBool();
            const QStringList remembered =
                os.value(QStringLiteral("models/outliner/openGroups")).toStringList();
            const int groups = m_outlineModel->rowCount(host);
            for (int g = 0; g < groups; ++g) {
                const QModelIndex gi = m_outlineModel->index(g, 0, host);
                const QString label = gi.data(Qt::DisplayRole).toString();
                const QString word  = label.section(QLatin1Char(' '), 0, 0);
                bool open;
                if (!reveal)               // in-place: keep exactly what was open
                    open = wasOpen.contains(word);
                else if (rememberOpen)     // carry the last model's open groups over
                    open = remembered.contains(word);
                else                       // fresh: useful groups open, Skeleton folded
                    open = autoExpand && !label.startsWith(QStringLiteral("Skeleton"));
                m_outlineView->setExpanded(gi, open);
            }
            if (reveal &&
                m_browseStack && m_browseStack->currentWidget() == m_outlineView)
                m_outlineView->scrollTo(host, QAbstractItemView::PositionAtCenter);
        }
        if (!reveal)
            m_outlineView->verticalScrollBar()->setValue(savedScroll);
    }
}

// Push the parts-panel eye states into the outliner (All/None/Invert etc.),
// keeping the two visibility UIs in lockstep without a feedback loop.
void ModelsTab::syncOutlinerParts()
{
    if (!m_outlineModel || !m_partsList) return;
    for (int i = 0; i < m_partsList->topLevelItemCount(); ++i)
        m_outlineModel->setPartCheck(
            i, m_partsList->topLevelItem(i)->checkState(0) == Qt::Checked);
}

// Repaint every browse view that shares the model so a newly cached thumbnail
// (texture or 3D) shows up wherever it is on screen.
void ModelsTab::repaintBrowseViews()
{
    if (m_table)       m_table->viewport()->update();
    if (m_gridView)    m_gridView->viewport()->update();
    if (m_outlineView) m_outlineView->viewport()->update();
}

// Push the current icon edge (m_thumbPx) into all three browse views. The grid
// shows the model larger (2x) with room for a wrapped label; list + outliner
// track the row height so async thumbnails never clip (the outliner pins its
// row height in the model so uniformRowHeights measures it correctly upfront).
void ModelsTab::applyThumbSize()
{
    const int s = m_thumbPx;
    if (m_table) {
        m_table->setIconSize(QSize(s, s));
        m_table->verticalHeader()->setDefaultSectionSize(s + 4);
    }
    if (m_gridView) {
        const int g = s * 2;
        m_gridView->setIconSize(QSize(g, g));
        // No fixed gridSize: the GridTileDelegate's sizeHint drives a compact,
        // uniform cell that tracks the icon size (a fixed gridSize plus long
        // path labels was what spread the grid to 3 sparse columns).
        m_gridView->setGridSize(QSize());
    }
    if (m_outlineView) {
        m_outlineView->setIconSize(QSize(s, s));
        if (m_outlineModel) m_outlineModel->setRowHeight(s + 4);
    }
}

// Ctrl+wheel zoom: step the icon edge and persist it.
void ModelsTab::bumpThumbSize(int notches)
{
    const int next = qBound(24, m_thumbPx + notches * 8, 160);
    if (next == m_thumbPx) return;
    m_thumbPx = next;
    QSettings().setValue(QStringLiteral("models/view/thumbPx"), m_thumbPx);
    applyThumbSize();
    emit statusText(QStringLiteral("Icon size: %1 px").arg(m_thumbPx));
}

// Rows the user has selected, working for BOTH browse views.
//
// selectedRows() requires every column of a row to be selected. The grid is a
// QListView showing one column (setModelColumn) that shares the table's
// selection model, so a grid selection never satisfies that and selectedRows()
// came back empty — which made "export selection" silently fall through to
// exporting whichever model happened to be loaded. Deduping selectedIndexes()
// by row answers correctly whichever view made the selection.
QModelIndexList ModelsTab::selectedRowIndexes() const
{
    QModelIndexList out;
    if (!m_table || !m_table->selectionModel()) return out;
    QSet<int> seen;
    QList<int> rows;
    for (const QModelIndex& ix : m_table->selectionModel()->selectedIndexes()) {
        if (!ix.isValid() || seen.contains(ix.row())) continue;
        seen.insert(ix.row());
        rows << ix.row();
    }
    std::sort(rows.begin(), rows.end());
    for (int r : rows) out << m_table->model()->index(r, 0);
    return out;
}

void ModelsTab::applyPartsMask()
{
    if (!m_lastMesh) return;
    const int n = m_partsList->topLevelItemCount();
    std::vector<uint8_t> mask((size_t)n, 1);
    int shown = 0;
    for (int i = 0; i < n; ++i) {
        const bool on = m_partsList->topLevelItem(i)->checkState(0) == Qt::Checked;
        mask[(size_t)i] = on ? 1 : 0;
        if (on) ++shown;
    }
    m_view->setPartSubmeshMask(0, std::move(mask));
    syncOutlinerParts();   // keep the outliner eyes in lockstep (All/None/Invert)
    emit statusText(QStringLiteral("%1 of %2 parts shown").arg(shown).arg(n));
}

// ── Right-column panel management (D4 strip + splitter) ────────────────────
QLabel* ModelsTab::addRightPage(const QString& title, QWidget* content)
{
    if (!m_rstack || !m_rstripLay) return nullptr;
    const int page = (int)m_rsections.size();
    m_panelIndex.insert(title, page);   // title -> page, for ensurePanelVisible
    auto* box = new PanelBox(title, content, m_rstack);
    box->hide();                        // up only when its strip toggle says so
    m_rstack->addWidget(box);
    m_rsections.push_back(box);
    // Key off the REGISTRATION title (its first word): the live label later
    // grows counts ("PARTS · 3 of 5 shown"), so a write-time key would drift.
    m_sectKeys << (QStringLiteral("models/panel/") +
                   title.section(QLatin1Char(' '), 0, 0));

    auto* b = new QToolButton(m_rstripLay->parentWidget());
    b->setIcon(QIcon(panelGlyph(title)));   // distinct glyph; tooltip carries the name
    b->setIconSize(QSize(18, 18));
    b->setCheckable(true);              // checked = panel up
    b->setToolTip(title.section(QLatin1Char(' '), 0, 0));
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedSize(20, 20);
    b->setStyleSheet(QStringLiteral(
        "QToolButton{border:1px solid transparent;border-radius:3px;"
        "background:transparent;}"
        "QToolButton:hover{border-color:#5a5a5a;}"
        "QToolButton:checked{background:#37476b;border-color:#4a5f8a;}"));
    connect(b, &QToolButton::toggled, this,
            [this, page](bool on) { showPanel(page, on); });
    connect(box->up, &QToolButton::clicked, this,
            [this, page] { movePanel(page, -1); });
    connect(box->down, &QToolButton::clicked, this,
            [this, page] { movePanel(page, +1); });
    connect(box->close, &QToolButton::clicked, this, [this, page] {
        if (page < (int)m_rpageBtns.size()) m_rpageBtns[(size_t)page]->setChecked(false);
    });
    int at = m_rstripLay->count();      // insert before the trailing stretch
    if (at > 0 && m_rstripLay->itemAt(at - 1)->spacerItem()) --at;
    m_rstripLay->insertWidget(at, b);
    m_rpageBtns.push_back(b);
    return box->label;
}

void ModelsTab::showPanel(int page, bool on)
{
    if (page < 0 || page >= (int)m_rsections.size()) return;
    PanelBox* box = m_rsections[(size_t)page];
    const bool was = !box->isHidden();
    box->setVisible(on);
    if (on && !was && !m_panelRestore) panelBoxArrive(m_rstack, box);
    savePanelLayout();
}

void ModelsTab::movePanel(int page, int delta)
{
    if (!m_rstack || page < 0 || page >= (int)m_rsections.size()) return;
    PanelBox* box = m_rsections[(size_t)page];
    std::vector<int> vis;   // splitter indices of the up panels, in order
    for (int i = 0; i < m_rstack->count(); ++i)
        if (!m_rstack->widget(i)->isHidden()) vis.push_back(i);
    const int myIdx = m_rstack->indexOf(box);
    int cur = -1;
    for (size_t k = 0; k < vis.size(); ++k)
        if (vis[k] == myIdx) { cur = (int)k; break; }
    const int tgt = cur + delta;
    if (cur < 0 || tgt < 0 || tgt >= (int)vis.size()) return;   // already at an end
    const QList<int> sizes = m_rstack->sizes();
    m_rstack->insertWidget(vis[(size_t)tgt], box);   // moves the existing child
    m_rstack->setSizes(sizes);                       // insertWidget resets sizes
    savePanelLayout();
}

// Persist which panels are up, in visible order (names, not a positional blob —
// hidden panels still occupy splitter slots, so an index would drift).
void ModelsTab::savePanelLayout()
{
    if (!m_rstack || m_panelRestore) return;
    QStringList shown;
    for (int i = 0; i < m_rstack->count(); ++i) {
        QWidget* w = m_rstack->widget(i);
        if (w->isHidden()) continue;
        for (size_t k = 0; k < m_rsections.size(); ++k)
            if (m_rsections[k] == w) {
                shown << m_sectKeys.value((int)k).section(QLatin1Char('/'), -1);
                break;
            }
    }
    QSettings().setValue(QStringLiteral("models/panels/shown"), shown);
}

void ModelsTab::restorePanelLayout()
{
    QSettings s;
    m_panelRestore = true;
    if (!s.contains(QStringLiteral("models/panels/shown"))) {
        for (QToolButton* b : m_rpageBtns) b->setChecked(true);   // first run: all up
    } else {
        const QStringList shown =
            s.value(QStringLiteral("models/panels/shown")).toStringList();
        for (const QString& key : shown)
            for (size_t k = 0; k < m_rsections.size(); ++k)
                if (m_sectKeys.value((int)k).section(QLatin1Char('/'), -1) == key) {
                    m_rstack->addWidget(m_rsections[k]);   // re-append → saved order
                    m_rpageBtns[k]->setChecked(true);      // → showPanel shows it
                    break;
                }
    }
    m_panelRestore = false;
    savePanelLayout();
}

void ModelsTab::buildAttachList()
{
    m_attachSyncing = true;
    m_attachList->clear();
    std::vector<std::pair<std::string, int32_t>> sibs;
    if (m_idx && m_idx->store && m_curRepoIdx >= 0)
        sibs = di::findSiblingModels(*m_idx->store, m_curRepoIdx);
    const QHash<qint32, QString> mem = m_attachMemory.value(m_curRepoIdx);
    bool anyRestored = false;
    for (const auto& [name, repoIdx] : sibs) {
        auto* it = new QListWidgetItem(QString::fromStdString(name));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        const bool remembered = mem.contains(repoIdx);
        it->setCheckState(remembered ? Qt::Checked : Qt::Unchecked);
        it->setData(Qt::UserRole, repoIdx);
        m_attachList->addItem(it);
        anyRestored |= remembered;
    }
    m_attachBox->setVisible(!sibs.empty());
    m_attachSyncing = false;
    if (anyRestored) onAttachToggled();   // reload the remembered attachments
}

// Load a sibling model synchronously (small parts; a brief hitch is fine).
// Returns false and leaves `a` partial on failure.
bool ModelsTab::loadAttachmentModel(di::DiAssetStore* store, int32_t repoIdx,
                                    ModelsTab::Attachment& a)
{
    bool ok = false;
    seh::runGuarded("attach-load", [&] {
        const di::ResolvedModel res = di::resolveModelChain(*store, repoIdx);
        if (res.meshBlob == (size_t)-1) return;
        const std::vector<uint8_t> raw = store->mpk().readAsset(res.meshBlob);
        auto m = std::make_shared<di::MeshData>();
        std::string err;
        if (!di::parseMesh(raw.data(), raw.size(), m.get(), &err)) return;
        a.mesh = m;
        if (res.skinBlob != (size_t)-1) {
            const std::vector<uint8_t> sraw = store->mpk().readAsset(res.skinBlob);
            auto s = std::make_shared<di::SkinSkeleton>();
            std::string serr;
            if (di::parseSkinSkeleton(sraw.data(), sraw.size(), s.get(), &serr))
                a.skel = s;
        }
        const size_t skelFile = di::findSkeletonByConvention(*store, repoIdx);
        if (skelFile != (size_t)-1) {
            const std::vector<uint8_t> hraw = store->mpk().readAsset(skelFile);
            auto hp = std::make_shared<di::BoneParents>();
            auto l = std::make_shared<di::BoneLocals>();
            std::string herr;
            if (di::parseSkeletonHierarchy(hraw.data(), hraw.size(), hp.get(), &herr, l.get())) {
                a.locals = l;
                a.hier = hp;
            }
        }
        auto decodeTex = [&](size_t blob) -> QImage {
            if (blob == (size_t)-1) return {};
            const std::vector<uint8_t> traw = store->mpk().readAsset(blob);
            di::Texture2D tex;
            std::string terr;
            if (!di::isTexture2D(traw.data(), traw.size()) ||
                !di::parseTexture2D(traw.data(), traw.size(), &tex, &terr))
                return {};
            return TextureDecode::decode(tex).image;
        };
        a.textures.diffuse  = decodeTex(res.texBlob);
        a.textures.normal   = decodeTex(res.nrmBlob);
        a.textures.mix      = decodeTex(res.mixBlob);
        a.textures.emissive = decodeTex(res.emiBlob);
        a.textures.fx       = res.matFx;   // animated emissive layers
        a.anims = std::make_shared<std::vector<AnimRef>>();
        for (const auto& [stem, id] : di::findFolderAnims(*store, repoIdx))
            a.anims->push_back({QString::fromStdString(stem), (quint32)id});
        ok = true;
    });
    return ok;
}

void ModelsTab::onAttachToggled()
{
    if (!m_idx || !m_idx->store) return;
    // Rebuild m_attach from the currently-checked rows. Reuse already-loaded
    // groups (keyed by repoIdx) so a toggle elsewhere does not reload them.
    std::vector<Attachment> next;
    for (int i = 0; i < m_attachList->count(); ++i) {
        const QListWidgetItem* it = m_attachList->item(i);
        if (it->checkState() != Qt::Checked) continue;
        const int32_t repoIdx = it->data(Qt::UserRole).toInt();
        // move an existing loaded group over, if present
        bool reused = false;
        for (auto& a : m_attach) {
            if (a.name_repoIdx == repoIdx && a.mesh) {
                next.push_back(std::move(a));
                reused = true;
                break;
            }
        }
        if (reused) continue;
        Attachment a;
        a.name_repoIdx = repoIdx;
        a.name = it->text();
        a.overrideClip = m_attachMemory.value(m_curRepoIdx).value(repoIdx);
        if (loadAttachmentModel(m_idx->store.get(), repoIdx, a) && a.mesh)
            next.push_back(std::move(a));
        else
            emit statusText(QStringLiteral("attachment failed to load: %1").arg(it->text()));
    }
    m_attach = std::move(next);
    // Remember this model's attachment set (+ any per-part clip overrides).
    if (m_curRepoIdx >= 0) {
        QHash<qint32, QString> mem;
        for (const Attachment& a : m_attach) mem.insert(a.name_repoIdx, a.overrideClip);
        if (mem.isEmpty()) m_attachMemory.remove(m_curRepoIdx);
        else m_attachMemory.insert(m_curRepoIdx, mem);
    }
    rebuildParts();        // create the GL parts first (indices must exist)
    syncAttachAnims();     // then load each group's matching clip + pose
    if (m_player.valid()) animTick(m_animT);   // show the poses immediately
    buildOutliner();       // reflect load/unload in the tree (eyes + expansions)
    emit statusText(QStringLiteral("%1 attachment(s) shown").arg(m_attach.size()));
}

void ModelsTab::showAttachMenu(const QPoint& pos)
{
    QListWidgetItem* it = m_attachList->itemAt(pos);
    if (!it) return;
    const qint32 repoIdx = (qint32)it->data(Qt::UserRole).toInt();
    Attachment* a = nullptr;
    for (Attachment& x : m_attach)
        if (x.name_repoIdx == repoIdx) { a = &x; break; }
    QMenu menu(this);
    if (!a) {   // not currently shown — offer to add it
        menu.addAction(QStringLiteral("Show this attachment"), this,
                       [it] { it->setCheckState(Qt::Checked); });
        menu.exec(m_attachList->viewport()->mapToGlobal(pos));
        return;
    }
    // Set an override (or clear it) and re-sync this part immediately.
    auto apply = [this](qint32 rid, const QString& clip) {
        for (Attachment& x : m_attach)
            if (x.name_repoIdx == rid) { x.overrideClip = clip; break; }
        if (m_curRepoIdx >= 0 && m_attachMemory.contains(m_curRepoIdx))
            m_attachMemory[m_curRepoIdx].insert(rid, clip);
        syncAttachAnims();
        if (m_player.valid()) animTick(m_animT);
        emit statusText(clip.isEmpty()
                            ? QStringLiteral("attachment follows the main animation")
                            : QStringLiteral("attachment plays \"%1\"").arg(clip));
    };
    QAction* def = menu.addAction(QStringLiteral("Match main animation"));
    def->setCheckable(true);
    def->setChecked(a->overrideClip.isEmpty());
    connect(def, &QAction::triggered, this, [apply, repoIdx] { apply(repoIdx, QString()); });
    if (a->anims && !a->anims->empty()) {
        menu.addSeparator();
        for (const AnimRef& r : *a->anims) {
            QAction* act = menu.addAction(r.display);
            act->setCheckable(true);
            act->setChecked(a->overrideClip == r.display);
            const QString nm = r.display;
            connect(act, &QAction::triggered, this, [apply, repoIdx, nm] { apply(repoIdx, nm); });
        }
    }
    menu.exec(m_attachList->viewport()->mapToGlobal(pos));
}

void ModelsTab::rebuildParts()
{
    std::vector<ViewPart> parts;
    parts.push_back({m_lastMesh, m_lastTextures});     // primary = part 0
    for (const Attachment& a : m_attach)
        parts.push_back({a.mesh, a.textures});
    m_view->setParts(std::move(parts));
    applyPartsMask();                                  // re-apply primary mask
    for (const Attachment& a : m_attach)               // re-apply attachment masks
        if (m_attachPartHidden.contains(a.name_repoIdx))
            applyAttachMask(a.name_repoIdx);
    if (m_player.valid())
        animTick(m_animT);                             // re-push all poses
    else                                               // restore bind overlay
        m_view->setSkeleton(std::vector<float>(m_bindSegs),
                            std::vector<float>(m_bindJoints));
}

std::shared_ptr<di::AnimClip> ModelsTab::maybeCloth(
    const std::shared_ptr<di::AnimClip>& clip, const di::SkinSkeleton* skel,
    const di::BoneParents* hier, const di::BoneLocals* locals) const
{
    return cloth::maybeBake(clip, skel, hier, locals,
                            m_clothBox && m_clothBox->isChecked());
}

void ModelsTab::showClothDialog()
{
    // Turning cloth on isn't implied by tuning it, but if it's already on the
    // new numbers must take effect now — re-bake and re-tick.
    cloth::showTuningDialog(this, [this] {
        if (m_clothBox && m_clothBox->isChecked()) onClothToggled();
    });
}

// The Settings dialog writes export/cloth directly, and its Cancel and Restore
// Defaults paths rewrite it too. This tab caches that key in a checkbox, so
// without a re-read the popup would still show "Cloth physics" ticked while
// ExportSettings::clothPhysics() answered false — the viewport posing one way
// and the exporter the other. Called after the dialog closes.
void ModelsTab::syncExportSettings()
{
    if (!m_clothBox) return;
    const bool want = ExportSettings::clothPhysics();
    if (m_clothBox->isChecked() == want) return;
    QSignalBlocker block(m_clothBox);   // re-bake once below, not twice
    m_clothBox->setChecked(want);
    onClothToggled();
}

void ModelsTab::onClothToggled()
{
    // Re-bake the primary clip in place and re-init its player, then let the
    // attachments follow. A running clip keeps its playhead; a paused one is
    // re-evaluated at the current time so the change is visible immediately.
    if (m_clip) {
        m_playClip = maybeCloth(m_clip, m_lastSkel.get(), m_lastHier.get(),
                                m_lastLocals.get());
        m_player.init(m_playClip.get(), m_lastSkel.get(), m_lastLocals.get(),
                      m_lastHier.get());
    }
    // Force attachments to re-load/re-bake their current clip.
    for (Attachment& a : m_attach) { a.clipName.clear(); }
    syncAttachAnims();
    if (m_player.valid() || !m_attach.empty()) animTick(m_animT);
}

void ModelsTab::syncAttachAnims()
{
    if (!m_idx || !m_idx->store) return;
    for (size_t k = 0; k < m_attach.size(); ++k) {
        Attachment& a = m_attach[k];
        // A per-part override wins; otherwise the attachment follows the primary.
        const QString target = a.overrideClip.isEmpty() ? m_curAnimName : a.overrideClip;
        auto toBind = [&] {                            // fall back to rest pose
            a.player.init(nullptr, nullptr);
            a.clip.reset();
            a.playClip.reset();
            a.clipName.clear();
            m_view->setPartPose(k + 1, {});            // empty pose -> bind mesh
        };
        if (target.isEmpty()) { toBind(); continue; }  // primary bind, no override
        if (a.clipName == target && a.player.valid()) continue;
        quint32 id = 0;
        bool found = false;
        if (a.anims)
            for (const AnimRef& r : *a.anims)
                if (r.display == target) { id = r.entryId; found = true; break; }
        std::shared_ptr<di::AnimClip> clip;
        if (found) {
            clip = std::make_shared<di::AnimClip>();
            bool ok = false;
            seh::runGuarded("attach-clip", [&] {
                const std::vector<uint8_t> raw =
                    m_idx->store->mpk().readAsset((size_t)id);
                std::string aerr;
                ok = !raw.empty() &&
                     di::parseAnim(raw.data(), raw.size(), clip.get(), &aerr);
            });
            if (!ok) clip.reset();
        }
        if (!clip) { toBind(); continue; }             // this part lacks that clip
        a.clip = clip;
        a.playClip = maybeCloth(a.clip, a.skel.get(), a.hier.get(), a.locals.get());
        a.player.init(a.playClip.get(), a.skel.get(), a.locals.get(), a.hier.get());
        a.clipName = target;
        // If the primary isn't driving the clock, still show this part's pose
        // (e.g. an override on a part while the main model sits at bind).
        if (!m_player.valid() && a.player.valid()) {
            std::vector<float> m2, s2, j2;
            a.player.evaluate(m_animT, &m2, &s2, &j2);
            m_view->setPartPose(k + 1, std::move(m2));
        }
    }
}

// The clip scrubber handed to ExportCapture. Frames are the transport's own
// unit (30 fps blocks, kFrameMs each), so frame N here is exactly the frame N
// the spinner shows — a GIF and the on-screen playhead cannot disagree.
ExportCapture::AnimSource ModelsTab::captureAnim()
{
    ExportCapture::AnimSource src;
    if (!m_player.valid()) return src;
    const float dur = m_player.durationMs();
    const int n = std::max(1, (int)(dur / kFrameMs));
    src.frameCount = n;
    src.fps        = 1000.0f / kFrameMs;
    src.savedFrame = qBound(0, (int)(m_animT / kFrameMs), n - 1);
    src.seek = [this, dur](int f) {
        animPose(qBound(0.0f, (float)f * kFrameMs, dur));
    };
    return src;
}

QString ModelsTab::captureBaseName() const
{
    return m_lastName.isEmpty() ? QStringLiteral("viewport")
                                : exportBaseName(m_lastName);
}

void ModelsTab::saveViewImage()
{
    if (!m_lastMesh) { addInfoNote(QStringLiteral("load a model first")); return; }
    QString msg;
    ExportCapture::runSaveImage(this, m_view, captureBaseName(), &msg);
    if (!msg.isEmpty()) emit statusText(msg);
}

void ModelsTab::exportTurntableGif()
{
    if (!m_lastMesh) { addInfoNote(QStringLiteral("load a model first")); return; }
    // The idle spin would fight the capture's own yaw stepping, and a running
    // playback timer would move the pose between grabs. Both are suspended for
    // the duration and put back exactly as they were.
    const bool wasSpinning = m_turntable && m_turntable->isChecked();
    if (wasSpinning) m_turntable->setChecked(false);
    const bool wasPlaying = m_animTimer->isActive();
    if (wasPlaying) m_animTimer->stop();

    QString msg;
    ExportCapture::runTurntableGif(this, m_view, captureBaseName(), captureAnim(),
                                  &msg);

    if (m_player.valid()) animTick(m_animT);   // resync the transport to the playhead
    if (wasPlaying) m_animTimer->start();
    if (wasSpinning && m_turntable) m_turntable->setChecked(true);
    if (!msg.isEmpty()) emit statusText(msg);
}

void ModelsTab::exportAnimGif()
{
    if (!m_lastMesh || !m_player.valid()) {
        addInfoNote(QStringLiteral("load and play an animation first"));
        return;
    }
    const bool wasSpinning = m_turntable && m_turntable->isChecked();
    if (wasSpinning) m_turntable->setChecked(false);
    const bool wasPlaying = m_animTimer->isActive();
    if (wasPlaying) m_animTimer->stop();

    QString msg;
    ExportCapture::runAnimLoopGif(this, m_view, captureBaseName(), captureAnim(),
                                 &msg);

    animTick(m_animT);
    if (wasPlaying) m_animTimer->start();
    if (wasSpinning && m_turntable) m_turntable->setChecked(true);
    if (!msg.isEmpty()) emit statusText(msg);
}

QList<int> ModelsTab::menuTargetRows(int clickedRow) const
{
    QList<int> sel;
    const QModelIndexList rows = selectedRowIndexes();
    for (const QModelIndex& mi : rows) sel.append(mi.row());
    std::sort(sel.begin(), sel.end());
    if (clickedRow >= 0 && !sel.contains(clickedRow))
        return {clickedRow};   // clicked row wins unless it's in the selection
    if (!sel.isEmpty()) return sel;
    if (clickedRow >= 0) return {clickedRow};
    return {};
}

void ModelsTab::showListMenu(const QPoint& pos, QAbstractItemView* view)
{
    if (!m_idx) return;
    if (!view) view = m_table;   // the list is the default source
    QModelIndex idx = view->indexAt(pos);
    // In the outliner a right-click can land on a child node (bone/part/anim);
    // walk up to its top-level browse row so the menu targets the model, and
    // never a stray row the child index happens to alias.
    int clickedRow = -1;
    if (view == m_outlineView) {
        while (idx.isValid() && idx.parent().isValid()) idx = idx.parent();
        clickedRow = idx.isValid() ? idx.row() : -1;
    } else {
        clickedRow = idx.isValid() ? idx.row() : -1;
    }
    const QList<int> rows = menuTargetRows(clickedRow);
    if (rows.isEmpty()) return;

    QList<Job> jobs;
    QStringList names, meanings, mpkPaths;
    for (int r : rows) {
        if (const AssetRow* row = m_model->rowAt(r)) {
            jobs.append({row->repoIdx, row->display});
            names << row->display;
            if (!row->meaning.isEmpty()) meanings << row->meaning;
            if (!row->mpkName.isEmpty()) mpkPaths << row->mpkName;
        }
    }
    if (jobs.isEmpty()) return;
    const int n = jobs.size();
    const QString what = n == 1 ? QStringLiteral("1 model")
                                : QStringLiteral("%1 models").arg(n);

    QMenu menu(this);
    QSettings s;
    const QString last = s.value(QLatin1String(kExportDirKey)).toString();
    if (!last.isEmpty() && QDir(last).exists() && !m_batchRunning)
        menu.addAction(MenuText::exportSetLast(what, MenuText::condensePath(last)),
                       this, [this, jobs, last] { exportBatch(jobs, last); });
    if (!m_batchRunning)
        menu.addAction(MenuText::exportSetPrompt(what), this, [this, jobs, what] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Export %1 to…").arg(what),
                QSettings().value(QLatin1String(kExportDirKey)).toString());
            if (!dir.isEmpty()) exportBatch(jobs, dir);
        });

    // The LOADED model exports with its viewport parts state; the batch above
    // always parses fresh. Offer both only when they'd differ.
    if (n == 1 && m_lastMesh && m_lastName == jobs[0].name) {
        menu.addSeparator();
        const int tris = (int)(m_lastMesh->indices.size() / 3);
        // menu.exec spins an event loop — a debounced load can swap m_lastMesh
        // underneath it, so re-check at click time.
        const QString nm = jobs[0].name;
        if (!last.isEmpty() && QDir(last).exists())
            menu.addAction(MenuText::withValue(MenuText::kExportModelLast,
                                               MenuText::condensePath(last)),
                           this, [this, nm] {
                               if (m_lastMesh && m_lastName == nm)
                                   exportCurrentGlb(true);
                           });
        menu.addAction(MenuText::prompts(
                           MenuText::withCount(MenuText::kExportModel, tris)),
                       this, [this, nm] {
                           if (m_lastMesh && m_lastName == nm)
                               exportCurrentGlb(false);
                       });
    }

    // Viewport actions on the currently-loaded model — GIF and Save image live
    // in the menus (Export + right-click), never as toolbar buttons (D4 rule).
    if (m_lastMesh) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("Save preview image…"), this,
                       [this] { saveViewImage(); });
        menu.addAction(QStringLiteral("Turntable GIF…"), this,
                       [this] { exportTurntableGif(); });
        if (m_player.valid())
            menu.addAction(QStringLiteral("Animation loop GIF…"), this,
                           [this] { exportAnimGif(); });
    }

    menu.addSeparator();
    const QString multi = QStringLiteral(" — %1 rows").arg(n);
    // Copy the animation currently playing on the loaded model.
    if (n == 1 && m_lastName == jobs[0].name && !m_curAnimName.isEmpty()) {
        const QString clip = m_curAnimName;
        menu.addAction(QStringLiteral("Copy animation name  \"%1\"").arg(clip),
                       this, [clip] { QGuiApplication::clipboard()->setText(clip); });
    }
    menu.addAction(n == 1 ? MenuText::withValue(MenuText::kCopyName, names[0])
                          : MenuText::kCopyName + multi,
                   this, [names] {
                       QGuiApplication::clipboard()->setText(
                           names.join(QLatin1Char('\n')));
                   });
    if (!meanings.isEmpty())
        menu.addAction(n == 1 ? MenuText::withValue(MenuText::kCopyMeaning,
                                                    meanings[0])
                              : MenuText::kCopyMeaning + multi,
                       this, [meanings] {
                           QGuiApplication::clipboard()->setText(
                               meanings.join(QLatin1Char('\n')));
                       });
    if (!mpkPaths.isEmpty())
        menu.addAction(n == 1 ? MenuText::withValue(MenuText::kCopyMpkPath,
                                                    mpkPaths[0])
                              : MenuText::kCopyMpkPath + multi,
                       this, [mpkPaths] {
                           QGuiApplication::clipboard()->setText(
                               mpkPaths.join(QLatin1Char('\n')));
                       });
    menu.exec(view->viewport()->mapToGlobal(pos));
}

void ModelsTab::exportCurrentGlb(bool toLastDir)
{
    if (!m_lastMesh) return;
    const int generation = m_generation.load();

    // EVERY member read happens HERE, before the save dialog. The dialog spins a
    // nested event loop, and the queued meshReady handler fires inside it — so a
    // snapshot taken afterwards could describe a different model than the
    // filename the dialog was seeded with. (It also makes the swap-and-restore
    // callers below safe: their window no longer spans an event loop.)
    GlbExporter::Part part;
    part.mesh     = m_lastMesh;
    // The parts panel hides submeshes in the viewport; unless the user asked for
    // the whole model, the file should match what they were looking at. The
    // menu has always claimed this ("exports with its viewport parts state") —
    // it just never did it.
    if (!QSettings().value(QStringLiteral("export/hiddenParts"), false).toBool() &&
        m_partsList && m_lastMesh &&
        m_partsList->topLevelItemCount() == (int)m_lastMesh->submeshes.size()) {
        std::vector<uint8_t> keep((size_t)m_partsList->topLevelItemCount(), 1);
        for (size_t i = 0; i < keep.size(); ++i)
            keep[i] = m_partsList->topLevelItem((int)i)->checkState(0) == Qt::Checked;
        if (auto vis = extractVisibleSubmeshes(*m_lastMesh, keep)) part.mesh = vis;
    }
    part.skel     = m_lastSkel;
    part.textures = m_lastTextures;
    part.name     = m_lastName;
    auto hier   = m_lastHier;
    auto locals = m_lastLocals;
    const QString name = m_lastName;
    // Settings ▸ Export ▸ Animations to embed. The Wardrobe moved to this
    // months-equivalent-ago; the Models tab was still on the old three-way enum,
    // so one question had two answers depending on which tab you started from.
    const AnimExportScope scope = AnimExportScope::load();
    // NOT m_animBox->currentText(): that combo is editable and doubles as a
    // search box, so its text is whatever the user last typed. m_curAnimName
    // is set from the clip that actually loaded.
    const QString curClipName = m_curAnimName;
    auto primaryClip = scope.includeAnim && scope.previewed ? m_clip : nullptr;
    // `original` = the clips found next to this model. `base` has no meaning for
    // a standalone model (there is no class rig to inherit from), so it is not
    // consulted here — it is a Wardrobe source.
    AnimListPtr primaryAnims =
        (scope.includeAnim && scope.original) ? m_anims : nullptr;

    // Attachments ride along as extra parts, each with its own skeleton + clip,
    // when enabled. Snapshot them for the worker.
    struct ExAtt {
        GlbExporter::Part part;
        std::shared_ptr<di::BoneParents> hier;
        std::shared_ptr<di::BoneLocals>  locals;
        std::shared_ptr<di::AnimClip>    curClip;   // its current matching clip
        AnimListPtr                      anims;      // for AnimAll
    };
    std::vector<ExAtt> exAtt;
    if (ExportSettings::includeAttachments())
        for (const Attachment& a : m_attach) {
            if (!a.mesh) continue;
            ExAtt e;
            e.part.mesh = a.mesh; e.part.skel = a.skel;
            e.part.textures = a.textures; e.part.name = a.name;
            e.hier = a.hier; e.locals = a.locals;
            e.curClip = a.clip; e.anims = a.anims;
            exAtt.push_back(std::move(e));
        }
    auto store = m_idx ? m_idx->store : nullptr;
    // Read the GUI toggle + params here (never from the worker thread) and carry
    // them in, so the .glb bakes with exactly the tuning the viewport shows.
    const bool clothOn = m_clothBox && m_clothBox->isChecked();
    const di::ClothParams clothP = cloth::paramsFromSettings();

    // The snapshot above is complete, so the dialog can safely run now.
    QString base = exportBaseName(name);
    if (part.mesh && part.mesh->skinned) base += QStringLiteral("_rigged");
    QSettings s;
    const QString lastDir = s.value(QLatin1String(kExportDirKey)).toString();
    QString dest;
    if (toLastDir && !lastDir.isEmpty() && QDir(lastDir).exists()) {
        dest = QDir(lastDir).filePath(base + QStringLiteral(".glb"));
    } else {
        dest = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export rigged .glb"),
            lastDir.isEmpty() ? base + QStringLiteral(".glb")
                              : QDir(lastDir).filePath(base + QStringLiteral(".glb")),
            QStringLiteral("glTF binary (*.glb)"));
        if (dest.isEmpty()) return;
    }
    s.setValue(QLatin1String(kExportDirKey), QFileInfo(dest).absolutePath());

    QThread* worker = QThread::create(
        [this, generation, part, hier, locals, name, dest, scope, curClipName,
         primaryClip, primaryAnims, exAtt, store, clothOn, clothP]() mutable {
        seh::installSehTranslator();
        QString msg;
        seh::runGuarded("glb-export", [&] {
            QElapsedTimer timer;
            timer.start();
            // Merge the primary + attachment skeletons into one node tree (bone
            // names are unique across body/parts, so a plain union is correct).
            auto mHier   = std::make_shared<di::BoneParents>();
            auto mLocals = std::make_shared<di::BoneLocals>();
            if (hier)   *mHier   = *hier;
            if (locals) *mLocals = *locals;
            std::vector<GlbExporter::Part> parts;
            parts.push_back(part);
            for (const ExAtt& e : exAtt) {
                parts.push_back(e.part);
                if (e.hier)   for (const auto& kv : *e.hier)   mHier->emplace(kv.first, kv.second);
                if (e.locals) for (const auto& kv : *e.locals) mLocals->emplace(kv.first, kv.second);
            }
            auto loadClip = [&](quint32 id) -> std::shared_ptr<di::AnimClip> {
                if (!store) return nullptr;
                const std::vector<uint8_t> raw = store->mpk().readAsset((size_t)id);
                auto c = std::make_shared<di::AnimClip>();
                std::string e;
                return (!raw.empty() && di::parseAnim(raw.data(), raw.size(), c.get(), &e))
                           ? c : nullptr;
            };
            // Track indices and duplicate bones both need handling — see
            // di::mergeClipTracks. A plain vector append re-parents every
            // attachment bone and emits duplicate glTF channels.
            auto append = [](di::AnimClip& dst, const di::AnimClip& src) {
                di::mergeClipTracks(dst, src);
            };
            // Bake soft-body physics into a clip for one part (matches exactly
            // what the viewport plays). Returns the input unchanged when off or
            // when the part has no cloth bones.
            auto bakePart = [&](std::shared_ptr<di::AnimClip> c,
                                const std::shared_ptr<const di::SkinSkeleton>& sk,
                                const di::BoneParents* h, const di::BoneLocals* l)
                -> std::shared_ptr<di::AnimClip> {
                if (!clothOn || !c || !sk || !h || !l) return c;
                if (!di::hasClothBones(*sk)) return c;
                return di::bakeCloth(*c, *sk, *h, *l, clothP);
            };
            // Build the animation list, merging each attachment's same-named clip
            // into the primary's so one glTF animation drives every part.
            std::vector<GlbExporter::AnimExport> anims;
            if (primaryClip) {
                auto pb = bakePart(primaryClip, part.skel, hier.get(), locals.get());
                auto merged = std::make_shared<di::AnimClip>(*pb);
                for (const ExAtt& e : exAtt)
                    if (auto ab = bakePart(e.curClip, e.part.skel, e.hier.get(), e.locals.get()))
                        append(*merged, *ab);
                anims.push_back({curClipName, merged});
            }
            if (primaryAnims) {
                int failed = 0, filtered = 0;
                for (const AnimRef& ref : *primaryAnims) {
                    // The previewed clip is already in, and an explicit pick is
                    // never dropped by the filters.
                    if (!anims.empty() && ref.display == curClipName) continue;
                    if (!scope.accepts(ref.display, -1)) { ++filtered; continue; }
                    auto pc = loadClip(ref.entryId);
                    if (!pc) { ++failed; continue; }
                    auto pb = bakePart(pc, part.skel, hier.get(), locals.get());
                    auto merged = std::make_shared<di::AnimClip>(*pb);
                    for (const ExAtt& e : exAtt) {
                        if (!e.anims) continue;
                        for (const AnimRef& er : *e.anims)
                            if (er.display == ref.display) {
                                if (auto ac = loadClip(er.entryId))
                                    if (auto ab = bakePart(ac, e.part.skel, e.hier.get(), e.locals.get()))
                                        append(*merged, *ab);
                                break;
                            }
                    }
                    anims.push_back({ref.display, merged});
                }
                qInfo("glb: clips embedded %zu (%s), %d unreadable, %d filtered, "
                      "%lld ms",
                      anims.size(), qPrintable(scope.describe()), failed, filtered,
                      (long long)timer.elapsed());
            }
            QString gerr;
            if (GlbExporter::writeGlb(dest, parts, mHier.get(), mLocals.get(),
                                      anims, name, &gerr)) {
                msg = QStringLiteral("exported %1 (%2 part%3, %4, %5 ms)")
                          .arg(dest)
                          .arg(parts.size())
                          .arg(parts.size() == 1 ? QString() : QStringLiteral("s"))
                          .arg(anims.empty()
                                   ? QStringLiteral("no anims")
                                   : QStringLiteral("%1 anims").arg(anims.size()))
                          .arg(timer.elapsed());
            } else {
                msg = QStringLiteral("glb export FAILED: %1").arg(gerr);
            }
        });
        if (msg.isEmpty()) msg = QStringLiteral("glb export crashed (see log)");
        emit exportDone(generation, msg);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

// Export a single part (submesh) as its own .glb. Reuses the whole
// exportCurrentGlb pipeline by presenting the extracted sub-mesh as the current
// model for the duration of the (synchronous) snapshot: exportCurrentGlb copies
// m_lastMesh/m_lastName into the worker payload before the thread starts, so
// restoring them immediately after is safe. Attachments are forced off — a lone
// part shouldn't drag the rest of the outfit along.
void ModelsTab::exportPartGlb(int submesh)
{
    if (!m_lastMesh || submesh < 0 ||
        submesh >= (int)m_lastMesh->submeshes.size())
        return;
    auto sub = extractSubmesh(*m_lastMesh, (size_t)submesh);
    if (!sub || sub->indices.empty()) return;

    auto       savedMesh = m_lastMesh;
    const QString savedName = m_lastName;
    const bool savedAtt  = ExportSettings::includeAttachments();
    m_lastMesh = sub;
    m_lastName = savedName + QStringLiteral("_part%1").arg(submesh);
    ExportSettings::setIncludeAttachments(false);
    exportCurrentGlb(false);          // builds its worker payload synchronously
    ExportSettings::setIncludeAttachments(savedAtt);
    m_lastMesh = savedMesh;
    m_lastName = savedName;
}

// Export one loaded attachment as its own .glb. Same trick as exportPartGlb:
// present the attachment's mesh/skeleton/textures as the current model for the
// synchronous snapshot exportCurrentGlb takes, then restore.
void ModelsTab::exportAttachmentGlb(int repoIdx)
{
    const Attachment* found = nullptr;
    for (const Attachment& a : m_attach)
        if (a.name_repoIdx == repoIdx && a.mesh) { found = &a; break; }
    if (!found) return;

    // Swap the attachment in as "the model", export, swap back. Safe now that
    // exportCurrentGlb takes its whole snapshot before opening any dialog — the
    // window below no longer spans an event loop.
    //
    // The CLIP state is swapped too. Without it the attachment was exported with
    // the PRIMARY's clips: in AnimAll that meant the body's clip ids (which name
    // no bone on this part, so every one dropped), and in AnimCurrent it baked
    // the body's clip against the attachment's skeleton. Its own per-part
    // override was silently lost.
    auto sMesh = m_lastMesh; auto sSkel = m_lastSkel; auto sHier = m_lastHier;
    auto sLoc = m_lastLocals; auto sTex = m_lastTextures;
    const QString sName = m_lastName;
    auto sClip = m_clip; auto sAnims = m_anims;
    const QString sClipName = m_curAnimName;
    const bool sAtt = ExportSettings::includeAttachments();
    m_lastMesh = found->mesh; m_lastSkel = found->skel; m_lastHier = found->hier;
    m_lastLocals = found->locals; m_lastTextures = found->textures;
    m_lastName = found->name;
    m_clip  = found->clip;      // the clip this attachment is actually playing
    m_anims = found->anims;     // its OWN clip list, for AnimAll
    if (!found->overrideClip.isEmpty()) m_curAnimName = found->overrideClip;
    ExportSettings::setIncludeAttachments(false);
    exportCurrentGlb(false);
    ExportSettings::setIncludeAttachments(sAtt);
    m_lastMesh = sMesh; m_lastSkel = sSkel; m_lastHier = sHier;
    m_lastLocals = sLoc; m_lastTextures = sTex; m_lastName = sName;
    m_clip = sClip; m_anims = sAnims; m_curAnimName = sClipName;
}

// Push one attachment's submesh-visibility mask (from m_attachPartHidden) to its
// ViewPart. The ViewPart index is 1 + the attachment's position in m_attach
// (part 0 is the primary model), matching rebuildParts' ordering.
void ModelsTab::applyAttachMask(int repoIdx)
{
    int viewPart = -1;
    const di::MeshData* mesh = nullptr;
    for (size_t k = 0; k < m_attach.size(); ++k)
        if (m_attach[k].name_repoIdx == repoIdx && m_attach[k].mesh) {
            viewPart = 1 + (int)k;
            mesh = m_attach[k].mesh.get();
            break;
        }
    if (viewPart < 0 || !mesh) return;
    const QSet<int>& hidden = m_attachPartHidden.value(repoIdx);
    std::vector<uint8_t> mask(mesh->submeshes.size(), 1);
    for (size_t i = 0; i < mask.size(); ++i)
        mask[i] = hidden.contains((int)i) ? 0 : 1;
    m_view->setPartSubmeshMask((size_t)viewPart, std::move(mask));
}

// Export one submesh of a loaded attachment as its own .glb.
void ModelsTab::exportAttachPartGlb(int repoIdx, int submesh)
{
    const Attachment* found = nullptr;
    for (const Attachment& a : m_attach)
        if (a.name_repoIdx == repoIdx && a.mesh) { found = &a; break; }
    if (!found || submesh < 0 || submesh >= (int)found->mesh->submeshes.size())
        return;
    auto sub = extractSubmesh(*found->mesh, (size_t)submesh);
    if (!sub || sub->indices.empty()) return;

    auto sMesh = m_lastMesh; auto sSkel = m_lastSkel; auto sHier = m_lastHier;
    auto sLoc = m_lastLocals; auto sTex = m_lastTextures;
    const QString sName = m_lastName;
    const bool sAtt = ExportSettings::includeAttachments();
    m_lastMesh = sub; m_lastSkel = found->skel; m_lastHier = found->hier;
    m_lastLocals = found->locals; m_lastTextures = found->textures;
    m_lastName = found->name + QStringLiteral("_part%1").arg(submesh);
    ExportSettings::setIncludeAttachments(false);
    exportCurrentGlb(false);
    ExportSettings::setIncludeAttachments(sAtt);
    m_lastMesh = sMesh; m_lastSkel = sSkel; m_lastHier = sHier;
    m_lastLocals = sLoc; m_lastTextures = sTex; m_lastName = sName;
}

// Outliner right-click on an ATTACHMENT PART node — hide/show + export it.
void ModelsTab::showOutlinerAttachPartMenu(int repoIdx, int submesh, const QPoint& gp)
{
    QMenu menu(this);
    const bool hidden = m_attachPartHidden.value(repoIdx).contains(submesh);
    menu.addAction(hidden ? QStringLiteral("Show this part")
                          : QStringLiteral("Hide this part"),
                   this, [this, repoIdx, submesh, hidden] {
        QSet<int>& h = m_attachPartHidden[repoIdx];
        if (hidden) h.remove(submesh); else h.insert(submesh);
        applyAttachMask(repoIdx);
        buildOutliner();   // reflect the eye state (in-place, keeps scroll)
    });
    menu.addAction(QStringLiteral("Export this part…"), this,
                   [this, repoIdx, submesh] { exportAttachPartGlb(repoIdx, submesh); });
    menu.exec(gp);
}

// Outliner right-click on a PART node — the same toggle/frame/export actions as
// the PARTS panel, driven through the shared m_partsList + applyPartsMask path.
void ModelsTab::showOutlinerPartMenu(int submesh, const QPoint& gp)
{
    if (!m_partsList || submesh < 0 ||
        submesh >= m_partsList->topLevelItemCount())
        return;
    QMenu menu(this);
    QTreeWidgetItem* item = m_partsList->topLevelItem(submesh);
    const bool vis = item->checkState(0) == Qt::Checked;
    menu.addAction(QStringLiteral("Solo this part"), this, [this, submesh] {
        m_partsSyncing = true;
        for (int i = 0; i < m_partsList->topLevelItemCount(); ++i)
            m_partsList->topLevelItem(i)->setCheckState(
                0, i == submesh ? Qt::Checked : Qt::Unchecked);
        m_partsSyncing = false;
        applyPartsMask();
    });
    menu.addAction(vis ? QStringLiteral("Hide this part")
                       : QStringLiteral("Show this part"),
                   this, [this, submesh, vis] {
        m_partsList->topLevelItem(submesh)->setCheckState(
            0, vis ? Qt::Unchecked : Qt::Checked);
        applyPartsMask();
    });
    menu.addAction(QStringLiteral("Frame this part"), this,
                   [this, submesh] { m_view->frameToSubmesh(0, (size_t)submesh); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Export this part…"), this,
                   [this, submesh] { exportPartGlb(submesh); });
    menu.exec(gp);
}

// Toggle one attachment by repo index (drives the ATTACHMENTS list checkbox,
// which loads/unloads through the one existing path).
void ModelsTab::setAttachmentLoaded(int repoIdx, bool on)
{
    if (!m_attachList) return;
    for (int i = 0; i < m_attachList->count(); ++i) {
        QListWidgetItem* it = m_attachList->item(i);
        if (it->data(Qt::UserRole).toInt() != repoIdx) continue;
        if ((it->checkState() == Qt::Checked) != on)
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        break;
    }
}

// Enable / disable every attachment at once (one reload, not one-per-row).
void ModelsTab::setAllAttachments(bool on)
{
    if (!m_attachList) return;
    m_attachSyncing = true;
    for (int i = 0; i < m_attachList->count(); ++i)
        m_attachList->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    m_attachSyncing = false;
    onAttachToggled();
}

// Outliner right-click on an ATTACHMENT node — load/unload it, export it, or
// enable/disable the whole set.
void ModelsTab::showOutlinerAttachMenu(int repoIdx, bool loaded, const QPoint& gp)
{
    QMenu menu(this);
    menu.addAction(loaded ? QStringLiteral("Unload attachment")
                          : QStringLiteral("Load attachment"),
                   this, [this, repoIdx, loaded] {
        setAttachmentLoaded(repoIdx, !loaded);
    });
    QAction* exp = menu.addAction(QStringLiteral("Export attachment…"), this,
                                  [this, repoIdx] { exportAttachmentGlb(repoIdx); });
    exp->setEnabled(loaded);   // only a loaded attachment has a mesh to write
    menu.addSeparator();
    menu.addAction(QStringLiteral("Enable all attachments"), this,
                   [this] { setAllAttachments(true); });
    menu.addAction(QStringLiteral("Disable all attachments"), this,
                   [this] { setAllAttachments(false); });
    menu.exec(gp);
}

// Batch .glb export: every job parses FRESH on the worker (resolve -> mesh ->
// skeleton -> hierarchy -> textures -> write), one SEH guard per item so a
// malformed model costs one file, not the run (D4 bulk lesson). Failure
// reasons are recorded verbatim and land in <dir>/_batch_failed.txt.
void ModelsTab::exportBatch(const QList<Job>& jobs, const QString& dir)
{
    if (!m_idx || !m_idx->store || m_batchRunning || jobs.isEmpty()) return;
    QSettings().setValue(QLatin1String(kExportDirKey), dir);
    m_batchRunning = true;
    const int generation = m_batchGen.load();
    const AnimExportScope scope = AnimExportScope::load();
    auto store = m_idx->store;
    // Cloth read on the GUI thread, exactly as the single-model path does. The
    // batch used to bake nothing at all, so a 40-model export came out with
    // every cape frozen at bind pose while the same model exported one at a
    // time came out simulated.
    const bool clothOn = m_clothBox && m_clothBox->isChecked();
    const di::ClothParams clothP = cloth::paramsFromSettings();
    QThread* worker = QThread::create([this, generation, store, jobs, dir, scope,
                                       clothOn, clothP] {
        seh::installSehTranslator();
        int written = 0;
        QStringList failures;
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < jobs.size(); ++i) {
            if (generation != m_batchGen.load()) {
                qInfo("glb batch: abandoned after %d files (index reload)", written);
                break;
            }
            const Job& j = jobs[i];
            QString why;
            seh::HardwareFault fault;
            const bool guarded = seh::runGuarded("batch-glb", [&] {
                const di::ResolvedModel res =
                    di::resolveModelChain(*store, j.repoIdx);
                if (res.meshBlob == (size_t)-1) {
                    why = QStringLiteral("no Mesh dependency resolved (%1)")
                              .arg(res.note);
                    return;
                }
                const std::vector<uint8_t> raw = store->mpk().readAsset(res.meshBlob);
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
                    if (di::parseSkinSkeleton(sraw.data(), sraw.size(), sk.get(),
                                              &serr))
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
                    const std::vector<uint8_t> traw = store->mpk().readAsset(blob);
                    di::Texture2D tex;
                    std::string terr;
                    if (!di::isTexture2D(traw.data(), traw.size()) ||
                        !di::parseTexture2D(traw.data(), traw.size(), &tex, &terr))
                        return {};
                    return TextureDecode::decode(tex).image;
                };
                MeshTextures textures;
                textures.diffuse  = decodeTex(res.texBlob);
                textures.normal   = decodeTex(res.nrmBlob);
                textures.mix      = decodeTex(res.mixBlob);
                textures.emissive = decodeTex(res.emiBlob);

                std::vector<GlbExporter::AnimExport> anims;
                // A batch item that is not the loaded model has no "previewed"
                // clip, so `previewed` alone means "the first clip this model
                // owns" rather than nothing at all — silently writing a static
                // file when an animation source was ticked is the worse answer.
                const bool wantAll  = scope.includeAnim && scope.original;
                const bool wantOne  = scope.includeAnim && scope.previewed && !wantAll;
                if (skel && (wantAll || wantOne)) {
                    for (const auto& [stem, id] :
                         di::findFolderAnims(*store, j.repoIdx)) {
                        const std::vector<uint8_t> araw =
                            store->mpk().readAsset(id);
                        auto clip = std::make_shared<di::AnimClip>();
                        std::string aerr;
                        const QString nm = QString::fromStdString(stem);
                        if (!scope.accepts(nm, -1)) continue;
                        if (!araw.empty() &&
                            di::parseAnim(araw.data(), araw.size(), clip.get(),
                                          &aerr))
                            anims.push_back({nm, clip});
                        if (wantOne && !anims.empty()) break;   // one is enough
                    }
                }
                // Bake cloth per clip against this model's own skeleton, the
                // same way the single-model path does.
                if (clothOn && skel && hier && locals && di::hasClothBones(*skel))
                    for (GlbExporter::AnimExport& a : anims)
                        if (a.clip)
                            a.clip = di::bakeCloth(*a.clip, *skel, *hier, *locals,
                                                   clothP);

                GlbExporter::Part part;
                part.mesh     = mesh;
                part.skel     = skel;
                part.textures = textures;
                part.name     = j.name;
                QString base = flatExportName(j.name);
                if (mesh->skinned) base += QStringLiteral("_rigged");
                const QString dest =
                    QDir(dir).filePath(base + QStringLiteral(".glb"));
                QString gerr;
                if (GlbExporter::writeGlb(dest, {part}, hier.get(), locals.get(),
                                          anims, j.name, &gerr))
                    ++written;
                else
                    why = QStringLiteral("glb write failed: %1").arg(gerr);
            }, &fault);
            if (!guarded)
                why = QStringLiteral("CRASHED (%1) — skipped, run continues")
                          .arg(fault.what);
            if (!why.isEmpty()) {
                failures << QStringLiteral("%1 — %2").arg(j.name, why);
                qWarning("glb batch: %s FAILED: %s", qPrintable(j.name),
                         qPrintable(why));
            }
            emit batchProgress(generation,
                               QStringLiteral("exporting models… %1/%2 (%3 failed)")
                                   .arg(i + 1)
                                   .arg(jobs.size())
                                   .arg(failures.size()));
        }
        if (!failures.isEmpty()) {
            QFile f(QDir(dir).filePath(QStringLiteral("_batch_failed.txt")));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&f);
                for (const QString& line : failures) out << line << '\n';
            }
        }
        emit batchDone(generation,
                       QStringLiteral("exported %1 model(s) to %2 (%3 failed%4, "
                                      "%5 ms)")
                           .arg(written)
                           .arg(dir)
                           .arg(failures.size())
                           .arg(failures.isEmpty()
                                    ? QString()
                                    : QStringLiteral(" — see _batch_failed.txt"))
                           .arg(timer.elapsed()));
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

FilterSpec ModelsTab::currentSpec() const
{
    FilterSpec s = m_filter->spec();
    if (m_animFilter) s.anim = m_animFilter->currentData().toInt();
    if (m_whoFilter)  s.who  = m_whoFilter->currentData().toInt();
    return s;
}

// ── info panel ─────────────────────────────────────────────────────────────
// The panel is BASE + a bounded note list, never an ever-growing append. The
// old code did `setText(text() + …)`, so every clip that refused to parse left
// another line behind until the messages pushed the viewport off screen.

// Thin wrappers over the shared readout (util/InfoNotes.h) — the collapsing and
// bounding used to live here, and the Wardrobe tab had no equivalent at all.
void ModelsTab::setInfoBase(const QString& html) { m_infoText.setBase(html); }
void ModelsTab::addInfoNote(const QString& html) { m_infoText.addNote(html); }
void ModelsTab::renderInfo() {}   // the shared panel renders on every change

// Grey out a clip the parser refused, so the list shows at a glance which of
// this rig's clips are playable instead of making the user find out by clicking.
void ModelsTab::markClipUnsupported(const QString& name)
{
    if (!m_animBox) return;
    const int at = m_animBox->findText(name);
    if (at < 0) return;
    if (auto* m = qobject_cast<QStandardItemModel*>(m_animBox->model()))
        if (QStandardItem* it = m->item(at)) {
            if (!it->isEnabled()) return;
            it->setEnabled(false);
            it->setText(name + QStringLiteral("  (unsupported encoding)"));
        }
}

void ModelsTab::applyFilter()
{
    m_pendingRow = -1;   // a refilter remaps rows; a queued debounce would load
                         // whatever asset now sits at the old visible index
    m_model->setFilters(currentSpec());
    QSettings().setValue(QStringLiteral("models/animFilter"),
                         m_animFilter ? m_animFilter->currentIndex() : 0);
    QSettings().setValue(QStringLiteral("models/whoFilter"),
                         m_whoFilter ? m_whoFilter->currentIndex() : 0);
    emit statusText(QStringLiteral("%L1 model entries").arg(m_model->visibleCount()));
}

void ModelsTab::revealRepoIndex(int repoIdx)
{
    if (!m_model || repoIdx < 0) return;
    auto findRow = [this, repoIdx]() -> int {
        const int n = m_model->rowCount();
        for (int i = 0; i < n; ++i) {
            const AssetRow* r = m_model->rowAt(i);
            if (r && r->repoIdx == repoIdx) return i;
        }
        return -1;
    };
    int row = findRow();
    if (row < 0) {
        // The target is filtered out of the current view — drop every filter and
        // retry so a reveal always lands (block the combo signals so their
        // handlers don't each trigger a separate refilter).
        if (m_whoFilter) {
            m_whoFilter->blockSignals(true);
            m_whoFilter->setCurrentIndex(0);
            m_whoFilter->blockSignals(false);
        }
        if (m_animFilter) {
            m_animFilter->blockSignals(true);
            m_animFilter->setCurrentIndex(0);
            m_animFilter->blockSignals(false);
        }
        if (m_filter) m_filter->applySpec(FilterSpec());
        applyFilter();
        row = findRow();
    }
    if (row < 0) return;
    const QModelIndex idx = m_model->index(row, AssetListModel::ColName);
    if (m_table) {
        m_table->setCurrentIndex(idx);   // currentRowChanged -> debounced load
        m_table->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    }
    if (m_gridView && m_gridView->isVisible())
        m_gridView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
}

// ── ExportHooks: the adaptive Export menu + Ctrl+E family ──────────────────

bool ModelsTab::canExport() const
{
    if (m_batchRunning || !m_idx) return false;
    return m_table->selectionModel()->hasSelection() || (bool)m_lastMesh;
}

QString ModelsTab::exportWhat() const
{
    const int n = (int)selectedRowIndexes().size();
    if (n > 1) return QStringLiteral("%1 models").arg(n);
    return QStringLiteral("1 model");
}

void ModelsTab::exportNow(bool toLastDir)
{
    if (!canExport()) return;
    const QModelIndexList rows = selectedRowIndexes();
    // Single row that IS the loaded model exports through the viewport path
    // (respects the Parts state + anim settings); everything else is a fresh
    // batch parse — same split as the context menu.
    if (rows.size() <= 1) {
        const AssetRow* r =
            rows.isEmpty() ? nullptr : m_model->rowAt(rows.first().row());
        const QString rowName = r ? r->display : m_lastName;
        if (m_lastMesh && rowName == m_lastName) {
            exportCurrentGlb(toLastDir);
            return;
        }
    }
    QList<Job> jobs;
    for (const QModelIndex& mi : rows)
        if (const AssetRow* r = m_model->rowAt(mi.row()))
            jobs.append({r->repoIdx, r->display});
    if (jobs.isEmpty()) return;
    QSettings s;
    QString dir = toLastDir ? s.value(QLatin1String(kExportDirKey)).toString()
                            : QString();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export %1 to…").arg(exportWhat()),
            s.value(QLatin1String(kExportDirKey)).toString());
    }
    if (!dir.isEmpty()) exportBatch(jobs, dir);
}

QString ModelsTab::lastExportDir() const
{
    const QString d =
        QSettings().value(QLatin1String(kExportDirKey)).toString();
    return QDir(d).exists() ? d : QString();
}

bool ModelsTab::canSaveImage() const { return (bool)m_lastMesh; }

void ModelsTab::saveImageNow() { saveViewImage(); }

bool ModelsTab::canExportGif() const { return (bool)m_lastMesh; }
bool ModelsTab::canExportAnimGif() const { return m_player.valid(); }
void ModelsTab::exportGifTurntable() { exportTurntableGif(); }
void ModelsTab::exportGifAnim() { exportAnimGif(); }

bool ModelsTab::canExportAll() const
{
    return m_model && m_model->visibleCount() > 0;
}
int ModelsTab::exportAllCount() const
{
    return m_model ? m_model->visibleCount() : 0;
}
void ModelsTab::exportAllNow(bool toLastDir)
{
    if (!m_model) return;
    QList<Job> jobs;
    for (int i = 0; i < m_model->rowCount(); ++i)
        if (const AssetRow* r = m_model->rowAt(i))
            jobs.append({r->repoIdx, r->display});
    if (jobs.isEmpty()) return;
    QSettings s;
    QString dir = toLastDir ? s.value(QLatin1String(kExportDirKey)).toString()
                            : QString();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export all %1 matching to…").arg(jobs.size()),
            s.value(QLatin1String(kExportDirKey)).toString());
    if (!dir.isEmpty()) exportBatch(jobs, dir);
}

bool ModelsTab::canExportAnims() const
{
    return (bool)m_lastMesh && m_anims && !m_anims->empty();
}
void ModelsTab::exportAnimsNow()
{
    if (!m_lastMesh) return;
    // "Animations only" is an explicit request for clips, so it overrides the
    // configured sources — including the master switch — the same way D4's
    // Export ▸ Export animations only does. exportCurrentGlb reads the scope
    // synchronously (before any dialog), so the temporary override is safe.
    QSettings s;
    const QVariant pInc = s.value(QStringLiteral("export/includeAnim"));
    const QVariant pOrg = s.value(QStringLiteral("export/animOriginal"));
    s.setValue(QStringLiteral("export/includeAnim"), true);
    s.setValue(QStringLiteral("export/animOriginal"), true);
    exportCurrentGlb(false);
    if (pInc.isValid()) s.setValue(QStringLiteral("export/includeAnim"), pInc);
    else                s.remove(QStringLiteral("export/includeAnim"));
    if (pOrg.isValid()) s.setValue(QStringLiteral("export/animOriginal"), pOrg);
    else                s.remove(QStringLiteral("export/animOriginal"));
}

// Offered only when attachments are actually shown in the viewport.
bool ModelsTab::canExportWithAttachments() const
{
    return (bool)m_lastMesh && !m_attach.empty();
}
// Export the model + the attachments currently loaded, forcing them in even if
// the export/attachments setting is off. exportCurrentGlb snapshots the flag
// synchronously, so restoring it right after is safe.
void ModelsTab::exportWithAttachmentsNow(bool toLastDir)
{
    if (!m_lastMesh) return;
    const bool saved = ExportSettings::includeAttachments();
    ExportSettings::setIncludeAttachments(true);
    exportCurrentGlb(toLastDir);
    ExportSettings::setIncludeAttachments(saved);
}

void ModelsTab::startLoad(int modelRow)
{
    if (!m_idx) return;
    const AssetRow* r = m_model->rowAt(modelRow);
    if (!r) return;
    // Re-selecting the model that's already loaded (e.g. a right-click on it)
    // must NOT reload — that would rebuild and reset the outliner tree. It's
    // already on screen; leave the user's expansion/scroll as-is.
    if (r->repoIdx >= 0 && r->repoIdx == m_curRepoIdx && m_lastMesh) return;
    const AssetRow row = *r;   // copy: model may rebuild while the worker runs
    m_curRepoIdx = row.repoIdx;   // for the attachments panel (main thread)
    m_curRow     = row;           // for the INFO panel

    const int generation = ++m_generation;
    setInfoBase(QStringLiteral("<b>%1</b><br>loading…").arg(row.display.toHtmlEscaped()));
    m_loading->show();
    m_loading->raise();

    auto store = m_idx->store;
    QThread* worker = QThread::create([this, generation, store, row] {
        seh::installSehTranslator();
        std::shared_ptr<di::MeshData> mesh;
        std::shared_ptr<di::SkinSkeleton> skel;
        std::shared_ptr<di::BoneParents> hier;
        std::shared_ptr<di::BoneLocals> locals;
        MeshTextures textures;
        AnimListPtr anims;
        QString info;
        seh::runGuarded("model-load", [&] {
            QElapsedTimer timer;
            timer.start();
            const di::ResolvedModel res = di::resolveModelChain(*store, row.repoIdx);
            if (res.meshBlob == (size_t)-1) {
                info = QStringLiteral("<b>%1</b><br>no Mesh dependency resolved (%2)")
                           .arg(row.display.toHtmlEscaped(), res.note);
                return;
            }
            const std::vector<uint8_t> raw = store->mpk().readAsset(res.meshBlob);
            auto m = std::make_shared<di::MeshData>();
            std::string err;
            if (!di::parseMesh(raw.data(), raw.size(), m.get(), &err)) {
                info = QStringLiteral("<b>%1</b><br>mesh parse failed: %2")
                           .arg(row.display.toHtmlEscaped(), QString::fromStdString(err));
                qInfo("model: %s -> parse FAIL (%s)", qPrintable(row.display), err.c_str());
                return;
            }
            const qint64 meshMs = timer.elapsed();

            if (res.skinBlob != (size_t)-1) {
                const std::vector<uint8_t> sraw = store->mpk().readAsset(res.skinBlob);
                auto s = std::make_shared<di::SkinSkeleton>();
                std::string serr;
                if (di::parseSkinSkeleton(sraw.data(), sraw.size(), s.get(), &serr))
                    skel = s;
                else
                    qInfo("model: %s skeleton parse failed (%s)", qPrintable(row.display),
                          serr.c_str());
            }

            // Playable animations by folder convention (same walk-up) —
            // shared resolver, so the preview, this tab's batch export and
            // Bulk Extract can never drift apart.
            anims = std::make_shared<std::vector<AnimRef>>();
            for (const auto& [stem, id] : di::findFolderAnims(*store, row.repoIdx))
                anims->push_back({QString::fromStdString(stem), (quint32)id});

            // Authoritative bone hierarchy from the folder's .skeleton file.
            const size_t skelFile = di::findSkeletonByConvention(*store, row.repoIdx);
            if (skelFile != (size_t)-1) {
                const std::vector<uint8_t> hraw = store->mpk().readAsset(skelFile);
                auto h = std::make_shared<di::BoneParents>();
                auto l = std::make_shared<di::BoneLocals>();
                std::string herr;
                if (di::parseSkeletonHierarchy(hraw.data(), hraw.size(), h.get(),
                                               &herr, l.get())) {
                    hier   = h;
                    locals = l;
                    qInfo("model: %s hierarchy from %s (%zu nodes, %zu locals)",
                          qPrintable(row.display),
                          store->mpk().entries()[skelFile].name.c_str(),
                          h->size(), l->size());
                } else {
                    qWarning("model: %s .skeleton hierarchy parse failed (%s) - "
                             "falling back to Biped heuristics",
                             qPrintable(row.display), herr.c_str());
                }
            } else if (skel) {
                qInfo("model: %s no .skeleton file found by convention - "
                      "Biped heuristics only", qPrintable(row.display));
            }

            // Texture outcome must be honest in the UI: "untextured" (nothing
            // referenced) vs "texture FAILED" (referenced but undecodable) are
            // different bugs, and only the log told them apart before (audit).
            auto decodeTex = [&](size_t blob) -> QImage {
                if (blob == (size_t)-1) return {};
                const std::vector<uint8_t> traw = store->mpk().readAsset(blob);
                di::Texture2D tex;
                std::string terr;
                if (!di::isTexture2D(traw.data(), traw.size()) ||
                    !di::parseTexture2D(traw.data(), traw.size(), &tex, &terr))
                    return {};
                return TextureDecode::decode(tex).image;
            };
            QString texNote = QStringLiteral("untextured");
            if (res.texBlob != (size_t)-1) {
                textures.diffuse = decodeTex(res.texBlob);
                texNote = textures.diffuse.isNull()
                              ? QStringLiteral("diffuse FAILED to decode")
                              : res.note;
            }
            textures.normal   = decodeTex(res.nrmBlob);
            textures.mix      = decodeTex(res.mixBlob);
            textures.emissive = decodeTex(res.emiBlob);
            textures.fx       = res.matFx;   // animated emissive layers
            if (!textures.normal.isNull())   texNote += QStringLiteral(" +n");
            if (!textures.mix.isNull())      texNote += QStringLiteral(" +m");
            if (!textures.emissive.isNull()) texNote += QStringLiteral(" +e");
            if (!textures.emissive.isNull() && textures.fx.animated())
                texNote += QStringLiteral(" +fxAnim");
            mesh = m;
            QString streams;
            for (const std::string& s : m->streams)
                if (!s.empty() && s != "None")
                    streams += QString::fromLatin1(s.c_str()) + QLatin1Char(' ');
            info = QStringLiteral("<b>%1</b><br>%L2 verts · %L3 tris · %4 submeshes · "
                                  "%5 · streams %6· %7 · %8 ms")
                       .arg(row.display.toHtmlEscaped())
                       .arg(m->vertexCount())
                       .arg(m->indices.size() / 3)
                       .arg(m->submeshes.size())
                       .arg(m->skinned
                                ? (skel ? QStringLiteral("skinned, %1 bones")
                                              .arg(skel->bones.size())
                                        : QStringLiteral("skinned"))
                                : QStringLiteral("static"))
                       .arg(streams, texNote)
                       .arg(timer.elapsed());
            // Decoded true name (structural, or the real override if present):
            // shown prominently so the piece's identity reads at a glance.
            const QString trueName = NameTranslator::cosmeticName(row.display);
            if (!trueName.isEmpty())
                info = QStringLiteral("<b>%1</b><br>%2")
                           .arg(trueName.toHtmlEscaped(), info);
            // Permanent instrumentation: the log alone answers what loaded and how fast.
            qInfo("model: %s -> %zu verts %zu tris sub=%zu skinned=%d tex=%s%s%s%s mesh %lld ms total %lld ms",
                  qPrintable(row.display), m->vertexCount(), m->indices.size() / 3,
                  m->submeshes.size(), (int)m->skinned,
                  textures.diffuse.isNull() ? "none" : "ok",
                  textures.normal.isNull() ? "" : "+n",
                  textures.mix.isNull() ? "" : "+m",
                  textures.emissive.isNull() ? "" : "+e",
                  (long long)meshMs, (long long)timer.elapsed());
        });
        if (info.isEmpty())
            info = QStringLiteral("<b>%1</b><br>load crashed (see log)")
                       .arg(row.display.toHtmlEscaped());
        emit meshReady(generation, mesh, textures, info, skel, hier, locals,
                       row.display, anims);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}
