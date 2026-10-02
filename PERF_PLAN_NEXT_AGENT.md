# Performance plan for the next agent (AI-assisted)

From Copilot CLI session f38c5887-9439-4af3-ba03-cb4fa5be2b79. Read `CLINE_HANDOFF.md` and
`SWITCH_PORT_SESSION_NOTES.md` (FIX 13, 30/31, 39a, 42, 60, 63) first.
Repo fildicio/jak-project-switch, branch `slows-down-new-area` (HEAD `bf2dd91e2` or later).

## Goal
Stable 30 fps at 720p and shorter loads on Switch for jak1/jak2/jak3.

## Rules
- Build ONE game at a time, never in parallel. Use docker `devkitpro/devkita64` and `scripts/build-switch.sh`.
  - Build dirs: `build-switch-jak1-f61`, `build-switch-jak2-f60`, `build-switch-jak3-f61`.
- GOAL changes need a host goalc recompile:
  `./build-host/goalc/goalc --game jakN --instruction-set arm64 --cmd '(make-group "iso")'`
  - First export `DYLD_LIBRARY_PATH` with every `build-host` dir that contains a `.dylib`.
  - Diff `out/jakN/iso` against the SD card to find which files changed.
- One commit per step. The message ends with "(AI-assisted)" plus the trailer
  `Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>`.
- Add a notes entry per step (FIX 64, 65, ...). Never create PRs or issues.
- Don't deploy to the SD card until the user confirms. When deploying, keep `.bak` copies and md5-verify.
- Every Switch-only change goes behind `#ifdef __SWITCH__` or `(= (pc-get-os) 'switch)`.

## What we know
- jak2 is GPU-fill-bound: the user reports 960x540 makes Haven City much smoother.
- jak3 runs well everywhere except Haven City. There, 540p barely helps (loader ema p50 36 ms, p90 42 ms).
  So jak3 Haven is CPU-bound: GOAL logic plus draw submission.
- Threads (game/runtime.cpp ~562): DMP, IOP, EE, EE-Worker via SystemThread
  (game/system/SystemThread.cpp:130). Other threads:
  - the render/main thread (game/graphics/pipelines/opengl.cpp)
  - the loader thread (game/graphics/opengl_renderer/loader/Loader.cpp:26)
  - audio threads
- There is no core affinity code anywhere. On libnx, std::thread uses the default core, so
  probably EVERYTHING runs on core 0.
- playlog shows cpu_cores=3. Use only cores 0-2; core 3 is reserved for the OS.

---

## Step 1: Thread core diagnostics + pinning (do first; ~1 h; all games)
1. Diagnostic: each thread logs `svcGetCurrentProcessorNumber()` and its core mask once, plus every 10 s
   (to `gk_run_log.txt`, tag `[cores]`). Also log the process core mask via `svcGetInfo(InfoType_CoreMask)`.
2. Pinning, in a helper in `game/switch/platform.cpp`, e.g. `switch_pin_current_thread(role)`:
   - EE (GOAL logic) -> core 0
   - render/main thread -> core 1
   - loader thread, EE-Worker, IOP, audio, DMP -> core 2
   - Use `svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1<<core)`. Log the result codes.
   - Call it at the start of each thread function (SystemThread bootstrap by name, loader thread,
     audio threads, and the main thread before the render loop).
3. Build jak3 first. The user tests Haven City.
   - Compare the `[loader] ... ema` p50/p90 and the "Kernel dispatch time" count with the previous log
     (p50 36.4 ms, p90 42.3 ms, 233 frames > 50 ms).
4. Risk: true parallelism can expose data races that the single core hid. Watch `gk_fatal.txt`
   and crash reports. If a race shows up, fix it properly. Don't fall back to one core silently.
5. Then build jak2 and jak1.

### Step 1 FINAL VERDICT (corrected 2026-10-02 ~01:40 after F70b A/B) -- pinning NEUTRAL
- Initial verdict ("F70 regressed hitching, 88-99/min vs 4-70/min") was a
  workload confound: short pure-streaming boots vs longer mixed-play boots.
- Proper same-workload A/B on jak3: F70 pinned = 224 hitches / 98.7 min^-1 /
  worst 2964 ms; F70b floating = 275 / 106.3 min^-1 / worst 2985 ms. Same
  45-60 ms hitch band, no underruns, no fatals. Pinning changes nothing.
- A pre-F70 8-min boot had 277 total hitches vs F70b's 275 in 2.6 min:
  streaming produces ~250-275 hitches per session on EVERY build F62..F70b.
- Decision: pinning stays OPT-IN via `sdmc:/gk_pin.txt`, default off; the
  `[cores]` diagnostics stay always-on (they cost nothing and identified
  this). FIX 70b deployed to all three games.
- Hitch-rate metric rule for steps 2+: compare same-area sessions and report
  duration + total hitches + max-in-15 s; per-minute alone misleads.

## Step 2: CPU boost during loads (~1-2 h; all games)
- Call `appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad)` (CPU 1785 MHz, GPU lowered) while a blackout
  load / `update_blocking` / loader backlog is active, and `ApmCpuBoostMode_Normal` afterwards.
  - Hook where the loader budget mode becomes `blackout` (Loader.cpp `update_frame_budget`) and where it
    ends ("Blackout loads done").
  - Never leave boost on during gameplay: the GPU clock drops.
- Log the transitions with the tag `[boost]`. Measure the "level X ready in N s" times before and after.

## Step 3: Even 30 fps pacing (~half a day; all games)
- vsync is currently off (`[vsync] requested=0`; game/graphics/pipelines/opengl.cpp ~1245).
- When target-fps is 30 on Switch, use swap interval 2 (SDL_GL_SetSwapInterval(2) / eglSwapInterval)
  so frames are evenly spaced.
  - Keep the FIX 13 deferred-clear ordering.
  - Verify with the `[cam] dt` lines: they should cluster at ~33.3 ms.
- Make sure GOAL still sees the correct frame rate (`*pc-settings* target-fps`, display time-factor).

## Step 4: FSR 1.0 upscaler in the resolution menu (~1 day; all games, main win for jak2)
- Use the full spec in the prompt in this session's chat (summary below).
- Add two new entries to the resolution menu in jak1/jak2/jak3: "540p -> 720p (FSR)" and "720p -> 1080p (FSR)".
  - A new pc-setting `upscaler`, plus a kernel function `pc-set-upscaler`.
  - The resolution list lives in `game/system/hid/display_manager.cpp` ~547.
  - The menus are in `goal_src/jak1/pc/progress-pc.gc` ~1227 and
    `goal_src/jak{2,3}/pc/progress/progress-pc.gc` ~682 + `progress-static-pc.gc`.
  - Setting changes flow through `goal_src/jak1/pc/pckernel-common.gc` (shared).
- Replace the bilinear quad in `OpenGLRenderer::do_pcrtc_effects()` (~2211) with EASU; RCAS is optional.
  - Keep the brightness color_mult/add.
  - Cache the constants; recompute only when sizes or dock mode change.
  - Create intermediate FBOs lazily, never during a stream (FIX 39a).
- The swapchain may be 1080p even in handheld (FIX 30). Clamp the output to the panel size
  (`get_display_size_for_operation_mode`).
- HUD at native resolution: assessment only, don't implement.

## Step 5: Haven City traffic density (jak3, optionally jak2; ~half a day)
- The traffic code is in `goal_src/jak3/levels/city/traffic/traffic-manager.gc` (~534 target-count,
  ~590 'set-object-target-count) and `traffic-engine.gc` (~1600 `set-object-target-count`).
  jak2 has the equivalent under `goal_src/jak2/levels/city/traffic/`.
- On Switch, scale the vehicle/citizen target counts by a pc-setting `city-density`
  (default 0.6; options 1.0 / 0.8 / 0.6 / 0.4).
  - Never scale counts that missions require (guards / mission vehicles): check `object-type` and the
    'set-guard-target-count-range path, and keep mission-spawned objects untouched.
- Optional: an Options menu carousel entry in jak2/jak3.
- Measure Haven frame times before and after.

## Step 6: Switch performance preset (LOD / draw distance; ~half a day)
- `lod-force-tfrag` / `lod-force-tie` already exist in pc-settings (pckernel-common.gc,
  `pc-renderer-tree-set-lod`).
- Add a "Switch performance" preset: forced lower tie/tfrag LOD and a shorter draw distance.
  - Optionally apply it only in city levels.
  - Check the visual impact with the user.

## Step 7: Pre-compressed textures (several days; biggest win for loading and GPU memory)
- The Switch GPU (Tegra X1) supports BC1/BC3/BC7 (and ASTC).
- Compress textures and their full mip chains offline at extract time into the fr3 data, then upload with
  `glCompressedTexImage2D`.
  - Removes runtime mip generation (FIX 42 deferral).
  - Cuts upload bytes and VRAM 4-8x.
  - Reduces the GPU OOM risk (the FIX 63 jak2 crash).
- Needs changes in the decompiler/extractor (fr3 writer), `tfrag3::Level` serialization (version bump),
  and the Loader texture upload path. Keep a fallback to uncompressed RGBA.
- Write a design note first; get user approval before implementing.

## Long term (don't start): native deko3d backend
- It would replace OpenGL/Mesa and greatly cut the per-draw CPU cost (jak3 Haven).
- That's months of work. Mention it only in the notes.

## Recommended order
1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7. After each step the user tests on hardware before you continue.
Report per step: changed files, md5s of the NROs/CGOs/DGOs, and before/after numbers from the logs.
