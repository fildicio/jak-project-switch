# T39a — LIVE wiring 1/2: chain capture + kill-switch hook (P3.2, AI-assisted)

Track D P3.2 · Depends on: T38b ✅ + **P2.3 soak PASS (hard gate — goes LIVE)** ·
One session (~40 min) · First behavior change since T24: **expect a NEW NRO md5.**
Split from T39 (halving pass, 2026-10-18) — this half does NOT touch LevelData.

## Goal

Keep the received DMA chain (pointer only, no copy), register the world
recorder, and hook it into `present_frame` — all behind a compile-time
kill-switch `kTfrag3Draw = false`, so runtime behavior stays byte-identical.

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Re-reading = looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T39a`.

## Read (once, exactly these)

1. `game/graphics/pipelines/dk_renderer.cpp` L145-190 (render's wait+ack block)
   and L345-370 (`dk_send_chain`).
2. `game/graphics/pipelines/dk_device.cpp` L555-630 (`present_frame` + the
   `kSplash3D` constant at ~L78).
3. `grep -n -i tfrag game/graphics/opengl_renderer/buckets.h` (bucket id only).

## Facts (verified — do not re-derive)

- The chain data is owned by GOAL and valid ONLY until acknowledged:
  capture → consume → ack, in that order, inside the existing wait-success
  block. Never copy, never free.
- Recording order in present_frame: clear (3D) → `fn()` (3D draws) → existing
  barrier → blit (2D). The barrier already exists (~L611); world draws go
  BEFORE it, after the clear.
- No new fences: world recording happens after the slice-fence wait at the
  top of present_frame — safe by construction.

## Steps

1. `dk_device.h/.cpp`: plain registration API
   `void set_world_recorder(void (*fn)(void));` + a file-static fn pointer;
   `present_frame` calls `fn()` after the clear ONLY when
   `constexpr bool kTfrag3Draw = false;` (beside kSplash3D) AND fn is set.
   dk_device.h stays deko3d-free.
2. `dk_renderer.cpp`: `DkSyncData` gains `{const void* data; u32 offset;}`
   (zeroed); `dk_send_chain` stores them; in `render()`'s got-chain branch,
   AFTER wait-success and BEFORE `has_data_to_render = false`: if the world
   recorder is registered and device is up, run the bucket walk + entry —
   guarded, every null → skip frame, ONE `[dk]` line per failure type per
   report window (never per frame) — THEN ack.
3. The walk: `DmaFollower` to the tfrag bucket id from read 3; if the skip
   loop needs machinery beyond DmaFollower, copy the exact loop from GL
   dispatch and CITE the lines you copied.
4. With `kTfrag3Draw = false` the code must be dead: byte-identical runtime
   (splash-only, chain acked unread).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + **new md5 expected** (live call graph changed). Paste the md5 and
state that the kill-switch is OFF. Reviewer verifies the diff is wiring only.

## Doubts

- If `present_frame`'s structure resists the hook (no clean point after the
   clear), STOP with the proposed point — do not improvise ordering.
