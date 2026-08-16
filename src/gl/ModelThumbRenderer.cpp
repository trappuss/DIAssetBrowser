#include "gl/ModelThumbRenderer.h"

#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QSurfaceFormat>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

#include "app/SehGuard.h"
#include "model/MeshParser.h"
#include "model/ModelResolve.h"
#include "tex/TextureDecode.h"
#include "tex/TextureParser.h"

namespace {
constexpr size_t kQueueCap    = 384;   // fast scroll drops the oldest requests
constexpr size_t kPreparedCap = 48;    // bound parsed-payload RAM before render
constexpr int    kWorkers     = 2;
constexpr int    kBatch       = 6;     // FBO renders per commit() — frame-smooth

// Flat-shaded from screen-space derivatives, so a mesh with no normal stream
// still lights correctly; headlight + soft ambient reads well at icon size.
const char* kVert = R"(#version 450 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
uniform mat4 uMVP;
out vec3 vWorld;
out vec2 vUV;
void main(){ vWorld = aPos; vUV = aUV; gl_Position = uMVP * vec4(aPos, 1.0); }
)";

const char* kFrag = R"(#version 450 core
in vec3 vWorld;
in vec2 vUV;
uniform sampler2D uTex;
uniform int  uHasTex;
uniform vec3 uEye;
out vec4 frag;
void main(){
    vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    vec3 v = normalize(uEye - vWorld);
    if (dot(n, v) < 0.0) n = -n;                 // always face the camera
    float l = 0.28 + 0.72 * clamp(dot(n, v), 0.0, 1.0);
    vec3 albedo = (uHasTex == 1) ? texture(uTex, vUV).rgb : vec3(0.72, 0.72, 0.74);
    frag = vec4(albedo * l, 1.0);
}
)";

// Smallest mip that is still >= want on both axes, else the largest available.
// Matches ThumbnailProvider (this store keeps mips smallest-first).
QImage decodeDiffuse(const di::DiAssetStore& store, size_t blob, int want)
{
    if (blob == (size_t)-1) return {};
    const std::vector<uint8_t> raw = store.mpk().readAsset(blob);
    if (raw.empty()) return {};
    di::Texture2D tex;
    std::string err;
    if (!di::isTexture2D(raw.data(), raw.size()) ||
        !di::parseTexture2D(raw.data(), raw.size(), &tex, &err))
        return {};
    int best = -1, bestArea = -1, largest = -1, largestArea = -1;
    for (int i = 0; i < (int)tex.mips.size(); ++i) {
        const int w = (int)tex.mips[i].width, h = (int)tex.mips[i].height;
        const int area = w * h;
        if (area > largestArea) { largestArea = area; largest = i; }
        if (w >= want && h >= want && (best < 0 || area < bestArea)) { best = i; bestArea = area; }
    }
    const int pick = best >= 0 ? best : largest;
    if (pick < 0) return {};
    return TextureDecode::decode(tex, pick).image;
}
} // namespace

ModelThumbRenderer::ModelThumbRenderer(std::shared_ptr<di::DiAssetStore> store,
                                       int targetPx, QObject* parent)
    : QObject(parent), m_store(std::move(store)),
      m_targetPx(targetPx > 0 ? targetPx : 80)
{
    // Fixed memory band across icon sizes: ~1000 entries at 80px ~= 26 MB.
    const int cap = qMax(256, 4096 * 40 * 40 / (m_targetPx * m_targetPx));
    m_cache.setMaxCost(cap);
    for (int i = 0; i < kWorkers; ++i)
        m_threads.emplace_back([this] { loop(); });
}

ModelThumbRenderer::~ModelThumbRenderer()
{
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_threads) t.join();

    // Tear down GL on the GUI thread (the only thread that ever made it current).
    if (m_glReady && m_ctx && m_surface && m_ctx->makeCurrent(m_surface)) {
        delete m_fbo;     m_fbo = nullptr;
        delete m_prog;    m_prog = nullptr;
        if (m_vboPos) { m_vboPos->destroy(); delete m_vboPos; m_vboPos = nullptr; }
        if (m_vboUV)  { m_vboUV->destroy();  delete m_vboUV;  m_vboUV = nullptr; }
        if (m_ebo)    { m_ebo->destroy();    delete m_ebo;    m_ebo = nullptr; }
        if (m_vao)    { m_vao->destroy();    delete m_vao;    m_vao = nullptr; }
        m_ctx->doneCurrent();
    }
    delete m_ctx;     m_ctx = nullptr;
    delete m_surface; m_surface = nullptr;
}

bool ModelThumbRenderer::peek(uint32_t entryId, QPixmap* out) const
{
    if (QPixmap* pm = m_cache.object(entryId)) { *out = *pm; return true; }
    return false;
}

bool ModelThumbRenderer::get(const AssetRow& row, QPixmap* out)
{
    if (QPixmap* pm = m_cache.object(row.entryId)) { *out = *pm; return true; }
    if (m_glBad || m_failed.contains(row.entryId)) return false;
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

void ModelThumbRenderer::preparePayload(const Req& req, Prepared& prep)
{
    prep.entryId = req.entryId;
    seh::runGuarded("thumb3d", [&] {
        const di::ResolvedModel res = di::resolveModelChain(*m_store, req.repoIdx);
        if (res.meshBlob == (size_t)-1) return;
        const std::vector<uint8_t> raw = m_store->mpk().readAsset(res.meshBlob);
        if (raw.empty()) return;
        di::MeshData mesh;
        std::string err;
        if (!di::parseMesh(raw.data(), raw.size(), &mesh, &err)) return;
        if (mesh.positions.empty() || mesh.indices.empty()) return;   // nothing to draw

        const size_t vc = mesh.vertexCount();
        prep.pos = std::move(mesh.positions);
        prep.idx = std::move(mesh.indices);
        if (mesh.uv0.size() == vc * 2) prep.uv = std::move(mesh.uv0);
        else                           prep.uv.assign(vc * 2, 0.0f);

        // AABB for camera framing.
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t i = 0; i + 2 < prep.pos.size(); i += 3)
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], prep.pos[i + k]);
                mx[k] = std::max(mx[k], prep.pos[i + k]);
            }
        prep.aabbMin = QVector3D(mn[0], mn[1], mn[2]);
        prep.aabbMax = QVector3D(mx[0], mx[1], mx[2]);

        prep.diffuse = decodeDiffuse(*m_store, res.texBlob, m_targetPx);
        prep.ok = true;
    });
}

void ModelThumbRenderer::loop()
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

        Prepared prep;
        preparePayload(req, prep);

        bool emitReady = false;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_queued.remove(req.entryId);
            emitReady = m_prepared.empty();
            m_prepared.push_back(std::move(prep));
            while (m_prepared.size() > kPreparedCap) m_prepared.pop_front();
        }
        if (emitReady) emit ready();   // one ready() per burst
    }
}

bool ModelThumbRenderer::ensureGL()
{
    if (m_glBad) return false;
    if (m_glReady) return m_ctx->makeCurrent(m_surface);

    m_surface = new QOffscreenSurface;
    m_surface->setFormat(QSurfaceFormat::defaultFormat());
    m_surface->create();
    if (!m_surface->isValid()) { qWarning("thumb3d: offscreen surface invalid"); m_glBad = true; return false; }

    m_ctx = new QOpenGLContext;
    m_ctx->setFormat(QSurfaceFormat::defaultFormat());
    if (QOpenGLContext* sh = QOpenGLContext::globalShareContext()) m_ctx->setShareContext(sh);
    if (!m_ctx->create() || !m_ctx->makeCurrent(m_surface)) {
        qWarning("thumb3d: GL context create/makeCurrent failed"); m_glBad = true; return false;
    }

    const int rp = m_targetPx * 2;   // 2x supersample, downscaled for cheap AA
    QOpenGLFramebufferObjectFormat ff;
    ff.setAttachment(QOpenGLFramebufferObject::Depth);
    m_fbo = new QOpenGLFramebufferObject(rp, rp, ff);

    m_prog = new QOpenGLShaderProgram;
    m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert);
    m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag);
    m_prog->bindAttributeLocation("aPos", 0);
    m_prog->bindAttributeLocation("aUV", 1);
    if (!m_prog->link()) {
        qWarning("thumb3d: shader link failed: %s", qPrintable(m_prog->log()));
        m_glBad = true; m_ctx->doneCurrent(); return false;
    }
    m_uMVP    = m_prog->uniformLocation("uMVP");
    m_uEye    = m_prog->uniformLocation("uEye");
    m_uHasTex = m_prog->uniformLocation("uHasTex");

    m_vao = new QOpenGLVertexArrayObject; m_vao->create();
    m_vboPos = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer); m_vboPos->create();
    m_vboUV  = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer); m_vboUV->create();
    m_ebo    = new QOpenGLBuffer(QOpenGLBuffer::IndexBuffer);  m_ebo->create();

    m_glReady = true;
    return true;   // leaves the context current
}

QImage ModelThumbRenderer::drawTo(QOpenGLFramebufferObject* fbo, const Prepared& p)
{
    QOpenGLFunctions* f = m_ctx->functions();
    if (!fbo->bind()) return {};
    const int rp = fbo->width();
    f->glViewport(0, 0, rp, rp);
    f->glEnable(GL_DEPTH_TEST);
    f->glDisable(GL_CULL_FACE);
    // Transparent background: clear alpha to 0 so only the model's pixels (which
    // the fragment shader writes at alpha 1) are opaque. The icon then sits on the
    // row/grid background instead of a grey box.
    f->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Frame-to-fit: fit the model's PROJECTED bounding box to the square, so a
    // model fills the icon regardless of shape (the old bounding-sphere fit left
    // flat / wide pieces as thin bands). Slightly elevated 3/4 pose.
    const QVector3D c = (p.aabbMin + p.aabbMax) * 0.5f;
    const float yaw = 0.6f, pitch = 0.45f;
    const QVector3D off(std::cos(pitch) * std::sin(yaw),
                        std::sin(pitch),
                        std::cos(pitch) * std::cos(yaw));
    const QVector3D fwd = -off;                                   // eye → center
    QVector3D right = QVector3D::crossProduct(fwd, QVector3D(0, 1, 0));
    if (right.lengthSquared() < 1e-6f) right = QVector3D(1, 0, 0);
    right.normalize();
    const QVector3D upv = QVector3D::crossProduct(right, fwd).normalized();
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;                        // view-space half-extents
    for (int ci = 0; ci < 8; ++ci) {
        const QVector3D corner((ci & 1) ? p.aabbMax.x() : p.aabbMin.x(),
                               (ci & 2) ? p.aabbMax.y() : p.aabbMin.y(),
                               (ci & 4) ? p.aabbMax.z() : p.aabbMin.z());
        const QVector3D rel = corner - c;
        hx = std::max(hx, std::abs(QVector3D::dotProduct(rel, right)));
        hy = std::max(hy, std::abs(QVector3D::dotProduct(rel, upv)));
        hz = std::max(hz, std::abs(QVector3D::dotProduct(rel, fwd)));
    }
    constexpr float kFovY = 45.0f;
    constexpr float kTanHalf = 0.41421356f;                      // tan(22.5°), aspect 1
    const float need = std::max(hx, hy) / kTanHalf;              // distance to fit the wider axis
    const float dist = std::max(0.1f, need + hz) * 1.12f;        // + object depth + padding
    const QVector3D eye = c + off * dist;
    QMatrix4x4 proj;
    proj.perspective(kFovY, 1.0f, std::max(0.001f, (dist - hz) * 0.5f),
                     (dist + hz) * 1.5f + 1.0f);
    QMatrix4x4 view;
    view.lookAt(eye, c, QVector3D(0, 1, 0));
    const QMatrix4x4 mvp = proj * view;

    m_prog->bind();
    m_prog->setUniformValue(m_uMVP, mvp);
    m_prog->setUniformValue(m_uEye, eye);

    std::unique_ptr<QOpenGLTexture> tex;
    if (!p.diffuse.isNull()) {
        tex = std::make_unique<QOpenGLTexture>(
            p.diffuse.convertToFormat(QImage::Format_RGBA8888),
            QOpenGLTexture::DontGenerateMipMaps);
        tex->setMinificationFilter(QOpenGLTexture::Linear);
        tex->setMagnificationFilter(QOpenGLTexture::Linear);
        tex->bind(0);
        m_prog->setUniformValue(m_uHasTex, 1);
        m_prog->setUniformValue("uTex", 0);
    } else {
        m_prog->setUniformValue(m_uHasTex, 0);
    }

    m_vao->bind();
    m_vboPos->bind();
    m_vboPos->allocate(p.pos.data(), int(p.pos.size() * sizeof(float)));
    m_prog->enableAttributeArray(0);
    m_prog->setAttributeBuffer(0, GL_FLOAT, 0, 3);
    m_vboUV->bind();
    m_vboUV->allocate(p.uv.data(), int(p.uv.size() * sizeof(float)));
    m_prog->enableAttributeArray(1);
    m_prog->setAttributeBuffer(1, GL_FLOAT, 0, 2);
    m_ebo->bind();
    m_ebo->allocate(p.idx.data(), int(p.idx.size() * sizeof(uint32_t)));
    f->glDrawElements(GL_TRIANGLES, int(p.idx.size()), GL_UNSIGNED_INT, nullptr);
    m_vao->release();
    m_prog->release();

    QImage img = fbo->toImage();
    fbo->release();
    if (tex) tex->destroy();
    return img;   // full render resolution; callers downscale to their target
}

QImage ModelThumbRenderer::renderOne(const Prepared& p)
{
    const QImage full = drawTo(m_fbo, p);
    if (full.isNull()) return {};
    return full.scaled(m_targetPx, m_targetPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

bool ModelThumbRenderer::renderPreview(const AssetRow& row, int px, QImage* out)
{
    if (m_glBad || px <= 0) return false;
    Prepared p;
    preparePayload(Req{row.entryId, row.repoIdx, row.type}, p);
    if (!p.ok) return false;

    QOpenGLContext* prev = QOpenGLContext::currentContext();
    QSurface* prevSurf = prev ? prev->surface() : nullptr;
    if (!ensureGL()) {
        if (prev && prevSurf) prev->makeCurrent(prevSurf);
        return false;
    }
    QImage full;
    {
        const int rp = px * 2;   // 2x supersample for a clean hover image
        QOpenGLFramebufferObjectFormat ff;
        ff.setAttachment(QOpenGLFramebufferObject::Depth);
        QOpenGLFramebufferObject fbo(rp, rp, ff);
        seh::runGuarded("thumb3d-preview", [&] { full = drawTo(&fbo, p); });
    }
    m_ctx->doneCurrent();
    if (prev && prevSurf) prev->makeCurrent(prevSurf);

    if (full.isNull()) return false;
    *out = full.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return true;
}

void ModelThumbRenderer::commit()
{
    if (m_glBad) {   // GL unavailable — retire everything so rows stop retrying
        std::deque<Prepared> dead;
        { std::lock_guard<std::mutex> lock(m_mtx); dead.swap(m_prepared); }
        for (const Prepared& p : dead) m_failed.insert(p.entryId);
        return;
    }

    std::vector<Prepared> batch;
    bool more = false;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        const int n = (int)std::min<size_t>(kBatch, m_prepared.size());
        for (int i = 0; i < n; ++i) { batch.push_back(std::move(m_prepared.front())); m_prepared.pop_front(); }
        more = !m_prepared.empty();
    }
    if (batch.empty()) { if (more) QTimer::singleShot(0, this, [this] { emit ready(); }); return; }

    QOpenGLContext* prev = QOpenGLContext::currentContext();
    QSurface* prevSurf = prev ? prev->surface() : nullptr;

    if (!ensureGL()) {                 // may flip m_glBad
        for (const Prepared& p : batch) m_failed.insert(p.entryId);
        if (prev && prevSurf) prev->makeCurrent(prevSurf);
        QTimer::singleShot(0, this, [this] { emit ready(); });   // let commit() drain the rest as failed
        return;
    }

    for (const Prepared& p : batch) {
        if (!p.ok) { m_failed.insert(p.entryId); continue; }
        QImage img;
        seh::runGuarded("thumb3d-render", [&] { img = renderOne(p); });
        if (img.isNull()) m_failed.insert(p.entryId);
        else { m_cache.insert(p.entryId, new QPixmap(QPixmap::fromImage(img))); m_failed.remove(p.entryId); }
    }

    m_ctx->doneCurrent();
    if (prev && prevSurf) prev->makeCurrent(prevSurf);

    if (more) QTimer::singleShot(0, this, [this] { emit ready(); });
}
