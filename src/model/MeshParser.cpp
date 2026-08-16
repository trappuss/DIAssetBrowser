#include "model/MeshParser.h"

#include <cmath>
#include <cstring>

#include "store/Zzz4.h"

namespace di {

static uint32_t rdU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rdU16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

float halfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;                       // +-0
        } else {
            exp = 127 - 15 + 1;                   // subnormal: normalize
            while (!(mant & 0x400)) { mant <<= 1; --exp; }
            mant &= 0x3FF;
            f = (sign << 31) | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7F800000 | (mant << 13);   // inf/nan
    } else {
        f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}

namespace {

struct Component {
    char letter = 0;
    int  count  = 0;
    char type   = 0;
    int  bytes  = 0;
};

int typeSize(char t) { return t == 'F' ? 4 : t == 'H' ? 2 : t == 'B' ? 1 : 0; }

// "P3F_N4B_T2H" -> components. Parts that do not match letter+digit+type are
// skipped, exactly like the reference parser.
std::vector<Component> parseToken(const std::string& token)
{
    std::vector<Component> out;
    size_t start = 0;
    while (start <= token.size()) {
        const size_t us   = token.find('_', start);
        const size_t end  = us == std::string::npos ? token.size() : us;
        if (end > start) {
            const std::string part = token.substr(start, end - start);
            if (part.size() >= 3 && part[1] >= '0' && part[1] <= '9' &&
                typeSize(part.back()) > 0) {
                Component c;
                c.letter = part[0];
                c.count  = part[1] - '0';
                c.type   = part.back();
                c.bytes  = c.count * typeSize(c.type);
                out.push_back(c);
            }
        }
        if (us == std::string::npos) break;
        start = us + 1;
    }
    return out;
}

} // namespace

bool parseMesh(const uint8_t* data, size_t len, MeshData* out, std::string* err)
{
    std::vector<uint8_t> inflated;
    const uint8_t* d = data;
    size_t n = len;
    if (n >= 8 && isZzz4(data, n)) {
        inflated = inflateZzz4(data, n);
        if (inflated.empty()) {
            if (err) *err = "ZZZ4 inflate failed";
            return false;
        }
        d = inflated.data();
        n = inflated.size();
    }
    if (n < 0x1C + 8 || std::memcmp(d + 1, "MESSIAH", 7) != 0) {
        if (err) *err = "not a Messiah mesh (missing MESSIAH tag)";
        return false;
    }
    if (rdU32(d + 0x08) == 1) {
        if (err) *err = "no mesh data (type=1)";
        return false;
    }

    const uint16_t nSub   = rdU16(d + 0x12);
    const uint32_t nVerts = rdU32(d + 0x14);
    const uint32_t nIdx   = rdU32(d + 0x18);
    if (nVerts == 0 || nVerts > 8000000) {
        if (err) *err = "implausible vertex count " + std::to_string(nVerts);
        return false;
    }
    out->declaredVerts = nVerts;

    // 4 length-prefixed chunk names
    size_t pos = 0x1C;
    for (int i = 0; i < 4; ++i) {
        if (pos + 2 > n) { if (err) *err = "chunk names truncated"; return false; }
        const uint16_t ln = rdU16(d + pos);
        pos += 2;
        if (pos + ln > n) { if (err) *err = "chunk names truncated"; return false; }
        out->streams.emplace_back(reinterpret_cast<const char*>(d + pos), ln);
        pos += ln;
    }
    pos += 40;   // bbox/center/radius

    const uint32_t subCount = nSub > 0 ? nSub : 1;
    if (pos + (size_t)subCount * 16 > n) { if (err) *err = "submesh table truncated"; return false; }
    out->submeshes.reserve(subCount);
    for (uint32_t s = 0; s < subCount; ++s) {
        SubMeshRange r;
        r.startIndex = rdU32(d + pos);
        r.indexCount = rdU32(d + pos + 4);
        r.startVert  = rdU32(d + pos + 8);
        r.vertCount  = rdU32(d + pos + 12);
        out->submeshes.push_back(r);
        pos += 16;
    }

    // Index buffer: always u16 (see header).
    if (pos + (size_t)nIdx * 2 > n) { if (err) *err = "index buffer truncated"; return false; }
    out->indices.reserve(nIdx);
    for (uint32_t i = 0; i < nIdx; ++i)
        out->indices.push_back(rdU16(d + pos + (size_t)i * 2));
    pos += (size_t)nIdx * 2;

    // Vertex streams
    bool sawShortStream = false;
    for (const std::string& token : out->streams) {
        if (token.empty() || token == "None") continue;
        const std::vector<Component> comps = parseToken(token);
        size_t stride = 0;
        for (const Component& c : comps) stride += (size_t)c.bytes;
        if (stride == 0) continue;

        const size_t avail = (n - pos) / stride;
        const size_t cnt   = avail < nVerts ? avail : nVerts;
        if (cnt == 0) break;

        size_t off = 0;
        for (const Component& c : comps) {
            const uint8_t* base = d + pos + off;
            if (c.letter == 'P' && c.type == 'F' && c.count >= 3 && out->positions.empty()) {
                out->positions.resize(cnt * 3);
                for (size_t v = 0; v < cnt; ++v)
                    std::memcpy(&out->positions[v * 3], base + v * stride, 12);
            } else if (c.letter == 'Q' && c.type == 'H' && c.count == 4 &&
                       out->normals.empty()) {
                // Tangent-frame quaternion (x,y,z,w halfs). Normal = the
                // quaternion-rotated +Z axis, negated when w >= 0 (measured;
                // see header). Handedness/tangent stays undecoded until needed.
                out->normals.resize(cnt * 3);
                for (size_t v = 0; v < cnt; ++v) {
                    const float qx = halfToFloat(rdU16(base + v * stride));
                    const float qy = halfToFloat(rdU16(base + v * stride + 2));
                    const float qz = halfToFloat(rdU16(base + v * stride + 4));
                    const float qw = halfToFloat(rdU16(base + v * stride + 6));
                    const float s  = (qw >= 0.0f) ? -1.0f : 1.0f;
                    float nx = s * (2.0f * (qx * qz + qw * qy));
                    float ny = s * (2.0f * (qy * qz - qw * qx));
                    float nz = s * (1.0f - 2.0f * (qx * qx + qy * qy));
                    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 1e-12f) { nx /= len; ny /= len; nz /= len; }
                    else              { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
                    out->normals[v * 3]     = nx;
                    out->normals[v * 3 + 1] = ny;
                    out->normals[v * 3 + 2] = nz;
                }
                // tangent = q rotate +Y; handedness shares the -sign(w) factor
                out->tangents.resize(cnt * 4);
                for (size_t v = 0; v < cnt; ++v) {
                    const float qx = halfToFloat(rdU16(base + v * stride));
                    const float qy = halfToFloat(rdU16(base + v * stride + 2));
                    const float qz = halfToFloat(rdU16(base + v * stride + 4));
                    const float qw = halfToFloat(rdU16(base + v * stride + 6));
                    float tx = 2.0f * (qx * qy - qw * qz);
                    float ty = 1.0f - 2.0f * (qx * qx + qz * qz);
                    float tz = 2.0f * (qy * qz + qw * qx);
                    const float len = std::sqrt(tx * tx + ty * ty + tz * tz);
                    if (len > 1e-12f) { tx /= len; ty /= len; tz /= len; }
                    else              { tx = 1.0f; ty = 0.0f; tz = 0.0f; }
                    out->tangents[v * 4]     = tx;
                    out->tangents[v * 4 + 1] = ty;
                    out->tangents[v * 4 + 2] = tz;
                    out->tangents[v * 4 + 3] = (qw >= 0.0f) ? -1.0f : 1.0f;
                }
            } else if (c.letter == 'T' && out->uv0.empty() && c.count >= 2 &&
                       (c.type == 'F' || c.type == 'H')) {
                out->uv0.resize(cnt * 2);
                for (size_t v = 0; v < cnt; ++v) {
                    if (c.type == 'F') {
                        std::memcpy(&out->uv0[v * 2], base + v * stride, 8);
                    } else {
                        out->uv0[v * 2]     = halfToFloat(rdU16(base + v * stride));
                        out->uv0[v * 2 + 1] = halfToFloat(rdU16(base + v * stride + 2));
                    }
                }
            } else if (c.letter == 'W' && c.type == 'B' && out->boneWeights.empty()) {
                out->boneWeights.resize(cnt * 4, 0.0f);
                const int k = c.count < 4 ? c.count : 4;
                for (size_t v = 0; v < cnt; ++v)
                    for (int w = 0; w < k; ++w)
                        out->boneWeights[v * 4 + w] = base[v * stride + w] / 255.0f;
                out->skinned = true;
            } else if (c.letter == 'I' && c.type == 'B' && out->boneIndices.empty()) {
                out->boneIndices.resize(cnt * 4, 0);
                const int k = c.count < 4 ? c.count : 4;
                for (size_t v = 0; v < cnt; ++v)
                    for (int w = 0; w < k; ++w)
                        out->boneIndices[v * 4 + w] = base[v * stride + w];
                out->skinned = true;
            }
            off += (size_t)c.bytes;
        }
        pos += cnt * stride;
        if (cnt < nVerts) { sawShortStream = true; break; }   // matches reference
    }

    if (out->positions.empty()) {
        if (err) *err = "no position stream found";
        return false;
    }

    // Keep indices only when the position stream is complete and in range.
    const size_t nvHave = out->positions.size() / 3;
    if (!out->indices.empty()) {
        uint32_t maxIdx = 0;
        for (uint32_t i : out->indices)
            if (i > maxIdx) maxIdx = i;
        if (nvHave < nVerts || (size_t)maxIdx >= nvHave)
            out->indices.clear();   // partial / out-of-range -> point cloud
    }
    (void)sawShortStream;
    return true;
}

} // namespace di
