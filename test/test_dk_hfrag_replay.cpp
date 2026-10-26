// FIX 117 (AI-assisted): host replay of the deko3d hfrag CPU half (HfragDk.cpp,
// compiled with DK_HOST_REPLAY). This test DOCUMENTS the T113 misport and keeps
// the CPU half working for the future jak3 port:
//
//  - GL's Hfrag renderer is registered by init_bucket_renderers_jak3 (jak3's
//    HFRAG bucket is 8). Jak1's bucket 8 is TIE_NEAR_LEVEL0, which jak1 leaves
//    empty (GL registers nothing there), so the recorded jak1 chain's bucket 8
//    has the empty shape: head NEXT -> CALL to the default-regs chain -> RET ->
//    NEXT to the next bucket.
//  - No jak1 fr3 level carries hfrag geometry (checked below), so the pass
//    could never draw anything on jak1. dk_renderer now guards it off
//    (s_dk_hfrag, jak3 only).
//
//   cmake --build build-host --target goalc-test -j8
//   ./build-host/goalc-test --gtest_filter='HfragReplay.*'

#include <cstring>
#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"

#include "game/graphics/deko3d_renderer/HfragDk.h"
#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/deko3d_renderer/TieDk.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "gtest/gtest.h"

bool dk_replay_load(std::vector<u8>* chain, u32* start_offset);  // test_dk_tfrag_replay.cpp
const LevelData* dktf_host_level(const char* name);              // test_dk_tfrag_replay.cpp

namespace {
std::vector<u8> g_chain;
u32 g_start = 0;

// The levels the user actually visits. None of these carry hfrag data on jak1;
// the loop below is the proof (and would catch an extraction change).
const char* kLevels[] = {"village1", "beach", "misty", "intro", "rolling", "swamp"};
}  // namespace

TEST(HfragReplay, Jak1Bucket8IsEmptyAndNoLevelHasHfrag) {
  if (!dk_replay_load(&g_chain, &g_start)) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }

  // 1) The real chain: bucket 8 (jak1 TIE_NEAR_LEVEL0) must walk as EMPTY --
  //    head NEXT, then the default-regs CALL trampoline, RET, NEXT to bucket 9.
  //    If this ever fails, the game started using the near-tie bucket.
  dkhfrag::capture_chain(g_chain.data(), g_start);
  const dkhfrag::Diag& d = dkhfrag::diag();
  EXPECT_EQ(d.calls, 1u);
  EXPECT_EQ(d.head_call, 1u) << "jak1 bucket 8 is no longer empty -- revisit s_dk_hfrag";
  EXPECT_EQ(d.packets, 0u);

  // 2) No visited jak1 level has hfrag geometry. If this fails, the extractor
  //    changed and the hfrag pass could become real on jak1.
  for (const char* lev : kLevels) {
    const LevelData* ld = dktf_host_level(lev);
    if (ld == nullptr || ld->level == nullptr) {
      continue;
    }
    EXPECT_EQ(ld->level->hfrag.corners.size(), 0u) << lev << " now has hfrag corners";
    EXPECT_EQ(ld->level->hfrag.vertices.size(), 0u) << lev << " now has hfrag vertices";
  }

  // 3) The synthetic path still exercises the diag wiring end to end: a packet
  //    for a level that exists but has no hfrag data resolves the level, then
  //    load_level counts bad_data and build_frame counts load_fail.
  dktie::capture_chain(g_chain.data(), g_start);
  const TfragPcPortData* base = dktie::captured_pc(0);
  ASSERT_NE(base, nullptr) << "no tie PC_PORT packet for a camera to borrow";
  TfragPcPortData pc = *base;
  memset(pc.level_name, 0, sizeof(pc.level_name));
  strncpy(pc.level_name, "village1", sizeof(pc.level_name) - 1);
  dkhfrag::capture_chain(g_chain.data(), g_start);  // resets the capture state
  dkhfrag::capture_packet(&pc);
  EXPECT_EQ(dkhfrag::diag().packets, 1u);
  dkhfrag::build_frame(nullptr);
  const dkhfrag::Diag& dd = dkhfrag::diag();
  EXPECT_EQ(dd.no_level, 0u) << "village1 must resolve (it has no hfrag data, not missing)";
  EXPECT_EQ(dd.bad_data, 1u);
  EXPECT_EQ(dd.load_fail, 1u);
  std::printf("dk-hfrag: jak1 bucket 8 empty, no hfrag data in %zu checked levels, "
              "diag wiring ok (bad_data -> load_fail)\n",
              sizeof(kLevels) / sizeof(kLevels[0]));
}
