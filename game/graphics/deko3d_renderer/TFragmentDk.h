#pragma once
// deko3d tfrag-level GPU cache (AI-assisted, T36a). The .cpp body is
// #if defined(__SWITCH__) like the other dk files; this header stays plain
// C++ (repo rule: no deko3d includes in headers).
#include <cstdint>
#include <string>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"

// The loader's LevelData (opengl_renderer/loader/common.h) pulls GL headers
// (glad, TexturePool) and must NOT be included from dk code. The dk side
// takes the pure-data tfrag3::Level instead; the seam is wired in T39.
struct LevelData;
class Loader;

namespace dktf {

// One tfrag tree on the dk side (T36a fills the GPU buffers; T36b the texture).
struct TreeCacheDk {
  uint64_t vert_gpu = 0;    // BufferPool GPU addr (0 = none)
  uint32_t vert_count = 0;
  uint64_t idx_gpu = 0;
  uint32_t idx_count = 0;
  int tod_tex_id = -1;      // dktexgpu id, -1 = none (T36b fills it)
  const std::vector<tfrag3::StripDraw>* draws = nullptr;
  const tfrag3::BVH* vis = nullptr;
  bool use_strips = false;
};

// Upload every tfrag tree's vertices + indices to the level-data BufferPool
// (dkdev::buffer_pool()). lev_data is loader_data->level.get() — the caller
// makes that one-line extraction (T39). Skips (one log line each) any tree
// whose upload fails; never crashes. Render thread only.
bool setup_for_level(const tfrag3::Level* lev_data, const char* level_name);

// Drop the cached trees (BufferPool chunks are pooled — nothing to free, FIX 33).
void discard_tree_cache();

// ---- T60 (AI-assisted): P3 first visible tfrag draw (jak1, opt-in gk_dktfrag.txt) ----
// Split in two TUs: TFragmentDk.cpp walks the DMA chain + builds this plain draw list
// (CPU only, may use GL-side headers); dk_tfrag_record.cpp turns it into deko3d commands.

// Per-camera (= per level bucket) vertex UBO inputs.
struct CamParams {
  math::Vector4f camera[4];
  math::Vector4f pc_camera[4];  // make_new_cam_mat(rot, perspective, fog.x, hvdf_off.z)
  math::Vector4f hvdf_off;
  math::Vector4f trans;
  math::Vector4f fog;
};

// One indexed draw. The state axes use the dkpipe enum ORDER from PipelineTable.h
// (depth_op: 0 Always 1 Gequal 2 Greater 3 Never; blend: 0 Off .. 6 DstAlphaAdd).
struct DrawCmd {
  uint64_t vert_gpu = 0;
  uint32_t vert_bytes = 0;
  uint64_t idx_gpu = 0;  // address of the first u32 index
  uint32_t idx_count = 0;
  uint32_t tex_id = 0;   // dktexgpu id; 0 = draw untextured (gfx_hack_no_tex)
  uint32_t tod_id = 0;   // dktexgpu id of the tree's time-of-day palette
  float alpha_min = 0.f;
  float aref_second = 0.f;  // AFAIL second draw: alpha_max
  bool afail_second = false;
  bool depth_test = false;
  uint8_t depth_op = 0;
  bool depth_write = false;
  uint8_t blend = 0;
  bool clamp_s = false, clamp_t = false, filt = false;
  bool decal = false;
  bool strips = false;
  uint16_t cam = 0;  // index into FrameDraws::cams (T76: tie wind adds one per instance)
};

struct FrameDraws {
  std::vector<CamParams> cams;
  std::vector<DrawCmd> draws;
  uint8_t fog_rgba[4] = {0, 0, 0, 0};
  uint32_t tris = 0;
  uint32_t dropped = 0;  // draws skipped (index ring full / draw cap)
  uint32_t alpha_draws = 0;  // T62: draws from the trans/dirt/ice passes
  uint32_t wind_draws = 0;   // T76: tie wind instance draws (included in draws)
};

// Render thread, BEFORE the chain is acknowledged (EE memory is only stable until then).
// Copies the jak1 TFRAG_LEVEL0/1 camera + level name and the fog colour. No-op for
// other games. Never touches the GPU.
void capture_chain(const void* data, uint32_t start_offset);

// Render thread, after Loader::update and BEFORE present_frame (time-of-day palette
// uploads use the shared cmd buffer). loader is the dk-path Loader (opaque here).
void build_frame(Loader* loader);

// The list build_frame() produced; record_world() reads it inside present_frame.
const FrameDraws& frame_draws();

// T70 (AI-assisted): fill_state for other passes (ShrubDk): GS draw mode -> DrawCmd state bits.
void fill_draw_state(DrawMode m, DrawCmd* c);

// ---- R1 (AI-assisted): replay test hooks ----
// Non-null: build_frame appends a text "golden" list of what it built (no GPU addresses).
// test/test_dk_tfrag_replay.cpp uses it to compare a host replay with a Switch dump.
void set_golden(std::string* out);

// Switch: after `frames` frames with tfrag data, write <dir>/dk_replay.dma (that frame's
// DMA chain) and <dir>/dk_replay.golden.txt (that frame's golden). One shot.
void arm_replay_dump(const char* dir, int frames);

// deko3d side (dk_tfrag_record.cpp): record frame_draws() into dkdev::cmd_handle().
// Signature matches dkdev::set_world_recorder.
void record_world(int fb_w, int fb_h);

}  // namespace dktf
