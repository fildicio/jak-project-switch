# T47 — dktexgpu owner-thread guard (P2.3 safety, AI-assisted)

Track D P2.3 · Depends on: T43 ✅ · One session (~15 min) · INERT (nothing calls
`dktexgpu::init()` yet, so the guard never runs on hardware until T48)
Reviewer trial-built this exact patch on 2026-10-19 → PASS.

## Session rules (hard)

- Zero decisions: apply the patch, build, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T47`.

## Why

`dktexgpu::upload_rgba8` / `upload_bcn` record into dkdev's single command buffer and
submit on its single queue. `present_frame()` (render thread) uses the same two
objects, and deko3d command buffers/queues are not thread-safe. If a loader or game
thread ever uploads, both threads write the same buffer → GPU corruption. This patch
makes the thread that ran `init()` the only allowed uploader; any other thread gets
`0` ("skip this texture", FIX 33 contract) plus one log line. T48 then calls `init()`
on the render thread.

## Steps (exactly these)

1. `git apply docs/deko3d/qwen-tasks/patches/T47.patch`
   - If it fails: STOP, paste the error in the report. Do not hand-edit.
2. `git diff --stat` → must show only
   `game/graphics/deko3d_renderer/dk_texture_gpu.cpp | 34 ++++` (34 insertions).
3. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat until DONE-PASS / DONE-FAIL
```

Expected (reviewer trial build):
- **DONE-PASS**, `build ok (0 warnings)`, md5 **`4033aa17966d90f81388690104639633`**.
- A different md5 with DONE-PASS: paste it, flag it in Doubts, do NOT stop.

Report `## T47`: the `git diff --stat` line, the PASS line, md5. Flip the README T47
row to 🟡. No commit, no deploy.
