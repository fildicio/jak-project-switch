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
 * T29b (slice 3b, 2026-10-18): adds dkrt::FullTarget -- a colour+optional-depth
 * render target as ONE unit, backed by a fixed file-static pool (g_fulltargetPool).
 * FullTarget::bind() routes through the same bind_views() as the file-static
 * dkrt::bind(), so the T28 depth-view path is never duplicated. Like the
 * T27/T28 free functions, FullTarget is inert (no caller yet); T30 (MSAA/resolve)
 * and T31 (blit) are the next P3.1 slices.
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
static DkMemBlock s_mem = nullptr;          // the dedicated colour image memblock
static DkImage    s_img;                    // the colour target image
static DkImageView s_view;                  // the view bound as the render target
static uint32_t   s_width  = 0;
static uint32_t   s_height = 0;
// T28 (slice 2): the optional Z24S8 depth attachment, in its OWN SECOND memblock
// (never mixed with the colour block — T27's colour block stays untouched).
static DkMemBlock s_depthMem = nullptr;     // the dedicated depth image memblock
static DkImage    s_depthImg;               // the Z24S8 depth target image
static DkImageView s_depthView;             // the view bound as the depth target

// T29b (slice 3b): the FullTarget pool. A fixed, statically-allocated array of
// slots; each slot owns one RGBA8 colour image (its own memblock) plus, when
// with_depth, one Z24S8 depth image (its own SECOND memblock), the two
// DkImageViews that bind_views() consumes, and the cached in-slot dims.
// 1-based slot handle == array index + 1 (0 == free). 32 slots: far more than
// the 13 GL FBO owners enumerated in T29a's gl-fbo-inventory.md (largest single
// owner = GlowRenderer's 5-level downsample chain), giving ample headroom for
// every P3 frame-buffer owner to be re-created simultaneously.
struct FullTargetSlot {
  bool         in_use     = false;
  bool         with_depth = false;
  uint32_t     width      = 0;
  uint32_t     height     = 0;
  DkMemBlock   colourMem  = nullptr;  // dedicated colour image memblock
  DkImage      colourImg  = {};
  DkImageView  colourView = {};
  DkMemBlock   depthMem   = nullptr;  // dedicated depth image memblock (when with_depth)
  DkImage      depthImg   = {};
  DkImageView  depthView  = {};
};
static constexpr int kFullTargetPoolSize = 32;
static FullTargetSlot g_fulltargetPool[kFullTargetPoolSize];

// Forward declaration so init() can call teardown() on a depth-block failure
// (its definition follows below) — a later definition is not a visible
// declaration in C++, so without this it would not compile.
void teardown();

// T29b (slice 3b): the one (colour view, depth-or-null view) -> cmd bind, shared
// by the file-static bind() and FullTarget::bind() so the T28 depth view is
// never duplicated. Defined just below the namespace-static state.
void bind_views(const DkImageView* colour, const DkImageView* depth);

// The single dkCmdBufBindRenderTarget call both bind() paths funnel through:
// colour view (required) + depth view-or-nullptr (nullptr mirrors the GL Fbo
// with no zbuf_stencil, and the T27 colour-only path).
void bind_views(const DkImageView* colour, const DkImageView* depth) {
  dkCmdBufBindRenderTarget(dkdev::cmd_handle(), colour, depth);
}

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

  // (c.5) T28 (slice 2): when with_depth, build a Z24S8 depth image the SAME way
  // (layout maker -> size/alignment -> own 0x1000-rounded GpuCached|Image
  // memblock -> image at offset 0) but in a SECOND, separate block. The colour
  // block above is left exactly as T27 created it. Z24S8 is the standard format
  // matching the GL zbuf_stencil (depth24 + stencil8), so we do NOT use a 2D-engine
  // usage flag here (depth is not a blit source/destination).
  if (d.with_depth) {
    DkImageLayoutMaker dmk;
    DkImageLayout     dlayout;
    dkImageLayoutMakerDefaults(&dmk, dkdev::device_handle());
    dmk.type         = DkImageType_2D;
    dmk.dimensions[0] = d.width;
    dmk.dimensions[1] = d.height;
    dmk.dimensions[2] = 1;
    dmk.mipLevels     = 1;
    dmk.format        = DkImageFormat_Z24S8;
    dmk.flags         = DkImageFlags_UsageRender;  // depth render target only
    dkImageLayoutInitialize(&dlayout, &dmk);

    const uint32_t dblk =
        (uint32_t)((dkImageLayoutGetSize(&dlayout) + 0xFFF) & ~0xFFFu);  // >= 0x1000
    DkMemBlockMaker dmm;
    dkMemBlockMakerDefaults(&dmm, dkdev::device_handle(), dblk);
    dmm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;  // image, not CPU data
    s_depthMem = dkMemBlockCreate(&dmm);
    if (s_depthMem == nullptr) {
      // One failure line (depth), with dimensions and block size in MB.
      const uint32_t dmb = dblk / (1024 * 1024);
      switch_run_logf("[dk] rt depth init failed (%u x %u, %u MB)", d.width, d.height,
                      (unsigned)dmb);
      // Depth failed -> tear down the colour half too so nothing is half-live.
      teardown();
      return false;
    }
    // The Z24S8 DkImage sits at offset 0 of its OWN (second) block.
    dkImageInitialize(&s_depthImg, &dlayout, s_depthMem, 0);
    dkImageViewDefaults(&s_depthView, &s_depthImg);
  }

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
  // Bind this colour view as the render target. The depth target is the Z24S8
  // view when the depth image was created (with_depth), else nullptr (today's
  // T27 behaviour). Goes through bind_views() (T29b) so the T28 depth-view
  // logic and the dkCmdBufBindRenderTarget call are shared with
  // FullTarget::bind() and never duplicated.
  bind_views(&s_view, s_depthMem != nullptr ? &s_depthView : nullptr);
}

void teardown() {
  if (s_mem != nullptr) {
    dkMemBlockDestroy(s_mem);
  }
  s_mem = nullptr;
  s_img = {};
  s_view = {};
  // T28 (slice 2): destroy the depth memblock too (if it was ever created).
  if (s_depthMem != nullptr) {
    dkMemBlockDestroy(s_depthMem);
  }
  s_depthMem  = nullptr;
  s_depthImg  = {};
  s_depthView = {};
  s_width = 0;
  s_height = 0;
}

bool valid()        { return s_mem != nullptr; }
uint32_t width()     { return s_width; }
uint32_t height()    { return s_height; }

// ===== T29b (P3.1 slice 3b): FullTarget == one colour(+optional depth) RT unit, backed by the
// g_fulltargetPool above. =====

// Claim one free pool slot. Returns the 1-based slot handle, or 0 if the pool is
// exhausted. The slot's with_depth / dims are set by the caller (FullTarget::init)
// just after claiming, so this helper only allocates the slot and zeroes it.
static int fulltarget_alloc_slot() {
  for (int i = 0; i < kFullTargetPoolSize; ++i) {
    if (!g_fulltargetPool[i].in_use) {
      g_fulltargetPool[i] = FullTargetSlot{};  // reset to the struct's in-class defaults
      g_fulltargetPool[i].in_use = true;
      return i + 1;  // 1-based handle == array index + 1
    }
  }
  return 0;  // pool exhausted
}

// Release a 1-based slot: destroy whatever memblocks it still owns, then mark it free.
// Safe to call on a slot that was never live (its memblocks are already null).
static void fulltarget_free_slot(int slot) {
  if (slot <= 0 || slot > kFullTargetPoolSize) {
    return;  // out-of-range / free handle: nothing to do
  }
  FullTargetSlot& s = g_fulltargetPool[slot - 1];
  if (s.colourMem != nullptr) {
    dkMemBlockDestroy(s.colourMem);
  }
  s.colourMem  = nullptr;
  s.colourImg  = {};
  s.colourView = {};
  if (s.depthMem != nullptr) {
    dkMemBlockDestroy(s.depthMem);
  }
  s.depthMem  = nullptr;
  s.depthImg  = {};
  s.depthView = {};
  s.in_use    = false;
  s.with_depth = false;
  s.width     = 0;
  s.height    = 0;
}

bool FullTarget::init(const FullTargetDesc& d) {
  // Claim a free pool slot up front; on any failure below we free it again so a
  // failed init never leaves a half-live slot in the pool.
  const int slot = fulltarget_alloc_slot();
  if (slot == 0) {
    switch_run_logf("[dk] fulltarget init failed (no free slot, %u x %u)", d.width, d.height);
    return false;
  }
  FullTargetSlot& s = g_fulltargetPool[slot - 1];
  s.with_depth = d.with_depth;
  s.width      = d.width;
  s.height     = d.height;

  // (a) build the colour image layout: a 1-mip 2D RGBA8 target, usable as a render
  // target AND as a 2D-engine blit source/destination (identical to the free init()).
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

  // (b) round the layout size up to a DK_MEMBLOCK_ALIGNMENT (0x1000), then give the
  // colour image its OWN block (GpuCached | Image, never CPU data).
  const uint32_t blk =
      (uint32_t)((dkImageLayoutGetSize(&layout) + 0xFFF) & ~0xFFFu);  // >= 0x1000
  DkMemBlockMaker mm;
  dkMemBlockMakerDefaults(&mm, dkdev::device_handle(), blk);
  mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;  // image, not CPU data
  s.colourMem = dkMemBlockCreate(&mm);
  if (s.colourMem == nullptr) {
    // One failure line (color), with dimensions and block size in MB.
    switch_run_logf("[dk] fulltarget init failed (%u x %u, %u MB)", d.width, d.height,
                    (unsigned)(blk / (1024 * 1024)));
    fulltarget_free_slot(slot);
    return false;
  }

  // (c) the colour DkImage sits at offset 0 of its OWN block; the view mirrors it.
  dkImageInitialize(&s.colourImg, &layout, s.colourMem, 0);
  dkImageViewDefaults(&s.colourView, &s.colourImg);

  // (c.5) when with_depth, build a Z24S8 depth image the SAME way but in a SECOND,
  // separate block. The colour block above is left exactly as (b) created it. Z24S8
  // is the standard format matching the GL zbuf_stencil (depth24 + stencil8); depth is
  // not a blit source/destination, so no 2D-engine usage flag here (identical to T28).
  if (d.with_depth) {
    DkImageLayoutMaker dmk;
    DkImageLayout     dlayout;
    dkImageLayoutMakerDefaults(&dmk, dkdev::device_handle());
    dmk.type         = DkImageType_2D;
    dmk.dimensions[0] = d.width;
    dmk.dimensions[1] = d.height;
    dmk.dimensions[2] = 1;
    dmk.mipLevels     = 1;
    dmk.format        = DkImageFormat_Z24S8;
    dmk.flags         = DkImageFlags_UsageRender;  // depth render target only
    dkImageLayoutInitialize(&dlayout, &dmk);

    const uint32_t dblk =
        (uint32_t)((dkImageLayoutGetSize(&dlayout) + 0xFFF) & ~0xFFFu);  // >= 0x1000
    DkMemBlockMaker dmm;
    dkMemBlockMakerDefaults(&dmm, dkdev::device_handle(), dblk);
    dmm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;  // image, not CPU data
    s.depthMem = dkMemBlockCreate(&dmm);
    if (s.depthMem == nullptr) {
      // One failure line (depth). On depth failure tear down the colour half too so
      // nothing is left half-live (mirrors the free init()'s depth-failure path).
      const uint32_t dmb = dblk / (1024 * 1024);
      switch_run_logf("[dk] fulltarget depth init failed (%u x %u, %u MB)", d.width, d.height,
                      (unsigned)dmb);
      fulltarget_free_slot(slot);
      return false;
    }
    // The Z24S8 DkImage sits at offset 0 of its OWN (second) block.
    dkImageInitialize(&s.depthImg, &dlayout, s.depthMem, 0);
    dkImageViewDefaults(&s.depthView, &s.depthImg);
  }

  // (d) cache the dims in the FullTarget and log one success line.
  m_slot   = slot;
  m_width  = d.width;
  m_height = d.height;
  // block size in MB with one decimal (blk is 0x1000-aligned; /1024 = KB, the
  // fractional digit comes from (bytes % 1 MB) * 10 / 1 MB expressed as a 0.1 MB unit).
  const uint32_t mb10 = (blk / (1024 * 1024)) * 10 +
                         ((blk % (1024 * 1024)) * 10 / (1024 * 1024));
  switch_run_logf("[dk] fulltarget %ux%u %u.%u MB", d.width, d.height,
                  (unsigned)(mb10 / 10), (unsigned)(mb10 % 10));
  return true;
}

bool FullTarget::matches(uint32_t w, uint32_t h) const {
  // Mirror the GL Fbo::matches (Fbo.h:23-30) WITHOUT the msaa term -- MSAA is T30,
  // so a plain (non-MSAA) target matches on size only, and only when valid.
  return valid() && m_width == w && m_height == h;
}

void FullTarget::bind() {
  // Bind this FullTarget's colour view as the render target, with the Z24S8 depth
  // view when with_depth (else nullptr = colour-only, like the T27 path). Routes
  // through the shared bind_views() (T29b) so the dkCmdBufBindRenderTarget call and
  // the T28 depth-view logic are never duplicated -- identical to the free bind().
  if (m_slot == 0) {
    return;  // never created / already torn down: a no-op (mirrors a no-op bind)
  }
  FullTargetSlot& s = g_fulltargetPool[m_slot - 1];
  bind_views(&s.colourView, s.with_depth ? &s.depthView : nullptr);
}

void FullTarget::teardown() {
  // Free this FullTarget's memblock(s) and pool slot. Safe to call when init()
  // never ran or failed (m_slot == 0 == not live): a no-op then.
  if (m_slot == 0) {
    return;
  }
  const int slot = m_slot;
  m_slot   = 0;  // drop our handle first so a re-init can't see a stale slot
  m_width  = 0;
  m_height = 0;
  fulltarget_free_slot(slot);
}

bool     FullTarget::valid()  const { return m_slot != 0; }
uint32_t FullTarget::width()  const { return m_width; }
uint32_t FullTarget::height() const { return m_height; }

}  // namespace dkrt

#endif  // __SWITCH__
