// T80 (AI-assisted): deko3d DirectRenderer recorder (jak1 menus / HUD text / subtitles). Records
// dkdirect::frame_draws() (built by DirectDk.cpp) into the frame command buffer after sprites.
// Same rules as dk_sprite_record.cpp: called from present_frame, never submits.
#include "game/graphics/deko3d_renderer/DirectDk.h"

#if defined(__SWITCH__)
#include <cstring>

#include <deko3d.h>

#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/deko3d_renderer/dk_ocean_driver.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"

// switch_run_logf lives in run_log.h, which cannot be included next to <deko3d.h>.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dkdirect {
namespace {

constexpr uint32_t kTexImages = 2048;  // dktexgpu descriptor slots

// GL DirectRenderer constructor: xyzf float4, rgba u8x4 normalized, stq float3,
// tex_unit..fog u8x4 and use_uv u8 through glVertexAttribIPointer, scissor float4.
const DkVtxAttribState kDirectAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    {0, 0, 28, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
    {0, 0, 16, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 32, DkVtxAttribSize_4x8, DkVtxAttribType_Uint, 0},
    {0, 0, 36, DkVtxAttribSize_1x8, DkVtxAttribType_Uint, 0},
    {0, 0, 48, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
};

uint64_t s_frames = 0;
uint32_t s_misses = 0;

}  // namespace

// T88: shared recorder for one FrameDraws half (direct buckets or the sky bucket). Called
// from present_frame, never submits.
void record_draws(const FrameDraws& f, int fb_w, int fb_h, const char* tag, bool mipmap) {
  if (f.draws.empty() || f.consts_cpu == nullptr || !dkdev::direct_shaders_loaded()) {
    return;
  }
  FrameConsts k = {};
  k.game_sizes[0] = 512.f;
  k.game_sizes[1] = 448.f;
  k.game_sizes[2] = (float)fb_w;
  k.game_sizes[3] = (float)fb_h;
  for (int i = 0; i < 4; i++) {
    k.fog_color[i] = f.fog_rgba[i] / 255.f;
  }
  memcpy(f.consts_cpu, &k, sizeof(k));

  DkCmdBuf cb = dkdev::cmd_handle();
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());

  DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
  dkCmdBufSetViewports(cb, 0, &vp, 1);
  DkShader vsh = dkdev::direct_vsh();
  DkShader fsh = dkdev::direct_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());
  const DkVtxBufferState vbuf = {sizeof(Vertex), 0};
  dkCmdBufBindVtxAttribState(cb, kDirectAttribs, 6);
  dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
  dkCmdBufBindVtxBuffer(cb, 0, f.vert_gpu, f.vert_bytes);
  dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 6, f.consts_gpu, sizeof(FrameConsts));

  const dkpipe::PipelineDesc* bound = nullptr;
  uint64_t bound_frag = 0;
  uint32_t draws = 0;
  for (const DrawCmd& c : f.draws) {
    uint32_t tex_slot = UINT32_MAX;
    if (c.tex_id != 0) {
      tex_slot = dktexgpu::slot_of(c.tex_id);
      if (tex_slot == UINT32_MAX) {
        continue;
      }
    }
    dkpipe::DrawModeAxes ax;
    ax.depth_test = c.depth_test;
    ax.depth_op = c.depth_test ? (dkpipe::Compare)c.depth_op : dkpipe::Compare::Always;
    ax.depth_write = c.depth_write;
    ax.blend = (dkpipe::Blend)c.blend;
    ax.clamp_s = c.clamp_s;
    ax.clamp_t = c.clamp_t;
    ax.filt = c.filt;
    ax.mipmap = mipmap && !dkocean::no_mip_sampling();
    // FIX 112: ocean-far samples mip-filtered (GL set_mipmap(true)).
    // OCEAN_VISUAL_BUG (AI-assisted): gk_oceannomip.txt drops the far draws to
    // level 0 as well, so the whole ocean path A/Bs mip filtering in one go.
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
    if (tex_slot != UINT32_MAX) {
      dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(tex_slot, samp));
    }
    dkCmdBufDraw(cb, DkPrimitive_Triangles, c.count, 1, c.first, 0);
    draws++;
  }
  s_frames++;
  if (s_frames == 1 || s_frames % 600 == 0) {
    switch_run_logf("[dk] direct frame %llu (%s): verts=%u draws=%u/%u no_tex=%u dropped=%u misses=%u",
                    (unsigned long long)s_frames, tag, f.verts, draws,
                    (unsigned)f.draws.size(), f.no_tex, f.dropped, s_misses);
  }
}

void record_world(int fb_w, int fb_h) {
  record_draws(frame_draws(), fb_w, fb_h, "direct", false);
}

// T88 (AI-assisted): GL bucket order -- sky (bucket 3) is drawn before tfrag (bucket 6),
// i.e. before dkdirect::record_world in present_frame.
void record_sky_world(int fb_w, int fb_h) {
  record_draws(sky_frame_draws(), fb_w, fb_h, "sky", false);
}

// T95 (AI-assisted): GL bucket order -- the far-ocean draws (bucket 4) come after the
// sky (3) and before tfrag (6); recorded by the ocean pass before dktf::record_world.
void record_ocean_far_world(int fb_w, int fb_h) {
  record_draws(ocean_far_frame_draws(), fb_w, fb_h, "ocean-far", true);
}

}  // namespace dkdirect
#endif  // __SWITCH__
