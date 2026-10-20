# T42a — Shrub prep 1/2: vertex format + divisor layout (P3.3, AI-assisted)

Track D P3.3 · Depends on: T40 gate **PASS** (hard gate) · Inert · One session (~35 min)
Split from T42 (halving pass, 2026-10-18).

## Goal

Half 1 of T42. Create `deko3d_renderer/dk_shrub_prep.h`: Shrub's vertex-format
constants AND the instancing divisor layout (which buffer, which divisor) —
Shrub is the first instanced renderer in the port.

## Session rules (hard — read twice)

- Time box ~35 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T42a`.

## Facts (pasted verbatim from deko3d.h — do not open it)

```c
typedef struct DkVtxBufferState {      // L1111-1115
    uint32_t stride;
    uint32_t divisor;                  // per-BUFFER, not per-attrib
} DkVtxBufferState;
```

## Commands (exactly these)

1. `grep -n 'glVertexAttribPointer\|glVertexAttribIPointer\|glVertexAttribDivisor\|glDrawElementsInstanced\|glMultiDrawElementsInstanced' -A7 game/graphics/opengl_renderer/background/Shrub.cpp`
2. Find the streamed vertex struct (grep its name in Shrub.h); document
   size/offsets.

## Steps

1. Write `dk_shrub_prep.h` (namespace `dkshrub`): the attrib table (plain ints,
   T41a style) PLUS `kInstanceLayout`: which vertex buffer index carries the
   per-instance attribs, its divisor value, and a rationale comment citing the
   GL calls from command 1.
2. Note in the header: instance draws = `dkCmdBufDrawIndexed` with
   instanceCount > 1; if Shrub uses MultiDrawIndirect anywhere, that maps to
   `dkCmdBufDrawIndexedIndirect` (L1309) — record which sites need which.
3. If Shrub has BOTH merged/non-merged paths (check Shrub.h), document BOTH;
   flag the cheaper one for the port.

## Done-check

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_shrub_prep.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

HOST-OK + DONE-PASS + NRO md5 `ce12b1ecc3975f6a9bbf50d8266987d8` (R1 baseline, re-based 2026-10-20 13:30) UNCHANGED.

## Doubts

- If the divisor value differs per attrib in GL, that cannot map 1:1 to
   per-buffer divisors — STOP and report the exact GL layout.
