# Hybrid renderer: our own minimal "dkgl" (GL subset on deko3d) for the ocean (AI-assisted)

Text-only brief for the reviewer model (GLM). NEVER open image files.

## Decision (user, 2026-10-26)

Stop fixing the native ocean port (`deko3d_renderer/dk_ocean_*`). Instead, run the
**unchanged** GL ocean renderers (`opengl_renderer/ocean/*.cpp`) on deko3d through a
small GL-subset layer we write ourselves ("dkgl"). Native dk renderers (tfrag, shrub, tie,
merc, sprite, direct, sky, eye, hfrag) stay native. This is a **hybrid**: one frame, one
command buffer, native buckets + dkgl buckets in the GL bucket order.

Next after the ocean (same layer, no new design): `Generic2BucketRenderer` (10 jak1
buckets, likely the distant mountains), then `DepthCue`, `ShadowRenderer`.

Another team proved the idea: a full GL-subset layer on deko3d 0.5.0 ran all of jak1 at
60 fps on hardware. We do NOT have their code; we build a small one, only what the ocean
needs, and grow it per renderer.

## Scope: exact GL surface (measured with grep, 2026-10-26)

Ocean (`opengl_renderer/ocean/*.cpp`, 12k lines incl. VU1 interpreters):
glBindBuffer, glBindVertexArray, glTexParameteri, glBufferData, glEnableVertexAttribArray,
glVertexAttribPointer, glVertexAttribIPointer, glEnable, glDisable, glIsEnabled,
glBindTexture, glActiveTexture, glUniform1i, glUniform1f, glUniform4f, glGenBuffers,
glDeleteBuffers, glGenVertexArrays, glDeleteVertexArrays, glDrawArrays, glDrawElements,
glPrimitiveRestartIndex, glBlendFuncSeparate, glBlendEquation, glBlendFunc, glDepthMask,
glDepthFunc, glViewport, glClearColor, glClear.
Plus the helpers they call: `Shader::activate` (glUseProgram), `FramebufferTexturePair`
and `FramebufferTexturePairContext` in `opengl_utils.cpp` (glGenFramebuffers,
glBindFramebuffer, glFramebufferTexture2D, glTexImage2D, glGenTextures, glDrawBuffers…) —
grep them and add them to the list.
Generic2 adds only: glUniform1ui, glUniform3f, glUniformMatrix4fv, glBlendColor.

Shaders: OCEAN_COMMON, OCEAN_ENVMAP, OCEAN_ENVMAP_HAZE, OCEAN_TEXTURE,
OCEAN_TEXTURE_MIPMAP (`game/graphics/opengl_renderer/shaders/`).

## Architecture

1. **Seam = the T49 gl-census.** With the dk path active, every glad pointer already points
   at a logging no-op stub. dkgl replaces the stubs for the functions above with real
   implementations; everything else keeps the census stub (logs first call).
2. **Shared device.** dkgl records into the dk path's command buffer
   (`pipelines/dk_device.cpp`), never its own queue. Same frame slices, same fences.
3. **Shared targets.** Default framebuffer (FBO 0) = the dk colour + depth target of the
   current frame. Offscreen FBOs (ocean texture 128x128 + mips) = dkgl-owned images.
4. **Shared textures.** `glBindTexture(id)` for a TexturePool id resolves through
   `dktexgpu::slot_of(id)` (same mapping `dk_ocean_flush.cpp` uses). dkgl-created textures
   (glGenTextures) get their own slots from the same descriptor set.
5. **Uniforms.** Build-time script (extend `cmake/Deko3dShaders.cmake` flow): for each
   program, move all default uniforms of vert+frag into one std140 UBO with fixed offsets,
   give every sampler/UBO an explicit `layout(binding=N)`, compile with UAM, emit a C++
   table name -> offset/size. `glUniform*` writes a CPU shadow; upload + bind at draw.
6. **Lazy state.** glEnable/blend/depth/viewport/VAO calls only update a shadow struct;
   `glDraw*` emits the dk state (DkDepthStencilState, DkBlendState, DkColorState,
   viewport/scissor, vertex attrib/buffer state, primitive restart) then the draw.
7. **Buffers.** glBufferData = allocate from a per-frame ring in a CPU-visible memblock
   (data used in this frame only — the ocean re-uploads every frame). Each call gets a new
   ring slice (= renaming, see "Design lessons").
8. **State handover.** Before the first dkgl draw of a bucket and after its last, the
   native code re-binds its own state (native passes must not assume state survives).
9. **Switch.** dkgl ocean is the default on the dk path; `sdmc:/switch/jak1/gk_dkocean_native.txt`
   falls back to the old native ocean; `gk_noocean.txt` still turns the ocean off.

## Known deko3d 0.5.0 pitfalls (from the other team, each cost them a debug round)

- UpperLeft origin stores render targets in display order: negate `gl_Position.y` in
  every dkgl vertex shader, so offscreen textures keep GL's row-0-at-bottom layout.
- `DkCopyBuf.rowLength` is in bytes, not texels.
- Never use HwCompression on images written by the 2D/copy engines: GPU hang, console freeze.
- RGB_BC1 returns alpha 0 for the 3-colour black index on Tegra: force alpha=1 by swizzle.
- A zeroed image descriptor faults when sampled: slot 0 must be a 1x1 white texture.
- `GL_INT_2_10_10_10_REV` -> `DkVtxAttribSize_10_10_10_2`.
- Link the patched deko3d (`third-party/deko3d`, FuncAlphaDst fix) — the ocean relies on
  separate alpha blend factors.
- `std::thread` throws at boot (no heap for the stack); if a thread is ever needed use
  `threadCreate` with a static stack (dkgl itself must not need one).

## Design lessons from the other team's working layer (follow them)

- **Uniform snapshot per draw (critical).** The GPU runs after the CPU has recorded the
  whole frame. Each `glDraw*` must copy the current uniform shadow into a NEW slice of a
  per-frame UBO ring and bind that slice. Re-using one UBO means every draw sees the last
  `glUniform*` values of the frame.
- **Buffer renaming.** Every `glBufferData`/`glBufferSubData` on a buffer the GPU may still
  read gets new storage (per-frame ring, 2 frames in flight); never overwrite in place.
  The ocean re-uploads the same VBO several times per frame (mid, near, texgen).
- **Uploads** (`glTexImage2D` with data) go through a staging arena +
  `dkCmdBufCopyBufferToImage`, then one barrier before the first draw that samples them.
- **Descriptor slots** are recycled only after the frame that used them has completed
  (fence), never earlier.
- **Render-to-texture.** When a draw samples an image that an earlier draw rendered into
  (ocean texture -> mip chain -> water body), insert a barrier (Fragments + invalidate
  image/descriptor caches) before it. Only then, not after every pass.
- **Lazy state.** Emit dk state only at draw time and only what changed since the last draw.
- **Fallback.** If dkgl init fails, log it and use the native ocean, never crash.

## Rules

`#ifdef __SWITCH__` / dk path only. No edits to `opengl_renderer/` except, if unavoidable,
a one-line hook (name hint for the shader, like `dkgl::hint_program_name`). No new runtime
threads. Every deko3d name checked in `docs/deko3d-ref/include/deko3d.h` and
`python3 scripts/ai/check-dk.py` PASS. `scripts/ai/check-switch.sh jak1` PASS.

## Requested work (reviewer)

Split into Qwen tasks (AI_ROLES.md style, <=150 lines each), in this order:
1. Shader script + UBO tables for the 5 ocean programs (host-buildable, testable).
2. dkgl core: state shadow, buffers/VAO, textures/FBO, uniforms, draw emission.
3. Hook into the dk frame at buckets 4 and 63, replacing `dkocean::record_*`; fallback flag.
4. Host unit test: drive dkgl with a recorded GL call sequence against a mock backend and
   check the emitted state (no hardware needed).
5. One hardware check (bundle with other checks, SD-trip rule): ocean visible, no black
   slab, fps unchanged.
