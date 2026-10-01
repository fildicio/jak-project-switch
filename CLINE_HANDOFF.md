# Handoff from Cline session (AI-assisted) — 2026-10-02, FIX 70 deployed

Repo fildicio/jak-project-switch. Branch: **optmissation-openGoal-NX** (FIX 70).
`main` = F69 (PBO off by default), branch `pbo-async-texture-upload` merged
2026-10-02 (`1f78ecca0`). Working through PERF_PLAN_NEXT_AGENT.md — step 1
(FIX 70) implemented, built, deployed; hardware verdict pending.

## Card + naming convention (player-defined, standing)
- Games live in `sdmc:/switch/jak1/`, `sdmc:/switch/jak2/`, `sdmc:/switch/jak3/`.
  The live NRO is **`jakN.nro`** (lowercase, no space), e.g.
  `sdmc:/switch/jak3/jak3.nro`. Always refer to the games as jak1/jak2/jak3.
- Build artifact is `build-switch-jakN/game/gk.nro` — on deploy, copy it to the
  card **as `jakN.nro`** (rename at deploy time).
- FAT32 is case-insensitive: `jak2.nro` and `Jak 2.nro` are the SAME file.
  Rotate the old one away BEFORE an overwrite deploy.
- Rollback series on card: `sdmc:/switch/jakN/Jak N.fNN.bak` (f62…f69).
- Flag files: `sdmc:/gk_nopbo.txt` at the **CARD ROOT** (NOT `/switch/` — the
  FIX 69 flag-path bug, see notes); `sdmc:/switch/jakN/gk_no_vag.txt` (iso.cpp).
- Desktop copies of every deployed build: `~/Desktop/jak bakcups/jakN.fNN.nro`.

## Card state — LIVE: FIX 70 (thread core pinning), all three games
| game | live `jakN.nro` md5 | rollback |
|---|---|---|
| jak1 | `1b572fecb3b0fd132342a337fd0437da` | `Jak 1.f68.bak` = `83dfd6cc` |
| jak2 | `576599fdad828b1a4be5fa4f2c0cdb14` | `Jak 2.f69.bak` = `59041473`, `Jak 2.f68.bak` = `6050a3f1` |
| jak3 | `97910d0d3d0f5eb69eb9de315e955545` | `Jak 3.f68.bak` = `9e6e3b1d` |

F70 = F68/F69 texture path (PBO async upload OFF by default; the root flag is
belt-and-braces) + thread core pinning/diagnostics:
- core 0: EE (GOAL). core 1: render/main. core 2: loader, IOP, DMP, EE-Worker,
  audio (cubeb), DECI2. Core 3 stays OS-reserved.
- Code: helpers in `game/switch/platform.{h,cpp}`
  (`switch_pin_current_thread`, `switch_thread_core_report`,
  `switch_core_diag_periodic`); call sites in `game/system/SystemThread.cpp`,
  `game/graphics/opengl_renderer/loader/Loader.cpp`, `game/graphics/gfx.cpp`,
  `game/sound/989snd/player.cpp`, `game/system/Deci2Server.cpp`.
- Expect `[cores]` lines in `gk_run_log.txt`: one per pinned thread at start
  (`core=N pref=N mask=0x…`) plus a 10 s periodic line from render + loader.
  Any `pin … FAILED rc=…` line = pin rejected by the OS (treat as a finding).

## Hardware test — jak3 FIRST (plan step 1), Haven City
1. Boot jak3 (`jak3.nro`), go to Haven City, stream around for a while.
2. Success = loader ema p50/p90 below the F68 baseline (36.4 / 42.3 ms) and
   fewer >50 ms frames (baseline 233); no crash in `gk_fatal.txt`; no audio
   crackle during streams. If crackle: cubeb now shares core 2 with the
   loader — next iteration moves audio to core 1 or unpins it.
3. Send back `/switch/jak3/gk_run_log.txt`, `gk_stdout.txt`, `gk_fatal.txt`.
4. Races: real parallelism can expose what one crowded core hid. Any new
   crash/freeze → assume a data race first; fix the race, don't unpin.

## Next queue (PERF_PLAN_NEXT_AGENT.md order)
- Step 2: CPU boost during loads (`appletSetCpuBoostMode` APU/FastLoad on
  blackout loads) — only after step 1 verdict.
- Step 3: frame pacing; Step 4: FSR (jak2's main win); Step 5: city traffic
  density (jak3); Step 6: LOD preset; Step 7: precompressed textures.
- F69/PBO closed (nouveau verdict, see notes). Option 2b only if revisited.

## Operational reminders
- Build the three games SERIALLY, never concurrently. Docker
  `devkitpro/devkita64`, mount repo at **/work** (`-v "$PWD:/work" -w /work`)
  — existing build dirs hardcode it; `-e BUILD_DIR=build-switch-$G -e
  SWITCH_GAME=$G -e JOBS=2`; log to `build-switch-$G-fNN.log`; target
  `gk_nro`, artifact `build-switch-$G/game/gk.nro`.
- libnx gotcha: `svcGetThreadCoreMask(s32*, u64*, Handle)` — handle LAST, by
  value. (`svcSetThreadCoreMask`/`svcGetInfo` are the obvious signatures.)
- Deploy: rotate card NRO → `Jak N.fNN.bak` first, cp build `gk.nro` → card
  as `jakN.nro`, `sync`, then md5-compare card vs build.
- The reader drops the card randomly: a cp failing with ENOENT usually means
  the MOUNT vanished (cp aborted before any write — card is safe). Check
  `ls /Volumes/`, wait for `SWITCH SD` to remount, redo.
- `gk_stdout.txt` truncates each boot; `gk_run_log.txt` appends. Switch clock
  ~1 day ahead of the Mac. md5 anything before deleting it (FAT32 dupes).
