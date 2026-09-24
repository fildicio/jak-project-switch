# FIX 36 — agent brief #2: jak2 Switch, after FIX 35 Tasks 1+3 (AI-assisted)

Read this together with `SWITCH_FIX35_AGENT_BRIEF.md` (all hard constraints in
its §1 still apply — no runtime threads, no texture-upload changes, Mac host
first, `(AI-assisted)` on every commit, no issues/PRs).
Baseline commit: `b76591023`.

---

## 0. What the user reports after the FIX 35 build

1. **Zoomer frame rate got *worse* than before.**
2. **Loading inside the city got better**, BUT **leaving the city and coming back
   takes much longer** to load the city + its inhabitants + the zoomer.
3. **Jetboard tutorial (stadium), while the caption is on screen: ~10 fps,
   heavy slow motion.** Changing resolution still changes nothing.

## 1. What the new telemetry actually says (read this before coding)

From `sdmc:/switch/jak2/gk_run_log.txt`, city, steady state:

```
[fps]   12.8 avg (42.65ms) | wait_dma 9.90/40.00 render 29.74/69.19 swap 1.23 | worst 80.8ms | starved=23
[phase] setup 0.01 | loader 0.01 | buckets 29.51 | blit 0.00 | pcrtc 0.05
[buckets] 28 frames, all buckets 18.26ms/frame -- worst:
   [ 9] tie-l0-tfrag      avg 3.32ms  max 7.42ms (18.2%)  draws 0.0/frame  idx 0.0k/frame
   [31] tie-l2-tfrag      avg 3.29ms  max 4.24ms (18.0%)  draws 0.0/frame  idx 0.0k/frame
   [20] tie-l1-tfrag      avg 2.11ms  max 2.56ms (11.5%)  draws 0.0/frame  idx 0.0k/frame
   [ 4] tex-lcom-sky-pre  avg 1.70ms  max 6.99ms ( 9.3%)  draws 10.0/frame
   [30] tfrag-l2-tfrag    avg 1.34ms  ...  [ 8] tfrag-l0-tfrag 1.32ms
   [83] shrub-l1-shrub    avg 1.00ms  ...  [74] shrub-l0-shrub 0.90ms
[buckets] frame totals: 293.9 draws/frame, 29.3k idx/frame
```

Three conclusions, all of which drive the tasks below:

**(A) The profiler is blind to more than a third of the frame.**
`[phase] buckets = 29.5 ms` but `[buckets] all buckets = 18.26 ms`. The missing
**~11 ms** is not mysterious: `g_bucket_prof` is `std::array<SwitchBucketProf, 128>`
(`OpenGLRenderer.cpp:93`) and `switch_bucket_prof_record()` silently `return`s for
`bucket_id >= 128` (line ~127). **jak2 has ~326 buckets** (`buckets.h`, jak2 enum
ends `BUCKET_323, DEBUG2, DEBUG_NO_ZBUF2, DEBUG3, MAX_BUCKETS`) **and jak3 has 587**
(`buckets.h:1071`). Everything above 128 — **merc/emerc (all characters), sprites,
particles, HUD, subtitles, screen filter** — is unmeasured. Those are exactly the
things that appear when a tutorial caption + jetboard + NPCs are on screen, which
is very likely why the jetboard tutorial collapses to 10 fps with no visible cause.

**(B) The renderer is not GPU bound and not even draw-call bound — it is CPU bound
inside the bucket renderers.** 294 draws and 29.3k indices per frame is *nothing*
(~10k triangles). And the three TIE buckets burn **8.7 ms/frame between them while
issuing literally zero draw calls** (`draws 0.0/frame`). Time with no draws = pure
CPU: DMA-chain walking, visibility/culling loops, index-buffer building, and
buffer uploads. That, plus (A), is the real 29.5 ms.

**(C) The loader budget explains the slow return to the city.** In
`LoaderStages.cpp` the Switch limits are `LOAD_BUDGET 2 ms` and
`MAX_TEX_BYTES_PER_FRAME 256 KB` **per frame**. At the current 13 fps that is
~26 ms of loading work and ~3.3 MB of texture upload **per second**. A city level
is tens of MB, so a re-entry that has to re-upload everything takes tens of
seconds. FIX 33 also evicts aggressively (8-level cap, 30-frame retirement in
`Loader.cpp:pick_eviction_victim`), so leaving the city throws the data away and
coming back pays full price. `[loader] live=5 init=0 want=5 | pool=39 bufs
208.8MB free, 98 out` confirms levels are being dropped and the pool is sitting
on 208 MB of free buffers.

Also note: `mc-trace.txt` contains **no evidence that a sliced save ever ran** —
this session only performed loads (`[MC] synchronous load took 48-62 ms`). The
`[MC] starting async memcard worker...` line in that file is old (the trace is
append-only). **Task 3 of FIX 35 is unverified**, see Task 5.

---

## 2. TASK 1 (30 min, do first) — stop measuring only half the frame

`game/graphics/opengl_renderer/OpenGLRenderer.cpp`:
- Size the profiler from the actual bucket count instead of a magic 128: make
  `g_bucket_prof` a `std::vector<SwitchBucketProf>` sized in
  `switch_bucket_prof_begin_frame()` (or on first record) to
  `m_bucket_renderers.size()`, or simply size the array to 600 so jak1/2/3 all
  fit. **A silent `return` for out-of-range ids must never happen again** — add a
  one-shot log line `[buckets] WARNING: id %d out of range (cap %d)` if it does.
- Keep printing the top 8, but also print a line for **every bucket above
  0.30 ms/frame** (up to ~20 lines) so nothing hides in the tail.
- Print the **sum check** in the report: `all buckets X ms/frame` alongside the
  `[phase] buckets` value from the same interval, so the two can be compared at a
  glance. They must now agree within ~1 ms; if they do not, the remaining gap is
  in `dispatch_buckets_jak2` outside `renderer->render()` (the
  `vif_interrupt_callback` / per-bucket bookkeeping) and must be timed too.

**Then capture, on hardware, three 10-second samples: (a) quiet city street,
(b) riding the zoomer, (c) the stadium jetboard tutorial with the caption up.**
Label them in the log (e.g. bump a counter on a button combo, or just note the
timestamps). This single run gives the real ranking. Everything after this is
chosen by that ranking — do not guess.

## 3. TASK 2 — find out what the TIE/TFRAG/shrub buckets do with 8.7 ms and 0 draws

`background/Tie3.cpp`, `background/TFragment.cpp`, `background/Shrub.cpp`.
Add Switch-only sub-phase timers inside `Tie3::render` / `render_tree` (and the
equivalents) and report them in the `[buckets]` block:

```
[tie] dma-parse X.XXms | vis/cull X.XXms | idx-build X.XXms | buf-upload X.XXms | draw X.XXms
```

Specific suspects, in order:
1. **Per-frame index buffer rebuild + upload.** These renderers build an index
   buffer on the CPU every frame from visibility data and push it with
   `glBufferSubData`/`glBufferData`/map. On Tegra an unorphaned
   `glBufferSubData` into a buffer the GPU may still be reading **blocks until
   the GPU is done** — that would show as big CPU time with no draws, exactly
   what we see. Fix by buffer orphaning (`glBufferData(..., nullptr, ...)` before
   the write) or an N-frame ring of index buffers (N=3). This is a classic and
   would be the single biggest win if confirmed.
2. **`use-vis? #f`** is set in the card's `pc-settings.gc`. With vis off, the
   culling path walks/emits everything. Find why it is off for this port (grep
   the notes and `goal_src` for `use-vis`), and test it on. If vis data is
   genuinely broken here, that is a bug worth fixing on its own.
3. **DMA chain walking.** If `dma-parse` dominates, the cost is in the interpreter
   loop over the PS2 chain; look for per-qword work that can be hoisted.

Do all of this on the Mac first — the sub-phase *proportions* and the draw/index
counts are identical there; only absolute ms differ.

## 4. TASK 3 — fix the "coming back to the city is very slow" regression

`LoaderStages.cpp` (constants at the top) and `Loader.cpp`.

1. **Make the budget adaptive instead of a flat 2 ms / 256 KB.** During a
   *blackout / loading screen* (no gameplay on screen, the game is already
   stalled — `Loader::update_blocking` path) there is nothing to protect: use a
   large budget (e.g. **12 ms and 4 MB/frame**). During *gameplay streaming*
   keep a small budget, but scale it from the measured frame time: if the last
   8 frames averaged under budget, allow more (up to ~4 ms / 1 MB); if we are
   already missing 30 fps, stay at 2 ms / 256 KB. Log
   `[loader] budget ms=%.1f tex_kb=%d mode=%s` when it changes.
2. **Stop throwing away levels that are about to be needed again.** In
   `pick_eviction_victim` (`Loader.cpp:~437`) the Switch rules are a 30-frame
   retirement and an 8-level cap. Raise the retirement to several seconds'
   worth of frames and only evict when actually short of memory — the pool is
   reporting **208.8 MB free** while we evict, which is absurd. Drive eviction
   from the real GPU/heap pressure figure, not a frame counter.
3. **Do not re-decode/re-upload textures that are still resident.** Confirm from
   `[loader] live=/init=/want=` and the pool stats whether a re-entry is
   re-uploading data that was never freed; if so, key the cache by level name so
   a returning level is a no-op.
4. Verify by timing: log `[loader] level %s ready in %.2fs` for every level load,
   and compare city entry from a cold boot vs city re-entry after leaving. The
   re-entry must not be slower than the cold entry.

## 5. TASK 4 — the jetboard tutorial (the #1 user complaint)

Do **not** assume it is the memory card. With Task 1 done, the tutorial sample
will name the buckets responsible (merc/emerc/sprite/particle are all above id
128 and therefore invisible today). Likely contributors, to be confirmed by the
data:
- the caption/subtitle and hint-text path (`SUBTITLE`, `SCREEN_FILTER` buckets,
  text rendering through DirectRenderer2 — many tiny draws);
- particles/sprites from the jetboard;
- the auto-save that fires with the hint (Task 5);
- the slow-time / depth-cue post effects (`SlowTimeEffect.cpp`, `DepthCue.cpp`) —
  these are full-screen passes and are cheap to switch off for a test.

Deliver a fix for whatever the report indicts, and a `[tutorial]`-labelled
before/after measurement in the notes.

## 6. TASK 5 — prove the frame-sliced save actually works

There is currently **zero evidence** it ever ran. Add unmissable markers to
`mc-trace.txt`: `[MC] sliced save begin file=%d bank=%d bytes=%d`,
`[MC] sliced save chunk %d/%d %.2fms`, `[MC] sliced save done total %.2fms over %d frames`,
and `[MC] save skipped (unchanged)`. Then, on the Mac, trigger a real in-game save
(a tutorial hint or the save menu) and confirm:
- the chunk lines appear, each a few ms;
- the resulting `bank0.bin`/`bank1.bin` are byte-identical to the synchronous
  implementation's output for the same save;
- loading them works, and GOAL never gets stuck on BUSY.

Only then re-check on console. Loads are also worth slicing: `[MC] synchronous
load took 48-62 ms` is two lost frames each time, and loads happen repeatedly.

## 7. TASK 6 — the other ~10 ms: `wait_dma`

`wait_dma` is now ~10 ms (down from 18, good) but still the GOAL simulation. After
the render side is under control, profile the GOAL side in the same three
situations (see FIX 35 brief §6). Target: `render` ≤ 25 ms and `wait_dma` ≤ 8 ms
on the same frame, which is 30 fps with headroom.

---

## 8. Order of work and testing discipline

1. Task 1 (telemetry) — **Mac verify, then one console run producing the three
   labelled samples.** Everything else is chosen from that run.
2. Task 3 (loader) and Task 5 (save markers) can be developed and fully verified
   on the Mac in parallel and shipped in the **same** console build as the Task 2
   fix.
3. Never ship a build that has not booted and played on the Mac
   (`cmake --build build-host-fix33 --target gk -j 8` then
   `./build-host-fix33/game/gk --game jak2 -- -boot -fakeiso -debug` — verified
   working).
4. Switch build/deploy/verify commands: FIX 35 brief §8. Back up the current
   NROs to `backups/pre-fix36/` first, and confirm a new md5 on the card after
   `sync`.
5. Document every measurement in `SWITCH_PORT_SESSION_NOTES.md`.

## 9. Definition of done

- `[buckets]` accounts for ≥ 95 % of `[phase] buckets`, for all three games.
- City re-entry is no slower than the first entry, measured with
  `[loader] level ... ready in`.
- Jetboard tutorial: no sustained sub-20 fps, no slow motion.
- Zoomer: at least back to the pre-FIX-35 frame rate, target ≥ 25 fps.
- jak1 still boots and plays.
