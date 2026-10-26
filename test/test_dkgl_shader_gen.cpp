// T103 (AI-assisted): golden checks on the GENERATED dkgl ocean shaders and the
// std140 uniform-offset table (scripts/ai/dkgl_gen_shaders.py). Read-only sanity:
// header swap, uniform lift, sampler bindings, table shape. No GLSL parser, no
// hardware. File reads follow the test/decompiler/test_VuDisasm.cpp precedent
// (file_util::get_file_path + read_text_file on committed repo files).

#include "common/util/FileUtil.h"

#include "../game/graphics/deko3d_renderer/dkgl/dkgl_uniform_table.inc"
#include "gtest/gtest.h"

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::vector<std::string> kPrograms = {
    "ocean_common", "ocean_envmap", "ocean_envmap_haze", "ocean_texture",
    "ocean_texture_mipmap"};

std::string read_generated(const std::string& prog, const std::string& stage) {
  return file_util::read_text_file(
      file_util::get_file_path({"game/graphics/deko3d_renderer/shaders/dkgl/" + prog + "." +
                                stage + ".glsl"}));
}

size_t count_of(const std::string& hay, const std::string& needle) {
  size_t n = 0, pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    ++n;
    pos += needle.size();
  }
  return n;
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    out.push_back(line);
  }
  return out;
}

const dkgl::ProgramUniformTable* find_prog(const std::string& name) {
  for (const auto& t : dkgl::kProgramUniformTables) {
    if (name == t.program_name) {
      return &t;
    }
  }
  return nullptr;
}

}  // namespace

TEST(DkglShaderGen, GeneratedGlslShape) {
  for (const auto& prog : kPrograms) {
    for (const char* stage : {"vert", "frag"}) {
      const auto text = read_generated(prog, stage);
      const auto first_nl = text.find('\n');
      ASSERT_NE(first_nl, std::string::npos) << prog << "." << stage;
      EXPECT_EQ(text.substr(0, first_nl), "#version 310 es") << prog << "." << stage;
      EXPECT_EQ(text.find("#version 410"), std::string::npos) << prog << "." << stage;
      EXPECT_EQ(count_of(text, "precision highp float;"), 1) << prog << "." << stage;

      // no loose default uniforms left outside the std140 blocks
      const std::regex loose(
          R"(^[ \t]*uniform[ \t]+(float|int|uint|vec2|vec3|vec4|mat4)[ \t])");
      // every sampler line carries an explicit binding
      for (const auto& line : lines_of(text)) {
        EXPECT_FALSE(std::regex_search(line, loose))
            << prog << "." << stage << ": loose uniform: " << line;
        if (line.find("sampler2D") != std::string::npos) {
          EXPECT_NE(line.find("layout(binding ="), std::string::npos)
              << prog << "." << stage << ": unbound sampler: " << line;
        }
      }
    }
  }
}

TEST(DkglShaderGen, UniformTables) {
  ASSERT_EQ(dkgl::kProgramUniformTables->program_name != nullptr, true);  // sanity
  ASSERT_EQ(std::size(dkgl::kProgramUniformTables), kPrograms.size());
  std::set<std::string> names;
  for (const auto& t : dkgl::kProgramUniformTables) {
    names.insert(t.program_name);
    // one UBO ring slice (0x100) holds both stages (T106)
    EXPECT_LE(t.vert.block_size + t.frag.block_size, 256) << t.program_name;
  }
  EXPECT_EQ(names, std::set<std::string>(kPrograms.begin(), kPrograms.end()));

  const auto* common = find_prog("ocean_common");
  ASSERT_NE(common, nullptr);
  ASSERT_EQ(common->frag.count, 4u);
  std::set<std::string> slot_names;
  for (u32 i = 0; i < common->frag.count; ++i) {
    const auto& slot = common->frag.slots[i];
    slot_names.insert(slot.name);
    EXPECT_EQ(slot.offset % 16, 0) << slot.name;
  }
  const std::set<std::string> expected = {"color_mult", "alpha_mult", "fog_color", "bucket"};
  EXPECT_EQ(slot_names, expected);
  EXPECT_LE(common->frag.block_size, 256);
}