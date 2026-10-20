# T41b — Tie3 prep 2/2: pipeline ids + shader list (P3.3, AI-assisted)

Track D P3.3 · Depends on: T41a ✅ · Inert · One session (~30 min)

## Goal

Half 2 of T41. Append to `dk_tie3_prep.h`: the pipeline-table ids Tie3's draw
modes map to, and which shaders it needs (tfrag3 pair enough, or `tie_wind`?).

## Session rules (hard — read twice)

- Time box ~30 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T41b`.

## Commands (exactly these)

1. `grep -n 'setup_tfrag_shader\|setup_opengl_from_draw_mode\|ShaderId' \
   game/graphics/opengl_renderer/background/Tie3.cpp | head -30`
2. Read the PipelineTable.inc id list (`grep -n 'id' | head -40`).
3. `ls game/graphics/opengl_renderer/shaders/ | grep -i tie` and
   `ls game/graphics/deko3d_renderer/shaders/` (what exists where).
4. `grep -rn 'tie_wind' game/graphics/opengl_renderer/ | head` (who uses it —
   Tie3 or only Hfrag/Shrub?).

## Steps

1. Append to `dk_tie3_prep.h`:
   - `kPipelineIds` — the table ids covering Tie3's draw modes (cite the
     DrawMode features from command 1 for each),
   - `kShadersNeeded` — enum listing tfrag3 pair and/or tie_wind (grounded in
     command 4's answer; do not assume),
   - citations for every id in the header comment.
2. Host syntax check (below). 3. Report: the tie_wind verdict with evidence.

## Done-check

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_tie3_prep.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

HOST-OK + DONE-PASS + NRO md5 `ce12b1ecc3975f6a9bbf50d8266987d8` (R1 baseline, re-based 2026-10-20 13:30) UNCHANGED.

## Doubts

- If a Tie3 draw mode maps to NO existing table entry, list it as a GAP —
   never force-fit an id.
