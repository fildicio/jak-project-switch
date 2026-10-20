// T73/T74 (AI-assisted): host replay of the deko3d tie CPU half (TieDk.cpp, compiled with
// DK_HOST_REPLAY). Same dump as DkTfragReplay. Checks against an independent re-count of
// the level data: GL Tie3 culls with cull_check_all_slow, then draws the NORMAL category.
//
//   cmake --build build-host --target goalc-test -j8
//   ./build-host/goalc-test --gtest_filter='TieReplay.*'

#include <cstring>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/deko3d_renderer/TieDk.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "gtest/gtest.h"

bool dk_replay_load(std::vector<u8>* chain, u32* start_offset);  // test_dk_tfrag_replay.cpp
const LevelData* dktf_host_level(const char* name);                // test_dk_tfrag_replay.cpp

namespace {
std::vector<u8> g_tie_chain;
u32 g_tie_start = 0;
}  // namespace

TEST(TieReplay, CapturesBothBuckets) {
  if (!dk_replay_load(&g_tie_chain, &g_tie_start)) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  dktie::capture_chain(g_tie_chain.data(), g_tie_start);
  const TfragPcPortData* pc0 = dktie::captured_pc(0);
  ASSERT_NE(pc0, nullptr) << "bucket 9 (TIE_LEVEL0) had no tie data";
  EXPECT_GT(std::strlen(pc0->level_name), 0u);
  EXPECT_EQ(dktie::captured_pc(2), nullptr);
  EXPECT_EQ(dktie::captured_envmap(2), nullptr);
  ASSERT_NE(dktie::captured_envmap(0), nullptr);

  // Tie level 0 uses the same camera as tfrag pass 0 (same level, same frame).
  dktf::set_golden(nullptr);
  dktf::capture_chain(g_tie_chain.data(), g_tie_start);
  dktf::build_frame(nullptr);
  const auto& tf = dktf::frame_draws();
  ASSERT_FALSE(tf.cams.empty());
  EXPECT_EQ(std::memcmp(tf.cams[0].camera, pc0->camera.camera, sizeof(tf.cams[0].camera)), 0);
  const float* env = dktie::captured_envmap(0);
  std::printf("dk-tie: bucket9 %s, bucket16 %s, envmap %g %g %g %g\n", pc0->level_name,
              dktie::captured_pc(1) ? dktie::captured_pc(1)->level_name : "-", env[0], env[1],
              env[2], env[3]);
}

TEST(TieReplay, BuildMatchesLevelData) {
  if (!dk_replay_load(&g_tie_chain, &g_tie_start)) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  dktie::capture_chain(g_tie_chain.data(), g_tie_start);
  for (int i = 0; i < 4; i++) {  // 4 builds: every ring slot grows once
    dktie::build_frame(nullptr);
  }
  const auto& f = dktie::frame_draws();

  // Oracle: same culling helpers, counted here without TieDk.cpp.
  size_t want_draws = 0;
  u32 want_tris = 0;
  int passes = 0;
  std::vector<u8> vis;
  std::vector<std::pair<int, int>> groups;
  std::vector<u32> idx;
  for (int b = 0; b < dktie::kNumPasses; b++) {
    const TfragPcPortData* pc = dktie::captured_pc(b);
    if (!pc) {
      continue;
    }
    const LevelData* ld = dktf_host_level(pc->level_name);
    ASSERT_NE(ld, nullptr) << pc->level_name;
    passes++;
    for (const auto& tree : ld->level->tie_trees[0]) {
      vis.assign(tree.bvh.vis_nodes.size(), 0);
      cull_check_all_slow(pc->camera.planes, tree.bvh.vis_nodes, nullptr, vis.data());
      groups.assign(tree.static_draws.size(), {0, 0});
      idx.resize(tree.unpacked.indices.size());
      u32 tris = 0;
      make_index_list_from_vis_string(groups.data(), idx.data(), tree.static_draws, vis,
                                      tree.unpacked.indices.data(), &tris);
      want_tris += tris;
      const u32 first = tree.category_draw_indices[(int)tfrag3::TieCategory::NORMAL];
      const u32 last = tree.category_draw_indices[(int)tfrag3::TieCategory::NORMAL + 1];
      for (u32 i = first; i < last; i++) {
        if (groups[i].second > 0) {
          want_draws++;
        }
      }
    }
  }
  ASSERT_GT(want_draws, 0u) << "no visible tie draws: wrong dump?";
  EXPECT_EQ(f.cams.size(), (size_t)passes);
  EXPECT_EQ(f.dropped, 0u);
  EXPECT_EQ(f.draws.size(), want_draws);
  EXPECT_EQ(f.tris, want_tris);
  for (const auto& c : f.draws) {
    EXPECT_NE(c.vert_gpu, 0u);
    EXPECT_NE(c.idx_gpu, 0u);
    EXPECT_NE(c.tod_id, 0u);
    EXPECT_GT(c.idx_count, 0u);
    EXPECT_LT(c.cam, f.cams.size());
  }
  std::printf("dk-tie: %zu draws, %u tris, %zu cams -> %s\n", f.draws.size(), f.tris,
              f.cams.size(), f.draws.size() == want_draws && f.tris == want_tris ? "PASS" : "FAIL");
}
