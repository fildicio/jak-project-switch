# T41 — Tie3 prep: pipeline ids + vertex format + shader plan (P3.3, AI-assisted)

Track D P3.3 · Depends on: T40 gate **PASS** (hard gate — do not start on FAIL) ·
Inert by construction · One small session

## Goal

Ground-work header `deko3d_renderer/dk_tie3_prep.h` for porting Tie3 next
(knowledge-map §16 recipe, step 1-2 done on paper): the verified vertex-format
constants, the pipeline-table entries its draw modes map to, and which shaders
it needs (are the tfrag3 pair enough, or is `tie_wind` required? — find out, do
not assume). No .cpp, no CMake, no GPU calls.

## Context (all read-only)

- `game/graphics/opengl_renderer/background/Tie3.h/.cpp` — the port target.
  `grep -n 'glVertexAttribPointer\\|glVertexAttribIPointer' -A7 Tie3.cpp` for
  the vertex layout; find its vertex struct (likely shares `tfrag3::PreloadedVertex`
  — VERIFY; if different, document the diff).
- `grep -n 'setup_tfrag_shader\\|setup_opengl_from_draw_mode\\|ShaderId' Tie3.cpp`
  — which shader ids + draw-mode features it uses (wind! `tie_wind` exists in
  `game/graphics/opengl_renderer/shaders/`; check if Tie3 or only Hfrag/Shrub
  uses it).
- `game/graphics/deko3d_renderer/PipelineTable.inc` + `docs/deko3d/pipeline_table.md`
  — the ids that cover Tie3's draw modes (same mapping rules as
  `find_for_draw_mode`, dk_state.h T35).
- `game/graphics/deko3d_renderer/shaders/` — note which .glsl are missing vs
  Tie3's needs (tfrag3 pair exists from T25; tie_wind.glsl does NOT — that is a
  later UAM task, only LIST it here).

## Steps

1. Read everything above; write `dk_tie3_prep.h`:
   - `namespace dktie {` constants: vertex stride, attrib table as plain
     `constexpr` arrays (offset/size/type triples, deko3d-enum-FREE — use the
     same plain-int encoding convention as PipelineTable.h, with the deko3d
     names in comments),
   - `kPipelineIds` — the PipelineDesc ids (strings) Tie3 needs,
   - `kShadersNeeded` — enum listing tfrag3 pair and/or tie_wind,
   - header comment: file:line citations for EVERY constant (reviewer re-checks).
2. Host syntax check (same -fsyntax-only command as T37).

## Rules

- ONE new header. Nothing else changes. No GPU calls, no build changes.
- If Tie3's vertex struct differs from PreloadedVertex in more than field
  ORDER (e.g. different width), write its OWN table — never overload tfrag's.

## Done-check

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_tie3_prep.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

HOST-OK + DONE-PASS + NRO md5 unchanged (`08fc9510…` — unwritten header).

## Doubts

- Tie3's multi-tree/multi-geom structure vs tfrag's (Tie3 has `Tie3::Tree`
  wind animation state): note the state you will need; do not design it now.
