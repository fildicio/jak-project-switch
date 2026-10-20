#include "game/graphics/deko3d_renderer/TieDk.h"

// T73/T74 (AI-assisted): deko3d tie CPU half, cloned from ShrubDk.cpp. Port of GL
// Tie3::set_up_common_data_from_dma + setup_tree for jak1 (frustum culling, NORMAL category).
// DK_HOST_REPLAY: test/test_dk_tie_replay.cpp compiles this file on the Mac.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/dma/dma_chain_read.h"

#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/deko3d_renderer/dk_tie3_prep.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

#if defined(DK_HOST_REPLAY)
const dkpool::PoolBuffer* dk_loader_buffer_find(GLuint id);  // test-defined
const LevelData* dktf_host_level(const char* name);          // test-defined
#define DKTI_LEVEL(loader, name) dktf_host_level(name)
#define DKTI_NEED_LOADER 0
#else
#define DKTI_LEVEL(loader, name) (loader)->get_tfrag3_level(name)
#define DKTI_NEED_LOADER 1
#endif

// Defined in background_common.cpp, not declared in its header (same as TFragmentDk.cpp).
std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z);

namespace {

constexpr uint32_t kMaxDraws = 1000;  // shares the frame cmd slice with tfrag + shrub
constexpr int kTodColors = 8192;      // Tie3 TIME_OF_DAY_COLOR_COUNT (same as tfrag)
constexpr int kIdxSlots = 3;          // index ring slots, same reason as TFragmentDk.cpp

constexpr uint32_t kMaxWindCams = 400;  // T76: 256 B vertex UBO each, 256 KB ring per frame

// T76: same layout as GL Tie3::WindWork (jak1 DMA wind block).
struct WindWorkDk {
  u32 paused;
  u32 pad[3];
  math::Vector4f wind_array[64];
  math::Vector4f wind_normal;
  math::Vector4f wind_temp;
  float wind_force[64];
  u32 wind_time;
  u32 pad2[3];
};
static_assert(sizeof(WindWorkDk) == 84 * 16);

struct Captured {
  TfragPcPortData pc;
  float envmap[4] = {0, 0, 0, 0};
  WindWorkDk wind;
  bool has_wind = false;
  bool valid = false;
};
Captured s_cap[dktie::kNumPasses];
dktf::FrameDraws s_frame;

// Same reads as GL Tie3::set_up_common_data_from_dma for jak1. true = the bucket had tie data.
bool walk_bucket(const void* data, uint32_t slot, uint32_t next, Captured* out) {
  DmaFollower d(data, slot);
  d.read_and_advance();  // the bucket's own NEXT (size 0)
  if (d.current_tag().kind == DmaTag::Kind::CALL) {
    return false;  // renderer didn't run this frame
  }
  if (d.current_tag_offset() == next) {
    return false;
  }
  auto gs_test = d.read_and_advance();
  if (gs_test.size_bytes != 160) {
    d.read_and_advance();  // tie consts (9 qw)
  }
  d.read_and_advance();  // mscalf
  d.read_and_advance();  // row
  auto nx = d.read_and_advance();
  if (nx.size_bytes == 32) {
    d.read_and_advance();
  }
  auto pc = d.read_and_advance();
  if (pc.size_bytes != sizeof(TfragPcPortData)) {
    return false;
  }
  memcpy(&out->pc, pc.data, sizeof(TfragPcPortData));
  out->pc.level_name[11] = '\0';
  auto wind = d.read_and_advance();
  out->has_wind = wind.size_bytes == sizeof(WindWorkDk);
  if (out->has_wind) {
    memcpy(&out->wind, wind.data, sizeof(WindWorkDk));
  }
  auto env = d.read_and_advance();
  if (env.size_bytes == 16) {
    memcpy(out->envmap, env.data, 16);
  }
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
    switch_run_logf("[dk] tie index ring slot %u: %u KB %s", s_build % kIdxSlots, need >> 10,
                    s->pb.ok() ? "ok" : "FAILED");
  }
  s->want = 0;
  return s;
}

// ---- time-of-day palettes (copy of TFragmentDk.cpp's, keyed by TieTree) ----
struct TodEntry {
  uint32_t id = 0;
  uint64_t load_id = 0;
  math::Vector<s32, 4> itimes[4];
  bool valid = false;
  uint32_t last_build = 0;
};
std::unordered_map<const tfrag3::TieTree*, TodEntry> s_tod;
std::vector<uint32_t> s_tod_free;
std::vector<math::Vector<u8, 4>> s_tod_colors;

uint32_t tod_for(const LevelData* ld,
                 const tfrag3::TieTree& tree,
                 const math::Vector<s32, 4>* it) {
  const auto& colors = tree.colors;
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

// ---- T76: wind (port of GL Tie3 do_wind_math / render_tree_wind) ----
// One wind vector array per pass, like each GL Tie3 instance. Kept across frames.
std::vector<float> s_wind_vec[dktie::kNumPasses];
std::string s_wind_level[dktie::kNumPasses];

void do_wind_math(u16 wind_idx,
                  float* wind_vector_data,
                  const WindWorkDk& wind_work,
                  float stiffness,
                  std::array<math::Vector4f, 4>& mat) {
  float* my_vector = wind_vector_data + (4 * wind_idx);
  const auto& work_vector = wind_work.wind_array[(wind_work.wind_time + wind_idx) & 63];
  constexpr float cx = 0.5;
  constexpr float cy = 100.0;
  constexpr float cz = 0.0166;
  constexpr float cw = -1.0;
  float vf18_x = my_vector[2];
  float vf18_z = my_vector[3];
  float vf17_x = my_vector[0];
  float vf17_z = my_vector[1];
  math::Vector4f vf16 = work_vector;
  vf16.x() -= cx * vf18_x + cy * vf17_x;
  vf16.z() -= cx * vf18_z + cy * vf17_z;
  math::Vector4f vf18(vf18_x, 0.f, vf18_z, 0.f);
  vf18 += vf16 * cz;
  math::Vector4f vf17(vf17_x, 0.f, vf17_z, 0.f);
  vf17 += vf18 * cz;
  math::Vector4f vf27;
  for (int i = 0; i < 4; i++) {
    vf17[i] = std::min(vf17[i], 1.f);   // vector_min_in_place(vf17, 1.f)
    vf27[i] = std::max(vf17[i], cw);    // vector_max(vf17, cw)
  }
  vf27 *= stiffness;
  mat[0].x() += vf27.x() * mat[0].y();
  mat[0].z() += vf27.z() * mat[0].y();
  if (!wind_work.paused) {
    my_vector[0] = vf27.x();
    my_vector[1] = vf27.z();
    my_vector[2] = vf18.x();
    my_vector[3] = vf18.z();
  }
  mat[1].x() += vf27.x() * mat[1].y();
  mat[1].z() += vf27.z() * mat[1].y();
  mat[2].x() += vf27.x() * mat[2].y();
  mat[2].z() += vf27.z() * mat[2].y();
}

}  // namespace

namespace dktie {

// tfrag3.vert draws position p as pc_camera * (p - cam_trans) (w of column 3 dropped).
// A wind instance draws local v at world W = m0*v.x + m1*v.y + m2*v.z + m3, so:
//   pc'[i] = sum_j pc[j] * m_i[j]  (i < 3),  pc'[3] = pc[3],
//   trans' solves m0*t.x + m1*t.y + m2*t.z = trans - m3  (Cramer, in double).
// Returns false if the instance matrix is singular.
bool wind_cam(const dktf::CamParams& base,
              const std::array<math::Vector4f, 4>& m,
              dktf::CamParams* out) {
  *out = base;
  for (int i = 0; i < 3; i++) {
    out->pc_camera[i] = base.pc_camera[0] * m[i].x() + base.pc_camera[1] * m[i].y() +
                        base.pc_camera[2] * m[i].z();
  }
  double a[3][3], r[3];
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      a[row][col] = m[col][row];
    }
    r[row] = (double)base.trans[row] - (double)m[3][row];
  }
  auto det3 = [](const double x[3][3]) {
    return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) -
           x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
           x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
  };
  const double d = det3(a);
  if (d == 0.0) {
    return false;
  }
  for (int k = 0; k < 3; k++) {
    double t[3][3];
    memcpy(t, a, sizeof(t));
    for (int row = 0; row < 3; row++) {
      t[row][k] = r[row];
    }
    out->trans[k] = (float)(det3(t) / d);
  }
  return true;
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
  for (int i = 0; i < kNumPasses; i++) {
    const uint32_t slot = buckets_base + 16 * kJak1Buckets[i];
    s_cap[i].valid = walk_bucket(data, slot, slot + 16, &s_cap[i]);
  }
}

const TfragPcPortData* captured_pc(int pass) {
  if (pass < 0 || pass >= kNumPasses || !s_cap[pass].valid) {
    return nullptr;
  }
  return &s_cap[pass].pc;
}

const float* captured_envmap(int pass) {
  if (pass < 0 || pass >= kNumPasses || !s_cap[pass].valid) {
    return nullptr;
  }
  return s_cap[pass].envmap;
}

const dktf::FrameDraws& frame_draws() {
  return s_frame;
}

void build_frame(Loader* loader) {
  s_frame.cams.clear();
  s_frame.draws.clear();
  s_frame.tris = 0;
  s_frame.dropped = 0;
  s_frame.alpha_draws = 0;
  s_frame.wind_draws = 0;
  s_frame.envmap_draws = 0;
  s_frame.etie.clear();
  bool any = false;
  for (const auto& c : s_cap) {
    any = any || c.valid;
  }
  if ((DKTI_NEED_LOADER && loader == nullptr) || !any) {
    return;
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
    const LevelData* ld = DKTI_LEVEL(loader, s_cap[b].pc.level_name);
    if (ld == nullptr || !ld->level) {
      continue;
    }
    const uint16_t cam_idx = (uint16_t)s_frame.cams.size();
    dktf::CamParams cp;
    memcpy(cp.camera, cam.camera, sizeof(cp.camera));
    const auto pcm = make_new_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
    memcpy(cp.pc_camera, pcm.data(), sizeof(cp.pc_camera));
    cp.hvdf_off = cam.hvdf_off;
    cp.trans = cam.trans;
    cp.fog = cam.fog;
    s_frame.cams.push_back(cp);

    // T77: envmap shine params (GL init_etie_cam_uniforms + envmap_color: /128, x2 on jak1,
    // x m_envmap_strength = 1).
    const int8_t etie_idx = (int8_t)s_frame.etie.size();
    {
      float tint[4];
      for (int k = 0; k < 4; k++) {
        tint[k] = s_cap[b].envmap[k] / 128.f * 2.f;
      }
      EtieParams ep;
      fill_etie(&ep, cam.rot[0].data(), cam.perspective[0].data(), cam.hvdf_off.data(),
                cam.fog.x(), tint);
      std::array<float, 28> raw;
      static_assert(sizeof(raw) == sizeof(ep));
      memcpy(raw.data(), &ep, sizeof(ep));
      s_frame.etie.push_back(raw);
    }

    const int geom = 0;  // Gfx::g_global_settings.lod_tie default

    // T76: wind draws of one tree (GL Tie3::render_tree_wind). vis = this tree's cull result.
    auto add_wind = [&](int pass, const LevelData* lev, int g, size_t ti,
                        const tfrag3::TieTree& tree, const dkpool::PoolBuffer* vb, uint32_t tod,
                        uint16_t base_cam) {
      const auto& wc = s_cap[pass].wind;
      if (!s_cap[pass].has_wind || tree.instanced_wind_draws.empty() ||
          tree.wind_instance_info.empty()) {
        return;
      }
      auto& wv = s_wind_vec[pass];
      if (s_wind_level[pass] != s_cap[pass].pc.level_name) {
        s_wind_level[pass] = s_cap[pass].pc.level_name;
        wv.clear();
      }
      u16 max_idx = 0;
      for (const auto& info : tree.wind_instance_info) {
        max_idx = std::max(max_idx, info.wind_idx);
      }
      if (wv.size() < 4u * max_idx + 4u) {
        wv.resize(4u * max_idx + 4u, 0.f);
      }
      // Like GL: wind math for every instance (it also steps the shared wind state).
      static std::vector<std::array<math::Vector4f, 4>> mats;
      static std::vector<int> inst_cam;
      mats.resize(tree.wind_instance_info.size());
      inst_cam.assign(tree.wind_instance_info.size(), -1);
      for (size_t i = 0; i < tree.wind_instance_info.size(); i++) {
        const auto& info = tree.wind_instance_info[i];
        mats[i] = info.matrix;
        do_wind_math(info.wind_idx, wv.data(), wc, info.stiffness, mats[i]);
      }
      const dkpool::PoolBuffer* wb = dk_loader_buffer_find(lev->tie_data[g][ti].wind_indices);
      uint32_t draw_base = 0;
      for (const auto& draw : tree.instanced_wind_draws) {
        uint32_t off = draw_base;
        draw_base += (uint32_t)draw.vertex_index_stream.size();
        for (const auto& grp : draw.instance_groups) {
          const uint32_t at = off;
          off += grp.num;
          if (grp.vis_idx >= vis.size() || !vis[grp.vis_idx] || grp.num == 0) {
            continue;
          }
          if (wb == nullptr || grp.instance_idx >= mats.size() ||
              s_frame.draws.size() >= kMaxDraws) {
            s_frame.dropped++;
            continue;
          }
          int& ci = inst_cam[grp.instance_idx];
          if (ci < 0) {
            dktf::CamParams cp;
            if (s_frame.cams.size() >= kMaxWindCams ||
                !dktie::wind_cam(s_frame.cams[base_cam], mats[grp.instance_idx], &cp)) {
              s_frame.dropped++;
              continue;
            }
            ci = (int)s_frame.cams.size();
            s_frame.cams.push_back(cp);
          }
          dktf::DrawCmd c;
          c.vert_gpu = vb->gpu;
          c.vert_bytes = vb->capacity;
          c.idx_gpu = wb->gpu + (uint64_t)at * 4;
          c.idx_count = grp.num;
          if (draw.tree_tex_id >= 0 && (size_t)draw.tree_tex_id < lev->textures.size()) {
            c.tex_id = lev->textures[draw.tree_tex_id];
          }
          c.tod_id = tod;
          c.strips = tree.use_strips;
          c.cam = (uint16_t)ci;
          dktf::fill_draw_state(draw.mode, &c);
          s_frame.draws.push_back(c);
          s_frame.wind_draws++;
        }
      }
    };
    const auto& trees = ld->level->tie_trees[geom];
    for (size_t t = 0; t < trees.size() && t < ld->tie_data[geom].size(); t++) {
      const auto& tree = trees[t];
      const dkpool::PoolBuffer* vb = dk_loader_buffer_find(ld->tie_data[geom][t].vertex_buffer);
      if (vb == nullptr || tree.unpacked.indices.empty()) {
        continue;
      }
      const uint32_t tod = tod_for(ld, tree, cam.itimes);
      if (tod == 0) {
        continue;
      }
      if (vis.size() < tree.bvh.vis_nodes.size()) {
        vis.resize(tree.bvh.vis_nodes.size());
      }
      cull_check_all_slow(cam.planes, tree.bvh.vis_nodes, nullptr, vis.data());
      groups.resize(tree.static_draws.size());
      idx.resize(tree.unpacked.indices.size());
      u32 tris = 0;
      const u32 n = make_index_list_from_vis_string(groups.data(), idx.data(), tree.static_draws,
                                                    vis, tree.unpacked.indices.data(), &tris);
      const uint32_t bytes = n * 4;
      ring->want += bytes;
      if (!ring->pb.ok() || ring->used + bytes > ring->pb.capacity) {
        s_frame.dropped += (uint32_t)tree.static_draws.size();
        continue;  // ring_begin grows the slot next time this slot comes round
      }
      const uint32_t base = ring->used;
      memcpy(ring->pb.cpu + base, idx.data(), bytes);
      ring->used += (bytes + 255) & ~255u;
      s_frame.tris += tris;

      // GL Tie3WithEnvmapJak1 order: NORMAL, wind, NORMAL_ENVMAP (tfrag3 shader), then the
      // shiny NORMAL_ENVMAP_SECOND_DRAW with the ETIE shader (T77, etie >= 0).
      auto add_category = [&](tfrag3::TieCategory cat, int8_t etie) {
      const size_t first = tree.category_draw_indices[(int)cat];
      const size_t last = tree.category_draw_indices[(int)cat + 1];
      for (size_t i = first; i < last && i < tree.static_draws.size(); i++) {
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
        if (draw.tree_tex_id >= 0 && (size_t)draw.tree_tex_id < ld->textures.size()) {
          c.tex_id = ld->textures[draw.tree_tex_id];
        }
        c.tod_id = tod;
        c.strips = tree.use_strips;
        c.cam = cam_idx;
        c.etie = etie;
        dktf::fill_draw_state(draw.mode, &c);
        s_frame.draws.push_back(c);
        if (etie >= 0) {
          s_frame.envmap_draws++;
        }
      }
      };
      add_category(tfrag3::TieCategory::NORMAL, -1);
      add_wind(b, ld, geom, t, tree, vb, tod, cam_idx);
      add_category(tfrag3::TieCategory::NORMAL_ENVMAP, -1);
      add_category(tfrag3::TieCategory::NORMAL_ENVMAP_SECOND_DRAW, etie_idx);
    }
  }
  ring->want = std::max(ring->want, ring->used);
  tod_evict_unused();
}

}  // namespace dktie
#endif  // __SWITCH__ || DK_HOST_REPLAY
