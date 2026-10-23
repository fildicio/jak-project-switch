// T80 (AI-assisted): host replay test for the deko3d DirectRenderer CPU half (DirectDk.cpp).
// Same jak1 frame dump as the other replay tests. Texture lookups are faked (id = tbp + 1), so
// this checks the VIF/GIF walk, vertex assembly, batching and ring layout of buckets 67-69.
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/deko3d_renderer/DirectDk.h"
#include "game/graphics/deko3d_renderer/SkyDk.h"

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

// ---------------------------------------------------------------------------
// T88 (AI-assisted): dksky sky CPU blend tests. blend_sky_*_fast are the GL
// SkyBlendCPU scalar/NEON math (bit-identical); SyntheticChain builds the exact DMA
// layout GL SkyBlendHandler::render walks (bucket head NEXT -> qwc-8 set-display ->
// qwc-6 adgif setup + qwc-6 draw pairs) in buckets 32 (sky, coord 0x200) and
// 39 (clouds, coord 0x400); SkyCaptureOnReplay checks the bucket-3 sky-draw capture
// on the real frame dump and its list-reset contract with capture_chain().
// ---------------------------------------------------------------------------
TEST(SkyBlend, Math) {
  uint8_t in[16], out[16];
  memset(in, 0x80, sizeof(in));
  dksky::blend_sky_initial_fast(100, out, in, sizeof(in));
  EXPECT_EQ(out[0], 100);  // (128 * 100) >> 7
  EXPECT_EQ(out[15], 100);
  memset(in, 0xff, sizeof(in));
  dksky::blend_sky_initial_fast(200, out, in, sizeof(in));
  EXPECT_EQ(out[0], 255);  // (255 * 200) >> 7 = 398 saturates
  memset(in, 0x80, sizeof(in));
  dksky::blend_sky_initial_fast(100, out, in, sizeof(in));  // 100
  dksky::blend_sky_fast(100, out, in, sizeof(in));          // +100 = 200
  EXPECT_EQ(out[0], 200);
  dksky::blend_sky_fast(128, out, in, sizeof(in));  // +128 -> saturates
  EXPECT_EQ(out[0], 255);
}

namespace {
std::vector<uint8_t> g_sky_src32(4 * 32 * 32, 0x80);
std::vector<uint8_t> g_sky_src64(4 * 64 * 64, 0xc0);
std::vector<int> g_flush_idx;
std::vector<uint32_t> g_flush_val, g_flush_size;

dksky::SkySource fake_sky_source(uint32_t tbp) {
  dksky::SkySource s;
  if (tbp == 0x200) {
    s.data = g_sky_src32.data();
    s.bytes = (uint32_t)g_sky_src32.size();
  } else if (tbp == 0x400) {
    s.data = g_sky_src64.data();
    s.bytes = (uint32_t)g_sky_src64.size();
  }
  return s;  // anything else: not loaded -> no_src
}
}  // namespace

TEST(SkyBlend, SyntheticChain) {
  constexpr uint64_t kCnt = 1, kNext = 2, kCall = 5;
  const uint32_t slot32 = 16 + 16 * 32, slot39 = 16 + 16 * 39;
  std::vector<u8> m(0x8000, 0);
  put64(&m, 0, dma_tag(kCall, 0, 0x7000));  // frame chain head (CALL -> default regs)
  // one adgif setup (qwc 6) + draw (qwc 6) pair; AdGifData = data/addr u64 pairs.
  auto pair = [&](size_t* o, uint32_t tbp, bool blend, uint32_t intensity, uint32_t coord) {
    put64(&m, *o, dma_tag(kCnt, 6, 0));
    size_t p = *o + 16;
    memset(m.data() + p, 0, 96);
    put64(&m, p + 16 + 0, tbp);                // tex0_data (tbp0)
    put64(&m, p + 16 + 8, 6);                  // TEX0_1
    put64(&m, p + 16 + 16, 0);                 // tex1_data
    put64(&m, p + 16 + 24, 0x14);              // TEX1_1
    put64(&m, p + 16 + 32, 0);                 // mip_data
    put64(&m, p + 16 + 40, 0x34);              // MIPTBP1_1
    put64(&m, p + 16 + 48, 0);                 // clamp_data
    put64(&m, p + 16 + 56, 8);                 // CLAMP_1
    put64(&m, p + 16 + 64, 0x8000000068ull);   // alpha_data: Cs + Cd
    put64(&m, p + 16 + 72, 0x42);              // ALPHA_1
    *o += 16 + 96;
    put64(&m, *o, dma_tag(kCnt, 6, 0));
    p = *o + 16;
    memset(m.data() + p, 0, 96);
    put64(&m, p, gif_tag(1, true, true, blend ? (1u << 6) : 0, 1));  // prim, abe bit
    memcpy(m.data() + p + 16, &intensity, 4);
    memcpy(m.data() + p + 80, &coord, 4);
    *o += 16 + 96;
  };
  auto bucket = [&](uint32_t slot, size_t base, uint32_t tbp, uint32_t coord) {
    put64(&m, slot, dma_tag(kNext, 0, (uint32_t)base));  // head, size 0
    size_t o = base;
    put64(&m, o, dma_tag(kCnt, 8, 0));  // set-display-gs-state
    o += 16 + 8 * 16;
    pair(&o, tbp, false, 100, coord);  // first draw (abe 0) = initial blend
    pair(&o, tbp, true, 128, coord);   // blend draw (abe 1)
    put64(&m, o, dma_tag(kNext, 0, 0));  // trailer, qwc != 6 ends the walk
  };
  bucket(slot32, 0x1000, 0x200, 0x200);  // sky
  bucket(slot39, 0x2000, 0x400, 0x400);  // clouds

  dksky::capture_blend_chain(m.data(), 0, fake_sky_source);
  const auto& st = dksky::blend_stats();
  EXPECT_EQ(st.pairs, 4u);
  EXPECT_EQ(st.buckets, 2u);
  EXPECT_EQ(st.sky_draws, 1u);
  EXPECT_EQ(st.sky_blends, 1u);
  EXPECT_EQ(st.cloud_draws, 1u);
  EXPECT_EQ(st.cloud_blends, 1u);
  EXPECT_EQ(st.bad_adgif + st.bad_alpha + st.bad_draw + st.bad_coord + st.bad_size, 0u);
  EXPECT_EQ(st.no_src, 0u);
  uint32_t size = 0;
  const uint8_t* sky = dksky::blend_buffer(0, &size);
  ASSERT_NE(sky, nullptr);
  EXPECT_EQ(size, 4u * 32 * 32);
  // initial (0x80 * 100) >> 7 = 100, then blend (0x80 * 128) >> 7 = 128 -> 228
  EXPECT_EQ(sky[0], 228);
  EXPECT_EQ(sky[size - 1], 228);
  const uint8_t* cloud = dksky::blend_buffer(1, &size);
  ASSERT_NE(cloud, nullptr);
  EXPECT_EQ(size, 4u * 64 * 64);
  // initial (0xC0 * 100) >> 7 = 150, then blend (0xC0 * 128) >> 7 = 192 -> saturates
  EXPECT_EQ(cloud[0], 255);

  g_flush_idx.clear();
  g_flush_val.clear();
  g_flush_size.clear();
  dksky::flush_blend([](int idx, const uint8_t* rgba, uint32_t bytes) {
    g_flush_idx.push_back(idx);
    g_flush_val.push_back(rgba[0]);
    g_flush_size.push_back(bytes);
  });
  ASSERT_EQ(g_flush_idx.size(), 2u);  // sky then clouds, dirty flags cleared
  EXPECT_EQ(g_flush_idx[0], 0);
  EXPECT_EQ(g_flush_idx[1], 1);
  EXPECT_EQ(g_flush_val[0], 228u);
  EXPECT_EQ(g_flush_val[1], 255u);
  EXPECT_EQ(g_flush_size[0], 4u * 32 * 32);
  EXPECT_EQ(g_flush_size[1], 4u * 64 * 64);
  dksky::flush_blend([](int idx, const uint8_t*, uint32_t) { g_flush_idx.push_back(idx); });
  EXPECT_EQ(g_flush_idx.size(), 2u);
}

TEST(SkyBlend, SkyCaptureOnReplay) {
  if (g_dr_chain.empty() && !dk_replay_load(&g_dr_chain, &g_dr_start)) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  // Real frame order: sky first (owns the list reset), then the direct buckets append.
  dkdirect::capture_sky_chain(g_dr_chain.data(), g_dr_start, fake_lookup);
  EXPECT_EQ(dkdirect::capture_stats().bad, 0u);
  dkdirect::capture_chain(g_dr_chain.data(), g_dr_start, fake_lookup);
  EXPECT_EQ(dkdirect::capture_stats().bad, 0u);
  size_t sky_batches = 0;
  uint32_t next = 0;
  for (const auto& b : dkdirect::captured_batches()) {
    EXPECT_EQ(b.first, next) << "batches must tile the vertex list";
    if (b.bucket == dkdirect::kJak1SkyBucket) {
      sky_batches++;
    } else {
      EXPECT_TRUE(b.bucket >= 67 && b.bucket <= 69);
    }
    next = b.first + b.count;
  }
  EXPECT_EQ(next, dkdirect::captured_vertices().size());
  dkdirect::build_frame((const uint8_t[4]){0, 0, 0, 0});
  const auto& f = dkdirect::frame_draws();
  const auto& sf = dkdirect::sky_frame_draws();
  EXPECT_EQ(f.dropped, 0u);
  EXPECT_EQ(sf.draws.size(), sky_batches);  // bucket 3 routed to the sky half
  std::printf("dk-sky: sky batches=%zu sky draws=%zu\n", sky_batches, sf.draws.size());
}
