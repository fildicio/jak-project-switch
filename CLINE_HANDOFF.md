# Handoff from Cline session (AI-assisted) — 2026-10-02, FIX 70b deployed

Repo fildicio/jak-project-switch. Branch: **optmissation-openGoal-NX**.
`main` = F69. Working through PERF_PLAN_NEXT_AGENT.md — **step 1 closed**:
F70 (always-on core pinning) was hardware-tested, REGRESSED frame pacing,
and was replaced by FIX 70b (pinning opt-in via flag) which is now live on
all three games. Verdict details in PERF_PLAN_NEXT_AGENT.md §"Step 1
VERDICT" and SWITCH_PORT_SESSION_NOTES.md (bottom).

## Card + naming convention (player-defined, standing)
- Games live in `sdmc:/switch/jak1/`, `sdmc:/switch/jak2/`, `sdmc:/switch/jak3/`.
  The live NRO is **`jakN.nro`** (lowercase, no space). Always refer to the
  games as jak1/jak2/jak3.
- Build artifact is `build-switch-jakN/game/gk.nro` — deploy it to the card
  **as `jakN.nro`** (rename at deploy time), after rotating the old live NRO
  to `Jak N.fNN.bak`.
- FAT32 is case-insensitive: `jak2.nro` and `Jak 2.nro` are the SAME file.
- Flag files: `sdmc:/gk_nopbo.txt` at CARD ROOT; **`sdmc:/gk_pin.txt` at CARD
  ROOT (NEW, FIX 70b)** — create it to turn core pinning ON, absent = all
  threads float (pre-F70 behavior). `sdmc:/switch/jakN/gk_no_vag.txt`.
- Desktop copies of every deployed build: `~/Desktop/jak bakcups/jakN.fNN.nro`.

## Card state — LIVE: FIX 70b (pinning opt-in), all three games
| game | live `jakN.nro` md5 | rollbacks on card |
|---|---|---|
| jak1 | `a96f84c924ce8597d9d4e35c40859d77` | `Jak 1.f70.bak`=`1b572fec`, `Jak 1.f68.bak`=`83dfd6cc` |
| jak2 | `d56a98bb437fe3f0b04baebea6a8701c` | `Jak 2.f70.bak`=`576599fd`, `Jak 2.f69.bak`=`59041473`, `Jak 2.f68.bak`=`6050a3f1` |
| jak3 | `6b17e8c2f00adc40ad173fd74d24868a` | `Jak 3.f70.bak`=`97910d0d`, `Jak 3.f68.bak`=`9e6e3b1d` |

F70b = F68/F69 texture path + `[cores]` diagnostics always-on, pinning only
if `sdmc:/gk_pin.txt` exists (see game/switch/platform.cpp FIX 70b comment
for the hardware numbers that forced the default-off).
Log signature when pinning is OFF (expected on the card now): one line
`[cores] pinning disabled (create sdmc:/gk_pin.txt to enable)` and per-thread
`[cores]` reports showing wherever the scheduler floats them.

## Key metrics + baselines (from the F70 hardware test, 2026-10-02)
- **Hitch rate** (primary felt-metric): `[cam] ... HITCH dt=` lines in
  `gk_run_log.txt` per session minute. Pre-F70 boots: 4–70/min (jak2 20-min
  "behaved ok" session: 4.3/min). F70 pinned: jak2 88/min, jak3 99/min
  (rejected). F70b should land back in the pre-F70 range — VERIFY on next
  hardware run, same area (Haven City) and roughly similar duration.
- Loader ema p50/p90 (gk_stdout `[loader] ... ema` lines): jak3 baseline
  36.4/42.3 ms; F70 jak2 36.5/40.8, jak3 36.1/42.3 — pinning didn't move it.
- The ~3 s `[cam] dt≈3000ms` freezes are pre-existing level-load pauses,
  NOT a regression — don't chase them as new.
- gk_stdout.txt keeps only the last boot (truncates); gk_run_log.txt APPENDS
  across boots — segment sessions by `session start 7x` lines; F70+ boots are
  identifiable by `[cores]` lines.

## Next queue (PERF_PLAN_NEXT_AGENT.md order)
- **Next hardware test (F70b)**: play jak2/jak3 Haven City ~5 min each; send
  back gk_run_log/gk_stdout/gk_fatal. Compare hitch rate vs the numbers above.
  Optional science: drop `gk_pin.txt` on the root once, replay, compare.
- Step 2: CPU boost during loads (`appletSetCpuBoostMode` on blackout loads).
- Step 3: frame pacing; Step 4: FSR (jak2's main win); Step 5: city traffic
  density (jak3); Step 6: LOD preset; Step 7: precompressed textures.

## Operational reminders
- Build the three games SERIALLY, never concurrently. Docker
  `devkitpro/devkita64`, mount repo at **/work** (`-v "$PWD:/work" -w /work`),
  e.g.: `docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest
  env SWITCH_GAME=jak3 BUILD_DIR=/work/build-switch-jak3 bash scripts/build-switch.sh`
  (local `cmake --build` fails: caches were created under /work in-container).
- The Switch's clock is ~1 day ahead of the Mac; date FATAL/ASSERT entries by
  their position relative to the first `[cores]`-bearing boot, or by file
  mtimes, not by trusting "today".
- Deploy md5-verify (card md5 == build md5) and rotate `.bak`s BEFORE copy;
  sync; eject via Finder if `umount` says Operation not permitted.
- AI usage disclosure: append `(AI-assisted)` to every commit message.
