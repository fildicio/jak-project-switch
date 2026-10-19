# T30b — dkrt resolve sibling + resolve() (P3.1 slice 4b, AI-assisted)

Track D P3.1 · Depends on: T30a ✅ · Inert · One session (~40 min)

## Goal

Half 2 of T30. When `samples > 1`, create the 1x resolve sibling image (own
memblock), expose it via `resolved_view()`, and add `resolve()` which records
`dkCmdBufResolveImage`. Plus the pool-slot sweep in `dkrt::teardown()`.

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Catch yourself re-reading → you are looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**` (NOTE: the old
  T30 sent you to Example06 for a resolve pattern — it does not exist there;
  the paste below is complete), `SWITCH_PORT_SESSION_NOTES.md`, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T30b`.

## Read (once, exactly this)

1. `dk_render_target.cpp`: the T30a-touched `FullTarget::init` + `teardown`
   bodies, and the `g_fulltargetPool` definition (grep it).
2. `game/graphics/pipelines/dk_device.cpp` L606-613 — the existing barrier+blit
   order precedent (one small range).

## API (pasted verbatim from deko3d.h — do not open it)

```c
void dkCmdBufResolveImage(DkCmdBuf obj, DkImageView const* srcView,
                          DkImageView const* dstView);            // L1317
typedef enum DkBarrier {                                          // L333-340
    DkBarrier_None = 0, DkBarrier_Tiles = 1, DkBarrier_Fragments = 2,
    DkBarrier_Primitives = 3,
    DkBarrier_Full = 4, // completes all previous commands, disables prefetch
} DkBarrier;
void dkCmdBufBarrier(DkCmdBuf obj, DkBarrier mode, uint32_t invalidateFlags); // L1259
```

- The cmd buf: `#include "game/graphics/pipelines/dk_device_internal.h"` in the
  **.cpp only** → `dkdev::cmd_handle()` (declared L26 of that header).
- Resolve sibling layout: same w/h/format as the colour image, `DkMsMode_1x`,
  plain `DkImageType_2D`, flags `DkImageUsage_Render` (blittable). Own memblock
  `GpuCached | Image`, 0x1000-rounded size — mirror exactly how T29b creates
  the colour image's memblock.

## Steps

1. `FullTargetDesc`/`FullTarget` state: + resolve image/memblock/view handles
   (plain-hidden in the .h exactly like the existing colour handles are).
2. `init` (samples>1): create the sibling AFTER the MSAA colour image, checked
   like every other create; on failure one `[dk] rt resolve image create failed`
   line + full unwind of everything this init created, return false.
3. `const void* resolved_view() const` — .h stays deko3d-free (same opaque
   pattern as the existing view getter if any; if none exists yet, return
   `const void*` and document that T31/T38 cast it to `DkImageView*`).
4. `bool resolve()` — null/inactive guards (one `[dk]` line, false); records
   `dkCmdBufBarrier(cmd_handle(), DkBarrier_Full, 0)` then
   `dkCmdBufResolveImage(cmd_handle(), &msaa_view, &resolve_view)`. Only valid
   when samples>1 and init succeeded (guard, `[dk] rt resolve on non-msaa`).
5. `teardown()`: destroy the sibling's view/image/memblock; ALSO sweep
   `g_fulltargetPool` — for every live slot call the same per-target teardown,
   then clear the pool (one `[dk] rt teardown swept N pooled targets` line).
   This settles the ownership note left open in T29b's review.

## Rules

- Only `dk_render_target.h/.cpp` (+ the internal-header include). Checked
  creates, one-line `[dk]` failures, never crash. Stays inert (no callers).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `08fc9510745f7c584ab6c8bd73a1b883` UNCHANGED. Changed → STOP.

## Doubts

- If `FullTarget`'s existing handles cannot hide deko3d types the way this file
  assumes (check how T29b did it FIRST), copy T29b's exact hiding pattern and
  say so in the report.
