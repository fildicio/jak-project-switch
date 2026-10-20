# T73 — Tie CPU half: TieDk.cpp + host test (P3.3, AI-assisted)

Track D P3.3 · Depends on: T72 · One session (~15 min)
Reviewer trial-built and ran it on 2026-10-20 → `TieReplay.*` PASS.

## Session rules (hard)

- Zero decisions: apply the patch, build, test, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Report goes in QWEN_REPORT.md under `## T73`.

## Why

Tie = the big world objects (houses, rocks, trees). `TieDk.cpp` is the CPU half of a
deko3d tie pass: it reads the jak1 tie buckets (9 and 16) from the frame's DMA chain,
frustum-culls each tie tree like GL `Tie3::setup_tree`, and builds a draw list.
`TieDk.h` and `test/test_dk_tie_replay.cpp` are already committed; this patch adds
the .cpp and puts the test + .cpp into the host test build.

## Steps (exactly these)

1. `git status --short` → must be empty (clean tree). If not: STOP, report.
2. `git apply docs/deko3d/qwen-tasks/patches/T73-tie-cpu.patch`
   - If it fails: STOP, paste the error in the report. Do not hand-edit.
3. `git diff --stat; git status --short` → exactly:
   ```
    test/CMakeLists.txt | 2 ++
   ?? game/graphics/deko3d_renderer/TieDk.cpp
   ```
4. Done-check (below).

## Done-check

```sh
cmake --build build-host --target goalc-test -j8 2>&1 | grep -E "error|FAILED" | head -20
./build-host/goalc-test --gtest_filter='TieReplay.*:ShrubReplay.*:DkTfragReplay.*' 2>&1 | grep -E "dk-|PASSED|FAILED"
python3 scripts/ai/check-dk.py
```

Expected (reviewer trial):
```
dk-replay: 125 golden lines, 0 differ -> PASS
dk-shrub: bucket20 beach, bucket26 village1
dk-shrub: 47 draws, 164299 tris, 2 cams -> PASS
dk-tie: bucket9 beach, bucket16 village1, envmap 128 128 128 128
dk-tie: 117 draws, 153484 tris, 2 cams -> PASS
[  PASSED  ] 5 tests.
dk-lint: PASS
```
The first grep must print nothing. Any FAIL or different number: STOP, paste it.

Report `## T73`: the step-3 lines and the done-check output. Flip the README T73 row
to 🟡. No commit.
