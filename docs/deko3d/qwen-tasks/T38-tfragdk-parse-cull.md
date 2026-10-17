# T38 — TFragmentDk::render — DMA parse + cull + record tfrag3 draws (P3.2, AI-assisted)

Track D P3.2 · Depends on: T33 T34 T35 T36 T37 all ✅ + **P2.3 soak PASS** ·
Inert until T39 wires it · Biggest slice — if you hit the wall, STOP at a clean
compile and report; do not rush the record loop.

## Goal

The renderer proper, in `TFragmentDk.cpp` (T36's file): walk the tfrag DMA
bucket exactly like GL `TFragment::render`, reuse the shared culling helper, and
record real draw commands on the frame cmd buf. **All recording, no engine
wiring** — T39 flips it live. deko3d has no MultiDraw: each strip-draw becomes a
loop of `dkCmdBufDrawIndexed` (P5 revisit noted in code comment).

## Context (read all before coding)

- `game/graphics/opengl_renderer/background/TFragment.cpp` — port target:
  - `handle_initialization` (declaration TFragment.h:84) + the bucket walk in
    `render` — the VIF/VU parse that fills `m_pc_port_data` (background_common.h:22)
    and `m_test_setup`. Copy the parsing (pure DMA reading via
    `common/dma/dma_chain_read.h` — no GL in it).
  - `render_tree` draw loop L502-569 — texture select (`draw.tree_tex_id`,
    anim-slot negative encoding L518-523), decal uniform L525, the AFAIL
    double-draw L543-565.
- `make_multidraws_from_vis_string` — background_common.cpp:679. BEFORE calling
  it, verify purity: `sed -n '679,760p' background_common.cpp` must contain no
  `gl` call. If it does, STOP and report.
- `game/graphics/deko3d_renderer/dk_tfrag_ubo.h` (T37) — UBO structs + fill.
- `dk_state.h` (T35) — `record_graphics_state` + `find_for_draw_mode`.
- `dk_device_internal.h` — `cmd_handle()`, sampler set accessors (T34), tfrag3
  shader handles (T33).

## API cheat-sheet (verified, deko3d.h)

- Attribs (PreloadedVertex is 32 B; GL layout TFragment.cpp:316-337):
  loc0 pos  = buffer 0, offset 0,  `DkVtxAttribSize_3x32` (0x02),
              `DkVtxAttribType_Float` (7);
  loc1 st    = buffer 0, offset 16, `3x32`+Float — the third float reads `nor`
              as raw bits, exactly like GL's size-3 read from `&s` (unused .z);
  loc2 tod   = buffer 0, offset 28, `DkVtxAttribSize_2x16` (0x0f),
              `DkVtxAttribType_Uint` (4) — GL's AttribIPointer 2xUSHORT twin.
  Record via `dkCmdBufBindVtxAttribState` (1274) + `dkCmdBufBindVtxBufferState`
  (1275: stride 32, divisor 0) + `dkCmdBufBindVtxBuffer` (inline 1398).
- Indices: `dkCmdBufBindIdxBuffer(cb, DkIdxFormat_Uint32, gpuAddr)` (1277).
- Draw: `dkCmdBufDrawIndexed(cb, DkPrimitive_TriangleStrip|Triangles,
  indexCount, 1, firstIndex, 0, 0)` (1308). GL offset arithmetic
  (TFragment.cpp:533) maps to firstIndex = byte_offset/4.
- UBOs: alloc BOTH blocks from `dkdev::ubo_ring()` (0x100-aligned), memcpy the
  T37 structs, `dkCmdBufBindUniformBuffers(cb, DkStage_Vertex, 7, &ext, 1)` and
  again for `DkStage_Fragment` (1261; `DkBufExtents` = {addr, size} — grep it).
- Textures: `dkCmdBufBindTextures(cb, DkStage_Fragment, 0, &hT0, 1)` for the
  tree texture, and `DkStage_Vertex, 0, &hT10, 1` for time-of-day (the vert
  shader samples! tfrag3.vert.glsl:23). Handles = `dkMakeTextureHandle(imageId,
  samplerId)` (715): tree tex imageId from the T23 seam's `dk_id` (id arithmetic:
  image set base is `kOffDesc` so imageId = 8 + slot — confirm against
  dk_device.cpp:56-61 + T34's notes before using), samplerId = table index for
  (Repeat wrap, Linear) — take it from `find_for_draw_mode`'s mipmap rule.
- Double-draw (AFAIL): re-fill FragParams (alpha_min=-10, alpha_max=aref_second),
  re-bind the frag UBO, re-record depth state with write off
  (`DkDepthStencilState` writeMaskChange — grep the field), re-draw. Mirror
  TFragment.cpp:543-565 exactly.

## Steps

1. `void record_frame(const u8* chain, u32 offset)` in TFragmentDk.cpp:
   DmaFollower over the bucket, port the parse, cache `TfragPcPortData`-derived
   camera into the T37 UBOs. First call lazily ensures shaders/samplers/state.
2. Cull via `make_multidraws_from_vis_string` into fixed scratch buffers
   (member vectors like GL's m_cache, TFragment.h:153-160).
3. Per strip-draw: texture handle, `record_graphics_state(find_for_draw_mode(...))`,
   UBO binds, DrawIndexed loop, optional AFAIL second pass.
4. Counters for T40: `draws_recorded`, `tris`, `record_us` (armGetSystemTick
   deltas like ms_since in dk_device.cpp:114) — plain statics + getters.
5. Every GPU-object null/invalid path: one `[dk] tfrag render: <what>` line,
   skip frame's world pass, never crash.

## Rules

- Only `TFragmentDk.h/.cpp`. No GL calls anywhere; shared-includes only.
- `#if defined(__SWITCH__)` whole body; no `<switch.h>` include next to
  common_types.h — hand-declare `switch_run_logf` like dk_device.cpp:30.
- If ANY cheat-sheet arithmetic (imageId base, sampler pick) contradicts what
  you see in dk_device.cpp/T34 code — the CODE wins; report the diff.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`727feb1c…`) — record_frame has no caller.
Changed → explain.

## Doubts

- The tod per-frame palette upload (GL re-uploads after itime interpolation,
  FIX 37): if porting the interpolation cache cleanly does not fit this slice,
  upload the initial palette each frame is WRONG — instead STOP and propose the
  split; the reviewer will slice it.
