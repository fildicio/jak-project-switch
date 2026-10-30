// T107 (AI-assisted): dkgl frame hook. See dkgl_frame.h for the capture/record split.
//
// Review note (deviation from the T107 task text, which ran the GL renderers from the
// record sites): by the time the world recorder runs, dk_renderer.cpp has already acked the
// DMA chain ("read the chain BEFORE the ack below -- after it the EE rewrites that memory",
// T60), and the ocean renderers walk that chain (tag kinds, CALL/RET, end tags), so a
// flattened copy would not do either. They therefore run at capture time, exactly where the
// native ocean captured, and their GPU work goes into dkgl's own command lists, which the
// world recorder calls at buckets 4 and 63 (dkCmdBufCallList).
#include "game/graphics/deko3d_renderer/dkgl/dkgl_frame.h"

#if defined(__SWITCH__)

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <vector>

#include <deko3d.h>

#include "common/dma/dma_chain_read.h"
#include "common/util/FileUtil.h"

#include "game/graphics/deko3d_renderer/EyeDk.h"  // #19: dkeye::eye_tex_id for Merc2
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/DepthCue.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/opengl_renderer/ShadowRenderer.h"
#include "game/graphics/opengl_renderer/foreground/Generic2.h"
#include "game/graphics/opengl_renderer/foreground/Generic2BucketRenderer.h"
#include "game/graphics/opengl_renderer/EyeRenderer.h"
#include "game/graphics/opengl_renderer/foreground/Merc2.h"
#include "game/graphics/opengl_renderer/foreground/Merc2BucketRenderer.h"
#include "game/graphics/opengl_renderer/loader/GpuBufferPool.h"
#include "game/graphics/opengl_renderer/opengl_utils.h"
#include "game/graphics/opengl_renderer/ocean/OceanMidAndFar.h"
#include "game/graphics/opengl_renderer/ocean/OceanNear.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

// T103 embedded dksh (game/CMakeLists.txt _DKGL_SHADERS)
#include "dksh/dkgl_depth_cue_frag_dksh.h"  // T112
#include "dksh/dkgl_depth_cue_vert_dksh.h"
#include "dksh/dkgl_direct_basic_frag_dksh.h"  // far ocean (GL DirectRenderer)
#include "dksh/dkgl_direct_basic_textured_frag_dksh.h"
#include "dksh/dkgl_direct_basic_textured_vert_dksh.h"
#include "dksh/dkgl_direct_basic_vert_dksh.h"
#include "dksh/dkgl_emerc_frag_dksh.h"  // TODO #5: merc envmap shine
#include "dksh/dkgl_emerc_vert_dksh.h"
#include "dksh/dkgl_merc2_frag_dksh.h"  // #19: characters (Merc2 normal draws)
#include "dksh/dkgl_merc2_vert_dksh.h"
#include "dksh/dkgl_eye_frag_dksh.h"  // #19: eyes (GL EyeRenderer)
#include "dksh/dkgl_eye_vert_dksh.h"
#include "dksh/dkgl_generic_frag_dksh.h"  // T111
#include "dksh/dkgl_generic_vert_dksh.h"
#include "dksh/dkgl_ocean_common_frag_dksh.h"
#include "dksh/dkgl_ocean_common_vert_dksh.h"
#include "dksh/dkgl_ocean_envmap_frag_dksh.h"
#include "dksh/dkgl_ocean_envmap_haze_frag_dksh.h"
#include "dksh/dkgl_ocean_envmap_haze_vert_dksh.h"
#include "dksh/dkgl_ocean_envmap_vert_dksh.h"
#include "dksh/dkgl_ocean_texture_frag_dksh.h"
#include "dksh/dkgl_ocean_texture_mipmap_frag_dksh.h"
#include "dksh/dkgl_ocean_texture_mipmap_vert_dksh.h"
#include "dksh/dkgl_ocean_texture_vert_dksh.h"
#include "dksh/dkgl_shadow_frag_dksh.h"  // T113
#include "dksh/dkgl_shadow_vert_dksh.h"
#include "dksh/dkgl_solid_color_frag_dksh.h"  // blackout fade (GL FullScreenDraw)
#include "dksh/dkgl_solid_color_vert_dksh.h"

#include "game/switch/run_log.h"

namespace dkgl {
namespace {

// ---- memory: one CPU-visible memblock ----------------------------------------------------
// kSlots frame slots (each: command memory + vertex/index ring + UBO ring) + one persistent
// region (init-time glBufferData). A slot is rewritten kSlots captures later and only after
// the fence signalled by the last frame that called its lists (record_bucket).
constexpr u32 kSlots = 3;
constexpr u32 kCmdBytes = 512 * 1024;
constexpr u32 kCmdSpareBytes = 256 * 1024;   // handed to deko3d if a slot's command memory fills
constexpr u32 kDataBytes = 8 * 1024 * 1024;  // ocean + Generic2 (T111) re-upload per bucket
// one <=512 B slice per draw; #19: 1.5 MB = ocean (~190 KB) + ~2400 merc draws
constexpr u32 kUboBytes = 1536 * 1024;
constexpr u32 kSlotBytes = kCmdBytes + kCmdSpareBytes + kDataBytes + kUboBytes;
constexpr u32 kPersistBytes = 2 * 1024 * 1024;
constexpr u32 kMemBytes = kSlots * kSlotBytes + kPersistBytes;
constexpr u32 kCodeBytes = 64 * 1024;
constexpr u32 kTexImages = 2048;  // dktexgpu descriptor slots (dk_direct_record.cpp)

struct Ring {
  u32 base = 0;  // offset into s_mem
  u32 size = 0;
  u32 used = 0;
  u32 high = 0;
  u32 fails = 0;
};

bool s_active = false;
DkMemBlock s_mem = nullptr;
DkMemBlock s_code = nullptr;
u8* s_cpu = nullptr;
DkGpuAddr s_gpu = 0;
DkCmdBuf s_cb = nullptr;
DkFence s_fence[kSlots];
bool s_fence_armed[kSlots] = {};
Ring s_data[kSlots], s_ubo[kSlots], s_persist;
u32 s_slot = 0;          // slot being recorded / last recorded
u64 s_captures = 0;
bool s_recording = false;
bool s_spare_used = false;
u64 s_cmd_overflows = 0;
int s_fb_w = 1280, s_fb_h = 720;

// #19 eye probe (AI-assisted), gated by gk_dkgl_eyeprobe.txt on the SD: after the eye
// bucket replayed, copy ALL 40 eye slots into a CPU buffer and log a per-slot content map
// two frames later ('#' content, '+' partial, '.' black, 'x' no image). Black NPC slots +
// Jak's working means the eye pass skips those slots; content everywhere means Merc2's
// sampling is the suspect. EyeRenderer::run_gpu also logs which iris/pupil/lid source
// textures the pool missed (with their tbps) during the probe window -- skipped quads
// leave the slot black because glClearBufferfv is not implemented in dkgl, so the
// pristine image is black, not red.
dkpool::PoolBuffer s_eyeprobe_buf;
DkFence s_eyeprobe_fence;
bool s_eyeprobe_armed = false;
bool s_eyeprobe_on = false;
bool s_eyeprobe_have[40] = {};
int s_src_probe_frames = 0;  // capture frames in which run_gpu logs source misses
int s_src_probe_budget = 0;  // max miss lines per window
u64 s_eyeprobe_at = 0, s_eyeprobe_last = 0, s_eyeprobe_rounds = 0;

struct Segment {
  DkCmdList list = 0;
  bool frame_target = false;  // the caller re-binds the frame target before calling it
  // T112: a blit segment has no list -- the frame image is only known at record time, so
  // record_bucket copies frame -> blit_dst_tex there (glBlitFramebuffer in DepthCue).
  bool blit = false;
  u32 blit_dst_tex = 0;
  DkImageRect blit_src{}, blit_dst{};
  bool blit_linear = false;
};
struct BucketLists {
  std::vector<Segment> segs;
  bool valid = false;
};
// jak1 buckets run through dkgl, in GL bucket order (OpenGLRenderer.cpp
// init_bucket_renderers_jak1): 4 ocean-mid-far, the 10 Generic2 buckets (T111), 47 shadow
// (T113), 63 ocean-near, 64 depth-cue (T112). kBlackoutList is not a GOAL bucket: it is the
// pmode_alp fade that OpenGLRenderer::do_pcrtc_effects draws over the finished frame.
// TODO #5 / #19: the 8 merc buckets (10 17 45 49 52 55 58 61, MercDk.h kJak1Buckets) run the
// GL Merc2: characters (merc2) and their shine (emerc). gk_dkgl_nativemerc.txt makes the
// merc2 program inert, so only emerc reaches the GPU and the native MercDk draws characters.
// #19: bucket 54 "common-pris-eyes" runs the GL EyeRenderer, so Merc2 samples eye textures
// through its normal eye_renderer lookup (gk_dkgl_nativeeyes.txt: native deko3d eye pass).
constexpr int kBlackoutList = 1000;
// 48 / 51 (pris-tex) only run the level NPC eyes found there (PrisTexEyes, #19).
constexpr int kBuckets[] = {4,  10, 11, 17, 18, 24, 30, 45, 46, 47, 48, 49, 50, 51,
                            52, 53, 54, 55, 56, 58, 59, 61, 62, 63, 64, kBlackoutList};
constexpr int kNumBuckets = sizeof(kBuckets) / sizeof(kBuckets[0]);
BucketLists s_lists[kNumBuckets];
int list_index(int bucket) {
  for (int i = 0; i < kNumBuckets; ++i) {
    if (kBuckets[i] == bucket) {
      return i;
    }
  }
  return -1;
}
int s_cur_bucket = -1;
bool s_seg_open = false;
bool s_seg_frame = false;
u32 s_color_mask = DkColorMask_RGBA;  // T113: last glColorMask emitted (segments re-apply it)

bool ring_take(Ring& r, u32 bytes, u32 align, void** cpu, u64* gpu) {
  const u32 a = align ? align : 1;
  const u32 off = (r.used + a - 1) & ~(a - 1);
  if (off + bytes > r.size) {
    r.fails++;
    return false;
  }
  r.used = off + bytes;
  r.high = r.used > r.high ? r.used : r.high;
  *cpu = s_cpu + r.base + off;
  *gpu = s_gpu + r.base + off;
  return true;
}

// deko3d asks for more command memory when a slot's region is full: give it the slot's
// spare region once, then stop recording for the frame (lists stay valid, the frame just
// loses the rest of the ocean).
void cmd_add_mem(void*, DkCmdBuf cb, size_t min_size) {
  s_cmd_overflows++;
  if (!s_spare_used && min_size <= kCmdSpareBytes) {
    s_spare_used = true;
    dkCmdBufAddMemory(cb, s_mem, s_slot * kSlotBytes + kCmdBytes, kCmdSpareBytes);
    return;
  }
  // No memory left: give it the spare again would corrupt the lists. Log; the caller
  // notices s_cmd_overflows and drops this frame's lists.
  switch_run_logf("[dkgl] command memory exhausted (need %u B)", (unsigned)min_size);
  dkCmdBufAddMemory(cb, s_mem, s_slot * kSlotBytes + kCmdBytes, kCmdSpareBytes);
}

// ---- shaders: own code memblock, same .dksh layout as dk_device.cpp load_shader --------
struct DkshHeader {
  uint32_t magic;
  uint32_t header_sz;
  uint32_t control_sz;
  uint32_t code_sz;
  uint32_t programs_off;
  uint32_t num_programs;
};

bool load_shader(DkShader* out, const uint8_t* blob, uint32_t size, uint32_t* offset) {
  if (size < sizeof(DkshHeader)) {
    return false;
  }
  DkshHeader hdr;
  memcpy(&hdr, blob, sizeof(hdr));
  if (hdr.control_sz + hdr.code_sz > size) {
    return false;
  }
  *offset = (*offset + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
  if (*offset + hdr.code_sz > kCodeBytes) {
    switch_run_logf("[dkgl] shader code pool full (%u + %u > %u)", *offset, hdr.code_sz,
                    kCodeBytes);
    return false;
  }
  memcpy((uint8_t*)dkMemBlockGetCpuAddr(s_code) + *offset, blob + hdr.control_sz, hdr.code_sz);
  DkShaderMaker sm;
  dkShaderMakerDefaults(&sm, s_code, *offset);
  sm.control = blob;
  sm.programId = 0;
  dkShaderInitialize(out, &sm);
  if (!dkShaderIsValid(out)) {
    return false;
  }
  *offset += hdr.code_sz;
  return true;
}

struct ProgramShaders {
  const char* name;
  const uint8_t* vert;
  uint32_t vert_size;
  const uint8_t* frag;
  uint32_t frag_size;
  DkShader vsh, fsh;
  bool ok;
};
ProgramShaders s_programs[] = {
    {"ocean_common", dkgl_ocean_common_vert_dksh, dkgl_ocean_common_vert_dksh_size,
     dkgl_ocean_common_frag_dksh, dkgl_ocean_common_frag_dksh_size, {}, {}, false},
    {"ocean_envmap", dkgl_ocean_envmap_vert_dksh, dkgl_ocean_envmap_vert_dksh_size,
     dkgl_ocean_envmap_frag_dksh, dkgl_ocean_envmap_frag_dksh_size, {}, {}, false},
    {"ocean_envmap_haze", dkgl_ocean_envmap_haze_vert_dksh,
     dkgl_ocean_envmap_haze_vert_dksh_size, dkgl_ocean_envmap_haze_frag_dksh,
     dkgl_ocean_envmap_haze_frag_dksh_size, {}, {}, false},
    {"ocean_texture", dkgl_ocean_texture_vert_dksh, dkgl_ocean_texture_vert_dksh_size,
     dkgl_ocean_texture_frag_dksh, dkgl_ocean_texture_frag_dksh_size, {}, {}, false},
    {"ocean_texture_mipmap", dkgl_ocean_texture_mipmap_vert_dksh,
     dkgl_ocean_texture_mipmap_vert_dksh_size, dkgl_ocean_texture_mipmap_frag_dksh,
     dkgl_ocean_texture_mipmap_frag_dksh_size, {}, {}, false},
    {"generic", dkgl_generic_vert_dksh, dkgl_generic_vert_dksh_size, dkgl_generic_frag_dksh,
     dkgl_generic_frag_dksh_size, {}, {}, false},  // T111
    {"shadow", dkgl_shadow_vert_dksh, dkgl_shadow_vert_dksh_size, dkgl_shadow_frag_dksh,
     dkgl_shadow_frag_dksh_size, {}, {}, false},  // T113
    {"depth_cue", dkgl_depth_cue_vert_dksh, dkgl_depth_cue_vert_dksh_size,
     dkgl_depth_cue_frag_dksh, dkgl_depth_cue_frag_dksh_size, {}, {}, false},  // T112
    {"direct_basic", dkgl_direct_basic_vert_dksh, dkgl_direct_basic_vert_dksh_size,
     dkgl_direct_basic_frag_dksh, dkgl_direct_basic_frag_dksh_size, {}, {}, false},
    {"direct_basic_textured", dkgl_direct_basic_textured_vert_dksh,
     dkgl_direct_basic_textured_vert_dksh_size, dkgl_direct_basic_textured_frag_dksh,
     dkgl_direct_basic_textured_frag_dksh_size, {}, {}, false},  // far ocean
    {"solid_color", dkgl_solid_color_vert_dksh, dkgl_solid_color_vert_dksh_size,
     dkgl_solid_color_frag_dksh, dkgl_solid_color_frag_dksh_size, {}, {}, false},  // blackout
    {"emerc", dkgl_emerc_vert_dksh, dkgl_emerc_vert_dksh_size, dkgl_emerc_frag_dksh,
     dkgl_emerc_frag_dksh_size, {}, {}, false},  // TODO #5: merc envmap shine
    {"merc2", dkgl_merc2_vert_dksh, dkgl_merc2_vert_dksh_size, dkgl_merc2_frag_dksh,
     dkgl_merc2_frag_dksh_size, {}, {}, false},  // #19: characters
    {"eye", dkgl_eye_vert_dksh, dkgl_eye_vert_dksh_size, dkgl_eye_frag_dksh,
     dkgl_eye_frag_dksh_size, {}, {}, false},  // #19: eyes
};

const ProgramShaders* find_program(const char* name) {
  for (const auto& p : s_programs) {
    if (p.ok && strcmp(p.name, name) == 0) {
      return &p;
    }
  }
  return nullptr;
}

// ---- segments ------------------------------------------------------------------------
// GPU state that the dkgl diff cache never emits: bound once per segment (natives may have
// changed it in between, and the cache is reset per bucket anyway).
void emit_fixed_state(DkCmdBuf cb) {
  const DkGpuAddr data = dkMemBlockGetGpuAddr(dkdev::data_mem());
  dkCmdBufBindImageDescriptorSet(cb, data + dkdev::desc_off() + 0x100, kTexImages);
  dkCmdBufBindSamplerDescriptorSet(cb, data + dkdev::sampler_set_off(),
                                   dkdev::dkgl_sampler_slot(15) + 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;  // GL default: culling off (the ocean never enables it)
  dkCmdBufBindRasterizerState(cb, &rs);
  // DkBlendState has no enable bit: dkgl encodes "blend off" as ONE/ZERO, so blending stays
  // enabled for target 0 here (dkgl_draw.cpp header note).
  DkColorState cs;
  dkColorStateDefaults(&cs);
  dkColorStateSetBlendEnable(&cs, 0, true);
  dkCmdBufBindColorState(cb, &cs);
  DkColorWriteState cw;
  dkColorWriteStateDefaults(&cw);
  dkColorWriteStateSetMask(&cw, 0, s_color_mask);  // T113: keep the diff cache truthful
  dkCmdBufBindColorWriteState(cb, &cw);
  DkMultisampleState ms;
  dkMultisampleStateDefaults(&ms);
  dkCmdBufBindMultisampleState(cb, &ms);
}

void open_segment(bool frame_target) {
  s_seg_open = true;
  s_seg_frame = frame_target;
  emit_fixed_state(s_cb);
}

void close_segment() {
  if (!s_seg_open || s_cur_bucket < 0) {
    return;
  }
  Segment seg;
  seg.list = dkCmdBufFinishList(s_cb);
  seg.frame_target = s_seg_frame;
  s_lists[s_cur_bucket].segs.push_back(seg);
  s_seg_open = false;
}

// ---- the Switch emitter --------------------------------------------------------------
class SwitchEmitter final : public Emitter {
 public:
  void bind_shaders(u32, const char* name) override {
    if (!s_recording) {
      return;
    }
    const ProgramShaders* p = find_program(name);
    if (!p) {
      return;  // unknown programs are inert upstream; nothing to bind
    }
    DkShader const* sh[] = {&p->vsh, &p->fsh};
    dkCmdBufBindShaders(s_cb, DkStageFlag_GraphicsMask, sh, 2);
  }
  void set_depth(const DkDepthStencilState& st) override {
    if (s_recording) {
      dkCmdBufBindDepthStencilState(s_cb, &st);
    }
  }
  void set_blend(const DkBlendState& st) override {
    if (s_recording) {
      dkCmdBufBindBlendStates(s_cb, 0, &st, 1);
    }
  }
  void set_viewport(const DkViewport& vp) override {
    if (s_recording) {
      dkCmdBufSetViewports(s_cb, 0, &vp, 1);
    }
  }
  void set_vtx_format(const DkVtxAttribState* attribs, u32 n_attribs, const DkVtxBufferState* bufs,
                      const u64* buf_gpu, const u32* buf_bytes, u32 n_bufs) override {
    if (!s_recording) {
      return;
    }
    dkCmdBufBindVtxAttribState(s_cb, attribs, n_attribs);
    dkCmdBufBindVtxBufferState(s_cb, bufs, n_bufs);
    for (u32 b = 0; b < n_bufs; ++b) {
      dkCmdBufBindVtxBuffer(s_cb, b, buf_gpu[b], buf_bytes[b]);
    }
  }
  void bind_idx_buffer(u64 gpu, DkIdxFormat fmt) override {
    m_idx_fmt = fmt;
    if (s_recording) {
      dkCmdBufBindIdxBuffer(s_cb, fmt, gpu);
    }
  }
  void bind_ubo(DkStage stage, u32 slot, u64 gpu, u32 bytes) override {
    if (s_recording) {
      dkCmdBufBindUniformBuffer(s_cb, stage, slot, gpu, (bytes + 15u) & ~15u);
    }
  }
  void bind_tex(u32 binding, u32 dk_tex_id, u32 combo) override {
    if (!s_recording) {
      return;
    }
    const u32 image = dk_tex_id ? dktexgpu::slot_of(dk_tex_id) : 0;
    dkCmdBufBindTexture(s_cb, DkStage_Fragment, binding,
                        dkMakeTextureHandle(image, dkdev::dkgl_sampler_slot(combo)));
  }
  void set_blend_color(const float rgba[4]) override {
    if (s_recording) {
      dkCmdBufSetBlendConst(s_cb, rgba[0], rgba[1], rgba[2], rgba[3]);
    }
  }
  void set_stencil(u8 write_mask, u8 ref, u8 func_mask) override {
    if (s_recording) {
      dkCmdBufSetStencil(s_cb, DkFace_FrontAndBack, write_mask, ref, func_mask);
    }
  }
  void set_color_mask(u32 mask) override {
    s_color_mask = mask & DkColorMask_RGBA;
    if (s_recording) {
      DkColorWriteState cw;
      dkColorWriteStateDefaults(&cw);
      dkColorWriteStateSetMask(&cw, 0, s_color_mask);
      dkCmdBufBindColorWriteState(s_cb, &cw);
    }
  }
  void blit_frame_to_tex(u32 dk_tex_id, const DkImageRect& src, const DkImageRect& dst,
                         bool linear) override {
    if (!s_recording || s_cur_bucket < 0) {
      return;
    }
    // end the list, queue the blit, continue on the same target (the 2D engine does not
    // touch the 3D render-target binding)
    const bool frame = s_seg_frame;
    close_segment();
    Segment seg;
    seg.blit = true;
    seg.blit_dst_tex = dk_tex_id;
    seg.blit_src = src;
    seg.blit_dst = dst;
    seg.blit_linear = linear;
    s_lists[s_cur_bucket].segs.push_back(seg);
    open_segment(frame);
  }
  void set_primitive_restart(bool enable, u32 index) override {
    if (s_recording) {
      dkCmdBufSetPrimitiveRestart(s_cb, enable, index);
    }
  }
  void bind_frame_target() override {
    if (!s_recording) {
      return;
    }
    // the frame's colour+depth views are only known in the world recorder: end this list,
    // the caller re-binds the frame target before calling the next one
    close_segment();
    open_segment(true);
    m_offscreen = false;
  }
  void bind_offscreen_target(u32 dk_tex_id, u32 level) override {
    if (!s_recording) {
      return;
    }
    const DkImage* img = (const DkImage*)dktexgpu::image_of(dk_tex_id);
    if (!img) {
      return;
    }
    DkImageView view;
    dkImageViewDefaults(&view, img);
    view.mipLevelOffset = (uint8_t)level;
    view.mipLevelCount = 1;
    dkCmdBufBindRenderTarget(s_cb, &view, nullptr);
    DkScissor sc = {0, 0, 4096, 4096};  // the target clips; GL has no scissor here
    dkCmdBufSetScissors(s_cb, 0, &sc, 1);
    m_offscreen = true;
  }
  void clear(bool color, bool depth, const float rgba[4]) override {
    if (!s_recording) {
      return;
    }
    // The ocean only clears its offscreen targets (colour, no depth attachment). A clear of
    // the frame target would wipe the native passes drawn before it: never emitted.
    if (!m_offscreen) {
      if (!m_logged_frame_clear) {
        switch_run_logf("[dkgl] glClear on the frame target ignored (color=%d depth=%d)",
                        (int)color, (int)depth);
        m_logged_frame_clear = true;
      }
      return;
    }
    if (color) {
      dkCmdBufClearColorFloat(s_cb, 0, DkColorMask_RGBA, rgba[0], rgba[1], rgba[2], rgba[3]);
    }
  }
  void barrier_before_sample() override {
    if (s_recording) {
      dkCmdBufBarrier(s_cb, DkBarrier_Fragments,
                      DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    }
  }
  bool ring_alloc(u32 bytes, u32 align, void** cpu, u64* gpu) override {
    return ring_take(s_data[s_slot], bytes, align, cpu, gpu);
  }
  bool persistent_alloc(u32 bytes, u32 align, void** cpu, u64* gpu) override {
    return ring_take(s_persist, bytes, align, cpu, gpu);
  }
  bool ubo_alloc(u32 bytes, void** cpu, u64* gpu) override {
    return ring_take(s_ubo[s_slot], bytes, DK_UNIFORM_BUF_ALIGNMENT, cpu, gpu);
  }
  void draw_arrays(DkPrimitive prim, u32 first, u32 count) override {
    if (s_recording) {
      dkCmdBufDraw(s_cb, prim, count, 1, first, 0);
    }
  }
  void draw_elements(DkPrimitive prim, u32 count, u32 index_offset_bytes) override {
    if (s_recording) {
      const u32 isz = m_idx_fmt == DkIdxFormat_Uint16 ? 2 : 4;
      dkCmdBufDrawIndexed(s_cb, prim, count, 1, index_offset_bytes / isz, 0, 0);
    }
  }

  void bucket_start() { m_offscreen = false; }
  void frame_target_bound() { m_offscreen = false; }

 private:
  DkIdxFormat m_idx_fmt = DkIdxFormat_Uint32;
  bool m_offscreen = false;
  bool m_logged_frame_clear = false;
};

SwitchEmitter s_emitter;

// ---- GL renderers + shared state ---------------------------------------------------------
std::unique_ptr<SharedRenderState> s_srs;
std::unique_ptr<OceanMidAndFar> s_mid_far;
std::unique_ptr<OceanNear> s_near;
std::shared_ptr<Generic2> s_generic2;                                // T111
std::unique_ptr<Generic2BucketRenderer> s_generic_buckets[kNumBuckets];  // by list index
std::unique_ptr<ShadowRenderer> s_shadow;  // T113
std::unique_ptr<DepthCue> s_depth_cue;     // T112
std::shared_ptr<Merc2> s_merc2;            // TODO #5 / #19 (see kBuckets)
bool s_merc_native = false;                // #19: gk_dkgl_nativemerc.txt -> MercDk draws
std::unique_ptr<EyeRenderer> s_eyes;       // #19: GL eyes, bucket 54
std::unique_ptr<Merc2BucketRenderer> s_merc_buckets[kNumBuckets];  // by list index

// #19 (AI-assisted): jak1 eye.gc puts each level's NPC eyes in that level's pris-tex bucket
// (48 / 51), right after the page holding their iris/pupil/lid; only common characters (Jak,
// Daxter) use bucket 54. GL's TextureUploadHandler spots that eye DMA (qwc 8) and runs
// EyeRenderer::handle_eye_dma2 there. The upload replay (SpriteDk walk_tex_bucket) already
// did this bucket's uploads, so this is TextureUploadHandler::render minus the uploads.
class PrisTexEyes : public BucketRenderer {
 public:
  PrisTexEyes(const std::string& name, int my_id) : BucketRenderer(name, my_id) {}
  void render(DmaFollower& dma, SharedRenderState* render_state,
              ScopedProfilerNode& prof) override {
    for (int guard = 0; guard < 100000 && !dma.ended() &&
                        dma.current_tag_offset() != render_state->next_bucket;
         guard++) {
      auto dma_tag = dma.current_tag();
      if (dma_tag.qwc == (128 / 16)) {
        render_state->eye_renderer->handle_eye_dma2(dma, render_state, prof);
      }
      auto data = dma.read_and_advance();
      if (data.size_bytes == 0 && data.vif0() == 0 && data.vif1() == 0) {
        continue;
      }
      if (dma_tag.kind == DmaTag::Kind::CALL) {
        dma.read_and_advance();  // call
        dma.read_and_advance();  // cnt
        dma.read_and_advance();  // ret
      }
    }
  }
  void draw_debug_window() override {}
};
std::unique_ptr<PrisTexEyes> s_pris_tex_eyes[2];  // buckets 48, 51
// Merc2 reads m_anim_slot_array->at(slot) for animated textures (jak2+ only; jak1 never uses
// them). Zeros = "no texture", sized past jak1's slot count so at() never throws.
const std::vector<GLuint> s_no_anim_slots(256, 0);
std::unique_ptr<FullScreenDraw> s_blackout;  // pmode_alp fade (OpenGLRenderer m_blackout_renderer)
std::atomic<float> s_pmode_alp{1.f};         // GOAL set_pmode_alp, written on the EE thread
BucketRenderer* s_renderers[kNumBuckets] = {};
u64 s_dma_mismatch = 0;
u64 s_last_log_capture = 0;
u64 s_eye_draws = 0, s_interleaved_frames = 0;
// capture_begin/until/end state (#16/#19)
bool s_cap_open = false;
const void* s_cap_chain = nullptr;
u32 s_cap_base = 0;
int s_cap_next = 0;
u64 s_cap_overflows_before = 0;

// TODO #5: Loader buffer ids (lev->merc_vertices / merc_indices) are dkpool chunk ids
bool loader_buffer(u32 name, BufferObj* out) {
  const dkpool::PoolBuffer* pb = dk_loader_buffer_find(name);
  if (!pb || !pb->ok()) {
    return false;
  }
  out->cpu = pb->cpu;
  out->gpu = pb->gpu;
  out->bytes = pb->capacity;
  out->valid = true;
  return true;
}

bool flag(const char* name) {
  char path[96];
  snprintf(path, sizeof(path), "sdmc:/switch/jak1/%s", name);
  return access(path, F_OK) == 0;
}

void run_bucket(BucketRenderer* r, const void* data, u32 buckets_base, int bucket, int list_idx) {
  s_cur_bucket = list_idx;
  s_lists[list_idx].segs.clear();
  s_lists[list_idx].valid = false;
  begin_bucket();
  s_emitter.bucket_start();
  // OpenGLRenderer::setup_frame: glViewport(0, 0, game_res_w, game_res_h) before dispatch;
  // the frame target is FBO 0 at every bucket boundary.
  viewport(0, 0, s_fb_w, s_fb_h);
  if (bound_framebuffer() != 0) {
    bind_framebuffer(0);
  }
  pending_target_consumed();  // the caller binds the frame target before the first list
  if (bucket == 64) {
    // T112: DepthCue never sets blend / depth test itself; on GL it inherits what
    // CommonOceanRenderer::flush_near left (ocean-near 63 runs right before). Here 63 is
    // native deko3d, so recreate that state: blend on, depth test GEQUAL.
    enable(gl::BLEND);
    enable(gl::DEPTH_TEST);
    depth_func(gl::GEQUAL);
  }
  s_color_mask = DkColorMask_RGBA;  // begin_bucket reset the cache: start from GL defaults
  open_segment(true);
  const u32 slot = buckets_base + 16 * (u32)bucket;
  s_srs->next_bucket = slot + 16;
  DmaFollower dma(data, slot);
  {
    ProfilerNode node(r->name_and_id());
    ScopedProfilerNode prof(&node);
    r->render(dma, s_srs.get(), prof);
  }
  restore_bucket_end_state();  // T113: no colour mask / stencil leaks into native passes
  if (dma.current_tag_offset() != s_srs->next_bucket) {
    s_dma_mismatch++;  // OpenGLRenderer asserts here; dkgl only counts (logged per 10 s)
  }
  close_segment();
  s_lists[list_idx].valid = true;
  s_cur_bucket = -1;
}

// OpenGLRenderer::do_pcrtc_effects: when alp < 1, a black quad with alpha 1 - alp goes over
// the whole frame (the game fades to black for loads and cutscene cuts).
void run_blackout(float alp) {
  const int idx = list_index(kBlackoutList);
  s_lists[idx].segs.clear();
  s_lists[idx].valid = false;
  if (!s_blackout || !(alp < 1.f)) {
    return;
  }
  s_cur_bucket = idx;
  begin_bucket();
  s_emitter.bucket_start();
  viewport(0, 0, s_fb_w, s_fb_h);
  if (bound_framebuffer() != 0) {
    bind_framebuffer(0);
  }
  pending_target_consumed();
  disable(gl::DEPTH_TEST);
  enable(gl::BLEND);
  blend_func_separate(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA, gl::ONE, gl::ZERO);
  blend_equation(gl::FUNC_ADD);
  s_color_mask = DkColorMask_RGBA;
  open_segment(true);
  {
    ProfilerNode node("blackout");
    ScopedProfilerNode prof(&node);
    s_blackout->draw(math::Vector4f(0, 0, 0, 1.f - alp), s_srs.get(), prof);
  }
  restore_bucket_end_state();
  close_segment();
  s_lists[idx].valid = true;
  s_cur_bucket = -1;
}

}  // namespace

void set_pmode_alp(float alp) {
  s_pmode_alp.store(alp, std::memory_order_relaxed);
}

bool active() {
  return s_active;
}

bool merc_active() {
  return s_active && s_merc2 && !s_merc_native;
}

bool eyes_active() {
  return s_active && s_eyes != nullptr;
}

void note_interleaved_frame() {
  s_interleaved_frames++;
}

void set_frame_size(int w, int h) {
  if (w > 0 && h > 0) {
    s_fb_w = w;
    s_fb_h = h;
  }
}

bool init(TexturePool* pool, Loader* loader) {
  if (s_active) {
    return true;
  }
  if (g_game_version != GameVersion::Jak1) {
    return false;
  }
  if (flag("gk_dkocean_native.txt")) {
    switch_run_logf("[dkgl] gk_dkocean_native.txt: native dk ocean kept");
    return false;
  }
  if (!pool || !loader || !dkdev::is_up() || !dktexgpu::active()) {
    switch_run_logf("[dkgl] init skipped: dk device/texture store/pool not up");
    return false;
  }
  // ShaderLibrary reads every GL shader file (content unused: identity = name hint, behavior
  // = the embedded dksh); without the folder on the SD it dies before that matters.
  if (!fs::exists(file_util::get_file_path({Shader::shader_folder, "ocean_common.vert"}))) {
    switch_run_logf("[dkgl] init skipped: GL shader folder missing on SD (re-run sd-sync)");
    return false;
  }

  DkDevice dev = dkdev::device_handle();
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, dev, (kMemBytes + DK_MEMBLOCK_ALIGNMENT - 1) &
                                        ~(DK_MEMBLOCK_ALIGNMENT - 1));
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  s_mem = dkMemBlockCreate(&mm);
  dkMemBlockMakerDefaults(&mm, dev, kCodeBytes);
  mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
  s_code = dkMemBlockCreate(&mm);
  if (!s_mem || !s_code) {
    switch_run_logf("[dkgl] init failed: memblocks (%u KB + %u KB)", kMemBytes / 1024,
                    kCodeBytes / 1024);
    return false;
  }
  s_cpu = (u8*)dkMemBlockGetCpuAddr(s_mem);
  s_gpu = dkMemBlockGetGpuAddr(s_mem);
  for (u32 i = 0; i < kSlots; ++i) {
    const u32 base = i * kSlotBytes;
    s_data[i] = Ring{base + kCmdBytes + kCmdSpareBytes, kDataBytes, 0, 0, 0};
    s_ubo[i] = Ring{base + kCmdBytes + kCmdSpareBytes + kDataBytes, kUboBytes, 0, 0, 0};
  }
  s_persist = Ring{kSlots * kSlotBytes, kPersistBytes, 0, 0, 0};
  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, dev);
  cm.cbAddMem = cmd_add_mem;
  s_cb = dkCmdBufCreate(&cm);

  u32 code_off = 0;
  int loaded = 0;
  for (auto& p : s_programs) {
    p.ok = load_shader(&p.vsh, p.vert, p.vert_size, &code_off) &&
           load_shader(&p.fsh, p.frag, p.frag_size, &code_off);
    loaded += p.ok ? 1 : 0;
    if (!p.ok) {
      switch_run_logf("[dkgl] shader %s failed to load", p.name);
    }
  }
  if (loaded != (int)(sizeof(s_programs) / sizeof(s_programs[0]))) {
    return false;
  }

  // GL side: the glad overrides (after the census stubs), the emitter, then the unchanged
  // GL objects. Outside a frame, glBufferData goes to persistent storage.
  install_glad();
  // #19: decide before ShaderLibrary links the programs (SharedRenderState below)
  s_merc_native = flag("gk_dkgl_nativemerc.txt") || flag("gk_dkgl_noemerc.txt");
  force_program_inert(s_merc_native ? "merc2" : nullptr);
  set_emitter(&s_emitter);
  set_external_buffer_lookup(loader_buffer);
  frame_end();
  // dk_renderer owns the pool and loader (unique_ptr); SharedRenderState wants shared_ptr.
  std::shared_ptr<TexturePool> pool_ref(pool, [](TexturePool*) {});
  std::shared_ptr<Loader> loader_ref(loader, [](Loader*) {});
  s_srs = std::make_unique<SharedRenderState>(pool_ref, loader_ref, GameVersion::Jak1);
  // OpenGLRenderer.cpp jak1: bucket 4 "ocean-mid-far", bucket 63 "ocean-near".
  s_mid_far = std::make_unique<OceanMidAndFar>("ocean-mid-far", 4);
  s_near = std::make_unique<OceanNear>("ocean-near", 63);
  s_mid_far->init_shaders(s_srs->shaders);
  s_near->init_shaders(s_srs->shaders);
  s_mid_far->init_textures(*pool, GameVersion::Jak1);
  s_near->init_textures(*pool, GameVersion::Jak1);
  s_renderers[list_index(4)] = s_mid_far.get();
  s_renderers[list_index(63)] = s_near.get();
  // T111: one shared Generic2, ten bucket renderers (names as OpenGLRenderer registers them)
  if (!flag("gk_dkgl_nogeneric.txt")) {
    s_generic2 = std::make_shared<Generic2>(s_srs->shaders);
    const struct {
      int bucket;
      const char* name;
    } kGeneric[] = {{11, "l0-tfrag-generic"},    {18, "l1-tfrag-generic"},
                    {24, "l0-shrub-generic"},    {30, "l1-shrub-generic"},
                    {46, "common-alpha-generic"}, {50, "l0-pris-generic"},
                    {53, "l1-pris-generic"},     {56, "common-pris-generic"},
                    {59, "l0-water-generic"},    {62, "l1-water-generic"}};
    for (const auto& g : kGeneric) {
      const int i = list_index(g.bucket);
      s_generic_buckets[i] = std::make_unique<Generic2BucketRenderer>(
          g.name, g.bucket, s_generic2, Generic2::Mode::NORMAL);
      s_generic_buckets[i]->init_shaders(s_srs->shaders);
      s_generic_buckets[i]->init_textures(*pool, GameVersion::Jak1);
      s_renderers[i] = s_generic_buckets[i].get();
    }
  }
  // T113 shadow (bucket 47) + T112 depth-cue (bucket 64), OpenGLRenderer.cpp jak1 names.
  // Opt-out flags for hardware A/B: gk_dkgl_noshadow.txt / gk_dkgl_nodepthcue.txt.
  if (!flag("gk_dkgl_noshadow.txt")) {
    s_shadow = std::make_unique<ShadowRenderer>("shadow", 47);
    s_renderers[list_index(47)] = s_shadow.get();
  }
  if (!flag("gk_dkgl_nodepthcue.txt")) {
    s_depth_cue = std::make_unique<DepthCue>("depth-cue", 64);
    s_renderers[list_index(64)] = s_depth_cue.get();
  }
  // TODO #5 / #19: GL Merc2 for characters + shine. A/B flags: gk_dkgl_nativemerc.txt (native
  // characters, dkgl shine), gk_dkgl_noemerc.txt (native characters, no shine).
  if (!flag("gk_dkgl_noemerc.txt")) {
    s_merc2 = std::make_shared<Merc2>(s_srs->shaders, &s_no_anim_slots);
    if (!s_merc_native && !flag("gk_dkgl_nativeeyes.txt")) {
      // #19: eyes as in GL -- EyeRenderer renders them in bucket 54, Merc2 finds them through
      // render_state->eye_renderer (same lookup as the PC port)
      s_eyes = std::make_unique<EyeRenderer>("common-pris-eyes", 54);
      s_eyes->init_shaders(s_srs->shaders);
      s_eyes->init_textures(*pool, GameVersion::Jak1);
      s_renderers[list_index(54)] = s_eyes.get();
      s_srs->eye_renderer = s_eyes.get();
      // #19: level NPC eyes live in the pris-tex buckets (see PrisTexEyes)
      s_pris_tex_eyes[0] = std::make_unique<PrisTexEyes>("l0-pris-tex-eyes", 48);
      s_pris_tex_eyes[1] = std::make_unique<PrisTexEyes>("l1-pris-tex-eyes", 51);
      s_renderers[list_index(48)] = s_pris_tex_eyes[0].get();
      s_renderers[list_index(51)] = s_pris_tex_eyes[1].get();
      // #19 (AI-assisted): gated hardware probe, see the s_eyeprobe_* comment at the top
      s_eyeprobe_on = flag("gk_dkgl_eyeprobe.txt");
      if (s_eyeprobe_on) {
        switch_run_logf("[dkgl] eye probe on (gk_dkgl_eyeprobe.txt)");
      }
    } else {
      // the native deko3d eye pass renders them, Merc2 samples its targets
      g_merc2_eye_tex_lookup = [](u32 eye_id) -> u32 { return dkeye::eye_tex_id((u8)eye_id); };
    }
    const struct {
      int bucket;
      const char* name;
    } kMerc[] = {{10, "l0-tfrag-merc"},    {17, "l1-tfrag-merc"},    {45, "common-alpha-merc"},
                 {49, "l0-pris-merc"},     {52, "l1-pris-merc"},     {55, "common-pris-merc"},
                 {58, "l0-water-merc"},    {61, "l1-water-merc"}};
    for (const auto& m : kMerc) {
      const int i = list_index(m.bucket);
      s_merc_buckets[i] = std::make_unique<Merc2BucketRenderer>(m.name, m.bucket, s_merc2);
      s_renderers[i] = s_merc_buckets[i].get();
    }
  }
  s_blackout = std::make_unique<FullScreenDraw>();  // static quad -> persistent storage
  s_active = true;
  switch_run_logf(
      "[dkgl] init ok: programs=%d buckets=4,63%s%s%s%s%s mem=%u KB persistent used=%u B", loaded,
      s_generic2 ? " + generic x10" : "", s_shadow ? " + shadow 47" : "",
      s_depth_cue ? " + depth-cue 64" : "",
      !s_merc2 ? "" : (s_merc_native ? " + emerc x8 (native merc)" : " + merc2+emerc x8"),
      s_eyes ? " + eyes 54" : "",
      kMemBytes / 1024,
      s_persist.used);
  return true;
}

void capture_frame(const void* chain_data, u32 chain_offset) {
  if (capture_begin(chain_data, chain_offset)) {
    capture_end();
  }
}

bool capture_begin(const void* chain_data, u32 chain_offset) {
  s_cap_open = false;
  if (!s_active) {
    return false;
  }
  if (s_src_probe_frames > 0) {
    s_src_probe_frames--;  // #19: run_gpu may log source misses while this is nonzero
  }
  s_slot = (u32)(s_captures % kSlots);
  if (s_fence_armed[s_slot]) {
    // the last frame that called this slot's lists must be done before they are rewritten
    if (dkFenceWait(&s_fence[s_slot], 100'000'000) != DkResult_Success) {
      switch_run_logf("[dkgl] slot %u fence wait timed out: frame skipped", s_slot);
      for (auto& l : s_lists) {
        l.valid = false;
      }
      return false;
    }
    s_fence_armed[s_slot] = false;
  }
  s_data[s_slot].used = 0;
  s_ubo[s_slot].used = 0;
  s_spare_used = false;
  s_cap_overflows_before = s_cmd_overflows;
  dkCmdBufClear(s_cb);
  dkCmdBufAddMemory(s_cb, s_mem, s_slot * kSlotBytes, kCmdBytes);

  // OpenGLRenderer::dispatch_buckets_jak1: CALL to the default-regs chain, then the buckets.
  DmaFollower d(chain_data, chain_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return false;
  }
  const u32 buckets_base = d.current_tag_offset() + 16;
  d.read_and_advance();
  s_srs->default_regs_buffer = d.current_tag_offset();
  if (d.current_tag().kind == DmaTag::Kind::CNT) {
    auto regs = d.read_and_advance();
    if (regs.size_bytes > 148) {
      memcpy(s_srs->fog_color.data(), regs.data + 144, 4);
    }
  }
  s_srs->buckets_base = buckets_base;
  s_srs->ee_main_memory = (void*)chain_data;
  // T112: DepthCue sizes its textures and the frame blit from these. FBO 0 = the dk frame.
  s_srs->render_fb = 0;
  s_srs->draw_offset_x = 0;
  s_srs->draw_offset_y = 0;
  s_srs->draw_region_w = s_fb_w;
  s_srs->draw_region_h = s_fb_h;

  s_recording = true;
  frame_begin();
  s_cap_open = true;
  s_cap_chain = chain_data;
  s_cap_base = buckets_base;
  s_cap_next = 0;
  return true;
}

void capture_until(int bucket) {
  if (!s_cap_open) {
    return;
  }
  // #16/#19 (AI-assisted): runs the dkgl buckets below `bucket`. Called from the texture
  // upload replay, so each GL renderer looks textures up in VRAM as GL would see it there.
  while (s_cap_next < kNumBuckets && kBuckets[s_cap_next] < bucket) {
    const int i = s_cap_next++;
    if (s_renderers[i]) {
      const bool eyes = (kBuckets[i] == 54 && s_renderers[i] == s_eyes.get()) ||
                        kBuckets[i] == 48 || kBuckets[i] == 51;
      const u64 draws_before = draw_count();
      run_bucket(s_renderers[i], s_cap_chain, s_cap_base, kBuckets[i], i);
      if (eyes) {
        s_eye_draws += draw_count() - draws_before;
      }
    }
  }
}

void capture_end() {
  if (!s_cap_open) {
    return;
  }
  capture_until(kBlackoutList);  // whatever the replay did not reach
  s_cap_open = false;
  run_blackout(s_pmode_alp.load(std::memory_order_relaxed));
  frame_end();
  s_recording = false;
  if (s_cmd_overflows != s_cap_overflows_before) {
    for (auto& l : s_lists) {
      l.valid = false;  // a list may be truncated: drop the frame
    }
  }
  s_captures++;

  if (s_captures == 60 || s_captures - s_last_log_capture >= 600) {  // 1 s, then every ~10 s
    s_last_log_capture = s_captures;
    switch_run_logf(
        "[dkgl] ocean %llu frames | segs %u+%u | data %u/%u KB ubo %u/%u KB high | fails "
        "data %u ubo %u | draws %llu skipped %llu inert %llu | dma mismatch %llu | cmd overflow "
        "%llu | blit bad %llu | eye draws %llu | interleaved %llu",
        (unsigned long long)s_captures, (unsigned)s_lists[0].segs.size(),
        (unsigned)s_lists[list_index(63)].segs.size(), s_data[s_slot].high / 1024, kDataBytes / 1024,
        s_ubo[s_slot].high / 1024, kUboBytes / 1024, s_data[s_slot].fails, s_ubo[s_slot].fails,
        (unsigned long long)draw_count(), (unsigned long long)skipped_draw_count(),
        (unsigned long long)inert_draw_count(), (unsigned long long)s_dma_mismatch,
        (unsigned long long)s_cmd_overflows, (unsigned long long)unsupported_blit_count(),
        (unsigned long long)s_eye_draws, (unsigned long long)s_interleaved_frames);
  }
}

// #19: see the s_eyeprobe_* comment at the top. Runs on the world-recorder command
// stream, right where bucket 54's lists were called, so the copy reads the final eye
// content of THIS frame (the barrier above already ordered it).
void eye_probe(DkCmdBuf cb) {
  constexpr int kSlots = 40;          // NUM_EYE_PAIRS * 2
  constexpr u32 kSlotPx = 128 * 128;  // one eye render target
  if (!s_eyeprobe_on || !s_eyes || s_eyeprobe_rounds >= 5) {
    return;
  }
  if (s_eyeprobe_armed) {
    if (s_captures <= s_eyeprobe_at + 1) {
      return;  // two presents have not passed: the copy may still be in flight (P2)
    }
    s_eyeprobe_armed = false;
    if (dkFenceWait(&s_eyeprobe_fence, 100'000'000) != DkResult_Success) {
      switch_run_logf("[dkgl] eye probe #%u: fence wait failed", (unsigned)s_eyeprobe_rounds + 1);
      return;
    }
    char status[kSlots + 1];
    u32 content = 0, partial = 0, black = 0, missing = 0;
    u64 ar = 0, ag = 0, ab = 0, aa = 0;
    const u8* base = s_eyeprobe_buf.cpu;
    for (int i = 0; i < kSlots; i++) {
      if (!s_eyeprobe_have[i]) {
        status[i] = 'x';
        missing++;
        continue;
      }
      const u8* px = base + (u32)i * kSlotPx * 4;
      u32 dark = 0;
      for (u32 p = 0; p < kSlotPx; p++) {
        if (px[p * 4] + px[p * 4 + 1] + px[p * 4 + 2] < 12) {
          dark++;
        }
        if (i == 0) {
          ar += px[p * 4];
          ag += px[p * 4 + 1];
          ab += px[p * 4 + 2];
          aa += px[p * 4 + 3];
        }
      }
      if (dark == kSlotPx) {
        status[i] = '.';
        black++;
      } else if (dark > kSlotPx - kSlotPx / 8) {
        status[i] = '+';
        partial++;
      } else {
        status[i] = '#';
        content++;
      }
    }
    status[kSlots] = 0;
    switch_run_logf(
        "[dkgl] eye probe #%u: %s | # %u + %u . %u x %u | slot0 avg rgba %u,%u,%u,%u",
        (unsigned)s_eyeprobe_rounds + 1, status, content, partial, black, missing,
        (unsigned)(ar / kSlotPx), (unsigned)(ag / kSlotPx), (unsigned)(ab / kSlotPx),
        (unsigned)(aa / kSlotPx));
    s_eyeprobe_rounds++;
    return;
  }
  if (s_captures < 300 || s_captures < s_eyeprobe_last + 300) {
    return;  // first probe ~5 s in, then every ~5 s, five rounds
  }
  if (!s_eyeprobe_buf.ok()) {
    s_eyeprobe_buf = dkdev::buffer_pool()->acquire(kSlots * kSlotPx * 4);
    if (!s_eyeprobe_buf.ok()) {
      switch_run_logf("[dkgl] eye probe: buffer acquire failed -- disabling");
      s_eyeprobe_on = false;
      return;
    }
  }
  const DkImageRect rc{0, 0, 0, 128, 128, 1};
  u32 have = 0;
  for (int i = 0; i < kSlots; i++) {
    s_eyeprobe_have[i] = false;
    const u64 id = s_eyes->debug_eye_gl_tex(i);
    const DkImage* img = id ? (const DkImage*)dktexgpu::image_of((u32)id) : nullptr;
    if (!img) {
      continue;
    }
    DkImageView view;
    dkImageViewDefaults(&view, img);
    const DkCopyBuf cbuf{s_eyeprobe_buf.gpu + (u64)i * kSlotPx * 4, 0, 0};
    dkCmdBufCopyImageToBuffer(cb, &view, &rc, &cbuf, 0);
    s_eyeprobe_have[i] = true;
    have++;
  }
  if (!have) {
    switch_run_logf("[dkgl] eye probe: no image for any slot -- disabling");
    s_eyeprobe_on = false;
    return;
  }
  dkCmdBufSignalFence(cb, &s_eyeprobe_fence, false);
  s_eyeprobe_armed = true;
  s_eyeprobe_at = s_captures;
  s_eyeprobe_last = s_captures;
  s_src_probe_frames = 2;  // EyeRenderer logs the next captures' source misses
  s_src_probe_budget = 12;
  switch_run_logf("[dkgl] eye probe #%u armed: %u/40 images", (unsigned)s_eyeprobe_rounds + 1,
                  have);
}

// #19 probe v2: consumed by EyeRenderer::run_gpu (render thread, during capture).
int eye_src_probe_active() {
  return s_src_probe_frames > 0;
}

int eye_src_probe_take() {
  return s_src_probe_budget-- > 0;
}

bool record_bucket(int bucket) {
  if (!s_active) {
    return false;
  }
  const int idx = list_index(bucket);
  if (idx < 0 || !s_lists[idx].valid) {
    return false;
  }
  DkCmdBuf cb = dkdev::cmd_handle();
  for (const Segment& seg : s_lists[idx].segs) {
    if (seg.blit) {
      const DkImage* img = (const DkImage*)dktexgpu::image_of(seg.blit_dst_tex);
      if (img) {
        DkImageView dst;
        dkImageViewDefaults(&dst, img);
        dkdev::blit_frame_to(&dst, &seg.blit_src, &seg.blit_dst,
                             seg.blit_linear ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest);
      }
      continue;
    }
    if (seg.frame_target) {
      dkdev::rebind_frame_target();
    }
    dkCmdBufCallList(cb, seg.list);
  }
  // back to the frame target + full scissor for the native passes that follow
  dkdev::rebind_frame_target();
  if (bucket == 48 || bucket == 51 || bucket == 54) {
    // #19 (AI-assisted): Merc2 samples the eye textures through resolved dktexgpu ids, which
    // texture_needs_barrier does not track: finish the eye fragments before merc reads them.
    dkCmdBufBarrier(cb, DkBarrier_Fragments,
                    DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    if (bucket == 54) {
      eye_probe(cb);
    }
  }
  dkCmdBufSignalFence(cb, &s_fence[s_slot], false);
  s_fence_armed[s_slot] = true;
  return true;
}

bool record_blackout() {
  return record_bucket(kBlackoutList);
}

}  // namespace dkgl

#else  // host build: the frame hook is Switch-only

namespace dkgl {
bool init(TexturePool*, Loader*) {
  return false;
}
bool active() {
  return false;
}
bool merc_active() {
  return false;
}
bool eyes_active() {
  return false;
}
void note_interleaved_frame() {}
void capture_frame(const void*, u32) {}
bool capture_begin(const void*, u32) {
  return false;
}
void capture_until(int) {}
void capture_end() {}
bool record_bucket(int) {
  return false;
}
bool record_blackout() {
  return false;
}
void set_pmode_alp(float) {}
void set_frame_size(int, int) {}
}  // namespace dkgl

#endif
