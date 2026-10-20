// T78 (AI-assisted): deko3d merc2 recorder. Records dkmerc::frame_draws() (built by MercDk.cpp)
// into the frame command buffer. Same rules as dk_tfrag_record.cpp: called from present_frame
// after the world passes, never submits. Params/bones already live in MercDk's GPU ring.
#include "game/graphics/deko3d_renderer/MercDk.h"

#if defined(__SWITCH__)
#include <deko3d.h>

#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"

// switch_run_logf lives in run_log.h, which cannot be included next to <deko3d.h>.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dkmerc {
namespace {

constexpr uint32_t kTexImages = 2048;  // dktexgpu descriptor slots
constexpr uint32_t kParamUbo = 0x100;  // MercVertParams (208 B), padded

// tfrag3::MercVertex (64 B): pos @0, normal @16, weights @32, st @48, rgba u8x4 @56,
// mats u8x3 @60 (GL setup_merc_vao: rgba normalized, mats glVertexAttribIPointer).
const DkVtxAttribState kMercAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 16, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 32, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 48, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    {0, 0, 56, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
    {0, 0, 60, DkVtxAttribSize_3x8, DkVtxAttribType_Uint, 0},
};

uint64_t s_frames = 0;
uint32_t s_misses = 0;

}  // namespace

void record_world(int fb_w, int fb_h) {
  const FrameDraws& f = frame_draws();
  if (f.draws.empty() || !dkdev::merc_shaders_loaded()) {
    return;
  }
  DkCmdBuf cb = dkdev::cmd_handle();
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());

  DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
  dkCmdBufSetViewports(cb, 0, &vp, 1);
  DkShader vsh = dkdev::merc_vsh();
  DkShader fsh = dkdev::merc_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());
  const DkVtxBufferState vbuf = {64, 0};
  dkCmdBufBindVtxAttribState(cb, kMercAttribs, 6);
  dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
  dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX);

  const dkpipe::PipelineDesc* bound = nullptr;
  uint64_t bound_vert = 0, bound_params = 0, bound_bones = 0, bound_frag = 0;
  uint32_t draws = 0;
  for (const DrawCmd& c : f.draws) {
    const uint32_t tex_slot = dktexgpu::slot_of(c.tex_id);
    const uint64_t fu = c.frag < kFragVariants ? f.frag_gpu[c.frag] : 0;
    if (tex_slot == UINT32_MAX || fu == 0) {
      continue;
    }
    dkpipe::DrawModeAxes ax;
    ax.depth_test = c.depth_test;
    ax.depth_op = c.depth_test ? (dkpipe::Compare)c.depth_op : dkpipe::Compare::Always;
    ax.depth_write = c.depth_write;
    ax.blend = (dkpipe::Blend)c.blend;
    ax.clamp_s = c.clamp_s;
    ax.clamp_t = c.clamp_t;
    ax.filt = c.filt;
    ax.mipmap = !c.eye;  // GL do_draws: use_mipmaps_for_filtering is false only for eyes
    uint8_t samp = 0;
    const dkpipe::PipelineDesc* desc = dkpipe::find_for_draw_mode(ax, &samp);
    if (desc == nullptr) {
      s_misses++;
      continue;
    }
    if (desc != bound) {
      dkpipe::record_graphics_state(cb, *desc);
      bound = desc;
    }
    if (c.vert_gpu != bound_vert) {
      dkCmdBufBindVtxBuffer(cb, 0, c.vert_gpu, c.vert_bytes);
      bound_vert = c.vert_gpu;
    }
    if (c.params_gpu != bound_params) {
      dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 7, c.params_gpu, kParamUbo);
      bound_params = c.params_gpu;
    }
    if (c.bones_gpu != bound_bones) {
      dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 8, c.bones_gpu, c.bones_bytes);
      bound_bones = c.bones_gpu;
    }
    if (fu != bound_frag) {
      dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 7, fu, kParamUbo);
      bound_frag = fu;
    }
    dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(tex_slot, samp));
    dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, c.idx_gpu);
    dkCmdBufDrawIndexed(cb, c.strips ? DkPrimitive_TriangleStrip : DkPrimitive_Triangles,
                        c.idx_count, 1, 0, 0, 0);
    draws++;
  }
  s_frames++;
  if (s_frames == 1 || s_frames % 600 == 0) {
    switch_run_logf("[dk] merc frame %llu: models=%u draws=%u/%u tris=%u missing=%u "
                    "skipped=%u mod=%u dropped=%u bad_bones=%u misses=%u",
                    (unsigned long long)s_frames, f.models, draws, (unsigned)f.draws.size(),
                    f.tris, f.missing, f.skipped, f.mod, f.dropped, f.bad_bones, s_misses);
  }
}

}  // namespace dkmerc
#endif  // __SWITCH__
