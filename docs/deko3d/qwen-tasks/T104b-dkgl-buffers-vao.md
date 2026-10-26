# T104b — dkgl core: buffers/VAO/state implementations + glad wiring

**Depends on:** T104a. **Category:** D dkgl (brief item 2, second half). **Size:** medium
(1 session). **Law:** zero design freedom — replace exactly the stub bodies named below;
glad wiring copies the `dk_gl_census.cpp` pattern verbatim. Anything else → STOP.

## Goal

Implement the buffer/VAO/state half of the dkgl shadow (glBufferData ring semantics =
renaming, GL attrib-capture rules), plus the Switch-only glad override install, and teach
`scripts/ai/check-dk.py` that dkgl files legitimately reference glad.

## Create 1 — `game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h` (verbatim)

Stable GL ABI constants (same values glad/gl.h carries) — the host build has no glad:

```cpp
#pragma once
// T104b (AI-assisted): GL enum values used by the dkgl layer, as plain u32.
// Values are the stable GL ABI constants (glad/gl.h). Host tests use these too.
#include "common/common_types.h"

namespace dkgl::gl {
constexpr u32 NO_ERROR = 0;
constexpr u32 FALSE = 0, TRUE = 1;
// blend factors
constexpr u32 ZERO = 0, ONE = 1;
constexpr u32 SRC_COLOR = 0x0300, ONE_MINUS_SRC_COLOR = 0x0301;
constexpr u32 SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr u32 DST_ALPHA = 0x0304, ONE_MINUS_DST_ALPHA = 0x0305;
constexpr u32 DST_COLOR = 0x0306, ONE_MINUS_DST_COLOR = 0x0307;
// blend equations
constexpr u32 FUNC_ADD = 0x8006, FUNC_SUBTRACT = 0x800A, FUNC_REVERSE_SUBTRACT = 0x800B;
// caps
constexpr u32 BLEND = 0x0BE2, DEPTH_TEST = 0x0B71, SCISSOR_TEST = 0x0C11;
constexpr u32 PRIMITIVE_RESTART = 0x8558;
// depth funcs
constexpr u32 NEVER = 0x0200, LESS = 0x0201, EQUAL = 0x0202, LEQUAL = 0x0203;
constexpr u32 GREATER = 0x0204, NOTEQUAL = 0x0205, GEQUAL = 0x0206, ALWAYS = 0x0207;
// vertex types
constexpr u32 UNSIGNED_BYTE = 0x1401, INT = 0x1404, UNSIGNED_INT = 0x1405, FLOAT = 0x1406;
constexpr u32 INT_2_10_10_10_REV = 0x8D9F, UNSIGNED_INT_2_10_10_10_REV = 0x8368;
// primitives
constexpr u32 LINES = 0x0001, TRIANGLES = 0x0004, TRIANGLE_STRIP = 0x0005, TRIANGLE_FAN = 0x0006;
// buffer targets
constexpr u32 ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893;
// clear bits
constexpr u32 COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x0100;
// formats
constexpr u32 RGBA = 0x1908, RGBA8 = 0x8058, RGB = 0x1907;
constexpr u32 UNSIGNED_INT_8_8_8_8_REV = 0x8367, UNSIGNED_BYTE_TYPE = UNSIGNED_BYTE;
// textures
constexpr u32 TEXTURE_2D = 0x0DE1;
constexpr u32 TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800;
constexpr u32 TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803;
constexpr u32 NEAREST = 0x2600, LINEAR = 0x2601;
constexpr u32 NEAREST_MIPMAP_NEAREST = 0x2700, NEAREST_MIPMAP_LINEAR = 0x2702;
constexpr u32 LINEAR_MIPMAP_NEAREST = 0x2701, LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr u32 REPEAT = 0x2901, CLAMP_TO_EDGE = 0x812F;
// FBO
constexpr u32 FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0;
constexpr u32 FRAMEBUFFER_COMPLETE = 0x8CD5;
}  // namespace dkgl::gl
```

## Edit 2 — `game/graphics/deko3d_renderer/dkgl/dkgl_state.cpp`: replace these stubs

Semantics (GL spec, no freedom):

| entry | behavior |
|---|---|
| `enable/disable/is_enabled` | `BLEND`→`shadow().blend`, `DEPTH_TEST`→`shadow().depth_test`, `PRIMITIVE_RESTART`→`shadow().primitive_restart`; `SCISSOR_TEST` or anything else → bump a `u64 unknown_cap_count` + return false (draw path logs first use in T106) |
| `blend_func(s,d)` | sets src_rgb=dst_rgb copy: `blend_src_rgb=blend_src_a=s; blend_dst_rgb=blend_dst_a=d` |
| `blend_func_separate` | sets the 4 fields |
| `blend_equation(m)` | `blend_eq_rgb=blend_eq_a=m` |
| `depth_mask/depth_func/viewport/clear_color` | direct shadow writes |
| `clear(mask)` | record `pending_clear_mask \|= mask` (a new static in the .cpp; emitted by T106's draw/clear flush, cleared on `frame_begin`) |
| `primitive_restart_index(i)` | `shadow().restart_index = i` (ocean only ever passes 0xFFFFFFFF; if != that, bump a counter — do NOT fail) |
| `gen_buffers` | hand out ids 1..kMaxBuffers (never 0); mark live |
| `delete_buffers` | mark dead; if bound to ARRAY_BUFFER/ELEMENT_ARRAY_BUFFER, rebind those to 0 |
| `bind_buffer(target,name)` | track bound per target (element binding goes to the CURRENT VAO: `bound_vao()->element_buffer`) |
| `buffer_data(target,size,data,usage)` | **renaming**: `emitter->ring_alloc(size, 4, &cpu, &gpu)`; fail → `valid=false`, return false (caller contract: draws using an invalid buffer are skipped, never crash); success → memcpy(cpu,data,size), store {cpu,gpu,bytes,valid=true}. target must be ARRAY_BUFFER or ELEMENT_ARRAY_BUFFER, else counter + false |
| `gen/bind/delete_vertex_arrays`, `bind_vertex_array(0)` | VAO table; bind swaps the whole attrib set; name 0 = the default VAO object (entry 0, always live) |
| `enable_vertex_attrib_array(i)` | `i < kMaxAttribs` guard; set `enabled` on the CURRENT VAO |
| `vertex_attrib_pointer/vertex_attrib_i_pointer` | record into current VAO slot: size/type/normalized (i_pointer: `normalized=false`, integer path), `stride`; `offset = (u64)pointer`; `buffer = bound_array_buffer()` (GL captures the ARRAY_BUFFER binding at call time — NOT at draw time); if `buffer == 0` → mark the attrib invalid (CPU-pointer draws unsupported; counter + skip later) |
| `draw_arrays/draw_elements` | only: bump `draw_count()`, store last-draw params in statics for T106 (`pending_draw` struct: mode/first/count or mode/count/idx_type/idx_offset_bytes + the VAO/buffer/program/texture-unit snapshot is taken by T106, not here). NO emission in this task. |

Add to `dkgl_state.h` (3 lines, inside `namespace dkgl`): 
`u64 unknown_cap_count(); u64 cpu_pointer_attrib_count(); u32 pending_clear_mask();`

## Create 3 — `game/graphics/deko3d_renderer/dkgl/dkgl_glad.cpp` (Switch-only)

`#if defined(__SWITCH__)` file. It assigns the glad function pointers for exactly this
subset (buffer/VAO/state — textures/programs come in T105; the rest stay census stubs):

`glad_glGenBuffers, glad_glDeleteBuffers, glad_glBindBuffer, glad_glBufferData,
glad_glGenVertexArrays, glad_glDeleteVertexArrays, glad_glBindVertexArray,
glad_glEnableVertexAttribArray, glad_glVertexAttribPointer, glad_glVertexAttribIPointer,
glad_glEnable, glad_glDisable, glad_glIsEnabled, glad_glBlendFunc, glad_glBlendFuncSeparate,
glad_glBlendEquation, glad_glDepthMask, glad_glDepthFunc, glad_glViewport,
glad_glClearColor, glad_glClear, glad_glPrimitiveRestartIndex, glad_glDrawArrays,
glad_glDrawElements`

- Copy the pointer type + assignment style VERBATIM from the census install tail
  (`game/graphics/deko3d_renderer/dk_gl_census.cpp`, ~lines 1820-1930): the census already
  writes `glad_gl*` pointers; dkgl wraps them as `static void GLAPIENTRY
  dkgl_glBindBuffer(GLenum t, GLuint n) { bind_buffer(t, n); }` etc. (signatures from the
  census stubs — identical).
- `void install_glad();` (declared in `dkgl_state.h` under `#if defined(__SWITCH__)`)
  performs the assignments and logs one line: `[dkgl] glad overrides: buffers/vao/state (23)`.
- Call order contract: `dk_gl_census` installs first (boot), `dkgl::install_glad()` runs
  later (T107 init) and OVERWRITES only this subset. No uninstall (process lifetime).

## Edit 4 — `scripts/ai/check-dk.py` (gl allowance)

The lint currently fails on GL calls in deko3d code (rule P7), yet `dk_gl_census.cpp`
legitimately touches glad. Read how check-dk.py exempts `dk_gl_census.cpp` today (grep the
script for `census` / `glad`), and extend that exemption to
`game/graphics/deko3d_renderer/dkgl/*` with the SAME mechanism and this rule: in dkgl
files, `glad_gl*` pointer references and GLenum parameter names are allowed; a DIRECT
`glXxx(...)` call (i.e. calling GL through anything but the assigned pointer) still fails.
If check-dk.py has no census exemption (i.e. it passes for another reason) → STOP, paste
the relevant script section + how dk_gl_census.cpp passes today.

## Create 5 — host test additions (`test/test_dkgl_state.cpp`)

Extend with a `MockEmitter` (records `ring_alloc` calls; implements every virtual by
string-append `emit.log += "..."`) — `dkgl::set_emitter(&mock)`:
1. renaming: two `buffer_data` on the SAME name → two distinct `ring_alloc` calls with
   distinct cpu pointers (mock returns bumping pointers), second memcpy overwrote nothing
   of the first (assert first slice bytes still intact in the mock's storage).
2. attrib capture: bind buffer 1 → `vertex_attrib_pointer(0, 3, FLOAT, false, 32, (void*)0)`
   → bind buffer 2 → `vertex_attrib_pointer(1, 4, FLOAT, false, 32, (void*)12)` → VAO slot
   0 has `buffer==1`, slot 1 has `buffer==2` (binding captured at call time, not draw time).
3. element buffer is VAO state: bind VAO A → `bind_buffer(ELEMENT_ARRAY_BUFFER, 9)` → bind
   VAO B → its element_buffer is 0 → bind VAO A → 9 again.
4. `buffer_data` with mock returning false → `buffer(name)->valid == false`.
5. `bind_buffer(GL_ARRAY_BUFFER, 0)` then `vertex_attrib_pointer` → slot invalid +
   `cpu_pointer_attrib_count() == 1`.

## Checks

- Host build + `test_dkgl_state` PASS (all cases, old + new).
- `python3 scripts/ai/check-dk.py` → PASS **with the dkgl allowance active and a
  deliberate negative test**: temporarily add `glBindTexture(1,1);` to `dkgl_glad.cpp`,
  confirm the lint FAILS, remove it, confirm PASS. Describe both runs in the report.
- Do NOT run check-switch here (no Switch-side behavior changed yet; the file compiles
  only under `__SWITCH__` — the Switch compile is validated in T107).

## Report

Report: per-file diffs (should be stub-body replacements + the 3 new files), gtest tail,
check-dk PASS + negative-test output, any STOP conditions.
