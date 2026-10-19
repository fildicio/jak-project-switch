# T48 — wire `dktexgpu::init()` on the render thread (P2.3, AI-assisted)

Track D P2.3 · Depends on: T47 (guard must land first) · One session (~15 min) ·
**LIVE on the dk path** (allocates the 48 MB texture image pool at the first frame)
Reviewer trial-built T47+T48 on 2026-10-19 → PASS.

## Session rules (hard)

- Zero decisions: apply the patch, build, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T48`.

## Why

The 2026-10-19 hardware run logged `[dk] tex -`: `dktexgpu::init()` had no caller, so
the texture pool never existed. This patch calls it once in the dk render loop's
first frame (render thread → T47 records it as the owner) and tears it down in
`dk_exit()` before `dkdev::shutdown()`. Nothing uploads yet on the dk path (no Loader
there — later P2.3 work), so the visible effect is telemetry only.

## Steps (exactly these)

1. Check T47 is applied: `git diff --stat` lists `dk_texture_gpu.cpp | 34`. If not:
   STOP, report "T47 missing".
2. `git apply docs/deko3d/qwen-tasks/patches/T48.patch`
   - If it fails: STOP, paste the error. Do not hand-edit.
3. `git diff --stat game/graphics/pipelines/dk_renderer.cpp` → `10 insertions`.
4. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat until DONE-PASS / DONE-FAIL
```

Expected (reviewer trial build, T47+T48):
- **DONE-PASS**, md5 **`91e3442e34b00c2a1107ee07995b9565`**.
- `build ok (1 warnings)` is EXPECTED and NOT yours: `SDL_VERSION redefined`
  (sdl3_compat.h:384), printed because dk_renderer.cpp recompiles. Do NOT fix it.
- A different md5 with DONE-PASS: paste it, flag it in Doubts, do NOT stop.

Report `## T48`: the `git diff --stat` lines, PASS line, md5. Flip the README T48
row to 🟡. No commit, no deploy.

## After review — HUMAN hardware check (2 min, dk mode)

With `gk_dk3d.txt` on the card, the run log must show:
- `[dk] tex pool init: ok (48 MB image memblock, render thread owns uploads)`
- telemetry `[dk] tex live=0 high=0 fail=0 | …` instead of `[dk] tex -`
- `heap free` about 48 MB lower than the 2026-10-19 run (2927 MB → ~2879 MB)
- NO `refused (T47 guard)` line, no crash, still 30 fps.
