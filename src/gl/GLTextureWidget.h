#pragma once
// Ported 1:1 from D4AssetBrowser Native (src/gl/GLTextureWidget.{h,cpp}),
// retrofitted to DI's pixel formats. Uploads a compressed BC texture payload
// straight to the GPU with glCompressedTexImage2D and draws it aspect-fit — no
// CPU decode on the hot path. Channel isolation, alpha checkerboard, zoom/pan,
// and a full-resolution grabImage() readback are all in-shader, exactly as D4.
//
// Retrofit: the format argument is DI's EPixelFormat byte (BC1=18…BC7=25); the
// GL internal format + block size come from DiPixelFormat::codec, and DI mips
// are stored TIGHTLY (no D3D12 256-byte row pitch), so there is no aligned-row
// padding to crop (uUmax stays 1.0). The CPU fallback uses DI's BcDecode.

#include <QByteArray>
#include <QImage>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>

class GLTextureWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core {
    Q_OBJECT
public:
    explicit GLTextureWidget(QWidget* parent = nullptr);
    ~GLTextureWidget() override;

    // Upload one mip of a BC payload. diFormat is DI's EPixelFormat byte; width/
    // height are that mip's real dimensions. Safe to call from the GUI thread.
    void setTexture(const QByteArray& bcData, int width, int height, int diFormat);
    void clearTexture();

    // Render the uploaded texture to an offscreen FBO at full resolution and read
    // it back as a QImage (the GPU does the BC decode). Null if nothing is loaded.
    QImage grabImage();
    bool hasTexture() const { return m_ready; }

    void setCheckerboard(bool on);   // show a checker behind transparent pixels
    void resetView();                // zoom 1, no pan

signals:
    // Texture-space UV (0..1) under the cursor, or (-1,-1) when outside the image.
    void hoverUv(QPointF uv);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    void uploadPending();
    void destroyTexture();

    // pending upload (applied on the next paintGL with a current context)
    bool       m_hasPending = false;
    QByteArray m_data;
    int        m_w = 0, m_h = 0, m_fmt = 0;

    // GL objects / state
    GLuint  m_prog = 0, m_vao = 0, m_vbo = 0, m_tex = 0;
    int     m_texW = 0, m_texH = 0;   // actual dimensions for aspect fit
    int     m_channels = 0;           // 0=RGBA, 1=BC4 (R→grey), 2=BC5 (RG)
    float   m_umax = 1.0f;            // DI: always 1.0 (tight mips, no row padding)
    bool    m_ready = false;
    QString m_error;

    // View transform (zoom/pan) + alpha checkerboard.
    float   m_zoom = 1.0f;
    float   m_panX = 0.0f, m_panY = 0.0f;   // NDC offset
    bool    m_checker = false;
    bool    m_dragging = false;
    QPoint  m_lastPos;

    // Map a widget point to texture UV (0..1), honoring aspect-fit + zoom/pan.
    bool widgetToUv(const QPoint& p, float& u, float& v) const;
};
