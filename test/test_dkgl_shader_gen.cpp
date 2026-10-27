// T103 (AI-assisted): golden checks on the GENERATED dkgl ocean shaders and the
// std140 uniform-offset table (scripts/ai/dkgl_gen_shaders.py). Read-only sanity:
// header swap, uniform lift, sampler bindings, table shape. No GLSL parser, no
// hardware. File reads follow the test/decompiler/test_VuDisasm.cpp precedent
// (file_util::get_file_path + read_text_file on committed repo files).

#include "common/util/FileUtil.h"

#include "../game/graphics/deko3d_renderer/dkgl/dkgl_uniform_table.inc"
#include "gtest/gtest.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::vector<std::string> kPrograms = {
    "ocean_common", "ocean_envmap", "ocean_envmap_haze", "ocean_texture",
    "ocean_texture_mipmap", "generic", "shadow", "depth_cue",
    "direct_basic", "direct_basic_textured", "solid_color", "emerc", "merc2", "eye"};

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
        // named std140 blocks (emerc ub_bones) need an explicit binding for UAM
        if (line.find("uniform ub_") != std::string::npos) {
          EXPECT_NE(line.find("layout(std140, binding = "), std::string::npos)
              << prog << "." << stage << ": unbound uniform block: " << line;
        }
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
    EXPECT_LE(t.vert.block_size + t.frag.block_size, 512) << t.program_name;  // kUniformShadowBytes
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
// dkgl review (AI-assisted): the table offsets must be where the GPU reads the members.
// Recompute std140 offsets from the generated GLSL block (padding members included) and
// compare with dkgl_uniform_table.inc. The first generator packed every member in a
// 16-byte slot without padding, so `float a; float b; vec4 c;` read 0/4/16 on the GPU while
// the table said 0/16/32.
TEST(DkglShaderGen, Std140OffsetsMatchTable) {
  const std::regex member(R"(^\s*(float|int|uint|bool|vec2|vec3|vec4|mat4)\s+(\w+)\s*;)");
  auto size_align = [](const std::string& t, u32* size, u32* align) {
    if (t == "vec2") {
      *size = 8, *align = 8;
    } else if (t == "vec3") {
      *size = 12, *align = 16;
    } else if (t == "vec4") {
      *size = 16, *align = 16;
    } else if (t == "mat4") {
      *size = 64, *align = 16;
    } else {
      *size = 4, *align = 4;
    }
  };
  for (const auto& t : dkgl::kProgramUniformTables) {
    for (int st = 0; st < 2; ++st) {
      const dkgl::StageUniformTable& tab = st == 0 ? t.vert : t.frag;
      if (tab.count == 0) {
        continue;
      }
      const std::string text = read_generated(t.program_name, st == 0 ? "vert" : "frag");
      const size_t open = text.find("uniform ");
      const size_t lb = text.find('{', open);
      const size_t rb = text.find("};", lb);
      ASSERT_NE(lb, std::string::npos) << t.program_name;
      std::istringstream body(text.substr(lb + 1, rb - lb - 1));
      std::string line;
      u32 off = 0;
      std::vector<std::pair<std::string, u32>> got;
      while (std::getline(body, line)) {
        std::smatch m;
        if (!std::regex_search(line, m, member)) {
          continue;
        }
        u32 size, align;
        size_align(m[1].str(), &size, &align);
        off = (off + align - 1) & ~(align - 1);
        got.push_back({m[2].str(), off});
        off += size;
      }
      for (u32 i = 0; i < tab.count; ++i) {
        auto it = std::find_if(got.begin(), got.end(),
                               [&](const auto& g) { return g.first == tab.slots[i].name; });
        ASSERT_NE(it, got.end()) << t.program_name << " " << tab.slots[i].name;
        EXPECT_EQ(it->second, tab.slots[i].offset) << t.program_name << " " << tab.slots[i].name;
      }
    }
  }
}

// dkgl review: UAM compiles each stage alone, so every varying needs an explicit location
// and the frag input must carry the location of the vert output of the same name.
TEST(DkglShaderGen, VaryingLocationsMatchByName) {
  const std::regex var(
      R"(layout\(location = (\d+)\) (?:flat |smooth |noperspective )?(in|out) \w+ (\w+);)");
  for (const auto& prog : kPrograms) {
    std::map<std::string, std::string> vert_out;
    const std::string v = read_generated(prog, "vert");
    for (std::sregex_iterator it(v.begin(), v.end(), var), end; it != end; ++it) {
      if ((*it)[2] == "out") {
        vert_out[(*it)[3]] = (*it)[1];
      }
    }
    const std::string f = read_generated(prog, "frag");
    for (std::sregex_iterator it(f.begin(), f.end(), var), end; it != end; ++it) {
      if ((*it)[2] == "in") {
        ASSERT_TRUE(vert_out.count((*it)[3])) << prog << " " << (*it)[3];
        EXPECT_EQ(vert_out[(*it)[3]], (*it)[1]) << prog << " " << (*it)[3];
      }
    }
  }
}
