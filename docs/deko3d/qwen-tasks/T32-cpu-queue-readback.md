# T32 — dkrt CPU-queue readback (P3.1 slice 6, AI-assisted)

Track D P3.1 · Depends on: T29 ✅ · Inert (no caller yet) · One session (biggest P3.1 slice)

## Goal

`dkrt::readback_rgba8()` — copy an RGBA8 image to CPU memory on a DEDICATED
deko3d queue (the plan's "CPU1 queue" rule, plan §P3.1: readback only there, with
a fence, so the render queue never blocks). This is the equivalent of the single
`glReadPixels` in the inventory, and the base for TextureAnimator (P3.7-3.10) and
`finish_screenshot` (P4).

Design: one extra `DkQueue` + its own small `DkCmdBuf` + a
`CpuUncached|GpuCached` staging memblock, all owned by dkrt, created lazily on
first use and destroyed in teardown. The call is BLOCKING (fence wait) — callers
pay the stall knowingly.

## Context

- `game/graphics/pipelines/dk_device.cpp` — queue/cmdbuf creation pattern
  (`DkQueueMaker`/`dkQueueCreate`, `dkCmdBufAddMemory`), the fence discipline in
  `submit_upload_cmds` (L655-659) is the model for submit+signal+wait.
- `game/graphics/pipelines/dk_device_internal.h` — the T22a pattern for exposing
  device handles to sibling TUs; read it before deciding where the new queue's
  handles live (they stay PRIVATE to dk_render_target.cpp — nothing else needs them).

## API cheat-sheet (verified against docs/deko3d-ref/include/deko3d.h)

- `DkQueueMaker` fields: `device, flags, commandMemorySize, flushThreshold,
  perWarpScratchMemorySize, maxConcurrentComputeJobs`; use
  `dkQueueMakerDefaults` + `DkQueueFlags_None` (grep `DkQueueFlags` for the enum;
  a plain graphics queue is fine — this queue only records a copy, check the
  `dkQueueCreate` result like every create).
- `dkCmdBufCopyImageToBuffer(cb, &srcView, &srcRect, &dst, flags)` — L1319;
  `DkCopyBuf` = `{ DkGpuAddr address; uint32_t rowPitch; uint32_t slicePitch; }`
  (grep `typedef struct DkCopyBuf`). rowPitch = w*4 for RGBA8.
- Flags for the copy: grep `DkCopyFlag` (L~1185+) — pick linear/rows order so the
  CPU gets top-row-first RGBA8 (the format the screenshot path and Mac reference
  PNGs use; if both orders exist, choose the one whose comment says linear/rows
  and note the other).
- Fence: `dkQueueSignalFence(q, &fence, true)` then `dkFenceWait(&fence, -1)`
  (L1240, L1328) — exactly the T18 pattern.
- Sync: the SOURCE image may be written by the render queue — the caller must
  have a completed frame (or call `dkdev::wait_idle()` first); document this in
  the header comment, do not add cross-queue sync yourself.

## Steps

1. New section in `dk_render_target.cpp` (namespace-level `namespace dkrt`):
   lazy `ensure_readback_queue(w,h)` — creates queue+cmdbuf (64 KB cmd memblock
   region `CpuUncached|GpuCached`) + staging memblock `w*h*4` bytes (0x1000-rounded,
   same block type), all checked, one `[dk]` failure line each, full unwind on failure.
2. `bool readback_rgba8(const void* view_src, uint32_t w, uint32_t h, uint8_t* out)`
   — signature in the .h must stay plain: take `const struct RtViewHandle*`
   (opaque) + sizes; the .cpp casts to `DkImageView*`. Records: cmd memory add →
   `dkCmdBufCopyImageToBuffer` → finish list → submit → signal fence (flush=true)
   → `dkFenceWait` → memcpy staging→out → true. Any null/zero-size input: one
   `[dk]` line, return false.
3. `teardown()` destroys queue/cmdbuf/staging; safe when never created.

## Rules

- Only `dk_render_target.h/.cpp`. Plain .h (no deko3d types), .cpp in
  `#if defined(__SWITCH__)`. All creates checked. Never crash.
- No `std::thread` (this is the CPU1-QUEUE solution precisely to avoid one).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`74635895…`) — nothing calls readback yet, so the
whole addition must gc-strip. Changed → not inert, stop.

## Doubts

- If `DkCopyFlag` semantics for row order are ambiguous from the header comments,
  STOP and quote the comment text in your report; do not guess the byte order.
