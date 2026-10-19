# T54 — dk buffer pool 128 MB + skip mipq GL on the dk path (P2.3, AI-assisted)

Track D P2.3 · Depends on: T52 ✅ · One session (~15 min)
Reviewer trial-built this exact patch on 2026-10-19 → PASS (md5 below).

## Session rules (hard)

- Zero decisions: apply the patch, build, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T54`.

## Why

The T51 hardware GL census showed 5 GL calls still reached on the deko3d Loader
path. One is `glActiveTexture`, called every frame by `mipq_process()` even though
the dk path never queues mipmaps. This patch returns early there when deko3d is up.
It also grows the dk level-geometry pool from 8 MB to 128 MB (T55 puts level
geometry in it; 4 live jak1 levels need far more than 8 MB) and adds the pool to
the boot heap check. Hardware runs show ~2.5 GB heap free, so 128 MB is safe.

## Steps (exactly these)

1. `git apply docs/deko3d/qwen-tasks/patches/T54.patch`
   - If it fails: STOP, paste the error in the report. Do not hand-edit.
2. `git diff --stat` → must show exactly these 3 files:
   ```
   game/graphics/opengl_renderer/loader/LoaderStages.cpp |  7 +++++++
   game/graphics/pipelines/dk_device.cpp                 | 19 +++++++++++--------
   game/graphics/pipelines/dk_device.h                   |  2 +-
   ```
3. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat until DONE-PASS / DONE-FAIL
```

Expected (reviewer trial build):
- **DONE-PASS**, md5 **`7df9eef8d51208850ce153a0482c3553`**.
- A different md5 with DONE-PASS: paste it, flag it in Doubts, do NOT stop.

Report `## T54`: the `git diff --stat` lines, the PASS line, md5. Flip the README T54
row to 🟡. No commit, no deploy.
