#include "tex/DiPixelFormat.h"

namespace DiPixelFormat {

// ASTC block footprints for formats 36..49 (LDR) and 55..68 (HDR).
static bool astcFootprint(int f, int* bw, int* bh)
{
    static const int dims[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},
                                    {8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    if (f >= 36 && f <= 49) { *bw = dims[f - 36][0]; *bh = dims[f - 36][1]; return true; }
    if (f >= 55 && f <= 68) { *bw = dims[f - 55][0]; *bh = dims[f - 55][1]; return true; }
    return false;
}

Codec codec(int f)
{
    Codec c;
    c.name = name(f);
    switch (f) {
        case 18: c.kind = K_BC1; c.bytesPerBlock = 8;  c.blockW = c.blockH = 4; return c;
        case 19: c.kind = K_BC2; c.bytesPerBlock = 16; c.blockW = c.blockH = 4; return c;
        case 20: c.kind = K_BC3; c.bytesPerBlock = 16; c.blockW = c.blockH = 4; return c;
        case 21: c.kind = K_BC4; c.bytesPerBlock = 8;  c.blockW = c.blockH = 4; return c;
        case 22: c.kind = K_BC5; c.bytesPerBlock = 16; c.blockW = c.blockH = 4; return c;
        case 23: case 24:
                 c.kind = K_BC6; c.bytesPerBlock = 16; c.blockW = c.blockH = 4; return c;
        case 25: c.kind = K_BC7; c.bytesPerBlock = 16; c.blockW = c.blockH = 4; return c;
        // 3: the .bt template calls this R32G32B32A32, but measured on the
        // live .1 blobs (2026-08-01) pitch is 4 bytes/pixel and the channel
        // statistics of a fmt-3 _m map match the RGBA mix-map convention
        // (R=rough G=metal B=AO) byte-for-byte — it decodes as plain RGBA8.
        // All 6,092 fmt-3 textures on PC are .1 blobs (terrain index maps +
        // some cosmetic _m maps).
        case 3:
        case 5:  c.kind = K_RGBA8; c.bytesPerPixel = 4; return c;
        case 12: c.kind = K_L8;    c.bytesPerPixel = 1; return c;
        case 14: c.kind = K_A8;    c.bytesPerPixel = 1; return c;
        case 31: case 32: case 33:
                 c.kind = K_ETC; c.bytesPerBlock = (f == 33) ? 16 : 8;
                 c.blockW = c.blockH = 4; return c;
        default: break;
    }
    int bw = 0, bh = 0;
    if (astcFootprint(f, &bw, &bh)) {
        c.kind = K_ASTC; c.bytesPerBlock = 16; c.blockW = bw; c.blockH = bh;
        return c;
    }
    c.kind = K_Unknown;
    return c;
}

QString name(int f)
{
    switch (f) {
        case 3:  return QStringLiteral("RGBA8(3)");   // template said R32G32B32A32;
                                                      // measured 4 B/px (see codec)
        case 4:  return QStringLiteral("A16B16G16R16");
        case 5:  return QStringLiteral("R8G8B8A8");
        case 6:  return QStringLiteral("B5G6R5");
        case 7:  return QStringLiteral("A8L8");
        case 12: return QStringLiteral("L8");
        case 14: return QStringLiteral("A8");
        case 18: return QStringLiteral("BC1");
        case 19: return QStringLiteral("BC2");
        case 20: return QStringLiteral("BC3");
        case 21: return QStringLiteral("BC4");
        case 22: return QStringLiteral("BC5");
        case 23: return QStringLiteral("BC6S");
        case 24: return QStringLiteral("BC6U");
        case 25: return QStringLiteral("BC7");
        case 27: return QStringLiteral("PVRTC2_RGB");
        case 28: return QStringLiteral("PVRTC2_RGBA");
        case 29: return QStringLiteral("PVRTC4_RGB");
        case 30: return QStringLiteral("PVRTC4_RGBA");
        case 31: return QStringLiteral("ETC1");
        case 32: return QStringLiteral("ETC2RGB");
        case 33: return QStringLiteral("ETC2RGBA");
        default: break;
    }
    if (f >= 36 && f <= 49) return QStringLiteral("ASTC_LDR_%1").arg(f);
    if (f >= 55 && f <= 68) return QStringLiteral("ASTC_HDR_%1").arg(f);
    return QStringLiteral("format_%1").arg(f);
}

} // namespace DiPixelFormat
