// T79 (AI-assisted): host replay test for the deko3d Sprite3 CPU half (SpriteDk.cpp).
// Uses the same jak1 frame dump as the tfrag/tie/merc replay tests. Texture lookups are faked
// (id = tbp + 1), so this checks the DMA walk, quad layout, batching and ring layout.
#include <cstdio>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/deko3d_renderer/SpriteDk.h"

#include "gtest/gtest.h"

bool dk_replay_load(std::vector<u8>* chain, u32* start_offset);  // test_dk_tfrag_replay.cpp

namespace {
std::vector<u8> g_sp_chain;
u32 g_sp_start = 0;
uint32_t g_uploads = 0;

void count_upload(uint32_t, int64_t) {
  g_uploads++;
}
uint32_t fake_lookup(uint32_t tbp) {
  return tbp + 1;
}

bool load_and_capture() {
  if (!dk_replay_load(&g_sp_chain, &g_sp_start)) {
    return false;
  }
  g_uploads = 0;
  dksprite::capture_chain(g_sp_chain.data(), g_sp_start, count_upload, fake_lookup);
  return true;
}
}  // namespace

TEST(SpriteReplay, CapturesSprites) {
  if (!load_and_capture()) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  const auto& st = dksprite::capture_stats();
  EXPECT_FALSE(st.bad) << "sprite DMA did not match the GL layout";
  EXPECT_GT(st.uploads, 0u) << "no texture-upload packets in the tex buckets";
  EXPECT_EQ(st.uploads, g_uploads);
  ASSERT_TRUE(st.ran) << "sprite renderer did not run in the dump frame";
  EXPECT_EQ(st.capped, 0u);

  const auto& v = dksprite::captured_vertices();
  const auto& idx = dksprite::captured_indices();
  const auto& batches = dksprite::captured_batches();
  EXPECT_EQ(v.size() % 4, 0u);
  EXPECT_EQ(v.size() / 4, (size_t)st.count_2d + st.count_hud);

  uint32_t quads = 0;
  for (const auto& b : batches) {
    EXPECT_GT(b.idx_count, 0u);
    EXPECT_EQ(b.idx_count % 5, 0u);
    EXPECT_LE((size_t)b.idx_first + b.idx_count, idx.size());
    EXPECT_NE(b.tex_id, 0u);
    quads += b.idx_count / 5;
    for (uint32_t i = 0; i < b.idx_count; i += 5) {
      const uint32_t v0 = idx[b.idx_first + i];
      ASSERT_LT((size_t)v0 + 3, v.size());
      EXPECT_EQ(idx[b.idx_first + i + 4], UINT32_MAX);
      EXPECT_EQ(v[v0].info[2], 0);
      EXPECT_EQ(v[v0 + 1].info[2], 1);
      EXPECT_EQ(v[v0 + 2].info[2], 3);
      EXPECT_EQ(v[v0 + 3].info[2], 2);
      const uint16_t mode = v[v0].info[3];
      EXPECT_TRUE(b.hud ? mode == 2 : (mode == 1 || mode == 3));
      for (int c = 0; c < 4; c++) {
        EXPECT_GE(v[v0].rgba[c], 0.f);
        EXPECT_LE(v[v0].rgba[c], 1.f);
      }
    }
  }
  EXPECT_EQ(quads * 4, v.size());

  const auto& p = dksprite::captured_params();
  if (!v.empty()) {
    EXPECT_NE(p.consts0[0], 0.f) << "pfog0";
    EXPECT_NE(p.consts1[1], 0.f) << "deg_to_rad";
  }
  std::printf("dk-sprite: uploads=%u direct=%u distort=%u 2d/3d=%u hud=%u batches=%zu\n",
              st.uploads, st.direct, st.distort, st.count_2d, st.count_hud, batches.size());
}

TEST(SpriteReplay, BuildRing) {
  if (!load_and_capture()) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  dksprite::build_frame();
  const auto& f = dksprite::frame_draws();
  const auto& batches = dksprite::captured_batches();
  EXPECT_EQ(f.dropped, 0u);
  EXPECT_EQ(f.no_tex, 0u);
  EXPECT_GE(f.draws.size(), batches.size());
  if (!batches.empty()) {
    EXPECT_NE(f.vert_gpu, 0u);
    EXPECT_EQ(f.vert_bytes, dksprite::captured_vertices().size() * sizeof(dksprite::Vertex));
    EXPECT_EQ(f.params_gpu & 0xff, 0u);
  }
  for (const auto& c : f.draws) {
    EXPECT_NE(c.tex_id, 0u);
    EXPECT_GT(c.idx_count, 0u);
    EXPECT_EQ(c.idx_gpu % 4, 0u);
    EXPECT_NE(c.frag_gpu, 0u);
    EXPECT_EQ(c.frag_gpu & 0xff, 0u);
  }
  std::printf("dk-sprite: sprites=%u draws=%zu\n", f.sprites, f.draws.size());
}
