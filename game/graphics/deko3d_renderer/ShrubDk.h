#pragma once
// deko3d shrub CPU half (AI-assisted, T70/T71). Same shape as TFragmentDk.cpp: walk the jak1
// shrub buckets of a frame's DMA chain, then build a plain draw list (dktf::FrameDraws) that
// a recorder turns into deko3d commands. Plain C++ header: no deko3d includes (repo rule).
#include <cstdint>

#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/opengl_renderer/buckets.h"  // jak2::BucketId (§7.2 phase 1 tables)

struct TfragPcPortData;
class Loader;
enum class GameVersion : int;  // common/versions/versions.h (§7.2 phase 1 set_game pick)

namespace dkshrub {

// jak1 shrub buckets (buckets.h SHRUB_NORMAL_LEVEL0 = 20, SHRUB_NORMAL_LEVEL1 = 26), GL order.
inline constexpr int kJak1Buckets[2] = {20, 26};
inline constexpr int kNumPasses = 2;
// §7.2 phase 1: jak2 shrub buckets (shrub-l{n}-shrub, GL GET_BUCKET_ID_FOR_LIST stride).
constexpr int jak2_lvl(jak2::BucketId a, jak2::BucketId b, int i) {
  return (int)a + ((int)b - (int)a) * i;
}
inline constexpr int kJak2Buckets[6] = {
    jak2_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 0),
    jak2_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 1),
    jak2_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 2),
    jak2_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 3),
    jak2_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 4),
    jak2_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 5),
};
inline constexpr int kJak2NumPasses = 6;
inline constexpr int kMaxPasses = kJak2NumPasses;  // capture arrays sized for either game

// T70: read both shrub buckets (same reads as GL Shrub::render, jak1: no proto-vis mask).
void capture_chain(const void* data, uint32_t start_offset);
// T70: test access. pass = index into kJak1Buckets. nullptr when the bucket had no shrub data.
const TfragPcPortData* captured_pc(int pass);

// T71: build this frame's draw list from the captured buckets (Loader: Switch only, may be
// nullptr on the host replay).
void build_frame(Loader* loader);
const dktf::FrameDraws& frame_draws();

// T72: record frame_draws() with the SHRUB shaders (dk_tfrag_record.cpp, Switch only).
void record_world(int fb_w, int fb_h);

// ---- §7.2 phase 1 (AI-assisted): per-game bucket tables ----
// Default = jak1; dk_renderer calls set_game at boot. On jak2 the bucket also carries a
// proto-vis packet after the pc data (GL Shrub::render); phase 1 skips it = GL with an
// all-true mask (applying it rides the phase-2/3 tie-category/proto-vis work).
void set_game(GameVersion v);
// Capture passes of the active game (jak1: 2, jak2: 6).
int pass_count();
// Switch only: record one pass's draws at its GL bucket position (jak2 split).
void record_pass(int range, int fb_w, int fb_h);

}  // namespace dkshrub
