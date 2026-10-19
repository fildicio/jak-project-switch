# T40 — tfrag3 telemetry + P3.2 PERF GATE hardware protocol (AI-assisted)

Track D P3.2 (gate) · Depends on: T39b ✅ · Part 1 coder (~25 min, one file), Part 2 HUMAN (like T08+T24) · One session + hw run

## Session rules for Part 1 (hard — read twice)

- Time box ~25 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan. Part 1 needs NO deko3d API at all.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T40`.

## Goal — Part 1 (coder): tfrag3 draw telemetry

Extend the T24 `[dk]` report line (dk_renderer.cpp:257-290) with tfrag3 draw
stats fed by T38's counters, same guard/window/null-safety pattern:
`| tfrag draws=N tris=M rec=U.XXXus/draw` (N/M totals in the window, U = mean of
per-frame record µs ÷ draws). Inactive (no counters yet / 0 draws) prints
`tfrag -`. Also log ONE `[dk] tfrag setup: level <name>, trees=N` line at
setup_for_level success (once per level, not per frame).

## Context

- `game/graphics/pipelines/dk_renderer.cpp:249-290` — the report block; extend
  the `tel[192]` buffer (raise to 256) — keep the SINGLE switch_run_logf call.
- T38's counter getters (TFragmentDk.h) — call via null-safe guards.

## Rules (Part 1)

- Only `dk_renderer.cpp` (+ TFragmentDk.h getter decls if missing).
- Never crash on division by zero draws. One line per report window max.

## Done-check (Part 1)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + md5 (changed — live telemetry). Paste both + the diff summary.

---

## Part 2 (HUMAN) — P3.2 PERF GATE, plan §P3.2 (L362-365)

Prepare (coder): flip `kTfrag3Draw = true` in a gate-test build, md5 it, deploy
with the T08 conventions (backup old NRO on SD, md5-verify after copy), put
`gk_dk3d.txt` back so deko3d is selected. Then the user runs:

1. **Boot & see**: jak1 boots, hbmenu → game, and with the switch on you should
   see the tfrag geometry of the current area (expected first visual: wrong-ish
   colors/textures somewhere is OK; geometry must be present and stable; no
   crash within 5 min; `gk_fatal.txt` 0 bytes).
2. **2-min soak on title/Sandover** watching the new `[dk] … tfrag …` line:
   draws count > 0 in-level, rec µs/draw recorded for the gate math.
3. **Perf gate (the decision)**: same clocks (jak1 reference 1200 MHz CPU/GPU —
   set the usual OC profile), same route (title → Sandover first area), two runs:
   GL NRO (old build) vs dk NRO. Compare µs/draw: GL numbers come from the
   `[fps]`/profiler bucket line, dk from the new tfrag line.
   **PASS = dk per-draw ≤ ~1/3 of GL per-draw** and no 60-min-class instability
   in a 10-min informal run.
   PASS → T41/T42 unblock, plan §0 records it.
   FAIL → STOP: write a notes entry (per plan), T41/T42 stay gated, reviewer
   schedules diagnosis tasks.
4. **Parity screenshot**: same spot, same time-of-day: dk via T32 readback
   (`dkrt::readback_rgba8` on the resolved game target — if unwired, capture is
   a follow-up; note it) vs Mac `gk` desktop PNG. Eyeball compare; log deltas.

Record everything in `gk_run_log.txt` lines `[dk] tfrag …` — the reviewer reads
the SD logs afterwards to close the gate.

## Reviewer close-out

On PASS: mark this row ✅ with the two µs/draw numbers + md5s; unblock T41/T42;
update plan §0 (P3.2 gate closed) — the plan's "cheapest point to learn deko3d
won't deliver" checkpoint (L364-365).
