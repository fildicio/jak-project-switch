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

| T28 | D P3.1 | `dkrt` depth RT — Z24S8 slice (P3.1 slice 2) | T27 | ✅ reviewed (closed 2026-10-18): impl verified against deko3d.h (Z24S8@428, UsageRender@375, BindRenderTarget@1388, DkMsMode_1x@530); reviewer re-ran check-bg/tail → DONE-PASS `md5=74635895…` = clean-HEAD baseline — the only FAIL was the stale expected `727feb1c…`. Code complete and inert; dependents T29a/T29b are ✅. (Row had stayed 🟡 by oversight after the 2026-10-17 fix.) |
| T29 | D P3.1 | ~~`dkrt` FullTarget + GL Fbo inventory map~~ **✂️ split 2026-10-18** after a 100-min no-output coder loop (too big for one session) — superseded, do not run | T28 | ✂️ split |
| T29a | D P3.1 | GL Fbo inventory **doc only** (`docs/deko3d/gl-fbo-inventory.md`; exact pre-verified file list embedded; ~30-min time box; no build) | T28 | ✅ reviewed 2026-10-18 — doc verified against the code: `FboState`@OpenGLRenderer.h:154, `m_fsr_mid`@150, `make_fbo`@OpenGLRenderer.cpp:1294 (runtime call sites 1709/1726/2309), exactly 9 `glGenFramebuffers` call sites in the 7 files, 6 `glBlitFramebuffer`, 1 `glReadPixels`, GlowRenderer loop@219-220 — all claims match; 13-owner table + 9-vs-13-vs-6 reconciliation correct; zero code files touched. |
| T29b | D P3.1 | `dkrt` FullTarget (color+depth pair, `matches(w,h)`, `bind_views()` factor; pastes T29a's table into the .h) | T29a ✅ | ✅ reviewed 2026-10-18 — full +320/−3 diff reviewed: plain header (slot handle + cached dims only), 32-slot pool w/ proper destroy+zero, dedicated colour+depth image memblocks (`GpuCached\|Image`), 0x1000 rounding = house style (L290/325 vs L117/156), every `dkMemBlockCreate` checked w/ one-line `[dk]` log, depth-failure tears down the colour half, `bind_views()` shared so the T28 depth path is not duplicated; no new deko3d APIs, no threads. Reviewer re-ran check-bg → DONE-PASS `md5=08fc9510745f7c584ab6c8bd73a1b883` = Qwen's Build A = its reverted-HEAD Build B → **provably inert**; the `74635895…`→`08fc9510…` drift is the devkitPro image re-pull (2nd drift: 727feb1c→74635895→08fc9510) and the re-baseline is **approved**: T29b+T30–T38 done-checks + T43's DIFFERENT-check now cite `08fc9510…`. Reviewer notes: free `dkrt::teardown()` doesn't sweep live pool slots (T30 decides caller ownership); Qwen's report quoted a non-verbatim stop-text (actions were correct). See QWEN_REPORT.md `## T29a + T29b — REVIEWER VERDICT`. |
| T30a | D P3.1 | dkrt MSAA image creation (`samples`→2DMS, `matches`+msaa) | T29b ✅ | ✅ reviewed 2026-10-18 — samples 1/2/4/8 validated BEFORE the slot claim (no slot leak); 2DMS+msMode map verified vs deko3d.h (2DMS@359, MsMode@530-533, field@558); depth stays 1x w/ comment; `matches(w,h,msaa)` + cached `m_samples`; grep across game/: zero in-tree FullTarget/dkrt users → signature change safe. Reviewer re-ran check-bg/tail → DONE-PASS md5 `ce41507c…` unchanged (task file cites the stale `08fc9510…`; inert A/B-proven). Verdict: QWEN_REPORT `## T30a + T30b + T31a + T31b — REVIEWER VERDICT`. |
| T30b | D P3.1 | dkrt 1x resolve sibling + `resolve()` + pool-slot sweep | T30a ✅ | ✅ reviewed 2026-10-18 — coder note: `FullTargetSlot` gained `resolveMem/resolveImg/resolveView`; `init` (c.6) creates the 1x 2D RGBA8 sibling (own `GpuCached\|Image` block, `DkImageType_2D`, `DkMsMode_1x`, same 0x1000 rounding as the colour image) when `samples > 1`, with full unwind on create failure; `resolve()` records `dkCmdBufBarrier(cmd, DkBarrier_Full, 0)` then `dkCmdBufResolveImage(cmd, &colourView, &resolveView)` (guards: non-live / 1x → one `[dk]` line + false, never crash); `resolved_view()` returns the sibling as opaque `const void*` (h stays deko3d-free); `fulltarget_free_slot` + `teardown` free the sibling + sweep the whole `g_fulltargetPool` (one `[dk] rt teardown swept N` line; no lock, render-thread only). `matches` left at T30a's 3-arg form (not changed). Build `scripts/ai/check-bg.sh jak1` → DONE-PASS, EL0 audit clean, NRO md5 `ce41507c…` unchanged = provably inert (same re-baselined inert baseline T30a produced; reviewer re-ran the build and reproduced it). Reviewer: every deko3d name re-verified vs the vendored header (ResolveImage@1317, Barrier_Full@339), unwind + pool sweep correct — settles T29b's ownership note; **one deviation APPROVED**: sibling flags `UsageRender\|Usage2DEngine` (task text said Usage_Render only — deko3d.h L378 requires 2DEngine for image-transfer blits, and T31 blits from this image; = the colour image's flags). Verdict: QWEN_REPORT `## T30a + T30b + T31a + T31b — REVIEWER VERDICT`. |
| T31a | D P3.1 | dkrt `blit()` core (flags + barrier + record) | T29b ✅ | ✅ reviewed 2026-10-18 — signature verbatim per spec; flags Linear→`1U<<4`/FlipX→`1U<<0`/FlipY→`1U<<1`/Nearest→0 (deko3d.h L1171-1178), `DkImageRect` z=0 depth=1, barrier(Full,0) FIRST then `dkCmdBufBlitImage` factor 0 (L1316/L1259; = dk_device L606-613 order); null-view guard; .h stays deko3d-free; inert grep clean. Reviewer re-run DONE-PASS md5 `ce41507c…` unchanged. |
| T31b | D P3.1 | blit GL-site grounding table (header comment) | T31a ✅ | ✅ reviewed 2026-10-18 — reviewer re-grepped: exactly 6 real `glBlitFramebuffer` sites (opengl_utils.cpp:244 = comment, gl_time_split_funcs.h = wrapper, BlitDisplays.cpp = none) — the .h table matches the code 1:1; spot-checked site 1 (COLOR+LINEAR full→full) + site 5 (DEPTH+NEAREST → correct OUT-OF-SCOPE row); no stretch+flip site → no T38:verify rows. Count 6 agrees with gl-fbo-inventory.md (the "7" in the task text was the wrong guess). Comment-only; DONE-PASS md5 `ce41507c…` unchanged. |
| T32a | D P3.1 | dkrt readback resources: lazy queue+cmdbuf+staging | T29b ✅ | ✅ reviewed 2026-10-18 — two CpuUncached\|GpuCached memblocks (staging `w*h*4` 0x1000-rounded + 64 KB cmd mem), dedicated `DkQueueFlags_Graphics` queue, every create null-checked w/ one-line `[dk]` failure + FULL unwind; `destroy_readback_resources()` reverse-order (queue→cmdbuf→staging→cmdmem) wired into `dkrt::teardown()`; fast path checks all 4 handles + size; grow tears down all first (no leak). Reviewer re-verified all names vs vendored header (dkQueueCreate@1324 returns opaque `DkQueue` NOT `DkResult` — task text wrong, code right; MakerDefaults@193/252/281, DkQueueFlags_Graphics@261, dkCmdBufAddMemory@1248). Inert grep clean. Reviewer re-run DONE-PASS md5 `ce41507c…` unchanged. Verdict: QWEN_REPORT `## T32a + T32b + T33a + T33b — REVIEWER VERDICT`. |
| T32b | D P3.1 | dkrt `readback_rgba8()` (copy→submit→fence→memcpy) | T32a ✅ | ✅ reviewed 2026-10-18 — guards → ensure (T32a) → fresh cmd slice (`dkCmdBufClear`+AddMemory AFTER the previous call's `dkFenceWait(-1)` → reuse safe) → `dkCmdBufCopyImageToBuffer` flags=0 (`DkImageRect{x,y,z,w,h,d}`={0,0,0,w,h,1} and `DkCopyBuf{addr,rowLength,imageHeight}` field orders verified vs deko3d.h L1190-1204) → finish→submit→signal(flush=true)→wait(-1) = `submit_upload_cmds` order verbatim → memcpy w*h*4. `s_rbFence` zero-init file-static = `s_uploadFence` precedent. Inert grep clean (T40/T4x wire it). Reviewer re-run DONE-PASS md5 `ce41507c…` unchanged. Verdict: QWEN_REPORT `## T32a + T32b + T33a + T33b — REVIEWER VERDICT`. |
| T33a | D P3.2 | tfrag3 dksh pair → append into s_codeMem + overflow check | T25 ✅ + T24 ✅ | ✅ reviewed 2026-10-18 — +32/−0 additive in `dk_device.cpp`: includes (`_dksh` suffix names — real, verified in build-switch-jak1/game/dksh_gen/dksh/), state, post-splash append reusing `load_shader()`; overflow pre-check `v_off=align_up(code_off,0x100); f_off=align_up(v_off+vh.code_sz,0x100); end=f_off+fh.code_sz; end>kCodePool → one line, skip BOTH` — an EXACT mirror of load_shader's align-then-fit math (reviewer checked L167-195; load_shader never advances `*offset` on failure, so the projection holds even after a failed splash half). dksh pair = 1024 B + 1024 B. **md5 moved ce41507c→ba3d9a08 = EXPECTED, escape-clause-approved** (live boot init code; not strippable; no call-graph change). Verdict: QWEN_REPORT `## T32a + T32b + T33a + T33b — REVIEWER VERDICT`. |
| T33b | D P3.2 | tfrag3 shader getters (dk_device_internal.h) | T33a ✅ | ✅ reviewed 2026-10-18 — 3 by-value getters beside the T22b accessors (matches `device_handle()` by-value convention — task doubt answered correctly) + `shutdown()` re-zeros `s_have_tfrag3` (DkShader has no destroy; memblock teardown exists). **md5 moved ba3d9a08→e819b40d**: coder's clean A/B (revert own 2 files → ba3d9a08; re-apply → e819b40d) attributes the whole delta to this diff — reviewer re-ran the build on the final tree and reproduced **e819b40d2be605b19099288c9f215ecf**. Mechanism note: the diff's live `shutdown()` edit alone suffices to move the md5 (uncalled external-linkage fns DO strip — dkrt/T29b-T32b precedent), so the coder's "getters don't strip" conclusion is unproven in mechanism but irrelevant in effect. Verdict: QWEN_REPORT `## T32a + T32b + T33a + T33b — REVIEWER VERDICT`. |
| T34a | D P3.2 | sampler set: data-pool layout + init loop | T16 ✅ | ✅ reviewed 2026-10-19 — in `dk_device.cpp`: (+2 includes `dk_state.h`+`PipelineTable.inc`, +1 `static_assert` + new `kOffSamplers`/`kSamplerCount`/`kSamplersBytes` layout consts after the UBO ring, +16-line init loop appended after the splash descriptor block). 16 samplers = `sizeof(kSamplers)/sizeof(kSamplers[0])` (=16, PipelineTable.inc static-asserted). 0x200 (512 B) fits inside the data memblock's pre-existing 0x1000 rounding slack (0x2B4100 → 0x2B5000 both ways), so no allocation-size change. Inert (nothing reads these samplers until T38). **check-bg.sh jak1 PASS, md5 A/B clean:** pre-T34a `e819b40d2be605b19099288c9f215ecf` (4th baseline, T33a) → post-T34a `3117bb6d8493a963fa09d7a56b0ea7b7`; revert→rebuild reproduced `e819b40d` exactly, restore→rebuild reproduced `3117bb6d` exactly. EL0 audit unchanged (cntpct 3110 / tpidrro 238 / dczid 1). 1 bug used of 2: `const void*→void*` memcpy (sbase was `const uint8_t*`, fixed). Full write-up: `## T34a – PASS (needs reviewer)` in  **REVIEWER VERDICT 2026-10-19: APPROVED** — diff verified vs vendored deko3d.h (`dkSamplerDescriptorInitialize`@1348, `DK_SAMPLER_DESCRIPTOR_ALIGNMENT` 0x20 @L154), offset math + include order checked; QWEN_REPORT `## T34a + T34b + T35a — REVIEWER VERDICT`. |
| T34b | D P3.2 | sampler set accessors + real count verify | T34a | ✅ reviewed 2026-10-19 — in `dk_device_internal.h` (+2 decls `sampler_set_off()`/`sampler_set_count()`) and `dk_device.cpp` (+2 one-liner defs returning `kOffSamplers`/`kSamplerCount`). The `static_assert(kOffSamplers % DK_SAMPLER_DESCRIPTOR_ALIGNMENT == 0)` from T34a already existed (line 97), so step 3 was a no-op. **Real table count verified: 16** (PipelineTable.inc `kSamplers[]`, IDs 0–15, `static_assert(... == 16)` at L248) — FIX 43's "16-combo" label is correct; max sampler id is 15. Inert (uncalled until T38). **check-bg.sh jak1 DONE-PASS, md5 A/B clean & deterministic:** without T34b `3117bb6d8493a963fa09d7a56b0ea7b7` (= T34a's reported md5 exactly) → with T34b `54d0e6405c312834cd819046c0377187` (two independent builds, identical). Move attributes entirely to T34b. EL0 audit unchanged (cntpct 3110 / tpidrro 238 / dczid 1). Full write-up: `## T34b – PASS (needs reviewer)` in  **REVIEWER VERDICT 2026-10-19: APPROVED** — diff verified vs vendored deko3d.h (`dkSamplerDescriptorInitialize`@1348, `DK_SAMPLER_DESCRIPTOR_ALIGNMENT` 0x20 @L154), offset math + include order checked; QWEN_REPORT `## T34a + T34b + T35a — REVIEWER VERDICT`. |
| T35a | D P3.2 | `dkpipe::record_graphics_state` (state binds) | T26 ✅ | ✅ reviewed 2026-10-19 — only `dk_state.h` (+4 decl lines, `record_graphics_state` in `dkpipe`) + `dk_state.cpp` (+48: 1 `switch_run_logf` fwd-decl copied from `dk_device.cpp` L43, 1 null-cb guard, body). Order: `to_deko3d(p,&s)` → `dkCmdBufBindDepthStencilState(&s.ds)` → `ColorState(&s.cs)` → `BlendStates(0,&s.bs,1)` → `apply_blend_const(cb,p.blend)` (T26) → rasterizer defaults + `cullMode=DkFace_None` (lifted verbatim from the real splash template `record_splash_3d` L237-239) → color-write defaults → multisample defaults (1x). GL counterpart on each bind. Inert (uncalled until T38). **check-bg.sh jak1 DONE-PASS, EL0 clean.** NRO md5 moved from the task's stated `e819b40d…` to `833b9139247d58ee72b6e7935d11a392`; **the stated baseline is STALE** — clean current-tree build (no T35a) = `54d0e6405c312834cd819046c0377187` (= **T34b's** reported md5), and `e819b40d` is **T33b's** md5 (T34a→`3117bb6d`, T34b→`54d0e640` are uncommitted working-tree changes). A/B **clean & deterministic** (without `54d0e640` ×2, with `833b9139` ×2, identical); the whole delta 54d0e640→833b9139 is attributable to T35a — same escape clause T33a/T34a/T34b used. Full write-up: `## T35a – PASS (needs reviewer)` in  **REVIEWER VERDICT 2026-10-19: APPROVED WITH 1 FIX** — bind order + API names verified vs splash template; coder BUG: task L52-53 required the null-cb guard to log AND **return**, coder only logged → null cb would fall into `dkCmdBufBind*` — reviewer added the `return` (dk_state.cpp L217-220, log text set to the task's exact `[dk] state null cb`), rebuilt: DONE-PASS **md5 `67018cd5a4411b3f3cc77f198773b261` = 5th baseline** (chain e819b40d→3117bb6d→54d0e640→833b9139→67018cd5; only delta since T35a's build = the 4-line fix). QWEN_REPORT `## T34a + T34b + T35a — REVIEWER VERDICT`. |
| T35b | D P3.2 | `find_for_draw_mode` + sampler_idx mapping | T35a ✅ | ✅ reviewed 2026-10-19: code = task v2 verbatim; reviewer re-ran check-switch jak1 → PASS, 0 warnings, EL0 audit identical (3110/238/1). md5 moved bb560fa7→2f5c9f7f only because the commit stamp (BUILT_SHA) changed — gate now pins it (`OPENGOAL_PIN_REVISION=aigate`), new stable baseline **`c613ba81bb31570d13750a2fd24d07a7`** (proved: same md5 at HEAD 2a666e1 and e5997897) |
| T36a | D P3.2 | TFragmentDk skeleton + verts/indices upload | T21 ✅ T22b ✅ T23 ✅ | ✅ v2 PASS + reviewed: verbatim copy, md5 frozen bb560fa7 (inert TU, A/B clean) |
| T36b | D P3.2 | TFragmentDk time-of-day texture + discard | T36a | ✅ v2 PASS + reviewed: 3 verbatim edits, md5 frozen bb560fa7 (A/B identical) |
| T37a | D P3.2 | dk_tfrag_ubo.h std140 structs + static_asserts (host) | — | ✅ PASS + reviewed; **reviewer fix 2026-10-19:** FragParams was NOT std140 — vec4 `fog_color` must start at 16, not 8. Added `float pad0[2]`; fog_color @16, gfx_hack_no_tex @32 (asserts updated). Header still unused → md5 unaffected |
| T37b | D P3.2 | UBO fill functions (GL math port, host-testable) | T37a | ✅ reviewed 2026-10-19 (md5 c613ba81 reproduced). v2: coder applied `patches/T37b.patch` + host test (T37B-HOST-OK) + build. DONE-PASS md5 `c613ba81bb31570d13750a2fd24d07a7` unchanged; only `dk_tfrag_ubo.h` changed. Needs review |
| T38a | D P3.2 | TFragmentDk `record_frame`: DMA parse (no draws) | T33b,T34b,T35b,T36b,T37b ✅ + soak PASS | ⛔ SUPERSEDED by T60 (reviewer wrote the code) |
| T38b | D P3.2 | TFragmentDk cull + draw recording + AFAIL + counters (stop-ladder) | T38a ✅ + soak PASS | ⛔ SUPERSEDED by T60 (reviewer wrote the code) |
| T39a | D P3.2 | **LIVE** wiring 1/2: chain capture + kill-switch world hook | T38b ✅ + soak PASS | ⛔ SUPERSEDED by T60 (reviewer wrote the code) |
| T39b | D P3.2 | **LIVE** wiring 2/2: LevelData seam + setup_for_level | T39a ✅ | ⛔ SUPERSEDED by T60 (reviewer wrote the code) |
| T40 | D P3.2 | tfrag telemetry (coder) + **HUMAN P3.2 PERF GATE** (≤ ~⅓ GL per-draw, plan L362-365) | T39b ✅ | ⏳ part 2 = user hw run |
| T41a | D P3.3 | Tie3 prep 1/2: vertex format table | T40 gate PASS | ⛔ gated on T40 PASS |
| T41b | D P3.3 | Tie3 prep 2/2: pipeline ids + shader list | T41a | ⛔ gated on T40 PASS |
| T42a | D P3.3 | Shrub prep 1/2: vertex format + `DkVtxBufferState::divisor` layout | T40 gate PASS | ⛔ gated on T40 PASS |
| T42b | D P3.3 | Shrub prep 2/2: pipeline ids + shader list | T42a | ⛔ gated on T40 PASS |
| T43 | D P2.3 | Soak fix: level-texture seam `add_texture` → `dktexgpu::upload_rgba8` on the dk path (LoaderStages still runs GL with no context: tex live=0, jungle "a level is still staging", pool 100 out / 0.0 MB free) | T24 ✅ + soak finding; **run AFTER the T37b inert batch** | ✅ code reviewed, ⚠️ HW 2026-10-19: INERT on dk — dk path never creates/drives a Loader (Loader lives in opengl.cpp) and `dktexgpu::init()` is never called (log `tex -`). The "still staging" lines were from a GL session in the same appended log. Before going live it needs: dk-path Loader ownership + init wiring + a lock (s_cmd/queue shared with present_frame). Earlier: REVIEW FIX: `in.gpu_texture = dk_tex` (was 0; `unload_texture` matches by that id and would ASSERT on level unload). New PASS md5 `65444d85c511f386f98a23462a8ec037`. Needs console soak (human). Coder report: E1/E2 typed verbatim into LoaderStages.cpp (only file touched, +48); done-check DONE-PASS md5 `9d9a85aeadc029f65a227ef07ea6ac98` (LIVE — differs from the trial-built `e49fb3eb…`; per task that diff is expected and "paste and do NOT stop"); 7 pre-existing LoaderStages warnings (all pre-existing, none names an added line). Needs review; user jungle soak after. |

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

**P3 core batch — HALVED 2026-10-18 for the coder model.** After T29's 100-min
no-output loop and T30's 5-h/11-compaction context death (83 reads / 127 cmds /
4 edits / no build), every remaining task was split a/b: ~half the work per
session, one file-pair (or one investigation) each, ~40-min hard time boxes,
verbatim-pasted API lines, forbidden-read lists (deko3d.h, examples, session
notes), exact read ranges, and the done-check spelled as
`scripts/ai/check-bg.sh jak1`. The old whole-task files carry SPLIT banners and
are history only. T30a–T37b are all runtime-inert but the md5 rule changed at T33: **4th baseline `e819b40d2be605b19099288c9f215ecf`** (set 2026-10-18 night; chain 727feb1c→74635895→08fc9510→ce41507c→e819b40d; ce41507c→ba3d9a08 = T33a's live boot-init code, ba3d9a08→e819b40d = T33b's diff incl. its live shutdown() edit — both A/B-attributed, both approved; all pending task files re-baselined). **Rule for every pending task:** file-static uncalled code keeps the md5 frozen (dkrt proof); ANY edit inside a live function (init/shutdown/present) or that adds kept symbols WILL move it — if it moves, do NOT stop for the move itself: A/B-attribute (revert/stash YOUR files → rebuild → compare) and report both md5s; STOP only if the A/B fails to attribute cleanly. Deps per row: T30a/T31a/T32a need only T29b;
T33a needs T25; T34a only T16; T35a only T26; T36a T21/T22b/T23; T37a
independent; each b-half needs its a-half. T38a needs all five b-halves
**plus the P2.3 soak PASS**; T38a/T38b stay ⛔ until then (T38b has an
internal stop-ladder: cull → binds → draw loop → AFAIL). T39a/T39b are the
first LIVE changes since T24 — new md5 expected, run only after the soak
closes. T40 part 1 is coder telemetry, part 2 is the human P3.2 PERF GATE:
**on FAIL, T41a–T42b stay gated and the plan requires a stop-and-notes
entry** (plan L362-365).

---

## MODEL-FIT AUDIT — 2026-10-19 (reviewer)

Model: `qwen3.8-cline` (local MLX, 47 GB, 128K ctx). Evidence across 40+ sessions:

- ✅ PASSES: any task whose code/API/predicate is given verbatim — T34a/b, T35a,
  T35b-v2, T12/T13, the whole P2.3 batch. Typing, building, A/B, reporting: solid.
- ❌ FAILS: any task with hidden design freedom — T30 v1 (5 h death), T35b v1
  (2x compaction death), T36a v1 (3x design loop, 0 files). "Design the struct /
  choose the includes / mirror the GL loop" reliably melts it.

**AUTHORING LAW (every task written from now on):**
1. The REVIEWER writes all new structs, signatures, include lists and loops —
   VERBATIM in the task file. Qwen copies them.
2. A task is done-shaping only when a session could pass with ZERO decisions:
   if any step can be done two reasonable ways, it is not shaped yet.
3. Read lists: only files needed to type the answer; if the facts are in the
   task file, the read list is EMPTY.
4. Time boxes ~20-30 min (typing + build + A/B is ~15 min of work).
5. (2026-10-19) The REVIEWER trial-builds every task before marking it ⏳: apply the
   verbatim code in a scratch clone, run the exact done-check, paste the real md5 and
   warning count into the task. Never hand over an expected md5 you did not see.
6. md5 baselines: the gate pins the git stamp (`OPENGOAL_PIN_REVISION=aigate`), so the
   md5 depends on code only. Current inert baseline: `c613ba81bb31570d13750a2fd24d07a7`.

Audit of pending rows:

| Task | Verdict | Action |
|---|---|---|
| T36a | REWRITTEN v2 | ready (verbatim files) |
| T36b | REWRITTEN v2 | ready (3 mechanical edits) |
| T37a | DONE | std140 padding bug fixed by reviewer |
| T37b | REWRITTEN v2 | ready: pre-tested patch, zero decisions |
| T38a | RESHAPE + likely re-split | verbatim command ladder needed |
| T38b | RESHAPE + likely re-split | verbatim per-pipeline draw sequence |
| T39a | LIKELY OK | exact call sites added when T38 lands |
| T39b | LIKELY OK | same |
| T40 | OK | keep small, verbatim log lines |
| T41a/b | later | table tasks, verbatim after T40 gate |
| T42a/b | later | same |
| T43 | READY v2 | verbatim + reviewer trial build PASS |
| T44 | D PG.2 | jak1 bg shaders (shrub, tfrag3_no_tex, tie_wind) → std140 .glsl via script + uam CMake rule | — | ✅ reviewed 2026-10-19: CMake block verbatim, shaders = converter output, B==A |
| T45 | D PG.2 | Shader survey: converter over all GL shaders + UAM compile → `docs/deko3d/pg2-shader-survey.md` (report only) | — | ✅ reviewed 2026-10-19: doc complete; the 13 UAM-FAILs were a reviewer-script bug (fixed, all now UAM-OK, note appended to the doc). Next PG.2 step: design UBO blocks for the 38 NEEDS-BLOCK shaders from the 'Missing uniforms' list |
| T46 | D | jak2 + jak3 Switch build check (report only, long — overnight OK) | — | ✅ reviewed 2026-10-19 (both NRO md5s re-checked on disk). Coder: both builds PASS (jak2 md5 `85909735…` 770 warn; jak3 md5 `a7504ed1…` 770 warn). No code changed. |
| T47 | D P2.3 | dktexgpu owner-thread guard: uploads/refs off the init thread are refused (apply `patches/T47.patch`) | T43 ✅ | ✅ reviewed 2026-10-19 (diff == patch byte-for-byte, md5 matches trial). Coder: 🟡 2026-10-19: DONE-PASS, md5 `4033aa17966d90f81388690104639633` (matches trial; 1-warn summary line — see QWEN_REPORT T47) |
| T48 | D P2.3 | Wire `dktexgpu::init()` in the dk render loop's first frame + teardown in `dk_exit` (apply `patches/T48.patch`) | T47 | ✅ reviewed 2026-10-19 (diff == patch byte-for-byte, md5 matches trial). Coder: 🟡 2026-10-19: coder done — DONE-PASS, md5 `91e3442e34b00c2a1107ee07995b9565` (exact match to trial; 1-warn SDL_VERSION line expected, not fixed). See QWEN_REPORT §T48. |
| T49 | D P2.3 | Reviewer-direct (cost law): opt-in dk-path Loader behind `gk_dkload.txt` + GL census (`scripts/ai/gen-gl-census.py` → `dk_gl_census.cpp`, no-op glad stubs that log first call) | T48 | ⏸ hardware 2026-10-19 (md5 `9f847ced…`): census installed, then render thread FROZE in the first frame (no `tex id=` line, no splash/telemetry, EE alive to 28 s, no fatal) = first-ever dk texture upload never returned from `dkFenceWait(-1)`. Fix in T51. Logs: `logs-T49-dk/` |
| T50 | D | jak2 + jak3 build re-check after T49 (report only, long; T49 code is already in the working tree) | — | ✅ reviewed 2026-10-19 (md5s re-checked on disk: jak2 `fb717da5…`, jak3 `34662278…`, 1 pre-existing SDL_VERSION warn each; built before T51 — recheck T51 in the next Qwen build job) |
| T51 | D P2.3 | Reviewer-direct: upload hang fix — `DkCopyBuf {addr,0,0}` in upload_rgba8 + readback (imageHeight is a LAYER STRIDE in bytes, deko3d dk_image.cpp L738/L856), no barrier (= working splash upload), `submit_upload_cmds` 1 s timeout → bool + log + uploads disabled, dk-loader step breadcrumbs | T49 | ✅ hardware 2026-10-19 (md5 `eb44a99e…`): no freeze; fence wait timed out after 1 s (`DkResult 2`), then splash + 30 fps frames fine, dk Loader up, levels load (`live=4`, beach+jungle cached), census = glGetError, glGenBuffers, glBindBuffer, glBufferData, glActiveTexture. Logs `logs-T51-dk/`. Timeout → root cause in T52 |
| T52 | D P2.3 | Reviewer-direct ROOT-CAUSE fix: `dkQueueFlush` after every `dkQueueSignalFence` that is waited on (upload, copy_to_gpu, readback). SignalFence's `flush` = GPU cache flush only; deko3d `waitIdle` = signalFence+flush+wait (dk_queue.cpp L359) | T51 | ✅ hardware PASS 2026-10-19 (md5 `94171101…`, 171 s): placeholder + 1163 level textures uploaded via dktexgpu (`tex live=1163 fail=0`, ~20.5 MB of 48 MB, all rgba8), no fence FAILED, 30.0 fps, no fatal; common in 86 ms. Logs `logs-T52-dk/` |
| T53 | D | jak2 + jak3 build re-check after T51+T52 (report only, long) | — | ✅ jak2 `93a5c74f…` jak3 `2a18f3b5…` |
| T54 | D P2.3 | dk buffer pool 8 → 128 MB (+ heap check) and skip `mipq_process` GL on the dk path (apply `patches/T54.patch`) | — | ✅ reviewed 2026-10-19 (diff == patches byte-for-byte, md5s re-checked on disk). Coder: 🟡 coder done 2026-10-19: DONE-PASS, md5 `7df9eef8d51208850ce153a0482c3553` (exact match to reviewer trial); diff-stat = exact 3 files; 0 fixes. See QWEN_REPORT §T54 |
| T55 | D P2.3 | Loader geometry seam: `GpuBufferPool` → dkpool chunks + `upload_to_buffer` → `copy_to_gpu` on the dk path (apply `patches/T55.patch`) | T54 | ✅ reviewed 2026-10-19 (diff == patches byte-for-byte, md5s re-checked on disk). Coder: 🟡 coder done 2026-10-19: DONE-PASS, md5 `af69f0c61a2c1a21d277d3d9a8185d80` (exact match to reviewer trial); diff-stat = exact 4 files (GpuBufferPool.h/LoaderStages.cpp/dk_device.cpp/dk_device.h); 0 warnings from new lines; 0 fixes. See QWEN_REPORT §T55 |
| T56 | D | jak2 + jak3 build re-check after T54+T55 (report only, long) | T55 | ✅ reviewed 2026-10-19 (diff == patches byte-for-byte, md5s re-checked on disk). Coder: 🟡 coder done 2026-10-19: census up to date; jak2 PASS md5 `821c1659bbf146fdf242494e6778e96a` (125 warn, 0 on new lines); jak3 PASS md5 `a24731aca2919cb72bd18287d2e6ce42` (125 warn, 0 on new lines); 0 errors each; no code changed. See QWEN_REPORT §T56 |
| T57 | D P2.3 | dk buffer pool 128 → 384 MB + copy_to_gpu log only first 8 / slow calls (apply `patches/T57.patch`) | — | ✅ reviewed 2026-10-20 (diff identical to patches, md5 re-checked) |
| T58 | D P2.3 | Separate deko3d upload queue for copy_to_gpu + texture uploads (apply `patches/T58.patch`) | T57 | ✅ reviewed 2026-10-20 (diff identical to patches, md5 re-checked) |
| T59 | D | jak2 + jak3 build re-check after T57+T58 (report only, long) | T58 | ✅ reviewed 2026-10-20 (diff identical to patches, md5 re-checked) |
| T60 | D P3.2 | First visible tfrag draw, opt-in via `gk_dktfrag.txt` (apply `patches/T60a/b/c.patch`) | T58 ✅ + soak PASS | ✅ reviewed 2026-10-20 (diff identical to patches, md5 re-checked). **HW: village tfrag renders, right way up, 30 fps** |
| T61 | D | jak2 + jak3 build re-check after T60 (report only, long) | T60 | ✅ reviewed 2026-10-20 (jak2 d54714df / jak3 074182ec match) |
| T62 | D P3.2 | Splash blit FlipY + tfrag trans/dirt/ice passes (buckets 32/34/36/39/41/43) — reviewer-written and committed directly, no Qwen run | T61 | ✅ reviewer 2026-10-20, jak1 md5 `0624560a…` |
| T63 | D | jak2 + jak3 build re-check after T62 (report only, long) | T62 | ⏳ |

Reshaping happens when a task becomes next-in-line (not before — T37+ shape
depends on what T36/T38 actually produce).
