#include "model/AnimParser.h"

#include <cmath>
#include <unordered_set>
#include <cstring>

#include "store/Zzz4.h"

namespace di {

namespace {

uint16_t rdU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rdU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
float rdF32(const uint8_t* p)
{
    float f;
    std::memcpy(&f, p, 4);
    return f;
}

// 40-bit packed quaternion -> x,y,z,w.
//
// Measured layout (solved 2026-08-01 on 7 real clips): three 12-bit fields at
// bits 0/12/24, the implied-component index at bits 36-37, bits 38-39 zero.
//   value_j = (field_j - 2047.5) / (2048*sqrt(2))          [range +-1/sqrt2]
//   q[(idx - 1 - j) mod 4] = value_j                       [CYCLIC fill]
//   q[idx] = +sqrt(1 - sum of squares)
// i.e. the encoder stores the three components CYCLICALLY FOLLOWING the
// implied one (idx=3/w -> fields are z,y,x; idx=0/x -> w,z,y; ...).
// Verification: selector-transition continuity across clips has median error
// 0.43 deg on slow-motion pairs (was ~180 deg under earlier guesses), Pelvis
// and cloth-root bind matches are exact (0.09/0.04 deg), and the skinned
// golden renders an upright idle figure.
// Shared smallest-three unpack. v2 packs 3x12-bit fields in 40 bits; v1 packs
// 3x15-bit fields in 48 bits (verified 2026-08-10 on 2,131 real v1 keys: bits
// 47 zero and sum-of-squares <= 1 on every key, clean playback continuity;
// the 14-bit alternative violates both on most keys, so the law is not
// ambiguous). Same cyclic fill and positive implied component as v2.
static void unpackSmallest3(uint64_t v, int fbits, float q[4])
{
    const uint32_t mask = (1u << fbits) - 1;
    const int idx = (int)((v >> (3 * fbits)) & 3);
    const double half = (double)(1u << (fbits - 1)) - 0.5;
    const double scale = (double)(1u << (fbits - 1)) * 1.41421356237309515;
    double sq = 0.0;
    q[0] = q[1] = q[2] = 0.0f;
    q[3] = 1.0f;
    for (int j = 0; j < 3; ++j) {
        const double val = ((double)((v >> (fbits * j)) & mask) - half) / scale;
        q[(idx - 1 - j + 4) & 3] = (float)val;
        sq += val * val;
    }
    q[idx] = (float)std::sqrt(sq < 1.0 ? 1.0 - sq : 0.0);
}

void unpackQuat(const uint8_t* p, float q[4])
{
    const uint64_t v = (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
                       ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
                       ((uint64_t)p[4] << 32);
    unpackSmallest3(v, 12, q);
}

void unpackQuat48(const uint8_t* p, float q[4])
{
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v |= (uint64_t)p[i] << (8 * i);
    unpackSmallest3(v, 15, q);
}

} // namespace

// Everything of CHAR::ANIM v3 that has been MEASURED (see AnimParser.h): the
// header, the track name/parent table, and the 30 fps frame-block records.
// Keys are not decoded, so tracks come back with no pos/rot data.
bool probeAnimV3(const uint8_t* d, size_t n, AnimClip* out, std::string* err)
{
    if (n < 64 || std::memcmp(d + 16, "CHAR::ANIM", 10) != 0) {
        if (err) *err = "not a v3 container";
        return false;
    }
    // payloadLen + 128 == n on the three original goldens, but 1,584 real v3
    // files violate it (measured, --anim-verify 2026-08-10) while their track
    // tables parse fine - so it is a note, not a gate.
    out->version    = rdU16(d + 26);
    const uint32_t trackCount = rdU32(d + 28);
    out->durationMs = rdU32(d + 32);
    if (trackCount == 0 || trackCount > 4096) {
        if (err) *err = "implausible track count " + std::to_string(trackCount);
        return false;
    }
    if (rdU16(d + 48) != (uint16_t)trackCount) {
        if (err) *err = "track count mismatch between header and name table";
        return false;
    }
    size_t pos = 50;
    out->tracks.clear();
    out->tracks.reserve(trackCount);
    for (uint32_t t = 0; t < trackCount; ++t) {
        if (pos + 1 > n) { if (err) *err = "v3 name table truncated"; return false; }
        const uint8_t nl = d[pos++];
        // nl == 0 is legal: some rigs carry a nameless root/helper track
        // ([0x00][s16 parent]). Rejecting it dropped a whole family of clips.
        if (pos + (size_t)nl + 2 > n) {
            if (err) *err = "v3 name truncated at track " + std::to_string(t);
            return false;
        }
        AnimTrack tr;
        tr.name.assign(reinterpret_cast<const char*>(d + pos), nl);
        pos += nl;
        tr.parent = (int16_t)rdU16(d + pos);
        pos += 2;
        if (tr.parent >= (int)t) {
            if (err) *err = "v3 forward parent reference in " + tr.name;
            return false;
        }
        out->tracks.push_back(std::move(tr));
    }
    // Frame-block records after the 0xAC10AC10 marker. Verified law: frames
    // are 30 fps, durationMs == (totalFrames - 1) * 1000 / 30.
    static const uint8_t kMarker[4] = {0x10, 0xAC, 0x10, 0xAC};
    size_t marker = (size_t)-1;
    for (size_t p = pos; p + 12 <= n && p < pos + 64; ++p)   // +12: the block
        if (std::memcmp(d + p, kMarker, 4) == 0) { marker = p; break; }
    if (marker == (size_t)-1) {                              // count is read at
        if (err) *err = "v3 block marker not found after the name table";
        return false;                                        // marker+10
    }
    const uint16_t blockCount = rdU16(d + marker + 10);
    if (blockCount == 0) {
        if (err) *err = "v3 block table is empty";
        return false;
    }
    size_t rec = marker + 0x28;
    uint32_t frames = 0;
    for (uint16_t b = 0; b < blockCount; ++b, rec += 20) {
        if (rec + 20 > n) {
            if (err) *err = "v3 block table truncated at block " + std::to_string(b) +
                            " of " + std::to_string(blockCount);
            return false;
        }
        const uint32_t fc = rdU32(d + rec);
        // 16/17 on the goldens, 18 measured on battlepet_serrat (6,380 files
        // failed on that alone). Sanity-range only; the REAL validator is the
        // frames-vs-duration cross-check below.
        if (fc == 0 || fc > 64) {
            if (err) *err = "v3 block " + std::to_string(b) +
                            " has implausible frame count " + std::to_string(fc);
            return false;
        }
        frames += fc;
    }
    if (frames > 1) {
        // fps is NOT always 30: the block header carries it (bs+0x20, i.e.
        // marker+0x18) and real clips ship 24 / 30 / 60. Using a hardcoded 30
        // here falsely rejected every off-speed clip (bosses, fx). The decoder
        // already samples at the block's own fps, so honour it in the gate too.
        uint32_t fps = 30;
        if (marker + 0x1c <= n) {
            const uint32_t f = rdU32(d + marker + 0x18);
            if (f >= 1 && f <= 240) fps = f;
        }
        const uint32_t predicted = (uint32_t)(((frames - 1) * 1000ull + fps / 2) / fps);
        const long diff = (long)predicted - (long)out->durationMs;
        // Tolerance ~2 frames: the stored durationMs is rounded independently of
        // the frame count, so long clips drift a few ms (measured max ~9). A
        // gross mismatch (wrong fps / shifted format) is 100s of ms and still
        // caught; block integrity itself is guaranteed by the FNV checksum in
        // parseV3Frames, so this gate only needs to reject the obviously wrong.
        const long tol = 64;
        if (diff > tol || diff < -tol) {
            if (err) *err = "v3 frame/duration mismatch (" + std::to_string(frames) +
                            " frames at " + std::to_string(fps) + " fps predict " +
                            std::to_string(predicted) + " ms, header says " +
                            std::to_string(out->durationMs) + ")";
            return false;
        }
    }
    return true;
}

// ── CHAR::ANIM v3 frame-block codec (decoded from the game binary) ─────────
// The value path was reverse-engineered from the running decoder (Ghidra on the
// process dump): sampler FUN_..40e0 -> per-channel readers 582e0 (rotation),
// 58ae0 / 587c0 (linear). Rotation is a DROP-W smallest-three quaternion (the
// 4th component is always the reconstructed w = sqrt(1-x^2-y^2-z^2), NOT a
// selected largest — this corrects an earlier misread). Per animated channel,
// per frame, three components are decoded then de-quantized in two affine
// steps: a per-track/segment base+range in unsigned bytes/255, and a per-track
// bias+scale from table E. Validated: serrat a_idle decodes to unit-norm quats
// with a perfect looping idle (max inter-frame 7.3 deg over 7,865 keys).
//
// Constants read from .rdata: rotation base/range scale = 1/255, raw-int16
// scale = 1/65535, width W -> [scale=1/(2^W-1), mask=2^W-1]. Type byte -> W via
// the table at 0x317b2a0: {0:0, 1..0x11:3..19, 0x12:32(full float)}.
static const uint8_t kV3TypeWidth[0x13] = {
    0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 32};

// Generic 3-component variable-width reader (port of FUN_..d6cc70): each
// component is W bits from a BIG-ENDIAN bitstream, normalized to [0,1].
static void v3ReadWidth(const uint8_t* dreg, size_t dregLen, int W, uint32_t bit,
                        float out[3])
{
    const uint32_t mask = (W >= 32) ? 0xffffffffu : ((1u << W) - 1u);
    const float scale = (W <= 0) ? 1.0f : 1.0f / (float)((1u << W) - 1u);
    for (int j = 0; j < 3; ++j) {
        const uint32_t bo = bit + (uint32_t)j * (uint32_t)W;
        const size_t byte = bo >> 3;
        uint32_t dw = 0;
        if (byte + 4 <= dregLen) dw = rdU32(dreg + byte);
        else for (int k = 0; k < 4 && byte + k < dregLen; ++k)
            dw |= (uint32_t)dreg[byte + k] << (8 * k);
        const uint32_t be = (dw << 24) | ((dw & 0xff00u) << 8) |
                            ((dw >> 8) & 0xff00u) | (dw >> 24);   // bswap32
        const uint32_t raw = be >> (32 - W - (int)(bo & 7));
        out[j] = (float)(raw & mask) * scale;
    }
}

// type 0x12 path: three raw float32, bit-packed big-endian (port of the 0x12
// branch in 582e0). Rare full-precision keys.
static void v3ReadFloat32(const uint8_t* dreg, size_t dregLen, uint32_t bit,
                          float out[3])
{
    const size_t byte = bit >> 3;
    const int sh = (int)(bit & 7);
    for (int j = 0; j < 3; ++j) {
        uint64_t u = 0;
        for (int k = 0; k < 8; ++k)
            if (byte + 4 * j + k < dregLen)
                u |= (uint64_t)dreg[byte + 4 * j + k] << (8 * k);
        uint64_t be = 0;                                          // bswap64
        for (int k = 0; k < 8; ++k) be |= ((u >> (8 * k)) & 0xff) << (8 * (7 - k));
        const uint32_t v = (uint32_t)((be << sh) >> 32);
        float f;
        std::memcpy(&f, &v, 4);
        out[j] = f;
    }
}

// Decode all v3 rotation tracks into out->tracks[*].rot (dense per-frame keys;
// AnimClip::sample interpolates). Returns false on any structural OOB (refuse
// rather than emit garbage). `out->tracks` must already be filled by
// probeAnimV3. `marker` is the file offset of the 0xAC10AC10 tag.
static bool parseV3Frames(const uint8_t* d, size_t n, size_t marker, AnimClip* out)
{
    const size_t bs = marker - 8;                 // block start (see NOTES)
    const size_t puVar1 = bs + 0x10;
    if (bs + 0x30 > n) return false;
    const uint32_t blockLen  = rdU32(d + bs);
    const uint16_t trackCount = rdU16(d + bs + 0x10);
    const uint16_t K          = rdU16(d + bs + 0x12);
    const uint32_t frameCount = rdU32(d + bs + 0x1c);
    const uint32_t fps        = rdU32(d + bs + 0x20);
    const uint8_t  segFlag    = d[bs + 0x18];
    const uint8_t  flag19     = d[bs + 0x19];
    const int cpt = (flag19 != 0 ? 1 : 0) + 2;    // presence bits per track
    // Channel roles (measured across the archive): 0 = rotation, 1 = local
    // translation, 2 = scale (when a third channel is enabled; ~[1,1,1], and
    // the pose has no scale slot so it is decoded-and-skipped, not emitted as
    // position). Position is ALWAYS channel 1.
    const uint32_t posChan = 1u;
    if (fps == 0 || frameCount == 0 || trackCount == 0 || K == 0) return false;
    // Defensive cap: the decoder emits up to one key per channel per frame, so a
    // corrupt header (huge frameCount/trackCount that still passes the
    // frame/duration check) could try to allocate billions of keys and exhaust
    // memory. Real clips peak near 85k channel-frames; refuse anything absurd.
    if ((uint64_t)frameCount * (uint64_t)cpt * (uint64_t)trackCount > 4000000ull)
        return false;
    if (trackCount != out->tracks.size()) return false;
    if (bs + 0x2c + 2 > n) return false;
    const uint16_t offRec = rdU16(d + bs + 0x24);
    const uint16_t offB   = rdU16(d + bs + 0x26);   // table B (absent mask)
    const uint16_t offC   = rdU16(d + bs + 0x28);   // table C (hold mask)
    const uint16_t offD   = rdU16(d + bs + 0x2a);   // table D (rest pose)
    const uint16_t offE   = rdU16(d + bs + 0x2c);   // table E (bias/scale)
    const size_t recBase = puVar1 + offRec;
    const size_t tblB = puVar1 + offB, tblC = puVar1 + offC, tableE = puVar1 + offE;
    const size_t tblD = (offD != 0xffff) ? puVar1 + offD : 0;   // 12 B/track
    if (recBase + (size_t)K * 20 > n) return false;

    struct Seg { uint32_t fc, bpf, b, c, dd; };
    std::vector<Seg> segs(K);
    for (uint16_t r = 0; r < K; ++r) {
        const size_t p = recBase + (size_t)r * 20;
        segs[r] = {rdU32(d + p), rdU32(d + p + 4), rdU32(d + p + 8),
                   rdU32(d + p + 12), rdU32(d + p + 16)};
    }
    // presence bit test: MSB-first within LE-loaded u32 words.
    auto presBit = [&](size_t table, uint32_t idx) -> bool {
        const size_t wa = table + (size_t)(idx >> 5) * 4;
        if (wa + 4 > n) return true;              // OOB -> treat as absent/skip
        const uint32_t word = rdU32(d + wa);
        return (word & (1u << (31 - (idx & 31)))) != 0;
    };
    const float ROT_S = 1.0f / 255.0f;
    const float RAW_S = 1.0f / 65535.0f;

    for (AnimTrack& t : out->tracks) t.rot.clear();

    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        // locate segment covering this frame
        uint32_t st = 0, si = 0, fis = 0;
        bool found = false;
        for (uint16_t r = 0; r < K; ++r) {
            if (frame < st + segs[r].fc) { si = r; fis = frame - st; found = true; break; }
            st += segs[r].fc;
        }
        if (!found) { si = K - 1; fis = segs[si].fc ? segs[si].fc - 1 : 0; }
        const Seg& sg = segs[si];
        const size_t breg = puVar1 + sg.b, creg = puVar1 + sg.c, dreg = puVar1 + sg.dd;
        const size_t dregLen = (dreg <= n) ? n - dreg : 0;
        const uint16_t timeMs = (uint16_t)(((uint64_t)frame * 1000ull + fps / 2) / fps);

        uint32_t keyCounter = 0, dPrefix = 0, cCur = 0, eCur = 0;
        const uint32_t nChan = (uint32_t)cpt * trackCount;
        for (uint32_t ch = 0; ch < nChan; ++ch) {
            if (presBit(tblB, ch) || presBit(tblC, ch)) continue;   // absent/hold
            const uint32_t track = ch / cpt, chan = ch % cpt;
            if (breg + keyCounter >= n) return false;
            const uint8_t tb = d[breg + keyCounter];
            const int W = (tb < 0x13) ? kV3TypeWidth[tb] : 0;

            // Decode this channel's 3 components (shared by rotation + position).
            // ok=false on OOB. skipBiasScale set for the raw-float type.
            float comp[3] = {0, 0, 0};
            bool ok = true, skipBiasScale = false;
            if (chan == 0 || chan == posChan) {
                if (tb == 0) {                       // raw UNSIGNED u16/65535 -> [0,1]
                    if (creg + cCur + 8 > n) ok = false;
                    else for (int j = 0; j < 3; ++j)
                        comp[j] = (float)rdU16(d + creg + cCur + 2 * j) * RAW_S;
                } else if (tb == 0x12) {             // 3x float32
                    v3ReadFloat32(d + dreg, dregLen, fis * sg.bpf + dPrefix, comp);
                    skipBiasScale = true;
                } else {                             // width-coded residual
                    float res[3];
                    v3ReadWidth(d + dreg, dregLen, W, fis * sg.bpf + dPrefix, res);
                    if (segFlag & (1u << chan)) {    // base+range (bytes/255)
                        if (creg + cCur + 6 > n) ok = false;
                        else for (int j = 0; j < 3; ++j) {
                            const float base = (float)d[creg + cCur + j] * ROT_S;
                            const float rng  = (float)d[creg + cCur + 3 + j] * ROT_S;
                            comp[j] = res[j] * rng + base;
                        }
                    } else {
                        for (int j = 0; j < 3; ++j) comp[j] = res[j];
                    }
                }
                if (ok && !skipBiasScale && tableE + eCur + 24 <= n) {
                    for (int j = 0; j < 3; ++j) {    // per-track bias+scale (table E)
                        const float bias  = rdF32(d + tableE + eCur + 4 * j);
                        const float scale = rdF32(d + tableE + eCur + 12 + 4 * j);
                        comp[j] = comp[j] * scale + bias;
                    }
                }
            }
            if (!ok) return false;

            if (chan == 0) {                          // rotation: reconstruct w
                const float s = comp[0]*comp[0] + comp[1]*comp[1] + comp[2]*comp[2];
                float w = s < 1.0f ? std::sqrt(1.0f - s) : 0.0f;
                // Low-precision (e.g. 3-bit) keys can push |xyz| slightly past 1
                // after de-quant; renormalize so every stored key is a valid unit
                // quaternion (matters for glTF export; render normalizes anyway).
                const float len = std::sqrt(s + w * w);
                const float inv = len > 1e-6f ? 1.0f / len : 1.0f;
                AnimRotKey k;
                k.timeMs = timeMs;
                k.q[0] = comp[0] * inv; k.q[1] = comp[1] * inv;
                k.q[2] = comp[2] * inv; k.q[3] = w * inv;
                out->tracks[track].rot.push_back(k);
            } else if (chan == posChan) {             // animated local translation
                AnimPosKey k;
                k.timeMs = timeMs;
                k.p[0] = comp[0]; k.p[1] = comp[1]; k.p[2] = comp[2];
                out->tracks[track].pos.push_back(k);
            }
            // advance cursors for EVERY animated channel (rot + linear)
            keyCounter += 1;
            dPrefix    += (uint32_t)W * 3;
            eCur       += 24;
            if (segFlag & (1u << chan)) cCur += 6;
        }
    }
    // Tracks whose position channel is absent carry NO pos keys; the pose player
    // fills their translation from the authoritative .skeleton rest pose.
    (void)blockLen;
    (void)tblD;
    return true;
}

// ── the version-2 track stream (bounded walk) ──────────────────────────────
// Parses [begin,end) as a v2 track stream. Used for BOTH containers: the
// classic 4-byte-magic file (stream at 36, mask at 25) and the 16-byte
// preamble container's CHANNEL_DATA section when the inner version is <= 2
// (measured 2026-08-10: those 9,025 "track count mismatch" files are plain v2
// streams at the section start, everything shifted +12 by the bigger
// preamble). All the layout rules live here once.
static bool parseV2Stream(const uint8_t* d, size_t begin, size_t end,
                          uint32_t trackCount, uint8_t channelMask, AnimClip* out)
{
    const int preferred = (channelMask & 1) ? 2 : 1;
    const int order[4] = {preferred, preferred == 2 ? 1 : 2, 0, 3};
    const size_t n = end;

    out->tracks.assign(trackCount, AnimTrack());
    std::vector<uint8_t> chosen(trackCount, 0);

    auto tryTrack = [&](uint32_t t, size_t pos, int arrays, size_t* nextPos) {
        if (pos + 1 > n) return false;
        const uint8_t nl = d[pos++];
        // nl == 0 is legal: some rigs carry a nameless root/helper track near the
        // end of the stream. Rejecting it left a family of clips unparseable
        // (measured: the only reason ~58 v2 clips refused). Structure after it
        // (parent, key arrays, 0x32 rot block) still fully constrains the parse.
        if (pos + (size_t)nl + 2 > n) return false;
        AnimTrack& tr = out->tracks[t];
        tr.name.assign(reinterpret_cast<const char*>(d + pos), nl);
        pos += nl;
        tr.parent = (int16_t)rdU16(d + pos);
        pos += 2;
        if (tr.parent >= (int)t) return false;
        tr.pos.clear();
        tr.rot.clear();
        for (int a = 0; a < arrays; ++a) {
            if (pos + 4 > n) return false;
            const uint32_t kc = rdU32(d + pos);
            pos += 4;
            if (kc > 100000 || pos + (size_t)kc * 14 > n) return false;
            if (a == arrays - 1) {
                tr.pos.resize(kc);
                for (uint32_t i = 0; i < kc; ++i) {
                    const uint8_t* k = d + pos + (size_t)i * 14;
                    tr.pos[i].timeMs = rdU16(k);
                    tr.pos[i].p[0] = rdF32(k + 2);
                    tr.pos[i].p[1] = rdF32(k + 6);
                    tr.pos[i].p[2] = rdF32(k + 10);
                }
            }
            pos += (size_t)kc * 14;
        }
        if (pos + 2 > n) return false;
        const uint16_t rc = rdU16(d + pos);
        pos += 2;
        // rc == 0 carries NO tag byte (measured; see header note).
        if (rc == 0) {
            *nextPos = pos;
            return true;
        }
        if (pos + 1 > n) return false;
        const uint8_t tag = d[pos++];
        if (tag != 0x32 || pos + (size_t)rc * 7 > n) return false;
        tr.rot.resize(rc);
        for (uint32_t i = 0; i < rc; ++i) {
            const uint8_t* k = d + pos + (size_t)i * 7;
            tr.rot[i].timeMs = rdU16(k);
            unpackQuat(k + 2, tr.rot[i].q);
        }
        pos += (size_t)rc * 7;
        *nextPos = pos;
        return true;
    };

    struct Frame { size_t pos; int cand; };
    for (int pass = 0; pass < 2; ++pass) {
        std::unordered_set<uint64_t> dead;
        std::vector<Frame> stk;
        stk.push_back({begin, 0});
        while (!stk.empty()) {
            const uint32_t t = (uint32_t)(stk.size() - 1);
            if (t == trackCount) {
                const size_t leftover = n - stk.back().pos;
                if (pass == 1 || leftover <= 160) {
                    uint8_t mn = 255, mx = 0;
                    for (uint8_t c : chosen) { mn = c < mn ? c : mn; mx = c > mx ? c : mx; }
                    out->keyArrays   = trackCount ? mx : 1;
                    out->mixedArrays = trackCount ? (mn != mx) : false;
                    return true;
                }
                stk.pop_back();
                continue;
            }
            Frame& f = stk.back();
            const uint64_t key = ((uint64_t)t << 40) | (uint64_t)f.pos;
            bool advanced = false;
            if (!dead.count(key)) {
                while (f.cand < 4) {
                    size_t nxt = 0;
                    const int arrays = order[f.cand++];
                    if (tryTrack(t, f.pos, arrays, &nxt)) {
                        chosen[t] = (uint8_t)arrays;
                        stk.push_back({nxt, 0});
                        advanced = true;
                        break;
                    }
                }
                if (!advanced && f.cand >= 4) dead.insert(key);
            }
            if (!advanced && !stk.empty() && stk.back().cand >= 4)
                stk.pop_back();
            else if (!advanced && !stk.empty() && dead.count(key))
                stk.pop_back();
        }
    }
    out->tracks.clear();
    return false;
}

// ── the version-1 track stream ──────────────────────────────────────────────
// Layout solved 2026-08-10 by exact-extent arithmetic on both shipped v1
// goldens (spans between located track names left zero unexplained bytes):
//   u8 nameLen, name            (NO parent field - the rig comes from the
//                                skeleton; the player matches by name)
//   u32 posKeyCount, keys of [u16 timeMs][3 x f32]      (14 B, same as v2)
//   u16 rotKeyCount, u16 tag (0x4000 on every measured track),
//     keys of [u16 timeMs][48-bit smallest-three quaternion]  (8 B)
static bool parseV1Stream(const uint8_t* d, size_t begin, size_t end,
                          uint32_t trackCount, AnimClip* out, std::string* err)
{
    size_t pos = begin;
    out->tracks.assign(trackCount, AnimTrack());
    for (uint32_t t = 0; t < trackCount; ++t) {
        AnimTrack& tr = out->tracks[t];
        if (pos + 1 > end) { if (err) *err = "v1 track header truncated"; return false; }
        const uint8_t nl = d[pos++];
        if (nl == 0 || pos + nl + 4 > end) {
            if (err) *err = "v1 name truncated at track " + std::to_string(t);
            return false;
        }
        tr.name.assign(reinterpret_cast<const char*>(d + pos), nl);
        pos += nl;
        tr.parent = -1;
        const uint32_t pc = rdU32(d + pos);
        pos += 4;
        if (pc > 100000 || pos + (size_t)pc * 14 > end) {
            if (err) *err = "v1 pos keys truncated in " + tr.name;
            return false;
        }
        tr.pos.resize(pc);
        for (uint32_t i = 0; i < pc; ++i) {
            const uint8_t* k = d + pos + (size_t)i * 14;
            tr.pos[i].timeMs = rdU16(k);
            tr.pos[i].p[0] = rdF32(k + 2);
            tr.pos[i].p[1] = rdF32(k + 6);
            tr.pos[i].p[2] = rdF32(k + 10);
        }
        pos += (size_t)pc * 14;
        if (pos + 4 > end) { if (err) *err = "v1 rot header truncated in " + tr.name; return false; }
        const uint16_t rc  = rdU16(d + pos);
        const uint16_t tag = rdU16(d + pos + 2);
        pos += 4;
        if (tag != 0x4000) {
            if (err) *err = "v1 rot tag 0x" + std::to_string(tag) + " in " + tr.name +
                            " (0x4000 on every measured track) - refusing";
            return false;
        }
        if (pos + (size_t)rc * 8 > end) {
            if (err) *err = "v1 rot keys truncated in " + tr.name;
            return false;
        }
        tr.rot.resize(rc);
        for (uint32_t i = 0; i < rc; ++i) {
            const uint8_t* k = d + pos + (size_t)i * 8;
            tr.rot[i].timeMs = rdU16(k);
            unpackQuat48(k + 2, tr.rot[i].q);
        }
        pos += (size_t)rc * 8;
    }
    out->keyArrays = 1;
    return true;
}

// ── the 16-byte-preamble container's section directory ──────────────────────
// Measured 2026-08-10 (byte-exact on 15/15 sampled files): u32 at offset 4 is
// the DIRECTORY offset; from there to EOF run 32-byte entries
// [u32 size][8 unused][u32 nameLen][name, padded]. Sections follow the 16-byte
// preamble in directory order, each 16-byte aligned: HEADER (32 bytes:
// "CHAR::ANIM", inner version, trackCount, durationMs, 12 mask/stamp bytes),
// CHANNEL_DATA (the track stream), MOTION_DATA, TRACKS_DATA. This also
// explains the old "+128" law: 4 entries x 32 bytes of directory.
static bool findChannelData(const uint8_t* d, size_t n, size_t* chBegin,
                            size_t* chEnd)
{
    const uint32_t dirOff = rdU32(d + 4);
    if (dirOff <= 16 || dirOff >= n || (n - dirOff) % 32) return false;
    size_t pos = 16;
    for (size_t p = dirOff; p + 32 <= n; p += 32) {
        const uint32_t size = rdU32(d + p);
        const uint32_t nl   = rdU32(d + p + 12);
        if (nl < 1 || nl > 15) return false;
        const char* nm = (const char*)d + p + 16;
        if (size > n) return false;
        if (nl == 12 && std::memcmp(nm, "CHANNEL_DATA", 12) == 0) {
            *chBegin = pos;
            *chEnd   = pos + size;
            return *chEnd <= n;
        }
        pos = (pos + size + 15) & ~(size_t)15;
    }
    return false;
}

bool parseAnim(const uint8_t* data, size_t len, AnimClip* out, std::string* err)
{
    std::vector<uint8_t> inflated;
    const uint8_t* d = data;
    size_t n = len;
    if (n >= 8 && isZzz4(data, n)) {
        inflated = inflateZzz4(data, n);
        if (inflated.empty()) {
            if (err) *err = "anim ZZZ4 inflate failed";
            return false;
        }
        d = inflated.data();
        n = inflated.size();
    }
    if (n >= 48 && std::memcmp(d + 16, "CHAR::ANIM", 10) == 0) {
        // 16-byte-preamble container (HEADER / CHANNEL_DATA / ... sections).
        const uint16_t innerVer = rdU16(d + 26);
        const uint32_t trackCount = rdU32(d + 28);
        out->version    = innerVer;
        out->durationMs = rdU32(d + 32);
        if (trackCount > 4096) {
            if (err) *err = "implausible track count " + std::to_string(trackCount);
            return false;
        }
        if (innerVer == 3) {
            // The real v3: CHANNEL_DATA is a 30 fps frame-block codec. Value
            // path decoded from the game binary (see parseV3Frames). Build the
            // track table, then decode dense per-frame rotation keys.
            std::string perr;
            if (!probeAnimV3(d, n, out, &perr)) {
                if (err) *err = "CHAR::ANIM version 3 (" + perr + ")";
                return false;
            }
            // Re-locate the 0xAC10AC10 marker after the name table.
            static const uint8_t kMk[4] = {0x10, 0xAC, 0x10, 0xAC};
            size_t mk = (size_t)-1;
            for (size_t p = 50; p + 12 <= n; ++p)
                if (std::memcmp(d + p, kMk, 4) == 0 && rdU16(d + p + 4) == 3) {
                    mk = p; break;                 // +4: inner version word (==3)
                }
            if (mk == (size_t)-1) {
                if (err) *err = "CHAR::ANIM v3: block marker vanished";
                return false;
            }
            if (!parseV3Frames(d, n, mk, out)) {
                if (err) *err = "CHAR::ANIM v3: frame decode failed (structure shifted)";
                return false;
            }
            out->keyArrays = 1;
            return true;
        }
        // Inner version 1/2: the section holds the same track stream as the
        // classic container. Bound the walk to CHANNEL_DATA when the directory
        // parses; fall back to the whole file otherwise (the walk tolerates a
        // trailing tail via its lenient pass).
        size_t chBegin = 48, chEnd = n;
        findChannelData(d, n, &chBegin, &chEnd);
        if (trackCount == 0) { out->tracks.clear(); out->keyArrays = 1; return true; }
        const bool ok =
            innerVer == 1
                ? parseV1Stream(d, chBegin, chEnd, trackCount, out, err)
                : parseV2Stream(d, chBegin, chEnd, trackCount, d[37], out);
        if (!ok && innerVer != 1 && err)
            *err = "no key-array layout parses this clip (16B container, channel mask 0x" +
                   std::to_string((int)d[37]) + ", " + std::to_string(trackCount) +
                   " tracks) - the format shifted";
        return ok;
    }
    if (n < 40 || std::memcmp(d + 4, "CHAR::ANIM", 10) != 0) {
        if (err) *err = "not a CHAR::ANIM blob";
        return false;
    }
    out->version = rdU16(d + 14);
    const uint32_t trackCount = rdU32(d + 16);
    out->durationMs = rdU32(d + 20);
    if (trackCount > 4096) {
        if (err) *err = "implausible track count " + std::to_string(trackCount);
        return false;
    }
    if (trackCount == 0) { out->tracks.clear(); out->keyArrays = 1; return true; }
    if (out->version == 1)
        return parseV1Stream(d, 36, n, trackCount, out, err);
    if (parseV2Stream(d, 36, n, trackCount, d[25], out)) return true;
    if (err)
        *err = "no key-array layout parses this clip (channel mask 0x" +
               std::to_string((int)d[25]) + ", " +
               std::to_string(trackCount) + " tracks) - the format shifted";
    return false;
}

void AnimClip::sample(size_t trackIdx, float tMs, float outPos[3], float outQuat[4]) const
{
    outPos[0] = outPos[1] = outPos[2] = 0.0f;
    outQuat[0] = outQuat[1] = outQuat[2] = 0.0f;
    outQuat[3] = 1.0f;
    if (trackIdx >= tracks.size()) return;
    const AnimTrack& tr = tracks[trackIdx];

    if (!tr.pos.empty()) {
        size_t i = 0;
        while (i + 1 < tr.pos.size() && tr.pos[i + 1].timeMs <= tMs) ++i;
        const AnimPosKey& a = tr.pos[i];
        const AnimPosKey& b = tr.pos[i + 1 < tr.pos.size() ? i + 1 : i];
        const float span = (float)(b.timeMs - a.timeMs);
        const float f = span > 0 ? std::fmin(1.0f, std::fmax(0.0f, (tMs - a.timeMs) / span)) : 0.0f;
        for (int c = 0; c < 3; ++c)
            outPos[c] = a.p[c] + (b.p[c] - a.p[c]) * f;
    }
    if (!tr.rot.empty()) {
        size_t i = 0;
        while (i + 1 < tr.rot.size() && tr.rot[i + 1].timeMs <= tMs) ++i;
        const AnimRotKey& a = tr.rot[i];
        const AnimRotKey& b = tr.rot[i + 1 < tr.rot.size() ? i + 1 : i];
        const float span = (float)(b.timeMs - a.timeMs);
        float f = span > 0 ? std::fmin(1.0f, std::fmax(0.0f, (tMs - a.timeMs) / span)) : 0.0f;
        // nlerp with hemisphere fix
        float dot = 0;
        for (int c = 0; c < 4; ++c) dot += a.q[c] * b.q[c];
        const float s = dot < 0 ? -1.0f : 1.0f;
        float q[4], len = 0;
        for (int c = 0; c < 4; ++c) {
            q[c] = a.q[c] + (s * b.q[c] - a.q[c]) * f;
            len += q[c] * q[c];
        }
        len = std::sqrt(len);
        if (len > 1e-12f)
            for (int c = 0; c < 4; ++c) outQuat[c] = q[c] / len;
    }
}

} // namespace di
