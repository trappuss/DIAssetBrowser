#pragma once
// Texture2D -> QImage. Picks the mip, dispatches by format (BCn via BcDecode,
// uncompressed raw), and reports what happened so the UI can say it plainly.

#include <QImage>
#include <QString>

#include "tex/TextureParser.h"

namespace TextureDecode {

struct Result {
    QImage  image;                 // null on failure
    int     mipWidth = 0, mipHeight = 0;
    QString formatName;            // e.g. "BC7"
    QString error;                 // set when image is null
};

// Decode one mip (mipIndex into tex.mips), or the largest mip when mipIndex<0.
// When the largest mip decodes to a near-constant image (a known failure shape
// on some high mips in the Python tool), falls back to the largest mip that
// decodes with real content — and says so in `error` alongside the image.
Result decode(const di::Texture2D& tex, int mipIndex = -1);

} // namespace TextureDecode
