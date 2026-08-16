#pragma once
// Painter-drawn viewport-toolbar glyphs, ported 1:1 from D4AssetBrowser Native
// (src/tabs/ViewGlyphs.h). Blender-style shading spheres + the two-circle
// Overlays toggle. Header-only. (The D4 N-strip glyphs that reused the outliner
// icon set are omitted here — DI's viewport toolbar doesn't have that strip yet.)

#include <QColor>
#include <QPainter>

#include <cmath>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRadialGradient>
#include <QRectF>

// Blender's shading spheres — 0 wireframe · 1 flat/solid · 2 shaded · 3 rendered.
// 20px canvas: at bar height anything smaller reads as a featureless dot.
inline QPixmap shadeBallGlyph(int mode)
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(10.0, 10.0);
    const double r = 8.4;
    if (mode == 0) {          // wire sphere: outline + equator + meridian
        p.setPen(QPen(QColor(210, 200, 165), 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r, r);
        p.drawEllipse(c, r, r * 0.42);
        p.drawEllipse(c, r * 0.42, r);
    } else if (mode == 1) {   // flat: plain matte ball
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xa8, 0xa8, 0xa8));
        p.drawEllipse(c, r, r);
    } else if (mode == 2) {   // shaded: soft-lit ball
        QRadialGradient g(QPointF(7.0, 6.8), 13.0);
        g.setColorAt(0.0, QColor(0xe4, 0xe4, 0xe4));
        g.setColorAt(1.0, QColor(0x4a, 0x4a, 0x50));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(c, r, r);
    } else {                  // rendered: warm glossy ball + specular hit
        QRadialGradient g(QPointF(7.0, 6.8), 13.0);
        g.setColorAt(0.0, QColor(0xff, 0xf4, 0xd8));
        g.setColorAt(0.45, QColor(0xe0, 0xa8, 0x3e));
        g.setColorAt(1.0, QColor(0x4a, 0x33, 0x18));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(c, r, r);
        p.setBrush(QColor(255, 255, 255, 235));
        p.drawEllipse(QPointF(7.0, 6.4), 1.9, 1.9);
    }
    return pm;
}

// Right-column panel-strip icons — a small distinct glyph per panel kind, chosen
// by a keyword in the panel title so the strip reads as a toolbar of icons (D4's
// N-strip) rather than bare letters. 18px canvas, light warm stroke.
inline QPixmap panelGlyph(const QString& title)
{
    QPixmap pm(18, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(206, 198, 168);
    p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const QString t = title.toUpper();
    if (t.contains(QStringLiteral("PART"))) {            // 2x2 grid of tiles
        p.setBrush(ink);
        p.setPen(Qt::NoPen);
        for (int gx = 0; gx < 2; ++gx)
            for (int gy = 0; gy < 2; ++gy)
                p.drawRoundedRect(QRectF(3 + gx * 7, 3 + gy * 7, 5, 5), 1, 1);
    } else if (t.contains(QStringLiteral("ATTACH"))) {   // chain link
        p.drawRoundedRect(QRectF(3, 6.5, 7, 5), 2.5, 2.5);
        p.drawRoundedRect(QRectF(8, 6.5, 7, 5), 2.5, 2.5);
    } else if (t.contains(QStringLiteral("ANIM"))) {     // play triangle
        p.setBrush(ink);
        p.setPen(Qt::NoPen);
        QPolygonF tri;
        tri << QPointF(6, 4) << QPointF(14, 9) << QPointF(6, 14);
        p.drawPolygon(tri);
    } else if (t.contains(QStringLiteral("FRAME"))) {    // overlapping frames
        p.drawRect(QRectF(3, 3, 8, 8));
        p.drawRect(QRectF(7, 7, 8, 8));
    } else if (t.contains(QStringLiteral("MODEL"))) {    // little cube
        p.drawRect(QRectF(4, 6, 7, 7));
        p.drawLine(QPointF(4, 6), QPointF(7, 3));
        p.drawLine(QPointF(11, 6), QPointF(14, 3));
        p.drawLine(QPointF(7, 3), QPointF(14, 3));
        p.drawLine(QPointF(14, 3), QPointF(14, 10));
        p.drawLine(QPointF(11, 13), QPointF(14, 10));
    } else if (t.contains(QStringLiteral("MIP"))) {      // descending stack
        for (int i = 0; i < 3; ++i)
            p.drawRect(QRectF(3, 3 + i * 5, 11 - i * 3, 3));
    } else {                                             // list lines (default)
        for (int y : {5, 9, 13}) {
            p.drawEllipse(QPointF(3.5, y), 1.0, 1.0);
            p.drawLine(QPointF(6.5, y), QPointF(14.5, y));
        }
    }
    return pm;
}

// Browse-view mode glyphs for the header dropdown — 0 List · 1 Grid · 2 Outliner
// (D4 puts view style in one dropdown, not a row of buttons). 16px.
inline QPixmap viewModeGlyph(int mode)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(206, 198, 168);
    p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (mode == 0) {                    // List: three full-width rows
        for (int y : {3, 8, 13}) p.drawLine(QPointF(2, y), QPointF(14, y));
    } else if (mode == 1) {             // Grid: 2x2 tiles
        p.setBrush(ink);
        p.setPen(Qt::NoPen);
        for (int gx = 0; gx < 2; ++gx)
            for (int gy = 0; gy < 2; ++gy)
                p.drawRoundedRect(QRectF(2 + gx * 7, 2 + gy * 7, 5, 5), 1, 1);
    } else {                            // Outliner: trunk with indented branches
        p.drawLine(QPointF(3, 2), QPointF(3, 14));
        for (int y : {4, 8, 12}) p.drawLine(QPointF(3, y), QPointF(13, y));
    }
    return pm;
}

// Viewport N-strip icons — Camera / Lighting / Physics for the popover buttons on
// the viewport's right edge. Drawn 1:1 from D4AssetBrowser's stripGlyph (cases
// 2 Camera · 3 Lighting-sun · 7 Physics-spring), on D4's 14px canvas so the
// strokes land identically.
inline QPixmap cameraGlyph()
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c(200, 190, 150);
    p.setPen(QPen(c, 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(1.5, 4.0, 11.0, 7.5), 1.5, 1.5);   // body
    p.drawEllipse(QPointF(7.0, 7.8), 2.3, 2.3);                 // lens
    p.drawLine(QPointF(4.0, 4.0), QPointF(5.5, 2.0));           // viewfinder hump
    p.drawLine(QPointF(5.5, 2.0), QPointF(8.5, 2.0));
    p.drawLine(QPointF(8.5, 2.0), QPointF(10.0, 4.0));
    return pm;
}
inline QPixmap lightGlyph()
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(230, 200, 110), 1.3));   // sun
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(7, 7), 3.0, 3.0);
    for (int i = 0; i < 8; ++i) {
        const double a = i * 3.14159265 / 4.0;
        p.drawLine(QPointF(7 + 4.4 * std::cos(a), 7 + 4.4 * std::sin(a)),
                   QPointF(7 + 6.2 * std::cos(a), 7 + 6.2 * std::sin(a)));
    }
    return pm;
}

// Physics / soft-body icon for the N-strip — D4's spring wave (stripGlyph k=7).
inline QPixmap clothGlyph()
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(120, 190, 220), 1.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    QPainterPath w;
    w.moveTo(1.5, 7.0);
    w.cubicTo(4.0, 1.5, 6.0, 12.5, 8.5, 7.0);
    w.cubicTo(10.0, 3.5, 11.5, 10.5, 12.5, 7.0);
    p.drawPath(w);
    return pm;
}

// Padlock for the Wardrobe's per-slot pin. Two states because a checkable
// button's pressed colour alone is a weak signal on a 22px square: the OPEN
// lock has its shackle offset and lifted, the CLOSED one sits square on the
// body, so the state is readable from the silhouette at a glance.
inline QPixmap lockGlyph(bool closed)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor tint = closed ? QColor(0xe8, 0xd8, 0xa8) : QColor(0x9a, 0x9a, 0x9a);
    // shackle: a half-circle over the body, hinged left when open
    p.setPen(QPen(tint, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    const qreal cx = closed ? 7.0 : 8.6;
    const qreal top = closed ? 5.4 : 4.4;
    QPainterPath sh;
    sh.moveTo(cx - 2.6, top + 1.4);
    sh.arcTo(QRectF(cx - 2.6, top - 1.2, 5.2, 5.2), 180.0, -180.0);
    p.drawPath(sh);
    // body
    p.setPen(QPen(tint.darker(140), 1.0));
    p.setBrush(tint);
    p.drawRoundedRect(QRectF(2.5, 6.6, 9.0, 6.4), 1.6, 1.6);
    // keyhole
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x1b, 0x1b, 0x1b));
    p.drawEllipse(QPointF(7.0, 9.2), 1.15, 1.15);
    p.drawRect(QRectF(6.5, 9.2, 1.0, 2.4));
    return pm;
}

// Blender's Overlays toggle: two overlapping circles (20px to match the shading balls).
inline QPixmap overlayGlyph()
{
    QPixmap g(20, 20);
    g.fill(Qt::transparent);
    QPainter p(&g);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(215, 210, 195), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(7.6, 10.0), 5.6, 5.6);
    p.drawEllipse(QPointF(12.4, 10.0), 5.6, 5.6);
    return g;
}
