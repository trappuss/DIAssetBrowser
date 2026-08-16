#include "tabs/WardrobeTab.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QAbstractItemView>
#include <QEvent>
#include <QButtonGroup>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QIcon>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QWidgetAction>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPoint>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/ClothControls.h"
#include "app/ExportNotifier.h"
#include "app/ExportSettings.h"
#include "app/ExportReport.h"
#include "app/SetListOptions.h"   // which sets the Set list offers
#include "app/SetsExportPlan.h"   // what "Export all sets…" writes
#include "model/ExportExtras.h"
#include "util/AnimExportScope.h"
#include "util/ExportLayout.h"
#include "util/NameTemplate.h"
#include "app/AppPaths.h"
#include "app/SehGuard.h"
#include "index/NameTranslator.h"
#include "util/HoverInfo.h"
#include "model/ModelResolve.h"
#include "store/Zzz4.h"
#include "tex/TextureDecode.h"
#include "tex/TextureParser.h"
#include "util/CameraPanel.h"   // shared Camera popover (both 3D tabs)
#include "util/PanelPersist.h"
#include "tabs/BrowserTab.h"    // kIconBtnQss
#include "tabs/HintBar.h"       // makeHintBar
#include "tabs/PanelBox.h"      // PanelBox, panelBoxArrive
#include "tabs/ViewGlyphs.h"    // shadeBallGlyph, panelGlyph, camera/light/cloth

namespace {

const char* kSlotLabels[WardrobeTab::kSlots] = {
    "Helmet", "Chest", "Shoulders", "Legs", "Hair",
    "Back weapon", "Main hand", "Off hand", "Attachment", "Other"};

// DI clips are authored at 30 fps (measured — same constant the Models tab
// transport uses), so one frame is this many milliseconds.
constexpr float kFrameMs = 1000.0f / 30.0f;

// Where the export prompts OPEN. Deliberately not "export straight to here":
// the dialog still appears and is still confirmed — it just starts where the
// last one finished instead of at whatever the process considers its working
// directory, which is almost never where you want to be.
const char kWardrobeDirKey[] = "wardrobe/lastExportDir";

// The six TEXTURE PREVIEW tiles, in panel order.
const char* const kTexChan[6] = {"COLOR", "ROUGHNESS", "METAL",
                                 "NORMAL", "ALPHA", "EMISSIVE"};

// The rig node that holds a weapon of type <t>: main hand "zhushou_<t>"
// (child of Bip001 R Hand), off hand "fushou_<t>" (Bip001 L Hand) — measured
// on f_barbarian, and the clips animate these very names.
const char* kHandPrefix[2] = {"zhushou_", "fushou_"};
// Generic holders, used when a weapon type has no node of its own — measured
// at the same hand position as the typed ones, and animated by every clip.
const char* kGenericHolder[2] = {"R_weapon", "L_weapon"};

// base body piece tokens, one per body slot (measured: f_barbarian ships
// lian/bozi/yanqiu/jiemao defaults; barbarian adds huzi beards; wizard-style
// classes ship none — their body is baked into the armor pieces)
const char* kBodyTokens[WardrobeTab::kBodySlots] = {
    "_lian_", "_bozi_", "_yanqiu_", "_jiemao_", "_huzi_"};
const char* kBodyLabels[WardrobeTab::kBodySlots] = {
    "face", "neck", "eyes", "lashes", "beard"};

// Decoded textures, shared across an export run and keyed by BLOB id (not by
// piece): the same map is reachable from several pieces — a set's left/right
// shoulder pair, the _ext/_int variants of one garment, a class's shared body
// maps — and decoding a 2K BC-compressed texture is the single most expensive
// thing a bulk export does. QImage is implicitly shared, so a hit hands back a
// refcount bump rather than a copy.
//
// Correctness notes, since this is read from several export lanes at once:
//  * a miss decodes OUTSIDE the lock. Two lanes can therefore decode the same
//    blob simultaneously; the second insert simply wins and the loser's copy is
//    dropped. Holding the lock across a decode would serialise the whole run,
//    which is the opposite of the point.
//  * over budget the whole map is dropped rather than evicted one entry at a
//    time. Already-handed-out QImages stay valid (refcounted), so a flush costs
//    future hits, never correctness.
struct TexCache {
    std::mutex mtx;
    std::unordered_map<size_t, QImage> map;
    qint64 bytes = 0;
    int hits = 0, misses = 0, flushes = 0;
    // Decoded pixels held at once. 384 MB is roughly 24 uncompressed 2K RGBA
    // maps — comfortably more than one set needs, far less than a whole class.
    static constexpr qint64 kBudget = 384ll << 20;

    QImage get(const di::DiAssetStore& store, size_t blob)
    {
        if (blob == (size_t)-1) return {};
        {
            std::lock_guard<std::mutex> g(mtx);
            auto it = map.find(blob);
            if (it != map.end()) { ++hits; return it->second; }
            ++misses;
        }
        const std::vector<uint8_t> t = store.mpk().readAsset(blob);
        di::Texture2D tex;
        std::string terr;
        if (t.empty() || !di::isTexture2D(t.data(), t.size()) ||
            !di::parseTexture2D(t.data(), t.size(), &tex, &terr))
            return {};
        QImage img = TextureDecode::decode(tex).image;
        std::lock_guard<std::mutex> g(mtx);
        // Another lane may have decoded the same blob while this one was
        // working (that is the accepted cost of decoding outside the lock).
        // Keep ITS copy and charge nothing: adding this image's bytes to a map
        // that did not grow would drive `bytes` far above what is resident and
        // flush a cache that is mostly empty — turning the shared cache into a
        // slowdown, which is the exact opposite of the point.
        auto ins = map.emplace(blob, img);
        if (!ins.second) return ins.first->second;
        const qint64 sz = img.isNull() ? 0 : (qint64)img.sizeInBytes();
        if (bytes + sz > kBudget) {
            map.clear();
            bytes = 0;
            ++flushes;
            map.emplace(blob, img);   // keep the one just decoded
        }
        bytes += sz;
        return img;
    }
};

// The opposite-gender twin of one repository entry, or -1. DI names cosmetics
// "<g>_<class>_<slot>_<set>", so the twin is a rename. Free-standing (rather
// than only a WardrobeTab method) because the bulk export's lanes run on raw
// threads and must not read GUI members like m_idx while the GUI can reassign
// them; they hold their own store pointer and call this.
int twinOfRepoEntry(const di::Repository& repo, int repoIdx)
{
    if (repoIdx < 0 || (size_t)repoIdx >= repo.entries.size()) return -1;
    const QString name = QString::fromStdString(repo.entries[(size_t)repoIdx].name);
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    const QString stem = slash >= 0 ? name.mid(slash + 1) : name;
    QString twin = stem;
    if (twin.startsWith(QStringLiteral("f_")))      twin[0] = QLatin1Char('m');
    else if (twin.startsWith(QStringLiteral("m_"))) twin[0] = QLatin1Char('f');
    else return -1;                       // genderless (shared item weapons)
    if (twin == stem) return -1;
    // Exact-name lookup through the repository's name index. A linear sweep of
    // 551,524 entries per piece put ~14M string compares on the caller.
    const size_t hit = repo.findByNameType(twin.toStdString(), "Model");
    return hit == (size_t)-1 ? -1 : (int)hit;
}

// The other gender's folder for a class folder ("Char/f_barbarian" ->
// "Char/m_barbarian"), for either naming convention DI uses. Empty when the
// folder carries no gender token — item-folder weapons are genderless.
QString twinClassFolder(const QString& folder)
{
    QString f = folder;
    const int slash = f.lastIndexOf(QLatin1Char('/'));
    QString leaf = slash >= 0 ? f.mid(slash + 1) : f;
    const QString head = slash >= 0 ? f.left(slash + 1) : QString();
    if (leaf.startsWith(QStringLiteral("f_")))      leaf[0] = QLatin1Char('m');
    else if (leaf.startsWith(QStringLiteral("m_"))) leaf[0] = QLatin1Char('f');
    else if (leaf.endsWith(QStringLiteral("_f")))   leaf[leaf.size() - 1] = QLatin1Char('m');
    else if (leaf.endsWith(QStringLiteral("_m")))   leaf[leaf.size() - 1] = QLatin1Char('f');
    else return {};
    return head + leaf;
}

// Parse one class rig — bone hierarchy + local rest transforms — out of that
// class's "<folder>/<leaf>.skeleton". Worker-safe. Returns false (leaving both
// outputs empty) when the class ships no skeleton or it fails to parse.
//
// This exists because an opposite-gender export needs the TWIN's rig: the two
// skeletons have different proportions, and writing a female garment against
// the male hierarchy produces a file whose bind pose does not match the mesh.
bool loadClassRig(const di::DiAssetStore& store, const std::string& classFolder,
                  di::BoneParents* hier, di::BoneLocals* locals)
{
    if (!hier || !locals || classFolder.empty()) return false;
    hier->clear();
    locals->clear();
    const size_t slash = classFolder.find_last_of('/');
    const std::string leaf =
        slash == std::string::npos ? classFolder : classFolder.substr(slash + 1);
    const size_t id =
        store.mpk().find("Package/" + classFolder + "/" + leaf + ".skeleton");
    if (id == (size_t)-1) return false;
    const std::vector<uint8_t> raw = store.mpk().readAsset(id);
    std::string herr;
    if (!di::parseSkeletonHierarchy(raw.data(), raw.size(), hier, &herr, locals)) {
        qWarning("wardrobe: %s .skeleton parse failed (%s)", classFolder.c_str(),
                 herr.c_str());
        hier->clear();
        locals->clear();
        return false;
    }
    return true;
}

// Build one export part straight from a repository entry — mesh, skin and
// (optionally) textures. Worker-safe: it touches only the store, never GUI
// state. Used by the opposite-gender twin export, which has no loaded slots to
// read from because the twin was never equipped, and by the bulk set export.
// `cache`, when given, shares decoded maps across the whole run.
bool buildPartFromRepo(const di::DiAssetStore& store, int repoIdx,
                       const QString& name, bool wantTex,
                       GlbExporter::Part* out, MeshTextures* outTex,
                       TexCache* cache = nullptr)
{
    if (repoIdx < 0 || !out) return false;
    const di::ResolvedModel res = di::resolveModelChain(store, repoIdx);
    if (res.meshBlob == (size_t)-1) return false;
    const std::vector<uint8_t> raw = store.mpk().readAsset(res.meshBlob);
    auto m = std::make_shared<di::MeshData>();
    std::string merr;
    if (!di::parseMesh(raw.data(), raw.size(), m.get(), &merr)) return false;
    out->mesh = m;
    out->name = name;
    if (res.skinBlob != (size_t)-1) {
        const std::vector<uint8_t> sraw = store.mpk().readAsset(res.skinBlob);
        auto sk = std::make_shared<di::SkinSkeleton>();
        std::string serr;
        if (di::parseSkinSkeleton(sraw.data(), sraw.size(), sk.get(), &serr))
            out->skel = sk;
    }
    const auto decodeTex = [&](size_t blob) -> QImage {
        if (blob == (size_t)-1) return {};
        if (cache) return cache->get(store, blob);
        const std::vector<uint8_t> t = store.mpk().readAsset(blob);
        di::Texture2D tex;
        std::string terr;
        if (t.empty() || !di::isTexture2D(t.data(), t.size()) ||
            !di::parseTexture2D(t.data(), t.size(), &tex, &terr))
            return {};
        return TextureDecode::decode(tex).image;
    };
    MeshTextures mt;
    if (wantTex) {
        mt.diffuse  = decodeTex(res.texBlob);
        mt.normal   = decodeTex(res.nrmBlob);
        mt.mix      = decodeTex(res.mixBlob);
        mt.emissive = decodeTex(res.emiBlob);
    }
    if (outTex) *outTex = mt;
    out->textures = mt;
    return true;
}

// True when nm contains "<slotTok>_<sub>_" for any sub in subs — the piece is
// a SUB-ATTACHMENT of that slot, not the slot's main garment. Measured census
// (2026-08-02, all 20 class folders): yifu carries parts(585)/fujian(93)/
// back(56)/wing(6)/part(4)/suishi(2); toukui carries parts(455)/face(194)/
// maofa(108)/piaodai(27)/beard(20); jianjia parts(203)/back(16); tui parts(33).
// These used to sit in the main combos, where "yifu_back_K" alphabetically
// outranked "yifu_K" and the set picker equipped the back item as the chest.
bool isSubAttachment(const std::string& nm, const char* slotTok,
                     std::initializer_list<const char*> subs)
{
    const size_t at = nm.find(slotTok);
    if (at == std::string::npos) return false;
    const size_t rest = at + std::strlen(slotTok);
    for (const char* sub : subs) {
        const size_t sl = std::strlen(sub);
        if (nm.compare(rest, sl, sub) == 0)
            return true;
    }
    return false;
}

// slot for a lowercase model name; -1 = skip (lod variant)
int slotOf(const std::string& nm)
{
    if (nm.find("_lod") != std::string::npos) return -1;
    static const char* weaponToks[] = {"_wuqi", "_danshou", "_liandao", "_lianren",
                                       "_lianchui", "_fazhang", "_faqi", "_faqiu",
                                       "_quantao", "_txinggun", "_juanzhou",
                                       "_xianjian"};
    if (nm.find("_toukui") != std::string::npos)
        return isSubAttachment(nm, "_toukui_", {"parts_", "face_", "maofa_",
                                                "piaodai_", "beard_"})
                   ? WardrobeTab::kAttachSlot : 0;
    if (nm.find("_yifu") != std::string::npos)
        return isSubAttachment(nm, "_yifu_", {"parts_", "part_", "fujian_",
                                              "back_", "wing_", "suishi_"})
                   ? WardrobeTab::kAttachSlot : 1;
    if (nm.find("_jianjia") != std::string::npos)
        return isSubAttachment(nm, "_jianjia_", {"parts_", "back_"})
                   ? WardrobeTab::kAttachSlot : 2;
    if (nm.find("_tui") != std::string::npos)
        return isSubAttachment(nm, "_tui_", {"parts_"})
                   ? WardrobeTab::kAttachSlot : 3;
    if (nm.find("_toufa") != std::string::npos)   return 4;
    for (const char* t : weaponToks)
        if (nm.find(t) != std::string::npos) return 5;   // back-worn cosmetic
    return 9;                                            // "Other"
}

QString slotDisplayName(int slot)
{
    if (slot < WardrobeTab::kSlots)
        return QString::fromLatin1(kSlotLabels[slot]);
    if (slot < WardrobeTab::kSlots + WardrobeTab::kBodySlots)
        return QStringLiteral("Body(%1)").arg(
            QString::fromLatin1(kBodyLabels[slot - WardrobeTab::kSlots]));
    return QStringLiteral("Attach(%1)")
        .arg(slot - WardrobeTab::kSlots - WardrobeTab::kBodySlots);
}

} // namespace

WardrobeTab::WardrobeTab(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    if (QWidget* hint = makeHintBar(
            this,
            QStringLiteral(
                "Tip: the panel strip on the right toggles INFO / MATERIALS / "
                "TEXTURE PREVIEW / PARTS / EQUIPPED / ANIMATIONS · EQUIPPED lists "
                "every part a set pulled in · wheel on the timeline steps one "
                "frame, Shift+wheel changes speed"),
            "hints/wardrobe"))
        lay->addWidget(hint);

    auto* split = new QSplitter(Qt::Horizontal, this);
    lay->addWidget(split, 1);

    auto* leftW = new QWidget(split);
    auto* left  = new QVBoxLayout(leftW);
    left->setContentsMargins(0, 0, 0, 0);
    auto* grid = new QGridLayout();
    grid->addWidget(new QLabel(QStringLiteral("Character:"), this), 0, 0);
    m_classBox = new QComboBox(this);
    m_classBox->setMaxVisibleItems(30);
    grid->addWidget(m_classBox, 0, 1, 1, 4);
    // Search over the character list. Forty rows (twenty classes x two genders)
    // is enough that "which row was the female crusader" is a scan every time,
    // and the combo's own type-to-search only matches from the front. This
    // matches ANY part of the label or of the folder leaf, so "cru", "_f" and
    // "crusader" all narrow it, and it hides the rows that do not match rather
    // than just highlighting one — the list you are looking at IS the result.
    m_classFilter = new QLineEdit(this);
    m_classFilter->setPlaceholderText(QStringLiteral("Search characters…"));
    m_classFilter->setClearButtonEnabled(true);
    m_classFilter->setToolTip(QStringLiteral(
        "Narrow the character list. Matches anywhere in the name, and in the "
        "folder leaf, so \"cru\", \"_f\" and \"crusader\" all work."));
    grid->addWidget(m_classFilter, 1, 1, 1, 4);
    grid->addWidget(new QLabel(QStringLiteral("Find:"), this), 1, 0);
    connect(m_classFilter, &QLineEdit::textChanged, this,
            [this](const QString& t) { applyClassFilter(t); });
    // Enter equips the first match, so narrowing and switching is one gesture
    // rather than "type, then open the list, then click".
    connect(m_classFilter, &QLineEdit::returnPressed, this, [this] {
        if (m_classFilterFirst >= 0 && m_classFilterFirst < m_classBox->count() &&
            m_classFilterFirst != m_classBox->currentIndex())
            m_classBox->setCurrentIndex(m_classFilterFirst);   // -> scanClass
    });
    // armor-set cycler: equips the matching helmet/chest/shoulders/legs at once
    grid->addWidget(new QLabel(QStringLiteral("Set:"), this), 2, 0);
    m_setBox = new QComboBox(this);
    m_setBox->setEnabled(false);
    m_setBox->setMaxVisibleItems(30);
    grid->addWidget(m_setBox, 2, 1);
    m_setPrev = new QPushButton(QStringLiteral("<"), this);
    m_setPrev->setFixedWidth(28);
    m_setPrev->setEnabled(false);
    grid->addWidget(m_setPrev, 2, 2);
    m_setNext = new QPushButton(QStringLiteral(">"), this);
    m_setNext->setFixedWidth(28);
    m_setNext->setEnabled(false);
    grid->addWidget(m_setNext, 2, 3);
    for (int s = 0; s < kSlots; ++s) {
        auto* slotLbl = new QLabel(QString::fromLatin1(kSlotLabels[s]) +
                                       QLatin1Char(':'), this);
        // The label is part of the row, so it opens the row's menu too.
        slotLbl->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(slotLbl, &QWidget::customContextMenuRequested, this,
                [this, s, slotLbl](const QPoint& p) {
                    showSlotMenu(s, slotLbl->mapToGlobal(p));
                });
        grid->addWidget(slotLbl, s + 3, 0);
        m_slotBox[s] = new QComboBox(this);
        m_slotBox[s]->setEnabled(false);
        m_slotBox[s]->setMaxVisibleItems(30);
        // type-to-search: substring completion over the piece names
        m_slotBox[s]->setEditable(true);
        m_slotBox[s]->setInsertPolicy(QComboBox::NoInsert);
        if (QCompleter* c = m_slotBox[s]->completer()) {
            c->setCompletionMode(QCompleter::PopupCompletion);
            c->setFilterMode(Qt::MatchContains);
            c->setCaseSensitivity(Qt::CaseInsensitive);
        }
        grid->addWidget(m_slotBox[s], s + 3, 1, 1, 3);   // spans the old < > Export columns
        // Right-click anywhere on the slot: export, variants, equip this
        // piece's set, lock, clear. Three fixed buttons per row bought four
        // actions and cost a third of the row's width on ten rows; a menu buys
        // all of them, names what it is acting on, and leaves the combo wide
        // enough to actually read a piece name.
        m_slotBox[s]->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_slotBox[s], &QWidget::customContextMenuRequested, this,
                [this, s](const QPoint& p) {
                    showSlotMenu(s, m_slotBox[s]->mapToGlobal(p));
                });
        m_slotBox[s]->setIconSize(QSize(ThumbnailProvider::kSize,
                                        ThumbnailProvider::kSize));
        // decode icons only when this dropdown actually opens (see eventFilter)
        m_slotBox[s]->view()->installEventFilter(this);
        // Dwell over a row in the open dropdown -> a big preview of that piece.
        // The combo icons are 40px; this is the one place you can actually SEE
        // a cosmetic before equipping it.
        m_slotHover[s] = new HoverPreview(
            m_slotBox[s]->view(),
            [this, s](const QModelIndex& idx, QList<HoverPreview::Line>* lines,
                      QImage* img) { return resolveSlotHover(s, idx, lines, img); },
            this);
        // Icons are fetched for the VISIBLE window only, so scrolling has to ask
        // again — otherwise everything past the first screenful stays blank.
        if (QScrollBar* sb = m_slotBox[s]->view()->verticalScrollBar())
            connect(sb, &QScrollBar::valueChanged, this,
                    [this, s](int) { requestThumbs(s); });
        // click-anywhere-to-open: an editable combo turns the whole body into
        // a text cursor and only the narrow arrow opens the list — which reads
        // as "the dropdown stopped working". The line-edit filter below
        // restores normal dropdown behaviour while keeping type-to-search.
        if (m_slotBox[s]->lineEdit()) {
            m_slotBox[s]->lineEdit()->installEventFilter(this);
            // The combo is EDITABLE, so its line edit covers most of the widget
            // and would answer a right-click with Qt's cut/copy/paste menu —
            // the slot menu would only appear on the narrow arrow. Route the
            // line edit to the same handler so the whole row behaves alike.
            m_slotBox[s]->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(m_slotBox[s]->lineEdit(), &QWidget::customContextMenuRequested,
                    this, [this, s](const QPoint& p) {
                        showSlotMenu(s, m_slotBox[s]->lineEdit()->mapToGlobal(p));
                    });
        }
        // Per-slot lock. Cycling sets is how you browse them, and the thing you
        // want to keep while browsing — the helmet you already picked, the
        // weapon you like — used to be replaced by every step. A locked slot is
        // skipped by the set picker and the equip-theme menu; choosing a piece
        // in the combo yourself still works, because that is you acting on the
        // slot rather than a set acting on it.
        //
        // The gender switch deliberately still moves a locked slot: it re-equips
        // the SAME piece on the other rig, which is preserving the slot, not
        // overwriting it. Skipping it there would leave the wrong gender's mesh
        // equipped — the one outcome a lock should never produce.
        m_slotLock[s] = new QPushButton(this);
        m_slotLock[s]->setCheckable(true);
        m_slotLock[s]->setFixedSize(24, 22);
        m_slotLock[s]->setIconSize(QSize(14, 14));
        m_slotLock[s]->setIcon(QIcon(lockGlyph(false)));
        m_slotLock[s]->setStyleSheet(QLatin1String(kPushBtnQss));
        m_slotLock[s]->setToolTip(QStringLiteral(
            "Lock this slot — the set picker and the equip-theme menu leave it "
            "alone. Choosing a piece here yourself still works, and a gender "
            "switch still re-equips this piece on the other rig."));
        // The icon carries the state, not just the button's checked colour: on a
        // 22px square that colour alone is easy to miss across ten rows.
        connect(m_slotLock[s], &QPushButton::toggled, this, [this, s](bool on) {
            m_slotLock[s]->setIcon(QIcon(lockGlyph(on)));
        });
        grid->addWidget(m_slotLock[s], s + 3, 4);
        connect(m_slotBox[s], qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, s](int idx) {
                    if (idx <= 0 || !m_slotRepoIdx ||
                        s >= (int)m_slotRepoIdx->size() ||
                        idx - 1 >= (*m_slotRepoIdx)[s].size()) {
                        ++m_slotSeq[s];   // drop any in-flight load for this slot
                        m_slotLoading[s] = false;
                        m_slots[s] = SlotState();
                        clearPartVisibility(s);
                        m_view->setPart(s, ViewPart());
                        refreshView();
                        return;
                    }
                    // The menu reads live state (m_slots[s]), so there is
                    // nothing to disable during the load window — a variant
                    // action can only ever act on what is actually loaded.
                    loadPiece(s, (*m_slotRepoIdx)[s][idx - 1]);
                });
    }
    grid->setColumnStretch(1, 1);
    left->addLayout(grid);
    auto* bodyRow = new QHBoxLayout();
    m_body = new QCheckBox(QStringLiteral("Base body (face/eyes/neck)"), this);
    m_body->setEnabled(false);
    // Persisted like every other view toggle — it used to reset each launch.
    // Set before the handler is wired so applyBodyToggle can't run pre-scan.
    m_body->setChecked(
        QSettings().value(QStringLiteral("wardrobe/view/body"), false).toBool());
    bodyRow->addWidget(m_body);
    m_exportOutfit = new QPushButton(QStringLiteral("Export outfit .glb…"), this);
    m_exportOutfit->setEnabled(false);
    bodyRow->addWidget(m_exportOutfit);
    m_exportSets = new QPushButton(QStringLiteral("Export all sets…"), this);
    m_exportSets->setEnabled(false);
    m_exportSets->setToolTip(
        QStringLiteral("Write one .glb per armor set of this class into a folder"));
    bodyRow->addWidget(m_exportSets);
    // Exports exactly the rows in the Set matches list — this set's
    // alternatives, one .glb each. Its label carries the count (syncMatchButton)
    // because a button next to a visible list must not do something of a
    // different size than the list.
    m_exportMatches = new QPushButton(QStringLiteral("Export matches…"), this);
    m_exportMatches->setEnabled(false);
    bodyRow->addWidget(m_exportMatches);
    bodyRow->addStretch(1);
    left->addLayout(bodyRow);
    // m_info now heads the right-hand panel column (Models-tab parity) rather
    // than the equipment grid — it is a load/status readout, not a control.
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_infoText.attach(m_info);
    // Every piece carrying the current set key. A set often ships more than one
    // candidate per slot and the combo can only show the one the picker chose.
    m_matchList = new QListWidget(this);
    m_matchList->setAlternatingRowColors(true);
    // No maximum height. It had one of 150px, which with 40px thumbnail rows
    // showed three of them — and the whole point of the list is comparing a
    // set's alternatives against each other. It now takes the slack the left
    // column has going spare (see the stretch on left->addWidget below).
    m_matchList->setMinimumHeight(180);
    m_matchList->setVisible(false);
    m_matchList->setToolTip(QStringLiteral(
        "Every piece of this class carrying the selected set key. The bold green "
        "row is the one the set picker chose for that slot; click any other to "
        "equip it instead. Right-click to export without equipping."));
    // Same dwell preview as the slot dropdowns, resolved through the slot/row
    // pair each item already carries.
    m_matchHover = new HoverPreview(
        m_matchList,
        [this](const QModelIndex& idx, QList<HoverPreview::Line>* lines,
               QImage* img) { return resolveMatchHover(idx, lines, img); },
        this);
    m_matchList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_matchList, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& p) {
                QListWidgetItem* it = m_matchList->itemAt(p);
                if (!it) return;
                const int slot = it->data(Qt::UserRole).toInt();
                const int row  = it->data(Qt::UserRole + 1).toInt();
                showMatchMenu(slot, row, m_matchList->viewport()->mapToGlobal(p));
            });
    connect(m_matchList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* it) {
                if (!it) return;
                const int slot = it->data(Qt::UserRole).toInt();
                const int row  = it->data(Qt::UserRole + 1).toInt();
                if (slot < 0 || slot >= kSlots || !m_slotBox[slot]) return;
                if (row > 0 && row < m_slotBox[slot]->count() &&
                    row != m_slotBox[slot]->currentIndex())
                    m_slotBox[slot]->setCurrentIndex(row);   // triggers loadPiece
                // Re-mark which row is now the equipped one.
                const int si = m_setBox ? m_setBox->currentIndex() : 0;
                if (si > 0 && si - 1 < m_setKeys.size())
                    buildMatchList(m_setKeys[si - 1]);
            });
    m_matchLabel = new QLabel(QStringLiteral("Set matches:"), this);
    m_matchLabel->setVisible(false);
    left->addWidget(m_matchLabel);
    // The list takes the column's spare height WHEN IT IS SHOWN, and a trailing
    // spacer takes it when it is not. Both are needed, and exactly one of them
    // may hold the stretch at a time — see setMatchListVisible for why.
    left->addWidget(m_matchList, 1);
    left->addStretch(0);
    m_leftCol      = left;
    m_matchListRow = left->indexOf(m_matchList);
    m_matchFillRow = m_matchListRow + 1;   // the trailing spacer
    setMatchListVisible(false);
    split->addWidget(leftW);

    auto* rightW = new QWidget(split);
    auto* right  = new QVBoxLayout(rightW);
    right->setContentsMargins(0, 0, 0, 0);
    auto* controls = new QHBoxLayout();
    // Shading balls (D4 / Blender) — parity with the Models tab viewport.
    {
        const int mode = qBound(0,
            QSettings().value(QStringLiteral("wardrobe/view/shadeMode"), 3).toInt(), 3);
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
            QSettings().setValue(QStringLiteral("wardrobe/view/shadeMode"), id);
            m_view->setShadingMode(id);
        });
    }
    // ── Channel dropdown (D4 "shadeMore"): render one raw material channel
    //    instead of the lit result. Wheel over the button cycles channels.
    {
        m_shadeMoreBtn = new QToolButton(this);
        m_shadeMoreBtn->setAutoRaise(true);
        m_shadeMoreBtn->setFixedWidth(16);
        m_shadeMoreBtn->setToolTip(QStringLiteral("Material channel to display"));
        m_shadeMoreBtn->setStyleSheet(QLatin1String(kIconBtnQss));
        m_shadeMoreBtn->setPopupMode(QToolButton::InstantPopup);
        auto* chMenu = new QMenu(m_shadeMoreBtn);
        auto* chRow = new QWidget(chMenu);
        auto* chLay = new QHBoxLayout(chRow);
        chLay->setContentsMargins(8, 4, 8, 4);
        chLay->addWidget(new QLabel(QStringLiteral("Channel"), chRow));
        m_channelCombo = new QComboBox(chRow);
        m_channelCombo->addItems({QStringLiteral("Shaded"), QStringLiteral("Base Color"),
                                  QStringLiteral("Normal"), QStringLiteral("Roughness"),
                                  QStringLiteral("Metallic"), QStringLiteral("AO"),
                                  QStringLiteral("Emissive")});
        m_channelCombo->setCurrentIndex(qBound(0,
            QSettings().value(QStringLiteral("wardrobe/view/channel"), 0).toInt(), 6));
        chLay->addWidget(m_channelCombo, 1);
        auto* chAct = new QWidgetAction(chMenu);
        chAct->setDefaultWidget(chRow);
        chMenu->addAction(chAct);
        m_shadeMoreBtn->setMenu(chMenu);
        auto syncChannelBtn = [this] {
            m_shadeMoreBtn->setText(m_channelCombo->currentIndex() == 0
                                        ? QStringLiteral("⌄") : QStringLiteral("◆"));
        };
        connect(m_channelCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, syncChannelBtn](int ch) {
                    QSettings().setValue(QStringLiteral("wardrobe/view/channel"), ch);
                    if (m_view) m_view->setViewChannel(ch);
                    syncChannelBtn();
                });
        syncChannelBtn();
        m_shadeMoreBtn->installEventFilter(this);   // wheel cycles channels
        controls->addWidget(m_shadeMoreBtn);
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

    // ── Overlays ▾ — skeleton / grid / culling / alpha / FX, grouped exactly
    //    as the Models tab groups them (D4 keeps these out of the toolbar).
    {
        QVBoxLayout* ovl = nullptr;
        auto* ovBtn = makeDrop(QStringLiteral("Overlays ▾"), ovl);
        ovBtn->setIcon(QIcon(overlayGlyph()));
        ovBtn->setIconSize(QSize(18, 18));
        ovBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        controls->addWidget(ovBtn);
        QWidget* oh = ovl->parentWidget();
        QSettings settings;
        m_bones = new QCheckBox(QStringLiteral("Skeleton (bones)"), oh);
        m_grid  = new QCheckBox(QStringLiteral("Ground grid"), oh);
        m_cull  = new QCheckBox(QStringLiteral("Cull backfaces"), oh);
        m_alphaBox = new QCheckBox(QStringLiteral("Alpha (cutout transparency)"), oh);
        m_alphaBox->setToolTip(QStringLiteral(
            "Alpha transparency: cut out see-through texture areas (hair, lashes, "
            "cloth fringes) instead of drawing them as solid quads."));
        // Material emissive glow: the equipped pieces' own authored animated
        // emissive layer (arcane scroll / star sparkle / fresnel rim). Normal
        // material rendering; the particle-effect experiment was removed (see
        // FX_ARCHIVE.md).
        m_fxBox = new QCheckBox(QStringLiteral("FX (emissive glow)"), oh);
        m_fxBox->setToolTip(QStringLiteral(
            "Show each piece's authored animated emissive glow "
            "(arcane scroll, star sparkle, fresnel rim)."));
        // Persisted state is applied BEFORE the toggled handlers are wired, so
        // no lambda fires while m_view is still null.
        m_bones->setChecked(
            settings.value(QStringLiteral("wardrobe/view/bones"), false).toBool());
        m_grid->setChecked(
            settings.value(QStringLiteral("wardrobe/view/grid"), true).toBool());
        m_cull->setChecked(
            settings.value(QStringLiteral("wardrobe/view/cull"), false).toBool());
        m_alphaBox->setChecked(
            settings.value(QStringLiteral("wardrobe/view/alpha"), false).toBool());
        m_fxBox->setChecked(
            settings.value(QStringLiteral("wardrobe/view/fx"), true).toBool());
        for (QCheckBox* c : {m_bones, m_grid, m_cull, m_alphaBox, m_fxBox})
            ovl->addWidget(c);
    }

    // ── Cloth — soft-body physics popover (shared params with the Models tab
    //    and with .glb export, so all three agree).
    {
        QVBoxLayout* clv = nullptr;
        m_clothBtn = makeDrop(QStringLiteral("Cloth"), clv);
        m_clothBox = new QCheckBox(QStringLiteral("Cloth physics"), clv->parentWidget());
        m_clothBox->setToolTip(QStringLiteral(
            "Simulate soft-body bones (cape / tail / hair) so they follow the body "
            "and sway, instead of floating at bind pose. Also baked into exports."));
        m_clothBox->setChecked(cloth::enabled());
        clv->addWidget(m_clothBox);
        auto* clothCfg = new QPushButton(QStringLiteral("Tune…"), clv->parentWidget());
        clothCfg->setToolTip(QStringLiteral("Tune the soft-body solver."));
        clothCfg->setStyleSheet(QLatin1String(kPushBtnQss));
        clv->addWidget(clothCfg);
        connect(m_clothBox, &QCheckBox::toggled, this, [this](bool on) {
            cloth::setEnabled(on);
            onClothToggled();
        });
        connect(clothCfg, &QPushButton::clicked, this, [this] {
            cloth::showTuningDialog(this, [this] { if (clothOn()) onClothToggled(); });
        });
    }

    // ── Lighting — exposure (the shared global GLMeshView key).
    {
        QVBoxLayout* ll = nullptr;
        m_lightBtn = makeDrop(QStringLiteral("Lighting"), ll);
        ll->addWidget(new QLabel(QStringLiteral("Exposure"), ll->parentWidget()));
        auto* expo = new QSlider(Qt::Horizontal, ll->parentWidget());
        expo->setRange(20, 400);
        expo->setValue(qBound(20,
            int(QSettings().value(QStringLiteral("view/exposure"), 1.0).toFloat()
                * 100.0f), 400));
        connect(expo, &QSlider::valueChanged, this, [this](int v) {
            QSettings().setValue(QStringLiteral("view/exposure"), v / 100.0);
            if (m_view) m_view->update();
        });
        ll->addWidget(expo);
    }
    controls->addStretch(1);
    right->addLayout(controls);

    m_view = new GLMeshView(this);
    m_view->setMinimumWidth(420);
    // ── Camera — the shared panel (util/CameraPanel.h). Built HERE, after the
    //    view exists, because CameraPanel seeds its readouts from the live
    //    camera. The Models tab builds the identical panel; the only difference
    //    is the settings prefix and the auto-frame wording. Its popover button
    //    is reparented onto the viewport N-strip further down.
    {
        QVBoxLayout* cl2 = nullptr;
        // The gizmo persists the projection it toggles under this prefix,
        // so it can only ever write THIS tab's key.
        m_view->setSettingsPrefix(QStringLiteral("wardrobe"));
        m_camBtn = makeDrop(QStringLiteral("Camera"), cl2);
        const CameraPanel::Widgets cw = CameraPanel::build(
            cl2, m_view, QStringLiteral("wardrobe"),
            QStringLiteral("Auto-frame on equip"));
        m_turntable = cw.turntable;
        m_frameBtn  = cw.frameBtn;
    }
    {   // apply every persisted view setting once, now that the view exists
        QSettings settings;
        m_view->setAlpha(m_alphaBox->isChecked());
        m_view->setFxVisible(m_fxBox->isChecked());
        m_view->setShowSkeleton(m_bones->isChecked());
        m_view->setShowGrid(m_grid->isChecked());
        m_view->setBackfaceCull(m_cull->isChecked());
        m_view->setTurntable(m_turntable->isChecked());
        m_view->setViewChannel(m_channelCombo->currentIndex());
        m_view->setAutoFrame(
            settings.value(QStringLiteral("wardrobe/autoFrame"), true).toBool());
        m_view->setShadingMode(qBound(0,
            settings.value(QStringLiteral("wardrobe/view/shadeMode"), 3).toInt(), 3));
        m_view->setTurntableSpeed(float(qBound(5,
            settings.value(QStringLiteral("wardrobe/view/turntableSpeed"), 30)
                .toInt(), 180)));
        CameraPanel::applyStartupState(m_view, QStringLiteral("wardrobe"));
    }
    right->addWidget(m_view, 1);

    // ── Transport row, BELOW the viewport (D4 layout, Models-tab parity) ────
    auto* animRow = new QHBoxLayout();
    animRow->addWidget(new QLabel(QStringLiteral("Anim:"), this));
    m_animBox = new QComboBox(this);
    m_animBox->setEnabled(false);
    m_animBox->setMinimumWidth(160);
    animRow->addWidget(m_animBox, 1);
    m_stepBack = new QPushButton(QStringLiteral("<"), this);
    m_stepBack->setFixedWidth(26);
    m_stepBack->setEnabled(false);
    m_stepBack->setToolTip(QStringLiteral("Step one frame back (pauses)"));
    animRow->addWidget(m_stepBack);
    m_playBtn = new QPushButton(QStringLiteral("Pause"), this);
    m_playBtn->setFixedWidth(52);
    m_playBtn->setEnabled(false);
    animRow->addWidget(m_playBtn);
    m_stepFwd = new QPushButton(QStringLiteral(">"), this);
    m_stepFwd->setFixedWidth(26);
    m_stepFwd->setEnabled(false);
    m_stepFwd->setToolTip(QStringLiteral("Step one frame forward (pauses)"));
    animRow->addWidget(m_stepFwd);
    m_timeSlider = new QSlider(Qt::Horizontal, this);
    m_timeSlider->setEnabled(false);
    m_timeSlider->setToolTip(QStringLiteral(
        "Scrub · wheel steps one frame · Shift+wheel changes speed"));
    m_timeSlider->installEventFilter(this);
    animRow->addWidget(m_timeSlider, 2);
    m_frameSpin = new QSpinBox(this);
    m_frameSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_frameSpin->setAlignment(Qt::AlignRight);
    m_frameSpin->setFixedWidth(52);
    m_frameSpin->setEnabled(false);
    m_frameSpin->setToolTip(QStringLiteral("Current frame — type to jump"));
    animRow->addWidget(m_frameSpin);
    m_frameMax = new QLabel(QStringLiteral("/ 0"), this);
    animRow->addWidget(m_frameMax);
    m_timeLabel = new QLabel(QStringLiteral("0.00 / 0.00 s"), this);
    m_timeLabel->setStyleSheet(QStringLiteral("color:#9a9a9a;"));
    animRow->addWidget(m_timeLabel);
    m_speedBox = new QComboBox(this);
    for (const auto& sp : {std::pair<const char*, float>{"0.25x", 0.25f},
                           {"0.5x", 0.5f}, {"1x", 1.0f}, {"1.5x", 1.5f},
                           {"2x", 2.0f}})
        m_speedBox->addItem(QLatin1String(sp.first), sp.second);
    m_speedBox->setCurrentIndex(2);
    m_speedBox->setToolTip(QStringLiteral("Playback speed"));
    animRow->addWidget(m_speedBox);
    m_loop = new QCheckBox(QStringLiteral("Loop"), this);
    m_loop->setChecked(true);
    animRow->addWidget(m_loop);
    right->addLayout(animRow);
    split->addWidget(rightW);

    // ── Viewport N-strip (D4): Camera / Lighting / Cloth popover icons on the
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
    {   // reparent the Camera / Lighting / Cloth popover buttons onto the strip
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

    // ── RIGHT: D4-style panel strip + PanelBox splitter ────────────────────
    auto* farW = new QWidget(split);
    m_farW = farW;
    auto* far = new QVBoxLayout(farW);
    far->setContentsMargins(0, 0, 0, 0);
    far->setSpacing(3);
    far->addWidget(m_info);             // load/status readout heads the column
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

    buildPanelPages(farW);              // creates the six panel bodies
    addRightPage(QStringLiteral("INFO"), m_infoBox);
    addRightPage(QStringLiteral("MATERIALS"), m_matBox);
    addRightPage(QStringLiteral("TEXTURE PREVIEW"), m_texPrevBox);
    addRightPage(QStringLiteral("PARTS"), m_partsBox);
    addRightPage(QStringLiteral("EQUIPPED"), m_equipBox);
    addRightPage(QStringLiteral("ANIMATIONS"), m_animList);
    restorePanelLayout();

    split->setStretchFactor(0, 2);   // equipment grid
    split->setStretchFactor(1, 4);   // viewport
    split->setStretchFactor(2, 2);   // panels
    PanelPersist::bind(split, QStringLiteral("panels/wardrobeSplit3"));

    // View toggles — wired after m_view exists so the initial setChecked calls
    // above could not reach a null viewport.
    connect(m_bones, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe/view/bones"), on);
        m_view->setShowSkeleton(on);
    });
    connect(m_grid, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe/view/grid"), on);
        m_view->setShowGrid(on);
    });
    connect(m_cull, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe/view/cull"), on);
        m_view->setBackfaceCull(on);
    });
    connect(m_alphaBox, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe/view/alpha"), on);
        m_view->setAlpha(on);
    });
    connect(m_fxBox, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe/view/fx"), on);
        m_view->setFxVisible(on);
    });
    // Persisting the state is CameraPanel's job; driving the viewport is the
    // tab's, because the tab is also what suspends the spin during a capture.
    connect(m_turntable, &QCheckBox::toggled, this,
            [this](bool on) { m_view->setTurntable(on); });

    // Transport wiring
    connect(m_stepBack, &QPushButton::clicked, this, [this] { seekFrames(-1); });
    connect(m_stepFwd,  &QPushButton::clicked, this, [this] { seekFrames(+1); });
    connect(m_speedBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) { applySpeed(i); });
    connect(m_frameSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int f) {
                if (!m_clip) return;
                const float want = float(f) * kFrameMs;
                if (std::abs(want - m_animT) < 0.5f) return;   // echo of our own write
                if (m_animTimer && m_animTimer->isActive()) {
                    m_animTimer->stop();      // typing a frame implies pause, or
                    m_playBtn->setText(QStringLiteral("Play"));   // the next tick
                }                                                 // overwrites it
                m_animT = qBound(0.0f, want, float(m_clip->durationMs));
                animTick(m_animT);
            });
    // Double-click a piece in the viewport to select it in the PARTS panel.
    // partPicked (not submeshPicked) because a Wardrobe piece can be any part,
    // not just part 0.
    connect(m_view, &GLMeshView::partPicked, this, [this](int part, int sub) {
        if (part < 0 || part >= kParts || sub < 0 || !m_partsList) return;
        ensurePanelVisible(QStringLiteral("PARTS"));
        setFocusPart(part);
        for (int i = 0; i < m_partsList->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = m_partsList->topLevelItem(i);
            if (top->data(0, Qt::UserRole).toInt() != part) continue;
            QTreeWidgetItem* target = sub < top->childCount() ? top->child(sub) : top;
            top->setExpanded(true);
            m_partsList->setCurrentItem(target);
            m_partsList->scrollToItem(target);
            break;
        }
    });
    connect(m_body, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("wardrobe/view/body"), on);
        applyBodyToggle(on);
    });

    connect(m_setBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) { applySet(idx); });
    connect(m_setPrev, &QPushButton::clicked, this, [this] { stepSet(-1); });
    connect(m_setNext, &QPushButton::clicked, this, [this] { stepSet(+1); });
    // guard re-entry: two concurrent runs would interleave QFile writes into
    // identical paths (audit finding). BOTH buttons are disabled for either
    // run, because they write into the same folder and can name the same files.
    // They re-enable when the bulk worker finishes (or if no run started).
    const auto startBulk = [this](BulkMode mode) {
        m_exportSets->setEnabled(false);
        m_exportMatches->setEnabled(false);
        exportAllSets(mode);
        if (!m_bulkRunning) {
            m_exportSets->setEnabled(!m_setKeys.isEmpty());
            syncMatchButton();
        }
    };
    connect(m_exportSets, &QPushButton::clicked, this,
            [startBulk] { startBulk(BulkAllSets); });
    connect(m_exportMatches, &QPushButton::clicked, this,
            [startBulk] { startBulk(BulkListedMatches); });

    connect(this, &WardrobeTab::bulkProgress, this,
            [this](int generation, QString message) {
                if (generation != m_generation.load()) return;
                m_infoText.setBase(message);
            },
            Qt::QueuedConnection);

    // Anim picker doubles as a search box — a class ships hundreds of clips.
    m_animBox->setEditable(true);
    m_animBox->setInsertPolicy(QComboBox::NoInsert);
    if (QCompleter* c = m_animBox->completer()) {
        c->setCompletionMode(QCompleter::PopupCompletion);
        c->setFilterMode(Qt::MatchContains);
        c->setCaseSensitivity(Qt::CaseInsensitive);
    }
    if (m_animBox->lineEdit())
        m_animBox->lineEdit()->installEventFilter(this);   // click opens list

    connect(m_exportOutfit, &QPushButton::clicked, this, [this] {
        // Scope, hidden parts and texture embedding all come from Settings ▸
        // Export; the filename comes from the outfit template. Both are shared
        // with the bulk set export so the two cannot disagree.
        std::vector<GlbExporter::Part> parts = outfitParts();
        if (parts.empty()) {
            m_infoText.addNote(QStringLiteral("nothing equipped to export"));
            return;
        }
        exportGlb(outfitFileName(), std::move(parts));
    });

    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(33);
    connect(m_animTimer, &QTimer::timeout, this, [this] {
        if (!m_clip) return;
        m_animT += 33.0f * m_speed;
        if (m_animT > (float)m_clip->durationMs) {
            if (m_loop && m_loop->isChecked()) {
                m_animT = 0.0f;
            } else {                       // park on the last frame and stop
                m_animT = (float)m_clip->durationMs;
                m_animTimer->stop();
                if (m_playBtn) m_playBtn->setText(QStringLiteral("Play"));
            }
        }
        animTick(m_animT);
    });
    connect(m_animBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) { selectAnim(idx); });
    connect(m_playBtn, &QPushButton::clicked, this, [this] {
        if (!m_clip) return;
        if (m_animTimer->isActive()) {
            m_animTimer->stop();
            m_playBtn->setText(QStringLiteral("Play"));
        } else {
            // Pressing Play while parked on the last frame restarts the clip,
            // otherwise it would sit there doing nothing (Models-tab behaviour).
            if (m_animT >= float(m_clip->durationMs)) m_animT = 0.0f;
            m_animTimer->start();
            m_playBtn->setText(QStringLiteral("Pause"));
        }
    });
    connect(m_timeSlider, &QSlider::sliderMoved, this, [this](int v) {
        if (!m_clip) return;
        m_animT = (float)v;
        animTick(m_animT);
    });

    connect(m_classBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                if (idx < 0 || idx >= m_classFolders.size()) return;
                // Remember the look before the rig changes. Switching gender is
                // the common case and the pieces are the same cosmetics under a
                // different prefix, so the outfit can be carried across instead
                // of being silently emptied.
                m_carryOver.clear();
                m_carryOverClass.clear();
                if (m_classBox->count() > 0) {
                    for (int s = 0; s < kSlots; ++s)
                        if (m_slotBox[s] && m_slotBox[s]->currentIndex() > 0)
                            m_carryOver.insert(s, m_slotBox[s]->currentText());
                    if (!m_carryOver.isEmpty() && !m_curClassFolder.isEmpty())
                        m_carryOverClass = m_curClassFolder;
                }
                scanClass(m_classFolders[idx]);
            });

    connect(this, &WardrobeTab::classScanned, this,
            [this](int generation,
                   std::shared_ptr<std::vector<QStringList>> slotNames,
                   std::shared_ptr<std::vector<QList<int>>> slotRepoIdx,
                   std::shared_ptr<QStringList> animNames,
                   std::shared_ptr<QList<quint32>> animIds,
                   std::shared_ptr<QList<int>> bodyRepoIdx,
                   std::shared_ptr<di::BoneParents> hier,
                   std::shared_ptr<di::BoneLocals> locals) {
                if (generation != m_generation.load()) return;
                m_slotRepoIdx = slotRepoIdx;
                m_bodyRepoIdx = bodyRepoIdx;
                m_hier   = (hier && !hier->empty()) ? hier : nullptr;
                m_locals = (locals && !locals->empty()) ? locals : nullptr;
                // fixed part structure so later slot changes are O(1) uploads
                m_view->setParts(std::vector<ViewPart>(kParts));
                int total = 0;
                for (int s = 0; s < kSlots; ++s) {
                    m_slotBox[s]->blockSignals(true);
                    m_slotBox[s]->clear();
                    m_slotBox[s]->addItem(QStringLiteral("(none)"));
                    for (const QString& n : (*slotNames)[s])
                        m_slotBox[s]->addItem(n);
                    m_slotBox[s]->blockSignals(false);
                    m_slotBox[s]->setEnabled(!(*slotNames)[s].isEmpty());
                    total += (*slotNames)[s].size();
                }
                m_animIds = animIds;
                m_animBox->blockSignals(true);
                m_animBox->clear();
                m_animBox->addItem(QStringLiteral("(bind pose)"));
                for (const QString& n : *animNames)
                    m_animBox->addItem(n);
                m_animBox->blockSignals(false);
                m_animBox->setEnabled(!animNames->isEmpty());
                syncAnimList();          // mirror the clips into the panel list
                int bodyPieces = 0;
                for (int b = 0; b < kBodySlots; ++b)
                    if ((*bodyRepoIdx)[b] >= 0) ++bodyPieces;
                m_body->setEnabled(bodyPieces > 0);
                if (m_body->isChecked() && bodyPieces > 0)
                    applyBodyToggle(true);
                m_exportOutfit->setEnabled(true);
                qInfo("wardrobe: %lld main-hand / %lld off-hand weapons listed",
                      (long long)(*slotNames)[kMainHand].size(),
                      (long long)(*slotNames)[kOffHand].size());
                // armor-set keys: "(sz|t|s)NN_NNN" tokens present in at least
                // two of the four armor slots
                {
                    // The key, plus the awakened tier when the piece carries
                    // one. An "_aw<N>" piece is a DIFFERENT set, not another
                    // form of the base one — it used to fold onto the base key
                    // and so could never be equipped as a set at all.
                    static const QRegularExpression re(QStringLiteral(
                        "_((?:sz|t|s)\\d+_\\d+(?:_aw\\d+)?)"));
                    QHash<QString, int> slotsOf;   // key -> bitmask of slots
                    for (int s = 0; s < 4; ++s)
                        for (const QString& n : (*slotNames)[s]) {
                            const auto m = re.match(n.toLower());
                            if (m.hasMatch())
                                slotsOf[m.captured(1)] |= (1 << s);
                        }
                    const SetList::Options setOpts = SetList::Options::load();
                    m_setKeys.clear();
                    int hiddenAw = 0;
                    QSet<int> tiersSeen;
                    for (auto it = slotsOf.begin(); it != slotsOf.end(); ++it) {
                        int bits = it.value(), cnt = 0;
                        while (bits) { cnt += bits & 1; bits >>= 1; }
                        if (cnt < 2) continue;   // a set fills at least two slots
                        const int tier = SetList::tierOf(it.key());
                        if (tier > 0) tiersSeen.insert(tier);
                        if (!setOpts.wantTier(tier)) { ++hiddenAw; continue; }
                        m_setKeys << it.key();
                    }
                    m_setKeys.sort();
                    if (!tiersSeen.isEmpty()) {
                        QStringList t;
                        for (int v : tiersSeen) t << QString::number(v);
                        t.sort();
                        qInfo("wardrobe: awakened tiers present: %s (%d set(s) "
                              "hidden by the Set list options)",
                              qPrintable(t.join(QLatin1Char('/'))), hiddenAw);
                    }
                    m_setBox->blockSignals(true);
                    m_setBox->clear();
                    m_setBox->addItem(QStringLiteral("(custom)"));
                    for (const QString& k : m_setKeys)
                        m_setBox->addItem(setLabel(k));   // real name when known
                    m_setBox->blockSignals(false);
                    const bool haveSets = !m_setKeys.isEmpty();
                    m_setBox->setEnabled(haveSets);
                    m_setPrev->setEnabled(haveSets);
                    m_setNext->setEnabled(haveSets);
                    // A different class means different sets, different
                    // pieces and different twins — every count is stale.
                    invalidateSetsCount();
                    syncExportButtons();
                    // Combos are populated now, so the look captured before the
                    // switch can be re-equipped on this rig.
                    applyCarryOver();
                    qInfo("wardrobe: %lld armor sets, %d body pieces",
                          (long long)m_setKeys.size(), bodyPieces);
                }
                {
                    QString counts;
                    for (int s = 0; s < kSlots; ++s)
                        counts += QStringLiteral("%1=%2 ")
                                      .arg(QLatin1String(kSlotLabels[s]))
                                      .arg((*slotNames)[s].size());
                    qInfo("wardrobe: combos %s anims=%lld", qPrintable(counts.trimmed()),
                          (long long)animNames->size());
                }
                m_infoText.setBase(
                    QStringLiteral("%L1 pieces across %2 slots · %L3 anims")
                        .arg(total).arg(kSlots).arg(animNames->size()));
                emit statusText(QStringLiteral("%L1 wardrobe pieces").arg(total));
            },
            Qt::QueuedConnection);

    connect(this, &WardrobeTab::pieceLoaded, this,
            [this](int generation, int slot, int seq, ViewPart part,
                   std::shared_ptr<di::SkinSkeleton> skel, QString note,
                   std::shared_ptr<QStringList> varNames,
                   std::shared_ptr<QList<int>> varRepoIdx,
                   std::shared_ptr<std::vector<float>> bindPose) {
                if (generation != m_generation.load()) return;
                if (slot < 0 || slot >= kParts) return;
                if (seq != m_slotSeq[slot].load()) return;   // superseded
                m_slotLoading[slot] = false;
                m_slots[slot].part = part;
                m_slots[slot].skel = std::move(skel);
                m_slots[slot].player.init(nullptr, nullptr);
                m_slots[slot].varNames   = varNames ? *varNames : QStringList();
                m_slots[slot].varRepoIdx = varRepoIdx ? *varRepoIdx : QList<int>();
                m_slots[slot].varIdx     = 0;
                m_slots[slot].bindPose   = bindPose ? *bindPose
                                                    : std::vector<float>();
                if (!note.isEmpty()) {
                    m_slots[slot].name = note.section(QLatin1Char('|'), 0, 0);
                    const QString msg = note.section(QLatin1Char('|'), 1);
                    if (!msg.isEmpty()) m_infoText.addNote(msg.toHtmlEscaped());
                    else {
                        // show the piece's decoded structural true name (the
                        // real-name override, when present, appears in the
                        // Meaning column of the asset tables)
                        const QString tn =
                            NameTranslator::cosmeticName(m_slots[slot].name);
                        if (!tn.isEmpty()) m_infoText.addNote(tn.toHtmlEscaped());
                    }
                }
                m_view->setPart(slot, std::move(part));   // O(1): only this
                                                          // slot re-uploads
                // a playing clip immediately drives the new piece too
                if (m_clip && m_slots[slot].skel) {
                    initSlotPlayer(slot);
                    animTick(m_animT);
                } else if (!m_slots[slot].bindPose.empty()) {
                    // hand weapon at rest: hold it at its hardpoint
                    m_view->setPartPose((size_t)slot,
                                        std::vector<float>(m_slots[slot].bindPose));
                }
                refreshView();
            },
            Qt::QueuedConnection);

    connect(this, &WardrobeTab::variantLoaded, this,
            [this](int generation, int slot, int seq, MeshTextures tex,
                   QString label) {
                if (generation != m_generation.load()) return;
                if (slot < 0 || slot >= kParts) return;
                if (seq != m_slotSeq[slot].load()) return;
                if (!m_slots[slot].part.mesh) return;
                m_slots[slot].part.textures = tex;
                m_view->setPart(slot, m_slots[slot].part);
                // setPart clears that part's pose — re-apply it, or a dye step
                // would throw a hand weapon back to the weapon-space origin
                // and un-pose a paused clip (audit finding).
                if (m_clip)
                    animTick(m_animT);
                else if (!m_slots[slot].bindPose.empty())
                    m_view->setPartPose((size_t)slot,
                                        std::vector<float>(m_slots[slot].bindPose));
                m_infoText.addNote(QStringLiteral("%1 variant: %2")
                                       .arg(m_slots[slot].name, label).toHtmlEscaped());
            },
            Qt::QueuedConnection);

    connect(this, &WardrobeTab::exportDone, this,
            [this](QString message) {
                m_infoText.setBase(message.toHtmlEscaped());
                qInfo("wardrobe glb: %s", qPrintable(message));
            },
            Qt::QueuedConnection);
}

WardrobeTab::~WardrobeTab()
{
    CameraPanel::rememberOnExit(m_view, QStringLiteral("wardrobe"));
    ++m_generation;
    const QSet<QThread*> workers = m_workers;
    for (QThread* w : workers)
        w->wait();
}

// The class token of the current selection ("barbarian" from "Char/f_barbarian")
// for narrowing a set's real name to a class-specific override.
QString WardrobeTab::currentClassName() const
{
    const int i = m_classBox ? m_classBox->currentIndex() : -1;
    if (i < 0 || i >= m_classFolders.size()) return {};
    static const QRegularExpression re(
        QStringLiteral("(bloodknight|demonhunter|necromancer|barbarian|crusader|"
                       "sorceress|wizard|tempest|warlock|druid|monk)"));
    const auto m = re.match(m_classFolders[i].toLower());
    return m.hasMatch() ? m.captured(1) : QString();
}

// Label for a set key in the picker: the real name when data\di_item_names.csv
// supplies one ("Dread Warlord (sz08_007)"), else the bare key.
QString WardrobeTab::setLabel(const QString& setKey) const
{
    QString real = ItemNames::nameForSetKey(m_names, setKey, currentClassName());
    // An awakened key is not in di_item_names.csv — that file is keyed by the
    // base set. Fall back to the base set's real name and say which tier this
    // is, rather than dropping to a bare "t07_004_aw3".
    const int tier = SetList::tierOf(setKey);
    if (real.isEmpty() && tier > 0)
        real = ItemNames::nameForSetKey(m_names, SetList::baseKeyOf(setKey),
                                        currentClassName());
    if (real.isEmpty()) return setKey;
    if (tier > 0)
        real += QStringLiteral(" · Awakened %1").arg(tier);
    return QStringLiteral("%1 (%2)").arg(real, setKey);
}

void WardrobeTab::setIndex(std::shared_ptr<AssetIndex> idx)
{
    ++m_generation;
    m_idx = std::move(idx);
    // real-name overrides for the set picker (same file the index uses)
    m_names = ItemNames::load(AppPaths::file(QStringLiteral("di_item_names.csv")));
    stopAnim();
    m_animIds.reset();
    m_clip.reset();
    m_animBox->blockSignals(true);
    m_animBox->clear();
    m_animBox->blockSignals(false);
    m_animBox->setEnabled(false);
    m_setKeys.clear();
    m_setBox->blockSignals(true);
    m_setBox->clear();
    m_setBox->blockSignals(false);
    m_setBox->setEnabled(false);
    m_setPrev->setEnabled(false);
    m_setNext->setEnabled(false);
    for (int s = 0; s < kParts; ++s) {
        ++m_slotSeq[s];
        m_slots[s] = SlotState();
        clearPartVisibility(s);
    }
    for (int s = 0; s < kSlots; ++s) {
        m_slotBox[s]->blockSignals(true);
        m_slotBox[s]->clear();
        m_slotBox[s]->blockSignals(false);
        m_slotBox[s]->setEnabled(false);
    }
    // The Set matches rows hold (slot, combo row) pairs into the OLD class's
    // piece lists. Left up, clicking one equips whatever now happens to sit at
    // that row of the new class — so the list goes with the combos it indexes.
    buildMatchList(QString());
    m_body->setEnabled(false);
    m_exportOutfit->setEnabled(false);
    m_exportSets->setEnabled(false);
    m_exportMatches->setEnabled(false);
    m_slotRepoIdx.reset();
    m_bodyRepoIdx.reset();
    m_hier.reset();
    m_locals.reset();
    m_view->clearMesh();
    m_infoText.clear();
    refreshView();      // panels must not keep listing the old store's pieces
    m_classFolders.clear();
    // Thumbnail provider is per-store; drop it BEFORE the old one joins its
    // workers, same order the Models tab uses.
    m_thumbs.reset();
    m_thumbs3d.reset();
    m_thumbSlot = -1;
    if (m_idx && m_idx->store) {
        // Decode/render at 128px so the combo icons stay crisp; both caches sit
        // in a fixed memory band regardless of that size.
        constexpr int kThumbPx = 128;
        m_thumbs = std::make_unique<ThumbnailProvider>(m_idx->store, kThumbPx);
        connect(m_thumbs.get(), &ThumbnailProvider::ready, this, [this] {
            if (!m_thumbs) return;   // a queued ready() can outlive a swap
            m_thumbs->commit();
            if (m_thumbSlot >= 0) requestThumbs(m_thumbSlot);
            refreshMatchIcons();
        });
        // 3D-rendered icons (opt-in): parses on worker threads, renders on the
        // GUI thread during commit(), same contract as the texture provider.
        m_thumbs3d = std::make_unique<ModelThumbRenderer>(m_idx->store, kThumbPx);
        connect(m_thumbs3d.get(), &ModelThumbRenderer::ready, this, [this] {
            if (!m_thumbs3d) return;
            m_thumbs3d->commit();
            if (m_thumbSlot >= 0) requestThumbs(m_thumbSlot);
            refreshMatchIcons();
        });
    }
    m_thumb3dActive = thumbs3dOn();
    m_classBox->blockSignals(true);
    m_classBox->clear();
    if (m_idx && m_idx->store) {
        // PLAYABLE classes only (user directive): depth-2 Char/<x> folders
        // with their own skeleton file AND >= 25 chest-armor (_yifu) Models —
        // measured on the live repository this keeps exactly the 20 class
        // rigs (10 classes x 2 genders, druid/warlock at 34-72 pieces, the
        // rest at 197-288) and drops all 114 monster/NPC rigs.
        const auto& entries = m_idx->store->mpk().entries();
        std::unordered_set<std::string> cand;
        for (const auto& e : entries) {
            const std::string& n = e.name;
            if (n.size() < 20 || n.compare(0, 13, "Package/Char/") != 0 ||
                n.compare(n.size() - 9, 9, ".skeleton") != 0)
                continue;
            const size_t slash = n.find('/', 13);
            if (slash == std::string::npos) continue;
            const std::string leaf = n.substr(13, slash - 13);
            if (n != "Package/Char/" + leaf + "/" + leaf + ".skeleton") continue;
            cand.insert("Char/" + leaf);
        }
        std::unordered_map<std::string, int> yifu;
        if (const di::Repository* repo = m_idx->store->repo()) {
            for (const di::RepoEntry& e : repo->entries) {
                if (repo->typeOf(e) != "Model") continue;
                const std::string& f = repo->folderOf(e);
                if (!cand.count(f)) continue;
                std::string lower = e.name;
                for (char& c : lower)
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (lower.find("_yifu") != std::string::npos &&
                    lower.find("_lod") == std::string::npos)
                    ++yifu[f];
            }
        }
        QStringList folders;
        for (const std::string& f : cand)
            if (yifu[f] >= 25)
                folders << QString::fromLatin1(f.c_str());
        // Sort by CLASS first, gender second, so the two rigs of one class sit
        // next to each other. A plain folder sort gives every female class then
        // every male one, which puts "Barbarian (F)" and "Barbarian (M)" ten
        // rows apart — the two entries you most often switch between.
        std::sort(folders.begin(), folders.end(),
                  [](const QString& a, const QString& b) {
                      const auto key = [](const QString& f) {
                          QString leaf = f.mid(5);          // drop "Char/"
                          QString g;
                          if (leaf.startsWith(QStringLiteral("f_")))      { g = QStringLiteral("F"); leaf = leaf.mid(2); }
                          else if (leaf.startsWith(QStringLiteral("m_"))) { g = QStringLiteral("M"); leaf = leaf.mid(2); }
                          else if (leaf.endsWith(QStringLiteral("_f")))   { g = QStringLiteral("F"); leaf.chop(2); }
                          else if (leaf.endsWith(QStringLiteral("_m")))   { g = QStringLiteral("M"); leaf.chop(2); }
                          return QPair<QString, QString>(leaf.toLower(), g);
                      };
                      return key(a) < key(b);
                  });
        m_classFolders = folders;
        // Friendly display ("Barbarian (F)") while selection stays keyed to the
        // folder BY INDEX (m_classFolders) — the stable identity, not the label.
        for (const QString& f : folders) {
            const QString leaf = f.mid(5);   // drop "Char/"
            const QString disp = NameTranslator::classDisplay(leaf);
            m_classBox->addItem(disp.isEmpty() ? leaf : disp);
        }
        qInfo("wardrobe: %lld playable classes (of %zu rigged folders)",
              (long long)folders.size(), cand.size());
    }
    m_classBox->blockSignals(false);
    emit statusText(QStringLiteral("%L1 characters").arg(m_classFolders.size()));
    if (!m_classFolders.isEmpty()) {
        // restore the last class by its STABLE identity (the folder string,
        // never a display label — D4 lesson: labels drift across patches);
        // fall back to a playable-looking rig
        const QString last =
            QSettings().value(QStringLiteral("wardrobe/lastClass")).toString();
        int def = last.isEmpty() ? -1 : (int)m_classFolders.indexOf(last);
        if (def < 0)   // default to f_barbarian by FOLDER identity, not label
            def = (int)m_classFolders.indexOf(QStringLiteral("Char/f_barbarian"));
        m_classBox->setCurrentIndex(def >= 0 ? def : 0);   // triggers scanClass
        if (m_classBox->currentIndex() == 0 && def <= 0)
            scanClass(m_classFolders[0]);                  // index 0 emits no change
    }
}

void WardrobeTab::scanClass(const QString& folder)
{
    m_curClassFolder = folder;
    if (!m_idx || !m_idx->store) return;
    QSettings().setValue(QStringLiteral("wardrobe/lastClass"), folder);
    const int generation = ++m_generation;
    // Close the stale-selection window NOW: until classScanned arrives, the
    // combos must not offer the previous class's pieces (audit finding — a
    // pick in that window would load an old-class mesh into the new rig).
    for (int s = 0; s < kParts; ++s) {
        ++m_slotSeq[s];
        m_slots[s] = SlotState();
        clearPartVisibility(s);
    }
    for (int s = 0; s < kSlots; ++s) {
        m_slotBox[s]->blockSignals(true);
        m_slotBox[s]->clear();
        m_slotBox[s]->blockSignals(false);
        m_slotBox[s]->setEnabled(false);
    }
    // The Set matches rows hold (slot, combo row) pairs into the OLD class's
    // piece lists. Left up, clicking one equips whatever now happens to sit at
    // that row of the new class — so the list goes with the combos it indexes.
    buildMatchList(QString());
    m_body->setEnabled(false);
    m_exportOutfit->setEnabled(false);
    m_exportSets->setEnabled(false);
    m_exportMatches->setEnabled(false);
    m_slotRepoIdx.reset();
    m_bodyRepoIdx.reset();
    m_hier.reset();
    m_locals.reset();
    stopAnim();
    m_animIds.reset();
    m_clip.reset();
    m_animBox->blockSignals(true);
    m_animBox->clear();
    m_animBox->blockSignals(false);
    m_animBox->setEnabled(false);
    m_setKeys.clear();
    m_setBox->blockSignals(true);
    m_setBox->clear();
    m_setBox->blockSignals(false);
    m_setBox->setEnabled(false);
    m_setPrev->setEnabled(false);
    m_setNext->setEnabled(false);
    m_view->clearMesh();
    refreshView();      // panels must not keep listing the old class's pieces
    // A dwell popup open over a dropdown that is about to be cleared would
    // keep describing a piece this class does not have.
    for (HoverPreview* h : m_slotHover)
        if (h) h->hidePopup();
    m_hoverImg = QImage();
    m_hoverRepo = -1;

    auto store = m_idx->store;
    const std::string want = folder.toStdString();
    QThread* worker = QThread::create([this, generation, store, want] {
        seh::installSehTranslator();
        auto slotNames = std::make_shared<std::vector<QStringList>>(kSlots);
        auto slotIdx   = std::make_shared<std::vector<QList<int>>>(kSlots);
        auto animNames = std::make_shared<QStringList>();
        auto animIds   = std::make_shared<QList<quint32>>();
        auto bodyIdx   = std::make_shared<QList<int>>();
        auto hier      = std::make_shared<di::BoneParents>();
        auto locals    = std::make_shared<di::BoneLocals>();
        for (int b = 0; b < kBodySlots; ++b) bodyIdx->append(-1);
        seh::runGuarded("wardrobe-scan", [&] {
            // The class rig: hierarchy + local rest transforms. Needed for the
            // bones overlay, glb export AND the hand-weapon hardpoints, so it
            // is parsed once here on the worker instead of lazily on the GUI
            // thread.
            std::vector<std::string> handTypes[2];
            bool hasGenericHolder[2] = {false, false};
            {
                loadClassRig(*store, want, hier.get(), locals.get());
                // Weapon holders this rig offers. Measured on f_barbarian:
                // the GENERIC "R_weapon" / "L_weapon" nodes (children of the
                // hand bones) hold anything and are animated by every clip;
                // a few weapon types additionally get their own
                // "zhushou_<type>" / "fushou_<type>" node at the same spot.
                // So a rig can hold a weapon if it has either.
                for (int h = 0; h < 2; ++h) {
                    const std::string pre = kHandPrefix[h];
                    for (const auto& [node, parent] : *hier) {
                        if (node.size() <= pre.size() ||
                            node.compare(0, pre.size(), pre) != 0)
                            continue;
                        handTypes[h].push_back(node.substr(pre.size()));
                    }
                    std::sort(handTypes[h].begin(), handTypes[h].end());
                    handTypes[h].erase(
                        std::unique(handTypes[h].begin(), handTypes[h].end()),
                        handTypes[h].end());
                    hasGenericHolder[h] = locals->count(kGenericHolder[h]) > 0;
                }
                qInfo("wardrobe: %s rig — main hand: %zu typed holders%s, "
                      "off hand: %zu typed holders%s",
                      want.c_str(), handTypes[0].size(),
                      hasGenericHolder[0] ? " + generic R_weapon" : "",
                      handTypes[1].size(),
                      hasGenericHolder[1] ? " + generic L_weapon" : "");
            }
            // The class folder's animation clips. Uses the shared resolver so
            // ALL clip-folder spellings are accepted — "ani", the misspelled
            // "ain" (282 clips; Char/sorceress_f is 100% "ain"), and "Ani".
            // Hardcoding "/ani/" here left the whole sorceress family with zero
            // animations in the Wardrobe.
            for (const auto& [nm, id] : di::findClassAnims(*store, want)) {
                *animNames << QString::fromLatin1(nm.c_str());
                *animIds << (quint32)id;
            }
            const di::Repository* repo = store->repo();
            if (!repo) return;
            const size_t slash = want.find_last_of('/');
            std::string leafPrefix =
                (slash == std::string::npos ? want : want.substr(slash + 1)) + "_";
            for (char& c : leafPrefix)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            struct Row { std::string name; int idx; };
            std::vector<Row> bySlot[kSlots];
            // default base-body candidates per token: prefer the class-prefixed
            // name (skips W_/B_ specials), then the lexicographically first
            std::string bodyName[kBodySlots];
            bool bodyPrefixed[kBodySlots] = {};
            int  bodyRepo[kBodySlots];
            for (int b = 0; b < kBodySlots; ++b) bodyRepo[b] = -1;
            const int wantFolder = repo->folderIndexOf(want);
            const std::vector<size_t>* inClass =
                wantFolder >= 0 ? repo->byFolder((uint16_t)wantFolder) : nullptr;
            static const std::vector<size_t> kNoEntries;
            for (size_t i : (inClass ? *inClass : kNoEntries)) {
                const di::RepoEntry& e = repo->entries[i];
                if (repo->typeOf(e) != "Model") continue;
                std::string lower = e.name;
                for (char& c : lower)
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                const int s = slotOf(lower);
                if (s < 0) continue;
                bySlot[s].push_back({e.name, (int)i});
                for (int b = 0; b < kBodySlots; ++b) {
                    if (lower.find(kBodyTokens[b]) == std::string::npos) continue;
                    const bool pref = lower.compare(0, leafPrefix.size(),
                                                    leafPrefix) == 0;
                    const bool better =
                        bodyRepo[b] < 0 || (pref && !bodyPrefixed[b]) ||
                        (pref == bodyPrefixed[b] && lower < bodyName[b]);
                    if (better) {
                        bodyName[b] = lower;
                        bodyPrefixed[b] = pref;
                        bodyRepo[b] = (int)i;
                    }
                }
            }
            for (int b = 0; b < kBodySlots; ++b)
                (*bodyIdx)[b] = bodyRepo[b];
            // Hand weapons come from the shared "Char/item" folder, filtered to
            // the types this rig has hardpoints for (measured: item weapons are
            // modelled in weapon space with a one-bone "<type>_bone" skin, and
            // the rig's zhushou_/fushou_ nodes place them).
            if (hasGenericHolder[0] || hasGenericHolder[1] ||
                !handTypes[0].empty() || !handTypes[1].empty()) {
                const int itemFolder = repo->folderIndexOf("Char/item");
                const std::vector<size_t>* items =
                    itemFolder >= 0 ? repo->byFolder((uint16_t)itemFolder) : nullptr;
                static const std::vector<size_t> kNoItems;
                for (size_t i : (items ? *items : kNoItems)) {
                    const di::RepoEntry& e = repo->entries[i];
                    if (repo->typeOf(e) != "Model") continue;
                    std::string lower = e.name;
                    for (char& c : lower)
                        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    if (lower.find("_lod") != std::string::npos ||
                        lower.compare(0, 4, "low_") == 0)
                        continue;
                    const size_t us = lower.find('_');
                    if (us == std::string::npos) continue;
                    const std::string type = lower.substr(0, us);
                    for (int h = 0; h < 2; ++h) {
                        const bool typed = std::binary_search(
                            handTypes[h].begin(), handTypes[h].end(), type);
                        if (!typed && !hasGenericHolder[h]) continue;
                        bySlot[h == 0 ? kMainHand : kOffHand].push_back(
                            {e.name, (int)i});
                    }
                }
            }
            for (int s = 0; s < kSlots; ++s) {
                std::sort(bySlot[s].begin(), bySlot[s].end(),
                          [](const Row& a, const Row& b) { return a.name < b.name; });
                for (const Row& r : bySlot[s]) {
                    (*slotNames)[s] << QString::fromLatin1(r.name.c_str());
                    (*slotIdx)[s]   << r.idx;
                }
            }
        });
        emit classScanned(generation, slotNames, slotIdx, animNames, animIds,
                          bodyIdx, hier, locals);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

void WardrobeTab::loadPiece(int slot, int repoIdx)
{
    if (!m_idx || !m_idx->store) return;
    const int generation = m_generation.load();   // piece loads don't invalidate
                                                  // each other, only reload/rescan do
    const int seq = ++m_slotSeq[slot];
    // This slot now has a load in flight. stepVariant MUST NOT run until it
    // lands: it bumps m_slotSeq itself, which would make this load's
    // pieceLoaded fail its own seq check and be thrown away — leaving the old
    // mesh on screen while the combo and repoIdx say the new piece is equipped,
    // and no way to retrigger it because the combo index is already correct.
    // The old per-row < > buttons prevented this by disabling themselves; the
    // menu that replaced them needs the guard in the model, not in a widget.
    if (slot < kParts) m_slotLoading[slot] = true;
    // This slot is about to hold a DIFFERENT piece, so its visibility overrides
    // must go: the old submesh indices mean nothing on the new mesh, and a
    // hidden slot would otherwise silently swallow its replacement.
    clearPartVisibility(slot);
    // The repository entry behind this slot — needed by the raw-deps export and
    // by the opposite-gender twin lookup, neither of which the loaded mesh can
    // answer on its own.
    m_slots[slot].repoIdx = repoIdx;
    auto store = m_idx->store;
    auto hier   = m_hier;      // snapshot: the worker must not touch members
    auto locals = m_locals;
    QThread* worker = QThread::create(
        [this, generation, seq, store, slot, repoIdx, hier, locals] {
        seh::installSehTranslator();
        ViewPart part;
        std::shared_ptr<di::SkinSkeleton> skel;
        // fallback survives a guarded crash: empty name + honest message
        QString note = QStringLiteral("|piece load crashed (see log)");
        auto varNames = std::make_shared<QStringList>();
        auto varIdxs  = std::make_shared<QList<int>>();
        auto bindPose = std::make_shared<std::vector<float>>();
        QString handNote;   // why a hand weapon could not be placed, if so
        seh::runGuarded("wardrobe-load", [&] {
            const di::Repository* repo = store->repo();
            const std::string pieceName =
                repo ? repo->entries[(size_t)repoIdx].name : std::string();
            const di::ResolvedModel res = di::resolveModelChain(*store, repoIdx);
            if (res.meshBlob == (size_t)-1) {
                note = QString::fromLatin1(pieceName.c_str()) +
                       QStringLiteral("|no Mesh dependency resolved");
                return;
            }
            const std::vector<uint8_t> raw = store->mpk().readAsset(res.meshBlob);
            auto m = std::make_shared<di::MeshData>();
            std::string err;
            if (!di::parseMesh(raw.data(), raw.size(), m.get(), &err)) {
                note = QString::fromLatin1(pieceName.c_str()) +
                       QStringLiteral("|mesh parse failed: %1")
                           .arg(QString::fromStdString(err));
                return;
            }
            auto decodeTex = [&](size_t blob) -> QImage {
                if (blob == (size_t)-1) return {};
                const std::vector<uint8_t> traw = store->mpk().readAsset(blob);
                di::Texture2D tex;
                std::string terr;
                if (traw.empty() || !di::isTexture2D(traw.data(), traw.size()) ||
                    !di::parseTexture2D(traw.data(), traw.size(), &tex, &terr))
                    return {};
                return TextureDecode::decode(tex).image;
            };
            part.mesh = m;
            part.textures.diffuse  = decodeTex(res.texBlob);
            part.textures.normal   = decodeTex(res.nrmBlob);
            part.textures.mix      = decodeTex(res.mixBlob);
            part.textures.emissive = decodeTex(res.emiBlob);
            part.textures.fx       = res.matFx;   // animated emissive layers
            if (res.skinBlob != (size_t)-1) {
                const std::vector<uint8_t> sraw = store->mpk().readAsset(res.skinBlob);
                auto s = std::make_shared<di::SkinSkeleton>();
                std::string serr;
                if (di::parseSkinSkeleton(sraw.data(), sraw.size(), s.get(), &serr))
                    skel = s;
            }
            // HAND SLOT: the mesh is in weapon space. Rename its skin bone to
            // the rig's holder node ("zhushou_<type>" / "fushou_<type>") so the
            // clip drives it by name, and precompute the bind placement
            // (skin = invBind @ holderWorld) for when no clip does.
            if ((slot == kMainHand || slot == kOffHand) && skel && hier && locals &&
                skel->bones.size() == 1) {
                const int hand = (slot == kMainHand) ? 0 : 1;
                const size_t us = pieceName.find('_');
                std::string node;
                float world[12];
                bool placed = false;
                if (us != std::string::npos) {   // type-specific holder first
                    std::string type = pieceName.substr(0, us);
                    for (char& c : type)
                        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    node = kHandPrefix[hand] + type;
                    placed = di::worldOfBone(*hier, *locals, node, world);
                }
                if (!placed) {                   // generic hand holder
                    node = kGenericHolder[hand];
                    placed = di::worldOfBone(*hier, *locals, node, world);
                }
                if (placed) {
                    bindPose->assign(skel->bones.size() * 16, 0.0f);
                    for (size_t b = 0; b < skel->bones.size(); ++b) {
                        skel->bones[b].name = node;   // one-bone skin: measured
                        float S[12];
                        di::mul43Row(skel->bones[b].invBind, world, S);
                        float* m = bindPose->data() + b * 16;
                        for (int c = 0; c < 4; ++c) {
                            m[c * 4 + 0] = S[c * 3 + 0];
                            m[c * 4 + 1] = S[c * 3 + 1];
                            m[c * 4 + 2] = S[c * 3 + 2];
                            m[c * 4 + 3] = (c == 3) ? 1.0f : 0.0f;
                        }
                    }
                    qInfo("wardrobe: hand slot %d -> holder %s at (%.3f %.3f %.3f)",
                          slot, node.c_str(), world[9], world[10], world[11]);
                } else {
                    handNote = QStringLiteral(
                        "no hand holder in this rig for %1 - shown at the "
                        "origin, not in hand")
                        .arg(QString::fromLatin1(pieceName.c_str()));
                    qWarning("wardrobe: no holder node for %s in slot %d - "
                             "weapon stays in weapon space",
                             pieceName.c_str(), slot);
                }
            } else if ((slot == kMainHand || slot == kOffHand) && skel &&
                       skel->bones.size() != 1) {
                handNote = QStringLiteral(
                    "%1 has a %2-bone skin (expected 1) - shown at the origin, "
                    "not in hand")
                    .arg(QString::fromLatin1(pieceName.c_str()))
                    .arg(skel->bones.size());
                qWarning("wardrobe: %s has %zu skin bones, not the measured 1 - "
                         "refusing to remap it onto one holder",
                         pieceName.c_str(), skel->bones.size());
            } else if ((slot == kMainHand || slot == kOffHand) && (!hier || !locals)) {
                handNote = QStringLiteral(
                    "this class has no readable .skeleton - hand weapons cannot "
                    "be placed");
            }
            // dye/awakened variant materials: sibling Materials named
            // "<matBase>_<tok>_mat" (measured: b1/b2/g1/g2/dye0/aw...; single
            // token, never "lod"). Keyed off the RESOLVED material's name, not
            // the model's — some pieces share another piece's material
            // (measured: f_monk_bijia_L_s03_001 uses f_monk_yifu_s03_001_mat,
            // whose aw1-3 siblings are the awakened variants). Base first so
            // index 0 restores the default.
            if (repo && res.matRepo >= 0) {
                std::string matBase = repo->entries[(size_t)res.matRepo].name;
                if (matBase.size() > 4 &&
                    matBase.compare(matBase.size() - 4, 4, "_mat") == 0)
                    matBase.resize(matBase.size() - 4);
                const std::string base = matBase + "_";
                struct Var { std::string tok; int idx; };
                std::vector<Var> vars;
                for (size_t i = 0; i < repo->entries.size(); ++i) {
                    const di::RepoEntry& e = repo->entries[i];
                    const std::string& n = e.name;
                    if ((int)i == (int)res.matRepo) continue;   // the base itself
                    if (n.size() < base.size() + 5 ||
                        n.compare(0, base.size(), base) != 0 ||
                        n.compare(n.size() - 4, 4, "_mat") != 0)
                        continue;
                    if (repo->typeOf(e) != "Material") continue;
                    const std::string tok =
                        n.substr(base.size(), n.size() - base.size() - 4);
                    if (tok.empty() || tok.find('_') != std::string::npos ||
                        tok.compare(0, 3, "lod") == 0)
                        continue;
                    vars.push_back({tok, (int)i});
                }
                if (!vars.empty()) {
                    std::sort(vars.begin(), vars.end(),
                              [](const Var& a, const Var& b) { return a.tok < b.tok; });
                    varNames->append(QStringLiteral("base"));
                    varIdxs->append((int)res.matRepo);
                    for (const Var& v : vars) {
                        varNames->append(QString::fromLatin1(v.tok.c_str()));
                        varIdxs->append(v.idx);
                    }
                }
            }
            note = QString::fromLatin1(pieceName.c_str()) + QLatin1Char('|') + handNote;
            qInfo("wardrobe: slot %d <- %s %zu verts tex=%s%s%s%s vars=%d", slot,
                  pieceName.c_str(), m->vertexCount(),
                  part.textures.diffuse.isNull() ? "none" : "ok",
                  part.textures.normal.isNull() ? "" : "+n",
                  part.textures.mix.isNull() ? "" : "+m",
                  part.textures.emissive.isNull() ? "" : "+e",
                  (int)varIdxs->size());
        });
        emit pieceLoaded(generation, slot, seq, part, skel, note, varNames,
                         varIdxs, bindPose);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

// Which armour set a piece belongs to, as an index into m_setKeys (-1 = none).
// Piece names end with "_<key>", optionally with an "_ext"/"_int" form suffix,
// so the longest matching key wins — "sz06_006" must beat a shorter key that
// happens to be its tail.
int WardrobeTab::setKeyIndexOf(const QString& pieceName) const
{
    if (pieceName.isEmpty()) return -1;
    QString nm = pieceName.toLower();
    if (nm.endsWith(QStringLiteral("_ext")) || nm.endsWith(QStringLiteral("_int")))
        nm.chop(4);
    int best = -1, bestLen = 0;
    for (int i = 0; i < m_setKeys.size(); ++i) {
        const QString suffix = QLatin1Char('_') + m_setKeys[i].toLower();
        if (nm.endsWith(suffix) && suffix.size() > bestLen) {
            best = i;
            bestLen = suffix.size();
        }
    }
    return best;
}

// Equip a set at the given scope, keeping the Set combo honest about what is
// worn. Selecting the row would normally re-apply at ThemeAll through the
// combo's own handler, so the signal is blocked and applySet called directly.
void WardrobeTab::equipSetAt(int setKeyIdx, ThemeScope scope)
{
    if (setKeyIdx < 0 || setKeyIdx >= m_setKeys.size() || !m_setBox) return;
    const int row = setKeyIdx + 1;   // row 0 is "(none)"
    if (row < m_setBox->count() && m_setBox->currentIndex() != row) {
        QSignalBlocker block(m_setBox);
        m_setBox->setCurrentIndex(row);
    }
    applySet(row, scope);
}

// The slot row's menu. This replaces three fixed buttons per row (< > Export)
// AND the old "Equip ▾" dropdown, which asked the wrong question: it applied
// whatever set the combo happened to show, when what you actually want when you
// are looking at a slot is "give me the rest of THIS piece's set".
// The Set matches list's menu. Exporting from here is the point: it lets you
// pull a set's alternative weapon out as a .glb WITHOUT equipping it and losing
// whatever you had in that slot. The piece is loaded on demand from its repo
// entry (it is not equipped, so there is no SlotState to read).
void WardrobeTab::showMatchMenu(int slot, int comboRow, const QPoint& globalPos)
{
    if (slot < 0 || slot >= kSlots || !m_slotRepoIdx ||
        slot >= (int)m_slotRepoIdx->size())
        return;
    const QList<int>& idxs = (*m_slotRepoIdx)[slot];
    if (comboRow < 1 || comboRow - 1 >= idxs.size()) return;
    const int repoIdx = idxs[comboRow - 1];
    const QString name = m_slotBox[slot]->itemText(comboRow);
    const bool equipped = (m_slotBox[slot]->currentIndex() == comboRow);

    QMenu menu(this);
    QAction* head = menu.addAction(QStringLiteral("%1 · %2").arg(partLabel(slot), name));
    head->setEnabled(false);
    menu.addSeparator();
    QAction* eq = menu.addAction(QStringLiteral("Equip in %1").arg(partLabel(slot)),
                                 this, [this, slot, comboRow] {
        if (m_slotBox[slot] && comboRow < m_slotBox[slot]->count())
            m_slotBox[slot]->setCurrentIndex(comboRow);
        const int si = m_setBox ? m_setBox->currentIndex() : 0;
        if (si > 0 && si - 1 < m_setKeys.size()) buildMatchList(m_setKeys[si - 1]);
    });
    eq->setEnabled(!equipped);
    menu.addAction(QStringLiteral("Export this piece…"), this,
                   [this, repoIdx, name] { exportRepoPiece(repoIdx, name); });
    menu.addAction(QStringLiteral("Copy piece name"), this, [name] {
        QGuiApplication::clipboard()->setText(name);
    });
    menu.exec(globalPos);
}

// Export one piece straight from its repository entry — no slot involved, so
// this works for a match-list row that is not equipped. Parsing happens here on
// the GUI thread because it is ONE piece; exportGlb then does the rest on a
// worker exactly as it does for an equipped slot.
void WardrobeTab::exportRepoPiece(int repoIdx, const QString& name)
{
    if (!m_idx || !m_idx->store || repoIdx < 0) return;
    GlbExporter::Part p;
    MeshTextures tex;
    bool ok = false;
    seh::runGuarded("wardrobe-match-export", [&] {
        ok = buildPartFromRepo(*m_idx->store, repoIdx, name,
                               ExportSettings::includeTextures() ||
                                   ExportSettings::looseTextures(),
                               &p, &tex);
    });
    if (!ok || !p.mesh) {
        m_infoText.addNote(QStringLiteral("could not load %1 for export")
                               .arg(name.toHtmlEscaped()));
        return;
    }
    if (!ExportSettings::includeTextures()) p.textures = MeshTextures();
    QString base = name;
    if (base.isEmpty()) base = QStringLiteral("piece");
    exportGlb(base, {std::move(p)}, {repoIdx});
}

void WardrobeTab::showSlotMenu(int slot, const QPoint& globalPos)
{
    if (slot < 0 || slot >= kSlots || !m_slotBox[slot]) return;
    SlotState& st = m_slots[slot];
    const bool loaded = (bool)st.part.mesh;
    QMenu menu(this);

    // A header, because a menu opened from a label needs to say which row it is
    // acting on before it offers to change it.
    QAction* head = menu.addAction(
        loaded ? QStringLiteral("%1 · %2").arg(partLabel(slot), st.name)
               : QStringLiteral("%1 · (empty)").arg(partLabel(slot)));
    head->setEnabled(false);
    menu.addSeparator();

    // ── this piece's set ───────────────────────────────────────────────────
    const int setIdx = loaded ? setKeyIndexOf(st.name) : -1;
    if (setIdx >= 0) {
        QMenu* eq = menu.addMenu(
            QStringLiteral("Equip set \"%1\"").arg(setLabel(m_setKeys[setIdx])));
        const struct { const char* label; ThemeScope scope; } kItems[] = {
            {"Everything",       ThemeAll},
            {"Armour only",      ThemeArmor},
            {"Weapons only",     ThemeWeapons},
            {"Attachments only", ThemeAttach},
        };
        const QString setKey = m_setKeys[setIdx];
        for (const auto& e : kItems) {
            const ThemeScope sc = e.scope;
            // By KEY, not by index: menu.exec() spins an event loop, and a
            // class rescan landing while the menu is open renumbers m_setKeys.
            eq->addAction(QLatin1String(e.label), this, [this, setKey, sc] {
                const int i = m_setKeys.indexOf(setKey);
                if (i >= 0) equipSetAt(i, sc);
            });
        }
    } else if (loaded) {
        QAction* a = menu.addAction(QStringLiteral("No set matches this piece"));
        a->setEnabled(false);
    }

    // ── variants ───────────────────────────────────────────────────────────
    if (loaded && st.varRepoIdx.size() > 1 && !m_slotLoading[slot]) {
        menu.addSeparator();
        QAction* cur = menu.addAction(
            QStringLiteral("Variant %1/%2: %3")
                .arg(st.varIdx + 1).arg(st.varRepoIdx.size())
                .arg(st.varNames.value(st.varIdx)));
        cur->setEnabled(false);
        menu.addAction(QStringLiteral("Previous variant"), this,
                       [this, slot] { stepVariant(slot, -1); });
        menu.addAction(QStringLiteral("Next variant"), this,
                       [this, slot] { stepVariant(slot, +1); });
    }

    // ── export / copy ──────────────────────────────────────────────────────
    menu.addSeparator();
    QAction* exp = menu.addAction(QStringLiteral("Export this piece…"), this,
                                  [this, slot] { exportSlotPiece(slot); });
    exp->setEnabled(loaded);
    QAction* cp = menu.addAction(QStringLiteral("Copy piece name"), this,
                                 [this, slot] {
        QGuiApplication::clipboard()->setText(m_slots[slot].name);
    });
    cp->setEnabled(loaded);

    // ── slot state ─────────────────────────────────────────────────────────
    menu.addSeparator();
    if (m_slotLock[slot]) {
        QAction* lk = menu.addAction(QStringLiteral("Lock this slot"));
        lk->setCheckable(true);
        lk->setChecked(m_slotLock[slot]->isChecked());
        connect(lk, &QAction::toggled, this, [this, slot](bool on) {
            if (m_slotLock[slot]) m_slotLock[slot]->setChecked(on);
        });
    }
    QAction* clr = menu.addAction(QStringLiteral("Clear this slot"), this,
                                  [this, slot] {
        if (m_slotBox[slot] && m_slotBox[slot]->currentIndex() != 0)
            m_slotBox[slot]->setCurrentIndex(0);
    });
    clr->setEnabled(m_slotBox[slot]->currentIndex() != 0);
    menu.addAction(QStringLiteral("Clear every slot"), this, [this] {
        for (int k = 0; k < kSlots; ++k)
            if (m_slotBox[k] && !slotLocked(k) && m_slotBox[k]->currentIndex() != 0)
                m_slotBox[k]->setCurrentIndex(0);
        clearSetAttachments();
        refreshView();
    });

    menu.exec(globalPos);
}

// One .glb from one equipped slot. Shared by the slot menu and the equipped-list
// menu, which used to carry two copies of this.
void WardrobeTab::exportSlotPiece(int slot)
{
    if (slot < 0 || slot >= kParts || !m_slots[slot].part.mesh) return;
    GlbExporter::Part p;
    p.mesh     = m_slots[slot].part.mesh;
    p.textures = m_slots[slot].part.textures;
    p.skel     = m_slots[slot].skel;
    p.name     = m_slots[slot].name;
    QString base = m_slots[slot].name;
    if (base.isEmpty()) base = QStringLiteral("piece");
    exportGlb(base, {std::move(p)}, {m_slots[slot].repoIdx});
}

void WardrobeTab::stepVariant(int slot, int delta)
{
    if (slot < 0 || slot >= kSlots) return;
    // A load in flight owns this slot's sequence number — see loadPiece.
    if (m_slotLoading[slot]) return;
    SlotState& st = m_slots[slot];
    if (!st.part.mesh || st.varRepoIdx.size() < 2 || !m_idx || !m_idx->store)
        return;
    const int n = st.varRepoIdx.size();
    st.varIdx = ((st.varIdx + delta) % n + n) % n;
    const int matRepoIdx = st.varRepoIdx[st.varIdx];
    const QString label = st.varNames.value(st.varIdx);
    const int generation = m_generation.load();
    // bump: each variant load supersedes the previous one, so two rapid
    // clicks can never apply out of order (audit finding)
    const int seq = ++m_slotSeq[slot];
    auto store = m_idx->store;
    QThread* worker = QThread::create(
        [this, generation, seq, store, slot, matRepoIdx, label] {
        seh::installSehTranslator();
        MeshTextures tex;
        seh::runGuarded("wardrobe-variant", [&] {
            const di::ResolvedModel res =
                di::resolveMaterialTextures(*store, matRepoIdx);
            auto decodeTex = [&](size_t blob) -> QImage {
                if (blob == (size_t)-1) return {};
                const std::vector<uint8_t> traw = store->mpk().readAsset(blob);
                di::Texture2D t;
                std::string terr;
                if (traw.empty() || !di::isTexture2D(traw.data(), traw.size()) ||
                    !di::parseTexture2D(traw.data(), traw.size(), &t, &terr))
                    return {};
                return TextureDecode::decode(t).image;
            };
            tex.diffuse  = decodeTex(res.texBlob);
            tex.normal   = decodeTex(res.nrmBlob);
            tex.mix      = decodeTex(res.mixBlob);
            tex.emissive = decodeTex(res.emiBlob);
            qInfo("wardrobe: slot %d variant %s tex=%s%s%s%s", slot,
                  qPrintable(label), tex.diffuse.isNull() ? "none" : "ok",
                  tex.normal.isNull() ? "" : "+n", tex.mix.isNull() ? "" : "+m",
                  tex.emissive.isNull() ? "" : "+e");
        });
        emit variantLoaded(generation, slot, seq, tex, label);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

void WardrobeTab::applyBodyToggle(bool on)
{
    if (on) {
        if (!m_bodyRepoIdx) return;
        for (int b = 0; b < kBodySlots; ++b)
            if ((*m_bodyRepoIdx)[b] >= 0)
                loadPiece(kSlots + b, (*m_bodyRepoIdx)[b]);
    } else {
        for (int b = 0; b < kBodySlots; ++b) {
            const int slot = kSlots + b;
            ++m_slotSeq[slot];
            m_slots[slot] = SlotState();
            clearPartVisibility(slot);
            m_view->setPart(slot, ViewPart());
        }
        refreshView();
    }
}

// Combo row of the set's MAIN piece for one visible slot, chosen by explicit
// ranking so a sub-attachment or variant can never shadow the real garment:
//   rank 0  plain "<tok>_<key>"          (the set's own piece)
//   rank 1  "toukui_all_<key>"           (full helmet)
//   rank 2  "toukui_half_<key>"          (half helmet)
//   rank 3  "jianjia_left_<key>"         (LR-pair set: left is the slot main,
//                                         right rides along as an attachment)
//   rank 4  any other name ending in the key
//   rank 5  any name merely containing the key (old fallback)
// Measured: 15 of f_barbarian's 148 helmet-bearing keys have NO plain helmet
// (all/half only), 1-2 shoulder keys per class are left+right with no plain.
int WardrobeTab::pickMainInSlot(int slot, const QString& key) const
{
    static const char* mainTok[4] = {"_toukui_", "_yifu_", "_jianjia_", "_tui_"};
    const QString suffix = QLatin1Char('_') + key;
    // Score = form rank + suffix penalty. Forms: plain "<tok>_<key>" beats
    // toukui_all beats toukui_half beats jianjia_left beats any other name
    // ending in the key, and "contains" is the last resort. Suffix: a bare
    // name beats its "_ext" form beats its "_int" form (measured: monk
    // sz08_001 ships EVERY garment only as _ext/_int alternates — they are
    // FORMS of one piece, never worn together).
    const auto formScore = [&](const QString& nm) -> int {
        QString stem = nm;
        int pen = 0;
        if (stem.endsWith(QStringLiteral("_ext"))) { stem.chop(4); pen = 1; }
        else if (stem.endsWith(QStringLiteral("_int"))) { stem.chop(4); pen = 2; }
        if (!stem.endsWith(suffix))
            return nm.contains(suffix) ? 90 : -1;
        int base = 40;
        if (slot < 4) {
            if (stem.endsWith(QString::fromLatin1(mainTok[slot]) + key)) base = 0;
            else if (slot == 0 && stem.endsWith(QStringLiteral("_toukui_all") + suffix))
                base = 10;
            else if (slot == 0 && stem.endsWith(QStringLiteral("_toukui_half") + suffix))
                base = 20;
            else if (slot == 2 && stem.endsWith(QStringLiteral("_jianjia_left") + suffix))
                base = 30;
        } else {
            base = 0;   // weapon / hand / hair names are plain "<...>_<key>"
        }
        return base + pen;
    };
    int best = -1, bestScore = 1 << 20;
    for (int i = 1; i < m_slotBox[slot]->count(); ++i) {
        const int sc = formScore(m_slotBox[slot]->itemText(i).toLower());
        if (sc >= 0 && sc < bestScore) { bestScore = sc; best = i; }
        if (bestScore == 0) break;
    }
    return best;
}

void WardrobeTab::clearSetAttachments()
{
    for (int p = kSlots + kBodySlots; p < kParts; ++p) {
        // seq bump is UNCONDITIONAL: an in-flight pool load has mesh null and
        // name empty, and skipping the bump would let the previous set's
        // attachment land inside the new outfit (audit finding)
        ++m_slotSeq[p];
        m_slotLoading[p] = false;   // that bump just abandoned any in-flight load
        clearPartVisibility(p);
        if (!m_slots[p].part.mesh && m_slots[p].name.isEmpty()) continue;
        m_slots[p] = SlotState();
        m_view->setPart(p, ViewPart());
    }
}

// Equip the set: ranked MAIN piece per visible slot, then auto-equip every
// other same-key piece of the class into the attachment pool — a set is the
// whole group (measured: f_barbarian sz06_006 ships helmet+chest+chest-back+
// shoulders+shoulder-parts+legs+neck; monk sets add wuqi and both bijia).
// Slots without a match are left as-is so partial sets keep the rest of the
// outfit.
// Every piece of this class whose name carries the set key, from every bucket.
//
// The slot combos can each show ONE choice, but a set frequently ships more than
// one candidate for the same slot — two weapons, a plain and an _ext form, a
// left/right shoulder pair. The picker has to commit to one; this list shows all
// of them, so the ones the ranking passed over are still reachable.
// Re-equip the look captured before a class switch. Cosmetics are named
// "<gender>_<class>_<slot>_<set>", so the same piece on the other rig is the
// same name with the prefix swapped; genderless pieces (the shared Char/item
// weapons) carry over unchanged. A piece with no counterpart is simply left
// unequipped rather than substituted — a near-miss would be worse than a gap.
void WardrobeTab::applyCarryOver()
{
    if (m_carryOver.isEmpty() || m_carryOverClass.isEmpty()) return;
    // Only across the SAME class. Barbarian -> Crusader shares no piece names,
    // and trying would just equip nothing slowly.
    const auto classOf = [](QString leaf) {
        if (leaf.startsWith(QStringLiteral("Char/"))) leaf = leaf.mid(5);
        if (leaf.startsWith(QStringLiteral("f_")) ||
            leaf.startsWith(QStringLiteral("m_")))
            leaf = leaf.mid(2);
        else if (leaf.endsWith(QStringLiteral("_f")) ||
                 leaf.endsWith(QStringLiteral("_m")))
            leaf.chop(2);
        return leaf.toLower();
    };
    const QHash<int, QString> want = m_carryOver;
    const QString from = m_carryOverClass;
    m_carryOver.clear();
    m_carryOverClass.clear();
    if (classOf(from) != classOf(m_curClassFolder)) return;

    const auto twinName = [](const QString& nm) {
        QString t = nm;
        if (t.startsWith(QStringLiteral("f_")))      t[0] = QLatin1Char('m');
        else if (t.startsWith(QStringLiteral("m_"))) t[0] = QLatin1Char('f');
        else return QString();
        return t;
    };
    int moved = 0, missed = 0;
    for (auto it = want.constBegin(); it != want.constEnd(); ++it) {
        const int slot = it.key();
        if (slot < 0 || slot >= kSlots || !m_slotBox[slot]) continue;
        int row = -1;
        const QString twin = twinName(it.value());
        if (!twin.isEmpty()) row = m_slotBox[slot]->findText(twin);
        if (row < 0) row = m_slotBox[slot]->findText(it.value());   // genderless
        if (row > 0) {
            m_slotBox[slot]->setCurrentIndex(row);                  // -> loadPiece
            ++moved;
        } else {
            ++missed;
        }
    }
    if (moved || missed)
        qInfo("wardrobe: carried %d piece(s) across the gender switch, %d had no twin",
              moved, missed);
    if (missed && m_info)
        m_infoText.addNote(QStringLiteral("carried %1 piece(s) over · %2 had no "
                                          "counterpart on this rig")
                               .arg(moved).arg(missed));
}

// Show or hide the Set matches list, moving the left column's vertical stretch
// with it.
//
// A QVBoxLayout whose items all have stretch 0 CENTRES its content. The list
// was the column's only stretch consumer, and a hidden widget contributes none
// — so with no set selected the whole equipment grid drifted to the middle of
// the panel and the export buttons went with it, then snapped back to the top
// the moment a set was picked. A trailing spacer holds the stretch while the
// list is hidden, which pins the grid to the top in both states.
void WardrobeTab::setMatchListVisible(bool on)
{
    if (m_matchList)  m_matchList->setVisible(on);
    if (m_matchLabel) m_matchLabel->setVisible(on);
    if (!m_leftCol || m_matchListRow < 0) return;
    m_leftCol->setStretch(m_matchListRow, on ? 1 : 0);
    m_leftCol->setStretch(m_matchFillRow, on ? 0 : 1);
}

void WardrobeTab::buildMatchList(const QString& key)
{
    if (!m_matchList) return;
    m_matchList->clear();
    if (key.isEmpty() || !m_slotRepoIdx) {
        setMatchListVisible(false);
        syncMatchButton();
        return;
    }
    const QString needle = QLatin1Char('_') + key;
    // Icons, same source and same cache as the slot dropdowns' — this list is
    // where you compare a set's alternative weapons against each other, and
    // comparing them by filename is exactly the job a thumbnail does better.
    // A set has a handful of matches, not the ~250 a slot bucket holds, so all
    // of them are requested at once rather than windowed.
    const bool want3d = thumbs3dOn();
    const bool haveProvider = want3d ? (bool)m_thumbs3d : (bool)m_thumbs;
    m_matchList->setIconSize(QSize(ThumbnailProvider::kSize,
                                   ThumbnailProvider::kSize));
    int shown = 0;
    for (int s = 0; s < kSlots && s < (int)m_slotRepoIdx->size(); ++s) {
        const QList<int>& idxs = (*m_slotRepoIdx)[s];
        for (int i = 1; i < m_slotBox[s]->count(); ++i) {
            const QString nm = m_slotBox[s]->itemText(i);
            if (!nm.contains(needle, Qt::CaseInsensitive)) continue;
            auto* it = new QListWidgetItem(
                QStringLiteral("%1  ·  %2").arg(partLabel(s), nm));
            it->setData(Qt::UserRole, s);
            it->setData(Qt::UserRole + 1, i);
            it->setToolTip(nm);
            // A miss QUEUES the decode; the providers' ready() rebuilds this
            // list, so the icon lands on the second pass rather than blocking.
            if (haveProvider && i - 1 < idxs.size()) {
                AssetRow row;
                row.repoIdx = idxs[i - 1];
                row.type    = QStringLiteral("Model");
                row.entryId = (uint32_t)idxs[i - 1];   // the providers' cache key
                QPixmap pm;
                const bool hit = want3d ? m_thumbs3d->get(row, &pm)
                                        : m_thumbs->get(row, &pm);
                if (hit && !pm.isNull()) it->setIcon(QIcon(pm));
            }
            // The one the set picker actually chose reads differently from the
            // alternatives it passed over.
            if (m_slotBox[s]->currentIndex() == i) {
                QFont f = it->font();
                f.setBold(true);
                it->setFont(f);
                it->setForeground(QColor(0x8f, 0xbf, 0x8f));
            }
            m_matchList->addItem(it);
            ++shown;
        }
    }
    setMatchListVisible(shown > 0);
    syncMatchButton();
}

void WardrobeTab::syncMatchButton() { syncExportButtons(); }

// Every export button says how many FILES it will write, counted by the same
// buildBulkUnits() that the run itself uses — so the number on the button is
// the number of files, not an estimate that can drift from it.
//
// "Files" is not "units": with both genders on, only the units that actually
// resolve a counterpart write two. Genderless item-folder weapons have none,
// and a class the game ships one gender of has none at all — a naive "x2" would
// promise files that will never appear.
void WardrobeTab::syncExportButtons()
{
    const bool ready = m_idx && m_idx->store && !m_setKeys.isEmpty() &&
                       m_slotRepoIdx && !m_bulkRunning;

    if (m_exportMatches) {
        const int rows = m_matchList ? m_matchList->count() : 0;
        BulkCounts c;
        if (ready && rows > 0) buildBulkUnits(BulkListedMatches, &c);
        m_exportMatches->setText(
            c.files > 0 ? QStringLiteral("Export %1 matches…").arg(c.files)
                        : QStringLiteral("Export matches…"));
        m_exportMatches->setEnabled(ready && c.files > 0);
        m_exportMatches->setToolTip(
            c.files > 0
                ? QStringLiteral(
                      "Write the %1 pieces in the Set matches list as %2 .glb "
                      "file(s) — this set's alternatives, exactly the rows shown."
                      "%3\n\n"
                      "For every non-armour match of EVERY set, use \"Also export "
                      "all non-armour set matches\" in Settings ▸ Export ▸ "
                      "Wardrobe, which adds that pass to an \"Export all sets\" run.")
                      .arg(rows).arg(c.files)
                      .arg(c.twins > 0
                               ? QStringLiteral(" %1 of them have an opposite-gender "
                                                "counterpart, which doubles those.")
                                     .arg(c.twins)
                               : QString())
                : QStringLiteral("Pick a set first — this exports the pieces in the "
                                 "Set matches list."));
    }

    if (m_exportSets) {
        // Cached: the all-sets count depends on the CLASS and the export
        // settings, never on which set is selected or what is equipped — and
        // rebuilding it is the expensive one (with the match pass on it sweeps
        // every slot bucket of every set). Cycling sets with < > must not pay
        // for a count that cannot have changed.
        if (ready && !m_setsCountsValid) {
            buildBulkUnits(BulkAllSets, &m_setsCounts);
            m_setsCountsValid = true;
        }
        const BulkCounts c = ready ? m_setsCounts : BulkCounts{};
        m_exportSets->setText(c.files > 0
                                  ? QStringLiteral("Export all sets (%1)…").arg(c.files)
                                  : QStringLiteral("Export all sets…"));
        m_exportSets->setEnabled(ready && c.files > 0);
        QStringList parts;
        if (c.sets)    parts << QStringLiteral("%1 set unit(s)").arg(c.sets);
        if (c.matches) parts << QStringLiteral("%1 added match(es)").arg(c.matches);
        if (c.twins)   parts << QStringLiteral("%1 opposite-gender twin(s)").arg(c.twins);
        m_exportSets->setToolTip(
            c.files > 0
                ? QStringLiteral("%1 .glb file(s) for this class: %2.\n\nThe count "
                                 "follows Settings ▸ Export ▸ Wardrobe — shape, "
                                 "filters and \"both genders\" are all already in it.")
                      .arg(c.files, 0, 10)
                      .arg(parts.join(QStringLiteral(" + ")))
                : QStringLiteral("Write one .glb per armor set of this class into a "
                                 "folder"));
    }

    syncOutfitButton();
}

void WardrobeTab::syncOutfitButton()
{
    if (m_exportOutfit) {
        // Cheap enough to redo on every equip: at most kParts name lookups.
        // The equipped pieces either have counterparts on the other rig or they
        // do not — "both genders is on" alone does not mean two files.
        int files = 1;
        if (ExportSettings::bothGenders() && m_idx && m_idx->store) {
            for (int sl = 0; sl < kParts; ++sl) {
                if (!m_slots[sl].part.mesh) continue;
                if (oppositeGenderPiece(m_slots[sl].repoIdx) >= 0) { files = 2; break; }
            }
        }
        const bool can = canExport();
        m_exportOutfit->setText(can && files > 1
                                    ? QStringLiteral("Export outfit .glb (2)…")
                                    : QStringLiteral("Export outfit .glb…"));
        m_exportOutfit->setToolTip(
            files > 1
                ? QStringLiteral("Two files: this outfit and its opposite-gender "
                                 "counterpart.")
                : QStringLiteral("One .glb of the assembled outfit."));
    }
}

// A slot the user has pinned. The set picker and the theme menu check this; a
// direct pick in the combo does not (that is the user acting on the slot), and
// neither does the gender carry-over, which re-equips the same piece on the
// other rig rather than replacing it.
bool WardrobeTab::slotLocked(int slot) const
{
    return slot >= 0 && slot < kSlots && m_slotLock[slot] &&
           m_slotLock[slot]->isChecked();
}

// Narrow the character dropdown. Rows are HIDDEN rather than removed, so the
// combo index still equals the m_classFolders index — the stable identity every
// other piece of this tab keys off. Rebuilding the combo's contents to match a
// filter would break that mapping, which is exactly the bug the "select by
// folder, never by label" rule elsewhere in this file exists to avoid.
void WardrobeTab::applyClassFilter(const QString& text)
{
    if (!m_classBox) return;
    auto* view = qobject_cast<QListView*>(m_classBox->view());
    const QString t = text.trimmed();
    int shown = 0;
    m_classFilterFirst = -1;
    for (int i = 0; i < m_classBox->count(); ++i) {
        const bool hit =
            t.isEmpty() ||
            m_classBox->itemText(i).contains(t, Qt::CaseInsensitive) ||
            (i < m_classFolders.size() &&
             m_classFolders[i].contains(t, Qt::CaseInsensitive));
        if (view) view->setRowHidden(i, !hit);
        if (hit) {
            ++shown;
            if (m_classFilterFirst < 0) m_classFilterFirst = i;
        }
    }
    // A filter that matches nothing looks identical to one that matches
    // everything until you open the list, so say so in the field itself.
    if (m_classFilter)
        m_classFilter->setStyleSheet(
            (shown == 0 && !t.isEmpty())
                ? QStringLiteral("color:#c76b63;")
                : QString());
}

// Fill in icons that have finished decoding since the list was built. Only the
// icons are touched — rebuilding the rows would drop the user's selection and
// scroll position every time a thumbnail landed.
void WardrobeTab::refreshMatchIcons()
{
    if (!m_matchList || !m_matchList->isVisible() || !m_slotRepoIdx) return;
    const bool want3d = thumbs3dOn();
    if (want3d ? !m_thumbs3d : !m_thumbs) return;
    for (int r = 0; r < m_matchList->count(); ++r) {
        QListWidgetItem* it = m_matchList->item(r);
        if (!it || !it->icon().isNull()) continue;
        const int s = it->data(Qt::UserRole).toInt();
        const int i = it->data(Qt::UserRole + 1).toInt();
        if (s < 0 || s >= (int)m_slotRepoIdx->size()) continue;
        const QList<int>& idxs = (*m_slotRepoIdx)[s];
        if (i - 1 < 0 || i - 1 >= idxs.size()) continue;
        // peek() takes the cache KEY, and both providers are keyed by entryId,
        // which this tab sets to the repo index (see requestThumbs). It never
        // enqueues, so a still-decoding piece just stays iconless this pass.
        const uint32_t cacheKey = (uint32_t)idxs[i - 1];
        QPixmap pm;
        const bool hit = want3d ? m_thumbs3d->peek(cacheKey, &pm)
                                : m_thumbs->peek(cacheKey, &pm);
        if (hit && !pm.isNull()) it->setIcon(QIcon(pm));
    }
}

void WardrobeTab::applySet(int setIdx, ThemeScope scope)
{
    if (scope == ThemeAll || scope == ThemeAttach) clearSetAttachments();
    if (setIdx <= 0 || setIdx > m_setKeys.size()) {
        buildMatchList(QString());
        refreshView();
        return;
    }
    const QString key = m_setKeys[setIdx - 1];
    const QString suffix = QLatin1Char('_') + key;
    buildMatchList(key);
    // Armour = the worn garment slots and hair; weapons = the back cosmetic and
    // the hands. A narrowed scope leaves everything outside it exactly as it is.
    const auto inScope = [scope](int slot) {
        switch (scope) {
            case ThemeArmor:   return slot <= 4;
            case ThemeWeapons: return slot == 5 || slot == kMainHand || slot == kOffHand;
            case ThemeAttach:  return false;   // pool only; handled below
            default:           return true;
        }
    };
    // Sets are not all the same shape: some ship a hair piece, most do not. When
    // the new set has nothing for a slot the old set filled, that slot has to be
    // emptied or the previous set's hair rides along on top of the new outfit.
    const bool clearUnmatched =
        QSettings().value(QStringLiteral("wardrobe/setClearsUnmatched"), true).toBool();

    // 1. mains into the visible slots: armor, set hair (some sets ship a
    //    toufa_nocolor piece — it REPLACES the hair, never stacks), the
    //    back-weapon cosmetic, and the main hand for item-weapon sets
    QSet<QString> equipped;
    int mains = 0;
    for (int s : {0, 1, 2, 3, 4, 5, (int)kMainHand}) {
        if (!m_slotBox[s]->isEnabled()) continue;
        if (!inScope(s)) continue;
        if (slotLocked(s)) {
            // Still counts as worn, so the pool below will not double it up.
            if (m_slotBox[s]->currentIndex() > 0)
                equipped.insert(m_slotBox[s]->currentText().toLower());
            continue;
        }
        const int pick = pickMainInSlot(s, key);
        if (pick < 0) {
            if (clearUnmatched && m_slotBox[s]->currentIndex() != 0)
                m_slotBox[s]->setCurrentIndex(0);        // empties the slot
            continue;
        }
        equipped.insert(m_slotBox[s]->itemText(pick).toLower());
        ++mains;
        if (pick != m_slotBox[s]->currentIndex())
            m_slotBox[s]->setCurrentIndex(pick);         // triggers loadPiece
    }
    // Off hand is never a set main (no set ships one), but it must still be
    // cleared when a weapons scope moves you to a different set.
    if (clearUnmatched && inScope(kOffHand) && !slotLocked(kOffHand) &&
        m_slotBox[kOffHand]->isEnabled() &&
        m_slotBox[kOffHand]->currentIndex() != 0 && scope == ThemeAll)
        m_slotBox[kOffHand]->setCurrentIndex(0);

    // 2. attachments: every OTHER piece of the class whose name ends with the
    //    key (or key_ext / key_int — measured suffixed sub-pieces), from any
    //    bucket except the hands, into the pool
    // _ext / _int are FORMS of one piece — dedupe by stem, prefer the bare
    // name then _ext then _int, and never pool a piece whose stem is already
    // worn as a main. Hair is a main above; hands are Char/item — both skipped.
    const auto stemOf = [](QString nm) {
        if (nm.endsWith(QStringLiteral("_ext")) ||
            nm.endsWith(QStringLiteral("_int")))
            nm.chop(4);
        return nm;
    };
    QSet<QString> mainStems;
    for (const QString& e : equipped) mainStems.insert(stemOf(e));
    struct Cand { QString name; int repoIdx = -1; int pen = 0; };
    QHash<QString, Cand> byStem;
    // The pool is only the ATTACHMENT scope's business. It used to be refilled
    // on every scope, so "Weapons only" replaced the attachments too — and
    // since a narrowed scope does not clearSetAttachments(), any pool slot past
    // the new set's count kept the OLD set's piece, leaving two sets' worth of
    // attachments on the character at once.
    const bool poolInScope = (scope == ThemeAll || scope == ThemeAttach);
    if (m_slotRepoIdx && poolInScope) {
        // Pool sources: the Attachment and Other buckets ONLY. The main
        // buckets 0-3 hold nothing but garment forms now, and a set's other
        // forms (toukui_half beside all, jianjia left/right beside plain, a
        // second wuqi) are ALTERNATIVES of the worn main — pooling them would
        // double the geometry. One measured exception: when the shoulder main
        // is "jianjia_left_<key>" (an LR-pair set with no plain piece), the
        // matching right half completes the pair.
        const auto scanBucket = [&](int s, const char* require) {
            if (s >= (int)m_slotRepoIdx->size()) return;
            const QList<int>& idxs = (*m_slotRepoIdx)[s];
            for (int i = 0; i < idxs.size() && i + 1 < m_slotBox[s]->count(); ++i) {
                const QString nm = m_slotBox[s]->itemText(i + 1).toLower();
                if (require && !nm.contains(QLatin1String(require))) continue;
                int pen;
                if (nm.endsWith(suffix))                                pen = 0;
                else if (nm.endsWith(suffix + QStringLiteral("_ext")))  pen = 1;
                else if (nm.endsWith(suffix + QStringLiteral("_int")))  pen = 2;
                else continue;
                if (equipped.contains(nm)) continue;     // already a slot main
                const QString stem = stemOf(nm);
                if (mainStems.contains(stem)) continue;  // form of a worn main
                auto it = byStem.find(stem);
                if (it == byStem.end() || pen < it->pen)
                    byStem[stem] = {m_slotBox[s]->itemText(i + 1), idxs[i], pen};
            }
        };
        scanBucket(kAttachSlot, nullptr);
        scanBucket(kSlots - 1, nullptr);                 // "Other"
        if (m_slotBox[2]->currentText().toLower().contains(
                QLatin1String("_jianjia_left_")))
            scanBucket(2, "_jianjia_right_");
    }
    int pool = kSlots + kBodySlots, attached = 0, overflow = 0;
    QStringList attachNames;
    {
        QList<Cand> cands = byStem.values();
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b) { return a.name < b.name; });
        for (const Cand& c : cands) {
            if (pool >= kParts) { ++overflow; continue; }
            attachNames << c.name;
            loadPiece(pool++, c.repoIdx);
            ++attached;
        }
    }
    if (overflow)
        qWarning("wardrobe: set %s has %d attachments beyond the %d-part pool",
                 qPrintable(key), overflow, kSetAttach);
    qInfo("wardrobe: set %s -> %d mains + %d attachments%s", qPrintable(key),
          mains, attached,
          overflow ? qPrintable(QStringLiteral(" (%1 dropped)").arg(overflow))
                   : "");
    m_infoText.setBase(
        QStringLiteral("set %1: %2 mains + %3 attachments%4%5")
            .arg(setLabel(key))   // real name when data\di_item_names.csv has one
            .arg(mains)
            .arg(attached)
            .arg(attached ? QStringLiteral(" (%1)").arg(attachNames.join(
                                QStringLiteral(", ")))
                          : QString())
            .arg(overflow ? QStringLiteral(" — %1 more than the pool holds")
                                .arg(overflow)
                          : QString()));
    // Pool pieces were removed synchronously above; without this the panels
    // would only catch up when some async pieceLoaded happens to arrive.
    refreshView();
}

void WardrobeTab::stepSet(int delta)
{
    if (m_setKeys.isEmpty()) return;
    int idx = m_setBox->currentIndex() + delta;
    if (idx < 1) idx = m_setKeys.size();                 // wrap past "(custom)"
    if (idx > m_setKeys.size()) idx = 1;
    m_setBox->setCurrentIndex(idx);                      // triggers applySet
}

void WardrobeTab::stopAnim()
{
    if (m_animTimer) m_animTimer->stop();
    if (m_playBtn) {
        m_playBtn->setText(QStringLiteral("Play"));
        m_playBtn->setEnabled(false);
    }
    if (m_timeSlider) m_timeSlider->setEnabled(false);
    if (m_stepBack)  m_stepBack->setEnabled(false);
    if (m_stepFwd)   m_stepFwd->setEnabled(false);
    if (m_frameSpin) m_frameSpin->setEnabled(false);
    for (int s = 0; s < kParts; ++s)
        m_slots[s].player.init(nullptr, nullptr);
    m_clip.reset();
    m_animT = 0.0f;
    updateFrameUi();
    if (m_view) {
        m_view->clearPose();
        applyBindPoses();   // hand weapons must not snap back to weapon space
    }
}

// The name of the clip that is actually loaded. NOT m_animBox->currentText():
// that combo is editable and doubles as a search box, so its text is whatever
// was last typed — exporting used it as the glTF animation name.
QString WardrobeTab::currentClipName() const
{
    if (!m_animBox) return {};
    const int i = m_animBox->currentIndex();
    return i > 0 ? m_animBox->itemText(i) : QString();
}

bool WardrobeTab::clothOn() const
{
    return m_clothBox && m_clothBox->isChecked();
}

// Bake cloth (when enabled) into a per-slot play clip and (re)init that slot's
// player with the class hierarchy so undriven cloth bones ride the body instead
// of floating (the A1 skinned-pose fallback), and cloth bones get physics.
void WardrobeTab::initSlotPlayer(int slot)
{
    SlotState& S = m_slots[slot];
    if (!m_clip || !S.part.mesh || !S.skel) {
        S.player.init(nullptr, nullptr);
        S.playClip.reset();
        return;
    }
    S.playClip = cloth::maybeBake(m_clip, S.skel.get(), m_hier.get(),
                                  m_locals.get(), clothOn());
    S.player.init(S.playClip.get(), S.skel.get(), m_locals.get(), m_hier.get());
    // hand weapons fall back to their hardpoint when the clip has no track for
    // the holder, or parks it to hide the weapon
    S.player.setFallbackSkin(S.bindPose);
}

// The Settings dialog writes export/cloth directly, and its Cancel and Restore
// Defaults paths rewrite it too. This tab caches that key in a checkbox, so
// without a re-read the popup would still show "Cloth physics" ticked while
// ExportSettings::clothPhysics() answered false — the viewport posing one way
// and the exporter the other. Called after the dialog closes.
void WardrobeTab::syncExportSettings()
{
    // The Set list options are consumed by scanClass, so a change to them does
    // nothing at all until the class is scanned again — and switching class to
    // apply a setting is exactly the kind of dead control this dialog keeps
    // producing. Rescan when they actually changed, never otherwise: a rescan
    // drops the equipped outfit.
    {
        const SetList::Options o = SetList::Options::load();
        QString sig = o.includeAwakening ? QStringLiteral("1") : QStringLiteral("0");
        for (int i = 0; i < SetList::kMaxNamedTier; ++i)
            sig += o.tier[i] ? QLatin1Char('1') : QLatin1Char('0');
        if (!m_setListSig.isEmpty() && sig != m_setListSig &&
            !m_curClassFolder.isEmpty()) {
            m_setListSig = sig;
            scanClass(m_curClassFolder);
            return;   // the rescan re-runs everything below when it lands
        }
        m_setListSig = sig;
    }
    // Shape, filters, "both genders" and the file-name templates all move the
    // numbers on the export buttons, and all of them live in that dialog. This
    // runs BEFORE the cloth early-outs below — those are about one checkbox,
    // and returning early from them used to leave every count stale.
    invalidateSetsCount();
    syncExportButtons();
    if (!m_clothBox) return;
    const bool want = ExportSettings::clothPhysics();
    if (m_clothBox->isChecked() == want) return;
    QSignalBlocker block(m_clothBox);   // re-bake once below, not twice
    m_clothBox->setChecked(want);
    onClothToggled();
}

void WardrobeTab::onClothToggled()
{
    if (!m_clip) return;   // nothing playing — nothing to re-bake
    for (int s = 0; s < kParts; ++s)
        if (m_slots[s].part.mesh && m_slots[s].skel) initSlotPlayer(s);
    animTick(m_animT);
}

void WardrobeTab::selectAnim(int comboIdx)
{
    stopAnim();
    if (comboIdx <= 0 || !m_animIds || comboIdx > m_animIds->size() || !m_idx) {
        refreshView();   // restore bind overlay
        return;
    }
    const quint32 entryId = (*m_animIds)[comboIdx - 1];
    const std::vector<uint8_t> raw = m_idx->store->mpk().readAsset((size_t)entryId);
    auto clip = std::make_shared<di::AnimClip>();
    std::string err;
    if (raw.empty() || !di::parseAnim(raw.data(), raw.size(), clip.get(), &err)) {
        m_infoText.addNote(QStringLiteral("anim parse failed: %1")
                               .arg(QString::fromStdString(err).toHtmlEscaped()));
        qWarning("wardrobe: anim parse failed (%s)", err.c_str());
        return;
    }
    m_clip = clip;
    int driven = 0;
    for (int s = 0; s < kParts; ++s) {
        if (!m_slots[s].part.mesh || !m_slots[s].skel) continue;
        initSlotPlayer(s);   // bakes cloth (if on) + inits with class hierarchy
        ++driven;
    }
    qInfo("wardrobe: anim -> %zu tracks, %u ms, driving %d parts",
          m_clip->tracks.size(), m_clip->durationMs, driven);
    m_timeSlider->setRange(0, (int)m_clip->durationMs);
    m_timeSlider->setEnabled(true);
    m_playBtn->setEnabled(true);
    m_playBtn->setText(QStringLiteral("Pause"));
    if (m_stepBack)  m_stepBack->setEnabled(true);
    if (m_stepFwd)   m_stepFwd->setEnabled(true);
    if (m_frameSpin) m_frameSpin->setEnabled(true);
    m_animT = 0.0f;
    animTick(0.0f);
    syncAnimList();
    fillInfoPanel();
    m_animTimer->start();
}

// Pose-only tick — see ModelsTab::animPose. A capture scrubs the whole clip and
// must leave the transport slider/spinner (the user's playhead) alone.
void WardrobeTab::animPose(float tMs)
{
    if (!m_clip) return;
    // richest-skeleton slot supplies the overlay geometry
    int overlaySlot = -1;
    size_t bestBones = 0;
    for (int s = 0; s < kParts; ++s)
        if (m_slots[s].player.valid() &&
            m_slots[s].skel->bones.size() >= bestBones) {
            bestBones = m_slots[s].skel->bones.size();
            overlaySlot = s;
        }
    for (int s = 0; s < kParts; ++s) {
        if (!m_slots[s].player.valid()) continue;
        std::vector<float> mats, segs, joints;
        const bool wantOverlay = (s == overlaySlot);
        m_slots[s].player.evaluate(tMs, &mats, wantOverlay ? &segs : nullptr,
                                   wantOverlay ? &joints : nullptr);
        m_view->setPartPose((size_t)s, std::move(mats));
        if (wantOverlay)
            m_view->setSkeleton(std::move(segs), std::move(joints));
    }
}

void WardrobeTab::animTick(float tMs)
{
    if (!m_clip) return;
    animPose(tMs);
    m_timeSlider->blockSignals(true);
    m_timeSlider->setValue((int)tMs);
    m_timeSlider->blockSignals(false);
    updateFrameUi();
}

// Hand-held pieces are the only ones whose mesh is NOT already in character
// space, so they need their hardpoint placement pushed as a "pose" whenever no
// clip is driving them. Everything else renders correctly unposed.
void WardrobeTab::applyBindPoses()
{
    for (int s = 0; s < kParts; ++s)
        if (m_slots[s].part.mesh && !m_slots[s].bindPose.empty())
            m_view->setPartPose((size_t)s,
                                std::vector<float>(m_slots[s].bindPose));
}

// Lazy thumbnails for one slot's dropdown: only what the user just opened
// gets decoded (a class carries ~250 pieces per slot; decoding every slot up
// front would be ~2,000 texture decodes on every class switch).
// Dwell popup over a slot dropdown. The interesting half is the image: when 3D
// icons are on, the popup shows a FULL-SIZE 3D render rather than the 40px combo
// icon — the icon tells you which piece it is, this tells you what it looks like.
//
// renderPreview() is synchronous. That is the deliberate trade ModelThumbRenderer
// documents: a hover is one considered dwell, not a scroll, so paying for a parse
// and a render once is better than the complexity of an async round-trip that
// would arrive after the popup is already gone.
bool WardrobeTab::resolveSlotHover(int slot, const QModelIndex& idx,
                                   QList<HoverPreview::Line>* lines,
                                   QImage* image)
{
    // A dropdown row IS its combo row; row 0 is "(none)".
    return idx.isValid() ? resolvePieceHover(slot, idx.row(), lines, image) : false;
}

// The Set matches list carries (slot, combo row) on every item, which is the
// same pair a dropdown row resolves to — so the preview is literally the same
// one, not a second implementation that could show something different.
bool WardrobeTab::resolveMatchHover(const QModelIndex& idx,
                                    QList<HoverPreview::Line>* lines,
                                    QImage* image)
{
    if (!idx.isValid() || !m_matchList) return false;
    QListWidgetItem* it = m_matchList->item(idx.row());
    if (!it) return false;
    return resolvePieceHover(it->data(Qt::UserRole).toInt(),
                             it->data(Qt::UserRole + 1).toInt(), lines, image);
}

bool WardrobeTab::resolvePieceHover(int slot, int comboRow,
                                    QList<HoverPreview::Line>* lines,
                                    QImage* image)
{
    if (!lines || slot < 0 || slot >= kSlots) return false;
    if (!m_idx || !m_idx->store || !m_slotRepoIdx) return false;
    const int row = comboRow;
    if (row < 1) return false;                       // row 0 is "(none)"
    if (slot >= (int)m_slotRepoIdx->size()) return false;
    const QList<int>& idxs = (*m_slotRepoIdx)[slot];
    if (row - 1 >= idxs.size()) return false;
    const int repoIdx = idxs[row - 1];
    const di::Repository* repo = m_idx->store->repo();
    if (!repo || repoIdx < 0 || (size_t)repoIdx >= repo->entries.size()) return false;

    const QString name = QString::fromStdString(repo->entries[(size_t)repoIdx].name);
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    const QString leafName = slash >= 0 ? name.mid(slash + 1) : name;
    lines->append({leafName, HoverInfo::Col::kName});
    const QString meaning = NameTranslator::cosmeticName(leafName);
    if (!meaning.isEmpty() && meaning != leafName)
        lines->append({meaning, HoverInfo::Col::kSeries});
    lines->append({QStringLiteral("%1 · %2").arg(partLabel(slot), name),
                   HoverInfo::Col::kFile});
    if (m_slots[slot].repoIdx == repoIdx)
        lines->append({QStringLiteral("equipped"), HoverInfo::Col::kGood});

    if (!HoverInfo::imagePreview()) return true;

    AssetRow r;
    r.repoIdx = repoIdx;
    r.type    = QStringLiteral("Model");
    r.entryId = (uint32_t)repoIdx;   // the key both providers cache under
    if (m_hoverRepo == repoIdx) {
        // Null after a failed render means it CANNOT be rendered — never re-try
        // for this row, or every mouse twitch pays the parse again.
        if (!m_hoverImg.isNull()) *image = m_hoverImg;
        return true;
    }
    if (thumbs3dOn() && m_thumbs3d) {
        QImage img;
        const bool ok = m_thumbs3d->renderPreview(r, HoverInfo::previewPx(), &img);
        // Remember the ATTEMPT either way. Only recording successes meant a
        // mesh the renderer refuses was re-parsed on every mouse move, which is
        // exactly what the early-out above says it prevents.
        m_hoverImg  = ok ? img : QImage();
        m_hoverRepo = repoIdx;
        if (ok && !img.isNull()) {
            *image = img;
            return true;
        }
    }
    // Flat-icon mode, or a mesh the renderer refused: fall back to whatever the
    // texture provider already has cached. peek() never enqueues, so a hover
    // cannot kick off decode work behind the popup.
    if (m_thumbs) {
        QPixmap pm;
        if (m_thumbs->peek(r.entryId, &pm) && !pm.isNull()) {
            m_hoverImg = pm.toImage();
            m_hoverRepo = repoIdx;
            *image = m_hoverImg;
        }
    }
    return true;
}

bool WardrobeTab::thumbs3dOn() const
{
    return QSettings().value(QStringLiteral("wardrobe/view/thumb3d"), false).toBool();
}

// Drop every combo icon. Needed when the icon MODE flips: the two providers
// have separate caches, so stale flat-texture icons would otherwise sit next to
// freshly-rendered 3D ones until each row happened to be re-requested.
void WardrobeTab::clearSlotIcons()
{
    // The hover memo holds an image produced in the OLD mode; keeping it would
    // show a flat texture in the popup right after switching to 3D icons.
    m_hoverImg = QImage();
    m_hoverRepo = -1;
    for (int s = 0; s < kSlots; ++s) {
        if (!m_slotBox[s]) continue;
        for (int i = 1; i < m_slotBox[s]->count(); ++i)
            m_slotBox[s]->setItemIcon(i, QIcon());
    }
}

// Ask for the thumbnails of the rows currently ON SCREEN in one slot's popup.
//
// This used to request EVERY row in the slot. Both providers keep a bounded
// LIFO queue (384 / 512 requests) and evict from the BACK, so a slot with more
// rows than the cap pushed its own first rows out — and the first rows are
// exactly the ones the open dropdown is showing. The hand slots draw from
// Char/item, which is far larger than any armour bucket, so they lost their
// icons completely while smaller slots looked fine. Requesting the visible
// window instead fixes that and does a fraction of the work.
void WardrobeTab::requestThumbs(int slot)
{
    if (!m_idx || !m_slotRepoIdx) return;
    if (slot < 0 || slot >= kSlots || slot >= (int)m_slotRepoIdx->size()) return;
    // The setting is read here rather than cached at construction so a change in
    // the Settings dialog takes effect the next time a dropdown opens.
    const bool want3d = thumbs3dOn();
    if (want3d != m_thumb3dActive) {
        m_thumb3dActive = want3d;
        clearSlotIcons();
    }
    if (want3d && !m_thumbs3d) return;
    if (!want3d && !m_thumbs) return;
    m_thumbSlot = slot;
    const QList<int>& idxs = (*m_slotRepoIdx)[slot];
    const di::Repository* repo = m_idx->store ? m_idx->store->repo() : nullptr;
    if (!repo) return;
    // The visible window, plus a screenful of margin either side so a short
    // scroll finds its icons already rendered.
    int first = 0, last = idxs.size() - 1;
    if (QAbstractItemView* v = m_slotBox[slot]->view()) {
        const QModelIndex topIx = v->indexAt(QPoint(2, 2));
        const QModelIndex botIx =
            v->indexAt(QPoint(2, v->viewport()->height() - 2));
        // Combo row 0 is "(none)"; the repo list is offset by one.
        const int visTop = topIx.isValid() ? topIx.row() - 1 : 0;
        const int visBot = botIx.isValid() ? botIx.row() - 1 : visTop + 24;
        const int span = qMax(8, visBot - visTop + 1);
        first = qBound(0, visTop - span, idxs.size() - 1);
        last  = qBound(0, visBot + span, idxs.size() - 1);
    }
    bool anyNow = false;
    for (int i = first; i <= last; ++i) {
        AssetRow row;
        row.repoIdx = idxs[i];
        row.type    = QStringLiteral("Model");
        // Both providers key their cache / queued / failed sets by entryId, NOT
        // by repoIdx — leaving it 0 made every piece collide on one key, so a
        // single thumbnail was painted on all ~250 rows. Model rows resolve
        // through repoIdx, so entryId is free to be the unique key.
        row.entryId = (uint32_t)idxs[i];
        QPixmap pm;
        const bool hit = want3d ? m_thumbs3d->get(row, &pm)
                                : m_thumbs->get(row, &pm);
        if (hit && !pm.isNull()) {
            m_slotBox[slot]->setItemIcon(i + 1, QIcon(pm));
            anyNow = true;
        }
    }
    if (anyNow) m_slotBox[slot]->view()->update();
}

// The combo's popup view tells us when a dropdown opens — that is when the
// icons for that slot are worth decoding.
bool WardrobeTab::eventFilter(QObject* obj, QEvent* ev)
{
    // Keep the viewport N-strip pinned to the right edge, clear of the 88px
    // orientation gizmo — same geometry the Models tab uses.
    if (obj == m_view && m_vpStrip &&
        (ev->type() == QEvent::Resize || ev->type() == QEvent::Show)) {
        m_vpStrip->adjustSize();
        m_vpStrip->move(m_view->width() - m_vpStrip->width() - 6, 104);
        m_vpStrip->raise();
    }
    // Wheel over the channel button cycles the material channel.
    if (obj == m_shadeMoreBtn && ev->type() == QEvent::Wheel && m_channelCombo) {
        auto* we = static_cast<QWheelEvent*>(ev);
        const int n = m_channelCombo->count();
        if (n > 0) {
            const int step = we->angleDelta().y() > 0 ? -1 : 1;
            m_channelCombo->setCurrentIndex(
                (m_channelCombo->currentIndex() + step + n) % n);
        }
        return true;
    }
    // Wheel over the timeline steps exactly one frame; Shift+wheel changes the
    // playback speed. Consumed so QSlider's page-jump never fires.
    if (obj == m_timeSlider && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        const int dir = we->angleDelta().y() > 0 ? 1 : -1;
        if (we->modifiers() & Qt::ShiftModifier) {
            if (m_speedBox) {
                const int n = m_speedBox->count();
                if (n > 0)
                    m_speedBox->setCurrentIndex(
                        qBound(0, m_speedBox->currentIndex() + dir, n - 1));
            }
        } else {
            seekFrames(dir);
        }
        return true;
    }
    // Texture tiles: hover reports the value under the cursor for whichever mode
    // the panel is in, so it doubles as a pixel inspector.
    // Enter as well as MouseMove/Leave: the shared panel starts its hover-zoom
    // on Enter, so dropping that event left this tab with the pixel readout and
    // no zoom at all, while the other two tabs had both.
    if (ev->type() == QEvent::Enter || ev->type() == QEvent::Leave ||
        ev->type() == QEvent::MouseMove)
        m_texPrev.hover(obj, ev);
    if (ev->type() == QEvent::Show)
        for (int s = 0; s < kSlots; ++s)
            if (m_slotBox[s] && obj == m_slotBox[s]->view()) {
                requestThumbs(s);
                break;
            }
    // Editable combos: a click on the line edit opens the dropdown (on
    // RELEASE, so the popup can't swallow the same click as a selection).
    // Typing afterwards still filters through the completer.
    if (ev->type() == QEvent::MouseButtonRelease) {
        const auto openIf = [&](QComboBox* box) {
            if (!box || !box->isEnabled() || !box->lineEdit() ||
                obj != box->lineEdit())
                return false;
            if (!box->view()->isVisible()) {
                box->lineEdit()->selectAll();   // typing replaces stale text
                box->showPopup();
            }
            return true;
        };
        for (int s = 0; s < kSlots; ++s)
            if (openIf(m_slotBox[s])) return QWidget::eventFilter(obj, ev);
        openIf(m_animBox);
    }
    return QWidget::eventFilter(obj, ev);
}

// One .glb per armor set, written into a folder the user picks. Runs entirely
// on a worker off the scanned slot lists — it never touches the combos, so the
// UI stays live and a class switch (generation bump) abandons it.
// Build the exact list of units a bulk run would write, named and
// de-collided, WITHOUT writing anything. exportAllSets() runs this and then
// exports the result; the export buttons run it and then count it. One
// implementation, because a button that predicts a different number from the
// run it starts is worse than no number at all — which is exactly what
// "Export set matches" did before it read the list it sat next to.
//
// Cheap by construction: combo text and repository lookups only. No blob
// reads, no parsing, no decode — those all happen in the export lanes.
std::vector<WardrobeTab::SetJob> WardrobeTab::buildBulkUnits(
    BulkMode mode, BulkCounts* counts) const
{
    std::vector<SetJob> jobs;
    if (counts) *counts = BulkCounts{};
    if (!m_idx || !m_idx->store || m_setKeys.isEmpty() || !m_slotRepoIdx)
        return jobs;
    const bool matchesOnly = (mode == BulkListedMatches);
    // The listed-matches mode reads its units straight out of the Set matches
    // list, so the whole set resolution below is work it would throw away —
    // and this runs on the set-cycling hot path (< > rebuilds the list, which
    // recounts the button), where it meant scanning every slot bucket of every
    // set on the GUI thread for a discarded result.
    if (!matchesOnly)
    // Resolve each set up front with the SAME logic the Set cycler uses:
    // ranked mains (a sub-attachment can never shadow the real garment) plus
    // every same-key attachment, so the exported outfits are complete.
    // slots parallels repoIdx: the bulk worker needs to know which entry is a
    // HAND weapon, because those meshes are in weapon space and only become
    // hand-held after their skin bone is renamed to the rig's holder node.
    for (const QString& key : m_setKeys) {
        SetJob j;
        j.key = key;
        const QString suffix = QLatin1Char('_') + key;
        const auto stemOf = [](QString nm) {
            if (nm.endsWith(QStringLiteral("_ext")) ||
                nm.endsWith(QStringLiteral("_int")))
                nm.chop(4);
            return nm;
        };
        QSet<QString> takenStems;
        for (int s : {0, 1, 2, 3, 4, 5, (int)kMainHand}) {
            if (s >= (int)m_slotRepoIdx->size()) continue;
            const int pick = pickMainInSlot(s, key);
            if (pick < 1 || pick - 1 >= (*m_slotRepoIdx)[s].size()) continue;
            takenStems.insert(stemOf(m_slotBox[s]->itemText(pick).toLower()));
            j.repoIdx << (*m_slotRepoIdx)[s][pick - 1];
            j.names   << m_slotBox[s]->itemText(pick);
            j.slotOf  << s;
        }
        struct BCand { QString name; int repoIdx = -1; int pen = 0; };
        QHash<QString, BCand> byStem;
        const int shoulderPick = pickMainInSlot(2, key);
        const bool lrPair =
            shoulderPick >= 1 && m_slotBox[2]->itemText(shoulderPick).toLower()
                                     .contains(QLatin1String("_jianjia_left_"));
        const auto scanBucket = [&](int s, const char* require) {
            if (s >= (int)m_slotRepoIdx->size()) return;
            const QList<int>& idxs = (*m_slotRepoIdx)[s];
            for (int i = 0; i < idxs.size() && i + 1 < m_slotBox[s]->count(); ++i) {
                const QString nm = m_slotBox[s]->itemText(i + 1).toLower();
                if (require && !nm.contains(QLatin1String(require))) continue;
                int pen;
                if (nm.endsWith(suffix))                                pen = 0;
                else if (nm.endsWith(suffix + QStringLiteral("_ext")))  pen = 1;
                else if (nm.endsWith(suffix + QStringLiteral("_int")))  pen = 2;
                else continue;
                const QString stem = stemOf(nm);
                if (takenStems.contains(stem)) continue;
                auto it = byStem.find(stem);
                if (it == byStem.end() || pen < it->pen)
                    byStem[stem] = {m_slotBox[s]->itemText(i + 1), idxs[i], pen};
            }
        };
        scanBucket(kAttachSlot, nullptr);
        scanBucket(kSlots - 1, nullptr);                 // "Other"
        if (lrPair) scanBucket(2, "_jianjia_right_");
        QList<BCand> cs = byStem.values();
        std::sort(cs.begin(), cs.end(),
                  [](const BCand& a, const BCand& b) { return a.name < b.name; });
        for (const BCand& c : cs) {
            j.repoIdx << c.repoIdx;
            j.names   << c.name;
            j.slotOf  << -1;          // pool attachment: already character space
        }
        if (!j.repoIdx.isEmpty()) jobs.push_back(std::move(j));
    }

    // Filename from the STABLE folder leaf, never the friendly combo label —
    // those carry spaces and parentheses and get reworded by patches. Needed
    // before the plan below, which is what names the files.
    const int clsIdx = m_classBox->currentIndex();
    const QString cls = (clsIdx >= 0 && clsIdx < m_classFolders.size())
                            ? m_classFolders[clsIdx].mid(5)
                            : m_classBox->currentText();

    const QString classFolder = (clsIdx >= 0 && clsIdx < m_classFolders.size())
                                    ? m_classFolders[clsIdx] : QString();

    // Opposite-gender twins. Resolved HERE, before anything is named, because
    // whether a counterpart exists decides what BOTH files are called — and its
    // own class folder is what the twin's name is built from, so that the pair
    // comes out of the File-names template rather than out of a suffix bolted
    // on afterwards. The twin's RIG is still loaded on the worker: the two
    // genders' skeletons differ in proportion, and a twin written against the
    // equipped rig has the wrong bind pose.
    const bool wantTwin = ExportSettings::bothGenders();
    QString genderTag, twinTag, twinFolder;
    if (wantTwin) {
        QString ignored;
        splitClassFolder(classFolder, &ignored, &genderTag);
        if (genderTag == QLatin1String("F")) twinTag = QStringLiteral("M");
        else if (genderTag == QLatin1String("M")) twinTag = QStringLiteral("F");
        const QString tf = twinClassFolder(classFolder);
        if (!tf.isEmpty() && m_classFolders.contains(tf)) twinFolder = tf;
    }
    const bool doTwin = wantTwin && !twinTag.isEmpty();

    // ── The two passes (app/SetsExportPlan.h) ──────────────────────────────
    // The SET pass always runs; the MATCH pass runs IN ADDITION when it is on.
    // Both produce SetJobs, so everything downstream — cloth bake, layout, raw
    // deps, loose textures, the opposite-gender twin, cancel, progress — treats
    // them identically. That is the whole reason the modes are expressed as a
    // job LIST rather than a second exporter.
    SetsExport::Plan plan = SetsExport::Plan::load();
    static_assert(SetsExport::kMainHand == kMainHand &&
                      SetsExport::kOffHand == kOffHand,
                  "SetsExportPlan's slot numbers must match the Wardrobe's");
    const int curSi = m_setBox ? m_setBox->currentIndex() : 0;
    const QString curKey = (curSi > 0 && curSi - 1 < m_setKeys.size())
                               ? m_setKeys[curSi - 1] : QStringLiteral("matches");

    // The "Export N matches…" button reads its work straight out of the Set
    // matches list — the same widget the user is looking at, so the two cannot
    // report different sizes. It does NOT sweep every set: that is what the
    // settings checkbox adds to a full run, and conflating the two is what made
    // a five-row list start a five-hundred-file export.
    if (matchesOnly) {
        plan = SetsExport::Plan{};   // no set pass, no filters — the list is the list
        jobs.clear();
        if (m_matchList)
            for (int r = 0; r < m_matchList->count(); ++r) {
                QListWidgetItem* it = m_matchList->item(r);
                if (!it) continue;
                const int sl  = it->data(Qt::UserRole).toInt();
                const int row = it->data(Qt::UserRole + 1).toInt();
                if (sl < 0 || sl >= kSlots || sl >= (int)m_slotRepoIdx->size())
                    continue;
                const QList<int>& idxs = (*m_slotRepoIdx)[sl];
                if (row < 1 || row - 1 >= idxs.size()) continue;
                SetJob one;
                one.key = curKey;
                one.repoIdx << idxs[row - 1];
                one.names   << m_slotBox[sl]->itemText(row);
                one.slotOf  << sl;
                jobs.push_back(std::move(one));
            }
    }

    // File names come from Settings ▸ Export ▸ File names, exactly as the
    // single-outfit export does — a set unit through the outfit template, a
    // per-piece unit through the model template. This run used to hardcode
    // "<class>_<key>" and ignore both, which is what made the page look dead.
    const auto pieceStem = [&](int repoIdx, const QString& name) {
        QString leaf = name;
        const int sl = leaf.lastIndexOf(QLatin1Char('/'));
        if (sl >= 0) leaf = leaf.mid(sl + 1);
        const QString meaning = NameTranslator::cosmeticName(leaf);
        return NameTemplate::model(leaf, repoIdx, meaning);
    };

    // ── SET pass: filter, then optionally explode into one unit per piece ──
    if (plan.noWeapons) {
        std::vector<SetJob> kept;
        for (SetJob& j : jobs) {
            SetJob f;
            f.key = j.key;
            for (int i = 0; i < j.repoIdx.size(); ++i) {
                if (!plan.wantSetSlot(j.slotOf.value(i, -1))) continue;
                f.repoIdx << j.repoIdx[i];
                f.names   << j.names.value(i);
                f.slotOf  << j.slotOf.value(i, -1);
            }
            // A set stripped to nothing is dropped rather than left to count as
            // a failure in the run's own summary.
            if (!f.repoIdx.isEmpty()) kept.push_back(std::move(f));
        }
        jobs.swap(kept);
    }
    // Name the SET units while they are still whole: the outfit template's
    // per-slot placeholders only mean anything before the explode below.
    // Listed-match units are single pieces and are named below through the
    // MODEL template instead — running them through the outfit template would
    // describe an outfit that is not what is being written.
    const di::Repository* nameRepo = m_idx->store->repo();
    // Resolve a unit's twin pieces and give it its two names. The twin's name
    // is built from the TWIN's class folder, so a template carrying {{Gender}}
    // already tells the pair apart and no suffix is needed; the tags are only
    // appended when the template produced the same name twice, which is the
    // one case where they would otherwise overwrite each other.
    const auto nameUnit = [&](SetJob& j, const QString& own, auto twinNamer) {
        j.stem = own;
        if (!doTwin || !nameRepo) return;
        for (int k = 0; k < j.repoIdx.size(); ++k) {
            const int t = twinOfRepoEntry(*nameRepo, j.repoIdx[k]);
            if (t < 0 || (size_t)t >= nameRepo->entries.size()) continue;
            j.twinRepoIdx << t;
            j.twinNames   << QString::fromStdString(nameRepo->entries[(size_t)t].name);
            j.twinSlotOf  << j.slotOf.value(k, -1);
        }
        if (j.twinRepoIdx.isEmpty()) return;   // genderless: no pair, no tags
        j.twinStem = twinNamer(j);
        if (j.twinStem == j.stem) {
            j.stem     += QLatin1Char('_') + genderTag;
            j.twinStem += QLatin1Char('_') + twinTag;
        }
    };
    const auto slotNamesOf = [](const QList<int>& slotOf, const QStringList& names) {
        QHash<int, QString> m;
        for (int i = 0; i < slotOf.size(); ++i)
            if (slotOf[i] >= 0 && slotOf[i] < 8) m.insert(slotOf[i], names.value(i));
        return m;
    };
    const auto namePiece = [&](SetJob& j) {
        nameUnit(j, pieceStem(j.repoIdx.first(), j.names.value(0)),
                 [&](const SetJob& u) {
                     return pieceStem(u.twinRepoIdx.first(), u.twinNames.first());
                 });
    };
    for (SetJob& j : jobs) {
        if (matchesOnly) { namePiece(j); continue; }
        nameUnit(j,
                 outfitStem(classFolder, j.key, setLabel(j.key),
                            slotNamesOf(j.slotOf, j.names)),
                 [&](const SetJob& u) {
                     return outfitStem(twinFolder.isEmpty() ? classFolder : twinFolder,
                                       u.key, setLabel(u.key),
                                       slotNamesOf(u.twinSlotOf, u.twinNames));
                 });
    }
    if (plan.setsPerPiece()) {
        std::vector<SetJob> split;
        QSet<int> seen;
        for (const SetJob& j : jobs)
            for (int i = 0; i < j.repoIdx.size(); ++i) {
                if (seen.contains(j.repoIdx[i])) continue;   // shared attachment
                seen.insert(j.repoIdx[i]);
                SetJob one;
                one.key = j.key;
                one.repoIdx << j.repoIdx[i];
                one.names   << j.names.value(i);
                one.slotOf  << j.slotOf.value(i, -1);
                namePiece(one);
                split.push_back(std::move(one));
            }
        jobs.swap(split);
    }
    const size_t setUnits = matchesOnly ? 0 : jobs.size();
    const size_t listedUnits = matchesOnly ? jobs.size() : 0;

    // ── MATCH pass: ADDED to the set pass, never instead of it ─────────────
    // Every piece whose name carries a set key, armour excluded. Weapons stay
    // in even when "exclude weapons" is on — that switch is about what rides
    // along inside an outfit, and this pass is how you get the weapons on their
    // own. Deduped by repository index against the set units already queued and
    // against itself, so nothing is written twice.
    size_t matchUnits = 0;
    if (plan.wantMatchPass()) {
        QSet<int> seen;
        // Seed from the set units ONLY when the set pass wrote them as their
        // own files. With sets exported as outfits, a weapon inside one is not
        // a file you have — it is geometry buried in an outfit — so skipping it
        // here would drop exactly the pieces this pass exists to produce, and
        // "in addition" would quietly mean "instead" for a set's own weapons.
        if (plan.setsPerPiece())
            for (const SetJob& j : jobs)
                for (int ri : j.repoIdx) seen.insert(ri);
        for (const QString& key : m_setKeys) {
            const QString needle = QLatin1Char('_') + key;
            for (int sl = 0; sl < kSlots && sl < (int)m_slotRepoIdx->size(); ++sl) {
                if (!plan.wantMatchSlot(sl)) continue;
                const QList<int>& idxs = (*m_slotRepoIdx)[sl];
                for (int i = 0; i < idxs.size() && i + 1 < m_slotBox[sl]->count(); ++i) {
                    const QString nm = m_slotBox[sl]->itemText(i + 1);
                    if (!nm.contains(needle, Qt::CaseInsensitive)) continue;
                    // Dedup by repository index is load-bearing, not tidiness:
                    // the class scan pushes a weapon into BOTH hand buckets
                    // whenever the rig has a holder for that hand, and the
                    // generic R_weapon/L_weapon nodes hold anything — so most
                    // weapons appear twice with identical names, and two units
                    // with the same name would overwrite each other's file.
                    //
                    // KNOWN LIMIT: the main hand is scanned first, so a weapon
                    // the rig only offers a TYPED off-hand holder for
                    // ("fushou_<type>" with no "zhushou_<type>") is exported
                    // against the generic right-hand node instead of its own
                    // left-hand one. Placing it correctly needs the rig's typed
                    // holder lists, which scanClass computes and discards; that
                    // is a data question this code cannot answer on its own, so
                    // it is written down rather than guessed at.
                    if (seen.contains(idxs[i])) continue;
                    seen.insert(idxs[i]);
                    SetJob one;
                    one.key = key;
                    one.repoIdx << idxs[i];
                    one.names   << nm;
                    one.slotOf  << sl;
                    namePiece(one);
                    jobs.push_back(std::move(one));
                    ++matchUnits;
                }
            }
        }
    }

    if (jobs.empty()) return jobs;

    // ── One output name per unit ───────────────────────────────────────────
    // The old hardcoded "<class>_<key>" could not collide. Names now come from
    // a free-text template, and several ways of writing one collapse: a set
    // template with no {{Set}} renders identically for every set; a template
    // whose every placeholder is empty for this unit falls back to a CONSTANT;
    // and two repository entries can genuinely share a leaf name (measured, and
    // the Models tab's batch export mangles paths for exactly this reason).
    // Any of those silently overwrote one file with another while counting both
    // as written. Claim each name once, suffixing duplicates.
    //
    // Case-insensitive, because the target file systems are: "Sword" and
    // "sword" are one file on Windows.
    int renamedNames = 0;
    {
        QSet<QString> used;
        int& renamed = renamedNames;
        const auto claim = [&used, &renamed](QString stem) {
            if (stem.isEmpty()) stem = QStringLiteral("piece");
            const QString base = stem;
            for (int n = 2; used.contains(stem.toLower()); ++n) {
                stem = base + QStringLiteral("_%1").arg(n);
                if (n == 2) ++renamed;
            }
            used.insert(stem.toLower());
            return stem;
        };
        for (SetJob& j : jobs) {
            j.stem = claim(j.stem);
            if (!j.twinStem.isEmpty()) j.twinStem = claim(j.twinStem);
        }
        // Loud, because the usual cause is a file-name template that cannot
        // tell these units apart — worth fixing rather than living with _2, _3.
        // NOT logged here: this runs on every UI recount as well as on a real
        // run, and a warning per keystroke is noise. The count travels out in
        // BulkCounts::renamed and exportAllSets logs it once, when it matters.

    }
    if (counts) {
        counts->units  = jobs.size();
        counts->renamed = (size_t)renamedNames;
        counts->sets   = setUnits;
        counts->matches = matchUnits;
        counts->listed = listedUnits;
        for (const SetJob& j : jobs)
            if (!j.twinRepoIdx.isEmpty()) ++counts->twins;
        counts->files = counts->units + counts->twins;
    }
    return jobs;
}

void WardrobeTab::exportAllSets(BulkMode mode)
{
    const bool matchesOnly = (mode == BulkListedMatches);
    if (!m_idx || !m_idx->store || m_setKeys.isEmpty() || !m_slotRepoIdx) return;

    // The unit list is built BEFORE the folder prompt, so the prompt can say
    // how many files it is about to write. Nothing is read from disk to do it.
    BulkCounts counts;
    std::vector<SetJob> jobs = buildBulkUnits(mode, &counts);
    if (jobs.empty()) {
        m_infoText.addNote(
            matchesOnly
                ? QStringLiteral("the Set matches list is empty — pick a set first")
                : QStringLiteral("nothing to export with these settings"));
        return;
    }

    // Seed the prompt from the last folder used, and remember what is chosen.
    // NOT the same as exporting there silently — the dialog still opens and you
    // still confirm; it just opens where you were instead of wherever the
    // process happens to think the working directory is.
    QSettings dirSettings;
    const QString lastDir =
        dirSettings.value(QLatin1String(kWardrobeDirKey)).toString();
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        mode == BulkListedMatches
            ? QStringLiteral("Export %1 matching piece(s) into…").arg(counts.files)
            : QStringLiteral("Export %1 file(s) into…").arg(counts.files),
        QDir(lastDir).exists() ? lastDir : QString());
    if (dir.isEmpty()) return;
    dirSettings.setValue(QLatin1String(kWardrobeDirKey), dir);

    // Re-derived here for the report and the lanes; buildBulkUnits works these
    // out for itself from the same settings, so they cannot disagree.
    const int clsIdx = m_classBox ? m_classBox->currentIndex() : -1;
    const QString classFolder = (clsIdx >= 0 && clsIdx < m_classFolders.size())
                                    ? m_classFolders[clsIdx] : QString();
    QString cls, genderTag, twinTag, twinFolder;
    splitClassFolder(classFolder, &cls, &genderTag);
    cls = (clsIdx >= 0 && clsIdx < m_classFolders.size())
              ? m_classFolders[clsIdx].mid(5)
              : (m_classBox ? m_classBox->currentText() : QString());
    if (ExportSettings::bothGenders()) {
        if (genderTag == QLatin1String("F")) twinTag = QStringLiteral("M");
        else if (genderTag == QLatin1String("M")) twinTag = QStringLiteral("F");
        const QString tf = twinClassFolder(classFolder);
        if (!tf.isEmpty() && m_classFolders.contains(tf)) twinFolder = tf;
    }
    const bool doTwin = !twinTag.isEmpty();
    auto report = std::make_shared<ExportReport>(
        mode == BulkListedMatches ? QStringLiteral("Wardrobe — export set matches")
                                  : QStringLiteral("Wardrobe — export all sets"),
        dir);
    const bool wantReport = ExportReport::enabled();
    const size_t setUnits = counts.sets;
    const size_t matchUnits = counts.matches;
    const size_t listedUnits = counts.listed;
    const int curSi = m_setBox ? m_setBox->currentIndex() : 0;
    const QString curKey = (curSi > 0 && curSi - 1 < m_setKeys.size())
                               ? m_setKeys[curSi - 1] : QStringLiteral("matches");
    const SetsExport::Plan plan = SetsExport::Plan::load();
    const QString planText =
        matchesOnly ? QStringLiteral("the %1 piece(s) listed under set \"%2\"")
                          .arg(jobs.size()).arg(curKey)
                    : plan.describe();
    qInfo("wardrobe bulk: %zu unit(s) (%zu set, %zu match, %zu listed) — %s",
          jobs.size(), setUnits, matchUnits, listedUnits, qPrintable(planText));

    const int generation = m_generation.load();
    // Every option below is Settings ▸ Export — the bulk run is the same export
    // as the outfit button, repeated, so it must answer to the same switches.
    const AnimExportScope scope = AnimExportScope::load();
    QList<QPair<QString, quint32>> allRefs;
    std::vector<GlbExporter::AnimExport> baseAnims;
    {
        const QString playingName = currentClipName();
        for (const auto& r : outfitAnimRefs(scope)) {
            if (m_clip && r.first == playingName) baseAnims.push_back({r.first, m_clip});
            else                                  allRefs.append(r);
        }
    }
    const bool wantDeps  = ExportSettings::withRawDeps();
    const bool wantLoose = ExportSettings::looseTextures();
    const bool haveTex   = ExportSettings::includeTextures();
    const QString layout = ExportLayout::mode();

    if (counts.renamed)
        qWarning("wardrobe bulk: %zu name collision(s) suffixed — the file-name "
                 "template does not distinguish these units", counts.renamed);
    if (wantReport) {
        if (counts.renamed)
            report->note(QStringLiteral(
                             "%1 unit(s) rendered the same file name and were "
                             "suffixed _2, _3 … — the File-names template does not "
                             "distinguish them")
                             .arg(counts.renamed));
        report->context(QStringLiteral("class: %1").arg(classFolder));
        report->context(QStringLiteral("plan: %1").arg(planText));
        report->context(QStringLiteral("%1 unit(s): %2 set, %3 match, %4 listed")
                            .arg(jobs.size()).arg(setUnits).arg(matchUnits)
                            .arg(listedUnits));
        report->context(QStringLiteral("textures: %1embedded, %2loose · raw deps: %3")
                            .arg(haveTex ? QString() : QStringLiteral("not "))
                            .arg(wantLoose ? QString() : QStringLiteral("no "))
                            .arg(wantDeps ? QStringLiteral("yes") : QStringLiteral("no")));
        report->context(QStringLiteral("both genders: %1")
                            .arg(doTwin ? QStringLiteral("yes (%1 + %2)")
                                              .arg(genderTag, twinTag)
                                        : QStringLiteral("no")));
        report->context(QStringLiteral("overwrite existing: %1")
                            .arg(ExportSettings::overwriteExisting()
                                     ? QStringLiteral("yes")
                                     : QStringLiteral("no — duplicates suffixed")));
        report->context(QStringLiteral("folder layout: %1").arg(layout));
        report->context(QStringLiteral("animations: %1").arg(scope.describe()));
    }

    auto hier   = m_hier;
    auto locals = m_locals;
    auto store  = m_idx->store;
    const bool clothBake = clothOn();
    const di::ClothParams clothP = cloth::paramsFromSettings();
    const size_t jobCount = jobs.size();   // jobs is moved into the worker below
    m_bulkCancel.store(false);

    QThread* worker = QThread::create(
        [this, generation, jobs = std::move(jobs), hier, locals, store, dir, cls,
         baseAnims, allRefs, clothBake, clothP, wantDeps, wantLoose, haveTex,
         layout, doTwin, genderTag, twinTag, twinFolder, report,
         wantReport, matchesOnly]() mutable {
        seh::installSehTranslator();
        QString msg;
        seh::runGuarded("wardrobe-bulk-glb", [&] {
            QElapsedTimer timer;
            timer.start();
            std::vector<GlbExporter::AnimExport> anims = baseAnims;
            int animSkipped = 0;
            for (const auto& [nm, id] : allRefs) {
                const std::vector<uint8_t> raw = store->mpk().readAsset((size_t)id);
                auto clip = std::make_shared<di::AnimClip>();
                std::string aerr;
                if (!raw.empty() &&
                    di::parseAnim(raw.data(), raw.size(), clip.get(), &aerr))
                    anims.push_back({nm, clip});
                else
                    ++animSkipped;
            }
            if (!allRefs.isEmpty())
                qInfo("wardrobe bulk: %zu clips parsed, %d skipped",
                      anims.size(), animSkipped);

            // The twin rig, loaded once for the whole run.
            std::shared_ptr<di::BoneParents> tHier;
            std::shared_ptr<di::BoneLocals>  tLocals;
            if (doTwin) {
                auto h = std::make_shared<di::BoneParents>();
                auto l = std::make_shared<di::BoneLocals>();
                if (!twinFolder.isEmpty() &&
                    loadClassRig(*store, twinFolder.toStdString(), h.get(), l.get())) {
                    tHier = h;
                    tLocals = l;
                } else {
                    qWarning("wardrobe bulk: no rig for the opposite-gender class "
                             "(%s) - twins written against the equipped rig",
                             qPrintable(twinFolder.isEmpty()
                                            ? QStringLiteral("unresolved")
                                            : twinFolder));
                    tHier = hier;
                    tLocals = locals;
                }
            }

            TexCache texCache;   // shared by every lane, keyed by texture blob

            // Assemble one outfit's parts. `idxs`/`names`/`slotOf` are parallel;
            // slotOf < 0 marks a pool attachment (already in character space).
            // Shared by the normal pass and the opposite-gender pass, which
            // differ only in which repo entries and which rig they use.
            auto buildOutfit =
                [&](const QList<int>& idxs, const QStringList& names,
                    const QList<int>& slotOf,
                    const std::shared_ptr<di::BoneParents>& rigHier,
                    const std::shared_ptr<di::BoneLocals>& rigLocals,
                    std::vector<MeshTextures>* looseOut)
                -> std::vector<GlbExporter::Part> {
                std::vector<GlbExporter::Part> parts;
                for (int pi = 0; pi < idxs.size(); ++pi) {
                    GlbExporter::Part p;
                    MeshTextures mt;
                    if (!buildPartFromRepo(*store, idxs[pi], names.value(pi),
                                           haveTex || wantLoose, &p, &mt, &texCache))
                        continue;
                    // HAND SLOT: same holder rename the interactive loader does.
                    // Without it the weapon keeps its own one-bone skin name,
                    // the exporter finds no rig node for it, and the sword is
                    // written at the world origin in weapon space.
                    const int slot = slotOf.value(pi, -1);
                    if ((slot == kMainHand || slot == kOffHand) && p.skel &&
                        rigHier && rigLocals && p.skel->bones.size() == 1) {
                        const int hand = (slot == kMainHand) ? 0 : 1;
                        const di::RepoEntry& e =
                            store->repo()->entries[(size_t)idxs[pi]];
                        const std::string pieceName = e.name;
                        const size_t us = pieceName.find('_');
                        std::string node;
                        float world[12];
                        bool placed = false;
                        if (us != std::string::npos) {
                            std::string type = pieceName.substr(0, us);
                            for (char& c : type)
                                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                            node = kHandPrefix[hand] + type;
                            placed = di::worldOfBone(*rigHier, *rigLocals, node, world);
                        }
                        if (!placed) {
                            node = kGenericHolder[hand];
                            placed = di::worldOfBone(*rigHier, *rigLocals, node, world);
                        }
                        if (placed) {
                            auto sk = std::make_shared<di::SkinSkeleton>(*p.skel);
                            for (size_t b = 0; b < sk->bones.size(); ++b)
                                sk->bones[b].name = node;
                            p.skel = sk;
                        } else {
                            qWarning("wardrobe bulk: no holder node for %s "
                                     "(slot %d) - exported at the origin",
                                     pieceName.c_str(), slot);
                        }
                    }
                    // Embedding off: keep the decoded maps for the loose write
                    // but hand the exporter a texture-free part.
                    if (looseOut) looseOut->push_back(mt);
                    if (!haveTex) p.textures = MeshTextures();
                    parts.push_back(std::move(p));
                }
                return parts;
            };

            // Write one .glb plus whatever extras are switched on. Returns the
            // exporter's own error text on failure.
            std::mutex extrasMtx;   // deps/ and textures/ are shared between sets
            // Output paths already handed to a lane but not yet on disk. See
            // writeOne for why uniquePath() alone is not enough.
            std::mutex pathMtx;
            std::set<QString> takenPaths;
            auto writeOne =
                [&](const QString& outDir, const QString& stem,
                    const std::vector<GlbExporter::Part>& parts,
                    const std::vector<MeshTextures>& looseTex,
                    const std::vector<GlbExporter::AnimExport>& jobAnims,
                    const QList<int>& idxs,
                    const std::shared_ptr<di::BoneParents>& rigHier,
                    const std::shared_ptr<di::BoneLocals>& rigLocals,
                    QString* err) -> bool {
                // Within-run name collisions were already ruled out when the
                // units were named; this is about what is ALREADY on disk, which
                // is export/overwrite's question.
                //
                // Resolved AND reserved under one lock. uniquePath() is
                // check-then-write, and its "_2" scheme is the same one the
                // naming pass uses — so two lanes could independently land on
                // the same "_2" name (one finds "sword.glb" on disk, the other
                // was already named "sword_2") and the second write would eat
                // the first. Reserving the resolved path closes both halves.
                const QString want = QDir(outDir).filePath(stem + QStringLiteral(".glb"));
                QString out;
                {
                    std::lock_guard<std::mutex> g(pathMtx);
                    out = ExportSettings::uniquePath(want);
                    while (takenPaths.count(out.toLower())) {
                        // Reserved by another lane this run but not on disk yet.
                        const QFileInfo fi(out);
                        out = fi.absolutePath() + QLatin1Char('/') +
                              fi.completeBaseName() + QStringLiteral("_x") +
                              QString::number((int)takenPaths.size()) +
                              QStringLiteral(".glb");
                    }
                    takenPaths.insert(out.toLower());
                }
                if (!GlbExporter::writeGlb(out, parts, rigHier.get(), rigLocals.get(),
                                           jobAnims, stem, err)) {
                    qWarning("wardrobe bulk: %s FAILED (%s)", qPrintable(out),
                             qPrintable(*err));
                    if (wantReport) report->failed(out, *err);
                    return false;
                }
                if (wantReport) {
                    report->wrote(out,
                                  QStringLiteral("%1 part(s)").arg(parts.size()));
                    if (out != want)
                        report->note(QStringLiteral("%1 already existed — written "
                                                    "as %2 (overwrite is off)")
                                         .arg(QFileInfo(want).fileName(),
                                              QFileInfo(out).fileName()));
                }
                if (!wantDeps && !wantLoose) return true;
                // Two sets can share a piece, so two lanes can target the same
                // dep file. Serialised rather than raced: these writes are small
                // next to the decode work the lock does NOT cover.
                std::lock_guard<std::mutex> g(extrasMtx);
                if (wantDeps)
                    for (int ri : idxs)
                        di::writeRawDeps(*store, ri, QString(),
                                         QDir(outDir).filePath(QStringLiteral("deps")));
                if (wantLoose)
                    for (size_t k = 0; k < parts.size() && k < looseTex.size(); ++k)
                        di::writeLooseTextures(
                            looseTex[k], parts[k].name,
                            QDir(outDir).filePath(QStringLiteral("textures")));
                return true;
            };

            // ── The run ────────────────────────────────────────────────────
            // Sets are independent: each resolves its own pieces, bakes its own
            // cloth and writes its own file. MpkIndex serialises reads per pak
            // internally and the parsers hold no global state, so the only
            // shared things are the texture cache and the extras folders, both
            // locked above. Lanes are capped at 4 because each one holds a
            // whole outfit's decoded maps in flight.
            std::atomic<size_t> nextJob{0};
            std::atomic<int> written{0}, failed{0}, doneCount{0};
            // Counted apart from `written` so the summary can say "no
            // counterparts in the data" instead of leaving the user to wonder
            // whether the option did anything.
            std::atomic<int> twinWritten{0}, twinAttempted{0};
            std::atomic<bool> canceled{false}, abandoned{false};
            const int hw = (int)std::thread::hardware_concurrency();
            const int lanes = qBound(1, hw > 1 ? hw - 1 : 1,
                                     std::min(4, (int)jobs.size()));

            // One set, start to finish. Called inside a per-set SEH guard so a
            // single malformed blob costs that outfit and nothing else — the
            // old single-threaded loop guarded the WHOLE run, so one bad set
            // took every set after it down with it.
            auto exportSet = [&](const SetJob& j) {
                std::vector<MeshTextures> looseTex;
                std::vector<GlbExporter::Part> parts =
                    buildOutfit(j.repoIdx, j.names, j.slotOf, hier, locals,
                                &looseTex);
                if (parts.empty()) { ++failed; return; }
                // Per-set cloth bake: parts differ per set, so bake this
                // set's cloth pieces into a per-set copy of the clip list.
                std::vector<GlbExporter::AnimExport> jobAnims = anims;
                if (clothBake && hier && locals)
                    for (GlbExporter::AnimExport& a : jobAnims)
                        for (const GlbExporter::Part& pt : parts)
                            if (a.clip && pt.skel && di::hasClothBones(*pt.skel))
                                a.clip = di::bakeCloth(*a.clip, *pt.skel, *hier,
                                                       *locals, clothP);
                // Folder layout applies to the run, not to one file: every
                // set is its own group, so "by model" gives each outfit a
                // self-contained folder with its textures and raw sources.
                QString outDir = dir;
                if (layout == ExportLayout::kModel())
                    outDir = QDir(dir).filePath(
                        ExportLayout::safeSegment(j.stem, j.key));
                else if (layout == ExportLayout::kType())
                    outDir = QDir(dir).filePath(QStringLiteral("Model"));
                else if (layout == ExportLayout::kFolder())
                    outDir = QDir(dir).filePath(ExportLayout::safeSegment(cls, cls));
                if (outDir != dir) {
                    std::lock_guard<std::mutex> g(extrasMtx);
                    QDir().mkpath(outDir);
                }
                // ── Both genders ──────────────────────────────────────
                // Both names, and whether a pair exists at all, were settled on
                // the GUI thread — see nameUnit. Doing it here meant naming went
                // through the File-names template and the item-name table from a
                // raw worker thread, and it also produced "<piece>_F.glb" with
                // no _M anywhere for every genderless asset (the hand weapons in
                // Char/item are ALL genderless, and they are most of what the
                // match pass exports).
                const bool paired = doTwin && !j.twinRepoIdx.isEmpty();

                QString gerr;
                if (writeOne(outDir, j.stem, parts, looseTex, jobAnims, j.repoIdx,
                             hier, locals, &gerr))
                    ++written;
                else
                    ++failed;

                if (paired) {
                    ++twinAttempted;
                    std::vector<MeshTextures> tLoose;
                    std::vector<GlbExporter::Part> tParts =
                        buildOutfit(j.twinRepoIdx, j.twinNames, j.twinSlotOf,
                                    tHier, tLocals, &tLoose);
                    if (tParts.empty()) {
                        // Entries existed but nothing built: a real failure, not
                        // the "this piece has no twin" case, which never gets
                        // here. Counting it keeps "(N failed)" honest — a
                        // per-piece run has thousands of units and a silently
                        // missing one is invisible.
                        ++failed;
                        qWarning("wardrobe bulk: twin of %s built no parts",
                                 qPrintable(j.stem));
                    } else {
                        std::vector<GlbExporter::AnimExport> tAnims = anims;
                        if (clothBake && tHier && tLocals)
                            for (GlbExporter::AnimExport& a : tAnims)
                                for (const GlbExporter::Part& pt : tParts)
                                    if (a.clip && pt.skel && di::hasClothBones(*pt.skel))
                                        a.clip = di::bakeCloth(*a.clip, *pt.skel,
                                                               *tHier, *tLocals, clothP);
                        QString terr;
                        if (writeOne(outDir, j.twinStem, tParts, tLoose, tAnims,
                                     j.twinRepoIdx, tHier, tLocals, &terr))
                            ++twinWritten;
                        else
                            ++failed;
                    }
                }
            };

            auto lane = [&] {
                seh::installSehTranslator();
                for (;;) {
                    const size_t ji = nextJob.fetch_add(1);
                    if (ji >= jobs.size()) return;
                    if (generation != m_generation.load()) { abandoned.store(true); return; }
                    // Cancel is polled BEFORE a set starts, so the sets already
                    // in flight always finish and no half-written outfit lands.
                    if (m_bulkCancel.load()) { canceled.store(true); return; }
                    const SetJob& j = jobs[ji];
                    if (!seh::runGuarded("wardrobe-bulk-set", [&] { exportSet(j); })) {
                        ++failed;
                        qWarning("wardrobe bulk: set %s crashed, skipped",
                                 qPrintable(j.key));
                    }
                    ++doneCount;
                    Q_EMIT bulkProgress(generation,
                                        QStringLiteral("exporting… %1/%2 (%3)")
                                            .arg(doneCount.load())
                                            .arg(jobs.size()).arg(j.key));
                }
            };

            std::vector<std::thread> pool;
            pool.reserve((size_t)std::max(0, lanes - 1));
            for (int i = 1; i < lanes; ++i) pool.emplace_back(lane);
            lane();                                   // this thread is lane 0
            for (std::thread& t : pool) t.join();

            if (abandoned.load())
                qInfo("wardrobe bulk: abandoned after %d files (class changed)",
                      written.load());
            if (canceled.load())
                qInfo("wardrobe bulk: cancelled after %d files", written.load());
            qInfo("wardrobe bulk: %d lanes, texture cache %d hit / %d miss, %d flush",
                  lanes, texCache.hits, texCache.misses, texCache.flushes);
            if (wantReport) {
                if (abandoned.load())
                    report->note(QStringLiteral("abandoned — the class changed "
                                                "while the run was going"));
                if (canceled.load()) report->note(QStringLiteral("cancelled by the user"));
                const QString rp = report->write(canceled.load());
                if (!rp.isEmpty())
                    qInfo("wardrobe bulk: report -> %s", qPrintable(rp));
            }

            msg = canceled.load()
                      ? QStringLiteral("cancelled after %1 of %2 unit(s) (%3 failed)")
                            .arg(doneCount.load()).arg(jobs.size()).arg(failed.load())
                      : QStringLiteral("exported %1 file(s) to %2 (%3 failed, %4 ms)")
                            .arg(written.load() + twinWritten.load())
                            .arg(dir).arg(failed.load()).arg(timer.elapsed());
            // Not on a cancelled run: "no counterpart exists in the data" is a
            // claim about the DATA, and a run stopped after two of six hundred
            // units has not looked at enough of it to make one.
            if (doTwin && !canceled.load()) {
                // Three different outcomes, and they used to read as one: a
                // twin that was never attempted (nothing in the data), one that
                // was attempted and failed, and one that worked. Saying "no
                // counterpart exists" after N failures is the opposite of true.
                if (twinWritten.load() > 0)
                    msg += QStringLiteral(" · both genders (%1 + %2): %3 twin file(s)")
                               .arg(genderTag, twinTag).arg(twinWritten.load());
                else if (twinAttempted.load() > 0)
                    msg += QStringLiteral(" · both genders: all %1 %2 twin(s) FAILED")
                               .arg(twinAttempted.load()).arg(twinTag);
                else
                    msg += QStringLiteral(" · both genders: no %1 counterpart "
                                          "exists in the data for this class")
                               .arg(twinTag);
            }
            const int total = written.load() + twinWritten.load();
            if (!canceled.load() && total > 0 && ExportSettings::notifyOnFinish())
                ExportNotifier::instance().notify(
                    QStringLiteral("Exported %1 file%2").arg(total)
                            .arg(total == 1 ? QString() : QStringLiteral("s")) +
                        ExportNotifier::optionsLine(haveTex, wantLoose, wantDeps,
                                                    (int)baseAnims.size() +
                                                        allRefs.size()),
                    dir, failed.load());
        });
        if (msg.isEmpty()) msg = QStringLiteral("bulk set export crashed (see log)");
        Q_EMIT exportDone(msg);
    });
    m_bulkRunning = true;
    m_workers.insert(worker);

    // A modeless progress window driven by bulkProgress. Modeless on purpose:
    // the run is on a worker, so the app stays usable, and a modal dialog would
    // only pretend otherwise. Cancel sets the flag each lane polls before it
    // picks up its next set — the sets already in flight still finish, so
    // nothing half-written lands.
    auto* prog = new QProgressDialog(
        matchesOnly ? QStringLiteral("Exporting set matches…")
                    : QStringLiteral("Exporting armor sets…"),
        QStringLiteral("Cancel"), 0, (int)jobCount, this);
    prog->setWindowTitle(matchesOnly ? QStringLiteral("Export set matches")
                                     : QStringLiteral("Export all sets"));
    prog->setWindowModality(Qt::NonModal);
    prog->setMinimumDuration(0);
    prog->setAutoClose(false);
    prog->setAutoReset(false);
    prog->setValue(0);
    connect(prog, &QProgressDialog::canceled, this, [this, prog] {
        m_bulkCancel.store(true);
        // QProgressDialog wires its OWN canceled() -> cancel() when the cancel
        // button is created, i.e. before this connection, and cancel() force-
        // hides the dialog no matter what setAutoClose(false) says. The lanes
        // in flight keep writing files for several more seconds, so leaving it
        // hidden would tell the user the export had stopped while it had not.
        // Put it back and say what is actually happening.
        prog->setLabelText(
            QStringLiteral("Cancelling — finishing the sets already in flight…"));
        prog->show();
    });
    // The bar counts with its OWN tally rather than prog->value() + 1: the
    // forced cancel above calls reset(), which rewinds the dialog's value, and
    // the count must keep meaning "sets finished".
    connect(this, &WardrobeTab::bulkProgress, prog,
            [prog, done = 0](int, const QString& message) mutable {
                prog->setLabelText(message);
                prog->setValue(++done);
            });
    connect(worker, &QThread::finished, prog, &QObject::deleteLater);

    connect(worker, &QThread::finished, this, [this, worker] {
        m_bulkRunning = false;
        m_bulkCancel.store(false);
        // A run can write files into the folder the next run reads, and the
        // settings may have been changed while it ran — recount rather than
        // restore whatever the labels said before.
        invalidateSetsCount();
        syncExportButtons();
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

// Shared .glb export path for single pieces and full outfits. Reads the
// Settings > Export animations mode; clip parsing for "all" happens in the
// worker (hundreds of clips are possible — the log carries the count).
// ── Export helpers (settings-driven, shared by every Wardrobe export path) ──

// The parts an outfit export writes. Honours export/wardrobeScope and, unless
// export/hiddenParts is on, the viewport's hidden-part state — so by default
// the file matches what you were looking at.
std::vector<GlbExporter::Part> WardrobeTab::outfitParts(QList<int>* outRepoIdx) const
{
    const bool itemsOnly =
        ExportSettings::wardrobeScope() == ExportSettings::WardrobeItems;
    const bool keepHidden = ExportSettings::exportHiddenParts();
    std::vector<GlbExporter::Part> parts;
    for (int s = 0; s < kParts; ++s) {
        const SlotState& S = m_slots[s];
        if (!S.part.mesh) continue;
        // "Equipped items only" drops the base body pieces (face/neck/eyes/
        // lashes/beard) — the gear is what you fit onto your own character.
        if (itemsOnly && s >= kSlots && s < kSlots + kBodySlots) continue;
        if (!keepHidden && m_partHidden.contains(s)) continue;
        GlbExporter::Part p;
        p.mesh     = S.part.mesh;
        p.skel     = S.skel;
        p.name     = S.name.isEmpty() ? partLabel(s) : S.name;
        // Textures are embedded unless the user asked for a geometry-only file.
        if (ExportSettings::includeTextures()) p.textures = S.part.textures;
        parts.push_back(std::move(p));
        if (outRepoIdx) *outRepoIdx << S.repoIdx;
    }
    return parts;
}

// Which clips to embed. In the Wardrobe the meaningful source is `base`: armour
// carries no animation of its own, so the CLASS RIG's clips (which is exactly
// what m_animBox holds) are what drives an outfit.
QList<QPair<QString, quint32>> WardrobeTab::outfitAnimRefs(
    const AnimExportScope& sc) const
{
    QList<QPair<QString, quint32>> refs;
    if (!sc.includeAnim || !m_animIds || !m_animBox) return refs;
    const QString playing = currentClipName();
    QSet<QString> taken;
    // The previewed clip is an explicit choice and is never filtered out.
    if (sc.previewed && !playing.isEmpty()) {
        const int i = m_animBox->currentIndex();
        if (i > 0 && i - 1 < m_animIds->size()) {
            refs.append({playing, (*m_animIds)[i - 1]});
            taken.insert(playing);
        }
    }
    // `original` and `base` name the same pool here — a class folder's clips ARE
    // the base-rig clips its armour inherits — so either ticked pulls the list,
    // deduped against whatever `previewed` already added.
    if (sc.base || sc.original) {
        for (int i = 0; i < m_animIds->size(); ++i) {
            const QString nm = m_animBox->itemText(i + 1);
            if (nm.isEmpty() || taken.contains(nm)) continue;
            if (!sc.accepts(nm, /*frames=*/-1)) continue;
            refs.append({nm, (*m_animIds)[i]});
            taken.insert(nm);
        }
    }
    return refs;
}

// The same cosmetic on the opposite-gender rig. DI names pieces
// "<g>_<class>_<slot>_<set>" (f_barbarian_yifu_t07_004), so the twin is a rename
// away — and a repository lookup proves whether it actually exists rather than
// assuming symmetry.
int WardrobeTab::oppositeGenderPiece(int repoIdx) const
{
    if (!m_idx || !m_idx->store) return -1;
    const di::Repository* repo = m_idx->store->repo();
    return repo ? twinOfRepoEntry(*repo, repoIdx) : -1;
}

// The outfit's filename, from export/wardrobeNameTemplate.
// Split "Char/f_barbarian" into the class token and its gender tag, the way
// every filename in this tab spells them. One implementation, because the bulk
// export and the single-outfit export must produce identical names from the
// same template — they did not, which is what made "Export all sets" look like
// it ignored the File names page entirely.
void WardrobeTab::splitClassFolder(const QString& folder, QString* cls,
                                   QString* gender)
{
    QString c = folder.startsWith(QStringLiteral("Char/")) ? folder.mid(5) : folder;
    QString g;
    if (c.startsWith(QStringLiteral("f_")))      { g = QStringLiteral("F"); c = c.mid(2); }
    else if (c.startsWith(QStringLiteral("m_"))) { g = QStringLiteral("M"); c = c.mid(2); }
    else if (c.endsWith(QStringLiteral("_f")))   { g = QStringLiteral("F"); c.chop(2); }
    else if (c.endsWith(QStringLiteral("_m")))   { g = QStringLiteral("M"); c.chop(2); }
    if (cls)    *cls = c;
    if (gender) *gender = g;
}

// The outfit file name from export/wardrobeNameTemplate. `slotName` supplies
// the {{Helmet}}…{{Off}} placeholders for slots 0-7; anything it has no entry
// for renders empty and NameTemplate collapses the separator it orphans.
QString WardrobeTab::outfitStem(const QString& classFolder, const QString& setKey,
                                const QString& setName,
                                const QHash<int, QString>& slotName)
{
    QString cls, gender;
    splitClassFolder(classFolder, &cls, &gender);
    if (cls.isEmpty()) cls = QStringLiteral("class");
    QHash<QString, QString> v;
    v.insert(QStringLiteral("Class"), cls);
    v.insert(QStringLiteral("Gender"), gender);
    v.insert(QStringLiteral("Set"), setKey.isEmpty() ? QStringLiteral("custom") : setKey);
    v.insert(QStringLiteral("Name"), setName);
    static const char* const kSlotVar[] = {"Helmet", "Chest", "Shoulders", "Legs",
                                           "Hair", "Back", "Main", "Off"};
    for (int i = 0; i < 8; ++i)
        v.insert(QLatin1String(kSlotVar[i]), slotName.value(i));
    return NameTemplate::outfit(v, cls + QStringLiteral("_outfit"));
}

QString WardrobeTab::outfitFileName() const
{
    const int ci = m_classBox ? m_classBox->currentIndex() : -1;
    const QString folder = (ci >= 0 && ci < m_classFolders.size())
                               ? m_classFolders[ci] : QString();
    const int si = m_setBox ? m_setBox->currentIndex() : 0;
    const bool haveSet = si > 0 && si - 1 < m_setKeys.size();
    QHash<int, QString> slotName;
    for (int i = 0; i < 8 && i < kParts; ++i)
        if (!m_slots[i].name.isEmpty()) slotName.insert(i, m_slots[i].name);
    return outfitStem(folder,
                      haveSet ? m_setKeys[si - 1] : QString(),
                      haveSet ? setLabel(m_setKeys[si - 1]) : QString(),
                      slotName);
}

// `partRepoIdx`: the repository entry behind each element of `parts`, in the
// same order. It is what the raw-deps, loose-texture and opposite-gender-twin
// passes work from. It used to be swept out of m_slots unconditionally, which
// was right for the outfit export and WRONG for every other caller: "Export
// this piece…" wrote the whole outfit's raw dependencies, the whole outfit's
// loose PNGs, and a complete outfit twin .glb beside a single-piece file.
// Empty (the default) still means "the equipped outfit", because that is what
// the outfit export passes and what its `parts` genuinely are.
void WardrobeTab::exportGlb(const QString& suggestedBase,
                            std::vector<GlbExporter::Part> parts,
                            const QList<int>& partRepoIdx)
{
    if (!m_idx || !m_idx->store) return;
    QString base = suggestedBase;
    base.replace(QLatin1Char('/'), QLatin1Char('_'));
    base.replace(QLatin1Char('.'), QLatin1Char('_'));
    // Same rule as the bulk run: the prompt opens where you last saved, with
    // the templated name already filled in.
    QSettings dirSettings;
    const QString lastDir =
        dirSettings.value(QLatin1String(kWardrobeDirKey)).toString();
    const QString seed = QDir(lastDir).exists()
                             ? QDir(lastDir).filePath(base + QStringLiteral(".glb"))
                             : base + QStringLiteral(".glb");
    const QString dest = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export .glb"), seed,
        QStringLiteral("glTF binary (*.glb)"));
    if (dest.isEmpty()) return;
    dirSettings.setValue(QLatin1String(kWardrobeDirKey),
                         QFileInfo(dest).absolutePath());

    // Which clips go in is Settings ▸ Export ▸ Animations to embed. The already
    // decoded playing clip is reused verbatim; everything else is parsed on the
    // worker from its entry id.
    const AnimExportScope scope = AnimExportScope::load();
    std::vector<GlbExporter::AnimExport> anims;
    QList<QPair<QString, quint32>> allRefs;
    const QString playingName = currentClipName();
    for (const auto& r : outfitAnimRefs(scope)) {
        if (m_clip && r.first == playingName) anims.push_back({r.first, m_clip});
        else                                  allRefs.append(r);
    }
    // Raw sources / loose PNGs are written next to the .glb once it lands.
    const bool wantDeps  = ExportSettings::withRawDeps();
    const bool wantLoose = ExportSettings::looseTextures();
    const bool haveTex   = ExportSettings::includeTextures();
    QList<int> partRepo;
    std::vector<MeshTextures> partTex;
    if (partRepoIdx.isEmpty()) {
        for (int sl = 0; sl < kParts; ++sl) {
            if (!m_slots[sl].part.mesh) continue;
            partRepo << m_slots[sl].repoIdx;
            partTex.push_back(m_slots[sl].part.textures);
        }
    } else {
        partRepo = partRepoIdx;
        for (const GlbExporter::Part& p : parts) partTex.push_back(p.textures);
    }
    // Opposite-gender twin: resolved HERE, on the GUI thread, because it is a
    // repository lookup per piece. A piece with no twin in the data (shared
    // item-folder weapons are genderless) simply drops out, so the twin file is
    // whatever of the outfit actually exists on the other rig.
    QList<int>  twinRepo;
    QStringList twinNames;
    QString genderTag, twinTag, twinFolder, twinBase;
    if (ExportSettings::bothGenders()) {
        const int ci = m_classBox ? m_classBox->currentIndex() : -1;
        const QString folder = (ci >= 0 && ci < m_classFolders.size())
                                   ? m_classFolders[ci] : QString();
        const QString leaf = folder.mid(5);
        if (leaf.startsWith(QStringLiteral("f_")) || leaf.endsWith(QStringLiteral("_f")))
            { genderTag = QStringLiteral("F"); twinTag = QStringLiteral("M"); }
        else if (leaf.startsWith(QStringLiteral("m_")) || leaf.endsWith(QStringLiteral("_m")))
            { genderTag = QStringLiteral("M"); twinTag = QStringLiteral("F"); }
        // The twin's own class folder, and only if the scan actually found it.
        // Its rig is loaded on the worker: the two genders' skeletons differ in
        // proportion, so the twin file must be written against ITS hierarchy,
        // not the one currently equipped.
        const QString tf = twinClassFolder(folder);
        if (!tf.isEmpty() && m_classFolders.contains(tf)) twinFolder = tf;
        if (!twinTag.isEmpty() && m_idx && m_idx->store) {
            // The twin of what is actually being exported — partRepo, not the
            // outfit — so a single-piece export gets a single-piece twin.
            const di::Repository* repo = m_idx->store->repo();
            for (int ri : partRepo) {
                const int t = oppositeGenderPiece(ri);
                if (t < 0 || !repo || (size_t)t >= repo->entries.size()) continue;
                twinRepo  << t;
                twinNames << QString::fromStdString(repo->entries[(size_t)t].name);
            }
            // The twin's name, from ITS class folder through the SAME template,
            // so a template carrying {{Gender}} tells the pair apart on its own.
            // Appending "_F"/"_M" to one stem produced "barbarian_F_custom_F"
            // beside "barbarian_F_custom_M" — both claiming to be the female
            // outfit — and renamed the file the user had just named in the save
            // dialog to get there.
            //
            // Only for a WHOLE-OUTFIT export (partRepoIdx empty). A single
            // piece has no outfit to describe, and the outfit template's slot
            // placeholders would describe the equipped outfit rather than the
            // piece being written — there, the plain gender tag is right.
            if (partRepoIdx.isEmpty() && !twinRepo.isEmpty() && repo) {
                QHash<int, QString> twinSlotName;
                for (int sl = 0; sl < 8 && sl < kParts; ++sl) {
                    if (!m_slots[sl].part.mesh) continue;
                    const int t = oppositeGenderPiece(m_slots[sl].repoIdx);
                    if (t < 0 || (size_t)t >= repo->entries.size()) continue;
                    twinSlotName.insert(
                        sl, QString::fromStdString(repo->entries[(size_t)t].name));
                }
                const int si = m_setBox ? m_setBox->currentIndex() : 0;
                const bool haveSet = si > 0 && si - 1 < m_setKeys.size();
                twinBase =
                    outfitStem(twinFolder.isEmpty() ? folder : twinFolder,
                               haveSet ? m_setKeys[si - 1] : QString(),
                               haveSet ? setLabel(m_setKeys[si - 1]) : QString(),
                               twinSlotName);
            }
        }
    }
    auto hier   = m_hier;
    auto locals = m_locals;
    auto store  = m_idx->store;
    // Cloth state read on the GUI thread (never from the worker).
    const bool clothBake = clothOn();
    const di::ClothParams clothP = cloth::paramsFromSettings();
    QThread* worker = QThread::create(
        [this, parts = std::move(parts), hier, locals, anims, allRefs, store,
         base, dest, clothBake, clothP, wantDeps, wantLoose, haveTex,
         partRepo, partTex, twinRepo, twinNames, genderTag, twinTag,
         twinFolder, twinBase]() mutable {
        seh::installSehTranslator();
        QString msg;
        seh::runGuarded("wardrobe-glb", [&] {
            QElapsedTimer timer;
            timer.start();
            // Bake each cloth piece from the RAW clip and merge the results.
            // Chaining (feeding part k-1's output into part k) is what the
            // viewport does NOT do: it bakes every slot independently from the
            // raw clip, and a chained bake sees the previous part's appended
            // cloth tracks as pre-existing anchors, which changes the solve and
            // lets two pieces sharing a bone overwrite each other. Baking
            // independently and merging by name is what makes the file match
            // what you were looking at.
            auto bakeAll = [&](std::shared_ptr<const di::AnimClip> c)
                -> std::shared_ptr<const di::AnimClip> {
                if (!clothBake || !c || !hier || !locals) return c;
                auto merged = std::make_shared<di::AnimClip>(*c);
                bool any = false;
                for (const GlbExporter::Part& pt : parts) {
                    if (!pt.skel || !di::hasClothBones(*pt.skel)) continue;
                    const auto baked =
                        di::bakeCloth(*c, *pt.skel, *hier, *locals, clothP);
                    if (!baked) continue;
                    di::mergeClipTracks(*merged, *baked);
                    any = true;
                }
                return any ? std::shared_ptr<const di::AnimClip>(merged) : c;
            };
            if (!allRefs.isEmpty()) {
                int failed = 0;
                for (const auto& [nm, id] : allRefs) {
                    const std::vector<uint8_t> raw =
                        store->mpk().readAsset((size_t)id);
                    auto clip = std::make_shared<di::AnimClip>();
                    std::string aerr;
                    if (!raw.empty() &&
                        di::parseAnim(raw.data(), raw.size(), clip.get(), &aerr)) {
                        anims.push_back({nm, clip});
                    } else {
                        ++failed;
                        qInfo("wardrobe glb: clip %s skipped (%s)",
                              qPrintable(nm), aerr.c_str());
                    }
                }
                qInfo("wardrobe glb: all-clips mode — %zu parsed, %d skipped, "
                      "%lld ms",
                      anims.size(), failed, (long long)timer.elapsed());
            }
            for (GlbExporter::AnimExport& a : anims) a.clip = bakeAll(a.clip);
            QString gerr;
            if (GlbExporter::writeGlb(dest, parts, hier.get(), locals.get(),
                                      anims, base, &gerr)) {
                // Extras land only after the .glb itself is on disk: a deps
                // folder beside a file that failed to write is just litter.
                const QString outDir = QFileInfo(dest).absolutePath();
                int deps = 0, loose = 0;
                if (wantDeps)
                    for (int ri : partRepo)
                        deps += di::writeRawDeps(*store, ri, QString(),
                                                 QDir(outDir).filePath(
                                                     QStringLiteral("deps")));
                if (wantLoose)
                    for (size_t k = 0; k < partTex.size() && k < parts.size(); ++k)
                        loose += di::writeLooseTextures(
                            partTex[k], parts[k].name,
                            QDir(outDir).filePath(QStringLiteral("textures")));
                msg = QStringLiteral("exported %1 (%2 parts, %3, %4 ms)")
                          .arg(dest)
                          .arg(parts.size())
                          .arg(anims.empty()
                                   ? QStringLiteral("no anims")
                                   : QStringLiteral("%1 anims").arg(anims.size()))
                          .arg(timer.elapsed());
                if (deps)  msg += QStringLiteral(" + %1 raw file(s)").arg(deps);
                if (loose) msg += QStringLiteral(" + %1 loose PNG(s)").arg(loose);

                // ── Both genders ──────────────────────────────────────────
                // The first file is RENAMED to carry its own gender, so the
                // pair is symmetric rather than one plain name plus one
                // suffixed oddity. Renaming only once the twin actually built
                // means a failed twin never leaves a mysteriously renamed file.
                if (!twinRepo.isEmpty() && !twinTag.isEmpty()) {
                    // The twin's OWN rig. Falling back to the equipped rig
                    // would put the other gender's mesh on this gender's bind
                    // pose — visibly wrong limb lengths — so the fallback is
                    // logged rather than silent.
                    auto tHier   = std::make_shared<di::BoneParents>();
                    auto tLocals = std::make_shared<di::BoneLocals>();
                    if (twinFolder.isEmpty() ||
                        !loadClassRig(*store, twinFolder.toStdString(),
                                      tHier.get(), tLocals.get())) {
                        qWarning("wardrobe: no rig for the opposite-gender class "
                                 "(%s) - twin written against the equipped rig",
                                 qPrintable(twinFolder.isEmpty()
                                                ? QStringLiteral("unresolved")
                                                : twinFolder));
                        tHier = hier;
                        tLocals = locals;
                    }
                    std::vector<GlbExporter::Part> tparts;
                    std::vector<MeshTextures> ttex;
                    for (int k = 0; k < twinRepo.size(); ++k) {
                        GlbExporter::Part tp;
                        MeshTextures tt;
                        if (buildPartFromRepo(*store, twinRepo[k],
                                              twinNames.value(k),
                                              haveTex || wantLoose, &tp, &tt)) {
                            if (!haveTex) tp.textures = MeshTextures();
                            tparts.push_back(std::move(tp));
                            ttex.push_back(tt);
                        }
                    }
                    if (!tparts.empty()) {
                        const QFileInfo fi(dest);
                        const QString stem = fi.completeBaseName();
                        // The twin's own templated name. Only when the template
                        // cannot tell the two apart — no {{Gender}} in it, or
                        // the user renamed in the save dialog to exactly this —
                        // do the gender tags go on, and only then is the file
                        // the user just named renamed to match.
                        QString tstem = twinBase;
                        if (tstem.isEmpty() || tstem == stem)
                            tstem = stem + QLatin1Char('_') + twinTag;
                        const bool tagMine = (tstem == stem + QLatin1Char('_') + twinTag);
                        const QString mine =
                            tagMine ? fi.dir().filePath(stem + QLatin1Char('_') +
                                                        genderTag + QStringLiteral(".glb"))
                                    : dest;
                        const QString other = ExportSettings::uniquePath(
                            fi.dir().filePath(tstem + QStringLiteral(".glb")));
                        QString terr;
                        if (GlbExporter::writeGlb(other, tparts, tHier.get(),
                                                  tLocals.get(), anims, tstem,
                                                  &terr)) {
                            if (mine != dest) {
                                QFile::remove(mine);
                                QFile::rename(dest, mine);
                            }
                            if (wantDeps)
                                for (int ri : twinRepo)
                                    di::writeRawDeps(*store, ri, QString(),
                                                     QDir(outDir).filePath(
                                                         QStringLiteral("deps")));
                            if (wantLoose)
                                for (size_t k = 0; k < tparts.size() && k < ttex.size(); ++k)
                                    di::writeLooseTextures(
                                        ttex[k], tparts[k].name,
                                        QDir(outDir).filePath(QStringLiteral("textures")));
                            msg += QStringLiteral(" · both genders (%1 + %2)")
                                       .arg(genderTag, twinTag);
                        } else {
                            msg += QStringLiteral(" · opposite gender FAILED: %1").arg(terr);
                        }
                    } else {
                        msg += QStringLiteral(" · no opposite-gender twin in the data");
                    }
                }
                if (ExportSettings::notifyOnFinish())
                    ExportNotifier::instance().notify(
                        QStringLiteral("Exported %1")
                                .arg(QFileInfo(dest).fileName()) +
                            ExportNotifier::optionsLine(haveTex, loose > 0,
                                                        deps > 0,
                                                        (int)anims.size()),
                        outDir);
            } else {
                msg = QStringLiteral("glb export FAILED: %1").arg(gerr);
            }
        });
        if (msg.isEmpty()) msg = QStringLiteral("glb export crashed (see log)");
        emit exportDone(msg);
    });
    m_workers.insert(worker);
    connect(worker, &QThread::finished, this, [this, worker] {
        m_workers.remove(worker);
        worker->deleteLater();
    });
    worker->start();
}

// Parts are already in the view (setPart); this recomputes the overlay and
// the status line.
void WardrobeTab::refreshView()
{
    // Equipping changes whether an opposite-gender counterpart exists, which is
    // what decides whether the outfit button says one file or two.
    syncOutfitButton();
    size_t verts = 0, nParts = 0;
    const di::SkinSkeleton* bestSkel = nullptr;
    for (int s = 0; s < kParts; ++s) {
        if (!m_slots[s].part.mesh) continue;
        ++nParts;
        verts += m_slots[s].part.mesh->vertexCount();
        if (m_slots[s].skel &&
            (!bestSkel || m_slots[s].skel->bones.size() > bestSkel->bones.size()))
            bestSkel = m_slots[s].skel.get();
    }

    // Bones overlay: the richest piece's SkinSkeleton + the class .skeleton
    // tree for parenting (lazily loaded once per class). While a clip plays,
    // animTick owns the overlay — do not stomp it with bind worlds.
    if (!bestSkel && !m_clip) {
        // last skinned piece unequipped: clear the stale overlay too
        m_view->setSkeleton({}, {});
    } else if (bestSkel && !m_clip && m_idx && m_idx->store) {
        std::vector<float> segs, joints;
        QHash<QString, int> byName;
        for (int j = 0; j < (int)bestSkel->bones.size(); ++j) {
            const di::SkinBone& b = bestSkel->bones[j];
            const QString nm = QString::fromStdString(b.name);
            if (!byName.contains(nm)) byName[nm] = j;
            joints.insert(joints.end(), {b.world[0], b.world[1], b.world[2]});
        }
        if (m_hier) {
            for (int j = 0; j < (int)bestSkel->bones.size(); ++j) {
                int pj = -1;
                std::string cur = bestSkel->bones[j].name;
                for (int hop = 0; hop < 256; ++hop) {
                    auto it = m_hier->find(cur);
                    if (it == m_hier->end() || it->second.empty()) break;
                    cur = it->second;
                    const int c = byName.value(QString::fromStdString(cur), -1);
                    if (c >= 0 && c != j) { pj = c; break; }
                }
                if (pj < 0) continue;
                const di::SkinBone& b = bestSkel->bones[j];
                const di::SkinBone& p = bestSkel->bones[pj];
                segs.insert(segs.end(), {p.world[0], p.world[1], p.world[2],
                                         b.world[0], b.world[1], b.world[2]});
            }
        }
        m_view->setSkeleton(std::move(segs), std::move(joints));
    }

    // Drop visibility overrides for parts that are no longer equipped, so a
    // hidden slot doesn't silently hide whatever gets equipped into it next.
    for (int s = 0; s < kParts; ++s)
        if (!m_slots[s].part.mesh) {
            m_partHidden.remove(s);
            m_subHidden.remove(s);
        }
    if (m_focusPart >= 0 &&
        (m_focusPart >= kParts || !m_slots[m_focusPart].part.mesh))
        m_focusPart = -1;
    if (m_focusPart < 0)                       // default focus: first live piece
        for (int s = 0; s < kParts; ++s)
            if (m_slots[s].part.mesh) { m_focusPart = s; break; }

    applyPartsMask();
    buildPartsList();
    buildEquipList();
    fillMaterialsPanel();
    fillTexturePreview(m_focusPart >= 0 ? m_slots[m_focusPart].part.textures
                                        : MeshTextures());
    fillInfoPanel();
    emit statusText(QStringLiteral("%L1 pieces · %L2 verts").arg(nParts).arg(verts));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Right-hand panel column — ported from the Models tab so both viewports
//  behave identically. (D4AssetBrowser shares PanelBox between its own Models
//  and Wardrobe tabs; this brings DI to the same parity.)
// ═══════════════════════════════════════════════════════════════════════════

QLabel* WardrobeTab::addRightPage(const QString& title, QWidget* content)
{
    if (!m_rstack || !m_rstripLay) return nullptr;
    const int page = (int)m_rsections.size();
    m_panelIndex.insert(title, page);
    auto* box = new PanelBox(title, content, m_rstack);
    box->hide();                        // up only when its strip toggle says so
    m_rstack->addWidget(box);
    m_rsections.push_back(box);
    // Key off the REGISTRATION title (its first word): the live label later
    // grows counts ("PARTS · 3 of 5 shown"), so a write-time key would drift.
    m_sectKeys << (QStringLiteral("wardrobe/panel/") +
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
    int at = m_rstripLay->count();      // insert before the trailing stretch
    if (at > 0 && m_rstripLay->itemAt(at - 1)->spacerItem()) --at;
    m_rstripLay->insertWidget(at, b);
    m_rpageBtns.push_back(b);
    return box->label;
}

void WardrobeTab::showPanel(int page, bool on)
{
    if (page < 0 || page >= (int)m_rsections.size()) return;
    PanelBox* box = m_rsections[(size_t)page];
    const bool was = !box->isHidden();
    box->setVisible(on);
    if (on && !was && !m_panelRestore) panelBoxArrive(m_rstack, box);
    savePanelLayout();
}

void WardrobeTab::movePanel(int page, int delta)
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

void WardrobeTab::savePanelLayout()
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
    QSettings().setValue(QStringLiteral("wardrobe/panels/shown"), shown);
}

void WardrobeTab::restorePanelLayout()
{
    QSettings s;
    m_panelRestore = true;
    if (!s.contains(QStringLiteral("wardrobe/panels/shown"))) {
        for (QToolButton* b : m_rpageBtns) b->setChecked(true);   // first run: all up
    } else {
        const QStringList shown =
            s.value(QStringLiteral("wardrobe/panels/shown")).toStringList();
        for (const QString& key : shown)
            for (size_t k = 0; k < m_rsections.size(); ++k)
                if (m_sectKeys.value((int)k).section(QLatin1Char('/'), -1) == key) {
                    m_rstack->addWidget(m_rsections[k]);   // re-append → saved order
                    m_rpageBtns[k]->setChecked(true);
                    break;
                }
    }
    m_panelRestore = false;
    savePanelLayout();
}

void WardrobeTab::ensurePanelVisible(const QString& title)
{
    const int page = m_panelIndex.value(title, -1);
    if (page < 0 || page >= (int)m_rpageBtns.size()) return;
    if (!m_rpageBtns[(size_t)page]->isChecked())
        m_rpageBtns[(size_t)page]->setChecked(true);   // → showPanel(page, true)
}

// Human name for a part index. Parts 0..kSlots-1 are the visible slot combos,
// the next kBodySlots are the base-body pieces, and the tail is the pool the
// set picker auto-fills — the pool is exactly what had no UI before.
QString WardrobeTab::partLabel(int part) const
{
    if (part < 0 || part >= kParts) return {};
    if (part < kSlots) return QLatin1String(kSlotLabels[part]);
    if (part < kSlots + kBodySlots)
        return QStringLiteral("Body — %1")
            .arg(QLatin1String(kBodyLabels[part - kSlots]));
    return QStringLiteral("Set piece %1").arg(part - kSlots - kBodySlots + 1);
}

// Build the six panel bodies. They are registered by the constructor straight
// after this returns; contents are filled on every equip through refreshView().
void WardrobeTab::buildPanelPages(QWidget* host)
{
    // ── INFO ───────────────────────────────────────────────────────────────
    m_infoBox = new QWidget(host);
    {
        auto* form = new QFormLayout(m_infoBox);
        form->setContentsMargins(2, 2, 2, 2);
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(3);
        form->setLabelAlignment(Qt::AlignLeft);
        for (const QString& key :
             {QStringLiteral("Character"), QStringLiteral("Set"),
              QStringLiteral("Pieces"), QStringLiteral("Vertices"),
              QStringLiteral("Triangles"), QStringLiteral("Submeshes"),
              QStringLiteral("Bones"), QStringLiteral("Skinned"),
              QStringLiteral("Animations"), QStringLiteral("Clip"),
              QStringLiteral("Cloth"), QStringLiteral("Focus"),
              QStringLiteral("Variant")}) {
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

    // ── MATERIALS ──────────────────────────────────────────────────────────
    // One top-level row per equipped piece (its resolved Material name), with
    // the four bound channels as children. Models shows a single material
    // because a model has exactly one; an outfit has one PER PIECE.
    m_matBox = new QWidget(host);
    {
        auto* mv = new QVBoxLayout(m_matBox);
        mv->setContentsMargins(0, 2, 0, 2);
        mv->setSpacing(2);
        m_matList = new QTreeWidget(m_matBox);
        m_matList->setColumnCount(3);
        m_matList->setHeaderLabels({QStringLiteral("PIECE / CHANNEL"),
                                    QStringLiteral("TEXTURE"),
                                    QStringLiteral("SIZE")});
        m_matList->setAlternatingRowColors(true);
        m_matList->setUniformRowHeights(true);
        m_matList->header()->setStretchLastSection(false);
        m_matList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_matList->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_matList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        // Selection drives the TEXTURE PREVIEW, and WHICH row you pick decides
        // the mode: the piece row is a material → show the PBR channels; a
        // channel row is one texture → show that image split into R/G/B/A.
        connect(m_matList, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                    if (!cur) return;
                    QTreeWidgetItem* top = cur->parent() ? cur->parent() : cur;
                    const int part = top->data(0, Qt::UserRole).toInt();
                    if (part < 0 || part >= kParts || !m_slots[part].part.mesh) return;
                    const bool wasFocus = (m_focusPart == part);
                    setFocusPart(part);
                    if (!cur->parent()) {                 // the material itself
                        if (wasFocus) fillTexturePreview(m_slots[part].part.textures);
                        return;
                    }
                    const int ch = cur->data(0, Qt::UserRole + 1).toInt();
                    const MeshTextures& t = m_slots[part].part.textures;
                    const QImage* src = ch == 0   ? &t.diffuse
                                      : ch == 1   ? &t.normal
                                      : ch == 2   ? &t.mix
                                                  : &t.emissive;
                    if (src->isNull()) return;            // nothing bound
                    showTextureChannels(*src, QStringLiteral("%1 · %2")
                                                  .arg(m_slots[part].name,
                                                       cur->text(0)));
                });
        mv->addWidget(m_matList);
    }

    // ── TEXTURE PREVIEW ────────────────────────────────────────────────────
    // Shared body: PBR channels when a piece/material is selected, and the raw
    // RGBA split when a texture row is. Per-tile copy/save stays here because
    // the menu wording is the tab's business.
    m_texPrevBox = m_texPrev.build(host, this);
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

    // ── PARTS (per-submesh visibility) ─────────────────────────────────────
    m_partsBox = new QWidget(host);
    {
        auto* pbox = new QVBoxLayout(m_partsBox);
        pbox->setContentsMargins(0, 2, 0, 2);
        auto* phdr = new QHBoxLayout();
        phdr->addStretch(1);
        auto* pAll  = new QPushButton(QStringLiteral("All"), m_partsBox);
        auto* pNone = new QPushButton(QStringLiteral("None"), m_partsBox);
        auto* pInv  = new QPushButton(QStringLiteral("Invert"), m_partsBox);
        for (QPushButton* b : {pAll, pNone, pInv}) {
            b->setFixedWidth(56);
            b->setStyleSheet(QLatin1String(kPushBtnQss));
            phdr->addWidget(b);
        }
        connect(pAll,  &QPushButton::clicked, this, [this] { setAllParts(1); });
        connect(pNone, &QPushButton::clicked, this, [this] { setAllParts(0); });
        connect(pInv,  &QPushButton::clicked, this, [this] { setAllParts(2); });
        pbox->addLayout(phdr);
        m_partsList = new QTreeWidget(m_partsBox);
        m_partsList->setColumnCount(2);
        m_partsList->setHeaderLabels({QStringLiteral("PIECE / PART"),
                                      QStringLiteral("TRIS")});
        m_partsList->setUniformRowHeights(true);
        m_partsList->setAlternatingRowColors(true);
        m_partsList->header()->setStretchLastSection(false);
        m_partsList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_partsList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_partsList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_partsList, &QTreeWidget::itemChanged, this,
                [this](QTreeWidgetItem* it, int) {
                    if (m_partsSyncing || !it) return;
                    const int part = it->data(0, Qt::UserRole).toInt();
                    const int sub  = it->data(0, Qt::UserRole + 1).toInt();
                    if (part < 0 || part >= kParts) return;
                    const bool on  = it->checkState(0) == Qt::Checked;
                    if (sub < 0) {                       // whole piece
                        if (on) m_partHidden.remove(part);
                        else    m_partHidden.insert(part);
                    } else {
                        if (on) {
                            m_subHidden[part].remove(sub);
                            // Showing a submesh of a hidden piece must bring the
                            // piece back, or the tick would have no visible effect.
                            m_partHidden.remove(part);
                        } else {
                            m_subHidden[part].insert(sub);
                        }
                    }
                    applyPartsMask();
                    syncVisibilityChecks();
                });
        connect(m_partsList, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                    if (!cur) return;
                    setFocusPart(cur->data(0, Qt::UserRole).toInt());
                });
        connect(m_partsList, &QTreeWidget::customContextMenuRequested, this,
                [this](const QPoint& p) {
                    QTreeWidgetItem* it = m_partsList->itemAt(p);
                    if (!it) return;
                    const int part = it->data(0, Qt::UserRole).toInt();
                    const int sub  = it->data(0, Qt::UserRole + 1).toInt();
                    const bool vis = it->checkState(0) == Qt::Checked;
                    QMenu menu(this);
                    menu.addAction(QStringLiteral("Solo this piece"), this,
                                   [this, part] {
                        m_partHidden.clear();
                        for (int s = 0; s < kParts; ++s)
                            if (s != part && m_slots[s].part.mesh)
                                m_partHidden.insert(s);
                        m_subHidden.remove(part);
                        applyPartsMask();
                        syncVisibilityChecks();
                    });
                    menu.addAction(vis ? QStringLiteral("Hide this")
                                       : QStringLiteral("Show this"),
                                   this, [this, part, sub, vis] {
                        if (sub < 0) {
                            if (vis) m_partHidden.insert(part);
                            else     m_partHidden.remove(part);
                        } else {
                            if (vis) m_subHidden[part].insert(sub);
                            else     m_subHidden[part].remove(sub);
                        }
                        applyPartsMask();
                        syncVisibilityChecks();
                    });
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("Show everything"), this,
                                   [this] { setAllParts(1); });
                    menu.addAction(QStringLiteral("Invert visibility"), this,
                                   [this] { setAllParts(2); });
                    menu.exec(m_partsList->viewport()->mapToGlobal(p));
                });
        pbox->addWidget(m_partsList);
    }

    // ── EQUIPPED ───────────────────────────────────────────────────────────
    // The roster of every LIVE part — the slot combos, the base-body pieces
    // and (the point of this panel) the set-attachment pool that applySet()
    // fills invisibly. Before this it existed only as a comma-joined string.
    m_equipBox = new QWidget(host);
    {
        auto* ev = new QVBoxLayout(m_equipBox);
        ev->setContentsMargins(0, 2, 0, 2);
        m_equipList = new QTreeWidget(m_equipBox);
        m_equipList->setColumnCount(3);
        m_equipList->setHeaderLabels({QStringLiteral("SLOT"),
                                      QStringLiteral("PIECE"),
                                      QStringLiteral("TRIS")});
        m_equipList->setRootIsDecorated(false);
        m_equipList->setUniformRowHeights(true);
        m_equipList->setAlternatingRowColors(true);
        m_equipList->header()->setStretchLastSection(false);
        m_equipList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_equipList->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_equipList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_equipList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_equipList, &QTreeWidget::itemChanged, this,
                [this](QTreeWidgetItem* it, int) {
                    if (m_equipSyncing || !it) return;
                    const int part = it->data(0, Qt::UserRole).toInt();
                    if (part < 0 || part >= kParts) return;
                    if (it->checkState(0) == Qt::Checked) m_partHidden.remove(part);
                    else                                  m_partHidden.insert(part);
                    applyPartsMask();
                    syncVisibilityChecks();
                });
        connect(m_equipList, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                    if (cur) setFocusPart(cur->data(0, Qt::UserRole).toInt());
                });
        connect(m_equipList, &QTreeWidget::customContextMenuRequested, this,
                [this](const QPoint& p) {
                    QTreeWidgetItem* it = m_equipList->itemAt(p);
                    if (!it) return;
                    const int part = it->data(0, Qt::UserRole).toInt();
                    if (part < 0 || part >= kParts || !m_slots[part].part.mesh) return;
                    QMenu menu(this);
                    menu.addAction(QStringLiteral("Frame this piece"), this,
                                   [this, part] {
                        if (m_view) m_view->framePart((size_t)part);
                    });
                    menu.addAction(QStringLiteral("Solo this piece"), this,
                                   [this, part] {
                        m_partHidden.clear();
                        for (int s = 0; s < kParts; ++s)
                            if (s != part && m_slots[s].part.mesh)
                                m_partHidden.insert(s);
                        applyPartsMask();
                        syncVisibilityChecks();
                    });
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("Export this piece…"), this,
                                   [this, part] { exportSlotPiece(part); });
                    menu.addAction(QStringLiteral("Copy piece name"), this,
                                   [this, part] {
                        QGuiApplication::clipboard()->setText(m_slots[part].name);
                    });
                    // Whole-outfit exports are reachable from here too, so
                    // "right-click to export" holds at every level of this tab:
                    // one piece, the outfit, or every set of the class.
                    menu.addSeparator();
                    QAction* eo = menu.addAction(
                        QStringLiteral("Export outfit .glb…"), this,
                        [this] { if (m_exportOutfit) m_exportOutfit->click(); });
                    eo->setEnabled(m_exportOutfit && m_exportOutfit->isEnabled());
                    QAction* es = menu.addAction(
                        QStringLiteral("Export all sets…"), this,
                        [this] { if (m_exportSets) m_exportSets->click(); });
                    es->setEnabled(m_exportSets && m_exportSets->isEnabled());
                    // Viewport capture, same wording and same code path as the
                    // Models tab's list menu (never a toolbar button — D4 rule).
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("Save preview image…"), this,
                                   [this] { saveImageNow(); });
                    menu.addAction(QStringLiteral("Turntable GIF…"), this,
                                   [this] { exportGifTurntable(); });
                    if (canExportAnimGif())
                        menu.addAction(QStringLiteral("Animation loop GIF…"), this,
                                       [this] { exportGifAnim(); });
                    menu.exec(m_equipList->viewport()->mapToGlobal(p));
                });
        ev->addWidget(m_equipList);
    }

    // ── ANIMATIONS ─────────────────────────────────────────────────────────
    m_animList = new QListWidget(host);
    m_animList->setAlternatingRowColors(true);
    m_animList->setToolTip(QStringLiteral(
        "Select a clip to play it — arrow keys and click-drag autoplay as you move"));
    // currentItemChanged (not itemClicked) so ARROW KEYS and CLICK-DRAG through
    // the list autoplay each clip as the selection lands on it, matching D4.
    connect(m_animList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* cur, QListWidgetItem*) {
                if (!cur || !m_animBox || m_animListSyncing) return;
                const int idx = m_animBox->findText(cur->text());
                if (idx >= 0 && idx != m_animBox->currentIndex())
                    m_animBox->setCurrentIndex(idx);   // triggers playback
            });
}

// The piece whose material + textures the MATERIALS / TEXTURE PREVIEW panels
// describe. Everything else in the outfit stays visible; this is a read focus,
// not a visibility change.
void WardrobeTab::setFocusPart(int part)
{
    if (part < 0 || part >= kParts || !m_slots[part].part.mesh) return;
    if (m_focusPart == part) return;
    m_focusPart = part;
    fillTexturePreview(m_slots[part].part.textures);
    fillInfoPanel();
}

void WardrobeTab::fillInfoPanel()
{
    auto set = [this](const QString& k, const QString& v) {
        if (QLabel* l = m_infoVals.value(k))
            l->setText(v.isEmpty() ? QStringLiteral("—") : v);
    };
    if (m_infoVals.isEmpty()) return;
    size_t verts = 0, tris = 0, subs = 0, pieces = 0, bones = 0;
    bool skinned = false;
    for (int s = 0; s < kParts; ++s) {
        const std::shared_ptr<di::MeshData>& m = m_slots[s].part.mesh;
        if (!m) continue;
        ++pieces;
        verts += m->vertexCount();
        tris  += m->indices.size() / 3;
        subs  += m->submeshes.size();
        skinned = skinned || m->skinned;
        if (m_slots[s].skel) bones = std::max(bones, m_slots[s].skel->bones.size());
    }
    const int setIdx = m_setBox ? m_setBox->currentIndex() : 0;
    set(QStringLiteral("Character"),
        m_classBox ? m_classBox->currentText() : QString());
    set(QStringLiteral("Set"),
        setIdx > 0 && setIdx - 1 < m_setKeys.size()
            ? setLabel(m_setKeys[setIdx - 1]) : QStringLiteral("(custom)"));
    set(QStringLiteral("Pieces"), QString::number(pieces));
    set(QStringLiteral("Vertices"), QLocale().toString((qulonglong)verts));
    set(QStringLiteral("Triangles"), QLocale().toString((qulonglong)tris));
    set(QStringLiteral("Submeshes"), QString::number(subs));
    set(QStringLiteral("Bones"), bones ? QString::number(bones) : QString());
    set(QStringLiteral("Skinned"), skinned ? QStringLiteral("yes")
                                           : QStringLiteral("no"));
    set(QStringLiteral("Animations"),
        m_animIds ? QString::number(m_animIds->size()) : QString());
    set(QStringLiteral("Clip"),
        m_clip && m_animBox && m_animBox->currentIndex() > 0
            ? QStringLiteral("%1 · %2 ms").arg(m_animBox->currentText())
                  .arg(m_clip->durationMs)
            : QStringLiteral("(bind pose)"));
    set(QStringLiteral("Cloth"), clothOn() ? QStringLiteral("on")
                                           : QStringLiteral("off"));
    set(QStringLiteral("Focus"),
        m_focusPart >= 0 && m_focusPart < kParts && m_slots[m_focusPart].part.mesh
            ? QStringLiteral("%1 — %2").arg(partLabel(m_focusPart),
                                            m_slots[m_focusPart].name)
            : QString());
    QString variant;
    if (m_focusPart >= 0 && m_focusPart < kParts) {
        const SlotState& S = m_slots[m_focusPart];
        if (!S.varNames.isEmpty())
            variant = QStringLiteral("%1  (%2 of %3)")
                          .arg(S.varNames.value(S.varIdx))
                          .arg(S.varIdx + 1).arg(S.varNames.size());
    }
    set(QStringLiteral("Variant"), variant);
}

// One row per equipped piece: its resolved Material name (from the variant
// table, which holds real Material repo indices) and the four bound channels.
void WardrobeTab::fillMaterialsPanel()
{
    if (!m_matList) return;
    m_matList->clear();
    const di::Repository* repo =
        (m_idx && m_idx->store) ? m_idx->store->repo() : nullptr;
    for (int s = 0; s < kParts; ++s) {
        const SlotState& S = m_slots[s];
        if (!S.part.mesh) continue;
        QString matName;
        if (repo && S.varIdx >= 0 && S.varIdx < S.varRepoIdx.size()) {
            const int mi = S.varRepoIdx[S.varIdx];
            if (mi >= 0 && (size_t)mi < repo->entries.size())
                matName = QString::fromStdString(repo->entries[(size_t)mi].name);
        }
        auto* top = new QTreeWidgetItem(m_matList);
        top->setText(0, QStringLiteral("%1 — %2").arg(partLabel(s), S.name));
        top->setText(1, matName.isEmpty() ? QStringLiteral("—") : matName);
        top->setData(0, Qt::UserRole, s);
        QFont f = top->font(0);
        f.setBold(true);
        top->setFont(0, f);
        const struct { const char* label; const QImage* img; } chans[4] = {
            {"Base Color",       &S.part.textures.diffuse},
            {"Normal",           &S.part.textures.normal},
            {"Rough/Metal/AO",   &S.part.textures.mix},
            {"Emissive",         &S.part.textures.emissive}};
        int chIdx = 0;
        for (const auto& c : chans) {
            auto* it = new QTreeWidgetItem(top);
            it->setText(0, QLatin1String(c.label));
            it->setData(0, Qt::UserRole, s);
            // Which source texture this row stands for, for the RGBA split.
            it->setData(0, Qt::UserRole + 1, chIdx++);
            if (c.img->isNull()) {
                it->setText(1, QStringLiteral("—"));
            } else {
                it->setText(1, QStringLiteral("bound"));
                it->setText(2, QStringLiteral("%1×%2")
                                   .arg(c.img->width()).arg(c.img->height()));
            }
        }
        top->setExpanded(s == m_focusPart);
    }
}

void WardrobeTab::fillTexturePreview(const MeshTextures& tex)
{
    const QString who =
        (m_focusPart >= 0 && m_focusPart < kParts) ? m_slots[m_focusPart].name : QString();
    m_texPrev.showMaterial(tex, who);
    refreshTexPanelTitle();
}

void WardrobeTab::showTextureChannels(const QImage& img, const QString& name)
{
    if (img.isNull()) return;
    ensurePanelVisible(QStringLiteral("TEXTURE PREVIEW"));
    m_texPrev.showTexture(img, name);
    refreshTexPanelTitle();
}

// Keep the panel header honest about what is on screen.
void WardrobeTab::refreshTexPanelTitle()
{
    const int page = m_panelIndex.value(QStringLiteral("TEXTURE PREVIEW"), -1);
    if (page < 0 || page >= (int)m_rsections.size()) return;
    if (QLabel* l = m_rsections[(size_t)page]->label) l->setText(m_texPrev.title());
}


void WardrobeTab::buildPartsList()
{
    if (!m_partsList) return;
    m_partsSyncing = true;
    m_partsList->clear();
    int shown = 0, total = 0;
    for (int s = 0; s < kParts; ++s) {
        const std::shared_ptr<di::MeshData>& m = m_slots[s].part.mesh;
        if (!m) continue;
        const bool pHid = m_partHidden.contains(s);
        auto* top = new QTreeWidgetItem(m_partsList);
        top->setText(0, QStringLiteral("%1 — %2").arg(partLabel(s), m_slots[s].name));
        top->setText(1, QString::number(m->indices.size() / 3));
        top->setFlags(top->flags() | Qt::ItemIsUserCheckable);
        top->setCheckState(0, pHid ? Qt::Unchecked : Qt::Checked);
        top->setData(0, Qt::UserRole, s);
        top->setData(0, Qt::UserRole + 1, -1);   // -1 = the whole piece
        const QSet<int>& hid = m_subHidden[s];
        for (size_t k = 0; k < m->submeshes.size(); ++k) {
            const bool vis = !pHid && !hid.contains((int)k);
            ++total;
            if (vis) ++shown;
            auto* it = new QTreeWidgetItem(top);
            it->setText(0, QStringLiteral("part %1").arg(k));
            it->setText(1, QString::number(m->submeshes[k].indexCount / 3));
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            // A submesh reads as hidden when its piece is hidden, even if the
            // submesh itself was never individually switched off.
            it->setCheckState(0, vis ? Qt::Checked : Qt::Unchecked);
            it->setData(0, Qt::UserRole, s);
            it->setData(0, Qt::UserRole + 1, (int)k);
        }
        if (m->submeshes.empty()) { ++total; if (!pHid) ++shown; }
        top->setExpanded(s == m_focusPart);
    }
    m_partsSyncing = false;
    if (const int page = m_panelIndex.value(QStringLiteral("PARTS"), -1); page >= 0)
        if (page < (int)m_rsections.size() && m_rsections[(size_t)page]->label)
            m_rsections[(size_t)page]->label->setText(
                total ? QStringLiteral("PARTS · %1 of %2 shown").arg(shown).arg(total)
                      : QStringLiteral("PARTS"));
}

void WardrobeTab::buildEquipList()
{
    if (!m_equipList) return;
    m_equipSyncing = true;
    m_equipList->clear();
    int live = 0;
    for (int s = 0; s < kParts; ++s) {
        const std::shared_ptr<di::MeshData>& m = m_slots[s].part.mesh;
        if (!m) continue;
        ++live;
        auto* it = new QTreeWidgetItem(m_equipList);
        it->setText(0, partLabel(s));
        it->setText(1, m_slots[s].name);
        it->setText(2, QString::number(m->indices.size() / 3));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(0, m_partHidden.contains(s) ? Qt::Unchecked : Qt::Checked);
        it->setData(0, Qt::UserRole, s);
        it->setToolTip(1, m_slots[s].name);
        // The auto-equipped pool reads differently from a slot you chose.
        if (s >= kSlots + kBodySlots)
            it->setForeground(0, QColor(0xa8, 0x9a, 0x78));
    }
    m_equipSyncing = false;
    if (const int page = m_panelIndex.value(QStringLiteral("EQUIPPED"), -1); page >= 0)
        if (page < (int)m_rsections.size() && m_rsections[(size_t)page]->label)
            m_rsections[(size_t)page]->label->setText(
                QStringLiteral("EQUIPPED · %1").arg(live));
}

// Merge the whole-part and per-submesh hide sets into one mask per part. An
// all-zero mask is the viewport's "draw nothing" signal, which is what makes a
// piece with no submesh table hideable at all.
void WardrobeTab::applyPartsMask()
{
    if (!m_view) return;
    for (int s = 0; s < kParts; ++s) {
        const std::shared_ptr<di::MeshData>& m = m_slots[s].part.mesh;
        if (!m) continue;
        const bool pHid = m_partHidden.contains(s);
        const QSet<int>& hid = m_subHidden[s];
        if (!pHid && hid.isEmpty()) {
            m_view->setPartSubmeshMask((size_t)s, {});   // empty = all visible
            continue;
        }
        const size_t n = m->submeshes.empty() ? 1 : m->submeshes.size();
        std::vector<uint8_t> mask(n, 1);
        for (size_t k = 0; k < n; ++k)
            if (pHid || hid.contains((int)k)) mask[k] = 0;
        m_view->setPartSubmeshMask((size_t)s, std::move(mask));
    }
    int shown = 0, total = 0;
    for (int s = 0; s < kParts; ++s) {
        if (!m_slots[s].part.mesh) continue;
        ++total;
        if (!m_partHidden.contains(s)) ++shown;
    }
    emit statusText(QStringLiteral("%1 of %2 pieces shown").arg(shown).arg(total));
}

// Re-read the hide sets into the rows that already exist. Cheaper than a
// rebuild and, unlike one, legal from inside an itemChanged emission.
void WardrobeTab::syncVisibilityChecks()
{
    int shown = 0, total = 0;
    if (m_partsList) {
        m_partsSyncing = true;
        for (int i = 0; i < m_partsList->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = m_partsList->topLevelItem(i);
            const int part = top->data(0, Qt::UserRole).toInt();
            if (part < 0 || part >= kParts) continue;
            const bool pHid = m_partHidden.contains(part);
            top->setCheckState(0, pHid ? Qt::Unchecked : Qt::Checked);
            const QSet<int> hid = m_subHidden.value(part);
            for (int c = 0; c < top->childCount(); ++c) {
                QTreeWidgetItem* it = top->child(c);
                const int sub = it->data(0, Qt::UserRole + 1).toInt();
                const bool vis = !pHid && !hid.contains(sub);
                it->setCheckState(0, vis ? Qt::Checked : Qt::Unchecked);
                ++total;
                if (vis) ++shown;
            }
            if (top->childCount() == 0) { ++total; if (!pHid) ++shown; }
        }
        m_partsSyncing = false;
    }
    if (m_equipList) {
        m_equipSyncing = true;
        for (int i = 0; i < m_equipList->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = m_equipList->topLevelItem(i);
            const int part = it->data(0, Qt::UserRole).toInt();
            if (part < 0 || part >= kParts) continue;
            it->setCheckState(0, m_partHidden.contains(part) ? Qt::Unchecked
                                                             : Qt::Checked);
        }
        m_equipSyncing = false;
    }
    if (const int page = m_panelIndex.value(QStringLiteral("PARTS"), -1); page >= 0)
        if (page < (int)m_rsections.size() && m_rsections[(size_t)page]->label)
            m_rsections[(size_t)page]->label->setText(
                total ? QStringLiteral("PARTS · %1 of %2 shown").arg(shown).arg(total)
                      : QStringLiteral("PARTS"));
}

void WardrobeTab::clearPartVisibility(int part)
{
    m_partHidden.remove(part);
    m_subHidden.remove(part);
}

void WardrobeTab::setAllParts(int mode)
{
    for (int s = 0; s < kParts; ++s) {
        if (!m_slots[s].part.mesh) continue;
        const bool hidden = m_partHidden.contains(s);
        const bool want = mode == 1 ? true : (mode == 0 ? false : hidden);
        if (want) m_partHidden.remove(s);
        else      m_partHidden.insert(s);
        m_subHidden.remove(s);   // a blanket call clears per-submesh overrides
    }
    applyPartsMask();
    syncVisibilityChecks();
}

// ── Animation transport (Models-tab parity) ────────────────────────────────

void WardrobeTab::seekFrames(int delta)
{
    if (!m_clip) return;
    if (m_animTimer && m_animTimer->isActive()) {
        m_animTimer->stop();                      // stepping implies pause
        if (m_playBtn) m_playBtn->setText(QStringLiteral("Play"));
    }
    const float dur = float(m_clip->durationMs);
    m_animT = qBound(0.0f, m_animT + float(delta) * kFrameMs, dur);
    animTick(m_animT);
}

void WardrobeTab::updateFrameUi()
{
    if (!m_frameSpin || !m_frameMax || !m_timeLabel) return;
    const float dur = m_clip ? float(m_clip->durationMs) : 0.0f;
    const int frames = dur > 0.0f ? int(dur / kFrameMs) : 0;
    const int cur = dur > 0.0f ? int(m_animT / kFrameMs) : 0;
    m_frameSpin->blockSignals(true);
    m_frameSpin->setRange(0, qMax(0, frames));
    m_frameSpin->setValue(qBound(0, cur, qMax(0, frames)));
    m_frameSpin->blockSignals(false);
    m_frameMax->setText(QStringLiteral("/ %1").arg(frames));
    m_timeLabel->setText(QStringLiteral("%1 / %2 s")
                             .arg(m_animT / 1000.0f, 0, 'f', 2)
                             .arg(dur / 1000.0f, 0, 'f', 2));
}

void WardrobeTab::applySpeed(int comboIdx)
{
    if (!m_speedBox) return;
    bool ok = false;
    const float v = m_speedBox->itemData(comboIdx).toFloat(&ok);
    m_speed = (ok && v > 0.0f) ? v : 1.0f;
    emit statusText(QStringLiteral("Speed: %1").arg(m_speedBox->itemText(comboIdx)));
}

// Keep the ANIMATIONS panel selection in step with the combo, without letting
// the list's own currentItemChanged bounce back and re-trigger playback.
void WardrobeTab::syncAnimList()
{
    if (!m_animList || !m_animBox) return;
    m_animListSyncing = true;
    if (m_animList->count() != m_animBox->count() - 1) {
        m_animList->clear();
        for (int i = 1; i < m_animBox->count(); ++i)
            m_animList->addItem(m_animBox->itemText(i));
    }
    const int want = m_animBox->currentIndex() - 1;
    if (want >= 0 && want < m_animList->count())
        m_animList->setCurrentRow(want);
    else
        m_animList->setCurrentRow(-1);
    m_animListSyncing = false;
}

// ── ExportHooks: Ctrl+E / the Export menu drive the outfit export ──────────
// The wardrobe's export already owns its dialog + naming (stable class/set
// identifiers), so the hook simply presses the same button a mouse would —
// one code path, no drift. toLastDir has no meaning here (the outfit dialog
// always confirms a filename) and is deliberately ignored.

bool WardrobeTab::canExport() const
{
    if (!m_exportOutfit || !m_exportOutfit->isEnabled()) return false;
    // Enabled-on-class-scan is not enough: with nothing equipped the export
    // writes no file and only prints a note, which is exactly the "action shown
    // that cannot be performed" ExportHooks forbids.
    for (int s = 0; s < kParts; ++s)
        if (m_slots[s].part.mesh) return true;
    return false;
}

QString WardrobeTab::exportWhat() const
{
    return QStringLiteral("outfit");
}

void WardrobeTab::exportNow(bool toLastDir)
{
    (void)toLastDir;
    if (canExport()) m_exportOutfit->click();
}

// ── Viewport capture ───────────────────────────────────────────────────────
// Deliberately identical in shape to ModelsTab's: same ExportCapture calls,
// same settings keys, same suspend/restore of the spin + playback timers. The
// only per-tab parts are the filename seed and how a frame index is posed.

QString WardrobeTab::captureBaseName() const
{
    const QString base = outfitFileName();
    return base.isEmpty() ? QStringLiteral("outfit") : base;
}

ExportCapture::AnimSource WardrobeTab::captureAnim()
{
    ExportCapture::AnimSource src;
    if (!m_clip) return src;
    const float dur = (float)m_clip->durationMs;
    const int n = std::max(1, (int)(dur / kFrameMs));
    src.frameCount = n;
    src.fps        = 1000.0f / kFrameMs;
    src.savedFrame = qBound(0, (int)(m_animT / kFrameMs), n - 1);
    src.seek = [this, dur](int f) {
        animPose(qBound(0.0f, (float)f * kFrameMs, dur));
    };
    return src;
}

bool WardrobeTab::canSaveImage() const
{
    for (int s = 0; s < kParts; ++s)
        if (m_slots[s].part.mesh) return true;
    return false;
}

bool WardrobeTab::canExportGif() const { return canSaveImage(); }

bool WardrobeTab::canExportAnimGif() const
{
    return m_clip && canSaveImage();
}

void WardrobeTab::saveImageNow()
{
    if (!canSaveImage()) { emit statusText(QStringLiteral("equip something first")); return; }
    QString msg;
    ExportCapture::runSaveImage(this, m_view, captureBaseName(), &msg);
    if (!msg.isEmpty()) emit statusText(msg);
}

void WardrobeTab::exportGifTurntable()
{
    if (!canExportGif()) { emit statusText(QStringLiteral("equip something first")); return; }
    const bool wasSpinning = m_turntable && m_turntable->isChecked();
    if (wasSpinning) m_turntable->setChecked(false);
    const bool wasPlaying = m_animTimer && m_animTimer->isActive();
    if (wasPlaying) m_animTimer->stop();

    QString msg;
    ExportCapture::runTurntableGif(this, m_view, captureBaseName(), captureAnim(),
                                  &msg);

    if (m_clip) animTick(m_animT);
    if (wasPlaying) m_animTimer->start();
    if (wasSpinning && m_turntable) m_turntable->setChecked(true);
    if (!msg.isEmpty()) emit statusText(msg);
}

void WardrobeTab::exportGifAnim()
{
    if (!canExportAnimGif()) {
        emit statusText(QStringLiteral("load an animation first"));
        return;
    }
    const bool wasSpinning = m_turntable && m_turntable->isChecked();
    if (wasSpinning) m_turntable->setChecked(false);
    const bool wasPlaying = m_animTimer && m_animTimer->isActive();
    if (wasPlaying) m_animTimer->stop();

    QString msg;
    ExportCapture::runAnimLoopGif(this, m_view, captureBaseName(), captureAnim(),
                                 &msg);

    animTick(m_animT);
    if (wasPlaying) m_animTimer->start();
    if (wasSpinning && m_turntable) m_turntable->setChecked(true);
    if (!msg.isEmpty()) emit statusText(msg);
}
