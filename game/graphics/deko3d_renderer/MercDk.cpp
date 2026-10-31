#include "game/graphics/deko3d_renderer/MercDk.h"

// T78 (AI-assisted): deko3d merc2 CPU half. Port of GL Merc2::handle_all_dma /
// handle_pc_model / alloc_normal_draw / flush_draw_buckets for jak1.
// DK_HOST_REPLAY: test/test_dk_merc_replay.cpp compiles this file on the Mac.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <cstring>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/versions/versions.h"  // §7.2 phase 1: set_game pick

#include "game/graphics/deko3d_renderer/EyeDk.h"
#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

#if defined(DK_HOST_REPLAY)
const dkpool::PoolBuffer* dk_loader_buffer_find(GLuint id);  // test-defined
bool dktf_host_merc(const char* name, MercRef* out);         // test-defined
#define DKME_NEED_LOADER 0
#else
#define DKME_NEED_LOADER 1
#endif

namespace {

// Merc2::MAX_LIGHTS (one light set per model per frame). Was 256: busy scenes (village,
// 256+ models) silently dropped every later model, e.g. Jak himself (AI-assisted).
constexpr uint32_t kMaxModels = 1024;
constexpr int kSlots = 3;             // ring slots, same reason as TieDk's index ring
constexpr uint32_t kMercMatBytes = 112;  // GL MercMat: tmat[4] + nmat[3]

// GL Merc2::LowMemory minus the two tags (hvdf_offset, perspective[4], fog).
struct LowMem {
  float hvdf[4];
  float persp[16];
  float fog[4];
};

struct CapModel {
  char name[128];
  float lights[28];  // GL VuLights (112 B)
  int bucket = 0;
  bool water = false;
  uint8_t bits = 0;  // PcMercFlags::bitflags
  uint64_t enable = 0, ignore_alpha = 0;
  uint32_t bone_first = 0;  // into s_bones (kMercMatBytes each)
  uint32_t bone_slots = 0;
};

std::vector<CapModel> s_models;
std::vector<uint8_t> s_bones;
// §7.2 phase 1: per-game bucket pick (defaults to jak1). The jak2 table is 40 buckets --
// unused today because jak2 merc2 runs through dkgl (DKGL_TODO §7.1); s_low is sized for
// it anyway so a future opt-in cannot overflow.
const int* s_buckets = dkmerc::kJak1Buckets;
int s_bucket_count = dkmerc::kNumBuckets;
LowMem s_low[dkmerc::kMaxBuckets];
uint32_t s_bad_bones = 0;
uint32_t s_cap_dropped = 0;  // models past kMaxModels this frame
dkmerc::FrameDraws s_frame;

// GL handle_pc_model, capture half. base = EE memory, t = the PC_PORT transfer.
void capture_model(const uint8_t* base, uint32_t mem_size, const DmaTransfer& t, int bucket) {
  const uint8_t* in = t.data;
  const uint8_t* end = t.data + t.size_bytes;
  if (s_models.size() >= kMaxModels) {
    s_cap_dropped++;
    return;
  }
  if (t.size_bytes < 128 + 112 + 16 + 128 + 32) {
    return;
  }
  CapModel m;
  memcpy(m.name, in, 128);
  m.name[127] = '\0';
  in += 128;
  memcpy(m.lights, in, sizeof(m.lights));
  in += 112;
  uint64_t water = 0;
  memcpy(&water, in, 8);  // jak1 only: uses_water
  m.water = water != 0;
  in += 16;
  const uint8_t* slots = in;
  const uint8_t* addrs = in + 128;
  int i = 0;
  uint8_t max_slot = 0;
  for (; i < 128 && slots[i] != 0xff; i++) {
    max_slot = std::max(max_slot, slots[i]);
  }
  if (addrs + 16 * i + 32 > end || max_slot >= dkmerc::kMaxBones) {
    return;
  }
  m.bucket = bucket;
  m.bone_first = (uint32_t)(s_bones.size() / kMercMatBytes);
  m.bone_slots = i > 0 ? max_slot + 1u : 0u;
  s_bones.resize(s_bones.size() + (size_t)m.bone_slots * kMercMatBytes, 0);
  uint8_t* bones = s_bones.data() + (size_t)m.bone_first * kMercMatBytes;
  for (int k = 0; k < i; k++) {
    uint32_t addr;
    memcpy(&addr, addrs + 16 * k, 4);  // GOAL address of the bone's MercMat
    if ((uint64_t)addr + kMercMatBytes > mem_size) {
      s_bad_bones++;
      continue;
    }
    memcpy(bones + (size_t)slots[k] * kMercMatBytes, base + addr, kMercMatBytes);
  }
  in += 128 + 16 * i;
  memcpy(&m.enable, in, 8);
  memcpy(&m.ignore_alpha, in + 8, 8);
  m.bits = in[17];
  s_models.push_back(m);
}

bool nothing_next(const DmaFollower& d) {
  return d.current_tag().kind == DmaTag::Kind::NEXT && d.current_tag().qwc == 0 &&
         d.current_tag_vif0() == 0 && d.current_tag_vif1() == 0;
}

// GL handle_all_dma + handle_setup_dma + handle_merc_chain for one jak1 merc bucket.
void walk_bucket(const uint8_t* base, uint32_t mem_size, uint32_t slot, uint32_t next, int b) {
  DmaFollower d(base, slot);
  d.read_and_advance();  // the bucket's own NEXT (size 0)
  if (d.current_tag().kind == DmaTag::Kind::CALL || d.current_tag_offset() == next) {
    return;  // renderer didn't run / nothing to draw
  }
  auto setup = d.read_and_advance();
  if (setup.size_bytes != 160) {
    return;
  }
  // LowMemory: 2 tags (32 B), hvdf_offset, perspective[4], fog.
  memcpy(&s_low[b], setup.data + 16 + 32, sizeof(LowMem));
  d.read_and_advance();  // jak1: 32 B test register
  d.read_and_advance();  // empty
  for (int guard = 0; guard < 100000 && !d.ended() && d.current_tag_offset() != next; guard++) {
    while (nothing_next(d) && d.current_tag_offset() != next) {
      d.read_and_advance();
    }
    if (d.current_tag_offset() == next) {
      break;
    }
    if (d.current_tag().kind == DmaTag::Kind::CALL) {
      for (int k = 0; k < 4 && !d.ended(); k++) {
        d.read_and_advance();
      }
      continue;
    }
    auto init = d.read_and_advance();
    while (init.vifcode1().kind == VifCode::Kind::PC_PORT && !d.ended()) {
      capture_model(base, mem_size, init, b);
      d.read_and_advance();  // jak1: 2 empty link tags
      d.read_and_advance();
      init = d.read_and_advance();
    }
    if (init.vifcode0().kind == VifCode::Kind::FLUSHA) {
      break;  // GL skips the (< 4) remaining tags up to next_bucket
    }
  }
}

// ---- per-frame GPU ring (params + bones + frag variants), 3 slots like TieDk ----
struct Slot {
  dkpool::PoolBuffer pb;
  uint32_t used = 0;
  uint32_t want = 0;
};
Slot s_slot[kSlots];
uint32_t s_build = 0;

Slot* slot_begin() {
  Slot* s = &s_slot[s_build % kSlots];
  s->used = 0;
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  const uint32_t need = std::max<uint32_t>(256u << 10, s->want + s->want / 2);
  if (pool && (!s->pb.ok() || s->pb.capacity < s->want)) {
    if (s->pb.ok()) {
      pool->release(s->pb.id);
    }
    s->pb = pool->acquire(need);
    switch_run_logf("[dk] merc ring slot %u: %u KB %s", s_build % kSlots, need >> 10,
                    s->pb.ok() ? "ok" : "FAILED");
  }
  s->want = 0;
  return s;
}

// 256-B aligned bytes from the slot; nullptr (and want grows) when full.
uint8_t* slot_alloc(Slot* s, uint32_t bytes, uint64_t* gpu) {
  bytes = (bytes + 0xff) & ~0xffu;
  s->want += bytes;  // total asked this build: slot_begin grows the slot to fit next time
  if (!s->pb.ok() || s->used + bytes > s->pb.capacity) {
    return nullptr;
  }
  uint8_t* p = s->pb.cpu + s->used;
  *gpu = s->pb.gpu + s->used;
  s->used += bytes;
  return p;
}

bool find_model(Loader* loader, const char* name, MercRef* out) {
#if defined(DK_HOST_REPLAY)
  (void)loader;
  return dktf_host_merc(name, out);
#else
  auto r = loader->get_merc_model(name);
  if (!r) {
    return false;
  }
  *out = *r;
  return true;
#endif
}

void fill_vert(dkmerc::MercVertParams* p, const CapModel& m, const LowMem& low) {
  memset(p, 0, sizeof(*p));
  const float* l = m.lights;  // dir0+w0, dir1+w1, dir2+w2, col0, col1, col2, ambient
  memcpy(p->light_dir0_fade, l + 0, 12);
  p->light_dir0_fade[3] = 1.f;  // GL: fade = 1 when w1 == 0
  memcpy(p->light_dir1_fade_en, l + 4, 12);
  p->light_dir1_fade_en[3] = 0.f;  // jak1: GL forces w1 = 0 (merc fade off)
  memcpy(p->light_dir2, l + 8, 12);
  memcpy(p->light_col0, l + 12, 16);
  memcpy(p->light_col1, l + 16, 16);
  memcpy(p->light_col2, l + 20, 16);
  memcpy(p->light_ambient, l + 24, 16);
  memcpy(p->hvdf_offset, low.hvdf, 16);
  memcpy(p->fog_constants, low.fog, 16);
  memcpy(p->perspective, low.persp, 64);  // glUniformMatrix4fv(transpose = false)
}

}  // namespace

namespace dkmerc {

void capture_chain(const void* data, uint32_t start_offset, uint32_t mem_size) {
  s_models.clear();
  s_bones.clear();
  s_bad_bones = 0;
  s_cap_dropped = 0;
  if (data == nullptr) {
    return;
  }
  DmaFollower d(data, start_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;  // not a game frame chain
  }
  const uint32_t buckets_base = d.current_tag_offset() + 16;
  for (int b = 0; b < s_bucket_count; b++) {
    const uint32_t slot = buckets_base + 16 * s_buckets[b];
    walk_bucket((const uint8_t*)data, mem_size, slot, slot + 16, b);
  }
}

uint32_t captured_models() {
  return (uint32_t)s_models.size();
}

const char* captured_name(uint32_t i) {
  return i < s_models.size() ? s_models[i].name : nullptr;
}

int captured_bucket(uint32_t i) {
  return i < s_models.size() ? s_models[i].bucket : -1;
}

const FrameDraws& frame_draws() {
  return s_frame;
}

void build_frame(Loader* loader, const uint8_t fog_rgba[4]) {
  s_frame.draws.clear();
  s_frame.models = (uint32_t)s_models.size();
  s_frame.missing = s_frame.skipped = s_frame.mod = s_frame.tris = 0;
  s_frame.dropped = s_cap_dropped;
  s_frame.bad_bones = s_bad_bones;
  for (auto& g : s_frame.frag_gpu) {
    g = 0;
  }
  if ((DKME_NEED_LOADER && loader == nullptr) || s_models.empty()) {
    return;
  }
  s_build++;
  Slot* ring = slot_begin();

  // Fragment param variants (GL sets these per draw as uniforms).
  for (uint32_t v = 0; v < kFragVariants; v++) {
    uint64_t gpu = 0;
    uint8_t* p = slot_alloc(ring, sizeof(MercFragParams), &gpu);
    if (p == nullptr) {
      s_frame.dropped = s_frame.models;
      ring->want += s_frame.models * (kParamBytes + 32 * kBoneBytes);
      return;
    }
    MercFragParams f = {};
    const bool fog = v & 4;
    for (int c = 0; c < 3; c++) {
      f.fog_color[c] = fog_rgba[c] / 255.f;
    }
    f.fog_color[3] = fog ? 1.f / 255.f : 0.f;  // GL: fog_intensity (1.f) / 255
    f.ignore_alpha = v & 1;
    f.decal_enable = (v >> 1) & 1;
    f.no_tex = (v >> 3) & 1;
    f.fade = 1.f;
    f.fade_en = 0.f;
    memcpy(p, &f, sizeof(f));
    s_frame.frag_gpu[v] = gpu;
  }

  // GL flushes once per bucket renderer, grouping draws by level in first-seen order.
  static std::vector<const LevelData*> lev_order;
  static std::vector<std::vector<DrawCmd>> lev_draws;
  size_t mi = 0;
  for (int b = 0; b < s_bucket_count; b++) {
    lev_order.clear();
    for (auto& v : lev_draws) {
      v.clear();
    }
    for (; mi < s_models.size() && s_models[mi].bucket == b; mi++) {
      const CapModel& m = s_models[mi];
      MercRef ref;
      if (!find_model(loader, m.name, &ref) || ref.model == nullptr || ref.level == nullptr) {
        s_frame.missing++;
        continue;
      }
      const LevelData* lev = ref.level;
      const tfrag3::MercModel* model = ref.model;
      const dkpool::PoolBuffer* vb = dk_loader_buffer_find(lev->merc_vertices);
      const dkpool::PoolBuffer* ib = dk_loader_buffer_find(lev->merc_indices);
      if (vb == nullptr || ib == nullptr) {
        s_frame.missing++;
        continue;
      }
      const uint32_t bone_count = std::min<uint32_t>(model->max_bones + 1, kMaxBones);
      uint64_t pgpu = 0;
      uint8_t* p = slot_alloc(ring, kParamBytes + bone_count * kBoneBytes, &pgpu);
      if (p == nullptr) {
        s_frame.dropped++;
        continue;
      }
      MercVertParams vp;
      fill_vert(&vp, m, s_low[b]);
      memcpy(p, &vp, sizeof(vp));
      uint8_t* bones = p + kParamBytes;
      const uint8_t* src = s_bones.data() + (size_t)m.bone_first * kMercMatBytes;
      for (uint32_t k = 0; k < bone_count; k++) {
        uint8_t* dst = bones + k * kBoneBytes;
        if (k < m.bone_slots) {
          memcpy(dst, src + k * kMercMatBytes, kMercMatBytes);
        } else {
          memset(dst, 0, kMercMatBytes);
        }
        memset(dst + kMercMatBytes, 0, kBoneBytes - kMercMatBytes);
      }

      size_t li = 0;
      while (li < lev_order.size() && lev_order[li] != lev) {
        li++;
      }
      if (li == lev_order.size()) {
        lev_order.push_back(lev);
        if (lev_draws.size() < lev_order.size()) {
          lev_draws.resize(lev_order.size());
        }
      }
      auto& out = lev_draws[li];

      const bool disable_fog = m.bits & 2;
      const bool disable_envmap = m.bits & 8;
      if ((m.bits & 1) || (m.bits & 4)) {
        s_frame.mod++;  // mod / pc blerc: drawn from all_draws (rest pose), like GL with mod off
      }
      for (size_t ei = 0; ei < model->effects.size() && ei < 64; ei++) {
        if (!(m.enable & (1ull << ei))) {
          continue;
        }
        const bool ignore_alpha = m.ignore_alpha & (1ull << ei);
        const auto& effect = model->effects[ei];
        for (const auto& md : effect.all_draws) {
          if (effect.has_envmap && !disable_envmap) {
            s_frame.skipped++;  // emerc shine: not ported yet
          }
          // T82: eyes sample the dkeye render target (GL lookup_eye_texture(eye_id % 40)).
          const uint32_t eye_tex = md.eye_id != 0xff ? dkeye::eye_tex_id(md.eye_id) : 0;
          if (md.eye_id != 0xff ? eye_tex == 0
                                : (md.tree_tex_id < 0 ||
                                   (size_t)md.tree_tex_id >= lev->textures.size() ||
                                   lev->textures[md.tree_tex_id] == 0)) {
            s_frame.skipped++;  // eye not rendered yet / anim slot / missing texture
            continue;
          }
          DrawMode mode = md.mode;
          if (m.water) {
            mode.set_ab(true);
            mode.disable_depth_write();
          }
          if (disable_fog) {
            mode.set_fog(false);
          }
          dktf::DrawCmd st;
          dktf::fill_draw_state(mode, &st);
          DrawCmd c;
          c.vert_gpu = vb->gpu;
          c.vert_bytes = vb->capacity;
          c.idx_gpu = ib->gpu + (uint64_t)md.first_index * 4;
          c.idx_count = md.index_count;
          c.tex_id = eye_tex != 0 ? eye_tex : lev->textures[md.tree_tex_id];
          c.eye = eye_tex != 0;
          c.params_gpu = pgpu;
          c.bones_gpu = pgpu + kParamBytes;
          c.bones_bytes = bone_count * kBoneBytes;
          c.frag = (uint8_t)((ignore_alpha ? 1 : 0) | (mode.get_decal() ? 2 : 0) |
                             (mode.get_fog_enable() ? 4 : 0));
          c.depth_test = st.depth_test;
          c.depth_op = st.depth_op;
          c.depth_write = st.depth_write;
          c.blend = st.blend;
          c.clamp_s = st.clamp_s;
          c.clamp_t = st.clamp_t;
          c.filt = st.filt;
          c.strips = !md.no_strip;
          out.push_back(c);
          s_frame.tris += md.num_triangles;
        }
      }
    }
    for (size_t li = 0; li < lev_order.size(); li++) {
      s_frame.draws.insert(s_frame.draws.end(), lev_draws[li].begin(), lev_draws[li].end());
    }
  }
}

// ---- §7.2 phase 1 (AI-assisted): per-game bucket tables ----
// Table + pick only: on jak2 the characters are dkgl's Merc2 (§7.1), and the jak2 chain
// head differs (no CALL), so capture_chain is not called on jak2 yet.

void set_game(GameVersion v) {
  s_buckets = v == GameVersion::Jak2 ? kJak2Buckets : kJak1Buckets;
  s_bucket_count = v == GameVersion::Jak2 ? kJak2NumBuckets : kNumBuckets;
}

int bucket_count() {
  return s_bucket_count;
}

}  // namespace dkmerc
#endif  // __SWITCH__ || DK_HOST_REPLAY
