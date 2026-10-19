# T53 — jak2 + jak3 build re-check after T51+T52 (AI-assisted)

Track D · Depends on: — (T51/T52 code already in the working tree) · Report only, NO code change · Long (2 incremental builds)

## Goal

T51+T52 changed dk_device.cpp, dk_texture_gpu.cpp, dk_render_target.cpp and dk_renderer.cpp (built only for jak1).
Check that jak2 and jak3 still compile. Do NOT fix anything — just report.

## Session rules (hard)

- Do NOT edit any file except QWEN_REPORT.md and the README T53 row.
- Build jak2 and jak3 here (exception to "jak1 only", same as T46).
- A FAIL is a valid result: report it, don't fix it, don't stop early.

## Steps

0. Census file up to date (must print `gl-census: up to date`):
   ```sh
   python3 scripts/ai/gen-gl-census.py --check
   ```
1. jak2: `scripts/ai/check-bg.sh jak2`, then repeat `scripts/ai/check-tail.sh`
   every ~2 min until `DONE-PASS` or `DONE-FAIL`.
2. Same for jak3: `scripts/ai/check-bg.sh jak3`, poll `check-tail.sh`.
3. For EACH game collect (replace jak2 with jak3 for the second):
   ```sh
   tail -3 /tmp/$(ls -t /tmp | grep dk-check-jak2 | head -1)
   grep -nE "error:|undefined reference" build-switch-jak2/ai-check.log | head -20
   grep -cE "warning:" build-switch-jak2/ai-check.log
   grep -E "warning:" build-switch-jak2/ai-check.log | grep -cE "dk_device|dk_texture_gpu|dk_render_target|dk_renderer"
   ```
4. Report under `## T53`: step-0 line; per game PASS/FAIL, md5, warning count,
   count of warnings in those 4 files (expected 0), first 20 error
   lines if FAIL — copied, not interpreted. Set the README T53 row to 🟡.
