# Handoff from Cline session (AI-assisted) — 2026-10-03, FIX 76 deployed (area prefetch; FIX 75 rejected + reverted)

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

## FIX 75 REJECTED (hardware, user) → reverted; FIX 76 area prefetch deployed
- **f75 (catchup-burst) is unplayable**: the assumption that stream-in loads
  happen while the old level is FROZEN was FALSE. jak1 area transitions
  (bridges/midsections) are PLAYABLE — EMA 33.3 ms during loads is live
  gameplay rendering, so the burst's 16-66 ms frames hit the player directly
  (user: "10 fps"). Loads finished in 7-8.7 s but at unacceptable cost.
  Reverted (commit `4c6f9a1cd`); f74c NRO restored to card before rebuilding.
- **Standing rule**: with the locked-30fps constraint the loader gets ~7 ms
  of vsync slack per frame (the f74c `catchup` rate, user-validated). A 3.5 s
  upload CANNOT be made faster once started — it must START EARLIER.
- **FIX 76 (area prefetch)**: while the game plays normally and the loader is
  idle, quietly prefetch the most likely NEXT area at the proven catchup rate.
  When the player crosses, `set_want_levels` finds the level already in
  `m_loaded_tfrag3_levels` → nothing queued → `pending=0` → zero loader cost
  at the crossing and no LoadBoost dip: instant area, locked 30 fps. GOAL
  never reads loader state back (only pushes `__pc-set-levels`), so the extra
  cached level is invisible to the game — verified in
  goal_src/jak1/engine/level/level.gc (only `__pc-set-levels`, no poll).
- Prediction = runtime-LEARNED transitions (every observed desired-set change
  records an edge; works for all games, heals bad guesses) over a static
  jak1 overworld table (`kJak1LevelAdjacency` in Loader.cpp).
- Safety: prefetch cancelled within one frame if the game asks for anything
  else (FIX 63 unload path / loader_thread post-read abandon); never starts
  during blackout, under buffer-pool pressure, above live=5, or past 2 cached
  prefetches; every target is `fs::exists`-checked (missing fr3 = fatal).
  Evicted prefetched levels are session-retired (no thrash loop). Pure-prefetch
  work does NOT trigger LoadBoost streaming (`game_pending` vs `pending` in
  update_frame_budget) — a hidden load must never show as a resolution dip.
- Log signatures: `[loader] prefetch: <name> cached (budget catchup)`,
  `prefetch: dropped ... mid-staging`, `prefetch: abandoned ... after file
  read`, and `| pf loading <name>` / `pf N cached` in the [loader] telemetry.
  Success on hardware = crossing a prefetched area shows NO `ready in` line at
  all (level was resident) and ema stays ~33.3 ms throughout.
- jak1 f76 = f74c + revert + FIX 76. jak2/jak3 still on f74c-less line —
  carryover: extract BCn fr3 + rebuild + deploy for both.


## Card state — LIVE: FIX 76 (area prefetch), jak1 only
| game | live `jakN.nro` md5 | rollbacks on card |
|---|---|---|
| jak1 | `15dc4e17df6fa60ae710e7ad76975228` (f76) | `Jak 1.f74c.bak`=`047309a3` (known good, pre-f75), plus older f62..f71b chain |
| jak2 | `3de120596b0a3851c6a9206686285ce7` | `Jak 2.f71b.bak`=`95921de8` (boost, good), `Jak 2.f71.bak`=`e7202346` (**BAD**), `Jak 2.f70b.bak`=`d56a98bb`, `Jak 2.f70.bak`=`576599fd`, `Jak 2.f69.bak`=`59041473` |
| jak3 | `eacc630632f343b5a4c48fad1764fedf` | `Jak 3.f71b.bak`=`49012ce4` (boost, good), `Jak 3.f71.bak`=`bd064d8f` (**BAD**), `Jak 3.f70b.bak`=`6b17e8c2`, `Jak 3.f70.bak`=`97910d0d`, `Jak 3.f68.bak`=`9e6e3b1d` |

Desktop copies: `~/Desktop/jak bakcups/jak1.f76.nro` (md5 == live, verified). Also `jak1.f75.nro` (rejected) and prior fNN chain.
F72 = F71b + PERF_PLAN step 3. `pc_get_display_mode()` reports `fullscreen`
on Switch (kmachine.cpp) so GOAL's `set-frame-rate!` stops force-clearing
`vsync?` (it had persisted `(vsync #f)` into pc-settings.gc — flipped back to
`#t` on card for jak1/jak3, backups `pc-settings.gc.f72.bak`), and
`pc_set_vsync()` clamps to true on Switch (one-shot log `[vsync] Switch:
vsync is the pacing mechanism, ignoring request to disable`). Also fixed the
`[vsync]` log printing garbage for target_fps (missing `(int)` cast on a
float, opengl.cpp). Expected log signature: `[vsync] requested=2 set_ok=1
actual=2` and **no later `requested=0`**; `[cam] dt` should quantize to
33/50/67 ms (the 36-49 ms limiter-overshoot band must vanish). GOAL-side
vsync menu entry is inert on Switch now (cosmetic, documented in FIX 72).
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
- **Step 2 DEPLOYED (F71b), HARDWARE-VALIDATED 2026-10-03 (AI-assisted)**: CPU boost during loads —
  `appletSetCpuBoostMode(FastLoad)` while a blackout load / `update_blocking`
  is active, `Normal` right after (see plan §Step 2 DONE note for the hooks).
  Results (pos-matched HITCH dt, F71b session vs old sessions in same logs):
  jak1 clean (worst 293 ms); jak2 boot freeze ~3.6-4.2 s -> **3.15 s**, forest
  2.5-3.5 s -> **1.57 s** (~20-45% cut); jak3 boot freeze ~3.3 -> 3.12 s
  (marginal). Verdict: keep — real win on jak2, no regressions, but the
  remaining freeze time is GPU-upload + SD-IO bound (FastLoad clamps GPU to
  min clock while the loader is still uploading textures — jak3 Haven City is
  the most upload-heavy), so don't iterate further here; step 3 (pacing) and
  step 4 (FSR) attack the actual bottleneck.
- **Step 3 DEPLOYED (F72), HARDWARE-VALIDATED 2026-10-03 (AI-assisted)**: even 30 fps
  pacing via vsync interval 2. Root cause found in logs: C++ defaults vsync ON
  and boot reaches `requested=2 actual=2`, but GOAL's `set-frame-rate!`
  (pckernel-common.gc:81) force-clears `vsync?` because Switch reported
  `windowed` while refresh 60 != target 30 — persisted `(vsync #f)` into
  pc-settings.gc — and `update-to-os` then pushed vsync off every frame
  (`[vsync] requested=0` at t~11-16 s), leaving the sleeping limiter to pace
  (45-55 ms spread in every [cam] histogram). Fix is C++-only (no GOAL
  recompile): report `fullscreen` from `pc_get_display_mode()` + clamp
  `pc_set_vsync()` true on Switch + `(vsync #t)` re-flip on card (jak1/jak3).
  Test = run each game a few minutes; verify `[vsync] requested=2 ... actual=2`
  persists (no `requested=0` line later in the session) and `[cam] dt` bins
  move to 33/50/67 ms with the 36-49 ms band gone. jak2/jak3 heavy areas may
  still show 50/67 ms slips (GPU-bound — that is step 4 FSR's job, not pacing).
- Step 4: FSR (jak2's main win); Step 5: city traffic
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

## 2026-10-03 addendum — FIX 73c deployed (branch **upscale**, FSR follow-up)

F73/73b (FSR EASU upscaler + panel-height menu labels) verified working on
hardware. **F73c removes the handheld double-resample**: with the old
always-1080p swapchain, a 540p frame did EASU 540→720, bilinear 720→1080,
then vi 1080→720 (two wasted resamples). Changes:

- `SWITCH_RES_OVERRIDE` back to **1** (platform.h): window/swapchain is
  created at the operation-mode size (720 handheld / 1080 docked). This is
  the FIX 7f A/B toggle; the *other* 7f mitigation (capped probe logging)
  stays, and the real 7c killer (kernel-thread SD write storm) is long
  fixed. If a dispatch-start fatalThrow returns, flip it back to 0 —
  `__wrap_nwindowQueueBuffer` logs the rejected vi op in gk_fatal.txt.
- `DisplayManager::set_display_mode` / `set_window_size` are **no-ops on
  Switch**: the swapchain size is decided exactly once at window creation.
  This closes the 2026-09-11 resize-crash class AND stops GOAL's boot-time
  application of saved "fullscreen 1920x1080" from resizing the 720p
  handheld window right back to 1080p (that call really does run at boot:
  "[DISPLAY] Setting to display mode: 1, with window_size: 1920,1080").
- `pc_get_active_display_size` reports the size the window was **created**
  with (not live opmode), so docking mid-session cannot desync GOAL's draw
  region from the framebuffer (mid-session dock = vi upscales 720→1080
  until the next boot; correct but soft).
- FSR renderer logic unchanged: with a 720p handheld draw region the
  mid-FBO path is skipped automatically — EASU 540→720 lands 1:1 on the
  panel, single pass. Docked 540p keeps EASU→720 FBO + bilinear→1080.

Deployed to all three games (md5-verified, previous live NROs rotated to
`Jak N.f73b.bak`, desktop copies `~/Desktop/jak bakcups/jakN.f73c.nro`):
jak1 `466528d998692e1f3a870b18cf52f539`, jak2 `52b0b444055e3a217b59fe4060062ab7`,
jak3 `051753bcb7c30aea4590b9021e63505c`. CGOs untouched (no GOAL changes).

Hardware test for the user: boot each game handheld — expect
`[disp] create_window 1280x720 (res_override=1)` and
`[fsr] EASU 960x540 -> 1280x720` (no "then bilinear", panel 1280x720);
docked should log `create_window 1920x1080` and the mid-FBO path for 540p.

## 2026-10-01+ — F73c hardware-confirmed; next step chosen: STEP 7 (AI-assisted)

User confirmed the f73c NROs work on hardware (handheld + docked). The user's
remaining #1 issue is the **new-area streaming slow motion**. Analysis of the
FIX 9/33/42/48/68/69 history (see `STEP7_COMPRESSED_TEXTURES_DESIGN.md` §1)
shows every cheaper lever is exhausted — FIX 69 (PBO) was hardware-rejected —
so **step 7 (pre-compressed BCn fr3 textures, FIX 74)** was selected.
Design note written: `STEP7_COMPRESSED_TEXTURES_DESIGN.md` (repo root),
**awaiting user approval before implementation** per the plan's step-7 rule.
Steps 5/6 (Haven traffic / LOD) remain queued but address constant city load,
not streaming. Also pending: push the `upscale` branch (cf995cb33).


## 2026-10-02 — FIX 74 implemented, jak1 deployed (AI-assisted)

User approved step 7. FIX 74 (commit 8990981ee, branch `compression-ecc`):
BC1/BC3 fr3 textures with file mips, TFRAG3_VERSION 44.

Deployed on the card, md5-verified, `._` files purged:
- jak1 NRO `a2236f70...`
- all jak1 fr3 files
- old NRO kept as `Jak 1.f73c.bak`
- full pre-FIX74 data backup at `~/Desktop/jak bakcups/2026-10-01-pre-FIX74-full/`

jak2/jak3 are untouched (F73c).
**Next: user hardware test of jak1.** Do NOT convert jak2/jak3 until the user reports back.
