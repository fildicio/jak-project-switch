# T38a — TFragmentDk record_frame: DMA parse only (P3.2, AI-assisted)

Track D P3.2 · Depends on: T33b T34b T35b T36b T37b all ✅ + **P2.3 soak PASS** ·
Inert until T39 · One session (~45 min)
Split from T38 (halving pass, 2026-10-18) — this half records NO draws at all.

## Goal

Half 1 of T38. `record_frame(const u8* chain, u32 offset)` in TFragmentDk.cpp:
walk the tfrag DMA bucket, port the VIF/VU parse that fills the pc-port data
and camera state, cache it. NO culling, NO draw recording (T38b).

## Session rules (hard — read twice)

- Time box ~45 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Re-reading = looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T38a`.

## Read (once, exactly these)

1. `grep -n 'handle_initialization\|void TFragment::render' \
   game/graphics/opengl_renderer/background/TFragment.cpp` → read
   `handle_initialization` + the FIRST 80 lines of `render` (the bucket walk
   to the parse). This is the port target.
2. `game/graphics/opengl_renderer/background/background_common.h` L15-30 —
   `TfragPcPortData` (the parse output struct).
3. `common/dma/dma_chain_read.h` — whole file (small; DmaFollower API).

## Facts (verified — do not re-derive)

- The parse is pure DMA reading (no GL) — copy it, do not redesign it.
- `DmaFollower` over the bucket until its end tag is the whole walk mechanism.
- The camera data for the UBOs comes from the parsed pc-port data
  (T37b's `fill_vert` input) — cache it in a member, do not fill UBOs yet.

## Steps

1. Add to `TFragmentDk.h`: `void record_frame(const u8* chain, u32 offset);`
   (+ `void record_frame_entry();` — the plain fn-pointer signature T39a
   registers; it calls record_frame with cached chain state).
2. `.cpp`: port the walk + parse into file-static state (pc-port data + camera
   + the flags GL derives). Null chain / zero offset → one
   `[dk] tfrag render: bad chain` line (once per report window, not per
   frame), return.
3. Lazy-ensure hook: first call checks `dkdev::tfrag3_shaders_loaded()` +
   sampler set accessors exist (T33b/T34b) — if not, one `[dk] tfrag render:
   shaders/samplers missing` line and return (T38b uses them next).
4. Report: which parsed fields you cached + any GL-side state the parse feeds
   that has no dk home yet (list only).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `67018cd5a4411b3f3cc77f198773b261` (a move is NOT a stop: A/B-attribute — stash only your files, rebuild, compare, restore, rebuild — and report both md5s; STOP only if the A/B does not attribute) (no caller).
Changed → explain.

## Doubts

- If the parse references GL renderer member state (m_test_setup etc.),
   cache the equivalent in file-statics and note the mapping — do not edit GL.
