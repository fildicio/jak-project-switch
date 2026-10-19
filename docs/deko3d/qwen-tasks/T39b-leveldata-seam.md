# T39b — LIVE wiring 2/2: LevelData seam + setup_for_level (P3.2, AI-assisted)

Track D P3.2 · Depends on: T39a ✅ + soak PASS · One session (~40 min) · LIVE
md5 change expected. This half is mostly INVESTIGATION — stopping to report is
a valid outcome (see the stop-point).

## Goal

Wire `dktf::setup_for_level` to the loader's `LevelData` WITHOUT going through
GL renderer objects, and prepare the gate-test build for T40.

## Session rules (hard — read twice)

- Time box ~40 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T39b`.

## Read (once, exactly these)

1. `grep -rn 'loader_data' game/graphics/opengl_renderer/OpenGLRenderer.cpp | head`
   — where GL gets it.
2. Follow ONE promising access path only (e.g. the loader global/singleton the
   hits point at). Read just the declaration sites.

## Stop-point (hard)

If `LevelData` is only reachable THROUGH the GL renderer object graph, STOP
after the investigation and report the candidate seams (global? pointer the
loader already exposes? a small plain accessor added to the loader?) — the
REVIEWER picks the seam. Do NOT invent a global, do NOT edit GL renderer files.

## Steps (if a clean seam exists)

1. Call `dktf::setup_for_level(...)` once per level load at the seam (the same
   place the GL path triggers its setup), guarded `#ifdef __SWITCH__` +
   device-up, one-shot log already exists (T36b).
2. `discard_tree_cache()` at level unload (same seam family).
3. Gate-test build prep (NO deploy): flip `kTfrag3Draw = true` locally, build,
   record the md5, flip it BACK to false, build again, record that md5 too.
   Both md5s go in the report. Leave the tree with the switch OFF.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + new md5 (wiring). If you hit the stop-point, the report IS the
deliverable: seams found, each with file:line.

## Doubts

- None beyond the stop-point — everything else was decided in T39a/T36.
