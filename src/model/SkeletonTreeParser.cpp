#include "model/SkeletonTreeParser.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace di {

namespace {

struct Node {
    uint8_t  key = 0;
    uint32_t childCount = 0;
    int      value = -1;       // index into strings, -1 = non-string value
    std::vector<uint32_t> children;
};

bool readCValue(const uint8_t* d, size_t n, size_t* pos, uint32_t* out)
{
    if (*pos >= n) return false;
    const uint8_t b0 = d[(*pos)++];
    if (!(b0 & 0x80)) {
        *out = b0;
        return true;
    }
    if (*pos >= n) return false;
    const uint8_t b1 = d[(*pos)++];
    *out = (uint32_t)(b0 & 0x7F) | ((uint32_t)(b1 & 0x7F) << 7);
    return true;
}

} // namespace

void mul43Row(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = a[r * 3 + 0] * b[0 + c] + a[r * 3 + 1] * b[3 + c] +
                      a[r * 3 + 2] * b[6 + c];
            if (r == 3) s += b[9 + c];
            out[r * 3 + c] = s;
        }
}

bool parseSkeletonHierarchy(const uint8_t* d, size_t n, BoneParents* out,
                            std::string* err, BoneLocals* locals)
{
    static const uint8_t kMagic[4] = {0xC1, 0x59, 0x41, 0x0D};
    if (n < 24 || std::memcmp(d, kMagic, 4) != 0) {
        if (err) *err = "not a .skeleton property tree (bad magic)";
        return false;
    }
    size_t pos = 12;
    const uint8_t dictCount = d[pos++];
    std::vector<std::string> dict;
    dict.reserve(dictCount);
    for (uint8_t i = 0; i < dictCount; ++i) {
        const void* nul = std::memchr(d + pos, 0, n - pos);
        if (!nul) { if (err) *err = "dictionary truncated"; return false; }
        const size_t end = (const uint8_t*)nul - d;
        dict.emplace_back(reinterpret_cast<const char*>(d + pos), end - pos);
        pos = end + 1;
    }
    ++pos;                                          // u8 zero
    if (pos + 8 > n) { if (err) *err = "header truncated"; return false; }
    uint32_t dataStart;
    std::memcpy(&dataStart, d + pos, 4);
    pos += 8;                                       // dataStart + u32 zero

    uint32_t count = 0;
    if (!readCValue(d, n, &pos, &count) || count == 0) {
        if (err) *err = "bad element count";
        return false;
    }

    std::vector<Node> nodes(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (pos >= n) { if (err) *err = "descriptor stream truncated"; return false; }
        nodes[i].key = d[pos++];
        if (!readCValue(d, n, &pos, &nodes[i].childCount)) {
            if (err) *err = "descriptor varint truncated";
            return false;
        }
    }

    // Element stream (values); we keep only strings.
    size_t ep = 12 + (size_t)dataStart;
    if (ep != pos) {
        // Both derivations must agree — if not, the format shifted under us.
        if (err) *err = "element stream offset mismatch (format change?)";
        return false;
    }
    std::vector<std::string> strings;
    for (uint32_t i = 0; i < count; ++i) {
        if (ep + 2 > n) { if (err) *err = "element stream truncated"; return false; }
        const uint8_t z = d[ep];
        const uint8_t t = d[ep + 1];
        ep += 2;
        if (z != 0) { if (err) *err = "element marker != 0 at " + std::to_string(ep); return false; }
        switch (t) {
            case 0: break;
            case 1: {
                const void* nul = std::memchr(d + ep, 0, n - ep);
                if (!nul) { if (err) *err = "string element truncated"; return false; }
                const size_t end = (const uint8_t*)nul - d;
                nodes[i].value = (int)strings.size();
                strings.emplace_back(reinterpret_cast<const char*>(d + ep), end - ep);
                ep = end + 1;
                break;
            }
            case 2: case 4: ep += 4; break;
            case 3: ep += 1; break;
            case 5: ep += 4; break;
            case 6: {
                if (ep + 4 > n) { if (err) *err = "float array truncated"; return false; }
                uint16_t cnt;
                std::memcpy(&cnt, d + ep, 2);
                ep += 4 + (size_t)cnt * 4;
                break;
            }
            default:
                if (err) *err = "unknown element type " + std::to_string(t);
                return false;
        }
        if (ep > n) { if (err) *err = "element stream overran"; return false; }
    }

    // Breadth-first tree reconstruction from child counts.
    {
        uint32_t next = 1;
        std::deque<uint32_t> queue{0};
        while (!queue.empty()) {
            const uint32_t cur = queue.front();
            queue.pop_front();
            for (uint32_t k = 0; k < nodes[cur].childCount && next < count; ++k) {
                nodes[cur].children.push_back(next);
                queue.push_back(next);
                ++next;
            }
        }
        if (next != count) {
            if (err) *err = "tree reconstruction consumed " + std::to_string(next)
                            + " of " + std::to_string(count);
            return false;
        }
    }

    // Walk every `node` subtree; `identifier` string children name the bones.
    const auto keyName = [&](uint32_t i) -> const std::string& {
        static const std::string empty;
        return nodes[i].key < dict.size() ? dict[nodes[i].key] : empty;
    };
    struct Walk { uint32_t idx; std::string parent; };
    std::deque<Walk> walk;
    for (uint32_t c : nodes[0].children)
        if (keyName(c) == "node") walk.push_back({c, std::string()});
    while (!walk.empty()) {
        const Walk w = walk.front();
        walk.pop_front();
        std::string ident;
        int transformNode = -1;
        for (uint32_t c : nodes[w.idx].children) {
            if (keyName(c) == "identifier" && nodes[c].value >= 0)
                ident = strings[(size_t)nodes[c].value];
            else if (keyName(c) == "transform")
                transformNode = (int)c;
        }
        const std::string& parentForKids = ident.empty() ? w.parent : ident;
        if (!ident.empty()) {
            out->emplace(ident, w.parent);
            // local rest transform: `transform` children row0..row3, each a
            // string of three floats (rows 0-2 rotation, row 3 translation)
            if (locals && transformNode >= 0) {
                BoneLocal bl;
                int rowsSeen = 0;
                for (uint32_t rc : nodes[(uint32_t)transformNode].children) {
                    const std::string& k = keyName(rc);
                    if (k.size() != 4 || k.compare(0, 3, "row") != 0 ||
                        k[3] < '0' || k[3] > '3' || nodes[rc].value < 0)
                        continue;
                    const int row = k[3] - '0';
                    const char* s = strings[(size_t)nodes[rc].value].c_str();
                    char* end = nullptr;
                    bool ok = true;
                    for (int c2 = 0; c2 < 3; ++c2) {
                        const float v = std::strtof(s, &end);
                        if (end == s) { ok = false; break; }
                        bl.m[row * 3 + c2] = v;
                        s = end;
                    }
                    if (ok) rowsSeen |= (1 << row);
                }
                if (rowsSeen == 0xF)
                    locals->emplace(ident, bl);
            }
        }
        for (uint32_t c : nodes[w.idx].children)
            if (keyName(c) == "node") walk.push_back({c, parentForKids});
    }
    if (out->empty()) {
        if (err) *err = "no named nodes found in Root/node tree";
        return false;
    }
    return true;
}

bool worldOfBone(const BoneParents& parents, const BoneLocals& locals,
                 const std::string& name, float out[12])
{
    auto it = locals.find(name);
    if (it == locals.end()) return false;
    std::memcpy(out, it->second.m, 12 * sizeof(float));
    std::string cur = name;
    for (int hop = 0; hop < 256; ++hop) {          // cycle guard: BoneParents
        auto pit = parents.find(cur);              // is first-wins by name
        if (pit == parents.end() || pit->second.empty()) return true;
        auto lit = locals.find(pit->second);
        if (lit == locals.end()) return false;     // broken chain: refuse rather
                                                   // than return a partial world
        float acc[12];
        mul43Row(out, lit->second.m, acc);            // world = local @ parentLocal...
        std::memcpy(out, acc, sizeof(acc));
        cur = pit->second;
    }
    return false;   // hit the hop cap: a name cycle, so the accumulated world
                    // is nonsense — say so instead of placing something wrong
}

} // namespace di
