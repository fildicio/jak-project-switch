# FIX 37 — agent brief #3: jak2 Switch, lock 30 fps + fix streaming (AI-assisted)

Baseline commit: `96b263193` (FIX 36). Hard constraints from
`SWITCH_FIX35_AGENT_BRIEF.md` §1 still apply in full (no runtime threads, no
texture-upload changes, Mac host first, `(AI-assisted)`, no issues/PRs).
Previous briefs: `SWITCH_FIX35_AGENT_BRIEF.md`, `SWITCH_FIX36_AGENT_BRIEF.md`.

## 0. User-reported state after FIX 36

- City frame rate **got worse**, zoomer **much worse**.
- Loading inside the city slightly better, **but**: leave the city and come back
  and **NPCs and zoomers disappear**, then the city takes a very long time to
  repopulate.
- **Jetboard tutorial is still ~10 fps in slow motion.**
- Goal: **locked 30 fps at 720p**, and it is acceptable to **drop resolution and
  visual quality while streaming/loading** if that makes areas load fast.

## 1. What the FIX 36 telemetry proves

The profiler now covers 327 buckets and accounts for 96 % of the dispatch — good
work, keep it. City, steady state:

```
[phase] buckets 31.27-37.04 | bucket-sum 26.92-27.30        <- was 29.5 before FIX 36
[fps]   12.8 avg | render 32.29 (max 119-228!) | wait_dma 9.01 | worst 129-245ms
[buckets] 27.30ms/frame (dispatch 28.32, 96% accounted, 327 buckets)
   [53] tie-l4-tfrag    3.30ms  draws  65.3  idx 24.0k
   [ 9] tie-l0-tfrag    3.24ms  draws 209.1  idx 58.4k
   [220] tex-lcom-pris  3.11ms  draws  16.0  idx  0.1k     <- TextureAnimator
   [20] tie-l1-tfrag    2.25ms  draws  78.1  idx 12.3k
   [ 4] tex-lcom-sky-pre 1.43ms draws  10.0                <- TextureAnimator
   [83] shrub-l1-shrub  1.25ms | [221] merc-lcom-pris 1.22ms (111 draws)
   [8]/[52] tfrag-l0/l4 1.17/1.15 | [291] tfrag-w-l4-alpha 1.15 | [110] shrub-l4 1.00
   frame totals: 669.3 draws/frame, 134.3k idx/frame
[tie]   tod 0.12ms | protovis 0.00 | vis/cull 0.00 | idx-build 0.00 | buf-upload 0.00 | draw 0.08ms (49 tree-renders)
[tfrag] tod 0.90ms | ... | draw 0.16ms     [shrub] tod 0.83ms | ... | draw 0.17ms
```

Four things jump out. **Fix them in this order.**

---

## 2. TASK 1 — the TIE sub-phase timers measure almost nothing; find the real 9 ms

The three TIE buckets cost **8.8 ms/frame**, but the `[tie]` sub-phases sum to
**~0.3 ms** (tod 0.12 + draw 0.08 + everything else 0.00). 209 draw calls
"costing" 0.08 ms is not credible. The instrumentation is in the wrong place or
is being reset wrongly — right now it is actively misleading.

Do exactly what worked for the bucket report: **make the unaccounted time
visible and bisect.**
- Time the whole of `Tie3::render()` (and TFragment/Shrub) and print
  `unaccounted = bucket_ms - sum(sub-phases)` in the `[tie]` line, plus the
  number of times the timed region was entered per frame. If `unaccounted` is
  ~8.5 ms, the cost is *outside* `render_tree` — candidates: the DMA-chain walk
  at the top of `render()`, `setup_for_level()` / texture-pool lookups, the
  per-draw `glBindTexture`/uniform/state block, `glBufferSubData` of the
  index buffer (see FIX 36 §3.1: unorphaned buffer writes block on Tegra), and
  the time-of-day / envmap palette work.
- Keep adding nested timers until **unaccounted < 10 %** of the bucket. Do not
  propose a fix before that number is small. This is all measurable on the Mac
  (proportions carry over; absolute ms do not).
- Once located: the likely wins are buffer orphaning / a 3-deep ring of index
  buffers, hoisting redundant GL state out of the per-draw loop, and caching the
  level/texture lookups.

## 3. TASK 2 — the two `tex-*` buckets are 4.5 ms/frame: throttle the TextureAnimator

`[220] tex-lcom-pris 3.11 ms` and `[ 4] tex-lcom-sky-pre 1.43 ms`, with 16 and 10
draws — that is `TextureAnimator.cpp` doing many small render-to-texture passes
and CPU-side conversions every frame. On Tegra each tiny pass costs a fixed
driver/GPU overhead, so this is nearly free to fix and worth ~4 ms:

- **Update animated textures at a reduced rate on Switch**: run each animation
  every 2nd frame (15 Hz at a 30 fps target), staggered so they do not all land
  on the same frame (`anim_index % 2 == frame % 2`). Most of these are slow
  scrolling/pulsing effects — visually indistinguishable.
- **Skip animations whose output texture was not sampled last frame** (off-screen
  levels, unused sky variants). The texture pool already knows what was bound.
- Gate both behind `#ifdef __SWITCH__` + a settings flag so it can be A/B'd, and
  log `[texanim] updated N/M this frame, X.XXms`.
- Expected: `tex-lcom-pris` + `tex-lcom-sky-pre` drop from 4.5 ms to ~2 ms.

## 4. TASK 3 — the adaptive loader is in a death spiral; NPCs/zoomers vanish because of it

From `gk_stdout.txt`:

```
[loader] budget ms=1.0 tex_kb=128 mode=struggle (ema 45.9ms)
[loader] budget ms=2.0 tex_kb=256 mode=lean    (ema 44.9ms)
[loader] live=7 init=0 want=5 | pool=21 bufs 75.8MB free, 164 out | budget struggle (ema 83.9ms)
```

The budget is driven by **frame time**. The game is at 13 fps, so the EMA is
always 35–85 ms, so the loader is permanently in `struggle` mode at **1 ms /
128 KB per frame** — at 13 fps that is **13 ms and 1.6 MB of loading per
second**. That is why the city takes forever to repopulate and why NPCs and the
zoomer are missing for a long time after re-entry: the actors' geometry/textures
are still queued behind a self-inflicted throttle. The logic is exactly
backwards: it throttles loading hardest precisely when there is most to load.

**Replace the frame-time-driven policy with a backlog-driven one:**

1. **Backlog wins over frame time.** Compute the pending work (levels where
   `want > live`, plus the size of the staging queue). If there is a backlog,
   the loader is in **catch-up** mode and gets a *large* budget (start at
   **8 ms / 2 MB per frame**, tune up). A few dropped frames while the world
   populates is exactly what the user wants; a 30-second wait is not.
2. **Frame time only modulates within catch-up**, it must never take the budget
   below a floor of ~4 ms / 1 MB while a backlog exists.
3. **Blackout / loading screens**: no throttle at all (12 ms / 4 MB+). Nothing
   is on screen to protect.
4. **Idle (no backlog)**: small budget, as now.
5. Log the decision: `[loader] mode=%s backlog=%d levels/%dKB budget ms=%.1f tex_kb=%d ema=%.1f`.

**Also fix the eviction that creates the backlog in the first place.**
`live=7 want=5` with `pool=21 bufs 75.8MB free`: levels are being dropped and
re-fetched while memory is available. In `Loader.cpp:pick_eviction_victim`,
evict on **real memory pressure**, not a 30-frame timer, and keep a most-recently
-used level resident for at least ~10 seconds (a player stepping outside and back
must hit a warm cache). Add `[loader] evict level=%s reason=%s free=%dMB` so the
next log answers this immediately.

**Acceptance:** leave the city, come back, and NPCs + zoomers are present within
a couple of seconds; `[loader] level %s ready in %.2fs` for a re-entry must be
well under the cold-boot time.

## 5. TASK 4 — "quality shed while streaming": drop resolution and effects *during* loads

This is the user's own idea and it is a good one, but note it is a **loading-time**
optimisation, not an fps fix (resolution changes have repeatedly shown no effect
on steady-state fps here — the port is CPU/driver bound).

Implement a single Switch-only `LoadBoost` mode, active whenever the loader is in
catch-up/blackout:
- render resolution → **960x540** (or 640x360 during full blackout loads);
- TextureAnimator → paused or 4 Hz;
- particles, shadows, envmap, depth-cue/slow-time post passes → off;
- draw distance / LOD → the lowest the settings allow;
- give the freed CPU/GPU time straight to the loader budget.

Restore the previous settings with a short fade/hysteresis (≥ 1 s) so it cannot
oscillate. Log `[boost] enter/exit (reason=..., res=WxH)`. Make sure the FBO
resize path is the existing settings-driven one
(`OpenGLRenderer.cpp` FBO setup / `game_res_w/h`) — **do not touch the swapchain**
(FIX 7f: resizing it fatalThrows).

## 6. TASK 5 — lock 30 fps with dynamic *quality* scaling

The game currently goes into slow motion because it misses the frame deadline and
the GOAL clock follows the real frame time. Add a Switch-only governor:

- Track an 8-frame moving average of total frame time.
- Define ~4 quality levels, shed in this order (cheapest visual loss first):
  **L1** particles/effects density ↓, TextureAnimator to 15 Hz;
  **L2** shadows simplified/off, envmap off, depth-cue/slow-time off;
  **L3** lower LOD and shorter draw distance (`lod-force-tfrag/tie`, `ps2-lod-dist?`);
  **L4** render resolution 960x540.
- Step **down one level** when the average exceeds ~31 ms for ~0.5 s, **up one
  level** only after ~3 s below ~26 ms. Never oscillate; log every transition
  `[gov] level %d -> %d (avg %.1fms)`.
- Expose it as a setting (`dynamic-quality?`) so it can be disabled for A/B tests.

Also investigate the **frame-time spikes** that are independent of the average:
`render max 119–245 ms` while the average is 32 ms. One 245 ms frame is a visible
freeze. Log what the spike frame was doing (biggest bucket that frame, loader
activity, memcard activity) — a per-frame "worst bucket" line printed only when a
frame exceeds 60 ms is enough and costs nothing in normal frames.

## 7. TASK 6 — pay back the instrumentation cost (perf regressed since FIX 35)

`[phase] buckets` went **29.5 ms → 31–37 ms** between FIX 35 and FIX 36, and the
user feels it. Audit the telemetry itself before blaming the game:
- `switch_bucket_prof_record()` takes `const std::string& name` and call sites
  pass `renderer->name_and_id()`, **which returns a `std::string` by value — that
  is one heap allocation per bucket per frame, ~327 of them every frame**, plus
  the existing `g_current_renderer = renderer->name_and_id()`. Cache the name
  once per renderer (store it in the renderer, or fill the table on the first
  frame) and pass a `const char*` afterwards.
- Make sure the sub-phase timers are not calling `Timer` more than a few dozen
  times per frame per renderer.
- Then **bisect the regression on hardware**: build `b76591023` (FIX 35) and
  `96b263193` (FIX 36) and compare `[phase] buckets` at the same save spot. If
  the gap survives the allocation fix, something else in FIX 36 (the Tie3 changes)
  is the cause and must be reworked.

## 8. TASK 7 — the jetboard tutorial, with data this time

It is still ~10 fps and we still have no bucket sample from that scene.
Add a **manual sample trigger** so the user can capture it without typing:
hold a button combo (e.g. L + R + Up, like the existing L3+R3+Minus diag toggle
in `opengl.cpp:825`) → log `[sample] START label=N`, force an immediate
`[buckets]` + `[tie]`/`[tfrag]`/`[shrub]` + `[texanim]` + `[loader]` dump every
2 s for 10 s, then `[sample] END`. Ask the user for three labelled samples:
quiet city, zoomer, jetboard tutorial with the caption up.

Suspects to check in that sample: `merc-*`/`emerc-*` buckets (the jetboard and
NPCs), `particles` (id 313), the subtitle/text path (many tiny DirectRenderer2
draws), the `tex-*` animator buckets, and the auto-save.

## 9. TASK 8 — prove the frame-sliced save works (still unverified)

`mc-trace.txt` still shows only `[MC] synchronous load took 48–62 ms` and no
sliced-save markers. On the Mac, trigger a real save, confirm the chunk markers,
confirm byte-identical bank files vs the synchronous path, then slice **loads**
too (48–62 ms is two lost frames, and loads are frequent).

---

## 10. Order, testing, definition of done

1. Task 7 (sample trigger) + Task 6 (instrumentation cost) + Task 3 (loader
   policy) + Task 2 (TextureAnimator) — all Mac-verifiable, ship as **one**
   console build with the three labelled samples requested.
2. Task 1 (find the real TIE cost) with the data from that build, then the fix.
3. Tasks 4/5 (LoadBoost + governor) last, since they are compensation for
   whatever cannot be optimised away.
4. Mac loop: `cmake --build build-host-fix33 --target gk -j 8` then
   `./build-host-fix33/game/gk --game jak2 -- -boot -fakeiso -debug`.
   Switch build/deploy/verify: FIX 35 brief §8; back up current NROs to
   `backups/pre-fix37/`; confirm a new md5 on the card after `sync`.
5. Every change documented in `SWITCH_PORT_SESSION_NOTES.md` with before/after.

**Done means:** city and zoomer at a locked 30 fps (worst frame < 60 ms), no
slow motion in the jetboard tutorial, NPCs/zoomers back within ~2 s of re-entering
the city, `[phase] buckets` ≤ 25 ms, and jak1 still boots.
