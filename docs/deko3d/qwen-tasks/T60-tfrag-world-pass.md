# T60 — first visible tfrag draw, opt-in (P3.2, AI-assisted)

Track D P3.2 · Depends on: T58 ✅ + P2.3 soak PASS (T58 hardware run) · One session (~15 min)
Supersedes T38a / T38b / T39a / T39b (reviewer wrote all the code).
Reviewer trial-built these exact patches on 2026-10-20 → PASS (md5 below).

## Session rules (hard)

- Zero decisions: apply the 3 patches, build, report. Do NOT edit any C++ by hand.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`,
  `SWITCH_PORT_SESSION_NOTES.md`, `DEKO3D_MIGRATION_PLAN.md`.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1 only.
- Report goes in QWEN_REPORT.md under `## T60`.

## Why

The P2.3 soak passed, so the tfrag (level background) renderer can draw for real.
Everything is behind a NEW opt-in file `sdmc:/switch/jak1/gk_dktfrag.txt`; without it
the game behaves exactly like T58 (blue pulse).

- `T60a.patch` — device: 1 MB command slices, a Z24S8 depth buffer, GL depth/origin
  conventions, a frame-start cache barrier and a `set_world_recorder` hook in
  `present_frame`; `dktexgpu::slot_of` + `update_rgba8`.
- `T60b.patch` — `TFragmentDk.cpp`: reads the jak1 tfrag buckets (6, 13) from the DMA
  chain, looks the level up in the dk Loader, culls, builds index lists and time-of-day
  palettes, and produces a plain draw list.
- `T60c.patch` — new `dk_tfrag_record.cpp` (turns the list into deko3d draws), CMake
  entry, and the wiring in `dk_renderer.cpp` (flag file, capture before the chain ack,
  build before present).

## Steps (exactly these, in this order)

1. Apply, in order (each must print nothing):
   ```sh
   git apply docs/deko3d/qwen-tasks/patches/T60a.patch
   git apply docs/deko3d/qwen-tasks/patches/T60b.patch
   git apply docs/deko3d/qwen-tasks/patches/T60c.patch
   ```
   - If one fails: STOP, paste the error in the report. Do not hand-edit.
2. `git add -N game/graphics/deko3d_renderer/dk_tfrag_record.cpp && git diff --stat` → must show exactly:
   ```
    game/CMakeLists.txt                               |   1 +
    game/graphics/deko3d_renderer/TFragmentDk.cpp     | 354 ++++++++++++++++++++++
    game/graphics/deko3d_renderer/TFragmentDk.h       |  60 ++++
    game/graphics/deko3d_renderer/dk_texture_gpu.cpp  |  29 ++
    game/graphics/deko3d_renderer/dk_texture_gpu.h    |   9 +
    game/graphics/deko3d_renderer/dk_tfrag_record.cpp | 181 +++++++++++
    game/graphics/pipelines/dk_device.cpp             |  64 +++-
    game/graphics/pipelines/dk_device.h               |   5 +
    game/graphics/pipelines/dk_renderer.cpp           |  39 ++-
    9 files changed, 735 insertions(+), 7 deletions(-)
   ```
3. Done-check (below).

## Done-check (background wrappers ONLY)

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh   # repeat check-tail until DONE-PASS / DONE-FAIL
```

Expected (reviewer trial build):
- **DONE-PASS**, md5 **`cf9c638484856b7bf1769952ec8b9f85`**.
- This must print nothing:
  `grep -E "(TFragmentDk|dk_tfrag_record|dk_device|dk_renderer|dk_texture_gpu).*(warning|error)" build-switch-jak1/ai-check.log`
- A different md5 with DONE-PASS: paste it, flag it in Doubts, do NOT stop.

Report `## T60`: the `git diff --stat` lines, the PASS line, md5, the grep output
(should be empty). Flip the README T60 row to 🟡. No commit, no deploy.
