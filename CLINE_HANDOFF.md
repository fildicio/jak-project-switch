# Handoff from Cline session (AI-assisted) — 2026-10-03, FIX 76d deployed (area prefetch hardened; f76c hardware results + 3 fixes)

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


## Card state — LIVE: FIX 76d (area prefetch hardened), jak1 only
| game | live `jakN.nro` md5 | rollbacks on card |
|---|---|---|
| jak1 | `2189e2178eb4a381a860ac9612446ff9` (f76e) | `Jak 1.f76d.bak`=`118ef137`, `Jak 1.f76c.bak`=`3fd59674` (prefetch works, purge/dispatch bugs), `Jak 1.f76.bak`=`196d47f0`, `Jak 1.f74c.bak`=`047309a3` (known good, pre-f75), plus older f62..f71b chain |
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

## 2026-10-03 addendum 2 — f76c hardware verdict + FIX 76d (AI-assisted)

User tested f76c: crossings WAY better ("way better than before"), but (a) a
small fps hiccup still perceptible while a prefetch stages, (b) after warping
to Geyser Rock the game crawled ~10-15 fps EVERYWHERE for a while, and (c)
the prefetch stopped working afterwards ("the forbidden forest wasn't loaded
like it did at the beginning").

Log forensics (gk_stdout.txt + gk_run_log.txt, session 7x):
1. **Feature death at the warp exit**: `blackout purge: recycling 3 retired
   level(s)` at every warp — purge takes everything not active/desired, and
   caches are NEVER desired, so it ate the beach+jungle caches AND
   session-retired them (76b rule). Next beach crossing: `ready in 22.47s`, no
   re-prefetch ever. FIX 76d: purge skips `m_prefetch_resident` names; the
   FIX 63 reclaim-on-real-allocation-failure path remains the memory valve.
2. **The crawl**: warp loads training (blackout) + village1 as an
   "additional level" that staged DURING play at catchup-floor. The texture
   stage's dispatch-count clamp has a floor of 4 checked AFTER dispatch, and
   one dispatch costs 1-15 ms of nouveau driver time → every frame 60-100 ms
   (ema 55-67 ms sustained, 43 hitches ≥100 ms in run log). FIX 76d: new
   `g_loader_budget.dispatch_cap` retuned per frame — 1 while staging a pure
   prefetch (plus skip staging entirely when frame ema >34.5 ms), 2 when a
   game load pushes ema >45 ms (non-blackout), 20 otherwise; blackout
   uncapped. FIX 38 floor untouched (progress ≥1 dispatch/frame always).
3. **Prediction noise**: after the Geyser Rock trip the static picker fell
   through to rank-3 `training` (twice: `dropped training mid-staging` ×2,
   wasted staging + hitches). FIX 76d: static table now priority-only
   (each area's #1 entry; learned graph decides otherwise) + 60 s cooldown
   on cancelled targets (`m_prefetch_cooldown`).

Commits: `e8bad800f` (code). NRO f76d = `118ef137a7c4e816214069ec9936dd04`,
verified on card; rollback `Jak 1.f76c.bak`=`3fd59674`.

**f76d hardware validation checklist** (expect):
- `pf N cached` still appears right after boot in village1; crossings to
  cached areas = no `ready in` line.
- Geyser Rock warp: arrival crawl replaced by at most brief dips — no ema
  55-67 stretch, no 60-100 ms hitch cluster in `[cam] HITCH`.
- Warp EXIT keeps caches: next crossing after a warp should still be instant
  (no `ready in 20+s` after `blackout purge`).
- No repeated `dropped X mid-staging` for the same X within a minute.
- Prefetches take ~2-4x longer to cache (dispatch_cap 1) — that is intended.

## FIX 76e — prefetch must never cost a frame (AI-assisted)

Problem: f76d hardware showed "terrible fps in Geyser Rock". The prefetch staged at catchup-pace (7-8 ms) on frames with ~0 slack, and the 34.5 ms EMA gate produced a sawtooth.

Fixes:
- **pf-lean budget.** When the ONLY thing in flight is the prefetch, the budget is 2 ms / 256 KB.
- **Miss backoff.** A real missed frame (raw gap > 40 ms) pauses prefetch staging for 1 s, doubling per repeat up to 8 s (240 frames). It resets after 60 clean staged frames.
- **Big-texture deferral.** During a pure prefetch, textures over 384 KB are deferred. If the same texture has blocked for 90 frames, it is pushed through anyway.
- **Static prediction.** The table now takes the top-2 entries (76d's #1-only never prefetched the jungle).
- **Guard.** `m_in_update_blocking` keeps all of these throttles out of the post-blackout sync sweep, where m_blackout is already false.

Build and deploy:
- NRO f76e = `2189e2178eb4a381a860ac9612446ff9`, verified on card.
- Rollback: `Jak 1.f76d.bak`=`118ef137`.
- jak1 only. jak2/jak3 are untouched and still have no prefetch / BCn.

**f76e checklist:**
- Geyser Rock and village1 dwell: no 36-41 ms budget oscillation; budget lines show `mode=pf-lean`.
- `pf N cached` still appears; it just takes longer.
- Crossing village1 -> jungle on a fresh boot is instant once cached.
- No regression in the warp/blackout load times (`ready in`).

## FIX 76f — Geyser Rock crash + island prefetch (AI-assisted)
- Crash: stack overflow in `joint-exploder-method-28` (recursive bbox split). A joint that moved >20480 units in one long frame never changes side, so it recursed on the same list forever -> null pc. Fix: depth cap 32 (`*joint-exploder-split-depth*`), in GAME.CGO.
- Loader: no prefetch while on training/misty, and they are never prefetch targets (static table + learned). The f76e log showed a beach prefetch on Geyser Rock with 6-11 ms texture stages at ema 39 ms.
- Host goalc build fix: `make_texture(..., true, false)` in goalc/build_level (FIX 74 signature).
- SD: jak1.nro md5 791051f05ccfaa078780f1e6667505dd (rollback `Jak 1.f76e.bak`), GAME.CGO d29d23c9676693f6e92d028f176468a8 (old one in ~/Desktop/jak bakcups/GAME.CGO.f76e.bak).

## FIX 77 — Geyser Rock is GOAL-bound + [goal] profiler (AI-assisted)
- f76f capture (R3+Minus on Geyser Rock): render only 10-14 ms, but `wait_dma` 35-40 ms and `starved=25-37` => the GOAL engine, not the GPU, is over budget. Worst near the warp-gate start area (x≈-5.37M z≈4.37M), even standing still; east side is locked 30. Loader idle (no prefetch).
- Added `[goal]` lines to gk_run_log (Switch, only while R3+Minus diag is on): aggregates the existing jak1 pc-prof events (per process type + display-loop phases) and logs top self-times every 2 s. kmachine.cpp `pc_prof`.
- SD: jak1.nro md5 59dbea88afff5f7f4b2ba8da72820993 (rollback `Jak 1.f76f.bak`). f76f logs saved in ~/Desktop/jak bakcups/logs-f76f-geyser/.
- Next: read `[goal]` from a Geyser Rock capture and fix the named process.

## FIX 78 (AI-assisted) — Geyser Rock particle lag

- Cause: `check-drop-level-training-spout-rain` launches 2 splash particles per dying drop from inside
  particle processing, so they go through `*sp-launch-queue*` (32 entries). At 30fps it overflowed
  constantly; each overflow did `format 0 "ERROR: ... queue is full"`, which goes to the SD game log
  (data/log/jak1.*.log) with an fflush per line. 13,157 such lines in one session; FIX 77 `[goal]`
  showed process-particles at 40-50ms/frame (max 130ms) vs ~1.6ms normally.
- Fix (goal_src/jak1/engine/gfx/sprite/sparticle/sparticle-launcher.gc): SPARTICLE_QUEUE_SIZE 32 -> 64
  (PS2 launches-per-second at 30fps), overflow now silent.
- Deployed ENGINE.CGO 4996e971 + GAME.CGO 6ea857fb (backups ~/Desktop/jak bakcups/*.f77.bak). NRO unchanged.
- Note: any other per-frame `format 0` spam is equally expensive on Switch; check data/log/*.log.

## Next: jak2/jak3 rollout (AI-assisted)
See `JAK2_JAK3_ROLLOUT_PLAN.md`. Blocker: TextureAnimator (jak2/3 only) reads RGBA pixels from GAME.fr3, so BCn extraction must keep those textures RGBA first.

## FIX 79: jak2 kid-escort crash + jak3 artifact-race crash (AI-assisted)
Crash dumps were symbolized against `build-switch-jakN/game/gk` (it matches the deployed F73c NROs).
Only the dumps with svcBreak `lr_off=0x1138` come from the current NROs; the older entries are from earlier builds.

**jak2: crash when the kid/crocadog escort mission loads (`lkiddoge`).** Two sessions (Oct 4 and Oct 5):
`dgo file header kid-escort has overrun heap by 12016 bytes`, followed by a crash in `klink.cpp:604` (registers hold "kid-escort").
- Cause: `lkiddoge` borrows ctywide slot 0 = `#x17c` KB × BORROW_MULT 1.1 = 418 KB. The arm64 DGO is 525 KB (the PS2 original is 262 KB).
  `lerlchal` (520 KB) has the same problem; `lmeetbrt` fits with only 33 KB to spare.
- Fix: in `goal_src/jak2/engine/level/level-info.gc`, ctywide `:borrow-size` goes from `#x17c #x82f` to `#x1fe #x7ad`.
  Slot 0 becomes 561 KB; slot 1 becomes 2161 KB (lwidea peak is 2127 KB). The total is unchanged, so ctywide's own heap is unchanged
  (that heap has only about 68 KB free, so don't take room from it).
- Deployed: jak2 `GAME.CGO` 6f4e27f7…

**jak3: GOAL crash in the desert artifact race (`desrace1`).** pc = EE+0x2431470, which is inside `artifact-race.o`.
- far = garbage root (0xc4001a17) + 0x9c, which is `root-prim` (offset 0xa0, minus 4 for the basic tag).
- Cause: `was-artifact::check-pickup` dereferences `(handle->process (-> *target* pilot vehicle))` without a null check.
- Fix: a guard on all 5 copies of this pattern: artifact-race, desert-chase, desert-jump, des-bush, forest-ring-chase.
- Deployed: 18 jak3 DGOs (DESRACE1/2, DESCHASE, DESJUMP, FRSTA, LBB*).

Backups: `~/Desktop/jak bakcups/f79/`. NROs are unchanged.

**Not fixed (need more data):**
- jak3 `DmaFollower` `tag.addr == 0` assert (Oct 4, desert-artifact-race-2 intro / warpcast).
- jak3 jump to a garbage PC from `nouveau_fence_trigger_work` (Oct 5, Haven ctysluma, 57 minutes in).
Both look like memory corruption in the render path. There is no root cause yet.

## FIX 80: jak3 audio — repeated sounds and music cutting out (AI-assisted)
User report:
- The intro cutscene's first sound repeats several times before the cutscene starts.
- Haven City music sometimes stops.
- Zoomer sounds sometimes don't trigger.
- jak1/2 don't have these problems.

Cause: jak3's overlord refills streamed audio only from the IOP vblank thread
(`VBlankThread` → `CheckVagStreamsProgress` → `ProcessStreamData`).
- Our IOP vblank came from `Gfx::register_vsync_callback`, i.e. once per *rendered frame*, and it collapsed to a single bool.
- During a loading freeze (jak3 has 1–3 s freezes), no refills happened.
- The SPU mixer looped the old 0x2000-byte stream halves, which is the repeated audio.
- `CheckVAGStreamProgress` then saw the play position behind the data, returned 0 and `StopVagStream`, which is the music stopping.
- jak1/2 refill their streams differently.

Fix:
- `IOP_Kernel` gets a 60 Hz timer vblank (`set_timer_vblank`), polled at the top of `dispatch()` and between threads. The IOP loop already wakes every ≤1 ms.
- It is enabled for Jak3/JakX in `runtime.cpp`, and the render vsync signal is ignored in that mode.
- In `vblank_handler.cpp`, the per-tick stream clock divides by 60 instead of `g_nFPS`, because ticks are no longer per frame.

Deployed NRO = **F73c (cf995cb33) + this patch only**, built in a worktree, so it stays compatible with the v43 fr3 files on the card.
- md5 `a7a778db…`; ELF in `~/Desktop/jak bakcups/f80/gk-f80.elf`.
- Rollback: `Jak 3.f73c.bak` on the card (also in `~/Desktop/jak bakcups/f80/`).
- The zoomer-sound symptom is not proven to have the same cause. Re-check it after the user tests.

## FIX 81 — Free secrets / cheats, level select from start (jak2 + jak3) (AI-assisted)

User wanted to skip missions without the debug menu (debug segment costs perf). GOAL-only, GAME.CGO only.

- jak2 `progress.gc` `menu-update-purchase-secrets`: every secret auto-purchased (no task/orb gate) except hero-mode.
  `'auto` items (gungame-blue/dark, reverse-races) get enabled as before; toggleable ones stay off.
- jak2 `progress-draw-pc.gc`: names shown for purchased secrets (not "????????").
- jak2 `pckernel.gc` `update-cheats`: PC cheats always revealed/purchased/unlocked; toggles stay off.
- jak2 `progress-draw.gc` `memcard-unlocked-secrets?`: title Secrets menu always unlocked (level select,
  scene players, scrapbooks = #x0fe0, no hero-mode). Level Select = mission select from the title screen.
- jak3 `progress-draw.gc`: method-12 no story lock (except hero-mode); shown cost 0; title Secrets menu always
  unlocked (level select 1-3, scene players, scrapbooks, model viewers; not commentary).
- jak3 `progress.gc`: buy always allowed, orbs not deducted. Player still chooses what to buy.
- Deployed GAME.CGO for jak2 (ebbf6ba9) and jak3 (27f26752). Backups: ~/Desktop/jak bakcups/f81/.

## FIX 82 — In-game "skip to mission" keeping the save (jak2 + jak3) (AI-assisted)

Pause menu -> Secrets -> Level Select (jak3: Level Select Act 1/2/3, buy free first) opens the select-start list.
When `starting-state != 'title`, confirm calls `play-task idx #f 'play` (no `play-clean` -> `initialize! 'game`
reset), so orbs/gems/purchases/perms are kept; `task-node-open!` closes the prerequisite nodes (and runs their
eval-add item grants). Missions whose play-node is already closed are blocked (low beep) = forward-only.
Title-screen Level Select unchanged (fresh game). Files: jak2 progress.gc + pc/progress/progress-pc.gc,
jak3 progress.gc. GAME.CGO jak2 239c83fb, jak3 ba802585; backups ~/Desktop/jak bakcups/f82/.
Risk: skipping while inside an active mission/race; untested on hardware.

## FIX 83 — jak3 crash C: DirectRenderer vertex ring buffer (AI-assisted)

Three jak3 crashes (Spargus/wascity return, ~19 min) died inside nouveau: `nouveau_fence_emit` jumping to a garbage
PC via `nouveau_fence_wait/next` <- `nouveau_scratch_more` <- `st_bufferobj_data` <- `DirectRenderer::update_gl_*`.
Every DirectRenderer flush did `glBufferData` with a new size -> fresh pipe buffer + old one released through fence
work, hundreds of times per frame. Switch-only now: fixed-size buffer used as a ring, upload with
`glMapBufferRange(WRITE|UNSYNCHRONIZED)` (fallback glBufferSubData), `glDrawArrays` first = ring offset, orphan
with same-size `glBufferData(nullptr)` only on wrap. Safe: regions are never rewritten before the orphan.
Note: `gk_fatal.txt` pc_off/lr_off are relative to `get_memory_info`; add 0x9e510 (f80 ELF) for the gk offset.
Deployed jak3.nro = cf995cb33 + FIX 80 + FIX 83 (md5 02b7d29f). Backups ~/Desktop/jak bakcups/f83/ (+ gk-f83.elf),
card `Jak 3.f80.bak`. jak1/jak2 NROs not rebuilt yet (same code path, untouched until jak3 is confirmed).

## FIX 84 — jak2/jak3: DirectRenderer ring never re-specified, fence-synchronized (AI-assisted)

FIX 83 did not fix the crash. Evidence from the card on 2026-10-06 (f83 NRO):
- jak3 21:46 session died at 1122 s with **erpt 2520-0000 = GPU MMU fault** (GR unit, read, VA 0x6bff4ce000, fault
  type 3). This was the first GPU fault ever recorded on the card. It is in `atmosphere/erpt_reports`, and there is no
  crash_report or gk_fatal for it. Silent session ends: check erpt_reports first.
- jak3 23:03 session crashed at 1135 s: far=0x8 in libdrm_nouveau `bo_map_hash`. The call path is `pushbuf_kref` <-
  `pushbuf_validate` <- `nvc0_draw_vbo` <- `DirectRenderer::flush_pending` (glDrawArrays). The bufctx held a **NULL bo**.
- Cause: FIX 83 orphaned on wrap with a same-size `glBufferData(nullptr)`. Mesa turns that into `invalidate_resource` ->
  `nouveau_buffer_reallocate`, which swaps the BO under the same pipe_resource. Stale vertex state then references the
  freed BO, giving the GPU fault and the NULL/dead bo on the CPU side. The pre-83 code (new-size glBufferData per flush)
  died in the deferred-free fence work (crash C). Both crashes are the same family: buffer storage is reallocated and
  freed while it is still referenced.
- Fix (`DirectRenderer.cpp/.h`, Switch only): storage is allocated once and never re-specified. The ring is split into
  segments (segment = min(buffer, 1 MB), at least 4 segments, so the ring is at most ~4 MB larger than before).
  Leaving a segment drops a `glFenceSync`. Entering a segment waits on its fence (glClientWaitSync, falls back to
  glFinish). An upload larger than a segment drains the whole ring and restarts at 0. Writes still use an
  unsynchronized map.
- jak2 had an "error screen" at ~22:14 in mincan, 10 s after an auto-save. It left no dump and no erpt, so there is no
  hard evidence. jak2 got the same fix, because it still ran the pre-83 per-flush glBufferData path.
- Built in worktree `../jak-f84-wt` = cf995cb33 (F73c) + FIX 80 game diff + DirectRenderer from HEAD.
  - jak3.nro `10649c6a170ad756be73878429af7304`, rollback `Jak 3.f83.bak` (02b7d29f).
  - jak2.nro `e079129aed20183069471f6c400dc9fb`, rollback `Jak 2.f73c.bak` (52b0b444).
  - ELFs and backups are in `~/Desktop/jak bakcups/f84/`.
  - gk_fatal pc_off is relative to get_memory_info. In the f84 jak3 ELF, look up its offset with `nm`.
- If it still crashes, read `atmosphere/erpt_reports` (GpuError*) as well as crash_reports and gk_fatal.

## FIX 85 — jak2 BCn textures (v44 fr3) + prefetch + GOAL fixes; desktop "boot crash" ROOT-CAUSED (AI-assisted)

Scope (source already in tree at HEAD): `decompiler/level_extractor/extract_level.cpp` (BCn fr3,
TFRAG3_VERSION 44), `game/graphics/opengl_renderer/loader/Loader.cpp` (BCn upload path + kill
switch `#ifdef __SWITCH__` — dead on macOS), `goal_src/jak2/engine/anim/joint-exploder.gc`,
`goal_src/jak2/engine/gfx/sprite/particles/sparticle-launcher.gc`.

### The desktop jak2 boot crash was NOT a regression — it was a missing build flag

After rebuilding all 2683 jak2 GOAL targets, desktop jak2 died during kernel boot (EE thread at a
bad PC in unsymbolized GOAL heap code, right after RPC/IOP-CD init). Everything was exonerated in
turn (texture-name errors, gk binary, FIX 79–85 C++, GAME.CGO bisect, clean-vs-incremental cache).
The smoking gun: KERNEL.CGO shrank 119024 -> 101760 bytes with zero kernel source changes, and
**every** object file differed from the Sept backup.

**Root cause: the rebuild ran `(make-group "iso")` WITHOUT `--instruction-set arm64`.** goalc
defaulted to x86-64 and emitted x86-64 GOAL machine code. The host `gk` on this arm64 Mac (and the
Switch NRO) execute GOAL code natively as **aarch64** — so the kernel crashed on first execution.
Not Xcode, not a goalc miscompile, not FIX 85.

Proof chain:
- Old (Sept, boots) iso + ONLY the new KERNEL.CGO -> still crashes (T1 bisect).
- Rebuild with `--instruction-set arm64` -> KERNEL.CGO is **119024 bytes again (exact Sept size)**,
  all 2683 targets in ~18 s.
- Boot test: title -> `[texfmt] FIX 74 BCn compressed texture path active` -> ctywide/lwidea/ctysluma
  streaming, 2200 draws/frame, stable >60 s. Log `/tmp/gk_jak2_arm64c.log`. Only errors are the
  pre-existing "Multiple textures named" ones (byte-identical counts in Oct 1 card logs).

**Standing rule: every GOAL compile for this port MUST pass `--instruction-set arm64`** (see
PERF_PLAN_NEXT_AGENT.md): `./build-host/goalc/goalc --game jak2 --instruction-set arm64 --cmd '(make-group "iso")'`.
jak1 boots at HEAD only because its data was built with the flag. A stale obj cache built without
the flag is poison — wipe `out/jakN/obj` before rebuilding if unsure (build is deterministic:
incremental == clean, byte-for-byte).

### State / leftovers from the debug session
- `out/jak2/obj` + `out/jak2/iso` = clean arm64 FIX 85 build (good; boots).
- `out/jak2/iso.f85` = the **bad x86-64** build — delete it, do not deploy.
- /tmp: `GAME.CGO.f85-clean.bak`, `GAME.CGO.mixed.bak` (both x86-64), `goalc_arm64_build.log`,
  `gk_jak2_arm64*.log`, earlier bisect logs.

### Deployed (2026-10-08, all md5-verified)
- NRO `jak2.nro` = fdd60cef1 build, md5 `49451ab136ce96122152f5a6fc2b87f4` (15,160,616 B);
  f84 rotated to `Jak 2.f84.bak`; desktop copy `~/Desktop/jak bakcups/jak2.f85.nro`.
- Data sync (copy-if-differs): **163 copied, 2711 already identical** — all 148 fr3 (v43→v44 BCn,
  incl. `test-zone.fr3`: leaving it v43 against a v44 NRO is a version-assert landmine),
  iso only `GAME.CGO` + `TSZ.DGO` differed (arm64 rebuild is byte-identical to the working Sept
  data everywhere else — goalc/Xcode fully exonerated), 13 obj (joint-exploder, sparticle-launcher
  + dependents). Full re-verify: **2874 files, 0 mismatches**.
- fr3 shrank 633 MB -> 501 MB (-21%); total game data ~5.07 GB -> ~4.89 GB. The BCn win is GPU
  upload/VRAM, not disk.
- **Rollback caveat**: `Jak 2.f84.bak` NRO alone will NOT run against v44 fr3 (version assert);
  no v43 fr3 backup exists on card or desktop. If f85 misbehaves: fix forward (BCn path is
  loader-side), or re-extract v43 (TFRAG3_VERSION 43) — not a quick revert. Prefetch A/B kill
  switch: create `sdmc:/switch/jak2/gk_no_pf.txt`.
- Awaiting user hardware test: expect `[texfmt] FIX 74 BCn compressed texture path active` in
  `gk_boot_log.txt`, 30 fps target, BCn textures. jak3 rollout ONLY after jak2 confirmed.

## FIX 86 — jak2: area prefetch was structurally OFF in Haven City; now memory-gated + jak2 seed table (AI-assisted)

### FIX 85 hardware verdict (user session 2026-10-09, gk_run_log/stdout on card)
- BCn **confirmed live**: `[texfmt] FIX 74 BCn compressed texture path active` + `FIX 74c
  glTexStorage2D accepted - immutable BCn upload`. User reports clear improvement.
- Remaining complaints: lag between areas + small slowdowns. Log evidence:
  - **`pf 0 cached` in ALL 85 `[loader]` telemetry lines — the FIX 76 prefetch never cached a
    single level all session.** No `[pf] DISABLED` (no kill file). Root cause below.
  - Haven City holds `live 8/9` constantly; districts `ready in 2-15 s` (hiphog 14.8 s,
    gungame 12.7 s, ctypal 7.0 s) at catchup budget; pool had 67.8 MB free.
  - 3091 `Loader::update slow setup` frames (9-18 ms vs 2-8 ms budget) = the visible micro-dips;
    residual runtime mipgen bursts (`mip rate` did=8/16 frames). That is FIX 87 territory.

### Root cause (Loader.cpp set_want_levels idle branch)
Old gate: `loaded + 1 >= min(max_live_levels(), 5)` -> with jak2's max_live 9, city at 8 live,
prefetch could NEVER start exactly where the player crosses districts. The `5` was a jak1-shaped
proxy for memory. Additionally `pick_prefetch_target_locked` had a static table for jak1 only;
jak2 relied on the per-session learned graph (empty at boot).

### Changes (Loader.cpp only, all under `#ifdef __SWITCH__`; no GOAL, no fr3, v44 data untouched)
- Gate replaced with honest signals: live-level hard cap (`max_live_levels()`), FIX 76c's
  `failed_allocations() > 0`, and pool free bytes >= `kPfPressureFreeBytes` (32 MB = 2x the
  budget logic's 16 MB pressure line; f85 session measured 67.8 MB free in-city).
- `kJak2LevelAdjacency` hub-and-spoke seed (ctywide -> districts/outdoors; siblings paired;
  `l*` sub-levels and blackout-only destinations deliberately excluded). Learned transitions
  still outrank it; jak3 remains learned-graph-only.
- Diagnostics: throttled `[pf] idle, not caching: <reason>` on every reason change (<= 1/30 s)
  and `[pf] start: background-caching <name>` — the next hardware log states the prefetch's
  situation instead of us inferring it from `pf 0 cached`.

### Deployed (2026-10-08 build, md5-verified)
- NRO `jak2.nro` md5 `6b3fe8ae5940cb985aab86298f0c6be6` (15,164,712 B); f85 rotated to
  `Jak 2.f85.bak`; desktop copy `~/Desktop/jak bakcups/jak2.f86.nro`.
- **Rollback is trivial this time**: restore `Jak 2.f85.bak` (fr3/CGO unchanged, no version
  assert). Kill switch unchanged: `sdmc:/switch/jak2/gk_no_pf.txt`.

### Hardware test expectations
- In-city idle: `[pf] start: background-caching <district>` lines; `pf 1-2 cached` in telemetry.
- District crossings on a learned/seeded route: `ready in` should drop or vanish (level already
  resident); watch for cancel churn (`[pf]` cancels + 60 s cooldowns = wrong guesses, acceptable).
- Watch `gk_fatal.txt` / crash reports: prefetch into the 9th live slot exercises the eviction
  path more; FIX 63 OOM history means any GPU-memory fatal = report immediately.

## FIX 87 — jak2: prefetch must also ask TIME health, not just bytes (AI-assisted)

### FIX 86 hardware verdict (user session 2026-10-09, ~140 s; user: "worse than before")
- BCn still live (`[texfmt] FIX 74 BCn compressed texture path active`) — the shrink is NOT the
  regression. The regression is the FIX 86 gate: **bytes-free was treated as health.**
- Gate opened exactly once (55.9 s, `[pf] start: background-caching`) — into a loader running
  catchup the WHOLE session (`catchup (ema 4.9->17.2)`, `catchup-pace (ema 36.4)`,
  `catchup-floor (ema 39.9)` with 69.8 MB pool free — memory-rich, time-poor).
- Cost, all measured: `atollext ready in 33.15s` (same load class = 9.97 s in the f85 log,
  3.3x worse); `gc 746 tex` reclaimed the volunteer (live 5 -> 4); **`pf 0 cached` in all 33
  telemetry lines** (zero benefit); **238 `[cam]` hitches in 140 s** vs 205 hitches in f85's
  68-minute session. Session was NOT in Haven City (live 0-5, want 2-4, atoll/wasteland).
- Secondary bug found: the FIX 86 `[pf]` diag lines used `{}` fmt syntax but `switch_run_logf`
  is printf-style (`__attribute__((format(printf,1,2)))`) — logs printed a literal `{}` and
  passed a `std::string` through varargs (UB). We were blind by construction.

### Changes (Loader.cpp only, `#ifdef __SWITCH__`; no GOAL, no data changes)
- New gate arm BEFORE the bytes check: `m_frame_gap_ema_ms > kPfMaxFrameEmaMs` (25.0) blocks
  with reason `frames are too slow to volunteer work`. At the gate nothing is in flight, so
  EMA <= 25 is exactly the `idle-healthy` budget tier: prefetch only ever starts on frames
  that genuinely have room. Pool-bytes check (32 MB) stays — the gate now asks BOTH questions.
  The FIX 76d/e mid-flight skip (34.5 ms EMA + pause-on-miss) stays as the backstop; the
  start gate prevents the un-skippable part (the SD read burst of the volunteer level) from
  ever landing in a starved stream.
- Fixed all three `[pf]` log lines to `%s` printf style (DISABLED/idle/start). Next session's
  log will name the target and the blocking reason for real.

### Deployed (build 2026-10-09, md5-verified)
- NRO `jak2.nro` md5 `1159733eb062f93a820c7c7ddd21b036` (15,164,712 B); f86 rotated to
  `Jak 2.f86.bak`; desktop copy `~/Desktop/jak bakcups/jak2.f87.nro`.
- Rollback trivial: `Jak 2.f86.bak` (data untouched). Kill switch unchanged:
  `sdmc:/switch/jak2/gk_no_pf.txt`.

### Hardware test expectations
- Heavy outdoor areas (atoll/wasteland): expect `[pf] idle, not caching: frames are too slow
  to volunteer work` — that is the FIX working, not failing (f86 proved volunteering there
  is a 3.3x regression on wanted loads). `ready in` for atoll-class loads should return to
  ~10 s, hitch count to f85 levels.
- Haven City calm moments (idle-healthy): `[pf] start: background-caching <district>` with a
  real name in the log (finally, thanks to `%s`), then `pf 1 cached`. If the city NEVER reaches
  idle-healthy EMA, the idle lines will say so — that data decides whether the threshold
  should be 30 (idle-lean allowed) or the city simply can't afford a prefetch.
- A/B via `gk_no_pf.txt` if in doubt.

## FIX 88 — BCn PBO ring staging (the real "before compression" fix)
Date: 2026-10-08 (AI-assisted)

### f87 hardware verdict (gate WORKED; the real bottleneck finally measured)
- The FIX 87 gate + `%s` logs behaved exactly as designed: `[pf] start: background-caching
  ctysluma` at 49.7 s (a real name, in Haven City), honest idle reasons elsewhere
  (`frames are too slow`, `blackout load in progress`, `a level is still staging`). Atoll-class
  `ready in` back to 1-10 s (`title 1.04s`, `lwidea 9.93s`, `ctykora 9.03s`). Gate verdict: KEEP.
- But the user reported performance "kinda like before the compression" — and the data agrees:
  **176 `[cam]` hitches (session 41 of gk_run_log) vs 177 in f85's session 39.** The prefetch
  was never the constant drag; f86 added hitches (238), f87 removed them again (176), the
  baseline never moved. Why, finally measured:
- **`[loader] tex stage: 1222 textures, upload 9762.9ms` (8.0 ms/texture) and
  `738 textures, upload 8844.1ms` (12.0 ms/texture)** — the BCn path costs 4.5-12.7 ms
  PER TEXTURE, vs 1.2-1.4 ms/texture for the RGBA path it replaced (f73c), despite moving
  4-8x fewer bytes. Boot log confirms `glTexStorage2D accepted`, so this is NOT the FIX 74c
  fallback loop — it is the per-mip `glCompressedTexSubImage2D` calls themselves: each hands
  nouveau a CLIENT pointer, and the driver pays validation + transient staging alloc +
  synchronous copy (~1 ms) per call, 5-11 calls per texture. ~18.6 s of render-thread staging
  in one city session. BCn shrank the bytes; the CALLS are the cost. This also explains
  `slow setup 11-18ms` with `mip rate=3 did=0, gpu=0` (2,460 frames — submit cost, not mipgen).
- Slow-setup trend across builds (never the prefetch's fault): f85 3,091 -> f86 2,638 ->
  f87 2,460.

### Changes (LoaderStages.cpp only, `#ifdef __SWITCH__`; no GOAL, no data changes)
- FIX 88: **PBO RING staging for the BCn mip chain, default ON.** Stage the whole `bcn_data`
  into one of 16 ring PBO slots via one orphan+refill `glBufferData(GL_STREAM_DRAW)`, then run
  the per-mip `glCompressedTexSubImage2D` calls from buffer OFFSETS (PBO stays bound through
  the loop, unbound after). bo-relative uploads have no client pointer to stage/copy per call.
- Why this does not repeat the FIX 69 hardware rejection (56.5 ms implicit-sync stalls): FIX 69
  used ONE PBO orphaned on EVERY texture, so every refill waited on the still-in-flight upload.
  A 16-slot ring reuses a slot 16 textures later (2+ frames under vsync) — the GPU has drained.
  Worst case (blackout burst wrapping the ring in one frame) the orphan path allocates fresh
  storage; a hitch behind a black screen is invisible. Oversized textures (>512 KB) fall back
  to the client-pointer loop; `sdmc:/gk_nopbo.txt` (same file FIX 69 honors) kills the whole
  path, same build, one file moved.
- First-BCn-texture log line: `[texfmt] FIX 88 BCn PBO ring staging active (16 slots, max 512 KB)`
  (or DISABLED with the kill-switch reason). Desktop path untouched.

### Deployed (build 2026-10-08, md5-verified)
- NRO `jak2.nro` md5 `8bd2b1974c46cfd6fda0056f366c1ae2` (15,168,808 B); f87 rotated to
  `Jak 2.f87.bak`; desktop copy `~/Desktop/jak bakcups/jak2.f88.nro` (same md5).
- Kill switch: `sdmc:/gk_nopbo.txt` (BCn PBO off -> f87 behaviour). Prefetch kill switch
  unchanged: `sdmc:/switch/jak2/gk_no_pf.txt`.

### Hardware test expectations (one line settles it)
- `gk_boot_log.txt`/`gk_stdout.txt` early: `[texfmt] FIX 88 BCn PBO ring staging active`.
- The money metric — `tex stage:` lines. f87 baseline: 8.0-12.0 ms/texture in cities. If FIX 88
  works, expect **1-2.5 ms/texture** (same texture counts, upload totals ~4-8x smaller, e.g.
  the 1222-texture level ~2-3 s instead of 9.8 s). If it prints ~8+ ms/texture still, nouveau
  is syncing the ring too -> revert via `gk_nopbo.txt` and record the numbers (next idea:
  per-texture dedicated PBOs, as the FIX 69 note already mused).
- Felt effect: the in-area streaming stutter (176-hitch class) should drop substantially —
  this is the first fix that attacks the per-frame submit cost itself rather than re-pacing it.
  `slow setup` frames should collapse toward the 7 ms budget (overshoot was one 8-12 ms texture).
- Failure modes to watch in `gk_fatal.txt`/logs: nouveau_mm_allocate aborts (FIX 33 class —
  ring allocs are small, 16x<=512 KB, and fewer than the per-call staging allocs they replace,
  so risk is lower than status quo, but the class is known). Visual corruption of BCn textures
  would mean a bo-relative copy misread — `gk_nopbo.txt` restores the old path.

### Numbering
- The previously planned "FIX 88" (~3,091 over-budget loader frames retune + residual runtime
  mipgen) is now **FIX 89**. Much of its premise just dissolved: "runtime mipgen" is already
  dead (`did=0` on 2,413/2,460 slow frames, `0 mip chains deferred` in every city tex-stage
  line) — the slow setups were BCn submit cost, which FIX 88 removes at the source. Reassess
  the staging-budget retune AFTER f88's numbers are in; the admission-pacing idea (EMA-based
  "will this texture fit" check before dispatch) remains the follow-up if slow setups persist.

### 2026-10-09 postscript — first f88 hardware run was silently DISABLED (trap to remember)
- The first f88 session looked unchanged ("works like before", tex stage still 8.0/11.7
  ms/tex, 1222 tex @ 9776 ms) because stdout line 43 said it all:
  `[texfmt] FIX 88 BCn PBO ring DISABLED (sdmc:/gk_nopbo.txt present)`. A zero-byte
  `gk_nopbo.txt` from the FIX 69 hardware rejection (Oct 1) was still sitting at the card
  root and toggled the fix off — the kill switch worked perfectly, against us. Session 42:
  148 hitches vs 176 (f87) = area-mix variance, not a fix.
- **Always grep for the `FIX 88 ... staging active` line before judging a session.**
- File deleted from the card 2026-10-09; next boot runs the ring for real. Lesson banked:
  any default-ON fix with an sdmc: kill switch must have its switch-file state checked
  against the log line, not assumed.


## FIX 89 — S3TC storage probe + optional CPU BCn→RGBA8 decode (jak2, Switch)

- Context: FIX 88 PBO ring did not reduce BCn upload cost (8.8–13.1 ms/texture) → now
  default OFF (`sdmc:/gk_pbo.txt` opts in, `gk_nopbo.txt` hard off).
- Hypothesis: driver may be decompressing S3TC on upload. At first BCn upload a 4x4 DXT1
  probe texture is created and `GL_TEXTURE_COMPRESSED` / `GL_TEXTURE_INTERNAL_FORMAT` are
  queried. Log line: `[texfmt] FIX 89 S3TC probe: ...`. If storage is not compressed,
  BC1/BC3 mips are decoded on CPU (`fix89_decode_bcn_mip`, verified bit-exact against a
  Python reference incl. BC1 3-colour mode, BC3 6/8-alpha modes, non-multiple-of-4 edges)
  and uploaded as RGBA8; byte accounting uses w*h*4 when decode is active.
- Caveat: Mesa answers `GL_TEXTURE_COMPRESSED` from the logical format, so the probe will
  probably report "keeps S3TC" even if the driver does a fallback. Use the overrides for a
  real A/B: `sdmc:/gk_decode.txt` = force decode, `sdmc:/gk_nodecode.txt` = force off.
  Compare `tex stage:` ms/texture against f88 (8.8–13.1). Decoding raises VRAM 4–8x for
  those textures — watch for OOM/GPU faults (erpt) in long sessions.
- NRO `jak2.nro` md5 `7cc8d7b574dadd442088fc89774ba64b` (15,172,904 B); f88 rotated to
  `Jak 2.f88.bak` (8bd2b197…). Copy in `~/Desktop/jak bakcups/f89/`. jak3 unchanged.

## FIX 90 — decode is default; ONE upload call per texture (jak2, Switch)

- f89 verdict: probe said `compressed=1 internal=0x83f0 -> driver keeps S3TC`. A/B with
  `gk_decode.txt` (per-mip RGBA uploads) cost the SAME as the per-mip BCn path:
  1222 tex 10231 ms vs 9975 ms, 738 tex 8869 vs 8789. So the cost is the NUMBER of
  glTex*Image calls (~1 ms each on nouveau), not bytes/format. User: intro, save load and
  area loads all slower since BCn (f85).
- FIX 90: decode path uploads level 0 only (one atomic RGBA glTexImage2D, exactly the f73c
  path at 1.2 ms/tex) and defers the chain to the FIX 42 mipq. Decode is now DEFAULT
  ON; `sdmc:/gk_nodecode.txt` restores the f88 BCn path. SD files stay BCn (smaller reads).
- Expect: `[texfmt] FIX 90 decode path: level 0 only + deferred mipgen`, `tex stage:`
  ~1-2 ms/tex (1222-tex level ~1.5-2.5 s), `mipmaps: N deferred chains left` lines return.
  VRAM back to f73c (RGBA) level.
- NRO md5 `97aa0e405d7528e5f40a374e8caa852d`; f89 rotated to `Jak 2.f89.bak`;
  `gk_decode.txt` removed from SD root (no longer needed). Copy in `~/Desktop/jak bakcups/f90/`.

## FIX 91 — BCn decode moved to the loader thread (jak2, Switch)

- f90 verdict: tex stage 3.5-4x faster (1222 tex 10.2 s -> 2.76 s; 738 tex 8.9 -> 2.2 s), but
  `stage texture took` p50 8.6 ms / `slow setup` p50 12.1 ms per streaming frame, and the
  user still sees zoomer slow motion. Note budget EMA is 25-44 ms even with `pending 0`:
  the city is over 33 ms/frame without any loading -> needs an `[fps]` capture
  (hold R3+Minus; diag is opt-in since FIX 41) to split wait_dma / render / swap.
- FIX 91: `decode_level_bcn_to_rgba()` runs in `Loader::loader_thread` after unpack (and in
  `load_common`), converts BC1/BC3 level 0 to `tex.data` RGBA, sets format RGBA, frees
  bcn_data. Render thread now runs the exact f73c RGBA path (glTexImage2D + mipq).
  `sdmc:/gk_nodecode.txt` keeps BCn. Log: `[texfmt] FIX 91 loader-thread decode: N ...`.
- NRO md5 `97f49e6c49fcbcf0d03244295982aada`; f90 rotated to `Jak 2.f90.bak`.

## FIX 92 — CPU floor 1785 MHz + TOD budget 1 (jak2, Switch)

- f91 R3+Minus capture (Haven City, zoomer, 82-141 s): `wait_dma 0.00`, `swap ~1 ms`,
  `render 32 ms` of 33.3 -> RENDER-THREAD CPU bound, zero slack. Streaming adds
  `loader 13-17 ms` -> 40-60 ms frames (22-25 fps) = the slow motion. Top buckets avg:
  blit 4.55 (0 draws), merc-l2 3.7, tie-l0 2.8, sky-pre 2.4, ocean-mid-far 2.2; ~860
  draws/frame. `[tod] trees 3/9 recomputed 5.18 ms (upload 5.01)`. FIX 91 decode costs
  only 43 ms per 1222 textures on the loader thread (KEEP).
- FIX 92a `switch_clock_tick()` (platform.cpp, called per frame from
  `update_frame_budget`): clkrst CPU floor 1785 MHz (stock boost clock), GPU untouched,
  never lowers a higher horizon-oc/sys-clk profile, re-checked every 2 s and right after a
  FastLoad window. Logs `[clk] now cpu=.. gpu=.. mem=.. MHz`. Kill: `sdmc:/gk_noclk.txt`.
- FIX 92b `kTodRecomputeBudget` 3 -> 1 (~3.4 ms/frame in the city).
- Loader tiers deliberately NOT tightened (FIX 38 lesson); the clock creates the slack.
- NRO md5 `83367d6bdacf7abf9ec5e34e7360dd8b`; f91 rotated to `Jak 2.f91.bak`.

## FIX 93 (jak2 Switch) — render-thread trims (AI-assisted)

The f91 diag capture showed the render thread at about 32 ms of a 33.3 ms budget. Three changes; the loader budget is intentionally unchanged at the user's request.

1. **Blit stall.** `BlitDisplays::render` cleared fb 0 at frame start, which made the driver acquire the swapchain image early (`blit` bucket 4.55 ms, 0 draws). On Switch it now sets `SharedRenderState::deferred_window_clear`, and `do_pcrtc_effects` performs the clear just before the final window blit. This is the jak2 equivalent of FIX 13.
2. **Clouds every other frame.** `TextureAnimator::handle_clouds_and_fog` skips `run_clouds` on alternate frames and re-binds the previous texture.
3. **Ocean envmap every other frame.** `OceanTexture::handle_ocean_texture_jak2` still consumes the full DMA but skips the VU emulation, draw and mip chain on alternate frames.

NRO md5 aac3f9115cd6a48b450ee8985c78bd1a. Rollback file: `Jak 2.f92.bak`.

## After FIX 93 — hardware verdict and load-speed analysis (AI-assisted)

- User: "way better". Jak 3 carry-over notes are written in `JAK3_CARRYOVER_FROM_JAK2.md`.
- `[clk] FIX 92 CPU 1428 -> 1785` repeats every 2 s, so sys-clk is resetting the CPU to 1428 MHz. The user must set the sys-clk profile for the hbmenu host title to at least 1785, or remove it.
- Area loads are bound by texture upload calls, not by the SD card.
  - atollext: file load, decompress and unpack took about 0.3 s in total, and the decode took 167 ms.
  - Its 848 textures took about 2.3 ms each, about 3 per frame at the 7 ms tier, so 219 frames and 10.3 s until ready.
- After loads, the mip drain (`mip rate=16`) costs 5–8 ms per frame against a 2 ms idle budget.
- Proposals:
  - (a) Raise the blackout budget (intro, save load and warps are black screens).
  - (b) Time-cap the mip drain.
  - (c) Recycle same-size GL textures from evicted levels.
  - (d) Upload on the loader thread through a shared EGL context. This could take uploads off the render thread entirely, but it is high risk on nouveau and must be gated by a file.

## FIX 94 — faster frozen loads + time-capped mip drain (jak2, Switch) (AI-assisted)

- **Blocking sweep.** Save loads and warps call `update()` back to back while the game is frozen, but they used the gameplay tiers (`catchup-floor` 8 ms, plus a 2-dispatch crawl cap because the EMA was above 45 ms). The new `blocking` tier gives 40 ms, 32 MB and 64 dispatches per call, and the mip drain is deferred until after the fade-in.
- **Black-screen frames.** The `m_blackout` tier goes from 12 to 24 ms and from 4 to 8 MB, with 40 dispatches. When no uploads are waiting, the mips drain at up to 64 chains or 16 ms per frame.
- **Mip drain cap.** `mipq_process(rate, max_ms)` now checks a wall-clock cap after each chain, and always runs at least one. In idle play it gets 2 ms (EMA above 30) or 4 ms, instead of the measured 5–8 ms. This is not the reverted FIX 49, which clamped against the upload's time.
- **Not done:**
  - A second-thread GL context: the devkitPro Mesa is 20.1, and nouveau there is not thread-safe across contexts.
  - Texture recycling: FIX 88 showed the ~1 ms per call cost remains even with the storage already allocated.
- NRO md5 cd926b16acfeb26a626298e5a5ae1999. Rollback file: `Jak 2.f93.bak`.
- **Expect** `mode=blocking` lines after `coming out of blackout`, and a shorter `[boost] cpu boost OFF after N ms` on save loads (f93: 3341 ms).

## FIX 94b (AI-assisted) — partial revert of FIX 94

User report: f94 felt worse than f93. f94 log: loads faster (lwidea 3.02→1.82 s, ctykora 2.26→1.60 s), but the
blocking sweep ran mip rate 0, so the backlog grew to 1463 chains (f93 peak ~650). It then drained during gameplay
at 4-8 chains/frame (5-15 ms "slow setup" lines), with unfiltered textures meanwhile. There were also 1.6-2.1 s hitches right after sweeps.

Changes:
- update_blocking sweep: mip rate 16 with a 10 ms cap, instead of 0 (chains are built while frozen, so it's invisible).
- blackout tier back to 12 ms / 4 MB (f93). Removed the 24 ms tier, dispatch cap 40, and the blackout rate-64 burst (and `m_budget_pending`).
- Kept: 40 ms blocking tier, mipq time cap, and the idle-path 2/4 ms cap.
NRO md5 d6ebc49a138c450b8c6dd518770d32ea. Rollback: `Jak 2.f93.bak` (best known) or `Jak 2.f94.bak`.

## FIX 95 (AI-assisted) — city slowdown without touching clocks

- f94b city log: while an 848-texture level streamed during play, each frame spent 10-15 ms in "slow setup":
  the texture stage took ~9 ms against a 7-8 ms tier, plus 2-4 mip chains. EMA was 37-38 ms, which is the slow motion.
- f93 [phase]: buckets ~20 ms plus pcrtc ~11 ms, mostly the swapchain-acquire wait. That wait is free time.
- OpenGLRenderer publishes `g_switch_frame_free_ms = ph_loader + ph_pcrtc`. The loader takes the minimum over 4 frames minus a
  4 ms margin and uses it to cap the gameplay streaming line (between 1 ms and the tier value). The mip drain gets whatever is left; if
  less than 0.8 ms is left, it does 1 chain every 4th frame. The stage still always dispatches at least 1 texture per frame.
- The game no longer changes clocks: FIX 71 FastLoad boost and FIX 92 CPU floor are now no-ops (user request:
  the horizon-oc profile owns the clocks).
- Log: `[loader] FIX 95 free X ms -> stream budget Y ms` every 60 frames while streaming.
NRO md5 b44c85e8eeaadb622eecc4c13f26919d. Rollback: `Jak 2.f94b.bak`, `Jak 2.f93.bak`.

## Jak 1 rebuilt at FIX 95 (AI-assisted)

- The jak1 card data was already current: v44 BCn fr3 since FIX 74, and the CGOs match local `out/jak1/iso` (FIX 78). FIX 85's extractor change
  only affects jak2/3, so no re-extract was needed. Only the NRO was replaced.
- NRO built from 230e71cdf, md5 868719c435ea6f15658cb4a20b41b50b. The old de7a2f7b1 NRO (59dbea88…) is now `Jak 1.pre-f95.bak`.
  Desktop copy is in `~/Desktop/jak bakcups/f95-jak1/`.
- Jak 1 gets: FIX 84, 86/87 prefetch, 90/91 loader-thread decode, 94/94b, 95 free-time streaming, and no clock changes.
  It does not get FIX 93 (BlitDisplays, clouds and jak2 ocean are jak2/3-only paths).

## Jak 3 rolled out to FIX 95 (AI-assisted)

Executed `JAK3_CARRYOVER_FROM_JAK2.md` §8 in order.

- **GOAL ports (the plan's "to check/port" row): needed and done.** jak3's `engine/anim/joint-exploder.gc` and
  `engine/gfx/sprite/particles/sparticle-launcher.gc` were the untouched Sep 9 decompiles — ported the FIX 85 jak2 changes
  exactly: recursion-depth cap 32 on `adjust-bbox-for-limits` (stack-overflow crash) and the silent `sp-queue-launch` overflow
  (per-particle `format 0` = SD-log fflush cost).
- **Extraction (v44 BCn)**: `task` CLI not on PATH — wrote `scripts/tasks/.env` (GAME=jak3) by hand and invoked the decompiler
  directly with the `extract` task's exact arguments. Binary: `build-host/decompiler/decompiler` (Oct 8 11:04; no extractor
  commits since = current). GAME.fr3 stays RGBA (TextureAnimator rule — 1024 common textures), levels go BCn
  (e.g. waschase 33.5% of old texture bytes). fr3 876 MB -> 701 MB (-20%).
- **GOAL rebuild**: wiped `out/jak3/obj` (stale-cache poison rule), `./build-host/goalc/goalc --game jak3 --instruction-set arm64
  --cmd '(make-group "iso")'` — 3066 targets in 16.5 s. Sanity: pre-rebuild GAME.CGO md5 `ba802585a12e90b0d4c1d7e93c663d16` =
  the card's live CGO, proving the Sept build was arm64. Only `GAME.CGO` (`219a1cafffd3ba4a94a42b58073c3fd2`) and `TSZ.DGO`
  (`31b3b192c319674a816045b04a62bf64`) changed; the other 761 iso files are byte-identical to the card.
- **NRO**: docker `devkitpro/devkita64:latest`, `SWITCH_GAME=jak3 BUILD_DIR=/work/build-switch-jak3 JOBS=2` — 75/75, clean.
  md5 `ec1526ba179fb7d10a7c158863edc8cd` (15,188,931 B). Deployed as `sdmc:/switch/jak3/jak3.nro`; desktop copy
  `~/Desktop/jak bakcups/jak3.f95.nro`.
- **Deploy verification**: all 274 fr3 `cmp`-identical repo↔card; the two changed CGOs md5-verified on card. The only card extras
  in `data/out/jak3/iso/` are the old `GAME.CGO.f72.bak` / `f73.bak` (left in place). SD root and `switch/jak3/` have **no**
  `gk_*.txt` kill switches (checked — the FIX 88 trap).
- **Rollback (jak2 had none; jak3 does)**: `Jak 3.f83.bak` on card (md5 `10649c6a170ad756be73878429af7304`) plus the v43 fr3
  set saved to `~/Desktop/jak bakcups/jak3-v43-fr3/` (274 files, 867 MB). Restore **both together** (f83 NRO + v43 fr3 is a
  consistent pair). Fix-forward is still preferred — the BCn path is loader-side.
- **Awaiting hardware test**: expect `[texfmt] FIX 74 BCn compressed texture path active`, `[texfmt] FIX 91 loader-thread decode`,
  `[loader] FIX 95 free X ms -> stream budget Y ms` (X ~8-11 ms busy), no `[clk]`/`[boost]` lines. jak3 gets FIX 84-95 + both GOAL
  ports, learned-graph-only prefetch (no seed table), and FIX 93 (jak2 render paths). Jak 3 holds up to 11 live levels — if the
  FIX 95 free time is tighter than jak2's, start at 432p.

## FIX 96 (AI-assisted) — f95 hardware verdict + jak3 prefetch, crash guard, zoomer sound

**FIX 95 hardware verdict (user session 2026-10-09, 27 min, card logs): the slowdowns and frame
drops are GONE.** All expected lines present (`FIX 74 BCn path active`, `FIX 91 loader-thread
decode`, `FIX 95 free 2.8-10.5 ms -> stream budget`), zero `[clk]`/`[boost]`. Remaining: area
arrival still cold, zoomer engine sound drops mid-ride, and the recurring crash. All three root-caused
from the card logs:

1. **Prefetch was structurally OFF in jak3.** `pf 0 cached` in every telemetry line for 28 minutes;
   every `[pf]` reason was `frames are too slow to volunteer work`. Cause: FIX 87's start gate is
   `frame-gap EMA > 25.0`, but jak3 pins to 30 fps — the EMA floor is ~33.3 ms with zero correlation
   to headroom (FIX 95 measured 2.8-10.5 ms genuinely free on those same frames). The 25.0 line was
   calibrated on jak2, where sub-25 windows exist (the f87 jak2 test did prefetch ctysluma). And the
   learned graph is empty at boot, so even an open gate had nothing to pick.
   - Fix (Loader.cpp, `#ifdef __SWITCH__`): per-game gate line — `kPfMaxFrameEmaMsJak3 = 34.0`
     (just under the 34.5 mid-flight pause so a started prefetch can keep streaming; the earlier
     gates — in flight / staging / blackout / pool bytes >= 32 MB — still run first, and dispatch
     stays capped at 1 during a prefetch).
   - `kJak3LevelAdjacency` seed table, built from the player's own observed `GAMEPLAY: enter`
     transitions across the five card sessions (not guesswork): desert/Wasteland hub -> outposts,
     Spargus (wascitya), Haven web (ctyinda/ctyport/ctyslum*), sewers, factory/forest, temple
     cluster. `l*` sub-levels and blackout destinations excluded, same policy as jak2's table.
   - Latent bug fixed: the seed-table scan compared `sit == kJak1LevelAdjacency.end()` regardless
     of the selected table (worked only by accident — iterators from different maps never compare
     equal). Now `seed_table->end()`.
   - Kill switch unchanged: `sdmc:/switch/jak3/gk_no_pf.txt`.
2. **The recurring crash (gk_fatal blocks 5/6, 2026-10-07, both identical).** Symbolized against
   `~/Desktop/jak bakcups/f83/gk-f83.elf` (anchor `get_memory_info` = 0x9e510): pc =
   `link_control::jak3_finish+0x1e0`, stack `jak3::link_begin+0x128` / `_stack_call_arm64`. The
   faulting read is `*((entry - 4).cast<u32>())` with **m_entry.offset == 0**: both fatals have
   far = rw_base + 0xfffffffc exactly (entry 0, minus 4, zero-extended), and the DGO name
   ("lfaccar" family — vehicle-h ships in LFACCAR) sits in the argument registers from
   `basename_goal`. So the v5 work loop finished without ever setting the top-level segment
   (`m_entry = m_object_data + 4`) and finish() called through a null entry. Root cause of the
   null entry is still unknown (suspect: link state/heap corruption — see the nouveau family
   below); the fix makes it non-fatal and loud:
   - All four klinks (jak1/2/3/x, same latent pattern) now guard the top-level call: null or
     out-of-arena entry -> skip the call + `[klink] FIX 96 object <name> has invalid top-level
     entry …` naming the object, version and flags, so the next occurrence pinpoints the source.
   - The other fatal blocks: #2 is the pre-FIX-79 artifact-race crash (fixed), #1/#3/#4 are the
     documented nouveau/fence GPU-corruption family (crash C, still open).
3. **Zoomer engine sound going away mid-ride.** The engine/tire loops are bank voices
   (`sound-play-by-name`, vehicle-sound-info), and jak3 swaps the district soundbank when crossing
   Haven City boundaries (`Unload soundbank ctyslmbh / Load ctyslmah` in the data log).
   `snd::Player::UnloadBank` erased the outgoing bank's handlers **without calling Stop()** —
   voices were never keyed off and their grains kept referencing a handler destroyed a moment
   later. Riding across a district boundary = engine loop silently killed.
   - Fix: `Stop()` each handler (the same path `Player::StopSound` uses) before erasing, plus an
     `[aud] FIX 96 bank unload stopped N live sound handler(s)` breadcrumb. If the sound still
     drops WITHOUT that line, the stop comes from the GOAL side or voice stealing — next lead.
   - Note: audio-device re-init (cubeb) was ruled out — all 44 inits are boot-time only.

### Deployed (2026-10-08 build, md5-verified)
- NRO `jak3.nro` md5 `cb9ce08b51c0fa480feb5aff261233ea` (15,201,219 B); f95 rotated to
  `Jak 3.f95.bak` (md5 `ec1526ba179fb7d10a7c158863edc8cd`); desktop copy
  `~/Desktop/jak bakcups/jak3.f96.nro`. CGOs/fr3 unchanged from the FIX 95 rollout — **rollback is
  the f95 NRO alone** (no data pairing this time).
- Card-reader note: the first deploy attempt hit a transient write failure (truncated .bak,
  stale-looking md5s). Redone stepwise; final state verified by md5 on card and desktop.

### Hardware test expectations
- In Haven/wasteland idle: `[pf] start: background-caching <level>` lines and `pf 1-2 cached` in
  telemetry; first cross-district arrivals should feel instant after the prefetch lands.
  Performance must stay at f95 level — the EMA still gates during streams (37-41 ms idle reasons
  stay honest), and `gk_no_pf.txt` A/Bs it if anything regresses.
- No more lfaccar-style hard crash; if the condition recurs it logs `[klink] FIX 96 …` instead —
  send that line back, it names the object.
- Zoomer across district borders keeps its sound; `[aud] FIX 96 bank unload stopped N …` lines
  appearing at swaps is the fix working.


## FIX 97 (AI-assisted) — per-load soundbank accumulation, prefetch churn gate (documented after the fact)
Built and deployed by a previous agent session (commit dda1cc5dd), this section was missing.
- `sndshim.cpp`: `snd_BankLoadFromIOPPartialEx_{Start,,Completion}` now take a `load_token`
  (the ISO command's `SoundBankInfo*`) and accumulate per token, so interleaved jak3 LOAD_SOUNDBANKs
  (WASCITY1/2/3) no longer mix bytes into one global vector. A completion with no data returns
  handle 0 instead of ASSERT-killing the ISO thread. Callers: `overlord/jak3/iso.cpp`,
  `overlord/jakx/iso.cpp` only (jak1/jak2 unaffected).
- `Loader.cpp`: prefetch only volunteers work after the want-set has been stable for
  `kPfWantStableSec`; desert sub-area seed table trimmed.
- Deployed jak3 NRO md5 `5e429b4811fff769835d73c105232ea2` (stdout says "4a17e8ee9" because it
  was built before its own commit). Now kept as `Jak 3.f97.bak` / `~/Desktop/jak bakcups/jak3.f97.nro`.

## FIX 98 (AI-assisted) — jak3 f97 crash: driver staging ran out of heap during a merc upload
### Crash analysis (f97 session, 228 s, desertb streaming in)
- `gk_fatal.txt` real block: `esr=0x92000045` (write, translation fault) `far=0x2`, X00=0x2,
  non-main-thread sp. Symbolized against the f97 ELF (`offset + nm(get_memory_info)`):
  `memcpy` <- `u_default_buffer_subdata` (Mesa) <- `MercLoaderStage::run`
  (LoaderStages.cpp:1857, glBufferSubData of merc indices) <- `Loader::update`.
- Mesa 20.1 NULL-checks the transfer map, so the driver handed back a bogus near-NULL mapping:
  the nouveau buffer/GART staging allocation failed silently. Telemetry right before:
  `live=10 init=1 want=5`, pool 35.5 MB free, **0 failed** allocations. On nouveau
  `glBufferData` never reports OOM, so the FIX 63 "evict on failed allocation" path never ran and
  5 retired levels stayed resident until the heap ran dry.
- The last `gk_fatal.txt` block (pc_off 0x15f8, far = stack top) was the exception handler
  faulting on its own fixed 768-word stack scan (the thread had only ~6 KB above sp).
- Not the sound code: no `[snd]` warnings; FIX 97's accumulator is fine.

### Fix
- `switch_platform::heap_headroom_bytes()` = unclaimed sbrk tail (`fake_heap_end - sbrk(0)`) +
  `mallinfo().fordblks`. (`get_memory_info().used` is flat: libnx claims the whole heap at boot.)
- `Loader::heap_guard()` (Switch, top of `update()`): samples every frame while staging, every 15
  otherwise. Below **96 MB**: purge retired (not active, not desired) levels, return pooled
  buffers, flush texture garbage, block prefetch (`[pf] ... heap headroom is low (FIX 98)`).
  Below **48 MB**: also drop prefetch caches, delete garbage buffers, and pause staging for up to
  60 frames so fenced frees land (never hangs a load). Rate-limited (30 / 10 frames).
- Telemetry line now ends with `| heap NNNMB`; reclaim logs `[loader] FIX 98 heap low|CRITICAL
  X MB -> Y MB` to stdout and `gk_run_log.txt`.
- Exception handler bounds the stack scan with `svcQueryMemory(sp)`.

### Deployed (md5-verified)
- `jak3.nro` md5 `6fc00ac252c73485784897ea86dc84b9`; f97 rotated to `Jak 3.f97.bak`; desktop
  `~/Desktop/jak bakcups/f98/jak3.f98.nro`. Rollback: `Jak 3.f97.bak` (or `Jak 3.f96.bak`).
- jak1/jak2 not rebuilt (shared code, compiles the same; carry over at their next build).

### Hardware test expectations
- Normal play: `heap` in the telemetry stays well above 96 MB → nothing changes, no perf cost.
- Long wasteland/desert sessions: occasional `FIX 98 heap low` lines instead of a crash; a brief
  stream slowdown is possible right after one (retired areas reload from scratch).
- If it still crashes: send `gk_fatal.txt` + the last `heap` values — the thresholds may need
  raising, and the stack dump will now be complete.

## FIX 98b (AI-assisted) — FIX 98 heap guard REVERTED (it caused the slowdown + slow streaming)
- f98 hardware log: `heap 1.7-1.8 GB` free in every telemetry line, so heap exhaustion was NOT the
  f97 crash cause. Worse, `mallinfo()` in `heap_guard()` cost **~12 ms per call** (newlib walks
  every free chunk of a huge fragmented arena): every staging frame and every 15th idle frame
  showed `slow setup: 12-14ms (did=0)`. Frame EMA climbed to 40+ ms, the FIX 95 free-time clamp
  then cut the stream budget to 1 ms -> areas took 15-17 s to stream in.
- Loader.cpp/.h and platform.h restored to the f97 (dda1cc5dd) state. Kept only the
  `svcQueryMemory`-bounded exception stack scan in platform.cpp.
- **Never call mallinfo() per frame on Switch.**
- The f97 merc-upload crash (memcpy to 0x2 inside Mesa glBufferSubData) stays open; the next
  lead is nouveau GART/nvmap exhaustion, not the newlib heap. Need another crash dump to tell.
- Deployed `jak3.nro` md5 see below; f98 kept as `Jak 3.f98.bak`, desktop `f98b/jak3.f98b.nro`.
- jak3 f98b md5 `96b49e261237fa43049fb432685f02ed`.

## FIX 99 (AI-assisted) — jak3 streaming: time-boxed texture garbage + no stage chaining over budget
### f98b hardware verdict (city / gungame / hiphog session, no crash)
- EMA ~33 ms idle (f98 regression gone). Gameplay streams 4-10 s (gungame 10.7 s, hiphog 8.5 s).
- Every `PC unloading X` was followed by one 28-40 ms frame and then **15-20 frames of
  `slow setup: 10-12ms (mip did=0)`** with no stage over 5 ms: the garbage drain deleting 20
  textures/frame (~0.5 ms per glDeleteTextures on nouveau). It overlapped the next area's
  stream, so FIX 95 measured 2-5 ms free -> stream budget 1 ms -> slow loads + hitches.
- Per-texture upload cost stays ~1-1.5 ms per glTexImage2D call (FIX 90 finding), so the
  remaining limit is free frame time; this fix gives that time back to the stream.
### Fix (Loader.cpp, Switch only)
- Garbage texture drain: gameplay caps at ~1.5 ms/frame (3 ms when >400 queued), min 1;
  blackout / update_blocking keep 20/frame uncapped.
- Stage loop: after a stage that did real work (>0.2 ms) finishes, don't start the next stage
  on a gameplay frame whose budget is already spent. Finished stages return instantly, so
  there is no stall.
### Deployed
- jak3 `jak3.nro` md5 `ec64fb00a165bc49a23bf45ebfd34dfd`; f98b kept as `Jak 3.f98b.bak`;
  desktop `~/Desktop/jak bakcups/f99/`.
- Shared code: jak2/jak1 get it at their next rebuild (not deployed: jak2 is "almost perfect").
### Expect
- No 10 ms frames after unloads; `FIX 95 free` values higher during streams -> bigger stream
  budgets -> shorter `ready in` times; fewer `[cam] HITCH` lines around area changes.

## FIX 100 — f99 crash: stale mip queue, prefetch re-fetch churn, handler scans (AI-assisted)

f99 jak3 session: city loads improved (1.8-5.3 s streams), wasteland 6.7-11.6 s.
Crash at 422 s (12 s AFTER the player went back from 60 to 30 fps, so not the
fps toggle itself): `[pf] start: background-caching warpcast` right after
`PC unloading warpcast`, then SIGSEGV in Mesa `_mesa_format_has_color_component`
<- `_mesa_Clear` <- `OpenGLRenderer::do_pcrtc_effects` (FIX 13 deferred window
clear), far=0x13e6b64dd0 — a garbage renderbuffer pointer (Mesa heap object
clobbered; same class as the f97 merc memcpy crash). Root cause not proven.
The last gk_fatal block was the handler faulting again: the FIX 98 bound only
covered the module scan; the GOAL backtrace + FIX 28 scans still read 768 words.

Changes (shared code, built for jak1/2/3):
- `mipq_forget()` (LoaderStages): `unload_level_gpu_objects` drops the level's
  textures from the deferred-mipmap queue. Unloaded textures linger in garbage
  (FIX 99 time-box) and pass `glIsTexture`, so mipq_process wasted 3-5 ms
  glGenerateMipmap per dead texture; after deletion a recycled GL name could make
  a stale entry touch another level's texture.
- Evicted levels get a 90 s prefetch cooldown (no more unload -> immediate re-cache).
- platform.cpp: all three stack scans use the svcQueryMemory-bounded `scan_words`.

Deployed: jak3 f0ed3dbe1e8e79786d27c2a232577c6a (prev `Jak 3.f99.bak`),
jak2 a856d849492b6b5fe4bfba8a07d73452 (prev `Jak 2.f95.bak`),
jak1 fdbbe06eb81c1cd7af85868478055cb5 (prev `Jak 1.f95.bak`). Desktop: `f100/`.
jak1/jak2 also gain FIX 96-99 (soundbank accumulator, prefetch churn gate,
garbage time-box, stage-chain break). Note: never build two games in parallel —
draco's configure writes `draco_features.h` into the source tree (race corrupts it).

**jak1 REVERTED to f95 (868719c4...)** — user reported f100 jak1 brought back slow loads and slowdowns. f100 jak1 kept on SD as `Jak 1.f100.bak`. Do not ship FIX 96-100 to jak1 again without a hardware A/B. (AI-assisted)

## FIX 101 — jak3-only loader rules (AI-assisted)
Root cause of the f100 jak1 regression: the FIX 97 want-set churn gate. jak1/jak2
rewrite __pc-set-levels constantly while walking, so `[pf] idle: the game is streaming
levels (want-set churn)` blocked the prefetch for the whole session -> beach 15.6 s /
jungle 11.8 s visible streams with 9-13 ms loader frames. Now gated to GameVersion::Jak3:
churn gate, FIX 99 stage-chain break, FIX 99 garbage time-box, FIX 100 eviction cooldown.
jak1/jak2 loader = f95 behaviour (+ mipq_forget, crash guards, bounded handler scans).
Deployed jak1 f1159fe5f30ded7ee5295371ad19cbe5 (prev `Jak 1.f95.bak`, `Jak 1.f100.bak`),
jak2 00aa88d333bfdc8761d7efe97ecf58c4 (prev `Jak 2.f100.bak`, `Jak 2.f95.bak`). jak3 unchanged (f100).
RULE: any new loader/prefetch tuning made from jak3 logs must be gated to Jak3.

## FIX 102 — jak1/jak2 prefetch was dead at 30 fps (AI-assisted)
f101 logs: jak1 `[pf] idle: frames are too slow to volunteer work` all session (+ `buffer pool
low on free bytes`), `pf 0 cached`, beach 13.8 s / jungle 12.6 s visible streams, ~45 cam
hitches per 10 s (good f77 session 25: ~1 per 10 s outside area changes). jak2 same reason
(125x). Cause: FIX 87's 25 ms EMA line is a 60 fps line; at target_fps=30 the EMA floor is
33.3. The "perfect" jak1 was f77 (59dbea88, de7a2f7b1), which had neither gate.
Fix: EMA line is 34.0 (FIX 96 jak3 line) whenever target_fps <= 35, for all games; the
pooled-bytes gate is skipped for jak1 (0 MB recycled is its steady state). jak3 unchanged.
Deployed jak1 adf0d2f1ebbb03baa9aa5ab5a9dcbbd9 (prev `Jak 1.f101.bak`), jak2
4e3ec42ccc71b722af38ea7eadb4fa76 (prev `Jak 2.f101.bak`). Desktop `f102/`.
If jak1 is still worse than f77: rebuild de7a2f7b1 in a worktree (59dbea88 binary is lost).

---

## FIX 103 — TODO FOR NEXT AGENT: texture dedup / sharing (AI-assisted)

**Status: designed and measured; NO CODE WRITTEN YET.** SD state is unchanged: jak1 and jak2 = f102,
jak3 = f100. f102 has not yet been hardware-tested by the user.

### Why (the "creative" fix for slow area loads, all 3 games)
On Switch the live texture path is FIX 91 (decode BCn to RGBA on the loader thread) plus FIX 90 (one
`glTexImage2D` per texture, deferred mipgen). nouveau charges about 1.2 ms per call **regardless of
size**, so load time = number of textures × 1.2 ms. Measured offline on `out/<game>/fr3/*.fr3`:

| game | uploads | byte-identical unique | wasted |
|---|---|---|---|
| jak1 (26 lv) | 10070 | 3891 | **62%** |
| jak2 (148 lv) | 32506 | 14332 | **56%** |
| jak3 (274 lv) | 43916 | 19438 | **56%** |

For example, jak1 beach has 670 textures but only 257 are unique, so about 0.5 s of GL time per area
is wasted. Duplicates are also shared **across** neighbouring levels. jak2 examples: ctymarkb is 89%
identical to ctymarka, ctyslumb is 76% identical to ctysluma, ctyslumc is 60% identical to ctyslumb.
Jak1 neighbours share about 17–22%. The duplicates usually have **different combo_ids**, so matching
by combo_id finds almost nothing. They must be matched **by content**.

The extra GL calls are pure waste, and removing them has no quality cost.

### Design (Switch-only `#ifdef __SWITCH__`, all games; kill switch `sdmc:/gk_nodedup.txt`)
Add a content-keyed, refcounted registry in `game/graphics/opengl_renderer/loader/LoaderStages.cpp`.
It is used only on the render thread.

```
struct TexShareEntry { GLuint gl; std::vector<const tfrag3::Texture*> users; };
std::unordered_map<u64 /*hash*/, std::vector<TexShareEntry>> g_texshare;   // collisions -> vector
std::unordered_map<GLuint, u64> g_texshare_by_gl;
```
- **Hash:** 64-bit FNV-1a/mix over w, h, format, `data` (u32 words), `bcn_data` and `mip_offsets`.
  About 2–3 ms per level on the render thread is OK (it saves about 1.2 ms per duplicate). Moving the
  hash into the loader thread next to `decode_level_bcn_to_rgba` (Loader.cpp around line 1325) is
  optional.
- **Equality must be exact:** compare w, h, format, then `memcmp` of `data`, `bcn_data` and
  `mip_offsets` against `users.front()`. Never trust the hash alone.
- `texshare_acquire(tex)`: return the matching entry's gl and push `&tex` into `users`; otherwise
  return 0.
- `texshare_register(hash, gl, tex)` after a real `add_texture`.
- `texshare_release(gl, tex)`: remove `&tex` from `users`. Return true (the caller should delete the
  texture) only when `users` is empty; then erase both map entries. If gl is not in the registry
  (dedup disabled, or a common texture), return true. `users.front()` always stays a live pointer,
  because each level's `Texture` objects live until that level releases them.

**Hook 1: `TextureLoaderStage::run`** (LoaderStages.cpp around line 1050, Switch branch).
- Before calling `add_texture`, try `texshare_acquire`.
- On a hit, push the shared gl into `ld.textures` without any GL call:
  - Do **not** add to `bytes_this_run`, `tex_this_run`, `g_tex_uploaded` or
    `g_loader_gpu_submits_this_frame`.
  - Do not `mipq_defer` it again.
  - Do not apply the FIX 76e big-texture deferral to it.
  - If `tex.load_to_pool`, still call `pool.give_texture(...)` with the shared gl. Copy the
    `TextureInput` fill from `add_texture` (around lines 899–1005): `src_data` = `tex.data.data()`,
    or nullptr if `data` is empty. The pool lock is already held in that loop.
    `TexturePool::unload_texture` erases one `{gl}` entry per call, so duplicate entries are
    symmetric and safe.
- On a miss, call `add_texture` as today, then `texshare_register`.
- Log once per level:
  `[loader] FIX 103 tex dedup: N shared of M (saved ~X ms)`.

**Hook 2: `Loader::unload_level_gpu_objects`** (Loader.cpp around line 1652).
- Keep the pool-unload loop exactly as it is.
- Replace `for (auto tex : lev.textures) m_garbage_textures.push_back(tex);` with a loop over `i`:
  if `texshare_release(lev.textures[i], lev.level->textures[i])`, push the texture to garbage and to
  a local `freed` vector.
- Call `mipq_forget(freed)`, not `lev.textures`. Otherwise a still-shared texture would lose its
  pending mips.
- Guard the indices: a partially staged level has `lev.textures.size() <= level->textures.size()`.
- **Critical:** never push the same gl to `m_garbage_textures` twice. The FIX 99 time-boxed drain can
  interleave `glGenTextures`, which recycles names, so a double delete would destroy a live texture.

Everything else is unchanged:
- `flush_texture_garbage`, `reclaim_gpu_memory` and the garbage drain go through hook 2 already.
- The common level (`m_common_level`, around lines 1383 and 2469) stays **not** deduped.
- The desktop path stays unchanged.

### Build / deploy / verify
- Build sequentially, never in parallel (draco race), using the commands in the "Build" notes above:
  jak1 and jak2 mount `/work`, jak3 mounts `/src`.
- Deploy each game as `switch/jakN/jakN.nro` with `.f102.bak` rotation (jak3: `.f100.bak`), md5 checks,
  and a copy in `~/Desktop/jak bakcups/f103/`. Delete the `._jak*.nro` files.
- Hardware check in `gk_stdout.txt`:
  - The `[loader] tex stage: N textures, upload X ms` totals should drop by about 55–60%.
  - The new FIX 103 line should appear.
  - Texture-swap visual glitches would mean a broken memcmp/refcount; test `gk_nodedup.txt` to compare.
- Commit with "(AI-assisted)" and the Copilot co-author trailer.

### Offline measurement script (re-run to verify numbers)
fr3 = 8-byte LE uncompressed size + zstd. Level fields: u16 version (44), str (u64 len + bytes),
u64 n_textures. Per texture: u16 w, u16 h, u32 combo, vec<u32> data, str, str, u8 pool, u8 fmt,
vec<u8> bcn, vec<u32> mips. (`pip3 install zstandard`; skip `test-zone.fr3`, which is v43.)

### Next creative item after FIX 103 (slow motion)
`goal_src/<game>/engine/draw/drawable.gc`: `time-ratio` uses `float-time-ratio`, which is real
elapsed time (see SWITCH_PORT_SESSION_NOTES.md around lines 1720–1780, jak1). Check that jak2 and
jak3 got the same float-ratio fix and that the `fmin 4.0` cap is in place, so a dropped frame
advances game time instead of producing slow motion. This needs a GOAL CGO rebuild, not only the NRO.

### FIX 103 — REVISED SCOPE (supersedes the cross-level registry above) (AI-assisted)
The offline counts above (62% / 56% / 56%) were computed **per level file**, so they come entirely
from duplicates within a single level. Build the **intra-level** version first. It has no lifetime
risk because every copy is loaded and unloaded with its own level.
- In the Switch branch of `TextureLoaderStage::run`, keep a per-level map
  `hash -> index of first texture` (store it in `LevelData` and clear it when staging starts). On a
  verified match (hash plus memcmp), push `ld.textures[first]` and skip the GL call. Pool
  `give_texture` stays as described above.
- `unload_level_gpu_objects`: put the names in an `unordered_set` before pushing them to
  `m_garbage_textures` and `mipq_forget`, so each GL name is deleted exactly once.
- Safety net: before queueing a name for deletion, skip it and log
  `FIX 103 LEAK-GUARD` if any other entry in `m_loaded_tfrag3_levels` or `m_common_level.textures`
  holds it. The worst case becomes a leak, not a texture being deleted while still in use.
- No refcounts and no global registry. Cross-level sharing (jak2 city chunks, 60–89%) is a later
  step, only after the intra-level version is proven on hardware.

### FIX 103 — IMPLEMENTED (all 3 games) (AI-assisted)
**103a, in-level texture dedup.**
- `hash_level_textures()` runs on the loader thread right after the FIX 91 decode and stores
  `Texture::pc_dedup_hash`. This field is runtime-only and is not serialized.
- `TextureLoaderStage` keeps a `LevelData::tex_dedup` map (hash → first index). A verified
  byte-identical duplicate reuses that GL name: no GL call and no budget is charged, and it still gets
  `give_texture`.
- On unload, each GL name is queued for deletion exactly once (`unordered_set`).
- Log line: `[loader] FIX 103 tex dedup: N of M textures shared (~X ms saved)`.
- Kill switch: `sdmc:/gk_nodedup.txt`.

**103b, warm areas.** The f102 jak1 log showed beach being prefetched and wanted, then dropped by
the game (jak1 holds 2 levels). About 300 frames later the always-true `low_mem` evicted it, and
walking back cost a **13.96 s** visible re-stream, with 166 of the session's 216 hitches in that
window. `pick_eviction_victim` now keeps game-dropped levels that only `low_mem` would evict, as
long as their count is within `slots - prefetch_resident` (jak1 2, jak2/jak3 1). Peak residency
therefore never exceeds what FIX 76 prefetching already runs with. Behaviour falls back to
eviction in four cases:
- `at_cap` evicts as before;
- any `failed_allocations() > 0` restores the old rule;
- `reclaim_gpu_memory` can still take warm levels;
- the kill switch `sdmc:/gk_nowarm.txt`.

Deployed: jak1 17e490df54b699cfb39bcaf2364be1b7 (prev `Jak 1.f102.bak`), jak2
90173d52147a456bd0ef7dabd3388633 (prev `Jak 2.f102.bak`), jak3 d8e669453b3e588aa0b5e6bdc0b99242
(prev `Jak 3.f100.bak`). Desktop `f103/`.

To verify on hardware:
- The FIX 103 dedup lines appear, and `tex stage: N textures` drops to about the unique counts.
- Walking back to an area just left (beach ↔ village1 ↔ jungle) shows no `PC unloading` and no
  `ready in` re-stream.
- Watch for any `failed` > 0 in the `[loader] live=` line.
