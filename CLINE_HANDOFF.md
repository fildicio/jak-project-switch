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
