// R1 (AI-assisted): host replay of the deko3d tfrag CPU half (TFragmentDk.cpp, compiled here
// with DK_HOST_REPLAY). Input: one frame dumped on the Switch with gk_dkdump.txt
// (dk_replay.dma + dk_replay.golden.txt, see docs/deko3d/REPLAY_TEST.md) and the level files
// in out/jak1/fr3. The host build must print the same golden text, line for line.
//
//   cmake --build build-host --target goalc-test -j8
//   ./build-host/goalc-test --gtest_filter='DkTfragReplay.*'
//
// DK_REPLAY_DIR=<dir> picks another dump folder (default test/deko3d/replay/jak1).

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"

#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/opengl_renderer/loader/common.h"
#include "game/graphics/pipelines/dk_device.h"

#include "gtest/gtest.h"

namespace {
std::vector<uint8_t> g_gpu_mem;  // fake GPU memory behind the index ring
dkpool::BufferPool g_pool;
std::unordered_map<std::string, std::unique_ptr<LevelData>> g_levels;
std::unordered_map<GLuint, dkpool::PoolBuffer> g_vbufs;
uint32_t g_next_tex = 1;
GLuint g_next_buf = 1;
u64 g_next_load = 1;
fs::path g_fr3_dir;
}  // namespace

// ---- stand-ins for the Switch-only GPU helpers TFragmentDk.cpp calls ----
namespace dkdev {
dkpool::BufferPool* buffer_pool() {
  return &g_pool;
}
CopyResult copy_to_gpu(uint64_t, const void*, uint32_t) {
  CopyResult r;
  r.ok = true;
  return r;
}
}  // namespace dkdev

namespace dktexgpu {
uint32_t upload_rgba8(uint32_t, uint32_t, const void*) {
  return g_next_tex++;
}
bool update_rgba8(uint32_t, uint32_t, uint32_t, const void*) {
  return true;
}
}  // namespace dktexgpu

const dkpool::PoolBuffer* dk_loader_buffer_find(GLuint id) {
  auto it = g_vbufs.find(id);
  return it == g_vbufs.end() ? nullptr : &it->second;
}

// Same steps as Loader's level load (decompress + deserialize), minus every GPU upload.
const LevelData* dktf_host_level(const char* name) {
  auto it = g_levels.find(name);
  if (it != g_levels.end()) {
    return it->second.get();
  }
  const fs::path path = g_fr3_dir / (std::string(name) + ".fr3");
  if (!fs::exists(path)) {
    g_levels[name] = nullptr;
    return nullptr;
  }
  auto file = file_util::read_binary_file(path);
  auto raw = compression::decompress_zstd(file.data(), file.size());
  auto ld = std::make_unique<LevelData>();
  ld->level = std::make_unique<tfrag3::Level>();
  Serializer ser(raw.data(), raw.size());
  ld->level->serialize(ser);
  ld->load_id = g_next_load++;
  ld->textures.resize(ld->level->textures.size());
  for (size_t i = 0; i < ld->textures.size(); i++) {
    ld->textures[i] = GLuint(i + 1);
  }
  for (int g = 0; g < tfrag3::TFRAG_GEOS; g++) {
    for (auto& tree : ld->level->tfrag_trees[g]) {
      tree.unpack();  // Loader.cpp:1315 does this after deserialize: it builds unpacked.indices
      const GLuint id = g_next_buf++;
      dkpool::PoolBuffer pb;
      pb.id = id;
      pb.gpu = 0x100000000ull + (u64)id * 0x1000000ull;
      pb.capacity = u32(tree.packed_vertices.vertices.size() * sizeof(tfrag3::PreloadedVertex));
      g_vbufs[id] = pb;
      ld->tfrag_vertex_data[g].push_back(id);
    }
  }
  // T70 (AI-assisted): shrub trees too (Loader.cpp:1323 unpacks them; indices need no unpack).
  for (auto& tree : ld->level->shrub_trees) {
    tree.unpack();
    const GLuint id = g_next_buf++;
    dkpool::PoolBuffer pb;
    pb.id = id;
    pb.gpu = 0x100000000ull + (u64)id * 0x1000000ull;
    pb.capacity = u32(tree.unpacked.vertices.size() * sizeof(tfrag3::ShrubGpuVertex));
    g_vbufs[id] = pb;
    ld->shrub_vertex_data.push_back(id);
  }
  const LevelData* out = ld.get();
  g_levels[name] = std::move(ld);
  return out;
}

namespace {
std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < s.size()) {
    size_t end = s.find('\n', start);
    if (end == std::string::npos) {
      end = s.size();
    }
    out.push_back(s.substr(start, end - start));
    start = end + 1;
  }
  return out;
}
}  // namespace

// T70 (AI-assisted): shared by test_dk_shrub_replay.cpp. Loads the dump's chain, points the
// level lookup at out/jak1/fr3 and (once) sets up the fake GPU memory. false = no replay data.
bool dk_replay_load(std::vector<u8>* chain, u32* start_offset) {
  const char* env = std::getenv("DK_REPLAY_DIR");
  const fs::path dir = env ? fs::path(env)
                           : file_util::get_jak_project_dir() / "test" / "deko3d" / "replay" /
                                 "jak1";
  const fs::path dma_path = dir / "dk_replay.dma";
  g_fr3_dir = file_util::get_jak_project_dir() / "out" / "jak1" / "fr3";
  if (!fs::exists(dma_path) || !fs::exists(g_fr3_dir)) {
    return false;
  }
  auto dma_file = file_util::read_binary_file(dma_path);
  Serializer ser(dma_file.data(), dma_file.size());
  ser.from_ptr(start_offset);
  ser.from_pod_vector(chain);
  if (g_gpu_mem.empty()) {
    g_gpu_mem.assign(64u << 20, 0);
    g_pool.init(g_gpu_mem.data(), 0x200000000ull, (uint32_t)g_gpu_mem.size());
  }
  return !chain->empty();
}

TEST(DkTfragReplay, MatchesSwitchGolden) {
  const char* env = std::getenv("DK_REPLAY_DIR");
  const fs::path dir = env ? fs::path(env)
                           : file_util::get_jak_project_dir() / "test" / "deko3d" / "replay" /
                                 "jak1";
  const fs::path dma_path = dir / "dk_replay.dma";
  const fs::path gold_path = dir / "dk_replay.golden.txt";
  // dk_replay.dma = FixedChunkDmaCopier::serialize_last_result (start offset + relocated chain).
  u32 start_offset = 0;
  std::vector<u8> chain;
  if (!fs::exists(gold_path) || !dk_replay_load(&chain, &start_offset)) {
    GTEST_SKIP() << "no replay data in " << dir.string() << " (see docs/deko3d/REPLAY_TEST.md)";
  }

  // 4 builds: the Switch dump comes after hundreds of frames, so every index ring slot
  // (3 of them) has already grown. The 4th build reuses slot 0 at its grown size.
  std::string host;
  for (int i = 0; i < 4; i++) {
    host.clear();
    dktf::set_golden(i == 3 ? &host : nullptr);
    dktf::capture_chain(chain.data(), start_offset);
    dktf::build_frame(nullptr);
  }
  dktf::set_golden(nullptr);

  const fs::path out_path = file_util::get_jak_project_dir() / "build-host" / "dk_replay.host.txt";
  file_util::write_text_file(out_path, host);

  const auto want = split_lines(file_util::read_text_file(gold_path));
  const auto got = split_lines(host);
  ASSERT_GT(got.size(), 2u) << "host build produced no draws; full output in " << out_path;
  size_t bad = 0;
  for (size_t i = 0; i < std::max(want.size(), got.size()); i++) {
    const std::string w = i < want.size() ? want[i] : "<missing>";
    const std::string g = i < got.size() ? got[i] : "<missing>";
    if (w != g && bad++ < 10) {
      ADD_FAILURE() << "line " << i + 1 << "\n  switch: " << w << "\n  host:   " << g;
    }
  }
  EXPECT_EQ(bad, 0u) << bad << " differing lines; host output in " << out_path.string()
                     << " (diff it against " << gold_path.string() << ")";
  std::printf("dk-replay: %zu golden lines, %zu differ -> %s\n", want.size(), bad,
              bad == 0 ? "PASS" : "FAIL");
}
