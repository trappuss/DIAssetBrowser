#pragma once
// Flat Name | Type | Size | Pak model over AssetIndex rows — same architecture
// as the D4 tool's SnoListModel: an immutable entry vector plus a filtered,
// sorted index list. No per-row widget allocation, so 677k rows stay instant.
//
// Search grammar (matches the D4 filter box): space-separated terms AND'd;
// "-term" excludes; matching is case-insensitive substring over display name
// AND physical MPK name.

#include <QAbstractTableModel>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

#include "index/AssetIndex.h"
#include "util/FilterSpec.h"

class ThumbnailProvider;
class ModelThumbRenderer;

class AssetListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    // ColClips is appended, never inserted — tabs that don't want it hide it
    // (Assets, Textures), so an added column can't shift anyone's layout.
    enum Col { ColName = 0, ColMeaning, ColType, ColSize, ColPak, ColClips, ColCount };

    explicit AssetListModel(QObject* parent = nullptr);

    // Each of these triggers exactly ONE rebuild (full 677k scan + sort, ~100ms
    // class) — which is why the whole FilterSpec lands in a single call.
    void setIndex(std::shared_ptr<AssetIndex> idx, const FilterSpec& spec);
    void setFilters(const FilterSpec& spec);

    // Optional async thumbnails for ColName (Models/Textures tabs). The
    // provider must outlive this model or be cleared first; the model does
    // not own it.
    void setThumbnailProvider(ThumbnailProvider* p) { m_thumbs = p; }

    // Optional 3D-rendered thumbnails (Models tab). When both a renderer is set
    // and use-3D is on, ColName decoration comes from it instead of the flat
    // texture provider; the two caches are independent so toggling is instant.
    void setModelThumbs(ModelThumbRenderer* p) { m_thumbs3d = p; }
    void setUse3DThumbs(bool on) { m_use3d = on; }
    bool use3DThumbs() const { return m_use3d; }

    int  totalCount()   const { return m_idx ? (int)m_idx->rows.size() : 0; }
    int  visibleCount() const { return (int)m_visible.size(); }
    // nullptr when out of range.
    const AssetRow* rowAt(int row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orient, int role) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    void rebuild();

    std::shared_ptr<AssetIndex> m_idx;
    ThumbnailProvider*  m_thumbs   = nullptr;
    ModelThumbRenderer* m_thumbs3d = nullptr;
    bool                m_use3d    = false;
    QVector<int>  m_visible;
    QStringList   m_incTerms;
    QStringList   m_excTerms;
    FilterSpec    m_spec;
    int           m_sortCol   = ColName;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};
