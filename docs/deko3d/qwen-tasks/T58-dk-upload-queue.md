# T58 — separate deko3d upload queue (P2.3, AI-assisted)

Track D P2.3 · Depends on: T57 · One session (~15 min)
Reviewer trial-built this exact patch on 2026-10-19 → PASS (md5 below).

## Session rules (hard)

- Zero decisions: apply the patch, build, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T58`.

## Why

In the T55 hardware run every `copy_to_gpu` took ~16.6 ms (one screen refresh):
uploads were submitted to the same queue as the screen present, so they waited behind
present work that is blocked on vsync. This patch creates a second queue
(`s_upQueue`, same `DkQueueMaker` + `DkQueueFlags_Graphics` as the present queue, like
the T51 readback queue) and sends `copy_to_gpu` and `submit_upload_cmds` there. If the
queue cannot be created, uploads fall back to the present queue (logged).

## Steps (exactly these)

1. `git apply docs/deko3d/qwen-tasks/patches/T58.patch`
   - If it fails: STOP, paste the error in the report. Do not hand-edit.
2. `git diff --stat` → must show exactly:
   ```
T57
   ```
3. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat until DONE-PASS / DONE-FAIL
```

Expected (reviewer trial build):
- **DONE-PASS**, md5 **`   game/graphics/pipelines/dk_device.cpp | 61 +++++++++++++++++++++++++++--------
   game/graphics/pipelines/dk_device.h   |  2 +-
   (T57 + T58 together; T57 must be applied first)`**.
- This must print nothing: `grep -E "dk_device.cpp:.*(warning|error)" build-switch-jak1/ai-check.log`
- A different md5 with DONE-PASS: paste it, flag it in Doubts, do NOT stop.
0716032f72d76f240ac8dc9ea6494458
Report `## T58`: the `git diff --stat` lines, the PASS line, md5, the grep output
(should be empty). Flip the README T58 row to 🟡. No commit, no deploy.
