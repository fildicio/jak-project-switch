# T79 — jak2 + jak3 build re-check after the sprite3 (particles + HUD sprites) commit (report only, AI-assisted)

Depends on: — (reviewer already committed the code) · One session (long: two full builds)

## Session rules (hard)

- Report only. Change NO files except QWEN_REPORT.md and the README row.
- Report goes in QWEN_REPORT.md under `## T79`.

## Why

The reviewer added sprite3 for deko3d (`SpriteDk.cpp/.h`, `dk_sprite_record.cpp`, shaders
`sprite3.vert/frag.glsl` in `_BG_SHADERS`, `dk_device.cpp` loads the sprite3 shader pair,
`dk_renderer.cpp` sprite pass). They are compiled
into every Switch build, so jak2/jak3 must still build. jak1 was already built by the reviewer.

## Steps

1. jak2: `scripts/ai/check-bg.sh jak2`, then repeat `scripts/ai/check-tail.sh` until
   DONE-PASS / DONE-FAIL.
2. jak3: same with `jak3`.
3. `grep -c "error:" build-switch-jak2/ai-check.log build-switch-jak3/ai-check.log`

Expected: both DONE-PASS, `:0` error counts. Previous md5s were jak2
`0dfe4f437e4fd14067582e5d3277707b`, jak3 `1a1e57e68accc1ada2b481588884dea1`; they WILL differ
now — just paste the new ones.

If a build FAILS: do NOT edit code. Paste the first 30 lines containing `error` into the
report and stop.

Report `## T79`: both PASS lines + md5s, the grep output. Flip README T79 to 🟡.
