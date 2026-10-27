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

#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <vector>

#include <deko3d.h>

#include "common/dma/dma_chain_read.h"
#include "common/util/FileUtil.h"

#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/opengl_renderer/ocean/OceanMidAndFar.h"
#include "game/graphics/opengl_renderer/ocean/OceanNear.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/pipelines/dk_device_internal.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

// T103 embedded dksh (game/CMakeLists.txt _DKGL_SHADERS)
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
constexpr u32 kDataBytes = 4 * 1024 * 1024;  // CommonOceanRenderer re-uploads per bucket
constexpr u32 kUboBytes = 512 * 1024;        // one <=512 B slice per draw
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

struct Segment {
  DkCmdList list = 0;
  bool frame_target = false;  // the caller re-binds the frame target before calling it
};
struct BucketLists {
  std::vector<Segment> segs;
  bool valid = false;
};
BucketLists s_lists[2];  // [0] = bucket 4, [1] = bucket 63
int s_cur_bucket = -1;
bool s_seg_open = false;
bool s_seg_frame = false;

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
u64 s_dma_mismatch = 0;
u64 s_last_log_capture = 0;

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
  open_segment(true);
  const u32 slot = buckets_base + 16 * (u32)bucket;
  s_srs->next_bucket = slot + 16;
  DmaFollower dma(data, slot);
  {
    ProfilerNode node(r->name_and_id());
    ScopedProfilerNode prof(&node);
    r->render(dma, s_srs.get(), prof);
  }
  if (dma.current_tag_offset() != s_srs->next_bucket) {
    s_dma_mismatch++;  // OpenGLRenderer asserts here; dkgl only counts (logged per 10 s)
  }
  close_segment();
  s_lists[list_idx].valid = true;
  s_cur_bucket = -1;
}

}  // namespace

bool active() {
  return s_active;
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
  set_emitter(&s_emitter);
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
  s_active = true;
  switch_run_logf("[dkgl] init ok: programs=%d buckets=4,63 mem=%u KB persistent used=%u B",
                  loaded, kMemBytes / 1024, s_persist.used);
  return true;
}

void capture_frame(const void* chain_data, u32 chain_offset) {
  if (!s_active) {
    return;
  }
  s_slot = (u32)(s_captures % kSlots);
  if (s_fence_armed[s_slot]) {
    // the last frame that called this slot's lists must be done before they are rewritten
    if (dkFenceWait(&s_fence[s_slot], 100'000'000) != DkResult_Success) {
      switch_run_logf("[dkgl] slot %u fence wait timed out: frame skipped", s_slot);
      s_lists[0].valid = s_lists[1].valid = false;
      return;
    }
    s_fence_armed[s_slot] = false;
  }
  s_data[s_slot].used = 0;
  s_ubo[s_slot].used = 0;
  s_spare_used = false;
  const u64 overflows_before = s_cmd_overflows;
  dkCmdBufClear(s_cb);
  dkCmdBufAddMemory(s_cb, s_mem, s_slot * kSlotBytes, kCmdBytes);

  // OpenGLRenderer::dispatch_buckets_jak1: CALL to the default-regs chain, then the buckets.
  DmaFollower d(chain_data, chain_offset);
  if (d.current_tag().kind != DmaTag::Kind::CALL) {
    return;
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

  s_recording = true;
  frame_begin();
  run_bucket(s_mid_far.get(), chain_data, buckets_base, 4, 0);
  run_bucket(s_near.get(), chain_data, buckets_base, 63, 1);
  frame_end();
  s_recording = false;
  if (s_cmd_overflows != overflows_before) {
    s_lists[0].valid = s_lists[1].valid = false;  // a list may be truncated: drop the frame
  }
  s_captures++;

  if (s_captures == 60 || s_captures - s_last_log_capture >= 600) {  // 1 s, then every ~10 s
    s_last_log_capture = s_captures;
    switch_run_logf(
        "[dkgl] ocean %llu frames | segs %u+%u | data %u/%u KB ubo %u/%u KB high | fails "
        "data %u ubo %u | draws %llu skipped %llu inert %llu | dma mismatch %llu | cmd overflow "
        "%llu",
        (unsigned long long)s_captures, (unsigned)s_lists[0].segs.size(),
        (unsigned)s_lists[1].segs.size(), s_data[s_slot].high / 1024, kDataBytes / 1024,
        s_ubo[s_slot].high / 1024, kUboBytes / 1024, s_data[s_slot].fails, s_ubo[s_slot].fails,
        (unsigned long long)draw_count(), (unsigned long long)skipped_draw_count(),
        (unsigned long long)inert_draw_count(), (unsigned long long)s_dma_mismatch,
        (unsigned long long)s_cmd_overflows);
  }
}

bool record_bucket(int bucket) {
  if (!s_active) {
    return false;
  }
  const int idx = bucket == 4 ? 0 : (bucket == 63 ? 1 : -1);
  if (idx < 0 || !s_lists[idx].valid) {
    return false;
  }
  DkCmdBuf cb = dkdev::cmd_handle();
  for (const Segment& seg : s_lists[idx].segs) {
    if (seg.frame_target) {
      dkdev::rebind_frame_target();
    }
    dkCmdBufCallList(cb, seg.list);
  }
  // back to the frame target + full scissor for the native passes that follow
  dkdev::rebind_frame_target();
  dkCmdBufSignalFence(cb, &s_fence[s_slot], false);
  s_fence_armed[s_slot] = true;
  return true;
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
void capture_frame(const void*, u32) {}
bool record_bucket(int) {
  return false;
}
void set_frame_size(int, int) {}
}  // namespace dkgl

#endif
