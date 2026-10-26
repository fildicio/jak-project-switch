#pragma once
// T88 (AI-assisted): deko3d sky CPU blender (jak1). Port of GL SkyBlendCPU for the dk path:
// the jak1 sky texture is blended on the CPU from level textures each frame (two output
// buffers, 32x32 sky + 64x64 clouds), uploaded through dktexgpu's deferred staging slot and
// registered in the TexturePool at SKY_TEXTURE_VRAM_ADDRS so the sky-draw bucket
// (dkdirect::capture_sky_chain, bucket 3) resolves it like any other texture.
// The blend math is the exact GL scalar/NEON code (see SkyBlendCPU.cpp) so the output is
// bit-identical. Plain C++ header (no deko3d types).
#include <cstdint>

namespace dksky {

// One source texture for a blend step: RGBA8 CPU bytes (size 4*w*h), data = null if the
// level texture is missing / a placeholder (the step is skipped and counted).
struct SkySource {
  const uint8_t* data = nullptr;
  uint32_t bytes = 0;
  bool placeholder = false;  // FIX 117: pool entry exists but is still a placeholder
};

// tbp = raw GsTex0::tbp0() (TexturePool VRAM page index). Render thread, chain time.
using SourceFn = SkySource (*)(uint32_t tbp);
// Upload/update one blended buffer (index 0 = sky 32x32, 1 = clouds 64x64). Render thread,
// build time (after dktexgpu::begin_deferred_frame()).
using SinkFn = void (*)(int index, const uint8_t* rgba, uint32_t size);

struct BlendStats {
  uint32_t buckets = 0;  // walks that reached the sky copy pairs
  uint32_t pairs = 0;    // qwc-6 setup+draw pairs parsed
  uint32_t sky_draws = 0, sky_blends = 0;
  uint32_t cloud_draws = 0, cloud_blends = 0;
  uint32_t bad_adgif = 0, bad_alpha = 0, bad_draw = 0, bad_coord = 0, bad_size = 0;
  uint32_t no_src = 0;     // source texture missing / placeholder (skipped)
  // FIX 117: the boot-time title screen blended 30/30 pairs with no source (HW
  // 2026-10-24), which leaves the sky texture black behind the logo. Split the
  // counter by cause and remember the first distinct missing tbps.
  uint32_t no_src_absent = 0, no_src_placeholder = 0;
  uint32_t no_src_tbp[4] = {};
  uint32_t no_src_tbp_count[4] = {};
  uint64_t frames = 0;
  // FIX 112 diag: the most recent bad_size pair (which source texture has the wrong size).
  uint32_t last_bad_tbp = 0, last_bad_bytes = 0;
  int last_bad_idx = -1;
};

// Render thread, chain time: walk the two jak1 SkyBlendHandler buckets (32 = level 0,
// 39 = level 1) and blend. Buckets that didn't run (CALL head) or hold only trans tfrag
// (first tag qwc != 8) are skipped -- TFragmentDk walks those for the tfrag geometry.
// data = EE memory base (chain base), start_offset = chain start.
void capture_blend_chain(const void* data, uint32_t start_offset, SourceFn src);

// FIX 112: one level only (0 = bucket 32, 1 = bucket 39). The source textures live in VRAM
// only between their upload bucket and the next upload that reuses the address, so the
// renderer calls this from dksprite's bucket hook, at the GL bucket position.
// capture_blend_chain() = both levels back to back (tests, end-of-frame VRAM).
void capture_blend_level(const void* data, uint32_t start_offset, SourceFn src, int level);
// The blend bucket of a level (32 / 39), for the hook's ordering test.
inline constexpr int kBlendBucket[2] = {32, 39};

// Render thread, build time: push the buffers blended this frame to the GPU.
void flush_blend(SinkFn sink);

const BlendStats& blend_stats();

// Tests: direct access to the blended buffers (empty before the first capture).
const uint8_t* blend_buffer(int index, uint32_t* size);

// Declared here so the tests can call them (same contract as the GL SkyBlendCPU ones):
// initial: out[i] = saturate_u8((in[i] * intensity) >> 7)
// blend:   out[i] = saturating_add_u8(out[i], saturate_u8((in[i] * intensity) >> 7))
void blend_sky_initial_fast(uint8_t intensity, uint8_t* out, const uint8_t* in, uint32_t size);
void blend_sky_fast(uint8_t intensity, uint8_t* out, const uint8_t* in, uint32_t size);

}  // namespace dksky