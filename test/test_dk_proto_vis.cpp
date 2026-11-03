// (AI-assisted) dkprotovis::update vs GL TieProtoVisibility::update: zero-separated names in the
// mask hide every proto with that name, and the flags reset to visible each frame.
#include "game/graphics/deko3d_renderer/dk_proto_vis.h"

#include "gtest/gtest.h"

namespace {
std::vector<u8> mask_of(const std::string& s) {
  return std::vector<u8>(s.begin(), s.end());
}
}  // namespace

TEST(DkProtoVis, HidesNamedProtosAndResets) {
  const std::vector<std::string> names = {"lamp", "tree", "lamp", "crate"};
  dkprotovis::Cache c;
  auto f = dkprotovis::update(&c, 1, names, mask_of(std::string("lamp\0\0\0crate\0\0\0", 14)));
  EXPECT_EQ(f, (std::vector<u8>{0, 1, 0, 0}));
  EXPECT_FALSE(c.all_visible);

  f = dkprotovis::update(&c, 1, names, {});
  EXPECT_EQ(f, (std::vector<u8>{1, 1, 1, 1}));

  // unknown names and a mask without a trailing zero are ignored / parsed safely
  f = dkprotovis::update(&c, 1, names, mask_of(std::string("nothing\0tree", 12)));
  EXPECT_EQ(f, (std::vector<u8>{1, 0, 1, 1}));
}

TEST(DkProtoVis, RebuildsOnNewLevel) {
  dkprotovis::Cache c;
  dkprotovis::update(&c, 1, {"a", "b"}, mask_of(std::string("a\0", 2)));
  const auto& f = dkprotovis::update(&c, 2, {"x", "a", "y"}, mask_of(std::string("a\0", 2)));
  EXPECT_EQ(f, (std::vector<u8>{1, 0, 1}));
}
