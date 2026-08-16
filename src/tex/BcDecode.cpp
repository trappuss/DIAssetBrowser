#include "tex/BcDecode.h"
#include "tex/DiPixelFormat.h"

#include <QDebug>
#include <QMutex>
#include <QSet>

#include <cstring>
#include <utility>

namespace {

struct RGBA { quint8 r, g, b, a; };

RGBA from565(quint16 c)
{
    const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return {quint8((r << 3) | (r >> 2)), quint8((g << 2) | (g >> 4)),
            quint8((b << 3) | (b >> 2)), 255};
}

quint16 rd16(const quint8* p) { return quint16(p[0]) | (quint16(p[1]) << 8); }

// One BC1 colour block (8 bytes) -> 16 RGBA texels (row-major 4x4).
void decodeColorBlock(const quint8* b, RGBA out[16], bool allowPunch)
{
    const quint16 c0 = rd16(b), c1 = rd16(b + 2);
    RGBA col[4];
    col[0] = from565(c0);
    col[1] = from565(c1);
    if (c0 > c1 || !allowPunch) {
        col[2].r = quint8((2 * col[0].r + col[1].r) / 3);
        col[2].g = quint8((2 * col[0].g + col[1].g) / 3);
        col[2].b = quint8((2 * col[0].b + col[1].b) / 3);
        col[3].r = quint8((col[0].r + 2 * col[1].r) / 3);
        col[3].g = quint8((col[0].g + 2 * col[1].g) / 3);
        col[3].b = quint8((col[0].b + 2 * col[1].b) / 3);
        col[2].a = col[3].a = 255;
    } else {
        col[2].r = quint8((col[0].r + col[1].r) / 2);
        col[2].g = quint8((col[0].g + col[1].g) / 2);
        col[2].b = quint8((col[0].b + col[1].b) / 2);
        col[2].a = 255;
        col[3] = {0, 0, 0, 0};   // transparent black
    }
    quint32 bits = quint32(b[4]) | (quint32(b[5]) << 8) | (quint32(b[6]) << 16) | (quint32(b[7]) << 24);
    for (int i = 0; i < 16; ++i)
        out[i] = col[(bits >> (i * 2)) & 0x3];
}

// One BC4/interpolated-alpha block (8 bytes) -> 16 channel values.
void decodeAlphaBlock(const quint8* b, quint8 out[16])
{
    const int a0 = b[0], a1 = b[1];
    int a[8];
    a[0] = a0; a[1] = a1;
    if (a0 > a1) {
        for (int k = 1; k <= 6; ++k) a[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    } else {
        for (int k = 1; k <= 4; ++k) a[k + 1] = ((5 - k) * a0 + k * a1) / 5;
        a[6] = 0; a[7] = 255;
    }
    quint64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= quint64(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = quint8(a[(bits >> (i * 3)) & 0x7]);
}

// One BC2 explicit-alpha block (first 8 bytes of the 16): 4 bits per texel.
void decodeBc2AlphaBlock(const quint8* b, quint8 out[16])
{
    for (int i = 0; i < 16; ++i) {
        const int nib = (b[i / 2] >> ((i & 1) * 4)) & 0xF;
        out[i] = quint8((nib << 4) | nib);
    }
}

// ── BC7 (ported from the D4 tool; verified bit-exact vs Pillow there) ───────
const quint16 kP2[64] = {
0xCCCC,0x8888,0xEEEE,0xECC8,0xC880,0xFEEC,0xFEC8,0xEC80,0xC800,0xFFEC,0xFE80,0xE800,0xFFE8,0xFF00,0xFFF0,0xF000,
0xF710,0x008E,0x7100,0x08CE,0x008C,0x7310,0x3100,0x8CCE,0x088C,0x3110,0x6666,0x366C,0x17E8,0x0FF0,0x718E,0x399C,
0xAAAA,0xF0F0,0x5A5A,0x33CC,0x3C3C,0x55AA,0x9696,0xA55A,0x73CE,0x13C8,0x324C,0x3BDC,0x6996,0xC33C,0x9966,0x0660,
0x0272,0x04E4,0x4E40,0x2720,0xC936,0x936C,0x39C6,0x639C,0x9336,0x9CC6,0x817E,0xE718,0xCCF0,0x0FCC,0x7744,0xEE22};
const quint8 kA2[64] = {
15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,
15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15};
const quint8 kP3[64][16] = {
{0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},{0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
{0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},{0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
{0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},{0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
{0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},{0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
{0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},{0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
{0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},{0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
{0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},{0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
{0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},{0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
{0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},{0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
{0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
{0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},{0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
{0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},{0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
{0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},{0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
{0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},{0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
{0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},{0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}};
const quint8 kA3a[64] = {
3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,
8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3};
const quint8 kA3b[64] = {
15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,
15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8};
struct Bc7Mode { int NS,PB,RB,ISB,CB,AB,EPB,SPB,IB,IB2; };
const Bc7Mode kM[8] = {
{3,4,0,0,4,0,1,0,3,0},{2,6,0,0,6,0,0,1,3,0},{3,6,0,0,5,0,0,0,2,0},{2,6,0,0,7,0,1,0,2,0},
{1,0,2,1,5,6,0,0,2,3},{1,0,2,0,7,8,0,0,2,2},{1,0,0,0,7,7,1,0,4,0},{2,6,0,0,5,5,1,0,2,0}};
const int kW2[4]={0,21,43,64};
const int kW3[8]={0,9,18,27,37,46,55,64};
const int kW4[16]={0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};
const int* weights(int n){ return n==2?kW2 : n==3?kW3 : kW4; }
inline int bc7unq(int v,int p){ return p>=8 ? v : ((v<<(8-p))|(v>>(2*p-8))); }

void decodeBC7Block(const quint8* blk, RGBA out[16])
{
    int pos = 0;
    auto get = [&](int n) -> int {
        int r = 0;
        for (int i = 0; i < n; ++i) { r |= ((blk[pos >> 3] >> (pos & 7)) & 1) << i; ++pos; }
        return r;
    };
    if (blk[0] == 0) { for (int i = 0; i < 16; ++i) out[i] = {0,0,0,255}; return; }
    int m = 0; while (!((blk[0] >> m) & 1)) ++m;
    get(m + 1);
    const Bc7Mode& M = kM[m];
    const int NS = M.NS, ne = NS * 2;
    const int part = M.PB ? get(M.PB) : 0;
    const int rot  = M.RB ? get(M.RB) : 0;
    const int isel = M.ISB ? get(M.ISB) : 0;

    int col[6][3] = {};
    for (int c = 0; c < 3; ++c) for (int e = 0; e < ne; ++e) col[e][c] = get(M.CB);
    int alpha[6] = {};
    if (M.AB) for (int e = 0; e < ne; ++e) alpha[e] = get(M.AB);
    int pb[6] = {};
    if (M.EPB) { for (int e = 0; e < ne; ++e) pb[e] = get(1); }
    else if (M.SPB) { int sp[3]={}; for (int s = 0; s < NS; ++s) sp[s]=get(1); for (int e=0;e<ne;++e) pb[e]=sp[e/2]; }

    int ep[6][4];
    const bool hasP = (M.EPB || M.SPB);
    const int cp = M.CB + (hasP ? 1 : 0);
    const int ap = M.AB ? (M.AB + (hasP ? 1 : 0)) : 0;
    for (int e = 0; e < ne; ++e) {
        int r=col[e][0], g=col[e][1], b=col[e][2], a=alpha[e];
        if (hasP) { r=(r<<1)|pb[e]; g=(g<<1)|pb[e]; b=(b<<1)|pb[e]; if (M.AB) a=(a<<1)|pb[e]; }
        ep[e][0]=bc7unq(r,cp); ep[e][1]=bc7unq(g,cp); ep[e][2]=bc7unq(b,cp);
        ep[e][3]=M.AB ? bc7unq(a,ap) : 255;
    }
    auto subOf = [&](int t){ return NS==2 ? ((kP2[part]>>t)&1) : (NS==3 ? int(kP3[part][t]) : 0); };
    auto anchorOf = [&](int sub){
        if (sub==0) return 0;
        if (NS==2) return int(kA2[part]);
        return sub==1 ? int(kA3a[part]) : int(kA3b[part]);   // NS==3
    };

    int idx[16], idx2[16];
    for (int t = 0; t < 16; ++t) { int sub=subOf(t); idx[t]=get(t==anchorOf(sub) ? M.IB-1 : M.IB); }
    if (M.IB2) for (int t = 0; t < 16; ++t) idx2[t]=get(t==0 ? M.IB2-1 : M.IB2);

    const int* wp  = weights(M.IB);
    const int* wp2 = M.IB2 ? weights(M.IB2) : nullptr;
    for (int t = 0; t < 16; ++t) {
        const int sub = subOf(t);
        const int* e0 = ep[sub*2]; const int* e1 = ep[sub*2+1];
        int r,g,b,a;
        if (M.IB2) {
            int ci=idx[t], ai=idx2[t];
            if (isel==1) { ci=idx2[t]; ai=idx[t]; }
            const int cw = (isel==0 ? wp : wp2)[ci];
            const int aw = (isel==0 ? wp2 : wp)[ai];
            r=((64-cw)*e0[0]+cw*e1[0]+32)>>6; g=((64-cw)*e0[1]+cw*e1[1]+32)>>6;
            b=((64-cw)*e0[2]+cw*e1[2]+32)>>6; a=((64-aw)*e0[3]+aw*e1[3]+32)>>6;
        } else {
            const int w = wp[idx[t]];
            r=((64-w)*e0[0]+w*e1[0]+32)>>6; g=((64-w)*e0[1]+w*e1[1]+32)>>6;
            b=((64-w)*e0[2]+w*e1[2]+32)>>6; a=((64-w)*e0[3]+w*e1[3]+32)>>6;
        }
        quint8 px[4] = {quint8(r),quint8(g),quint8(b),quint8(a)};
        if (rot==1) std::swap(px[0],px[3]);
        else if (rot==2) std::swap(px[1],px[3]);
        else if (rot==3) std::swap(px[2],px[3]);
        out[t] = {px[0],px[1],px[2],px[3]};
    }
}

} // namespace

QImage BcDecode::decode(const uint8_t* data, size_t len, int width, int height,
                        int diFormat, int pitchInByte)
{
    if (!data || width <= 0 || height <= 0)
        return {};
    const DiPixelFormat::Codec codec = DiPixelFormat::codec(diFormat);

    enum { D_BC1, D_BC2, D_BC3, D_BC4, D_BC5, D_BC7, D_NONE } kind = D_NONE;
    switch (codec.kind) {
        case DiPixelFormat::K_BC1: kind = D_BC1; break;
        case DiPixelFormat::K_BC2: kind = D_BC2; break;
        case DiPixelFormat::K_BC3: kind = D_BC3; break;
        case DiPixelFormat::K_BC4: kind = D_BC4; break;
        case DiPixelFormat::K_BC5: kind = D_BC5; break;
        case DiPixelFormat::K_BC7: kind = D_BC7; break;
        default: break;
    }
    if (kind == D_NONE) {
        // LOUD once per format code — a silent blank preview everywhere is how
        // new-format-after-patch bugs hide.
        static QSet<int> warned;
        static QMutex warnMtx;
        QMutexLocker lock(&warnMtx);
        if (!warned.contains(diFormat) && warned.size() < 32) {
            warned.insert(diFormat);
            qWarning("BcDecode: format %d (%s) not decodable yet (%dx%d, %lld bytes)",
                     diFormat, qPrintable(codec.name), width, height, qlonglong(len));
        }
        return {};
    }

    const int blockBytes  = codec.bytesPerBlock;
    const int tightPerRow = (width + 3) / 4;
    int blocksPerRow = (pitchInByte > 0 && pitchInByte % blockBytes == 0)
                           ? pitchInByte / blockBytes
                           : tightPerRow;
    // A file-supplied pitch SMALLER than the tight pitch would let the row loop
    // address past the buffer while the size check passed — treat it as tight.
    if (blocksPerRow < tightPerRow) blocksPerRow = tightPerRow;
    const int blockRows = (height + 3) / 4;
    // The last block row only needs its tight blocks, not the row padding.
    const qint64 need = (qint64(blockRows - 1) * blocksPerRow + tightPerRow) * blockBytes;
    if ((qint64)len < need)
        return {};

    QImage img(width, height, QImage::Format_RGBA8888);
    img.fill(Qt::black);

    for (int by = 0; by < blockRows; ++by) {
        for (int bx = 0; bx < tightPerRow; ++bx) {   // never sample pitch padding
            const quint8* blk = data + (qint64(by) * blocksPerRow + bx) * blockBytes;
            RGBA texel[16];
            if (kind == D_BC1) {
                decodeColorBlock(blk, texel, /*allowPunch=*/true);
            } else if (kind == D_BC2) {
                quint8 alpha[16];
                decodeBc2AlphaBlock(blk, alpha);
                decodeColorBlock(blk + 8, texel, /*allowPunch=*/false);
                for (int i = 0; i < 16; ++i) texel[i].a = alpha[i];
            } else if (kind == D_BC3) {
                quint8 alpha[16];
                decodeAlphaBlock(blk, alpha);
                decodeColorBlock(blk + 8, texel, /*allowPunch=*/false);
                for (int i = 0; i < 16; ++i) texel[i].a = alpha[i];
            } else if (kind == D_BC4) {
                quint8 r[16];
                decodeAlphaBlock(blk, r);
                for (int i = 0; i < 16; ++i) texel[i] = {r[i], r[i], r[i], 255};
            } else if (kind == D_BC5) {
                quint8 r[16], g[16];
                decodeAlphaBlock(blk, r);
                decodeAlphaBlock(blk + 8, g);
                for (int i = 0; i < 16; ++i) texel[i] = {r[i], g[i], 0, 255};
            } else { // D_BC7
                decodeBC7Block(blk, texel);
            }
            for (int py = 0; py < 4; ++py) {
                const int y = by * 4 + py;
                if (y >= height) break;
                quint8* line = img.scanLine(y);
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px;
                    if (x >= width) continue;
                    const RGBA& t = texel[py * 4 + px];
                    quint8* o = line + x * 4;
                    o[0] = t.r; o[1] = t.g; o[2] = t.b; o[3] = t.a;
                }
            }
        }
    }
    return img;
}

QString BcDecode::selfTest()
{
    for (int p = 0; p < 64; ++p) {
        if (((kP2[p] >> 0) & 1) != 0)
            return QStringLiteral("BC7 kP2[%1]: texel 0 not in subset 0").arg(p);
        const int a = kA2[p];
        if (a < 0 || a > 15) return QStringLiteral("BC7 kA2[%1] out of range").arg(p);
        if (((kP2[p] >> a) & 1) != 1)
            return QStringLiteral("BC7 kA2[%1]=%2 anchor not in subset 1").arg(p).arg(a);
    }
    for (int p = 0; p < 64; ++p) {
        if (kP3[p][0] != 0) return QStringLiteral("BC7 kP3[%1]: texel 0 not in subset 0").arg(p);
        bool seen[3] = {false, false, false};
        for (int t = 0; t < 16; ++t) { const int s = kP3[p][t];
            if (s < 0 || s > 2) return QStringLiteral("BC7 kP3[%1][%2]=%3 invalid subset").arg(p).arg(t).arg(s);
            seen[s] = true; }
        if (!(seen[0] && seen[1] && seen[2]))
            return QStringLiteral("BC7 kP3[%1] missing a subset").arg(p);
        if (kP3[p][kA3a[p]] != 1) return QStringLiteral("BC7 kA3a[%1]=%2 not in subset 1").arg(p).arg(kA3a[p]);
        if (kP3[p][kA3b[p]] != 2) return QStringLiteral("BC7 kA3b[%1]=%2 not in subset 2").arg(p).arg(kA3b[p]);
    }
    // BC4 round-trip (DI format 21): a0=255,a1=0; texel 0 index=1 (->0), rest ->255.
    {
        uint8_t blk[8] = {0xFF, 0x00, 0x01, 0, 0, 0, 0, 0};
        const QImage im = decode(blk, sizeof(blk), 4, 4, 21, 8);
        if (im.isNull()) return QStringLiteral("BC4 decode returned null");
        const QRgb t0 = im.pixel(0, 0), t1 = im.pixel(1, 0);
        if (qRed(t0) > 8)   return QStringLiteral("BC4 texel0 expected ~0, got %1").arg(qRed(t0));
        if (qRed(t1) < 247) return QStringLiteral("BC4 texel1 expected ~255, got %1").arg(qRed(t1));
    }
    // BC1 round-trip (DI format 18): c0=white, c1=black, indices 0 -> white.
    {
        uint8_t blk[8] = {0xFF, 0xFF, 0x00, 0x00, 0, 0, 0, 0};
        const QImage im = decode(blk, sizeof(blk), 4, 4, 18, 8);
        if (im.isNull()) return QStringLiteral("BC1 decode returned null");
        const QRgb w = im.pixel(0, 0);
        if (qRed(w) < 240 || qGreen(w) < 240 || qBlue(w) < 240)
            return QStringLiteral("BC1 white texel too dark: (%1,%2,%3)").arg(qRed(w)).arg(qGreen(w)).arg(qBlue(w));
    }
    return QString();
}
