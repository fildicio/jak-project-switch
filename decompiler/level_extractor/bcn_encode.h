#pragma once

// FIX 74 (AI-assisted): offline S3TC (BC1/BC3) encoder for fr3 level textures.
//
// The Switch port's new-area slow-motion is texture-upload bound: whole RGBA
// glTexImage2D uploads (1222 textures / 2132 ms for one city level) plus runtime
// glGenerateMipmap (~1 s per level). FIX 9/33/42/48/68 paced that work and FIX 69
// (PBO async) was hardware-rejected, so the only remaining lever is the number of
// bytes: BC1 is 4 bits/pixel, BC3 is 8, versus 32 for RGBA8888 - and the mip
// chain comes from the file, so glGenerateMipmap disappears entirely.
// Full rationale: STEP7_COMPRESSED_TEXTURES_DESIGN.md.
//
// This file is part of the DECOMPILER ONLY. The game never encodes anything;
// it receives the pre-made compressed bytes inside the fr3 and hands them to
// glCompressedTexImage2D. (S3TC is used instead of BC7/BPTC for the first cut
// because nouveau's S3TC support is confirmed on our Mesa build; BPTC is not.)
//
// Sky textures are excluded by the caller (extract_level), not here:
// SkyBlendCPU reads sky pixels on the CPU (get_data_ptr), which would see
// compressed bytes.

#include <vector>

#include "common/common_types.h"

namespace bcn {

// format codes stored in tfrag3::Texture::format
constexpr u8 FMT_RGBA = 0;
constexpr u8 FMT_BC1 = 1;  // GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 8 bytes per 4x4 block
constexpr u8 FMT_BC3 = 3;  // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16 bytes per 4x4 block

struct EncodedTexture {
  u8 format = FMT_RGBA;
  std::vector<u8> data;          // every mip level, concatenated, in order
  std::vector<u32> mip_offsets;  // [i] = byte offset of mip i; size() == mip count
};

// Compress `rgba` (0xAABBGGRR words, the exact layout of tfrag3::Texture::data)
// into a full BC1/BC3 mip chain down to 1x1. Returns false for empty/invalid
// input, in which case the caller keeps the RGBA texture as-is.
bool encode_texture(const std::vector<u32>& rgba, u16 w, u16 h, EncodedTexture* out);

}  // namespace bcn
