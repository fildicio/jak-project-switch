# T106 — dkgl draw emission: lazy state → Dk state, per-draw UBO snapshot, barriers

**Depends on:** T105. **Category:** D dkgl (brief item 2, final quarter + lessons
"Uniform snapshot per draw", "Lazy state", "Render-to-texture"). **Size:** large
(1 session). **Law:** zero design freedom — the emit order, the maps and the snapshot rule
below are fixed. Enum spellings marked ⧉ must be copied verbatim from
`docs/deko3d-ref/include/deko3d.h` (check-dk.py validates them). Anything else → STOP.

## Goal

Replace the `draw_arrays`/`draw_elements` stub bodies with real emission through the
`Emitter`, plus the dkgl sampler block in `dk_device.cpp`. The GL side never sees a
difference; the emitter sees a minimal, diffed command stream.

## Create 1 — `game/graphics/deko3d_renderer/dkgl/dkgl_draw.cpp`

`void flush_pending(u32 mode, int first, int count, u32 idx_type, u64 idx_off, bool indexed)`
— called by both draw entries. Emit order (fixed):

1. **pending target switch** (a `bind_framebuffer` queued a different target): emit
   `bind_frame_target()` / `bind_offscreen_target(id)`; mark vtx/viewport dirty (target size changed).
2. **pending clear** (`pending_clear_mask()` from T104b): emit `clear(color, depth,
   shadow().clear_color)`, zero the mask. A clear pending at `frame_begin()` → counter
   `[dkgl] lost clear` (must not happen; T108 asserts it).
3. **program**: inert program (`program_table() == null`) → skip the whole draw, bump
   `inert_draw_count`, return. Program changed → `bind_shaders(vsh, fsh)` — the emitter
   owns the name→DkShader map (T107); dkgl passes the program's GL ids as `u32` handles:
   extend `Emitter::bind_shaders(u32 program_id, const char* name)` — change the T104a
   signature accordingly (see Edit 3).
4. **depth changed** → `set_depth(DkDepthStencilState)` from shadow:
   depth-test enable, writeMask `= shadow().depth_write`, compareOp from the map.
   Convention (P5): the device runs GL-style depth (`DkDeviceFlags_DepthMinusOneToOne`,
   clears 0.0) — GL funcs map verbatim; the ocean mid pass is `GL_ALWAYS`+write, matching
   GL's `flush_mid` semantics (T100/FIX 114 history).
5. **blend changed** → `set_blend(DkBlendState)`: enable + the 4 factor fields + ops.
   **Requires T101's patched deko3d** (separate alpha blend factor fix — brief pitfall).
6. **viewport changed** → `set_viewport({x, y, w, h, 0.f, 1.f})`.
7. **vtx format changed** → build `DkVtxAttribState[kMaxAttribs]` from the CURRENT VAO
   (skip disabled/invalid attribs; any invalid → skip draw + counter), single
   `DkVtxBufferState{stride, 0}`; all enabled attribs must share one stride, else counter +
   first stride. Emit `set_vtx_format(attribs, n, vbuf, vtx_gpu, vtx_bytes)` with the
   buffer bound at attrib-capture time (T104b).
8. **indexed draw** → `bind_idx_buffer(idx_buffer_gpu, fmt)` where fmt =
   `GL_UNSIGNED_INT`→`DkIdxFormat_Uint32` ⧉, `GL_UNSIGNED_SHORT`→`DkIdxFormat_Uint16` ⧉;
   `idx_buffer_gpu` = element buffer of the current VAO; missing element buffer → skip + counter.
9. **samplers**: for each unit 0..3 with a bound texture: if `texture_needs_barrier(id)` →
   `barrier_before_sample()` + `texture_barrier_sent(id)`; then `bind_tex(unit, dk_tex_id,
   sampler_combo)` (signature per Edit 3). `sampler_combo` = bit0 min_linear |
   bit1 mag_linear | bit2 mip_linear | bit3 repeat, derived from the texture's stored
   filters/wrap (min `LINEAR_MIPMAP_LINEAR` → min_linear=1, mip_linear=1; etc.).
   A bound texture whose `dk_tex_id == 0` and `gl_id < kTexIdBase` means it IS a raw
   dktexgpu id → pass it through. Unbound unit → skip.
10. **uniforms — snapshot EVERY draw** (brief lesson, critical): `ubo_alloc(vs+fs)` →
    memcpy the combined 256-byte shadow (T105 layout: vert block at 0, frag at
    `vert.block_size`), `bind_ubo(DkStage_Vertex, 7, gpu, vs)` +
    `bind_ubo(DkStage_Fragment, 7, gpu + vs, fs)`; then `program_uniform_clear_dirty`.
    NEVER reuse a slice within a frame — the mock/host tests assert distinct slices.
11. **primitive restart changed** → `set_primitive_restart(shadow().primitive_restart,
    shadow().restart_index)`.
12. the draw: `draw_arrays` / `draw_elements` with the primitive map.

### Maps (verbatim tables; ⧉ = copy spelling from deko3d.h)

| GL blend factor | DkBlendFactor (in-tree verified spelling) |
|---|---|
| `ZERO` / `ONE` | `DkBlendFactor_Zero` / `DkBlendFactor_One` |
| `SRC_ALPHA` | `DkBlendFactor_SrcAlpha` |
| `ONE_MINUS_SRC_ALPHA` | `DkBlendFactor_InvSrcAlpha` |
| `DST_ALPHA` | `DkBlendFactor_DstAlpha` |
| `CONST_COLOR` | `DkBlendFactor_ConstColor` |
| `DST_COLOR`, `ONE_MINUS_DST_COLOR`, `SRC_COLOR`, `ONE_MINUS_SRC_COLOR`, `ONE_MINUS_DST_ALPHA` | ⧉ copy the `DkBlendFactor_*` spelling for each from deko3d.h |

Blend equation: `FUNC_ADD`→`DkBlendOp_Add`, `FUNC_REVERSE_SUBTRACT`→`DkBlendOp_RevSub`,
`FUNC_SUBTRACT`→⧉ (`DkBlendOp_Sub` or as spelled in the header). Unmapped value → counter,
treat as `Add`.

| GL depth func | DkCompareOp |
|---|---|
| `NEVER`/`ALWAYS` | `DkCompareOp_Never`/`DkCompareOp_Always` |
| `GEQUAL`/`GREATER` | `DkCompareOp_Gequal`/`DkCompareOp_Greater` |
| `LESS`, `LEQUAL`, `EQUAL`, `NOTEQUAL` | `DkCompareOp_Less` / `DkCompareOp_Lequal` / `DkCompareOp_Equal` / `DkCompareOp_NotEqual` |

| GL primitive | DkPrimitive |
|---|---|
| `TRIANGLES` / `TRIANGLE_STRIP` | `DkPrimitive_Triangles` / `DkPrimitive_TriangleStrip` |
| `LINES`, `TRIANGLE_FAN` | `DkPrimitive_Lines` / `DkPrimitive_TriangleFan` |

| GL vertex attrib (size,type,normalized) | DkVtxAttribSize / DkVtxAttribType |
|---|---|
| {1..4, `FLOAT`, -} | `DkVtxAttribSize_{1x32,2x32,3x32,4x32}` + `DkVtxAttribType_Float` |
| {1, `UNSIGNED_INT`, -} | `1x32` + `DkVtxAttribType_Uint` |
| {4, `UNSIGNED_BYTE`, true} | `4x8` + `DkVtxAttribType_Unorm` |
| {-, `INT_2_10_10_10_REV`, -} | `10_10_10_2` + `DkVtxAttribType_Snorm` (brief pitfall) |

Anything else → counter + skip draw. `DkVtxAttribState` field order:
`{buffer, isFixed, offset, size, type, isBgra}` (recipe #2 / dk_tfrag_record.cpp).

## Edit 2 — `game/graphics/pipelines/dk_device.cpp`: dkgl sampler block

After the existing sampler init block (the `T34a` loop ending ~line 960, pattern
`smp.minFilter = DkFilter_Linear; … wrapMode[0..2] = DkWrapMode_ClampToEdge;`), add a
16-entry dkgl block: `for (combo 0..15)` set min/mag/mip filters
(`DkFilter_Nearest`/`DkFilter_Linear` — ⧉ copy the mip field name/spelling from the
existing block or the header) and wrap (`DkWrapMode_Repeat` ⧉ /
`DkWrapMode_ClampToEdge`), write to consecutive sampler descriptor slots, and add
`u32 dkgl_sampler_slot(u32 combo);` (declared in `dk_device.h`, returns the slot index).
~16 lines + 3 declaration lines. If the existing block reserves a fixed count that would
overflow the descriptor set → STOP with the numbers.

## Edit 3 — `dkgl_state.h`: two Emitter signature changes

- `bind_shaders(DkShader, DkShader)` → `bind_shaders(u32 program_id, const char* program_name)`
  (the Switch emitter owns name→DkShader; host mocks just log).
- `bind_tex(u32 binding, u32 image_slot, u32 sampler_slot)` →
  `bind_tex(u32 binding, u32 dk_tex_id, u32 sampler_combo)`
  (`dktexgpu::slot_of` is dk-only; the emitter resolves it on Switch).
Both keep the emitter host-mockable. Update the T104b mock accordingly.

## Create 4 — host tests (`test/test_dkgl_state.cpp` extend)

Scripted sequence (mirrors the ocean's real order): VAO+VBO setup, `buffer_data` ×2,
`use_program(ocean_common)`, `bind_framebuffer(offscreen)`, `clear`, `draw_arrays`,
`bind_framebuffer(0)`, `uniform4f`, `bind_texture(ocean RT)`, `draw_elements`. Assert on
the mock's log:
1. **distinct UBO slices per draw** (mock returns bumping pointers) — two draws, two
   `bind_ubo` pairs at different addresses (the critical lesson).
2. **diffing**: state emitted at draw 1, NOT re-emitted at draw 2 when unchanged;
   re-emitted after `begin_bucket()`.
3. **barrier**: exactly one `barrier_before_sample` before the first `bind_tex` of the RT,
   none for the second bind of the same texture in the same frame, none for pool textures.
4. **clear ordering**: `clear` emitted before the following draw's `bind_shaders`.
5. **inert program**: draws skipped, counted.
6. **separate alpha blend**: `blend_func_separate(0x0302, 0x0303, 1, 0x0303)` produces a
   `DkBlendState` copy in the mock with the four distinct factors.
7. **no lost clear**: `frame_begin()` with a pending clear increments the counter → assert
   it stays 0 in the scripted run above.

## Checks

- Host build + tests PASS.
- `python3 scripts/ai/check-dk.py` → PASS (all ⧉ spellings resolved — list each one you
  copied in the report so the reviewer can eyeball them).
- `git diff game/graphics/pipelines/dk_device.cpp` = sampler block + accessor only.

## Report

Report: diffs, the resolved ⧉ spelling list, gtest tail, check-dk output, STOP conditions.
