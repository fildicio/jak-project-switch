# T35a — dkpipe record_graphics_state (P3.2, AI-assisted)

Track D P3.2 · Depends on: T26 ✅ · Inert until T38 · One session (~40 min)
Split from T35 (halving pass, 2026-10-18).

## Goal

Half 1 of T35. `dkpipe::record_graphics_state(DkCmdBuf cb, const PipelineDesc& p)`
— record the full fixed-function state for one pipeline-table entry: T16's
`DkStates` + T26's blend-const + safe defaults. `find_for_draw_mode` is T35b.

## Session rules (hard — read twice)

- Time box ~40 min. Over budget → STOP at the last clean compile; report what remains.
- Read each listed range ONCE. Re-reading = looping → STOP, report.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan. Everything is pasted below.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T35a`.

## Read (once, exactly these)

1. `game/graphics/deko3d_renderer/dk_state.h` — whole file (small).
2. `game/graphics/deko3d_renderer/dk_state_internal.h` — whole file (small;
   `apply_blend_const` lives here — REUSE it).
3. `game/graphics/pipelines/dk_device.cpp` L196-216 — the splash full
   state-bind block (your template; same defaults helpers, same order).

## API (pasted verbatim from deko3d.h — do not open it)

```c
void dkCmdBufBindRasterizerState(DkCmdBuf, DkRasterizerState const*);    // L1268
void dkCmdBufBindMultisampleState(DkCmdBuf, DkMultisampleState const*);  // L1269
void dkCmdBufBindColorState(DkCmdBuf, DkColorState const*);              // L1270
void dkCmdBufBindColorWriteState(DkCmdBuf, DkColorWriteState const*);    // L1271
void dkCmdBufBindBlendStates(DkCmdBuf, uint32_t firstId,
                             DkBlendState const*, uint32_t num);         // L1272
void dkCmdBufBindDepthStencilState(DkCmdBuf, DkDepthStencilState const*);// L1273
// Defaults (all static inline in the same header):
// dkRasterizerStateDefaults, dkMultisampleStateDefaults, dkColorStateDefaults,
// dkColorWriteStateDefaults, dkBlendStateDefaults, dkDepthStencilStateDefaults
```

## Steps

1. Declare in `dk_state.h` (it already includes deko3d.h — Switch-only TU):
   ```cpp
   // Records the complete fixed-function state for one table entry.
   // Cull mode is always None (GL parity on the tfrag/tie paths).
   void record_graphics_state(DkCmdBuf cb, const PipelineDesc& p);
   ```
2. Implement in `dk_state.cpp`: null-cb guard (one `[dk] state null cb` line,
   return). Fill `DkStates` via `to_deko3d`; bind depth-stencil, color, blend
   (firstId 0, count 1), then `apply_blend_const` (T26), then the Defaults
   binds (rasterizer cull NONE, color-write, multisample 1x). ORDER mirrors
   the splash block exactly.
3. Comment each bind with its GL counterpart (one short line each; the splash
   block + pipeline_table.md naming is enough — do not open the .md).

## Rules

- Only `dk_state.h` / `dk_state.cpp`. No GL includes. Never crash. Uncalled → inert.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `e819b40d2be605b19099288c9f215ecf` UNCHANGED. Changed → STOP.

## Doubts

- If `DkStates` (T16) does not carry a depth-stencil state, check dk_state.h —
   it should (T16 defined the trio). If truly missing, STOP and report.
