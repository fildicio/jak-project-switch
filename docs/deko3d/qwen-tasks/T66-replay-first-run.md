# T66 — first host replay run on the Switch dump (AI-assisted)

Track D · Depends on: the human dump step in `docs/deko3d/REPLAY_TEST.md` §1 (files in
`test/deko3d/replay/jak1/`) · Report only, NO code change · Short

## Goal

Run the tfrag replay test against the first real Switch dump and report the result exactly.

## Session rules (hard)

- Do NOT edit any file except QWEN_REPORT.md and the README T66 row.
- If `test/deko3d/replay/jak1/dk_replay.dma` or `dk_replay.golden.txt` is missing: write
  "no dump yet" in the report, set the row ⛔ and stop.
- A FAIL is a valid result: report it, don't fix it.

## Steps

1. ```sh
   ls -la test/deko3d/replay/jak1/
   head -3 test/deko3d/replay/jak1/dk_replay.golden.txt
   tail -1 test/deko3d/replay/jak1/dk_replay.golden.txt
   cmake --build build-host --target goalc-test -j8 2>&1 | grep -E "error|FAILED" | head -20
   ./build-host/goalc-test --gtest_filter='DkTfragReplay.*' 2>&1 | tail -40
   ```
2. If the test FAILED, also:
   ```sh
   diff test/deko3d/replay/jak1/dk_replay.golden.txt build-host/dk_replay.host.txt | head -40
   diff test/deko3d/replay/jak1/dk_replay.golden.txt build-host/dk_replay.host.txt | grep -c "^<"
   ```
3. Report under `## T66`: the outputs above, copied, not interpreted, plus the `dk-replay:` line.
   Set the README T66 row to 🟡.

Expected: `dk-replay: N golden lines, 0 differ -> PASS`. A few differing `tree`/`draw` lines would
point at float differences in culling between the Switch and the Mac — the reviewer decides.
