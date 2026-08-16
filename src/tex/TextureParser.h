#pragma once
// Diablo Immortal Texture2D container parser (framework-free: std only).
//
// Layout per the CucFlavius/Zee DiabloImmortal_Texture.bt template, verified
// against blobs extracted from the live PC install (2026-08-01):
//
//   Texture2DInfo (40 bytes)
//   0x00  u8 magFilter, minFilter, mipFilter    (the 02 02 02 01 "magic" is
//   0x03  u8 addressU, addressV                  this sampler config, constant
//   0x05  u8 format      (EPixelFormat)          for game textures)
//   0x06  u8 mipLevel, 0x07 u8 flags
//   0x08  u8 compressionPreset, lodGroup, mipGenPreset, textureType
//   0x0C  i16 width, i16 height
//   0x10  Color4 defaultColor (16 bytes)
//   0x20  u32 size
//   0x24  u16 unk, u16 sliceCount
//   0x28  TextureSliceInfo x sliceCount, smallest-first:
//         u32 size (record length)
//         u16 width, height, depth, pitchInByte
//         u32 sliceInByte
//         'NNNN' -> raw payload follows
//         'ZZZ4' -> u32 uncompressedSize + LZ4 block
//
// The PC build stores desktop formats (BC7 = 25 measured on character diffuse);
// the mobile ASTC range (36..49) exists in the enum but may not appear on PC —
// di_probe --tex-formats measures the actual distribution.

#include <cstdint>
#include <string>
#include <vector>

namespace di {

struct TextureMip {
    uint16_t width  = 0;
    uint16_t height = 0;
    uint16_t depth  = 0;
    uint16_t pitchInByte = 0;   // bytes per block-row as stored (0 = unknown)
    std::vector<uint8_t> data;  // decompressed payload (sliceInByte bytes)
};

struct Texture2D {
    uint8_t  format    = 0;     // EPixelFormat byte
    uint8_t  mipLevel  = 0;
    int16_t  width     = 0;     // base dimensions from the header
    int16_t  height    = 0;
    uint16_t sliceCount = 0;    // declared; mips.size() = successfully parsed
    std::vector<TextureMip> mips;   // smallest-first, as stored

    const TextureMip* largestMip() const;
};

extern const uint8_t TEXTURE2D_MAGIC[4];   // 02 02 02 01

bool isTexture2D(const uint8_t* data, size_t len);

// Parse a raw Texture2D blob (a .6/.1 blob as read from the MPK — NOT
// ZZZ4-wrapped at the container level). Returns false with *err on failure;
// individual undecodable mips are skipped, reflected in mips.size().
bool parseTexture2D(const uint8_t* data, size_t len, Texture2D* out, std::string* err);

} // namespace di
