# T57 — dk buffer pool 384 MB + quieter copy_to_gpu log (P2.3, AI-assisted)

Track D P2.3 · Depends on: — · One session (~15 min)
Reviewer trial-built this exact patch on 2026-10-19 → PASS (md5 below).

## Session rules (hard)

- Zero decisions: apply the patch, build, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T57`.

## Why

The T55 hardware run (2026-10-19) worked — level geometry went into the dk pool with
zero copy failures — but the 128 MB pool filled up with common + 2 levels (6 failed
allocations at the beach prefetch). jak1 keeps up to 4 levels live, so the pool goes to
384 MB (heap free was ~2.5 GB). `copy_to_gpu` also logged one line per geometry chunk;
it now logs the first 8 calls plus slow ones (> 5 ms), max 40 lines.

## Steps (exactly these)

1. `git apply docs/deko3d/qwen-tasks/patches/T57.patch`
   - If it fails: STOP, paste the error in the report. Do not hand-edit.
2. `git diff --stat` → must show exactly:
   ```
—
   ```
3. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat until DONE-PASS / DONE-FAIL
```

Expected (reviewer trial build):
- **DONE-PASS**, md5 **`   game/graphics/pipelines/dk_device.cpp | 21 ++++++++++++++-------
   game/graphics/pipelines/dk_device.h   |  2 +-`**.
- This must print nothing: `grep -E "dk_device.cpp:.*(warning|error)" build-switch-jak1/ai-check.log`
- A different md5 with DONE-PASS: paste it, flag it in Doubts, do NOT stop.
dd922caa35bddf919b07fed9589d7a0e
Report `## T57`: the `git diff --stat` lines, the PASS line, md5, the grep output
(should be empty). Flip the README T57 row to 🟡. No commit, no deploy.
