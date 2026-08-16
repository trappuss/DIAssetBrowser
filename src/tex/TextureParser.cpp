#include "tex/TextureParser.h"

#include <cstring>

#include "store/Zzz4.h"

namespace di {

const uint8_t TEXTURE2D_MAGIC[4] = {0x02, 0x02, 0x02, 0x01};

static uint32_t rdU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rdU16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool isTexture2D(const uint8_t* data, size_t len)
{
    return len >= 0x28 && std::memcmp(data, TEXTURE2D_MAGIC, 4) == 0;
}

const TextureMip* Texture2D::largestMip() const
{
    const TextureMip* best = nullptr;
    for (const TextureMip& m : mips)
        if (!best || (uint32_t)m.width * m.height > (uint32_t)best->width * best->height)
            best = &m;
    return best;
}

bool parseTexture2D(const uint8_t* data, size_t len, Texture2D* out, std::string* err)
{
    if (!isTexture2D(data, len)) {
        if (err) *err = "not a Texture2D container (bad magic)";
        return false;
    }
    out->format     = data[0x05];
    out->mipLevel   = data[0x06];
    out->width      = (int16_t)rdU16(data + 0x0C);
    out->height     = (int16_t)rdU16(data + 0x0E);
    out->sliceCount = rdU16(data + 0x26);

    size_t pos = 0x28;
    while (pos + 20 <= len) {
        const uint32_t recLen = rdU32(data + pos);
        // recLen < 20 would underflow the payload arithmetic below (a corrupt
        // record could then read gigabytes past the buffer) — reject, don't clamp.
        if (recLen < 20 || pos + recLen > len) break;
        TextureMip mip;
        mip.width       = rdU16(data + pos + 4);
        mip.height      = rdU16(data + pos + 6);
        mip.depth       = rdU16(data + pos + 8);
        mip.pitchInByte = rdU16(data + pos + 10);
        const uint32_t sliceInByte = rdU32(data + pos + 12);
        const uint8_t* marker = data + pos + 16;

        if (std::memcmp(marker, "NNNN", 4) == 0) {
            const size_t avail = recLen - 20;
            const size_t take  = sliceInByte <= avail ? sliceInByte : avail;
            mip.data.assign(data + pos + 20, data + pos + 20 + take);
        } else if (std::memcmp(marker, "ZZZ4", 4) == 0) {
            // marker + u32 usize + LZ4 block, all inside this record
            mip.data = inflateZzz4(data + pos + 16, recLen - 16);
            if (mip.data.size() > sliceInByte && sliceInByte > 0)
                mip.data.resize(sliceInByte);
        } else {
            break;   // unknown chunk id: stop rather than misparse
        }
        if (!mip.data.empty() && sliceInByte > 0 && mip.data.size() >= sliceInByte)
            out->mips.push_back(std::move(mip));
        pos += recLen;
    }

    if (out->mips.empty()) {
        if (err) *err = "no mip records decoded (declared "
                        + std::to_string(out->sliceCount) + ")";
        return false;
    }
    return true;
}

} // namespace di
