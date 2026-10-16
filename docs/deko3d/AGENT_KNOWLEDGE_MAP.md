# Repo knowledge map for the deko3d migration (AI-assisted)

Read on demand: open the section you need, then the files it points to.
Facts verified against the code on branch `deko3d-migration`, 2026-10-16.
If code and this map disagree, the code wins. Fix this map in the same commit.

Contents: 1 Big picture · 2 GOAL → C++ frame handoff · 3 Renderer module contract ·
4 OpenGL renderer · 5 Shaders · 6 GL state → deko3d · 7 Textures · 8 Loader & level data ·
9 Threads, timing, telemetry · 10 Switch platform · 11 Build · 12 Deploy & test ·
13 GOAL recompile · 14 Docs index · 15 Hard-won lessons · 16 Porting recipe

---

## 1. Big picture

```
GOAL game code (goal_src/jakN/engine/...)  builds PS2-style DMA chains in buckets
        │  send_chain(data, offset)            sceGsSyncPath / sceGsSyncV shims
        ▼
GfxRendererModule (game/graphics/gfx.h)   selected at boot: OpenGL or Deko3D
        │
        ├─ OpenGL: pipelines/opengl.cpp → OpenGLRenderer::render(DmaFollower, RenderOptions)
        │          → dispatch_buckets_jakN → ~25 BucketRenderer subclasses → GL/Mesa → NVN
        └─ Deko3D: pipelines/dk_renderer.cpp + dk_device.cpp → deko3d → NVN
                   (Phase 1: splash + pulse only; chains received but not drawn yet)
```

The renderer's job: walk the DMA chain the game sends each frame, and for each bucket
run the matching renderer, which turns PS2 GS/VIF data + preloaded level data (fr3)
into draws.

## 2. GOAL → C++ frame handoff

- GOAL bucket construction: `goal_src/jak{1,2,3}/engine/dma/dma-bucket.gc`; producers
  under `goal_src/jakN/engine/gfx/` (`background/`, `foreground/`, `tfrag/`, `tie/`,
  `merc/`, `sprite/`, `ocean/`, `hw/`).
- C++ mirror of bucket IDs + `BucketCategory`: `game/graphics/opengl_renderer/buckets.h`
  (BucketCategory near line 1075). Bucket IDs differ per game.
- PS2 sync shims: `game/graphics/sceGraphicsInterface.cpp`
  - `sceGsSyncPath` → `Gfx::sync_path()`: DMA-consumption barrier, called twice per
    frame. Renderer must finish reading the chain before returning. Time spent here
    is the `wait_dma` number in `[fps]`.
  - `sceGsSyncV` → `Gfx::vsync()`: the real frame boundary.
- DMA reading: `DmaFollower` in `common/dma/dma_chain_read.h` (renderer gets it by
  value, bucket renderers by `DmaFollower&`). `DrawMode` is also defined there.
- deko3d today: `dk_send_chain` stores one pending chain; render thread acknowledges
  it and advances `frame_idx` (`dk_renderer.cpp`). Chain decoding = P2/P3 work.

## 3. Renderer module contract (`game/graphics/gfx.h`, `gfx.cpp`, `display.h`)

- `GfxPipeline { Invalid = 0, OpenGL, Deko3D }`.
- `GfxRendererModule` callbacks: `init(GfxGlobalSettings&)`, `make_display(w, h, title,
  settings, GameVersion, is_main)`, `exit()`, `vsync()`, `sync_path()`,
  `send_chain(const void*, u32)`, `texture_upload_now(const u8*, int, u32)`,
  `texture_relocate(u32, u32, u32)`, `set_levels`, `set_active_levels`,
  `force_reload_all`, `force_reload_level`, `force_reload_common`, `set_pmode_alp(float)`,
  plus `pipeline` and `name`. Deko3D implements all; most are stubs in Phase 1.
- Selection (`gfx.cpp` ~156–225): on Switch, deko3d is tried only if
  `sdmc:/switch/<game>/gk_dk3d.txt` exists; any init failure falls back to OpenGL in
  the same boot.
- `GfxGlobalSettings` (gfx.h ~60–113): `game_res_w/h`, `lbox_w/h`, `msaa_samples`,
  brightness/contrast, `vsync`, `target_fps`, `framelimiter`, tfrag/tie LOD, `hack_no_tex`.
- `GfxDisplay` (`display.h`): `get_display_manager()`, `get_input_manager()`,
  `render()`, `init_splash()`, `draw_splash(int,int)`.
- deko3d display: SDL **video is never initialized** (`SDL_INIT_EVENTS` + gamepads),
  `DisplayManager(nullptr)` / `InputManager(nullptr)`, sizes via `set_native_window`,
  events via SDL pump + `switch_platform::applet_pump()`. Anything that assumes an SDL
  window must be guarded on this path.

## 4. OpenGL renderer (`game/graphics/opengl_renderer/`): the thing being ported

- Entry: `OpenGLRenderer::render(DmaFollower dma, const RenderOptions& settings)`
  (`OpenGLRenderer.h:74`). `RenderOptions` (h:20–63): game res, msaa, window/draw region,
  screenshot, PMODE alpha, brightness/contrast, `gpu_sync`.
- Dispatch: private `dispatch_buckets` → `dispatch_buckets_jak1/2/3` (h:79–82); renderers
  registered by `init_bucket_renderers_jakN()` via `init_bucket_renderer<T>(name,
  category, id, ...)` (h:90–107).
- End of frame: `do_pcrtc_effects` (cpp ~2216: PMODE alpha, brightness/contrast, post),
  `blit_display`, `finish_screenshot`.
- `SharedRenderState` (`BucketRenderer.h:26–94`): `ShaderLibrary`, `TexturePool`,
  `Loader`, DMA addresses (`buckets_base`, `next_bucket`, `default_regs_buffer`), camera
  matrices/planes/fog, vis data, framebuffer/draw region, game version, frame index,
  eye renderer, stencil/deferred-clear state, `no_multidraw`.
- `BucketRenderer` (`BucketRenderer.h:97–116`): `render(DmaFollower&, SharedRenderState*,
  ScopedProfilerNode&)` and `draw_debug_window()` are pure virtual; `empty()`,
  `init_shaders(ShaderLibrary&)`, `init_textures(TexturePool&, GameVersion)` virtual.
  Helpers: `RenderMux`, `EmptyBucketRenderer`, `SkipRenderer`, `PrintRenderer`.
- Renderer families (port order per plan §5 P3/P4):

| Renderer | Files | Draws |
|---|---|---|
| `TFragment` | `background/TFragment.*` | world terrain (tfrag3) |
| `Tie3` (+`Tie3AnotherCategory`, `Tie3WithEnvmapJak1`) | `background/Tie3.*` | instanced props, etie, wind |
| `Shrub` | `background/Shrub.*` | shrubs/billboards |
| `Hfrag` | `background/Hfrag.*` | jak2/3 heightfield terrain |
| `Merc2` (+`Merc2BucketRenderer`) | `foreground/Merc2*` | skinned characters/models |
| `Generic2` | `foreground/Generic2*` (`Generic2_OpenGL.cpp` = GL part) | generic geometry |
| `Shadow2`, `ShadowRenderer` | `foreground/Shadow2*`, `ShadowRenderer.*` | shadows (stencil) |
| `Sprite3` (+distort, glow) | `sprite/` | particles, sprites, glow |
| `DirectRenderer`, `DirectRenderer2` | top level | GS-direct prims: HUD, text, debug |
| `SkyRenderer`, `SkyBlendCPU/GPU` | top level | sky + blended sky textures |
| `OceanNear`, `OceanMidAndFar`, `OceanTexture` | `ocean/` | ocean |
| `EyeRenderer` | top level | eyes (render-to-texture + readback) |
| `TextureAnimator`, `TextureUploadHandler` | top level | animated textures, GOAL uploads |
| `DepthCue`, `Warp`, `SlowTimeEffect`, `BlitDisplays`, `ProgressRenderer`, `CollideMeshRenderer`, `VisDataHandler` | top level | effects / special passes |

- Draw conventions: TFragment/Tie3/Shrub/Hfrag/Generic2/Sprite3/ocean use indexed
  `GL_TRIANGLE_STRIP` + primitive restart, `glMultiDrawElements` when allowed
  (TFragment.cpp ~470–560, Tie3.cpp ~621–766, Shrub.cpp ~339–437). Merc2 (~1128–1358)
  uses triangles or strips per `draw.no_strip`. deko3d: `dkCmdBufSetPrimitiveRestart`
  exists, check the header for the exact signature.
- GL helpers: `opengl_utils.h`: `FramebufferTexturePair(Context)`, `FullScreenDraw`,
  `FullScreenTexDraw`, `FramebufferCopier`. `Fbo.h`.

## 5. Shaders

- 94 files in `game/graphics/opengl_renderer/shaders/` (47 `.vert/.frag` pairs incl.
  `splash`). `ShaderId` enum + `ShaderLibrary`: `Shader.h:46–105`.
- `Shader::Shader(name, GameVersion)` (`Shader.cpp:51`) substitutes `HEIGHT_SCALE`
  (1.0 jak1 / 0.5 jak2-3), `SCISSOR_HEIGHT` (448.0 / 416.0), `SCISSOR_ADJUST`.
  On Switch (`Shader.cpp:71–92`) `#version 410` → `#version 310 es` + `precision highp`.
- Uniforms: `gl_uniform_loc(program, name)` cache (`Shader.cpp:28–47`) + `glUniform*`
  everywhere (~300+ sites). For deko3d these become std140 UBO structs (UAM rejects
  loose uniforms, see `docs/deko3d/uam-spike/README.md`, tfrag3 already converted there).

## 6. GL state → deko3d pipeline state

- `setup_opengl_from_draw_mode(DrawMode, u32 tex_unit, bool mipmap)`:
  `background/background_common.cpp:107`. Maps blend, depth test/write, alpha test,
  clamp, filter/mipmap; returns `DoubleDraw` (alpha-test second pass info).
- The full set of distinct states is enumerated in `docs/deko3d/pipeline_table.{md,json}`
  (224 pipelines, 16 sampler combos). deko3d equivalents: `DkBlendState`, `DkColorState`,
  `DkDepthStencilState`, `DkRasterizerState`, `DkSampler`. Alpha test does not exist as
  fixed function: it is a `discard` in the fragment shader (the GLSL already does this).
- Every GL call site is listed per file in `docs/deko3d/gl-inventory.{md,json}`; regenerate
  with `scripts/gen-gl-inventory.py` to show port coverage.

## 7. Textures

- `game/graphics/texture/TexturePool.{h,cpp}`: `GpuTexture`, `TextureReference`, slots;
  `give_texture`, `give_texture_and_load_to_vram`, `update_gl_texture`,
  `get_gpu_texture_for_slot`. `upload_to_gpu` is GL-specific (cpp ~39–62).
- GOAL texture data: `goal_src/jakN/engine/gfx/texture/texture{,-h}.gc`; arrives via
  module `texture_upload_now` / `texture_relocate` (deko3d: ignored in Phase 1).
- Level textures: `loader/LoaderStages.cpp` uploads RGBA (`glTexImage2D`) and BCn
  (`glCompressedTexImage2D` ~646, ~931). fr3 v44 carries BC1/BC3 payloads (FIX 74).
  On deko3d, BCn uploads directly (`DkImageFormat_RGB_BC1`, `RGBA_BC3`, `RGBA_BC7_Unorm`).
- `TextureAnimator` (render-to-texture, CLUT blends) and `EyeRenderer` (FBO + readback)
  are the two renderers that read GPU output back: highest sync risk.

## 8. Loader & level data

- Format: `common/custom_data/Tfrag3Data.h` (`tfrag3::Level`, Tfrag/Tie/Merc/Texture
  structs), `TFRAG3_VERSION = 44`. Producers: `decompiler/level_extractor/`,
  `goalc/build_level/`.
- `loader/Loader.h:22`: `update`, `update_blocking`, `get_tfrag3_level`,
  `get_merc_model`, `load_common`. Owns `m_loader_stages` and `GpuBufferPool m_buffer_pool`.
- `loader/LoaderStages.{h,cpp}`: `LoaderStage`, `MercLoaderStage`, `LoaderFrameBudget`.
  Budgets: `TIE_LOAD_BUDGET 1.5f`; `SHARED_TEXTURE_LOAD_BUDGET` 3.0 desktop / 1.5 Switch
  (`Loader.h:25–34`). Switch adds adaptive budgets, GPU-cost probe, blackout loading,
  memory-pressure reclaim (`Loader.h:129–175`). Keep every budget rule when porting (plan P2.3).

## 9. Threads, timing, telemetry

- `game/runtime.cpp` (~530–604): graphics init on the main thread before GOAL boots;
  then DMP, IOP, EE, EE-worker threads; main thread runs `Gfx::Loop` (= render thread,
  pinned to core 1 on Switch, `gfx.cpp` ~239–245). Core 0 EE/GOAL, core 2 loader/IOP/audio
  (`game/switch/platform.cpp`).
- Log lines (all via `switch_run_logf` → `sdmc:/switch/jakN/gk_run_log.txt`):
  `[fps]` (`pipelines/opengl.cpp:1225`: avg, wait_dma, render, swap), `[phase]`
  (`OpenGLRenderer.cpp:1513`: setup/loader/buckets/blit), `[buckets]` (per-bucket ms,
  draws, idx), `[glt]` (GL time split, `gl_time_split.cpp`, enabled by `gk_glt.txt`),
  `[dk]` (deko3d: fps, frame/wait/record/submit ms, chains, pool KB, heap), `[heap]`.
- Analyzers: `scripts/analyze-glt.py`.

## 10. Switch platform (`game/switch/`)

- `platform.cpp/.h`: applet lifecycle, operation mode/display size, memory queries,
  thread pinning, crash/exception support. `run_log.h`: `switch_run_logf` (cannot be
  included next to `<switch.h>`, u128 clash; declare by hand as `dk_device.cpp` does).
  `boot_log.h`, `safe_stdout.h`, `sdl3_compat.h`, `link_bases.h`.
- Toolchain: `cmake/toolchains/Switch.cmake` (defines `__SWITCH__`, A57 flags,
  `-fsigned-char`, `NINTENDO_SWITCH`). `game/CMakeLists.txt` ~371–399 links `deko3d` + `nx`.
- Option files (presence = on): card root `gk_pin.txt`, `gk_nopbo.txt`, `gk_pbo.txt`,
  `gk_nodecode.txt`, `gk_decode.txt`, `gk_nodedup.txt`, `gk_noclk.txt`, `gk_nowarm.txt`,
  `gk_nobcn.txt`, `gk_log_host.txt`; per game `switch/jakN/`: `gk_dk3d.txt` (deko3d
  opt-in), `gk_no_vag.txt`, `gk_no_pf.txt`, `gk_glt.txt`.

## 11. Build

```sh
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest \
  env SWITCH_GAME=jak1 BUILD_DIR=/work/build-switch-jak1 JOBS=2 bash scripts/build-switch.sh
```
- Mount at `/work` is required (CMake caches were created there). One game at a time.
- Output `build-switch-jakN/game/gk.nro`. Image contains deko3d 0.5.0, UAM
  (`/opt/devkitpro/tools/bin/uam`), and the deko3d examples.
- Before an SD deploy: objdump audit of EL0 system-register reads (the class that
  crashed the first `[glt]` NRO; plan §0 "Hardware-deploy rule").
- One command does build + audit: `scripts/ai/check-switch.sh jakN [--no-build]`.
- `scripts/package-switch.sh` builds a clean SD tree (NRO + `out/jakN/{iso,obj}` + assets + shaders).

## 12. Deploy & test

- Card layout: `sdmc:/switch/jakN/jakN.nro` (deploy `gk.nro` renamed). Rotate the old
  one to `Jak N.fNN.bak` first, `sync`, md5-verify everything. Desktop copies in
  `~/Desktop/jak bakcups/`. On the Mac the card mounts as `/Volumes/SWITCH SD`.
- Launch through hbmenu with title takeover (full memory), not applet mode.
- Logs: `sdmc:/switch/jakN/gk_run_log.txt` (appends), `gk_stdout.txt` (last boot only),
  `gk_boot_log.txt`, `gk_fatal.txt` (symbolize with `scripts/analyze-goal-crash.py`),
  GOAL logs `data/log/`. Silent crash: also `atmosphere/erpt_reports`.
- A/B rules: delete stale `gk_*.txt`, same route + duration + clocks, report totals.
- Boot-critical, never touch: `/atmosphere/`, `/bootloader/`, `boot.dat`, `exosphere.ini`.

## 13. GOAL recompile

`./build-host/goalc/goalc --game jakN --instruction-set arm64 --cmd '(make-group "iso")'`
(or `"kernel"`/`"engine"`), then copy changed `out/jakN/{iso,obj,fr3}` to
`sdmc:/switch/jakN/data/out/jakN/`. Copying `.gc` sources does nothing.

## 14. Docs index (read when…)

| File | Lines | Read when |
|---|---|---|
| `AGENTS.md` | 49 | always: AI disclosure, no PRs/issues |
| `AI_ROLES.md` | ~34 | always: planner/coder split |
| `DEKO3D_MIGRATION_PLAN.md` | 565 | always §0 (live status); phase sections when starting one |
| `docs/deko3d/pipeline_table.md` | 119 | building pipeline state / PipelineTable |
| `docs/deko3d/gl-inventory.md` | 273 | porting a renderer file (its GL call list) |
| `docs/deko3d/uam-spike/README.md` | 51 | any shader work |
| `docs/deko3d-ref/` | n/a | any deko3d API use (headers + examples, not committed) |
| `SWITCH_PORT_SESSION_NOTES.md` | 7.2k | grep for a FIX number or subsystem; never read whole |
| `PERF_PLAN_NEXT_AGENT.md`, `performance-improvement.md` | 173 / 366 | perf work, measurement method |
| `STEP7_COMPRESSED_TEXTURES_DESIGN.md` | 247 | BCn / texture upload work |
| `JAK2_JAK3_ROLLOUT_PLAN.md`, `JAK3_CARRYOVER_FROM_JAK2.md` | 178 / 134 | jak2/jak3 specifics |
| `CLINE_HANDOFF.md` | 1.2k | card naming/deploy conventions only; status inside is from 2026-10-03 (older branch), superseded by the plan §0 |
| `tools/switch/dk_probe/` | n/a | standalone deko3d test NRO (logs to `sdmc:/switch/dk_probe/`) |

## 15. Hard-won lessons (grep the FIX number in `*.md` at repo root; most are in `SWITCH_PORT_SESSION_NOTES.md`)

- FIX 34c/35: never create a runtime `std::thread`; ~4 MB heap headroom, pthread stack alloc fails.
- FIX 43a: never call a GL/driver entry point without checking it exists (deko3d: check every create result).
- FIX 44: devkitA64 `char` is unsigned unless `-fsigned-char` (toolchain sets it; don't rely on `char` sign).
- FIX 56: deko3d does not fix loading stalls (`sceCdRead` round trips); that is Track L.
- FIX 63/64: GPU allocation failures and stray `.bak` files in scanned ISO folders break boots.
- FIX 65/66: Docker builds serially, `JOBS=2`, md5-verify every deploy.
- FIX 67: diagnostics must not block (buffered stdout sink).
- FIX 70b: compare A/B runs on identical workloads; per-minute numbers alone mislead.
- FIX 74: fr3 v44 BCn payloads; FIX 90/91 loader BCn→RGBA decode was a Mesa workaround.
- FIX 77: 60 fps needs GOAL (`wait_dma`) ≤ 16.6 ms too; renderer speed alone is not enough.
- FIX 95: city slowdown fixed via loader stream budget, without touching CPU/GPU clocks.
- Plan §0: the old "render 30.7 ms" was mostly vsync wait; real GL work ≈ 11.6 ms p50 (jak1).

## 16. Porting recipe (one renderer = one session)

1. Read the GL renderer's `.h/.cpp` and its row in `docs/deko3d/gl-inventory.md`.
2. Convert its shaders: explicit bindings, uniforms → std140 UBO; compile with UAM.
3. Create `deko3d_renderer/<Name>Dk.{h,cpp}` (new file; never edit the GL one). Reuse
   pure logic (DMA parsing, culling) via small shared helpers without GL calls.
4. Map each `setup_opengl_from_draw_mode` result to a pipeline-table entry.
5. Buffers: per-frame ring in a `CpuUncached|GpuCached` memblock; respect alignments.
6. Build jak1, objdump audit, deploy with `gk_dk3d.txt`, compare a screenshot with
   the GL build at the same spot, check `[dk]` timings and heap lines.
7. Update plan §0 + notes (next FIX number), commit with `(AI-assisted)`.
