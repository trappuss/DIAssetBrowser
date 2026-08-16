#include "model/MaterialParser.h"

#include <cstdlib>
#include <cstring>

#include "store/Zzz4.h"

namespace di {

namespace {

// GUID text "11d9765c-9a0a-4b34-b6a6-c14b703c3d36" -> 32 lowercase hex chars.
// Empty result = malformed or all-zero.
std::string guidAt(const uint8_t* p, size_t avail)
{
    std::string hex;
    hex.reserve(32);
    size_t i = 0;
    bool nonZero = false;
    while (i < avail && hex.size() < 32) {
        const char c = (char)p[i];
        if (c == '-') { ++i; continue; }
        const bool lo = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        const bool up = (c >= 'A' && c <= 'F');
        if (!lo && !up) break;
        hex += up ? (char)(c - 'A' + 'a') : c;
        if (c != '0') nonZero = true;
        ++i;
    }
    if (hex.size() != 32 || !nonZero) return {};
    return hex;
}

void scanKey(const uint8_t* d, size_t n, const char* key, std::string* out)
{
    if (!out->empty()) return;
    const size_t klen = std::strlen(key);
    if (n < klen) return;
    for (size_t i = 0; i + klen <= n; ++i) {
        if (std::memcmp(d + i, key, klen) != 0) continue;
        // reject only when the preceding byte is an identifier character —
        // that is a longer key ending in ours ("XtBaseMap="). '\n' and the
        // record framing's binary length bytes are both valid predecessors
        // (a key CAN be the first line of a record's text block).
        if (i > 0) {
            const uint8_t c = d[i - 1];
            const bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '_';
            if (ident) continue;
        }
        const std::string g = guidAt(d + i + klen, n - i - klen);
        if (!g.empty()) { *out = g; return; }
    }
}

// Parse "key=1.5" or "key=(a,b,c[,d])" float constants out of the key=value
// text. Returns how many floats landed in out[]; 0 = key absent/malformed.
int scanFloats(const uint8_t* d, size_t n, const char* key, float* out, int maxN)
{
    const size_t klen = std::strlen(key);
    for (size_t i = 0; i + klen < n; ++i) {
        if (std::memcmp(d + i, key, klen) != 0) continue;
        if (i > 0) {
            const uint8_t c = d[i - 1];
            const bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '_';
            if (ident) continue;
        }
        size_t p = i + klen;
        int got = 0;
        while (p < n && got < maxN) {
            while (p < n && (d[p] == '(' || d[p] == ',' || d[p] == ' ')) ++p;
            if (p >= n || d[p] == ')' || d[p] == '\n') break;
            char buf[48];
            size_t b = 0;
            while (p < n && b + 1 < sizeof buf &&
                   ((d[p] >= '0' && d[p] <= '9') || d[p] == '.' || d[p] == '-' ||
                    d[p] == '+' || d[p] == 'e' || d[p] == 'E'))
                buf[b++] = (char)d[p++];
            if (b == 0) break;
            buf[b] = 0;
            out[got++] = (float)std::atof(buf);
        }
        return got;
    }
    return 0;
}

} // namespace

bool parseMaterial(const uint8_t* data, size_t len, MaterialMaps* out,
                   std::string* err)
{
    std::vector<uint8_t> inflated;
    const uint8_t* d = data;
    size_t n = len;
    if (n >= 8 && isZzz4(data, n)) {
        inflated = inflateZzz4(data, n);
        if (inflated.empty()) {
            if (err) *err = "material ZZZ4 inflate failed";
            return false;
        }
        d = inflated.data();
        n = inflated.size();
    }
    if (n < 16 || std::memcmp(d + 1, "MESSIAH", 7) != 0) {
        if (err) *err = "not a MESSIAH material blob";
        return false;
    }

    scanKey(d, n, "tBaseMap=",     &out->baseMap);
    scanKey(d, n, "tNormalMap=",   &out->normalMap);
    scanKey(d, n, "tMixMap=",      &out->mixMap);
    scanKey(d, n, "tEmissionMap=", &out->emissionMap);
    scanKey(d, n, "tEmmsiveMap=",  &out->emissionMap);   // sic — key exists in data

    // Animated-emissive FX constants (keys measured on the live blobs; the
    // game's own misspelling "cEmmsionColor" included).
    {
        float f[4];
        if (scanFloats(d, n, "cEmmsionColor=", f, 3) == 3) {
            out->emissionColor = {f[0], f[1], f[2]};
            out->emissionColorSet = (f[0] + f[1] + f[2]) > 0.0001f;
        }
        if (scanFloats(d, n, "cArcaneColor=", f, 3) == 3)
            out->arcaneColor = {f[0], f[1], f[2]};
        if (scanFloats(d, n, "cArcaneIntensity=", f, 1) == 1)
            out->arcaneIntensity = f[0];
        if (scanFloats(d, n, "cArcaneScrolling=", f, 4) == 4)
            out->arcaneScroll = {f[0], f[1], f[2], f[3]};
        float uv[2], sp[2];
        if (scanFloats(d, n, "_StarUV1=", uv, 2) == 2 &&
            scanFloats(d, n, "_StarSpeed1=", sp, 2) == 2)
            out->star1 = {uv[0], uv[1], sp[0], sp[1]};
        if (scanFloats(d, n, "_StarUV2=", uv, 2) == 2 &&
            scanFloats(d, n, "_StarSpeed2=", sp, 2) == 2)
            out->star2 = {uv[0], uv[1], sp[0], sp[1]};
        if (scanFloats(d, n, "cFresnelColor=", f, 3) == 3)
            out->fresnelColor = {f[0], f[1], f[2]};
        if (scanFloats(d, n, "cFresnelIntensity=", f, 1) == 1)
            out->fresnelIntensity = f[0];
        if (scanFloats(d, n, "cFresnelRange=", f, 1) == 1)
            out->fresnelRange = f[0];
    }

    // First record's shader name: header ends with u8 recordCount at 14, then
    // u16 nameLen at 15, name at 17, u16 shaderLen, shader (verified on the
    // golden). Purely informational; a mismatch only costs the log line.
    if (n > 17) {
        const uint16_t nameLen = (uint16_t)(d[15] | (d[16] << 8));
        const size_t sh = 17 + nameLen;
        if (sh + 2 < n) {
            const uint16_t shLen = (uint16_t)(d[sh] | (d[sh + 1] << 8));
            if (shLen > 0 && shLen < 64 && sh + 2 + shLen <= n) {
                bool print = true;
                for (size_t i = 0; i < shLen; ++i)
                    if (d[sh + 2 + i] < 0x20 || d[sh + 2 + i] >= 0x7F) print = false;
                if (print)
                    out->shader.assign((const char*)d + sh + 2, shLen);
            }
        }
    }
    return true;
}

} // namespace di
