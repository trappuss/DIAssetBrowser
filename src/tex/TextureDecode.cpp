#include "tex/TextureDecode.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "tex/BcDecode.h"
#include "tex/DiPixelFormat.h"

namespace {

QImage decodeUncompressed(const di::TextureMip& m, const DiPixelFormat::Codec& c)
{
    const qint64 need = qint64(m.width) * m.height * c.bytesPerPixel;
    if ((qint64)m.data.size() < need) return {};
    QImage img(m.width, m.height, QImage::Format_RGBA8888);
    const uint8_t* src = m.data.data();
    for (int y = 0; y < m.height; ++y) {
        quint8* line = img.scanLine(y);
        for (int x = 0; x < m.width; ++x) {
            const uint8_t* p = src + (qint64(y) * m.width + x) * c.bytesPerPixel;
            quint8* o = line + x * 4;
            switch (c.kind) {
                case DiPixelFormat::K_RGBA8: o[0]=p[0]; o[1]=p[1]; o[2]=p[2]; o[3]=p[3]; break;
                case DiPixelFormat::K_L8:    o[0]=o[1]=o[2]=p[0]; o[3]=255;              break;
                case DiPixelFormat::K_A8:    o[0]=o[1]=o[2]=255;  o[3]=p[0];             break;
                default:                     o[0]=o[1]=o[2]=0;    o[3]=255;              break;
            }
        }
    }
    return img;
}

QImage decodeMip(const di::TextureMip& m, int format)
{
    const DiPixelFormat::Codec c = DiPixelFormat::codec(format);
    switch (c.kind) {
        case DiPixelFormat::K_BC1: case DiPixelFormat::K_BC2: case DiPixelFormat::K_BC3:
        case DiPixelFormat::K_BC4: case DiPixelFormat::K_BC5: case DiPixelFormat::K_BC7:
            return BcDecode::decode(m.data.data(), m.data.size(), m.width, m.height,
                                    format, m.pitchInByte);
        case DiPixelFormat::K_RGBA8: case DiPixelFormat::K_L8: case DiPixelFormat::K_A8:
            return decodeUncompressed(m, c);
        default:
            return {};   // BcDecode-style warn already covers block formats; the
                         // caller reports the format name for the rest
    }
}

// Cheap content check: sampled per-channel spread. Near-constant output is the
// known bad-decode shape for some high mips (seen on the Python tool).
bool hasContent(const QImage& img)
{
    if (img.isNull()) return false;
    // All four channels: an A8 mask decodes with constant RGB and its content
    // living entirely in alpha — RGB-only spread misjudged every one of those.
    int minv[4] = {255, 255, 255, 255}, maxv[4] = {0, 0, 0, 0};
    const int stepY = std::max(1, img.height() / 64);
    const int stepX = std::max(1, img.width() / 64);
    for (int y = 0; y < img.height(); y += stepY) {
        const quint8* line = img.constScanLine(y);
        for (int x = 0; x < img.width(); x += stepX) {
            const quint8* p = line + x * 4;
            for (int c = 0; c < 4; ++c) {
                minv[c] = std::min<int>(minv[c], p[c]);
                maxv[c] = std::max<int>(maxv[c], p[c]);
            }
        }
    }
    return (maxv[0] - minv[0]) + (maxv[1] - minv[1]) + (maxv[2] - minv[2]) +
           (maxv[3] - minv[3]) > 24;
}

} // namespace

TextureDecode::Result TextureDecode::decode(const di::Texture2D& tex, int mipIndex)
{
    Result res;
    res.formatName = DiPixelFormat::name(tex.format);
    if (tex.mips.empty()) {
        res.error = QStringLiteral("no mips");
        return res;
    }

    if (mipIndex >= 0 && mipIndex < (int)tex.mips.size()) {
        const di::TextureMip& m = tex.mips[mipIndex];
        res.image = decodeMip(m, tex.format);
        res.mipWidth = m.width;
        res.mipHeight = m.height;
        if (res.image.isNull())
            res.error = QStringLiteral("mip %1 (%2x%3, %4) did not decode")
                            .arg(mipIndex).arg(m.width).arg(m.height).arg(res.formatName);
        return res;
    }

    // Largest first, then fall back to the largest mip with real content.
    std::vector<const di::TextureMip*> ordered;
    ordered.reserve(tex.mips.size());
    for (const di::TextureMip& m : tex.mips) ordered.push_back(&m);
    std::sort(ordered.begin(), ordered.end(), [](const di::TextureMip* a, const di::TextureMip* b) {
        return (uint32_t)a->width * a->height > (uint32_t)b->width * b->height;
    });

    QImage fallback;
    int fw = 0, fh = 0;
    for (const di::TextureMip* m : ordered) {
        QImage img = decodeMip(*m, tex.format);
        if (img.isNull()) continue;
        if (fallback.isNull()) { fallback = img; fw = m->width; fh = m->height; }
        if (hasContent(img)) {
            res.image = img;
            res.mipWidth = m->width;
            res.mipHeight = m->height;
            if (m != ordered.front())
                res.error = QStringLiteral("largest mip decoded near-constant; showing %1x%2")
                                .arg(m->width).arg(m->height);
            return res;
        }
    }
    if (!fallback.isNull()) {
        res.image = fallback;
        res.mipWidth = fw;
        res.mipHeight = fh;
        res.error = QStringLiteral("all mips decoded near-constant (showing largest)");
        return res;
    }
    res.error = QStringLiteral("no mip decoded (%1)").arg(res.formatName);
    return res;
}
