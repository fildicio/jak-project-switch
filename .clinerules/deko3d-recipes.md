# deko3d recipes for this repo (AI-assisted)

Copy these patterns; don't invent new ones. Every name below exists in deko3d 0.5.0
(`docs/deko3d-ref/include/deko3d.h`) or in this repo. They are taken from code that **runs on
the Switch**. When unsure, open the file in brackets and copy from there.

**After every edit, run** `python3 scripts/ai/check-dk.py`. It must print `dk-lint: PASS`.
It fails on any `dk…`/`Dk…`/`DK_…` name that doesn't exist, and on GL calls in deko3d code.

## 0. The one rule that matters most
deko3d does nothing for you. If the GPU might still be reading memory, **don't touch it**
until that frame's fence has passed. Wrong code compiles and then shows a black screen or
crashes only on the console.

## 1. Where things live (use the project helpers, not raw memblocks)
| Need | Use | Example file |
|---|---|---|
| per-frame uniforms (UBO) | `dkdev::ubo_ring()->alloc(0x100, 0x100)` → `a.cpu` / `a.gpu` | `dk_tfrag_record.cpp` |
| long-lived vertex/index data | `dkdev::buffer_pool()->acquire(bytes)` → `PoolBuffer{cpu,gpu,capacity}` | `TFragmentDk.cpp` `ring_begin` |
| copy CPU bytes into a pool buffer from a loader thread | `dkdev::copy_to_gpu(gpu_dst, src, size)` | `LoaderStages.cpp` |
| a texture (RGBA8) | `dktexgpu::upload_rgba8(w, h, rgba)` → id; `dktexgpu::slot_of(id)` → descriptor slot | `dk_texture_gpu.cpp` |
| the frame command buffer | `dkdev::cmd_handle()` — **only inside the world recorder** | `dk_tfrag_record.cpp` |
| pipeline state from a GS draw mode | `dkpipe::find_for_draw_mode(axes, &sampler)` + `dkpipe::record_graphics_state(cb, *desc)` | `dk_tfrag_record.cpp` |

Raw memblocks (only in `dk_device.cpp`): size is a multiple of `DK_MEMBLOCK_ALIGNMENT`,
flags `DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image` for images,
`DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached` for CPU-written data.

## 2. Recipe: draw indexed geometry (from `dk_tfrag_record.cpp`)
```cpp
DkCmdBuf cb = dkdev::cmd_handle();
DkViewport vp = {0.0f, 0.0f, (float)fb_w, (float)fb_h, 0.0f, 1.0f};
dkCmdBufSetViewports(cb, 0, &vp, 1);
DkShader vsh = dkdev::tfrag3_vsh(), fsh = dkdev::tfrag3_fsh();
DkShader const* sh[] = {&vsh, &fsh};
dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, sh, 2);
// vertex layout: {buffer, isFixed, offset, size, type, isBgra}
const DkVtxAttribState attribs[] = {
    {0, 0, 0,  DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
    {0, 0, 28, DkVtxAttribSize_1x16, DkVtxAttribType_Uint,  0},
};
const DkVtxBufferState vbuf = {32 /*stride*/, 0 /*divisor*/};
dkCmdBufBindVtxAttribState(cb, attribs, 2);
dkCmdBufBindVtxBufferState(cb, &vbuf, 1);
dkCmdBufBindVtxBuffer(cb, 0, vert_gpu, vert_bytes);
dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 7, ubo_gpu, 0x100);  // binding = GLSL layout(binding=7)
dkCmdBufBindTexture(cb, DkStage_Fragment, 0, dkMakeTextureHandle(tex_slot, sampler_slot));
dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, idx_gpu);
dkCmdBufDrawIndexed(cb, DkPrimitive_Triangles, idx_count, 1, 0, 0, 0);
```

## 3. Recipe: fill a UBO
```cpp
dkring::Alloc a = dkdev::ubo_ring()->alloc(0x100, 0x100);  // size, alignment (UBO = 0x100)
if (!a.ok()) { /* count it in a *_fail counter, skip the draw */ }
MyParams p;            // C++ struct laid out like the GLSL std140 block
memcpy(a.cpu, &p, sizeof(p));
dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 7, a.gpu, 0x100);
```

## 4. Recipe: 3D clear, then a 2D blit (from `dk_device.cpp` `present_frame`)
```cpp
dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.1f, 1.0f);
dkCmdBufBarrier(cmd, DkBarrier_Full, 0);   // 3D engine -> 2D engine: REQUIRED
dkCmdBufBlitImage(cmd, &srcView, &srcRect, &fbView, &dstRect,
                  DkBlitFlag_FilterLinear | DkBlitFlag_FlipY, 0);  // FlipY: see pitfall P4
```

## 5. GL → deko3d table (you know GL; translate with this)
| OpenGL | deko3d / this repo |
|---|---|
| `glUseProgram` | `dkCmdBufBindShaders` |
| `glVertexAttribPointer` (+Enable) | `DkVtxAttribState[]` + `dkCmdBufBindVtxAttribState`, stride in `DkVtxBufferState` + `dkCmdBufBindVtxBufferState` |
| `glVertexAttribIPointer` (integer) | `DkVtxAttribType_Uint` / `DkVtxAttribType_Sint` |
| normalized `GL_TRUE` | `DkVtxAttribType_Unorm` / `DkVtxAttribType_Snorm` |
| `GL_INT_2_10_10_10_REV` | `DkVtxAttribSize_10_10_10_2` + `DkVtxAttribType_Snorm` |
| `GL_UNSIGNED_BYTE ×4` | `DkVtxAttribSize_4x8` |
| `glBindBuffer(GL_ARRAY_BUFFER)` | `dkCmdBufBindVtxBuffer(cb, slot, gpu_addr, size)` |
| `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER)` | `dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, gpu_addr)` |
| `glBufferData` (static) | `buffer_pool()->acquire` + `copy_to_gpu` |
| `glBufferData` (per frame) | write into a fence-protected ring slot (see `TFragmentDk.cpp` `ring_begin`) |
| `glUniform*` | std140 UBO + `dkCmdBufBindUniformBuffer` (no loose uniforms) |
| `glActiveTexture` + `glBindTexture` | `dkCmdBufBindTexture(cb, stage, binding, dkMakeTextureHandle(img, smp))` |
| `glTexParameteri` (wrap/filter) | sampler slot picked by `dkpipe::find_for_draw_mode` |
| `glTexImage2D` | `dktexgpu::upload_rgba8` |
| `glEnable(GL_DEPTH_TEST)` / `glDepthFunc` / `glDepthMask` / `glBlendFunc` | `dkpipe::DrawModeAxes` → `record_graphics_state` (binds `DkDepthStencilState`, `DkBlendState`) |
| `glViewport` / `glScissor` | `dkCmdBufSetViewports` / `dkCmdBufSetScissors` |
| `glClear` | `dkCmdBufClearColorFloat` / `dkCmdBufClearDepthStencil` |
| `glPrimitiveRestartIndex` | `dkCmdBufSetPrimitiveRestart(cb, true, UINT32_MAX)` |
| `glDrawElements` / `glDrawArrays` | `dkCmdBufDrawIndexed` / `dkCmdBufDraw` |
| `glFenceSync` / `glClientWaitSync` | `dkCmdBufSignalFence` / `dkFenceWait` |
| `glFinish` | `dkQueueWaitIdle` (avoid in the frame loop) |

## 6. Pitfalls (each one has happened or nearly happened here)
- **P1 Invented names.** `DkVtxType_U8x4` and friends were written in `dk_tie3_prep.h`; they don't <!-- dk-lint: ok -->
  exist. The real names are `DkVtxAttribSize_*` + `DkVtxAttribType_*`. `check-dk.py` catches this.
- **P2 Reusing GPU memory too early.** `dkCmdBufClear` + reuse of command or index memory
  is only safe after that slice's fence. Present N waits for frame N−2.
- **P3 Missing barrier.** A 3D clear or draw followed by a 2D blit needs `dkCmdBufBarrier`.
- **P4 Origin.** The device uses `DkDeviceFlags_OriginLowerLeft` (GL style). 3D draws come out
  right; 2D blits need `DkBlitFlag_FlipY` or they show upside down (T62 splash bug).
- **P5 Depth like GL.** Clear depth to `0.0f` and test GEQUAL/GREATER (`DkDeviceFlags_DepthMinusOneToOne`).
- **P6 Shared upload cmd buffer.** Texture uploads must happen before `present_frame` records
  (`build_frame` runs before present; `record_world` runs inside it).
- **P7 No GL in deko3d files.** Nothing from `opengl_renderer/` that calls `gl*` may run on the dk path.
- **P8 Image memory never comes back.** `dktexgpu::release` doesn't free the image bump memory.
  Recycle ids (see the tod palette free list in `TFragmentDk.cpp`).
