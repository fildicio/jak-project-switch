# T75 — jak2 + jak3 build re-check after T72–T74 (report only, AI-assisted)

Depends on: T74 · One session (long: two full builds)

## Session rules (hard)

- Report only. Change NO files except QWEN_REPORT.md and the README row.
- Report goes in QWEN_REPORT.md under `## T75`.

## Why

ShrubDk/TieDk are compiled into every Switch build. jak2/jak3 never turn them on
(jak1-only flags), but they must still build.

## Steps

1. jak2: `scripts/ai/check-bg.sh jak2`, then repeat `scripts/ai/check-tail.sh` until
   DONE-PASS / DONE-FAIL.
2. jak3: same with `jak3`.
3. `grep -c "error:" build-switch-jak2/ai-check.log build-switch-jak3/ai-check.log`

Expected: both DONE-PASS, `:0` error counts. Reviewer saw (shrub only, before T74):
jak2 `80599b543f9eb5a2286ee4a0f6282e42`, jak3 `c185f49411b50412e1475ea535413836`;
after T74 the md5s WILL differ — just paste them.

Report `## T75`: both PASS lines + md5s, the grep output. Flip README T75 to 🟡.
