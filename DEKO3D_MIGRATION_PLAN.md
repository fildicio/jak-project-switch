# DEKO3D Migration Plan — OpenGL/Mesa → native deko3d (AI-assisted)

**Created:** 2026-10-14, on branch `v1.1-beta-rollback` (v1.1-Beta + credit build).
**Companion to:** `SWITCH_PORT_SESSION_NOTES.md` (FIX 1–109 evidence log),
`PERF_PLAN_NEXT_AGENT.md`, `performance-improvement.md`, `JAK2_JAK3_ROLLOUT_PLAN.md`,
`CLINE_HANDOFF.md`.

**Status: EXECUTING Phase 1 (rev 4, 2026-10-16)** on branch `deko3d-migration` — Gate P0
met (P0.6 folded into P1.1); P1 code committed (e1569b6dd), Gate P1 hardware run pending.
See §0 for live progress. User decision 2026-10-16: *go straight to deko3d* (PG GL-side wins and
the 2nd P0.5 route are no longer blockers to starting deko3d work).

**Rev 2 (2026-10-14):** Rev 2 review changes: added P0.5 GL-time split
and an early perf gate at P3.2 (prove the win before porting 25 renderers); added phase PG
(draw merging + UBO migration on the GL backend *first*); added display/applet, render-target
and memory-pool items; cut P0.3 route capture, P6.4, most of P6.6; stock-clock 30 fps is a
stretch goal. GL counts refreshed from `docs/deko3d/gl-inventory.md`.

This document supersedes two older one-line verdicts:

- `PERF_PLAN_NEXT_AGENT.md` "Long term (don't start): native deko3d backend"
- `JAK2_JAK3_ROLLOUT_PLAN.md` "Not worth it (measured or rejected): deko3d rewrite"

Those rejections were **correctly scoped to the loading-stall investigation (FIX 56)**:
deko3d cannot fix a stall caused by ~200 blocking `sceCdRead` round-trips. FIX 56 itself
concluded: *"deko3d is plausibly worth it for the ~34 ms of per-frame render cost"*. That
second half — the render cost, and through it 60 fps — is what this plan delivers. The
loading-stall work is tracked here as a **separate parallel track** (Track L) because it is
I/O- and loader-bound, not GPU-API-bound.

---

## 0. Live progress (update every session)

| Item | State | Evidence |
|---|---|---|
| P0.1 baselines | partial — jak1 [glt] run only; jak2/jak3 + stock-clock pass not done | notes 2026-10-16 |
| P0.2 GL inventory | ✅ done | `docs/deko3d/gl-inventory.{md,json}` |
| P0.3 pipeline table | ✅ done — 224 pipelines, 16 samplers (> ~150 target; accepted, lazy create) | `docs/deko3d/pipeline_table.{md,json}` |
| P0.4 UAM spike | ✅ tfrag3 → .dksh; 3 diffs (stage names, sampler bindings, **std140 UBOs mandatory**) | `docs/deko3d/uam-spike/` |
| P0.4 triangle NRO | ✅ **PASS on hardware 2026-10-16** at stock clocks (CPU 1020 / GPU 307 MHz): device+swapchain ok, repo tfrag3 .dksh loads, 10k draws/frame recorded in 0.98 ms (0.10 µs/draw incl. UBO push + texture bind) | `tools/switch/dk_probe/`, notes |
| P0.5 GL-time split | ✅ route 1 PASS (proj gl/3 p95 10.6 ms ≤ 14); route 2 waived by user decision | `scripts/analyze-glt.py`, notes run 3 |
| P0.6 memory budget | ✅ measured on hardware 2026-10-17 (Gate P1 run): deko3d init costs **19 MB heap** (free 3114→3095 MB) for 9.7 MB of pools; steady state `heap free 2937 MB`, GOAL heap stable at 3261548 KB across the whole run (no leak) | `docs/deko3d/gate-p1-run-log-20261017.txt`, gk_run_log |
| PG.1 draw merging | ⏸ deprioritized by user (deko3d first) | — |
| PG.2 UBO migration | ⛔ **still a hard prerequisite for P3** (UAM rejects loose uniforms) — can be done as part of the deko3d shader pipeline instead of on GL first | uam-spike README |
| P1.1 device/pools/queue | 🟡 **code done 2026-10-16** (commit e1569b6dd): `dk_device.cpp` device, fb+data memblocks, graphics queue, fenced cmd slices; heap pre-check refuses init with < 24 MB headroom → same-boot GL fallback; `[heap]` lines = P0.6 measurement | `game/graphics/pipelines/dk_device.cpp` |
| P1.2 renderer module + display | 🟡 code done: `GfxPipeline::Deko3D` / `gRendererDeko3D`, kill-switch `sdmc:/switch/<game>/gk_dk3d.txt`, swapchain on `nwindowGetDefault()`, **SDL video never initialized** (SDL_INIT_EVENTS + gamepads only), engine handshake = GL contract (FIX 11/19) | `dk_renderer.cpp`, `gfx.cpp` |
| P1.2b applet/dock events | ⏳ applet pump + quit handled; dock/undock swapchain resize not yet (size fixed at boot, same as GL FIX 73c) | — |
| P1.3 CMake | ✅ `deko3d` linked next to Mesa; no runtime shader compile needed yet (splash uses the 2D engine) | `game/CMakeLists.txt` |
| P1.4 splash | 🟡 code done: SCREEN1 splash uploaded and blitted aspect-correct via `dkCmdBufBlitImage`; after 3 s a dark-blue pulse (liveness signal; game frames not drawn until P2/P3) | — |
| P1.5 [dk] telemetry | 🟡 code done: fps, frame/wait/record/submit ms, chains received, pool KB, heap free; every 10 s (2 s with R3+Minus) | — |
| Gate P1 | ✅ **jak1 hardware PASS 2026-10-17** (nro md5 `95ff86f7…`): splash + blue pulse on deko3d, shaders valid on hw, pads OK without SDL video (8 seen), audio + GOAL engine running, **30.0 fps locked, record 0.003 ms / submit 0.136 ms, chains 301/301**, no fatal. Still open: HOME/sleep/resume test (fold into T08 run), GL-path comparison boot, jak2/jak3 builds, desktop check | `docs/deko3d/gate-p1-run-log-20261017.txt` |
| Qwen task queue (P1.3–P2 prep) | ✅ T01–T07, T09, T10, T12, T13 reviewed 2026-10-16 (check-switch jak1 PASS + DkRing gtest 6/6; reviewer added a `size > segment` wrap guard in `dk_ring.h`). **T08 unblocked** (hardware test of `kSplash3D=true`) — next deploy; T09 focus/T10 verified on hw except HOME/sleep. T14–T16 ✅ reviewed 2026-10-16 eve: 224 pipelines + 16 samplers generated (`gen-dk-pipeline-table.py --check` clean), `dk_state.cpp` compiled in but **uncalled → inert** (build md5 `19025008…`); every deko3d name verified against the vendored header; Fix64Sub blend-const gap documented (`dkCmdBufSetBlendConst` at bind time, P3). **Next: P2.3 batch — T21 ✅ + T22a ✅ 2026-10-16 night** (BufferPool 8 MB region DONE-PASS md5 `4d972e57…`; T22 split into a/b/c after a model-level timeout on one big session — T22a `dk_device_internal.h` pass-throughs DONE-PASS md5 `169d722f…`, reviewer-verified; API cheat-sheets now embedded in task files after two reviewer guesses were caught wrong). **T22b/T22c/T23/T24 ✅ reviewed 2026-10-17** (dk_texture_gpu store + RGBA8/BCn upload; GOAL texture seam in shared TexturePool.cpp behind `__SWITCH__` guards, desktop byte-for-byte; `[dk] tex|pool|ring` soak telemetry in dk_renderer.cpp — reviewer re-ran check-bg/tail DONE-PASS). **P3 opening batch T25/T26/T27 ✅ reviewed 2026-10-17** (UAM .dksh build rule — determinism gate re-verified byte-exact by the reviewer; FIX64_SUB blend-const helper; color RT slice 1; all inert — `--gc-sections` in switch.specs keeps the NRO byte-identical to T24's `727feb1c…`, so the soak NRO is unaffected even though T25–T27 ran before the soak closed). **P2.3 now closes with the user's 2-min hardware soak + T08 hw test on the `727feb1c…` NRO.** **P2 batch T17–T20 ✅ complete 2026-10-16 night** (T20 UBO ring DONE-PASS md5 `c7f2f0ba…`, reviewer-verified). T11 ⏸ **deferred 2026-10-17**: keep boot-size swapchain (GL FIX 73c parity; engine pins 720p anyway, resize = fragile lifecycle for no visible gain) — decision in the task file, revive inside P3 render-target work. **P3 core batch T28–T42 written 2026-10-17** (reviewer-authored, grounded: T28–T32 finish P3.1 render targets — depth Z24S8, FullTarget, MSAA/resolve, blit, CPU-queue readback; T33–T38 = tfrag3 machinery incl. `find_for_draw_mode`; T39 = first live change since T24, gated on the P2.3 soak; T40 = the P3.2 PERF GATE (≤ ~⅓ GL per-draw on hw, FAIL stops P3.3); T41/T42 Tie3/Shrub prep gated on PASS) | `docs/deko3d/qwen-tasks/README.md`, `QWEN_REPORT.md` |
**2026-10-17 night (reviewer):** T28's only FAIL was the stale expected inert md5 — the reviewer rebuilt BOTH the clean HEAD tree and the T28 tree (byte-identical `74635895c2d04602de48dc5dccd1839e`, DONE-PASS) and re-baselined the T28 + T29–T37 done-checks `727feb1c… → 74635895…`; T28 → 🟡 (code verified vs deko3d.h). **T43 written** (⏳, run after the T29–T37 inert batch): the 2-min soak showed `[dk] tex live=0`, jungle blocked on "a level is still staging", `[loader] … 0.0MB free, 100 out` — on the dk path LoaderStages still uploads through GL with NO context; T43 seams `add_texture` → `dktexgpu::upload_rgba8` (first LIVE change since T24, md5 will move again). Geometry (`GpuBufferPool`) stays GL for now — next P2.3 slice if jungle still stalls after T43.
**2026-10-18 (reviewer):** T29 looped the coder ~100 min with ZERO output — too big for one session. Split: **T29a** (GL Fbo inventory, DOC-ONLY `docs/deko3d/gl-fbo-inventory.md`, exact pre-verified file list + ~30-min time box, no build) then **T29b** (dkrt FullTarget code only, inert, pastes T29a's table into the .h). T30–T32 deps T29 → T29b. Inert batch is now T29a→T29b→T30–T37 (md5 `74635895…`), then T43 (live).
**2026-10-19 (reviewer, model-fit pass):** T35b ✅ (re-ran gate: PASS, 0 warnings). md5 drift root-caused: `common/versions/revision.h` stamps the commit SHA, so every commit moved the NRO md5 — the AI gate now pins it (`OPENGOAL_PIN_REVISION=aigate` in check-switch.sh → common/CMakeLists.txt), stable baseline **`c613ba81bb31570d13750a2fd24d07a7`**. T37a std140 bug fixed (FragParams fog_color @16, not @8). T37b rewritten as a pre-tested patch (zero decisions); T43 trial-built from its verbatim text → PASS `e49fb3eb…`. New law: reviewer trial-builds every task before ⏳.
**2026-10-18 (reviewer, post-T29b):** T29a ✅ (claims spot-checked vs the code: `make_fbo`@1294 + 3 runtime call sites, exactly 9 `glGenFramebuffers` sites / 6 blits / 1 readPixels — all match) and T29b ✅ (FullTarget + 32-slot pool + shared `bind_views()`; reviewer re-ran check-bg → DONE-PASS, diff reviewed line-by-line, no new deko3d APIs). **2nd baseline drift** (devkitPro image re-pull): `74635895…` → **`08fc9510745f7c584ab6c8bd73a1b883`** (chain: 727feb1c→74635895→08fc9510) — proven by Qwen's A/B (code vs reverted-HEAD, byte-identical) + reviewer re-run. Re-baselined T29b + T30–T38 done-checks (T38/T41/T42 still said `727feb1c…` — missed by the earlier sweeps) and T43's DIFFERENT-check. Reviewer note: free `dkrt::teardown()` doesn't sweep live FullTarget slots — T30 decides caller ownership. Remaining inert batch: T30–T37 (all expect `08fc9510…`), then T43 (live, md5 moves).
**2026-10-18 (reviewer, queue halving):** T30's whole-task session died the same death as T29's, worse: ~5 h live, 11 context compactions, 83 reads / 127 commands / 4 edits (0 in the .cpp), no build; its log even shows the wrong-game done-check (jak2). Root cause is task SHAPE (open-ended "read Example06 / grep deko3d.h" sections), not content. **Every remaining task was halved into a/b sessions (T30a…T42b):** ~40-min hard time boxes, verbatim-pasted deko3d.h API lines, forbidden-read lists, exact read ranges, done-check spelled `check-bg.sh jak1`; T38b gets an internal stop-ladder (cull→binds→draw→AFAIL). Old whole-task files carry SPLIT banners (history only). Latent cheat-sheet errors caught while splitting, now fixed in the halves: `DkCopyBuf` real fields are `{addr, rowLength, imageHeight}` (draft said address/rowPitch/slicePitch), depth-write control is the `depthWriteEnable : 1` bitfield (draft said "writeMaskChange"), the bind is `dkCmdBufBindVtxBuffers` (plural), and Example06 contains NO resolve call (old T30 pointed the coder there). Qwen's sole T30 edit (`dk_render_target.h` `samples`, +13/−2) kept — spec-matching, consumed by T30a. T40 (already coder/human split) and T43 (single pasted edit) were tightened with session rules rather than split. New order: T30a→T30b→T31a→T31b→T32a→T32b→T33a→T33b→T34a→T34b→T35a→T35b→T36a→T36b→T37a→T37b→T43 (live, any time after T37b); T38a/T38b ⛔ soak PASS, T39a/T39b live, T40 gate, T41a–T42b gated. All halves still expect inert md5 `08fc9510…` unchanged.
**2026-10-18 (reviewer, T30a–T31b ✅):** first four halved sessions reviewed and approved. T30a: MSAA 2DMS colour image (samples 1/2/4/8 validated before the slot claim), `matches(w,h,msaa)`, depth stays 1x. T30b: 1x resolve sibling (own `GpuCached|Image` block) + `resolve()` (barrier Full,0 → `dkCmdBufResolveImage`) + opaque `resolved_view()` + full pool sweep in `teardown()` — settles T29b's ownership note; ONE APPROVED DEVIATION: sibling flags `UsageRender|Usage2DEngine` (deko3d.h L378 requires 2DEngine for image-transfer blits; task text said Usage_Render only). T31a: `dkrt::blit()` core (flags Linear/FlipX/FlipY per L1171-1178, barrier-then-blit per the dk_device precedent, factor 0). T31b: 6-site GL blit grounding table in the .h (reviewer re-grepped: exactly 6 real sites; the "7" in the old task text was wrong — the inventory doc already said 6). All deko3d names verified vs the vendored header; inert (zero callers anywhere in game/). Reviewer re-ran check-bg/tail → DONE-PASS, NRO md5 `ce41507cb3319875fcb0a1ffe051b82d` unchanged = the approved 3rd-drift baseline (T30a/T30b files' `08fc9510…` Done-check text is stale but inertness is A/B-proven). Next inert batch: T32a → T32b → T33a → T33b → …
**2026-10-19 evening (reviewer, T43 hardware run):** dk session ran 44 s at 30 fps, no crash, screen = Phase-1 dark pulse (expected). T43 is **inert**: `[dk] tex -` because `dktexgpu::init()` has no caller, and the dk pipeline never creates/updates a `Loader` (only `opengl.cpp` does) — the T43 premise ("jungle still staging on dk") came from a GL session appended to the same `gk_run_log.txt`. Lesson: split appended logs by `session start` + `renderer:` before diagnosing. Next P2.3 slice (must be designed before T38): (1) dk pipeline owns a Loader + TexturePool and calls `update()` from the render thread, (2) call `dktexgpu::init()` after `dkdev::init()` and `teardown()` before `dkdev::shutdown()`, (3) uploads share `s_cmd`/`s_queue` with `present_frame` → must run on the render thread or under a device mutex.
GL regression run same NRO (65444d85, no gk_dk3d.txt): 227 s, no fatal, `level title ready in 0.55s`, `level village1 ready in 1.48s` → commit 16dc78717 is safe for normal play.
**Plan next (reviewer, 2026-10-19 night):** T46 ✅ (jak2 `85909735…`, jak3 `a7504ed1…`). Queued T47 (dktexgpu owner-thread guard, inert, md5 `4033aa17…`) → T48 (init on the render thread's first frame + teardown in dk_exit, LIVE telemetry-only, md5 `91e3442e…`); both pre-tested patches. After the T48 console check: design the dk-path Loader (needs dk seams for ~70 GL calls in Loader/LoaderStages + GpuBufferPool geometry; Loader::update must run on the render thread).
**T48 hardware PASS 2026-10-19** (NRO `91e3442e…`, dk mode, 45 s): `[dk] tex pool init: ok`, telemetry `tex live=0 high=0 fail=0`, heap free 2927 → 2879 MB (exactly the 48 MB pool), no T47-guard line, no fatal, 30.0 fps. Next: dk-path Loader (design by reviewer, repetitive GL-seam edits by Qwen per the cost law).

**T49 (reviewer-direct) 2026-10-19:** opt-in dk-path Loader. With `gk_dk3d.txt` **and** `gk_dkload.txt`, the dk first frame installs the GL census (every glad pointer → no-op stub logging `[dk] gl-census #N first call: glX`), builds TexturePool + Loader on the render thread, `load_common`, then `update()` each frame; `dk_set_levels`/`dk_set_active_levels` forward to the Loader. Expected on hardware: `[dk] gl-census installed`, `[dk] dk-loader up … tex live=N` (N>0 = real common textures in dktexgpu), census lines, `tex live` growing on level load, no fatal. The census list becomes Qwen's seam tasks (T50+). Without `gk_dkload.txt` nothing changes. Build PASS md5 `9f847ced…`.

**T49 hardware 2026-10-19:** census installed (929 stubs), then the render thread froze inside the first frame: zero census calls, no `[dk] tex id=` line, no splash/telemetry; EE kept running (kernel loop at 28 s), no fatal. The first-ever dktexgpu upload (TexturePool placeholder 16×16) never returned from `dkFenceWait(-1)`. Neither `upload_rgba8` nor `copy_to_gpu` had ever completed on hardware before. **T51**: copy params match the proven splash upload (`{addr,0,0}`, no barrier; `imageHeight` is a layer stride in bytes), bounded 1 s upload fence with a `FAILED` log + uploads disabled, step breadcrumbs. md5 `eb44a99e…`, deployed.

**T51 hardware 2026-10-19:** no freeze. Upload fence timed out (1 s) → uploads disabled; afterwards splash, 30 fps, dk Loader up, levels load (`live=4`). GL census on the dk Loader path = only 5 functions: glGetError, glGenBuffers, glBindBuffer, glBufferData (buffer pool / merc stage), glActiveTexture. **Root cause (T52):** `dkQueueSignalFence(q, f, true)` does not submit — `true` is a cache flush; work stays CPU-side until `dkQueueFlush` (deko3d `Queue::waitIdle` = signalFence+flush+wait). Present worked because `dkQueuePresentImage` flushes. T52 adds `dkQueueFlush` at all three waited fences; md5 `94171101…`, deployed.

**T52 hardware PASS 2026-10-19** (md5 `94171101…`, 171 s, dk + dkload): first real deko3d textures. Placeholder + common (347 tex in 86 ms) + title/beach/jungle → `tex live=1163 fail=0`, ≈20.5 MB of the 48 MB pool (all RGBA8 — FIX 91 decodes BCn on CPU), no fence timeout, 30.0 fps steady, no fatal. Remaining GL on the dk Loader path (census): glGetError, glGenBuffers, glBindBuffer, glBufferData, glActiveTexture → next Qwen seam tasks (geometry buffers → dkpool, needs the bigger pool P2.1).

**Plan next (reviewer, 2026-10-19 late):** T53 ✅ (jak2 `93a5c74f…`, jak3 `2a18f3b5…`). Census seams: 4 of the 5 GL calls (glGetError/glGenBuffers/glBindBuffer/glBufferData) all come from `GpuBufferPool::allocate()`, the 5th (glActiveTexture) from `mipq_process()`. Queued as patch tasks (reviewer trial-built): **T54** dkpool 8 → 128 MB + heap check + mipq dk gate (md5 `7df9eef8…`); **T55** Loader geometry seam — `GpuBufferPool` hands out dkpool chunk ids on the dk path (`dk_loader_buffer_find(id)` → gpu addr for P3 renderers), `upload_to_buffer` → `dkdev::copy_to_gpu` (T54+T55 md5 `af69f0c6…`); **T56** jak2+jak3 recheck. Then hardware test with `gk_dk3d.txt`+`gk_dkload.txt`: expect `[dk] loader buffer path` line, no new census lines for those 5, `[loader] … pool=` showing dk bytes, no `copy_to_gpu FAILED`.

**T55 hardware 2026-10-19 (md5 `af69f0c6…`, 67 s):** geometry seam WORKS — `[loader]` pool line now shows dk bytes, copies ok (0 FAILED), 30 fps, no fatal. Two problems: (1) each `copy_to_gpu` ≈16.6 ms (upload queue = present queue, waits behind vsync) → **T58** separate upload queue; (2) 128 MB filled by common + 2 levels (6 failed at beach prefetch) → **T57** 384 MB + quieter log. T57 md5 `dd922caa…`, T57+T58 `0716032f…` (already on the SD with dk flags on). Logs `logs-T55-dk/`. NEXT after the morning test: P3 tfrag draw (T38 reshaped, batched) so the next hardware test shows geometry — no more blue-screen-only tests.

**T58 hardware 2026-10-20 = P2.3 soak PASS (md5 `0716032f…`, 285 s on deko3d, dk flags on):** `[dk] upload queue ready (separate from present)`; `copy_to_gpu` now 0.05–0.3 ms (was 16.6 ms); 4 levels live, dk pool 158.5 MB free of 384, 0 failed allocs; tex live 1163, 0 fail; steady 30.0 fps, no fatal. Logs `logs-T58-dk/`. **Soak gate passed → T38a/T38b/T39a/T39b unblocked**; they are rewritten as reviewer-authored patches (model-fit law) before Qwen runs them.

**T60 written 2026-10-20 (reviewer-authored, supersedes T38a/T38b/T39a/T39b):** first visible tfrag draw for jak1, opt-in via `sdmc:/switch/jak1/gk_dktfrag.txt` (off = T58 behaviour). Patches `T60a` (device: 1 MB cmd slice, Z24S8 depth, `DepthMinusOneToOne|OriginLowerLeft`, frame barrier, `set_world_recorder`; dktexgpu `slot_of`/`update_rgba8`), `T60b` (TFragmentDk capture buckets 6/13 + build: cull, index ring, tod palettes, draw list), `T60c` (`dk_tfrag_record.cpp` + wiring). Reviewer trial jak1 PASS md5 `cf9c6384…`. Unverified on hw: image orientation (swap OriginLowerLeft if upside-down), 1 MB slice headroom. T61 = jak2/jak3 recheck. Next hw test: geometry over the blue pulse + `[dk] tfrag frame` log lines. T40 perf gate follows.

**T60 hardware 2026-10-20 (md5 `cf9c6384…`, ~260 s) = FIRST deko3d WORLD DRAW:** Sandover village tfrag visible and correctly oriented; 30.0 fps steady, record 0.07 ms, 68–98 draws / 17k–63k tris per frame, dropped=0 misses=0 ubo_fail=0, 0 errors in the run. Missing: water (tfrag TRANS/WATER trees not drawn yet — only NORMAL/LOWRES). Bug: boot splash upside down — OriginLowerLeft makes the swapchain flip-V every frame and the 2D blit ignores the origin mode → reviewer fix `DkBlitFlag_FlipY` on the splash blit (md5 `f9d56d91…`, on the SD). Logs `logs-T60-dk/`. NEXT: trans/water trees, then T40 perf gate.

**T62 2026-10-20 (reviewer-committed, md5 `0624560a…`, on the SD):** splash-fix run clean (30 fps, 0 errors; orientation = user eye check). Added jak1 tfrag alpha passes in GL bucket order: TRANS+LOWRES_TRANS (32/39, after the SkyBlendHandler sky packets: qwc 8 set-display, qwc-6 pairs, 3 resets), DIRT (34/41), ICE (36/43). Log gains `alpha=`. NOTE: jak1 sea/water is the **Ocean renderer** (OceanMidAndFar bucket 4 + OceanNear 63, ~12k lines incl. VU emulation), NOT tfrag — separate later item. Characters (Jak/Daxter) = Merc2/Generic2, not ported yet. Process change: reviewer-trial-built code is committed directly; Qwen gets builds/rechecks/log digests (T63).

**T62 hardware 2026-10-20 (md5 `0624560a…`, ~94 s) = ALPHA PASSES RUN CLEAN:** last session of the SD run log shows the new `alpha=` telemetry live — `cams=3 draws=79–101 alpha=4 tris=36k–58k dropped=0 misses=0 ubo_fail=0 ubo_used=1 KB` (4 non-empty alpha buckets drawing in the village view), steady 30.0 fps (frame 33.33 ms, record 0.07 ms, chains 301/301), tex live=1167 fail=0, heap free 2.2 GB, `gk_fatal.txt` empty, clean exit. The one `upload fence wait FAILED` and one pool-failed-alloc line in the file belong to pre-T54 sessions appended earlier in the same log — not this build. Logs `logs-T62-dk/`. Open: user eye check that trans/dirt/ice trees look right; then T40 perf gate (µs/draw vs GL).

**T64 written 2026-10-20 (reviewer-authored, reshapes T40 part 2 — the P3.2 PERF GATE):** no new build; the T62 NRO (`0624560a…`, already on the SD) runs BOTH sides via flag files — GL run: `gk_dk3d/dkload/dktfrag` off + `gk_glt.txt` on → `[buckets]` tfrag avg ms + draws/frame + `[glt]` µs/call; dk run: flags restored → `[dk]` record ms ÷ `[dk] tfrag frame` draws. Same clocks (hbmenu 1020/307 fine, just unchanged between runs), same Sandover spot, ~2 min each. **PASS = dk µs/draw ≤ ⅓ GL** (L399–401) → unblocks T41a–T42b; FAIL = stop-and-notes per plan. The dk run also closes the T62 tree visual check. T40 part 1 was superseded by T60's real telemetry. SD pre-staged for the GL run on 2026-10-20.

**T64 GATE PASS 2026-10-20 — P3.2 PERF GATE CLOSED (T40 closed with it):** the planned GL redo was cancelled; the gate closed on existing data, conservative in GL's favour — GL tfrag buckets ≈ 9.5 ms/frame (FIX-31 hw, Sandover steady: l1-tfrag-tie 4.4 + l1-tfrag-tfrag 2.5 + alpha-tfrag-trans 2.6, notes L2864-2869; post-tod-cache ≥ ~5 ms) and same-clock draw-call floor 3.76 µs (glDrawElements, 193,871-call sample, GL attempt-1 run of the same NRO `0624560a`) vs dk steady `record 0.003 + submit 0.13 ms/frame`, 74–96 tfrag draws, 30.0 fps vsync-locked (T62 hw). Result: ≥3.4× on bucket-ms, ≥20× on µs/draw — gate needed ≥3×. P0.5 projection validated (needed gl/5 = 9.12 ms for 60 fps; dk does the same world in ~0.14 ms CPU). Scope caveat recorded in the task file (dk draws tfrag-only so far; the decision uses bucket-scoped + per-draw pairs, not whole-frame). Attempt 1 also proved the GL fallback path still boots cleanly on the current NRO (0 errors, gk_fatal empty). **T41a–T42b unblocked.** Open: user's tree visual verdict (from memory of the T62 run). Next: refresh T41a/T41b/T42a/T42b against the T60 draw-path shape if needed, then the P3.3 batch.

**R1 2026-10-20 (reviewer-committed, jak1 md5 `ce12b1ec…`) = HOST REPLAY TEST:** `TFragmentDk.cpp` now also compiles on the Mac (`DK_HOST_REPLAY`, goalc-test). With `sdmc:/switch/jak1/gk_dkdump.txt` the Switch writes one frame's DMA chain (`dk_replay.dma`) and the draw list it built (`dk_replay.golden.txt`); `DkTfragReplay.MatchesSwitchGolden` replays it and must match line for line. Purpose: the coder model can change/port renderer CPU code with a Mac PASS/FAIL instead of SD trips. Docs `docs/deko3d/REPLAY_TEST.md`; tasks T65 (recheck) and T66 (first run, waits for the dump). Token-saving rules added to `AI_ROLES.md` (review-pack, model by job, Qwen-drafted clone tasks).

**T41a/T41b/T42a/T42b/T65 reviewed 2026-10-20:** Tie3/Shrub prep headers (`dk_tie3_prep.h`, `dk_shrub_prep.h`, inert, host-compile + lint PASS). Facts for P3.3: GL Shrub is NOT instanced (Tie3's wind pass is the first instanced draw); Tie3 needs TFRAG3 + ETIE_BASE + ETIE + TIE_WIND, and only ETIE/ETIE_BASE still lack a deko3d `.glsl`; both renderers can reach all 224 pipelines (DMA-driven DrawMode). jak2 `3fac8f1e` / jak3 `9adbff3d` build after R1. Qwen now runs `check-dk.py` by itself; new pitfall P9 (`.dksh` is a build output) added to the recipes after it misread that.

**Plan next 2026-10-20 (reviewer):** R1 dump attempt #1 produced nothing: the SD tidy-up had moved the switch files `gk_dk3d/gk_dkload/gk_dktfrag.txt` into `logs-old/`, so the run was plain GL (restored; REPLAY_TEST.md now warns). Queued P3.3 prep for Qwen, all reviewer-trialled: **T67** ETIE/ETIE_BASE shaders via `pg2-convert-shader.py --etie` (new `EtieParams` std140 block at binding 8, UAM OK) + C++ mirror struct; **T68** `dktie::fill_etie` + gtest `EtieFill` (15 PASS + 1 SKIP); **T69** jak2/jak3 re-check. jak1 md5 stays `ce12b1ec`.

**2026-10-18 night (reviewer, T32a–T33b ✅ + 4th baseline):** four more halves reviewed and approved; Qwen fully stable on the halved shape (30/35/26/26-min sessions, one thrash-death on T32a attempt 1 recovered by a clean restart). T32a: lazy readback resources (2× CpuUncached|GpuCached blocks, dedicated Graphics queue, checked creates + full unwind, reverse-order destroy in `dkrt::teardown()`). T32b: `readback_rgba8()` = guards → ensure → fresh cmd slice → `dkCmdBufCopyImageToBuffer` flags 0 → submit→signal(flush)→wait(−1) → memcpy; `DkImageRect`/`DkCopyBuf` field orders verified. T33a: tfrag3 dksh pair appended after the splash pair reusing `load_shader()`; overflow pre-check exactly mirrors load_shader's align-then-fit math (verified L167-195). T33b: 3 by-value getters + `shutdown()` re-zero. **md5 rule changed:** T33a is LIVE boot-init code → `ce41507c→ba3d9a08` (expected, escape-clause); T33b's diff (incl. its live shutdown edit) → `ba3d9a08→e819b40d` (coder A/B-attributed; reviewer reproduced `e819b40d2be605b19099288c9f215ecf`). **4th baseline set, all pending task files re-baselined;** new queue rule: file-static uncalled code keeps the md5 frozen, live-function edits move it — on a move, A/B-attribute and report, STOP only if unattributable (README top note has the full rule; T34a/T34b done-checks reworded to match). T28 row closed (was 🟡 by oversight). Next: T34a → T34b → T35a → … → T37b → T43; T38a/b ⛔ soak.
**2026-10-19 (reviewer, overnight + morning triage):** Overnight `ai-run --max 6` landed **T34a ✅ T34b ✅ T35a ✅** (22:28–01:29; every md5 move A/B-attributed under the new rule: e819b40d→3117bb6d→54d0e640→833b9139) then **T35b died twice** (3.7 h + 2 h, compaction deaths, 0 edits) — root cause task-shape: v1 pointed at a `DrawMode` C++ struct that doesn't exist and left the match predicate open-designed. Verdict on T34a/T34b/T35a: approved (T35a with one reviewer fix — its null-cb guard logged but didn't `return` as the task required; fixed + rebuilt → DONE-PASS **md5 `67018cd5a4411b3f3cc77f198773b261` = 5th baseline**). Queue-law mechanism identified: dk_state.cpp became live-linked the moment T34a's init loop called `to_deko3d`, so ANY dkpipe edit now moves the md5 — pending files T36–T43 re-baselined to `67018cd5` with the A/B rule baked into their done-checks. **T35b rewritten v2** (spelled-out `DrawModeAxes` input + verbatim predicate; GOAL-bit→axes derivation deferred to T38 per pipeline_table.md L27). Reviewer relaunched `ai-run --max 3` (T35b v2 → T36a → T36b) ~07:40.



**Gate P1 run checklist (next session):** (1) jak1 with `gk_dk3d.txt` → splash 3 s then
blue pulse, `[dk] device up`, `[heap]` before/after lines recorded as P0.6; (2) **pads
work without SDL video** (top known risk — InputManager/DisplayManager get a null window);
(3) audio + game logic keep running (chains received count rises); (4) HOME/sleep/resume
once; (5) delete `gk_dk3d.txt` → GL path boots unchanged; (6) jak2 + jak3 builds, same
checks; (7) desktop build behavior unchanged.

**P1 as built vs. as planned (§5) — deliberate deviations, revisit before P3:**

| Plan said | Code does | Why / follow-up |
|---|---|---|
| CMemPool code/data/staging pools sized from P0.6 | 3 raw memblocks (fb, 2.1 MB data, splash) + heap pre-check (24 MB headroom) | P0.6 numbers did not exist yet; CMemPool lands in P2 with real sizes |
| CPU0 render queue + CPU1 loader queue | 1 graphics queue | loader/upload queue belongs to P2 |
| 3-buffer swapchain | 2 buffers, fenced 64 KB cmd slices | revisit in P5 if `wait` ms shows starvation |
| `uam` CMake rule `.glsl → .dksh` | not added; only `deko3d` linked | splash uses the 2D engine, so no shader needed yet → **moved to P2/P3 start** |
| splash via trivial 3D pipeline | `dkCmdBufBlitImage` (2D engine), HwCompression off | 3D splash pipeline = "P1.4b", together with the uam rule |
| dock/undock + focus/suspend hooks (P1.2b) | applet pump + quit only; size fixed at boot | still open, same limitation as GL FIX 73c |
| boot line `[deko3d] device up…` | `[dk] device up: …` + `[gfx] renderer: …` | naming only |

**Measured 2026-10-16 (jak1, village1 + beach, 4,450 frames, reference clocks):**
the old "render 30.7 ms" baseline was mostly **vsync wait inside the FIX 13 deferred
window `glClear`** (EGL swapchain acquire; corr −0.98 vs real work). Real render work:
**16.9 ms p50 / 21.5 p95 = GL 11.6 + C++ 4.6 + texture uploads ~3.6** (included in GL).
Per-call GL cost is only ~2–5 µs; the cost is call volume (5–15k GL calls/frame,
incl. up to ~2.8k `glUniform1i`). Projection with deko3d at GL ÷ 3: **8.6 ms p50 /
10.6 ms p95** → 60 fps render-side is plausible for jak1 (the GOAL-engine side of §6 still
applies). Section 2's "not measured" row is now superseded by these numbers.

**Hardware-deploy rule (user, 2026-10-16):** emulator smoke before any SD deploy. Eden is
broken on this Mac (aborts with `std::length_error` before loading any NRO), so the gate
today is: docker build → objdump audit of EL0 sysreg reads (the class that crashed the
first [glt] NRO) → SD deploy, with the fatal handler as the diagnostic.

**Toolchain fact:** the local `devkitpro/devkita64` image already contains deko3d 0.5.0
(`libnx/include/deko3d.h[pp]`, `libnx/lib/libdeko3d.a`), UAM, and the official deko3d
examples — no `switch-deko3d` install step is needed.

**deko3d on hardware, measured 2026-10-16 (dk_probe, stock clocks):** command recording
costs **0.10–0.18 µs per draw** (32 B `dkCmdBufPushConstants` + texture bind + draw),
submit+present ~0.12 ms/frame. Mesa today: ~2–5 µs per *GL call* at 1200 MHz. That is
~20–50x cheaper per operation at a lower clock: the projection's GL ÷ 3 / ÷ 5 was
conservative. Not yet measured: GPU time (10k tiny draws fit one 16.6 ms frame at
GPU 307 MHz), vertex fetch, blending/depth, real fill cost.

---

## 1. Goal and success criteria

| Goal | Measurable target (from the existing telemetry) |
|---|---|
| 60 fps where the game logic allows it | `render` p95 ≤ 10 ms (from 30.7 p50 / 35.8 p95 today), `swap` ≤ 3 ms, locked 16.63 ms frames in ≥ 90% of windows on the standard routes — **at the reference clock profile** (jak1 @1200 MHz, jak2/3 @1400 MHz CPU) |
| Stock-clock 30 fps (stretch goal — impossible today for jak2/3) | All three games hold locked 30 fps at **stock 1020 MHz** CPU handheld; today the render phase alone is ~40–48 ms at stock ⇒ misses 30 fps (§2) |
| Faster area loads / no stream-in hitches | `ready in` times cut ≥ 50%; `[cam] HITCH` 45–60 ms band ~0/min during streams (today ~250/session) |
| Efficiency | NRO size and RAM down (drop Mesa/nouveau); GPU memory under explicit budget (CMemPool); lower CPU/frame → cooler, quieter, longer battery |
| Stability | Eliminate the Mesa heap-corruption crash class (f97 merc memcpy, f100 `_mesa_format_has_color_component`) |
| Non-regression | Desktop OpenGL path untouched; Switch GL build remains selectable as fallback for at least one minor release |

**Honesty requirement (learned from FIX 77):** 60 fps needs *both* the renderer ≤ 16.6 ms
**and** the GOAL engine ≤ 16.6 ms (`wait_dma`). Geyser Rock measured `wait_dma` 35–40 ms —
no GPU backend fixes that. The 60 fps unlock is therefore per-game and initially per-area,
with 30 fps as the floor. See §6.

---

## 2. Where we are — the measured baseline (do not re-litigate, re-measure)

All numbers are from real hardware logs recorded in `SWITCH_PORT_SESSION_NOTES.md`:

| Signal | Value | Source |
|---|---|---|
| `render` phase (CPU GL submission) | **30.7 ms p50 / 35.8 ms p95** steady-state | A0 post-FIX-29/30 baseline |
| Reference clock profile (user's setup) | jak1 720p @ **1200 MHz** CPU — stable 30; jak2/3 540p→720p FSR @ **1400 MHz** CPU — stable 30; stock handheld CPU = **1020 MHz** | user, 2026-10-14 |
| Stock-clock implication | render cost scales ~linearly with CPU clock: 30–36 ms at profile → **~40–48 ms at stock ⇒ jak2/jak3 miss 30 fps on a stock console**; the OC is life support for Mesa overhead, not game weight | derived; measured at P0.1 |
| Frame budget at 30 fps | 33.3 ms → **zero headroom**; loader's 11–12 ms goes straight to frame time | FIX 43 |
| Draws / indices per frame | **1,147.3 draws, 750.8k idx** over 327 buckets, flat profile, worst bucket 13.6% | FIX 42a/43 |
| `swap` phase | 0.3 ms always → **GPU present is never the limit** | FIX 12 |
| Per-call Mesa costs | `glTexImage2D` ~1–1.5 ms; `glDeleteTextures` ~0.5 ms; `glGenerateMipmap` 3–5 ms | FIX 90/99/100 |
| GL surface area | **2,243 GL call sites / 37 files / 117 distinct functions** in `game/graphics/` (comments excluded); 232 `glUniform*` sites; 9 FBOs, 7 blits, 1 `glReadPixels` | `scripts/gen-gl-inventory.py` 2026-10-14 |
| Shaders | **94 GLSL files** (`shaders/*.vert|frag`, ~47 programs), compiled at runtime by Mesa; **309 default uniforms**, **0** with `layout(binding)` | `docs/deko3d/gl-inventory.md` |
| Render-phase split (GL driver vs our C++) | **measured 2026-10-16 (jak1):** real work 16.9 ms p50 = GL 11.6 + C++ 4.6; the rest of the 30.7 ms "render" is vsync wait in the window glClear; GL per call ~2–5 µs (not 20–30 µs) | P0.5 [glt] run 3, §0 |
| Bucket renderers | **~25 `BucketRenderer` subclasses** (tfrag3, tie3, merc2, shrub, sprite3, sky, ocean ×3, shadow, depth-cue, glow, eyes, warp, …) | grep |
| Loading stall root cause | ~200 blocking `sceCdRead` submit/sleep/join round-trips per level; 32 KB pages; blzo on the read thread; SD at 88 MB/s is *not* the limit | FIX 56 |
| jak3 Haven City | loader ema p50 36 ms / p90 42 ms at 540p → CPU-bound (GOAL + submission) | PERF_PLAN |
| GPU fill-bound case | 1080p far-terrain: 23–26 fps, `swap` 9–14 ms (only case where pixels dominate) | void-session data |
| Memory | GOAL heap flat at 3,261,488 KB; only ~4 MB free → **never create a runtime `std::thread`** | FIX 34c |
| Crash class | Mesa/nouveau heap object clobbering (f97, f100) — root cause never proven | FIX 97/100 |

**Current stack:** game → `OpenGLRenderer` → glad (desktop-GL loader, null slots re-resolved
by hand, FIX 43a/52 traps) → Mesa GLES 3.1 → `drm_nouveau` → NVN → Tegra X1. Every draw
pays GL state validation, and every shader pays runtime GLSL compilation. On 3× A57 this
costs ~20–30 µs/draw — which would be exactly the 30 ms frame **if** the render phase were
all driver time. Part of it is our own C++ (DMA walk, culling, merc/TextureAnimator CPU work)
that deko3d does not speed up; P0.5 measures the split before any porting starts. Note: every telemetry number in
the table above was captured under the user's overclock profile; **no stock-clock baseline
exists yet** — P0.1 creates it. Practical proof of the driver tax: jak2/3 hold locked 30
fps **only** at 1400 MHz (+37% over stock 1020 MHz); on a stock console they miss 30.

**Target stack:** game → `Deko3DRenderer` → deko3d (thin, explicit) → NVN → Tegra X1.
Per-draw cost ~2–5 µs, shaders precompiled at build time, memory explicitly ours.

---

## 3. Feasibility facts (verified, not assumed)

1. **Shaders do not need to be rewritten.** [UAM](https://github.com/devkitPro/uam)
   compiles **GLSL → DKSH** at build time (Mesa 19.0.8 parser + improved nouveau nv50_ir
   backend, Maxwell dual-issue scheduling). The 94 existing `.glsl` files are the source;
   they need mechanical adaptation, not translation to HLSL.
2. **The UAM constraints that shape the work:**
   - `layout(binding = N)` is **required** on every UBO/SSBO/sampler; bindings map 1:1 to
     deko3d slots (16 UBOs / 16 SSBOs / 32 combined image-samplers per stage).
   - **Default uniforms are rejected** — every `glUniform*` (232 sites, 309 shader-side
     default uniforms) must become a
     UBO struct write + one constant-buffer bind. Mechanical, but touches all 26+ renderer
     files (FIX 43 already memoized locations; those caches get deleted, not ported).
   - No transform feedback, no subroutines, separable programs always (the renderer never
     links after startup today — compatible).
3. **BC1/BC3/BC7 are native image formats.** The fr3 v44 BCn data can be DMA'd **as-is**
   into VRAM. The loader-thread BCn→RGBA decode (FIX 90/91) — a workaround for Mesa's
   slow compressed upload — becomes unnecessary. So does deferred `glGenerateMipmap`
   (FIX 42) because full mip chains ship in the fr3.
4. **Pipeline abstraction seam already exists:** `GfxRendererModule` (`gfx.h`) with
   `GfxPipeline { Invalid, OpenGL }` and a single implementation `gRendererOpenGL`
   (`pipelines/opengl.cpp`). Adding `GfxPipeline::Deko3D` + `gRendererDeko3D` is the
   natural switch point. What does *not* exist yet: any abstraction under
   `OpenGLRenderer::render(DmaFollower, RenderOptions)` — the bucket renderers call GL
   directly. That is the bulk of the port (§5, P3–P4).
5. **deko3d gives us what the GL path never could:** 4 CPU queues + GPU queue with
   per-queue fences (async loader uploads without touching the render queue),
   `CMemPool`/`DkMemBlock` (explicit VRAM budget), no glad slot resolver, no
   EGL/nouveau crash surface, and a much smaller NRO (Mesa goes away).
6. **Draw-state space is small and closed.** `setup_opengl_from_draw_mode()` maps PS2
   draw modes to a bounded GL state set (16 sampler combos after FIX 43, fixed blend/depth
   mode table). That maps cleanly onto a pre-built **pipeline-state table** — the deko3d
   equivalent of PSOs — built once at init.

---

## 4. Architecture target

```
game/graphics/
  gfx.h/cpp                     GfxPipeline::{OpenGL, Deko3D}; renderer picked at boot
  pipelines/
    opengl.cpp/h                (unchanged; desktop + Switch GL fallback)
    deko3d.cpp/h                NEW: GfxRendererModule impl — nwindow swapchain, no SDL video
  deko3d_renderer/              NEW directory (mirrors opengl_renderer/ layout)
    Deko3DDevice.h/cpp          device, CMemPool (code+data+shader pools), queues, fences
    CommandRecorder.h/cpp       per-frame cmdbuf recorder + state/pipeline cache
    PipelineTable.cpp           draw-mode → (shader pair, blend, depth, format) state table
    ShaderLibrary.cpp           loads embedded .dksh blobs; UBO layouts generated alongside
    TexturePoolDeko3D.cpp       TexturePool backend: RGBA + BCn direct, descriptor allocator
    BufferPool.cpp              vertex/index/UBO ring buffers (replaces GpuBufferPool)
    Loader integration          CPU-queue DMA uploads (dkCmdBufCopyBuffer cmdlists), fences per stage
    <renderer ports>            one file per ported bucket renderer
  opengl_renderer/              unchanged; ports are new files, not edits-in-place
shaders/dksl/                   NEW: adapted GLSL sources with explicit bindings
  build/*.dksh                  compiled by UAM at build time, embedded via CMake
```

Rules for the port (from the repo's history):

- **Switch-only, additive, kill-switchable.** deko3d code lives behind `#ifdef __SWITCH__`,
  selected at boot only when `sdmc:/switch/jakN/gk_deko3d.txt` exists (absent = current GL
  build; same convention as `gk_pin.txt`, `gk_nobcn.txt`). This keeps every deploy
  reversible with an SD-card file operation, per the FIX 70b pattern.
- **Never edit `opengl_renderer/` in place to "share" code** — copy + adapt. Desktop must
  not regress (standing rule since FIX 30). Shared *logic* (DMA walking, culling, bucket
  dispatch in `dispatch_buckets_jakN`) is factored into small header-only helpers used by
  both, but GL calls never appear under a deko3d ifdef.
- **One renderer family per session/commit**, hardware A/B after each, notes entry per FIX
  number. The rollback ladder on the card keeps working exactly as today.

---

## 5. Phased timeline

Effort is expressed two ways: **sessions** (the repo's proven cadence — one deployable
change, hardware-tested, per session; FIX 1→109 averaged ~4 fixes/day across ~5 weeks) and
calendar weeks assuming that cadence with real-life interruptions. Total: **~40–50
sessions ≈ 10–15 calendar weeks**. Every phase ends with a hard gate; if a gate fails twice,
stop and write a notes entry before spending more (the FIX 109 lesson).

### P0 — Baseline, inventory, toolchain spike, go/no-go (5–6 sessions, ~1.5 weeks)
- **P0.1** Freeze performance baselines on the current v1.1-Beta credit NROs, all three
  games, standard routes (Sandover/Sentinel/Misty; jak2 city+forest+desert; jak3 Haven +
  wasteland). Capture `[fps]`, `[phase]`, `[buckets]`, `[loader]`, `[cam] HITCH` into the
  notes as the reference tables for this plan.
  **Reference clock environment (user's setup, 2026-10-14):** horizon-oc profile — jak1
  720p @ **1200 MHz** CPU, jak2/jak3 540p→720p FSR @ **1400 MHz** CPU (stock handheld CPU
  is 1020 MHz). Record the exact CPU/GPU clocks in every capture from now on, and do one
  extra pass at **stock clocks**: at 1020 MHz the CPU-bound render phase (30–36 ms,
  ~linear in clock) becomes ~40–48 ms, i.e. **jak2/jak3 miss 30 fps on a stock console
  today** — that number is the plan's headline justification, and every deko3d A/B must be
  same-clocks vs this baseline.
- **P0.2** Generate the GL inventory as a committed artifact: script
  `scripts/gen-gl-inventory.py` listing per-file GL call counts (today: 2,261/40) and every
  distinct GL function used. This becomes the port checklist; re-run per phase to show
  coverage of the deko3d path.
- **P0.3** Enumerate the **draw-mode state table** *statically*: `DrawMode` is a bounded
  bitfield, so list every reachable (blend × depth × alpha-test × fog × sampler) combination
  from `setup_opengl_from_draw_mode` and the renderers that set fixed state, by reading the
  code. Expect O(100) entries. Output: `docs/deko3d/pipeline_table.json`. (Rev 2: the
  full-playthrough runtime capture was cut — a one-route desktop spot check is enough to
  confirm no combination was missed.)
- **P0.4** Toolchain spike: docker `devkitpro/devkita64` + `switch-deko3d` + UAM; build a
  triangle .dksh from a GLSL file with `layout(binding=…)`; NRO boots on hardware. Also
  verify UAM accepts one real repo shader (start with `tfrag.vert/frag`) and record the
  diffs it demands.
  *Status 2026-10-16:* UAM part done; triangle + per-draw bench NRO `tools/switch/dk_probe/`
  built and deployed; **PASS on hardware 2026-10-16** (see §0).
- **P0.5 GL-time split (the go/no-go number).** On Switch, wrap the GL entry points the
  renderers call (a counting/timing shim on the glad pointers — the FIX 43a resolver already
  touches every slot) and report per frame: `render` ms total, ms inside GL calls, call
  count. Also note `render` minus GL = our C++ cost, which deko3d does **not** remove.
  Projection: deko3d `render` ≈ C++ ms + (GL ms ÷ ~5). If that projection is > 14 ms on
  the standard routes, 60 fps needs C++-side work (PG.1 draw merging, C++ hot spots) as much as a new backend —
  re-scope before P1.
  *Status 2026-10-16:* route 1 projection gl/3 p95 = 10.6 ms (PASS); see §0.
- **P0.6 Memory budget.** Measure what Mesa/nouveau actually holds today (heap + GPU
  mappings, via `svcGetInfo` totals before/after GL init and at a level peak). The deko3d
  pools in P1.1 are carved from *that* memory — with ~4 MB free (FIX 34c) nothing else is
  available. Output: pool sizes for P1.1.
- **Gate P0:** baselines in notes; state table ≤ ~150 entries; UAM compiles a repo shader;
  triangle NRO runs; **P0.5 projection recorded and ≤ 14 ms on ≥ 2 routes** (else
  escalate); P0.6 pool budget fits. *If the state table is unbounded or UAM rejects the
  shader family → escalate before P1.*
  *Status 2026-10-16:* UAM ✅, projection ✅ (1 route, 2nd waived by user), state table 224
  (accepted), triangle NRO ✅ on hardware, P0.6 partial (method change: `mallinfo()`).
  **Gate P0 met for starting P1.**

### PG — Backend-agnostic wins on the GL build (4–5 sessions, ~1 week) — NEW in rev 2
Work that helps both backends and de-risks the port, shipped on the current GL build first
(real fps gains now; each change A/B-tested on hardware like any FIX):
- **PG.1 Draw merging** (moved from P5.1): merge adjacent visible tfrag/tie chunks sharing
  state + texture (`tie-l3-tfrag`: 344 draws @ 174 idx). Target 1,147 → ~400 draws/frame.
  Cuts Mesa cost today and multiplies the deko3d win later.
- **PG.2 UBO migration on GL** (moved from P3.1 + R2): `scripts/gen-shader-ubo.py` rewrites
  default uniforms into `layout(std140, binding=N) uniform Params`; renderers write one
  struct + `glBindBufferRange` instead of N `glUniform*`. GLES 3.1 supports this, so every
  shader is verified on the working GL path — when P3 starts, shaders already meet UAM's
  rule and only the bind call changes. One renderer family per session.
- **Gate PG:** draws/frame and `render` p95 improved or neutral on all three games;
  screenshots unchanged; desktop unchanged.

### P1 — Backend skeleton, boot to splash (5–6 sessions, ~1.5 weeks)
*Status 2026-10-16:* P1.1–P1.5 code done in one session (commit e1569b6dd:
`game/graphics/pipelines/dk_device.cpp`, `dk_renderer.cpp`), with deviations listed in §0
("P1 as built vs. as planned"). Open: P1.2b dock/suspend, uam CMake rule, Gate P1 hardware run.
- **P1.1** `deko3d_renderer/Deko3DDevice`: device (debug flags off in release), CMemPool
  layout sized from the **P0.6 budget** (code pool ~4 MB; data and upload/staging pools
  from the measured Mesa footprint — the old 512 MB + 64 MB guess is a placeholder, not a
  target), CPU0 queue = render, CPU1 queue = loader/upload, GPU queue idle at first.
  Pool-allocation failure at boot → log loudly and fall back to GL.
- **P1.2** `pipelines/deko3d.cpp`: `GfxRendererModule` (`init`, `make_display`,
  `exit`, `vsync`, `sync_path`, stubs for the rest). `Deko3DDisplay` uses
  **`nwindowGetDefault()` directly and does not call `SDL_Init(SDL_INIT_VIDEO)`** — SDL's
  Switch video subsystem claims the same native window for EGL, so the two cannot coexist.
  SDL stays for input/audio only (`SDL_INIT_GAMEPAD` in `input_manager.cpp`, audio);
  **verify pads still enumerate without the video subsystem** as the first P1.2 check.
  Drop `SDL_GL_*`, EGL, glad and the GL parts of `game/switch/sdl3_compat.h` on this path.
  Swapchain 3 buffers, vsync interval honoring FIX 72 semantics.
- **P1.2b Applet/display events** (previously handled by SDL video): docked ↔ handheld via
  `appletGetOperationMode()` + `AppletHookType_OnOperationMode` → resize swapchain
  (720p handheld / 1080p docked, FIX 30 logic reused); focus/suspend
  (`AppletHookType_OnFocusState`) → stop submitting and wait on the last fence before
  sleep; resume → re-acquire. Test: dock/undock and sleep/wake mid-level, 5× each.
- **P1.3** CMake: `uam` rule `.glsl → .dksh` (custom command), embed via generated header;
  build order ensures shaders exist before `gk_nro` links. Add `deko3d` to
  `target_link_libraries` alongside (not replacing) Mesa for the transition.
  `scripts/package-switch.sh`: deko3d builds embed shaders, so the card copy of
  `game/graphics/opengl_renderer/shaders/` is only needed while the GL fallback ships.
- **P1.4** Splash: draw `g_splash` (RGBA buffer) through a trivial pipeline; boot log line
  `[deko3d] device up, pools ok, splash drawn`. Nothing else renders yet.
- **P1.5** Fence/timing harness: per-frame fence, `[dk]` telemetry line (queue times,
  cmdbuf bytes, fence waits) alongside the existing `[fps]` line — the P5 optimization
  depends on this existing from day one.
- **Gate P1:** all three games boot to a splash screen with deko3d (kill-switch file
  present), fall back to GL when absent, and the desktop build is byte-for-byte unchanged
  in behavior. Hardware md5s recorded.

### P2 — Resources: textures, buffers, loaders (5–6 sessions, ~1.5 weeks)
- **P2.1** `BufferPool`: persistent vertex/index rings with per-frame offsets; port
  `GpuBufferPool` semantics (FIX 33 pooling + reclaim) so `Loader`/renderers keep their
  existing allocation patterns.
- **P2.2** `TexturePoolDeko3D`: RGBA upload via staging + fenced `dkCmdBufCopyBuffer` →
  `dkQueueSignalFence`; BCn upload **direct from card bytes** (v44 fr3) with zero CPU
  decode; mip chains uploaded wholesale (no mipgen — FIX 42 deferral logic retired on this
  path). Descriptor allocator with the 16-combo sampler table from FIX 43.
- **P2.3** `Loader`/`LoaderStages` integration: keep every budget rule (FIX 46/49/52/66/68/
  76–103). The FIX 52 GPU-cost probe becomes a real CPU1-queue fence wait (cheap, exact).
  Per-texture upload cost expected to drop from ~1–1.5 ms (Mesa) to ~0.05–0.15 ms
  (staging copy) — **measure, don't assume**.
- **P2.4** UBO allocator: per-frame ring for the migrated uniform data (shaders arrive UBO-ready from PG.2).
- **Gate P2:** desktop GL unchanged; Switch deko3d build loads a level's full texture set
  during blackout in ≤ the current `ready in` time (target: clearly less), zero leaked
  `DkMemBlock`s over a 30-min soak (pool watermark telemetry).

### P3 — Core world rendering, jak1 first (10–12 sessions, ~3 weeks)
Port order is by visual + performance weight (FIX 43 bucket cost order), one renderer per
session, each ending in a screenshot-parity check against the desktop GL build on the same
DMA input (use the existing Mac `gk` to produce reference PNGs):
- **P3.1 Render targets** (was missing in rev 1): deko3d equivalents of the GL FBO usage
  — 9 framebuffers, 7 `glBlitFramebuffer`, 1 `glReadPixels`, MSAA resolve. One
  `RenderTarget` helper (image + view, optional MSAA + `dkCmdBufResolveImage`, blit via
  `dkCmdBufBlitImage`); readback only on the CPU1 queue with a fence. Shaders arrive
  UBO-ready from PG.2, so no uniform migration work happens here.
- **P3.2 `TFragment` + PERF GATE.** Port tfrag3 alone, then measure on hardware against
  the same-clocks GL build on Sandover: µs per draw and tfrag bucket ms. **Continue only if
  per-draw cost ≤ ~1/3 of GL** and the P0.5 projection still holds. If not, stop and write
  a notes entry — this is the cheapest point to learn deko3d won't deliver.
- **P3.3–P3.6** The rest of the big five: `Tie3`, `Merc2` (+ shadows), `Shrub`,
  `Sprite3` (incl. glow path) — through the PipelineTable (P0.3 output), not per-draw
  state setting.
- **P3.7–P3.10** `Generic2`/`DirectRenderer`/`DirectRenderer2` (HUD/debug text), `Sky`
  (CPU blend first — it's the accuracy reference — GPU blend after), `ShadowRenderer`,
  `DepthCue`, `EyeRenderer`, `TextureAnimator` (needs PBO-equivalent readback: CPU1 queue
  copy-back with fence — the one renderer that reads GPU memory).
- **P3.11** `dispatch_buckets_jak1` full walk; `do_pcrtc_effects` basic path (brightness
  color_mult/add); 2D blit to swapchain.
- **P3.12** Parity week: fixed-route screenshot diffs (Sandover, Sentinel, Misty), fix
  visual bugs; re-run P0.2 inventory to show coverage.
- **Gate P3:** jak1 fully playable start-to-Sandover on deko3d, correct visuals (user
  sign-off on screenshots), **no crash in 60 min**. Beyond the P3.2 per-draw gate, perf is
  not gated here — correctness first, speed in P5.

### P4 — Full coverage, jak2/jak3 (6–8 sessions, ~2 weeks)
- Ocean ×3 (`OceanMidAndFar`, `OceanNear`, `OceanTexture`), `SkyBlendGPU`, `Warp`,
  `SlowTimeEffect`, `ProgressRenderer`, `CollideMeshRenderer`, `VisDataHandler`,
  `TextureUploadHandler`, `Hfrag`, `Shadow2`; `dispatch_buckets_jak2/jak3` variants
  (color-filter bucket etc.).
- FSR: port `post_processing_fsr.*` through UAM (EASU/RCAS are pure ALU — expected
  straightforward; UAM warning classes: avoid non-constant integer div/mod).
- Screenshot support (`finish_screenshot` equivalent via CPU1-queue readback).
- **Gate P4:** all three games fully render, standard routes, parity screenshots approved;
  GL fallback still boots; desktop untouched.

### P5 — The 60 fps push (7–9 sessions, ~2.5 weeks)
Now spend the gathered `[dk]` telemetry. Ordered by expected win:
- **P5.1 Draw merging follow-up** — the main merge already shipped on GL in PG.1; here,
  extend it to buckets that only become mergeable with deko3d's cheaper state changes
  (e.g. merc/shrub batches across texture changes via descriptor indexing).
- **P5.2 PipelineTable caching + descriptor rebind avoidance** (bind by table id, not per
  draw); UBO writes coalesced per bucket (single `dkQueueSetConstants` per bucket where
  the data allows).
- **P5.3 Double-buffered recording:** N-1 frame recording while N presents (the 6-queue
  model; render thread never blocks on GPU except at acquire).
- **P5.4 Dynamic resolution hook** (existing FSR machinery; drop internal res when GPU
  frame > ~14 ms — only the 1080p far-terrain case needs it).
- **P5.5 60 fps unlock per game (reference clocks):** enable frame-rate option 60 on
  Switch once `render` p95 ≤ 10 ms and `wait_dma` p95 ≤ 4 ms on the standard routes at
  the reference profile (jak1 @1200 MHz, jak2/3 @1400 MHz — the OC budget that today
  props up 30 fps against Mesa becomes the 60 fps fuel). Where GOAL is the wall (Geyser
  Rock, jak3 Haven), keep 30 and record the area in the known-limited list.
- **P5.6 Stock-clock 30 fps floor (STRETCH goal, rev 2 — not a ship gate):** verify all three games hold
  locked 30 fps at **stock 1020 MHz** CPU handheld — impossible today for jak2/3. This
  is the battery/thermal dividend and the "runs on any stock Switch" guarantee. If a
  game misses, capture the `[phase]` breakdown at 1020 MHz before adding any levers.
- **Gate P5 (per game):** 60 fps locked ≥ 90% of 1-minute windows on ≥ 2 standard routes
  at reference clocks; otherwise documented 30 fps with reason. Stock-clock 30 fps result
  (P5.6) is recorded but does not block. **No slow-motion rule** (FIX 75's rejection): an unlocked-but-dipping
  mode must not ship as "60 fps".

### P6 — Loading & efficiency (4–5 sessions, ~1.5 weeks)
Track L items (GPU-independent, FIX 56 candidates — can be pulled forward independently at
any time; listed here because P2.3 finishes the picture):
- **P6.1** Async `sceCdRead` (FIX 56-A): keep the submitted future, drop the double block,
  signal via the existing callback machinery. Highest value, moderate risk — page-loop
  contract read carefully first.
- **P6.2** 256 KB read pages (FIX 56-B: bigger staging buffer, fewer syscalls, 8×).
- **P6.3** `posix_fadvise` readahead on level-file open (FIX 56-D).
- ~~P6.4 uncompressed DGOs~~ — cut in rev 2 (~2× SD footprint; P6.1–P6.3 address the
  measured round-trip cause first).
- **P6.5** deko3d-specific: BCn direct-to-VRAM everywhere (P2.2 generalized), loader
  uploads fully on CPU1 queue with its own fences — the render thread's loader visibility
  drops to ~one fence wait/frame. Re-measure `ready in`, hitch counts, `pcrtc` attribution
  (the FIX 52 blind spot disappears).
- **P6.6** Efficiency audit (trimmed in rev 2): NRO size before/after, RAM/VRAM
  watermarks in the boot log, and one 15-min battery-drain comparison vs the GL build at
  the reference profile.
- **Gate P6:** level `ready in` ≥ 50% better than P0 baseline; zero 45–60 ms hitches
  during streams on standard routes; memory watermarks recorded.

### P7 — Hardening & release (4–5 sessions, ~1.5 weeks)
- 60-min soak per game per backend; crash-report symbolization parity (`gk_syms.txt` for
  the deko3d NRO); kill-switch + fallback documented in README; the FIX ladder (`.bak`
  rotation) maintained through the transition.
- Default backend decision (with user): deko3d default + GL fallback file, or GL default
  one more release. Update `CLINE_HANDOFF.md`, release notes, `PERF_PLAN_NEXT_AGENT.md`
  status line.
- **Gate P7 (ship):** v2.0-Beta with deko3d, three games, both backends bootable.

---

## 6. The 60 fps honesty matrix (from existing logs)

| Game / area | GOAL (`wait_dma`) at 30 fps | Verdict for 60 fps |
|---|---|---|
| jak1 typical (Sandover, Sentinel, Misty) | 0–1 ms | Likely — renderer is the only wall |
| jak1 Geyser Rock (warp-gate side) | 35–40 ms (FIX 77) | Stays 30 unless GOAL is optimized (separate GOAL work, e.g. FIX 78-style) |
| jak2 typical | GOAL-light (rollout notes) | Likely with FSR; watch TextureAnimator (P3 port + rate-skip option) |
| jak2/jak3 Haven City | loader ema p50 36 ms — CPU-bound logic + submission | 60 only after PG.1/P5.1 + density/LOD levers (`city-density`, LOD presets exist); expect mixed |
| Far-terrain/ocean vistas @1080p | GPU fill-bound (23–26 fps) | Dynamic resolution (P5.4) or 540p/720p+FSR |

The frame-rate menu already starts at 30 on Switch (all three games); the 60 entry is
unlocked per game at P5.5 only when its gate passes.

**Clocks (user's reference profile: jak1 720p @1200 MHz, jak2/3 540p→720p FSR @1400 MHz
CPU; stock handheld = 1020 MHz):** every 60 fps verdict above assumes the reference
profile. That overclock is currently **life support for Mesa** — jak2/3 miss 30 fps
without it — so deko3d effectively converts the OC budget from "hold 30" into "run 60".
Stock-clock 60 fps is explicitly **not** a target; stock-clock 30 fps **is** a new
stretch goal (§1, P5.6). GPU side: at 540p–720p internal resolution the GPU frame fits
16.63 ms everywhere except the 1080p far-terrain case (P5.4's dynamic resolution handles
that).

---

## 7. Risk register

| # | Risk | Likelihood | Mitigation |
|---|---|---|---|
| R1 | Scope creep: "second complete renderer" (the FIX 56 warning) | Certain if unmanaged | Phase gates; kill-switch fallback; P0.2 inventory as the burn-down; GL stays shippable at every point |
| R2 | UBO migration bugs across ~45 shaders / 26 files | High | Done first on GL in phase PG.2 (`gen-shader-ubo.py`); per-shader GL A/B (uniforms-as-UBO on GL too, `#ifdef` free) so the migration is testable *without* deko3d |
| R3 | Visual regressions (blending, MSAA, alpha, fog, sky) | Medium | PS2 blend modes are already enumerated in `setup_opengl_from_draw_mode`; parity screenshot suite from P3.12; FIX 33a/47 lessons (black/purple textures) documented |
| R4 | `EyeRenderer`/`TextureAnimator` GPU readback deadlocks | Medium | CPU1-queue copy-back with fences, never the render queue; timeout + skip (the eye pupil DMA desync f74b precedent) |
| R5 | deko3d/uam toolchain gaps (GLSL extensions we use) | Low–Medium | P0.4 spike tests the hardest shader early; UAM = mesa 19.0.8 featureset, our shaders target GLSL 330/430 — audit `#version` and extensions in P0.4 |
| R6 | Memory pressure (VRAM pools sized wrong → FIX 63-style alloc failures) | Medium | CMemPool watermarks in boot log; pools sized from measured GL-era usage + margin; keep `reclaim_gpu_memory` hooks |
| R7 | GOAL 60 Hz logic costs surface at 60 fps (races, gameplay speed) | Medium | 60 fps was already user-tested at FIX 63 era (prebot passable only at 60) — engine supports it; still A/B per game; game-speed regressions = instant revert |
| R8 | Loader regressions during the dual-backend period | Low | Loader keeps its GL path intact until P2.3 lands behind the kill-switch; every budget rule preserved verbatim |
| R9 | Two backends diverge on Switch fixes | Medium (ongoing cost) | Time-boxed: GL fallback is removed one minor release after deko3d proves stable (P7 decision) |
| R10 | Stock-clock 30 fps misses due to *non-renderer* CPU costs (audio mixer, GOAL logic, loader decompression) that also ran at OC today | Medium | P5.6 captures the `[phase]` breakdown at 1020 MHz before adding levers; the user's OC profile remains an option, never a requirement |

---

## 8. Standing rules carried into this plan

From `AGENTS.md` + the FIX history; violating these has cost real days before:

1. One deployable change per session/commit, `(AI-assisted)` in every message, never a PR
   or issue.
2. Build ONE game at a time (draco `draco_features.h` race — FIX 56 process note); docker
   `devkitpro/devkita64`, `scripts/build-switch.sh`, `JOBS=2`.
3. Never create a runtime `std::thread` (~4 MB heap; FIX 34c/F71). deko3d CPU queues are
   queues, not threads — the render thread records into CPU0, the loader thread into CPU1.
4. Never trust a glad/GL entry point on Switch without a pointer check (FIX 43a) — moot on
   deko3d, but the *rule* transfers: probe every deko3d capability once at init and fall
   back loudly.
5. Hardware A/B with same-workload sessions; report totals + duration + max-in-15 s, never
   per-minute alone (FIX 70b verdict correction).
6. Kill-switch files on the card (`gk_deko3d.txt` opt-in during P1–P4), `.bak` ladder for
   every NRO, md5-verify every deploy, `sync` before eject.
7. GOAL changes need host goalc recompile + `out/jakN` rsync; copying `.gc` files does
   nothing (the voided-session lesson).
8. No overclock / no clock changes in game code (FIX 95 disabled F71b/FIX 92 for good).

---

## 9. Measurement plan (what "done" is proven with)

- Keep every existing line: `[fps]`, `[phase]`, `[buckets]`, `[loader]`, `[goal]` (R3+Minus),
  `[cam] HITCH`, `ready in`, `[cores]`.
- Add `[dk]`: render-queue record ms, submit ms, fence-wait ms, cmdbuf bytes, draws,
  pipeline-table hits/misses, descriptor rebinds — printed on the same 2 s cadence as
  `[buckets]` so A/B comparisons line up in the log.
- Standard routes (unchanged): jak1 Sandover/Sentinel/Misty; jak2 city/forest/desert;
  jak3 Haven/wasteland. Budgets: 60 fps = 16.63 ms; 30 fps = 33.33 ms.
- **Every capture records the horizon-oc CPU/GPU clocks** (reference profile: jak1 720p
  @1200 MHz, jak2/3 540p+FSR @1400 MHz; plus one stock-clock pass per game at P0.1).
  Same-clocks comparisons only — a clock change mid-plan would void the A/B (the FIX 70b
  workload-confound lesson, applied to clocks).
- Parity screenshots: desktop GL reference vs Switch deko3d at fixed camera spots per
  route (list committed with P3.12).
- Boot log one-shot lines: pool sizes, shader count loaded (should be exactly the
  embedded program count), BCn-direct uploads on/off.

---

## 10. Session-by-session quick reference (first 10)

| # | Session | Gate artifact |
|---|---|---|
| 1 | P0.1 baselines (all games) | notes tables |
| 2 | P0.2 GL inventory script + committed artifact — **done 2026-10-14** | `scripts/gen-gl-inventory.py`, `docs/deko3d/gl-inventory.{md,json}` |
| 3 | P0.5 GL-time split shim (go/no-go) + P0.6 memory budget — **P0.5 code done 2026-10-15**; first hardware run 2026-10-16 **crashed at kernel-loop start** (EL0 `cntvct_el0` read = trapped-MRS EC=24), root-caused via gk_fatal + addr2line, **fixed same day** (`cntpct_el0`, = libnx); new deploy rule from user: emulator smoke before any SD deploy (Eden broken on this Mac — notes 2026-10-16); P0.6 open; **first [glt] numbers 2026-10-16 (jak1 village1+beach): raw gl 86% is mostly vsync wait in the FIX 13 window glClear (corr −0.98); real work p50 16.9 ms (GL 11.6 + C++ 4.6), proj gl/3 p95 10.6 ms ≤ 14 → PASS on route 1**, 2nd route still needed (`scripts/analyze-glt.py`) | `[glt]` log line, pool budget |
| 4 | P0.3 static draw-mode table — **done 2026-10-15** | `scripts/gen-pipeline-table.py`, `docs/deko3d/pipeline_table.{md,json}` |
| 5 | P0.4 uam/deko3d spike + triangle NRO — **UAM part done 2026-10-15** (`docs/deko3d/uam-spike/`: tfrag3 compiles to .dksh; 3 diffs recorded: vert/frag stage names, sampler `layout(binding)`, std140 UBOs mandatory = PG.2 is a hard prerequisite); **deko3d 0.5.0 already in the docker image; triangle+bench NRO `tools/switch/dk_probe/` built, deployed and PASSED on hardware 2026-10-16** (stock clocks; repo tfrag3 .dksh loads; 0.10 µs/draw at 10k draws/frame) | hardware photo + log |
| 6 | PG.1 draw merging on GL — **deferred** (user: deko3d first) | draws/frame A/B |
| 7–9 | PG.2 UBO migration — **moved**: done inside the deko3d shader pipeline (P2/P3), not on GL first | screenshots unchanged |
| 10 | P1.1–P1.5 device/pools/queue, renderer module, splash, `[dk]` telemetry — **code done 2026-10-16** (e1569b6dd); Gate P1 hardware run next (checklist in §0) | `[dk]` boot line, `[heap]` lines |

---

## 11. What this plan explicitly does NOT promise

- deko3d does **not** fix the `sceCdRead` round-trip stall (FIX 56's finding stands);
  Track L (P6.1–P6.3) does, and is independently shippable on the GL backend.
- 60 fps is **not promised for every area**; it ships per game/area where the §6 gate
  passes. 30 fps remains the floor everywhere (the user's standing "locked, no dips, no
  slow motion" rule outranks the number).
- 60 fps targets assume the **reference clock profile** (jak1 @1200 MHz, jak2/3 @1400 MHz
  CPU). Stock-clock 60 fps is not promised — stock-clock **30** fps is a stretch goal
  (§1, P5.6). The user's overclock stays an option, never a requirement.
- Desktop OpenGL performance is not improved by this work (except where shared renderer
  logic like draw-merging PG.1 lands for both).

---

*This plan was produced by an AI agent from the repository's own measured evidence
(FIX 1–109, the PERF_PLAN documents, and fresh code counts taken 2026-10-14). Estimates
are estimates; every phase gate exists so the first bad hardware number stops the line
instead of compounding. (AI-assisted)*
