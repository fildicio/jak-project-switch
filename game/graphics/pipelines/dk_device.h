#pragma once

/*!
 * @file dk_device.h
 * deko3d migration Phase 1 (AI-assisted): the libnx/deko3d-facing half of the deko3d
 * renderer module. Plain interface on purpose: like game/switch/platform.h, nothing
 * reachable from here may include <switch.h> (its u128 typedef collides with
 * common/common_types.h), so the implementation in dk_device.cpp is the only TU that
 * sees libnx and deko3d headers. dk_renderer.cpp (the GfxRendererModule) talks to it
 * only through these functions.
 */

#if defined(__SWITCH__)

#include <cstdint>  // uint64_t/uint32_t for CopyResult / copy_to_gpu (plain header, no <switch.h>)

namespace dkdev {

//! newlib heap accounting (P0.6). deko3d memblocks and Mesa's GPU buffers are both
//! carved out of this heap, so svcGetInfo totals cannot see them -- this can.
struct HeapInfo {
  unsigned long long arena_bytes;      // heap grown so far (sbrk'd)
  unsigned long long in_use_bytes;     // malloc'd and live
  unsigned long long free_in_arena;    // freed chunks inside the arena
  unsigned long long unreserved_bytes; // never-sbrk'd space up to fake_heap_end
  unsigned long long total_free() const { return free_in_arena + unreserved_bytes; }
};
HeapInfo heap_info();
//! One "[heap] <tag> ..." line in gk_run_log.txt.
void log_heap(const char* tag);

//! Device, memory blocks, queue and swapchain on the default native window.
//! Returns false (and logs why) on any failure, including a heap pre-check, so the
//! caller can fall back to OpenGL. Never initializes SDL video.
bool init(int fb_width, int fb_height);
bool is_up();
void shutdown();

void set_swap_interval(unsigned interval);

//! Upload the boot splash (RGBA8, row-major, top row first). Copies the pixels.
bool upload_splash(const unsigned char* rgba, int width, int height);

//! Result of a copy_to_gpu() call.
struct CopyResult {
  bool ok = false;
  double ms = 0.0;  // total wall time of the call, including the per-chunk fence waits
};

//! Copy `size` bytes from CPU `src` to the GPU address `gpu_dst` (which MUST lie inside a
//! dkdev memblock), through the data-pool staging area, in chunks of kStagingMax. The call
//! is blocking: it submits a chunk to the queue, signals a fence, and waits for that fence
//! before copying the next chunk. This is the runtime upload primitive every later
//! texture/buffer upload will build on (T19+). Nothing calls it yet (loader integration
//! is a later task). Render thread only. Returns {false, 0} when the device is down,
//! `src` is null, or `size` is zero.
CopyResult copy_to_gpu(uint64_t gpu_dst, const void* src, uint32_t size);

struct FrameTiming {
  double wait_ms;    // slice fence + swapchain image acquire (vsync pacing lives here)
  double record_ms;  // command recording
  double submit_ms;  // submit + present
};
//! Acquire, clear to black, blit the splash (if uploaded and show_splash) aspect-correct,
//! present. dkQueueAcquireImage is where vsync pacing blocks.
FrameTiming present_frame(bool show_splash);

//! Bytes held by this module's memory blocks (for the [dk] line / P0.6).
unsigned long long pool_bytes();

  //! 1 = docked (TV), 0 = handheld. Polled once per frame (cheap, no IPC wait).
  int operation_mode();

  //! true when this applet has focus (not in HOME menu / overlay / sleep).
  bool in_focus();
  //! Wait until the GPU queue has finished all submitted work.
  void wait_idle();

}  // namespace dkdev

#endif
