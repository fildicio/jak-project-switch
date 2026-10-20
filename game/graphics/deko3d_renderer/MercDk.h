#pragma once
// deko3d merc2 CPU half (AI-assisted, T78). Port of GL Merc2 (foreground/Merc2.cpp) for jak1:
// capture_chain() copies every PC_PORT merc model of the frame (name, lights, bones, flags) out of
// the DMA chain before the EE reuses it; build_frame() resolves the models in the Loader, writes
// each model's params + bones into a GPU ring slot and lists the draws for the recorder.
// Not ported yet: emerc envmap shine, eye textures, blerc/mod vertices (drawn in rest pose),
// anim-slot textures. Plain C++ header (no deko3d types).
#include <cstdint>
#include <vector>

class Loader;

namespace dkmerc {

// jak1 merc buckets (buckets.h): tfrag-tex l0/l1, after-alpha, pris l0/l1, after-pris, water l0/l1.
inline constexpr int kJak1Buckets[8] = {10, 17, 45, 49, 52, 55, 58, 61};
inline constexpr int kNumBuckets = 8;

// std140 mirror of merc2.vert.glsl MercVert (vertex binding 7).
struct MercVertParams {
  float light_dir0_fade[4];     // @0
  float light_dir1_fade_en[4];  // @16
  float light_dir2[4];          // @32
  float light_col0[4];          // @48
  float light_col1[4];          // @64
  float light_col2[4];          // @80
  float light_ambient[4];       // @96
  float hvdf_offset[4];         // @112
  float fog_constants[4];       // @128
  float perspective[16];        // @144, column-major like glUniformMatrix4fv
};                              // 208 B
static_assert(sizeof(MercVertParams) == 208, "std140 MercVert");

// std140 mirror of merc2.frag.glsl MercFrag (fragment binding 7).
struct MercFragParams {
  float fog_color[4];   // @0
  int32_t ignore_alpha;  // @16
  int32_t decal_enable;  // @20
  int32_t no_tex;        // @24
  float fade;            // @28  light_dir0_fade.w
  float fade_en;         // @32  light_dir1_fade_en.w (0 on jak1: GL forces merc fade off)
};
static_assert(sizeof(MercFragParams) == 36, "std140 MercFrag");

inline constexpr uint32_t kBoneBytes = 128;     // ShaderMercMat: tmat[4] + nmat[3] + pad
inline constexpr uint32_t kMaxBones = 128;      // shader array size / GL MAX_SKEL_BONES
inline constexpr uint32_t kParamBytes = 0x100;  // MercVertParams, padded to the UBO alignment
inline constexpr uint32_t kFragVariants = 16;   // ignore_alpha | decal << 1 | fog << 2 | no_tex << 3

struct DrawCmd {
  uint64_t vert_gpu = 0;
  uint32_t vert_bytes = 0;
  uint64_t idx_gpu = 0;     // first u32 index
  uint32_t idx_count = 0;
  uint32_t tex_id = 0;      // dktexgpu id
  uint64_t params_gpu = 0;  // MercVertParams (vertex binding 7)
  uint64_t bones_gpu = 0;   // MercBones (vertex binding 8)
  uint32_t bones_bytes = 0;
  uint8_t frag = 0;         // index into FrameDraws::frag_gpu
  bool depth_test = false;
  uint8_t depth_op = 0;
  bool depth_write = false;
  uint8_t blend = 0;
  bool clamp_s = false, clamp_t = false, filt = false;
  bool strips = true;
};

struct FrameDraws {
  std::vector<DrawCmd> draws;
  uint64_t frag_gpu[kFragVariants] = {};
  uint32_t models = 0;   // captured models
  uint32_t missing = 0;  // name not in a loaded level (yet)
  uint32_t skipped = 0;  // draws not ported yet (eyes, anim slot, envmap, bad texture)
  uint32_t dropped = 0;  // GPU ring full
  uint32_t mod = 0;      // models with blerc/mod effects (drawn in rest pose)
  uint32_t bad_bones = 0;  // bone addresses outside EE memory (zeroed)
  uint32_t tris = 0;
};

// Read the merc buckets of a frame chain (render thread, before the chain is acked).
// data = EE memory base; mem_size bounds the bone reads (EE RAM size on the Switch).
void capture_chain(const void* data, uint32_t start_offset, uint32_t mem_size);
// Test access.
uint32_t captured_models();
const char* captured_name(uint32_t i);
int captured_bucket(uint32_t i);  // index into kJak1Buckets

// fog_rgba = dktf::frame_draws().fog_rgba (same frame). Render thread, before present.
void build_frame(Loader* loader, const uint8_t fog_rgba[4]);
const FrameDraws& frame_draws();

// Switch only: records frame_draws() into the frame command buffer (dk_merc_record.cpp).
void record_world(int fb_w, int fb_h);

}  // namespace dkmerc
