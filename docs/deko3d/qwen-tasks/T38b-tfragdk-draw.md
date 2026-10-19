# T38b — TFragmentDk: cull + draw recording + AFAIL (P3.2, AI-assisted)

Track D P3.2 · Depends on: T38a ✅ + soak PASS · Inert until T39 · One session (~45 min)
Biggest half in the batch — work the STOP LADDER: (1) cull wiring → (2) state+UBO
binds → (3) draw loop → (4) AFAIL + counters. Any point past 45 min: stop at the
last clean compile, report which ladder step you reached.

## Session rules (hard — read twice)

- Time box ~45 min HARD. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan. Everything is pasted below.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T38b` (state the ladder step reached).

## Read (once, exactly these)

1. `game/graphics/opengl_renderer/background/TFragment.cpp` L500-570 — the
   draw loop (texture select, decal, AFAIL double-draw).
2. `sed -n '679,760p' game/graphics/opengl_renderer/background/background_common.cpp`
   — `make_multidraws_from_vis_string`. If ANY `gl` call appears in it: STOP,
   report the line — the helper is supposed to be pure.

## API (pasted verbatim from deko3d.h — do not open it)

```c
// Attribs (PreloadedVertex is 32 B; GL layout TFragment.cpp:316-337):
//  loc0 pos: buffer 0, offset 0,  DkVtxAttribSize_3x32 (0x02), Type_Float (7)
//  loc1 st:  buffer 0, offset 16, 3x32+Float (third float reads `nor` as raw
//            bits — same as GL's size-3 read from &s; unused .z)
//  loc2 tod: buffer 0, offset 28, DkVtxAttribSize_2x16 (0x0f), Type_Uint (4)
void dkCmdBufBindVtxAttribState(DkCmdBuf, DkVtxAttribState const[], uint32_t);  // L1274
void dkCmdBufBindVtxBufferState(DkCmdBuf, DkVtxBufferState const[], uint32_t);  // L1275
//   DkVtxBufferState { uint32_t stride; uint32_t divisor; }  — stride 32, divisor 0
void dkCmdBufBindVtxBuffers(DkCmdBuf, uint32_t firstId,
                            DkBufExtents const[], uint32_t num);               // L1276
typedef struct DkBufExtents { DkGpuAddr addr; uint32_t size; } DkBufExtents;    // L699-703
void dkCmdBufBindIdxBuffer(DkCmdBuf, DkIdxFormat, DkGpuAddr);                   // L1277
void dkCmdBufDrawIndexed(DkCmdBuf, DkPrimitive, uint32_t indexCount,
                         uint32_t instanceCount, uint32_t firstIndex,
                         uint32_t firstVertex, int32_t firstInstance);          // L1308
DK_CONSTEXPR DkResHandle dkMakeTextureHandle(uint32_t imageId,
                                             uint32_t samplerId);               // L715
// DkDepthStencilState write control (L1017-1018, real field names):
//   uint32_t depthTestEnable : 1;   uint32_t depthWriteEnable : 1;
// UBO bind: dkCmdBufBindUniformBuffers(cb, stage, firstId, DkBufExtents const*, n) // L1261
// Texture bind: dkCmdBufBindTextures(cb, stage, firstId, DkResHandle const*, n)
// Shader bind: dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, DkShader const*[], n)
```

- deko3d has NO MultiDraw: each strip-draw = one `dkCmdBufDrawIndexed` (P5
  revisit comment in code). GL byte offset (TFragment.cpp:533) → firstIndex =
  byte_offset/4.
- UBO alloc: `dkdev::ubo_ring()` (dk_device.h L70-73), 0x100-aligned, memcpy
  the T37a structs, bind VertParams at (Vertex,7) and FragParams at
  (Fragment,7).
- Texture handles: tree tex imageId from the T23/T43 seam id arithmetic
  (image set base + slot — CHECK dk_device.cpp L56-61 + T34b's note; the CODE
  wins over this text), samplerId from `find_for_draw_mode`'s out-param.
  Vertex stage TOO (tfrag3.vert samples tex_T10 at binding 0 — tod texture).

## Steps (the stop ladder)

1. Cull: call `make_multidraws_from_vis_string` into member scratch vectors
   (GL's m_cache shape, TFragment.h:153-160).
2. Per strip-draw: texture handles → `record_graphics_state(find_for_draw_mode(...))`
   (T35a/T35b) → UBO binds (`fill_vert`/`fill_frag` T37b) → attrib/idx binds →
   DrawIndexed loop. DrawMode → pipeline entry via T35b; nullptr match → skip
   that draw + one `[dk]` line per report window.
3. AFAIL double-draw (TFragment.cpp:543-565): re-fill FragParams, re-bind frag
   UBO, depth state copy with `depthWriteEnable=0`, re-draw. Mirror exactly.
4. Counters for T40: `draws_recorded`, `tris`, `record_us` (armGetSystemTick
   deltas like dk_device.cpp:114) — plain statics + getters in the .h.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 `67018cd5a4411b3f3cc77f198773b261` (a move is NOT a stop: A/B-attribute — stash only your files, rebuild, compare, restore, rebuild — and report both md5s; STOP only if the A/B does not attribute) (record_frame
uncalled). Changed → explain.

## Doubts

- The per-frame tod palette re-interpolation (FIX 37): if porting the
   interpolation cache does not fit your remaining time box, STOP after step 3
   and report — the reviewer slices it into T38c rather than you rushing it.
