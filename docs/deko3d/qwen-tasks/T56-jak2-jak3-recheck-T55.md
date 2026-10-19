# T56 — jak2 + jak3 build re-check after T54+T55 (AI-assisted)

Track D · Depends on: T55 (T54+T55 patches already applied in the working tree) · Report only, NO code change · Long (2 incremental builds)

## Goal

T54+T55 changed GpuBufferPool.h, LoaderStages.cpp, dk_device.cpp and dk_device.h (built only for jak1). Run this ONLY after T55 is 🟡 (its patches are in the working tree).
Check that jak2 and jak3 still compile. Do NOT fix anything — just report.

## Session rules (hard)

- Do NOT edit any file except QWEN_REPORT.md and the README T56 row.
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
   grep -E "warning:" build-switch-jak2/ai-check.log | grep -cE "GpuBufferPool.h|LoaderStages.cpp:(4[0-9]|[5-7][0-9]|1[6-7][0-9]):|dk_device"
   ```
4. Report under `## T56`: step-0 line; per game PASS/FAIL, md5, warning count,
   count of warnings on the new T54/T55 lines (expected 0), first 20 error
   lines if FAIL — copied, not interpreted. Set the README T56 row to 🟡.
