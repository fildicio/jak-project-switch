#include "game/graphics/deko3d_renderer/EyeDk.h"

// T82 (AI-assisted): CPU half of the deko3d eye renderer. Port of EyeRenderer::render /
// handle_eye_dma2 / get_draws / add_draw_to_buffer_* (jak1 only). The GL ASSERTs become
// checks: an unexpected DMA shape stops the walk (stats.bad) instead of crashing.

#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <cstring>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"

#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

namespace dkeye {
namespace {

constexpr uint32_t kBucket = 54;  // jak1 MERC_EYES_AFTER_PRIS
constexpr int kSingleEyeSize = 32;
constexpr int kNumPairs = kNumEyes / 2;
constexpr int kSlots = 3;  // ring slots, same reason as DirectDk / SpriteDk

struct Sprite {
  uint64_t uv0 = 0;
  uint32_t xyz0[3] = {};
  uint32_t xyz1[3] = {};
};
struct Draw {
  Sprite sprite;
  uint32_t sc_y0 = 0, sc_y1 = 0;
};

std::vector<Eye> s_eyes;
CaptureStats s_stats;
LookupFn s_lookup = nullptr;
FrameDraws s_frame;
uint32_t s_eye_ids[kNumEyes] = {};

// GL decode_scissor + decode_sprite.
bool read_draw(DmaFollower& d, Draw* out) {
  auto sc = d.read_and_advance();
  if (sc.size_bytes != 32) {
    return false;
  }
  uint64_t val;
  memcpy(&val, sc.data + 16, 8);
  GsScissor reg(val);
  out->sc_y0 = reg.y0();
  out->sc_y1 = reg.y1();
  auto sp = d.read_and_advance();
  if (sp.size_bytes != 96) {
    return false;
  }
  memcpy(&out->sprite.uv0, sp.data + 32, 8);
  memcpy(out->sprite.xyz0, sp.data + 48, 12);
  memcpy(out->sprite.xyz1, sp.data + 80, 12);
  return true;
}

// A 96-byte adgif transfer (tag + 5 a+d); returns the TEX0 tbp0 as a dk texture id.
bool read_adgif(DmaFollower& d, uint32_t* tex) {
  auto t = d.read_and_advance();
  if (t.size_bytes != 96) {
    return false;
  }
  uint64_t tex0;
  memcpy(&tex0, t.data + 16, 8);
  *tex = s_lookup ? s_lookup(GsTex0(tex0).tbp0()) : 0;
  return true;
}

// GL add_draw_to_buffer_32 / _64 (same unsigned integer math).
void quad(Vertex* v, const Sprite& s, bool using_64, int pair, int lr) {
  uint32_t x0, y0, x1, y1;
  if (using_64) {
    const uint32_t x_off = lr * kSingleEyeSize * 32;
    const uint32_t y_off = (pair / 4) * kSingleEyeSize * 32;
    x0 = (s.xyz0[0] - x_off) / 2;
    y0 = (s.xyz0[1] - y_off) / 2;
    x1 = (s.xyz1[0] - x_off) / 2;
    y1 = (s.xyz1[1] - y_off) / 2;
  } else {
    const uint32_t x_off = lr * kSingleEyeSize * 16;
    const uint32_t y_off = pair * kSingleEyeSize * 16;
    x0 = s.xyz0[0] - x_off;
    y0 = s.xyz0[1] - y_off;
    x1 = s.xyz1[0] - x_off;
    y1 = s.xyz1[1] - y_off;
  }
  v[0] = {(float)x0, (float)y0, 0.f, 0.f};
  v[1] = {(float)x1, (float)y0, 1.f, 0.f};
  v[2] = {(float)x0, (float)y1, 0.f, 1.f};
  v[3] = {(float)x1, (float)y1, 1.f, 1.f};
}

// GL add_clear_draw_to_buffer: whole target, st = 0,0 (the iris corner colour).
void clear_quad(Vertex* v) {
  const float lo = 768.f - 256.f, hi = 768.f + 256.f;
  v[0] = {lo, lo, 0.f, 0.f};
  v[1] = {hi, lo, 0.f, 0.f};
  v[2] = {lo, hi, 0.f, 0.f};
  v[3] = {hi, hi, 0.f, 0.f};
}

// For the right eye: an optional adgif (next tag qwc == 6) swaps the texture.
bool right_tex(DmaFollower& d, uint32_t left, uint32_t* out) {
  if (d.current_tag().qwc == 6) {
    return read_adgif(d, out);
  }
  *out = left;
  return true;
}

// GL get_draws for one eye pair. False = unexpected DMA (the caller stops).
bool read_pair(DmaFollower& d) {
  uint32_t iris_l = 0;
  if (!read_adgif(d, &iris_l)) {
    return false;
  }
  Draw draw0;
  if (!read_draw(d, &draw0)) {
    return false;
  }
  const bool using_64 = draw0.sc_y1 - draw0.sc_y0 == 63;
  uint32_t y0 = (draw0.sprite.xyz0[1] - 512) >> 4;
  if (using_64) {
    y0 = ((draw0.sprite.xyz0[1] - 1024) >> 5) * 4;
  }
  const uint32_t pair = y0 / kSingleEyeSize;

  Draw iris[2], pupil[2], lid[2];
  uint32_t iris_r = 0, pupil_l = 0, pupil_r = 0, lid_l = 0, lid_r = 0;
  if (!read_draw(d, &iris[0]) || !right_tex(d, iris_l, &iris_r) || !read_draw(d, &iris[1])) {
    return false;
  }
  d.read_and_advance();  // test1
  // FIX 74: the pupil transfers are always consumed, even without a texture.
  if (!read_adgif(d, &pupil_l) || !read_draw(d, &pupil[0]) || !right_tex(d, pupil_l, &pupil_r) ||
      !read_draw(d, &pupil[1])) {
    return false;
  }
  d.read_and_advance();  // test2
  if (!read_adgif(d, &lid_l) || !read_draw(d, &lid[0]) || !right_tex(d, lid_l, &lid_r) ||
      !read_draw(d, &lid[1])) {
    return false;
  }
  auto end = d.read_and_advance();  // jak1: empty end transfer
  if (end.size_bytes != 0) {
    return false;
  }
  if (pair >= (uint32_t)kNumPairs) {
    return false;
  }

  const uint32_t ir[2] = {iris_l, iris_r}, pu[2] = {pupil_l, pupil_r}, li[2] = {lid_l, lid_r};
  for (int lr = 0; lr < 2; lr++) {
    Eye e;
    e.slot = (uint8_t)(pair * 2 + lr);
    e.iris = ir[lr];
    e.pupil = pu[lr];
    e.lid = li[lr];
    if (!e.iris || !e.pupil || !e.lid) {
      s_stats.no_tex++;
    }
    clear_quad(e.v[0]);
    quad(e.v[1], iris[lr].sprite, using_64, (int)pair, lr);
    quad(e.v[2], pupil[lr].sprite, using_64, (int)pair, lr);
    quad(e.v[3], lid[lr].sprite, using_64, (int)pair, lr);
    s_eyes.push_back(e);
    s_stats.eyes++;
  }
  return true;
}

// ---- per-frame GPU ring (eyes are tiny: 40 * 256 B) ----
struct Slot {
  dkpool::PoolBuffer pb;
};
Slot s_slot[kSlots];
uint32_t s_build = 0;
constexpr uint32_t kSlotBytes = 16u << 10;

}  // namespace

void capture_chain(const void* data, uint32_t start_offset, LookupFn lookup) {
  s_eyes.clear();
  s_stats = CaptureStats();
  s_lookup = lookup;
  if (data == nullptr) {
    return;
  }
  DmaFollower d(data, start_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;  // not a game frame chain
  }
  const uint32_t buckets_base = d.current_tag_offset() + 16;
  const uint32_t slot = buckets_base + 16 * kBucket;
  const uint32_t next = slot + 16;
  DmaFollower b(data, slot);
  b.read_and_advance();  // GL render: data0, the bucket's own empty tag
  if (b.ended() || b.current_tag().kind == DmaTag::Kind::CALL ||
      b.current_tag_offset() == next) {
    return;  // the eye code did not run this frame
  }
  // GL handle_eye_dma2: offset setup (128 B), alpha setup (32 B), jak1 NEXT qwc 0.
  if (b.read_and_advance().size_bytes != 128 || b.read_and_advance().size_bytes != 32) {
    s_stats.bad++;
    return;
  }
  b.read_and_advance();
  for (int guard = 0; guard < kNumPairs + 1; guard++) {
    if (b.ended() || b.current_tag_offset() == next || b.current_tag().qwc == 8) {
      return;  // GL: an 8 qw transfer restores the gs
    }
    if (!read_pair(b)) {
      s_stats.bad++;
      return;
    }
  }
}

const CaptureStats& capture_stats() {
  return s_stats;
}
const std::vector<Eye>& captured_eyes() {
  return s_eyes;
}

void build_frame() {
  s_frame = FrameDraws();
  if (s_eyes.empty()) {
    return;
  }
  Slot* s = &s_slot[s_build++ % kSlots];
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  if (pool && !s->pb.ok()) {
    s->pb = pool->acquire(kSlotBytes);
  }
  const uint32_t bytes = (uint32_t)(s_eyes.size() * sizeof(Eye::v));
  if (!s->pb.ok() || bytes > s->pb.capacity) {
    s_frame.dropped = (uint32_t)s_eyes.size();
    return;
  }
  for (size_t i = 0; i < s_eyes.size(); i++) {
    memcpy(s->pb.cpu + i * sizeof(Eye::v), s_eyes[i].v, sizeof(Eye::v));
  }
  s_frame.eyes = s_eyes;
  s_frame.vert_gpu = s->pb.gpu;
  s_frame.vert_bytes = bytes;
}

const FrameDraws& frame_draws() {
  return s_frame;
}

uint32_t eye_tex_id(uint8_t eye_id) {
  return s_eye_ids[eye_id % kNumEyes];
}

void set_eye_tex_id(int slot, uint32_t id) {
  if (slot >= 0 && slot < kNumEyes) {
    s_eye_ids[slot] = id;
  }
}

}  // namespace dkeye
#endif  // __SWITCH__ || DK_HOST_REPLAY
