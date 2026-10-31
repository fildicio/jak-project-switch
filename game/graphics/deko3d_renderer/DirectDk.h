#pragma once
// deko3d DirectRenderer CPU half (AI-assisted, T80). Port of GL opengl_renderer/DirectRenderer for
// the jak1 late buckets: debug 67 (menus, HUD text), debug-no-zbuf 68, subtitle 69.
// capture_chain() interprets the GIF packets (A+D registers, tri/strip/fan/sprite/line prims) into
// a vertex list split into batches exactly where GL flush_pending() would draw. Textures are
// resolved at capture time (VRAM is reused later in the frame), so it must run AFTER
// dksprite::capture_chain(), which replays this frame's texture uploads.
// build_frame() copies the capture into a GPU ring slot for the recorder.
// Plain C++ header (no deko3d types).
#include <cstdint>
#include <vector>

#include "game/graphics/opengl_renderer/buckets.h"  // jak2::BucketId (§7.2 phase 1 tables)

enum class GameVersion : int;  // common/versions/versions.h (§7.2 phase 1 set_game pick)

namespace dkdirect {

inline constexpr int kJak1Buckets[3] = {67, 68, 69};
// T88: jak1 sky-draw bucket (GL SkyRenderer -- a DirectRenderer walk over bucket 3).
inline constexpr int kJak1SkyBucket = 3;
// §7.2 phase 1 (AI-assisted): jak2 late DirectRenderer buckets (GL registers debug2,
// debug-no-zbuf2, debug3; screen-filter and sky-draw are DirectRenderers too but stay
// dkgl-owned per DKGL_TODO §7.1). This native walk is NOT wired on jak2 -- jak2 chains
// have no CALL head (no default-regs chain for the GIF defaults) -- the table exists so
// the ids are enum-driven the day it gets ported.
inline constexpr int kJak2Buckets[3] = {
    (int)jak2::BucketId::DEBUG2,
    (int)jak2::BucketId::DEBUG_NO_ZBUF2,
    (int)jak2::BucketId::DEBUG3,
};
inline constexpr int kJak2SkyBucket = (int)jak2::BucketId::SKY_DRAW;

// §7.2 phase 1: per-game pick. Default = jak1 (safe before set_game). Calling it with
// Jak2 only switches the table pointers; capture_chain still expects a jak1 chain.
void set_game(GameVersion v);
// Buckets of the active game (jak1: 3, jak2: 3).
int bucket_count();
// T95 (AI-assisted): jak1 ocean-mid-far bucket (GL OceanMidAndFar, bucket 4).
// The far-ocean half is DIRECT GIF data fed by the ocean driver walk
// (dk_ocean_driver.cpp) through render_ocean_far_gif below.
inline constexpr int kJak1OceanFarBucket = 4;

// GL DirectRenderer::Vertex (64 B), same layout so the shader is a straight port.
struct Vertex {
  float xyzf[4];
  float stq[3];
  uint8_t rgba[4];
  uint8_t tex_unit, tcc, decal, fog_enable, use_uv;
  uint8_t pad[11];
  float scissor[4];
};
static_assert(sizeof(Vertex) == 64, "DirectRenderer::Vertex");

// std140 mirror of direct.frag.glsl DirectFrag (fragment binding 7, one per draw).
struct FragParams {
  float alpha[4];  // alpha_min, alpha_max, greater (0/1), ta0
  float mult[4];   // color_mult, alpha_mult, textured (0/1), scissor_enable (0/1)
  float fix[4];    // x = constant alpha for (Cs-Cd)*FIX+Cd, < 0 = off
};
// std140 mirror of DirectConsts (fragment binding 6, one per frame, filled at record time
// because only the recorder knows the framebuffer size).
struct FrameConsts {
  float game_sizes[4];  // 512, 448, fb_w, fb_h
  float fog_color[4];   // rgb, intensity (0..1)
};
inline constexpr uint32_t kFragParamBytes = 0x100;

// One GL glDrawArrays (or the two of a double draw) from flush_pending().
struct Batch {
  uint32_t first = 0;  // into captured vertices
  uint32_t count = 0;
  int bucket = 0;
  bool textured = false;
  uint32_t tex_id = 0;  // dktexgpu id, 0 = not resolved
  bool filt = false, clamp_s = true, clamp_t = true;
  bool depth_test = true;
  uint8_t depth_op = 1;  // dkpipe::Compare
  bool depth_write = false;
  uint8_t blend = 0;     // dkpipe::Blend
  float fix_alpha = -1.f;
  float color_mult = 1.f;
  float alpha_min = 0.f, alpha_max = 10.f;
  bool greater = false;
  bool double_draw = false;
  float double_aref = 0.f;
  float ta0 = 0.f;
  bool scissor = false;
};

struct DrawCmd {
  uint32_t first = 0, count = 0;
  uint32_t tex_id = 0;  // 0 = untextured (no texture bound)
  uint64_t frag_gpu = 0;
  bool depth_test = true;
  uint8_t depth_op = 1;
  bool depth_write = false;
  uint8_t blend = 0;
  bool clamp_s = true, clamp_t = true, filt = false;
};

struct FrameDraws {
  std::vector<DrawCmd> draws;
  uint64_t vert_gpu = 0;
  uint32_t vert_bytes = 0;
  uint8_t* consts_cpu = nullptr;  // FrameConsts, written by record_world
  uint64_t consts_gpu = 0;
  uint8_t fog_rgba[4] = {0, 0, 0, 0};
  uint32_t verts = 0;
  uint32_t no_tex = 0;   // textured batches whose VRAM address had no texture (skipped)
  uint32_t dropped = 0;  // GPU ring full
};

struct CaptureStats {
  uint32_t packets = 0;      // GIF tags read
  uint32_t batches = 0;
  uint32_t bad = 0;          // packets GL would assert on (skipped)
  uint32_t odd_blend = 0;    // blend combos GL logs as unsupported (drawn as StdAlpha)
  uint32_t capped = 0;       // vertices over the per-frame cap
};

using LookupFn = uint32_t (*)(uint32_t tbp, bool mt4hh);

// Render thread, before the chain is acked. data = EE memory base (chain base).
void capture_chain(const void* data, uint32_t start_offset, LookupFn lookup);
// T88: GL SkyRenderer::render (jak1 sky-draw, bucket 3) on the shared capture state. Must
// run BEFORE capture_chain() in the same frame (it owns the list reset; the sky batches
// keep bucket kJak1SkyBucket so build_frame() routes them to sky_frame_draws()).
void capture_sky_chain(const void* data, uint32_t start_offset, LookupFn lookup);
const CaptureStats& capture_stats();
const std::vector<Vertex>& captured_vertices();
const std::vector<Batch>& captured_batches();

// Render thread, before present. fog_rgba = dktf::frame_draws().fog_rgba (same frame).
void build_frame(const uint8_t fog_rgba[4]);
const FrameDraws& frame_draws();
// T88: the sky-draw half of the same build (bucket 3 batches). Recorded before tfrag.
const FrameDraws& sky_frame_draws();

// Switch only: records frame_draws() into the frame command buffer (dk_direct_record.cpp).
void record_world(int fb_w, int fb_h);
// T88: records sky_frame_draws() (GL bucket order: sky 3 before tfrag 6).
void record_sky_world(int fb_w, int fb_h);

// T95 (AI-assisted): GL OceanMidAndFar::handle_ocean_far support. The ocean
// driver (dk_ocean_driver.cpp) brackets its far-walk with ocean_far_begin/end
// and feeds every DIRECT GIF chunk through render_ocean_far_gif. Batches are
// stamped with kJak1OceanFarBucket so build_frame routes them to
// ocean_far_frame_draws() (same mechanism as the sky walk, T88).
void ocean_far_begin();
void render_ocean_far_gif(const uint8_t* data, uint32_t size);
void ocean_far_end();
const FrameDraws& ocean_far_frame_draws();
// Switch only: records ocean_far_frame_draws() (bucket 4: after sky 3, before tfrag 6).
void record_ocean_far_world(int fb_w, int fb_h);

}  // namespace dkdirect
