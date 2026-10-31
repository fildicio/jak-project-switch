#pragma once
// deko3d Sprite3 CPU half (AI-assisted, T79). Port of GL sprite/Sprite3.cpp render_jak1 for the
// normal sprites: 3D + 2D world sprites (group 0) and HUD sprites (group 1).
// capture_chain() replays the jak1 texture-upload buckets (so VRAM addresses resolve to textures
// in bucket order, like TextureUploadHandler), then reads the sprite bucket and builds the quads.
// build_frame() copies them into a GPU ring slot for the recorder.
// Not ported yet (counted only): the pre-sprite DirectRenderer data, distort sprites, glow.
// Plain C++ header (no deko3d types).
#include <cstdint>
#include <vector>

#include "game/graphics/opengl_renderer/buckets.h"  // jak2::BucketId (phase 1 walk)

namespace dksprite {

// jak1 buckets.h: TextureUploadHandler buckets (tfrag/shrub/alpha/pris/water l0+l1, common).
inline constexpr int kJak1TexBuckets[11] = {5, 12, 19, 25, 31, 38, 48, 51, 57, 60, 65};
inline constexpr int kJak1SpriteBucket = 66;
// jak2 buckets.h: every TextureUploadHandler bucket of init_bucket_renderers_jak2 in GL
// dispatch order, including the no_anim ones (debug-no-zbuf1 318 / tex-all-map 319 /
// subtitle 322 -- they flush uploads too). No lcom-pris2 entries: GL leaves 248-251
// unregistered. The sprite data lives in PARTICLES (313, GL "particles" Sprite3);
// read_sprites=false walks without it (SpriteDk ports render_jak1 only -- jak2 sprites
// wait for their own phase).
inline constexpr int kJak2TexBuckets[47] = {
    (int)jak2::BucketId::TEX_LCOM_SKY_PRE,                                   // 4
    (int)jak2::BucketId::TEX_L0_TFRAG,    (int)jak2::BucketId::TEX_L1_TFRAG,
    (int)jak2::BucketId::TEX_L2_TFRAG,    (int)jak2::BucketId::TEX_L3_TFRAG,
    (int)jak2::BucketId::TEX_L4_TFRAG,    (int)jak2::BucketId::TEX_L5_TFRAG,
    (int)jak2::BucketId::TEX_L0_SHRUB,    (int)jak2::BucketId::TEX_L1_SHRUB,
    (int)jak2::BucketId::TEX_L2_SHRUB,    (int)jak2::BucketId::TEX_L3_SHRUB,
    (int)jak2::BucketId::TEX_L4_SHRUB,    (int)jak2::BucketId::TEX_L5_SHRUB,
    (int)jak2::BucketId::TEX_L0_ALPHA,    (int)jak2::BucketId::TEX_L1_ALPHA,
    (int)jak2::BucketId::TEX_L2_ALPHA,    (int)jak2::BucketId::TEX_L3_ALPHA,
    (int)jak2::BucketId::TEX_L4_ALPHA,    (int)jak2::BucketId::TEX_L5_ALPHA,
    (int)jak2::BucketId::TEX_LCOM_TFRAG,  (int)jak2::BucketId::TEX_LCOM_SHRUB,
    (int)jak2::BucketId::TEX_L0_PRIS,     (int)jak2::BucketId::TEX_L1_PRIS,
    (int)jak2::BucketId::TEX_L2_PRIS,     (int)jak2::BucketId::TEX_L3_PRIS,
    (int)jak2::BucketId::TEX_L4_PRIS,     (int)jak2::BucketId::TEX_L5_PRIS,
    (int)jak2::BucketId::TEX_LCOM_PRIS,
    (int)jak2::BucketId::TEX_L0_PRIS2,    (int)jak2::BucketId::TEX_L1_PRIS2,
    (int)jak2::BucketId::TEX_L2_PRIS2,    (int)jak2::BucketId::TEX_L3_PRIS2,
    (int)jak2::BucketId::TEX_L4_PRIS2,    (int)jak2::BucketId::TEX_L5_PRIS2,
    (int)jak2::BucketId::TEX_L0_WATER,    (int)jak2::BucketId::TEX_L1_WATER,
    (int)jak2::BucketId::TEX_L2_WATER,    (int)jak2::BucketId::TEX_L3_WATER,
    (int)jak2::BucketId::TEX_L4_WATER,    (int)jak2::BucketId::TEX_L5_WATER,
    (int)jak2::BucketId::TEX_LCOM_WATER,  (int)jak2::BucketId::TEX_LCOM_SKY_POST,
    (int)jak2::BucketId::TEX_ALL_SPRITE,                                      // 349
    (int)jak2::BucketId::TEX_ALL_WARP,    (int)jak2::BucketId::DEBUG_NO_ZBUF1,
    (int)jak2::BucketId::TEX_ALL_MAP,     (int)jak2::BucketId::SUBTITLE,
};
inline constexpr int kJak2SpriteBucket = (int)jak2::BucketId::PARTICLES;  // 350

// std140 mirror of sprite3.vert.glsl SpriteVert (vertex binding 7).
struct SpriteVertParams {
  float hvdf_offset[4];         // @0
  float camera[16];             // @16
  float hud_matrix[16];         // @80
  float hud_hvdf_offset[4];     // @144
  float hud_hvdf_user[75][4];   // @160
  float basis_x[4];             // @1360
  float basis_y[4];             // @1376
  float xy_array[8][4];         // @1392
  float xyz_array[4][4];        // @1520
  float st_array[4][4];         // @1584
  float consts0[4];             // @1648 pfog0, fog_min, fog_max, min_scale
  float consts1[4];             // @1664 max_scale, deg_to_rad, inv_area, 0
};                              // 1680 B
static_assert(sizeof(SpriteVertParams) == 1680, "std140 SpriteVert");
inline constexpr uint32_t kVertParamBytes = 0x700;  // padded to the UBO alignment
inline constexpr uint32_t kFragParamBytes = 0x100;  // SpriteFrag: vec4 (alpha_min, alpha_max)

// GL Sprite3::SpriteVertex3D (64 B).
struct Vertex {
  float xyz_sx[4];
  float quat_sy[4];
  float rgba[4];
  uint16_t flags_matrix[2];
  uint16_t info[4];  // 0, tcc, corner id, mode (1 = 2D, 2 = HUD, 3 = 3D)
  uint8_t pad[4];
};
static_assert(sizeof(Vertex) == 64, "SpriteVertex3D");

// One GL glDrawElements of flush_sprites (a texture + draw-mode bucket).
struct Batch {
  uint32_t idx_first = 0;  // into captured indices
  uint32_t idx_count = 0;
  uint32_t tex_id = 0;     // dktexgpu id, 0 = not resolved
  uint32_t mode = 0;       // DrawMode::as_int()
  bool hud = false;        // flush_sprites(double_draw = true)
};

struct DrawCmd {
  uint64_t idx_gpu = 0;
  uint32_t idx_count = 0;
  uint32_t tex_id = 0;
  uint64_t frag_gpu = 0;  // SpriteFrag
  bool depth_test = false;
  uint8_t depth_op = 0;
  bool depth_write = false;
  uint8_t blend = 0;
  bool clamp_s = false, clamp_t = false, filt = false;
};

struct FrameDraws {
  std::vector<DrawCmd> draws;
  uint64_t vert_gpu = 0;
  uint32_t vert_bytes = 0;
  uint64_t params_gpu = 0;
  uint32_t sprites = 0;   // quads built
  uint32_t no_tex = 0;    // batches whose VRAM address had no texture (skipped)
  uint32_t dropped = 0;   // GPU ring full / sprite cap
};

struct CaptureStats {
  bool ran = false;          // sprite renderer data present this frame
  bool bad = false;          // DMA did not look like GL expects (capture stopped)
  uint32_t uploads = 0;      // texture-upload packets replayed
  uint32_t direct = 0;       // pre-sprite direct transfers (not drawn yet)
  uint32_t distort = 0;      // distort sprites (not drawn yet)
  uint32_t count_2d = 0;     // group 0 sprites (2D + 3D)
  uint32_t count_hud = 0;    // group 1 sprites
  uint32_t capped = 0;       // sprites over the per-frame cap
};

// upload(page, mode): TextureUploadHandler::flush_uploads for one packet (page = EE address).
// lookup(tbp): TexturePool::lookup -> dktexgpu id, 0 when nothing (or a placeholder) is there.
using UploadFn = void (*)(uint32_t page, int64_t mode);
using LookupFn = uint32_t (*)(uint32_t tbp);

// Render thread, before the chain is acked. data = EE memory base (chain base).
// FIX 112: called with the bucket id before each texture-upload bucket is replayed (and
// with the sprite bucket after the last one), so passes that read VRAM mid-frame (sky
// blend, buckets 32/39) see the pool exactly as GL does at that bucket.
using BucketHookFn = void (*)(int bucket);
// §7.2 phase 1 (AI-assisted): the walk list / sprite bucket are parameters so jak2 can
// replay its own (longer) upload list without reading sprites (render_jak1 port only).
void capture_chain(const void* data,
                   uint32_t start_offset,
                   UploadFn upload,
                   LookupFn lookup,
                   BucketHookFn hook = nullptr,
                   const int* tex_buckets = kJak1TexBuckets,
                   int tex_bucket_count = (int)(sizeof(kJak1TexBuckets) / sizeof(int)),
                   int sprite_bucket = kJak1SpriteBucket,
                   bool read_sprites = true);
const CaptureStats& capture_stats();
const std::vector<Vertex>& captured_vertices();
const std::vector<uint32_t>& captured_indices();
const std::vector<Batch>& captured_batches();
const SpriteVertParams& captured_params();

// Render thread, before present: copies the capture into a GPU ring slot.
void build_frame();
const FrameDraws& frame_draws();

// #17 (AI-assisted): world 2D sprites fade out as their screen box grows towards
// screen_frac_end (1 = the whole screen) and are skipped beyond it. 0 = off (GL behaviour).
void set_near_fade(float screen_frac_end);

// Switch only: records frame_draws() into the frame command buffer (dk_sprite_record.cpp).
void record_world(int fb_w, int fb_h);
// #17 (AI-assisted, Switch only): early-Z shader for sprites without depth write (default on).
void set_early_z(bool on);

}  // namespace dksprite
