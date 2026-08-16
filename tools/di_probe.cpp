// di_probe — console prover for the DI storage layer. No Qt, no GL.
//
// Prints measured numbers for every format claim the storage layer makes:
//   1. .mpkinfo parse: entry counts, placeholders, collisions, per-base split
//   2. pak mapping: every referenced <base><idx>.mpk must exist on disk
//   3. extension histogram (top 20)
//   4. read path: Built.version content, app.ico magic, first Resources blob
//   5. resource.repository: declared vs parsed entries, leftover bytes,
//      type histogram, dependency stats
//   6. hash bridge: GUID blobs found, repository hashes resolved (coverage)
//   7. optional --baseline <file>: name diff vs a prior index dump
//   8. optional --dump-names <file>: write current names for future diffs
//   9. optional --tex-formats: read EVERY repository Texture2D header (fmt
//      byte @0x05 of the 40-byte header) and print the format histogram —
//      the measured answer to "is anything on PC not BC-decodable?"
//
//  10. optional --guid-blobs [N] [outDir]: characterize the undashed-GUID
//      high-entropy blob family (the last unread on-disk frontier for real
//      item names). Confirms the population, then for N samples (default 48,
//      spread across the size range) reports: raw + inflated size, container
//      framing (ZZZ4 / gzip / zlib / known magics), Shannon entropy, printable
//      ratio, ALL ASCII runs >= 4 chars (a string table would surface here),
//      and a fixed-record-stride guess. Writes guid_blob_report.txt and dumps
//      each sample (raw + inflated) into outDir (default guid_blob_samples\)
//      so the small blobs can be analysed byte-exactly off-machine.
//  11. optional --dump-blob <entryName> <outFile>: carve one entry's INFLATED
//      bytes (readAsset) to a file; --dump-blob-raw for the exact stored bytes.
//  12. optional --anim-coverage [outFile]: measure WHERE .anim / .skeleton files
//      actually sit relative to the Model entries that need them. Non-player
//      models come up with no clips in the app; this reports, per top-level
//      folder bucket, how many Models are skinned and what fraction each clip
//      strategy would find:
//        S1  "<walk-up>/ani/*.anim"        (what ModelResolve does today)
//        S2  "<own folder>/*.anim"
//        S3  any *.anim at or under the own folder (recursive)
//        S4  S3 widened up the folder chain, reported per ancestor level
//      plus skeleton strategies K1 "<walk-up>/<leaf>.skeleton" (today) vs
//      K2 any *.skeleton in the walk-up chain, the .anim parent-directory-name
//      histogram, the clip-count distribution (ambiguity check for widening),
//      and examples of models S1 misses but S3 finds. Writes the report to
//      outFile (default anim_coverage_report.txt) as well as stdout.
//
//  13. optional --asset <substring>: trace one repository asset - type, blob,
//      owning parent, every dependency with its type, what the folder-convention
//      clip search finds, and which SkinSkeleton it binds plus the clips that
//      live near THAT. The measured answer to "does this thing animate?".
//  14. optional --skin-map: every repository type with counts, then per
//      top-level folder how many Models bind a SkinSkeleton and what fraction
//      could reach clips through the skeleton's folder instead of their own
//      (S7 - the shared-rig route), plus the most-shared skeletons.
//  15. optional --anim-assets: the routes nobody has read yet - the
//      "Animation"-typed repository entries (what they are, who references
//      them, whether any Model depends on one) and the .graph / .mont /
//      .motion / .human file families by folder, with sample payloads scanned
//      for path-like strings. If a .graph names clip paths then IT, not a
//      folder walk, is the authoritative per-model clip list.
//
//  16. optional --anim-tags [N] / --anim-forensics [N]: walk every .anim
//      header the way AnimParser does, but RECORD unknown rot-key tags instead
//      of bailing. --anim-tags gives the census (versions, tag histogram,
//      solved key stride per tag, which folders are affected). --anim-forensics
//      adds everything needed to DECODE an unknown tag without a second run:
//      whether its bytes satisfy the v2 40-bit quaternion law (bits 38-39 zero,
//      sum of squares <= 1, monotonic times ending at durationMs), which bones
//      and track slots carry it, whether files MIX tags, raw key dumps beside a
//      known-good 0x32 reference from the same file, and the v3 files whose
//      name table disagrees with their header. N caps the clips read.
//
// Usage: di_probe <mpkDir> [--baseline <file>] [--dump-names <file>]
//                          [--tex-formats]
//                          [--guid-blobs [N] [outDir]]
//                          [--dump-blob <name> <out>] [--dump-blob-raw <name> <out>]
//                          [--anim-coverage [outFile]]
//                          [--asset <substring>] [--skin-map] [--anim-assets]
//                          [--anim-tags [N]] [--anim-forensics [N]]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "model/AnimParser.h"
#include "store/AssetStore.h"
#include "store/Zzz4.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ── helpers for the GUID-blob probe ─────────────────────────────────────────

// Leaf stem is 32 lowercase hex with no dash → the undashed-GUID family.
static bool isUndashedGuid(const std::string& name)
{
    const size_t slash = name.find_last_of('/');
    std::string leaf = slash == std::string::npos ? name : name.substr(slash + 1);
    const size_t dot = leaf.find('.');
    const std::string stem = dot == std::string::npos ? leaf : leaf.substr(0, dot);
    if (stem.size() != 32) return false;
    for (char c : stem)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

static double shannon(const uint8_t* d, size_t n)
{
    if (!n) return 0.0;
    size_t freq[256] = {0};
    for (size_t i = 0; i < n; ++i) freq[d[i]]++;
    double h = 0.0;
    for (int i = 0; i < 256; ++i)
        if (freq[i]) {
            const double p = (double)freq[i] / (double)n;
            h -= p * std::log2(p);
        }
    return h;   // bits/byte, 0..8
}

static double printableRatio(const uint8_t* d, size_t n)
{
    if (!n) return 0.0;
    size_t p = 0;
    for (size_t i = 0; i < n; ++i)
        if ((d[i] >= 0x20 && d[i] < 0x7F) || d[i] == '\t' || d[i] == '\n') ++p;
    return (double)p / (double)n;
}

// Every ASCII run of >= minRun printable chars (candidate field/string data).
static std::vector<std::string> asciiRuns(const uint8_t* d, size_t n, size_t minRun)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < n; ++i) {
        if (d[i] >= 0x20 && d[i] < 0x7F) {
            cur.push_back((char)d[i]);
        } else {
            if (cur.size() >= minRun) out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() >= minRun) out.push_back(cur);
    return out;
}

// A short, printable, sane framing/magic label for the first bytes.
static std::string framing(const std::vector<uint8_t>& d)
{
    if (d.size() >= 4 && std::memcmp(d.data(), "ZZZ4", 4) == 0) return "ZZZ4";
    if (d.size() >= 4 && std::memcmp(d.data(), "CCCC", 4) == 0) return "CCCC+ZZZ4";
    // Messiah-engine compressed chunk: constant e2 06 magic, then a structured
    // header (byte[3] high-nibble concentrated on 0xC across the family, so it
    // is compressed NOT encrypted), then a high-entropy LZ-family body no
    // standard codec (lz4/lzo/zlib/lzma/zstd) decodes. Game ships minilzo +
    // MessiahAssetIndexerClient DLLs that read it. Content (per its readable
    // ZZZ4 siblings) is engine scene / material / component serialization,
    // not item-name text.
    if (d.size() >= 2 && d[0] == 0xe2 && d[1] == 0x06) return "messiah-e206";
    if (d.size() >= 2 && d[0] == 0x1f && d[1] == 0x8b) return "gzip";
    if (d.size() >= 2 && d[0] == 0x78 &&
        (d[1] == 0x01 || d[1] == 0x9c || d[1] == 0xda)) return "zlib";
    if (d.size() >= 4 && std::memcmp(d.data(), "\x89PNG", 4) == 0) return "png";
    if (d.size() >= 3 && std::memcmp(d.data(), "DDS", 3) == 0) return "dds";
    if (d.size() >= 4 && std::memcmp(d.data(), "OggS", 4) == 0) return "ogg";
    if (d.size() >= 4 && std::memcmp(d.data(), "RIFF", 4) == 0) return "riff";
    if (d.size() >= 4 && std::memcmp(d.data(), "\x04\x22\x4d\x18", 4) == 0)
        return "lz4-frame";
    if (!d.empty() && (d[0] == '{' || d[0] == '[')) return "json?";
    if (d.size() >= 6 && std::memcmp(d.data(), "<?xml", 5) == 0) return "xml";
    // FlatBuffers: u32 root-table offset that lands inside the buffer, then a
    // small signed soffset back to the vtable.
    if (d.size() >= 8) {
        const uint32_t root = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
        if (root >= 4 && root + 4 <= d.size()) return "flatbuffer?";
    }
    return "raw/unknown";
}

// Smallest fixed record stride (4..64) that divides the length and whose column
// bytes repeat with low variety — a hint at a packed table. 0 = none found.
static int recordStride(const uint8_t* d, size_t n)
{
    if (n < 32) return 0;
    for (int s = 4; s <= 64; ++s) {
        if (n % s) continue;
        const size_t rows = n / s;
        if (rows < 4) continue;
        // count columns where the byte value varies little across rows
        int stableCols = 0;
        for (int c = 0; c < s; ++c) {
            std::set<uint8_t> vals;
            for (size_t r = 0; r < rows && vals.size() <= 4; ++r)
                vals.insert(d[r * s + c]);
            if (vals.size() <= 2) ++stableCols;
        }
        if (stableCols >= s / 3) return s;   // a third of columns near-constant
    }
    return 0;
}

static double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static std::string extOf(const std::string& name)
{
    const size_t slash = name.find_last_of('/');
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot   = name.find_last_of('.');
    if (dot == std::string::npos || dot < start) return "(none)";
    std::string e = name.substr(dot + 1);
    if (e.size() > 24) return "(long)";
    for (char& c : e)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e;
}

static void printTop(const std::map<std::string, size_t>& hist, size_t topN, const char* label)
{
    std::vector<std::pair<std::string, size_t>> v(hist.begin(), hist.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::printf("%s (top %zu of %zu):\n", label, topN < v.size() ? topN : v.size(), v.size());
    for (size_t i = 0; i < v.size() && i < topN; ++i)
        std::printf("  %-28s %zu\n", v[i].first.c_str(), v[i].second);
}

int main(int argc, char** argv)
{
    std::string mpkDir = "G:\\G Games\\Diablo Immortal\\Package\\MPK";
    std::string baselinePath, dumpPath;
    bool texFormats = false;
    bool guidBlobs = false;
    int  guidN = 48;
    std::string guidOutDir = "guid_blob_samples";
    std::string dumpBlobName, dumpBlobOut;
    bool dumpBlobRaw = false;
    bool animCoverage = false;
    std::string animOut = "anim_coverage_report.txt";
    std::string assetQuery;
    bool skinMap = false;
    bool animAssets = false;
    bool animTags = false;
    bool animForensics = false;
    bool animVerify = false;
    size_t animTagsN = 0;   // 0 = every .anim in the archive
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--baseline" && i + 1 < argc)       baselinePath = argv[++i];
        else if (a == "--dump-names" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == "--tex-formats")                texFormats = true;
        else if (a == "--guid-blobs") {
            guidBlobs = true;
            // optional trailing [N] [outDir]
            if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]))
                guidN = std::atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                guidOutDir = argv[++i];
        } else if (a == "--anim-coverage") {
            animCoverage = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') animOut = argv[++i];
        } else if (a == "--asset" && i + 1 < argc) {
            assetQuery = argv[++i];
        } else if (a == "--skin-map") {
            skinMap = true;
        } else if (a == "--anim-assets") {
            animAssets = true;
        } else if (a == "--anim-tags") {
            animTags = true;
            if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]))
                animTagsN = (size_t)std::atol(argv[++i]);
        } else if (a == "--anim-verify") {
            animVerify = true;
        } else if (a == "--anim-forensics") {
            animTags = true;        // the census is the first half of forensics
            animForensics = true;
            if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]))
                animTagsN = (size_t)std::atol(argv[++i]);
        } else if ((a == "--dump-blob" || a == "--dump-blob-raw") &&
                   i + 2 < argc) {
            dumpBlobRaw = (a == "--dump-blob-raw");
            dumpBlobName = argv[++i];
            dumpBlobOut  = argv[++i];
        } else {
            mpkDir = a;
        }
    }
    std::printf("di_probe - DI storage layer prover\nmpkDir: %s\n\n", mpkDir.c_str());

    // ---- 1. index ----------------------------------------------------------
    di::DiAssetStore store;
    std::string err;
    auto t0 = Clock::now();
    if (!store.open(mpkDir, &err)) {
        std::printf("FAIL open: %s\n", err.c_str());
        return 1;
    }
    const auto& mpk = store.mpk();
    std::printf("[1] index parse: %.1f ms\n", msSince(t0));
    std::printf("    entries (length>0):     %zu\n", mpk.entries().size());
    std::printf("    placeholders skipped:   %zu\n", mpk.placeholdersSkipped());
    std::printf("    name collisions:        %zu\n", mpk.nameCollisions());
    std::printf("    raw total (entries+ph): %zu\n",
                mpk.entries().size() + mpk.placeholdersSkipped());
    {
        std::vector<size_t> perBase(mpk.bases().size(), 0);
        for (const auto& e : mpk.entries()) perBase[e.baseId]++;
        for (size_t b = 0; b < mpk.bases().size(); ++b)
            std::printf("    base %-12s %zu\n", mpk.bases()[b].c_str(), perBase[b]);
    }

    // ---- 2. pak mapping ----------------------------------------------------
    t0 = Clock::now();
    {
        std::set<std::string> paks;
        for (const auto& e : mpk.entries()) paks.insert(mpk.pakFileName(e));
        size_t missing = 0;
        for (const std::string& p : paks)
            if (!fs::exists(fs::u8path(mpkDir + "/" + p))) {
                std::printf("    MISSING pak: %s\n", p.c_str());
                ++missing;
            }
        std::printf("[2] pak mapping: %.1f ms - %zu distinct paks referenced, %zu missing%s\n",
                    msSince(t0), paks.size(), missing,
                    missing ? "  <-- pakField/2 assumption VIOLATED" : "");
    }

    // ---- 3. extension histogram -------------------------------------------
    {
        std::map<std::string, size_t> hist;
        for (const auto& e : mpk.entries()) hist[extOf(e.name)]++;
        printTop(hist, 20, "[3] extension histogram");
    }

    // ---- 4. read path ------------------------------------------------------
    t0 = Clock::now();
    {
        const std::string bv = store.buildVersion();
        std::printf("[4] read path:\n");
        std::printf("    Built.version: %s\n", bv.empty() ? "<UNREADABLE>" : bv.c_str());

        const size_t ico = mpk.find("Engine/Content/Icons/app.ico");
        if (ico != (size_t)-1) {
            std::vector<uint8_t> h = mpk.readHeader(ico, 4);
            const bool ok = h.size() == 4 && h[0] == 0 && h[1] == 0 && h[2] == 1 && h[3] == 0;
            std::printf("    app.ico magic 00 00 01 00: %s\n", ok ? "OK" : "WRONG");
        } else {
            std::printf("    app.ico: not in index\n");
        }

        // first ZZZ4 blob in the index: inflate and report
        size_t tried = 0, inflated = 0, firstShown = 0;
        for (size_t i = 0; i < mpk.entries().size() && tried < 50; ++i) {
            std::vector<uint8_t> h = mpk.readHeader(i, 16);
            if (h.size() >= 8 && di::isZzz4(h.data(), h.size())) {
                ++tried;
                std::vector<uint8_t> a = mpk.readAsset(i);
                if (!a.empty()) {
                    ++inflated;
                    if (!firstShown++) {
                        char head[9] = {0};
                        for (int k = 0; k < 8 && (size_t)k < a.size(); ++k)
                            head[k] = (a[k] >= 0x20 && a[k] < 0x7F) ? (char)a[k] : '.';
                        std::printf("    first ZZZ4 blob: %s -> %zu bytes, head \"%s\"\n",
                                    mpk.entries()[i].name.c_str(), a.size(), head);
                    }
                }
            }
        }
        std::printf("    ZZZ4 sample inflate: %zu/%zu ok (%.1f ms total)\n",
                    inflated, tried, msSince(t0));
    }

    // ---- 5. repository -----------------------------------------------------
    t0 = Clock::now();
    if (!store.loadRepository(&err)) {
        std::printf("[5] repository: FAIL - %s\n", err.c_str());
    } else {
        const di::Repository& r = *store.repo();
        std::printf("[5] repository: %.1f ms\n", msSince(t0));
        std::printf("    declared count (u32):  %u\n", r.declaredCount);
        std::printf("    entries parsed:        %zu\n", r.entries.size());
        std::printf("    types / folders:       %zu / %zu\n", r.types.size(), r.folders.size());
        std::printf("    bytes consumed/total:  %zu / %zu (%zu leftover)\n",
                    r.bytesConsumed, r.bytesTotal, r.bytesTotal - r.bytesConsumed);
        std::map<std::string, size_t> thist;
        size_t withDeps = 0, depTotal = 0;
        for (const auto& e : r.entries) {
            thist[r.typeOf(e).empty() ? "(none)" : r.typeOf(e)]++;
            if (!e.related.empty()) { ++withDeps; depTotal += e.related.size(); }
        }
        printTop(thist, 15, "    type histogram");
        std::printf("    entries with deps:     %zu (avg %.2f deps)\n", withDeps,
                    withDeps ? (double)depTotal / (double)withDeps : 0.0);

        // ---- 6. hash bridge ------------------------------------------------
        t0 = Clock::now();
        size_t resolved = 0;
        for (const auto& e : r.entries)
            if (store.blobForHash(e.hashHex) != (size_t)-1) ++resolved;
        std::printf("[6] hash bridge: %.1f ms - GUID blobs %zu; repo hashes resolved %zu/%zu (%.1f%%)\n",
                    msSince(t0), store.hashBridgeSize(), resolved, r.entries.size(),
                    r.entries.empty() ? 0.0 : 100.0 * (double)resolved / (double)r.entries.size());
    }

    // ---- 9. texture format histogram ----------------------------------------
    if (texFormats) {
        t0 = Clock::now();
        if (!store.repo()) {
            std::printf("[9] tex-formats: repository unavailable, skipped\n");
        } else {
            auto fmtName = [](int f) -> std::string {
                switch (f) {
                    case 5:  return "R8G8B8A8";
                    case 12: return "L8";
                    case 14: return "A8";
                    case 18: return "BC1";
                    case 19: return "BC2";
                    case 20: return "BC3";
                    case 21: return "BC4";
                    case 22: return "BC5";
                    case 23: return "BC6S";
                    case 24: return "BC6U";
                    case 25: return "BC7";
                    case 31: return "ETC1";
                    case 32: return "ETC2RGB";
                    case 33: return "ETC2RGBA";
                }
                if (f >= 36 && f <= 49) return "ASTC_LDR_" + std::to_string(f);
                if (f >= 55 && f <= 68) return "ASTC_HDR_" + std::to_string(f);
                return "format_" + std::to_string(f);
            };
            const di::Repository& r = *store.repo();
            std::map<std::string, size_t> hist;
            size_t total = 0, noBlob = 0, shortHdr = 0;
            // Any format the decoder does not handle is worth naming: the
            // handful of stragglers (6 files at last count, formats 10/11) can
            // then be extracted by name and decoded. A histogram alone said
            // they exist but not WHICH files.
            static const std::set<int> kDecoded = {3, 5, 12, 14, 18, 19, 20,
                                                   21, 22, 23, 24, 25};
            std::vector<std::string> unknowns;
            size_t unknownTotal = 0;
            for (const auto& e : r.entries) {
                if (r.typeOf(e) != "Texture2D") continue;
                ++total;
                const size_t blob = store.blobForHash(e.hashHex);
                if (blob == (size_t)-1) { ++noBlob; continue; }
                const std::vector<uint8_t> h = mpk.readHeader(blob, 8);
                if (h.size() < 6) { ++shortHdr; continue; }
                const int fmt = h[5];
                hist[fmtName(fmt)]++;
                if (!kDecoded.count(fmt)) ++unknownTotal;
                if (!kDecoded.count(fmt) && unknowns.size() < 200) {
                    const auto& ent = mpk.entries()[blob];
                    unknowns.push_back("fmt " + std::to_string(fmt) + "  " +
                                       e.name + "  blob " + ent.name +
                                       " off " + std::to_string(ent.offset) +
                                       " len " + std::to_string(ent.length) +
                                       " pak " + std::to_string(ent.pak));
                }
            }
            std::printf("[9] tex-formats: %.1f ms - %zu Texture2D entries "
                        "(%zu without blob, %zu short header)\n",
                        msSince(t0), total, noBlob, shortHdr);
            printTop(hist, 40, "    format histogram");
            std::printf("    undecoded formats: %zu entries (listing %zu)\n",
                        unknownTotal, unknowns.size());
            for (const std::string& u : unknowns)
                std::printf("      %s\n", u.c_str());
        }
    }

    // ---- 11. targeted blob dump -------------------------------------------
    if (!dumpBlobName.empty()) {
        const size_t id = mpk.find(dumpBlobName);
        if (id == (size_t)-1) {
            std::printf("[11] dump-blob: '%s' not in index\n", dumpBlobName.c_str());
        } else {
            const std::vector<uint8_t> bytes =
                dumpBlobRaw ? mpk.read(id) : mpk.readAsset(id);
            std::ofstream f(fs::u8path(dumpBlobOut), std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()),
                    (std::streamsize)bytes.size());
            std::printf("[11] dump-blob%s: %s -> %s (%zu bytes)\n",
                        dumpBlobRaw ? "-raw" : "", dumpBlobName.c_str(),
                        dumpBlobOut.c_str(), bytes.size());
        }
    }

    // ---- 10. undashed-GUID high-entropy blob family ------------------------
    if (guidBlobs) {
        t0 = Clock::now();
        std::vector<size_t> fam;
        fam.reserve(32768);
        for (size_t i = 0; i < mpk.entries().size(); ++i)
            if (isUndashedGuid(mpk.entries()[i].name)) fam.push_back(i);

        // population stats
        std::map<uint32_t, size_t> perPak;
        size_t totalBytes = 0, dotZero = 0;
        for (size_t id : fam) {
            const auto& e = mpk.entries()[id];
            perPak[e.pak]++;
            totalBytes += e.length;
            if (e.name.size() >= 2 && e.name.compare(e.name.size() - 2, 2, ".0") == 0)
                ++dotZero;
        }
        std::vector<size_t> sizes;
        sizes.reserve(fam.size());
        for (size_t id : fam) sizes.push_back(mpk.entries()[id].length);
        std::sort(sizes.begin(), sizes.end());

        fs::create_directories(fs::u8path(guidOutDir));
        std::ofstream rep(fs::u8path(guidOutDir + "/guid_blob_report.txt"),
                          std::ios::binary);
        auto emit = [&](const std::string& s) {
            std::fputs(s.c_str(), stdout);
            rep << s;
        };
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
                      "[10] undashed-GUID blob family\n"
                      "  population: %zu entries, %.1f MB total, %zu with .0 ext\n"
                      "  sizes: min %zu  median %zu  p90 %zu  max %zu\n",
                      fam.size(), totalBytes / 1e6, dotZero,
                      sizes.empty() ? 0 : sizes.front(),
                      sizes.empty() ? 0 : sizes[sizes.size() / 2],
                      sizes.empty() ? 0 : sizes[(size_t)(sizes.size() * 0.9)],
                      sizes.empty() ? 0 : sizes.back());
        emit(buf);
        {
            std::string pk = "  paks: ";
            for (auto& [p, c] : perPak)
                pk += std::to_string(p) + "=" + std::to_string(c) + " ";
            emit(pk + "\n");
        }
        // Is ANY of these referenced by a repository entry (as its own hash or
        // a dependency)? If zero, they are a SEPARATE GUID-keyed store, not
        // asset-graph nodes — which is exactly what a server-side data table
        // addressed by numeric/hash id would look like.
        if (store.repo()) {
            std::set<std::string> refHashes;
            for (const auto& e : store.repo()->entries) {
                refHashes.insert(e.hashHex);
                for (const auto& h : e.related) refHashes.insert(h);
            }
            size_t referenced = 0;
            for (size_t id : fam) {
                std::string hex;
                if (di::guidBlobHash(mpk.entries()[id].name, &hex) &&
                    refHashes.count(hex))
                    ++referenced;
            }
            std::snprintf(buf, sizeof(buf),
                          "  referenced by a repository entry: %zu / %zu  "
                          "(0 = standalone GUID-keyed store)\n",
                          referenced, fam.size());
            emit(buf);
        }

        // sample spread across the size range (sorted by length, even stride)
        std::vector<size_t> order = fam;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return mpk.entries()[a].length < mpk.entries()[b].length;
        });
        std::vector<size_t> sample;
        if (!order.empty()) {
            const int N = guidN < (int)order.size() ? guidN : (int)order.size();
            for (int k = 0; k < N; ++k)
                sample.push_back(order[(size_t)((double)k / N * order.size())]);
            // always include the three largest
            for (int k = 0; k < 3 && (int)order.size() - 1 - k >= 0; ++k)
                sample.push_back(order[order.size() - 1 - k]);
        }

        emit("\n  --- samples (raw | inflated) ---\n");
        std::map<std::string, size_t> framingHist;
        size_t inflatedCount = 0, textish = 0;
        std::set<std::string> allRuns;
        for (size_t id : sample) {
            const auto& e = mpk.entries()[id];
            const std::vector<uint8_t> raw = mpk.read(id);
            const std::vector<uint8_t> inf = mpk.readAsset(id);
            const bool didInflate = inf.size() != raw.size();
            if (didInflate) ++inflatedCount;
            const std::string frm = framing(raw);
            framingHist[frm]++;
            const double eRaw = shannon(raw.data(), raw.size());
            const double eInf = shannon(inf.data(), inf.size());
            const double pr = printableRatio(inf.data(), inf.size());
            if (pr > 0.55) ++textish;
            const int stride = recordStride(inf.data(), inf.size());
            const auto runs = asciiRuns(inf.data(), inf.size(), 4);
            for (const auto& r : runs)
                if (allRuns.size() < 4000) allRuns.insert(r);

            std::snprintf(buf, sizeof(buf),
                          "  %s\n"
                          "    raw %zu B  framing=%s  Hraw=%.2f  |  inflated %zu B "
                          "(%s)  Hinf=%.2f  printable=%.0f%%  stride=%d  runs=%zu\n",
                          e.name.c_str(), raw.size(), frm.c_str(), eRaw,
                          inf.size(), didInflate ? "inflated" : "as-is", eInf,
                          pr * 100.0, stride, runs.size());
            emit(buf);
            // show up to 12 of the sample's ASCII runs inline
            int shown = 0;
            for (const auto& r : runs) {
                if (shown++ >= 12) { emit("      …\n"); break; }
                emit("      \"" + r + "\"\n");
            }
            // dump the bytes for off-machine analysis
            std::string stem = e.name;
            for (char& c : stem) if (c == '/' || c == '\\') c = '_';
            std::ofstream rf(fs::u8path(guidOutDir + "/" + stem + ".raw"),
                             std::ios::binary);
            rf.write(reinterpret_cast<const char*>(raw.data()),
                     (std::streamsize)raw.size());
            if (didInflate) {
                std::ofstream inff(fs::u8path(guidOutDir + "/" + stem + ".inflated"),
                                   std::ios::binary);
                inff.write(reinterpret_cast<const char*>(inf.data()),
                           (std::streamsize)inf.size());
            }
        }

        emit("\n  --- summary ---\n");
        std::snprintf(buf, sizeof(buf),
                      "  sampled %zu; %zu inflated (ZZZ4/CCCC), %zu look text-ish "
                      "(printable>55%%)\n",
                      sample.size(), inflatedCount, textish);
        emit(buf);
        {
            std::string fh = "  framing: ";
            for (auto& [k, c] : framingHist)
                fh += k + "=" + std::to_string(c) + " ";
            emit(fh + "\n");
        }
        std::snprintf(buf, sizeof(buf),
                      "  distinct ASCII runs (>=4 chars) across samples: %zu\n"
                      "  (full raw/inflated bytes written to %s\\ for analysis)\n",
                      allRuns.size(), guidOutDir.c_str());
        emit(buf);
        // write every distinct run to its own file — this is the payload that
        // matters: if a string table exists, its field names live here.
        {
            std::ofstream rf(fs::u8path(guidOutDir + "/ascii_runs.txt"),
                             std::ios::binary);
            for (const auto& r : allRuns) rf << r << "\n";
        }
        std::printf("[10] guid-blobs: %.1f ms - report + samples in %s\\\n",
                    msSince(t0), guidOutDir.c_str());
    }

    // ---- 7. baseline diff --------------------------------------------------
    if (!baselinePath.empty()) {
        t0 = Clock::now();
        std::ifstream f(fs::u8path(baselinePath));
        if (!f) {
            std::printf("[7] baseline: cannot open %s\n", baselinePath.c_str());
        } else {
            std::unordered_set<std::string> base;
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                base.insert(line);
            }
            std::unordered_set<std::string> cur;
            cur.reserve(mpk.entries().size() * 2);
            for (const auto& e : mpk.entries()) cur.insert(e.name);

            size_t missing = 0, added = 0;
            std::vector<std::string> exMissing, exAdded;
            for (const auto& nm : base)
                if (!cur.count(nm)) { ++missing; if (exMissing.size() < 5) exMissing.push_back(nm); }
            for (const auto& nm : cur)
                if (!base.count(nm)) { ++added; if (exAdded.size() < 5) exAdded.push_back(nm); }
            std::printf("[7] baseline diff: %.1f ms - baseline %zu names, current %zu\n",
                        msSince(t0), base.size(), cur.size());
            std::printf("    in baseline, not here: %zu\n", missing);
            for (const auto& nm : exMissing) std::printf("      - %s\n", nm.c_str());
            std::printf("    here, not in baseline: %zu\n", added);
            for (const auto& nm : exAdded) std::printf("      + %s\n", nm.c_str());
        }
    }

    // ---- 8. name dump ------------------------------------------------------
    if (!dumpPath.empty()) {
        std::ofstream f(fs::u8path(dumpPath), std::ios::binary);
        std::vector<std::string> names;
        names.reserve(mpk.entries().size());
        for (const auto& e : mpk.entries()) names.push_back(e.name);
        std::sort(names.begin(), names.end());
        f << "# DIAssetBrowser di_probe name dump\n# total: " << names.size() << "\n";
        for (const auto& nm : names) f << nm << "\n";
        std::printf("[8] dumped %zu names -> %s\n", names.size(), dumpPath.c_str());
    }

    // ---- 12. animation coverage --------------------------------------------
    if (animCoverage) {
        t0 = Clock::now();
        if (!store.repo()) {
            std::printf("[12] anim-coverage: repository unavailable, skipped\n");
        } else {
            const di::Repository& r = *store.repo();
            std::ofstream rep(fs::u8path(animOut), std::ios::binary);
            auto emit = [&](const std::string& s) {
                std::fputs(s.c_str(), stdout);
                rep << s;
            };
            char buf[1200];

            auto endsWith = [](const std::string& s, const char* suf) {
                const size_t n = std::strlen(suf);
                return s.size() > n && s.compare(s.size() - n, n, suf) == 0;
            };
            auto leafOf = [](const std::string& s) {
                const size_t p = s.find_last_of('/');
                return p == std::string::npos ? s : s.substr(p + 1);
            };

            // (a) index every .anim / .skeleton in the archive by parent dir.
            std::unordered_map<std::string, size_t> animCnt, animFirst, skelCnt;
            std::map<std::string, size_t> animParentHist;
            size_t animTotal = 0, skelTotal = 0;
            for (size_t i = 0; i < mpk.entries().size(); ++i) {
                const std::string& n = mpk.entries()[i].name;
                const bool isAnim = endsWith(n, ".anim");
                const bool isSkel = !isAnim && endsWith(n, ".skeleton");
                if (!isAnim && !isSkel) continue;
                const size_t slash = n.find_last_of('/');
                if (slash == std::string::npos) continue;
                const std::string dir = n.substr(0, slash);
                if (isAnim) {
                    if (!animCnt.count(dir)) animFirst[dir] = i;
                    animCnt[dir]++;
                    animParentHist[leafOf(dir)]++;
                    ++animTotal;
                } else {
                    skelCnt[dir]++;
                    ++skelTotal;
                }
            }

            // Case-folded dir -> count, so the shipped spelling variants of the
            // clip folder ("ani", "ain", "Ani" - all measured in [12d]) can be
            // probed without guessing which casing a given family used.
            std::unordered_map<std::string, size_t> animCntLower, skelCntLower;
            auto lower = [](std::string s) {
                for (char& c : s)
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                return s;
            };
            for (const auto& kv : animCnt) animCntLower[lower(kv.first)] += kv.second;
            for (const auto& kv : skelCnt) skelCntLower[lower(kv.first)] += kv.second;

            // "<name>_lod" / "_lod1" / "_lod2" are LOD variants of one asset;
            // their clip folder (if any) is named for the base asset.
            auto stripLod = [](const std::string& s) {
                size_t n = s.size();
                while (n > 0 && s[n - 1] >= '0' && s[n - 1] <= '9') --n;
                if (n >= 4 && s.compare(n - 4, 4, "_lod") == 0) return s.substr(0, n - 4);
                return s;
            };

            // sorted dir list + prefix sums, so "any .anim under X" is O(log n)
            std::vector<std::string> animDirs;
            animDirs.reserve(animCnt.size());
            for (const auto& kv : animCnt) animDirs.push_back(kv.first);
            std::sort(animDirs.begin(), animDirs.end());
            std::vector<size_t> cum(animDirs.size() + 1, 0);
            for (size_t i = 0; i < animDirs.size(); ++i)
                cum[i + 1] = cum[i] + animCnt[animDirs[i]];

            // Count of .anim at OR under dir D. The subtree range is
            // [D+"/", D+"0") - '/' is 0x2F and '0' is 0x30, so every string in
            // that range provably starts with D+"/" and nothing else does.
            auto underRange = [&](const std::string& D, size_t* lo, size_t* hi) {
                *lo = (size_t)(std::lower_bound(animDirs.begin(), animDirs.end(),
                                                D + "/") - animDirs.begin());
                *hi = (size_t)(std::lower_bound(animDirs.begin(), animDirs.end(),
                                                D + "0") - animDirs.begin());
            };
            auto countUnder = [&](const std::string& D) {
                size_t lo = 0, hi = 0;
                underRange(D, &lo, &hi);
                size_t n = cum[hi] - cum[lo];
                auto it = animCnt.find(D);
                if (it != animCnt.end()) n += it->second;
                return n;
            };
            auto exampleUnder = [&](const std::string& D) -> std::string {
                auto it = animCnt.find(D);
                if (it != animCnt.end()) return mpk.entries()[animFirst[D]].name;
                size_t lo = 0, hi = 0;
                underRange(D, &lo, &hi);
                if (lo < hi) return mpk.entries()[animFirst[animDirs[lo]]].name;
                return std::string();
            };

            struct Bucket {
                size_t n = 0, skinned = 0;
                size_t s1 = 0, s1v = 0, s5 = 0, s6 = 0, comb = 0, s3 = 0;
                size_t k1 = 0, k1v = 0;
            };
            std::map<std::string, Bucket> buckets;    // top-level folder
            std::map<std::string, Bucket> sub;        // second level under Char/
            Bucket all;
            size_t noFolder = 0;
            size_t clipBucket[5] = {0, 0, 0, 0, 0};   // 1 / 2-5 / 6-20 / 21-100 / >100
            std::vector<std::string> misses;          // still missed after COMB

            for (const auto& e : r.entries) {
                const std::string& t = r.typeOf(e);
                if (t != "Model" && t != "LodModel") continue;
                const std::string folder = r.folderOf(e);
                if (folder.empty()) { ++noFolder; continue; }

                const size_t seg = folder.find('/');
                const std::string key =
                    seg == std::string::npos ? folder : folder.substr(0, seg);
                Bucket& b = buckets[key];
                ++b.n;
                ++all.n;
                Bucket* b2 = nullptr;
                if (key == "Char" && seg != std::string::npos) {
                    const size_t seg2 = folder.find('/', seg + 1);
                    b2 = &sub[folder.substr(seg + 1,
                                            seg2 == std::string::npos
                                                ? std::string::npos
                                                : seg2 - seg - 1)];
                    ++b2->n;
                }

                // skinned = a SkinSkeleton dep, directly or one Model hop down
                bool skinned = false;
                for (const std::string& h : e.related) {
                    auto it = r.byHash.find(h);
                    if (it == r.byHash.end()) continue;
                    const di::RepoEntry& d = r.entries[it->second];
                    const std::string& dt = r.typeOf(d);
                    if (dt == "SkinSkeleton") { skinned = true; break; }
                    if (dt == "Model") {
                        for (const std::string& h2 : d.related) {
                            auto i2 = r.byHash.find(h2);
                            if (i2 != r.byHash.end() &&
                                r.typeOf(r.entries[i2->second]) == "SkinSkeleton") {
                                skinned = true;
                                break;
                            }
                        }
                        if (skinned) break;
                    }
                }
                if (skinned) {
                    ++b.skinned;
                    ++all.skinned;
                    if (b2) ++b2->skinned;
                }

                // ancestor chain, deepest first
                std::vector<std::string> chain;
                for (std::string s = folder;;) {
                    chain.push_back(s);
                    const size_t p = s.find_last_of('/');
                    if (p == std::string::npos) break;
                    s.resize(p);
                }

                const std::string own = "Package/" + folder;

                // Per-ASSET clip folders: "<folder>/<name>/ani". Measured need -
                // Char/item models all share folder "Char/item" while their clips
                // sit in a subfolder named for the item itself, so a folder-only
                // walk either misses them or sweeps in every other item's clips.
                const std::string nmBase = stripLod(e.name);
                std::vector<std::string> selfDirs;
                selfDirs.push_back(own + "/" + e.name);
                if (nmBase != e.name) selfDirs.push_back(own + "/" + nmBase);

                auto aniCountAt = [&](const std::string& base) {
                    const std::string lb = lower(base);
                    size_t c = 0;
                    auto i1 = animCntLower.find(lb + "/ani");
                    auto i2 = animCntLower.find(lb + "/ain");
                    if (i1 != animCntLower.end()) c += i1->second;
                    if (i2 != animCntLower.end()) c += i2->second;
                    return c;
                };

                bool s1 = false, s1v = false, k1 = false;
                size_t s1vClips = 0;
                for (size_t lv = 0; lv < chain.size(); ++lv) {
                    const std::string base = "Package/" + chain[lv];
                    if (!s1 && animCnt.count(base + "/ani")) s1 = true;
                    if (!s1v) {
                        const size_t c = aniCountAt(base);
                        if (c) { s1v = true; s1vClips = c; }
                    }
                    if (!k1 &&
                        mpk.find(base + "/" + leafOf(chain[lv]) + ".skeleton") !=
                            (size_t)-1)
                        k1 = true;
                }

                bool s5 = false, s6 = false, k1v = k1;
                size_t s5clips = 0, s6clips = 0;
                for (const std::string& d : selfDirs) {
                    const size_t c = aniCountAt(d);
                    if (c && c > s5clips) { s5 = true; s5clips = c; }
                    const size_t u = countUnder(d);
                    if (u && u > s6clips) { s6 = true; s6clips = u; }
                    if (!k1v) {
                        if (mpk.find(d + "/" + leafOf(d) + ".skeleton") != (size_t)-1)
                            k1v = true;
                        else if (skelCntLower.count(lower(d)))
                            k1v = true;
                    }
                }

                const size_t ownUnder = countUnder(own);
                const bool   s3   = ownUnder != 0;
                const bool   comb = s1v || s5 || s6;

                if (s1)   { ++b.s1;   ++all.s1;   if (b2) ++b2->s1; }
                if (s1v)  { ++b.s1v;  ++all.s1v;  if (b2) ++b2->s1v; }
                if (s5)   { ++b.s5;   ++all.s5;   if (b2) ++b2->s5; }
                if (s6)   { ++b.s6;   ++all.s6;   if (b2) ++b2->s6; }
                if (comb) { ++b.comb; ++all.comb; if (b2) ++b2->comb; }
                if (s3)   { ++b.s3;   ++all.s3;   if (b2) ++b2->s3; }
                if (k1)   { ++b.k1;   ++all.k1;   if (b2) ++b2->k1; }
                if (k1v)  { ++b.k1v;  ++all.k1v;  if (b2) ++b2->k1v; }

                // Tightness: how many clips the CHOSEN strategy would attach.
                if (comb) {
                    const size_t c = s5 ? s5clips : s6 ? s6clips : s1vClips;
                    clipBucket[c == 1 ? 0 : c <= 5 ? 1 : c <= 20 ? 2 : c <= 100 ? 3 : 4]++;
                }
                if (!comb && misses.size() < 40) {
                    std::snprintf(buf, sizeof(buf),
                                  "      %s/%s  (%s)\n        %zu clip(s) anywhere "
                                  "under %s%s%s\n",
                                  folder.c_str(), e.name.c_str(),
                                  skinned ? "skinned" : "static", ownUnder, own.c_str(),
                                  ownUnder ? ", e.g. " : "",
                                  ownUnder ? exampleUnder(own).c_str() : "");
                    misses.push_back(buf);
                }
            }

            auto pct = [](size_t a2, size_t b2) {
                return b2 ? 100.0 * (double)a2 / (double)b2 : 0.0;
            };

            std::snprintf(buf, sizeof(buf),
                          "[12] anim-coverage: %.1f ms\n"
                          "  archive: %zu .anim in %zu dirs, %zu .skeleton in %zu dirs\n"
                          "  Model/LodModel entries: %zu (+%zu with no repository folder)\n"
                          "  skinned (SkinSkeleton dep): %zu (%.1f%%)\n\n",
                          msSince(t0), animTotal, animCnt.size(), skelTotal,
                          skelCnt.size(), all.n, noFolder, all.skinned,
                          pct(all.skinned, all.n));
            emit(buf);

            const char* kHdr =
                "    bucket                models  skinned      S1     S1v      S5      S6"
                "    COMB      S3      K1     K1v\n"
                "    --------------------------------------------------------------"
                "--------------------------------\n";
            auto row = [&](const std::string& name, const Bucket& b) {
                std::snprintf(buf, sizeof(buf),
                              "    %-20s %7zu %7.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%%"
                              " %6.1f%% %6.1f%% %6.1f%% %6.1f%%\n",
                              name.substr(0, 20).c_str(), b.n, pct(b.skinned, b.n),
                              pct(b.s1, b.n), pct(b.s1v, b.n), pct(b.s5, b.n),
                              pct(b.s6, b.n), pct(b.comb, b.n), pct(b.s3, b.n),
                              pct(b.k1, b.n), pct(b.k1v, b.n));
                emit(buf);
            };
            auto tableOf = [&](std::map<std::string, Bucket>& m, size_t topN) {
                std::vector<std::pair<std::string, Bucket>> bv(m.begin(), m.end());
                std::sort(bv.begin(), bv.end(), [](const auto& a2, const auto& b2) {
                    return a2.second.n > b2.second.n;
                });
                for (size_t i = 0; i < bv.size() && i < topN; ++i)
                    row(bv[i].first, bv[i].second);
            };

            emit("  [12a] coverage by top-level repository folder\n");
            emit(kHdr);
            tableOf(buckets, 20);
            row("ALL", all);
            emit("    S1  = <walk-up>/ani/*.anim               (what the resolver does today)\n"
                 "    S1v = <walk-up>/{ani,ain}/*.anim, case-insensitive\n"
                 "    S5  = <folder>/<assetName>/{ani,ain}/*.anim  (per-asset clip folder)\n"
                 "    S6  = any *.anim at/under <folder>/<assetName>\n"
                 "    COMB= S1v or S5 or S6                    S3 = any *.anim under <folder>\n"
                 "          (S3 is the ambiguous upper bound, not a candidate)\n"
                 "    K1  = <walk-up>/<leaf>.skeleton (today)  K1v = K1 or per-asset skeleton\n\n");

            emit("  [12f] coverage by second-level folder under Char/\n");
            emit(kHdr);
            tableOf(sub, 20);
            emit("\n");

            std::snprintf(buf, sizeof(buf),
                          "  [12c] clips the COMB strategy would attach per model\n"
                          "    1 clip %zu   2-5 %zu   6-20 %zu   21-100 %zu   >100 %zu\n"
                          "    (a fat >100 tail means the strategy is sweeping in clips that\n"
                          "     belong to other assets - it should stay thin)\n\n",
                          clipBucket[0], clipBucket[1], clipBucket[2], clipBucket[3],
                          clipBucket[4]);
            emit(buf);

            emit("  [12d] .anim parent-directory name histogram (top 25)\n");
            {
                std::vector<std::pair<std::string, size_t>> ph(animParentHist.begin(),
                                                               animParentHist.end());
                std::sort(ph.begin(), ph.end(),
                          [](const auto& a2, const auto& b2) { return a2.second > b2.second; });
                for (size_t i = 0; i < ph.size() && i < 25; ++i) {
                    std::snprintf(buf, sizeof(buf), "    %-28s %zu\n",
                                  ph[i].first.substr(0, 28).c_str(), ph[i].second);
                    emit(buf);
                }
                std::snprintf(buf, sizeof(buf), "    (%zu distinct parent dir names)\n\n",
                              ph.size());
                emit(buf);
            }

            std::snprintf(buf, sizeof(buf),
                          "  [12e] models COMB still misses (first %zu)\n",
                          misses.size());
            emit(buf);
            for (const std::string& m : misses) emit(m);
            if (misses.empty()) emit("      (none)\n");
            emit("\n");

            std::snprintf(buf, sizeof(buf), "  report written to %s\n", animOut.c_str());
            emit(buf);
        }
    }

    // ---- 13/14. animation via the repository graph -------------------------
    // Section 12 only ever asked "is there a .anim FILE near this model's
    // folder". A skinned model with no nearby clips may still animate: it binds
    // one of the archive's few shared SkinSkeletons, and the clips that drive
    // that skeleton live wherever the skeleton does. This measures that.
    if (!assetQuery.empty() || skinMap) {
        t0 = Clock::now();
        if (!store.repo()) {
            std::printf("[13] asset/skin-map: repository unavailable, skipped\n");
        } else {
            const di::Repository& r = *store.repo();
            char b2[1400];
            auto lower = [](std::string s) {
                for (char& c : s)
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                return s;
            };
            auto endsW = [](const std::string& s, const char* suf) {
                const size_t n = std::strlen(suf);
                return s.size() > n && s.compare(s.size() - n, n, suf) == 0;
            };
            std::unordered_map<std::string, size_t> aniDir, aniFirst, skelDir;
            for (size_t i = 0; i < mpk.entries().size(); ++i) {
                const std::string& n = mpk.entries()[i].name;
                const bool isA = endsW(n, ".anim");
                const bool isS = !isA && endsW(n, ".skeleton");
                if (!isA && !isS) continue;
                const size_t slash = n.find_last_of('/');
                if (slash == std::string::npos) continue;
                const std::string d = lower(n.substr(0, slash));
                if (isA) {
                    if (!aniDir.count(d)) aniFirst[d] = i;
                    aniDir[d]++;
                } else {
                    skelDir[d]++;
                }
            }
            auto aniAt = [&](const std::string& base) {
                const std::string lb = lower(base);
                size_t c = 0;
                auto i1 = aniDir.find(lb + "/ani");
                auto i2 = aniDir.find(lb + "/ain");
                if (i1 != aniDir.end()) c += i1->second;
                if (i2 != aniDir.end()) c += i2->second;
                return c;
            };
            auto exAt = [&](const std::string& base) -> std::string {
                const std::string lb = lower(base);
                for (const char* v : {"/ani", "/ain"}) {
                    auto it = aniFirst.find(lb + v);
                    if (it != aniFirst.end()) return mpk.entries()[it->second].name;
                }
                return std::string();
            };
            // walk the folder chain, return {clips, level, dir} of the first hit
            auto walkUp = [&](const std::string& folder, size_t* lvOut,
                              std::string* dirOut) {
                std::string f = folder;
                for (size_t lv = 0;; ++lv) {
                    const size_t c = aniAt("Package/" + f);
                    if (c) {
                        *lvOut = lv;
                        *dirOut = f;
                        return c;
                    }
                    const size_t p = f.find_last_of('/');
                    if (p == std::string::npos) break;
                    f.resize(p);
                }
                *lvOut = 0;
                dirOut->clear();
                return (size_t)0;
            };
            auto stripLod2 = [](const std::string& s) {
                size_t n = s.size();
                while (n > 0 && s[n - 1] >= '0' && s[n - 1] <= '9') --n;
                if (n >= 4 && s.compare(n - 4, 4, "_lod") == 0) return s.substr(0, n - 4);
                return s;
            };
            // the SkinSkeleton an entry binds: own rels, then one Model hop,
            // then the owning parent's rels (bare Mesh rows borrow it)
            auto skinOf = [&](size_t idx) -> size_t {
                for (int pass = 0; pass < 2; ++pass) {
                    size_t cur = idx;
                    if (pass == 1) {
                        cur = store.parentEntryOf(idx);
                        if (cur == (size_t)-1) break;
                    }
                    const di::RepoEntry& e = r.entries[cur];
                    for (const std::string& h : e.related) {
                        auto it = r.byHash.find(h);
                        if (it == r.byHash.end()) continue;
                        const di::RepoEntry& d = r.entries[it->second];
                        const std::string& dt = r.typeOf(d);
                        if (dt == "SkinSkeleton") return it->second;
                        if (dt == "Model") {
                            for (const std::string& h2 : d.related) {
                                auto i2 = r.byHash.find(h2);
                                if (i2 != r.byHash.end() &&
                                    r.typeOf(r.entries[i2->second]) == "SkinSkeleton")
                                    return i2->second;
                            }
                        }
                    }
                }
                return (size_t)-1;
            };

            // ---- 13. named asset trace -------------------------------------
            if (!assetQuery.empty()) {
                const std::string q = lower(assetQuery);
                std::vector<size_t> hits;
                for (size_t i = 0; i < r.entries.size() && hits.size() < 6; ++i)
                    if (lower(r.pathOf(r.entries[i])).find(q) != std::string::npos)
                        hits.push_back(i);
                std::printf("[13] asset trace for \"%s\": %zu shown\n",
                            assetQuery.c_str(), hits.size());
                for (size_t idx : hits) {
                    const di::RepoEntry& e = r.entries[idx];
                    const std::string folder = r.folderOf(e);
                    const size_t blob = store.blobForHash(e.hashHex);
                    std::printf("\n  %s\n    type %s   blob %s\n", r.pathOf(e).c_str(),
                                r.typeOf(e).empty() ? "(none)" : r.typeOf(e).c_str(),
                                blob == (size_t)-1
                                    ? "<unresolved>"
                                    : mpk.entries()[blob].name.c_str());
                    const size_t par = store.parentEntryOf(idx);
                    if (par != (size_t)-1)
                        std::printf("    parent %s (%s)\n",
                                    r.pathOf(r.entries[par]).c_str(),
                                    r.typeOf(r.entries[par]).c_str());
                    std::printf("    deps (%zu):\n", e.related.size());
                    for (const std::string& h : e.related) {
                        auto it = r.byHash.find(h);
                        if (it == r.byHash.end()) { std::printf("      <unresolved>\n"); continue; }
                        const di::RepoEntry& d = r.entries[it->second];
                        std::printf("      %-16s %s\n",
                                    r.typeOf(d).empty() ? "(none)" : r.typeOf(d).c_str(),
                                    r.pathOf(d).c_str());
                    }
                    // what the resolver would find today
                    const std::string own = "Package/" + folder;
                    const std::string nmB = stripLod2(e.name);
                    size_t perAsset = aniAt(own + "/" + e.name);
                    if (!perAsset && nmB != e.name) perAsset = aniAt(own + "/" + nmB);
                    size_t lv = 0;
                    std::string dir;
                    const size_t up = walkUp(folder, &lv, &dir);
                    std::printf("    clips by folder convention: per-asset %zu, "
                                "walk-up %zu%s%s\n",
                                perAsset, up,
                                up ? " at " : "", up ? dir.c_str() : "");
                    // per-clip verdict from the REAL parser: enumerate the
                    // clips this asset's folder walk reaches and parse each
                    const std::string ownDir = lower(own);
                    std::string clipDir;
                    for (const char* v : {"/ani", "/ain"})
                        if (aniDir.count(ownDir + v)) { clipDir = ownDir + v; break; }
                    if (clipDir.empty()) {
                        std::string f2 = folder;
                        size_t lv2 = 0;
                        std::string dir2;
                        if (walkUp(f2, &lv2, &dir2))
                            for (const char* v : {"/ani", "/ain"})
                                if (aniDir.count(lower("Package/" + dir2) + v)) {
                                    clipDir = lower("Package/" + dir2) + v;
                                    break;
                                }
                    }
                    if (!clipDir.empty()) {
                        size_t okC = 0, failC = 0;
                        std::vector<std::string> lines;
                        for (size_t q = 0; q < mpk.entries().size(); ++q) {
                            const std::string& cn = mpk.entries()[q].name;
                            if (cn.size() <= 5 ||
                                cn.compare(cn.size() - 5, 5, ".anim") != 0)
                                continue;
                            const size_t sl2 = cn.find_last_of('/');
                            if (sl2 == std::string::npos ||
                                lower(cn.substr(0, sl2)) != clipDir)
                                continue;
                            const std::vector<uint8_t> raw = mpk.readAsset(q);
                            {
                                fs::create_directories(
                                    fs::u8path("_probe/anim_samples"));
                                std::string fn = cn;
                                for (char& c2 : fn)
                                    if (c2 == '/' || c2 == '\\' || c2 == ':') c2 = '_';
                                std::ofstream fo(
                                    fs::u8path("_probe/anim_samples/" + fn),
                                    std::ios::binary);
                                fo.write((const char*)raw.data(),
                                         (std::streamsize)raw.size());
                            }
                            di::AnimClip clip;
                            std::string aerr;
                            if (!raw.empty() &&
                                di::parseAnim(raw.data(), raw.size(), &clip, &aerr)) {
                                ++okC;
                                if (lines.size() < 60) {
                                    char lb[512];
                                    std::snprintf(lb, sizeof(lb),
                                                  "      OK   %-40s %zu tracks, %u ms, %u arr%s\n",
                                                  cn.substr(sl2 + 1).c_str(),
                                                  clip.tracks.size(), clip.durationMs,
                                                  (unsigned)clip.keyArrays,
                                                  clip.mixedArrays ? " (mixed)" : "");
                                    lines.push_back(lb);
                                }
                            } else {
                                ++failC;
                                if (lines.size() < 60)
                                    lines.push_back("      FAIL " + cn.substr(sl2 + 1) +
                                                    "  " + aerr + "\n");
                            }
                        }
                        std::printf("    REAL-parser verdict on this asset's clips: "
                                    "%zu ok, %zu fail (all dumped to "
                                    "_probe/anim_samples/)\n",
                                    okC, failC);
                        for (const std::string& l2 : lines) std::fputs(l2.c_str(), stdout);
                    }
                    // what the SHARED skeleton offers
                    const size_t sk = skinOf(idx);
                    if (sk == (size_t)-1) {
                        std::printf("    SkinSkeleton: none (static mesh)\n");
                    } else {
                        const di::RepoEntry& se = r.entries[sk];
                        const std::string sf = r.folderOf(se);
                        size_t slv = 0;
                        std::string sdir;
                        const size_t sc = walkUp(sf, &slv, &sdir);
                        std::printf("    SkinSkeleton: %s\n"
                                    "      clips near the skeleton: %zu%s%s\n",
                                    r.pathOf(se).c_str(), sc,
                                    sc ? ", e.g. " : "",
                                    sc ? exAt("Package/" + sdir).c_str() : "");
                    }
                }
                std::printf("\n");
            }

            // ---- 14. skeleton sharing map ----------------------------------
            if (skinMap) {
                std::printf("[14] repository types (all %zu):\n", r.types.size());
                {
                    std::map<std::string, size_t> th;
                    for (const auto& e : r.entries)
                        th[r.typeOf(e).empty() ? "(none)" : r.typeOf(e)]++;
                    for (const auto& kv : th)
                        std::printf("    %-24s %zu\n", kv.first.c_str(), kv.second);
                }
                struct SB { size_t n = 0, skinned = 0, s1v = 0, s7 = 0; };
                std::map<std::string, SB> bk;
                std::map<size_t, size_t> skinUse;
                SB all;
                for (size_t i = 0; i < r.entries.size(); ++i) {
                    const di::RepoEntry& e = r.entries[i];
                    const std::string& t = r.typeOf(e);
                    if (t != "Model" && t != "LodModel") continue;
                    const std::string folder = r.folderOf(e);
                    if (folder.empty()) continue;
                    const size_t seg = folder.find('/');
                    SB& b = bk[seg == std::string::npos ? folder : folder.substr(0, seg)];
                    ++b.n;
                    ++all.n;
                    size_t lv = 0;
                    std::string dir;
                    const std::string nmB = stripLod2(e.name);
                    size_t own = aniAt("Package/" + folder + "/" + e.name);
                    if (!own && nmB != e.name) own = aniAt("Package/" + folder + "/" + nmB);
                    if (own || walkUp(folder, &lv, &dir)) { ++b.s1v; ++all.s1v; }
                    const size_t sk = skinOf(i);
                    if (sk == (size_t)-1) continue;
                    ++b.skinned;
                    ++all.skinned;
                    skinUse[sk]++;
                    size_t slv = 0;
                    std::string sdir;
                    if (walkUp(r.folderOf(r.entries[sk]), &slv, &sdir)) { ++b.s7; ++all.s7; }
                }
                auto pc = [](size_t a2, size_t b3) {
                    return b3 ? 100.0 * (double)a2 / (double)b3 : 0.0;
                };
                std::printf("\n  [14a] clips reachable via the bound SkinSkeleton\n"
                            "    bucket                models  skinned    COMB      S7\n"
                            "    ------------------------------------------------------\n");
                std::vector<std::pair<std::string, SB>> bv(bk.begin(), bk.end());
                std::sort(bv.begin(), bv.end(),
                          [](const auto& x, const auto& y) { return x.second.n > y.second.n; });
                for (size_t i = 0; i < bv.size() && i < 12; ++i) {
                    std::snprintf(b2, sizeof(b2),
                                  "    %-20s %7zu %7.1f%% %6.1f%% %6.1f%%\n",
                                  bv[i].first.substr(0, 20).c_str(), bv[i].second.n,
                                  pc(bv[i].second.skinned, bv[i].second.n),
                                  pc(bv[i].second.s1v, bv[i].second.n),
                                  pc(bv[i].second.s7, bv[i].second.n));
                    std::fputs(b2, stdout);
                }
                std::snprintf(b2, sizeof(b2), "    %-20s %7zu %7.1f%% %6.1f%% %6.1f%%\n",
                              "ALL", all.n, pc(all.skinned, all.n), pc(all.s1v, all.n),
                              pc(all.s7, all.n));
                std::fputs(b2, stdout);
                std::printf("    S7 = clips found by walking up the SKELETON's folder\n"
                            "         (not the model's) - the shared-rig route\n");

                std::vector<std::pair<size_t, size_t>> su(skinUse.begin(), skinUse.end());
                std::sort(su.begin(), su.end(),
                          [](const auto& x, const auto& y) { return x.second > y.second; });
                std::printf("\n  [14b] most-shared SkinSkeletons (top 20 of %zu used)\n",
                            su.size());
                for (size_t i = 0; i < su.size() && i < 20; ++i) {
                    size_t slv = 0;
                    std::string sdir;
                    const std::string sf = r.folderOf(r.entries[su[i].first]);
                    const size_t sc = walkUp(sf, &slv, &sdir);
                    std::snprintf(b2, sizeof(b2), "    %6zu models  %-52s  %zu clip(s)\n",
                                  su[i].second,
                                  r.pathOf(r.entries[su[i].first]).substr(0, 52).c_str(), sc);
                    std::fputs(b2, stdout);
                }
                std::printf("\n");
            }
            std::printf("[13/14] %.1f ms\n", msSince(t0));
        }
    }

    // ---- 15. the unread animation-adjacent families ------------------------
    // Folder convention and the skeleton route are both exhausted. What is left
    // unread: the 141 "Animation"-typed repository entries, and the .graph /
    // .mont / .motion / .human file families. If a .graph names clip paths, it -
    // not a folder walk - is the authoritative per-model clip list.
    if (animAssets) {
        t0 = Clock::now();
        const di::Repository* rp = store.repo();
        if (!rp) {
            std::printf("[15] anim-assets: repository unavailable, skipped\n");
        } else {
            const di::Repository& r = *rp;
            auto topOf = [](const std::string& s) {
                const size_t p = s.find('/');
                return p == std::string::npos ? s : s.substr(0, p);
            };

            // (a) the Animation-typed entries and who points at them
            std::vector<size_t> anims;
            for (size_t i = 0; i < r.entries.size(); ++i)
                if (r.typeOf(r.entries[i]) == "Animation") anims.push_back(i);
            std::unordered_map<std::string, std::vector<size_t>> refOf;
            for (size_t i = 0; i < r.entries.size(); ++i)
                for (const std::string& h : r.entries[i].related) refOf[h].push_back(i);

            std::printf("[15a] Animation-typed repository entries: %zu\n", anims.size());
            std::map<std::string, size_t> aTop;
            for (size_t i : anims) aTop[topOf(r.folderOf(r.entries[i]))]++;
            for (const auto& kv : aTop)
                std::printf("    %-20s %zu\n", kv.first.c_str(), kv.second);
            for (size_t k = 0; k < anims.size() && k < 12; ++k) {
                const di::RepoEntry& e = r.entries[anims[k]];
                const size_t blob = store.blobForHash(e.hashHex);
                std::printf("    %-58s %s\n", r.pathOf(e).substr(0, 58).c_str(),
                            blob == (size_t)-1
                                ? "<no blob>"
                                : mpk.entries()[blob].name.c_str());
                auto it = refOf.find(e.hashHex);
                if (it == refOf.end()) {
                    std::printf("        referenced by: nothing\n");
                } else {
                    std::printf("        referenced by %zu:\n", it->second.size());
                    for (size_t j = 0; j < it->second.size() && j < 3; ++j)
                        std::printf("          %-14s %s\n",
                                    r.typeOf(r.entries[it->second[j]]).c_str(),
                                    r.pathOf(r.entries[it->second[j]]).c_str());
                }
            }

            // do any Models reference an Animation at all?
            {
                std::unordered_set<std::string> animHash;
                for (size_t i : anims) animHash.insert(r.entries[i].hashHex);
                std::map<std::string, size_t> hit;
                size_t total = 0;
                for (const auto& e : r.entries) {
                    const std::string& t = r.typeOf(e);
                    if (t != "Model" && t != "LodModel") continue;
                    for (const std::string& h : e.related)
                        if (animHash.count(h)) {
                            hit[topOf(r.folderOf(e))]++;
                            ++total;
                            break;
                        }
                }
                std::printf("    Models/LodModels with an Animation dep: %zu\n", total);
                for (const auto& kv : hit)
                    std::printf("      %-20s %zu\n", kv.first.c_str(), kv.second);
            }

            // (b) the unread file families, by top-level folder
            static const char* kExts[] = {".graph", ".mont", ".motion", ".human"};
            std::printf("\n[15b] unread animation-adjacent file families\n");
            for (const char* ext : kExts) {
                const size_t el = std::strlen(ext);
                std::map<std::string, size_t> byTop;
                std::vector<size_t> samples;
                size_t n = 0;
                for (size_t i = 0; i < mpk.entries().size(); ++i) {
                    const std::string& nm = mpk.entries()[i].name;
                    if (nm.size() <= el || nm.compare(nm.size() - el, el, ext) != 0)
                        continue;
                    ++n;
                    std::string rest = nm.compare(0, 8, "Package/") == 0 ? nm.substr(8) : nm;
                    byTop[topOf(rest)]++;
                    if (samples.size() < 3) samples.push_back(i);
                }
                std::printf("  %s: %zu file(s)\n", ext, n);
                size_t shown = 0;
                for (const auto& kv : byTop) {
                    if (shown++ >= 8) break;
                    std::printf("      %-24s %zu\n", kv.first.c_str(), kv.second);
                }
                // Does the payload name clip paths? That is the whole question.
                for (size_t id : samples) {
                    const std::vector<uint8_t> raw = mpk.readAsset(id);
                    std::printf("    sample %s (%zu bytes inflated)\n",
                                mpk.entries()[id].name.c_str(), raw.size());
                    size_t runStart = (size_t)-1, printed = 0;
                    for (size_t i = 0; i <= raw.size() && printed < 10; ++i) {
                        const bool pr = i < raw.size() && raw[i] >= 0x20 && raw[i] < 0x7F;
                        if (pr) {
                            if (runStart == (size_t)-1) runStart = i;
                            continue;
                        }
                        if (runStart != (size_t)-1 && i - runStart >= 6) {
                            const std::string s((const char*)raw.data() + runStart,
                                                i - runStart);
                            if (s.find(".anim") != std::string::npos ||
                                s.find('/') != std::string::npos ||
                                s.find("ani") != std::string::npos) {
                                std::printf("        \"%s\"\n", s.substr(0, 90).c_str());
                                ++printed;
                            }
                        }
                        runStart = (size_t)-1;
                    }
                    if (!printed) std::printf("        (no path-like strings)\n");
                }
            }
            std::printf("\n[15] %.1f ms\n", msSince(t0));
        }
    }

    // ---- 16. .anim rotation-key tag census ---------------------------------
    // AnimParser refuses any track whose rot-key tag is not 0x32, because 0x32
    // is all the goldens ever showed - and every golden was a PLAYER clip.
    // Non-player rigs report tag 0. This walks every .anim header the same way
    // the parser does, but records unknown tags instead of bailing, and for each
    // one SOLVES the key stride: the only stride that leaves the following bytes
    // parsing as a valid next track is the real one.
    if (animTags) {
        t0 = Clock::now();
        std::ofstream rep(fs::u8path("anim_tag_report.txt"), std::ios::binary);
        auto emit = [&](const std::string& s) { std::fputs(s.c_str(), stdout); rep << s; };
        char b3[1200];

        auto rdU16 = [](const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8)); };
        auto rdU32 = [](const uint8_t* p) {
            return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
        };
        auto bucketOf = [](const std::string& nm) {
            // "Package/Char/npc/..." -> "Char/npc"
            std::string s = nm.compare(0, 8, "Package/") == 0 ? nm.substr(8) : nm;
            const size_t a1 = s.find('/');
            if (a1 == std::string::npos) return s;
            const size_t a2 = s.find('/', a1 + 1);
            return a2 == std::string::npos ? s : s.substr(0, a2);
        };
        // A plausible next-track header: printable name, sane length, parent
        // strictly before this track (the format's own invariant).
        auto looksLikeTrack = [&](const uint8_t* d, size_t n, size_t pos, uint32_t t) {
            if (pos + 1 > n) return false;
            const uint8_t nl = d[pos];
            if (nl < 2 || nl > 64 || pos + 1 + nl + 6 > n) return false;
            for (uint8_t k = 0; k < nl; ++k) {
                const uint8_t c = d[pos + 1 + k];
                if (c < 0x20 || c >= 0x7F) return false;
            }
            const int16_t par = (int16_t)rdU16(d + pos + 1 + nl);
            if (par < -1 || par >= (int)t) return false;
            const uint32_t pc = rdU32(d + pos + 1 + nl + 2);
            return pc <= 100000 && pos + 1 + nl + 6 + (size_t)pc * 14 <= n;
        };

        std::vector<size_t> animIds;
        for (size_t i = 0; i < mpk.entries().size(); ++i) {
            const std::string& nm = mpk.entries()[i].name;
            if (nm.size() > 5 && nm.compare(nm.size() - 5, 5, ".anim") == 0)
                animIds.push_back(i);
        }
        if (animTagsN && animIds.size() > animTagsN) animIds.resize(animTagsN);

        std::map<uint32_t, size_t> verHist;
        std::map<std::string, std::map<uint32_t, size_t>> verByBucket;
        std::map<uint32_t, size_t> tagHist, tagZeroRc;      // tag -> tracks
        std::map<uint32_t, std::map<int, size_t>> strideOf;  // tag -> stride -> n
        std::map<std::string, size_t> badFileByBucket;
        std::map<std::string, size_t> v3MismatchByBucket, v3OkByBucket;
        size_t files = 0, notAnim = 0, v2Files = 0, v2Clean = 0, v2Unknown = 0;
        std::vector<std::string> examples;

        // ---- forensics accumulators ------------------------------------
        // Everything needed to DECODE an unknown tag offline, so this is one
        // run and not a conversation: which bones/tracks carry it, the solved
        // stride, whether the bytes satisfy the v2 40-bit quaternion laws, and
        // raw key dumps next to a known-good 0x32 reference from the same file.
        struct Forensic {
            size_t tracks = 0, strideSolved = 0, keys = 0;
            size_t monoTracks = 0, endsAtDuration = 0;
            size_t hiZero = 0, sumsqOk = 0, selectorOk = 0;
            size_t rcMin = (size_t)-1, rcMax = 0, rcSum = 0;
            std::map<int, size_t> strideHist, trackIdxHist;
            std::map<std::string, size_t> boneHist;
            std::vector<std::string> dumps;
        };
        std::map<uint32_t, Forensic> forensic;
        // Layout prover: walk a whole v2 file assuming N u32-counted 14-byte key
        // arrays per track before the rot block, and demand EVERY track parse
        // with tag 0x32. The smallest N that survives is that file's real
        // layout. One array is what AnimParser assumes today.
        std::map<int, size_t> layoutHist;
        std::map<std::string, std::map<int, size_t>> layoutByBucket;
        std::map<int, std::map<std::string, size_t>> hdr12ByLayout;
        std::map<int, size_t> leftoverOk;
        std::vector<std::string> ref32Dumps, v3Dumps;
        size_t mixedTagFiles = 0;

        auto tryLayout = [&](const uint8_t* d, size_t n, uint32_t trackCount,
                             int arrays, size_t* endOut) {
            size_t pos = 36;
            for (uint32_t t = 0; t < trackCount; ++t) {
                if (pos + 1 > n) return false;
                const uint8_t nl = d[pos++];
                if (nl == 0 || pos + nl + 6 > n) return false;
                for (uint8_t k = 0; k < nl; ++k) {
                    const uint8_t c = d[pos + k];
                    if (c < 0x20 || c >= 0x7F) return false;
                }
                pos += nl;
                const int16_t par = (int16_t)rdU16(d + pos);
                pos += 2;
                if (par < -1 || par >= (int)t) return false;
                for (int a = 0; a < arrays; ++a) {
                    if (pos + 4 > n) return false;
                    const uint32_t c = rdU32(d + pos);
                    pos += 4;
                    if (c > 100000 || pos + (size_t)c * 14 > n) return false;
                    pos += (size_t)c * 14;
                }
                if (pos + 3 > n) return false;
                const uint32_t rc = rdU16(d + pos);
                const uint8_t tg = d[pos + 2];
                pos += 3;
                if (tg != 0x32) return false;
                if (pos + (size_t)rc * 7 > n) return false;
                pos += (size_t)rc * 7;
            }
            *endOut = pos;
            return true;
        };

        auto hexOf = [](const uint8_t* k, int len) {
            static const char* H = "0123456789abcdef";
            std::string o;
            for (int i = 0; i < len; ++i) {
                o += H[k[i] >> 4];
                o += H[k[i] & 15];
                o += ' ';
            }
            return o;
        };

        for (size_t id : animIds) {
            const std::vector<uint8_t> raw = mpk.readAsset(id);
            if (raw.size() < 40) { ++notAnim; continue; }
            const uint8_t* d = raw.data();
            const size_t n = raw.size();
            ++files;
            const std::string bucket = bucketOf(mpk.entries()[id].name);

            if (std::memcmp(d + 16, "CHAR::ANIM", 10) == 0) {
                const uint32_t ver = rdU16(d + 26);
                verHist[ver]++;
                verByBucket[bucket][ver]++;
                // does the v3 name table match the header count?
                const uint32_t hdrTracks = rdU32(d + 28);
                const uint32_t tblTracks = rdU16(d + 48);
                if (hdrTracks != tblTracks) v3MismatchByBucket[bucket]++;
                else v3OkByBucket[bucket]++;
                if (animForensics && hdrTracks != tblTracks && v3Dumps.size() < 10) {
                    std::string dmp = "      " + mpk.entries()[id].name +
                                      "\n        inflated " + std::to_string(n) +
                                      " B, header tracks " + std::to_string(hdrTracks) +
                                      ", table u16 @48 " + std::to_string(tblTracks) +
                                      ", dur " + std::to_string(rdU32(d + 32)) + "\n";
                    size_t marker = (size_t)-1;
                    for (size_t q = 50; q + 4 <= n && q < 200000; ++q)
                        if (rdU32(d + q) == 0xAC10AC10u) { marker = q; break; }
                    dmp += "        0xAC10AC10 marker at " +
                           (marker == (size_t)-1 ? std::string("NOT FOUND")
                                                 : std::to_string(marker)) +
                           "\n        bytes @16: " + hexOf(d + 16, 48) + "\n";
                    v3Dumps.push_back(dmp);
                }
                continue;
            }
            if (std::memcmp(d + 4, "CHAR::ANIM", 10) != 0) { ++notAnim; continue; }

            const uint32_t ver = rdU16(d + 14);
            verHist[ver]++;
            verByBucket[bucket][ver]++;
            const uint32_t trackCount = rdU32(d + 16);
            const uint32_t durationMs = rdU32(d + 20);
            if (trackCount == 0 || trackCount > 4096) continue;
            ++v2Files;

            if (animForensics) {
                int won = -1;
                size_t endAt = 0;
                for (int arrays = 0; arrays <= 4 && won < 0; ++arrays)
                    if (tryLayout(d, n, trackCount, arrays, &endAt)) won = arrays;
                layoutHist[won]++;
                layoutByBucket[bucket][won]++;
                if (won >= 0) {
                    const size_t left = n - endAt;
                    if (left <= 160) leftoverOk[won]++;
                    // the 12 unknown header bytes, as a hex key, so any field
                    // that predicts the array count shows up as a split here
                    hdr12ByLayout[won][hexOf(d + 24, 12)]++;
                }
            }

            size_t pos = 36;
            bool unknownHere = false, broke = false, sawKnown = false;
            std::string ref32;   // a known-good 0x32 track from THIS file
            for (uint32_t t = 0; t < trackCount && !broke; ++t) {
                if (pos + 1 > n) { broke = true; break; }
                const uint8_t nl = d[pos++];
                if (nl == 0 || pos + nl + 6 > n) { broke = true; break; }
                const std::string tname((const char*)d + pos, nl);
                pos += nl + 2;
                const uint32_t pc = rdU32(d + pos);
                pos += 4;
                if (pc > 100000 || pos + (size_t)pc * 14 > n) { broke = true; break; }
                pos += (size_t)pc * 14;
                if (pos + 3 > n) { broke = true; break; }
                const uint32_t rc = rdU16(d + pos);
                const uint32_t tag = d[pos + 2];
                pos += 3;
                tagHist[tag]++;
                if (rc == 0) tagZeroRc[tag]++;
                if (tag == 0x32) {
                    if (pos + (size_t)rc * 7 > n) { broke = true; break; }
                    sawKnown = true;
                    if (animForensics && ref32.empty() && rc >= 4) {
                        ref32 = "      ref 0x32 \"" + tname + "\" rc " +
                                std::to_string(rc) + " dur " +
                                std::to_string(durationMs) + " keys(7B): ";
                        for (int q = 0; q < 4; ++q)
                            ref32 += "[" + hexOf(d + pos + q * 7, 7) + "] ";
                        ref32 += "\n";
                    }
                    pos += (size_t)rc * 7;
                    continue;
                }
                // Unknown tag: solve the stride. Stride 0 is meaningful too
                // (rc keys but zero bytes = count is not a key count here).
                unknownHere = true;
                int solved = -1;
                int hits = 0;
                for (int s = 0; s <= 32; ++s) {
                    const size_t nxt = pos + (size_t)rc * (size_t)s;
                    if (nxt > n) break;
                    if (t + 1 == trackCount || looksLikeTrack(d, n, nxt, t + 1)) {
                        if (solved < 0) solved = s;
                        ++hits;
                    }
                }
                strideOf[tag][hits == 1 ? solved : (solved < 0 ? -1 : -2)]++;
                if (animForensics) {
                    Forensic& fi = forensic[tag];
                    ++fi.tracks;
                    fi.boneHist[tname]++;
                    fi.trackIdxHist[(int)(t < 8 ? t : 8)]++;
                    fi.rcMin = rc < fi.rcMin ? rc : fi.rcMin;
                    fi.rcMax = rc > fi.rcMax ? rc : fi.rcMax;
                    fi.rcSum += rc;
                    fi.strideHist[hits == 1 ? solved : -1]++;
                    if (hits == 1 && solved >= 3 && rc > 0 &&
                        pos + (size_t)rc * (size_t)solved <= n) {
                        ++fi.strideSolved;
                        const uint8_t* kb = d + pos;
                        uint32_t prevT = 0;
                        bool mono = true;
                        const uint32_t lim = rc < 64 ? rc : 64;
                        for (uint32_t i2 = 0; i2 < lim; ++i2) {
                            const uint8_t* k = kb + (size_t)i2 * (size_t)solved;
                            const uint32_t tm = rdU16(k);
                            if (i2 && tm < prevT) mono = false;
                            prevT = tm;
                            ++fi.keys;
                            if (solved < 7) continue;
                            // the v2 40-bit quaternion law, applied to these bytes
                            uint64_t v = 0;
                            for (int q = 0; q < 5; ++q)
                                v |= (uint64_t)k[2 + q] << (8 * q);
                            if (((v >> 38) & 3) == 0) ++fi.hiZero;
                            if (((v >> 36) & 3) <= 3) ++fi.selectorOk;
                            double sq = 0.0;
                            for (int j = 0; j < 3; ++j) {
                                const double f =
                                    (double)((v >> (12 * j)) & 0xFFF);
                                const double val = (f - 2047.5) / (2048.0 * 1.414213562);
                                sq += val * val;
                            }
                            if (sq <= 1.0) ++fi.sumsqOk;
                        }
                        if (mono) ++fi.monoTracks;
                        const long dd = (long)prevT - (long)durationMs;
                        if (dd > -40 && dd < 40) ++fi.endsAtDuration;
                        if (fi.dumps.size() < 12) {
                            std::string dmp = "      " + mpk.entries()[id].name +
                                              "\n        track " + std::to_string(t) +
                                              " \"" + tname + "\" rc " +
                                              std::to_string(rc) + " stride " +
                                              std::to_string(solved) + " dur " +
                                              std::to_string(durationMs) + "\n        keys: ";
                            const uint32_t sh = rc < 4 ? rc : 4;
                            for (uint32_t q = 0; q < sh; ++q)
                                dmp += "[" + hexOf(kb + (size_t)q * (size_t)solved,
                                                   solved) + "] ";
                            dmp += "\n";
                            fi.dumps.push_back(dmp);
                        }
                    }
                }
                if (examples.size() < 25) {
                    std::snprintf(b3, sizeof(b3),
                                  "    %s\n      track %u \"%s\": tag %u, rc %u, "
                                  "stride %s (%d candidate%s)\n",
                                  mpk.entries()[id].name.c_str(), t, tname.c_str(),
                                  tag, rc,
                                  solved < 0 ? "NONE" : std::to_string(solved).c_str(),
                                  hits, hits == 1 ? "" : "s");
                    examples.push_back(b3);
                }
                if (hits == 1 && solved >= 0) {
                    pos += (size_t)rc * (size_t)solved;   // keep walking
                } else {
                    broke = true;
                }
            }
            if (unknownHere && sawKnown) ++mixedTagFiles;
            if (animForensics && unknownHere && !ref32.empty() && ref32Dumps.size() < 12)
                ref32Dumps.push_back("      " + mpk.entries()[id].name + "\n" + ref32);
            if (unknownHere) { ++v2Unknown; badFileByBucket[bucket]++; }
            else if (!broke) ++v2Clean;
        }

        std::snprintf(b3, sizeof(b3),
                      "[16] .anim tag census: %.1f ms over %zu file(s)\n"
                      "  readable %zu, not CHAR::ANIM %zu\n"
                      "  v2 files %zu - clean %zu, hit an unknown tag %zu\n\n",
                      msSince(t0), animIds.size(), files, notAnim, v2Files, v2Clean,
                      v2Unknown);
        emit(b3);

        emit("  [16a] container version histogram\n");
        for (const auto& kv : verHist) {
            std::snprintf(b3, sizeof(b3), "    version %-4u %zu\n", kv.first, kv.second);
            emit(b3);
        }
        emit("\n  [16b] rot-key tag histogram (per TRACK)\n");
        for (const auto& kv : tagHist) {
            std::snprintf(b3, sizeof(b3),
                          "    tag %-5u %8zu track(s)   of which rot-key-count 0: %zu\n",
                          kv.first, kv.second,
                          tagZeroRc.count(kv.first) ? tagZeroRc[kv.first] : 0);
            emit(b3);
        }
        emit("\n  [16c] solved key stride per unknown tag (bytes per key)\n"
             "        -1 = no stride works, -2 = ambiguous (several fit)\n");
        for (const auto& kv : strideOf) {
            for (const auto& sv : kv.second) {
                std::snprintf(b3, sizeof(b3), "    tag %-5u stride %-4d %zu track(s)\n",
                              kv.first, sv.first, sv.second);
                emit(b3);
            }
        }
        emit("\n  [16d] files hitting an unknown tag, by folder\n");
        {
            std::vector<std::pair<std::string, size_t>> bv(badFileByBucket.begin(),
                                                           badFileByBucket.end());
            std::sort(bv.begin(), bv.end(),
                      [](const auto& x, const auto& y) { return x.second > y.second; });
            for (size_t i = 0; i < bv.size() && i < 25; ++i) {
                std::snprintf(b3, sizeof(b3), "    %-34s %zu\n",
                              bv[i].first.substr(0, 34).c_str(), bv[i].second);
                emit(b3);
            }
        }
        emit("\n  [16e] v3 name-table vs header track count\n");
        {
            std::map<std::string, std::pair<size_t, size_t>> j;
            for (const auto& kv : v3OkByBucket) j[kv.first].first = kv.second;
            for (const auto& kv : v3MismatchByBucket) j[kv.first].second = kv.second;
            std::vector<std::pair<std::string, std::pair<size_t, size_t>>> jv(j.begin(),
                                                                              j.end());
            std::sort(jv.begin(), jv.end(), [](const auto& x, const auto& y) {
                return x.second.second > y.second.second;
            });
            for (size_t i = 0; i < jv.size() && i < 20; ++i) {
                std::snprintf(b3, sizeof(b3), "    %-34s match %zu  mismatch %zu\n",
                              jv[i].first.substr(0, 34).c_str(), jv[i].second.first,
                              jv[i].second.second);
                emit(b3);
            }
        }
        emit("\n  [16f] examples\n");
        for (const std::string& e : examples) emit(e);

        if (animForensics) {
            std::snprintf(b3, sizeof(b3),
                          "\n  [16g] files mixing 0x32 with an unknown tag: %zu\n"
                          "        (non-zero means the tag is a PER-TRACK encoding\n"
                          "         choice, not a property of the rig or the file)\n\n",
                          mixedTagFiles);
            emit(b3);

            for (const auto& kv : forensic) {
                const Forensic& f = kv.second;
                std::snprintf(b3, sizeof(b3),
                              "  [16h] tag %u forensics\n"
                              "    tracks %zu (stride solved on %zu), keys sampled %zu\n"
                              "    rot-key count: min %zu  max %zu  mean %.1f\n",
                              kv.first, f.tracks, f.strideSolved, f.keys,
                              f.rcMin == (size_t)-1 ? 0 : f.rcMin, f.rcMax,
                              f.tracks ? (double)f.rcSum / (double)f.tracks : 0.0);
                emit(b3);
                emit("    solved stride histogram (-1 = unsolved/ambiguous):\n");
                for (const auto& sv : f.strideHist) {
                    std::snprintf(b3, sizeof(b3), "      %-4d %zu\n", sv.first, sv.second);
                    emit(b3);
                }
                // THE decisive test: do these bytes obey the v2 quaternion law?
                const double kk = f.keys ? (double)f.keys : 1.0;
                std::snprintf(b3, sizeof(b3),
                              "    v2 40-bit quaternion law applied to these bytes\n"
                              "    (chance baselines measured on uniform random bytes,\n"
                              "     20k samples - read each number against ITS baseline):\n"
                              "      bits 38-39 == 0        %.1f%%   [chance 24.3%% - DECISIVE]\n"
                              "      sum of squares <= 1    %.1f%%   [chance 96.7%% - weak, ignore alone]\n"
                              "      time non-decreasing    %.1f%% of tracks\n"
                              "      last time == duration  %.1f%% of tracks\n"
                              "      Verdict rule: bits 38-39 near 100%% AND time monotonic\n"
                              "      near 100%% => tag %u is the SAME 40-bit encoding as 0x32\n"
                              "      and the parser only has to accept the tag. If bits 38-39\n"
                              "      sits near 24%% it is a DIFFERENT encoding - use the raw\n"
                              "      dumps below against the 0x32 reference.\n",
                              100.0 * (double)f.hiZero / kk,
                              100.0 * (double)f.sumsqOk / kk,
                              f.strideSolved ? 100.0 * (double)f.monoTracks /
                                                   (double)f.strideSolved : 0.0,
                              f.strideSolved ? 100.0 * (double)f.endsAtDuration /
                                                   (double)f.strideSolved : 0.0,
                              kv.first);
                emit(b3);
                emit("    track index carrying this tag (8 = 8th or later):\n");
                for (const auto& tv : f.trackIdxHist) {
                    std::snprintf(b3, sizeof(b3), "      track %-3d %zu\n", tv.first,
                                  tv.second);
                    emit(b3);
                }
                emit("    bones carrying this tag (top 15):\n");
                {
                    std::vector<std::pair<std::string, size_t>> bv(f.boneHist.begin(),
                                                                   f.boneHist.end());
                    std::sort(bv.begin(), bv.end(), [](const auto& x, const auto& y) {
                        return x.second > y.second;
                    });
                    for (size_t i = 0; i < bv.size() && i < 15; ++i) {
                        std::snprintf(b3, sizeof(b3), "      %-28s %zu\n",
                                      bv[i].first.substr(0, 28).c_str(), bv[i].second);
                        emit(b3);
                    }
                    std::snprintf(b3, sizeof(b3), "      (%zu distinct bones)\n",
                                  bv.size());
                    emit(b3);
                }
                emit("    raw key dumps:\n");
                for (const std::string& dd : f.dumps) emit(dd);
                emit("\n");
            }

            emit("  [16i] known-good 0x32 reference keys from the SAME files\n"
                 "        (diff these against the dumps above)\n");
            for (const std::string& dd : ref32Dumps) emit(dd);

            emit("\n  [16k] LAYOUT PROVER - how many u32-counted 14-byte key\n"
                 "        arrays each v2 track really carries before the rot block\n"
                 "        (-1 = no array count 0..4 parses the file cleanly)\n");
            for (const auto& kv : layoutHist) {
                std::snprintf(b3, sizeof(b3),
                              "    %2d array(s): %6zu file(s)   (%zu also end within"
                              " 160 B of EOF)\n",
                              kv.first, kv.second,
                              leftoverOk.count(kv.first) ? leftoverOk[kv.first] : 0);
                emit(b3);
            }
            emit("    AnimParser assumes 1. Any bucket above 1 is a whole key\n"
                 "    array it never reads.\n\n");
            emit("    by folder (top 20):\n");
            {
                std::vector<std::pair<std::string, std::map<int, size_t>>> lv(
                    layoutByBucket.begin(), layoutByBucket.end());
                std::sort(lv.begin(), lv.end(), [](const auto& x, const auto& y) {
                    size_t sx = 0, sy = 0;
                    for (const auto& q : x.second) sx += q.second;
                    for (const auto& q : y.second) sy += q.second;
                    return sx > sy;
                });
                for (size_t i = 0; i < lv.size() && i < 20; ++i) {
                    std::string line = "      " + lv[i].first.substr(0, 30);
                    while (line.size() < 38) line += ' ';
                    for (const auto& q : lv[i].second)
                        line += std::to_string(q.first) + ":" +
                                std::to_string(q.second) + "  ";
                    line += "\n";
                    emit(line);
                }
            }
            emit("\n    12 unknown header bytes @24, grouped by proven layout\n"
                 "    (a value that appears under ONE layout only is the field\n"
                 "     that selects it - that is the discriminator to read):\n");
            for (const auto& kv : hdr12ByLayout) {
                std::snprintf(b3, sizeof(b3), "      layout %d: %zu distinct value(s)\n",
                              kv.first, kv.second.size());
                emit(b3);
                std::vector<std::pair<std::string, size_t>> hv(kv.second.begin(),
                                                               kv.second.end());
                std::sort(hv.begin(), hv.end(),
                          [](const auto& x, const auto& y) { return x.second > y.second; });
                for (size_t i = 0; i < hv.size() && i < 8; ++i) {
                    std::snprintf(b3, sizeof(b3), "        %s  %zu\n",
                                  hv[i].first.c_str(), hv[i].second);
                    emit(b3);
                }
            }

            emit("\n  [16j] v3 files whose name table disagrees with the header\n");
            if (v3Dumps.empty()) emit("      (none)\n");
            for (const std::string& dd : v3Dumps) emit(dd);
        }
        emit("\n  report written to anim_tag_report.txt\n");
    }

    // ---- 17. REAL-parser verification sweep --------------------------------
    // Every census above re-implements pieces of the walk; this runs the
    // ACTUAL di::parseAnim that the app ships over every .anim and reports
    // what plays now, what still fails and WHY - per folder, with header hex
    // for the failure families so the next fix needs no extra run.
    if (animVerify) {
        t0 = Clock::now();
        auto bucketOf = [](const std::string& nm) {
            std::string s2 = nm.compare(0, 8, "Package/") == 0 ? nm.substr(8) : nm;
            const size_t a1 = s2.find('/');
            if (a1 == std::string::npos) return s2;
            const size_t a2 = s2.find('/', a1 + 1);
            return a2 == std::string::npos ? s2 : s2.substr(0, a2);
        };
        auto normErr = [](std::string e) {
            for (char& c : e)
                if (c >= '0' && c <= '9') c = '#';
            if (e.size() > 72) e.resize(72);
            return e;
        };
        auto hex64 = [](const uint8_t* p, size_t n2) {
            static const char* H = "0123456789abcdef";
            std::string o;
            const size_t lim = n2 < 64 ? n2 : 64;
            for (size_t i = 0; i < lim; ++i) {
                o += H[p[i] >> 4];
                o += H[p[i] & 15];
                o += (i % 16 == 15) ? '\n' : ' ';
                if (i % 16 == 15) o += "          ";
            }
            o += '\n';
            return o;
        };

        // Failing exemplars land on disk so the next decode step has real
        // bytes without another probe run - the bridge picks them up directly.
        fs::create_directories(fs::u8path("_probe/anim_samples"));
        std::map<std::string, size_t> dumped;
        auto flatName = [](std::string s2) {
            for (char& c : s2)
                if (c == '/' || c == '\\' || c == ':') c = '_';
            return s2;
        };
        size_t total = 0, ok = 0, arr1 = 0, arr2 = 0, mixed = 0;
        std::map<std::string, std::pair<size_t, size_t>> perBucket;  // ok/total
        // per-version parse counts + v3 pose-quality audit (the new decoder):
        // a clip that PARSES can still decode to garbage, so check every v3
        // rotation is unit-norm and every position is finite/bounded.
        std::map<int, size_t> verParsed;
        size_t v3n = 0, v3badQ = 0, v3badP = 0, v3noRot = 0;
        size_t v3dumpQ = 0, v3dumpP = 0, v3dumpR = 0;   // exemplar dump caps
        double v3worstQ = 0, v3worstP = 0;
        std::string v3worstQnm, v3worstPnm;
        std::map<std::string, std::pair<size_t, size_t>> v3bucket;  // clean/total
        std::map<std::string, size_t> errHist;
        // one header dump per distinct (version, first-24-bytes) failure family
        std::map<std::string, std::pair<size_t, std::string>> failFam;
        for (size_t i = 0; i < mpk.entries().size(); ++i) {
            const std::string& nm = mpk.entries()[i].name;
            if (nm.size() <= 5 || nm.compare(nm.size() - 5, 5, ".anim") != 0)
                continue;
            const std::vector<uint8_t> raw = mpk.readAsset(i);
            if (raw.empty()) continue;
            ++total;
            const std::string bucket = bucketOf(nm);
            perBucket[bucket].second++;
            di::AnimClip clip;
            std::string aerr;
            if (di::parseAnim(raw.data(), raw.size(), &clip, &aerr)) {
                ++ok;
                perBucket[bucket].first++;
                if (clip.mixedArrays) ++mixed;
                else if (clip.keyArrays == 2) ++arr2;
                else ++arr1;
                verParsed[clip.version]++;
                if (clip.version == 3) {
                    ++v3n;
                    v3bucket[bucket].second++;
                    double worstQ = 0, worstP = 0;
                    bool anyRot = false, nonfinite = false;
                    for (const auto& tr : clip.tracks) {
                        if (!tr.rot.empty()) anyRot = true;
                        for (const auto& rk : tr.rot) {
                            const double L = std::sqrt((double)rk.q[0]*rk.q[0] +
                                (double)rk.q[1]*rk.q[1] + (double)rk.q[2]*rk.q[2] +
                                (double)rk.q[3]*rk.q[3]);
                            const double e = std::fabs(L - 1.0);
                            if (e > worstQ) worstQ = e;
                            if (!std::isfinite(L)) nonfinite = true;
                        }
                        for (const auto& pk : tr.pos) {
                            const double m = std::sqrt((double)pk.p[0]*pk.p[0] +
                                (double)pk.p[1]*pk.p[1] + (double)pk.p[2]*pk.p[2]);
                            if (m > worstP) worstP = m;
                            if (!std::isfinite(m)) nonfinite = true;
                        }
                    }
                    bool clean = true;
                    auto dumpV3 = [&](const char* tag) {
                        std::ofstream fo(fs::u8path("_probe/anim_samples/v3_" +
                                          std::string(tag) + "_" + flatName(nm)),
                                         std::ios::binary);
                        fo.write((const char*)raw.data(), (std::streamsize)raw.size());
                    };
                    if (!anyRot) {
                        ++v3noRot; clean = false;
                        if (v3dumpR++ < 3) dumpV3("norot");
                    }
                    if (worstQ > 1e-2 || nonfinite) {
                        ++v3badQ; clean = false;
                        if (v3dumpQ++ < 4) dumpV3("badQ");
                    }
                    if (worstP > 200.0 || nonfinite) {
                        ++v3badP; clean = false;
                        if (v3dumpP++ < 4) dumpV3("badP");
                    }
                    if (clean) v3bucket[bucket].first++;
                    if (worstQ > v3worstQ) { v3worstQ = worstQ; v3worstQnm = nm; dumpV3("worstQ"); }
                    if (worstP > v3worstP) { v3worstP = worstP; v3worstPnm = nm;
                        if (std::isfinite(worstP)) dumpV3("worstP"); }
                }
            } else {
                errHist[normErr(aerr)]++;
                if (dumped[normErr(aerr)]++ < 3) {
                    std::ofstream fo(fs::u8path("_probe/anim_samples/" + flatName(nm)),
                                     std::ios::binary);
                    fo.write((const char*)raw.data(), (std::streamsize)raw.size());
                }
                std::string sig;
                {
                    static const char* H = "0123456789abcdef";
                    const size_t lim = raw.size() < 28 ? raw.size() : 28;
                    for (size_t q = 0; q < lim; ++q) {
                        sig += H[raw[q] >> 4];
                        sig += H[raw[q] & 15];
                    }
                }
                auto& fam = failFam[normErr(aerr) + "|" + sig.substr(0, 32)];
                if (!fam.first)
                    fam.second = "      " + nm + "\n          " + hex64(raw.data(), raw.size());
                fam.first++;
            }
        }
        std::printf("[17] REAL-parser verify: %.1f ms\n"
                    "  %zu clips: %zu parse (%.1f%%), %zu fail\n"
                    "  parsed layouts: 1-array %zu, 2-array %zu, per-track mixed %zu\n\n",
                    msSince(t0), total, ok, total ? 100.0 * (double)ok / (double)total : 0.0,
                    total - ok, arr1, arr2, mixed);
        std::printf("  [17a] per folder (worst 25 by failures)\n");
        {
            std::vector<std::pair<std::string, std::pair<size_t, size_t>>> pv(
                perBucket.begin(), perBucket.end());
            std::sort(pv.begin(), pv.end(), [](const auto& x, const auto& y) {
                return (x.second.second - x.second.first) >
                       (y.second.second - y.second.first);
            });
            for (size_t i = 0; i < pv.size() && i < 25; ++i)
                std::printf("    %-34s %6zu/%zu ok  (%zu fail)\n",
                            pv[i].first.substr(0, 34).c_str(), pv[i].second.first,
                            pv[i].second.second,
                            pv[i].second.second - pv[i].second.first);
        }
        std::printf("\n  [17b] failure reasons (digits masked)\n");
        {
            std::vector<std::pair<std::string, size_t>> ev(errHist.begin(), errHist.end());
            std::sort(ev.begin(), ev.end(),
                      [](const auto& x, const auto& y) { return x.second > y.second; });
            for (const auto& kv : ev)
                std::printf("    %6zu  %s\n", kv.second, kv.first.c_str());
        }
        std::printf("\n  [17c] failure families - one 64-byte header dump each\n"
                    "        (top 20 by count; enough to crack the next layout)\n");
        {
            std::vector<std::pair<std::string, std::pair<size_t, std::string>>> fv(
                failFam.begin(), failFam.end());
            std::sort(fv.begin(), fv.end(), [](const auto& x, const auto& y) {
                return x.second.first > y.second.first;
            });
            for (size_t i = 0; i < fv.size() && i < 20; ++i) {
                std::printf("    family x%zu  [%s]\n%s",
                            fv[i].second.first,
                            fv[i].first.substr(0, fv[i].first.find('|')).c_str(),
                            fv[i].second.second.c_str());
            }
        }
        std::printf("\n  [17d] parsed clips by version\n");
        for (const auto& kv : verParsed)
            std::printf("    v%d: %zu\n", kv.first, kv.second);
        std::printf("\n  [17e] v3 pose-quality audit (%zu v3 clips parsed)\n"
                    "    non-unit rotation (|q|-1 > 1e-2): %zu\n"
                    "    extreme position (|p| > 200):      %zu\n"
                    "    no rotation tracks at all:         %zu\n"
                    "    worst |q|-1 = %.4g  (%s)\n"
                    "    worst |p|   = %.4g  (%s)\n",
                    v3n, v3badQ, v3badP, v3noRot,
                    v3worstQ, v3worstQnm.c_str(), v3worstP, v3worstPnm.c_str());
        {
            std::printf("\n  [17f] v3 folders with unclean clips (worst 25)\n");
            std::vector<std::pair<std::string, std::pair<size_t, size_t>>> vv(
                v3bucket.begin(), v3bucket.end());
            std::sort(vv.begin(), vv.end(), [](const auto& x, const auto& y) {
                return (x.second.second - x.second.first) >
                       (y.second.second - y.second.first);
            });
            for (size_t i = 0; i < vv.size() && i < 25; ++i) {
                const size_t bad = vv[i].second.second - vv[i].second.first;
                if (bad == 0) break;
                std::printf("    %-34s %6zu/%zu clean  (%zu unclean)\n",
                            vv[i].first.substr(0, 34).c_str(), vv[i].second.first,
                            vv[i].second.second, bad);
            }
        }
        std::printf("\n");
    }

    std::printf("\ndi_probe done.\n");
    return 0;
}
