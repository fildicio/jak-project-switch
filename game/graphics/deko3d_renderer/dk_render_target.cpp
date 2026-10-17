/*!
 * @file dk_render_target.cpp
 * deko3d migration Phase 3.1 (T27, AI-assisted): the first slice of the deko3d
 * equivalent of the GL FBO colour attachment. It owns ONE RGBA8 colour render
 * target (a single DkImage in its OWN image memblock, plus its DkImageView) and
 * can bind it to the current command buffer with no depth target (the depth, MSAA
 * resolve and CPU readback are the later P3.1 slices).
 *
 * Inert: nothing outside this file calls dkrt::* yet (the 2D-engine blits of the
 * later P3.1 slices are the first consumer). So the file is wrapped in
 * `#if defined(__SWITCH__)` exactly like dk_texture_gpu.cpp -- only a deko3d TU
 * may include <deko3d.h>, and the public interface in dk_render_target.h stays
 * plain (host-inspectable, no deko3d).
 *
 * Memory map (reviewer-verified against T22b):
 *   - the single DkImage -> a NEW, dedicated image memblock OWNED BY THIS FILE,
 *     sized to the image layout and rounded up to DK_MEMBLOCK_ALIGNMENT (0x1000).
 *     It is the image's own block, so the DkImage is placed at offset 0 (never
 *     any other offset, and never a shared/pooled block).
 *   - the memblock uses GpuCached | Image flags (NOT the CpuUncached | GpuCached
 *     default from dkMemBlockMakerDefaults — this is a GPU image, not CPU-written
 *     data), so we never go through make_block() / buffer_pool().
 */

#if defined(__SWITCH__)

#include "game/graphics/deko3d_renderer/dk_render_target.h"

#include <deko3d.h>
#include "game/graphics/pipelines/dk_device_internal.h"  // T22a: dk-TU internals

// switch_run_logf lives in run_log.h, which cannot be included next to <switch.h>
// (u128 collision). Same hand declaration as dk_device.cpp / dk_texture_gpu.cpp.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace dkrt {

// File-static state: exactly one memblock, one image, one view, plus the
// dimensions of the last init(). DkImage / DkImageView are opaque VALUE structs
// (DK_DECL_OPAQUE, deko3d.h:80-82), so plain value members are the right shape.
static DkMemBlock s_mem = nullptr;          // the dedicated image memblock
static DkImage    s_img;                    // the colour target image
static DkImageView s_view;                  // the view bound as the render target
static uint32_t   s_width  = 0;
static uint32_t   s_height = 0;

bool init(const Desc& d) {
  // (a) build the image layout for a 1-mip 2D RGBA8 target used as a render
  // target AND as a 2D-engine blit source/destination.
  DkImageLayoutMaker mk;
  DkImageLayout layout;
  dkImageLayoutMakerDefaults(&mk, dkdev::device_handle());
  mk.type         = DkImageType_2D;
  mk.dimensions[0] = d.width;
  mk.dimensions[1] = d.height;
  mk.dimensions[2] = 1;
  mk.mipLevels     = 1;
  mk.format        = DkImageFormat_RGBA8_Unorm;
  mk.flags         = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
  dkImageLayoutInitialize(&layout, &mk);

  // (b) round the layout size up to a DK_MEMBLOCK_ALIGNMENT (0x1000) so the
  // memblock is a valid, aligned allocation, then give the image its OWN block.
  const uint32_t blk =
      (uint32_t)((dkImageLayoutGetSize(&layout) + 0xFFF) & ~0xFFFu);  // >= 0x1000
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, dkdev::device_handle(), blk);
  mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;  // image, not CPU data
  s_mem = dkMemBlockCreate(&mm);
  if (s_mem == nullptr) {
    // One failure line, with the dimensions and the block size in MB.
    switch_run_logf("[dk] rt init failed (%u x %u, %u MB)", d.width, d.height,
                    (unsigned)(blk / (1024 * 1024)));
    s_width = 0;
    s_height = 0;
    return false;
  }

  // (c) the single DkImage sits at offset 0 of its OWN block (never any other
  // offset), and the view mirrors it with the default swizzle/format.
  dkImageInitialize(&s_img, &layout, s_mem, 0);
  dkImageViewDefaults(&s_view, &s_img);

  // (d) record the dimensions and log one success line, e.g. "[dk] rt 1280x720 3.5 MB".
  s_width = d.width;
  s_height = d.height;
  // block size in MB with one decimal (blk is 0x1000-aligned; /1024 = KB, the
  // fractional digit comes from (bytes % 1 MB) * 10 / 1 MB expressed as a 0.1 MB unit).
  const uint32_t mb10 = (blk / (1024 * 1024)) * 10 +
                        ((blk % (1024 * 1024)) * 10 / (1024 * 1024));
  switch_run_logf("[dk] rt %ux%u %u.%u MB", d.width, d.height,
                  (unsigned)(mb10 / 10), (unsigned)(mb10 % 10));
  return true;
}

void bind() {
  // Bind this colour view as the sole render target (no depth in this slice).
  dkCmdBufBindRenderTarget(dkdev::cmd_handle(), &s_view, nullptr);
}

void teardown() {
  if (s_mem != nullptr) {
    dkMemBlockDestroy(s_mem);
  }
  s_mem = nullptr;
  s_img = {};
  s_view = {};
  s_width = 0;
  s_height = 0;
}

bool valid()        { return s_mem != nullptr; }
uint32_t width()     { return s_width; }
uint32_t height()    { return s_height; }

}  // namespace dkrt

#endif  // __SWITCH__
