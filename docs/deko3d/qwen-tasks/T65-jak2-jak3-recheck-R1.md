# T65 — jak2 + jak3 build re-check after R1 replay hooks + host test build (AI-assisted)

Track D · Depends on: R1 (reviewer-committed, already in HEAD) · Report only, NO code change · Long (2 Switch builds + 1 host build)

## Goal

R1 added replay-test hooks to TFragmentDk.h/.cpp, dk_renderer.cpp and dk_device.h, plus the host
test `test/test_dk_tfrag_replay.cpp`. Check that jak2 and jak3 still compile for the Switch and that
the host test target builds. Do NOT fix anything — just report.

## Session rules (hard)

- Do NOT edit any file except QWEN_REPORT.md and the README T65 row.
- Build jak2 and jak3 here (exception to "jak1 only", same as T63).
- A FAIL is a valid result: report it, don't fix it, don't stop early.

## Steps

0. Lint (must print `dk-lint: PASS`):
   ```sh
   python3 scripts/ai/check-dk.py
   ```
1. jak2: `scripts/ai/check-bg.sh jak2`, then repeat `scripts/ai/check-tail.sh`
   every ~2 min until `DONE-PASS` or `DONE-FAIL`.
2. Same for jak3: `scripts/ai/check-bg.sh jak3`, poll `check-tail.sh`.
3. For EACH game collect (replace jak2 with jak3 for the second):
   ```sh
   tail -3 /tmp/$(ls -t /tmp | grep dk-check-jak2 | head -1)
   grep -nE "error:|undefined reference" build-switch-jak2/ai-check.log | head -20
   grep -cE "warning:" build-switch-jak2/ai-check.log
   grep -E "warning:" build-switch-jak2/ai-check.log | grep -cE "TFragmentDk|dk_renderer|dk_device"
   ```
4. Host test (no dump yet, so SKIPPED is the expected result):
   ```sh
   cmake --build build-host --target goalc-test -j8 2>&1 | grep -E "error|FAILED" | head -20
   ./build-host/goalc-test --gtest_filter='DkTfragReplay.*:DkRing.*:DkBufferPool*' 2>&1 | tail -5
   ```
5. Report under `## T65`: step-0 line; per game PASS/FAIL, md5, warning count, count of warnings
   on the R1 files (expected 0), first 20 error lines if FAIL — copied, not interpreted; the
   step-4 output. Set the README T65 row to 🟡.

Expected: DONE-PASS for both games, host build with no errors, `DkTfragReplay` SKIPPED and the
other dk tests PASSED. No reviewer md5s for jak2/jak3 — just report them.
