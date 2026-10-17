# Qwen task queue — deko3d migration (AI-assisted)

One file = one task for the local coder model. The reviewer (GLM / Copilot) checks
each result before the next task starts. Written 2026-10-16, updated 2026-10-17 (Gate P1 hardware PASS — see plan §0).

## How to run a task

From the repo root, in the Qwen terminal:

```sh
cline qwen "Do the task in docs/deko3d/qwen-tasks/T01-splash-shaders.md. Follow it exactly."
```

When Qwen finishes, tell the reviewer: `check T01`. The reviewer reads the diff, runs the
done-check, fixes small issues, and marks the row below ✅. Nothing is committed or
deployed by Qwen.

## Rules for every task (Qwen: read this once)

- Only change the files the task lists. No renames, no reformatting, no extra refactors.
- deko3d API: only names that exist in `docs/deko3d-ref/include/deko3d.h`.
  Patterns: `docs/deko3d-ref/examples/`. libnx: only names the task gives you.
- Never edit `game/graphics/opengl_renderer/` or desktop-only code.
- New C++ files: whole body inside `#if defined(__SWITCH__)` … `#endif` unless the task
  says the file is host-testable. Files that include `<switch.h>` must not include
  `common/common_types.h` (u128 clash) — declare `switch_run_logf` by hand like
  `game/graphics/pipelines/dk_device.cpp` does.
- No `std::thread`, no exceptions, no new dependencies.
- **Shell commands are killed after 30 s** (Cline hard cap; there is NO settings
  key to raise it — checked the CLI config). So never `sleep` more than ~20 s and
  never run a build in the foreground. For `check-switch.sh` you MUST use the
  wrappers — they cannot time out:
  1. `scripts/ai/check-bg.sh jak1` — starts the Docker build in the background,
     returns instantly, prints the log path.
  2. `scripts/ai/check-tail.sh` — repeat until it prints `DONE-PASS` or
     `DONE-FAIL` (each call waits at most ~20 s and shows the log tail).
  No hand-written `nohup … &`, no `tail -f`, no long sleeps. If you ever see
  "Command timed out after 30000ms": only the foreground *wait* died — a
  background build is still running. Poll `scripts/ai/check-tail.sh`; do NOT
  restart the build and do NOT treat the timeout as a failure.
- Run the task's **Done-check** and paste its last lines in your final message.
- If something in the task looks wrong or an API is missing: stop and say so. Don't guess.

## Queue

Status: ⏳ todo · 🟡 coder done, needs review · ✅ reviewed · ⛔ blocked · ⏸ deferred

| ID | Track | Task | Depends on | Status |
|---|---|---|---|---|
| T01 | A shaders | UAM-ready splash shaders | — | ✅ |
| T02 | A shaders | `cmake/EmbedBinary.cmake` (binary → C header) | — | ✅ |
| T03 | A shaders | `cmake/Deko3dShaders.cmake` (uam rule) | T02 | ✅ |
| T04 | A shaders | Wire shader build into `game/CMakeLists.txt` | T01 T03 | ✅ |
| T05 | B 3D splash | Code memblock + shader loading in `dk_device.cpp` | T04 | ✅ |
| T06 | B 3D splash | Descriptor sets + UBO area in the data pool | T05 | ✅ |
| T07 | B 3D splash | 3D splash draw behind `kSplash3D` (off) | T06 | ✅ |
| T08 | human | HW test of T07 (`kSplash3D=true`) + HOME/sleep/resume + dock/undock | T07 + Gate P1 ✅ | ⏳ NRO ready: md5 `ad789291…`, `~/Desktop/jak bakcups/gk.T08-splash3d.ad7892914d7d.nro` |
| T09 | C applet | Log docked/handheld changes | — | ✅ |
| T10 | C applet | Stop presenting while out of focus | T09 | ✅ |
| T11 | C applet | Swapchain resize on dock/undock | T10 + reviewer decision | ⏸ deferred 2026-10-17 (decision in task file) |
| T12 | D P2 prep | `dk_ring.h` per-frame ring allocator (host-testable) | — | ✅ |
| T13 | D P2 prep | gtest for `dk_ring.h` | T12 | ✅ |
| T14 | D P2 prep | `PipelineTable.h` plain structs | — | ✅ |
| T15 | D P2 prep | `scripts/gen-dk-pipeline-table.py` → `.inc` | T14 | ✅ |
| T16 | D P2 prep | `dk_state.cpp`: plain entry → deko3d state structs | T15 | ✅ |
| T17 | D P2.1 | `dk_buffer_pool.h` FIX 33 pooling on a fixed region (+gtest) | — | ✅ 7/7 gtests (reviewer fixed 2 structural bugs, see QWEN_REPORT) |
| T18 | D P2.2 | `dkdev::copy_to_gpu`: staging copy + fence upload primitive | — | ✅ reviewer build DONE-PASS md5 `25748a7b…`, inert (0 callers) |
| T19 | D P2.2 | `dk_texture_pool.h` texture store skeleton (+gtest) | — | ✅ 8/8 gtests (reviewer-rerun) |
| T20 | D P2.4 | UBO ring wired into dk_device (512 KB × 2 frames) | — | ✅ reviewer build DONE-PASS md5 `c7f2f0ba…`, segment_size() proxy verified vs dk_ring.h |
| T21 | D P2.3 | BufferPool region: dedicated 8 MB memblock in `dk_device` | — | ✅ reviewer build DONE-PASS md5 `4d972e57…`, make_block reuse verified, inert |
| T22a | D P2.3 | `dk_device_internal.h` + dk-TU pass-throughs | T21 | ✅ reviewer build DONE-PASS md5 `169d722f…` (identical), APIs verified L1328/29/1240, inert |
| T22b | D P2.3 | dk_texture_gpu: store + DkImage + RGBA8 upload | T22a | ✅ reviewed 2026-10-17: slot_of + dk_texture_gpu.cpp (172 L) + CMake entry; coder DONE-PASS md5 `6847e804…`; reviewer re-verified every deko3d name vs vendored header, diff vs task skeleton, inert grep |
| T22c | D P2.3 | dk_texture_gpu: BCn direct upload | T22b | ✅ reviewed 2026-10-17: upload_bcn in dk_texture_gpu.{h,cpp} (tight-pack `{gpu,0,0}` CopyBuf, 4-fmt map, Rgba8 reject); DONE-PASS md5 `6847e804…` (= T22b — gc-sections strips the uncalled fn, reviewer confirmed `--gc-sections` in switch.specs); no callers |
| T23 | D P2.3 | TexturePool seam: GOAL texture path → dk behind kill-switch | T22c | ✅ reviewed 2026-10-17: one-branch seam in `upload_to_gpu` (cpp:69–81) + `dk_id` member in `GpuTexture` (h:182–187), both inside `__SWITCH__`, desktop untouched; DONE-PASS md5 `3616142b…` (first NRO change — the seam CALLS upload_rgba8, so it is no longer inert at runtime) |
| T24 | D P2.3 | Telemetry soak: tex/pool/ring stats + 2-min hw protocol | T23 | ✅ reviewed 2026-10-17: `[dk] tex … pool … ring …` line in `dk_renderer.cpp` render() report window (piggybacks the fps line's guard, null-safe per subsystem); DONE-PASS md5 `727feb1c8496d3941c32123710de7b18` (0 fixes; reviewer reproduced); **HW 2-min soak = user step, closes P2.3** |
| T25 | D P3.0 | UAM shader build rule: tfrag3 .glsl → .dksh in CMake (PG.2 vehicle) | T24 ✅+soak | ✅ reviewed 2026-10-17: `scripts/gen-dksh.py` + two tfrag3 `add_custom_command`s in the `if(NINTENDO_SWITCH)` splash block; DONE-PASS NRO md5 `727feb1c…` (inert — headers not #include'd); **determinism gate reviewer-reproduced**: build `tfrag3.vert.dksh` `316acbd2…` / `tfrag3.frag.dksh` `debcdd3e…` byte-identical to spike; EL0 audit clean. Ran before the P2.3 soak closed — harmless, output NRO is byte-identical to T24's |
| T26 | D P3.0 | `dkpipe::apply_blend_const` FIX64_SUB at bind time (deko3d.h:1293) | T24 ✅+soak | ✅ reviewed 2026-10-17: new `dk_state_internal.h` (T22a precedent) + `apply_blend_const` in `dk_state.cpp` — exhaustive no-default switch over all 7 Blend values (reviewer checked the enum), only Fix64Sub → `dkCmdBufSetBlendConst(cb,0.5,0.5,0.5,0.5)`; DONE-PASS NRO md5 `727feb1c…` (inert); grep clean |
| T27 | D P3.1 | `dk_render_target`: color RT create/bind/destroy (slice 1) | T24 ✅+soak | ✅ reviewed 2026-10-17: new `dk_render_target.{h,cpp}` (RGBA8, `UsageRender|Usage2DEngine`, own 0x1000-rounded `GpuCached|Image` memblock, image at offset 0, `dkCmdBufBindRenderTarget(cmd,&view,nullptr)`, never make_block/buffer_pool) + CMake entry; DONE-PASS NRO md5 `727feb1c…` (inert, 0 callers); const-qualifier fix on free fns valid (task text was wrong, not the code) |

| T28 | D P3.1 | `dkrt` depth RT — Z24S8 slice (P3.1 slice 2) | T27 | ⏳ |
| T29 | D P3.1 | `dkrt` FullTarget (color+depth pair) + GL Fbo inventory map | T28 | ⏳ |
| T30 | D P3.1 | `dkrt` MSAA + resolve (`DkMsMode`, `dkCmdBufResolveImage`) | T29 | ⏳ |
| T31 | D P3.1 | `dkrt` blit helper — maps the 7 GL blit sites | T29 | ⏳ |
| T32 | D P3.1 | `dkrt` CPU-queue readback (dedicated queue + fence) | T29 | ⏳ |
| T33 | D P3.2 | tfrag3 dksh pair → code memblock + internal getters | T25 ✅ + T24 ✅ | ⏳ |
| T34 | D P3.2 | 16-combo sampler descriptor set appended to data pool | T16 ✅ | ⏳ |
| T35 | D P3.2 | `dkpipe::record_graphics_state` + `find_for_draw_mode` | T26 ✅ | ⏳ |
| T36 | D P3.2 | `TFragmentDk::setup_for_level` (verts/idx/tod → BufferPool + dktexgpu) | T21+T22b+T23 ✅ | ⏳ |
| T37 | D P3.2 | tfrag3 std140 UBO structs + pure fill (host-testable header) | T20 ✅ | ⏳ |
| T38 | D P3.2 | `TFragmentDk::render` — DMA parse + cull + draw recording | T33,T34,T35,T36,T37 ✅ + soak PASS | ⛔ until P2.3 soak verdict |
| T39 | D P3.2 | **LIVE** wiring: chain → world recorder in present_frame, kill-switch | T38 ✅ + soak PASS | ⛔ until P2.3 soak verdict |
| T40 | D P3.2 | tfrag telemetry (coder) + **HUMAN P3.2 PERF GATE** (≤ ~⅓ GL per-draw, plan L362-365) | T39 | ⏳ part 2 = user hw run |
| T41 | D P3.3 | Tie3 prep: pipeline ids + vertex format + shader plan | T40 gate PASS | ⛔ gated on T40 PASS |
| T42 | D P3.3 | Shrub prep: + instancing (`DkVtxBufferState::divisor`) layout | T40 gate PASS | ⛔ gated on T40 PASS |

Tracks A/C/D are independent: any ready task can go next. **P2.3 batch: T22a → T22b →
T22c → T23 → T24, one fresh session each** (T22 was split 2026-10-16 after a model-level
timeout on the single big session — slices are T17-sized; API cheat-sheets are embedded,
reviewer-verified). Review each ✅ before the next. T24 last, then the user's 2-minute
hardware soak closes P2.3. T08 is the human hardware test (deploy together with the T24
soak; the SD is currently on the GL path — `gk_dk3d.txt` removed). T11 deferred.

**P3 opening batch (written 2026-10-17, grounded — runs only after T24 ✅ + the 2-min
soak closes P2.3): T25 (UAM .dksh build rule, byte-exact vs the spike artifacts),
T26 (FIX64_SUB blend const, tiny), T27 (color render target, P3.1 slice 1) — T25 and
T26 are independent of each other, T27 independent of both; any order, one fresh
session each.** Later P3.1 slices (depth, MSAA resolve, CPU1-queue readback) and the
P3.2 tfrag3 draw task get written when these land.

**P3 core batch (T28–T42, written 2026-10-17, all grounded — API cheat-sheets
verified line-by-line against the vendored deko3d.h, T17-sized slices, one fresh
session each).** T28–T32 finish P3.1 (all inert: T28→T29 chain; T30/T31/T32 each
depend on T29 only and are independent of each other). T33–T38 build the tfrag3
machinery (all inert — expected NRO md5 stays `727feb1c…` until T39). T33 needs
T25; T34 needs only T16; T35 only T26; T36 T21/T22b/T23; T37 is independent; T38
needs T33–T37 **plus the P2.3 soak PASS** (live-behavior rule). T39 is the first
LIVE change since T24 — new md5 expected, run only after the soak closes. T40
part 1 is coder telemetry, part 2 is the human P3.2 PERF GATE: **on FAIL, T41/T42
stay gated and the plan requires a stop-and-notes entry** (plan L362-365). T41/T42
are inert prep headers gated on that PASS.
