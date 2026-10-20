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

#include <cstring>  // T32b: memcpy the readback out of staging
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
  // T30b (slice 4b): the 1x RESOLVE SIBLING, only created when samples > 1. It is a
  // plain 2D RGBA8 image (same w/h/format as the MSAA colour image, DkMsMode_1x) in its
  // own memblock; resolve() copies the MSAA colour image into it and resolved_view()
  // hands its DkImageView out (as an opaque const void*). nullptr/empty for a 1x target.
  DkMemBlock   resolveMem = nullptr;  // dedicated 1x resolve-sibling memblock (when samples > 1)
  DkImage      resolveImg = {};
  DkImageView  resolveView = {};
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

// T32a (P3.1 slice 6a): forward decl of the readback-resource release helper so the
// file-static dkrt::teardown() above can call it. Its definition lives in the
// "readback (T32)" section near the end of this file. Returns true iff at least one
// resource (queue / cmd buffer / memblock) was actually destroyed (so the caller logs
// exactly one line).
static bool destroy_readback_resources();

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
  // T32a (P3.1 slice 6a): also release the dedicated readback resources (the lazy
  // DkQueue + cmd buffer + staging / command memblocks). destroy_readback_resources()
  // is null-safe (never-created state is a clean no-op) and reports back whether it
  // actually freed anything; only then do we log one "[dk] rt readback teardown" line.
  if (destroy_readback_resources()) {
    switch_run_logf("[dk] rt readback teardown");
  }
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
  // T30b (slice 4b): destroy the 1x resolve sibling's memblock too (only live when
  // samples > 1); nullptr-safe for the 1x baseline. Mirrors the colour/depth frees above.
  if (s.resolveMem != nullptr) {
    dkMemBlockDestroy(s.resolveMem);
  }
  s.resolveMem  = nullptr;
  s.resolveImg  = {};
  s.resolveView = {};
  s.in_use    = false;
  s.with_depth = false;
  s.width     = 0;
  s.height    = 0;
}

bool FullTarget::init(const FullTargetDesc& d) {
  // (0) T30a (slice 4a): validate the sample count up front, before touching any
  // pool slot. deko3d's DkMsMode supports only 1x/2x/4x/8x, so 1/2/4/8 are the
  // only legal sample counts; anything else is rejected with one [dk] line and a
  // false return (no pool slot is taken, so a bad caller can't leak a slot).
  if (d.samples != 1 && d.samples != 2 && d.samples != 4 && d.samples != 8) {
    switch_run_logf("[dk] rt bad sample count %u", d.samples);
    return false;
  }

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

  // (a) build the colour image layout: a 1-mip RGBA8 target, usable as a render
  // target AND as a 2D-engine blit source/destination. T30a (slice 4a): when
  // d.samples > 1 this is a 2DMS (multisampled) image with the sample count mapped
  // to its DkMsMode; the single-sample default (d.samples == 1) is the plain 2D
  // image and leaves the default msMode (DkMsMode_1x, set by the defaults) untouched.
  DkImageLayoutMaker mk;
  DkImageLayout layout;
  dkImageLayoutMakerDefaults(&mk, dkdev::device_handle());
  if (d.samples > 1) {
    mk.type = DkImageType_2DMS;
    switch (d.samples) {
      case 2: mk.msMode = DkMsMode_2x; break;
      case 4: mk.msMode = DkMsMode_4x; break;
      case 8: mk.msMode = DkMsMode_8x; break;
      default: break;  // only 1/2/4/8 reach here (validated up in step 0)
    }
  } else {
    mk.type = DkImageType_2D;
  }
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
  // T30a (slice 4a): the depth image stays a plain 1x 2D target -- multisampled depth
  // is out of scope for T30a (only the colour image becomes 2DMS), so dmk.type is
  // never set to DkImageType_2DMS and its msMode stays the default DkMsMode_1x.
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

  // (c.6) T30b (slice 4b): when samples > 1, create the 1x RESOLVE SIBLING -- a
  // plain 2D RGBA8 image, same w/h/format as the MSAA colour image, in its OWN
  // dedicated memblock (GpuCached | Image, size rounded up to DK_MEMBLOCK_ALIGNMENT,
  // EXACTLY like the colour image's block above). It stays DkImageType_2D and
  // DkMsMode_1x, so resolve() can copy the multisampled result into it and later
  // consumers (T31/T38) can blit from / sample the 1x result. Created AFTER the MSAA
  // colour image and checked like every other create; on failure we unwind
  // everything this init created via fulltarget_free_slot() (colour + depth + the
  // (null) resolve memblock), so nothing is left half-live.
  if (d.samples > 1) {
    DkImageLayoutMaker rmk;
    DkImageLayout     rlayout;
    dkImageLayoutMakerDefaults(&rmk, dkdev::device_handle());
    rmk.type         = DkImageType_2D;     // plain 1x (NOT 2DMS)
    rmk.msMode       = DkMsMode_1x;         // explicit 1x: the resolve target's sample mode
    rmk.dimensions[0] = d.width;
    rmk.dimensions[1] = d.height;
    rmk.dimensions[2] = 1;
    rmk.mipLevels     = 1;
    rmk.format        = DkImageFormat_RGBA8_Unorm;
    rmk.flags         = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;  // render + 2D-engine -> blittable, same as the colour image
    dkImageLayoutInitialize(&rlayout, &rmk);

    const uint32_t rblk =
        (uint32_t)((dkImageLayoutGetSize(&rlayout) + 0xFFF) & ~0xFFFu);  // >= 0x1000
    DkMemBlockMaker rmm;
    dkMemBlockMakerDefaults(&rmm, dkdev::device_handle(), rblk);
    rmm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;  // image, not CPU data
    s.resolveMem = dkMemBlockCreate(&rmm);
    if (s.resolveMem == nullptr) {
      // One failure line (resolve). On failure unwind everything this init created.
      switch_run_logf("[dk] rt resolve image create failed (%u x %u, %u MB)", d.width, d.height,
                      (unsigned)(rblk / (1024 * 1024)));
      fulltarget_free_slot(slot);
      return false;
    }
    // The 1x DkImage sits at offset 0 of its OWN (resolve) block; the view mirrors it.
    dkImageInitialize(&s.resolveImg, &rlayout, s.resolveMem, 0);
    dkImageViewDefaults(&s.resolveView, &s.resolveImg);
  }

  // (d) cache the dims in the FullTarget and log one success line.
  m_slot   = slot;
  m_width  = d.width;
  m_height = d.height;
  m_samples = d.samples;  // T30a (slice 4a): cache the sample count for matches()
  // block size in MB with one decimal (blk is 0x1000-aligned; /1024 = KB, the
  // fractional digit comes from (bytes % 1 MB) * 10 / 1 MB expressed as a 0.1 MB unit).
  const uint32_t mb10 = (blk / (1024 * 1024)) * 10 +
                         ((blk % (1024 * 1024)) * 10 / (1024 * 1024));
  switch_run_logf("[dk] fulltarget %ux%u %u.%u MB", d.width, d.height,
                  (unsigned)(mb10 / 10), (unsigned)(mb10 % 10));
  return true;
}

bool FullTarget::matches(uint32_t w, uint32_t h, uint32_t msaa) const {
  // Mirror the GL Fbo::matches (Fbo.h:23-30), now WITH the msaa term (T30a
  // slice 4a): a target matches only when valid, the dims agree, AND the cached
  // sample count (1/2/4/8) agrees with the requested msaa. A single-sample target
  // (samples == 1) matches msaa == 1; a 2DMS target matches its own sample count.
  return valid() && m_width == w && m_height == h && m_samples == msaa;
}

void FullTarget::bind() {
  // Bind this FullTarget's colour view as the render target, with the Z24S8 depth
  // view when with_depth (else nullptr = colour-only, like the T27 path). Routes
  // through the shared bind_views() (T29b) so the dkCmdBufBindRenderTarget call and
  // the T28 depth-view logic are never duplicated -- identical to the free bind().
  // T30a (slice 4a): when this is a 2DMS colour target (samples > 1) the draws land
  // in the multisampled image; the consumer must RESOLVE it to a 1x image (T30b)
  // before sampling, so this bind alone never makes the MSAA result sampleable.
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

  // T30b (P3.1 slice 4b): also SWEEP the rest of g_fulltargetPool. This FullTarget's
  // own slot was freed above; free every OTHER still-live slot too (colour + depth +
  // resolve-sibling memblocks, via the same fulltarget_free_slot()) so a single teardown
  // call settles ALL pooled targets and no other FullTarget leaves a memblock behind.
  // There is no lock here by design: the pool is touched only from the one render thread,
  // matching every other accessor in this file (fulltarget_alloc_slot / fulltarget_free_slot).
  int swept = 0;
  for (int i = 0; i < kFullTargetPoolSize; ++i) {
    if (g_fulltargetPool[i].in_use) {
      fulltarget_free_slot(i + 1);  // 1-based handle
      ++swept;
    }
  }
  switch_run_logf("[dk] rt teardown swept %d pooled target(s)", (int)swept);
}

bool     FullTarget::valid()  const { return m_slot != 0; }
uint32_t FullTarget::width()  const { return m_width; }
uint32_t FullTarget::height() const { return m_height; }

// T30b (P3.1 slice 4b): the 1x resolve sibling's DkImageView, returned as an opaque
// const void* so the public .h stays deko3d-free (no <deko3d.h>). T31/T38 consumers
// static_cast this back to a DkImageView* to blit from / sample the resolved result.
// Only valid for a live 2DMS target (samples > 1): for a 1x target there is no sibling
// memblock (resolveView is an empty {}), so return nullptr; for a non-live target
// (m_slot == 0) there is nothing to hand out, so return nullptr too.
const void* FullTarget::resolved_view() const {
  if (m_slot == 0) {
    return nullptr;  // never created / already torn down: nothing to resolve into
  }
  FullTargetSlot& s = g_fulltargetPool[m_slot - 1];
  // A 1x target (samples == 1) never created a resolve sibling, so its resolveMem is
  // null; only a 2DMS target (samples > 1) owns a live resolve image + view.
  if (s.resolveMem == nullptr) {
    return nullptr;
  }
  return static_cast<const void*>(&s.resolveView);
}

// T30b (P3.1 slice 4b): record, on the current command buffer, a full barrier followed
// by dkCmdBufResolveImage that copies this FullTarget's MSAA colour image (the source)
// into its 1x resolve sibling (the destination). This makes the multisampled result
// sampleable/blittable: after this, consumers read the 1x sibling via resolved_view().
// Mirrors the existing barrier+copy precedent in dk_device.cpp (dkCmdBufBarrier(...,
// DkBarrier_Full, 0)). Only valid for a live 2DMS target (samples > 1) that init()
// created successfully; a 1x target has no sibling and a non-live target has no slot, so
// both log exactly one "[dk]" line and return false (never crash).
bool FullTarget::resolve() {
  if (m_slot == 0) {
    switch_run_logf("[dk] rt resolve: target not live (no slot to resolve)");
    return false;
  }
  FullTargetSlot& s = g_fulltargetPool[m_slot - 1];
  if (s.resolveMem == nullptr) {
    // A 1x target (samples == 1) never created a resolve sibling, so there is nothing
    // multissampled to resolve into. One failure line, then a clean false (no crash).
    switch_run_logf("[dk] rt resolve: 1x target has no resolve sibling (%u x %u)", m_width, m_height);
    return false;
  }
  // A full barrier completes all prior GPU work on the MSAA colour image before it is
  // read as the resolve source (same DkBarrier_Full, invalidateFlags 0 as dk_device.cpp).
  dkCmdBufBarrier(dkdev::cmd_handle(), DkBarrier_Full, 0);
  // Resolve the MSAA colour image (srcView) into the 1x sibling image (dstView).
  dkCmdBufResolveImage(dkdev::cmd_handle(), &s.colourView, &s.resolveView);
  return true;
}

// T31a (P3.1 slice 5a): dkrt::blit() -- one plain-signature colour blit.
// Half 1 of T31. src/dst are OPAQUE const void* (in the .h); here we cast them
// to DkImageView const* (the same type resolved_view()/dk_device.cpp hand out).
// Mirrors the barrier-then-blit order of the splash precedent in
// dk_device.cpp (L606-613): a full barrier FIRST (3D engine -> 2D engine mixing),
// then dkCmdBufBlitImage. Inert: no caller yet (T31b grounds the inventory,
// T38/T39 wire it). Never crash: a null src/dst view logs exactly one line.
bool blit(const void* src, const BlitRect& src_rect, const void* dst,
           const BlitRect& dst_rect, BlitFilter filter, bool flip_x,
           bool flip_y, bool with_barrier) {
  // Both ends must be a real DkImageView. A null pointer is the only bad input;
  // one "[dk] rt blit null view" line, then a clean false (no crash, no throw).
  if (src == nullptr || dst == nullptr) {
    switch_run_logf("[dk] rt blit null view");
    return false;
  }

  const DkImageView* srcView = static_cast<const DkImageView*>(src);
  const DkImageView* dstView = static_cast<const DkImageView*>(dst);

  // DkImageRect (deko3d.h L1189-1197): one 2D slice -> z = 0, depth = 1
  // (matches the splash precedent in dk_device.cpp L608-609).
  DkImageRect sRect = {src_rect.x, src_rect.y, 0, src_rect.w, src_rect.h, 1};
  DkImageRect dRect = {dst_rect.x, dst_rect.y, 0, dst_rect.w, dst_rect.h, 1};

  // Compose the blit flags (deko3d.h L1168-1186):
  //   FilterLinear = 1U<<4, FilterNearest = 0 (no bit); FlipX = 1U<<0; FlipY = 1U<<1.
  // deko3d blits are always colour -- there is no mask parameter -- so nothing
  // else goes into the flags.
  uint32_t flags = 0;
  if (filter == BlitFilter::Linear) {
    flags |= (1U << 4);  // DkBlitFlag_FilterLinear
  }
  if (flip_x) {
    flags |= (1U << 0);  // DkBlitFlag_FlipX
  }
  if (flip_y) {
    flags |= (1U << 1);  // DkBlitFlag_FlipY
  }

  DkCmdBuf cmd = dkdev::cmd_handle();
  // 3D -> 2D mix: a full barrier FIRST, then the blit (same order as dk_device.cpp).
  if (with_barrier) {
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
  }
  // factor = 0 (last argument of dkCmdBufBlitImage; unused by deko3d blits).
  dkCmdBufBlitImage(cmd, srcView, &sRect, dstView, &dRect, flags, 0);
  return true;
}

// =====================================================================
// ---- readback (T32) ----
// T32a (P3.1 slice 6a, AI-assisted): the RESOURCES only for a later CPU
// readback of a colour render target. Half 1 of T32; the actual
// readback_rgba8() is T32b. We own, lazily, a dedicated DkQueue + its
// DkCmdBuf + a CPU-visible staging memblock + a command-memory memblock,
// and we destroy them in dkrt::teardown().
//
// Why a *dedicated* queue (and not dkdev::queue_handle()): the readback copy
// runs on its own command buffer submitted to its own queue, so it never
// contends with the per-frame present queue's command slices. There is NO
// std::thread anywhere here -- the dedicated queue IS the answer (the task's
// "NO std::thread" rule).
//
// Inert: NOTHING calls ensure_readback_resources() yet (T32b is the first
// consumer). So this is compiled but never executed on the Switch at boot,
// and the gc keeps the NRO byte-identical (done-check: md5 unchanged).
//
// Memblock design (task spec): TWO separate memblocks, both CpuUncached|
// GpuCached (CPU-visible so a CPU read can pull bytes back; GPU-cached so
// the GPU can write the readback copy into staging -- the same flags the
// upload staging area in dk_device.cpp uses):
//   1) the STAGING memblock: s_rbStageMem -- w*h*4 bytes rounded up to
//      DK_MEMBLOCK_ALIGNMENT (0x1000). This is where the GPU writes the
//      copied RGBA8 image before the CPU reads it back (T32b).
//   2) the COMMAND memblock: s_rbCmdMem -- a 64 KB (0x10000) region that
//      dkCmdBufAddMemory() gives to s_rbCmdBuf. Kept as its own block so
//      the readback command memory is fully independent of the present queue's.
//
// Every create is checked (dkQueueCreate / dkCmdBufCreate / dkMemBlockCreate
// all return opaque handles, compared to nullptr exactly like dk_device.cpp);
// on ANY failure we log one "[dk] rt readback resources failed (<what>)"
// line, FULLY UNWIND whatever was already created, and return false (never
// crash, never leave a half-live resource).
//
// Re-size / re-create: if a resource is already live but the new (w,h) needs
// more bytes than it holds, we destroy-and-recreate (same ordering as
// destroy_readback_resources below) before building the new ones.

// The 64 KB command-memory region handed to the readback cmd buffer.
static constexpr uint32_t kRbCmdMemSize = 0x10000;  // 64 KB

// File-static, zero-initialised handles. All start null/empty so a teardown
// before any ensure_readback_resources() call is a clean no-op.
static DkQueue     s_rbQueue     = nullptr;  // the dedicated readback queue
static DkCmdBuf    s_rbCmdBuf    = nullptr;  // its command buffer
static DkMemBlock  s_rbStageMem  = nullptr;  // CPU-visible staging for the readback
static DkMemBlock  s_rbCmdMem    = nullptr;  // 64 KB command memory for s_rbCmdBuf
static uint32_t    s_rbStageSize = 0;        // bytes currently held by s_rbStageMem
// T32b: the fence the dedicated readback queue signals and we wait on. A
// file-static DkFence is zero-initialised, the same shape as s_uploadFence /
// s_sliceFence in dk_device.cpp (no dkFenceCreate exists [dk-lint: ok]; submit/upload signal
// and wait on their default-constructed fence directly).
static DkFence     s_rbFence;

// Internal-only helper: destroy and null out every readback resource, in the
// reverse-create order (queue -> cmd buffer -> memblocks). Each destroy is
// null-safe (a never-created handle is simply skipped). Returns true iff at
// least one resource was actually destroyed, so dkrt::teardown() can log its
// one "[dk] rt readback teardown" line only when something real happened.
static bool destroy_readback_resources() {
  bool destroyed_any = false;
  // (1) the queue first: it owns in-flight submissions for the readback cmd
  // buffer, so it must go away before the cmd buffer it submitted is freed.
  if (s_rbQueue != nullptr) {
    dkQueueDestroy(s_rbQueue);
    s_rbQueue = nullptr;
    destroyed_any = true;
  }
  // (2) the cmd buffer second: it references s_rbCmdMem, so free it before
  // that memblock.
  if (s_rbCmdBuf != nullptr) {
    dkCmdBufDestroy(s_rbCmdBuf);
    s_rbCmdBuf = nullptr;
    destroyed_any = true;
  }
  // (3a) the staging memblock.
  if (s_rbStageMem != nullptr) {
    dkMemBlockDestroy(s_rbStageMem);
    s_rbStageMem  = nullptr;
    s_rbStageSize = 0;
    destroyed_any = true;
  }
  // (3b) the command-memory memblock.
  if (s_rbCmdMem != nullptr) {
    dkMemBlockDestroy(s_rbCmdMem);
    s_rbCmdMem = nullptr;
    destroyed_any = true;
  }
  return destroyed_any;
}

// Lazily create (or re-create on a size change) the readback resources for an
// image of `w` x `h` pixels. Idempotent + fast: if the resources already exist
// and are big enough for (w,h), return true immediately with no GPU work. On
// ANY create failure, log one line and unwind fully (see the helper above).
//
// Returns true on success (resources live). Returns false on a create failure
// (after full unwind). w/h are in pixels; the stage size is w*h*4 bytes.
static bool ensure_readback_resources(uint32_t w, uint32_t h) {
  // 0x1000 (DK_MEMBLOCK_ALIGNMENT) rounding via the house idiom
  // (size + 0xFFF) & ~0xFFF, the same rounding used everywhere in this file
  // and in dk_device.cpp (e.g. the splash / pool blocks).
  const uint32_t stage_want = (w * h * 4u);
  const uint32_t stage_size = (stage_want + 0xFFFu) & ~0xFFFu;  // >= 0x1000

  // Fast path: already live and big enough -> nothing to do.
  if (s_rbStageMem != nullptr && s_rbStageSize >= stage_size &&
      s_rbQueue != nullptr && s_rbCmdBuf != nullptr) {
    return true;
  }

  // (Re-)create path: if ANY resource is already live, tear them ALL down first
  // so we never leak a previous-generation block when enlarging the staging area.
  if (s_rbStageMem != nullptr || s_rbStageSize != 0 || s_rbCmdMem != nullptr ||
      s_rbQueue != nullptr || s_rbCmdBuf != nullptr) {
    destroy_readback_resources();
  }
  s_rbStageSize = 0;

  // 1) STAGING memblock: CpuUncached | GpuCached so the CPU can read it back and
  // the GPU can write the readback copy into it (same flags as dk_device.cpp's
  // upload staging).
  {
    DkMemBlockMaker sm;
    dkMemBlockMakerDefaults(&sm, dkdev::device_handle(), stage_size);
    sm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    s_rbStageMem = dkMemBlockCreate(&sm);
    if (s_rbStageMem == nullptr) {
      switch_run_logf("[dk] rt readback resources failed (staging memblock %u B)",
                      (unsigned)stage_size);
      destroy_readback_resources();  // full unwind (nothing else live yet -> no-op)
      return false;
    }
  }
  // 2) COMMAND memblock: 64 KB (0x10000), CpuUncached | GpuCached.
  {
    DkMemBlockMaker cm;
    dkMemBlockMakerDefaults(&cm, dkdev::device_handle(), kRbCmdMemSize);
    cm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    s_rbCmdMem = dkMemBlockCreate(&cm);
    if (s_rbCmdMem == nullptr) {
      switch_run_logf("[dk] rt readback resources failed (cmd memblock %u B)",
                      (unsigned)kRbCmdMemSize);
      destroy_readback_resources();  // wipes staging + cmd (full unwind)
      return false;
    }
  }
  // 3) DEDICATED QUEUE: graphics-only. Same pattern as dk_device.cpp L418-421
  // (DkQueueMaker + dkQueueMakerDefaults + DkQueueFlags_Graphics).
  {
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, dkdev::device_handle());
    qm.flags = DkQueueFlags_Graphics;
    s_rbQueue = dkQueueCreate(&qm);
    if (s_rbQueue == nullptr) {
      switch_run_logf("[dk] rt readback resources failed (queue)");
      destroy_readback_resources();  // wipes both memblocks (full unwind)
      return false;
    }
  }
  // 4) CMD BUFFER for that queue, with the 64 KB command-memory region handed in
  // via dkCmdBufAddMemory (the same call dk_device.cpp uses for its present slice).
  {
    DkCmdBufMaker cmb;
    dkCmdBufMakerDefaults(&cmb, dkdev::device_handle());
    s_rbCmdBuf = dkCmdBufCreate(&cmb);
    if (s_rbCmdBuf == nullptr) {
      switch_run_logf("[dk] rt readback resources failed (cmd buffer)");
      destroy_readback_resources();  // wipes queue + both memblocks (full unwind)
      return false;
    }
    dkCmdBufAddMemory(s_rbCmdBuf, s_rbCmdMem, 0, kRbCmdMemSize);
  }

  // All four resources are live; record the staging size for the fast path next time.
  s_rbStageSize = stage_size;
  return true;
}

// T32b (P3.1 slice 6b): the blocking CPU readback. Half 2 of T32; first consumer
// of the T32a readback resources (the dedicated queue / cmd buffer / staging +
// command memblocks). Does NOT recreate anything -- it only drives them.
//
// Mirrors submit_upload_cmds / copy_to_gpu in dk_device.cpp:
//   - a FRESH command slice is re-recorded on the dedicated readback cmd buffer
//     (dkCmdBufClear + dkCmdBufAddMemory over its command memblock, the exact
//     per-chunk re-record in copy_to_gpu L549-550);
//   - the record is a single dkCmdBufCopyImageToBuffer on that slice;
//   - submit -> signal fence (flush=true) -> dkFenceWait(-1) is the SAME order
//     as submit_upload_cmds (L656-658) / copy_to_gpu (L552-554).
// The fence wait makes the staging bytes safe to read on the CPU and makes the
// re-recorded command memory safe to reuse next call (same rule as the per-frame
// cmd slices in present_frame). It is also what makes this call BLOCKING: when
// it returns, the GPU has finished writing staging, so the memcpy below is valid.
//
// Inert: no caller yet (T40/T4x wire it). Never crash: bad args / a missing
// resource set each log exactly one "[dk] rt readback ..." line and return false.
bool readback_rgba8(const void* view, uint32_t w, uint32_t h, uint8_t* out) {
  // Guard 1: the source view (opaque DkImageView*), the destination buffer, and
  // the dimensions must all be valid. One line, then a clean false (no crash).
  if (view == nullptr || out == nullptr || w == 0 || h == 0) {
    switch_run_logf("[dk] rt readback bad args");
    return false;
  }

  // Guard 2: lazily (re)create the dedicated readback resources for this (w,h).
  // ensure_readback_resources (T32a) already logs its own "[dk] rt readback
  // resources failed (...)" line on any create failure, so we just propagate the
  // false without adding a second line.
  if (!ensure_readback_resources(w, h)) {
    return false;
  }

  const DkImageView* srcView = static_cast<const DkImageView*>(view);

  // Fresh command slice on the dedicated readback cmd buffer (the per-chunk
  // re-record of copy_to_gpu L549-550): clear the buffer, then hand it its
  // 64 KB command-memory region. The cmd buffer + its command memblock both
  // already live (ensure_readback_resources), so nothing is created here.
  dkCmdBufClear(s_rbCmdBuf);
  dkCmdBufAddMemory(s_rbCmdBuf, s_rbCmdMem, 0, kRbCmdMemSize);

  // DkImageRect (deko3d.h L1189-1197): the WHOLE image, one 2D slice ->
  // z = 0, depth = 1 (matches the splash precedent in dk_device.cpp L608).
  DkImageRect srcRect = {0, 0, 0, w, h, 1};

  // DkCopyBuf (deko3d.h L1199-1204): addr / rowLength / imageHeight. T51 fix: in
  // deko3d (dk_image.cpp L852/L856) rowLength is a row pitch in BYTES and imageHeight a
  // LAYER STRIDE in bytes, 0 = tightly packed. The old {w, h} gave a 4x-too-small pitch.
  DkGpuAddr stagingGpu = dkMemBlockGetGpuAddr(s_rbStageMem);
  DkCopyBuf dst = {stagingGpu, 0, 0};

  // flags = 0: the FlipX/FlipY/Blit filter bits are "only for BlitImage and
  // CopyDataToImage" (the task's note on the header comment), and
  // dkCmdBufCopyImageToBuffer is not in that list, so it takes no flag bits.
  dkCmdBufCopyImageToBuffer(s_rbCmdBuf, srcView, &srcRect, &dst, 0);

  // Finish the list, then submit -> signal fence (flush=true) -> wait, the exact
  // order of submit_upload_cmds (L656-658) on this file's own dedicated queue
  // (s_rbQueue), not dkdev::queue_handle().
  const DkCmdList list = dkCmdBufFinishList(s_rbCmdBuf);
  dkQueueSubmitCommands(s_rbQueue, list);
  dkQueueSignalFence(s_rbQueue, &s_rbFence, /*flush*/ true);
  dkQueueFlush(s_rbQueue);  // T52: SignalFence's flush = CACHE flush; this sends the work
  dkFenceWait(&s_rbFence, -1);

  // The fence has signalled: staging now holds w*h*4 bytes of RGBA8 written by
  // the GPU. s_rbStageMem is CpuUncached|GpuCached, so the CPU address and the
  // GPU address share the same byte offset (offset 0 here), and a plain memcpy
  // of the whole staging area into the caller's `out` is valid.
  uint8_t* stagingCpu = (uint8_t*)dkMemBlockGetCpuAddr(s_rbStageMem);
  memcpy(out, stagingCpu, (size_t)w * (size_t)h * 4u);
  return true;
}


}  // namespace dkrt


#endif  // __SWITCH__
