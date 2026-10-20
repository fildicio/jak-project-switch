// T79 (AI-assisted): deko3d Sprite3 recorder. Records dksprite::frame_draws() (built by
// SpriteDk.cpp) into the frame command buffer after merc. Same rules as dk_merc_record.cpp:
// called from present_frame, never submits. Vertices/indices/params live in SpriteDk's GPU ring.
#include "game/graphics/deko3d_renderer/SpriteDk.h"

#if defined(__SWITCH__)
#include <deko3d.h>

#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"

// switch_run_logf lives in run_log.h, which cannot be included next to <deko3d.h>.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dksprite {
namespace {

constexpr uint32_t kTexImages = 2048;  // dktexgpu descriptor slots

// GL Sprite3::opengl_setup_normal: SpriteVertex3D (64 B). xyz_sx/quat_sy/rgba floats,
// flags_matrix u16x2 and info u16x4 through glVertexAttribIPointer.
const DkVtxAttribState kSpriteAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    {0, 0, 16, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    {0, 0, 32, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    {0, 0, 48, DkVtxAttribSize_2x16, DkVtxAttribType_Uint, 0},
    {0, 0, 52, DkVtxAttribSize_4x16, DkVtxAttribType_Uint, 0},
};

uint64_t s_frames = 0;
uint32_t s_misses = 0;

}  // namespace

void record_world(int fb_w, int fb_h) {
  const FrameDraws& f = frame_draws();
  if (f.draws.empty() || !dkdev::sprite_shaders_loaded()) {
    return;
  }
  DkCmdBuf cb = dkdev::cmd_handle();
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());

  DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
  dkCmdBufSetViewports(cb, 0, &vp, 1);
  DkShader vsh = dkdev::sprite_vsh();
  DkShader fsh = dkdev::sprite_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());
  const DkVtxBufferState vbuf = {64, 0};
  dkCmdBufBindVtxAttribState(cb, kSpriteAttribs, 5);
  dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
  dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX);
  dkCmdBufBindVtxBuffer(cb, 0, f.vert_gpu, f.vert_bytes);
  dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 7, f.params_gpu, kVertParamBytes);

  const dkpipe::PipelineDesc* bound = nullptr;
  uint64_t bound_frag = 0;
  uint32_t draws = 0;
  for (const DrawCmd& c : f.draws) {
    const uint32_t tex_slot = dktexgpu::slot_of(c.tex_id);
    if (tex_slot == UINT32_MAX) {
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
    ax.mipmap = false;  // GL flush_sprites: setup_opengl_from_draw_mode(..., false)
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
    if (c.frag_gpu != bound_frag) {
      dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 7, c.frag_gpu, kFragParamBytes);
      bound_frag = c.frag_gpu;
    }
    dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(tex_slot, samp));
    dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, c.idx_gpu);
    dkCmdBufDrawIndexed(cb, DkPrimitive_TriangleStrip, c.idx_count, 1, 0, 0, 0);
    draws++;
  }
  s_frames++;
  if (s_frames == 1 || s_frames % 600 == 0) {
    switch_run_logf("[dk] sprite frame %llu: sprites=%u draws=%u/%u no_tex=%u dropped=%u "
                    "misses=%u",
                    (unsigned long long)s_frames, f.sprites, draws, (unsigned)f.draws.size(),
                    f.no_tex, f.dropped, s_misses);
  }
}

}  // namespace dksprite
#endif  // __SWITCH__
