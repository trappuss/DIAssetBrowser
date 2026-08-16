#pragma once
// FilterBar — the shared search + funnel control (the D4 filter pattern):
// search box (debounced, -term excludes), a funnel popup of facet checkbox
// groups (Type / Category / Subcategory, with row counts), and removable
// chips for every active facet. Emits one changed() per user action; callers
// read spec() and hand it to AssetListModel::setFilters (single rebuild).

#include <QSet>
#include <QStringList>
#include <QWidget>

#include <memory>

#include "index/AssetIndex.h"
#include "util/FilterSpec.h"

class QHBoxLayout;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;

class FilterBar : public QWidget {
    Q_OBJECT
public:
    // restrictTypes: non-empty = the type facet offers ONLY these (Models tab);
    //                when none are checked the spec carries ALL of them.
    // pinnedType:    always applied, type group hidden (Textures tab).
    // cosmeticFacets: also offer the Class and Slot groups (decoded from the
    //                 asset-name grammar, not the folder) — Models tab.
    explicit FilterBar(QWidget* parent = nullptr,
                       const QStringList& restrictTypes = {},
                       const QString& pinnedType = {},
                       bool cosmeticFacets = false);

    // Remember search text + every checked facet across restarts under this
    // key. Restores immediately; saves on every later change.
    void setPersistKey(const QString& key);

    void setIndex(std::shared_ptr<AssetIndex> idx);   // refresh facet lists
    FilterSpec spec() const;

    // Preset support (Bulk Extract query presets): restore a captured spec —
    // search text + checked facet sets — emitting exactly one changed().
    // Meaningful on an UNRESTRICTED bar (no pinnedType/restrictTypes); on a
    // restricted bar the type set is intersected with what the bar offers.
    void applySpec(const FilterSpec& s);

    // Host a caller-owned section inside the funnel popup (above "Clear all
    // filters"). Lets a tab fold its own facet controls (e.g. the Models tab's
    // Show / Animation dropdowns) into the one filter menu instead of a
    // separate header row. Forces the popup to build if it hasn't yet.
    void addPopupSection(QWidget* content);

signals:
    void changed();

private:
    void buildPopup();
    void persist() const;
    void refreshPopupLists();
    void refreshChips();
    QListWidget* makeGroupList(const QString& title, QWidget* parent);

    std::shared_ptr<AssetIndex> m_idx;
    QStringList m_restrictTypes;
    QString     m_pinnedType;
    bool        m_cosmeticFacets = false;
    QString     m_persistKey;

    QLineEdit*   m_search = nullptr;
    QToolButton* m_funnel = nullptr;
    QTimer*      m_debounce = nullptr;
    QWidget*     m_chipRow  = nullptr;
    QHBoxLayout* m_chipLay  = nullptr;

    QWidget*     m_popup    = nullptr;
    QVBoxLayout* m_popupLay = nullptr;   // popup's vbox (for addPopupSection)
    QPushButton* m_popupClear = nullptr; // sections insert above this
    QListWidget* m_typeList = nullptr;
    QListWidget* m_catList  = nullptr;
    QListWidget* m_subList  = nullptr;
    QListWidget* m_clsList  = nullptr;
    QListWidget* m_slotList = nullptr;

    QSet<QString> m_checkedTypes, m_checkedCats, m_checkedSubs;
    QSet<QString> m_checkedCls, m_checkedSlots;
    bool m_updating = false;   // guard: programmatic checks must not re-emit
};
