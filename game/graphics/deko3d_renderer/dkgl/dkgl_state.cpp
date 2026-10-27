// T104a (AI-assisted): dkgl shadow-state implementation. State entry points update the
// shadow for real (that is what the host tests drive); textures/programs land in T105,
// draw emission in T106 (grep `implemented in T10` to find those).
// T104b (AI-assisted): buffers/VAO/state implemented -- glBufferData ring renaming
// (brief "Design lessons"), GL attrib-capture rules, pending draw recording (NO
// emission here; T106 reads the statics and emits).
#include <cstdio>
#include <cstring>

#include "game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"

namespace dkgl {
namespace {
ShadowState g_shadow;
Emitter* g_emitter = nullptr;
constexpr u64 kDirtyAll = ~0ull;
u64 g_dirty = kDirtyAll;
u64 g_draws = 0;
BufferObj g_buffers[kMaxBuffers];
// T104b: entry 0 = the DEFAULT VAO (name 0, always live); names 1..kMaxVaos.
VaoObj g_vaos[kMaxVaos + 1];
u32 g_next_buffer = 1, g_next_vao = 1;
u32 g_bound_vao = 0, g_bound_array_buffer = 0, g_bound_program = 0;
u32 g_active_texture_unit = 0;
u32 g_bound_textures[kMaxTexUnits] = {};
// T104b counters + per-frame bookkeeping
u64 g_unknown_cap_count = 0;          // enable/disable/is_enabled on an unsupported cap
u64 g_cpu_pointer_attrib_count = 0;   // VertexAttribPointer with no ARRAY_BUFFER bound
u64 g_odd_restart_index_count = 0;    // primitive_restart_index != 0xFFFFFFFF
u64 g_bad_buffer_target_count = 0;    // buffer_data on a target that is not ARRAY/ELEMENT
u32 g_pending_clear_mask = 0;         // glClear bits recorded; flushed by T106, reset per frame
u64 g_lost_clear_count = 0;           // T106: clears pending at frame_begin (must stay 0)
// dkgl review: between frame_begin and frame_end glBufferData renames into the frame ring;
// outside (renderer init) it gets persistent storage.
bool g_in_frame = false;

// last-draw params recorded by draw_arrays/draw_elements; T106 snapshots the
// VAO/buffer/program/texture units and emits -- nothing is emitted in T104b.
struct PendingDraw {
  bool indexed = false;
  u32 mode = 0;         // GL primitive enum
  int first = 0;
  int count = 0;
  u32 idx_type = 0;     // GL index type enum (draw_elements only)
  u64 idx_offset_bytes = 0;
};
PendingDraw g_pending_draw;

// name 0 is the default VAO and can never be deleted; other names die with the object.
bool vao_name_ok(u32 name) {
  return name <= kMaxVaos && (name == 0 || g_vaos[name].live);
}
}  // namespace

void set_emitter(Emitter* e) {
  g_emitter = e;
}
void frame_begin() {
  g_draws = 0;
  // T106: a clear still pending here was never flushed by a draw -- the ocean's order
  // always flushes before the frame ends; anything else is a bug (T108 asserts on it).
  if (g_pending_clear_mask != 0) {
    ++g_lost_clear_count;
  }
  g_pending_clear_mask = 0;  // T104b: a recorded clear that was never flushed dies with the frame
  // objects + shadow persist (GL semantics)
  g_in_frame = true;
}
void frame_end() {
  g_in_frame = false;
}
bool in_frame() {
  return g_in_frame;
}
void begin_bucket() {
  g_dirty = kDirtyAll;
  reset_emit_cache();  // T106: re-emit ALL state on the next draw (bucket boundary)
}

ShadowState& shadow() {
  return g_shadow;
}
VaoObj* bound_vao() {
  // Never null: name 0 is the default VAO and is always live (T104b, GL semantics).
  return &g_vaos[vao_name_ok(g_bound_vao) ? g_bound_vao : 0];
}
BufferObj* buffer(u32 name) {
  return (name && name <= kMaxBuffers && g_buffers[name - 1].live) ? &g_buffers[name - 1]
                                                                   : nullptr;
}
u32 bound_array_buffer() {
  return g_bound_array_buffer;
}
u32 bound_program() {
  return g_bound_program;
}
u32 active_unit() {
  return g_active_texture_unit;
}
u32 bound_texture(u32 unit) {
  return unit < kMaxTexUnits ? g_bound_textures[unit] : 0;
}
u64 draw_count() {
  return g_draws;
}
u64 unknown_cap_count() {
  return g_unknown_cap_count;
}
u64 cpu_pointer_attrib_count() {
  return g_cpu_pointer_attrib_count;
}
u32 pending_clear_mask() {
  return g_pending_clear_mask;
}
void consume_pending_clear() {
  g_pending_clear_mask = 0;  // T106 flush step 2
}
u64 lost_clear_count() {
  return g_lost_clear_count;
}

// ---- state entry points: shadow updates (host-testable) ----------------------
// T104b: BLEND / DEPTH_TEST / PRIMITIVE_RESTART are real shadow caps; SCISSOR_TEST and
// anything else bumps unknown_cap_count (the ocean surface never uses them; the T106
// draw path logs the first one it sees while emitting).
namespace {
bool cap_lookup(u32 cap, bool* out) {
  if (cap == gl::BLEND) {
    *out = g_shadow.blend;
  } else if (cap == gl::DEPTH_TEST) {
    *out = g_shadow.depth_test;
  } else if (cap == gl::PRIMITIVE_RESTART || cap == gl::PRIMITIVE_RESTART_FIXED_INDEX) {
    *out = g_shadow.primitive_restart;
  } else {
    return false;
  }
  return true;
}
bool set_cap(u32 cap, bool value) {
  bool sink;
  if (!cap_lookup(cap, &sink)) {
    ++g_unknown_cap_count;
    return false;
  }
  if (cap == gl::BLEND) {
    g_shadow.blend = value;
  } else if (cap == gl::DEPTH_TEST) {
    g_shadow.depth_test = value;
  } else {
    g_shadow.primitive_restart = value;
  }
  return true;
}
}  // namespace

void enable(u32 cap) {
  set_cap(cap, true);
}
void disable(u32 cap) {
  set_cap(cap, false);
}
bool is_enabled(u32 cap) {
  bool value = false;
  if (!cap_lookup(cap, &value)) {
    ++g_unknown_cap_count;
    return false;
  }
  return value;
}
void blend_func(u32 sfactor, u32 dfactor) {
  g_shadow.blend_src_rgb = g_shadow.blend_src_a = sfactor;
  g_shadow.blend_dst_rgb = g_shadow.blend_dst_a = dfactor;
}
void blend_func_separate(u32 srgb, u32 drgb, u32 sa, u32 da) {
  g_shadow.blend_src_rgb = srgb;
  g_shadow.blend_dst_rgb = drgb;
  g_shadow.blend_src_a = sa;
  g_shadow.blend_dst_a = da;
}
void blend_equation(u32 mode) {
  g_shadow.blend_eq_rgb = g_shadow.blend_eq_a = mode;
}
void depth_mask(bool flag) {
  g_shadow.depth_write = flag;
}
void depth_func(u32 func) {
  g_shadow.depth_func = func;
}
void viewport(int x, int y, int w, int h) {
  g_shadow.vp_x = x;
  g_shadow.vp_y = y;
  g_shadow.vp_w = w;
  g_shadow.vp_h = h;
}
void clear_color(float r, float g, float b, float a) {
  g_shadow.clear_color[0] = r;
  g_shadow.clear_color[1] = g;
  g_shadow.clear_color[2] = b;
  g_shadow.clear_color[3] = a;
}
void primitive_restart_index(u32 index) {
  // T104b: the ocean only ever passes 0xFFFFFFFF; anything else bumps a counter
  // (do NOT fail -- the value is still recorded).
  if (index != 0xFFFF'FFFFu) {
    ++g_odd_restart_index_count;
  }
  g_shadow.restart_index = index;
}
void note_table_full(const char* what) {
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    std::fprintf(stderr, "[dkgl] object table full: %s (raise kMax* in dkgl_state.h)\n", what);
  }
}
void get_integerv(u32 pname, int* out) {
  if (!out) {
    return;
  }
  if (pname == gl::VIEWPORT) {
    out[0] = g_shadow.vp_x;
    out[1] = g_shadow.vp_y;
    out[2] = g_shadow.vp_w;
    out[3] = g_shadow.vp_h;
  } else if (pname == gl::FRAMEBUFFER_BINDING) {
    out[0] = (int)bound_framebuffer();
  } else {
    out[0] = 0;
  }
}
void clear(u32 mask) {
  // T104b: recorded only. The T106 draw/clear flush emits it (colour via the shadow's
  // clear_color, depth via depth-write enable); frame_begin clears the mask.
  g_pending_clear_mask |= mask;
}

// ---- buffers / VAO: name allocation + shadow binding -------------------------
void gen_buffers(int n, u32* out) {
  for (int i = 0; i < n; ++i) {
    out[i] = 0;
    for (u32 attempt = 0; attempt < kMaxBuffers && out[i] == 0; ++attempt) {
      const u32 name = g_next_buffer;
      g_next_buffer = g_next_buffer % kMaxBuffers + 1;
      if (!g_buffers[name - 1].live) {
        g_buffers[name - 1] = BufferObj{};
        g_buffers[name - 1].live = true;
        out[i] = name;
      }
    }
    if (out[i] == 0) {
      note_table_full("buffers");
    }
  }
}
void delete_buffers(int n, const u32* in) {
  for (int i = 0; i < n; ++i) {
    if (in[i] && in[i] <= kMaxBuffers) {
      g_buffers[in[i] - 1].live = false;
      if (g_bound_array_buffer == in[i]) {
        g_bound_array_buffer = 0;
      }
      // T104b: a deleted element buffer must vanish from every VAO that binds it.
      for (auto& vao : g_vaos) {
        if (vao.element_buffer == in[i]) {
          vao.element_buffer = 0;
        }
      }
    }
  }
}
void bind_buffer(u32 target, u32 name) {
  // T104b: the element binding is VAO state (GL semantics) and lands in the CURRENT
  // VAO; the array binding is global and is captured by vertex_attrib_pointer.
  if (target == gl::ARRAY_BUFFER) {
    g_bound_array_buffer = name;
  } else if (target == gl::ELEMENT_ARRAY_BUFFER) {
    bound_vao()->element_buffer = name;
  }
  // any other target: ignored (nothing in the ocean subset uses one)
}
void gen_vertex_arrays(int n, u32* out) {
  for (int i = 0; i < n; ++i) {
    out[i] = 0;
    for (u32 attempt = 0; attempt < kMaxVaos && out[i] == 0; ++attempt) {
      const u32 name = g_next_vao;  // names 1..kMaxVaos; 0 is the default VAO
      g_next_vao = g_next_vao % kMaxVaos + 1;
      if (!g_vaos[name].live) {
        g_vaos[name] = VaoObj{};
        g_vaos[name].live = true;
        out[i] = name;
      }
    }
    if (out[i] == 0) {
      note_table_full("vertex arrays");
    }
  }
}
void delete_vertex_arrays(int n, const u32* in) {
  for (int i = 0; i < n; ++i) {
    if (in[i] && in[i] <= kMaxVaos) {  // name 0 = default VAO: never deleted
      g_vaos[in[i]].live = false;
      if (g_bound_vao == in[i]) {
        g_bound_vao = 0;
      }
    }
  }
}
void bind_vertex_array(u32 name) {
  g_bound_vao = vao_name_ok(name) ? name : 0;
}
void enable_vertex_attrib_array(u32 index) {
  // T104b: bound_vao() never returns null (name 0 = default VAO), so attrib arrays
  // are legal without binding one -- GL compatibility semantics.
  if (index < kMaxAttribs) {
    bound_vao()->attribs[index].enabled = true;
  }
}
void use_program(u32 program) {
  g_bound_program = program;
  mark_program_used(program);  // T105: ever_used bookkeeping (dkgl_program.cpp)
}
void active_texture(u32 unit_enum) {
  // T104b: GL_TEXTURE0 (0x84C0) + i -> unit index.
  if (unit_enum >= gl::TEXTURE0 && unit_enum < gl::TEXTURE0 + kMaxTexUnits) {
    g_active_texture_unit = unit_enum - gl::TEXTURE0;
  }
  // invalid enum: keep the current unit (nothing in the ocean subset sends one)
}

// ---- buffers: glBufferData ring renaming (T104b) ------------------------------
bool buffer_data(u32 target, u64 size, const void* data, u32 usage) {
  (void)usage;  // GL usage hints are meaningless under ring renaming
  if (target != gl::ARRAY_BUFFER && target != gl::ELEMENT_ARRAY_BUFFER) {
    ++g_bad_buffer_target_count;
    return false;
  }
  const u32 name =
      (target == gl::ARRAY_BUFFER) ? g_bound_array_buffer : bound_vao()->element_buffer;
  BufferObj* bo = buffer(name);
  if (!bo || size > 0xFFFF'FFFFull) {
    return false;
  }
  // RENAMING: every glBufferData is a FRESH ring slice -- the previous slice is never
  // reused while the GPU may still read it (brief "Design lessons"). Caller contract:
  // on false, draws using this buffer are skipped, never crash.
  void* cpu = nullptr;
  u64 gpu = 0;
  // A size-only glBufferData outside a frame (CommonOceanRenderer sizes its stream buffers
  // at init) reserves nothing: the first in-frame upload renames it anyway.
  if (!g_in_frame && !data) {
    bo->valid = false;
    bo->cpu = nullptr;
    bo->gpu = 0;
    bo->bytes = 0;
    return true;
  }
  // Outside a frame (renderer init: static quads, index lists) the data must outlive the
  // ring rotation -> persistent storage (dkgl review).
  const bool ok = g_emitter && (g_in_frame ? g_emitter->ring_alloc((u32)size, 16, &cpu, &gpu)
                                           : g_emitter->persistent_alloc((u32)size, 16, &cpu, &gpu));
  if (!ok) {
    bo->valid = false;
    bo->cpu = nullptr;
    bo->gpu = 0;
    bo->bytes = 0;
    return false;
  }
  if (data && size) {
    memcpy(cpu, data, (size_t)size);
  }
  bo->cpu = cpu;
  bo->gpu = gpu;
  bo->bytes = (u32)size;
  bo->valid = true;
  return true;
}

// ---- vertex attribs: captured at CALL time into the current VAO (T104b) --------
namespace {
// GL stride 0 = tightly packed (OceanTexture_PC's position VBO): the real vertex size.
u32 packed_stride(int size, u32 type) {
  if (type == gl::INT_2_10_10_10_REV || type == gl::UNSIGNED_INT_2_10_10_10_REV) {
    return 4;
  }
  const u32 comp = (type == gl::UNSIGNED_BYTE) ? 1 : (type == gl::UNSIGNED_SHORT ? 2 : 4);
  return comp * (u32)(size > 0 ? size : 1);
}
void capture_attrib(u32 index, int size, u32 type, bool normalized, bool integer, u32 stride,
                    const void* pointer) {
  if (index >= kMaxAttribs) {
    return;  // GL fixes locations 0..7 for the ocean attribs; anything else is a bug
  }
  AttribShadow& a = bound_vao()->attribs[index];
  a.size = size;
  a.type = type;
  a.normalized = normalized;
  a.integer = integer;
  a.stride = stride ? stride : packed_stride(size, type);
  a.offset = (u64)pointer;
  a.buffer = g_bound_array_buffer;  // GL captures the ARRAY_BUFFER binding NOW, not at draw
  if (a.buffer == 0) {
    // CPU-pointer draws are unsupported: counted here, skipped by the T106 draw path.
    ++g_cpu_pointer_attrib_count;
  }
}
}  // namespace

void vertex_attrib_pointer(u32 index, int size, u32 type, bool normalized, u32 stride,
                           const void* pointer) {
  capture_attrib(index, size, type, normalized, false, stride, pointer);
}
void vertex_attrib_i_pointer(u32 index, int size, u32 type, u32 stride, const void* pointer) {
  capture_attrib(index, size, type, false, true, stride, pointer);  // integer path: never normalized
}

// ---- draws: record + flush (dkgl_draw.cpp emits; T106) -------------------------
void draw_arrays(u32 mode, int first, int count) {
  ++g_draws;
  g_pending_draw = PendingDraw{};
  g_pending_draw.indexed = false;
  g_pending_draw.mode = mode;
  g_pending_draw.first = first;
  g_pending_draw.count = count;
  flush_pending(mode, first, count, 0, 0, false);
}
void draw_elements(u32 mode, int count, u32 type, const void* indices) {
  ++g_draws;
  g_pending_draw = PendingDraw{};
  g_pending_draw.indexed = true;
  g_pending_draw.mode = mode;
  g_pending_draw.count = count;
  g_pending_draw.idx_type = type;
  g_pending_draw.idx_offset_bytes = (u64)indices;
  flush_pending(mode, 0, count, type, (u64)indices, true);
}

// ---- T105 entry points live in dkgl_texture.cpp / dkgl_program.cpp ------------

// ---- internal seams used by the T105 files ------------------------------------
Emitter* emitter() {
  return g_emitter;
}
void set_bound_texture(u32 name) {
  g_bound_textures[g_active_texture_unit] = name;
}
void forget_texture_binding(u32 name) {
  for (u32 u = 0; u < kMaxTexUnits; ++u) {
    if (g_bound_textures[u] == name) {
      g_bound_textures[u] = 0;
    }
  }
}

}  // namespace dkgl
