#pragma once
// deko3d tie CPU half (AI-assisted, T73/T74). Same shape as ShrubDk: walk the jak1 tie
// buckets of a frame's DMA chain, then build a dktf::FrameDraws list for the recorder.
// NORMAL + wind + NORMAL_ENVMAP base draws (T76; no envmap shine yet). Plain C++ header.
#include <array>
#include <cstdint>

#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/opengl_renderer/buckets.h"  // jak2::BucketId (§7.2 phase 1 tables)

struct TfragPcPortData;
class Loader;
enum class GameVersion : int;  // common/versions/versions.h (§7.2 phase 1 set_game pick)

namespace dktie {

// jak1 tie buckets (buckets.h TIE_LEVEL0 = 9, TIE_LEVEL1 = 16), GL order.
inline constexpr int kJak1Buckets[2] = {9, 16};
inline constexpr int kNumPasses = 2;
// §7.2 phase 1: jak2 tie buckets (tie-l{n}-tfrag, GL GET_BUCKET_ID_FOR_LIST stride).
constexpr int jak2_lvl(jak2::BucketId a, jak2::BucketId b, int i) {
  return (int)a + ((int)b - (int)a) * i;
}
inline constexpr int kJak2Buckets[6] = {
    jak2_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 0),
    jak2_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 1),
    jak2_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 2),
    jak2_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 3),
    jak2_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 4),
    jak2_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 5),
};
inline constexpr int kJak2NumPasses = 6;
// jak2 draws the envmap categories from the separate ETIE_L{n}_TFRAG bucket (GL
// Tie3AnotherCategory reuses this pass's camera), so each pass has TWO record ranges:
// 2*pass = NORMAL + wind (at TIE_L{n}), 2*pass + 1 = envmap base + shine (at ETIE_L{n}).
inline constexpr int kRangesPerPass = 2;
inline constexpr int kMaxPasses = kJak2NumPasses;  // capture arrays sized for either game

// T73: read both tie buckets (same reads as GL Tie3::set_up_common_data_from_dma, jak1).
void capture_chain(const void* data, uint32_t start_offset);
// T73: test access. nullptr when the bucket had no tie data.
const TfragPcPortData* captured_pc(int pass);
// T73: envmap color of that pass (raw 4 floats from the DMA, before GL's /128*2 scaling).
const float* captured_envmap(int pass);

// T74: build this frame's draw list (frustum-culled like GL, NORMAL category only).
void build_frame(Loader* loader);
const dktf::FrameDraws& frame_draws();

// T76: tie wind. pc_camera/trans for one wind instance so the tfrag3 shader draws its local
// vertices at their swayed world position (m = instance matrix after the wind math).
bool wind_cam(const dktf::CamParams& base,
              const std::array<math::Vector4f, 4>& m,
              dktf::CamParams* out);

// T74: record frame_draws() with the TFRAG3 shaders (dk_tfrag_record.cpp, Switch only).
void record_world(int fb_w, int fb_h);

// ---- §7.2 phase 1 (AI-assisted): per-game bucket tables ----
// Default = jak1 (safe before set_game); dk_renderer calls set_game at boot. On jak2 the
// bucket holds a proto-vis packet instead of the WindWork block and the envmap tint is
// /128 without the jak1 x2 (GL Tie3::set_up_common_data_from_dma).
void set_game(GameVersion v);
// Capture passes of the active game (jak1: 2, jak2: 6).
int pass_count();
// Record ranges of the active game: jak1 = kNumPasses, jak2 = 2 per pass (see above).
int range_count();
// Switch only: record one range's draws at its GL bucket position (jak2 split).
void record_pass(int range, int fb_w, int fb_h);

}  // namespace dktie
