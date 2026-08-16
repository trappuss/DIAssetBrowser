#pragma once
// ── TEXTURE PREVIEW panel body, shared by the Models and Wardrobe tabs ──────
// One implementation so the two tabs cannot drift (the same reason PanelBox and
// HintBar are shared). The panel has TWO modes and switches on what you select:
//
//   MATERIAL mode — you picked a material (or a model just loaded): the six
//     tiles are the PBR channels the renderer actually consumes,
//     COLOR · ROUGHNESS · METAL · NORMAL · ALPHA · EMISSIVE. Roughness and
//     metal are the mix map's R and G, alpha is the base colour's A, so this
//     view answers "what is this surface made of".
//
//   TEXTURE mode — you picked one texture: the tiles become that single image
//     split into RGB · R · G · B · A. This answers "what is actually stored in
//     this file", which the PBR view cannot show — DI packs unrelated data per
//     channel (the mix map is three different maps in one image), and a packed
//     channel is invisible until you look at it on its own.
//
// Both modes share the hover readout, so the pixel under the cursor always
// reports the values of whatever is currently on screen.

#include <QColor>
#include <QEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

#include "model/MeshTextures.h"

namespace texprev {

// Isolate one byte of every pixel as greyscale (channel 0=R 1=G 2=B 3=A).
inline QImage channelGrey(const QImage& srcAny, int ch)
{
    if (srcAny.isNull()) return {};
    const QImage src = srcAny.convertToFormat(QImage::Format_RGBA8888);
    QImage out(src.width(), src.height(), QImage::Format_RGBA8888);
    for (int y = 0; y < src.height(); ++y) {
        const uchar* s = src.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            const uchar v = s[x * 4 + ch];
            d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = v;
            d[x * 4 + 3] = 255;
        }
    }
    return out;
}

// The colour of a texture with its alpha forced opaque — so a cut-out texture
// reads as its stored RGB rather than as a checkerboard of holes.
inline QImage opaqueRgb(const QImage& srcAny)
{
    if (srcAny.isNull()) return {};
    QImage out = srcAny.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < out.height(); ++y) {
        uchar* d = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x) d[x * 4 + 3] = 255;
    }
    return out;
}

struct Panel {
    static constexpr int kTiles = 6;

    QWidget* box  = nullptr;
    QLabel*  tile[kTiles] = {};
    QLabel*  cap[kTiles]  = {};
    QImage   full[kTiles];               // full-res per tile, for copy/save/hover
    QLabel*  rgba = nullptr;
    int      used = kTiles;              // tiles in use for the current mode
    bool     textureMode = false;
    QString  source;                     // name of what is being shown
    int      tileSide = 64;              // set by build()
    QLabel*  zoom = nullptr;             // hover-zoom popup (owned by `box`)

    static const char* materialName(int i)
    {
        static const char* const n[kTiles] = {"COLOR",  "ROUGHNESS", "METAL",
                                              "NORMAL", "ALPHA",     "EMISSIVE"};
        return (i >= 0 && i < kTiles) ? n[i] : "";
    }
    static const char* textureName(int i)
    {
        // LUMA is the sixth tile because the Textures tab's own strip had it and
        // it earns its place: a mask packed as a grey image reads instantly as
        // luminance and ambiguously as RGB.
        static const char* const n[kTiles] = {"RGB", "RED", "GREEN",
                                              "BLUE", "ALPHA", "LUMA"};
        return (i >= 0 && i < kTiles) ? n[i] : "";
    }
    const char* channelName(int i) const
    {
        return textureMode ? textureName(i) : materialName(i);
    }

    // The panel body. `eventTarget` receives each tile's hover events (pass the
    // owning tab, which forwards them to hover() from its eventFilter).
    // `tilePx` sizes the tiles: the panel-column tabs want 64, the Textures
    // tab's full-width strip wants 72, and that is the only thing that differed
    // between the two hand-written copies of this strip.
    QWidget* build(QWidget* parent, QObject* eventTarget, int tilePx = 64)
    {
        tileSide = std::max(24, tilePx);
        box = new QWidget(parent);
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 2, 0, 2);
        auto* row = new QHBoxLayout();
        row->setSpacing(1);
        row->setContentsMargins(0, 0, 0, 0);
        for (int i = 0; i < kTiles; ++i) {
            auto* t = new QLabel(box);
            t->setFixedSize(tileSide, tileSide);
            t->setAlignment(Qt::AlignCenter);
            t->setStyleSheet(idleQss());
            t->setContextMenuPolicy(Qt::CustomContextMenu);
            auto* c = new QLabel(QLatin1String(materialName(i)), t);
            c->setStyleSheet(QStringLiteral(
                "QLabel{color:#fff;background:rgba(0,0,0,150);padding:0 2px;"
                "font-size:7px;}"));
            c->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            c->move(2, tileSide - 12);
            t->setMouseTracking(true);
            if (eventTarget) t->installEventFilter(eventTarget);
            tile[i] = t;
            cap[i]  = c;
            row->addWidget(t);
        }
        row->addStretch(1);
        v->addLayout(row);
        rgba = new QLabel(QStringLiteral("—"), box);
        rgba->setStyleSheet(QStringLiteral(
            "QLabel{color:#c8c8c8;font-family:monospace;font-size:9px;}"));
        rgba->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(rgba);
        return box;
    }

    static QString idleQss()
    {
        return QStringLiteral("QLabel{border:1px solid #444;background:#1b1b1b;}");
    }

    // ── MATERIAL mode: the PBR channels the shader consumes ────────────────
    void showMaterial(const MeshTextures& t, const QString& name = {})
    {
        textureMode = false;
        source = name;
        used = kTiles;
        const QImage ch[kTiles] = {
            t.diffuse,                    // COLOR
            channelGrey(t.mix, 0),        // ROUGHNESS (mix.R)
            channelGrey(t.mix, 1),        // METAL     (mix.G)
            t.normal,                     // NORMAL
            channelGrey(t.diffuse, 3),    // ALPHA     (base colour's A)
            t.emissive,                   // EMISSIVE
        };
        apply(ch);
    }

    // ── TEXTURE mode: one image, split into its stored channels ────────────
    void showTexture(const QImage& img, const QString& name)
    {
        textureMode = true;
        source = name;
        used = kTiles;                    // RGB · R · G · B · A · LUMA
        const QImage ch[kTiles] = {
            opaqueRgb(img),
            channelGrey(img, 0),
            channelGrey(img, 1),
            channelGrey(img, 2),
            channelGrey(img, 3),
            img.isNull() ? QImage()
                         : img.convertToFormat(QImage::Format_Grayscale8),
        };
        apply(ch);
    }

    void clear()
    {
        textureMode = false;
        source.clear();
        used = kTiles;
        if (zoom) zoom->hide();
        const QImage empty[kTiles];
        apply(empty);
    }

    // Title for the PanelBox header, so the panel says what it is showing.
    QString title() const
    {
        if (source.isEmpty()) return QStringLiteral("TEXTURE PREVIEW");
        return QStringLiteral("TEXTURE PREVIEW · %1%2")
            .arg(textureMode ? QStringLiteral("RGBA ") : QString(), source);
    }

    int indexOf(const QObject* obj) const
    {
        for (int i = 0; i < kTiles; ++i)
            if (tile[i] == obj) return i;
        return -1;
    }

    // Hover → report the pixel under the cursor. Returns true when `obj` was one
    // of our tiles (so the caller knows the event was ours).
    bool hover(QObject* obj, QEvent* ev)
    {
        const int i = indexOf(obj);
        if (i < 0) return false;
        if (ev->type() == QEvent::Leave) {
            if (rgba) rgba->setText(QStringLiteral("—"));
            if (zoom) zoom->hide();
            return true;
        }
        // Hover-zoom. A 64px tile is enough to tell you a channel is not empty
        // and nothing more; this is what makes it readable. The Textures tab had
        // it and the panel-column tabs did not, purely because the strip was
        // written twice.
        if (ev->type() == QEvent::Enter) showZoom(i);
        if (ev->type() != QEvent::MouseMove) return true;
        if (!rgba || full[i].isNull() || !tile[i]) return true;
        // Map through the pixmap as actually drawn (aspect-fit, centred), not the
        // label rect, or the reading is off by the letterbox margin.
        const QPixmap pm = tile[i]->pixmap();
        if (pm.isNull()) return true;
        const QPoint p = static_cast<QMouseEvent*>(ev)->pos();
        const int offX = (tile[i]->width()  - pm.width())  / 2;
        const int offY = (tile[i]->height() - pm.height()) / 2;
        if (p.x() < offX || p.y() < offY ||
            p.x() >= offX + pm.width() || p.y() >= offY + pm.height()) {
            rgba->setText(QStringLiteral("—"));
            return true;
        }
        const QImage& img = full[i];
        const int x = std::clamp((p.x() - offX) * img.width()  / pm.width(),
                                 0, img.width()  - 1);
        const int y = std::clamp((p.y() - offY) * img.height() / pm.height(),
                                 0, img.height() - 1);
        const QColor c = img.pixelColor(x, y);
        rgba->setText(QStringLiteral("%1  %2,%3  R%4 G%5 B%6 A%7")
                          .arg(QLatin1String(channelName(i)))
                          .arg(x).arg(y)
                          .arg(c.red(), 3).arg(c.green(), 3)
                          .arg(c.blue(), 3).arg(c.alpha(), 3));
        return true;
    }

    // Briefly ring one tile so the eye finds a routed channel.
    void highlight(int i)
    {
        for (int k = 0; k < kTiles; ++k) {
            if (!tile[k]) continue;
            tile[k]->setStyleSheet(
                k == i ? QStringLiteral(
                             "QLabel{border:2px solid #6ea0ff;background:#1b1b1b;}")
                       : idleQss());
        }
    }

private:
    void showZoom(int i)
    {
        if (i < 0 || i >= kTiles || full[i].isNull() || !tile[i] || !box) return;
        if (!zoom) {
            // Parented to `box` so it is destroyed with the panel; Qt::ToolTip
            // still makes it a borderless top-level that can overhang the panel.
            zoom = new QLabel(box, Qt::ToolTip);
            // Never let a hover preview eat a mouse click — a lingering
            // Qt::ToolTip window otherwise swallows the first click meant for
            // something else, and this one is large enough to sit over the
            // Textures tab's main preview area.
            zoom->setAttribute(Qt::WA_TransparentForMouseEvents);
            zoom->setAttribute(Qt::WA_ShowWithoutActivating);
            zoom->setStyleSheet(
                QStringLiteral("QLabel{border:1px solid #666;background:#111;}"));
        }
        constexpr int kZoomPx = 320;
        zoom->setPixmap(QPixmap::fromImage(full[i].scaled(
            kZoomPx, kZoomPx, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        zoom->adjustSize();
        // Above the tile by preference, to its LEFT when there is no room —
        // which is the normal case for the panel-column tabs, where the strip
        // sits near the top of a right-hand column. Then clamped onto the
        // screen, so a tile near an edge cannot push the popup off it.
        const QPoint anchor = tile[i]->mapToGlobal(QPoint(0, 0));
        QPoint at(anchor.x(), anchor.y() - zoom->height() - 4);
        QRect screen;
        if (QScreen* sc = tile[i]->screen()) screen = sc->availableGeometry();
        if (screen.isValid() && at.y() < screen.top())
            at = QPoint(anchor.x() - zoom->width() - 4, anchor.y());
        if (screen.isValid()) {
            at.setX(std::min(std::max(at.x(), screen.left()),
                             screen.right() - zoom->width()));
            at.setY(std::min(std::max(at.y(), screen.top()),
                             screen.bottom() - zoom->height()));
        }
        zoom->move(at);
        zoom->show();
    }

    void apply(const QImage (&ch)[kTiles])
    {
        if (rgba) rgba->setText(QStringLiteral("—"));
        for (int i = 0; i < kTiles; ++i) {
            full[i] = ch[i];
            if (!tile[i]) continue;
            const bool inUse = i < used;
            tile[i]->setVisible(inUse);
            if (cap[i]) cap[i]->setText(QLatin1String(channelName(i)));
            tile[i]->setStyleSheet(idleQss());
            if (!inUse || ch[i].isNull()) {
                tile[i]->setPixmap(QPixmap());
                tile[i]->setText(inUse ? QStringLiteral("—") : QString());
                continue;
            }
            tile[i]->setText(QString());
            const int side = std::max(8, tile[i]->width() - 6);
            tile[i]->setPixmap(QPixmap::fromImage(
                ch[i].scaled(side, side, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation)));
        }
    }
};

}   // namespace texprev
