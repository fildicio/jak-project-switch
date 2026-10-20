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

namespace dksprite {

// jak1 buckets.h: TextureUploadHandler buckets (tfrag/shrub/alpha/pris/water l0+l1, common).
inline constexpr int kJak1TexBuckets[11] = {5, 12, 19, 25, 31, 38, 48, 51, 57, 60, 65};
inline constexpr int kJak1SpriteBucket = 66;

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
void capture_chain(const void* data, uint32_t start_offset, UploadFn upload, LookupFn lookup);
const CaptureStats& capture_stats();
const std::vector<Vertex>& captured_vertices();
const std::vector<uint32_t>& captured_indices();
const std::vector<Batch>& captured_batches();
const SpriteVertParams& captured_params();

// Render thread, before present: copies the capture into a GPU ring slot.
void build_frame();
const FrameDraws& frame_draws();

// Switch only: records frame_draws() into the frame command buffer (dk_sprite_record.cpp).
void record_world(int fb_w, int fb_h);

}  // namespace dksprite
