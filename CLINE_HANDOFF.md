# Handoff from Cline session (AI-assisted) — 2026-10-03, FIX 71b deployed (CPU boost on blackout loads, watchdog removed)

Repo fildicio/jak-project-switch. Branch: **optmissation-openGoal-NX**.
`main` = F69. Working through PERF_PLAN_NEXT_AGENT.md — **step 1 closed**:
F70 (always-on core pinning) hardware-tested, then F70b (pinning opt-in via
`sdmc:/gk_pin.txt`) A/B-tested on the same workload: **pinning is NEUTRAL**
(98.7 vs 106.3 hitches/min = noise; see plan §"Step 1 FINAL VERDICT").
F70b was live until today; **FIX 71b** (PERF_PLAN step 2:
`appletSetCpuBoostMode(FastLoad)` scoped to blackout loads + `update_blocking`)
is now live on all three games — needs the hardware test below. F71 lasted
about an hour on the card: its watchdog thread `_exit(1)`'d every game at the
first boot blackout (the FIX 34c wall: no runtime `std::thread`, ~4 MB heap
free of 3.2 GB — see kmemcard.cpp FIX 35 notes); 71b is the same boost logic
with the thread removed. Steps 3 (pacing) and 4 (FSR) still queued.

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

## Card state — LIVE: FIX 71b (CPU boost on blackout loads, no watchdog thread), all three games
| game | live `jakN.nro` md5 | rollbacks on card |
|---|---|---|
| jak1 | `eb447364372f1e76cf6a466ab15a0ed7` | `Jak 1.f71.bak`=`1d347363` (**BAD — _exit(1) at boot, do not restore**), `Jak 1.f70b.bak`=`a96f84c9` (last good), `Jak 1.f70.bak`=`1b572fec`, `Jak 1.f68.bak`=`83dfd6cc` |
| jak2 | `95921de813e2c776c909b443589b1a61` | `Jak 2.f71.bak`=`e7202346` (**BAD**), `Jak 2.f70b.bak`=`d56a98bb` (last good), `Jak 2.f70.bak`=`576599fd`, `Jak 2.f69.bak`=`59041473` |
| jak3 | `49012ce48bc87af1c4982748cffbf9b4` | `Jak 3.f71.bak`=`bd064d8f` (**BAD**), `Jak 3.f70b.bak`=`6b17e8c2` (last good), `Jak 3.f70.bak`=`97910d0d`, `Jak 3.f68.bak`=`9e6e3b1d` |

Desktop copies: `~/Desktop/jak bakcups/jakN.f71b.nro` (md5 == live, verified).
F71b log signature: `[boost] cpu boost ON ...` then `[boost] cpu boost OFF
after N ms ...` around every blackout load. Standing rule re-learned 2026-10-03:
**never create a runtime `std::thread` in this port** — the GOAL heap leaves
~4 MB free, pthread stack allocation fails, and the game `_exit(1)`s.

F70b = F68/F69 texture path + `[cores]` diagnostics always-on, pinning only
if `sdmc:/gk_pin.txt` exists (see game/switch/platform.cpp FIX 70b comment
for the hardware numbers that forced the default-off).
Log signature when pinning is OFF (expected on the card now): one line
`[cores] pinning disabled (create sdmc:/gk_pin.txt to enable)` and per-thread
`[cores]` reports showing wherever the scheduler floats them.

## Key metrics + baselines (corrected after F70b A/B, 2026-10-02)
- **Hitch accounting** (primary felt-metric): `[cam] ... HITCH dt=` lines in
  `gk_run_log.txt`. Per-minute rates MISLEAD across different session lengths;
  always report duration + total + max-in-15 s, same-area sessions only.
- jak3 Haven-streaming reality on EVERY build F62..F70b: ~250-275 hitches
  per streaming session (45-50 ms band), plus 1.3-3 s level-load freezes
  (pre-existing since FIX 16 era). Pinning on/off changes none of it
  (A/B: F70 98.7/min vs F70b 106.3/min same workload = noise).
- Loader ema p50/p90 (gk_stdout `[loader] ... ema` lines): jak3 baseline
  36.4/42.3 ms; "slow setup" lines ~10-12 ms vs 8 ms budget during streams.
- gk_stdout.txt keeps only the last boot (truncates); gk_run_log.txt APPENDS
  across boots — segment by `session start 7x` lines; F70+ boots have
  `[cores]` lines, F70b boots say `pinning disabled`.
- /Users/filippo/hitchrate.py prints the full per-boot hitch table for a game's
  gk_run_log.txt (python3, edit path at top).

## Next queue (PERF_PLAN_NEXT_AGENT.md order)
- **Step 2 DEPLOYED (F71b), needs hardware test**: CPU boost during loads —
  `appletSetCpuBoostMode(FastLoad)` while a blackout load / `update_blocking`
  is active, `Normal` right after (see plan §Step 2 DONE note for the hooks).
  Attacks the 1.3–3 s freezes. Test: jak3 Haven City with `hitchrate.py`;
  success = game boots past title (F71 didn't!), worst dt well under 3 s,
  `[boost]` ON/OFF pairs in gk_run_log. Deliberately NOT extended to
  non-blackout backlog (boost also throttles the GPU) — revisit only if loads
  still stall after this.
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
