# T107 — dkgl frame hook: ocean GL renderers in the dk frame (buckets 4 + 63)

**Depends on:** T103, T106 (and T101 already merged). **Category:** D dkgl (brief item 3).
**Size:** large (1-2 sessions). **Law:** zero design freedom. Every existing call this task
mirrors is cited by file/line — copy them. The ONE sanctioned `opengl_renderer/` edit is
pasted below verbatim. Anything else → STOP, paste the problem.

## Goal

The Switch-side `Emitter` implementation + `dkgl::init()` + the two frame hooks that run
the UNCHANGED GL ocean renderers (`opengl_renderer/ocean/*.cpp`) inside the dk world
recorder, replacing the `dkocean::record_*` calls, with `gk_dkocean_native.txt` /
`gk_noocean.txt` fallbacks. **No new threads** (brief rule; `std::thread` throws on Switch).

## Create 1 — `game/graphics/deko3d_renderer/dkgl/dkgl_frame.cpp` (~170 lines)

### A. `SwitchEmitter : dkgl::Emitter`

Every method records into `dkdev::cmd_handle()` — **only valid inside the world
recorder**, which is exactly where the hooks sit (recipe #1). Copy each call's spelling
from the cited place, don't retype from memory:

| method | implementation |
|---|---|
| `bind_shaders(id,name)` | `DkShader vsh/fsh` from the init-built name→shader map; `dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, {&vsh,&fsh}, 2)` — recipe #2 |
| `set_depth` / `set_blend` | the same state-bind calls `dkpipe::record_graphics_state` issues (grep them in the dk pipeline file; copy names) |
| `set_viewport` | `dkCmdBufSetViewports(cb, 0, &vp, 1)` — recipe #2 |
| `set_vtx_format` | `dkCmdBufBindVtxAttribState` + `dkCmdBufBindVtxBufferState` + `dkCmdBufBindVtxBuffer(cb, 0, gpu, bytes)` — recipe #2 |
| `bind_idx_buffer` | `dkCmdBufBindIdxBuffer` — recipe #2 |
| `bind_ubo` | slice from `dkdev::ubo_ring()->alloc(0x100, 0x100)` at draw time; `dkCmdBufBindUniformBuffer(cb, stage, 7, gpu, bytes)` — recipe #3 |
| `bind_tex` | `dkCmdBufBindTexture(cb, DkStage_Fragment, binding, dkMakeTextureHandle(dktexgpu::slot_of(dk_tex_id), dkdev::dkgl_sampler_slot(combo)))` — recipe #2 (slot_of: dk_texture_gpu.h:105) |
| `set_primitive_restart` | `dkCmdBufSetPrimitiveRestart(cb, enable, index)` — GL→dk table (recipe #5) |
| `bind_frame_target()` | re-bind the current frame's colour+depth target views — copy the render-target bind the world recorder/present uses (grep `BindRenderTargets` in `game/graphics/pipelines/dk_device.cpp` + `dk_tfrag_record.cpp`; use the frame-target variant) |
| `bind_offscreen_target(tex_id)` | bind the dktexgpu RT as colour target + NO depth — copy how `dk_ocean_flush.cpp` binds its 128×128 RT for the texgen pass (grep `BindRenderTargets` there; that code exists from T95) |
| `clear(color,depth,rgba)` | `dkCmdBufClearColorFloat(cb, 0, DkColorMask_RGBA, r,g,b,a)` and/or `dkCmdBufClearDepthStencil` — recipe #4 |
| `barrier_before_sample()` | `dkCmdBufBarrier(cb, DkBarrier_Fragments, DkInvalidateFlags_Image \| DkInvalidateFlags_Descriptors)` — brief lesson "Render-to-texture" |
| `ring_alloc` | see C below |
| `ubo_alloc` | `dkdev::ubo_ring()->alloc(...)`; fail → return false (draw skipped upstream) |
| `draw_arrays/draw_elements` | `dkCmdBufDraw` / `dkCmdBufDrawIndexed(cb, prim, count, 1, 0, 0, 0)` — recipes |

### B. `dkgl::init(texture_pool, loader)` — called once from dk init, AFTER the census install and AFTER the device is up

Order matters; every failure logs `[dkgl] …` and leaves `active() == false` (native ocean
stays; brief lesson "Fallback"):

1. Gate: `gGameVersion == GameVersion::Jak1` only (jak2/3 keep the native path — T109).
2. Flags, read once with the same fopen probe as the existing `gk_noocean.txt` /
   `gk_dkocean_native.txt` handling (dk_renderer.cpp ~line 110 pattern):
   native requested → inactive; no-ocean → ocean fully off (existing behavior).
3. **Shader-folder pre-check**: `file_util::get_file_path({Shader::shader_folder,
   "ocean_common.vert"})` must exist (Shader.h:10; on Switch FileUtil resolves project
   files under `sdmc:/switch/<game>/`, FileUtil.cpp:169-180). Missing → log
   `[dkgl] init skipped: GL shader folder missing on SD (re-run sd-sync)` → inactive.
   (The `ShaderLibrary` ctor reads ~45 GLSL files; without the folder it dies before the
   hint can matter. File CONTENT is irrelevant — identity comes from the hint, behavior
   from the embedded dksh — only existence matters.)
4. `install_glad()` (T104b/T105) — after the census, overwriting only the dkgl subset.
5. Load the 10 embedded dksh byte arrays (names from the T103 CMake output headers) the
   same way `dk_device.cpp` loads tfrag3 (grep the embedded-dksh load path); build
   the name→`DkShader` map keyed by the 5 program names × {vert, frag}.
6. `set_emitter(&s_switch_emitter)`.
7. Construct the shared state: `SharedRenderState(pool, loader, GameVersion::Jak1)` —
   signature + argument instances copied from how `OpenGLRenderer` builds it (grep
   `SharedRenderState(` in OpenGLRenderer.cpp); use the dk path's OWN TexturePool + Loader
   instances (the ones with the T23/T105 seams — grep how dk_renderer creates/holds them).
8. Construct the ocean renderers EXACTLY as the two registration sites do
   (OpenGLRenderer.cpp:520-534 and :683-692): same names, same bucket ids, same order;
   call `init_textures(pool, version)` on each that the GL sites call it on. dkgl does NOT
   use the bucket-id map — it keeps the (bucket → ordered renderer list) pairs in a tiny
   static array transcribed from those two sites; put the transcription in the report.
   The tbp-8160 registration rides the T105 seam automatically.
9. Log `[dkgl] init ok: programs=5 buckets=<list>`.

### C. Per-frame data ring (`ring_alloc`)

The ocean re-uploads the same VBOs several times per frame (mid/near/texgen) → renaming
per brief lesson. Read `dk_buffer_pool.h` first:
- If `acquire()` hands fence-protected slices safe to call ~10×/frame → use it directly.
- Else add a rotating ring INSIDE `dkgl_frame.cpp`: one `buffer_pool()->acquire()` block
  per frame (4 slices × 256 KiB is the ocean's worst case; justify the size from the
  T91c VU survey numbers), cursor reset at frame start (D below), next frame's block
  acquired only after the previous frame's fence (the pool's own contract — follow
  TFragmentDk.cpp `ring_begin`, line 294, for the fence-safe pattern).
Say which branch you took in the report.

### D. `dkgl::run_bucket(u8* data, int bucket)` + frame bookkeeping

- `frame_begin()` is called when bucket 4 is entered (the first ocean bucket of a frame —
  4 < 63): resets shadows, `barrier_done` flags per texture, ring cursor, counters.
  `was_rendered_to` LATCHES (texture written at bucket 63 is sampled at bucket 4 of the
  NEXT frame — one barrier per frame per texture at first sample is correct + cheap).
- `begin_bucket()` before the first renderer, `end_bucket()` after the last.
- For each renderer of the bucket, in registration order:
  `DmaFollower dma(data, 16 * bucket);` (same construction the dkocean sites /
  `dispatch_buckets` use — OpenGLRenderer.cpp:1846), set
  `srs.buckets_base = data; srs.next_bucket = data + 16 * (bucket + 1);`
  ProfilerNode root + scoped child named like the GL dispatch child (copy the naming from
  the dkocean call site's profiling), then `renderer->render(dma, &srs, &prof);` and
  `ASSERT(dma.current_tag_offset() == srs.next_bucket)` (dispatch parity,
  OpenGLRenderer.cpp:1867). Mismatch → STOP condition, paste the renderer + bucket.
- State handover (brief #8): the dkgl first draw re-emits all state (`begin_bucket`);
  for the return to native, check the code AFTER the bucket-4 site — if the native
  recorder binds its own state per draw (tfrag recipe pattern) nothing is needed; if it
  assumes state, re-issue the site's own state-init block after the dkgl call. Report which.

## Edit 2 — `game/graphics/opengl_renderer/Shader.cpp` (THE one-line hook)

At the top of the file (above the ctor), paste:

```cpp
#if defined(__SWITCH__)
// T107 (AI-assisted): the ONLY opengl_renderer/ edit allowed by the dkgl brief
// (DKGL_HYBRID_BRIEF.md "Rules"). Gives dkgl the program name so its program table
// (dkgl_uniform_table.inc) can key programs by ShaderLibrary name.
namespace dkgl {
void hint_program_name(const char* name);
}
#endif
```

and as the FIRST line of `Shader::Shader(const std::string& shader_name, GameVersion version)`:

```cpp
#if defined(__SWITCH__)
  dkgl::hint_program_name(shader_name.c_str());
#endif
```

## Edit 3 — `scripts/switch/sd-sync.sh`

Add `game/graphics/opengl_renderer/shaders/` (the whole folder, ~45 tiny files) to the SD
deploy so step B3's pre-check passes. Note in the sync script comment: content is never
read for behavior — only existence (identity = hint, behavior = embedded dksh).

## Edit 4 — `game/graphics/pipelines/dk_renderer.cpp` (the hooks)

At every `dkocean::record_*` ocean call site (grep `dkocean::record_` — expected around
lines 270-315 for the bucket-4 world pass and 398-412 / 652-662 / 744-750 for the
texture-generation path; enumerate them in the report):

```cpp
if (dkgl::active()) {
  dkgl::run_bucket(<the same data/bucket args that site passes to dkocean>, <bucket>);
} else {
  dkocean::record_<same call as today>(...);   // unchanged native fallback
}
```

If a site's args don't include raw `data` + `bucket` (e.g. it passes a follower or
pre-extracted pointers), adapt inside `run_bucket` to reconstruct them — copy how that
site built them. Any site where reconstruction is not 1:1 → STOP + report.

## Checks

- `python3 scripts/ai/check-dk.py` → PASS (list every new dk identifier used).
- `scripts/ai/check-switch.sh jak1` → PASS. **This is the first task that compiles the 10
  dksh via uam** — the build log must show all 10 dkgl shaders compiled; uam failure =
  T103 bug → STOP, paste the uam errors.
- Host tests still PASS (dkgl host tests + the rest of the suite).
- `git status` shows exactly: 1 new file (`dkgl_frame.cpp`), edits to Shader.cpp (2 blocks),
  sd-sync.sh, dk_renderer.cpp hooks — nothing else in `opengl_renderer/`.
- No `std::thread`, no new includes of `<thread>` (grep the diff).

## Report

Report: the bucket→renderer transcription, the ring_alloc branch taken (C), which sites
were hooked (line numbers), the handover verdict, check outputs, NRO md5, STOP conditions.
