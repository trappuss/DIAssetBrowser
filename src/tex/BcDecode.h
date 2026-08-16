#pragma once
#include <QImage>

#include <cstdint>
#include <cstddef>

// CPU block-compression decoder -> RGBA8888 QImage, ported from the D4 tool
// (BC7 there verified bit-exact vs Pillow; BC1/3/4/5 exercised by its exporter).
// Differences for DI:
//   - dispatch keys on the DI EPixelFormat byte (BC1=18, BC2=19, BC3=20,
//     BC4=21, BC5=22, BC7=25) instead of D4's eTexFormat;
//   - block rows follow the slice's OWN pitchInByte (DI stores mips tightly;
//     D4 used a fixed D3D12 256-byte pitch). pitch 0 = tight ceil(w/4) rows;
//   - BC1 always honours punch-through (no D4-style opaque-variant split; if
//     holes appear in opaque DI textures, measure before changing this).
// Unrecognized/unsupported formats return a null QImage and warn ONCE per code.
namespace BcDecode {

QImage decode(const uint8_t* data, size_t len, int width, int height,
              int diFormat, int pitchInByte);

// Fast self-check of the block decoders (BC7 partition/anchor invariants +
// BC4/BC1 round-trips). Empty string on success; run at startup.
QString selfTest();

} // namespace BcDecode
