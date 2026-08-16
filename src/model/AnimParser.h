#pragma once
// CHAR::ANIM parser — the .anim files (37,769 in the MPK by name, plus the
// repository's 141 Animation entries).
//
// CONTAINERS (both measured byte-exact):
//  * classic: u32 magic 42A14E65, "CHAR::ANIM" @4, u16 version @14,
//    u32 trackCount @16, u32 durationMs @20, 12 mask/stamp bytes @24,
//    track stream @36. Versions 1 and 2 ship in this container.
//  * 16-byte preamble: u32 magic 4142A14E, u32 DIRECTORY offset @4 (not a
//    payload length — solved 2026-08-10), 8 zero bytes, then "CHAR::ANIM"
//    @16 with the same header fields shifted +12. The u32 @4 points at a
//    section directory of 32-byte entries [u32 size][8 unused][u32 nameLen]
//    [name]; sections follow the preamble in order, 16-byte aligned:
//    HEADER (32 B), CHANNEL_DATA (the track stream), MOTION_DATA,
//    TRACKS_DATA. Verified aligned on 15/15 sampled files — this also
//    explains the old "+128" observation (4 entries x 32 B).
//    Inner version 2 here is the SAME v2 track stream (those were the 9,025
//    "track count mismatch" refusals); inner version 3 is the frame-block
//    codec, still undecoded.
//
// Version-2 track stream, measured on two goldens
// (a_idle_f_barbarian 84 tracks, a_dying 227 tracks), 2026-08-01:
//
//   ZZZ4 -> u32 6542'4EA1'?? magic bytes 65 4E A1 42, "CHAR::ANIM",
//   u16 version (2), u32 trackCount, u32 durationMs, 12 unknown bytes,
//   then trackCount x track at offset 36:
//     u8  nameLen, name          ("Bip001 L Forearm", "HP_head", ...)
//     s16 parentIndex            (-1 = root; matches the rig hierarchy)
//     u32 posKeyCount, keys of [u16 timeMs][f32 x][f32 y][f32 z]   (local
//         translation; verified: matches the .skeleton bind row3 exactly for
//         non-animated bones)
//     u16 rotKeyCount; IF rotKeyCount > 0: u8 tag (0x32 on every measured
//         track), then keys of [u16 timeMs][40-bit LE packed quaternion].
//         rotKeyCount == 0 carries NO tag byte (measured 2026-08-10: the
//         census's entire "unknown tag 1..35" family was rc==0 tracks whose
//         "tag" was the next track's name length; byte-verified on
//         bw_building_254_item_ani/a_dying_idle)
//   117-byte constant footer ("HEADER" + 2 checksum-ish bytes) — ignored.
//
// TWO KEY ARRAYS (measured 2026-08-10 over all 37,769 clips, di_probe
// --anim-forensics). A track carries ONE u32-counted 14-byte key array before
// the rot block in 5,520 files and TWO in 8,461. Bit 0 of the channel mask at
// offset 25 selects which — mask 0x02/0x06 -> one array, 0x03/0x07 -> two
// (the u32 at offset 26 is an export timestamp, not part of the mask). The
// LAST array is translation, verified byte-level on
// Char/mon/devil_assassin_1/ani/a_dying3_float1.anim: four keys at t =
// 0/33/66/100 ms against a 101 ms duration, Y drifting upward through a
// "dying_float", with the 0x32 rot block starting immediately after.
// Reading only the first array is what made every non-player clip land
// mid-stream and report a bogus "unknown rot key tag" — the tag byte was never
// a tag, it was the third byte of the NEXT array's u32 count. 2,410 files
// parse under NO uniform per-file count — the count varies PER TRACK there
// (consistent with the 401 files mixing clean and misparsed tracks). The
// parser therefore backtracks per track with a dead-state memo, mask bit as
// the preference order only; a layout is accepted when the WHOLE remaining
// file parses with every track landing on tag 0x32 with in-range counts.
//
// 40-bit quaternion — FINAL LAW (this supersedes two earlier "verified"
// readings that were coincidences of the Pelvis bone's x==z symmetry):
//   bits 0-11 / 12-23 / 24-35: three 12-bit fields; bits 36-37: index of the
//   IMPLIED (largest) component in (x,y,z,w) order; bits 38-39 always 0.
//   value_j = (field_j - 2047.5) / (2048*sqrt(2))     [range +-1/sqrt2]
//   q[(idx - 1 - j) mod 4] = value_j                  [CYCLIC fill: the three
//   stored components FOLLOW the implied one cyclically]
//   q[idx] = +sqrt(1 - sum of squares); all signs positive.
// Verification: residual-rotation clustering between selector classes plus
// slow-motion transition pairs across 7 clips -> median continuity error
// 0.43 deg (earlier laws: ~180 deg); idx1<->idx2 mutual residual ~1e-3;
// Pelvis / cloth-root bind matches 0.09 / 0.04 deg; renders upright idles.
// METHOD WARNING: bind-pose-only fits MISLEAD on this format — use transition
// continuity across many keys.
//
// The decoded quaternion is a STANDARD COLUMN-convention rotation. Downstream
// math is ROW-vector (v * M), matching the .skeleton tree and SkinSkeleton:
// world = local @ parentWorld and invBind @ bindWorld == I (verified to 6e-6
// on all 57 golden skin bones), so AnimPose uses qmat(q)^T as the local row
// block, while a glTF node rotation takes the decoded quaternion DIRECTLY.
//
// ---------------------------------------------------------------------------
// CHAR::ANIM VERSION 3 (the a_fashion_* clips, 253 files) — partially cracked
// 2026-08-02 on a_fashion_idle (236 tracks), v3_barb_illidan (158) and
// f_necromancer a_fashion (238). VERIFIED so far:
//   16-byte preamble: u32 0x4142A14E, u32 payloadLen (== inflatedLen - 128,
//     exact on all three), 8 zero bytes
//   @16 "CHAR::ANIM", @26 u16 version (3), @28 u32 trackCount,
//   @32 u32 durationMs, @36 12 unknown bytes, @48 u16 trackCount (repeated)
//   @50 name/parent table: trackCount x [u8 nameLen][name][s16 parentIdx]
//     (parses cleanly to real bone names on all three; parents precede kids)
//   then padding to a u32 marker 0xAC10AC10, followed by
//     u32 3, u16 trackCount, u16 K, 7 bytes 04 03 03 07 07 01 01 (suspected
//     bit widths), a rig-constant u32 (146 barbarian / 166 necromancer),
//     u32 30 (== the frame rate, see below), then six u16 rig constants
//   @marker+0x28: K x 20-byte block records [u32 frameCount][u32 a]
//     [u32 offB][u32 offC][u32 offD], offsets absolute into the inflated blob
//   FRAME LAW (verified): frameCount is 16 or 17; sum over blocks == total
//     frames; durationMs == round((frames - 1) * 1000 / 30) to within 1 ms on
//     all three files (146 frames / 4834 ms twice, 166 / 5501). So v3 is a
//     fixed 30 fps frame-block format, not a keyframe format.
//   BLOCK LAYOUT: per-segment record [u32 fc][u32 bitsPerFrame][u32 b][u32 c]
//     [u32 d] (offsets relative to blockStart+0x10). b = per-channel TYPE
//     bytes; c = per-channel base+range (3+3 unsigned bytes, /255); d = the
//     dense per-frame residual bitstream (big-endian). Table B/C = per-channel
//     absent/hold presence masks; table E = per-channel bias+scale f32 pairs.
//
// v3 CODEC — DECODED 2026-08-11 from the game binary (Ghidra on the process
// dump; sampler FUN_..40e0, readers 582e0/58ae0/587c0, width reader d6cc70).
// Rotation is a DROP-W smallest-three quaternion: three components are decoded
// per animated channel per frame and w = sqrt(1 - x^2-y^2-z^2) is reconstructed
// (the 4th is ALWAYS w, not a selected largest — this corrects the earlier
// "same as v2 40-bit" reading). Component pipeline: width-coded residual in
// [0,1] (W bits from the table 0x317b2a0 by type byte; scale 1/(2^W-1)), then
// affine base+range (bytes/255), then per-track bias+scale (table E). Type 0 =
// raw int16*(1/65535); type 0x12 = raw float32. Channels are indexed per frame
// by walking the presence tables; the d-region is dense (fc*bitsPerFrame bits).
// See parseV3Frames() in the .cpp and DI_ANIM_V3_NOTES.md for the full law.
// VALIDATED: serrat a_idle -> unit-norm quats, perfect looping idle (max
// inter-frame 7.3 deg over 7,865 keys); the C++ path matches the Python
// reference decoder byte-for-byte. Position/scale channels and held-channel
// constants are not yet emitted (bones fall back to bind, as v2 does).

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace di {

struct AnimPosKey {
    uint16_t timeMs = 0;
    float    p[3]   = {0, 0, 0};
};

struct AnimRotKey {
    uint16_t timeMs = 0;
    float    q[4]   = {0, 0, 0, 1};   // x,y,z,w — already unpacked
};

struct AnimTrack {
    std::string name;
    int16_t     parent = -1;          // index into AnimClip::tracks
    std::vector<AnimPosKey> pos;
    std::vector<AnimRotKey> rot;
};

struct AnimClip {
    uint16_t version    = 0;
    uint32_t durationMs = 0;
    uint8_t  keyArrays  = 1;   // max 14-byte key arrays any track carried
                               // before its rot block (see the v2 note above)
    bool     mixedArrays = false;   // tracks in this clip differ in array count
    std::vector<AnimTrack> tracks;

    // Linear pos / normalized-lerp rot sample at t (clamped to key range).
    // trackIdx out of range or empty tracks yield identity.
    void sample(size_t trackIdx, float tMs, float outPos[3], float outQuat[4]) const;
};

// Merge src's tracks into dst so ONE clip drives several parts (a body plus its
// attachments, or every piece of an outfit).
//
// Two things make this more than a vector append, and getting either wrong
// writes a broken rig:
//   * AnimTrack::parent is an index into ITS OWN clip's track vector. Copied
//     verbatim into a longer vector it silently names a different bone, and the
//     exporter treats clip parents as authoritative — so the attachment ends up
//     re-parented under whatever body bone happened to land on that index.
//   * a track whose bone dst already animates must NOT be duplicated: glTF
//     allows one channel per (node, path), and importers pick a duplicate
//     arbitrarily. Shared rig bones therefore map onto the existing track.
inline void mergeClipTracks(AnimClip& dst, const AnimClip& src)
{
    std::unordered_map<std::string, int> byName;
    byName.reserve(dst.tracks.size() + src.tracks.size());
    for (size_t i = 0; i < dst.tracks.size(); ++i)
        byName.emplace(dst.tracks[i].name, int(i));

    std::vector<int> remap(src.tracks.size(), -1);
    std::vector<size_t> copied;          // src indices actually appended
    copied.reserve(src.tracks.size());
    for (size_t i = 0; i < src.tracks.size(); ++i) {
        auto it = byName.find(src.tracks[i].name);
        if (it != byName.end()) { remap[i] = it->second; continue; }
        remap[i] = int(dst.tracks.size());
        dst.tracks.push_back(src.tracks[i]);
        byName.emplace(src.tracks[i].name, remap[i]);
        copied.push_back(i);
    }
    // Re-base only what moved. src guarantees parent < child, and appends keep
    // src order, so the parent's new index still precedes the child's.
    for (size_t i : copied) {
        AnimTrack& t = dst.tracks[size_t(remap[i])];
        const int sp = src.tracks[i].parent;
        t.parent = (sp >= 0 && size_t(sp) < remap.size())
                       ? int16_t(remap[size_t(sp)])
                       : int16_t(-1);
    }
    if (src.durationMs > dst.durationMs) dst.durationMs = src.durationMs;
}

// VERSION 1 (49 files, 2016-era stamps; solved 2026-08-10 by exact-extent
// arithmetic on both goldens): no parent field, u32 posKeyCount + 14 B keys,
// u16 rotKeyCount + u16 tag 0x4000 + 8 B rot keys [u16 t][48-bit
// smallest-three quaternion, 3 x 15-bit fields, same fill law as v2's 40-bit].
// Invariants held on all 2,131 measured keys; the 14-bit reading fails most.
//
// Parse a .anim blob (ZZZ4-wrapped or inflated). Returns false with *err set.
// out->keyArrays reports the layout that verified.
bool parseAnim(const uint8_t* data, size_t len, AnimClip* out, std::string* err);

// Read as much of a v3 clip as is understood: durationMs and the track
// name/parent table (no keys — the key encoding is still undecoded, see the
// note above). `data` must already be INFLATED. Returns false with *err on a
// structural mismatch, which would mean the format shifted under us.
bool probeAnimV3(const uint8_t* data, size_t len, AnimClip* out, std::string* err);

} // namespace di
