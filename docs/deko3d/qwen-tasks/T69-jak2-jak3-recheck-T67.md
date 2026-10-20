# T69 — jak2 + jak3 build re-check after T67/T68 (AI-assisted)

Track D · Depends on: T67, T68 (🟡 or ✅) · Report only, NO code change · Long (2 Switch builds, overnight OK)

## Goal

T67 added 4 shaders to the shared `_BG_SHADERS` CMake list. Check that jak2 and jak3 still
build for the Switch. Do NOT fix anything — just report.

## Session rules (hard)

- Do NOT edit any file except QWEN_REPORT.md and the README T69 row.
- Build jak2 and jak3 here (exception to "jak1 only", same as T65).
- A FAIL is a valid result: report it, don't fix it, don't stop early.

## Steps

0. `python3 scripts/ai/check-dk.py` → must print `dk-lint: PASS`.
1. jak2: `scripts/ai/check-bg.sh jak2`, then repeat `scripts/ai/check-tail.sh`
   every ~2 min until `DONE-PASS` or `DONE-FAIL`.
2. Same for jak3.
3. For EACH game collect (replace jak2 with jak3 for the second):
   ```sh
   tail -3 /tmp/$(ls -t /tmp | grep dk-check-jak2 | head -1)
   grep -nE "error:|undefined reference" build-switch-jak2/ai-check.log | head -20
   ls build-switch-jak2/game/dksh_gen/dksh/ | grep -cE "^(etie|etie_base)_(vert|frag)_dksh\.h$"
   ```
   Expected per game: `PASS ... md5=...`, no error lines, `4`.
   (Previous md5s: jak2 `3fac8f1e…`, jak3 `9adbff3d…`. They SHOULD stay the same,
   because no code includes the new headers; a different md5 is not a FAIL, just report it.)
4. Report both md5s and the checks. Set the README T69 row to 🟡.
