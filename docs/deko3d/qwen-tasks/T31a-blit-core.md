# T31a — dkrt blit() core (P3.1 slice 5a, AI-assisted)

Track D P3.1 · Depends on: T29b ✅ · Inert · One session (~40 min)

## Goal

Half 1 of T31. `dkrt::blit()` — one plain-signature wrapper that composes blit
flags and records barrier + `dkCmdBufBlitImage` on the current cmd buf.

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Catch yourself re-reading → you are looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, the plan. Everything is pasted below.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T31a`.

## Read (once, exactly this)

1. `game/graphics/pipelines/dk_device.cpp` L600-615 — the existing splash blit
   (view defaults, `DkImageRect`s, barrier before blit). This is your template.

## API (pasted verbatim from deko3d.h — do not open it)

```c
void dkCmdBufBlitImage(DkCmdBuf obj, DkImageView const* srcView,
    DkImageRect const* srcRect, DkImageView const* dstView,
    DkImageRect const* dstRect, uint32_t flags, uint32_t factor);      // L1316
typedef struct DkImageRect {                                           // L1189-1197
    uint32_t x; uint32_t y; uint32_t z; uint32_t width;
    uint32_t height; uint32_t depth;
} DkImageRect;
enum {                                       // L1168-1186 (verbatim excerpt)
    DkBlitFlag_FlipX     = 1U << 0, // only for BlitImage and CopyDataToImage
    DkBlitFlag_FlipY     = 1U << 1,
    DkBlitFlag_FilterNearest = 0U << 4,
    DkBlitFlag_FilterLinear  = 1U << 4,
};
// barrier: see T30b paste — dkCmdBufBarrier(obj, DkBarrier_Full, 0)  // L1259
```

- cmd buf: `#include "game/graphics/pipelines/dk_device_internal.h"` (.cpp only)
  → `dkdev::cmd_handle()` (L26 there).
- GL map: `GL_LINEAR`→FilterLinear, `GL_NEAREST`→FilterNearest. deko3d blits are
  always colour — no mask parameter exists. `factor` = 0.

## Steps

1. `dk_render_target.h` (stays deko3d-free) — add:
   ```cpp
   namespace dkrt {
   struct BlitRect { uint32_t x = 0, y = 0, w = 0, h = 0; };
   enum class BlitFilter { Nearest, Linear };
   // src/dst are opaque DkImageView* (from FullTarget::resolved_view() etc.).
   // with_barrier=true records dkCmdBufBarrier(Full,0) first (3D->2D mixing).
   bool blit(const void* src, const BlitRect& src_rect, const void* dst,
             const BlitRect& dst_rect, BlitFilter filter, bool flip_x,
             bool flip_y, bool with_barrier = true);
   }
   ```
2. `.cpp`: null-view guard (one `[dk] rt blit null view` line, false). Build
   `DkImageRect` with z=0, depth=1. flags = FilterLinear?1U<<4:0 | flip_x?1U<<0:0
   | flip_y?1U<<1:0. Record barrier (if asked) then blit, same order as the
   splash precedent. factor 0.

## Rules

- Only `dk_render_target.h/.cpp`. One `[dk]` line per bad input, never crash.
  Stays inert.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `ce41507cb3319875fcb0a1ffe051b82d` UNCHANGED. Changed → STOP.

## Doubts

- None expected — the paste is complete. If something still does not compile,
  quote the exact compiler error in the report; do NOT go read deko3d.h.
