# T76 — jak2 + jak3 build re-check after the tie wind/envmap commit (report only, AI-assisted)

Depends on: — (reviewer already committed the code) · One session (long: two full builds)

## Session rules (hard)

- Report only. Change NO files except QWEN_REPORT.md and the README row.
- Report goes in QWEN_REPORT.md under `## T76`.

## Why

The reviewer changed shared deko3d files (`TFragmentDk.h` DrawCmd::cam is now 16-bit,
`dk_tfrag_record.cpp` camera cache is a vector, `TieDk.cpp` wind + envmap). They are compiled
into every Switch build, so jak2/jak3 must still build. jak1 was already built by the reviewer.

## Steps

1. jak2: `scripts/ai/check-bg.sh jak2`, then repeat `scripts/ai/check-tail.sh` until
   DONE-PASS / DONE-FAIL.
2. jak3: same with `jak3`.
3. `grep -c "error:" build-switch-jak2/ai-check.log build-switch-jak3/ai-check.log`

Expected: both DONE-PASS, `:0` error counts. Previous md5s were jak2
`db61db513363c29ffaa2cd86be357252`, jak3 `858b09703df9ae9bd2c76d6320bfd074`; they WILL differ
now — just paste the new ones.

If a build FAILS: do NOT edit code. Paste the first 30 lines containing `error` into the
report and stop.

Report `## T76`: both PASS lines + md5s, the grep output. Flip README T76 to 🟡.
