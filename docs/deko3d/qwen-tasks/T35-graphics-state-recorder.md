> **SPLIT 2026-10-18** — halved for the coder model. Superseded by
> `T35a-record-state.md` + `T35b-find-for-draw-mode.md`; status lives on those
> rows. Kept for history — do NOT run this file.

# T35 — dkpipe graphics-state recorder (bind a PipelineTable entry, P3.2, AI-assisted)

Track D P3.2 · Depends on: T26 ✅ (blend const) · Inert until T38 · One session

## Goal

`dkpipe::record_graphics_state(DkCmdBuf cb, const PipelineDesc& p)` — record the
full fixed-function state for one pipeline-table entry on a command buffer:
the T16 `DkStates` trio + T26's blend-const + the safe defaults (rasterizer /
color write / multisample / viewport-swizzle none). This is the deko3d
replacement for `setup_opengl_from_draw_mode` (background_common.h:46) and the
function every P3.3+ renderer calls instead of per-draw GL state.

## Context

- `game/graphics/deko3d_renderer/dk_state.h` — `DkStates` + `to_deko3d(const
  PipelineDesc&, DkStates*)` (T16, reviewed). Your function CALLS it.
- `game/graphics/deko3d_renderer/dk_state.cpp` + `dk_state_internal.h` — T26's
  `apply_blend_const` lives in the internal header; reuse, do not duplicate.
- `game/graphics/pipelines/dk_device.cpp:198-215` — the splash's full state-bind
  block: the exact `dkXxxStateDefaults` + `dkCmdBufBind*State` sequence to
  reproduce (minus shaders/vertex, which stay in T38).
- `docs/deko3d/pipeline_table.md` — the table's meaning; read §"Blend states".
- `PipelineTable.inc` — `grep -n 'PipelineDesc' game/graphics/deko3d_renderer/PipelineTable.inc | head`
  to see the entry shape and pick NOTHING yet: T38 chooses the first entry to use.

## API cheat-sheet (verified, deko3d.h line numbers)

- `dkCmdBufBindRasterizerState` 1268 / `BindColorState` 1270 /
  `BindColorWriteState` 1271 / `BindBlendStates` 1272 (firstId 0, 1 state) /
  `BindDepthStencilState` 1273 / `BindMultisampleState` 1269.
- Defaults helpers: `dkRasterizerStateDefaults`, `dkColorStateDefaults`,
  `dkColorWriteStateDefaults`, `dkMultisampleStateDefaults`,
  `dkDepthStencilStateDefaults`, `dkBlendStateDefaults` (grep each — all exist as
  static inline in the same header region as the splash usage).
- Rasterizer defaults to cull NONE (tfrag strips are wound both ways — matches GL,
  which never culls on this path).
- Blend const: `dkpipe::apply_blend_const` (dk_state_internal.h, T26) — call after
  `BindBlendStates`.

## Steps

1. Declare in dk_state.h (Switch-only TU is fine, it already includes deko3d.h):
   ```cpp
   // Records the complete fixed-function state for one table entry.
   // Cull mode is always None (GL parity on the tfrag/tie paths).
   void record_graphics_state(DkCmdBuf cb, const PipelineDesc& p);
   ```
2. Implement in dk_state.cpp: fill `DkStates` via `to_deko3d`, bind depth-stencil,
   color, blend (1 state), blend-const via T26, then the four Defaults binds
   (rasterizer with cull None, color-write, multisample 1x). Order mirrors the
   splash block so the two paths read the same.
3. Comment each bind with its GL counterpart from pipeline_table.md.
4. Add `find_for_draw_mode()` to the same TU:
   ```cpp
   // Maps a PS2 DrawMode (+ mipmap flag) to its table entry. Grounded in how
   // the table was GENERATED: read scripts/gen-dk-pipeline-table.py + the id
   // list in PipelineTable.inc, then translate the axes the script used
   // (alpha blend, depth test/write, decal…). Returns nullptr when no entry
   // matches (caller skips the draw — one [dk] line per report window).
   const PipelineDesc* find_for_draw_mode(const DrawMode& mode, bool mipmap);
   ```
   `DrawMode` is pure (`common/dma/dma_chain_read.h` — grep it). Also return
   the sampler index for the (wrap, linear/mipmap) combo via an out-param
   `uint8_t* sampler_idx` (the kSamplers index whose SamplerDesc matches —
   enumerate `kSamplers[]` and compare fields; no guessing of indices).

## Rules

- Only `dk_state.h` / `dk_state.cpp` (+ read dk_state_internal.h).
- No GL includes, no behavior beyond recording; cb null-check → one `[dk]` line,
  return (never crash — a null cb would be a coding error upstream, log it).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`08fc9510…`) — uncalled → gc-stripped. Changed →
explain.

## Doubts

- If the table has entries needing per-target blend states >1 (it does not today
  — 1 output), note it for T38, do not generalize now.
