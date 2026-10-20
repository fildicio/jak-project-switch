#include "game/graphics/deko3d_renderer/SpriteDk.h"

// T79 (AI-assisted): deko3d Sprite3 CPU half. Port of GL Sprite3::render_jak1 (normal sprites)
// + TextureUploadHandler::render (VRAM uploads) for jak1.
// DK_HOST_REPLAY: test/test_dk_sprite_replay.cpp compiles this file on the Mac.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"

#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/opengl_renderer/sprite/sprite_common.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

namespace dksprite {
namespace {

constexpr int kSpritesPerChunk = 48;               // GL Sprite3::SPRITES_PER_CHUNK
constexpr uint32_t kMaxSprites = 1920 * 12;         // GL SPRITE_RENDERER_MAX_SPRITES
constexpr int kSlots = 3;                           // ring slots, same reason as MercDk

std::vector<Vertex> s_verts;
std::vector<uint32_t> s_idx;
std::vector<Batch> s_batches;
SpriteVertParams s_params;
CaptureStats s_stats;
FrameDraws s_frame;

// ---- GL Sprite3 bucketing state (m_sprite_buckets / m_bucket_list) ----
struct Bucket {
  std::vector<uint32_t> ids;
  uint32_t tex_id = 0;
};
std::map<uint64_t, Bucket> s_buckets;
std::vector<uint64_t> s_bucket_order;
DrawMode s_default_mode;
DrawMode s_mode;
uint32_t s_tbp = 0;
LookupFn s_lookup = nullptr;

void init_default_mode() {
  s_default_mode = DrawMode();
  s_default_mode.disable_depth_write();
  s_default_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  s_default_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  s_default_mode.set_aref(38);
  s_default_mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
  s_default_mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
  s_default_mode.set_at(true);
  s_default_mode.set_zt(true);
  s_default_mode.set_ab(true);
}

// GL flush_sprites: close the current buckets into draw batches (first-seen order).
void flush(bool hud) {
  for (uint64_t key : s_bucket_order) {
    Bucket& b = s_buckets[key];
    Batch bt;
    bt.idx_first = (uint32_t)s_idx.size();
    bt.idx_count = (uint32_t)b.ids.size();
    bt.tex_id = b.tex_id;
    bt.mode = (uint32_t)(key & 0xffffffff);
    bt.hud = hud;
    s_idx.insert(s_idx.end(), b.ids.begin(), b.ids.end());
    s_batches.push_back(bt);
  }
  s_buckets.clear();
  s_bucket_order.clear();
}

// GL update_mode_from_alpha1. Unknown combos keep SRC_DST_SRC_DST (GL asserts).
void mode_from_alpha(uint64_t val, DrawMode& mode) {
  GsAlpha reg(val);
  using B = GsAlpha::BlendMode;
  const auto a = reg.a_mode(), b = reg.b_mode(), c = reg.c_mode(), d = reg.d_mode();
  if (a == B::SOURCE && b == B::DEST && c == B::SOURCE && d == B::DEST) {
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  } else if (a == B::SOURCE && b == B::ZERO_OR_FIXED && c == B::SOURCE && d == B::DEST) {
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
  } else if (a == B::SOURCE && b == B::ZERO_OR_FIXED && c == B::ZERO_OR_FIXED && d == B::DEST) {
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_FIX_DST);
  } else if (a == B::SOURCE && b == B::DEST && c == B::ZERO_OR_FIXED && d == B::DEST) {
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
  } else if (a == B::ZERO_OR_FIXED && b == B::SOURCE && c == B::SOURCE && d == B::DEST) {
    mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
  } else {
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  }
}

// GL do_block_common for one chunk (mode: 1 = 2D, 2 = HUD, 3 = 3D).
void do_block(uint16_t mode, uint32_t count, const SpriteVecData2d* vec, const AdGifData* adgif) {
  s_mode = s_default_mode;
  for (uint32_t i = 0; i < count; i++) {
    const AdGifData& ag = adgif[i];
    GsTex0 tex0(ag.tex0_data);
    s_tbp = tex0.tbp0();
    s_mode.set_tcc(tex0.tcc());
    GsTex1 tex1(ag.tex1_data);
    s_mode.set_filt_enable(tex1.mmag());
    if (GsRegisterAddress(ag.clamp_addr) == GsRegisterAddress::ZBUF_1) {
      GsZbuf z(ag.clamp_data);
      s_mode.set_depth_write_enable(!z.zmsk());
    } else {
      s_mode.set_clamp_s_enable(ag.clamp_data & 0b001);
      s_mode.set_clamp_t_enable(ag.clamp_data & 0b100);
    }
    mode_from_alpha(ag.alpha_data, s_mode);

    if (s_verts.size() / 4 >= kMaxSprites) {
      s_stats.capped++;
      continue;
    }
    const uint64_t key = ((uint64_t)s_tbp << 32) | s_mode.as_int();
    auto it = s_buckets.find(key);
    if (it == s_buckets.end()) {
      it = s_buckets.emplace(key, Bucket()).first;
      it->second.tex_id = s_lookup ? s_lookup(s_tbp) : 0;  // resolved now: VRAM is reused later
      s_bucket_order.push_back(key);
    }
    const uint32_t v0 = (uint32_t)s_verts.size();
    it->second.ids.insert(it->second.ids.end(), {v0, v0 + 1, v0 + 2, v0 + 3, UINT32_MAX});

    SpriteVecData2d vd = vec[i];
    Vertex v = {};
    memcpy(v.xyz_sx, vd.xyz_sx.data(), 16);
    memcpy(v.quat_sy, vd.flag_rot_sy.data(), 16);
    for (int c = 0; c < 4; c++) {
      // ftoi'd in the original game; the VIF drops the upper bits on pack.
      v.rgba[c] = (float)((int)vd.rgba[c] & 0xff) / 255.f;
    }
    v.flags_matrix[0] = (uint16_t)vd.flag();
    v.flags_matrix[1] = (uint16_t)vd.matrix();
    v.info[1] = s_mode.get_tcc_enable();
    v.info[3] = mode;
    // GL corner order 0, 1, 3, 2 (PC swaps the last two for the strip).
    static const uint16_t kCorner[4] = {0, 1, 3, 2};
    for (int k = 0; k < 4; k++) {
      v.info[2] = kCorner[k];
      s_verts.push_back(v);
    }
  }
}

bool looks_like_2d_chunk_start(const DmaFollower& d) {
  return d.current_tag().qwc == 1 && d.current_tag().kind == DmaTag::Kind::CNT;
}

// One header + vec + adgif + run chunk. Returns the MSCAL program, -1 on bad data.
int read_chunk(DmaFollower& d, uint32_t* count, SpriteVecData2d* vec, AdGifData* adgif) {
  auto header = d.read_and_advance();
  if (header.size_bytes < 4) {
    return -1;
  }
  memcpy(count, header.data, 4);
  if (*count > (uint32_t)kSpritesPerChunk) {
    return -1;
  }
  auto vd = d.read_and_advance();
  memcpy(vec, vd.data, std::min<uint32_t>(vd.size_bytes, sizeof(SpriteVecData2d) * *count));
  auto ad = d.read_and_advance();
  memcpy(adgif, ad.data, std::min<uint32_t>(ad.size_bytes, sizeof(AdGifData) * *count));
  auto run = d.read_and_advance();
  if (run.vifcode1().kind != VifCode::Kind::MSCAL) {
    return -1;
  }
  return run.vifcode1().immediate;
}

bool looks_like_distort_frame_data(const DmaFollower& d) {
  return d.current_tag().kind == DmaTag::Kind::CNT &&
         d.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         d.current_tag_vifcode1().kind == VifCode::Kind::UNPACK_V4_32;
}

// GL TextureUploadHandler::render for one bucket: replay the PC_PORT upload packets in order.
void walk_tex_bucket(const void* data, uint32_t slot, uint32_t next, UploadFn upload) {
  DmaFollower d(data, slot);
  for (int guard = 0; guard < 100000 && !d.ended() && d.current_tag_offset() != next; guard++) {
    if (d.current_tag().kind == DmaTag::Kind::CALL) {
      break;  // the bucket's closing call to the default-regs chain
    }
    auto t = d.read_and_advance();
    if (t.size_bytes == 16 && t.vifcode0().kind == VifCode::Kind::PC_PORT && t.vif1() == 3) {
      uint64_t page = 0;
      int64_t mode = 0;
      memcpy(&page, t.data, 8);
      memcpy(&mode, t.data + 8, 8);
      s_stats.uploads++;
      if (upload) {
        upload((uint32_t)page, mode);
      }
    }
  }
}

// GL Sprite3::render_jak1 minus the draws. Fills s_verts / s_idx / s_batches / s_params.
void walk_sprite_bucket(const void* data, uint32_t slot, uint32_t next) {
  DmaFollower d(data, slot);
  d.read_and_advance();  // NEXT jump from the bucket table
  if (d.current_tag().kind == DmaTag::Kind::CALL) {
    return;  // sprite renderer didn't run
  }
  // pre-sprite DirectRenderer data (not drawn yet)
  while (d.current_tag().qwc != 7 && d.current_tag_offset() != next && !d.ended()) {
    d.read_and_advance();
    s_stats.direct++;
  }
  if (d.current_tag_offset() == next || d.ended()) {
    return;
  }
  s_stats.ran = true;

  // distorter (GL distort_dma): setup, aspect, tables, then frame-data blocks + MSCALF each
  d.read_and_advance();
  d.read_and_advance();
  d.read_and_advance();
  while (looks_like_distort_frame_data(d)) {
    do {
      const int dest = d.current_tag_vifcode1().immediate;
      auto t = d.read_and_advance();
      if (dest == 511 && t.size_bytes >= 4) {
        uint32_t n = 0;
        memcpy(&n, t.data, 4);
        s_stats.distort += n;
      }
    } while (looks_like_distort_frame_data(d));
    d.read_and_advance();  // MSCALF + FLUSH
  }

  // handle_sprite_frame_setup: direct setup (48 B), frame data, MSCALF, BASE/OFFSET
  auto direct = d.read_and_advance();
  auto frame = d.read_and_advance();
  if (direct.size_bytes != 48 || frame.size_bytes != (int)sizeof(SpriteFrameDataJak1)) {
    s_stats.bad = true;
    return;
  }
  SpriteFrameDataJak1 fd;
  memcpy(&fd, frame.data, sizeof(fd));
  d.read_and_advance();
  d.read_and_advance();

  // render_3d: one-time matrix data
  auto mat = d.read_and_advance();
  if (mat.size_bytes != (int)sizeof(Sprite3DMatrixData)) {
    s_stats.bad = true;
    return;
  }
  Sprite3DMatrixData m3;
  memcpy(&m3, mat.data, sizeof(m3));

  memset(&s_params, 0, sizeof(s_params));
  memcpy(s_params.hvdf_offset, m3.hvdf_offset.data(), 16);
  memcpy(s_params.camera, m3.camera.data(), 64);
  memcpy(s_params.basis_x, fd.basis_x.data(), 16);
  memcpy(s_params.basis_y, fd.basis_y.data(), 16);
  for (int i = 0; i < 8; i++) {
    memcpy(s_params.xy_array[i], fd.xy_array[i].data(), 16);
  }
  for (int i = 0; i < 4; i++) {
    memcpy(s_params.xyz_array[i], fd.xyz_array[i].data(), 16);
    memcpy(s_params.st_array[i], fd.st_array[i].data(), 16);
  }
  s_params.consts0[0] = fd.pfog0;
  s_params.consts0[1] = fd.fog_min;
  s_params.consts0[2] = fd.fog_max;
  s_params.consts0[3] = fd.min_scale;
  s_params.consts1[0] = fd.max_scale;
  s_params.consts1[1] = fd.deg_to_rad;
  s_params.consts1[2] = fd.inv_area;

  static SpriteVecData2d vec[kSpritesPerChunk];
  static AdGifData adgif[kSpritesPerChunk];

  // render_2d_group0: world 2D (program 3) and 3D sprites, flushed on program change
  int last_prog = -1;
  while (looks_like_2d_chunk_start(d)) {
    uint32_t count = 0;
    const int prog = read_chunk(d, &count, vec, adgif);
    if (prog < 0) {
      s_stats.bad = true;
      return;
    }
    s_stats.count_2d += count;
    if (prog != last_prog) {
      flush(false);
    }
    do_block(prog == SpriteProgMem::Sprites2dGrp0 ? 1 : 3, count, vec, adgif);
    last_prog = prog;
  }
  flush(false);

  // render_fake_shadow: nop + flushe
  d.read_and_advance();

  // render_2d_group1 (HUD): one-time matrix upload, then chunks
  auto hud = d.read_and_advance();
  if (hud.size_bytes != (int)sizeof(SpriteHudMatrixData)) {
    s_stats.bad = true;
    return;
  }
  SpriteHudMatrixData hm;
  memcpy(&hm, hud.data, sizeof(hm));
  memcpy(s_params.hud_matrix, hm.matrix.data(), 64);
  memcpy(s_params.hud_hvdf_offset, hm.hvdf_offset.data(), 16);
  for (int i = 0; i < 75; i++) {
    memcpy(s_params.hud_hvdf_user[i], hm.user_hvdf[i].data(), 16);
  }
  while (looks_like_2d_chunk_start(d)) {
    uint32_t count = 0;
    if (read_chunk(d, &count, vec, adgif) < 0) {
      s_stats.bad = true;
      return;
    }
    s_stats.count_hud += count;
    do_block(2, count, vec, adgif);
  }
  flush(true);
  // the rest (up to next) is ignored, like GL render_jak1
}

// ---- per-frame GPU ring (verts + indices + params), 3 slots like MercDk ----
struct Slot {
  dkpool::PoolBuffer pb;
  uint32_t used = 0;
  uint32_t want = 0;
};
Slot s_slot[kSlots];
uint32_t s_build = 0;

// need = this frame's bytes: unlike merc, the size is known before filling, so grow now.
Slot* slot_begin(uint32_t need_now) {
  Slot* s = &s_slot[s_build % kSlots];
  s->used = 0;
  s->want = std::max(s->want, need_now);
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  const uint32_t need = std::max<uint32_t>(256u << 10, s->want + s->want / 2);
  if (pool && (!s->pb.ok() || s->pb.capacity < s->want)) {
    if (s->pb.ok()) {
      pool->release(s->pb.id);
    }
    s->pb = pool->acquire(need);
    switch_run_logf("[dk] sprite ring slot %u: %u KB %s", s_build % kSlots, need >> 10,
                    s->pb.ok() ? "ok" : "FAILED");
  }
  s->want = 0;
  return s;
}

uint8_t* slot_alloc(Slot* s, uint32_t bytes, uint64_t* gpu) {
  bytes = (bytes + 0xff) & ~0xffu;
  s->want += bytes;
  if (!s->pb.ok() || s->used + bytes > s->pb.capacity) {
    return nullptr;
  }
  uint8_t* p = s->pb.cpu + s->used;
  *gpu = s->pb.gpu + s->used;
  s->used += bytes;
  return p;
}

uint64_t frag_params(Slot* s, float amin, float amax) {
  uint64_t gpu = 0;
  uint8_t* p = slot_alloc(s, kFragParamBytes, &gpu);
  if (p == nullptr) {
    return 0;
  }
  const float f[4] = {amin, amax, 0.f, 0.f};
  memcpy(p, f, sizeof(f));
  return gpu;
}

}  // namespace

void capture_chain(const void* data, uint32_t start_offset, UploadFn upload, LookupFn lookup) {
  s_verts.clear();
  s_idx.clear();
  s_batches.clear();
  s_buckets.clear();
  s_bucket_order.clear();
  s_stats = CaptureStats();
  s_lookup = lookup;
  init_default_mode();
  if (data == nullptr) {
    return;
  }
  DmaFollower d(data, start_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;  // not a game frame chain
  }
  const uint32_t buckets_base = d.current_tag_offset() + 16;
  for (int b : kJak1TexBuckets) {
    const uint32_t slot = buckets_base + 16 * b;
    walk_tex_bucket(data, slot, slot + 16, upload);
  }
  const uint32_t slot = buckets_base + 16 * kJak1SpriteBucket;
  walk_sprite_bucket(data, slot, slot + 16);
}

const CaptureStats& capture_stats() {
  return s_stats;
}
const std::vector<Vertex>& captured_vertices() {
  return s_verts;
}
const std::vector<uint32_t>& captured_indices() {
  return s_idx;
}
const std::vector<Batch>& captured_batches() {
  return s_batches;
}
const SpriteVertParams& captured_params() {
  return s_params;
}
const FrameDraws& frame_draws() {
  return s_frame;
}

void build_frame() {
  s_frame = FrameDraws();
  if (s_batches.empty()) {
    return;
  }
  s_frame.sprites = (uint32_t)(s_verts.size() / 4);
  s_build++;
  const uint32_t vbytes = (uint32_t)(s_verts.size() * sizeof(Vertex));
  const uint32_t ibytes = (uint32_t)(s_idx.size() * 4);
  const uint32_t need = vbytes + ibytes + 0x200 + kVertParamBytes +
                        (uint32_t)s_batches.size() * 2 * kFragParamBytes;
  Slot* ring = slot_begin(need);
  uint64_t vgpu = 0, igpu = 0, pgpu = 0;
  uint8_t* v = slot_alloc(ring, vbytes, &vgpu);
  uint8_t* ix = slot_alloc(ring, ibytes, &igpu);
  uint8_t* p = slot_alloc(ring, kVertParamBytes, &pgpu);
  if (v == nullptr || ix == nullptr || p == nullptr) {
    s_frame.dropped = s_frame.sprites;
    return;
  }
  memcpy(v, s_verts.data(), vbytes);
  memcpy(ix, s_idx.data(), ibytes);
  memcpy(p, &s_params, sizeof(s_params));
  s_frame.vert_gpu = vgpu;
  s_frame.vert_bytes = vbytes;
  s_frame.params_gpu = pgpu;

  for (const Batch& b : s_batches) {
    if (b.tex_id == 0) {
      s_frame.no_tex++;
      continue;
    }
    DrawMode mode;
    mode.as_int() = b.mode;
    dktf::DrawCmd st;
    dktf::fill_draw_state(mode, &st);
    DrawCmd c;
    c.idx_gpu = igpu + (uint64_t)b.idx_first * 4;
    c.idx_count = b.idx_count;
    c.tex_id = b.tex_id;
    c.depth_test = st.depth_test;
    c.depth_op = st.depth_op;
    c.depth_write = st.depth_write;
    c.blend = st.blend;
    c.clamp_s = st.clamp_s;
    c.clamp_t = st.clamp_t;
    c.filt = st.filt;
    // GL flush_sprites: world sprites alpha_min 0.016; HUD uses aref + AFAIL second pass.
    c.frag_gpu = frag_params(ring, b.hud ? st.alpha_min : 0.016f, 10.f);
    if (c.frag_gpu == 0) {
      s_frame.dropped++;
      continue;
    }
    s_frame.draws.push_back(c);
    if (b.hud && st.afail_second) {
      c.frag_gpu = frag_params(ring, -10.f, st.aref_second);
      c.depth_write = false;
      if (c.frag_gpu == 0) {
        s_frame.dropped++;
        continue;
      }
      s_frame.draws.push_back(c);
    }
  }
}

}  // namespace dksprite
#endif  // __SWITCH__ || DK_HOST_REPLAY
