# T41a — Tie3 prep 1/2: vertex format table (P3.3, AI-assisted)

Track D P3.3 · Depends on: T40 gate **PASS** (hard gate) · Inert · One session (~30 min)
Split from T41 (halving pass, 2026-10-18).

## Goal

Half 1 of T41. Create `deko3d_renderer/dk_tie3_prep.h` with the verified
vertex-format constants for Tie3 (attrib offsets/sizes/types as plain ints +
citations). No pipeline ids yet (T41b). No .cpp, no CMake, no GPU calls.

## Session rules (hard — read twice)

- Time box ~30 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T41a`.

## Commands (exactly these)

1. `grep -n 'glVertexAttribPointer\|glVertexAttribIPointer' -A7 \
   game/graphics/opengl_renderer/background/Tie3.cpp`
2. Find the vertex struct the calls stream (grep the struct name in Tie3.h);
   if it is NOT `tfrag3::PreloadedVertex`, document its diff field by field.

## Steps

1. Write `dk_tie3_prep.h` (namespace `dktie`, `#pragma once`, host-friendly):
   - `constexpr` vertex stride + attrib table as plain offset/size/type
     triples (same plain-int encoding convention as PipelineTable.h; the
     deko3d enum NAMES go in comments only),
   - a header comment with file:line citations for EVERY constant.
2. Host syntax check (below). 3. Report the vertex-struct verdict (same as
   PreloadedVertex or not).

## Done-check

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_tie3_prep.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

HOST-OK + DONE-PASS + NRO md5 `0624560aab120bb451bd2b2923878ca6` (T62 baseline, re-based 2026-10-20; a move is NOT a stop: A/B-attribute — stash only your files, rebuild, compare, restore, rebuild — and report both md5s; STOP only if the A/B does not attribute)
(unwritten header cannot change the NRO).

## Doubts

- If Tie3 streams MULTIPLE vertex buffers (separate attrib binds), record one
   table per buffer — do not merge them.
