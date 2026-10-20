# T42b — Shrub prep 2/2: pipeline ids + shader list (P3.3, AI-assisted)

Track D P3.3 · Depends on: T42a ✅ · Inert · One session (~30 min)

## Goal

Half 2 of T42. Append to `dk_shrub_prep.h`: the pipeline-table ids for Shrub's
draw modes and the shader conversion list (shrub.vert/frag exist GL-side only).

## Session rules (hard — read twice)

- Time box ~30 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T42b`.

## Commands (exactly these)

1. `grep -n 'ShaderId\|setup_' game/graphics/opengl_renderer/background/Shrub.cpp | head`
2. `grep -n 'id' game/graphics/deko3d_renderer/PipelineTable.inc | head -40`
3. `ls game/graphics/deko3d_renderer/shaders/`

## Steps

1. Append `kPipelineIds` (alpha-blended foliage → expect StdAlpha/PremulAdd
   entries — VERIFY which, with citations) + `kShadersNeeded` (shrub.vert/
   .frag listed as needed UAM conversions, like T41b lists tie_wind).
2. Citations for every id in the header comment.
3. Host syntax check (below). 4. Report any draw mode with no table entry.

## Done-check

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_shrub_prep.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

HOST-OK + DONE-PASS + NRO md5 `ce12b1ecc3975f6a9bbf50d8266987d8` (R1 baseline, re-based 2026-10-20 13:30) UNCHANGED.

## Doubts

- Same as T41b: gaps get listed, never force-fitted.
