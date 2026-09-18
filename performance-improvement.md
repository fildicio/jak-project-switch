# performance-improvement.md — Switch port roadmap

**Created:** 2026-09-18, after FIX 29/30 landed on `main` (`cbb01140e`).
**Companion to:** `SWITCH_PORT_SESSION_NOTES.md` (the FIX 1–30 history and evidence log).

Goal of this file: the single ordered roadmap to make the port (a) genuinely fast on Switch,
(b) ready to run **Jak II** the same way it runs Jak 1, and (c) "almost perfect" — stable,
honestly menued, cleanly released. Every item is self-contained: hypothesis, files, steps,
verification, expected gain, risk. Work them **one at a time**, like FIX 1–30.

## Ground rules (from AGENTS.md + what FIX 1–30 taught us)

- One item per session/commit. Each landed item gets a new `## FIX n` section in
  `SWITCH_PORT_SESSION_NOTES.md` with evidence.
- All commits carry `(AI-assisted)`. Never open issues or PRs.
- **No overclock.** Code-only fixes — also justified by FIX 12: the bottleneck is CPU-side,
  overclocking measurably changed nothing.
- Every performance claim needs a `[fps]`/`[buckets]` measurement **before and after**
  (cheat-sheet at the bottom).
- Keep diffs surgical; clang-format 18 (the version CI pins) the touched C++ files and
  check `git diff` for unrelated churn before committing.

## Where we are — the measured baseline

All numbers below are **pre-FIX-29/30** telemetry from real hardware (`gk_run_log.txt`),
recorded in the session notes (FIX 11/12, "Measurements from the 30fps run"):

| Signal | Value (Sandover area) | Meaning |
|---|---|---|
| `render` phase | 45–67 ms | **CPU-side GL command submission** — the whole bottleneck (FIX 12) |
| `buckets` total | ~22.5 ms avg | worst: `l1-tfrag-tie` 12 ms avg (34–40%) |
| `setup` phase | 31.68 ms | render setup before buckets |
| shared max stall | ~40 ms max | one stall attributed to whichever bucket was running — **never root-caused** |
| `swap` phase | 0.3 ms, always | GPU/present is **never** the limit |
| `wait_dma` | ~0 ms | the GOAL engine is not the limit |
| overclocking | no change | confirms CPU-bound |
| 30 fps + vsync (`si=2`) | perfect 33.33 ms lock, `vblanks 2x=57-60` | pacing solved (FIX 22/24) |
| memory | flat 3,261,488 KB | no leak |

**Landed since:** FIX 29 (uniform-lookup cache, texture-bind caching, `GL_MAX_SAMPLES`
cached once at init) and FIX 30 (game-size actually honored → handheld renders the chosen
720p instead of forced 1080p, ~44% of the previous fragment workload; honest per-device
MSAA carousel; Display Mode/Monitor hidden on Switch).

**Unknown:** nobody has re-measured after FIX 29/30. That is item A0 and it gates everything.

---

## Track A — performance (render/CPU path)

### A0 — Re-baseline on hardware *(do first; gates the whole track)*

- **What:** run the existing telemetry with the current NRO (`0cb12b6b…`) in the standard
  test loop (Sandover incl. looking down from high ground — the recorded worst case —
  Sentinel Beach, Misty; ~110 s sustained per spot).
- **Decide from the numbers:**
  - `render` avg ≤ ~20 ms → 30 fps is comfortable everywhere; aim remaining work at 60 fps.
  - `render` avg still 30 ms+ → A2 (submission cost) stays the priority.
  - bucket maxima still ~40 ms while averages are low → A1 (the stall) first.
- **Verify:** log lines captured into the session notes as the new reference baseline.
- **Risk:** none. Purely measurement.
- **✅ DONE 2026-09-18** (see session notes "A0 — post-FIX-29/30 baseline"): ~7 h organic
  play, NRO md5 re-verified. Steady-30: fps 29.4 avg, render p50 **30.7 / p95 35.8 ms**,
  bucket totals avg 22.5 / p95 35.2 ms, locked in 71.5% of windows; 60 fps holds only in
  light areas (render 15.3 ms there). **Verdict: A2 priority** (render ≥ 30 ms; buckets'
  own ~22.5 ms avg is now the floor), **A1 confirmed second** (1.86% of windows have a
  >100 ms frame; bucket spikes to 70–180 ms inside otherwise-steady windows).

### A1 — Root-cause the ~40 ms stall

- **Hypothesis:** a first-use cost hit mid-game, shared across buckets (the notes'
  averages-vs-maxima discrepancy): lazy shader-program compilation on first view of a
  material, or on-demand texture upload in the streaming texture system.
- **Steps:**
  1. Instrument: per-bucket GPU fence / `glFinish()` timing behind a debug flag, or check
     `GL_EXT_disjoint_timer_query` in the switch-mesa context; log any bucket with
     max > 20 ms.
  2. If shader compilation: pre-warm — compile all tfrag/tie/shrub/hfrag program variants
     for the loaded levels during the loading screen instead of on first draw (FIX 29's
     per-ShaderId uniform cache already gives a natural place to enumerate variants).
  3. If texture upload: force full upload at texture creation rather than streamed mips.
- **Files:** `game/graphics/opengl_renderer/Shader.h/.cpp`, `background_common.cpp`, the
  texture streaming/pool code.
- **Expected:** bucket maxima collapse toward averages → fewer long-frame hitches.
- **Risk:** slightly longer loading screens; a few MB more memory for pre-warmed programs.

### A2 — Keep cutting CPU-side submission (the FIX 12 bottleneck)

FIX 29 removed the *lookups*; these remove remaining per-draw CPU work, in expected-gain
order. Re-measure (A0 loop) after each sub-item:

- **A2a — draw-mode state cache:** `setup_opengl_from_draw_mode()` re-issues
  `glTexParameteri` etc. every draw. Cache GL state per (draw-mode, clamp, sRGB) key and
  skip redundant calls.
  *✅ implemented 2026-09-18 (pending on-hardware measurement): state mirror in
  `background_common.cpp` — global part (depth test/func, blend eq/funcs/color, depth
  mask) keyed by relevant DrawMode bits; sampler part keyed by clamp/filt/mipmap +
  caller-reported `texture_rebound` (params live on the texture object, so a rebind
  forces re-apply). `reset_draw_mode_state_cache()` at the start of every pass that uses
  the function (tfrag tree, tie base/envmap/wind, shrub, hfrag, merc `do_draws`, sprite
  `flush_sprites`, sprite distort) because other renderers mutate the same global state
  between buckets. AFAIL double-draw paths got a `glDepthMask(GL_TRUE)` restore (AFAIL
  implies depth-write-on, so this is the state setup applied — keeps the mirror valid).
  Bonus: Tie3 wind + Shrub AFAIL paths still did per-draw `glGetUniformLocation` (missed
  by FIX 29) — now routed through the uniform cache. Host build clean; NRO built.*
  *❌ measured 2026-09-19 (~4 min village session, NRO 33138483): NO gain. Village-
  fingerprint render p50 32.0 ms (n=10) vs 32.4 baseline (n=486); whole-session p50 31.9
  vs mixed-baseline 30.7. Skipping cheap fixed-function state calls saved ~0.4 ms —
  within noise. Kept (correct + free), but the lesson stands: the remaining ~24 ms of
  buckets is real per-draw submission/driver/GPU cost, not redundant state calls.*
- **A2b — hoist uniform uploads:** FIX 29 cached *locations*, not *uploads*. Matrices and
  constant colors are re-uploaded per draw in `first_tfrag_draw_setup` / Tie3 / Shrub
  paths; hoist to once per tree/pass where provably constant.
- **A2c — generic GL state cache:** track bound program/VAO/FBO/blend/depth-test state to
  skip redundant binds in merged buckets (`l0-tfrag-tie` etc.).
- **A2d — multidraw audit:** `no_multidraw` is false on Switch — verify what that maps to
  in the GLES 3.1 backend (real multi-draw vs emulated loop) and whether merging can
  extend across trees in the same bucket when adjacency allows.
- **Files:** `background_common.cpp`, `TFragment.cpp`, `Tie3.cpp`, `Shrub.cpp`,
  `OpenGLRenderer.cpp`, `game/graphics/pipelines/opengl.cpp`.
- **Expected:** this is the class of work that moves the `render` phase; each sub-item
  should show in its average.
- **Risk:** state-cache bugs render wrong (classic) — test Sandover + Sentinel + one
  indoor area; the FIX 29 AFAIL double-draw paths are the regression canary.

### A3 — Move `update-to-os` kernel calls out of the per-frame path (GOAL)

- **What:** `goal_src/jak1/pc/pckernel-common.gc:114-134` runs **every frame** and issues
  several `pc-set-*` kernel calls (an upstream TODO acknowledges this).
- **Steps:** keep the per-frame *check* (FIX 24/30 resolution/vsync application depends on
  it) but cache last-sent values in GOAL and skip kernel calls when unchanged.
- **Expected:** small (µs–ms) but free; removes syscall churn from the GOAL main loop.
- **Risk:** settings changes delayed one frame — exercise every options-menu toggle.

### A4 — Out-of-box defaults polish

- Fresh installs: 720p default is done (FIX 30b). Remaining: default **30 fps + vsync on**
  for Switch fresh installs (GOAL `reset-graphics` defaults in `progress-pc.gc`), matching
  what the README already recommends.
- **Risk:** 60 fps enthusiasts change one setting; note it in release notes.

### A5 — Explicitly not doing

- **Overclocking** — policy, and FIX 12 proved it useless (CPU-bound).
- **Sub-720p internal resolution** — quality floor; users can already pick lower.
- **Async GPU / Vulkan rewrite** — nouveau user-space can't; out of scope.

---

## Track B — Jak II readiness

**Facts established 2026-09-18:** this fork already carries `goal_src/jak2` (38 MB of
decompiled source), `game/kernel/jak2`, `task set-game-jak2`, and the extractor supports
`--game jak2`. The README says Jak 2/3 are "not supported on the Switch target" while
desktop "behaves like upstream". So this is a Switch-target plumbing + porting job, not a
new port. Jak 1 hardcoding found so far: `scripts/package-switch.sh` (dirs + `switch/jak1`
app folder), the per-game kernel `game/kernel/jak1/fileio.cpp` (`out/jak1/obj/...` — jak2
has its own equivalent), the crash symbolization in `game/switch/platform.cpp` (explicitly
jak1-layout-only), and `scripts/build-switch.sh` (no game parameter at all).

### B1 — Desktop gate: prove Jak 2 runs in this fork on PC

- **What:** `task set-game-jak2`, extract + compile your own Jak 2 disc, boot and play
  ~10 min on desktop. This proves the fork's upstream base is recent enough before any
  Switch work; if it fails, merge upstream first and re-run.
- **Verify:** game boots; write down every failing system — that list feeds B6 directly.
- **Risk:** none to the Switch port (separate data/tree); worst case it surfaces how far
  behind upstream we are.

### B2 — Parameterize the NRO build by game

- **What:** `scripts/build-switch.sh` always builds the default (jak1). Find the CMake
  game-select mechanism (`scripts/tasks/update-env.py` writes `.env`; CMake consumes it),
  make the script accept `GAME=jak2`, and make the `gk_nro` target compile/link
  `game/kernel/jak2` with jak2's fileio (`out/jak2/obj/...`).
- **Decision:** **one NRO per game in its own app dir** (`switch/jak1/`, `switch/jak2/`)
  rather than a game-picker NRO — matches the existing layout, keeps memory budgets
  separate, avoids a boot menu.
- **Verify:** docker build produces a jak2 NRO that reaches the GOAL boot banner.
- **Files:** `scripts/build-switch.sh`, `cmake/toolchains/Switch.cmake` / `CMakeLists.txt`
  as needed.

### B3 — Parametrize packaging

- **What:** `scripts/package-switch.sh` takes a game argument (default `jak1`):
  `ISO_DIR=iso_data/$GAME`, `APP=$OUT/switch/$GAME`, require `out/$GAME/{iso,obj}`,
  per-game README text. Settings/saves live under `OpenGOAL/$GAME` per the existing
  layout — verify kmachine derives them from the game version.
- **Verify:** fresh full package for **both** games; both boot from the same SD card with
  independent saves/settings.

### B4 — Extend crash symbolization to Jak 2

- **What:** FIX 27/28 (`game/switch/platform.cpp` link-base ring + in-process GOAL symbol
  walk, `scripts/analyze-goal-crash.py`) hardcode jak1's symbol-table offsets
  (`goal_constants.h`; the code comments say jak2/3/x use different parallel name tables).
- **Steps:** add a per-game layout table keyed by the game the NRO was built for; teach
  the analyzer a `--game jak2` flag.
- **Verify:** force a crash in jak2 on hardware; symbolized names must be sane
  (the standard B-series verification loop).

### B5 — Port the FIX 29/30 GOAL lessons into `goal_src/jak2/pc/`

- **What:** jak2 has its **own** PC/options code (`pckernel.gc`, `pckernel-impl.gc`,
  `progress/`). The FIX 30 bug (windowed display-mode forcing the window size as game
  resolution every frame via `update-to-os`) almost certainly exists there too.
- **Steps:** port, in order: (1) the game-size honoring fix (`pc-get-os` Switch check),
  (2) the `pc-get-max-msaa` kernel extern + per-device MSAA carousel + stale-setting
  coercion, (3) hide Display Mode/Monitor on Switch. The C++ kernels
  (`pc_get_max_msaa`, no-op `pc_set_display_mode`) are already game-agnostic.
- **Verify:** ARM64 compile of all jak2 targets (`extractor … --game jak2
  --instruction-set arm64`); menu behavior on hardware.
- **Note:** jak2's resolution/MSAA defaults may differ — audit before copying numbers.

### B6 — GLES 3.1 audit of Jak 2 renderer features

- **What:** enumerate jak2-only shader programs and draw paths (new tfrag versions,
  weather/effects) and confirm they compile and run in the ES 3.1 switch-mesa context —
  no geometry/tessellation shaders, check half-float and MRT usage. Stub or replace
  whatever fails.
- **Input:** the failing-systems list from B1.
- **Verify:** boot to in-game on hardware; zero shader-compile errors in the gfx log.

### B7 — Memory budget under full-RAM takeover

- **What:** jak2's GOAL heap is larger than jak1's. Watch the `Memory: NKB` telemetry
  through the first Haven City load and a 30-min soak. If tight, shrink GPU-side caches
  (texture pool) before touching GOAL heap sizes.
- **Verify:** memory flat, no OOM crash (jak1 reference: flat 3.2 GB for the whole run).

### B8 — Performance gate (requires A0–A2 done)

- **What:** measure Haven City (jak2's heaviest early area) with the standard loop.
  Target: locked 30 fps at 720p handheld. Treat jak2 numbers as the port's new worst
  case and spin further Track A items if needed.

### B9 — Docs + release for Jak 2

- **What:** `docs/setup/system/switch.md` gains a jak2 section (disc compatibility,
  separate SD folder); README flips "Jak 2 not supported" to experimental; release v0.3.0
  carrying **two** NRO assets. Extractor tarballs are already game-agnostic. Reuse the
  v0.2.3 coupling-warning pattern: NRO and recompiled data must update together.

---

## Track C — stability & polish ("almost perfect")

### C1 — The two reproducible crash moments

- **What:** the parked FIX 25–28 triage (session notes, "user intel: crashes are
  REPRODUCIBLE at two known moments"). The single biggest remaining "perfection" item.
- **Steps:** reproduce with the existing tooling (`gk_fatal.txt` + FIX 27/28 symbolized
  dumps + `scripts/analyze-goal-crash.py`); fix root cause; keep the crash handler
  improvements as-is. These are the last known Jak 1 crashes — everything else in the
  port has been addressed or measured.
- **Note:** can be worked any time crash logs are in hand; independent of A/B.

### C2 — Cut release v0.2.3

- **What:** ship FIX 29/30. The full, verified procedure is already written in
  `SWITCH_PORT_SESSION_NOTES.md` ("TODO — cut release v0.2.3", ~line 2559): tag → CI
  extractors → attach NRO `0cb12b6b…` → coupling warning + update path in the notes.
- **Why early:** users are stuck on forced-1080p until this ships; also gives public
  feedback on the perf work before investing further.

### C3 — Build the NRO in CI

- **What:** add a devkitPro docker job to `.github/workflows/extractor-release.yml` so
  `gk.nro` is built on tag push instead of hand-attached — releases become reproducible
  and auditable (the v0.2.2 NRO could not be verified against its tag; don't repeat that).
- **Verify:** a dry-run tag produces a byte-comparable NRO; md5 recorded in the release
  notes.

---

## Do them in this order (1-by-1)

1. **A0** re-baseline — every later decision depends on these numbers.
   *✅ done 2026-09-18 — verdict: A2 first, then A1 (see session notes).*
2. **C2** cut v0.2.3 — ship the measured win.
3. **A2a ✅ measured 2026-09-19: no gain** (village p50 32.0 vs 32.4 baseline; session
   p50 31.9). Lesson: skipping cheap state calls moves ~0. Before any more A2 CPU work,
   run the **CPU/GPU attribution test** (zero code): same village stretch ~2 min each at
   640x360 vs 720p via the options menu, then compare render avg. Scales with pixels →
   GPU-bound, pivot off the A2 CPU track (overdraw/resolution levers instead). Flat →
   CPU-bound, continue with **A2d** (multidraw audit — biggest remaining CPU lever)
   before A2b/A2c.
   *⚠️ 2026-09-19 update 2 (supersedes the first VOID note): the re-test after the FIX 31
   deploy was **VOID again, and the cause was the deploy itself** — the console launches
   the NRO at the **SD card root** (`sdmc:/gk.nro`), not `sdmc:/switch/jak1/gk.nro` where
   FIX 31 had been copied. Root still held the Sep-17 A2a build (`33138483…`). Proof: the
   02:11 session has zero `[disp] pc_set_game_resolution` lines even though FIX-30 GOAL
   data (deployed to `data/goal_src` at 01:06) applies 1280x720 every frame — `game_res_w`
   C-side defaults to 640, so the FIX 31 NRO would have logged `(was 640x480)` at boot.
   Zero lines ⇒ old NRO ran. No menu events at all ⇒ the res never changed during the
   session (pinned 1280x720); the felt "low res smooth / 720p drops" difference was
   view/area variance. Deploy corrected: FIX 31 (`9be0e8b9…`) now at BOTH root and
   `switch/jak1`, rollback `gk.nro.pre-fix31` (= old root, `33138483…`). **Next boot must
   show `[disp] pc_set_game_resolution -> 1280x720 (was 640x480)` near t≈14 s or it is the
   wrong NRO again — that line is the build-identity check.**
   *Incidental real data from the void session (constant 720p): far-terrain/ocean views
   saturate the GPU — fps 23–26, `[cam]` hitches 45–57 ms, `swap` (SwapWindow block)
   9–14 ms; buckets: l1-tfrag-tie 4.4 ms, sky 3.0, l1-alpha-sky-blend-and-tfrag-trans
   2.6, l1-tfrag-tfrag 2.5, sprite 1.9, ocean-mid-far 1.5. Classic fill-bound profile —
   strengthens the GPU-bound prior for step 3 even before the clean A/B.*
4. **A1** the stall — now the *stutter* item: A2a session shows worst-frame avg 54.1 ms
   (exactly one missed vblank slot, 2x→3x) in 61.8% of steady windows while render avg
   ~32 ms = 96% of the 33.3 ms budget — a zero-headroom cliff — plus 60–95 ms spike
   frames and one ~1 s loader hitch. Fix = real headroom (step 3 fork) and/or spike
   root-cause via the fence/disjoint-timer instrumentation below.
5. **B1** desktop jak2 gate — cheap, and its findings scope B6.
6. **B2** NRO by game → **B3** packaging → **B5** GOAL pc-options port.
7. **B4** crash symbolization for jak2.
8. **B6** GLES audit → **B7** memory → **B8** perf gate.
9. **A3** + **A4** GOAL-side polish (can slot in anywhere after step 4). *A3 is NOT a
   stutter fix — measured `wait_dma` 0–1 ms means the GOAL loop has slack; its own
   expected gain is µs–ms.*
10. **B9** v0.3.0 release with both games.
11. **C1** crash moments — start earlier if a crash log arrives; **C3** CI NRO — whenever
    convenient, ideally before v0.3.0.

Rule of thumb: finish each step with the FIX-notes entry + (for A-items) before/after
telemetry, so the roadmap and the evidence log never drift apart.

---

## Measurement cheat-sheet

- **Telemetry already in the build** (no code changes needed):
  - `[fps] N avg (Xms) | wait_dma a/m render a/m swap a/m | vblanks …` — phase costs.
  - `[buckets] … worst:` — eight most expensive buckets, every 2 s (FIX 12).
  - Boot log prints `GL_MAX_SAMPLES: N` (FIX 29).
- **Logs on SD:** `sdmc:/gk_boot_log.txt`, `gk_run_log.txt`, `gk_stdout.txt`,
  `gk_fatal.txt` (crash dumps). Crash analysis: `scripts/analyze-goal-crash.py`.
- **Standard test loop:** Sandover Village (incl. looking down from high ground — recorded
  worst case), Sentinel Beach, Misty Island; ~110 s sustained per spot, camera in motion.
- **Budgets:** 60 fps = 16.6 ms/frame; 30 fps = 33.3 ms (the `si=2` lock was proven exact).
- **Deploy loop:** docker NRO build (`scripts/build-switch.sh`) → verify md5 → **copy
  `gk.nro` to BOTH `sdmc:/gk.nro` (root — the copy hbmenu actually launches!) and
  `sdmc:/switch/jak1/gk.nro`** → `rsync -rc` `out/jak1/obj` + `out/jak1/iso` to the card →
  `sync`. Rollbacks: root `gk.nro.pre-fix31` (`33138483…`), `switch/jak1/gk.nro.pre-fix31`
  (`0cb12b6b…`).
- **Reference artifacts:** FIX 31 NRO `9be0e8b979567eb0a8d4720fbe6c9d8e` (deployed to both
  paths 2026-09-19), tag target `cbb01140e`+FIX31; older: `0cb12b6b2721728a5318ce8e535a664e`
  (FIX 29/30).



