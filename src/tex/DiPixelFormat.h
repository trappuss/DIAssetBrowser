#pragma once
// DI EPixelFormat (DiabloImmortal_Texture.bt, full enum) -> codec descriptor.
// PC build measured so far: BC7 (25) on character diffuse. The BC1..BC7 block
// decoders are ported from the D4 tool (BC7 verified bit-exact vs Pillow there).

#include <QString>

namespace DiPixelFormat {

enum Kind {
    K_Unknown = 0,
    K_BC1, K_BC2, K_BC3, K_BC4, K_BC5, K_BC7,   // block-compressed, decodable
    K_RGBA8, K_L8, K_A8,                        // uncompressed, decodable
    K_ASTC, K_ETC, K_BC6, K_Other               // recognized, not yet decoded
};

struct Codec {
    Kind    kind = K_Unknown;
    int     bytesPerBlock = 0;   // block formats: 8 or 16
    int     blockW = 1, blockH = 1;
    int     bytesPerPixel = 0;   // uncompressed formats
    QString name;
};

Codec   codec(int format);
QString name(int format);

} // namespace DiPixelFormat
