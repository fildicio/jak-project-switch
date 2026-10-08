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
