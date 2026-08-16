#pragma once
// Highly-optimized 3D model thumbnails for the browse views — an opt-in
// replacement for the flat texture icons ThumbnailProvider produces.
//
// Split of work, chosen so the GUI thread never blocks and the GPU never
// renders anything off-screen:
//   * CPU worker pool (2 threads): resolve the model chain, read + parse the
//     mesh, decode the diffuse texture's smallest usable mip, compute the AABB,
//     and hand a TRIMMED CPU payload (positions / uv / indices + one QImage) to
//     the GUI thread. No GL here.
//   * GUI thread (commit()): lazily builds ONE shared offscreen GL context +
//     FBO (GL objects are single-thread), renders a small batch of prepared
//     payloads to an FBO, reads them back, and caches the finished pixmaps.
//     Rendering a 160px mesh is sub-millisecond; a per-commit cap keeps frames
//     smooth and re-schedules the remainder.
//
// Only-what-is-visible falls out of the view/model contract: a view asks
// data(DecorationRole) for visible rows only, so get() is called only for rows
// on screen; misses enqueue LIFO (newest scroll position first) and the queue
// is capped so a fast scroll drops stale requests. Finished thumbnails live in
// a RAM pixmap LRU whose entry count is sized to a fixed memory band. Nothing
// is written to disk and no raw asset bytes are cached.
//
// The get()/peek()/commit()/ready() surface mirrors ThumbnailProvider so the
// model can hold either behind one code path.

#include <QCache>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QVector3D>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "index/AssetIndex.h"
#include "store/AssetStore.h"

class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLShaderProgram;
class QOpenGLFramebufferObject;
class QOpenGLVertexArrayObject;
class QOpenGLBuffer;

class ModelThumbRenderer : public QObject {
    Q_OBJECT
public:
    // targetPx: cached square edge. The grid view shows 2x the list icon, so a
    // mid value stays crisp there while the list downsizes cleanly.
    explicit ModelThumbRenderer(std::shared_ptr<di::DiAssetStore> store,
                                int targetPx = 80, QObject* parent = nullptr);
    ~ModelThumbRenderer() override;   // stops + joins workers, tears down GL

    int targetPx() const { return m_targetPx; }

    // GUI thread only. True = *out valid now (cache hit). False = queued (or a
    // known failure); a later ready() means results are waiting for commit().
    bool get(const AssetRow& row, QPixmap* out);
    // Cache-only lookup — never enqueues (hover popups).
    bool peek(uint32_t entryId, QPixmap* out) const;
    // GUI thread only: render prepared payloads to the FBO and cache them.
    void commit();

    // GUI thread only: synchronously resolve, parse and render ONE row at `px`
    // for the hover popup (a single deliberate dwell, so the brief parse is
    // acceptable). Not cached. False = not a mesh / parse failed / GL bad.
    bool renderPreview(const AssetRow& row, int px, QImage* out);

signals:
    void ready();   // emitted from a worker (queued) and to re-drive a backlog

private:
    struct Req { uint32_t entryId = 0; int32_t repoIdx = -1; QString type; };
    // A parsed, trimmed CPU payload waiting for the GUI thread to render it.
    struct Prepared {
        uint32_t entryId = 0;
        bool     ok = false;              // false = resolve/parse failed → mark dead
        std::vector<float>    pos;        // 3 per vertex
        std::vector<float>    uv;         // 2 per vertex (zeros when the mesh has none)
        std::vector<uint32_t> idx;        // triangle indices
        QVector3D aabbMin, aabbMax;
        QImage    diffuse;                // may be null → flat albedo
    };

    void loop();                          // worker body
    void preparePayload(const Req& req, Prepared& prep);   // CPU: resolve+parse+decode
    bool ensureGL();                      // lazy GL init on the GUI thread
    QImage drawTo(QOpenGLFramebufferObject* fbo, const Prepared& p);  // ctx current
    QImage renderOne(const Prepared& p);  // GUI thread, context already current

    std::shared_ptr<di::DiAssetStore> m_store;
    int m_targetPx = 80;

    // RAM cache of finished pixmaps (GUI thread). Cost = entries; cap sized so
    // total pixmap memory stays in a fixed band regardless of targetPx.
    QCache<uint32_t, QPixmap> m_cache;
    QSet<uint32_t> m_failed;              // GUI thread — dead rows, never retried
    QSet<uint32_t> m_queued;              // guarded by m_mtx

    std::mutex              m_mtx;
    std::condition_variable m_cv;
    std::deque<Req>         m_queue;      // requests awaiting a worker (LIFO front)
    std::deque<Prepared>    m_prepared;   // parsed payloads awaiting GUI render
    bool m_stop = false;
    std::vector<std::thread> m_threads;

    // Offscreen GL — created once, lazily, on the GUI thread.
    bool m_glReady = false;
    bool m_glBad   = false;               // init failed → stop trying, mark dead
    QOffscreenSurface*         m_surface = nullptr;
    QOpenGLContext*            m_ctx     = nullptr;
    QOpenGLShaderProgram*      m_prog    = nullptr;
    QOpenGLFramebufferObject*  m_fbo     = nullptr;
    QOpenGLVertexArrayObject*  m_vao     = nullptr;
    QOpenGLBuffer*             m_vboPos  = nullptr;
    QOpenGLBuffer*             m_vboUV   = nullptr;
    QOpenGLBuffer*             m_ebo     = nullptr;
    int m_uMVP = -1, m_uEye = -1, m_uHasTex = -1;
};
