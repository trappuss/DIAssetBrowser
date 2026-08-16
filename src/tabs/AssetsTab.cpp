#include "tabs/AssetsTab.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

#include "app/SehGuard.h"
#include "index/AssetListModel.h"
#include "store/Zzz4.h"
#include "util/CsvCopy.h"
#include "util/FilterBar.h"
#include "util/MenuText.h"
#include "util/PanelPersist.h"

namespace {

const char kRawDirKey[] = "assets/lastRawDir";

QString hexDump(const std::vector<uint8_t>& b, size_t maxBytes)
{
    QString out;
    const size_t n = b.size() < maxBytes ? b.size() : maxBytes;
    for (size_t off = 0; off < n; off += 16) {
        QString hex, asc;
        for (size_t i = off; i < off + 16 && i < n; ++i) {
            hex += QStringLiteral("%1 ").arg(b[i], 2, 16, QLatin1Char('0'));
            asc += (b[i] >= 0x20 && b[i] < 0x7F) ? QChar(b[i]) : QLatin1Char('.');
        }
        out += QStringLiteral("%1  %2 %3\n")
                   .arg(off, 6, 16, QLatin1Char('0'))
                   .arg(hex, -48)
                   .arg(asc);
    }
    return out;
}

} // namespace

AssetsTab::AssetsTab(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);

    m_filter = new FilterBar(this);
    lay->addWidget(m_filter);

    auto* split = new QSplitter(Qt::Horizontal, this);
    lay->addWidget(split, 1);

    m_model = new AssetListModel(this);
    m_table = new QTableView(split);
    m_table->setModel(m_model);
    // clip counts are a Models-tab concern
    m_table->setColumnHidden(AssetListModel::ColClips, true);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(AssetListModel::ColName, Qt::AscendingOrder);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(20);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(AssetListModel::ColName,
                                                      QHeaderView::Stretch);
    m_table->setShowGrid(false);
    // Context menu + Ctrl+C CSV. Policy is set BEFORE CsvCopy::install so its
    // fallback menu stands down and ours wins (documented CsvCopy trap).
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this,
            [this](const QPoint& p) { showRowMenu(p); });
    CsvCopy::install(m_table);

    auto* right = new QWidget(split);
    auto* rlay  = new QVBoxLayout(right);
    rlay->setContentsMargins(0, 0, 0, 0);
    m_header = new QLabel(right);
    m_header->setWordWrap(true);
    m_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rlay->addWidget(m_header);
    m_details = new QPlainTextEdit(right);
    m_details->setReadOnly(true);
    m_details->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_details->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{font-family:Consolas,'Courier New',monospace;font-size:11px;}"));
    rlay->addWidget(m_details, 1);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    PanelPersist::bind(split, QStringLiteral("panels/assetsSplit"));

    connect(m_filter, &FilterBar::changed, this, &AssetsTab::applyFilter);

    // Also fires with an INVALID index after every model reset (filter/sort) —
    // clear the pane then, or it keeps showing an asset that may no longer be
    // in the visible set at all.
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                m_curRow = cur.isValid() ? cur.row() : -1;
                showDetails(m_curRow);
            });
}

void AssetsTab::showRowMenu(const QPoint& viewportPos)
{
    if (!m_idx) return;
    const QModelIndex idx = m_table->indexAt(viewportPos);
    const int row = idx.isValid() ? idx.row() : m_curRow;
    const AssetRow* r = m_model->rowAt(row);
    if (!r) return;
    const QString name = r->display, meaning = r->meaning, mpk = r->mpkName;

    QMenu menu(this);
    menu.addAction(MenuText::exportSetPrompt(QStringLiteral("raw bytes")), this,
                   [this, row] {
                       // act on the clicked row even if it isn't the current one
                       const AssetRow* rr = m_model->rowAt(row);
                       if (!rr) return;
                       m_curRow = row;
                       exportNow(false);
                   });
    menu.addSeparator();
    menu.addAction(MenuText::withValue(MenuText::kCopyName, name), this, [name] {
        QGuiApplication::clipboard()->setText(name);
    });
    if (!meaning.isEmpty())
        menu.addAction(MenuText::withValue(MenuText::kCopyMeaning, meaning), this,
                       [meaning] {
                           QGuiApplication::clipboard()->setText(meaning);
                       });
    if (!mpk.isEmpty())
        menu.addAction(MenuText::withValue(MenuText::kCopyMpkPath, mpk), this,
                       [mpk] { QGuiApplication::clipboard()->setText(mpk); });
    menu.exec(m_table->viewport()->mapToGlobal(viewportPos));
}

// ── ExportHooks: Ctrl+E saves the current row's exact stored bytes ─────────

bool AssetsTab::canExport() const
{
    return m_idx && m_idx->store && m_model->rowAt(m_curRow) != nullptr;
}

QString AssetsTab::exportWhat() const
{
    return QStringLiteral("raw asset");
}

void AssetsTab::exportNow(bool toLastDir)
{
    const AssetRow* r = m_model->rowAt(m_curRow);
    if (!r || !m_idx || !m_idx->store) return;
    // RAW means raw: mpk().read(), the exact stored bytes, no inflate — the
    // same promise the Bulk tab's Raw mode makes.
    QString leaf = r->mpkName.isEmpty() ? r->display : r->mpkName;
    const int slash = leaf.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) leaf = leaf.mid(slash + 1);
    QSettings s;
    const QString last = s.value(QLatin1String(kRawDirKey)).toString();
    QString dest;
    if (toLastDir && !last.isEmpty() && QDir(last).exists()) {
        dest = QDir(last).filePath(leaf);
    } else {
        dest = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export raw bytes"),
            last.isEmpty() ? leaf : QDir(last).filePath(leaf));
        if (dest.isEmpty()) return;
    }
    const std::vector<uint8_t> raw =
        m_idx->store->mpk().read((size_t)r->entryId);
    QFile f(dest);
    const bool ok = !raw.empty() &&
                    f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    f.write(reinterpret_cast<const char*>(raw.data()),
                            (qint64)raw.size()) == (qint64)raw.size();
    if (ok) {
        s.setValue(QLatin1String(kRawDirKey), QFileInfo(dest).absolutePath());
        qInfo("assets: exported %zu raw bytes -> %s", raw.size(),
              qPrintable(dest));
    } else {
        qWarning("assets: raw export FAILED for %s (%zu bytes) -> %s",
                 qPrintable(r->display), raw.size(), qPrintable(dest));
    }
    updateStatus();
}

QString AssetsTab::lastExportDir() const
{
    const QString d = QSettings().value(QLatin1String(kRawDirKey)).toString();
    return QDir(d).exists() ? d : QString();
}

void AssetsTab::setIndex(std::shared_ptr<AssetIndex> idx)
{
    m_idx = std::move(idx);
    m_filter->setIndex(m_idx);
    m_model->setIndex(m_idx, m_filter->spec());
    updateStatus();
}

void AssetsTab::applyFilter()
{
    m_model->setFilters(m_filter->spec());
    updateStatus();
}

void AssetsTab::updateStatus()
{
    emit statusText(QStringLiteral("%L1 / %L2 assets")
                        .arg(m_model->visibleCount())
                        .arg(m_model->totalCount()));
}

void AssetsTab::showDetails(int modelRow)
{
    const AssetRow* r = m_model->rowAt(modelRow);
    if (!r || !m_idx) {
        m_header->clear();
        m_details->clear();
        return;
    }
    const di::DiAssetStore& store = *m_idx->store;
    const di::MpkEntry&     e     = store.mpk().entries()[r->entryId];

    QString head = QStringLiteral("<b>%1</b><br>%2 &nbsp;·&nbsp; %3 &nbsp;·&nbsp; "
                                  "%4 @ 0x%5")
                       .arg(r->display.toHtmlEscaped(), r->type,
                            QLocale::c().formattedDataSize(e.length, 1),
                            QString::fromLatin1(store.mpk().pakFileName(e).c_str()),
                            QString::number(e.offset, 16));

    QString body;
    if (r->repoIdx >= 0 && store.repo()) {
        const di::Repository& repo = *store.repo();
        const di::RepoEntry&  re   = repo.entries[(size_t)r->repoIdx];
        body += QStringLiteral("physical: %1\nhash:     %2\n")
                    .arg(r->mpkName, QString::fromLatin1(re.hashHex.c_str()));
        if (!re.related.empty()) {
            body += QStringLiteral("\ndependencies (%1):\n").arg(re.related.size());
            for (const std::string& h : re.related) {
                auto it = repo.byHash.find(h);
                if (it != repo.byHash.end()) {
                    const di::RepoEntry& dep = repo.entries[it->second];
                    body += QStringLiteral("  %1  %2\n")
                                .arg(QString::fromLatin1(repo.typeOf(dep).c_str()), -20)
                                .arg(QString::fromLatin1(repo.pathOf(dep).c_str()));
                } else {
                    body += QStringLiteral("  %1  <hash not in catalog>\n")
                                .arg(QString::fromLatin1(h.c_str()));
                }
            }
        }
        body += QLatin1Char('\n');
    }

    // Raw header hexdump (cheap partial read), plus the inflated head for ZZZ4
    // blobs so the contained format's magic is visible at a glance. The inflate
    // decodes AT MOST 128 bytes from a 64 KiB partial read — never the whole
    // blob, so arrow-keying over a multi-hundred-MB asset costs nothing.
    std::vector<uint8_t> raw;
    seh::runGuarded("details-read", [&] {
        raw = store.mpk().readHeader(r->entryId, 64 * 1024);
    });
    body += QStringLiteral("raw header (first %1 of %L2 bytes):\n")
                .arg(raw.size() < 256 ? raw.size() : 256)
                .arg(e.length);
    body += hexDump(raw, 256);
    if (raw.size() >= 8 && di::isZzz4(raw.data(), raw.size())) {
        std::vector<uint8_t> head;
        seh::runGuarded("details-inflate", [&] {
            // tolerant decoder stops when either 128 bytes are produced or the
            // partial input runs out — correct for a head preview either way
            head = di::lz4Tolerant(raw.data(), raw.size(), 8, 128);
        });
        body += QStringLiteral("\nZZZ4 head (first %1 bytes inflated):\n").arg(head.size());
        body += hexDump(head, 128);
    }

    m_header->setText(head);
    m_details->setPlainText(body);
}
