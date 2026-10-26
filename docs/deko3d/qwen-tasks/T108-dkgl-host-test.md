# T108 — dkgl host integration test: recorded ocean sequence vs mock emitter

**Depends on:** T106. **Category:** D dkgl (brief item 4). **Size:** small-medium
(1 session). **Law:** zero design freedom — the replay sequence and the assertion list
are fixed. Pure host test; no hardware, no glad, no Switch build.

## Goal

`test/test_dkgl_integration.cpp` (~150 lines) + one `test/CMakeLists.txt` row. Drives the
whole dkgl state machine (T104b+T105+T106 entry points) with a MockEmitter that records
every call as a string + structured ring/UBO allocations, replaying a call sequence in
the ocean's real shape. This is the "drive dkgl with a recorded GL call sequence against
a mock backend and check the emitted state" acceptance test from the brief.

## Create 1 — the replay (one test fixture, several TEST_F cases)

The sequence (constant enum values from `dkgl/dkgl_gl_enums.h`; helper lambdas keep it
readable). It mirrors the ocean's actual per-frame shape:

```
frame A:
  bucket 4 (body draws sampling LAST frame's RT):
    use_program(ocean_common)                        // via hint at link time
    bind_framebuffer(0)                              // frame target
    enable(DEPTH_TEST); depth_func(LEQUAL); depth_mask(true)
    enable(BLEND); blend_func_separate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA)
    gen_buffers×3 (verts-mid, verts-near, idx); gen_vertex_arrays×2
    bind_vertex_array(mid); bind_buffer(ARRAY_BUFFER, verts-mid)
    buffer_data(ARRAY_BUFFER, 4096, ptrA)
    vertex_attrib_pointer(0,3,FLOAT,false,32,0); vertex_attrib_pointer(1,4,FLOAT,false,32,12)
    enable_vertex_attrib_array(0); enable_vertex_attrib_array(1)
    bind_buffer(ELEMENT_ARRAY_BUFFER, idx); buffer_data(ELEMENT_ARRAY_BUFFER, 2048, ptrB)
    active_texture(TEXTURE0); bind_texture(kTexIdBase+1)   // last frame's ocean RT
    uniform4f(color_mult); uniform1f(alpha_mult); uniform1i(bucket)
    draw_elements(TRIANGLES, 1200, UNSIGNED_INT, 0)
    bind_vertex_array(near); bind_buffer(ARRAY_BUFFER, verts-near)
    buffer_data(ARRAY_BUFFER, 4096, ptrC)            // rename: same-frame re-upload
    uniform1f(alpha_mult, other value)               // uniforms change between draws
    draw_arrays(TRIANGLE_STRIP, 0, 64)
  bucket 63 (texgen writes the RT):
    bind_framebuffer(fbo1); framebuffer_texture_2d(..., kTexIdBase+1, 0)
    disable(BLEND); depth_func(ALWAYS); depth_mask(false)
    use_program(ocean_texture); clear(COLOR); draw_arrays(TRIANGLES, 0, 6)
    use_program(ocean_texture_mipmap) × 7 levels (draw per level)
frame B:
  bucket 4 again (same as frame A bucket 4)
```

## Create 2 — the assertions (TEST_F cases; MockEmitter from T104b extended with per-call
structs, not just strings)

1. **Uniform snapshot per draw** (brief "critical" lesson): every draw in the fixture has
   its own `ubo_alloc` slice (distinct cpu pointer) and its own `bind_ubo` pair; the two
   `alpha_mult` values land in DIFFERENT slices — diff the memcpy'd bytes of consecutive
   draws and show the `alpha_mult` offset differs (read the offset from the T103 table).
2. **Buffer renaming**: `buffer_data` on `verts-mid` (frame B) gets a fresh slice; the
   frame-A slice bytes are still intact in the mock's storage afterwards.
3. **Barriers**: exactly ONE `barrier_before_sample` in frame A (first `bind_tex` of the
   RT at bucket 4) and exactly ONE in frame B (flags reset per frame, `was_rendered_to`
   latched from frame A's bucket-63 writes). Zero barriers inside bucket 63 before its
   own draws (nothing samples the RT there).
4. **Diffing**: `set_depth`/`set_blend`/`set_vtx_format` appear at frame-A draw 1, NOT at
   draw 2 (unchanged), DO appear again after the near-VAO switch (vtx only) and after
   `begin_bucket()` (frame B).
5. **GL semantics**: the mid draw's attrib slot 0 was captured with `verts-mid` even
   though `ARRAY_BUFFER` was re-bound before the near draw (per-attrib capture time);
   the near draw's element buffer is VAO-local (near has none → `draw_arrays` used).
6. **Blend factors**: the recorded `DkBlendState` from assertion 4's emit has the 4
   distinct separate-alpha factors (needs T101's patched lib at runtime — the mock only
   records the struct; assert the struct fields).
7. **Inert program**: link a program with hint `"solid_color"` (not in the table) → its
   draw emits NOTHING after `bind_shaders` was… actually inert = no `bind_shaders` either;
   assert `inert_draw_count == 1` and the mock log unchanged by that draw.
8. **Clear ordering**: `clear` emitted BEFORE the texgen draw's `bind_shaders`, and the
   "lost clear" counter stays 0 across both `frame_begin`s.
9. **Depth maps**: mid draw's `DkDepthStencilState` compareOp == the enum the T106 map
   produced for `LEQUAL`; texgen's == the one for `ALWAYS` with writeMask false.

## Checks

- Host build + `test_dkgl_integration` PASS; the whole dkgl host suite green.
- `python3 scripts/ai/check-dk.py` PASS (test file uses only dkgl enums + recorded
  structs; if the lint flags the test's `Dk*` mentions, add the test file to the same
  dkgl allowance — say so in the report).

## Report

Report: gtest output for all 9 cases, the emitted mock log for frame A bucket 4 verbatim
(the reviewer reads the command stream), any assertion that needed a semantic decision
(there should be none), check-dk output.
