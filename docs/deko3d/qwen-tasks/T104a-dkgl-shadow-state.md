# T104a — dkgl core: emitter interface + shadow state (host-testable)

**Depends on:** —. **Category:** D dkgl (brief item 2, first half). **Size:** medium
(1 session). **Law:** zero design freedom — the header below is pasted verbatim (typos in
deko3d names are the only thing check-dk.py will catch; every `Dk*` name used is listed in
`docs/deko3d-ref/include/deko3d.h`). Anything not in a table → STOP, paste the problem.

## Goal

`game/graphics/deko3d_renderer/dkgl/dkgl_state.h` — the shared shadow state + the `Emitter`
interface every glad override talks to. With NO emitter installed, every dkgl entry point
only updates the shadow (this is what the host tests drive). GL enums arrive as plain
`u32` constants (values in `dkgl_gl_enums.h`, T104b) so the host build needs no glad.

## Create 1 — `game/graphics/deko3d_renderer/dkgl/dkgl_state.h` (verbatim)

```cpp
#pragma once
// T104a (AI-assisted): dkgl shadow state + emitter interface. GL-subset-on-deko3d layer
// for the ocean (docs/deko3d/DKGL_HYBRID_BRIEF.md). Host-testable: without an emitter,
// every entry point only updates the shadow. GL enums are raw u32 (dkgl_gl_enums.h).
// deko3d types appear only in Emitter; every name exists in deko3d.h 0.5.0.
#include "common/common_types.h"

#include <deko3d.h>

namespace dkgl {

// ---- constants -------------------------------------------------------------
constexpr u32 kMaxAttribs = 8;     // GL fixes locations 0..7 for the ocean attribs
constexpr u32 kMaxBuffers = 64;
constexpr u32 kMaxVaos = 16;
constexpr u32 kMaxTextures = 16;   // dkgl-created (glGenTextures) objects
constexpr u32 kMaxFbos = 4;
constexpr u32 kMaxTexUnits = 4;    // tex_T0..tex_T3 is all the ocean uses
constexpr u32 kTexIdBase = 0x4000'0000u;  // dkgl-created GL texture ids start here

// ---- emitter (implemented on Switch in T107; mocked in host tests) ---------
struct Emitter {
  virtual ~Emitter() = default;
  // lazy state, emitted at draw time by dkgl_draw (T106)
  virtual void bind_shaders(u32 program_id, const char* program_name) = 0;
  virtual void set_depth(const DkDepthStencilState& st) = 0;
  virtual void set_blend(const DkBlendState& st) = 0;
  virtual void set_viewport(const DkViewport& vp) = 0;
  virtual void set_vtx_format(const DkVtxAttribState* attribs, u32 n_attribs,
                              const DkVtxBufferState& vbuf, u64 vtx_gpu, u64 vtx_bytes) = 0;
  virtual void bind_idx_buffer(u64 gpu, DkIdxFormat fmt) = 0;
  virtual void bind_ubo(DkStage stage, u32 slot, u64 gpu, u32 bytes) = 0;
  virtual void bind_tex(u32 binding, u32 dk_tex_id, u32 sampler_combo) = 0;
  virtual void set_primitive_restart(bool enable, u32 index) = 0;
  // targets / clears / ordering
  virtual void bind_frame_target() = 0;  // FBO 0 = the dk frame colour+depth target
  virtual void bind_offscreen_target(u32 dkgl_tex_id) = 0;
  virtual void clear(bool color, bool depth, const float rgba[4]) = 0;
  virtual void barrier_before_sample() = 0;  // RT write -> later sample of that RT
  // per-frame rings (renaming semantics; see brief "Design lessons")
  virtual bool ring_alloc(u32 bytes, u32 align, void** cpu, u64* gpu) = 0;
  virtual bool ubo_alloc(u32 bytes, void** cpu, u64* gpu) = 0;
  // draws
  virtual void draw_arrays(DkPrimitive prim, u32 first, u32 count) = 0;
  virtual void draw_elements(DkPrimitive prim, u32 count, u32 index_offset_bytes) = 0;
};

// ---- shadow ------------------------------------------------------------------
struct AttribShadow {
  bool enabled = false;
  int size = 0;            // component count (1..4)
  u32 type = 0;            // GL_* type enum
  bool normalized = false;
  u32 stride = 0;
  u64 offset = 0;          // byte offset into the VBO
  u32 buffer = 0;          // GL_ARRAY_BUFFER bound at VertexAttribPointer time
};

struct BufferObj {
  bool live = false;
  bool valid = false;      // false = last ring_alloc failed -> draws using it are skipped
  u64 gpu = 0;
  void* cpu = nullptr;
  u32 bytes = 0;
};

struct VaoObj {
  bool live = false;
  AttribShadow attribs[kMaxAttribs];
  u32 element_buffer = 0;  // GL_ELEMENT_ARRAY_BUFFER is VAO state in GL
};

struct ShadowState {
  // caps (values are GL enum constants from dkgl_gl_enums.h)
  bool blend = false;
  bool depth_test = false;
  bool primitive_restart = false;
  // blend (defaults per GL spec: ONE / ZERO / FUNC_ADD)
  u32 blend_src_rgb = 1, blend_dst_rgb = 0;
  u32 blend_src_a = 1, blend_dst_a = 0;
  u32 blend_eq_rgb = 0x8006, blend_eq_a = 0x8006;
  // depth
  bool depth_write = true;
  u32 depth_func = 0x0201;  // GL_LESS
  // viewport / clear
  int vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
  float clear_color[4] = {0.f, 0.f, 0.f, 0.f};
  u32 restart_index = 0xFFFF'FFFFu;
  // scissor is NOT supported by the ocean surface; enable/glIsEnabled(GL_SCISSOR_TEST)
  // sets a counter instead (T104b) and the draw path logs first use.
};

// ---- core entry points (implemented across T104b/T105/T106) ------------------
// state
void enable(u32 cap);
void disable(u32 cap);
bool is_enabled(u32 cap);
void blend_func(u32 sfactor, u32 dfactor);
void blend_func_separate(u32 srgb, u32 drgb, u32 sa, u32 da);
void blend_equation(u32 mode);
void depth_mask(bool flag);
void depth_func(u32 func);
void viewport(int x, int y, int w, int h);
void clear_color(float r, float g, float b, float a);
void clear(u32 mask);  // recorded; emitted at draw time (T106)
void primitive_restart_index(u32 index);
// buffers / VAO
void gen_buffers(int n, u32* out);
void delete_buffers(int n, const u32* in);
void bind_buffer(u32 target, u32 name);
bool buffer_data(u32 target, u64 size, const void* data, u32 usage);
void gen_vertex_arrays(int n, u32* out);
void delete_vertex_arrays(int n, const u32* in);
void bind_vertex_array(u32 name);
void enable_vertex_attrib_array(u32 index);
void vertex_attrib_pointer(u32 index, int size, u32 type, bool normalized, u32 stride,
                           const void* pointer);
void vertex_attrib_i_pointer(u32 index, int size, u32 type, u32 stride, const void* pointer);
// textures / FBO (T105)
void gen_textures(int n, u32* out);
void delete_textures(int n, const u32* in);
void active_texture(u32 unit_enum);
void bind_texture(u32 name);
void tex_parameteri(u32 pname, u32 param);
bool tex_image_2d(u32 internal_format, int w, int h, u32 format, u32 type, const void* data);
void gen_framebuffers(int n, u32* out);
void delete_framebuffers(int n, const u32* in);
void bind_framebuffer(u32 name);
bool framebuffer_texture_2d(u32 target, u32 attachment, u32 tex_name, int level);
void draw_buffers(int n, const u32* bufs);
u32 check_framebuffer_status();
// programs / uniforms (T105)
void hint_program_name(const char* name);
u32 create_shader(u32 stage);
void shader_source(u32 shader, const char* src);
void compile_shader(u32 shader);
u32 create_program();
void attach_shader(u32 program, u32 shader);
void link_program(u32 program);
void delete_shader(u32 shader);
void use_program(u32 program);
void get_shaderiv(u32 shader, u32 pname, int* params);
void get_programiv(u32 program, u32 pname, int* params);
void get_shader_info_log(u32 shader, int max, int* len, char* log);
int get_uniform_location(u32 program, const char* name);
int get_uniform_block_index(u32 program, const char* name);
void uniform_block_binding(u32 program, u32 block, u32 binding);
void uniform1i(int location, int v);
void uniform1f(int location, float v);
void uniform4f(int location, float x, float y, float z, float w);
u32 get_error();
// draws (recorded here, emitted in T106)
void draw_arrays(u32 mode, int first, int count);
void draw_elements(u32 mode, int count, u32 type, const void* indices);

// ---- lifecycle ---------------------------------------------------------------
void set_emitter(Emitter* e);   // null = shadow-only (host tests)
void frame_begin();             // reset per-frame bookkeeping, keep objects
void begin_bucket();            // mark ALL state dirty so the next draw re-emits it
// read access for tests + T105/T106
ShadowState& shadow();
VaoObj* bound_vao();
BufferObj* buffer(u32 name);
u32 bound_array_buffer();
u32 bound_program();
u32 active_unit();
u32 bound_texture(u32 unit);
u64 draw_count();               // monotonic; T108 asserts on it

}  // namespace dkgl
```

## Create 2 — `game/graphics/deko3d_renderer/dkgl/dkgl_state.cpp`

~60 lines: the static globals (shadow, emitter pointer, object tables, dirty flags,
counters), `set_emitter`, `frame_begin`, `begin_bucket` (sets every dirty bit — the bit
set lives here as `u64 g_dirty; constexpr u64 kDirtyAll = ~0ull;`), and the trivial
accessors from the header. Every entry point listed above gets a 1-line stub body
`{ /* implemented in T104b/T105/T106 */ }` so the target links; the later tasks REPLACE
the stub bodies (grep `implemented in T10` to find them). No glad, no Switch-only code.

## Create 3 — `test/test_dkgl_state.cpp` + `test/CMakeLists.txt` row

Host gtest, ~80 lines. No emitter installed (`dkgl::set_emitter(nullptr)`):
1. defaults: `shadow().blend == false`, `depth_write == true`, `depth_func == 0x0201`,
   `blend_src_rgb == 1 && blend_dst_rgb == 0`, `restart_index == 0xFFFFFFFF`.
2. `enable(0x0BE2)` → `shadow().blend`; `enable(0x0B71)` → `shadow().depth_test`;
   `is_enabled` round-trips; unknown cap → no crash, no change.
3. `blend_func_separate(0x0302, 0x0303, 1, 0x0303)` sets the four fields;
   `blend_func(0x0306, 0)` sets src_rgb=0x0306 dst_rgb=0 AND src_a/dst_a to the same.
4. `depth_func(0x0203)` / `depth_mask(false)` / `viewport(0,0,512,448)` / `clear_color`.
5. `frame_begin()` keeps object tables (gen → frame_begin → still resolvable),
   `begin_bucket()` makes the next `T106` emit re-send everything (assert only the dirty
   flag semantics exposed via a tiny `bool dirty_all_consumed()` helper added to the .h
   by this task IF needed — otherwise assert indirectly in T108; prefer no new API: skip).

`test/CMakeLists.txt`: mirror the nearest host gtest row.

## Checks

- Host build + `test_dkgl_state` PASS.
- `python3 scripts/ai/check-dk.py` → PASS. The header's deko3d names:
  `DkShader, DkDepthStencilState, DkBlendState, DkViewport, DkVtxAttribState,
  DkVtxBufferState, DkIdxFormat, DkStage, DkPrimitive` — all in deko3d.h.
- No `glad` include, no `__SWITCH__` — pure C++.

## Report

Report: files + line counts, gtest tail, check-dk output, STOP conditions.
