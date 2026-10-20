/*!
 * @file dk_device.cpp
 * deko3d migration Phase 1 (AI-assisted): device / memory / queue / swapchain and the
 * splash present path. The only TU of the deko3d module that includes libnx/deko3d
 * headers (see dk_device.h for why).
 *
 * Memory model (P0.6 finding, notes 2026-10-16): deko3d memblocks are allocated from the
 * newlib heap -- the same heap the GOAL runtime and Mesa live in. There is no separate
 * pool to draw from, so init() checks the heap headroom FIRST and refuses (=> the caller
 * stays on OpenGL) instead of letting a later allocation die.
 */

#if defined(__SWITCH__)

#include "game/graphics/pipelines/dk_device.h"

#include <deko3d.h>
#include <malloc.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>
#include "dksh/splash_vert.h"
#include "dksh/splash_frag.h"
// T33a: the T25-built tfrag3 .dksh pair (P3.2). Same EmbedBinary rule as the splash
// shaders, so the headers sit beside them in dksh_gen/dksh/. Array names carry the _dksh
// suffix (tfrag3_vert_dksh / tfrag3_frag_dksh) unlike the splash ones -- see QWEN_REPORT.md.
#include "dksh/tfrag3_vert_dksh.h"
#include "dksh/shrub_vert_dksh.h"  // T72
#include "dksh/shrub_frag_dksh.h"
#include "dksh/tfrag3_frag_dksh.h"
#include "game/graphics/deko3d_renderer/dk_ring.h"  // T20: per-frame UBO ring (plain, deko3d-free header)
#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"  // T21: P2.3 level-data buffer pool (plain, deko3d-free header)
#include "game/graphics/pipelines/dk_device_internal.h"  // T22a: P2.3 dk-TU internals pass-throughs
// T34a: the game's sampler table + the SamplerDesc->DkSampler bridge (P3.2).
// dk_state.h (T16, reviewed) brings in PipelineTable.h (the dkpipe::SamplerDesc type) and
// declares dkpipe::to_deko3d(const SamplerDesc&, DkSampler*); it MUST come before the
// .inc, which uses those very types. PipelineTable.inc defines dkpipe::kSamplers[] (16
// entries, static-asserted) at file scope. Only this deko3d TU pulls in <deko3d.h>, so
// including dk_state.h here is safe.
#include "game/graphics/deko3d_renderer/dk_state.h"
#include "game/graphics/deko3d_renderer/PipelineTable.inc"

// switch_run_logf lives in run_log.h, which cannot be included next to <switch.h>
// (u128 collision). Same hand declaration as switch/platform.cpp.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// newlib's heap bounds (libsysbase malloc_vars.o); libnx sets them in __libnx_initheap.
extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
}

namespace dkdev {
namespace {

constexpr unsigned kFbNum = 2;
// T60 (AI-assisted): 64 KB -> 1 MB. The tfrag world pass records up to ~5000 indexed draws
// (2500 + AFAIL second draws) at ~150 B each; s_cmd has no add-memory callback, so the slice
// must hold a whole frame. Every later offset below is derived, so it shifts automatically.
constexpr uint32_t kCmdSlice = 1024 * 1024;  // per in-flight frame
// T18: a dedicated command-memory region for runtime uploads, sized 16 KB. It sits
// after the per-frame cmd slices and before the staging buffer:
//   [cmd slices | upload cmd | staging | descriptors | UBO]
// (so an in-flight upload never collides with the per-frame cmd slices in present_frame).
// The per-frame cmd slices are at offset `slice * kCmdSlice` for slice in {0, 1},
// so the upload region starts at kFbNum * kCmdSlice and is 0x4000 (16 KB) wide.
constexpr uint32_t kCmdUpload = 0x4000;                              // 16 KB upload command region
constexpr uint32_t kOffUpload = kFbNum * kCmdSlice;                  // 0x20000
constexpr uint32_t kOffStaging = kOffUpload + kCmdUpload;           // 0x24000
constexpr uint32_t kStagingMax = 512 * 1024 * 4;  // up to 512x1024 RGBA splash
// T06/T22b/T34a: descriptor region + UBO area (and, T34a, the sampler region) live in the
// data pool after the staging buffer. The order is:
//   [cmd slices | upload cmd | staging | descriptors | UBO | samplers].
// The first 0x100 of the descriptor region holds the splash's two Dk descriptors
// (image @+0, sampler @+0x20; 32 B each). T22b grows the region by
// kMaxTexImages * DK_IMAGE_DESCRIPTOR_ALIGNMENT so the P2.3 texture pool can place one
// DkImageDescriptor per live texture at (kOffDesc + 0x100 + i * DK_IMAGE_DESCRIPTOR_ALIGNMENT).
// NOTE: kDescBytes = 0x100 + 2048 * DK_IMAGE_DESCRIPTOR_ALIGNMENT. DK_IMAGE_DESCRIPTOR_ALIGNMENT
// is 0x20 (deko3d.h:153; DkImageDescriptor is an opaque 32-B struct), so the grown region is
// 0x10100. kMaxTexImages matches dktexgpu::kMaxTexImages (the 2048-slot texture store capacity).
constexpr uint32_t kMaxTexImages = 2048;
constexpr uint32_t kOffDesc = kOffStaging + kStagingMax;  // 0x224000, naturally 0x20-aligned
constexpr uint32_t kDescBytes = 0x100 + kMaxTexImages * DK_IMAGE_DESCRIPTOR_ALIGNMENT;  // 0x10100
constexpr uint32_t kOffUbo = kOffDesc + kDescBytes;       // 0x234100, 0x100-aligned
constexpr uint32_t kUboBytes = 0x80000;  // T20: 512 KB UBO ring = kFbNum (2) frames x 256 KB segments
// T34a: P3.2 sampler-descriptor region, appended AFTER the UBO ring at the data-pool end
// (never renumber existing offsets). kSamplers[] (PipelineTable.inc) is 16 entries
// (static-asserted at the end of that .inc). Each DkSamplerDescriptor needs
// DK_SAMPLER_DESCRIPTOR_ALIGNMENT (0x20) bytes, so 16 * 0x20 = 0x200 (512 B).
// That 0x200 fits inside the data memblock's pre-existing 0x1000 rounding slack
// (old kDataPool 0x2B4100 already aligns up to 0x2B5000, leaving 0xF00 > 0x200),
// so make_block's align_up leaves the allocated memblock size unchanged at 0x2B5000.
constexpr uint32_t kSamplerCount = sizeof(kSamplers) / sizeof(kSamplers[0]);  // 16 (PipelineTable.inc)
constexpr uint32_t kOffSamplers = kOffUbo + kUboBytes;             // 0x2B4100, == old data-pool end
constexpr uint32_t kSamplersBytes = kSamplerCount * DK_SAMPLER_DESCRIPTOR_ALIGNMENT;  // 0x200 = 512 B
constexpr uint32_t kDataPool = kOffSamplers + kSamplersBytes;     // 0x2B4300 (+0x200 = 512 B)
// The data memblock is rounded up to DK_MEMBLOCK_ALIGNMENT (0x1000) inside make_block; both
// 0x2B4100 and 0x2B4300 round up to the same 0x2B5000, so the 512 B growth is absorbed with no
// allocation-size change. (kOffSamplers is 0x20-aligned: 0x2B4100 % 0x20 == 0.)
static_assert(kOffDesc % DK_IMAGE_DESCRIPTOR_ALIGNMENT == 0, "desc region must be 0x20-aligned");
static_assert(kOffUbo % DK_UNIFORM_BUF_ALIGNMENT == 0, "ubo region must be 0x100-aligned");
static_assert(kOffSamplers % DK_SAMPLER_DESCRIPTOR_ALIGNMENT == 0, "sampler region must be 0x20-aligned");
// T21: a separate deko3d memblock for the P2.3 level-data buffer pool (dkpool::BufferPool).
// T54: 8 -> 128 MB. T57: 128 -> 384 MB -- the T55 hardware run filled 128 MB with common +
// 2 levels (6 failed allocations at the beach prefetch); jak1 keeps up to 4 levels live.
// Heap free was ~2.5 GB in that run. 0x18000000 is a DK_MEMBLOCK_ALIGNMENT multiple.
constexpr uint32_t kPoolBytes = 384 * 1024 * 1024;  // T57: 384 MB = 1536 x 256 KB size classes
  // T05: one memblock for shader code, sized to fit both splash shaders with room to
  // spare. A .dksh's code section is small (the control stays in its static embedding).
  constexpr uint32_t kCodePool = 64 * 1024;
// T07: splash 3D path switch. OFF by default; flip true after T08 hardware test confirms
// the 3D splash renders correctly on the console. The 2D blit is kept as the else branch.
constexpr bool kSplash3D = false;  // P1.4b: flip after T08 hardware test
// Never take the heap below this much headroom: the GOAL runtime still has to load
// levels, and FIX 34c showed how little the console can spare.
constexpr unsigned long long kMinHeadroom = 24ull * 1024 * 1024;

bool s_up = false;
int s_fb_w = 0, s_fb_h = 0;
DkDevice s_dev = nullptr;
DkQueue s_queue = nullptr;
// T58: a second queue only for uploads (copy_to_gpu + submit_upload_cmds). On the present
// queue every upload waited behind vsync-blocked present work: the T55 hardware run logged
// ~16.6 ms per copy_to_gpu. Null if creation failed -> uploads fall back to s_queue.
DkQueue s_upQueue = nullptr;
DkMemBlock s_fbMem = nullptr, s_dataMem = nullptr, s_splashMem = nullptr;
DkImage s_fb[kFbNum];
// T60: one Z24S8 depth buffer (frames are serialised on s_queue, so one is enough) and the
// world-pass hook. The hook is null unless gk_dktfrag.txt turned the tfrag pass on.
DkMemBlock s_depthMem = nullptr;
DkImage s_depth;
bool s_have_depth = false;
void (*s_world)(int, int) = nullptr;
DkSwapchain s_swap = nullptr;
DkCmdBuf s_cmd = nullptr;
DkFence s_sliceFence[kFbNum];
// T18: the per-upload fence. Initialized and waited on the same way as s_sliceFence.
DkFence s_uploadFence;
// T20: the P2.4 per-frame UBO ring carved from the data pool [kOffUbo, kOffUbo + kUboBytes).
// begin_frame() is driven inside present_frame(); nothing allocates from it yet (P3 renderers do).
static dkring::FrameRing s_uboRing;
// T21/T57: a dedicated 384 MB memblock for the P2.3 level-data buffer pool and the pool itself.
// Inert at boot: nothing acquires yet (the first consumer is a P3 renderer).
static DkMemBlock s_poolMem = nullptr;
static dkpool::BufferPool s_bufferPool;
unsigned s_frame = 0;
unsigned long long s_pool_bytes = 0;

bool s_have_splash = false;
DkImage s_splash;
int s_splash_w = 0, s_splash_h = 0;

// T05: code memblock + splash shaders. Nothing draws them yet (T07 does).
DkMemBlock s_codeMem = nullptr;
DkShader s_splashVsh, s_splashFsh;
bool s_have_shaders = false;
// T33a: tfrag3 shader pair (P3.2), loaded into the same code memblock after the splash
// pair. Inert: nothing reads s_have_tfrag3 yet (T33b adds the getters), so the load runs
// at boot but its result only moves memblock bytes + the flag.
DkShader s_tfrag3Vsh, s_tfrag3Fsh;
bool s_have_tfrag3 = false;
DkShader s_shrubVsh, s_shrubFsh;  // T72
bool s_have_shrub = false;
bool s_have_desc = false;  // T06: splash image + sampler descriptors written to the data pool

double ms_since(u64 t0) {
  return (double)armTicksToNs(armGetSystemTick() - t0) / 1.0e6;
}

uint32_t align_up(uint32_t v, uint32_t a) {
  return (v + a - 1) & ~(a - 1);
}

void dk_debug_cb(void*, const char* context, DkResult result, const char* message) {
  // deko3d calls this on API misuse/failure; the log line is the diagnostic.
  switch_run_logf("[dk] debug ctx=%s result=%d msg=%s", context ? context : "?", (int)result,
                  message ? message : "?");
}

DkMemBlock make_block(uint32_t size, uint32_t flags, const char* what) {
  size = align_up(size, DK_MEMBLOCK_ALIGNMENT);
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, s_dev, size);
  mm.flags = flags;
  DkMemBlock b = dkMemBlockCreate(&mm);
  if (b) {
    s_pool_bytes += size;
  } else {
    switch_run_logf("[dk] memblock %s (%u KB) FAILED", what, size / 1024);
  }
  return b;
}

struct DkshHeader {
  uint32_t magic;     // DKSH_MAGIC
  uint32_t header_sz; // sizeof(DkshHeader)
  uint32_t control_sz;
  uint32_t code_sz;
  uint32_t programs_off;
  uint32_t num_programs;
};

// Loads one embedded .dksh into s_codeMem at *offset (advanced on success). The code section
// goes into the (CPU-uncached/GPU-cached) code memblock; the control section stays in its
// static embedding and is pointed at directly (see deko3d DkShaderMaker.control).
// Returns false (and logs) on a malformed blob or an overflow of the code pool; it never
// touches *offset when it fails, so two shaders can share one offset and the second is
// skipped cleanly if the first overflows.
bool load_shader(DkShader* out, const uint8_t* blob, uint32_t size, uint32_t* offset) {
  if (!out || !blob || !offset || size < sizeof(DkshHeader)) {
    switch_run_logf("[dk] shader blob too small (%u B)", size);
    return false;
  }
  DkshHeader hdr;
  memcpy(&hdr, blob, sizeof(hdr));
  // control_sz + code_sz must fit in the blob (the code section sits right after control).
  if (hdr.control_sz + hdr.code_sz > size) {
    switch_run_logf("[dk] shader sizes exceed blob: control=%u code=%u blob=%u", hdr.control_sz,
                    hdr.code_sz, size);
    return false;
  }
  *offset = align_up(*offset, DK_SHADER_CODE_ALIGNMENT);
  if (*offset + hdr.code_sz > kCodePool) {
    switch_run_logf("[dk] shader code overflows code pool: off=%u code=%u pool=%u", *offset,
                    hdr.code_sz, kCodePool);
    return false;
  }
  // Copy only the code section (which begins after the control section) into GPU memory.
  memcpy((uint8_t*)dkMemBlockGetCpuAddr(s_codeMem) + *offset, blob + hdr.control_sz, hdr.code_sz);
  // The control section stays in the static embedding (point at it); the code lives at
  // *offset in the code memblock.
  DkShaderMaker sm;
  dkShaderMakerDefaults(&sm, s_codeMem, *offset);
  sm.control = blob;
  sm.programId = 0;
  dkShaderInitialize(out, &sm);
  if (!dkShaderIsValid(out)) {
    switch_run_logf("[dk] shader init failed (ok=false), code=%u B", hdr.code_sz);
    return false;
  }
  *offset += hdr.code_sz;
  return true;
}

// T07: draw the splash via the 3D pipeline (full-screen quad from gl_VertexID) using the
// T05 shaders + T06 descriptors/UBO. The 2D blit is the fallback when kSplash3D is off.
// The bind order follows Example04_TexturedCube.cpp but without a depth buffer (no
// clearDepthStencil / bindDepthStencilState / discardDepthStencil — the splash is a flat 2D
// image, so depth write/read is unnecessary).
void record_splash_3d(uint32_t fb_w, uint32_t fb_h) {
  const DkGpuAddr data = dkMemBlockGetGpuAddr(s_dataMem);
  DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
  dkCmdBufSetViewports(s_cmd, 0, &vp, 1);
  DkShader const* sh[] = {&s_splashVsh, &s_splashFsh};
  dkCmdBufBindShaders(s_cmd, DkStageFlag_GraphicsMask, sh, 2);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cw;
  dkColorWriteStateDefaults(&cw);
  dkCmdBufBindRasterizerState(s_cmd, &rs);
  dkCmdBufBindColorState(s_cmd, &cs);
  dkCmdBufBindColorWriteState(s_cmd, &cw);
  dkCmdBufBindVtxAttribState(s_cmd, nullptr, 0);  // no vertex buffers: gl_VertexID only
  dkCmdBufBindVtxBufferState(s_cmd, nullptr, 0);
  // T06 descriptors: image desc @ kOffDesc, sampler desc @ kOffDesc+0x20 (32B DkImageDescriptor + 32B DkSamplerDescriptor)
  dkCmdBufBindImageDescriptorSet(s_cmd, data + kOffDesc, 1);
  dkCmdBufBindSamplerDescriptorSet(s_cmd, data + kOffDesc + DK_IMAGE_DESCRIPTOR_ALIGNMENT, 1);
  struct {
    float res[2];
    float tex[2];
  } p = {{(float)fb_w, (float)fb_h}, {(float)s_splash_w, (float)s_splash_h}};
  dkCmdBufPushConstants(s_cmd, data + kOffUbo, kUboBytes, 0, sizeof(p), &p);
  dkCmdBufBindUniformBuffer(s_cmd, DkStage_Vertex, 0, data + kOffUbo, kUboBytes);
  dkCmdBufBindTexture(s_cmd, DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
  dkCmdBufDraw(s_cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

}  // namespace

HeapInfo heap_info() {
  struct mallinfo mi = mallinfo();
  HeapInfo h{};
  h.arena_bytes = mi.arena;
  h.in_use_bytes = mi.uordblks;
  h.free_in_arena = mi.fordblks;
  char* brk = (char*)sbrk(0);
  if (brk && fake_heap_end && brk <= fake_heap_end) {
    h.unreserved_bytes = (unsigned long long)(fake_heap_end - brk);
  }
  return h;
}

void log_heap(const char* tag) {
  const HeapInfo h = heap_info();
  const unsigned long long span =
      (fake_heap_start && fake_heap_end) ? (unsigned long long)(fake_heap_end - fake_heap_start)
                                         : 0;
  switch_run_logf("[heap] %-16s span=%llu MB arena=%llu MB in_use=%llu MB free=%llu MB "
                  "(arena %llu + unreserved %llu)",
                  tag, span >> 20, h.arena_bytes >> 20, h.in_use_bytes >> 20,
                  h.total_free() >> 20, h.free_in_arena >> 20, h.unreserved_bytes >> 20);
}

bool is_up() {
  return s_up;
}

void set_swap_interval(unsigned interval) {
  if (s_swap) {
    dkSwapchainSetSwapInterval(s_swap, interval);
  }
}

unsigned long long pool_bytes() {
  return s_pool_bytes;
}

int operation_mode() {
  return appletGetOperationMode() == AppletOperationMode_Console ? 1 : 0;
}

bool in_focus() {
  return appletGetFocusState() == AppletFocusState_InFocus;
}

void wait_idle() {
  if (s_up) {
    dkQueueWaitIdle(s_queue);
  }
}

void shutdown() {
  if (s_upQueue) {
    dkQueueWaitIdle(s_upQueue);
    dkQueueDestroy(s_upQueue);
    s_upQueue = nullptr;
  }
  if (s_queue) {
    dkQueueWaitIdle(s_queue);
    dkQueueDestroy(s_queue);
    s_queue = nullptr;
  }
  if (s_cmd) {
    dkCmdBufDestroy(s_cmd);
    s_cmd = nullptr;
  }
  if (s_swap) {
    dkSwapchainDestroy(s_swap);
    s_swap = nullptr;
  }
  // T21: tear down the P2.3 buffer-pool memblock in the same loop (order: right after s_dataMem).
  DkMemBlock* blocks[] = {&s_splashMem, &s_dataMem, &s_poolMem, &s_codeMem, &s_fbMem, &s_depthMem};
  for (DkMemBlock* b : blocks) {
    if (*b) {
      dkMemBlockDestroy(*b);
      *b = nullptr;
    }
  }
  s_have_depth = false;
  if (s_dev) {
    dkDeviceDestroy(s_dev);
    s_dev = nullptr;
  }
  if (s_up) {
    switch_run_logf("[dk] device shut down");
  }
  s_have_splash = false;
  s_have_shaders = false;
  s_have_shrub = false;  // T72
  s_have_tfrag3 = false;  // T33b: re-zero the tfrag3 loaded-bool (DkShader has no destroy; memblock teardown above handles the code region)
  s_pool_bytes = 0;
  s_up = false;
}

bool init(int fb_width, int fb_height) {
  if (s_up) {
    return true;
  }
  const u64 t0 = armGetSystemTick();
  s_fb_w = fb_width;
  s_fb_h = fb_height;
  log_heap("dk before init");

  DkDeviceMaker dm;
  dkDeviceMakerDefaults(&dm);
  dm.cbDebug = dk_debug_cb;
  // T60 (AI-assisted): GL conventions, so the ported GL shaders/matrices work unchanged:
  // clip-space z in [-1, 1] and window origin at the bottom-left (GL's gl_Position y-up).
  // The splash blit runs on the 2D engine and the clears are full-screen, so neither is
  // affected.
  dm.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginLowerLeft;
  s_dev = dkDeviceCreate(&dm);
  if (!s_dev) {
    switch_run_logf("[dk] dkDeviceCreate FAILED");
    return false;
  }

  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, s_dev);
  // No HwCompression: the splash is drawn with the 2D engine (dkCmdBufBlitImage), and the
  // 2D engine + compressed render targets is an untested combination on this console.
  // Compression can come back with the 3D splash pipeline in P1.4b.
  lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = (uint32_t)fb_width;
  lm.dimensions[1] = (uint32_t)fb_height;
  DkImageLayout fbLayout;
  dkImageLayoutInitialize(&fbLayout, &lm);
  const uint32_t fbSize =
      align_up((uint32_t)dkImageLayoutGetSize(&fbLayout), dkImageLayoutGetAlignment(&fbLayout));

  // T60: Z24S8 depth buffer, same flags as deko3d Example04 (L238).
  DkImageLayoutMaker dlm;
  dkImageLayoutMakerDefaults(&dlm, s_dev);
  dlm.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
  dlm.format = DkImageFormat_Z24S8;
  dlm.dimensions[0] = (uint32_t)fb_width;
  dlm.dimensions[1] = (uint32_t)fb_height;
  DkImageLayout depthLayout;
  dkImageLayoutInitialize(&depthLayout, &dlm);
  const uint32_t depthSize = align_up((uint32_t)dkImageLayoutGetSize(&depthLayout),
                                      dkImageLayoutGetAlignment(&depthLayout));

  // Budget check BEFORE any big allocation (see file comment).
  const unsigned long long need = (unsigned long long)kFbNum * fbSize + kDataPool + kCodePool +
                                  kPoolBytes + depthSize;  // T54: + pool, T60: + depth
  const HeapInfo h = heap_info();
  if (h.total_free() < need + kMinHeadroom) {
    switch_run_logf("[dk] heap check FAILED: need %llu MB + %llu MB headroom, free %llu MB", need >> 20,
                    kMinHeadroom >> 20, h.total_free() >> 20);
    shutdown();
    return false;
  }

  s_fbMem = make_block(kFbNum * fbSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "fb");
  s_dataMem =
      make_block(kDataPool, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "data");
  // T05: code memblock for the splash shaders. Code-flagged (executable shader code lives here);
  // the control sections stay in their static embeddings (pointed at directly, see load_shader).
  s_codeMem =
      make_block(kCodePool,
                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                 "code");
  if (!s_fbMem || !s_dataMem || !s_codeMem) {
    shutdown();
    return false;
  }
  // T20: the per-frame UBO ring lives in the data memblock at kOffUbo. s_dataMem is
  // CpuUncached|GpuCached, so the CPU base and the GPU base share the same byte offset.
  // begin_frame() (called every frame from present_frame) only becomes safe once the frame's
  // slice fence has been waited on; the ring never overflows one frame's segment (see dk_ring.h).
  s_uboRing.init((uint8_t*)dkMemBlockGetCpuAddr(s_dataMem) + kOffUbo,
                 dkMemBlockGetGpuAddr(s_dataMem) + kOffUbo,
                 kUboBytes, kFbNum);
  // FrameRing has no public usable() accessor; in dk_ring.h segment_size() is 0 exactly when
  // init() left the ring unusable (frames == 0 or fewer than one aligned 256-byte segment/frame).
  if (s_uboRing.segment_size() != 0) {
    switch_run_logf("[dk] ubo ring %u KB x %u frames, seg %u KB", kUboBytes / 1024, kFbNum,
                    s_uboRing.segment_size() / 1024);
  } else {
    switch_run_logf("[dk] ubo ring UNUSABLE (init failed) — continuing without UBO ring");
  }
  // T21/T57: a dedicated 384 MB deko3d memblock for the P2.3 level-data buffer pool. Same maker/flags
  // as s_dataMem (CpuUncached|GpuCached); make_block adds it to the s_pool_bytes accounting. Inert:
  // nothing acquires yet (the first consumer is a P3 renderer). A failed memblock is not fatal --
  // we log it, leave s_poolMem null, and continue (the game still boots and draws the splash).
  s_poolMem = make_block(kPoolBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "bufferpool");
  if (s_poolMem) {
    s_bufferPool.init((uint8_t*)dkMemBlockGetCpuAddr(s_poolMem),
                      dkMemBlockGetGpuAddr(s_poolMem), kPoolBytes);
    switch_run_logf("[dk] buffer pool %u MB ready (%u chunks x 256 KB)", kPoolBytes >> 20,
                    kPoolBytes / (256u * 1024u));
  } else {
    switch_run_logf("[dk] buffer pool UNUSABLE (memblock failed) — continuing without pool");
  }

  DkImage const* swapImgs[kFbNum];
  for (unsigned i = 0; i < kFbNum; i++) {
    dkImageInitialize(&s_fb[i], &fbLayout, s_fbMem, i * fbSize);
    swapImgs[i] = &s_fb[i];
  }
  // T60: not fatal when it fails -- only the tfrag world pass needs it.
  s_depthMem = make_block(depthSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "depth");
  if (s_depthMem) {
    dkImageInitialize(&s_depth, &depthLayout, s_depthMem, 0);
    s_have_depth = true;
  }
  // SDL video is never initialized on this path, so the default native window is ours.
  NWindow* win = nwindowGetDefault();
  nwindowSetDimensions(win, (u32)fb_width, (u32)fb_height);
  DkSwapchainMaker sm;
  dkSwapchainMakerDefaults(&sm, s_dev, win, swapImgs, kFbNum);
  s_swap = dkSwapchainCreate(&sm);
  if (!s_swap) {
    switch_run_logf("[dk] dkSwapchainCreate FAILED");
    shutdown();
    return false;
  }

  DkQueueMaker qm;
  dkQueueMakerDefaults(&qm, s_dev);
  qm.flags = DkQueueFlags_Graphics;
  s_queue = dkQueueCreate(&qm);
  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, s_dev);
  s_cmd = dkCmdBufCreate(&cm);
  if (!s_queue || !s_cmd) {
    switch_run_logf("[dk] queue/cmdbuf create FAILED");
    shutdown();
    return false;
  }
  {
    // T58: dedicated upload queue (same maker + flags as s_queue). Not fatal if it fails.
    DkQueueMaker uqm;
    dkQueueMakerDefaults(&uqm, s_dev);
    uqm.flags = DkQueueFlags_Graphics;
    s_upQueue = dkQueueCreate(&uqm);
    if (s_upQueue) {
      switch_run_logf("[dk] upload queue ready (separate from present)");
    } else {
      switch_run_logf("[dk] upload queue create FAILED - uploads share the present queue");
    }
  }
  // T05: load the embedded splash shaders into the code memblock. The two shaders share one
  // offset, so the second begins where the first (aligned) code section ends. A failure is
  // not fatal -- we log it, leave s_have_shaders = 0, and let init return true (no shader
  // means the 2D splash still works via the GL-path fallback, so the device is still usable).
  {
    uint32_t code_off = 0;
    bool vert_ok = load_shader(&s_splashVsh, splash_vert, splash_vert_size, &code_off);
    bool frag_ok = load_shader(&s_splashFsh, splash_frag, splash_frag_size, &code_off);
    s_have_shaders = vert_ok && frag_ok;
    switch_run_logf("[dk] shaders: splash vert %u B, frag %u B, code used %u KB (vert_ok=%d frag_ok=%d)",
                    splash_vert_size, splash_frag_size, (code_off + 1023) / 1024, vert_ok, frag_ok);

    // T33a (P3.2): append the tfrag3 .dksh pair right AFTER the splash pair in the SAME
    // 64 KB code memblock (0x100-aligned, vert then frag). Nothing reads s_have_tfrag3
    // yet, so this is inert except for the memblock bytes + the flag. We project the final
    // footprint ourselves first so an overflow skips BOTH (load_shader alone would skip only
    // the overflowing one); the projection mirrors load_shader exactly (same align + code_sz).
    {
      DkshHeader vh, fh;
      memcpy(&vh, tfrag3_vert_dksh, sizeof(vh));
      memcpy(&fh, tfrag3_frag_dksh, sizeof(fh));
      uint32_t v_off = align_up(code_off, DK_SHADER_CODE_ALIGNMENT);  // vert start
      uint32_t f_off = align_up(v_off + vh.code_sz, DK_SHADER_CODE_ALIGNMENT);  // frag start
      uint32_t end = f_off + fh.code_sz;
      if (end > kCodePool) {
        // device stays up, splash pair already loaded and untouched; do not grow the pool.
        switch_run_logf("[dk] tfrag3 shader code overflow (%u/65536)", end);
      } else {
        bool tv = load_shader(&s_tfrag3Vsh, tfrag3_vert_dksh, tfrag3_vert_dksh_size, &code_off);
        bool tf = load_shader(&s_tfrag3Fsh, tfrag3_frag_dksh, tfrag3_frag_dksh_size, &code_off);
        s_have_tfrag3 = tv && tf;
      }
    }
    // T72 (AI-assisted): SHRUB pair after tfrag3, same overflow projection.
    {
      DkshHeader vh, fh;
      memcpy(&vh, shrub_vert_dksh, sizeof(vh));
      memcpy(&fh, shrub_frag_dksh, sizeof(fh));
      uint32_t v_off = align_up(code_off, DK_SHADER_CODE_ALIGNMENT);
      uint32_t f_off = align_up(v_off + vh.code_sz, DK_SHADER_CODE_ALIGNMENT);
      uint32_t end = f_off + fh.code_sz;
      if (end > kCodePool) {
        switch_run_logf("[dk] shrub shader code overflow (%u/65536)", end);
      } else {
        bool sv = load_shader(&s_shrubVsh, shrub_vert_dksh, shrub_vert_dksh_size, &code_off);
        bool sf = load_shader(&s_shrubFsh, shrub_frag_dksh, shrub_frag_dksh_size, &code_off);
        s_have_shrub = sv && sf;
        switch_run_logf("[dk] shrub shaders %s, code used %u KB", s_have_shrub ? "ok" : "FAILED",
                        (code_off + 1023) / 1024);
      }
    }
  }
  memset(s_sliceFence, 0, sizeof(s_sliceFence));
  memset(&s_uploadFence, 0, sizeof(s_uploadFence));
  s_frame = 0;
  s_up = true;
  switch_run_logf("[dk] device up: swapchain %dx%d x%u, pools %llu KB (fb %u KB each), %.1f ms",
                  fb_width, fb_height, kFbNum, s_pool_bytes / 1024, fbSize / 1024, ms_since(t0));
  log_heap("dk after init");
  return true;
}

bool upload_splash(const unsigned char* rgba, int width, int height) {
  if (!s_up || !rgba || width <= 0 || height <= 0) {
    return false;
  }
  const uint32_t bytes = (uint32_t)width * (uint32_t)height * 4;
  if (bytes > kStagingMax) {
    switch_run_logf("[dk] splash %dx%d too big for staging (%u KB)", width, height, bytes / 1024);
    return false;
  }
  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, s_dev);
  lm.flags = DkImageFlags_Usage2DEngine;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = (uint32_t)width;
  lm.dimensions[1] = (uint32_t)height;
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &lm);
  const uint32_t size =
      align_up((uint32_t)dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout));
  s_splashMem = make_block(size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "splash");
  if (!s_splashMem) {
    return false;
  }
  dkImageInitialize(&s_splash, &layout, s_splashMem, 0);

  // Frames may already be in flight using cmd slice 0 and the queue: drain before reusing
  // that command memory (boot-time only, once).
  dkQueueWaitIdle(s_queue);
  memcpy((uint8_t*)dkMemBlockGetCpuAddr(s_dataMem) + kOffStaging, rgba, bytes);
  dkCmdBufClear(s_cmd);
  dkCmdBufAddMemory(s_cmd, s_dataMem, 0, kCmdSlice);
  DkImageView view;
  dkImageViewDefaults(&view, &s_splash);
  DkCopyBuf src = {dkMemBlockGetGpuAddr(s_dataMem) + kOffStaging, 0, 0};
  DkImageRect rect = {0, 0, 0, (uint32_t)width, (uint32_t)height, 1};
  dkCmdBufCopyBufferToImage(s_cmd, &src, &view, &rect, 0);
  dkQueueSubmitCommands(s_queue, dkCmdBufFinishList(s_cmd));
  dkQueueWaitIdle(s_queue);  // boot-time only; staging is reused by nothing else yet
  dkCmdBufClear(s_cmd);
  s_splash_w = width;
  s_splash_h = height;
  s_have_splash = true;
  // T06: create the image + sampler descriptors for the uploaded splash so a 3D splash shader
  // (T07) can sample it. Both go into the data pool's descriptor region at kOffDesc. The data
  // block is CpuUncached, so a plain memcpy of the CPU-side descs is safe (no flushing needed).
  {
    DkImageView v;
    dkImageViewDefaults(&v, &s_splash);  // single layer, full 0..mipCount range
    DkImageDescriptor idesc;
    dkImageDescriptorInitialize(&idesc, &v, /*usesLoadOrStore*/ false, /*decayMS*/ false);
    DkSampler smp;
    dkSamplerDefaults(&smp);
    smp.minFilter = DkFilter_Linear;
    smp.magFilter = DkFilter_Linear;
    smp.wrapMode[0] = DkWrapMode_ClampToEdge;
    smp.wrapMode[1] = DkWrapMode_ClampToEdge;
    smp.wrapMode[2] = DkWrapMode_ClampToEdge;
    DkSamplerDescriptor sdesc;
    dkSamplerDescriptorInitialize(&sdesc, &smp);
    uint8_t* base = (uint8_t*)dkMemBlockGetCpuAddr(s_dataMem);
    memcpy(base + kOffDesc, &idesc, sizeof(idesc));
    memcpy(base + kOffDesc + DK_IMAGE_DESCRIPTOR_ALIGNMENT, &sdesc, sizeof(sdesc));
    s_have_desc = true;
  }

  // T34a (P3.2): write the game's sampler table into the sampler-descriptor region of the
  // data pool (kOffSamplers, appended after the UBO ring). Pure CPU writes into the CpuUncached
  // pool, so a plain memcpy of each DkSamplerDescriptor is safe (no flush). Inert until T38 (first
  // P3 renderer) consumes these samplers; nothing reads these bytes yet, so this is additive-only.
  // The mapping dkpipe::to_deko3d(const SamplerDesc&, DkSampler*) (dk_state.h, T16) is pure and
  // void, and dkSamplerDescriptorInitialize is void; neither can fail, so the "log on odd
  // input" path the task names cannot trigger and is intentionally omitted. `base` is local to
  // the splash block above, so recompute the CPU base here.
  {
    uint8_t* sbase = (uint8_t*)dkMemBlockGetCpuAddr(s_dataMem);
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
      DkSampler smp;
      dkpipe::to_deko3d(kSamplers[i], &smp);
      DkSamplerDescriptor sdesc;
      dkSamplerDescriptorInitialize(&sdesc, &smp);
      memcpy(sbase + kOffSamplers + i * DK_SAMPLER_DESCRIPTOR_ALIGNMENT, &sdesc, sizeof(sdesc));
    }
  }
  switch_run_logf("[dk] splash %dx%d uploaded (%u KB image)", width, height, size / 1024);
  return true;
}

// T58: the queue every upload goes to (falls back to the present queue).
static DkQueue upload_queue() {
  return s_upQueue ? s_upQueue : s_queue;
}

CopyResult copy_to_gpu(uint64_t gpu_dst, const void* src, uint32_t size) {
  CopyResult r;
  if (!s_up || src == nullptr || size == 0) {
    switch_run_logf("[dk] copy_to_gpu failed: up=%d src=%p size=%u", (int)s_up, src, size);
    return r;
  }
  // The whole call is timed with ms_since, the same helper FrameTiming uses.
  const u64 t0 = armGetSystemTick();
  const uint8_t* base = static_cast<const uint8_t*>(src);
  // The staging area is a CPU-visible, GPU-cached region in the data pool. Its CPU and
  // GPU addresses share the same byte offset (kOffStaging) into s_dataMem.
  uint8_t* staging_cpu = (uint8_t*)dkMemBlockGetCpuAddr(s_dataMem) + kOffStaging;
  const DkGpuAddr staging_gpu = dkMemBlockGetGpuAddr(s_dataMem) + kOffStaging;
  // Each chunk is at most kStagingMax bytes. Chunk count is computed with uint64 to
  // avoid a 32-bit overflow if `size + kStagingMax - 1` crosses 0xFFFFFFFF.
  const uint32_t chunk_max = (uint32_t)kStagingMax;
  const uint32_t n_chunks = (uint32_t)(((uint64_t)size + chunk_max - 1) / chunk_max);
  // Walk every chunk. For each one: fill staging, re-record the upload command buffer
  // (using the dedicated kOffUpload command region, NOT a per-frame cmd slice — the
  // present loop owns those), submit, signal the upload fence, and wait. The fence
  // wait makes the upload command memory safe to reuse for the next chunk (same rule
  // as the per-frame cmd slices in present_frame), and it ensures the bytes in staging
  // cannot be overwritten before the GPU is done with them.
  DkGpuAddr dst = (DkGpuAddr)gpu_dst;
  for (uint32_t c = 0; c < n_chunks; c++) {
    const uint32_t off = c * chunk_max;
    const uint32_t rem = size - off;
    const uint32_t chunk = (rem < chunk_max) ? rem : chunk_max;
    memcpy(staging_cpu, base + off, chunk);
    dkCmdBufClear(s_cmd);
    dkCmdBufAddMemory(s_cmd, s_dataMem, kOffUpload, kCmdUpload);
    dkCmdBufCopyBuffer(s_cmd, staging_gpu, dst + (DkGpuAddr)off, chunk);
    const DkQueue uq = upload_queue();  // T58
    dkQueueSubmitCommands(uq, dkCmdBufFinishList(s_cmd));
    dkQueueSignalFence(uq, &s_uploadFence, /*flush*/ true);
    dkQueueFlush(uq);  // T52: SignalFence's flush = CACHE flush; this sends the work
    dkFenceWait(&s_uploadFence, -1);
  }
  r.ms = ms_since(t0);
  r.ok = true;
  // T57: the Loader now calls this for every geometry chunk (T55), so log only the first
  // 8 calls plus slow ones (> 5 ms), at most 40 lines per run.
  static unsigned s_logged = 0;
  if (s_logged < 8 || (r.ms > 5.0 && s_logged < 40)) {
    s_logged++;
    switch_run_logf("[dk] copy_to_gpu %u KB in %u chunks, %.3f ms", size / 1024, n_chunks, r.ms);
  }
  return r;
}

FrameTiming present_frame(bool show_splash) {
  FrameTiming ft{};
  if (!s_up) {
    return ft;
  }
  const unsigned slice = s_frame % kFbNum;
  u64 t = armGetSystemTick();
  dkFenceWait(&s_sliceFence[slice], -1);  // this slice's previous commands are done
  // T20: the slice fence for THIS frame (s_frame % kFbNum) has just finished; reuse that same
  // counter to select the UBO ring segment (kFbNum frames == s_sliceFence slots), resetting its
  // cursor for the frame about to be recorded. Safe here: frame-(N-2) GPU work is done.
  s_uboRing.begin_frame((uint64_t)s_frame);
  const int img = dkQueueAcquireImage(s_queue, s_swap);
  ft.wait_ms = ms_since(t);

  t = armGetSystemTick();
  dkCmdBufClear(s_cmd);
  dkCmdBufAddMemory(s_cmd, s_dataMem, slice * kCmdSlice, kCmdSlice);
  DkImageView fbView;
  dkImageViewDefaults(&fbView, &s_fb[img]);
  // T60: the world pass needs depth; everything else keeps the colour-only target.
  const bool world = s_world != nullptr && s_have_depth && !show_splash;
  DkImageView depthView;
  if (world) {
    dkImageViewDefaults(&depthView, &s_depth);
    // Uploads (textures, level geometry, CPU-written index lists) arrive from another queue
    // or the CPU: drop the stale texture/shader/descriptor/L2 cache lines first.
    dkCmdBufBarrier(s_cmd, DkBarrier_None,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Shader |
                        DkInvalidateFlags_Descriptors | DkInvalidateFlags_L2Cache);
  }
  dkCmdBufBindRenderTarget(s_cmd, &fbView, world ? &depthView : nullptr);
  DkScissor sc = {0, 0, (uint32_t)s_fb_w, (uint32_t)s_fb_h};
  dkCmdBufSetScissors(s_cmd, 0, &sc, 1);
  // Phase 1 draws no game frames yet. A slow dark-blue pulse instead of plain black makes
  // "alive, not frozen" visible on the console at a glance.
  const float pulse = 0.04f + 0.04f * (float)((s_frame / 2) % 60) / 59.0f;
  dkCmdBufClearColorFloat(s_cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, pulse, 1.0f);
  if (world) {
    // GL clears depth to 0 and draws with GEQUAL/GREATER (OpenGLRenderer.cpp glClearDepthf(0)).
    dkCmdBufClearDepthStencil(s_cmd, true, 0.0f, 0xFF, 0);
    s_world(s_fb_w, s_fb_h);
  }
  if (s_have_splash && show_splash) {
    if (kSplash3D && s_have_shaders && s_have_desc) {
      // T07: 3D-pipeline splash (full-screen quad from gl_VertexID). No barrier needed
      // before this -- both the clear above and this draw run on the 3D engine, so the
      // ordering is implicit. The 2D blit branch (below) is the fallback for when the 3D
      // path is unavailable (default: kSplash3D = false).
      record_splash_3d((uint32_t)s_fb_w, (uint32_t)s_fb_h);
    } else {
      // aspect-correct fit, centered (same math as splash.vert on the GL path)
      const float tex_aspect = (float)s_splash_w / (float)s_splash_h;
      const float fb_aspect = (float)s_fb_w / (float)s_fb_h;
      uint32_t dw = (uint32_t)s_fb_w, dh = (uint32_t)s_fb_h;
      if (fb_aspect > tex_aspect) {
        dw = (uint32_t)((float)s_fb_h * tex_aspect);
      } else {
        dh = (uint32_t)((float)s_fb_w / tex_aspect);
      }
      DkImageView srcView;
      dkImageViewDefaults(&srcView, &s_splash);
      DkImageRect srcRect = {0, 0, 0, (uint32_t)s_splash_w, (uint32_t)s_splash_h, 1};
      DkImageRect dstRect = {((uint32_t)s_fb_w - dw) / 2, ((uint32_t)s_fb_h - dh) / 2, 0, dw, dh, 1};
      // the clear above runs on the 3D engine, the blit on the 2D engine: order them
      dkCmdBufBarrier(s_cmd, DkBarrier_Full, 0);
      // T62: OriginLowerLeft (T60) stores the framebuffer upside down and the swapchain asks
      // the compositor to flip it back; the 2D engine ignores the origin mode, so pre-flip.
      dkCmdBufBlitImage(s_cmd, &srcView, &srcRect, &fbView, &dstRect,
                        DkBlitFlag_FilterLinear | DkBlitFlag_FlipY, 0);
    }
  }
  dkCmdBufSignalFence(s_cmd, &s_sliceFence[slice], true);
  const DkCmdList list = dkCmdBufFinishList(s_cmd);
  ft.record_ms = ms_since(t);

  t = armGetSystemTick();
  dkQueueSubmitCommands(s_queue, list);
  dkQueuePresentImage(s_queue, s_swap, img);
  ft.submit_ms = ms_since(t);
  s_frame++;
  return ft;
}

// T20: the P2.4 per-frame UBO ring. Null when the device is down; callers should check
// segment_size() != 0 to see whether a segment was actually carved at init (FrameRing has
// no public usable() accessor, so segment_size() being non-zero means the ring is usable).
dkring::FrameRing* ubo_ring() {
  return is_up() ? &s_uboRing : nullptr;
}

// T21/T57: the P2.3 level-data buffer pool (384 MB). Null when the device is down; the caller then
// skips the upload (FIX 33 caller contract). Nothing acquires from it yet (a P3 renderer first).
dkpool::BufferPool* buffer_pool() {
  return is_up() ? &s_bufferPool : nullptr;
}

// T22a: dk-TU internals pass-throughs. Each is a one-liner over this TU's module
// state (or the existing offset constants) so T22b's P2.3 dk_texture_gpu store can
// reach the raw deko3d handles and the upload staging without the public (deko3d-free)
// dk_device.h interface. submit_upload_cmds is the exact copy_to_gpu() tail (T18).
DkDevice device_handle() { return s_dev; }
DkQueue queue_handle() { return s_queue; }
DkCmdBuf cmd_handle() { return s_cmd; }

uint8_t* staging_cpu() { return (uint8_t*)dkMemBlockGetCpuAddr(s_dataMem) + kOffStaging; }
DkGpuAddr staging_gpu() { return dkMemBlockGetGpuAddr(s_dataMem) + kOffStaging; }
uint8_t* data_cpu() { return (uint8_t*)dkMemBlockGetCpuAddr(s_dataMem); }

uint32_t upload_cmd_off() { return kOffUpload; }
uint32_t upload_cmd_size() { return kCmdUpload; }

// T51 (AI-assisted): bounded wait. The T49 hardware run hung forever on the first texture
// upload (dkFenceWait(-1) never returned, render thread frozen, no log). A 1 s timeout turns
// that into one log line + "skip": after a timeout the queue is treated as wedged and every
// later upload is refused (returns false) instead of submitting more work behind it.
bool submit_upload_cmds(DkCmdList list) {
  static bool s_wedged = false;
  if (s_wedged) {
    return false;
  }
  const DkQueue uq = upload_queue();  // T58
  dkQueueSubmitCommands(uq, list);
  dkQueueSignalFence(uq, &s_uploadFence, /*flush*/ true);
  // T52 (AI-assisted) ROOT CAUSE of the T49/T51 upload hang: dkQueueSignalFence's `flush`
  // only flushes GPU CACHES; the commands stay in the CPU-side queue until dkQueueFlush.
  // deko3d's own dkQueueWaitIdle = signalFence(true) + flush() + wait (dk_queue.cpp L359).
  dkQueueFlush(uq);
  const DkResult r = dkFenceWait(&s_uploadFence, 1000000000LL);  // 1 s, in ns
  if (r != DkResult_Success) {
    s_wedged = true;
    switch_run_logf("[dk] upload fence wait FAILED (DkResult %d) - uploads disabled", (int)r);
    return false;
  }
  return true;
}

// T22b: P2.3 texture-pool accessors. The image-descriptor slot i for a live texture
// sits at (kOffDesc + 0x100 + i * DK_IMAGE_DESCRIPTOR_ALIGNMENT); desc_off()/desc_slot_stride()
// expose the base + stride so dk_texture_gpu.cpp never hard-codes the layout offset.
// data_mem() exposes the data DkMemBlock so the texture TU can register it with
// dkCmdBufAddMemory for the buffer->image upload (same memblock as data_cpu()/staging_*,
// but as a DkMemBlock since AddMemory needs the handle, not just a CPU pointer).
uint32_t desc_off() { return kOffDesc; }
uint32_t desc_slot_stride() { return DK_IMAGE_DESCRIPTOR_ALIGNMENT; }
DkMemBlock data_mem() { return s_dataMem; }

// T33b: tfrag3 shader-pair getters. Return the T33a-loaded DkShaders by value
// (mirrors the by-value handle getters above, e.g. device_handle() returns the
// DkDevice by value); the loaded-bool gates validity. Nothing calls these yet
// (T38 does) so they are written to be gc-strippable.
DkShader tfrag3_vsh() { return s_tfrag3Vsh; }
DkShader tfrag3_fsh() { return s_tfrag3Fsh; }
bool tfrag3_shaders_loaded() { return s_have_tfrag3; }
DkShader shrub_vsh() { return s_shrubVsh; }
DkShader shrub_fsh() { return s_shrubFsh; }
bool shrub_shaders_loaded() { return s_have_shrub; }

// T34b: expose the T34a sampler descriptor set to draw-time TUs (T38).
// sampler_set_off() = kOffSamplers (byte offset of the sampler region into the
// data memblock); sampler_set_count() = kSamplerCount (16, sizeof(kSamplers)).
// Uncalled until T38, so both are written to be gc-strippable.
uint32_t sampler_set_off() { return kOffSamplers; }
uint32_t sampler_set_count() { return kSamplerCount; }

// T60: see dk_device.h.
void set_world_recorder(void (*fn)(int fb_w, int fb_h)) {
  s_world = fn;
}

}  // namespace dkdev

#endif  // __SWITCH__
