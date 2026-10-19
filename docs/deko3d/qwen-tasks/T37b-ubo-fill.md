# T37b v2 — UBO fill functions (apply pre-tested patch) (P3.2, AI-assisted)

Track D P3.2 · Depends on: T37a ✅ · Inert until T38 · One session (~15 min)

v2 (2026-10-19, reviewer): v1 was an open "port the GL math" prompt and the
coder spiraled 30 min with 0 files. The reviewer wrote the code AND tested it on
the Mac host (T37B-HOST-OK). Your job: apply, check, report. ZERO decisions.

## Session rules (hard)

- Do NOT edit any code by hand. Do NOT read any source file. Run the commands
  below in order, exactly as written, from the repo root.
- If any command prints something other than the expected line: STOP, paste the
  output in the report. Do not try to fix it.
- Report goes in QWEN_REPORT.md under `## T37b`.

## Steps (run exactly)

1. Apply the patch:
   ```sh
   git apply docs/deko3d/qwen-tasks/patches/T37b.patch && echo APPLIED
   ```
   Expected: `APPLIED`

2. Host test (compiles and runs the new functions on the Mac):
   ```sh
   xcrun clang++ -std=c++17 -I. -Ithird-party/fmt/include -o /tmp/t37b_test docs/deko3d/qwen-tasks/patches/T37b_host_test.cpp && /tmp/t37b_test
   ```
   Expected last line: `T37B-HOST-OK`

3. Switch build:
   ```sh
   scripts/ai/check-bg.sh jak1
   ```
   then repeat `scripts/ai/check-tail.sh` every ~60 s until `DONE-PASS` or `DONE-FAIL`.
   Expected: `DONE-PASS` and md5 `c613ba81bb31570d13750a2fd24d07a7`
   (the header is not included by any .cpp yet, so the md5 must NOT move.
   The gate now pins the git revision, so commits no longer change the md5.)

4. Report: the three expected lines you got, and `git diff --stat`
   (expected: only `game/graphics/deko3d_renderer/dk_tfrag_ubo.h`, ~50 lines added).
   Set the README T37b row to 🟡.

## What the patch does (for the report, do not re-derive)

Adds to `dk_tfrag_ubo.h`: `#include <cstring>`, constants `kAlphaMaxNormal`
(10.f) and `kAlphaMinAfailSecond` (-10.f), and two inline functions
`fill_vert(...)` / `fill_frag(...)` that copy the GL uniform values from
`first_tfrag_draw_setup` / `setup_tfrag_shader` / TFragment.cpp AFAIL into the
std140 structs. `pc_camera` is passed in by the caller (T38), because
`make_new_cam_mat` lives in a GL file.
