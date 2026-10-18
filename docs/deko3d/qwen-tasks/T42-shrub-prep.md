# T42 — Shrub prep: pipeline ids + vertex format + instancing plan (P3.3, AI-assisted)

Track D P3.3 · Depends on: T40 gate **PASS** (hard gate — do not start on FAIL) ·
Inert by construction · One small session

## Goal

Same recipe as T41 for Shrub (`deko3d_renderer/dk_shrub_prep.h`): vertex format,
pipeline ids, shader list. Shrub's twist: the GL path uses instanced rendering
(`glVertexAttribDivisor` — verify in Shrub.cpp) — deko3d's answer is
`DkVtxBufferState::divisor` (verified field, deko3d.h:1111-1115) — so the prep
header must record the divisor layout (which buffer, which attribs, divisor
value) for the port task that follows.

## Context (all read-only)

- `game/graphics/opengl_renderer/background/Shrub.cpp` —
  `grep -n 'glVertexAttribPointer\\|glVertexAttribIPointer\\|glVertexAttribDivisor\\|glDrawElementsInstanced\\|glMultiDrawElementsInstanced' -A7 Shrub.cpp`
  for the full layout + instancing pattern; find the vertex struct it streams
  (grep the struct name in the attrib calls; document size/offsets).
- `grep -n 'ShaderId\\|setup_' Shrub.cpp | head` — shader usage
  (`shrub.vert/frag` exist in opengl_renderer/shaders/, NOT yet in
  deko3d_renderer/shaders/ — list as a needed UAM conversion, like T41 does for
  tie_wind).
- `PipelineTable.inc` + `docs/deko3d/pipeline_table.md` — ids for Shrub's draw
  modes (alpha-blended foliage → expect the StdAlpha/PremulAdd entries; verify
  which, with citations).
- Instancing reference in deko3d-land: `DkVtxBufferState` divisor + instance
  draw is `dkCmdBufDrawIndexed` with instanceCount > 1 (deko3d.h:1308) — note
  in the header; if GL uses MultiDrawIndirect, that is
  `dkCmdBufDrawIndexedIndirect` (1309) — record which sites need which.

## Steps

1. Write `dk_shrub_prep.h` (namespace `dkshrub`): same constants-table shape as
   T41 PLUS `kInstanceLayout` (divisor per buffer + rationale comment).
2. Header comment: file:line citation for every constant.
3. Host syntax check.

## Rules

- ONE new header. Nothing else. No GPU calls.
- If Shrub's GL path has BOTH instanced and non-instanced variants (it does have
  merged/non-merged paths — check `Shrub.h`), document BOTH, flag which one the
  port should do first (cheapest = fewer moving parts).

## Done-check

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_shrub_prep.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

HOST-OK + DONE-PASS + NRO md5 unchanged (`08fc9510…`).

## Doubts

- deko3d divisor semantics: confirm from deko3d-ref/examples (grep `divisor`)
  that divisor is per-BUFFER (DkVtxBufferState) not per-attrib before writing
  the layout — cite the example line.
