#pragma once
// Async list thumbnails for texture-backed rows (Texture2D directly; Model /
// LodModel / Mesh via their Material's tBaseMap). GUI thread asks get(); a
// cache hit returns the pixmap immediately, a miss enqueues the row (LIFO —
// what the user is looking at NOW decodes first) and returns false. Worker
// threads decode the SMALLEST usable mip (>=32px when available), post
// QImages back, and emit ready(); the tab then commit()s the results into the
// pixmap cache (QPixmap must only be touched on the GUI thread) and repaints
// the viewport, which re-queries data() and now hits.
//
// Never caches raw asset bytes — only the finished ~40px pixmaps (ground
// rule: minimal caching), capped by an LRU. Failed decodes are remembered so
// scrolling doesn't re-attempt dead rows.

#include <QCache>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "index/AssetIndex.h"
#include "store/AssetStore.h"

class ThumbnailProvider : public QObject {
    Q_OBJECT
public:
    static constexpr int kSize = 40;   // square edge, default view icon size

    // targetPx: decoded square edge (default kSize). The Textures tab's grid
    // view asks for a larger edge so thumbnails stay crisp at grid size; the
    // pixmap cache cap shrinks with the square of the edge so total pixmap
    // memory stays in the same ~25-50 MB band either way.
    explicit ThumbnailProvider(std::shared_ptr<di::DiAssetStore> store,
                               int targetPx = kSize, QObject* parent = nullptr);
    ~ThumbnailProvider() override;     // stops + joins the workers

    int targetPx() const { return m_targetPx; }

    // GUI thread only. True = *out valid now. False = queued (or failed) —
    // a later ready() means new results are waiting for commit().
    bool get(const AssetRow& row, QPixmap* out);

    // Cache-only lookup — NEVER enqueues. Hover popups use this so a dwell
    // can't flood the decode queue with rows the user merely swept across
    // (the D4 grid-thumb lesson).
    bool peek(uint32_t entryId, QPixmap* out) const;

    // GUI thread only: move finished decodes into the pixmap cache.
    void commit();

signals:
    void ready();   // emitted from a worker; connect queued (cross-thread default)

private:
    struct Req {
        uint32_t entryId = 0;
        int32_t  repoIdx = -1;
        QString  type;
    };
    void loop();

    std::shared_ptr<di::DiAssetStore> m_store;
    int m_targetPx = kSize;
    QCache<uint32_t, QPixmap> m_cache{4096};
    QSet<uint32_t> m_failed;   // GUI thread
    QSet<uint32_t> m_queued;   // GUI thread (guarded by m_mtx for erase on worker)

    std::mutex              m_mtx;
    std::condition_variable m_cv;
    std::deque<Req>         m_queue;
    std::vector<std::pair<uint32_t, QImage>> m_done;   // null image = failed
    bool m_stop = false;
    std::vector<std::thread> m_threads;
};
