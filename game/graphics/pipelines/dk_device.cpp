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
constexpr uint32_t kCmdSlice = 64 * 1024;  // per in-flight frame; P1 records ~10 commands
constexpr uint32_t kOffStaging = kFbNum * kCmdSlice;
constexpr uint32_t kStagingMax = 512 * 1024 * 4;  // up to 512x1024 RGBA splash
constexpr uint32_t kDataPool = kOffStaging + kStagingMax;
// Never take the heap below this much headroom: the GOAL runtime still has to load
// levels, and FIX 34c showed how little the console can spare.
constexpr unsigned long long kMinHeadroom = 24ull * 1024 * 1024;

bool s_up = false;
int s_fb_w = 0, s_fb_h = 0;
DkDevice s_dev = nullptr;
DkQueue s_queue = nullptr;
DkMemBlock s_fbMem = nullptr, s_dataMem = nullptr, s_splashMem = nullptr;
DkImage s_fb[kFbNum];
DkSwapchain s_swap = nullptr;
DkCmdBuf s_cmd = nullptr;
DkFence s_sliceFence[kFbNum];
unsigned s_frame = 0;
unsigned long long s_pool_bytes = 0;

bool s_have_splash = false;
DkImage s_splash;
int s_splash_w = 0, s_splash_h = 0;

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

void shutdown() {
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
  DkMemBlock* blocks[] = {&s_splashMem, &s_dataMem, &s_fbMem};
  for (DkMemBlock* b : blocks) {
    if (*b) {
      dkMemBlockDestroy(*b);
      *b = nullptr;
    }
  }
  if (s_dev) {
    dkDeviceDestroy(s_dev);
    s_dev = nullptr;
  }
  if (s_up) {
    switch_run_logf("[dk] device shut down");
  }
  s_have_splash = false;
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

  // Budget check BEFORE any big allocation (see file comment).
  const unsigned long long need = (unsigned long long)kFbNum * fbSize + kDataPool;
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
  if (!s_fbMem || !s_dataMem) {
    shutdown();
    return false;
  }

  DkImage const* swapImgs[kFbNum];
  for (unsigned i = 0; i < kFbNum; i++) {
    dkImageInitialize(&s_fb[i], &fbLayout, s_fbMem, i * fbSize);
    swapImgs[i] = &s_fb[i];
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
  memset(s_sliceFence, 0, sizeof(s_sliceFence));
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
  switch_run_logf("[dk] splash %dx%d uploaded (%u KB image)", width, height, size / 1024);
  return true;
}

FrameTiming present_frame(bool show_splash) {
  FrameTiming ft{};
  if (!s_up) {
    return ft;
  }
  const unsigned slice = s_frame % kFbNum;
  u64 t = armGetSystemTick();
  dkFenceWait(&s_sliceFence[slice], -1);  // this slice's previous commands are done
  const int img = dkQueueAcquireImage(s_queue, s_swap);
  ft.wait_ms = ms_since(t);

  t = armGetSystemTick();
  dkCmdBufClear(s_cmd);
  dkCmdBufAddMemory(s_cmd, s_dataMem, slice * kCmdSlice, kCmdSlice);
  DkImageView fbView;
  dkImageViewDefaults(&fbView, &s_fb[img]);
  dkCmdBufBindRenderTarget(s_cmd, &fbView, nullptr);
  DkScissor sc = {0, 0, (uint32_t)s_fb_w, (uint32_t)s_fb_h};
  dkCmdBufSetScissors(s_cmd, 0, &sc, 1);
  // Phase 1 draws no game frames yet. A slow dark-blue pulse instead of plain black makes
  // "alive, not frozen" visible on the console at a glance.
  const float pulse = 0.04f + 0.04f * (float)((s_frame / 2) % 60) / 59.0f;
  dkCmdBufClearColorFloat(s_cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, pulse, 1.0f);
  if (s_have_splash && show_splash) {
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
    dkCmdBufBlitImage(s_cmd, &srcView, &srcRect, &fbView, &dstRect, DkBlitFlag_FilterLinear, 0);
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

}  // namespace dkdev

#endif  // __SWITCH__
