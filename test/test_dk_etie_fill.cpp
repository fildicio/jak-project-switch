// T68 (AI-assisted): dktie::fill_etie vs hand-computed values of GL
// init_etie_cam_uniforms (Tie3.cpp:560-584).
#include "game/graphics/deko3d_renderer/dk_tie3_prep.h"

#include "gtest/gtest.h"

TEST(EtieFill, PerspAndRot) {
  float rot[16];
  for (int i = 0; i < 16; i++) {
    rot[i] = 100.f + i;
  }
  float persp[16] = {};
  persp[0] = 5.f;   // pxx
  persp[5] = 6.f;   // pyy
  persp[10] = 7.f;  // pzz
  persp[11] = 8.f;  // pzw
  persp[14] = 9.f;  // pwz
  const float hvdf[4] = {1.f, 2.f, 3.f, 4.f};
  dktie::EtieParams p;
  dktie::fill_etie(&p, rot, persp, hvdf, 2.f, nullptr);
  // scale = pzw / fog0 = 8 / 2 = 4
  EXPECT_FLOAT_EQ(p.persp0[0], 4.f);
  EXPECT_FLOAT_EQ(p.persp0[1], 8.f);
  EXPECT_FLOAT_EQ(p.persp0[2], 19.f);  // 4 * 3 + 7
  EXPECT_FLOAT_EQ(p.persp0[3], 4.f);
  EXPECT_FLOAT_EQ(p.persp1[0], 5.f);
  EXPECT_FLOAT_EQ(p.persp1[1], 6.f);
  EXPECT_FLOAT_EQ(p.persp1[2], 9.f);
  EXPECT_FLOAT_EQ(p.persp1[3], 0.f);
  for (int i = 0; i < 16; i++) {
    EXPECT_FLOAT_EQ(p.cam_no_persp[i], 100.f + i);
  }
  for (int i = 0; i < 4; i++) {
    EXPECT_FLOAT_EQ(p.envmap_tod_tint[i], 0.f);
  }
}

TEST(EtieFill, Tint) {
  float rot[16] = {};
  float persp[16] = {};
  persp[11] = 1.f;
  const float hvdf[4] = {};
  const float tint[4] = {0.5f, 0.25f, 1.f, 2.f};
  dktie::EtieParams p;
  dktie::fill_etie(&p, rot, persp, hvdf, 1.f, tint);
  EXPECT_FLOAT_EQ(p.envmap_tod_tint[0], 0.5f);
  EXPECT_FLOAT_EQ(p.envmap_tod_tint[1], 0.25f);
  EXPECT_FLOAT_EQ(p.envmap_tod_tint[2], 1.f);
  EXPECT_FLOAT_EQ(p.envmap_tod_tint[3], 2.f);
}
