// T60 (AI-assisted): P3 first visible tfrag draw — deko3d half. Records the plain
// dktf::FrameDraws list (built by TFragmentDk.cpp) into the frame command buffer.
// Called from dkdev::present_frame (via set_world_recorder) after the colour + depth
// clear, so it must never touch the cmd buffer's memory or submit anything itself.
#include "game/graphics/deko3d_renderer/TFragmentDk.h"
#include "game/graphics/deko3d_renderer/ShrubDk.h"
#include "game/graphics/deko3d_renderer/TieDk.h"

#if defined(__SWITCH__)
#include <deko3d.h>
#include <string.h>

#include <array>
#include <vector>

#include "game/graphics/deko3d_renderer/dk_ring.h"
#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/deko3d_renderer/dk_tfrag_ubo.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"

// switch_run_logf lives in run_log.h, which cannot be included next to <deko3d.h>.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dktf {
namespace {

constexpr uint32_t kUboSize = 0x100;     // VertParams (176 B) / FragParams (36 B), padded
constexpr uint32_t kTexImages = 2048;    // dktexgpu descriptor slots
constexpr uint32_t kFragCache = 16;      // distinct FragParams per frame (dedupe)

struct FragKey {
  float amin, amax;
  int no_tex;
  uint64_t gpu;
};

// T72 (AI-assisted): [0] = tfrag, [1] = shrub, [2] = tie (T76: tfrag shaders + vertex layout).
uint64_t s_frames[3] = {};
uint32_t s_draws[3] = {}, s_misses[3] = {}, s_ubo_fail[3] = {};

// PreloadedVertex (32 B): xyz @0, stq @16, color_index u16 @28 (TFragment.cpp L315-335).
const DkVtxAttribState kTfragAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 16, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 28, DkVtxAttribSize_1x16, DkVtxAttribType_Uint, 0},
};
// ShrubGpuVertex (32 B): xyz @0, st @12, color_index u16 @24, rgba_base 3 x u8 @28
// (dk_shrub_prep.h kAttribs; shader locations 0 pos, 1 uv, 2 rgba_base, 3 tod index).
const DkVtxAttribState kShrubAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 12, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    {0, 0, 28, DkVtxAttribSize_3x8, DkVtxAttribType_Unorm, 0},
    {0, 0, 24, DkVtxAttribSize_1x16, DkVtxAttribType_Uint, 0},
};

void record_list(const FrameDraws& f, int fb_w, int fb_h, int k) {
  const bool loaded = k == 1 ? dkdev::shrub_shaders_loaded() : dkdev::tfrag3_shaders_loaded();
  if (f.draws.empty() || !loaded) {
    return;
  }
  dkring::FrameRing* ring = dkdev::ubo_ring();
  if (ring == nullptr || ring->segment_size() == 0) {
    return;
  }
  DkCmdBuf cb = dkdev::cmd_handle();
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());

  // Fixed state for every tfrag draw.
  DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
  dkCmdBufSetViewports(cb, 0, &vp, 1);
  DkShader vsh = k == 1 ? dkdev::shrub_vsh() : dkdev::tfrag3_vsh();
  DkShader fsh = k == 1 ? dkdev::shrub_fsh() : dkdev::tfrag3_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());
  const DkVtxBufferState vbuf = {32, 0};
  if (k == 1) {
    dkCmdBufBindVtxAttribState(cb, kShrubAttribs, 4);
  } else {
    dkCmdBufBindVtxAttribState(cb, kTfragAttribs, 3);
  }
  dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
  dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX);

  // VertParams: one pair (decal 0/1) per camera, filled on first use.
  static std::vector<std::array<uint64_t, 2>> vert_ubo;
  vert_ubo.assign(f.cams.size(), {0, 0});
  FragKey frag[kFragCache];
  uint32_t n_frag = 0;
  auto frag_ubo = [&](float amin, float amax, bool no_tex) -> uint64_t {
    for (uint32_t i = 0; i < n_frag; i++) {
      if (frag[i].amin == amin && frag[i].amax == amax && frag[i].no_tex == (int)no_tex) {
        return frag[i].gpu;
      }
    }
    dkring::Alloc a = ring->alloc(kUboSize, 0x100);
    if (!a.ok()) {
      return 0;
    }
    FragParams p;
    math::Vector<uint8_t, 4> fog(f.fog_rgba[0], f.fog_rgba[1], f.fog_rgba[2], f.fog_rgba[3]);
    fill_frag(&p, amin, amax, fog, 1.f, no_tex);  // fog_intensity: SharedRenderState default
    memcpy(a.cpu, &p, sizeof(p));
    if (n_frag < kFragCache) {
      frag[n_frag++] = {amin, amax, (int)no_tex, a.gpu};
    }
    return a.gpu;
  };

  const dkpipe::PipelineDesc* bound = nullptr;
  uint64_t bound_vert = 0, bound_vubo = 0, bound_fubo = 0;
  uint32_t draws = 0;
  for (const DrawCmd& c : f.draws) {
    if (c.cam >= f.cams.size()) {
      continue;
    }
    uint64_t& vu = vert_ubo[c.cam][c.decal ? 1 : 0];
    if (vu == 0) {
      dkring::Alloc a = ring->alloc(kUboSize, 0x100);
      if (!a.ok()) {
        s_ubo_fail[k]++;
        break;
      }
      const CamParams& cp = f.cams[c.cam];
      VertParams p;
      fill_vert(&p, cp.camera, cp.pc_camera, cp.hvdf_off, cp.trans, cp.fog, c.decal);
      memcpy(a.cpu, &p, sizeof(p));
      vu = a.gpu;
    }
    const uint32_t tex_slot = dktexgpu::slot_of(c.tex_id);
    const uint32_t tod_slot = dktexgpu::slot_of(c.tod_id);
    if (tod_slot == UINT32_MAX) {
      continue;
    }
    const bool no_tex = tex_slot == UINT32_MAX;

    for (int pass = 0; pass < (c.afail_second ? 2 : 1); pass++) {
      dkpipe::DrawModeAxes ax;
      ax.depth_test = c.depth_test;
      ax.depth_op = c.depth_test ? (dkpipe::Compare)c.depth_op : dkpipe::Compare::Always;
      ax.depth_write = pass == 0 ? c.depth_write : false;
      ax.blend = (dkpipe::Blend)c.blend;
      ax.clamp_s = c.clamp_s;
      ax.clamp_t = c.clamp_t;
      ax.filt = c.filt;
      ax.mipmap = true;  // setup_tfrag_shader passes mipmap = true
      uint8_t samp = 0;
      const dkpipe::PipelineDesc* desc = dkpipe::find_for_draw_mode(ax, &samp);
      if (desc == nullptr) {
        s_misses[k]++;
        break;
      }
      const uint64_t fu = pass == 0 ? frag_ubo(c.alpha_min, kAlphaMaxNormal, no_tex)
                                    : frag_ubo(kAlphaMinAfailSecond, c.aref_second, no_tex);
      if (fu == 0) {
        s_ubo_fail[k]++;
        break;
      }
      if (desc != bound) {
        dkpipe::record_graphics_state(cb, *desc);
        bound = desc;
      }
      if (c.vert_gpu != bound_vert) {
        dkCmdBufBindVtxBuffer(cb, 0, c.vert_gpu, c.vert_bytes);
        bound_vert = c.vert_gpu;
      }
      if (vu != bound_vubo) {
        dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 7, vu, kUboSize);
        bound_vubo = vu;
      }
      if (fu != bound_fubo) {
        dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 7, fu, kUboSize);
        bound_fubo = fu;
      }
      dkCmdBufBindTexture(cb, DkStage_Vertex, 0, dkMakeTextureHandle(tod_slot, 0));
      dkCmdBufBindTexture(cb, DkStage_Fragment, 0,
                          dkMakeTextureHandle(no_tex ? tod_slot : tex_slot, samp));
      dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, c.idx_gpu);
      dkCmdBufDrawIndexed(cb, c.strips ? DkPrimitive_TriangleStrip : DkPrimitive_Triangles,
                          c.idx_count, 1, 0, 0, 0);
      draws++;
    }
  }
  s_draws[k] = draws;
  s_frames[k]++;
  if (s_frames[k] == 1 || s_frames[k] % 600 == 0) {
    switch_run_logf("[dk] %s frame %llu: cams=%u draws=%u alpha=%u wind=%u tris=%u "
                    "dropped=%u misses=%u ubo_fail=%u ubo_used=%u KB",
                    k == 0 ? "tfrag" : k == 1 ? "shrub" : "tie", (unsigned long long)s_frames[k],
                    (unsigned)f.cams.size(), draws, f.alpha_draws, f.wind_draws, f.tris, f.dropped,
                    s_misses[k], s_ubo_fail[k], ring->used() / 1024);
  }
}

}  // namespace

void record_world(int fb_w, int fb_h) {
  record_list(frame_draws(), fb_w, fb_h, 0);
}

}  // namespace dktf

namespace dkshrub {

void record_world(int fb_w, int fb_h) {
  dktf::record_list(frame_draws(), fb_w, fb_h, 1);
}

}  // namespace dkshrub

namespace dktie {

void record_world(int fb_w, int fb_h) {
  dktf::record_list(frame_draws(), fb_w, fb_h, 2);
}

}  // namespace dktie

namespace dktf {

}  // namespace dktf
#endif  // __SWITCH__
