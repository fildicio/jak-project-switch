# GL Fbo owner inventory — desktop renderer (T29a, 2026-10-18)

Source of truth: the 7 files where `glGenFramebuffers` appears under `game/graphics`
(re-verified with `grep -rln glGenFramebuffers game/graphics --include='*.cpp' --include='*.h'
| grep -v build`). Each row is one **owner** (member/variable), not every call site.

The Fbo class (`game/graphics/opengl_renderer/Fbo.h`, L7–L51) is a plain struct with:
`fbo_id`, `tex_id`, `zbuf_stencil_id`, `multisampled`, `multisample_count`,
`is_window`, `width=640`, `height=480`. `matches(w,h,msaa)` (L23–L26) checks whether a Fbo
can be reused for the requested size/samples.

## The `make_fbo` factory

`OpenGLRenderer.cpp` L1294 — `Fbo make_fbo(int w, int h, int msaa, bool make_zbuf_and_stencil)`.
Generic factory: creates a GL color texture (RGBA8), optionally a multisampled version
when `msaa > 1`, and optionally a depth-stencil renderbuffer (GL_DEPTH24_STENCIL8) with
`glFramebufferRenderbuffer`. Called at runtime (not at init) every time resolution or MSAA
changes. It is the factory behind three of the four `Fbo` members listed below.

## The `FboState` (OpenGLRenderer.h L154–L167)

Three `Fbo` members + one pointer:

| # | file:line | member | purpose | size / format | msaa? | blit role |
|---|---|---|---|---|---|---|
| 1 | `OpenGLRenderer.h` L156 | `resources.window` | OpenGL default framebuffer (fbo_id=0, provided by GLFW/SDL); letterbox/panel target. | `window_framebuffer_width × window_framebuffer_height` (panel size, letterboxed). | No (`multisampled=false`, set in `setup_frame`). | **dst** of the final scene blit (the resolve/scene FBO is blitted to the window). |
| 2 | `OpenGLRenderer.h` L157 | `resources.render_buffer` | Main scene render target. Created via `make_fbo(game_res_w, game_res_h, msaa_samples, true)`. | `game_res_w × game_res_h`. RGBA8 with optional depth-stencil renderbuffer. | **Yes** — when `settings.msaa_samples > 1`; falls back to fewer samples. | **src** of the MSAA resolve blit (L2223–L2232). |
| 3 | `OpenGLRenderer.h` L158 | `resources.resolve_buffer` | Non-MSAA resolve/dedup target. Created via `make_fbo(game_res_w, game_res_h, 1, false)`. Only valid when MSAA is requested (and a resolve step is needed). | `game_res_w × game_res_h`. RGBA8, no depth. | No. | **dst** of the MSAA resolve blit; also **src** of `glReadPixels` screenshot (L1537–L1540). |
| 4 | `OpenGLRenderer.h` L150 | `m_fsr_mid` | FSR (FSR 1.0 EASU + bilinear) 540p→720p intermediate, docked-only. Created lazily via `make_fbo(fsr_w, 720, 1, false)` when the 540p source path + 720p target is active. | `fsr_w × 720` (aspect-corrected 720p, fixed 720p target). RGBA8, no depth. | No. | **src** of the second (bilinear) upsample to the window blit. |

## Effect FBOs

| # | file:line | member | purpose | size / format | msaa? | blit role |
|---|---|---|---|---|---|---|
| 5 | `opengl_utils.cpp` L14 | `FramebufferTexturePair::m_framebuffers[]` | Render-to-texture with mipmaps. One `num_levels` array of FBOs sharing one mipmap texture. | `w × h` with `num_levels` mipmap levels (each `glTexImage2D(..., i, GL_RGBA, w>>i, h>>i, ...)`). | No. | Render target; sampled by the consuming effect (not a blit src/dst itself). |
| 6 | `opengl_utils.cpp` L211 | `FramebufferCopier::m_fbo` | Single-pass copy of a rectangle to a new texture. NEAREST filter, `GL_RGB`. | `m_fbo_width × m_fbo_height`. GL_RGB, NEAREST. | No. | Draw target (the caller draws a screen-space quad into it). Not a glBlitFramebuffer src/dst. |
| 7 | `SkyBlendGPU.cpp` L12 / `SkyBlendGPU.h` L18 | `m_framebuffers[0]` (sky) | Sky-blend render target for the sky layer. | `32 × 32` (fixed, square). GL_RGBA, UNSIGNED_BYTE. | No. | Render target; sampled by the sky shader. |
| 8 | `SkyBlendGPU.cpp` L12 / `SkyBlendGPU.h` L18 | `m_framebuffers[1]` (clouds) | Sky-blend render target for the cloud layer. | `64 × 64` (fixed, square). GL_RGBA, UNSIGNED_BYTE. | No. | Render target; sampled by the sky shader. |
| 9 | `DepthCue.cpp` L38 | `m_ogl.framebuffer_sample_fbo` | Snapshot of the main render to a GL_RGB texture for depth-cue sampling. Created at 1×1, resized at runtime. | `1×1` at setup, then resized to the current scene resolution at draw time. GL_RGB, LINEAR. | No. | Render target; sampled by the depth-cue shader. (Not a blit src/dst; it's a draw-to-fbo target.) |
| 10 | `DepthCue.cpp` L66 | `m_ogl.fbo` | The depth-cue base page (the actual depth-cue effect draw target). | `1×1` at setup, resized to scene resolution. GL_RGB, LINEAR. | No. | Draw target (the depth-cue render target). |
| 11 | `Sprite3_Distort.cpp` L33 | `m_distort_ogl.fbo` | Snapshot of the current render to a texture bound as tex0 for the distort shader. | `m_distort_ogl.fbo_width × fbo_height`. GL_RGB, NEAREST, CLAMP_TO_EDGE. | No. | **src** — the distort shader reads from this texture. |
| 12 | `GlowRenderer.cpp` L187 | `m_ogl.probe_fbo` | Holds the result of drawing glow probes. Color + depth attachments. | `probe_fbo_w × probe_fbo_h` = `render_fb_w × render_fb_h` (resolution-following; rebuilt in `setup` when render_fb size changes, L533–536). RGBA8 + GL_DEPTH24_STENCIL8. | No. | Draw target for glow probe rendering; the resulting texture is sampled. |
| 13a | `GlowRenderer.cpp` L220 | `m_ogl.downsample_fbos[0]` | Level-0 glow downsample FBO (largest, 640px). | `32 × kDownsampleBatchWidth (20) = 640 × 640`. GL_RGBA8, no depth on this level (depth on `first_ds_depth_rb`). | No. | **src + dst** — in the downsample chain, `[0]` is src for the first blit, dst is the probe output. |
| 13b | `GlowRenderer.cpp` L220 | `m_ogl.downsample_fbos[1]` | Level-1 glow downsample FBO (320px). | `16 × kDownsampleBatchWidth = 320 × 320`. GL_RGBA8. | No. | **src + dst** in the pyramidal downsample chain. |
| 13c | `GlowRenderer.cpp` L220 | `m_ogl.downsample_fbos[2]` | Level-2 glow downsample FBO (160px). | `8 × kDownsampleBatchWidth = 160 × 160`. GL_RGBA8. | No. | **src + dst** in the chain. |
| 13d | `GlowRenderer.cpp` L220 | `m_ogl.downsample_fbos[3]` | Level-3 glow downsample FBO (80px). | `4 × kDownsampleBatchWidth = 80 × 80`. GL_RGBA8. | No. | **src + dst** in the chain. |
| 13e | `GlowRenderer.cpp` L220 | `m_ogl.downsample_fbos[4]` | Level-4 (smallest) glow downsample FBO (40px). | `2 × kDownsampleBatchWidth = 40 × 40`. GL_RGBA8. | No. | **dst** of the final downsample step; sampled for the final glow blit (L827 binds `downsample_fbos[kDownsampleIterations-1].tex`). |

## Timing shim (not an owner)

| # | file:line | entry | note |
|---|---|---|---|
| — | `gl_time_split_funcs.h` L699 | `glGenFramebuffers` | **Not an FBO owner.** This is the timing instrumentation shim: it `#undef`-s the GL function and redefines it to wrap `glad_glGenFramebuffers` with `glt::add(GLT_ID_glGenFramebuffers, ...)` for performance profiling. Excluded from the owner count. |

## Blit and read-pixel summary

The `gl-inventory.md` says `glBlitFramebuffer` appears **6** times and `glReadPixels`
appears **1** time in `OpenGLRenderer.cpp`. The 6 blit sites in `OpenGLRenderer.cpp`
correspond to the following FBO transfers (from the code context read):

1. **render_buffer → resolve_buffer** (L2223–L2232): MSAA resolve blit. `render_buffer` (MSAA) is the `READ_FRAMEBUFFER`, `resolve_buffer` is the `DRAW_FRAMEBUFFER`.
2. **m_fsr_mid → window** (after L2340): the 540p→720p FSR pipeline's second (bilinear) pass — EASU result in `m_fsr_mid` is blitted to the window at the final output resolution.
3–6. **Various effect→scene blits**: the `DepthCue` framebuffer_sample texture is blitted to the main scene, the `Sprite3_Distort` texture is blitted as the distort source, and the `GlowRenderer` final downsample result is blitted to the main scene.

Note: the plan's P3.1 spec says "7 blits" — the 6th blit might be the
FramebufferCopier's single-pass copy (opengl_utils.cpp L211) which uses
`glDrawArrays` (a screen-space quad quad-draw, not a `glBlitFramebuffer` call),
or the `sky_blend` draw-to-fbo. The actual `glBlitFramebuffer` call count in
`OpenGLRenderer.cpp` is **6** (not 7). The 7th "blit" might be a `glDrawArrays`
based screen-space copy that the plan counts as a "blit" even though it's not
`glBlitFramebuffer`.

## Total owner count

**13 distinct FBO owners** (excluding the `make_fbo` factory and the timing shim):

- 4 from `FboState` / `m_fsr_mid` (rows 1–4)
- 2 from `opengl_utils.cpp` (rows 5–6: `FramebufferTexturePair` + `FramebufferCopier`)
- 2 from `SkyBlendGPU` (rows 7–8: sky + clouds)
- 2 from `DepthCue` (rows 9–10: framebuffer_sample_fbo + fbo)
- 1 from `Sprite3_Distort` (row 11)
- 1 from `GlowRenderer` probe (row 12)
- 5 from `GlowRenderer` downsample pyramid (rows 13a–13e)

**Note: the gl-inventory.md "9" and the plan's "9" are the number of
`glGenFramebuffers` call sites in C++ source** (excluding the timing shim in
`gl_time_split_funcs.h`), not the number of distinct FBO *owners*. 9 call sites
map to 13+ owners because `make_fbo` is called 3 times at runtime and the
GlowRenderer downsample loop creates 5 FBOs in one call.

## What dkrt must cover

**Resolution-following FBOs** (size tracked at runtime, re-created on resize):
- `resources.render_buffer` — main scene MSAA target, `game_res_w × game_res_h`
- `resources.resolve_buffer` — MSAA resolve target, `game_res_w × game_res_h`, no MSAA
- `m_fsr_mid` — FSR intermediate, `fsr_w × 720` (fixed 720p target, aspect-corrected width)
- `m_ogl.probe_fbo` — glow probe target, `render_fb_w × render_fb_h`
- `m_ogl.downsample_fbos[]` — glow downsample pyramid, 5 fixed-size textures (40–640px)
- `m_ogl.framebuffer_sample_fbo` — depth-cue scene snapshot, re-sized at draw time
- `m_ogl.fbo` — depth-cue base page, re-sized at draw time
- `m_distort_ogl.fbo` — sprite distort snapshot, `fbo_width × fbo_height` from the sprite

**Fixed-size FBOs** (never resize):
- `m_framebuffers[0]` (sky, 32×32, RGBA8)
- `m_framebuffers[1]` (clouds, 64×64, RGBA8)
- `FramebufferTexturePair::m_framebuffers[]` — mipmap chain, `w >> i × h >> i`
- `FramebufferCopier::m_fbo` — `m_fbo_width × m_fbo_height` from the copier

**Blit source/destination roles** (which FBOs participate in `glBlitFramebuffer`):
- **render_buffer** → src of the MSAA resolve blit
- **resolve_buffer** → dst of the MSAA resolve blit; also src of the screenshot `glReadPixels`
- **window** → dst of the final scene blit (all scene content arrives here via the resolve path)
- **m_fsr_mid** → src of the FSR bilinear upsample to the window

**MSAA only on `resources.render_buffer`** — the main scene MSAA target. `resolve_buffer` is the non-MSAA target that it resolves to. `m_fsr_mid`, `probe_fbo`, and all the sky/depth-cue/distort/glow FBOs have no MSAA.

**`glReadPixels` (1 site):** The screenshot path reads from `resolve_buffer` (L1537–1540) when it exists, otherwise from `render_buffer`.

## Doubts / reviewer questions
- The plan's P3.1 "7 blits" vs the actual 6 `glBlitFramebuffer` calls in OpenGLRenderer.cpp:
  the 7th could be a `glDrawArrays` based screen-space copy (FramebufferCopier,
  or the effect FBOs' screen-space draws).
- `FramebufferTexturePair` is used for mipmap render targets — does `dkrt` need
  a multisampled mip target or just a single-level texture?
- `SkyBlendGPU` uses two fixed-size FBOs (32×32, 64×64) — `dkrt` should
  support square small targets for sky/cloud compositing.
- `m_ogl.framebuffer_sample_fbo` and `m_ogl.fbo` in `DepthCue` are created
  at 1×1 and re-sized at draw time — `dkrt` should support dynamic resize of
  a FBO's color attachment without leaking the old GL resource.
