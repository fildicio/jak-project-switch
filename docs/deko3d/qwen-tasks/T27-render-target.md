# T27 — dk_render_target: color RT create/bind/destroy (P3.1 slice 1)

First slice of P3.1 (deko3d equivalents of the GL FBO usage — plan says 9 FBOs, 7 blits,
1 readPixels, MSAA resolve; THIS task is color-only create/bind/destroy; MSAA resolve,
depth and readback are later slices). **Inert**: nothing calls it yet.

## Verified anchors (vendored header docs/deko3d-ref/include/deko3d.h)
- `DkImageFlags_UsageRender = 1U<<8` L375 (render target); `DkImageFlags_Usage2DEngine`
  L378 (blit src/dst — the 7 blits need it).
- `dkCmdBufBindRenderTarget(cb, colorTarget, depthTarget)` inline L1388 →
  `dkCmdBufBindRenderTargets` L1267 (depth may be nullptr).
- `dkCmdBufBlitImage` — already proven at dk_device.cpp:606 (splash), mirror its usage.
- Layout maker / GetSize / GetAlignment / dkImageInitialize / dkImageViewDefaults —
  same calls as T22b (dk_texture_gpu.cpp); reuse that shape.
- Image memory needs `GpuCached | Image` flags — NOT make_block() (that is
  CpuUncached|GpuCached for CPU-written data). Create your own small
  `DkMemBlockMaker` block with `dkdev::device_handle()` (dk_device_internal.h, T22a).

## Edits
1. NEW `game/graphics/deko3d_renderer/dk_render_target.h` — plain header (cstdint only):
   `namespace dkrt { struct Desc { uint32_t width, height; };`
   `bool init(const Desc& d); void bind(); void teardown(); bool valid();`
   `uint32_t width() const; uint32_t height() const; }` — RGBA8 color target
   (`DkImageFormat_RGBA8_Unorm`, deko3d.h:412), no MSAA in this slice.
2. NEW `game/graphics/deko3d_renderer/dk_render_target.cpp` — `#if defined(__SWITCH__)`,
   includes `<deko3d.h>`, `game/graphics/pipelines/dk_device_internal.h`, its own header.
   File-static: one DkMemBlock, DkImage + DkImageView. Create the block EXACTLY (never
   make_block(), never buffer_pool()):
     `uint32_t blk = (uint32_t)((dkImageLayoutGetSize(&layout) + 0xFFF) & ~0xFFFu);`  // round up to DK_MEMBLOCK_ALIGNMENT 0x1000 (deko3d.h:140)
     `DkMemBlockMaker mm; dkMemBlockMakerDefaults(&mm, dkdev::device_handle(), blk);`   // deko3d.h:193
     `mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;`                    // override the CpuUncached|GpuCached default (deko3d.h:197)
     `s_mem = dkMemBlockCreate(&mm);`  // deko3d.h:1233; nullptr → the `[dk] rt init failed` path
   The single DkImage sits at offset 0 of ITS OWN block —
     `dkImageInitialize(&s_img, &layout, s_mem, 0);`  // never any other offset
   `init()`: layout maker (type 2D, dimensions {w,h,1}, mipLevels 1, format RGBA8,
   flags `DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine`), init image+view.
   Fail → one `[dk] rt init failed (w x h, N MB)` line, false.
   `bind()`: `dkCmdBufBindRenderTarget(dkdev::cmd_handle(), &s_view, nullptr)`.
   `teardown()`: destroy memblock, zero state. Log one `[dk] rt 1280x720 3.5 MB` style
   line on success.
3. `game/CMakeLists.txt`: add dk_render_target.cpp beside dk_state.cpp (:101 pattern).

## Done-check (background wrappers only)
`scripts/ai/check-bg.sh jak1` + `scripts/ai/check-tail.sh` → DONE-PASS.
Inert grep: `dkrt::` only inside dk_render_target.*. QWEN_REPORT `## T27` (files +
last 5 log lines + md5). README row → 🟡. No commit, no deploy.
