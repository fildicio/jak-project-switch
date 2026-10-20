// T82 (AI-assisted): deko3d eye recorder. Port of EyeRenderer::run_gpu: every captured eye is
// drawn into its own 128x128 RGBA8 render target (dktexgpu::create_render_target), which merc
// then samples. Recorded inside the world pass before merc; rebinds the frame target after.
#include "game/graphics/deko3d_renderer/EyeDk.h"

#if defined(__SWITCH__)
#include <deko3d.h>

#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"

// switch_run_logf lives in run_log.h, which cannot be included next to <deko3d.h>.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dkeye {
namespace {

constexpr uint32_t kTexImages = 2048;  // dktexgpu descriptor slots

const DkVtxAttribState kEyeAttribs[] = {
    {0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
};

uint32_t s_targets[kNumEyes] = {};
bool s_target_failed = false;
uint64_t s_frames = 0;
uint32_t s_draws = 0;
uint32_t s_misses = 0;

// GL run_gpu: clamp + linear, no mipmaps, no depth.
const dkpipe::PipelineDesc* pipe(dkpipe::Blend blend, uint8_t* samp) {
  dkpipe::DrawModeAxes ax;
  ax.depth_test = false;
  ax.depth_op = dkpipe::Compare::Always;
  ax.depth_write = false;
  ax.blend = blend;
  ax.clamp_s = true;
  ax.clamp_t = true;
  ax.filt = true;
  ax.mipmap = false;
  return dkpipe::find_for_draw_mode(ax, samp);
}

bool ensure_targets() {
  if (s_target_failed) {
    return false;
  }
  if (s_targets[kNumEyes - 1] != 0) {
    return true;
  }
  for (int i = 0; i < kNumEyes; i++) {
    if (s_targets[i] == 0) {
      s_targets[i] = dktexgpu::create_render_target(kEyeTexSize, kEyeTexSize);
      if (s_targets[i] == 0) {
        s_target_failed = true;
        switch_run_logf("[dk] eye render target %d FAILED", i);
        return false;
      }
    }
  }
  switch_run_logf("[dk] eye render targets ready (%d x %dx%d)", kNumEyes, kEyeTexSize,
                  kEyeTexSize);
  return true;
}

}  // namespace

void record_eyes() {
  const FrameDraws& f = frame_draws();
  if (f.eyes.empty() || f.vert_gpu == 0 || !dkdev::eye_shaders_loaded() || !ensure_targets()) {
    return;
  }
  uint8_t samp_off = 0, samp_alpha = 0;
  const dkpipe::PipelineDesc* p_off = pipe(dkpipe::Blend::Off, &samp_off);
  const dkpipe::PipelineDesc* p_alpha = pipe(dkpipe::Blend::StdAlpha, &samp_alpha);
  if (p_off == nullptr || p_alpha == nullptr) {
    s_misses++;
    return;
  }

  DkCmdBuf cb = dkdev::cmd_handle();
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());
  DkShader vsh = dkdev::eye_vsh();
  DkShader fsh = dkdev::eye_fsh();
  DkShader const* sh[] = {&vsh, &fsh};
  dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::sampler_set_count());
  const DkVtxBufferState vbuf = {sizeof(Vertex), 0};
  dkCmdBufBindVtxAttribState(cb, kEyeAttribs, 1);
  dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
  dkCmdBufBindVtxBuffer(cb, 0, f.vert_gpu, f.vert_bytes);
  DkViewport vp = {0.0f, 0.0f, (float)kEyeTexSize, (float)kEyeTexSize, 0.0f, 1.0f};
  dkCmdBufSetViewports(cb, 0, &vp, 1);
  DkScissor sc = {0, 0, (uint32_t)kEyeTexSize, (uint32_t)kEyeTexSize};

  // The source textures of this frame's eyes: GL draws a quad only if its texture exists.
  const dkpipe::PipelineDesc* bound = nullptr;
  for (size_t i = 0; i < f.eyes.size(); i++) {
    const Eye& e = f.eyes[i];
    const uint32_t target = s_targets[e.slot % kNumEyes];
    const DkImage* img = (const DkImage*)dktexgpu::image_of(target);
    if (img == nullptr) {
      continue;
    }
    DkImageView view;
    dkImageViewDefaults(&view, img);
    dkCmdBufBindRenderTarget(cb, &view, nullptr);
    dkCmdBufSetScissors(cb, 0, &sc, 1);
    dkCmdBufClearColorFloat(cb, 0, DkColorMask_RGBA, 1.0f, 0.0f, 0.0f, 0.0f);  // GL debug red
    const uint32_t first = (uint32_t)i * kQuadsPerEye * 4;
    // background (iris texel 0,0) + iris: no blend; pupil: alpha blend; lid: no blend.
    const uint32_t tex[kQuadsPerEye] = {e.iris, e.iris, e.pupil, e.lid};
    for (int q = 0; q < kQuadsPerEye; q++) {
      const uint32_t slot = tex[q] ? dktexgpu::slot_of(tex[q]) : UINT32_MAX;
      if (slot == UINT32_MAX) {
        continue;
      }
      const bool blend = q == 2;
      const dkpipe::PipelineDesc* p = blend ? p_alpha : p_off;
      if (p != bound) {
        dkpipe::record_graphics_state(cb, *p);
        bound = p;
      }
      dkCmdBufBindTexture(cb, DkStage_Fragment, 0,
                          dkMakeTextureHandle(slot, blend ? samp_alpha : samp_off));
      dkCmdBufDraw(cb, DkPrimitive_TriangleStrip, 4, 1, first + (uint32_t)q * 4, 0);
      s_draws++;
    }
    set_eye_tex_id(e.slot, target);
  }

  // Merc samples the targets next: finish the eye fragments and drop stale texture cache lines.
  dkCmdBufBarrier(cb, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
  dkdev::rebind_frame_target();

  s_frames++;
  if (s_frames == 1 || s_frames % 600 == 0) {
    const CaptureStats& st = capture_stats();
    switch_run_logf("[dk] eye frame %llu: eyes=%u quads=%u bad=%u no_tex=%u dropped=%u misses=%u",
                    (unsigned long long)s_frames, (unsigned)f.eyes.size(), s_draws, st.bad,
                    st.no_tex, f.dropped, s_misses);
  }
}

}  // namespace dkeye
#endif  // __SWITCH__
