#include "game/graphics/deko3d_renderer/TFragmentDk.h"

// DK_HOST_REPLAY (R1, AI-assisted): test/test_dk_tfrag_replay.cpp compiles this file on the
// Mac. The CPU half runs for real; the test defines the GPU helpers and the level lookup.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"

#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/opengl_renderer/background/background_common.h"  // T60: CPU helpers
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

#if defined(DK_HOST_REPLAY)
const dkpool::PoolBuffer* dk_loader_buffer_find(GLuint id);  // test-defined
const LevelData* dktf_host_level(const char* name);          // test-defined
#define DKTF_LEVEL(loader, name) dktf_host_level(name)
#define DKTF_NEED_LOADER 0
#else
#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/util/Serializer.h"
#define DKTF_LEVEL(loader, name) (loader)->get_tfrag3_level(name)
#define DKTF_NEED_LOADER 1
#endif

// T60: defined in background_common.cpp (external linkage) but not declared in its header.
std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z);

namespace {

// Per-geom tree caches, same shape as the GL renderer's m_cached_trees.
std::vector<dktf::TreeCacheDk> s_trees[tfrag3::TFRAG_GEOS];

}  // namespace

namespace dktf {

void discard_tree_cache() {
  // FIX 33: BufferPool chunks are pooled, not freed — dropping the cache IS
  // the discard (GL parity: TFragment::discard_tree_cache).
  for (auto& geom : s_trees) {
    geom.clear();
  }
  // T36b note: the time-of-day texture ids die with the cache (dktexgpu slots
  // are monotonic; T38 owns per-frame invalidation, not this function).
}

bool setup_for_level(const tfrag3::Level* lev_data, const char* level_name) {
  discard_tree_cache();
  const char* name = level_name ? level_name : "?";
  if (lev_data == nullptr) {
    switch_run_logf("[dk] tfrag setup: null level (%s)", name);
    return false;
  }
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  if (pool == nullptr) {
    switch_run_logf("[dk] tfrag setup: no buffer pool (%s)", name);
    return false;
  }

  int trees = 0;
  for (int geom = 0; geom < tfrag3::TFRAG_GEOS; ++geom) {
    for (size_t tree_idx = 0; tree_idx < lev_data->tfrag_trees[geom].size(); ++tree_idx) {
      const auto& tree = lev_data->tfrag_trees[geom][tree_idx];
      TreeCacheDk cache;

      // vertices: PreloadedVertex array, 32 bytes each.
      const auto& verts = tree.packed_vertices.vertices;
      if (!verts.empty()) {
        const uint32_t bytes = uint32_t(verts.size() * sizeof(tfrag3::PreloadedVertex));
        dkpool::PoolBuffer pb = pool->acquire(bytes);
        if (!pb.ok()) {
          switch_run_logf("[dk] tfrag setup: verts acquire failed (tree %zu, geom %d)", tree_idx, geom);
          continue;  // skip THIS tree, keep the rest
        }
        if (!dkdev::copy_to_gpu(pb.gpu, verts.data(), bytes).ok) {
          pool->release(pb.id);
          switch_run_logf("[dk] tfrag setup: verts copy failed (tree %zu, geom %d)", tree_idx, geom);
          continue;
        }
        cache.vert_gpu = pb.gpu;
        cache.vert_count = uint32_t(verts.size());
      }

      // indices: u32 each.
      const auto& inds = tree.unpacked.indices;
      if (!inds.empty()) {
        const uint32_t bytes = uint32_t(inds.size() * sizeof(uint32_t));
        dkpool::PoolBuffer pb = pool->acquire(bytes);
        if (!pb.ok()) {
          switch_run_logf("[dk] tfrag setup: indices acquire failed (tree %zu, geom %d)", tree_idx, geom);
          continue;
        }
        if (!dkdev::copy_to_gpu(pb.gpu, inds.data(), bytes).ok) {
          pool->release(pb.id);
          switch_run_logf("[dk] tfrag setup: indices copy failed (tree %zu, geom %d)", tree_idx, geom);
          continue;
        }
        cache.idx_gpu = pb.gpu;
        cache.idx_count = uint32_t(inds.size());
      }

      // time-of-day palette (T36b): one zeroed 8192x1 RGBA8 upload so frame 1
      // has a valid texture; T38 uploads the real interpolated palette later.
      {
        static uint8_t s_tod_zeros[8192 * 4] = {};  // TIME_OF_DAY_COLOR_COUNT x RGBA8
        const uint32_t tod_id = dktexgpu::upload_rgba8(8192, 1, s_tod_zeros);
        if (tod_id != 0) {
          cache.tod_tex_id = int(tod_id);
        }
        // tod_id == 0: failure already logged at the source — leave -1, keep going.
      }
      cache.draws = &tree.draws;
      cache.vis = &tree.bvh;
      cache.use_strips = tree.use_strips;
      s_trees[geom].push_back(cache);
      ++trees;
    }
  }
  switch_run_logf("[dk] tfrag setup: level %s, trees=%d", name, trees);
  return true;
}

}  // namespace dktf

// ============================================================================================
// T60 (AI-assisted): P3 first visible tfrag draw — CPU half. Ports TFragment::render /
// handle_initialization / render_tree (opengl_renderer/background/TFragment.cpp) without GL.
// ============================================================================================
namespace {

using dktf::DrawCmd;

// T62: one entry per jak1 tfrag bucket, in bucket (= GL draw) order so the alpha passes are
// recorded after the opaque ones. sky = SkyBlendHandler bucket (sky packets before the tfrag).
using TK = tfrag3::TFragmentTreeKind;
struct BucketPass {
  int bucket;
  bool sky;
  TK k0, k1;
};
constexpr BucketPass kJak1Passes[] = {
    {6, false, TK::NORMAL, TK::LOWRES},       // TFRAG_LEVEL0
    {13, false, TK::NORMAL, TK::LOWRES},      // TFRAG_LEVEL1
    {32, true, TK::TRANS, TK::LOWRES_TRANS},  // TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0
    {34, false, TK::DIRT, TK::DIRT},          // TFRAG_DIRT_LEVEL0
    {36, false, TK::ICE, TK::ICE},            // TFRAG_ICE_LEVEL0
    {39, true, TK::TRANS, TK::LOWRES_TRANS},  // TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1
    {41, false, TK::DIRT, TK::DIRT},          // TFRAG_DIRT_LEVEL1
    {43, false, TK::ICE, TK::ICE},            // TFRAG_ICE_LEVEL1
};
constexpr int kNumPasses = sizeof(kJak1Passes) / sizeof(kJak1Passes[0]);
constexpr int kJak1LevelMax = 2;                // jak1 LEVEL_MAX (num_vis_to_copy)
constexpr uint32_t kMaxDraws = 2500;            // keeps the frame cmd slice from overflowing
constexpr int kTodColors = 8192;                // TFragment::TIME_OF_DAY_COLOR_COUNT
constexpr int kIdxSlots = 3;                    // index ring slots; see ring_begin()

struct Captured {
  bool valid = false;
  TfragPcPortData pc;
};
Captured s_cap[kNumPasses];
uint8_t s_fog[4] = {0, 0, 0, 0};
dktf::FrameDraws s_frame;

// ---- R1 replay golden: a text list of what build_frame built, GPU addresses left out so the
// Switch and the host replay print the same bytes. Off (null) except for the dump frame/test.
std::string* s_golden = nullptr;

void gold(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void gold(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  s_golden->append(buf);
  s_golden->push_back('\n');
}

void gold_vec(const char* tag, const math::Vector4f* v, int n) {
  std::string line = tag;
  char buf[24];
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < 4; j++) {
      snprintf(buf, sizeof(buf), " %.9g", v[i][j]);
      line += buf;
    }
  }
  gold("%s", line.c_str());
}

uint32_t fnv1a(const void* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
  }
  return h;
}

#if !defined(DK_HOST_REPLAY)
int s_dump_wait = -1;  // frames with tfrag data still to skip before the dump; -1 = off
char s_dump_dir[96] = {};
bool s_dump_golden = false;  // the dump frame's build_frame writes the golden file

bool write_file(const char* path, const void* data, size_t n) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    return false;
  }
  const bool ok = fwrite(data, 1, n, f) == n;
  return fclose(f) == 0 && ok;
}
#endif

bool looks_like_tfrag_init(const DmaFollower& d) {
  return d.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         d.current_tag_vifcode1().kind == VifCode::Kind::DIRECT &&
         d.current_tag_vifcode1().immediate == 2;
}

// One bucket, same reads as TFragment::render (non-child mode). Returns true with
// out->pc filled when the bucket carried tfrag data.
bool walk_bucket(const void* data,
                 uint32_t slot,
                 uint32_t next,
                 bool vis_copy,
                 bool sky,
                 Captured* out) {
  DmaFollower d(data, slot);
  d.read_and_advance();  // the bucket's own NEXT (size 0)
  int guard = 0;
  auto at_end = [&] { return d.current_tag_offset() == next || ++guard > 20000; };
  if (d.current_tag().kind == DmaTag::Kind::CALL) {
    return false;  // renderer didn't run this frame
  }
  // T62: same skips as SkyBlendHandler::render (SkyRenderer.cpp) before its tfrag child.
  if (sky && d.current_tag().qwc == 8) {
    d.read_and_advance();  // set-display-gs-state
    while (!at_end() && d.current_tag().qwc == 6) {
      d.read_and_advance();  // sky blend setup
      d.read_and_advance();  // sky blend draw
    }
    for (int i = 0; i < 3 && !at_end(); i++) {
      d.read_and_advance();  // reset_alpha, reset_gs, empty
    }
  }
  if (vis_copy && d.current_tag_vifcode1().kind == VifCode::Kind::PC_PORT) {
    for (int i = 0; i < kJak1LevelMax && !at_end(); i++) {
      d.read_and_advance();  // occlusion string (unused for now: no occlusion culling)
      d.read_and_advance();
    }
  }
  if (d.current_tag().kind == DmaTag::Kind::CALL) {
    return false;
  }
  bool got = false;
  while (!at_end() && looks_like_tfrag_init(d)) {
    for (int i = 0; i < 5; i++) {  // setup_test, mat0, mat1, data, mscal
      d.read_and_advance();
    }
    auto pc = d.read_and_advance();
    if (pc.size_bytes == sizeof(TfragPcPortData)) {
      memcpy(&out->pc, pc.data, sizeof(TfragPcPortData));
      out->pc.level_name[11] = '\0';
      got = true;
    }
    d.read_and_advance();  // double-buffer setup
    while (!at_end() && d.current_tag_vifcode0().kind == VifCode::Kind::STCYCL) {
      d.read_and_advance();
    }
  }
  return got;
}

// ---- per-frame index ring: kIdxSlots pool chunks, one per frame -------------------------
// A slot is rewritten kIdxSlots builds later. Each build is followed by one present, and
// present N waits for frame N-2's fence, so by build k+3 frame k's GPU work is finished.
struct IdxSlot {
  dkpool::PoolBuffer pb;
  uint32_t used = 0;
  uint32_t want = 0;  // bytes the last build in this slot would have needed
};
IdxSlot s_idx[kIdxSlots];
uint32_t s_build = 0;

IdxSlot* ring_begin() {
  IdxSlot* s = &s_idx[s_build % kIdxSlots];
  s->used = 0;
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  const uint32_t need = std::max<uint32_t>(4u << 20, s->want + s->want / 2);
  if (pool && (!s->pb.ok() || s->pb.capacity < s->want)) {
    if (s->pb.ok()) {
      pool->release(s->pb.id);  // safe: this slot's last frame is done (see above)
    }
    s->pb = pool->acquire(need);
    switch_run_logf("[dk] tfrag index ring slot %u: %u KB %s", s_build % kIdxSlots, need >> 10,
                    s->pb.ok() ? "ok" : "FAILED");
  }
  s->want = 0;
  return s;
}

// ---- time-of-day palettes: one 8192x1 RGBA8 texture per tree, recycled -----------------
struct TodEntry {
  uint32_t id = 0;
  uint64_t load_id = 0;
  math::Vector<s32, 4> itimes[4];
  bool valid = false;
  uint32_t last_build = 0;
};
std::unordered_map<const tfrag3::TfragTree*, TodEntry> s_tod;
std::vector<uint32_t> s_tod_free;  // ids we own; never released (release leaks image memory)
std::vector<math::Vector<u8, 4>> s_tod_colors;

uint32_t tod_for(const LevelData* ld, const tfrag3::TfragTree& tree, const math::Vector<s32, 4>* it) {
  if (tree.colors.color_count == 0 || tree.colors.color_count > (u32)kTodColors) {
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
    e.valid = false;  // same address, different level load
  }
  if (!e.valid || tod_itimes_changed(e.itimes, it)) {
    s_tod_colors.resize(tree.colors.color_count);
    interp_time_of_day(it, tree.colors, s_tod_colors.data());
    if (dktexgpu::update_rgba8(e.id, tree.colors.color_count, 1, s_tod_colors.data())) {
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

// Pure port of setup_opengl_from_draw_mode + setup_tfrag_shader (background_common.cpp).
void fill_state(DrawMode m, DrawCmd* c) {
  c->clamp_s = m.get_clamp_s_enable();
  c->clamp_t = m.get_clamp_t_enable();
  c->filt = m.get_filt_enable();
  c->decal = m.get_decal();
  c->depth_test = m.get_zt_enable();
  c->depth_op = 0;
  if (c->depth_test) {
    switch (m.get_depth_test()) {
      case GsTest::ZTest::NEVER: c->depth_op = 3; break;
      case GsTest::ZTest::GEQUAL: c->depth_op = 1; break;
      case GsTest::ZTest::GREATER: c->depth_op = 2; break;
      default: c->depth_op = 0; break;  // ALWAYS
    }
  }
  c->blend = 0;
  if (m.get_ab_enable()) {
    switch (m.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST: c->blend = 1; break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST: c->blend = 2; break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST: c->blend = 3; break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST: c->blend = 4; break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST: c->blend = 5; break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST: c->blend = 6; break;
      default: c->blend = 0; break;  // DISABLED, SRC_SRC_SRC_SRC
    }
  }
  bool no_z_write = false;
  c->alpha_min = 0.f;
  c->afail_second = false;
  if (m.get_at_enable()) {
    if (m.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
      c->alpha_min = m.get_aref() / 127.f;
      if (m.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
        if (m.get_depth_write_enable()) {
          c->afail_second = true;
          c->aref_second = c->alpha_min;
        } else {
          c->alpha_min = 0.f;
        }
      }
    } else if (m.get_alpha_test() == DrawMode::AlphaTest::NEVER &&
               m.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
      no_z_write = true;
    }
  }
  c->depth_write = m.get_depth_write_enable() && !no_z_write;
}

}  // namespace

namespace dktf {

const FrameDraws& frame_draws() {
  return s_frame;
}

void fill_draw_state(DrawMode m, DrawCmd* c) {
  fill_state(m, c);
}

void capture_chain(const void* data, uint32_t start_offset) {
  for (auto& c : s_cap) {
    c.valid = false;
  }
  if (data == nullptr) {
    return;
  }
  DmaFollower d(data, start_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;  // not a game frame chain
  }
  const uint32_t buckets_base = d.current_tag_offset() + 16;
  d.read_and_advance();  // CALL to the default-regs chain
  if (d.current_tag().kind != DmaTag::Kind::CNT) {
    return;
  }
  auto regs = d.read_and_advance();
  if (regs.size_bytes > 148) {
    memcpy(s_fog, regs.data + 144, 4);  // same bytes as SharedRenderState::fog_color
  }
  for (int i = 0; i < kNumPasses; i++) {
    const uint32_t slot = buckets_base + 16 * kJak1Passes[i].bucket;
    s_cap[i].valid =
        walk_bucket(data, slot, slot + 16, i == 0, kJak1Passes[i].sky, &s_cap[i]);
  }
#if !defined(DK_HOST_REPLAY)
  // R1: save this frame's whole DMA chain (relocated copy, same format as the GL debug dump).
  if (s_dump_wait >= 0 && s_cap[0].valid && s_dump_wait-- == 0) {
    FixedChunkDmaCopier copier(EE_MAIN_MEM_SIZE);
    copier.run(data, start_offset);
    Serializer ser;
    copier.serialize_last_result(ser);
    const auto out = ser.get_save_result();
    char path[128];
    snprintf(path, sizeof(path), "%s/dk_replay.dma", s_dump_dir);
    const bool ok = write_file(path, out.first, out.second);
    switch_run_logf("[dk] replay dump: %s %zu bytes %s", path, out.second, ok ? "ok" : "FAILED");
    s_dump_golden = ok;
  }
#endif
}

void set_golden(std::string* out) {
  s_golden = out;
}

#if !defined(DK_HOST_REPLAY)
void arm_replay_dump(const char* dir, int frames) {
  snprintf(s_dump_dir, sizeof(s_dump_dir), "%s", dir);
  s_dump_wait = frames;
  switch_run_logf("[dk] replay dump armed: %s after %d tfrag frames", dir, frames);
}
#endif

void build_frame(Loader* loader) {
  s_frame.cams.clear();
  s_frame.draws.clear();
  s_frame.tris = 0;
  s_frame.dropped = 0;
  s_frame.alpha_draws = 0;
  memcpy(s_frame.fog_rgba, s_fog, 4);
  bool any = false;
  for (const auto& c : s_cap) {
    any = any || c.valid;
  }
  if ((DKTF_NEED_LOADER && loader == nullptr) || !any) {
    return;
  }
#if !defined(DK_HOST_REPLAY)
  std::string golden;
  if (s_dump_golden) {
    s_golden = &golden;
  }
#endif
  if (s_golden) {
    gold("dktf-golden v1");
    gold("fog %u %u %u %u", s_fog[0], s_fog[1], s_fog[2], s_fog[3]);
  }
  s_build++;
  IdxSlot* ring = ring_begin();
  static std::vector<u8> vis;
  static std::vector<std::pair<int, int>> groups;
  static std::vector<u32> idx;

  for (int b = 0; b < kNumPasses; b++) {
    if (!s_cap[b].valid) {
      continue;
    }
    const auto& cam = s_cap[b].pc.camera;
    const LevelData* ld = DKTF_LEVEL(loader, s_cap[b].pc.level_name);
    if (ld == nullptr || !ld->level) {
      if (s_golden) {
        gold("pass %d level %s missing", b, s_cap[b].pc.level_name);
      }
      continue;
    }
    const uint8_t cam_idx = (uint8_t)s_frame.cams.size();
    CamParams cp;
    memcpy(cp.camera, cam.camera, sizeof(cp.camera));
    const auto pcm = make_new_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
    memcpy(cp.pc_camera, pcm.data(), sizeof(cp.pc_camera));
    cp.hvdf_off = cam.hvdf_off;
    cp.trans = cam.trans;
    cp.fog = cam.fog;
    s_frame.cams.push_back(cp);
    if (s_golden) {
      gold("pass %d bucket %d level %s cam %u", b, kJak1Passes[b].bucket, s_cap[b].pc.level_name,
           cam_idx);
      gold_vec("camera", cp.camera, 4);
      gold_vec("pc_camera", cp.pc_camera, 4);
      gold_vec("hvdf_trans_fog", &cp.hvdf_off, 3);
    }

    const int geom = 0;  // jak1 lod() == 0
    const auto& trees = ld->level->tfrag_trees[geom];
    for (size_t t = 0; t < trees.size(); t++) {
      const auto& tree = trees[t];
      if (tree.kind != kJak1Passes[b].k0 && tree.kind != kJak1Passes[b].k1) {
        continue;
      }
      if (t >= ld->tfrag_vertex_data[geom].size()) {
        continue;
      }
      const dkpool::PoolBuffer* vb = dk_loader_buffer_find(ld->tfrag_vertex_data[geom][t]);
      if (vb == nullptr || tree.unpacked.indices.empty()) {
        if (s_golden) {
          gold("skip pass %d tree %zu %s", b, t, vb ? "no-indices" : "no-vertex-buffer");
        }
        continue;
      }
      const uint32_t tod = tod_for(ld, tree, cam.itimes);
      if (tod == 0) {
        if (s_golden) {
          gold("skip pass %d tree %zu no-tod", b, t);
        }
        continue;
      }
      if (vis.size() < tree.bvh.vis_nodes.size()) {
        vis.resize(tree.bvh.vis_nodes.size());
      }
      cull_check_all_slow(cam.planes, tree.bvh.vis_nodes, nullptr, vis.data());
      groups.resize(tree.draws.size());
      idx.resize(tree.unpacked.indices.size());
      u32 tris = 0;
      const u32 n = make_index_list_from_vis_string(groups.data(), idx.data(), tree.draws, vis,
                                                    tree.unpacked.indices.data(), &tris);
      const uint32_t bytes = n * 4;
      if (s_golden) {
        gold("tree pass %d t %zu kind %d idx %u tris %u hash %08x", b, t, (int)tree.kind, n, tris,
             fnv1a(idx.data(), bytes));
      }
      ring->want += bytes;
      if (!ring->pb.ok() || ring->used + bytes > ring->pb.capacity) {
        s_frame.dropped += (uint32_t)tree.draws.size();
        if (s_golden) {
          gold("drop pass %d tree %zu ring-full", b, t);
        }
        continue;  // ring_begin grows the slot next time this slot comes round
      }
      const uint32_t base = ring->used;
      memcpy(ring->pb.cpu + base, idx.data(), bytes);
      ring->used += (bytes + 255) & ~255u;
      s_frame.tris += tris;

      for (size_t i = 0; i < tree.draws.size(); i++) {
        if (groups[i].second == 0) {
          continue;
        }
        if (s_frame.draws.size() >= kMaxDraws) {
          s_frame.dropped++;
          continue;
        }
        const auto& draw = tree.draws[i];
        DrawCmd c;
        c.vert_gpu = vb->gpu;
        c.vert_bytes = vb->capacity;
        c.idx_gpu = ring->pb.gpu + base + (uint64_t)groups[i].first * 4;
        c.idx_count = (uint32_t)groups[i].second;
        if (draw.tree_tex_id >= 0 && (size_t)draw.tree_tex_id < ld->textures.size()) {
          c.tex_id = ld->textures[draw.tree_tex_id];
        }
        c.tod_id = tod;
        c.strips = tree.use_strips;
        c.cam = cam_idx;
        fill_state(draw.mode, &c);
        s_frame.draws.push_back(c);
        if (s_golden) {
          gold("draw pass %d t %zu i %zu tex %d ofs %u n %u strips %d st %d%d%d%d z %d/%u/%d "
               "blend %u amin %.6g afail %d %.6g",
               b, t, i, draw.tree_tex_id, base + groups[i].first * 4, c.idx_count, c.strips,
               c.clamp_s, c.clamp_t, c.filt, c.decal, c.depth_test, c.depth_op, c.depth_write,
               c.blend, c.alpha_min, c.afail_second, c.aref_second);
        }
        if (b >= 2) {
          s_frame.alpha_draws++;
        }
      }
    }
  }
  ring->want = std::max(ring->want, ring->used);
  tod_evict_unused();
  if (s_golden) {
    gold("sum draws %zu tris %u dropped %u alpha %u", s_frame.draws.size(), s_frame.tris,
         s_frame.dropped, s_frame.alpha_draws);
  }
#if !defined(DK_HOST_REPLAY)
  if (s_dump_golden) {
    s_dump_golden = false;
    s_golden = nullptr;
    char path[128];
    snprintf(path, sizeof(path), "%s/dk_replay.golden.txt", s_dump_dir);
    const bool ok = write_file(path, golden.data(), golden.size());
    switch_run_logf("[dk] replay golden: %s %zu bytes, %zu draws %s", path, golden.size(),
                    s_frame.draws.size(), ok ? "ok" : "FAILED");
  }
#endif
}

}  // namespace dktf
#endif  // __SWITCH__ || DK_HOST_REPLAY
