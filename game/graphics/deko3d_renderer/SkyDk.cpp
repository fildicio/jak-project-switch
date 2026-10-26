// T88 (AI-assisted): deko3d sky CPU blender (jak1), CPU half. Port of GL SkyBlendCPU for
// the dk path: no GL textures here -- blend into plain buffers and hand them to a sink
// (dk_renderer.cpp dk_sky_store) that uploads through dktexgpu. The walk is the GL
// SkyBlendHandler::render / SkyBlendCPU::do_sky_blends logic with ASSERTs turned into
// stats counters (a bad pair is skipped, never crashes the render thread).
#include "game/graphics/deko3d_renderer/SkyDk.h"

#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)

#include <cstring>
#include <vector>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"

#include "game/graphics/opengl_renderer/AdgifHandler.h"

namespace dksky {
namespace {

// GL SkyBlendCPU m_sizes: 32x32 sky + 64x64 clouds.
constexpr int kSizes[2] = {32, 64};

std::vector<uint8_t> s_data[2];
bool s_dirty[2] = {false, false};
BlendStats s_stats;
SourceFn s_source = nullptr;

// GL SkyBlendCPU::do_sky_blends body for one qwc-6 setup + qwc-6 draw pair.
void blend_pair(const uint8_t* setup_data, const uint8_t* draw_data, uint32_t draw_bytes) {
  s_stats.pairs++;
  // first is an adgif
  AdgifHelper adgif(setup_data + 16);
  if (!adgif.is_normal_adgif()) {
    s_stats.bad_adgif++;
    return;
  }
  if (adgif.alpha().data != 0x8000000068ull) {  // GL: Cs + Cd
    s_stats.bad_alpha++;
    return;
  }
  if (draw_bytes != 6 * 16) {
    s_stats.bad_draw++;
    return;
  }
  GifTag draw_tag(draw_data);
  // the first draw overwrites the previous frame's draw by disabling alpha blend (ABE = 0)
  const bool is_first_draw = !GsPrim(draw_tag.prim()).abe();
  // relying on the format of the drawing to get the alpha/offset.
  uint32_t coord = 0, intensity = 0;
  memcpy(&coord, draw_data + (5 * 16), 4);
  memcpy(&intensity, draw_data + 16, 4);
  // sky is smaller than the clouds, so the drawing coordinate tells them apart.
  int idx;
  if (coord == 0x200) {
    idx = 0;  // sky
  } else if (coord == 0x400) {
    idx = 1;  // clouds
  } else {
    s_stats.bad_coord++;
    return;
  }
  if (s_source == nullptr) {
    return;
  }
  const SkySource src = s_source(adgif.tex0().tbp0());
  if (src.data == nullptr) {
    s_stats.no_src++;  // texture not loaded yet (loader timing) -- skip like GL's null check
    // FIX 117: which tbp, and absent vs placeholder, so one log line identifies it.
    if (src.placeholder) {
      s_stats.no_src_placeholder++;
    } else {
      s_stats.no_src_absent++;
    }
    const uint32_t tbp = adgif.tex0().tbp0();
    int slot = -1;
    for (int i = 0; i < 4; i++) {
      if (s_stats.no_src_tbp_count[i] > 0 && s_stats.no_src_tbp[i] == tbp) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      for (int i = 0; i < 4; i++) {
        if (s_stats.no_src_tbp_count[i] == 0) {
          slot = i;
          s_stats.no_src_tbp[i] = tbp;
          break;
        }
      }
    }
    if (slot >= 0) {
      s_stats.no_src_tbp_count[slot]++;
    }
    return;
  }
  if (src.bytes != s_data[idx].size()) {
    s_stats.bad_size++;
    s_stats.last_bad_tbp = adgif.tex0().tbp0();
    s_stats.last_bad_bytes = src.bytes;
    s_stats.last_bad_idx = idx;
    return;
  }
  if (is_first_draw) {
    blend_sky_initial_fast(intensity, s_data[idx].data(), src.data,
                           (uint32_t)s_data[idx].size());
  } else {
    blend_sky_fast(intensity, s_data[idx].data(), src.data, (uint32_t)s_data[idx].size());
  }
  s_dirty[idx] = true;
  if (idx == 0) {
    is_first_draw ? s_stats.sky_draws++ : s_stats.sky_blends++;
  } else {
    is_first_draw ? s_stats.cloud_draws++ : s_stats.cloud_blends++;
  }
}

// GL SkyBlendHandler::render for one bucket: head is a size-0 NEXT; then either a CALL
// (bucket didn't run), a first tag with qwc != 8 (trans tfrag only -- TFragmentDk's walk,
// nothing to blend) or the 8-qw set-display-gs-state followed by the qwc-6 copy pairs.
void walk_blend_bucket(const void* data, uint32_t slot) {
  DmaFollower d(data, slot);
  d.read_and_advance();  // bucket head NEXT (size 0)
  if (d.ended() || d.current_tag().kind == DmaTag::Kind::CALL) {
    return;  // didn't run this frame
  }
  if (d.current_tag().qwc != 8) {
    return;  // no sky packets
  }
  d.read_and_advance();  // set-display-gs-state (blend regs, no texture data)
  s_stats.buckets++;
  int guard = 0;
  while (!d.ended() && d.current_tag().qwc == 6 && guard++ < 64) {
    // assuming that the vif and gif-tag is correct (GL)
    const auto setup = d.read_and_advance();
    if (d.ended() || d.current_tag().qwc != 6) {
      s_stats.bad_draw++;  // setup without its draw
      break;
    }
    const auto draw = d.read_and_advance();
    blend_pair(setup.data, draw.data, draw.size_bytes);
  }
  // reset_alpha / reset_gs_state / empty trailer carries no texture data.
}

}  // namespace

void blend_sky_initial_fast(uint8_t intensity, uint8_t* out, const uint8_t* in, uint32_t size) {
#ifdef __aarch64__
  // widen u8 to u16, multiply, shift, then narrow with saturation. 255*255 fits in a u16 so
  // the multiply can't overflow, and vqmovn_u16 clamps at 255 like packus does.
  const uint16x8_t intensity_vec = vdupq_n_u16(intensity);
  uint32_t i = 0;
  for (; i + 16 <= size; i += 16) {
    const uint8x16_t tex = vld1q_u8(in + i);
    const uint16x8_t lo =
        vshrq_n_u16(vmulq_u16(vmovl_u8(vget_low_u8(tex)), intensity_vec), 7);
    const uint16x8_t hi =
        vshrq_n_u16(vmulq_u16(vmovl_u8(vget_high_u8(tex)), intensity_vec), 7);
    vst1q_u8(out + i, vcombine_u8(vqmovn_u16(lo), vqmovn_u16(hi)));
  }
  for (; i < size; i++) {
    const uint32_t v = ((uint32_t)in[i] * intensity) >> 7;
    out[i] = v > 255 ? 255 : (uint8_t)v;
  }
#else
  // portable scalar (host tests): same math as the GL SSE path, incl. the saturation.
  for (uint32_t i = 0; i < size; i++) {
    const uint32_t v = ((uint32_t)in[i] * intensity) >> 7;
    out[i] = v > 255 ? 255 : (uint8_t)v;
  }
#endif
}

void blend_sky_fast(uint8_t intensity, uint8_t* out, const uint8_t* in, uint32_t size) {
#ifdef __aarch64__
  const uint16x8_t intensity_vec = vdupq_n_u16(intensity);
  uint32_t i = 0;
  for (; i + 16 <= size; i += 16) {
    const uint8x16_t tex = vld1q_u8(in + i);
    const uint8x16_t cur = vld1q_u8(out + i);
    const uint16x8_t lo =
        vshrq_n_u16(vmulq_u16(vmovl_u8(vget_low_u8(tex)), intensity_vec), 7);
    const uint16x8_t hi =
        vshrq_n_u16(vmulq_u16(vmovl_u8(vget_high_u8(tex)), intensity_vec), 7);
    // vqmovn_u16 clamps at 255, so it covers both the min and the packus the x86 path does
    vst1q_u8(out + i, vqaddq_u8(cur, vcombine_u8(vqmovn_u16(lo), vqmovn_u16(hi))));
  }
  for (; i < size; i++) {
    const uint32_t v = ((uint32_t)in[i] * intensity) >> 7;
    const uint32_t sum = out[i] + v;
    out[i] = sum > 255 ? 255 : (uint8_t)sum;
  }
#else
  for (uint32_t i = 0; i < size; i++) {
    const uint32_t v = ((uint32_t)in[i] * intensity) >> 7;
    const uint32_t sum = out[i] + v;
    out[i] = sum > 255 ? 255 : (uint8_t)sum;
  }
#endif
}

void capture_blend_level(const void* data, uint32_t start_offset, SourceFn src, int level) {
  s_source = src;
  if (s_data[0].empty()) {
    // GL constructor: fixed-size buffers, allocated once so data() stays stable (the pool's
    // TextureInput::src_data keeps pointing at it for the whole run).
    for (int i = 0; i < 2; i++) {
      s_data[i].resize(4 * kSizes[i] * kSizes[i]);
    }
  }
  if (data == nullptr || level < 0 || level > 1) {
    return;
  }
  DmaFollower d(data, start_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;  // not a game frame chain
  }
  const uint32_t buckets_base = d.current_tag_offset() + 16;
  walk_blend_bucket(data, buckets_base + 16 * kBlendBucket[level]);
  if (level == 1) {
    s_stats.frames++;
  }
}

void capture_blend_chain(const void* data, uint32_t start_offset, SourceFn src) {
  capture_blend_level(data, start_offset, src, 0);
  capture_blend_level(data, start_offset, src, 1);
}

void flush_blend(SinkFn sink) {
  if (sink == nullptr) {
    return;
  }
  for (int i = 0; i < 2; i++) {
    if (s_dirty[i]) {
      sink(i, s_data[i].data(), (uint32_t)s_data[i].size());
      s_dirty[i] = false;
    }
  }
}

const BlendStats& blend_stats() {
  return s_stats;
}

const uint8_t* blend_buffer(int index, uint32_t* size) {
  if (index < 0 || index > 1) {
    return nullptr;
  }
  if (size != nullptr) {
    *size = (uint32_t)s_data[index].size();
  }
  return s_data[index].empty() ? nullptr : s_data[index].data();
}

}  // namespace dksky
#endif  // __SWITCH__ || DK_HOST_REPLAY