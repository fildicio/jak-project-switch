# T78 — jak2 + jak3 build re-check after the merc2 (characters) commit (report only, AI-assisted)

Depends on: — (reviewer already committed the code) · One session (long: two full builds)

## Session rules (hard)

- Report only. Change NO files except QWEN_REPORT.md and the README row.
- Report goes in QWEN_REPORT.md under `## T78`.

## Why

The reviewer added merc2 for deko3d (`MercDk.cpp/.h`, `dk_merc_record.cpp`, shaders
`merc2.vert/frag.glsl` in `_BG_SHADERS`, `dk_device.cpp` loads the merc2 shader pair,
`dk_renderer.cpp` merc pass). They are compiled
into every Switch build, so jak2/jak3 must still build. jak1 was already built by the reviewer.

## Steps

1. jak2: `scripts/ai/check-bg.sh jak2`, then repeat `scripts/ai/check-tail.sh` until
   DONE-PASS / DONE-FAIL.
2. jak3: same with `jak3`.
3. `grep -c "error:" build-switch-jak2/ai-check.log build-switch-jak3/ai-check.log`

Expected: both DONE-PASS, `:0` error counts. Previous md5s were jak2
`ce9323ca47dc906e63a4f7438df03807`, jak3 `52497c73415ad8e2d0abed6bfce6c252`; they WILL differ
now — just paste the new ones.

If a build FAILS: do NOT edit code. Paste the first 30 lines containing `error` into the
report and stop.

Report `## T78`: both PASS lines + md5s, the grep output. Flip README T78 to 🟡.
