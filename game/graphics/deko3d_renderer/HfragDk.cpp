#include "game/graphics/deko3d_renderer/HfragDk.h"

// T113 (AI-assisted): deko3d Hfrag port -- the distant mountain terrain (jak1
// bucket 8). Port of GL background/Hfrag.cpp: capture reads the bucket's
// TfragPcPortData packets, build_frame resolves levels + culls corners +
// updates the tod palette, record_world renders the 17 montage textures (wang
// tree -> 128x128 RTs) and then the visible corner strips. The level's
// vertex/index buffers are already uploaded by the shared HfragLoaderStage.
// Occlusion culling is skipped (frustum only), same as the dk tfrag port.
//
// FIX 117 (AI-assisted): also compiled on the host with DK_HOST_REPLAY for
// test/test_dk_hfrag_replay.cpp. The first hardware runs loaded no level at all
// (no "[dk] hfrag level" line in gk_run_log.txt), so the CPU half now counts
// every early-out (diag(), "[dk] hfrag cap" line) and the once-silent
// get_tfrag3_level miss is counted and named.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/custom_data/Tfrag3Data.h"

#if defined(DK_HOST_REPLAY)
#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"  // no-ops switch_run_logf on the host
const dkpool::PoolBuffer* dk_loader_buffer_find(GLuint id);  // test-defined
const LevelData* dktf_host_level(const char* name);          // test-defined
#define DKHF_LEVEL(loader, name) dktf_host_level(name.c_str())
#define DKHF_NEED_LOADER 0
#else
#include <deko3d.h>

#include "game/graphics/deko3d_renderer/PipelineTable.h"
#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/deko3d_renderer/dk_ring.h"
#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/dk_tfrag_ubo.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"
// switch_run_logf lives in run_log.h, which cannot be included next to <deko3d.h>.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define DKHF_LEVEL(loader, name) (loader)->get_tfrag3_level(name)
#define DKHF_NEED_LOADER 1
#endif

// Declared/defined in the dk buffer-pool seam (same as TFragmentDk.cpp /
// TieDk.cpp): loader GLuint handles -> pooled GPU buffers.
const dkpool::PoolBuffer* dk_loader_buffer_find(GLuint id);

// Defined in background_common.cpp, not declared in its header (same as TieDk).
std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z);

namespace {

// GL Hfrag.h constants.
constexpr int kNumBuckets = 17;      // GL kNumBuckets
constexpr int kNumCorners = 1024;    // GL kNumCorners
constexpr int kNumTiles = 16;        // GL kNumMontageTiles
constexpr int kIndsPerTile = 5;      // GL kIndsPerTile (4 + restart)
constexpr uint32_t kMontSize = 128;  // MontageTexture fb size
constexpr uint32_t kTodColors = 8192;
constexpr int kMaxLevels = 2;  // GL kMaxLevels
constexpr int kHfragBucketJak1 = 8;  // buckets.h HFRAG
constexpr uint32_t kUboSize = 0x100;
constexpr uint32_t kTexImages = 2048;  // dktexgpu descriptor slots (dk_tfrag_record)
constexpr uint32_t kIndsPerBucket = kIndsPerTile * kNumTiles;

// ---- capture: the bucket's TfragPcPortData packets ---------------------------
// Same reads as GL Hfrag::render: bucket NEXT, init nop, FLUSHA+DIRECT setup,
// nop, then one PC_PORT packet per active level, nop, ending.
struct CapLev {
  bool valid = false;
  TfragPcPortData pc;
};
CapLev s_cap[kMaxLevels];
uint32_t s_ncap = 0;

// FIX 117: capture/build diagnostics (dkhfrag::diag()) + the first few level
// names that failed the loader lookup, so one run log names the culprit.
dkhfrag::Diag s_diag;
char s_no_level_name[4][16] = {};
int s_no_level_names = 0;

bool is_dma_nop(const DmaTransfer& xfer) {
  return xfer.size_bytes == 0 && xfer.vifcode0().kind == VifCode::Kind::NOP &&
         xfer.vifcode1().kind == VifCode::Kind::NOP;
}

}  // namespace

namespace dkhfrag {

void capture_chain(const void* data, uint32_t start_offset) {
  s_ncap = 0;
  for (auto& c : s_cap) {
    c.valid = false;
  }
  if (data == nullptr) {
    return;
  }
  s_diag.calls++;
  DmaFollower d(data, start_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;  // not a game frame chain
  }
  const uint32_t buckets_base = d.current_tag_offset() + 16;
  const uint32_t slot = buckets_base + 16 * kHfragBucketJak1;
  const uint32_t next = slot + 16;
  DmaFollower b(data, slot);
  b.read_and_advance();  // the bucket's own NEXT (size 0)
  if (b.current_tag().kind == DmaTag::Kind::CALL) {
    s_diag.head_call++;
    return;  // renderer didn't run this frame
  }
  b.read_and_advance();  // init nop (GL is_dma_init_nop)
  if (b.current_tag_offset() == next) {
    s_diag.empty++;
    return;  // empty bucket
  }
  b.read_and_advance();  // FLUSHA + DIRECT bucket setup (10 * 16)
  b.read_and_advance();  // first nop
  int guard = 0;
  while (b.current_tag_offset() != next && ++guard < 64) {
    auto xfer = b.read_and_advance();
    if (is_dma_nop(xfer)) {
      break;  // ending
    }
    if (xfer.size_bytes != sizeof(TfragPcPortData)) {
      s_diag.walk_break++;  // unexpected -- keep whatever we got
      break;
    }
    if (s_ncap < kMaxLevels) {
      memcpy(&s_cap[s_ncap].pc, xfer.data, sizeof(TfragPcPortData));
      s_cap[s_ncap].pc.level_name[11] = '\0';
      s_cap[s_ncap].valid = true;
      s_ncap++;
      s_diag.packets++;
    }
  }
}

// ---- level cache (2 slots, GL kMaxLevels) -----------------------------------
namespace {

struct Level {
  bool in_use = false;
  std::string name;
  uint64_t load_id = 0;
  const tfrag3::Hfragment* hf = nullptr;
  uint64_t vert_gpu = 0;
  uint64_t vert_bytes = 0;
  uint64_t idx_gpu = 0;
  uint32_t wang_id = 0;      // dktexgpu id
  uint32_t tod_id = 0;       // dktexgpu id (8192 x 1)
  math::Vector<s32, 4> tod_itimes[4];
  bool tod_valid = false;
  uint32_t rt[kNumBuckets] = {};  // dktexgpu render-target ids
  bool rts_ok = false;
  uint64_t montage_vb = 0;  // 17 x 16 x 4 verts x 16 B
  uint64_t last_build = 0;
  // per-frame:
  bool cam_valid = false;
  bool bucket_used[kNumBuckets] = {};
  bool corner_vis[kNumCorners] = {};
  dktf::CamParams cam;
};

Level s_lev[kMaxLevels];
uint64_t s_build = 0;
uint64_t s_montage_ib = 0;  // shared: 17 buckets x 80 u32 (4 + restart per tile)
uint64_t s_stats_frames = 0, s_stats_draws = 0, s_stats_corners = 0, s_stats_misses = 0;
uint64_t s_stats_ubo_fail = 0, s_stats_levels = 0;

// Shared montage index buffer (GL m_montage_indices): one continuous vertex
// counter over every bucket's tiles, 4 indices + UINT32_MAX restart per tile.
void ensure_montage_indices() {
  if (s_montage_ib != 0) {
    return;
  }
  static uint32_t idx[kNumBuckets * kIndsPerBucket];
  uint32_t vi = 0;
  for (int bi = 0; bi < kNumBuckets; bi++) {
    for (int t = 0; t < kNumTiles; t++) {
      idx[bi * kIndsPerBucket + t * kIndsPerTile + 0] = vi++;
      idx[bi * kIndsPerBucket + t * kIndsPerTile + 1] = vi++;
      idx[bi * kIndsPerBucket + t * kIndsPerTile + 2] = vi++;
      idx[bi * kIndsPerBucket + t * kIndsPerTile + 3] = vi++;
      idx[bi * kIndsPerBucket + t * kIndsPerTile + 4] = UINT32_MAX;
    }
  }
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  if (pool == nullptr) {
    return;
  }
  const dkpool::PoolBuffer pb = pool->acquire(sizeof(idx));
  if (!pb.ok() || !dkdev::copy_to_gpu(pb.gpu, idx, sizeof(idx)).ok) {
    switch_run_logf("[dk] hfrag montage indices %u B FAILED", (unsigned)sizeof(idx));
    return;
  }
  s_montage_ib = pb.gpu;
}

bool load_level(Level* L, const LevelData* ld, const std::string& name) {
  const tfrag3::Hfragment* hf = &ld->level->hfrag;
  if (hf->buckets.size() != (size_t)kNumBuckets || hf->corners.size() > (size_t)kNumCorners ||
      hf->vertices.empty() || hf->indices.empty()) {
    s_diag.bad_data++;  // FIX 117
    switch_run_logf("[dk] hfrag level %s: bad data (buckets %zu corners %zu)", name.c_str(),
                    hf->buckets.size(), hf->corners.size());
    return false;
  }
  const dkpool::PoolBuffer* vb = dk_loader_buffer_find(ld->hfrag_vertices);
  const dkpool::PoolBuffer* ib = dk_loader_buffer_find(ld->hfrag_indices);
  if (vb == nullptr || ib == nullptr) {
    s_diag.no_buffers++;  // FIX 117
    switch_run_logf("[dk] hfrag level %s: loader buffers missing", name.c_str());
    return false;
  }
  if (hf->wang_tree_tex_id[0] < 0 || (size_t)hf->wang_tree_tex_id[0] >= ld->textures.size()) {
    switch_run_logf("[dk] hfrag level %s: wang tex id %d out of range (%zu)", name.c_str(),
                    hf->wang_tree_tex_id[0], ld->textures.size());
    return false;
  }
  ensure_montage_indices();
  if (s_montage_ib == 0) {
    return false;
  }
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  if (pool == nullptr) {
    return false;
  }
  // Montage vertex buffer (GL load_hfrag_level's montage generation).
  struct MontageVertex {
    float x, y, u, v;
  };
  static MontageVertex mont[kNumBuckets * kNumTiles * 4];
  for (int bucket_idx = 0; bucket_idx < kNumBuckets; bucket_idx++) {
    for (int tile_idx = 0; tile_idx < kNumTiles; tile_idx++) {
      MontageVertex* tile = &mont[(bucket_idx * kNumTiles + tile_idx) * 4];
      const int tx = tile_idx % 4;
      const int ty = tile_idx / 4;
      tile[0].x = tx * 0.25f;
      tile[1].x = tx * 0.25f + 0.25f;
      tile[2].x = tx * 0.25f;
      tile[3].x = tx * 0.25f + 0.25f;
      tile[0].y = ty * 0.25f;
      tile[1].y = ty * 0.25f;
      tile[2].y = ty * 0.25f + 0.25f;
      tile[3].y = ty * 0.25f + 0.25f;
      const int montage_idx = hf->buckets[bucket_idx].montage_table[tile_idx];
      const int mx = montage_idx & 15;
      const int my = montage_idx / 16;
      tile[0].u = mx * 0.0625f;
      tile[1].u = mx * 0.0625f + 0.0625f;
      tile[2].u = mx * 0.0625f;
      tile[3].u = mx * 0.0625f + 0.0625f;
      tile[0].v = my * 0.125f;
      tile[1].v = my * 0.125f;
      tile[2].v = my * 0.125f + 0.125f;
      tile[3].v = my * 0.125f + 0.125f;
    }
  }
  const uint32_t mont_bytes = kNumBuckets * kNumTiles * 4 * (uint32_t)sizeof(MontageVertex);
  const dkpool::PoolBuffer mvb = pool->acquire(mont_bytes);
  if (!mvb.ok() || !dkdev::copy_to_gpu(mvb.gpu, mont, mont_bytes).ok) {
    switch_run_logf("[dk] hfrag level %s: montage verts FAILED", name.c_str());
    return false;
  }
  L->in_use = true;
  L->name = name;
  L->load_id = ld->load_id;
  L->hf = hf;
  L->vert_gpu = vb->gpu;
  L->vert_bytes = (uint64_t)hf->vertices.size() * sizeof(tfrag3::HfragmentVertex);
  L->idx_gpu = ib->gpu;
  L->wang_id = ld->textures[hf->wang_tree_tex_id[0]];
  L->montage_vb = mvb.gpu;
  L->tod_valid = false;
  // Render targets + tod palette are per SLOT and never freed (image memory
  // never comes back, P8): reused when a slot reloads.
  if (!L->rts_ok) {
    bool ok = true;
    for (int i = 0; i < kNumBuckets; i++) {
      if (L->rt[i] == 0) {
        L->rt[i] = dktexgpu::create_render_target(kMontSize, kMontSize);
      }
      ok = ok && L->rt[i] != 0;
    }
    L->rts_ok = ok;
    if (!ok) {
      switch_run_logf("[dk] hfrag level %s: render targets FAILED", name.c_str());
    }
  }
  if (L->tod_id == 0) {
    static uint8_t zeros[kTodColors * 4] = {};
    L->tod_id = dktexgpu::upload_rgba8(kTodColors, 1, zeros);
    if (L->tod_id == 0) {
      switch_run_logf("[dk] hfrag level %s: tod texture FAILED", name.c_str());
    }
  }
  switch_run_logf("[dk] hfrag level %s loaded: verts %zu corners %zu", name.c_str(),
                  hf->vertices.size(), hf->corners.size());
  return L->rts_ok && L->tod_id != 0;
}

bool itimes_eq(const math::Vector<s32, 4> a[4], const math::Vector<s32, 4> b[4]) {
  return memcmp(a, b, sizeof(math::Vector<s32, 4>) * 4) == 0;
}

}  // namespace

void build_frame(Loader* loader) {
  for (auto& Lv : s_lev) {
    Lv.cam_valid = false;
  }
  if (DKHF_NEED_LOADER && loader == nullptr) {
    return;
  }
  if (s_ncap == 0) {
    return;
  }
  s_build++;
  // FIX 117: one summary line per 600 builds (~10 s at 60 fps) covering every
  // early-out that used to be silent (see diag()).
  if (s_build == 1 || s_build % 600 == 0) {
    switch_run_logf(
        "[dk] hfrag cap %llu | pkts %llu head-call %llu empty %llu break %llu | "
        "no-level %llu load-fail %llu ok %llu",
        (unsigned long long)s_build, (unsigned long long)s_diag.packets,
        (unsigned long long)s_diag.head_call, (unsigned long long)s_diag.empty,
        (unsigned long long)s_diag.walk_break, (unsigned long long)s_diag.no_level,
        (unsigned long long)s_diag.load_fail, (unsigned long long)s_diag.level_ok);
  }
  for (uint32_t c = 0; c < s_ncap; c++) {
    std::string name = s_cap[c].pc.level_name;
    if (name == "desert-vis") {  // GL's rename
      name = "dst";
    }
    const LevelData* ld = DKHF_LEVEL(loader, name);
    if (ld == nullptr || !ld->level) {
      // FIX 117: this was silent on hardware -- count it and name it once.
      s_diag.no_level++;
      bool known = false;
      for (int i = 0; i < s_no_level_names; i++) {
        if (name == s_no_level_name[i]) {
          known = true;
          break;
        }
      }
      if (!known && s_no_level_names < 4) {
        snprintf(s_no_level_name[s_no_level_names], sizeof(s_no_level_name[0]), "%.15s",
                 name.c_str());
        s_no_level_names++;
        switch_run_logf("[dk] hfrag: level '%s' not loaded (loader miss)", name.c_str());
      }
      continue;
    }
    // Resolve a slot (GL get_hfrag_level): reuse by name+load_id, else a free
    // slot, else the least recently used one.
    Level* L = nullptr;
    for (auto& cand : s_lev) {
      if (cand.in_use && cand.name == name && cand.load_id == ld->load_id) {
        L = &cand;
        break;
      }
    }
    if (L == nullptr) {
      for (auto& cand : s_lev) {
        if (!cand.in_use) {
          L = &cand;
          break;
        }
      }
    }
    if (L == nullptr) {
      L = &s_lev[0];
      for (auto& cand : s_lev) {
        if (cand.last_build < L->last_build) {
          L = &cand;
        }
      }
    }
    if (!L->in_use || L->name != name || L->load_id != ld->load_id) {
      if (!load_level(L, ld, name)) {
        s_diag.load_fail++;
        L->in_use = false;
        continue;
      }
      s_stats_levels++;
    }
    s_diag.level_ok++;
    L->last_build = s_build;
    // Visibility: frustum only (GL also applies the occlusion string; the dk
    // tfrag port skips occlusion the same way).
    for (auto& b : L->bucket_used) {
      b = false;
    }
    uint32_t vis_count = 0, used_count = 0;
    for (size_t bucket_idx = 1; bucket_idx < L->hf->buckets.size(); bucket_idx++) {
      for (u32 corner_idx : L->hf->buckets[bucket_idx].corners) {
        if (corner_idx >= L->hf->corners.size() || corner_idx >= (size_t)kNumCorners) {
          continue;
        }
        const bool vis =
            sphere_in_view_ref(L->hf->corners[corner_idx].bsphere, s_cap[c].pc.camera.planes);
        L->corner_vis[corner_idx] = vis;
        if (vis) {
          L->bucket_used[bucket_idx] = true;
          vis_count++;
        }
      }
    }
    for (bool used : L->bucket_used) {
      if (used) {
        used_count++;
      }
    }
    s_diag.vis_corners = vis_count;
    s_diag.used_buckets = used_count;
    // Camera (same inputs as the tfrag build).
    const auto& cam = s_cap[c].pc.camera;
    memcpy(L->cam.camera, cam.camera, sizeof(L->cam.camera));
    const auto pcm = make_new_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
    memcpy(L->cam.pc_camera, pcm.data(), sizeof(L->cam.pc_camera));
    L->cam.hvdf_off = cam.hvdf_off;
    L->cam.trans = cam.trans;
    L->cam.fog = cam.fog;
    L->cam_valid = true;
    // Time-of-day palette (deferred upload; recorded by the world recorder).
    if (L->tod_id != 0 && (!L->tod_valid || !itimes_eq(L->tod_itimes, cam.itimes))) {
      static std::vector<math::Vector<u8, 4>> colors;
      colors.resize(L->hf->time_of_day_colors.color_count);
      interp_time_of_day(cam.itimes, L->hf->time_of_day_colors, colors.data());
      if (dktexgpu::update_rgba8_deferred(L->tod_id, L->hf->time_of_day_colors.color_count, 1,
                                          colors.data())) {
        memcpy(L->tod_itimes, cam.itimes, sizeof(L->tod_itimes));
        L->tod_valid = true;
      }
    }
  }
}

// ---- GPU half: Switch only (DK_HOST_REPLAY links just the CPU half above) ----
#if defined(__SWITCH__)

// GL's montage state: no depth test, no blend, no cull.
void record_montage_state(DkCmdBuf cb) {
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  dkCmdBufBindDepthStencilState(cb, &ds);
  DkColorState cs;
  dkColorStateDefaults(&cs);
  dkCmdBufBindColorState(cb, &cs);
  DkBlendState bs;
  dkBlendStateDefaults(&bs);
  dkCmdBufBindBlendStates(cb, 0, &bs, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  dkCmdBufBindRasterizerState(cb, &rs);
}

// HfragmentVertex (16 B): height f32 @0, vi u32 @4, color_index u16 @8,
// u,v u8 x2 @10 (shader locations 0..3, GL load_hfrag_level attrib order).
const DkVtxAttribState kHfragAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
    {0, 0, 8, DkVtxAttribSize_1x16, DkVtxAttribType_Uint, 0},
    {0, 0, 10, DkVtxAttribSize_2x8, DkVtxAttribType_Uint, 0},
    {0, 0, 4, DkVtxAttribSize_1x32, DkVtxAttribType_Uint, 0},
};
const DkVtxAttribState kMontageAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    {0, 0, 8, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
};

void record_world(int fb_w, int fb_h) {
  if (!dkdev::hfrag_shaders_loaded() || !dkdev::hfrag_montage_shaders_loaded()) {
    return;
  }
  bool any = false;
  for (auto& Lv : s_lev) {
    any = any || (Lv.in_use && Lv.cam_valid);
  }
  if (!any) {
    return;
  }
  dkring::FrameRing* ring = dkdev::ubo_ring();
  if (ring == nullptr || ring->segment_size() == 0) {
    return;
  }
  DkCmdBuf cb = dkdev::cmd_handle();
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());

  // ---- montage pass: wang tree -> 128x128 RT per used bucket -----------------
  uint8_t samp = 0;
  if (!dkpipe::find_sampler(false, false, true, false, &samp)) {  // REPEAT + LINEAR
    s_stats_misses++;
    return;
  }
  {
    DkShader vsh = dkdev::hfrag_montage_vsh();
    DkShader fsh = dkdev::hfrag_montage_fsh();
    DkShader const* sh[] = {&vsh, &fsh};
    dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
    const DkVtxBufferState vbuf = {16, 0};
    dkCmdBufBindVtxAttribState(cb, kMontageAttribs, 2);
    dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
    dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX);
    record_montage_state(cb);
    DkViewport vp = {0.0f, 0.0f, (float)kMontSize, (float)kMontSize, 0.0f, 1.0f};
    DkScissor sc = {0, 0, kMontSize, kMontSize};
    bool first = true;
    for (auto& Lv : s_lev) {
      Level* L = &Lv;
      if (!L->in_use || !L->cam_valid || L->montage_vb == 0) {
        continue;
      }
      const uint32_t wang_slot = dktexgpu::slot_of(L->wang_id);
      if (wang_slot == UINT32_MAX) {
        s_stats_misses++;
        continue;
      }
      if (first) {
        dkCmdBufBindVtxBuffer(cb, 0, L->montage_vb, kNumBuckets * kNumTiles * 4 * 16);
        dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, s_montage_ib);
        first = false;
      } else {
        dkCmdBufBindVtxBuffer(cb, 0, L->montage_vb, kNumBuckets * kNumTiles * 4 * 16);
      }
      dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(wang_slot, samp));
      for (int bi = 1; bi < kNumBuckets; bi++) {  // bucket 0 is not drawn (GL)
        if (!L->bucket_used[bi] || L->rt[bi] == 0) {
          continue;
        }
        const DkImage* img = (const DkImage*)dktexgpu::image_of(L->rt[bi]);
        if (img == nullptr) {
          continue;
        }
        DkImageView view;
        dkImageViewDefaults(&view, img);
        dkCmdBufBindRenderTarget(cb, &view, nullptr);
        dkCmdBufSetViewports(cb, 0, &vp, 1);
        dkCmdBufSetScissors(cb, 0, &sc, 1);
        dkCmdBufDrawIndexed(cb, DkPrimitive_TriangleStrip, kIndsPerBucket, 1,
                            bi * kIndsPerBucket, 0, 0);
      }
    }
    // The main pass samples the RTs this pass just wrote (same barrier as the
    // ocean texture pass / eye pass).
    dkCmdBufBarrier(cb, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    dkdev::rebind_frame_target();
  }

  // ---- main pass: the visible corner strips ----------------------------------
  const uint8_t* fog = dktf::frame_draws().fog_rgba;
  {
    DkShader vsh = dkdev::hfrag_vsh();
    DkShader fsh = dkdev::hfrag_fsh();
    DkShader const* sh[] = {&vsh, &fsh};
    dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
    const DkVtxBufferState vbuf = {16, 0};
    dkCmdBufBindVtxAttribState(cb, kHfragAttribs, 4);
    dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
    dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX);
    DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
    DkScissor sc = {0, 0, (uint32_t)fb_w, (uint32_t)fb_h};
    dkCmdBufSetViewports(cb, 0, &vp, 1);
    dkCmdBufSetScissors(cb, 0, &sc, 1);

    s_stats_frames++;
    uint32_t draws = 0, corners = 0;
    for (auto& Lv : s_lev) {
      Level* L = &Lv;
      if (!L->in_use || !L->cam_valid) {
        continue;
      }
      const uint32_t tod_slot = dktexgpu::slot_of(L->tod_id);
      if (tod_slot == UINT32_MAX) {
        s_stats_misses++;
        continue;
      }
      // Draw mode -> pipeline state (GL setup_opengl_from_draw_mode, mipmap off).
      dktf::DrawCmd c = {};
      dktf::fill_draw_state(L->hf->draw_mode, &c);
      dkpipe::DrawModeAxes ax;
      ax.depth_test = c.depth_test;
      ax.depth_op = c.depth_test ? (dkpipe::Compare)c.depth_op : dkpipe::Compare::Always;
      ax.depth_write = c.depth_write;
      ax.blend = (dkpipe::Blend)c.blend;
      ax.clamp_s = c.clamp_s;
      ax.clamp_t = c.clamp_t;
      ax.filt = c.filt;
      ax.mipmap = false;  // GL setup_opengl_from_draw_mode(..., false)
      uint8_t msamp = 0;
      const dkpipe::PipelineDesc* desc = dkpipe::find_for_draw_mode(ax, &msamp);
      if (desc == nullptr) {
        s_stats_misses++;
        continue;
      }
      // The montage textures use REPEAT + LINEAR regardless of the draw mode
      // (GL sets both explicitly before drawing).
      uint8_t tsamp = 0;
      if (!dkpipe::find_sampler(false, false, true, false, &tsamp)) {
        s_stats_misses++;
        continue;
      }
      // Per-level UBOs: camera (vertex) + fog (fragment).
      dkring::Alloc va = ring->alloc(kUboSize, 0x100);
      dkring::Alloc fa = ring->alloc(kUboSize, 0x100);
      if (!va.ok() || !fa.ok()) {
        s_stats_ubo_fail++;
        continue;
      }
      dktf::VertParams vp0;
      dktf::fill_vert(&vp0, L->cam.camera, L->cam.pc_camera, L->cam.hvdf_off, L->cam.trans,
                      L->cam.fog, false);
      memcpy(va.cpu, &vp0, sizeof(vp0));
      dktf::FragParams fp0;
      math::Vector<uint8_t, 4> fogv(fog[0], fog[1], fog[2], fog[3]);
      dktf::fill_frag(&fp0, -dktf::kAlphaMaxNormal, dktf::kAlphaMaxNormal, fogv, 1.f, false);
      memcpy(fa.cpu, &fp0, sizeof(fp0));

      dkpipe::record_graphics_state(cb, *desc);
      dkCmdBufBindVtxBuffer(cb, 0, L->vert_gpu, L->vert_bytes);
      dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, L->idx_gpu);
      dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 7, va.gpu, kUboSize);
      dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 7, fa.gpu, kUboSize);
      dkCmdBufBindTexture(cb, DkStage_Vertex, 0, dkMakeTextureHandle(tod_slot, 0));
      for (int bi = 1; bi < kNumBuckets; bi++) {
        if (!L->bucket_used[bi] || L->rt[bi] == 0) {
          continue;
        }
        const uint32_t mont_slot = dktexgpu::slot_of(L->rt[bi]);
        if (mont_slot == UINT32_MAX) {
          s_stats_misses++;
          continue;
        }
        dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(mont_slot, tsamp));
        for (u32 corner_idx : L->hf->buckets[bi].corners) {
          if (corner_idx >= L->hf->corners.size() || !L->corner_vis[corner_idx]) {
            continue;
          }
          const auto& corner = L->hf->corners[corner_idx];
          dkCmdBufDrawIndexed(cb, DkPrimitive_TriangleStrip, corner.index_length, 1,
                              corner.index_start, 0, 0);
          draws++;
          corners++;
        }
      }
    }
    s_stats_draws += draws;
    s_stats_corners += corners;
    if (s_stats_frames == 1 || s_stats_frames % 600 == 0) {
      switch_run_logf("[dk] hfrag frame %llu: levels %llu draws %llu corners %llu misses %llu "
                      "ubo_fail %llu",
                      (unsigned long long)s_stats_frames, (unsigned long long)s_stats_levels,
                      (unsigned long long)draws, (unsigned long long)corners,
                      (unsigned long long)s_stats_misses, (unsigned long long)s_stats_ubo_fail);
    }
  }
}

#endif  // __SWITCH__

// FIX 117: diagnostics accessor (host replay test + "[dk] hfrag cap" line).
const Diag& diag() {
  return s_diag;
}

// FIX 117: test hook -- see HfragDk.h. Appends one packet to this frame's capture.
void capture_packet(const TfragPcPortData* pc) {
  if (pc == nullptr || s_ncap >= kMaxLevels) {
    return;
  }
  memcpy(&s_cap[s_ncap].pc, pc, sizeof(TfragPcPortData));
  s_cap[s_ncap].pc.level_name[11] = '\0';
  s_cap[s_ncap].valid = true;
  s_ncap++;
  s_diag.packets++;
}

}  // namespace dkhfrag

#endif  // __SWITCH__ || DK_HOST_REPLAY

