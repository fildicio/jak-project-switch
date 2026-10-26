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

// DK_HOST_REPLAY (R1): the replay test declares the same API and defines host stand-ins.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)

#include <cstdint>  // uint64_t/uint32_t for CopyResult / copy_to_gpu (plain header, no <switch.h>)
#include "game/graphics/deko3d_renderer/dk_ring.h"  // T20: P2.4 UBO ring (plain, deko3d-free header)
#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"  // T21: P2.3 buffer pool (plain, deko3d-free header)

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
  double fence_ms;   // T83: part of wait_ms: GPU still busy with this slice's older frame
  double acquire_ms; // T83: part of wait_ms: swapchain image acquire (vsync)
};
//! T84: GPU timestamp marks inside the world pass. Mark 0 is written before the world
//! recorder, mark kGpuMarks-1 after it; the recorder may write 1..kGpuMarks-2 between passes.
//! gpu_mark_ms(i) = ms after mark 0 in the last finished frame of the slice just reused,
//! or -1 when that mark was not written.
constexpr int kGpuMarks = 16;
void gpu_mark(int i);
double gpu_mark_ms(int i);
//! T83: hardware tick counter (armGetSystemTick) and tick difference in ms, for callers
//! that cannot include <switch.h> (its u128 typedef clashes with common/common_types.h).
uint64_t tick_now();
double tick_ms(uint64_t a, uint64_t b);
//! Acquire, clear to black, blit the splash (if uploaded and show_splash) aspect-correct,
//! present. dkQueueAcquireImage is where vsync pacing blocks.
FrameTiming present_frame(bool show_splash);

//! P2.4: the per-frame UBO ring carved from the data memblock. nullptr when the
//! device is down or the ring is unusable. begin_frame() is called inside
//! present_frame; renderers alloc() with align = DK_UNIFORM_BUF_ALIGNMENT (0x100).
dkring::FrameRing* ubo_ring();

//! T106 (AI-assisted): sampler-descriptor slot of a dkgl sampler combo (bit0
//! min_linear | bit1 mag_linear | bit2 mip_linear | bit3 repeat), written at splash
//! init right after the T34a table. Returns kSamplerCount + combo.
u32 dkgl_sampler_slot(u32 combo);

//! P2.3: the level-data buffer pool, a dedicated 384 MB deko3d memblock (T57). nullptr when the
//! device is down; callers MUST skip the upload when this is null (FIX 33 caller contract)
//! and must also skip on a !ok() acquire(). Inert today: nothing acquires yet (a P3 renderer
//! is the first consumer).
dkpool::BufferPool* buffer_pool();

//! Bytes held by this module's memory blocks (for the [dk] line / P0.6).
unsigned long long pool_bytes();

  //! 1 = docked (TV), 0 = handheld. Polled once per frame (cheap, no IPC wait).
  int operation_mode();

  //! true when this applet has focus (not in HOME menu / overlay / sleep).
  bool in_focus();
  //! Wait until the GPU queue has finished all submitted work.
  void wait_idle();

//! T60 (AI-assisted): P3 world pass. When set (and the splash is not showing), present_frame
//! binds a depth buffer, clears it to 0 and calls fn(fb_w, fb_h) to record draws into the
//! frame command buffer. fn must not submit or clear the cmd buffer. nullptr = off.
void set_world_recorder(void (*fn)(int fb_w, int fb_h));

//! On-demand frame grab (AI-assisted, wifi diagnostics): arm a one-shot readback of the
//! next presented frame. The frame is copied on the GPU into a CPU-readable memblock and
//! streamed to the wifi log host kFbNum frames later (switch_net_send_frame in
//! game/switch/, receiver scripts/switch/frame-receiver.py). Combo: R3+Plus
//! (dk_renderer.cpp). Nothing allocates until the first grab; no SD writes.
void request_frame_grab();

}  // namespace dkdev

#endif
