#include "game/graphics/deko3d_renderer/ShrubDk.h"

// T70/T71 (AI-assisted): deko3d shrub CPU half, cloned from TFragmentDk.cpp. Port of GL
// Shrub::render + render_tree for jak1 (no culling, no proto-vis mask, strips only).
// DK_HOST_REPLAY: test/test_dk_shrub_replay.cpp compiles this file on the Mac.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/versions/versions.h"  // §7.2 phase 1: set_game pick

#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

#if defined(DK_HOST_REPLAY)
const dkpool::PoolBuffer* dk_loader_buffer_find(GLuint id);  // test-defined
const LevelData* dktf_host_level(const char* name);          // test-defined
#define DKSH_LEVEL(loader, name) dktf_host_level(name)
#define DKSH_NEED_LOADER 0
#else
#define DKSH_LEVEL(loader, name) (loader)->get_tfrag3_level(name)
#define DKSH_NEED_LOADER 1
#endif

// Defined in background_common.cpp, not declared in its header (same as TFragmentDk.cpp).
std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z);

namespace {

constexpr uint32_t kMaxDraws = 500;  // village1 = 47; small: shares the frame cmd slice with tfrag
constexpr int kTodColors = 8192;      // Shrub TIME_OF_DAY_COLOR_COUNT (same as tfrag)
constexpr int kIdxSlots = 3;          // index ring slots, same reason as TFragmentDk.cpp

struct Captured {
  TfragPcPortData pc;
  bool valid = false;
};
// §7.2 phase 1: per-game bucket pick (defaults to jak1, like dkgl_frame.cpp).
const int* s_buckets = dkshrub::kJak1Buckets;
int s_pass_count = dkshrub::kNumPasses;
bool s_jak2 = false;
Captured s_cap[dkshrub::kMaxPasses];
dktf::FrameDraws s_frame;

// Same reads as GL Shrub::render for jak1. true = the bucket had shrub data.
bool walk_bucket(const void* data, uint32_t slot, uint32_t next, Captured* out) {
  DmaFollower d(data, slot);
  d.read_and_advance();  // the bucket's own NEXT (size 0)
  if (d.current_tag().kind == DmaTag::Kind::CALL) {
    return false;  // renderer didn't run this frame
  }
  if (d.current_tag_offset() == next) {
    return false;
  }
  auto pc = d.read_and_advance();
  if (pc.size_bytes != sizeof(TfragPcPortData)) {
    return false;
  }
  memcpy(&out->pc, pc.data, sizeof(TfragPcPortData));
  out->pc.level_name[11] = '\0';
  return true;
}

// ---- index ring (copy of TFragmentDk.cpp's, own slots) ----
struct IdxSlot {
  dkpool::PoolBuffer pb;
  uint32_t used = 0;
  uint32_t want = 0;
};
IdxSlot s_idx[kIdxSlots];
uint32_t s_build = 0;

IdxSlot* ring_begin() {
  IdxSlot* s = &s_idx[s_build % kIdxSlots];
  s->used = 0;
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  const uint32_t need = std::max<uint32_t>(1u << 20, s->want + s->want / 2);
  if (pool && (!s->pb.ok() || s->pb.capacity < s->want)) {
    if (s->pb.ok()) {
      pool->release(s->pb.id);
    }
    s->pb = pool->acquire(need);
    switch_run_logf("[dk] shrub index ring slot %u: %u KB %s", s_build % kIdxSlots, need >> 10,
                    s->pb.ok() ? "ok" : "FAILED");
  }
  s->want = 0;
  return s;
}

// ---- time-of-day palettes (copy of TFragmentDk.cpp's, keyed by ShrubTree) ----
struct TodEntry {
  uint32_t id = 0;
  uint64_t load_id = 0;
  math::Vector<s32, 4> itimes[4];
  bool valid = false;
  uint32_t last_build = 0;
};
std::unordered_map<const tfrag3::ShrubTree*, TodEntry> s_tod;
std::vector<uint32_t> s_tod_free;
std::vector<math::Vector<u8, 4>> s_tod_colors;

uint32_t tod_for(const LevelData* ld,
                 const tfrag3::ShrubTree& tree,
                 const math::Vector<s32, 4>* it) {
  const auto& colors = tree.time_of_day_colors;
  if (colors.color_count == 0 || colors.color_count > (u32)kTodColors) {
    return 0;
  }
  TodEntry& e = s_tod[&tree];
  e.last_build = s_build;
  if (e.id == 0) {
    if (!s_tod_free.empty()) {
      e.id = s_tod_free.back();
      s_tod_free.pop_back();
    } else {
      static uint8_t s_zeros[kTodColors * 4] = {};
      e.id = dktexgpu::upload_rgba8(kTodColors, 1, s_zeros);
      if (e.id == 0) {
        s_tod.erase(&tree);
        return 0;
      }
    }
    e.valid = false;
  }
  if (e.load_id != ld->load_id) {
    e.load_id = ld->load_id;
    e.valid = false;
  }
  if (!e.valid || tod_itimes_changed(e.itimes, it)) {
    s_tod_colors.resize(colors.color_count);
    interp_time_of_day(it, colors, s_tod_colors.data());
    if (dktexgpu::update_rgba8_deferred(e.id, colors.color_count, 1, s_tod_colors.data())) {
      tod_itimes_store(e.itimes, it);
      e.valid = true;
    }
  }
  return e.id;
}

void tod_evict_unused() {
  for (auto it = s_tod.begin(); it != s_tod.end();) {
    if (s_build - it->second.last_build > 300) {
      s_tod_free.push_back(it->second.id);
      it = s_tod.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

namespace dkshrub {

void capture_chain(const void* data, uint32_t start_offset) {
  for (auto& c : s_cap) {
    c.valid = false;
  }
  if (data == nullptr) {
    return;
  }
  DmaFollower d(data, start_offset);
  uint32_t buckets_base;
  if (s_jak2) {
    // §7.2 phase 1: jak2 chains have no CALL head -- the bucket table starts at the
    // offset itself (GL dispatch_buckets_jak2).
    buckets_base = d.current_tag_offset();
  } else {
    if (d.current_tag().kind != DmaTag::Kind::CALL) {
      return;  // not a game frame chain
    }
    buckets_base = d.current_tag_offset() + 16;
  }
  for (int i = 0; i < s_pass_count; i++) {
    const uint32_t slot = buckets_base + 16 * s_buckets[i];
    s_cap[i].valid = walk_bucket(data, slot, slot + 16, &s_cap[i]);
  }
}

const TfragPcPortData* captured_pc(int pass) {
  if (pass < 0 || pass >= s_pass_count || !s_cap[pass].valid) {
    return nullptr;
  }
  return &s_cap[pass].pc;
}

const dktf::FrameDraws& frame_draws() {
  return s_frame;
}

void build_frame(Loader* loader) {
  // same as TieDk: record_list fogs with f.fog_rgba, taken from tfrag (built first) (AI-assisted)
  memcpy(s_frame.fog_rgba, dktf::frame_draws().fog_rgba, 4);
  s_frame.cams.clear();
  s_frame.draws.clear();
  s_frame.tris = 0;
  s_frame.dropped = 0;
  s_frame.alpha_draws = 0;
  bool any = false;
  for (const auto& c : s_cap) {
    any = any || c.valid;
  }
  if ((DKSH_NEED_LOADER && loader == nullptr) || !any) {
    return;
  }
  s_build++;
  IdxSlot* ring = ring_begin();
  static std::vector<std::pair<int, int>> groups;
  static std::vector<u32> idx;

  for (int b = 0; b < s_pass_count; b++) {
    if (!s_cap[b].valid) {
      continue;
    }
    const auto& cam = s_cap[b].pc.camera;
    const LevelData* ld = DKSH_LEVEL(loader, s_cap[b].pc.level_name);
    if (ld == nullptr || !ld->level) {
      continue;
    }
    const uint8_t cam_idx = (uint8_t)s_frame.cams.size();
    dktf::CamParams cp;
    memcpy(cp.camera, cam.camera, sizeof(cp.camera));
    const auto pcm = make_new_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
    memcpy(cp.pc_camera, pcm.data(), sizeof(cp.pc_camera));
    cp.hvdf_off = cam.hvdf_off;
    cp.trans = cam.trans;
    cp.fog = cam.fog;
    s_frame.cams.push_back(cp);

    const auto& trees = ld->level->shrub_trees;
    for (size_t t = 0; t < trees.size() && t < ld->shrub_vertex_data.size(); t++) {
      const auto& tree = trees[t];
      const dkpool::PoolBuffer* vb = dk_loader_buffer_find(ld->shrub_vertex_data[t]);
      if (vb == nullptr || tree.indices.empty()) {
        continue;
      }
      const uint32_t tod = tod_for(ld, tree, cam.itimes);
      if (tod == 0) {
        continue;
      }
      groups.resize(tree.static_draws.size());
      if (idx.size() < tree.indices.size()) {  // T86: grow only (no per-tree zero-fill)
        idx.resize(tree.indices.size());
      }
      const u32 n = make_all_visible_index_list(groups.data(), idx.data(), tree.static_draws,
                                                tree.indices.data());
      const uint32_t bytes = n * 4;
      ring->want += bytes;
      if (!ring->pb.ok() || ring->used + bytes > ring->pb.capacity) {
        s_frame.dropped += (uint32_t)tree.static_draws.size();
        continue;  // ring_begin grows the slot next time this slot comes round
      }
      const uint32_t base = ring->used;
      memcpy(ring->pb.cpu + base, idx.data(), bytes);
      ring->used += (bytes + 255) & ~255u;

      for (size_t i = 0; i < tree.static_draws.size(); i++) {
        if (groups[i].second == 0) {
          continue;
        }
        if (s_frame.draws.size() >= kMaxDraws) {
          s_frame.dropped++;
          continue;
        }
        const auto& draw = tree.static_draws[i];
        dktf::DrawCmd c;
        c.vert_gpu = vb->gpu;
        c.vert_bytes = vb->capacity;
        c.idx_gpu = ring->pb.gpu + base + (uint64_t)groups[i].first * 4;
        c.idx_count = (uint32_t)groups[i].second;
        if (draw.tree_tex_id < ld->textures.size()) {
          c.tex_id = ld->textures[draw.tree_tex_id];
        }
        c.tod_id = tod;
        c.strips = true;  // GL Shrub always draws GL_TRIANGLE_STRIP
        c.cam = cam_idx;
        c.range = (uint8_t)b;  // §7.2 phase 1: record split tag
        dktf::fill_draw_state(draw.mode, &c);
        s_frame.draws.push_back(c);
        s_frame.tris += draw.num_triangles;
      }
    }
  }
  ring->want = std::max(ring->want, ring->used);
  tod_evict_unused();
}

// ---- §7.2 phase 1 (AI-assisted): per-game bucket tables ----

void set_game(GameVersion v) {
  s_jak2 = v == GameVersion::Jak2;
  s_buckets = s_jak2 ? kJak2Buckets : kJak1Buckets;
  s_pass_count = s_jak2 ? kJak2NumPasses : kNumPasses;
}

int pass_count() {
  return s_pass_count;
}

}  // namespace dkshrub
#endif  // __SWITCH__ || DK_HOST_REPLAY
