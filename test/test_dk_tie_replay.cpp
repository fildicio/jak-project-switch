// T73/T74 (AI-assisted): host replay of the deko3d tie CPU half (TieDk.cpp, compiled with
// DK_HOST_REPLAY). Same dump as DkTfragReplay. Checks against an independent re-count of
// the level data: GL Tie3 culls with cull_check_all_slow, then draws the NORMAL category,
// the visible wind instance groups and the NORMAL_ENVMAP base draws (T76).
//
//   cmake --build build-host --target goalc-test -j8
//   ./build-host/goalc-test --gtest_filter='TieReplay.*'

#include <algorithm>
#include <cmath>
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
  size_t want_draws = 0, want_wind = 0, want_cams = 0;
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
      for (auto cat : {tfrag3::TieCategory::NORMAL, tfrag3::TieCategory::NORMAL_ENVMAP}) {
        const u32 first = tree.category_draw_indices[(int)cat];
        const u32 last = tree.category_draw_indices[(int)cat + 1];
        for (u32 i = first; i < last; i++) {
          if (groups[i].second > 0) {
            want_draws++;
          }
        }
      }
      std::vector<bool> inst_seen(tree.wind_instance_info.size(), false);
      for (const auto& draw : tree.instanced_wind_draws) {
        for (const auto& grp : draw.instance_groups) {
          if (grp.num > 0 && vis[grp.vis_idx]) {
            want_wind++;
            if (!inst_seen[grp.instance_idx]) {
              inst_seen[grp.instance_idx] = true;
              want_cams++;
            }
          }
        }
      }
    }
  }
  ASSERT_GT(want_draws, 0u) << "no visible tie draws: wrong dump?";
  EXPECT_EQ(f.cams.size(), (size_t)passes + want_cams);
  EXPECT_EQ(f.dropped, 0u);
  EXPECT_EQ(f.wind_draws, want_wind);
  EXPECT_EQ(f.draws.size(), want_draws + want_wind);
  EXPECT_EQ(f.tris, want_tris);
  for (const auto& c : f.draws) {
    EXPECT_NE(c.vert_gpu, 0u);
    EXPECT_NE(c.idx_gpu, 0u);
    EXPECT_NE(c.tod_id, 0u);
    EXPECT_GT(c.idx_count, 0u);
    EXPECT_LT(c.cam, f.cams.size());
  }
  const bool ok = f.draws.size() == want_draws + want_wind && f.wind_draws == want_wind &&
                  f.tris == want_tris && f.cams.size() == passes + want_cams && f.dropped == 0;
  std::printf("dk-tie: %zu draws (%u wind), %u tris, %zu cams -> %s\n", f.draws.size(),
              f.wind_draws, f.tris, f.cams.size(), ok ? "PASS" : "FAIL");
}

// T76: the wind camera must draw a local vertex where the plain camera draws its world point.
TEST(TieReplay, WindCamMatchesWorldTransform) {
  auto tfrag3_vert = [](const dktf::CamParams& c, const math::Vector4f& p) {
    math::Vector4f v(p.x() - c.trans.x(), p.y() - c.trans.y(), p.z() - c.trans.z(), 0.f);
    math::Vector4f t = c.pc_camera[3] * -1.f;
    t.w() = 0.f;
    for (int i = 0; i < 3; i++) {
      t -= c.pc_camera[i] * v[i];
    }
    return t;
  };
  dktf::CamParams base;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      base.pc_camera[i][j] = 0.01f * (float)((i * 7 + j * 3) % 11) - 0.04f + (i == j ? 1.f : 0.f);
      base.camera[i][j] = 0.f;
    }
  }
  base.trans = math::Vector4f(41000.f, -2300.f, 125000.f, 1.f);
  std::array<math::Vector4f, 4> m;
  m[0] = math::Vector4f(0.9f, 0.1f, -0.2f, 0.f);  // sheared like after do_wind_math
  m[1] = math::Vector4f(0.05f, 1.1f, 0.15f, 0.f);
  m[2] = math::Vector4f(0.2f, -0.1f, 0.95f, 0.f);
  m[3] = math::Vector4f(40000.f, -2000.f, 120000.f, 1.f);
  dktf::CamParams wc;
  ASSERT_TRUE(dktie::wind_cam(base, m, &wc));
  float worst = 0.f;
  for (int n = 0; n < 8; n++) {
    math::Vector4f local((float)(n * 37 % 13) * 300.f - 1800.f, (float)(n * 5 % 7) * 250.f,
                         (float)(n * 11 % 9) * -200.f + 600.f, 1.f);
    math::Vector4f world = m[3];
    for (int i = 0; i < 3; i++) {
      world += m[i] * local[i];
    }
    const math::Vector4f a = tfrag3_vert(base, world);
    const math::Vector4f b = tfrag3_vert(wc, local);
    for (int i = 0; i < 4; i++) {
      worst = std::max(worst, std::abs(a[i] - b[i]) / std::max(1.f, std::abs(a[i])));
    }
  }
  EXPECT_LT(worst, 1e-3f);
  std::printf("dk-tie: wind cam worst rel err %g -> %s\n", worst, worst < 1e-3f ? "PASS" : "FAIL");
}
