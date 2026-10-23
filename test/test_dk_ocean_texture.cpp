// test_dk_ocean_texture.cpp - host tests for the dk ocean texture generator
// (T95, AI-assisted). The generator is plain CPU code (the VU1 "PC" program),
// so it runs on the host build directly; the DMA-walk half needs chain fixtures
// that only exist on the console, so these tests cover the deterministic parts
// (init geometry, layout, stats defaults).

#include "game/graphics/deko3d_renderer/dk_ocean_texture.h"

#include <gtest/gtest.h>

using dkocean::TextureGen;

TEST(OceanDkTexture, InitGeometry) {
  TextureGen tg;
  tg.init();
  // GL init_pc: 32 strips x 66 verts = 2112 positions.
  ASSERT_EQ(tg.positions().size(), 2112u);
  // 32 strips x (33 vert pairs x 2 + 1 restart) = 32 x 67 = 2144 indices.
  ASSERT_EQ(tg.indices().size(), 32u * 67u);
  // each strip ends with a restart sentinel; strip k starts at 66 * k
  for (u32 s = 0; s < 32; s++) {
    EXPECT_EQ(tg.indices()[s * 67 + 66], UINT32_MAX) << "strip " << s;
    EXPECT_EQ(tg.indices()[s * 67], 66u * s) << "strip head " << s;
  }
  // first strip positions: pairs walk x by 64 across the 0..2048 input space
  EXPECT_EQ(tg.positions()[0].x(), 0.f);
  EXPECT_EQ(tg.positions()[0].y(), 0.f);
  EXPECT_EQ(tg.positions()[1].x(), 0.f);
  EXPECT_EQ(tg.positions()[1].y(), 64.f);
}

TEST(OceanDkTexture, NoOutputBeforeRun) {
  TextureGen tg;
  tg.init();
  EXPECT_FALSE(tg.output_valid());
  EXPECT_EQ(tg.stats().frames, 0u);
  EXPECT_EQ(tg.stats().aborts, 0u);
}

TEST(OceanDkTexture, Layouts) {
  // GL OceanTexture::Vertex: s,t float2 + rgba u8x4 + pad = 16 B.
  EXPECT_EQ(sizeof(TextureGen::VertexDynamic), 16u);
  // OceanTextureConstants' 112-byte size is static_asserted in the header at
  // compile time (same assert as the GL class), so it needs no runtime test.
}
