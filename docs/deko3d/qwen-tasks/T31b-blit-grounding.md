# T31b — dkrt blit GL-site grounding (P3.1 slice 5b, AI-assisted)

Track D P3.1 · Depends on: T31a ✅ · Inert · One session (~30 min)

## Goal

Half 2 of T31. Ground the wrapper in the REAL game blits: enumerate every
`glBlitFramebuffer` site, its filter/flips, and record the table in the
`dkrt::blit` header comment. Documentation-of-code half — few or no code lines.

## Session rules (hard — read twice)

- Time box ~30 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan. No deko3d API is needed in this half.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T31b`.

## Commands (run exactly these, nothing wider)

1. `grep -n 'glBlitFramebuffer' -B6 game/graphics/opengl_renderer/BlitDisplays.cpp`
2. `grep -rn 'glBlitFramebuffer' game/graphics/opengl_renderer/ | grep -v BlitDisplays`
3. For each site found: read ±6 lines around it (ONE read per site).

## Steps

1. Build the table: site (file:line) | src→dst | filter (LINEAR/NEAREST) |
   flip (from negative-width glScissor-style rects or Y inversion — note ONLY
   what the code shows) | stretch (src size ≠ dst size).
2. Write it into the `dkrt::blit` header comment block in
   `dk_render_target.h` as a numbered list (this grounds the "7 blits"
   inventory from docs/deko3d/gl-fbo-inventory.md — if your count differs from
   7, that is FINE, report the real number).
3. If any site needs stretch AND flip simultaneously: leave a `// T38: verify`
   comment on that row (the deko3d flag bits compose per the header enum —
   T31a pasted it — but the semantics get verified at first live use).
4. If a site uses a mask other than COLOR (depth/stencil blit): add a
   `// OUT OF SCOPE: depth blit` row — do not implement anything.

## Rules

- Only comment edits in `dk_render_target.h` (+ nothing else). No behavior.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `ce41507cb3319875fcb0a1ffe051b82d` UNCHANGED (comments do
not change codegen — if the md5 moves, something else changed → STOP).

## Doubts

- If grep 2 reveals sites outside `opengl_renderer/`, list them in the report
  and leave them out of the table (they are other pipelines' business).
