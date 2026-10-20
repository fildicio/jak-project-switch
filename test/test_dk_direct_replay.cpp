// T80 (AI-assisted): host replay test for the deko3d DirectRenderer CPU half (DirectDk.cpp).
// Same jak1 frame dump as the other replay tests. Texture lookups are faked (id = tbp + 1), so
// this checks the VIF/GIF walk, vertex assembly, batching and ring layout of buckets 67-69.
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/deko3d_renderer/DirectDk.h"

#include "gtest/gtest.h"

bool dk_replay_load(std::vector<u8>* chain, u32* start_offset);  // test_dk_tfrag_replay.cpp

namespace {
std::vector<u8> g_dr_chain;
u32 g_dr_start = 0;

uint32_t fake_lookup(uint32_t tbp, bool) {
  return tbp + 1;
}

bool load_and_capture() {
  if (!dk_replay_load(&g_dr_chain, &g_dr_start)) {
    return false;
  }
  dkdirect::capture_chain(g_dr_chain.data(), g_dr_start, fake_lookup);
  return true;
}
}  // namespace

TEST(DirectReplay, CapturesBuckets) {
  if (!load_and_capture()) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  const auto& st = dkdirect::capture_stats();
  EXPECT_EQ(st.bad, 0u) << "direct DMA did not match the GL layout";
  EXPECT_EQ(st.capped, 0u);
  const auto& v = dkdirect::captured_vertices();
  const auto& batches = dkdirect::captured_batches();
  EXPECT_EQ(v.size() % 3, 0u) << "triangle list";
  uint32_t next = 0;
  for (const auto& b : batches) {
    EXPECT_EQ(b.first, next) << "batches must tile the vertex list";
    EXPECT_GT(b.count, 0u);
    EXPECT_EQ(b.count % 3, 0u);
    EXPECT_TRUE(b.bucket >= 67 && b.bucket <= 69);
    if (b.textured) {
      EXPECT_NE(b.tex_id, 0u);
    }
    next = b.first + b.count;
  }
  EXPECT_EQ(next, v.size());
  for (const auto& x : v) {
    ASSERT_GE(x.xyzf[0], 0.f);
    ASSERT_LE(x.xyzf[0], 1.f);
    ASSERT_GE(x.xyzf[2], 0.f);
    ASSERT_LE(x.xyzf[2], 1.f);
  }
  std::printf("dk-direct: packets=%u batches=%zu verts=%zu odd_blend=%u\n", st.packets,
              batches.size(), v.size(), st.odd_blend);
}

TEST(DirectReplay, BuildRing) {
  if (!load_and_capture()) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  const uint8_t fog[4] = {1, 2, 3, 4};
  dkdirect::build_frame(fog);
  const auto& f = dkdirect::frame_draws();
  const auto& batches = dkdirect::captured_batches();
  EXPECT_EQ(f.dropped, 0u);
  EXPECT_EQ(f.no_tex, 0u);
  EXPECT_GE(f.draws.size(), batches.size());
  if (!batches.empty()) {
    EXPECT_NE(f.vert_gpu, 0u);
    EXPECT_NE(f.consts_cpu, nullptr);
    EXPECT_EQ(f.consts_gpu & 0xff, 0u);
    EXPECT_EQ(f.vert_bytes, dkdirect::captured_vertices().size() * sizeof(dkdirect::Vertex));
  }
  for (const auto& c : f.draws) {
    EXPECT_NE(c.frag_gpu, 0u);
    EXPECT_EQ(c.frag_gpu & 0xff, 0u);
    EXPECT_LE((size_t)c.first + c.count, dkdirect::captured_vertices().size());
  }
  std::printf("dk-direct: verts=%u draws=%zu\n", f.verts, f.draws.size());
}

namespace {
void put64(std::vector<u8>* b, size_t off, uint64_t v) {
  memcpy(b->data() + off, &v, 8);
}
uint64_t dma_tag(uint64_t kind, uint32_t qwc, uint32_t addr) {
  return qwc | (kind << 28) | ((uint64_t)addr << 32);
}
uint64_t gif_tag(uint32_t nloop, bool eop, bool pre, uint32_t prim, uint32_t nreg) {
  return nloop | ((uint64_t)eop << 15) | ((uint64_t)pre << 46) | ((uint64_t)prim << 47) |
         ((uint64_t)nreg << 60);
}
}  // namespace

// One menu-glyph-like packet in bucket 67: A+D state (tex0, alpha SDSD, alpha test GEQUAL,
// ztest ALWAYS), then a textured, blended, UV-addressed SPRITE.
TEST(DirectReplay, SyntheticSprite) {
  constexpr uint64_t kCnt = 1, kNext = 2, kCall = 5, kRet = 6;
  const uint32_t slot67 = 16 + 16 * 67, slot68 = slot67 + 16, slot69 = slot68 + 16;
  const uint32_t pkt = 0x1000, defregs = 0x2000;
  std::vector<u8> m(0x3000, 0);
  put64(&m, 0, dma_tag(kCall, 0, defregs));
  put64(&m, defregs, dma_tag(kRet, 0, 0));
  put64(&m, slot67, dma_tag(kNext, 0, pkt));
  put64(&m, slot68, dma_tag(kNext, 0, slot69));
  put64(&m, slot69, dma_tag(kNext, 0, slot69 + 16));
  const uint32_t qwc = 1 + 4 + 1 + 2 * 3;
  put64(&m, pkt, dma_tag(kCnt, qwc, 0));
  put64(&m, pkt + 8, (uint64_t)(0x50000000u | qwc) << 32);  // vif0 NOP, vif1 DIRECT qwc
  size_t o = pkt + 16;
  put64(&m, o, gif_tag(4, false, false, 0, 1));
  put64(&m, o + 8, 0xe);  // AD
  o += 16;
  put64(&m, o, 0x100 | (1ull << 34));  // TEX0: tbp 0x100, tcc
  put64(&m, o + 8, 0x06);
  put64(&m, o + 16, 0x44);  // ALPHA: (Cs-Cd)*As+Cd
  put64(&m, o + 24, 0x42);
  put64(&m, o + 32, 1 | (5 << 1) | (0x26 << 4) | (1 << 17));  // TEST: ate GEQUAL 0x26, z ALWAYS
  put64(&m, o + 40, 0x47);
  put64(&m, o + 48, 0x7000);  // TEX1 (mmag off)
  put64(&m, o + 56, 0x14);
  o += 64;
  const uint32_t prim = 6 | (1 << 4) | (1 << 6) | (1 << 8);  // SPRITE tme abe fst
  put64(&m, o, gif_tag(2, true, true, prim, 3));
  put64(&m, o + 8, 0x1 | (0x3 << 4) | (0x4 << 8));  // RGBAQ UV XYZF2
  o += 16;
  for (int k = 0; k < 2; k++) {
    const uint32_t c = 0x80, a = 0x40, x = (2048 - 256 + 100 * k) << 4, y = (2048 - 224 + 50 * k) << 4;
    const uint32_t rgba[4] = {c, c, c, a};
    memcpy(m.data() + o, rgba, 16);
    const uint32_t uv[2] = {(uint32_t)(16 * 8 * k), (uint32_t)(16 * 8 * k)};
    memcpy(m.data() + o + 16, uv, 8);
    memcpy(m.data() + o + 32, &x, 4);
    memcpy(m.data() + o + 36, &y, 4);
    put64(&m, o + 40, (uint64_t)0xffffff << 4);
    o += 48;
  }
  dkdirect::capture_chain(m.data(), 0, fake_lookup);
  const auto& st = dkdirect::capture_stats();
  EXPECT_EQ(st.bad, 0u);
  EXPECT_EQ(st.packets, 2u);
  const auto& v = dkdirect::captured_vertices();
  const auto& b = dkdirect::captured_batches();
  ASSERT_EQ(v.size(), 6u);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_EQ(b[0].bucket, 67);
  EXPECT_TRUE(b[0].textured);
  EXPECT_EQ(b[0].tex_id, 0x101u);
  EXPECT_EQ(b[0].blend, 1);
  EXPECT_EQ(b[0].depth_op, 0);
  EXPECT_FLOAT_EQ(b[0].alpha_min, 0x26 / 128.f);
  EXPECT_FALSE(b[0].filt);
  EXPECT_EQ(v[0].use_uv, 1);
  EXPECT_EQ(v[0].tcc, 1);
  EXPECT_EQ(v[0].rgba[3], 0x40);
  EXPECT_FLOAT_EQ(v[2].stq[0], 128.f);  // c2 = second corner
  EXPECT_NEAR(v[2].xyzf[0] - v[0].xyzf[0], (100.f * 16 * 65536) / UINT32_MAX, 1e-6);
  EXPECT_NEAR(v[0].xyzf[2], 1.f, 1e-6);

  dkdirect::build_frame((const uint8_t[4]){0, 0, 0, 0});
  EXPECT_EQ(dkdirect::frame_draws().draws.size(), 1u);
}
