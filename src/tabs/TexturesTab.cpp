#include "tabs/TexturesTab.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

#include "app/SehGuard.h"
#include "index/AssetListModel.h"
#include "index/ThumbnailProvider.h"
#include "model/ModelResolve.h"
#include "store/Repository.h"
#include "tabs/PanelBox.h"
#include "tabs/ViewGlyphs.h"   // panelGlyph — strip icons
#include "tex/DiPixelFormat.h"
#include "tex/TextureDecode.h"
#include "util/CsvCopy.h"
#include "util/FilterBar.h"
#include "util/HintBar.h"
#include "util/HoverInfo.h"
#include "util/MenuText.h"
#include "util/PanelPersist.h"

namespace {

// Remembered folders — three distinct keys on purpose (D4 lesson): batch
// texture exports, atlas-frame exports and ad-hoc image saves go to different
// places in practice, and one shared key made each workflow stomp the others.
const char kLastDirKey[]   = "tex/lastDir";
const char kFramesDirKey[] = "tex/framesLastDir";
const char kImageDirKey[]  = "tex/imageDir";
const char kGridViewKey[]  = "tex/gridView";
const char kGridPxKey[]    = "tex/gridPx";

QString exportBaseName(QString name)
{
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) name = name.mid(slash + 1);
    name.replace(QLatin1Char('.'), QLatin1Char('_'));
    return name;
}

// Grayscale view of one channel (0=R,1=G,2=B,3=A) of an image, as D4 does for
// its per-channel preview tiles.
QImage channelGrey(const QImage& src, int ch)
{
    const QImage s = src.convertToFormat(QImage::Format_ARGB32);
    QImage out(s.width(), s.height(), QImage::Format_Grayscale8);
    for (int y = 0; y < s.height(); ++y) {
        const QRgb* in = reinterpret_cast<const QRgb*>(s.scanLine(y));
        uchar* o = out.scanLine(y);
        for (int x = 0; x < s.width(); ++x) {
            const QRgb px = in[x];
            o[x] = (uchar)(ch == 0 ? qRed(px)
                                   : ch == 1 ? qGreen(px)
                                             : ch == 2 ? qBlue(px) : qAlpha(px));
        }
    }
    return out;
}

// Batch outputs mangle the WHOLE display path into the file name — leaf names
// alone collide across folders (measured on the live repository), and a
// collision in a batch silently overwrites file A with file B while counting
// both as written.
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

QString sanitizeFileToken(QString s)
{
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9_.-]+"));
    s.replace(bad, QStringLiteral("_"));
    return s.isEmpty() ? QStringLiteral("frame") : s;
}

// Thumbnail-grid tiles: square icon + one elided caption line. The stock
// IconMode paint wraps long repository paths into multi-line soup.
class GridItemDelegate : public QStyledItemDelegate {
public:
    GridItemDelegate(int px, QObject* parent) : QStyledItemDelegate(parent), m_px(px) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return QSize(m_px + 12, m_px + 26);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override
    {
        p->save();
        if (opt.state & QStyle::State_Selected)
            p->fillRect(opt.rect, QColor(0x2d, 0x43, 0x5c));
        else if (opt.state & QStyle::State_MouseOver)
            p->fillRect(opt.rect, QColor(0x2c, 0x2c, 0x30));
        const QRect iconR(opt.rect.x() + 6, opt.rect.y() + 4, m_px, m_px);
        const QPixmap pm = index.data(Qt::DecorationRole).value<QPixmap>();
        if (pm.isNull()) {
            p->fillRect(iconR, QColor(0x22, 0x22, 0x22));   // decode pending
        } else {
            const QPixmap fit = pm.scaled(iconR.size(), Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
            p->drawPixmap(iconR.x() + (iconR.width() - fit.width()) / 2,
                          iconR.y() + (iconR.height() - fit.height()) / 2, fit);
        }
        QString caption = index.data(Qt::DisplayRole).toString();
        const int slash = caption.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0) caption = caption.mid(slash + 1);
        const QRect capR(opt.rect.x() + 2, iconR.bottom() + 2,
                         opt.rect.width() - 4, 16);
        p->setPen(opt.state & QStyle::State_Selected ? QColor(Qt::white)
                                                     : QColor(0xcc, 0xcc, 0xcc));
        p->drawText(capR, Qt::AlignHCenter | Qt::AlignTop,
                    opt.fontMetrics.elidedText(caption, Qt::ElideMiddle,
                                               capR.width()));
        p->restore();
    }

private:
    int m_px;
};

}  // namespace

TexturesTab::TexturesTab(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);

    if (QWidget* hint = makeHintBar(
            this,
            QStringLiteral("Tip: right-click a row for export and copy actions · "
                           "wheel zooms the preview, drag pans, double-click "
                           "refits · Ctrl+wheel resizes grid thumbnails"),
            "hints/textures"))
        lay->addWidget(hint);

    auto* split = new QSplitter(Qt::Horizontal, this);
    lay->addWidget(split, 1);

    // ── left: filter row + list/grid stack ─────────────────────────────────
    auto* leftW = new QWidget(split);
    auto* left  = new QVBoxLayout(leftW);
    left->setContentsMargins(0, 0, 0, 0);
    auto* filterRow = new QHBoxLayout();
    m_filter = new FilterBar(this, {}, QStringLiteral("Texture2D"));
    filterRow->addWidget(m_filter, 1);
    m_gridBtn = new QPushButton(QStringLiteral("▦ Grid"), this);   // D4 grid glyph
    m_gridBtn->setCheckable(true);
    m_gridBtn->setToolTip(QStringLiteral(
        "Toggle thumbnail grid view (Ctrl+wheel resizes the tiles)"));
    filterRow->addWidget(m_gridBtn);
    left->addLayout(filterRow);

    m_model = new AssetListModel(this);
    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(AssetListModel::ColName, Qt::AscendingOrder);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    // multi-select: Ctrl/Shift ranges feed "Export selected"; the preview
    // still follows the current (lead) row
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(
        ThumbnailProvider::kSize + 4);
    m_table->setIconSize(QSize(ThumbnailProvider::kSize, ThumbnailProvider::kSize));
    m_table->horizontalHeader()->setSectionResizeMode(AssetListModel::ColName,
                                                      QHeaderView::Stretch);
    m_table->setShowGrid(false);
    m_table->setColumnHidden(AssetListModel::ColType, true);   // all Texture2D here
    // clip counts are a Models-tab concern
    m_table->setColumnHidden(AssetListModel::ColClips, true);
    // Context menu + Ctrl+C CSV. Policy is set BEFORE CsvCopy::install so its
    // fallback menu stands down and ours wins (documented CsvCopy trap).
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this,
            [this](const QPoint& p) { showListMenu(m_table, p); });
    CsvCopy::install(m_table);

    // Thumbnail grid over the SAME model and selection — toggling views never
    // loses the current pick, and the debounced preview follows both.
    m_grid = new QListView(this);
    m_grid->setModel(m_model);
    m_grid->setModelColumn(AssetListModel::ColName);
    m_grid->setSelectionModel(m_table->selectionModel());
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setUniformItemSizes(true);
    m_grid->setWordWrap(false);
    m_grid->setSpacing(6);
    m_grid->setLayoutMode(QListView::SinglePass);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Vertical bar always on so usable width — and column count — stays
    // constant; items don't reflow when the bar appears (D4 lesson).
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    {
        QSettings s;
        m_gridPx = qBound(48, s.value(QLatin1String(kGridPxKey), 96).toInt(), 192);
    }
    m_grid->setIconSize(QSize(m_gridPx, m_gridPx));
    m_grid->setItemDelegate(new GridItemDelegate(m_gridPx, m_grid));
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_grid, &QListView::customContextMenuRequested, this,
            [this](const QPoint& p) { showListMenu(m_grid, p); });
    m_grid->viewport()->installEventFilter(this);   // Ctrl+wheel tile size

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_table);
    m_stack->addWidget(m_grid);
    left->addWidget(m_stack, 1);
    split->addWidget(leftW);

    connect(m_gridBtn, &QPushButton::toggled, this,
            [this](bool on) { setGridView(on); });

    // ── MIDDLE: info + preview controls + canvas + pixel inspector ─────────
    m_mainSplit = split;
    auto* rightW = new QWidget(split);          // (the MIDDLE column)
    auto* right  = new QVBoxLayout(rightW);
    right->setContentsMargins(0, 0, 0, 0);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    right->addWidget(m_info);

    auto* controls = new QHBoxLayout();
    m_chR = new QCheckBox(QStringLiteral("R"), this);
    m_chG = new QCheckBox(QStringLiteral("G"), this);
    m_chB = new QCheckBox(QStringLiteral("B"), this);
    m_chA = new QCheckBox(QStringLiteral("A"), this);
    for (QCheckBox* c : {m_chR, m_chG, m_chB, m_chA}) {
        c->setChecked(c != m_chA);   // RGB on, straight-alpha off by default
        controls->addWidget(c);
        connect(c, &QCheckBox::toggled, this, [this] { refreshView(); });
    }
    m_fit = new QCheckBox(QStringLiteral("Fit"), this);
    m_fit->setChecked(true);
    connect(m_fit, &QCheckBox::toggled, this, [this](bool on) {
        if (on) m_zoom = 1.0;
        refreshView();
    });
    controls->addWidget(m_fit);
    m_alphaBg = new QPushButton(QStringLiteral("Alpha BG"), this);
    m_alphaBg->setCheckable(true);
    m_alphaBg->setChecked(true);
    m_alphaBg->setToolTip(QStringLiteral(
        "Show a checkerboard behind transparent pixels (preview only — never "
        "baked into exports)"));
    connect(m_alphaBg, &QPushButton::toggled, this, [this] { refreshView(); });
    controls->addWidget(m_alphaBg);
    m_mipBox = new QComboBox(this);
    m_mipBox->setMinimumWidth(110);
    controls->addWidget(m_mipBox);
    connect(m_mipBox, &QComboBox::activated, this, [this](int i) {
        if (m_currentRowEntry >= 0)
            startDecode(-1, m_mipBox->itemData(i).toInt());
    });
    // Texture / selection / shown-view export live in the Export menu and the
    // right-click menu (D4 keeps export out of the toolbar) — no toolbar buttons.
    controls->addStretch(1);
    right->addLayout(controls);

    // ── atlas frames (hidden unless the texture has a plist descriptor) ────
    m_atlasBox = new QWidget(this);
    auto* abox = new QVBoxLayout(m_atlasBox);
    abox->setContentsMargins(0, 2, 0, 2);
    auto* ahdr = new QHBoxLayout();
    ahdr->addStretch(1);               // title comes from the TEXFRAMES PanelBox header
    m_trim = new QCheckBox(QStringLiteral("Trim"), this);
    m_trim->setToolTip(QStringLiteral(
        "Crop exported frames to their tight alpha bounds"));
    ahdr->addWidget(m_trim);
    m_exportFrames = new QPushButton(QStringLiteral("Export frames…"), this);
    m_exportFrames->setToolTip(QStringLiteral(
        "Write the selected frames (or all frames when none are selected) as "
        "PNGs into a folder"));
    connect(m_exportFrames, &QPushButton::clicked, this,
            [this] { exportFrames(false, false); });
    ahdr->addWidget(m_exportFrames);
    abox->addLayout(ahdr);
    m_frames = new QTreeWidget(this);
    m_frames->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Name"),
                               QStringLiteral("Size"), QStringLiteral("Rot")});
    m_frames->setRootIsDecorated(false);
    m_frames->setAlternatingRowColors(true);
    m_frames->setUniformRowHeights(true);
    m_frames->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_frames->setMaximumHeight(150);
    m_frames->setColumnWidth(0, 40);
    m_frames->setColumnWidth(1, 220);
    m_frames->setColumnWidth(2, 80);
    CsvCopy::install(m_frames);   // Ctrl+C + right-click Copy / Copy all
    connect(m_frames, &QTreeWidget::itemSelectionChanged, this, [this] {
        QTreeWidgetItem* cur = m_frames->currentItem();
        const bool has = cur && !m_frames->selectedItems().isEmpty();
        m_frameFocus = has ? m_frames->indexOfTopLevelItem(cur) : -1;
        refreshView();
    });
    abox->addWidget(m_frames);
    // (m_atlasBox is hosted by the TEXFRAMES PanelBox in the right column below,
    //  not added to the middle column.)
    for (QPushButton* b : {m_gridBtn, m_exportFrames})
        b->setStyleSheet(QLatin1String(kPushBtnQss));   // D4 button skin

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(false);
    m_canvas = new QLabel(m_scroll);
    m_canvas->setAlignment(Qt::AlignCenter);
    m_canvas->setMouseTracking(true);   // pixel inspector
    m_canvas->installEventFilter(this);
    m_scroll->setWidget(m_canvas);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->viewport()->installEventFilter(this);   // re-fit on resize
    right->addWidget(m_scroll, 1);

    // ── CHANNELS strip — the shared TEXTURE PREVIEW body (util/TexturePreview.h),
    //    the same one the Models and Wardrobe tabs use. It was written twice:
    //    this copy had a LUMA tile and a hover-zoom the shared one lacked, and
    //    the shared one had a pixel readout this copy lacked. The shared panel
    //    now carries all three, so every tab gained whatever it was missing and
    //    a fix to any of it lands everywhere at once.
    //
    //    The R/G/B/A toolbar toggles isolate channels in the MAIN preview; this
    //    strip always shows every channel at once.
    {
        auto* chLbl = new QLabel(QStringLiteral("CHANNELS"), this);
        chLbl->setStyleSheet(QStringLiteral("color:#888;font-size:10px;"));
        right->addWidget(chLbl);
        constexpr int kTile = 72;   // the only thing that differed from the others
        right->addWidget(m_texPrev.build(this, this, kTile));
        for (int i = 0; i < texprev::Panel::kTiles; ++i) {
            QLabel* t = m_texPrev.tile[i];
            if (!t) continue;
            connect(t, &QWidget::customContextMenuRequested, this,
                    [this, i](const QPoint& p) {
                        if (m_texPrev.full[i].isNull() || !m_texPrev.tile[i]) return;
                        QMenu menu(this);
                        menu.addAction(MenuText::kCopyImage, this, [this, i] {
                            QApplication::clipboard()->setImage(m_texPrev.full[i]);
                        });
                        menu.addAction(MenuText::kSaveImage, this, [this, i] {
                            const QString f = QFileDialog::getSaveFileName(
                                this, QStringLiteral("Save channel"),
                                QStringLiteral("%1.png")
                                    .arg(QString::fromLatin1(
                                             m_texPrev.channelName(i)).toLower()),
                                QStringLiteral("PNG (*.png)"));
                            if (!f.isEmpty()) m_texPrev.full[i].save(f);
                        });
                        menu.exec(m_texPrev.tile[i]->mapToGlobal(p));
                    });
        }
    }

    m_pixel = new QLabel(this);
    m_pixel->setStyleSheet(QStringLiteral(
        "color:#9aa; font-family:Consolas,'Courier New',monospace; "
        "font-size:11px;"));
    m_pixel->setText(QStringLiteral(" "));
    right->addWidget(m_pixel);

    split->addWidget(rightW);   // middle column

    // ── RIGHT: D4-style panel strip + PanelBox splitter ────────────────────
    // A toggle strip drives ASSOCIATED MODELS and MIPMAPS; TEXFRAMES stays
    // data-driven (it exists only for atlas textures) as the top splitter child.
    auto* farW = new QWidget(split);
    auto* far  = new QHBoxLayout(farW);
    far->setContentsMargins(0, 0, 0, 0);
    far->setSpacing(3);
    auto* stripW = new QWidget(farW);
    stripW->setFixedWidth(26);
    stripW->setStyleSheet(QStringLiteral("background:#232323;border-radius:4px;"));
    m_rstripLay = new QVBoxLayout(stripW);
    m_rstripLay->setContentsMargins(3, 4, 3, 4);
    m_rstripLay->setSpacing(4);
    m_rstripLay->addStretch(1);
    far->addWidget(stripW);
    m_rstack = new QSplitter(Qt::Vertical, farW);
    m_rstack->setChildrenCollapsible(false);
    m_rstack->setHandleWidth(4);
    far->addWidget(m_rstack, 1);
    split->addWidget(farW);

    // TEXFRAMES — data-driven (shown only when the texture has an atlas plist).
    m_framesPanel = new PanelBox(QStringLiteral("TEXFRAMES"), m_atlasBox, m_rstack);
    connect(m_framesPanel->close, &QToolButton::clicked, m_framesPanel,
            [this] { m_framesPanel->setVisible(false); });
    m_rstack->addWidget(m_framesPanel);
    m_framesPanel->setVisible(false);

    // ASSOCIATED MODELS — the models that reference the shown texture (built by
    // inverting the whole repository's Model->Material->texture chain).
    m_assocList = new QListWidget(m_rstack);
    m_assocList->setAlternatingRowColors(true);
    m_assocList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_assocList->setContextMenuPolicy(Qt::CustomContextMenu);
    CsvCopy::install(m_assocList);   // Ctrl+C + right-click Copy / Copy all
    connect(m_assocList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QListWidgetItem* it = m_assocList->itemAt(pos);
                if (!it) return;
                const QString name = it->text();
                const int repoIdx = it->data(Qt::UserRole).toInt();
                QMenu menu(this);
                menu.addAction(QStringLiteral("Reveal model in Models tab"), this,
                               [this, repoIdx] { emit revealModelRequested(repoIdx); });
                menu.addAction(QStringLiteral("Copy model name"), this,
                               [name] { QApplication::clipboard()->setText(name); });
                menu.exec(m_assocList->viewport()->mapToGlobal(pos));
            });
    connect(m_assocList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* it) {
                if (it) emit revealModelRequested(it->data(Qt::UserRole).toInt());
            });
    m_assocPanel = addRightPage(QStringLiteral("ASSOCIATED MODELS"), m_assocList);

    // MIPMAPS panel — every mip of the current texture; click one to preview it.
    m_mipList = new QListWidget(m_rstack);
    m_mipList->setAlternatingRowColors(true);
    connect(m_mipList, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (it && m_currentRowEntry >= 0)
            startDecode(-1, it->data(Qt::UserRole).toInt());
    });
    addRightPage(QStringLiteral("MIPMAPS"), m_mipList);
    restorePanelLayout();               // bring up the saved (or default) set

    split->setStretchFactor(0, 3);   // list
    split->setStretchFactor(1, 4);   // preview
    split->setStretchFactor(2, 2);   // panels
    split->setSizes({360, 560, 320});   // sane first-run widths (all three visible)
    // fresh key — the old 2-section state starved the new 3rd (panel) column.
    PanelPersist::bind(split, QStringLiteral("panels/texturesSplit3"));

    connect(m_filter, &FilterBar::changed, this, &TexturesTab::applyFilter);

    // Debounced selection: arrow-keying down the list must not queue a full
    // blob read per row — only the row the user settles on decodes.
    m_selDebounce = new QTimer(this);
    m_selDebounce->setSingleShot(true);
    m_selDebounce->setInterval(150);
    connect(m_selDebounce, &QTimer::timeout, this, [this] {
        if (m_pendingRow >= 0) startDecode(m_pendingRow, -1);
    });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                if (cur.isValid()) {
                    m_pendingRow = cur.row();
                    m_selDebounce->start();
                }
            });
    connect(this, &TexturesTab::batchProgress, this,
            [this](int generation, QString message) {
                if (generation != m_batchGen.load()) return;
                emit statusText(message);
            },
            Qt::QueuedConnection);
    connect(this, &TexturesTab::batchDone, this,
            [this](int generation, QString message) {
                m_batchRunning = false;
                if (generation == m_batchGen.load())
                    m_info->setText(message);
                qInfo("texture batch: %s", qPrintable(message));
            },
            Qt::QueuedConnection);

    // associated-models index: progress + result (queued: built off-thread)
    connect(this, &TexturesTab::assocProgress, this,
            [this](int generation, int percent) {
                if (generation != m_assocGen.load()) return;
                if (m_assocPanel)
                    m_assocPanel->label->setText(
                        QStringLiteral("ASSOCIATED MODELS  ⟳ building %1%")
                            .arg(percent));
            },
            Qt::QueuedConnection);
    connect(this, &TexturesTab::assocReady, this,
            [this](int generation, std::shared_ptr<AssocIndex> index) {
                if (generation != m_assocGen.load()) return;
                m_assoc = std::move(index);
                populateAssoc();   // refresh for whatever is currently shown
            },
            Qt::QueuedConnection);

    // decode worker -> GUI (queued: payload crosses threads)
    connect(this, &TexturesTab::decoded, this,
            [this](int generation, QImage image, QString info, QString warn,
                   std::shared_ptr<di::Texture2D> tex,
                   std::shared_ptr<AtlasPlist::Sheet> atlas, QString atlasNote) {
                if (generation != m_generation.load()) return;
                m_image = image;
                m_composedMask = -1;   // invalidate the composed cache
                m_tex   = tex;
                // The stored image split into its channels — this is the
                // shared panel's TEXTURE mode, which is exactly what this tab
                // wants (the PBR mode answers a question only a material has).
                m_texPrev.showTexture(m_image, m_currentName);
                QString text = warn.isEmpty()
                                   ? info
                                   : info + QStringLiteral("<br><i>%1</i>").arg(warn);
                if (!atlasNote.isEmpty())
                    text += QStringLiteral("<br>%1").arg(atlasNote);
                m_info->setText(text);
                // mip picker reflects the parsed container (largest first)
                m_mipBox->blockSignals(true);
                m_mipBox->clear();
                m_mipBox->addItem(QStringLiteral("Largest mip"), -1);
                if (tex) {
                    for (int i = (int)tex->mips.size() - 1; i >= 0; --i)
                        m_mipBox->addItem(QStringLiteral("%1x%2")
                                              .arg(tex->mips[i].width)
                                              .arg(tex->mips[i].height), i);
                }
                m_mipBox->blockSignals(false);
                // MIPMAPS panel — mirror the mip picker as a clickable list.
                if (m_mipList) {
                    m_mipList->clear();
                    auto* big = new QListWidgetItem(QStringLiteral("Largest mip"));
                    big->setData(Qt::UserRole, -1);
                    m_mipList->addItem(big);
                    if (tex)
                        for (int i = (int)tex->mips.size() - 1; i >= 0; --i) {
                            auto* it = new QListWidgetItem(
                                QStringLiteral("%1x%2").arg(tex->mips[i].width)
                                    .arg(tex->mips[i].height));
                            it->setData(Qt::UserRole, i);
                            m_mipList->addItem(it);
                        }
                }
                // atlas frames panel
                m_atlas = std::move(atlas);
                m_frameFocus = -1;
                m_frames->blockSignals(true);
                m_frames->clear();
                if (m_atlas) {
                    for (int i = 0; i < (int)m_atlas->frames.size(); ++i) {
                        const AtlasPlist::Frame& f = m_atlas->frames[i];
                        auto* it = new QTreeWidgetItem(
                            {QString::number(i), f.name,
                             QStringLiteral("%1x%2").arg(f.w).arg(f.h),
                             f.rotated ? QStringLiteral("yes") : QString()});
                        m_frames->addTopLevelItem(it);
                    }
                    if (m_framesPanel)
                        m_framesPanel->label->setText(
                            QStringLiteral("TEXFRAMES · %1")
                                .arg(m_atlas->frames.size()));
                }
                if (m_framesPanel) m_framesPanel->setVisible(m_atlas != nullptr);
                m_frames->blockSignals(false);
                populateAssoc();   // models that reference this texture
                refreshView();
            },
            Qt::QueuedConnection);

    // Hover previews — one engine per view, one resolver (LAST so their event
    // filters run before this tab's own on the shared grid viewport, letting
    // the popup consume its resize-wheel while Ctrl+wheel still reaches the
    // tile-size handler).
    auto hoverResolve = [this](const QModelIndex& idx,
                               QList<HoverPreview::Line>* lines, QImage* img) {
        return resolveHover(idx, lines, img);
    };
    m_hoverList = new HoverPreview(m_table, hoverResolve, this);
    m_hoverGrid = new HoverPreview(m_grid, hoverResolve, this);
    connect(this, &TexturesTab::hoverDecoded, this,
            [this](int seq, int entryId, QImage image) {
                if (seq != m_hoverSeq.load()) return;   // swept past this row
                m_hoverImg   = image;
                m_hoverEntry = entryId;
                m_hoverList->refresh();
                m_hoverGrid->refresh();
            },
            Qt::QueuedConnection);

    // restore the remembered view mode (after all widgets exist)
    if (QSettings().value(QLatin1String(kGridViewKey), false).toBool())
        m_gridBtn->setChecked(true);   // toggled() runs setGridView(true)
}

// Popup content: identity lines immediately; the image starts as the cached
// list thumbnail and upgrades in place once the preview-size decode lands.
bool TexturesTab::resolveHover(const QModelIndex& idx,
                               QList<HoverPreview::Line>* lines, QImage* image)
{
    const AssetRow* r = m_model->rowAt(idx.row());
    if (!r || !m_idx || !m_idx->store) return false;

    QString leaf = r->display;
    const int slash = leaf.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) leaf = leaf.mid(slash + 1);
    lines->append({leaf, HoverInfo::Col::kName});
    if (HoverInfo::on("tex/path"))
        lines->append({r->display, HoverInfo::Col::kFile});
    if (HoverInfo::on("tex/meaning") && !r->meaning.isEmpty())
        lines->append({r->meaning, HoverInfo::Col::kSeries});
    if (HoverInfo::on("tex/info"))
        lines->append({QStringLiteral("%1 · %2")
                           .arg(QLocale::c().formattedDataSize(r->size, 1),
                                QString::fromStdString(
                                    m_idx->store->mpk().pakFileName(
                                        m_idx->store->mpk()
                                            .entries()[r->entryId]))),
                       HoverInfo::Col::kMeta});
    if (HoverInfo::on("tex/atlas") &&
        AtlasPlist::findDescriptor(m_idx->store->mpk(), r->display) != SIZE_MAX)
        lines->append({QStringLiteral("texture atlas (has frame descriptor)"),
                       HoverInfo::Col::kGood});

    if (!HoverInfo::imagePreview()) return true;
    const int entryId = (int)r->entryId;
    if (m_hoverEntry == entryId) {
        // Decode already ran for this row — null means it FAILED (ASTC-class
        // format); returning here stops a failed row from re-kicking a decode
        // on every popup refresh.
        if (!m_hoverImg.isNull()) *image = m_hoverImg;
        return true;
    }
    // Placeholder from the list-thumbnail cache (cache-only peek — a hover
    // sweep must never flood the decode queue)…
    if (m_thumbs) {
        QPixmap pm;
        if (m_thumbs->peek(r->entryId, &pm)) *image = pm.toImage();
    }
    // …and a one-off decode at preview size, seq-guarded so only the row the
    // cursor is still on applies.
    const int seq = ++m_hoverSeq;
    auto store = m_idx->store;
    const QString name = r->display;
    QThread* worker = QThread::create([this, seq, store, entryId, name] {
        seh::installSehTranslator();
        QImage img;
        seh::runGuarded("hover-decode", [&] {
            const std::vector<uint8_t> raw =
                store->mpk().readAsset((size_t)entryId);
            di::Texture2D tex;
            std::string err;
            if (!di::isTexture2D(raw.data(), raw.size()) ||
                !di::parseTexture2D(raw.data(), raw.size(), &tex, &err))
                return;
            // smallest mip >= previewPx by area (mips are smallest-FIRST in
            // this store — pick by size, never by slice order)
            const int want = HoverInfo::previewPx();
            int best = -1, bestArea = -1, largest = -1, largestArea = -1;
            for (int i = 0; i < (int)tex.mips.size(); ++i) {
                const int area = tex.mips[i].width * tex.mips[i].height;
                if (area > largestArea) { largestArea = area; largest = i; }
                if (tex.mips[i].width >= want && tex.mips[i].height >= want &&
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

bool TexturesTab::eventFilter(QObject* obj, QEvent* ev)
{
    // NULL-GUARD EVERY MEMBER HERE: this filter is installed on the grid
    // viewport EARLY in the constructor, and Qt delivers style/parent-change
    // events to it while later widgets (m_scroll, m_canvas…) are still null —
    // an unguarded m_scroll->viewport() crashed the app at startup (verified
    // with a debugger: segfault inside TexturesTab::TexturesTab via the
    // stacked-layout insert).
    if (m_scroll && obj == m_scroll->viewport() &&
        ev->type() == QEvent::Resize && m_fit->isChecked() && !m_image.isNull())
        refreshView();

    // Channel-tile hover: the shared panel owns both the zoom popup and the
    // pixel readout, so this is one call rather than a per-tab reimplementation.
    if (ev->type() == QEvent::Enter || ev->type() == QEvent::Leave ||
        ev->type() == QEvent::MouseMove)
        m_texPrev.hover(obj, ev);

    // Ctrl+wheel over the grid resizes tiles.
    if (m_grid && obj == m_grid->viewport() && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (we->modifiers() & Qt::ControlModifier) {
            setGridPx(m_gridPx + (we->angleDelta().y() > 0 ? 16 : -16));
            return true;
        }
    }

    // Preview canvas: zoom / pan / inspect / refit.
    if (m_canvas && m_scroll && m_fit && m_pixel && obj == m_canvas) {
        switch (ev->type()) {
        case QEvent::Wheel: {
            auto* we = static_cast<QWheelEvent*>(ev);
            if (m_inspectSrc.isNull()) break;
            if (m_fit->isChecked()) {
                // leave Fit at the CURRENT effective scale so the first notch
                // zooms from what's on screen, not from a 100% jump
                m_zoom = m_lastScale;
                m_fit->blockSignals(true);
                m_fit->setChecked(false);
                m_fit->blockSignals(false);
            }
            m_zoom = qBound(0.05, m_zoom * (we->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15),
                            16.0);
            refreshView();
            return true;
        }
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                m_panning  = true;
                m_panStart = me->globalPosition().toPoint();
                m_panScroll = QPoint(m_scroll->horizontalScrollBar()->value(),
                                     m_scroll->verticalScrollBar()->value());
                m_canvas->setCursor(Qt::ClosedHandCursor);
            }
            break;
        }
        case QEvent::MouseMove: {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (m_panning) {
                const QPoint d = me->globalPosition().toPoint() - m_panStart;
                m_scroll->horizontalScrollBar()->setValue(m_panScroll.x() - d.x());
                m_scroll->verticalScrollBar()->setValue(m_panScroll.y() - d.y());
            }
            updatePixelInspector(me->pos());
            break;
        }
        case QEvent::MouseButtonRelease:
            m_panning = false;
            m_canvas->setCursor(Qt::ArrowCursor);
            break;
        case QEvent::MouseButtonDblClick:
            m_zoom = 1.0;
            if (!m_fit->isChecked())
                m_fit->setChecked(true);   // toggled() refreshes
            else
                refreshView();
            return true;
        case QEvent::Leave:
            m_pixel->setText(QStringLiteral(" "));
            break;
        default: break;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

TexturesTab::~TexturesTab()
{
    ++m_generation;
    ++m_assocGen;   // let the associated-models scan bail out of its 69k loop
    const QSet<QThread*> workers = m_workers;
    for (QThread* w : workers)
        w->wait();
}

void TexturesTab::setIndex(std::shared_ptr<AssetIndex> idx)
{
    // A reload assigns NEW entry ids — everything keyed on the old index is
    // poison: the shown entry, the mip combo (its picks would decode a random
    // asset from the new store), and any decode still in flight (audit finding).
    ++m_generation;
    ++m_batchGen;   // batch exports die with the store, not with a selection
    ++m_hoverSeq;   // and so do hover decodes
    m_hoverImg = QImage();
    m_hoverEntry = -1;
    if (m_hoverList) m_hoverList->hidePopup();
    if (m_hoverGrid) m_hoverGrid->hidePopup();
    m_idx = std::move(idx);
    m_currentRowEntry = -1;
    m_currentName.clear();
    m_tex.reset();
    m_atlas.reset();
    m_frameFocus = -1;
    m_frames->clear();
    m_atlasBox->setVisible(false);
    m_image = QImage();
    m_texPrev.clear();            // clear the channel tiles
    m_inspectSrc = QImage();
    m_composedMask = -1;
    m_zoom = 1.0;
    m_mipBox->clear();
    if (m_mipList) m_mipList->clear();
    m_info->clear();
    m_pixel->setText(QStringLiteral(" "));
    m_canvas->clear();
    m_canvas->resize(0, 0);
    m_pendingRow = -1;
    m_filter->setIndex(m_idx);
    // Thumbnail provider is per-store; swap it BEFORE the model learns the new
    // index so no data() call can pair new entry ids with the old store.
    m_model->setThumbnailProvider(nullptr);
    m_thumbs.reset();
    if (m_idx && m_idx->store) {
        // Grid mode decodes at 128px so tiles stay crisp across Ctrl+wheel
        // sizes without a re-decode per notch (fixed-size trick from D4).
        const int px = m_stack->currentIndex() == 1 ? 128 : ThumbnailProvider::kSize;
        m_thumbs = std::make_unique<ThumbnailProvider>(m_idx->store, px);
        connect(m_thumbs.get(), &ThumbnailProvider::ready, this, [this] {
            // ready() is queued cross-thread; an event posted just before a
            // provider swap can still arrive after it — guard the pointer.
            if (!m_thumbs) return;
            m_thumbs->commit();
            m_table->viewport()->update();
            m_grid->viewport()->update();
        });
        m_model->setThumbnailProvider(m_thumbs.get());
    }
    m_model->setIndex(m_idx, m_filter->spec());
    emit statusText(QStringLiteral("%L1 textures").arg(m_model->visibleCount()));

    // The texture->model reverse index is per-store; rebuild it for the new one.
    ++m_assocGen;
    m_assoc.reset();
    if (m_assocList) m_assocList->clear();
    if (m_assocPanel) m_assocPanel->label->setText(QStringLiteral("ASSOCIATED MODELS"));
    buildAssocIndex();
}

// Invert the whole repository's Model -> Material -> texture chain into
// blob -> {models, materials}, off the GUI thread. ~69k Models each resolve a
// material blob, so this is deliberately a background build (D4 shows the same
// progress bar); the panel fills when assocReady arrives. Guarded by m_assocGen
// so a store reload abandons an in-flight build instead of publishing stale ids.
void TexturesTab::buildAssocIndex()
{
    if (!m_idx || !m_idx->store) return;
    const int generation = m_assocGen.load();
    auto store = m_idx->store;
    QThread* worker = QThread::create([this, generation, store] {
        seh::installSehTranslator();
        auto index = std::make_shared<AssocIndex>();
        seh::runGuarded("assoc-index", [&] {
            const di::Repository* repo = store->repo();
            if (!repo) return;
            const size_t total = repo->entries.size();
            // The cost is fillFromMaterial (blob read + text parse), which
            // resolveMaterialTextures does per Material. Many Models share one
            // Material, so resolve each Material's textures ONCE and reuse.
            // Finding a Model's Material is a cheap in-memory dep-walk — no blob
            // read — and every Model carries exactly one direct Material
            // dependency (measured). This is output-identical to calling
            // resolveModelChain per Model, just without re-parsing shared
            // materials tens of thousands of times.
            std::unordered_map<qint32, di::ResolvedModel> matCache;
            size_t done = 0;
            int lastPct = -1;
            for (size_t i = 0; i < total; ++i) {
                if (generation != m_assocGen.load()) return;   // store swapped
                ++done;
                const di::RepoEntry& e = repo->entries[i];
                if (repo->typeOf(e) != "Model") continue;
                qint32 matRepo = -1;
                for (const std::string& h : e.related) {
                    auto bit = repo->byHash.find(h);
                    if (bit == repo->byHash.end()) continue;
                    if (repo->typeOf(repo->entries[bit->second]) == "Material") {
                        matRepo = (qint32)bit->second;
                        break;
                    }
                }
                if (matRepo < 0) continue;
                auto cit = matCache.find(matRepo);
                if (cit == matCache.end())
                    cit = matCache.emplace(
                        matRepo,
                        di::resolveMaterialTextures(*store, matRepo)).first;
                const di::ResolvedModel& res = cit->second;
                const size_t blobs[4] = {res.texBlob, res.nrmBlob,
                                         res.mixBlob, res.emiBlob};
                for (size_t b : blobs) {
                    if (b == (size_t)-1) continue;
                    index->models[b].push_back((qint32)i);
                    index->mats[b].push_back(matRepo);
                }
                const int pct = total ? int(done * 100 / total) : 100;
                if (pct != lastPct) {
                    lastPct = pct;
                    emit assocProgress(generation, pct);
                }
            }
        });
        emit assocReady(generation, index);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

// Fill the ASSOCIATED MODELS panel for the currently-shown texture. The texture
// row's entryId IS its mpk blob id — the same key the reverse index uses.
void TexturesTab::populateAssoc()
{
    if (!m_assocList || !m_assocPanel) return;
    m_assocList->clear();
    if (!m_idx || !m_idx->store) {
        m_assocPanel->label->setText(QStringLiteral("ASSOCIATED MODELS"));
        return;
    }
    if (!m_assoc) {   // index still building
        m_assocPanel->label->setText(
            QStringLiteral("ASSOCIATED MODELS  ⟳ building…"));
        return;
    }
    const di::Repository* repo = m_idx->store->repo();
    const size_t blob = (size_t)m_currentRowEntry;
    auto it = m_assoc->models.find(blob);
    if (m_currentRowEntry < 0 || !repo || it == m_assoc->models.end() ||
        it->second.empty()) {
        m_assocPanel->label->setText(QStringLiteral("ASSOCIATED MODELS (0)"));
        return;
    }
    // distinct materials referencing this texture
    int matCount = 0;
    auto mit = m_assoc->mats.find(blob);
    if (mit != m_assoc->mats.end()) {
        std::vector<qint32> m = mit->second;
        std::sort(m.begin(), m.end());
        m.erase(std::unique(m.begin(), m.end()), m.end());
        matCount = (int)m.size();
    }
    // distinct models, sorted by name; repoIdx kept in UserRole for a future
    // "Reveal in Models tab" once the Models tab exposes a reveal slot.
    std::vector<qint32> models = it->second;
    std::sort(models.begin(), models.end());
    models.erase(std::unique(models.begin(), models.end()), models.end());
    std::vector<std::pair<QString, qint32>> named;
    named.reserve(models.size());
    for (qint32 idx : models) {
        if (idx < 0 || (size_t)idx >= repo->entries.size()) continue;
        named.emplace_back(
            QString::fromStdString(repo->entries[(size_t)idx].name), idx);
    }
    std::sort(named.begin(), named.end(),
              [](const std::pair<QString, qint32>& a,
                 const std::pair<QString, qint32>& b) {
                  return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
              });
    for (const auto& p : named) {
        auto* item = new QListWidgetItem(p.first);
        item->setData(Qt::UserRole, p.second);
        m_assocList->addItem(item);
    }
    m_assocPanel->label->setText(
        QStringLiteral("ASSOCIATED MODELS (%1 MODELS / %2 MATERIALS)")
            .arg(m_assocList->count()).arg(matCount));
}

// ── Right-column panel strip (mirrors the Models tab) ──────────────────────
PanelBox* TexturesTab::addRightPage(const QString& title, QWidget* content)
{
    if (!m_rstack || !m_rstripLay) return nullptr;
    const int page = (int)m_rsections.size();
    auto* box = new PanelBox(title, content, m_rstack);
    box->hide();
    m_rstack->addWidget(box);
    m_rsections.push_back(box);
    m_sectKeys << (QStringLiteral("textures/panel/") +
                   title.section(QLatin1Char(' '), 0, 0));

    auto* b = new QToolButton(m_rstripLay->parentWidget());
    b->setIcon(QIcon(panelGlyph(title)));
    b->setIconSize(QSize(18, 18));
    b->setCheckable(true);
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
    int at = m_rstripLay->count();
    if (at > 0 && m_rstripLay->itemAt(at - 1)->spacerItem()) --at;
    m_rstripLay->insertWidget(at, b);
    m_rpageBtns.push_back(b);
    return box;
}

void TexturesTab::showPanel(int page, bool on)
{
    if (page < 0 || page >= (int)m_rsections.size()) return;
    PanelBox* box = m_rsections[(size_t)page];
    const bool was = !box->isHidden();
    box->setVisible(on);
    if (on && !was && !m_panelRestore) panelBoxArrive(m_rstack, box);
    savePanelLayout();
}

void TexturesTab::movePanel(int page, int delta)
{
    if (!m_rstack || page < 0 || page >= (int)m_rsections.size()) return;
    PanelBox* box = m_rsections[(size_t)page];
    std::vector<int> vis;
    for (int i = 0; i < m_rstack->count(); ++i)
        if (!m_rstack->widget(i)->isHidden()) vis.push_back(i);
    const int myIdx = m_rstack->indexOf(box);
    int cur = -1;
    for (size_t k = 0; k < vis.size(); ++k)
        if (vis[k] == myIdx) { cur = (int)k; break; }
    const int tgt = cur + delta;
    if (cur < 0 || tgt < 0 || tgt >= (int)vis.size()) return;
    const QList<int> sizes = m_rstack->sizes();
    m_rstack->insertWidget(vis[(size_t)tgt], box);
    m_rstack->setSizes(sizes);
    savePanelLayout();
}

void TexturesTab::savePanelLayout()
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
    QSettings().setValue(QStringLiteral("textures/panels/shown"), shown);
}

void TexturesTab::restorePanelLayout()
{
    QSettings s;
    m_panelRestore = true;
    if (!s.contains(QStringLiteral("textures/panels/shown"))) {
        for (QToolButton* b : m_rpageBtns) b->setChecked(true);   // first run: all up
    } else {
        const QStringList shown =
            s.value(QStringLiteral("textures/panels/shown")).toStringList();
        for (const QString& key : shown)
            for (size_t k = 0; k < m_rsections.size(); ++k)
                if (m_sectKeys.value((int)k).section(QLatin1Char('/'), -1) == key) {
                    m_rstack->addWidget(m_rsections[k]);
                    m_rpageBtns[k]->setChecked(true);
                    break;
                }
    }
    m_panelRestore = false;
    savePanelLayout();
}

void TexturesTab::applyFilter()
{
    m_pendingRow = -1;   // refilter remaps rows; kill any queued selection load
    m_model->setFilters(m_filter->spec());
    emit statusText(QStringLiteral("%L1 textures").arg(m_model->visibleCount()));
}

void TexturesTab::setGridView(bool on)
{
    m_stack->setCurrentIndex(on ? 1 : 0);
    QSettings().setValue(QLatin1String(kGridViewKey), on);
    // Re-key the thumbnail decode size for the active view; the swap drops the
    // pixmap cache, which is exactly the price of changing decode size once.
    if (m_idx && m_idx->store) {
        const int px = on ? 128 : ThumbnailProvider::kSize;
        if (m_thumbs && m_thumbs->targetPx() == px) return;
        m_model->setThumbnailProvider(nullptr);
        m_thumbs.reset();
        m_thumbs = std::make_unique<ThumbnailProvider>(m_idx->store, px);
        connect(m_thumbs.get(), &ThumbnailProvider::ready, this, [this] {
            if (!m_thumbs) return;
            m_thumbs->commit();
            m_table->viewport()->update();
            m_grid->viewport()->update();
        });
        m_model->setThumbnailProvider(m_thumbs.get());
        m_table->viewport()->update();
        m_grid->viewport()->update();
    }
}

void TexturesTab::setGridPx(int px)
{
    px = qBound(48, px, 192);
    if (px == m_gridPx) return;
    m_gridPx = px;
    QSettings().setValue(QLatin1String(kGridPxKey), px);
    m_grid->setIconSize(QSize(px, px));
    QAbstractItemDelegate* old = m_grid->itemDelegate();
    m_grid->setItemDelegate(new GridItemDelegate(px, m_grid));
    delete old;
    m_grid->doItemsLayout();
}

QList<int> TexturesTab::menuTargetRows(int clickedRow) const
{
    QList<int> sel;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex& mi : rows) sel.append(mi.row());
    std::sort(sel.begin(), sel.end());
    if (clickedRow >= 0 && !sel.contains(clickedRow))
        return {clickedRow};   // clicked row wins unless it's in the selection
    if (!sel.isEmpty()) return sel;
    if (clickedRow >= 0) return {clickedRow};
    return {};
}

void TexturesTab::showListMenu(QWidget* view, const QPoint& viewportPos)
{
    auto* av = qobject_cast<QAbstractItemView*>(view);
    if (!av || !m_idx) return;
    const QModelIndex idx = av->indexAt(viewportPos);
    const QList<int> rows = menuTargetRows(idx.isValid() ? idx.row() : -1);
    if (rows.isEmpty()) return;

    QList<Job> jobs;
    QStringList names, meanings, mpkPaths;
    for (int r : rows) {
        if (const AssetRow* row = m_model->rowAt(r)) {
            jobs.append({(int)row->entryId, row->display});
            names << row->display;
            if (!row->meaning.isEmpty()) meanings << row->meaning;
            if (!row->mpkName.isEmpty()) mpkPaths << row->mpkName;
        }
    }
    if (jobs.isEmpty()) return;
    const int n = jobs.size();
    const QString what = n == 1 ? QStringLiteral("1 texture")
                                : QStringLiteral("%1 textures").arg(n);

    QMenu menu(this);
    QSettings s;
    const QString last = s.value(QLatin1String(kLastDirKey)).toString();
    if (!last.isEmpty() && QDir(last).exists())
        menu.addAction(MenuText::exportSetLast(what, MenuText::condensePath(last)),
                       this, [this, jobs, last] { exportRows(jobs, last); });
    menu.addAction(MenuText::exportSetPrompt(what), this, [this, jobs, what] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export %1 to…").arg(what),
            QSettings().value(QLatin1String(kLastDirKey)).toString());
        if (!dir.isEmpty()) exportRows(jobs, dir);
    });

    // Image actions apply to the texture the preview is actually showing.
    if (n == 1 && jobs[0].entryId == m_currentRowEntry && !m_image.isNull()) {
        menu.addSeparator();
        menu.addAction(MenuText::kCopyImage, this, [this] {
            QApplication::clipboard()->setImage(m_image);
        });
        menu.addAction(MenuText::kSaveImage, this, [this] { saveImageNow(); });
        menu.addAction(QStringLiteral("Export shown view (R/G/B/A)…"), this,
                       [this] { exportShownView(); });
    }

    menu.addSeparator();
    const QString multi = QStringLiteral(" — %1 rows").arg(n);
    menu.addAction(n == 1 ? MenuText::withValue(MenuText::kCopyName, names[0])
                          : MenuText::kCopyName + multi,
                   this, [names] {
                       QApplication::clipboard()->setText(
                           names.join(QLatin1Char('\n')));
                   });
    if (!meanings.isEmpty())
        menu.addAction(n == 1 ? MenuText::withValue(MenuText::kCopyMeaning,
                                                    meanings[0])
                              : MenuText::kCopyMeaning + multi,
                       this, [meanings] {
                           QApplication::clipboard()->setText(
                               meanings.join(QLatin1Char('\n')));
                       });
    if (!mpkPaths.isEmpty())
        menu.addAction(n == 1 ? MenuText::withValue(MenuText::kCopyMpkPath,
                                                    mpkPaths[0])
                              : MenuText::kCopyMpkPath + multi,
                       this, [mpkPaths] {
                           QApplication::clipboard()->setText(
                               mpkPaths.join(QLatin1Char('\n')));
                       });
    menu.exec(av->viewport()->mapToGlobal(viewportPos));
}

void TexturesTab::startDecode(int modelRow, int mipIndex)
{
    if (!m_idx) return;
    int entryId = m_currentRowEntry;
    QString name = m_currentName;
    if (modelRow >= 0) {
        const AssetRow* r = m_model->rowAt(modelRow);
        if (!r) return;
        entryId = (int)r->entryId;
        name    = r->display;
    }
    if (entryId < 0) return;
    m_currentRowEntry = entryId;
    m_currentName     = name;

    const int generation = ++m_generation;
    m_info->setText(QStringLiteral("<b>%1</b><br>decoding…").arg(name.toHtmlEscaped()));

    auto store = m_idx->store;
    QThread* worker = QThread::create([this, generation, store, entryId, name, mipIndex] {
        seh::installSehTranslator();
        QImage image;
        QString info, warn, atlasNote;
        std::shared_ptr<di::Texture2D> tex;
        std::shared_ptr<AtlasPlist::Sheet> atlas;
        seh::runGuarded("texture-decode", [&] {
            QElapsedTimer timer;
            timer.start();
            const std::vector<uint8_t> raw = store->mpk().readAsset((size_t)entryId);
            tex = std::make_shared<di::Texture2D>();
            std::string err;
            if (!di::parseTexture2D(raw.data(), raw.size(), tex.get(), &err)) {
                info = QStringLiteral("<b>%1</b><br>parse failed: %2")
                           .arg(name.toHtmlEscaped(), QString::fromStdString(err));
                tex.reset();
                return;
            }
            TextureDecode::Result res = TextureDecode::decode(*tex, mipIndex);
            image = res.image;
            warn  = res.error;
            // Permanent instrumentation: one line per decode so the log alone
            // answers "did the preview render, how big, how slow".
            qInfo("texture: %s fmt=%s %dx%d mips=%zu -> %s %dx%d in %lld ms%s%s",
                  qPrintable(name), qPrintable(DiPixelFormat::name(tex->format)),
                  tex->width, tex->height, tex->mips.size(),
                  res.image.isNull() ? "FAIL" : "ok",
                  res.mipWidth, res.mipHeight, (long long)timer.elapsed(),
                  warn.isEmpty() ? "" : " - ", warn.isEmpty() ? "" : qPrintable(warn));
            info  = QStringLiteral("<b>%1</b><br>%2 · %3x%4 base · %5 mips · "
                                   "shown %6x%7 · decoded in %8 ms")
                        .arg(name.toHtmlEscaped(), DiPixelFormat::name(tex->format))
                        .arg(tex->width).arg(tex->height)
                        .arg(tex->mips.size())
                        .arg(res.mipWidth).arg(res.mipHeight)
                        .arg(timer.elapsed());

            // Texture Atlas probe — DI's texframes. The descriptor is the
            // sibling Cocos plist under Package/UIScript/ (measured pairing).
            const size_t desc = AtlasPlist::findDescriptor(store->mpk(), name);
            if (desc != SIZE_MAX) {
                const std::vector<uint8_t> praw = store->mpk().readAsset(desc);
                auto sheet = std::make_shared<AtlasPlist::Sheet>();
                QString perr;
                if (AtlasPlist::parse(praw.data(), praw.size(), sheet.get(),
                                      &perr)) {
                    atlas = sheet;
                    atlasNote =
                        QStringLiteral("atlas: %1 frames (plist format %2)")
                            .arg(sheet->frames.size())
                            .arg(sheet->format);
                    qInfo("atlas: %s -> %zu frames (format %d, %zu bytes)",
                          qPrintable(name), sheet->frames.size(), sheet->format,
                          praw.size());
                } else {
                    // Loud, precise, never guessed — the message carries the
                    // measurement needed to extend the reader.
                    atlasNote = QStringLiteral("<i>atlas descriptor found but "
                                               "not read: %1</i>")
                                    .arg(perr.toHtmlEscaped());
                    qWarning("atlas: %s descriptor (%zu bytes) not read: %s",
                             qPrintable(name), praw.size(), qPrintable(perr));
                }
            }
        });
        if (info.isEmpty())
            info = QStringLiteral("<b>%1</b><br>decode crashed (see log)").arg(name.toHtmlEscaped());
        emit decoded(generation, image, info, warn, tex, atlas, atlasNote);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

QImage TexturesTab::composeChannels(const QImage& src, bool checkerboard) const
{
    if (src.isNull()) return {};
    const bool r = m_chR->isChecked(), g = m_chG->isChecked(),
               b = m_chB->isChecked(), a = m_chA->isChecked();
    const int shown = int(r) + int(g) + int(b) + int(a);

    QImage layer(src.width(), src.height(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y) {
        const quint8* sp = src.constScanLine(y);
        QRgb* dst = reinterpret_cast<QRgb*>(layer.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const quint8* p = sp + x * 4;
            if (shown == 1) {
                // single channel -> opaque grayscale
                const quint8 v = r ? p[0] : g ? p[1] : b ? p[2] : p[3];
                dst[x] = qRgba(v, v, v, 255);
            } else {
                dst[x] = qRgba(r ? p[0] : 0, g ? p[1] : 0, b ? p[2] : 0,
                               a ? p[3] : 255);
            }
        }
    }
    if (!checkerboard) return layer;

    QImage out(src.width(), src.height(), QImage::Format_ARGB32);
    // Alpha checkerboard shows through wherever output alpha < 255. One tiled
    // fill, not per-square fillRect calls (262k of those froze a 4k preview).
    {
        static QPixmap tile;
        if (tile.isNull()) {
            tile = QPixmap(16, 16);
            QPainter tp(&tile);
            tp.fillRect(0, 0, 16, 16, QColor(0x6a, 0x6a, 0x6a));
            tp.fillRect(0, 0, 8, 8,   QColor(0x9a, 0x9a, 0x9a));
            tp.fillRect(8, 8, 8, 8,   QColor(0x9a, 0x9a, 0x9a));
        }
        QPainter p(&out);
        p.fillRect(out.rect(), QBrush(tile));
        p.drawImage(0, 0, layer);
    }
    return out;
}

QImage TexturesTab::composed() const
{
    return composeChannels(m_image, m_alphaBg->isChecked());
}

// The preview's channel logic without the checkerboard: what you SEE is what
// lands in the file. Single channel exports as opaque grayscale; RGB drops
// alpha to opaque; any combo with A keeps real transparency.
QImage TexturesTab::composedForExport() const
{
    const bool any = m_chR->isChecked() || m_chG->isChecked() ||
                     m_chB->isChecked() || m_chA->isChecked();
    if (!any) return {};
    return composeChannels(m_image, false);
}

QImage TexturesTab::croppedFrame(int idx, bool trim) const
{
    if (!m_atlas || idx < 0 || idx >= (int)m_atlas->frames.size() ||
        m_image.isNull())
        return {};
    const AtlasPlist::Frame& f = m_atlas->frames[idx];
    // Plist rects are BASE-resolution sheet pixels; the preview may be showing
    // a smaller mip (mip combo, or the decoder's content fallback) — scale the
    // rect by the actual decoded size or the crop lands on the wrong art
    // (review finding).
    double sx = 1.0, sy = 1.0;
    if (m_tex && m_tex->width > 0 && m_tex->height > 0) {
        sx = double(m_image.width())  / double(m_tex->width);
        sy = double(m_image.height()) / double(m_tex->height);
    }
    // A rotated frame is stored 90° clockwise in the sheet, so its atlas rect
    // has w/h swapped; rotating -90° restores the authored orientation
    // (Cocos convention — visually verifiable per frame in the preview).
    const int fw = f.rotated ? f.h : f.w;
    const int fh = f.rotated ? f.w : f.h;
    QRect r(qRound(f.x * sx), qRound(f.y * sy),
            qMax(1, qRound(fw * sx)), qMax(1, qRound(fh * sy)));
    r &= m_image.rect();
    if (r.isEmpty()) return {};
    QImage img = m_image.copy(r);
    if (f.rotated)
        img = img.transformed(QTransform().rotate(-90));
    if (trim && img.hasAlphaChannel()) {
        int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                if (qAlpha(line[x]) != 0) {
                    minX = qMin(minX, x); maxX = qMax(maxX, x);
                    minY = qMin(minY, y); maxY = qMax(maxY, y);
                }
            }
        }
        if (maxX >= minX && maxY >= minY)
            img = img.copy(QRect(QPoint(minX, minY), QPoint(maxX, maxY)));
    }
    return img;
}

void TexturesTab::exportFrames(bool all, bool toLastDir)
{
    if (!m_atlas || m_image.isNull()) return;
    // Selected frames; nothing selected means ALL frames (D4 convention).
    QList<int> rows;
    if (!all)
        for (QTreeWidgetItem* it : m_frames->selectedItems())
            rows.append(m_frames->indexOfTopLevelItem(it));
    std::sort(rows.begin(), rows.end());
    if (rows.isEmpty())
        for (int i = 0; i < (int)m_atlas->frames.size(); ++i) rows.append(i);

    QSettings s;
    QString dir = toLastDir ? s.value(QLatin1String(kFramesDirKey)).toString()
                            : QString();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export %1 frame(s) to…").arg(rows.size()),
            s.value(QLatin1String(kFramesDirKey)).toString());
    }
    if (dir.isEmpty()) return;
    s.setValue(QLatin1String(kFramesDirKey), dir);

    const bool trim = m_trim->isChecked();
    const QString base = exportBaseName(m_currentName);
    int written = 0, failed = 0;
    for (int i : rows) {
        const QImage img = croppedFrame(i, trim);
        const QString fn = QStringLiteral("%1_%2_%3.png")
                               .arg(base)
                               .arg(i, 3, 10, QLatin1Char('0'))
                               .arg(sanitizeFileToken(m_atlas->frames[i].name));
        if (!img.isNull() && img.save(QDir(dir).filePath(fn), "PNG"))
            ++written;
        else
            ++failed;
    }
    m_info->setText(m_info->text() +
                    QStringLiteral("<br><i>exported %1 frame(s) to %2%3</i>")
                        .arg(written)
                        .arg(dir)
                        .arg(failed ? QStringLiteral(" — %1 FAILED").arg(failed)
                                    : QString()));
    qInfo("atlas: exported %d frame(s) of %s to %s (%d failed, trim=%d)",
          written, qPrintable(m_currentName), qPrintable(dir), failed, trim);
}

void TexturesTab::exportShownView()
{
    const QImage img = composedForExport();
    if (img.isNull()) return;
    const QString suffixTag = [this] {
        QString t;
        if (m_chR->isChecked()) t += QLatin1Char('r');
        if (m_chG->isChecked()) t += QLatin1Char('g');
        if (m_chB->isChecked()) t += QLatin1Char('b');
        if (m_chA->isChecked()) t += QLatin1Char('a');
        return t.isEmpty() ? QStringLiteral("none") : t;
    }();
    QSettings s;
    const QString seed = s.value(QLatin1String(kLastDirKey)).toString();
    const QString base = exportBaseName(m_currentName) + QLatin1Char('_') +
                         suffixTag + QStringLiteral(".png");
    const QString dest = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export shown view as PNG"),
        seed.isEmpty() ? base : QDir(seed).filePath(base),
        QStringLiteral("PNG images (*.png)"));
    if (dest.isEmpty()) return;
    if (img.save(dest, "PNG"))
        s.setValue(QLatin1String(kLastDirKey), QFileInfo(dest).absolutePath());
    else
        m_info->setText(m_info->text() + QStringLiteral("  <b>export FAILED</b>"));
}

void TexturesTab::exportSelected(bool toLastDir)
{
    if (!m_idx || !m_idx->store || m_batchRunning) return;
    QList<Job> jobs;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex& mi : rows) {
        const AssetRow* r = m_model->rowAt(mi.row());
        if (r) jobs.append({(int)r->entryId, r->display});
    }
    if (jobs.isEmpty()) return;
    QSettings s;
    QString dir = toLastDir ? s.value(QLatin1String(kLastDirKey)).toString()
                            : QString();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("Export %1 texture(s) as PNG into…").arg(jobs.size()),
            s.value(QLatin1String(kLastDirKey)).toString());
    }
    if (dir.isEmpty()) return;
    exportRows(jobs, dir);
}

// Batch export: decode every job's largest mip on a worker and write
// <display-name>.png into the folder. Jobs are snapshotted up front; an index
// reload (generation bump) abandons the run.
void TexturesTab::exportRows(const QList<Job>& jobs, const QString& dir)
{
    if (!m_idx || !m_idx->store || m_batchRunning || jobs.isEmpty() ||
        dir.isEmpty())
        return;
    QSettings().setValue(QLatin1String(kLastDirKey), dir);

    const int generation = m_batchGen.load();
    m_batchRunning = true;
    auto store = m_idx->store;
    QThread* worker = QThread::create([this, generation, store, jobs, dir] {
        seh::installSehTranslator();
        int written = 0, failed = 0;
        QElapsedTimer timer;
        timer.start();
        seh::runGuarded("texture-batch", [&] {
            for (int i = 0; i < jobs.size(); ++i) {
                if (generation != m_batchGen.load()) {
                    qInfo("texture batch: abandoned after %d files (index reload)",
                          written);
                    break;
                }
                const auto& j = jobs[i];
                const std::vector<uint8_t> raw =
                    store->mpk().readAsset((size_t)j.entryId);
                di::Texture2D tex;
                std::string err;
                QImage img;
                if (!raw.empty() &&
                    di::parseTexture2D(raw.data(), raw.size(), &tex, &err))
                    img = TextureDecode::decode(tex).image;
                const QString base = flatExportName(j.name);
                if (!img.isNull() &&
                    img.save(dir + QLatin1Char('/') + base + QStringLiteral(".png"),
                             "PNG")) {
                    ++written;
                } else {
                    ++failed;
                    qWarning("texture batch: %s FAILED (%s)", qPrintable(j.name),
                             err.empty() ? "decode/save" : err.c_str());
                }
                if ((i + 1) % 8 == 0 || i + 1 == jobs.size())
                    emit batchProgress(generation,
                                       QStringLiteral("exporting textures… %1/%2")
                                           .arg(i + 1).arg(jobs.size()));
            }
        });
        emit batchDone(generation,
                       QStringLiteral("exported %1 PNGs to %2 (%3 failed, %4 ms)")
                           .arg(written).arg(dir).arg(failed).arg(timer.elapsed()));
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

// ── ExportHooks: the adaptive Export menu + Ctrl+E family ──────────────────

bool TexturesTab::canExport() const
{
    if (m_batchRunning || !m_idx) return false;
    return m_table->selectionModel()->hasSelection() || m_currentRowEntry >= 0;
}

QString TexturesTab::exportWhat() const
{
    const int n =
        qMax(1, (int)m_table->selectionModel()->selectedRows().size());
    return n == 1 ? QStringLiteral("1 texture")
                  : QStringLiteral("%1 textures").arg(n);
}

void TexturesTab::exportNow(bool toLastDir)
{
    if (!canExport()) return;
    // Selection wins; with no selection the shown texture exports alone.
    QList<Job> jobs;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex& mi : rows)
        if (const AssetRow* r = m_model->rowAt(mi.row()))
            jobs.append({(int)r->entryId, r->display});
    if (jobs.isEmpty() && m_currentRowEntry >= 0)
        jobs.append({m_currentRowEntry, m_currentName});
    if (jobs.isEmpty()) return;
    QSettings s;
    QString dir = toLastDir ? s.value(QLatin1String(kLastDirKey)).toString()
                            : QString();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("Export %1 as PNG into…").arg(exportWhat()),
            s.value(QLatin1String(kLastDirKey)).toString());
    }
    if (!dir.isEmpty()) exportRows(jobs, dir);
}

QString TexturesTab::lastExportDir() const
{
    const QString d = QSettings().value(QLatin1String(kLastDirKey)).toString();
    return QDir(d).exists() ? d : QString();
}

bool TexturesTab::canExportFrames() const
{
    return m_atlas && !m_atlas->frames.empty() && !m_image.isNull();
}

int TexturesTab::frameCount() const
{
    return m_atlas ? (int)m_atlas->frames.size() : 0;
}

int TexturesTab::frameSelCount() const
{
    return (int)m_frames->selectedItems().size();
}

void TexturesTab::exportFramesNow(bool all, bool toLastDir)
{
    exportFrames(all, toLastDir);
}

QString TexturesTab::framesLastDir() const
{
    const QString d = QSettings().value(QLatin1String(kFramesDirKey)).toString();
    return QDir(d).exists() ? d : QString();
}

bool TexturesTab::canSaveImage() const { return !m_image.isNull(); }

void TexturesTab::saveImageNow()
{
    if (m_image.isNull()) return;
    QSettings s;
    const QString seed = s.value(QLatin1String(kImageDirKey)).toString();
    const QString base = exportBaseName(m_currentName) + QStringLiteral(".png");
    const QString dest = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save image"),
        seed.isEmpty() ? base : QDir(seed).filePath(base),
        QStringLiteral("PNG images (*.png)"));
    if (dest.isEmpty()) return;
    if (m_image.save(dest, "PNG"))
        s.setValue(QLatin1String(kImageDirKey), QFileInfo(dest).absolutePath());
    else
        m_info->setText(m_info->text() +
                        QStringLiteral("<br><i>image save FAILED</i>"));
}

bool TexturesTab::canExportAll() const
{
    return !m_batchRunning && m_model && m_model->visibleCount() > 0;
}
int TexturesTab::exportAllCount() const
{
    return m_model ? m_model->visibleCount() : 0;
}
void TexturesTab::exportAllNow(bool toLastDir)
{
    if (!m_model) return;
    QList<Job> jobs;
    for (int i = 0; i < m_model->rowCount(); ++i)
        if (const AssetRow* r = m_model->rowAt(i))
            jobs.append({(int)r->entryId, r->display});
    if (jobs.isEmpty()) return;
    QSettings s;
    QString dir = toLastDir ? s.value(QLatin1String(kLastDirKey)).toString()
                            : QString();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export all %1 textures to…").arg(jobs.size()),
            s.value(QLatin1String(kLastDirKey)).toString());
    if (!dir.isEmpty()) exportRows(jobs, dir);
}

void TexturesTab::updatePixelInspector(const QPoint& canvasPos)
{
    if (m_inspectSrc.isNull() || m_canvas->pixmap().isNull()) {
        m_pixel->setText(QStringLiteral(" "));
        return;
    }
    const QSize pmSize = m_canvas->pixmap().size();
    // The canvas is resized to the pixmap, but keep the mapping robust to the
    // centred-alignment case where the widget is larger than the pixmap.
    const int offX = qMax(0, (m_canvas->width()  - pmSize.width())  / 2);
    const int offY = qMax(0, (m_canvas->height() - pmSize.height()) / 2);
    const int px = canvasPos.x() - offX, py = canvasPos.y() - offY;
    if (px < 0 || py < 0 || px >= pmSize.width() || py >= pmSize.height()) {
        m_pixel->setText(QStringLiteral(" "));
        return;
    }
    const int ix = qBound(0, px * m_inspectSrc.width()  / pmSize.width(),
                          m_inspectSrc.width()  - 1);
    const int iy = qBound(0, py * m_inspectSrc.height() / pmSize.height(),
                          m_inspectSrc.height() - 1);
    const QRgb c = m_inspectSrc.pixel(ix, iy);
    m_pixel->setText(
        QStringLiteral("(%1, %2)   RGBA %3 %4 %5 %6   #%7   zoom %8%")
            .arg(ix, 4).arg(iy, 4)
            .arg(qRed(c), 3).arg(qGreen(c), 3).arg(qBlue(c), 3).arg(qAlpha(c), 3)
            .arg(QString::number((quint32)c, 16).rightJustified(8, QLatin1Char('0')))
            .arg(qRound(m_lastScale * 100.0)));
}

void TexturesTab::refreshView()
{
    if (m_image.isNull()) {
        m_composedPm = QPixmap();
        m_composedMask = -1;
        m_inspectSrc = QImage();
        m_canvas->clear();
        m_canvas->resize(0, 0);
        return;
    }

    QPixmap pm;
    if (m_frameFocus >= 0 && m_atlas) {
        // Frame preview: crop is small, compose per call (no cache needed).
        const QImage frameImg = croppedFrame(m_frameFocus, false);
        m_inspectSrc = frameImg;
        pm = QPixmap::fromImage(
            composeChannels(frameImg, m_alphaBg->isChecked()));
    } else {
        // Cache the channel-composed pixmap: window-resize ticks (Fit mode)
        // must only re-SCALE, never redo the 16M-pixel recompose (audit
        // finding). Mask bit 4 = the checkerboard toggle.
        const int mask = (int(m_chR->isChecked()) << 0) |
                         (int(m_chG->isChecked()) << 1) |
                         (int(m_chB->isChecked()) << 2) |
                         (int(m_chA->isChecked()) << 3) |
                         (int(m_alphaBg->isChecked()) << 4);
        if (m_composedPm.isNull() || mask != m_composedMask) {
            m_composedPm  = QPixmap::fromImage(composed());
            m_composedMask = mask;
        }
        m_inspectSrc = m_image;
        pm = m_composedPm;
    }
    if (pm.isNull()) return;

    const QSize srcSize = pm.size();
    if (m_fit->isChecked()) {
        const QSize avail = m_scroll->viewport()->size() - QSize(2, 2);
        if (pm.width() > avail.width() || pm.height() > avail.height()) {
            // Smooth for normal sizes; a 4k atlas smooth-scales in the hundreds
            // of ms, so big images use the fast path during interaction.
            const bool big = qint64(pm.width()) * pm.height() > 4 * 1024 * 1024;
            pm = pm.scaled(avail, Qt::KeepAspectRatio,
                           big ? Qt::FastTransformation : Qt::SmoothTransformation);
        }
    } else if (!qFuzzyCompare(m_zoom, 1.0)) {
        // Cap the drawn pixmap at 8192 on the long edge: zoom persists across
        // texture picks, and 16x carried from a 64px icon onto a 4k atlas
        // would otherwise ask for a 17 GB allocation (review finding).
        const int longEdge = qMax(srcSize.width(), srcSize.height());
        const double zoom =
            longEdge > 0 ? qMin(m_zoom, 8192.0 / longEdge) : m_zoom;
        const QSize z(qMax(1, qRound(srcSize.width() * zoom)),
                      qMax(1, qRound(srcSize.height() * zoom)));
        const bool big = qint64(z.width()) * z.height() > 4 * 1024 * 1024;
        pm = pm.scaled(z, Qt::KeepAspectRatio,
                       big || zoom > 2.0 ? Qt::FastTransformation
                                         : Qt::SmoothTransformation);
    }
    m_lastScale = srcSize.width() > 0
                      ? double(pm.width()) / double(srcSize.width())
                      : 1.0;
    m_canvas->setPixmap(pm);
    m_canvas->resize(pm.size());
}
