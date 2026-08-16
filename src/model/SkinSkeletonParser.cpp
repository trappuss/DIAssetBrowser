#include "model/SkinSkeletonParser.h"

#include <cstring>

namespace di {

static uint16_t rdU16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool parseSkinSkeleton(const uint8_t* data, size_t len, SkinSkeleton* out, std::string* err)
{
    if (len < 0x14 || std::memcmp(data, ".MESSIAH", 8) != 0) {
        if (err) *err = "not a .SkinSkeleton (missing .MESSIAH magic)";
        return false;
    }
    const uint16_t nb = rdU16(data + 0x12);
    size_t pos = 0x14;
    out->bones.reserve(nb);
    for (uint16_t i = 0; i < nb; ++i) {
        if (pos >= len) break;
        const uint8_t ln = data[pos];
        ++pos;
        if (ln == 0 || ln > 48 || pos + 48 + ln > len) break;
        SkinBone b;
        std::memcpy(b.invBind, data + pos, 48);
        pos += 48;
        b.name.assign(reinterpret_cast<const char*>(data + pos), ln);
        pos += ln;
        // world = -R @ t  (R = rows 0-2 of the 4x3, t = row 3)
        const float* m = b.invBind;
        const float t0 = m[9], t1 = m[10], t2 = m[11];
        b.world[0] = -(m[0] * t0 + m[1] * t1 + m[2] * t2);
        b.world[1] = -(m[3] * t0 + m[4] * t1 + m[5] * t2);
        b.world[2] = -(m[6] * t0 + m[7] * t1 + m[8] * t2);
        out->bones.push_back(std::move(b));
    }
    if (out->bones.empty()) {
        if (err) *err = "no bones parsed (declared " + std::to_string(nb) + ")";
        return false;
    }
    return true;
}

} // namespace di
