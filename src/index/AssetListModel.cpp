#include "index/AssetListModel.h"

#include <QLocale>
#include <QPixmap>

#include "index/ThumbnailProvider.h"
#include "gl/ModelThumbRenderer.h"

#include <algorithm>

AssetListModel::AssetListModel(QObject* parent) : QAbstractTableModel(parent) {}

void AssetListModel::setIndex(std::shared_ptr<AssetIndex> idx, const FilterSpec& spec)
{
    m_idx = std::move(idx);
    setFilters(spec);
}

void AssetListModel::setFilters(const FilterSpec& spec)
{
    m_spec = spec;
    m_incTerms.clear();
    m_excTerms.clear();
    const QStringList parts = spec.text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        if (p.startsWith(QLatin1Char('-')) && p.size() > 1)
            m_excTerms << p.mid(1);
        else if (!p.startsWith(QLatin1Char('-')))
            m_incTerms << p;
    }
    rebuild();
}

const AssetRow* AssetListModel::rowAt(int row) const
{
    if (!m_idx || row < 0 || row >= (int)m_visible.size()) return nullptr;
    return &m_idx->rows[m_visible[row]];
}

int AssetListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : (int)m_visible.size();
}

int AssetListModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColCount;
}

QVariant AssetListModel::data(const QModelIndex& index, int role) const
{
    const AssetRow* r = rowAt(index.row());
    if (!r) return {};
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case ColName: return r->display;
            case ColMeaning: return r->meaning;
            case ColType: return r->type;
            case ColSize: return QLocale::c().formattedDataSize(r->size, 1);
            case ColPak: {
                const auto& e = m_idx->store->mpk().entries()[r->entryId];
                return QString::fromLatin1(m_idx->store->mpk().pakFileName(e).c_str());
            }
            // Blank, not "0", for rows that cannot have clips at all — a zero
            // there would read as "measured none" for a Texture2D row.
            case ColClips:
                return r->repoIdx < 0 ? QString()
                                      : QString::number(r->animClips);
            default: break;
        }
    } else if (role == Qt::TextAlignmentRole && index.column() == ColClips) {
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    } else if (role == Qt::DecorationRole && index.column() == ColName &&
               (m_thumbs || m_thumbs3d)) {
        QPixmap pm;
        if (m_use3d && m_thumbs3d) {
            if (m_thumbs3d->get(*r, &pm)) return pm;
            return {};   // queued/rendering; ready() triggers a repaint
        }
        if (m_thumbs && m_thumbs->get(*r, &pm)) return pm;
        return {};   // queued; ThumbnailProvider::ready() triggers a repaint
    } else if (role == Qt::ToolTipRole && index.column() == ColName) {
        // physical location is always available on hover, even when the display
        // name is the logical repository path
        return r->mpkName;
    }
    return {};
}

QVariant AssetListModel::headerData(int section, Qt::Orientation orient, int role) const
{
    if (orient != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case ColName:    return QStringLiteral("Name");
        case ColMeaning: return QStringLiteral("Meaning");
        case ColType:    return QStringLiteral("Type");
        case ColSize:    return QStringLiteral("Size");
        case ColPak:     return QStringLiteral("Pak");
        case ColClips:   return QStringLiteral("Clips");
        default: return {};
    }
}

void AssetListModel::sort(int column, Qt::SortOrder order)
{
    m_sortCol   = column;
    m_sortOrder = order;
    rebuild();
}

void AssetListModel::rebuild()
{
    beginResetModel();
    m_visible.clear();
    if (m_idx) {
        const auto& rows = m_idx->rows;
        m_visible.reserve((int)rows.size());
        for (int i = 0; i < (int)rows.size(); ++i) {
            const AssetRow& r = rows[i];
            if (!m_spec.types.isEmpty()   && !m_spec.types.contains(r.type))   continue;
            if (!m_spec.cats.isEmpty()    && !m_spec.cats.contains(r.cat1))    continue;
            if (!m_spec.subcats.isEmpty() && !m_spec.subcats.contains(r.cat2)) continue;
            if (!m_spec.classes.isEmpty() && !m_spec.classes.contains(r.cls)) continue;
            if (!m_spec.cosSlots.isEmpty() && !m_spec.cosSlots.contains(r.slot)) continue;
            if (m_spec.anim == FilterSpec::AnimOnly   && r.animClips == 0) continue;
            if (m_spec.anim == FilterSpec::StaticOnly && r.animClips != 0) continue;
            if (m_spec.who == FilterSpec::PlayerOnly    && !r.player) continue;
            if (m_spec.who == FilterSpec::NonPlayerOnly && r.player)  continue;
            bool ok = true;
            for (const QString& t : m_incTerms) {
                if (!r.display.contains(t, Qt::CaseInsensitive) &&
                    !r.mpkName.contains(t, Qt::CaseInsensitive) &&
                    !r.meaning.contains(t, Qt::CaseInsensitive)) { ok = false; break; }
            }
            if (ok) {
                for (const QString& t : m_excTerms) {
                    if (r.display.contains(t, Qt::CaseInsensitive) ||
                        r.mpkName.contains(t, Qt::CaseInsensitive) ||
                        r.meaning.contains(t, Qt::CaseInsensitive)) { ok = false; break; }
                }
            }
            if (ok) m_visible.push_back(i);
        }

        const auto& all = m_idx->rows;
        const bool asc  = (m_sortOrder == Qt::AscendingOrder);
        const int  col  = m_sortCol;
        std::sort(m_visible.begin(), m_visible.end(), [&all, asc, col](int a, int b) {
            const AssetRow& ra = all[a];
            const AssetRow& rb = all[b];
            int c = 0;
            switch (col) {
                case ColMeaning:
                    c = QString::compare(ra.meaning, rb.meaning, Qt::CaseInsensitive);
                    break;
                case ColType: c = QString::compare(ra.type, rb.type, Qt::CaseInsensitive); break;
                case ColSize: c = ra.size < rb.size ? -1 : (ra.size > rb.size ? 1 : 0);    break;
                case ColPak:  c = ra.entryId < rb.entryId ? -1
                                : (ra.entryId > rb.entryId ? 1 : 0);                       break;
                case ColClips: c = ra.animClips < rb.animClips ? -1
                                 : (ra.animClips > rb.animClips ? 1 : 0);                  break;
                default:      break;
            }
            if (c == 0) c = QString::compare(ra.display, rb.display, Qt::CaseInsensitive);
            return asc ? c < 0 : c > 0;
        });
    }
    endResetModel();
}
