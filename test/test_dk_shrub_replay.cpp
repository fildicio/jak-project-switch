// T70/T71 (AI-assisted): host replay of the deko3d shrub CPU half (ShrubDk.cpp, compiled with
// DK_HOST_REPLAY). Same dump as DkTfragReplay; no Switch golden yet, so this checks invariants
// against the level data (GL Shrub draws every ShrubDraw: no culling in jak1).
//
//   cmake --build build-host --target goalc-test -j8
//   ./build-host/goalc-test --gtest_filter='ShrubReplay.*'

#include <cstring>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/deko3d_renderer/ShrubDk.h"
#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "gtest/gtest.h"

bool dk_replay_load(std::vector<u8>* chain, u32* start_offset);  // test_dk_tfrag_replay.cpp
const LevelData* dktf_host_level(const char* name);                // test_dk_tfrag_replay.cpp

namespace {
std::vector<u8> g_chain;
u32 g_start = 0;
}  // namespace

TEST(ShrubReplay, CapturesBothBuckets) {
  if (!dk_replay_load(&g_chain, &g_start)) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  dkshrub::capture_chain(g_chain.data(), g_start);
  const TfragPcPortData* pc0 = dkshrub::captured_pc(0);
  ASSERT_NE(pc0, nullptr) << "bucket 20 (SHRUB_NORMAL_LEVEL0) had no shrub data";
  EXPECT_GT(std::strlen(pc0->level_name), 0u);
  EXPECT_EQ(dkshrub::captured_pc(2), nullptr);  // out of range

  // Shrub level 0 is drawn with the same camera as tfrag pass 0 (same level, same frame).
  dktf::set_golden(nullptr);
  dktf::capture_chain(g_chain.data(), g_start);
  dktf::build_frame(nullptr);
  const auto& tf = dktf::frame_draws();
  ASSERT_FALSE(tf.cams.empty());
  EXPECT_EQ(std::memcmp(tf.cams[0].camera, pc0->camera.camera, sizeof(tf.cams[0].camera)), 0);
  std::printf("dk-shrub: bucket20 %s, bucket26 %s\n", pc0->level_name,
              dkshrub::captured_pc(1) ? dkshrub::captured_pc(1)->level_name : "-");
}

TEST(ShrubReplay, BuildMatchesLevelData) {
  if (!dk_replay_load(&g_chain, &g_start)) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  dkshrub::capture_chain(g_chain.data(), g_start);
  for (int i = 0; i < 4; i++) {  // 4 builds: every ring slot grows once, like the tfrag test
    dkshrub::build_frame(nullptr);
  }
  const auto& f = dkshrub::frame_draws();

  size_t want_draws = 0;
  u32 want_tris = 0;
  int passes = 0;
  for (int b = 0; b < dkshrub::kNumPasses; b++) {
    const TfragPcPortData* pc = dkshrub::captured_pc(b);
    if (!pc) {
      continue;
    }
    const LevelData* ld = dktf_host_level(pc->level_name);
    ASSERT_NE(ld, nullptr) << pc->level_name;
    passes++;
    for (const auto& tree : ld->level->shrub_trees) {
      for (const auto& d : tree.static_draws) {
        if (d.num_indices > 0) {
          want_draws++;
          want_tris += d.num_triangles;
        }
      }
    }
  }
  ASSERT_GT(want_draws, 0u) << "level has no shrubs: wrong dump?";
  EXPECT_EQ(f.cams.size(), (size_t)passes);
  EXPECT_EQ(f.dropped, 0u);
  EXPECT_EQ(f.draws.size(), want_draws);
  EXPECT_EQ(f.tris, want_tris);
  for (const auto& c : f.draws) {
    EXPECT_TRUE(c.strips);
    EXPECT_NE(c.vert_gpu, 0u);
    EXPECT_NE(c.idx_gpu, 0u);
    EXPECT_NE(c.tod_id, 0u);
    EXPECT_GT(c.idx_count, 0u);
    EXPECT_LT(c.cam, f.cams.size());
  }
  std::printf("dk-shrub: %zu draws, %u tris, %zu cams -> %s\n", f.draws.size(), f.tris,
              f.cams.size(), f.draws.size() == want_draws ? "PASS" : "FAIL");
}
