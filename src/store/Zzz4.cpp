#include "store/Zzz4.h"

#include <cstring>

#include <lz4.h>

namespace di {

bool isZzz4(const uint8_t* data, size_t len)
{
    return len >= 8 && data[0] == 'Z' && data[1] == 'Z' && data[2] == 'Z' && data[3] == '4';
}

static uint32_t rdU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

std::vector<uint8_t> lz4Tolerant(const uint8_t* src, size_t len, size_t start, size_t maxOut)
{
    std::vector<uint8_t> out;
    out.reserve(maxOut);
    size_t i = start;
    while (i < len && out.size() < maxOut) {
        const uint8_t token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            while (i < len) {
                const uint8_t b = src[i++];
                lit += b;
                if (b != 255) break;
            }
        }
        if (lit) {
            if (i + lit > len) lit = len - i;          // truncated literal run
            out.insert(out.end(), src + i, src + i + lit);
            i += lit;
        }
        if (out.size() >= maxOut || i + 2 > len) break;
        const size_t off = (size_t)src[i] | ((size_t)src[i + 1] << 8);
        i += 2;
        if (off == 0) break;
        size_t ml = token & 0xF;
        if (ml == 15) {
            while (i < len) {
                const uint8_t b = src[i++];
                ml += b;
                if (b != 255) break;
            }
        }
        ml += 4;
        if (off > out.size()) break;                    // corrupt back-reference
        // Grow first, THEN copy by index: inserting a vector's own range can
        // reallocate mid-insert (dangling source iterators, real UB — audit
        // finding). A forward byte copy is correct for overlapping matches too
        // (that is LZ4's run-repeat semantics).
        const size_t oldSize = out.size();
        const size_t s = oldSize - off;
        out.resize(oldSize + ml);
        uint8_t* d = out.data();
        if (off >= ml) {
            std::memcpy(d + oldSize, d + s, ml);        // non-overlapping
        } else {
            for (size_t k = 0; k < ml; ++k)
                d[oldSize + k] = d[s + k];
        }
    }
    return out;
}

std::vector<uint8_t> inflateZzz4(const uint8_t* data, size_t len)
{
    if (!isZzz4(data, len)) return {};
    const uint32_t usize = rdU32(data + 4);
    if (usize == 0) return {};

    std::vector<uint8_t> out(usize);
    const char*  src     = reinterpret_cast<const char*>(data + 8);
    const int    srcSize = (int)(len - 8);
    // safe_partial tolerates trailing slack after the block — exactly our case.
    const int n = LZ4_decompress_safe_partial(src, reinterpret_cast<char*>(out.data()),
                                              srcSize, (int)usize, (int)usize);
    if (n == (int)usize) return out;

    // Library path came up short (a few blobs carry unusual slack) — tolerant fallback.
    std::vector<uint8_t> tol = lz4Tolerant(data, len, 8, usize);
    if (tol.size() == usize) return tol;
    return {};                                          // caller decides how to report
}

} // namespace di
