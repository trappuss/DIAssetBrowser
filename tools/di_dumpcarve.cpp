// di_dumpcarve - pull the Messiah animation decoder (unpacked) out of a
// minidump. DiabloImmortal.exe on disk is packed (encrypted .jNS section); the
// UNPACKED code exists only in the running process, captured in a full-memory
// .DMP (Task Manager -> Create dump file). This tool never needs the whole
// 3.2 GB off the machine: it locates the decode function inside the dump and
// carves the few KB around it for offline disassembly.
//
// Method (no guessing - every carve is anchored to a real cross-reference):
//   1. Parse the minidump: Memory64List (VA<->file map), ModuleList (find the
//      DiabloImmortal.exe image base), MemoryInfoList (which pages execute).
//   2. Find the format strings in memory: "CHAR::ANIM", "CHANNEL_DATA",
//      "MOTION_DATA", "TRACKS_DATA". Record their virtual addresses.
//   3. Scan EXECUTABLE pages for RIP-relative operands (lea/mov/cmp) whose
//      target == one of those string VAs. Each hit is an instruction INSIDE the
//      function that reads that section - i.e. the decoder. Carve a window.
//   4. Also carve around the 0xAC10AC10 block-marker immediate where it appears
//      as code (executable page), which is the per-frame block dispatch.
//   5. Write carves + a report (module base, string VAs, per-carve VA/RVA) so
//      the disassembler has full address context.
//
// std-only, no Qt. Usage: di_dumpcarve <dump.DMP> [outDir]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

uint32_t rdU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
uint64_t rdU64(const uint8_t* p) {
    return (uint64_t)rdU32(p) | ((uint64_t)rdU32(p + 4) << 32);
}
int32_t rdI32(const uint8_t* p) { return (int32_t)rdU32(p); }

// One resident memory range: [va, va+size) lives at file offset foff.
struct MemRange {
    uint64_t va = 0;
    uint64_t size = 0;
    uint64_t foff = 0;
    bool exec = false;
};

// VA -> file offset via the range table (sorted by va).
struct MemMap {
    std::vector<MemRange> ranges;   // sorted by va

    const MemRange* find(uint64_t va) const {
        size_t lo = 0, hi = ranges.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (va < ranges[mid].va) hi = mid;
            else lo = mid + 1;
        }
        if (lo == 0) return nullptr;
        const MemRange& r = ranges[lo - 1];
        return (va >= r.va && va < r.va + r.size) ? &r : nullptr;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: di_dumpcarve <dump.DMP> [outDir]\n");
        return 2;
    }
    const std::string dumpPath = argv[1];
    const std::string outDir = argc > 2 ? argv[2] : "dump_carves";
    // Any trailing 0x... args are explicit function VAs to carve (16 KB each) —
    // used to follow a call the string-anchored pass revealed.
    std::vector<uint64_t> explicitVas;
    std::vector<std::pair<uint64_t, uint64_t>> regions;   // (va, size) for "va:size"
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const size_t colon = a.find(':');
        if (colon != std::string::npos) {
            const uint64_t va = std::strtoull(a.c_str(), nullptr, 16);
            const uint64_t sz = std::strtoull(a.c_str() + colon + 1, nullptr, 16);
            regions.push_back({va, sz});
        } else if (a.size() > 2 && a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) {
            explicitVas.push_back(std::strtoull(a.c_str() + 2, nullptr, 16));
        }
    }

    std::ifstream f(fs::u8path(dumpPath), std::ios::binary);
    if (!f) {
        std::printf("FAIL: cannot open %s\n", dumpPath.c_str());
        return 1;
    }
    f.seekg(0, std::ios::end);
    const uint64_t fileSize = (uint64_t)f.tellg();
    f.seekg(0);
    std::printf("dump: %s (%.2f GB)\n", dumpPath.c_str(), fileSize / 1e9);

    // Read the whole dump into RAM (a 3-4 GB alloc; the machine that produced
    // the dump has the headroom). Streaming would complicate the VA scan.
    std::vector<uint8_t> buf(fileSize);
    if (!f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)fileSize)) {
        std::printf("FAIL: short read\n");
        return 1;
    }
    const uint8_t* d = buf.data();
    const uint64_t n = fileSize;

    if (n < 32 || std::memcmp(d, "MDMP", 4) != 0) {
        std::printf("FAIL: not a minidump (missing MDMP signature). Recreate the\n"
                    "dump with Task Manager -> right-click DiabloImmortal.exe ->\n"
                    "Create dump file.\n");
        return 1;
    }

    const uint32_t nStreams = rdU32(d + 8);
    const uint32_t dirRva = rdU32(d + 12);
    std::printf("minidump: %u streams, directory @ %u\n", nStreams, dirRva);

    MemMap mem;
    uint64_t moduleBase = 0, moduleSize = 0;
    std::string moduleName;
    // exec ranges from MemoryInfoList, applied to Memory64 ranges afterward
    std::vector<std::pair<uint64_t, uint64_t>> execRegions;  // (base, size)

    for (uint32_t i = 0; i < nStreams; ++i) {
        const uint64_t e = (uint64_t)dirRva + (uint64_t)i * 12;
        if (e + 12 > n) break;
        const uint32_t type = rdU32(d + e);
        const uint32_t dsize = rdU32(d + e + 4);
        const uint32_t rva = rdU32(d + e + 8);
        if ((uint64_t)rva + dsize > n) continue;

        if (type == 9) {  // Memory64ListStream
            const uint64_t count = rdU64(d + rva);
            uint64_t base = rdU64(d + rva + 8);
            const uint8_t* rec = d + rva + 16;
            for (uint64_t k = 0; k < count; ++k) {
                const uint64_t va = rdU64(rec + k * 16);
                const uint64_t sz = rdU64(rec + k * 16 + 8);
                mem.ranges.push_back({va, sz, base, false});
                base += sz;
            }
        } else if (type == 4) {  // ModuleListStream
            const uint32_t nMod = rdU32(d + rva);
            const uint8_t* m = d + rva + 4;
            for (uint32_t k = 0; k < nMod; ++k) {
                const uint8_t* md = m + (size_t)k * 108;  // MINIDUMP_MODULE
                const uint64_t base = rdU64(md);
                const uint32_t sz = rdU32(md + 8);
                const uint32_t nameRva = rdU32(md + 16);
                std::string nm;
                if ((uint64_t)nameRva + 4 <= n) {
                    const uint32_t bytes = rdU32(d + nameRva);
                    for (uint32_t c = 0; c + 1 < bytes && nameRva + 4 + c + 1 <= n; c += 2) {
                        const uint16_t wc = (uint16_t)(d[nameRva + 4 + c] |
                                                       (d[nameRva + 4 + c + 1] << 8));
                        nm += (wc >= 0x20 && wc < 0x7F) ? (char)wc : '?';
                    }
                }
                // remember the main game module
                std::string lower = nm;
                for (char& c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower.find("diabloimmortal.exe") != std::string::npos &&
                    lower.find('\\') == std::string::npos) {
                    // basename compare: take substring after last backslash first
                }
                const size_t slash = lower.find_last_of("\\/");
                const std::string bn = slash == std::string::npos ? lower
                                                                  : lower.substr(slash + 1);
                if (bn == "diabloimmortal.exe") {
                    moduleBase = base;
                    moduleSize = sz;
                    moduleName = nm;
                }
            }
        } else if (type == 16) {  // MemoryInfoListStream
            const uint32_t hdrSize = rdU32(d + rva);
            const uint32_t entSize = rdU32(d + rva + 4);
            const uint64_t nEnt = rdU64(d + rva + 8);
            for (uint64_t k = 0; k < nEnt; ++k) {
                const uint8_t* me = d + rva + hdrSize + k * entSize;
                if (me + 48 > d + n) break;
                const uint64_t base = rdU64(me);
                const uint64_t rsize = rdU64(me + 16);
                const uint32_t prot = rdU32(me + 36);  // Protect
                const bool ex = (prot & 0xF0) != 0;    // PAGE_EXECUTE* family
                if (ex) execRegions.push_back({base, rsize});
            }
        }
    }

    std::sort(mem.ranges.begin(), mem.ranges.end(),
              [](const MemRange& a, const MemRange& b) { return a.va < b.va; });
    // mark exec ranges
    std::sort(execRegions.begin(), execRegions.end());
    auto isExecVa = [&](uint64_t va) {
        size_t lo = 0, hi = execRegions.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (va < execRegions[mid].first) hi = mid;
            else lo = mid + 1;
        }
        if (lo == 0) return false;
        const auto& r = execRegions[lo - 1];
        return va >= r.first && va < r.first + r.second;
    };
    for (MemRange& r : mem.ranges) r.exec = isExecVa(r.va);

    uint64_t execBytes = 0;
    for (const MemRange& r : mem.ranges)
        if (r.exec) execBytes += r.size;
    std::printf("ranges: %zu (%.1f MB executable)\n", mem.ranges.size(),
                execBytes / 1e6);
    if (moduleBase)
        std::printf("module: %s base=0x%llx size=0x%llx\n", moduleName.c_str(),
                    (unsigned long long)moduleBase, (unsigned long long)moduleSize);
    else
        std::printf("module: DiabloImmortal.exe not in module list (RVAs unavailable)\n");

    // ---- 2. find the format strings in memory --------------------------------
    struct Anchor {
        std::string name;
        std::vector<uint64_t> vas;
    };
    std::vector<Anchor> anchors = {{"CHAR::ANIM", {}},
                                   {"CHANNEL_DATA", {}},
                                   {"MOTION_DATA", {}},
                                   {"TRACKS_DATA", {}}};
    for (const MemRange& r : mem.ranges) {
        const uint8_t* p = d + r.foff;
        for (Anchor& a : anchors) {
            const size_t L = a.name.size();
            if (r.size < L) continue;
            for (uint64_t off = 0; off + L <= r.size; ++off)
                if (std::memcmp(p + off, a.name.data(), L) == 0)
                    a.vas.push_back(r.va + off);
        }
    }
    std::unordered_set<uint64_t> anchorVaSet;
    for (const Anchor& a : anchors) {
        std::printf("  string %-14s %zu occurrence(s)\n", a.name.c_str(), a.vas.size());
        for (uint64_t v : a.vas) anchorVaSet.insert(v);
    }

    fs::create_directories(fs::u8path(outDir));
    std::ofstream rep(fs::u8path(outDir + "/dump_carve_report.txt"), std::ios::binary);
    auto emit = [&](const std::string& s) { std::fputs(s.c_str(), stdout); rep << s; };
    {
        char b[512];
        std::snprintf(b, sizeof(b),
                      "di_dumpcarve report\n dump %s (%.2f GB)\n module base 0x%llx\n\n",
                      dumpPath.c_str(), fileSize / 1e9,
                      (unsigned long long)moduleBase);
        emit(b);
        for (const Anchor& a : anchors) {
            std::snprintf(b, sizeof(b), " string %-14s:", a.name.c_str());
            std::string line = b;
            for (uint64_t v : a.vas) {
                std::snprintf(b, sizeof(b), " 0x%llx", (unsigned long long)v);
                line += b;
            }
            emit(line + "\n");
        }
        emit("\n");
    }

    // ---- 3. RIP-relative x-refs from executable pages to the string VAs ------
    // Instruction forms we care about all end in a 4-byte signed disp that is
    // rip-relative to the END of the instruction. We don't know instruction
    // boundaries, so we test every offset: if (va_of_disp + 4 + disp) hits an
    // anchor VA, the bytes just before are a lea/mov/cmp into that string.
    // Windows are generous (2 KB before, 14 KB after) so a single run captures
    // whole decoder function bodies, not just the anchor instruction. 16 KB x
    // ~16 sites is still a few hundred KB to send back.
    auto carve = [&](uint64_t centerVa, const char* tag, int idx) {
        const MemRange* r = mem.find(centerVa);
        if (!r) return;
        const uint64_t pre = 2048, post = 14336;
        const uint64_t start = centerVa >= r->va + pre ? centerVa - pre : r->va;
        const uint64_t endVa = std::min(centerVa + post, r->va + r->size);
        const uint64_t foff = r->foff + (start - r->va);
        const uint64_t len = endVa - start;
        char fn[256];
        std::snprintf(fn, sizeof(fn), "%s/carve_%03d_%s_va%llx.bin", outDir.c_str(),
                      idx, tag, (unsigned long long)start);
        std::ofstream o(fs::u8path(fn), std::ios::binary);
        o.write(reinterpret_cast<const char*>(d + foff), (std::streamsize)len);
        char b[256];
        const uint64_t rva = (moduleBase && start >= moduleBase) ? start - moduleBase : 0;
        std::snprintf(b, sizeof(b),
                      "  carve %03d  %-12s VA 0x%llx  RVA 0x%llx  %llu bytes\n",
                      idx, tag, (unsigned long long)start, (unsigned long long)rva,
                      (unsigned long long)len);
        emit(b);
    };

    emit("[xrefs] executable code referencing the section strings\n");
    int idx = 0;
    std::unordered_set<uint64_t> carvedNear;  // dedupe windows
    for (const MemRange& r : mem.ranges) {
        if (!r.exec || r.size < 8) continue;
        const uint8_t* p = d + r.foff;
        for (uint64_t off = 0; off + 8 <= r.size; ++off) {
            const int32_t disp = rdI32(p + off);
            const uint64_t target = r.va + off + 4 + (int64_t)disp;
            if (!anchorVaSet.count(target)) continue;
            // the instruction start is a few bytes before `off` (opcode+modrm)
            const uint64_t instrVa = r.va + off;
            uint64_t bucket = instrVa & ~(uint64_t)0xFF;
            if (carvedNear.count(bucket)) continue;
            carvedNear.insert(bucket);
            // name the tag by which anchor it hit
            const char* tag = "xref";
            for (const Anchor& a : anchors)
                for (uint64_t v : a.vas)
                    if (v == target) tag = a.name.c_str();
            carve(instrVa, tag, idx++);
            if (idx > 400) break;
        }
        if (idx > 400) break;
    }
    if (idx == 0)
        emit("  (none - strings may be absent from resident memory; try the\n"
             "   other dump, or ensure the game had a model previewed when dumped)\n");

    // ---- 4. the 0xAC10AC10 block marker where it appears as CODE -------------
    emit("\n[marker] 0xAC10AC10 block-dispatch immediate in executable pages\n");
    int midx = idx;
    const uint8_t mk[4] = {0x10, 0xAC, 0x10, 0xAC};
    int markerCode = 0, markerData = 0;
    std::unordered_set<uint64_t> markerNear;  // separate, so a marker next to an
                                              // x-ref still gets its own carve
    for (const MemRange& r : mem.ranges) {
        const uint8_t* p = d + r.foff;
        if (r.size < 4) continue;
        for (uint64_t off = 0; off + 4 <= r.size; ++off) {
            if (std::memcmp(p + off, mk, 4) != 0) continue;
            if (!r.exec) { ++markerData; continue; }
            ++markerCode;
            uint64_t bucket = (r.va + off) & ~(uint64_t)0xFF;
            if (markerNear.count(bucket)) continue;
            markerNear.insert(bucket);
            carve(r.va + off, "marker", midx++);
            if (midx - idx > 60) break;
        }
        if (midx - idx > 60) break;
    }
    {
        char b[256];
        std::snprintf(b, sizeof(b),
                      "  marker in code: %d (carved), in data: %d (loaded anim blobs)\n",
                      markerCode, markerData);
        emit(b);
    }

    // ---- explicit function VAs (carve the whole body from the start) --------
    if (!explicitVas.empty()) {
        emit("\n[explicit] requested function VAs\n");
        for (uint64_t va : explicitVas) {
            const MemRange* r = mem.find(va);
            if (!r) {
                char b[128];
                std::snprintf(b, sizeof(b), "  VA 0x%llx not resident\n",
                              (unsigned long long)va);
                emit(b);
                continue;
            }
            // function START: tiny pre, large post
            const uint64_t start = va >= r->va + 16 ? va - 16 : r->va;
            const uint64_t endVa = std::min(va + 16352, r->va + r->size);
            const uint64_t foff = r->foff + (start - r->va);
            char fn[256];
            std::snprintf(fn, sizeof(fn), "%s/carve_%03d_fn_va%llx.bin", outDir.c_str(),
                          midx, (unsigned long long)start);
            std::ofstream o(fs::u8path(fn), std::ios::binary);
            o.write(reinterpret_cast<const char*>(d + foff),
                    (std::streamsize)(endVa - start));
            char b[256];
            std::snprintf(b, sizeof(b), "  carve %03d  fn  VA 0x%llx  %llu bytes\n",
                          midx++, (unsigned long long)start,
                          (unsigned long long)(endVa - start));
            emit(b);
        }
    }

    // ---- large contiguous regions (for full-function decompilation) ---------
    if (!regions.empty()) {
        emit("\n[region] large contiguous carves\n");
        for (auto& rg : regions) {
            const uint64_t rva0 = rg.first, want = rg.second;
            // gather across resident ranges; a region may span several
            uint64_t got = 0;
            char fn[256];
            std::snprintf(fn, sizeof(fn), "%s/region_va%llx_%llxB.bin",
                          outDir.c_str(), (unsigned long long)rva0,
                          (unsigned long long)want);
            std::ofstream o(fs::u8path(fn), std::ios::binary);
            uint64_t va = rva0;
            while (got < want) {
                const MemRange* r = mem.find(va);
                if (!r) {
                    // pad the hole with zeros up to next range or end
                    o.put(0);
                    ++va;
                    ++got;
                    continue;
                }
                const uint64_t avail = std::min(want - got, r->va + r->size - va);
                o.write(reinterpret_cast<const char*>(d + r->foff + (va - r->va)),
                        (std::streamsize)avail);
                va += avail;
                got += avail;
            }
            char b[256];
            std::snprintf(b, sizeof(b), "  region VA 0x%llx  %llu bytes -> %s\n",
                          (unsigned long long)rva0, (unsigned long long)got, fn);
            emit(b);
        }
    }

    {
        char b[256];
        std::snprintf(b, sizeof(b),
                      "\n%d carve(s) written to %s/. Send that folder back.\n",
                      midx, outDir.c_str());
        emit(b);
    }
    return 0;
}
