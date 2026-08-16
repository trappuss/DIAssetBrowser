#include "index/ThumbnailProvider.h"

#include <QPainter>

#include "app/SehGuard.h"
#include "model/ModelResolve.h"
#include "tex/TextureDecode.h"
#include "tex/TextureParser.h"

namespace {
constexpr size_t kQueueCap = 512;   // beyond this, oldest requests are stale
                                    // scroll positions — drop them
constexpr int    kWorkers  = 2;
} // namespace

ThumbnailProvider::ThumbnailProvider(std::shared_ptr<di::DiAssetStore> store,
                                     int targetPx, QObject* parent)
    : QObject(parent), m_store(std::move(store)),
      m_targetPx(targetPx > 0 ? targetPx : kSize)
{
    // Constant memory across icon sizes: 4096 entries at 40px is ~26 MB of
    // pixmaps; larger edges get proportionally fewer cached entries.
    const int cap = qMax(256, 4096 * kSize * kSize / (m_targetPx * m_targetPx));
    m_cache.setMaxCost(cap);
    for (int i = 0; i < kWorkers; ++i)
        m_threads.emplace_back([this] { loop(); });
}

ThumbnailProvider::~ThumbnailProvider()
{
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_threads)
        t.join();
}

bool ThumbnailProvider::peek(uint32_t entryId, QPixmap* out) const
{
    if (QPixmap* pm = m_cache.object(entryId)) {
        *out = *pm;
        return true;
    }
    return false;
}

bool ThumbnailProvider::get(const AssetRow& row, QPixmap* out)
{
    if (QPixmap* pm = m_cache.object(row.entryId)) {
        *out = *pm;
        return true;
    }
    if (m_failed.contains(row.entryId)) return false;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_queued.contains(row.entryId)) {
            m_queued.insert(row.entryId);
            m_queue.push_front(Req{row.entryId, row.repoIdx, row.type});
            while (m_queue.size() > kQueueCap) {
                m_queued.remove(m_queue.back().entryId);
                m_queue.pop_back();
            }
        }
    }
    m_cv.notify_one();
    return false;
}

void ThumbnailProvider::commit()
{
    std::vector<std::pair<uint32_t, QImage>> done;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        done.swap(m_done);
    }
    for (auto& [id, img] : done) {
        if (img.isNull()) {
            m_failed.insert(id);
        } else {
            m_cache.insert(id, new QPixmap(QPixmap::fromImage(img)));
            m_failed.remove(id);
        }
    }
}

void ThumbnailProvider::loop()
{
    seh::installSehTranslator();
    for (;;) {
        Req req;
        {
            std::unique_lock<std::mutex> lock(m_mtx);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            req = std::move(m_queue.front());
            m_queue.pop_front();
        }

        QImage thumb;
        seh::runGuarded("thumbnail", [&] {
            const size_t blob =
                di::resolveThumbTexture(*m_store, req.repoIdx, req.entryId, req.type);
            if (blob == (size_t)-1) return;
            const std::vector<uint8_t> raw = m_store->mpk().readAsset(blob);
            if (raw.empty()) return;
            di::Texture2D tex;
            std::string err;
            if (!di::isTexture2D(raw.data(), raw.size()) ||
                !di::parseTexture2D(raw.data(), raw.size(), &tex, &err))
                return;
            // smallest mip that is still >= targetPx on both axes; fall back
            // to the largest available. Selected by AREA, not slice order —
            // measured: this store keeps mips smallest-FIRST (yifu_d slice 8
            // is the 1024px top mip), the opposite of the usual convention.
            const int want = m_targetPx;
            int pick = -1, best = -1, bestArea = -1, largest = -1, largestArea = -1;
            for (int i = 0; i < (int)tex.mips.size(); ++i) {
                const int w = (int)tex.mips[i].width, h = (int)tex.mips[i].height;
                const int area = w * h;
                if (area > largestArea) { largestArea = area; largest = i; }
                if (w >= want && h >= want && (best < 0 || area < bestArea)) {
                    best = i;
                    bestArea = area;
                }
            }
            pick = best >= 0 ? best : largest;
            if (pick < 0) return;
            const QImage img = TextureDecode::decode(tex, pick).image;
            if (img.isNull()) return;
            // Composite over a dark backdrop: low-alpha textures (fmt-3 mix
            // maps carry ~0 alpha) must still show as a visible thumbnail.
            QImage flat(img.size(), QImage::Format_RGB32);
            flat.fill(QColor(45, 45, 48));
            {
                QPainter p(&flat);
                p.drawImage(0, 0, img);
            }
            thumb = flat.scaled(m_targetPx, m_targetPx, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
        });

        bool emitReady = false;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_queued.remove(req.entryId);
            emitReady = m_done.empty();
            m_done.emplace_back(req.entryId, std::move(thumb));
        }
        if (emitReady)   // coalesce: one ready() per burst, not per thumbnail
            emit ready();
    }
}
