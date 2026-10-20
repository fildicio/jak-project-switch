// T82 (AI-assisted): host replay test for the deko3d eye renderer CPU half (EyeDk.cpp).
// Same jak1 frame dump as the other replay tests; texture lookups are faked (id = tbp + 1).
#include <cstdio>
#include <set>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/deko3d_renderer/EyeDk.h"

#include "gtest/gtest.h"

bool dk_replay_load(std::vector<u8>* chain, u32* start_offset);  // test_dk_tfrag_replay.cpp

namespace {
uint32_t fake_lookup(uint32_t tbp) {
  return tbp + 1;
}
}  // namespace

TEST(EyeReplay, CapturesEyes) {
  std::vector<u8> chain;
  u32 start = 0;
  if (!dk_replay_load(&chain, &start)) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  dkeye::capture_chain(chain.data(), start, fake_lookup);
  const auto& st = dkeye::capture_stats();
  const auto& eyes = dkeye::captured_eyes();
  printf("eyes=%u bad=%u no_tex=%u\n", st.eyes, st.bad, st.no_tex);
  EXPECT_EQ(st.bad, 0u) << "eye DMA did not match the GL layout";
  EXPECT_EQ(st.no_tex, 0u);
  EXPECT_EQ(eyes.size() % 2, 0u) << "eyes come in l/r pairs";
  std::set<int> slots;
  for (const auto& e : eyes) {
    EXPECT_LT(e.slot, dkeye::kNumEyes);
    EXPECT_TRUE(slots.insert(e.slot).second) << "slot " << (int)e.slot << " twice";
    EXPECT_NE(e.iris, 0u);
    for (int q = 0; q < dkeye::kQuadsPerEye; q++) {
      for (const auto& v : e.v[q]) {
        // the target spans 512..1024 (eye.vert: (x - 768) / 256); lids slide partly out of it
        // when the eye opens (the viewport clips), but an unsigned wrap would give ~4e9.
        EXPECT_GE(v.x, 0.f);
        EXPECT_LE(v.x, 2048.f);
        EXPECT_GE(v.y, 0.f);
        EXPECT_LE(v.y, 2048.f);
      }
    }
  }
}

TEST(EyeReplay, EmptyChainIsSafe) {
  dkeye::capture_chain(nullptr, 0, fake_lookup);
  EXPECT_TRUE(dkeye::captured_eyes().empty());
  EXPECT_EQ(dkeye::eye_tex_id(0), 0u);
  dkeye::set_eye_tex_id(41 % dkeye::kNumEyes, 7);
  EXPECT_EQ(dkeye::eye_tex_id(41), 7u) << "GL lookup_eye_texture: eye_id % 40";
  dkeye::set_eye_tex_id(1, 0);
}
