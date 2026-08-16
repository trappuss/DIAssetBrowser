#include "util/FilterBar.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>

namespace {

// Top-N facet values by count for a group list (values beyond stay reachable
// through the search box; log-free cap keeps the popup usable).
constexpr int kMaxFacetRows = 40;

struct FacetRow {
    QString value;
    int     count = 0;
};

QVector<FacetRow> topFacets(const QHash<QString, int>& counts, int maxRows)
{
    QVector<FacetRow> rows;
    rows.reserve(counts.size());
    for (auto it = counts.begin(); it != counts.end(); ++it)
        rows.push_back({it.key(), it.value()});
    std::sort(rows.begin(), rows.end(), [](const FacetRow& a, const FacetRow& b) {
        return a.count != b.count ? a.count > b.count : a.value < b.value;
    });
    if (rows.size() > maxRows) rows.resize(maxRows);
    return rows;
}

} // namespace

FilterBar::FilterBar(QWidget* parent, const QStringList& restrictTypes,
                     const QString& pinnedType, bool cosmeticFacets)
    : QWidget(parent), m_restrictTypes(restrictTypes), m_pinnedType(pinnedType),
      m_cosmeticFacets(cosmeticFacets)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);

    auto* top = new QHBoxLayout();
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(
        QStringLiteral("Search…  (space = AND, -term = exclude)"));
    m_search->setClearButtonEnabled(true);
    top->addWidget(m_search, 1);
    m_funnel = new QToolButton(this);
    m_funnel->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_funnel->setText(QStringLiteral("Filters ▾"));
    m_funnel->setToolTip(QStringLiteral("Facet filters (type / category)"));
    {   // funnel glyph (D4 gives the filter toggle an icon, not just text)
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(206, 198, 168), 1.4, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        QPolygonF f;
        f << QPointF(2, 3) << QPointF(14, 3) << QPointF(9.5, 8)
          << QPointF(9.5, 13) << QPointF(6.5, 11) << QPointF(6.5, 8);
        p.drawPolygon(f);
        m_funnel->setIcon(QIcon(pm));
    }
    top->addWidget(m_funnel);
    lay->addLayout(top);

    m_chipRow = new QWidget(this);
    m_chipLay = new QHBoxLayout(m_chipRow);
    m_chipLay->setContentsMargins(0, 0, 0, 0);
    m_chipLay->setSpacing(4);
    m_chipRow->hide();
    lay->addWidget(m_chipRow);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, [this] { persist(); emit changed(); });
    connect(m_search, &QLineEdit::textChanged, m_debounce, qOverload<>(&QTimer::start));

    connect(m_funnel, &QToolButton::clicked, this, [this] {
        // Toggle: a second click on the funnel closes an open popup.
        if (m_popup && m_popup->isVisible()) { m_popup->hide(); return; }
        buildPopup();
        refreshPopupLists();
        // Force the nested layout to settle so sizeHint() is real on the first
        // open (a never-shown popup can otherwise report a stale/zero size and
        // land off-screen — which reads as "the filter won't open").
        m_popup->ensurePolished();
        m_popup->adjustSize();
        QSize hint = m_popup->sizeHint().expandedTo(m_popup->size());
        QPoint at = m_funnel->mapToGlobal(
            QPoint(m_funnel->width() - hint.width(), m_funnel->height()));
        if (QScreen* scr = m_funnel->screen()) {
            const QRect avail = scr->availableGeometry();
            // Cap to the screen so the popup can never exceed it — the inner
            // scroll area then scrolls the overflow instead of spilling off.
            hint.setWidth(qMin(hint.width(), avail.width() - 24));
            hint.setHeight(qMin(hint.height(), avail.height() - 48));
            m_popup->resize(hint);
            at = m_funnel->mapToGlobal(
                QPoint(m_funnel->width() - hint.width(), m_funnel->height()));
            at.setX(qBound(avail.left(), at.x(), avail.right() - hint.width()));
            at.setY(qBound(avail.top(), at.y(), avail.bottom() - hint.height()));
        }
        m_popup->move(at);
        qInfo("FilterBar: funnel clicked -> popup at (%d,%d) size (%d,%d)",
              at.x(), at.y(), hint.width(), hint.height());
        // Defer the show one tick: opening a Qt::Popup synchronously inside the
        // button's own click can let the residual release event dismiss it again
        // on some setups. A zero-delay singleShot shows it after the click settles.
        QWidget* p = m_popup;
        QTimer::singleShot(0, p, [p] {
            p->show();
            p->raise();
            p->activateWindow();
        });
    });
}

QListWidget* FilterBar::makeGroupList(const QString& title, QWidget* parent)
{
    auto* box  = new QWidget(parent);
    auto* v    = new QVBoxLayout(box);
    v->setContentsMargins(4, 4, 4, 4);
    auto* head = new QLabel(QStringLiteral("<b>%1</b>").arg(title), box);
    v->addWidget(head);
    auto* list = new QListWidget(box);
    // Compact: the popup lives in a scroll area now, so keep each column small
    // enough that several fit on a modest screen (overflow scrolls, not clips).
    list->setMinimumWidth(140);
    list->setMinimumHeight(150);
    list->setMaximumWidth(220);
    v->addWidget(list, 1);
    parent->layout()->addWidget(box);

    connect(list, &QListWidget::itemChanged, this, [this, list](QListWidgetItem* item) {
        if (m_updating) return;
        const QString value = item->data(Qt::UserRole).toString();
        QSet<QString>* target = list == m_typeList ? &m_checkedTypes
                              : list == m_catList  ? &m_checkedCats
                              : list == m_subList  ? &m_checkedSubs
                              : list == m_clsList  ? &m_checkedCls
                                                   : &m_checkedSlots;
        if (item->checkState() == Qt::Checked) target->insert(value);
        else                                   target->remove(value);
        refreshChips();
        persist();
        emit changed();
    });
    return list;
}

void FilterBar::buildPopup()
{
    if (m_popup) return;
    m_popup = new QWidget(this, Qt::Popup);
    auto* v = new QVBoxLayout(m_popup);
    m_popupLay = v;
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    // The facet lists live in a scroll area so the popup never grows past the
    // screen — on a small display the columns scroll instead of the popup
    // spilling off the edges (which read as "the filter is broken").
    auto* scroll = new QScrollArea(m_popup);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* row = new QWidget(scroll);
    auto* rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(0, 0, 0, 0);
    rowLay->setSpacing(4);
    scroll->setWidget(row);
    v->addWidget(scroll, 1);

    if (m_pinnedType.isEmpty())
        m_typeList = makeGroupList(QStringLiteral("Type"), row);
    m_catList = makeGroupList(QStringLiteral("Category"), row);
    m_subList = makeGroupList(QStringLiteral("Subcategory"), row);
    if (m_cosmeticFacets) {
        m_clsList  = makeGroupList(QStringLiteral("Class"), row);
        m_slotList = makeGroupList(QStringLiteral("Slot"), row);
    }

    auto* clear = new QPushButton(QStringLiteral("Clear all filters"), m_popup);
    m_popupClear = clear;
    v->addWidget(clear);   // pinned below the scroll area, always reachable
    connect(clear, &QPushButton::clicked, this, [this] {
        m_checkedTypes.clear();
        m_checkedCats.clear();
        m_checkedSubs.clear();
        m_checkedCls.clear();
        m_checkedSlots.clear();
        refreshPopupLists();
        refreshChips();
        persist();
        emit changed();
    });
}

void FilterBar::addPopupSection(QWidget* content)
{
    if (!content) return;
    buildPopup();                       // ensure the popup + its vbox exist
    content->setParent(m_popup);
    const int at = m_popupLay->indexOf(m_popupClear);
    m_popupLay->insertWidget(at < 0 ? m_popupLay->count() : at, content);
}

void FilterBar::refreshPopupLists()
{
    if (!m_popup || !m_idx) return;
    m_updating = true;
    struct Group {
        QListWidget* list;
        QVector<FacetRow> rows;
        const QSet<QString>* checked;
    };
    QVector<Group> groups;
    if (m_typeList) {
        QVector<FacetRow> t;
        if (!m_restrictTypes.isEmpty()) {
            for (const QString& ty : m_restrictTypes)
                t.push_back({ty, m_idx->typeCounts.value(ty, 0)});
        } else {
            t = topFacets(m_idx->typeCounts, kMaxFacetRows);
        }
        groups.push_back({m_typeList, t, &m_checkedTypes});
    }
    groups.push_back({m_catList, topFacets(m_idx->catCounts, kMaxFacetRows),
                      &m_checkedCats});
    groups.push_back({m_subList, topFacets(m_idx->subcatCounts, kMaxFacetRows),
                      &m_checkedSubs});
    if (m_clsList)
        groups.push_back({m_clsList, topFacets(m_idx->classCounts, kMaxFacetRows),
                          &m_checkedCls});
    if (m_slotList)
        groups.push_back({m_slotList, topFacets(m_idx->slotCounts, kMaxFacetRows),
                          &m_checkedSlots});
    for (const Group& g : groups) {
        g.list->clear();
        for (const FacetRow& r : g.rows) {
            auto* item = new QListWidgetItem(
                QStringLiteral("%1  (%L2)").arg(r.value).arg(r.count), g.list);
            item->setData(Qt::UserRole, r.value);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(g.checked->contains(r.value) ? Qt::Checked
                                                             : Qt::Unchecked);
        }
    }
    m_updating = false;
}

void FilterBar::refreshChips()
{
    // rebuild the chip row from the checked sets
    while (QLayoutItem* it = m_chipLay->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            w->hide();
            w->deleteLater();   // a chip's own clicked() lands here — never
                                // delete the emitting button mid-emission
        }
        delete it;
    }
    struct ChipGroup {
        const char* label;
        QSet<QString>* set;
    };
    const ChipGroup groups[] = {{"Type", &m_checkedTypes},
                                {"Category", &m_checkedCats},
                                {"Sub", &m_checkedSubs},
                                {"Class", &m_checkedCls},
                                {"Slot", &m_checkedSlots}};
    int chips = 0;
    for (const ChipGroup& g : groups) {
        QStringList vals(g.set->begin(), g.set->end());
        vals.sort(Qt::CaseInsensitive);
        for (const QString& v : vals) {
            auto* chip = new QToolButton(m_chipRow);
            chip->setText(QStringLiteral("%1: %2  ✕").arg(QLatin1String(g.label), v));
            chip->setAutoRaise(true);
            chip->setStyleSheet(QStringLiteral(
                "QToolButton{border:1px solid #6a6a6a;border-radius:8px;"
                "padding:1px 6px;background:#33301f;}"));
            QSet<QString>* set = g.set;
            connect(chip, &QToolButton::clicked, this, [this, set, v] {
                set->remove(v);
                refreshPopupLists();
                refreshChips();
                persist();
                emit changed();
            });
            m_chipLay->addWidget(chip);
            ++chips;
        }
    }
    m_chipLay->addStretch(1);
    m_chipRow->setVisible(chips > 0);
}

void FilterBar::setIndex(std::shared_ptr<AssetIndex> idx)
{
    m_idx = std::move(idx);
    // drop selections that no longer exist in the new index
    if (m_idx) {
        auto prune = [](QSet<QString>& set, const QHash<QString, int>& counts) {
            for (auto it = set.begin(); it != set.end();)
                it = counts.contains(*it) ? std::next(it) : set.erase(it);
        };
        prune(m_checkedTypes, m_idx->typeCounts);
        prune(m_checkedCats, m_idx->catCounts);
        prune(m_checkedSubs, m_idx->subcatCounts);
        prune(m_checkedCls, m_idx->classCounts);
        prune(m_checkedSlots, m_idx->slotCounts);
    }
    refreshPopupLists();
    refreshChips();
}

void FilterBar::applySpec(const FilterSpec& s)
{
    {
        // one changed() at the end, not one per programmatic edit
        QSignalBlocker block(m_search);
        m_search->setText(s.text);
        m_debounce->stop();   // setText started the debounce; we emit directly
    }
    if (m_pinnedType.isEmpty()) {
        m_checkedTypes = s.types;
        if (!m_restrictTypes.isEmpty()) {
            const QSet<QString> allowed(m_restrictTypes.begin(),
                                        m_restrictTypes.end());
            m_checkedTypes.intersect(allowed);
        }
    }
    m_checkedCats  = s.cats;
    m_checkedSubs  = s.subcats;
    m_checkedCls   = s.classes;
    m_checkedSlots = s.cosSlots;
    refreshPopupLists();
    refreshChips();
    persist();
    emit changed();
}

FilterSpec FilterBar::spec() const
{
    FilterSpec s;
    s.text = m_search->text();
    if (!m_pinnedType.isEmpty()) {
        s.types = {m_pinnedType};
    } else if (!m_restrictTypes.isEmpty()) {
        // restricted mode: no check = ALL allowed types (never "everything")
        s.types = m_checkedTypes.isEmpty()
                      ? QSet<QString>(m_restrictTypes.begin(), m_restrictTypes.end())
                      : m_checkedTypes;
    } else {
        s.types = m_checkedTypes;
    }
    s.cats     = m_checkedCats;
    s.subcats  = m_checkedSubs;
    s.classes  = m_checkedCls;
    s.cosSlots = m_checkedSlots;
    return s;
}

// ---- persistence -----------------------------------------------------------
// Stored per key so the Models bar and the Bulk Extract bar keep separate
// state. Facet sets round-trip as sorted string lists; anything the next
// index no longer contains is dropped by the prune in setIndex().

void FilterBar::persist() const
{
    if (m_persistKey.isEmpty()) return;
    QSettings st;
    st.beginGroup(QStringLiteral("filterbar/") + m_persistKey);
    st.setValue(QStringLiteral("text"), m_search->text());
    auto put = [&](const char* k, const QSet<QString>& v) {
        QStringList l(v.begin(), v.end());
        l.sort();
        st.setValue(QLatin1String(k), l);
    };
    put("types", m_checkedTypes);
    put("cats", m_checkedCats);
    put("subs", m_checkedSubs);
    put("cls", m_checkedCls);
    put("slot", m_checkedSlots);
    st.endGroup();
}

void FilterBar::setPersistKey(const QString& key)
{
    m_persistKey = key;
    if (key.isEmpty()) return;
    QSettings st;
    st.beginGroup(QStringLiteral("filterbar/") + key);
    if (st.childKeys().isEmpty()) { st.endGroup(); return; }
    {
        QSignalBlocker block(m_search);
        m_search->setText(st.value(QStringLiteral("text")).toString());
        m_debounce->stop();
    }
    auto get = [&](const char* k) {
        const QStringList l = st.value(QLatin1String(k)).toStringList();
        return QSet<QString>(l.begin(), l.end());
    };
    m_checkedTypes = get("types");
    m_checkedCats  = get("cats");
    m_checkedSubs  = get("subs");
    m_checkedCls   = get("cls");
    m_checkedSlots = get("slot");
    st.endGroup();
    if (!m_restrictTypes.isEmpty()) {
        const QSet<QString> allowed(m_restrictTypes.begin(), m_restrictTypes.end());
        m_checkedTypes.intersect(allowed);
    }
    refreshPopupLists();
    refreshChips();
}
