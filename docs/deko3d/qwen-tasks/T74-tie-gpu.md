# T74 — Tie GPU half: record + opt-in gk_dktie.txt (P3.3, AI-assisted)

Track D P3.3 · Depends on: T73 · One session (~25 min, long build)
Reviewer trial-built T73+T74 on 2026-10-20 → jak1 PASS (md5 below).

## Session rules (hard)

- Zero decisions: apply the patch, build, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T74`.

## Why

Ties use the same vertex format and shader as tfrag (`PreloadedVertex`, TFRAG3), so
the existing recorder (`record_list` in `dk_tfrag_record.cpp`) draws them with k = 2.
The pass only runs when `sdmc:/switch/jak1/gk_dktie.txt` exists (opt-in, like shrub);
`sd-sync.sh` now places that flag. Without the flag the game behaves as before.

## Steps (exactly these)

1. T73 must be applied (`git status --short` shows `?? .../TieDk.cpp`).
2. `git apply docs/deko3d/qwen-tasks/patches/T74-tie-gpu.patch`
   - If it fails: STOP, paste the error in the report. Do not hand-edit.
3. `git diff --stat` → must list exactly these 5 files:
   `game/CMakeLists.txt`, `dk_tfrag_record.cpp`, `dk_renderer.cpp`,
   `scripts/switch/sd-sync.sh`, `test/CMakeLists.txt`.
4. `bash -n scripts/switch/sd-sync.sh && echo syntax-ok` → `syntax-ok`.
5. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat check-tail until DONE-PASS / DONE-FAIL
python3 scripts/ai/check-dk.py
```

Expected: **DONE-PASS**, md5 **`610dde529c96e9ff4dbee079530c00bb`**, `dk-lint: PASS`.
A different md5 with DONE-PASS: paste it, flag it in Doubts, do NOT stop.

Report `## T74`: the diff --stat, syntax-ok, the PASS line + md5, lint line.
Flip the README T74 row to 🟡. No commit, no deploy.
