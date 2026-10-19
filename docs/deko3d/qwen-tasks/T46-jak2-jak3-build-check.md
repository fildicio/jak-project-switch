# T46 — jak2 + jak3 Switch build check (AI-assisted)

Track D · Depends on: — · Report only, NO code change · Long (2 full builds, ~1–2 h; can run overnight)

## Goal

All deko3d work so far was built only for jak1. Find out whether jak2 and jak3
still compile. Do NOT fix anything — just report.

## Session rules (hard)

- Do NOT edit any file except QWEN_REPORT.md and the README row.
- This task is the ONE exception to "jak1, never jak2": build jak2 and jak3 here.
- Do not stop because a build FAILS — a FAIL is a valid result; report it.

## Steps

1. jak2:
   ```sh
   scripts/ai/check-bg.sh jak2
   ```
   repeat `scripts/ai/check-tail.sh` every ~2 min until `DONE-PASS` or `DONE-FAIL`
   (first build of jak2 compiles everything: expect 30–60 min).
2. Same for jak3: `scripts/ai/check-bg.sh jak3`, then poll `check-tail.sh`.
3. For EACH game, collect:
   ```sh
   tail -3 /tmp/$(ls -t /tmp | grep dk-check-jak2 | head -1)
   grep -nE "error:|undefined reference" build-switch-jak2/ai-check.log | head -20
   grep -cE "warning:" build-switch-jak2/ai-check.log
   ```
   (replace jak2 with jak3 for the second game).
4. Report under `## T46`: per game PASS/FAIL, md5 (if PASS), warning count, and
   the first 20 error lines (if FAIL) — copied, not interpreted.
   Set the README T46 row to 🟡.
