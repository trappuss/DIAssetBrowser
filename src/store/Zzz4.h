#pragma once
// ZZZ4 container inflate — Diablo Immortal wraps most non-Texture2D assets in
//   'ZZZ4' + u32 uncompressedSize + one raw LZ4 block (+ optional trailing slack).
// resource.repository adds a 'CCCC' prefix in front of the same frame.
// Ported from the proven Python reader (diasset/ingest/mpk.py::_inflate_zzz4).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace di {

// True when the buffer starts with the ZZZ4 magic.
bool isZzz4(const uint8_t* data, size_t len);

// Inflate a ZZZ4 frame starting at data[0]. Returns empty on failure.
// Uses LZ4_decompress_safe_partial (tolerates trailing slack after the block);
// falls back to a byte-wise tolerant decoder if the library path comes up short.
std::vector<uint8_t> inflateZzz4(const uint8_t* data, size_t len);

// Tolerant raw-LZ4 block decode: decodes from src[start] until maxOut bytes are
// produced or input ends; ignores anything after the block. Never throws.
std::vector<uint8_t> lz4Tolerant(const uint8_t* src, size_t len, size_t start,
                                 size_t maxOut);

} // namespace di
