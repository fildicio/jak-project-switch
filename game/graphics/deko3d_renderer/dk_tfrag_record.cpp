// T60 (AI-assisted): P3 first visible tfrag draw — deko3d half. Records the plain
// dktf::FrameDraws list (built by TFragmentDk.cpp) into the frame command buffer.
// Called from dkdev::present_frame (via set_world_recorder) after the colour + depth
// clear, so it must never touch the cmd buffer's memory or submit anything itself.
#include "game/graphics/deko3d_renderer/TFragmentDk.h"

#if defined(__SWITCH__)
#include <deko3d.h>
#include <string.h>

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

uint64_t s_frames = 0;
uint32_t s_draws = 0, s_misses = 0, s_ubo_fail = 0;

}  // namespace

void record_world(int fb_w, int fb_h) {
  const FrameDraws& f = frame_draws();
  if (f.draws.empty() || !dkdev::tfrag3_shaders_loaded()) {
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
  DkShader vsh = dkdev::tfrag3_vsh();
  DkShader fsh = dkdev::tfrag3_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());
  // PreloadedVertex (32 B): xyz @0, stq @16, color_index u16 @28 (TFragment.cpp L315-335).
  const DkVtxAttribState attribs[] = {
      {0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
      {0, 0, 16, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
      {0, 0, 28, DkVtxAttribSize_1x16, DkVtxAttribType_Uint, 0},
  };
  const DkVtxBufferState vbuf = {32, 0};
  dkCmdBufBindVtxAttribState(cb, attribs, 3);
  dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
  dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX);

  // VertParams: one pair (decal 0/1) per camera, filled on first use.
  uint64_t vert_ubo[8][2] = {};
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
    if (c.cam >= f.cams.size() || c.cam >= 8) {
      continue;
    }
    uint64_t& vu = vert_ubo[c.cam][c.decal ? 1 : 0];
    if (vu == 0) {
      dkring::Alloc a = ring->alloc(kUboSize, 0x100);
      if (!a.ok()) {
        s_ubo_fail++;
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
        s_misses++;
        break;
      }
      const uint64_t fu = pass == 0 ? frag_ubo(c.alpha_min, kAlphaMaxNormal, no_tex)
                                    : frag_ubo(kAlphaMinAfailSecond, c.aref_second, no_tex);
      if (fu == 0) {
        s_ubo_fail++;
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
  s_draws = draws;
  s_frames++;
  if (s_frames == 1 || s_frames % 600 == 0) {
    switch_run_logf("[dk] tfrag frame %llu: cams=%u draws=%u tris=%u dropped=%u misses=%u "
                    "ubo_fail=%u ubo_used=%u KB",
                    (unsigned long long)s_frames, (unsigned)f.cams.size(), draws, f.tris,
                    f.dropped, s_misses, s_ubo_fail, ring->used() / 1024);
  }
}

}  // namespace dktf
#endif  // __SWITCH__
