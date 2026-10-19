# T55 — Loader geometry buffers → dkpool on the dk path (P2.3, AI-assisted)

Track D P2.3 · Depends on: T54 · One session (~20 min)
Reviewer trial-built T54+T55 together on 2026-10-19 → PASS (md5 below).

## Session rules (hard)

- Zero decisions: apply the patch, build, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T55`.

## Why

The hardware GL census (T51) showed glGetError, glGenBuffers, glBindBuffer and
glBufferData still running on the deko3d Loader path. All four come from ONE place:
`GpuBufferPool::allocate()`. With GL stubbed, `glGenBuffers` never writes an id,
so every level-geometry upload was silently skipped. This patch is the seam (same
idea as the T43 texture seam): when deko3d is up, `GpuBufferPool` hands out chunks
of the dk buffer pool (`dkdev::buffer_pool()`, 128 MB after T54) and
`upload_to_buffer()` copies the bytes in with `dkdev::copy_to_gpu`. The GL path is
unchanged (every new line is inside `#ifdef __SWITCH__` + `dkdev::is_up()`).

## Steps (exactly these)

1. T54 must already be applied (`git diff --stat` shows its 3 files).
2. `git apply docs/deko3d/qwen-tasks/patches/T55.patch`
   - If it fails: STOP, paste the error in the report. Do not hand-edit.
3. `git diff --stat` → must show exactly these 4 files:
   ```
   .../opengl_renderer/loader/GpuBufferPool.h         | 98 +++++++++++++++++++++-
   .../opengl_renderer/loader/LoaderStages.cpp        | 41 +++++++++
   game/graphics/pipelines/dk_device.cpp              | 19 +++--
   game/graphics/pipelines/dk_device.h                |  2 +-
   ```
4. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat until DONE-PASS / DONE-FAIL
```

This build is long (GpuBufferPool.h is included by many files → many files
recompile and old warnings are printed again — that is expected, do not fix them).

Expected (reviewer trial build):
- **DONE-PASS**, md5 **`af69f0c61a2c1a21d277d3d9a8185d80`**.
- This must print nothing (no warnings from the new lines):
  ```sh
  grep -E "GpuBufferPool.h:.*warning|LoaderStages.cpp:(4[0-9]|[5-7][0-9]|1[6-7][0-9]):.*warning" build-switch-jak1/ai-check.log
  ```
- A different md5 with DONE-PASS: paste it, flag it in Doubts, do NOT stop.

Report `## T55`: the `git diff --stat` lines, the PASS line, md5, the grep output
(should be empty). Flip the README T55 row to 🟡. No commit, no deploy.
