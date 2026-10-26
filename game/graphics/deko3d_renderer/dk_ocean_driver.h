#pragma once
// T95 (AI-assisted): deko3d ocean drivers, CPU half. Port of the GL bucket
// renderers' DMA walks (OceanNear.cpp render_jak1/render_jak2, OceanMidAndFar.cpp
// render_jak1 + handle_ocean_far/handle_ocean_mid, OceanMid.cpp run/run_jak2):
// pure chain reading + the T93/T94 VU1 interpreters; no GL, no deko3d.
//
// The GPU half (buffer upload + draw recording, GL flush_near/flush_mid +
// the ocean-texture render-to-texture) lives in dk_ocean_flush.cpp (Switch-only).
//
// Plain C++ header (no deko3d types), same split as DirectDk.h.
#include <cstdint>

#include "game/graphics/deko3d_renderer/dk_ocean_common.h"
#include "game/graphics/deko3d_renderer/dk_ocean_texture.h"

// Global-scope forward declarations: declared inside namespace dkocean they would
// name different (empty) types, dkocean::TexturePool / dkocean::GameVersion.
enum class GameVersion : int;  // common/versions/versions.h (scoped enum, int base)
class TexturePool;             // game/graphics/texture/TexturePool.h

namespace dkocean {

// Same contract as dkdirect::LookupFn: tbp -> dktexgpu id (0 = not found).
using LookupFn = uint32_t (*)(uint32_t tbp, bool mt4hh);

// jak1 bucket ids (game/graphics/opengl_renderer/buckets.h): the mid+far bucket
// runs early (after the sky bucket 3, before tfrag 6); the near bucket runs
// after the water buckets, before depth-cue/sprite.
inline constexpr int kJak1MidFarBucket = 4;
inline constexpr int kJak1NearBucket = 63;

struct DriverStats {
  uint32_t mid_far_walks = 0;  // bucket-4 walks that ran the full sequence
  uint32_t near_walks = 0;     // bucket-63 walks that ran
  uint32_t mid_far_empty = 0;  // CALL head (renderer didn't run)
  uint32_t near_empty = 0;
  uint32_t far_packets = 0;    // DIRECT GIF chunks fed to dkdirect
  uint32_t mid_runs = 0;       // Mid::run invocations
  uint32_t mid_aborts = 0;     // mid walks aborted on an unknown packet (FIX 111)
  uint64_t frames = 0;         // capture_chain calls with a chain
};

// Once at boot (after the texture pool exists): TextureGen::init().
void init_driver();

// Render thread, chain time. data = EE memory base (chain base), start_offset =
// chain start. far_enabled: feed the far-ocean GIF chunks into dkdirect (needs
// the direct pass; the DMA is consumed either way).
void capture_chain(const void* data, uint32_t start_offset, LookupFn lookup, bool far_enabled);

const DriverStats& driver_stats();

// ---- Switch-only GPU half (dk_ocean_flush.cpp); inert declarations on host ----

struct FlushStats {
  uint32_t tex_gen_draws = 0;
  uint32_t tex_mip_draws = 0;  // FIX 112: mip-chain passes (one per texgen draw)
  uint32_t near_draws = 0, mid_draws = 0;
  // T98 debug (AI-assisted): per-bucket draw counts (near b0 tex / b1 alpha /
  // b2 envmap, mid b0 tex / b1 envmap) so the periodic log shows which of the
  // five body draws actually record.
  uint32_t near_bucket_draws[3] = {0, 0, 0};
  uint32_t mid_bucket_draws[2] = {0, 0};
  uint32_t no_tex = 0;   // envmap texture missing (draw skipped)
  uint32_t dropped = 0;  // GPU ring full
  uint32_t misses = 0;   // no sampler entry
  uint32_t ubo_fail = 0;
  uint64_t frames = 0;
};

// Create the ocean-texture render target and register it in the pool at tbp
// 8160 (once, when the pass turns on).
void init_textures(TexturePool* pool, GameVersion version);
// Render thread, before present: upload this frame's decoded data.
void build_frame(const uint8_t fog_rgba[4]);
// Inside the world recorder, GL bucket order.
void record_mid_far(int fb_w, int fb_h);
void record_near(int fb_w, int fb_h);
const FlushStats& flush_stats();

// OCEAN_VISUAL_BUG (AI-assisted): read the two ocean debug flags once at pass
// init (sdmc:/switch/jak1/gk_oceannomip.txt + gk_oceanlevels.txt, the same
// gk_*.txt convention as dk_renderer.cpp; both off by default). When the
// no-mip flag is on, every ocean draw samples level 0 only.
// no_mip_sampling() is also read by the direct far recorder
// (dk_direct_record.cpp) so the far drops mip filtering together with the
// mid/near body draws and the texgen envmap sample.
void init_debug_flags();
bool no_mip_sampling();

// ---- for the deko3d build/record half (dk_ocean_flush.cpp) ----
CommonRecorder& near_recorder();
CommonRecorder& mid_recorder();
TextureGen& texture_gen();
LookupFn lookup();

}  // namespace dkocean
