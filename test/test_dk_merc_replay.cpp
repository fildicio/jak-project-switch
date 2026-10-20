// T78 (AI-assisted): host replay test for the deko3d merc2 CPU half (MercDk.cpp).
// Uses the same jak1 frame dump as the tfrag/shrub/tie replay tests. Bone addresses are not
// relocated in the dump, so only structure (models, draws, ring layout) is checked here.
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/deko3d_renderer/MercDk.h"
#include "game/graphics/deko3d_renderer/TieDk.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "gtest/gtest.h"

bool dk_replay_load(std::vector<u8>* chain, u32* start_offset);  // test_dk_tfrag_replay.cpp
const LevelData* dktf_host_level(const char* name);                // test_dk_tfrag_replay.cpp
bool dktf_host_merc(const char* name, MercRef* out);               // test_dk_tfrag_replay.cpp

namespace {
std::vector<u8> g_merc_chain;
u32 g_merc_start = 0;

bool load_and_capture() {
  if (!dk_replay_load(&g_merc_chain, &g_merc_start)) {
    return false;
  }
  dkmerc::capture_chain(g_merc_chain.data(), g_merc_start, (uint32_t)g_merc_chain.size());
  // The levels the Loader would have resident: GAME (common) + the tie levels of the frame.
  dktf_host_level("GAME");
  dktie::capture_chain(g_merc_chain.data(), g_merc_start);
  for (int p = 0; p < dktie::kNumPasses; p++) {
    if (const TfragPcPortData* pc = dktie::captured_pc(p)) {
      dktf_host_level(pc->level_name);
    }
  }
  return true;
}
}  // namespace

TEST(MercReplay, CapturesModels) {
  if (!load_and_capture()) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  const uint32_t n = dkmerc::captured_models();
  ASSERT_GT(n, 0u) << "no PC_PORT merc models in the jak1 merc buckets";
  int last_bucket = -1;
  std::set<std::string> names;
  for (uint32_t i = 0; i < n; i++) {
    const char* name = dkmerc::captured_name(i);
    ASSERT_NE(name, nullptr);
    EXPECT_GT(std::strlen(name), 0u);
    EXPECT_LT(std::strlen(name), 127u);
    const int b = dkmerc::captured_bucket(i);
    EXPECT_GE(b, last_bucket) << "models must come out in bucket order";
    EXPECT_LT(b, dkmerc::kNumBuckets);
    last_bucket = b;
    names.insert(name);
  }
  std::string list;
  for (auto& s : names) {
    list += s + " ";
  }
  std::printf("dk-merc: %u models (%zu distinct): %s\n", n, names.size(), list.c_str());
}

TEST(MercReplay, BuildMatchesLevelData) {
  if (!load_and_capture()) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  const uint8_t fog[4] = {10, 20, 30, 128};
  dkmerc::build_frame(nullptr, fog);
  const auto& f = dkmerc::frame_draws();
  EXPECT_EQ(f.models, dkmerc::captured_models());
  EXPECT_EQ(f.dropped, 0u);
  ASSERT_GT(f.draws.size(), 0u);

  // Oracle: upper bound = every effect enabled, minus eye / anim-slot draws.
  uint32_t oracle = 0;
  uint32_t found = 0;
  for (uint32_t i = 0; i < dkmerc::captured_models(); i++) {
    MercRef ref;
    if (!dktf_host_merc(dkmerc::captured_name(i), &ref)) {
      continue;
    }
    found++;
    for (const auto& e : ref.model->effects) {
      for (const auto& d : e.all_draws) {
        if (d.eye_id == 0xff && d.tree_tex_id >= 0) {
          oracle++;
        }
      }
    }
  }
  EXPECT_EQ(found + f.missing, f.models);
  EXPECT_LE(f.draws.size(), oracle);

  for (uint32_t v = 0; v < dkmerc::kFragVariants; v++) {
    EXPECT_NE(f.frag_gpu[v], 0u);
    EXPECT_EQ(f.frag_gpu[v] & 0xff, 0u);
  }
  for (const auto& c : f.draws) {
    EXPECT_EQ(c.params_gpu & 0xff, 0u);
    EXPECT_EQ(c.bones_gpu, c.params_gpu + dkmerc::kParamBytes);
    EXPECT_GT(c.bones_bytes, 0u);
    EXPECT_LE(c.bones_bytes, dkmerc::kMaxBones * dkmerc::kBoneBytes);
    EXPECT_LT(c.frag, dkmerc::kFragVariants);
    EXPECT_NE(c.tex_id, 0u);
    EXPECT_GT(c.idx_count, 0u);
    EXPECT_EQ(c.vert_bytes % sizeof(tfrag3::MercVertex), 0u);
    EXPECT_EQ((c.idx_gpu - 0x100000000ull) % 4, 0u);
  }
  std::printf("dk-merc: models=%u found=%u draws=%zu oracle<=%u tris=%u missing=%u skipped=%u "
              "mod=%u bad_bones=%u\n",
              f.models, found, f.draws.size(), oracle, f.tris, f.missing, f.skipped, f.mod,
              f.bad_bones);
}
