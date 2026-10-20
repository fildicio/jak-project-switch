# T63 — jak2 + jak3 build re-check after T62 (AI-assisted)

Track D · Depends on: T62 (reviewer-committed, already in HEAD) · Report only, NO code change · Long (2 builds)

## Goal

T62 changed dk_device.cpp (splash FlipY), TFragmentDk.h/.cpp and dk_tfrag_record.cpp (built only for
jak1). Check that jak2 and jak3 still compile.
Do NOT fix anything — just report.

## Session rules (hard)

- Do NOT edit any file except QWEN_REPORT.md and the README T63 row.
- Build jak2 and jak3 here (exception to "jak1 only", same as T61).
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
   grep -E "warning:" build-switch-jak2/ai-check.log | grep -cE "TFragmentDk|dk_tfrag_record|dk_device|dk_renderer|dk_texture_gpu"
   ```
4. Report under `## T63`: step-0 line; per game PASS/FAIL, md5, warning count,
   count of warnings on the T62 files (expected 0), first 20 error lines if FAIL —
   copied, not interpreted. Set the README T63 row to 🟡.

Expected: DONE-PASS for both. No reviewer md5 this time (the reviewer did not build jak2/jak3) — just report the md5s.
