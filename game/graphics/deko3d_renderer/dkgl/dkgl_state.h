#pragma once
// T104a (AI-assisted): dkgl shadow state + emitter interface. GL-subset-on-deko3d layer
// for the ocean (docs/deko3d/DKGL_HYBRID_BRIEF.md). Host-testable: without an emitter,
// every entry point only updates the shadow. GL enums are raw u32 (dkgl_gl_enums.h).
// deko3d types appear only in Emitter; every name exists in deko3d.h 0.5.0.
#include "common/common_types.h"

#include <deko3d.h>

// T105: T103 generated uniform-offset tables (defines UniformSlot / StageUniformTable /
// ProgramUniformTable inside namespace dkgl; it opens the namespace itself).
#include "game/graphics/deko3d_renderer/dkgl/dkgl_uniform_table.inc"

namespace dkgl {

// ---- constants -------------------------------------------------------------
constexpr u32 kMaxAttribs = 8;     // GL fixes locations 0..7 for the ocean attribs
// dkgl review (AI-assisted): sized for the ocean + Generic2. The ocean alone needs 11+ FBOs
// (FramebufferTexturePair makes one per mip level: the result texture has 8); with 4, the
// extra glGenFramebuffers returned 0 = the frame target and the mip passes drew a square in
// the bottom-left corner of the screen. Running out is logged once (dkgl_state.cpp).
// TODO #5: Merc2 adds a bone buffer + one buffer/VAO pair per blerc (MOD_VTX) draw
constexpr u32 kMaxBuffers = 512;
constexpr u32 kMaxVaos = 128;
constexpr u32 kMaxTextures = 64;   // dkgl-created (glGenTextures) objects
constexpr u32 kMaxFbos = 64;
// = DK_NUM_TEXTURE_BINDINGS: DirectRenderer (far ocean) samples on GL_TEXTURE20 (tex_T20)
constexpr u32 kMaxTexUnits = 32;
constexpr u32 kTexIdBase = 0x4000'0000u;  // dkgl-created GL texture ids start here
// TODO #5 (AI-assisted): dkgl buffer names start here. Lower non-zero names belong to the
// Loader (GpuBufferPool chunk ids 1, 2, ...: lev->merc_vertices etc.) and are resolved
// through set_external_buffer_lookup -- the two name spaces used to overlap.
constexpr u32 kBufIdBase = 0x2000'0000u;
constexpr u32 kMaxUboBindings = 8;  // glBindBufferRange(GL_UNIFORM_BUFFER, index < 8)
// = DK_UNIFORM_BUF_ALIGNMENT: what GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT reports, so Merc2
// places every glBindBufferRange offset where deko3d can bind it
constexpr u32 kUboAlign = 0x100;

// ---- emitter (implemented on Switch in T107; mocked in host tests) ---------
struct Emitter {
  virtual ~Emitter() = default;
  // lazy state, emitted at draw time by dkgl_draw (T106)
  virtual void bind_shaders(u32 program_id, const char* program_name) = 0;
  virtual void set_depth(const DkDepthStencilState& st) = 0;
  virtual void set_blend(const DkBlendState& st) = 0;
  virtual void set_viewport(const DkViewport& vp) = 0;
  // dkgl review (AI-assisted): one DkVtxBufferState per DISTINCT vertex buffer of the VAO
  // (OceanTexture_PC draws from a static position VBO + a dynamic data VBO at once);
  // attribs[i].buffer indexes bufs/buf_gpu/buf_bytes.
  virtual void set_vtx_format(const DkVtxAttribState* attribs, u32 n_attribs,
                              const DkVtxBufferState* bufs, const u64* buf_gpu,
                              const u32* buf_bytes, u32 n_bufs) = 0;
  virtual void bind_idx_buffer(u64 gpu, DkIdxFormat fmt) = 0;
  virtual void bind_ubo(DkStage stage, u32 slot, u64 gpu, u32 bytes) = 0;
  virtual void bind_tex(u32 binding, u32 dk_tex_id, u32 sampler_combo) = 0;
  virtual void set_primitive_restart(bool enable, u32 index) = 0;
  // T111: glBlendColor -> dkCmdBufSetBlendConst (GL_CONSTANT_COLOR factors in Generic2)
  virtual void set_blend_color(const float rgba[4]) { (void)rgba; }
  // T113 (AI-assisted): glStencilMask/glStencilFunc ref+mask -> dkCmdBufSetStencil, and
  // glColorMask -> DkColorWriteState (DkColorMask_* bits, R=1 G=2 B=4 A=8).
  virtual void set_stencil(u8 write_mask, u8 ref, u8 func_mask) {
    (void)write_mask, (void)ref, (void)func_mask;
  }
  virtual void set_color_mask(u32 mask) { (void)mask; }
  // T112 (AI-assisted): glBlitFramebuffer from the frame target into a dkgl texture.
  virtual void blit_frame_to_tex(u32 dk_tex_id, const DkImageRect& src, const DkImageRect& dst,
                                 bool linear) {
    (void)dk_tex_id, (void)src, (void)dst, (void)linear;
  }
  // targets / clears / ordering
  virtual void bind_frame_target() = 0;  // FBO 0 = the dk frame colour+depth target
  // dk texture id + mip level: the ocean renders its mip chain level by level
  // (FramebufferTexturePairContext(fb, level) -> glFramebufferTexture2D(..., level)).
  virtual void bind_offscreen_target(u32 dk_tex_id, u32 level) = 0;
  virtual void clear(bool color, bool depth, const float rgba[4]) = 0;
  virtual void barrier_before_sample() = 0;  // RT write -> later sample of that RT
  // per-frame rings (renaming semantics; see brief "Design lessons")
  virtual bool ring_alloc(u32 bytes, u32 align, void** cpu, u64* gpu) = 0;
  // dkgl review: storage that is never recycled, for glBufferData outside a frame (the
  // renderers' init-time GL_STATIC_DRAW quads would be lost when the frame ring wraps).
  virtual bool persistent_alloc(u32 bytes, u32 align, void** cpu, u64* gpu) {
    return ring_alloc(bytes, align, cpu, gpu);
  }
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
  bool integer = false;    // dkgl review: glVertexAttribIPointer (the shader reads ints)
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

// TODO #5: a buffer that dkgl did not create (the Loader's level data). Fills `out`
// (cpu/gpu/bytes, valid = true) and returns true if `name` is known. Installed on Switch by
// dkgl_frame (dk_loader_buffer_find); host tests install their own or none.
using ExternalBufferLookup = bool (*)(u32 name, BufferObj* out);
void set_external_buffer_lookup(ExternalBufferLookup fn);
// glBindBufferRange(GL_UNIFORM_BUFFER, index, ...) state; flush_pending binds every set
// index to the vertex + fragment stage. begin_bucket clears them (each bucket rebinds).
struct UboBinding {
  u32 name = 0;
  u64 offset = 0;
  u32 size = 0;
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
  float blend_color[4] = {0.f, 0.f, 0.f, 0.f};  // T111: glBlendColor (GL default 0)
  u32 restart_index = 0xFFFF'FFFFu;
  // T113 (AI-assisted): stencil + colour mask (GL defaults; ShadowRenderer uses them)
  bool stencil_test = false;
  u32 stencil_func = 0x0207;  // GL_ALWAYS
  u32 stencil_ref = 0, stencil_func_mask = 0xFF, stencil_write_mask = 0xFF;
  u32 stencil_sfail = 0x1E00, stencil_dpfail = 0x1E00, stencil_dppass = 0x1E00;  // GL_KEEP
  u32 color_mask = 0xF;  // bit0 R, bit1 G, bit2 B, bit3 A (== DkColorMask_*)
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
// T113 (AI-assisted): stencil + colour mask
void stencil_func(u32 func, int ref, u32 mask);
void stencil_op(u32 sfail, u32 dpfail, u32 dppass);
void stencil_mask(u32 mask);
void color_mask(bool r, bool g, bool b, bool a);
// dkgl review: GL_VIEWPORT / GL_FRAMEBUFFER_BINDING (FramebufferTexturePairContext saves and
// restores both); every other pname writes 0.
void get_integerv(u32 pname, int* out);
// dkgl review: one stderr line the first time an object table is full (gen_* returned 0)
void note_table_full(const char* what);
// buffers / VAO
void gen_buffers(int n, u32* out);
void delete_buffers(int n, const u32* in);
void bind_buffer(u32 target, u32 name);
bool buffer_data(u32 target, u64 size, const void* data, u32 usage);
// glBufferSubData: renamed like buffer_data (DirectRenderer writes its ring at an offset)
bool buffer_sub_data(u32 target, u64 offset, u64 size, const void* data);
// TODO #5: glBindBufferRange / glBindBufferBase on GL_UNIFORM_BUFFER (others are ignored)
void bind_buffer_range(u32 target, u32 index, u32 name, u64 offset, u64 size);
const UboBinding& ubo_binding(u32 index);  // index >= kMaxUboBindings -> an empty binding
u32 bound_uniform_buffer();
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
// T112: `level` is the GL mip level. Level 0 with new dimensions RESPECIFIES the texture
// (DepthCue sizes its targets 1x1 at init, then to the screen size in setup()).
bool tex_image_2d(int level, u32 internal_format, int w, int h, u32 format, u32 type,
                  const void* data);
void gen_framebuffers(int n, u32* out);
void delete_framebuffers(int n, const u32* in);
void bind_framebuffer(u32 name);
// T112: glBindFramebuffer with its target. READ only changes the blit source; DRAW and
// FRAMEBUFFER change the render target (FRAMEBUFFER also sets the read binding).
void bind_framebuffer_target(u32 target, u32 name);
// T112: frame target (read FBO 0) -> a texture-backed draw FBO, colour only. Returns false
// (and counts) for anything else.
bool blit_framebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                      u32 mask, u32 filter);
u64 unsupported_blit_count();
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
// #19 (AI-assisted): programs linked after this call with that name get no uniform table, so
// their draws are inert (used to keep the native merc for an A/B). nullptr/"" clears it.
void force_program_inert(const char* name);
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
void uniform4fv(int location, int count, const float* v);  // T112 (DepthCue u_color, count 1)
// T111: Generic2's extra uniform calls + glBlendColor
void uniform1ui(int location, u32 v);
void uniform3f(int location, float x, float y, float z);
void uniform_matrix4fv(int location, int count, bool transpose, const float* m);
void blend_color(float r, float g, float b, float a);
u32 get_error();
// draws (recorded here, emitted in T106)
void draw_arrays(u32 mode, int first, int count);
void draw_elements(u32 mode, int count, u32 type, const void* indices);

// ---- lifecycle ---------------------------------------------------------------
void set_emitter(Emitter* e);   // null = shadow-only (host tests)
void frame_begin();             // reset per-frame bookkeeping, keep objects
void frame_end();               // dkgl review: glBufferData is persistent until the next frame_begin
bool in_frame();
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
// T104b: telemetry + per-frame bookkeeping (draw path logs first use in T106)
u64 unknown_cap_count();        // enable/disable/glIsEnabled on an unsupported cap
u64 cpu_pointer_attrib_count(); // VertexAttribPointer with no ARRAY_BUFFER bound
u32 pending_clear_mask();       // glClear bits recorded, flushed by the T106 draw path

// ---- T105: textures/FBO (dkgl_texture.cpp) + programs (dkgl_program.cpp) -------
// uniform tables are the T103 generated .inc (included at the top of this file);
// program_table() returns null for inert programs (name not in the table -> the
// program draws nothing, by design).
u32 resolve_texture_id(u32 gl_id);      // dkgl pseudo id -> dktexgpu id; passthrough otherwise
u32 materialized_dk_tex(u32 gl_id);     // 0 = none / unknown id
bool texture_needs_barrier(u32 gl_id);  // was rendered to and no barrier sent yet
void texture_barrier_sent(u32 gl_id);
const ProgramUniformTable* program_table(u32 program);  // null = inert
const u8* program_uniform_shadow(u32 program);          // 512 bytes (vert block + frag block), nullptr if dead
bool program_uniform_dirty(u32 program);
void program_uniform_clear_dirty(u32 program);
// internal seams between dkgl_state.cpp and the T105 files
Emitter* emitter();                     // the installed emitter (null on host w/o mock)
void set_bound_texture(u32 name);       // record `name` on the CURRENT texture unit
void forget_texture_binding(u32 name);  // zero any unit bound to a deleted texture
void mark_program_used(u32 program);    // ever_used bookkeeping for use_program

// ---- T106: draw flush (dkgl_draw.cpp) + pending-target seam ---------------------
void flush_pending(u32 mode, int first, int count, u32 idx_type, u64 idx_off, bool indexed);
void reset_emit_cache();            // begin_bucket: re-emit everything on the next draw
// T113: undo per-draw state a renderer left on the GPU but never drew with again (shadow ends
// with glColorMask(1,1,1,1) + glDisable(GL_STENCIL_TEST) and no draw): the native passes
// that follow must see RGBA writes and no stencil test. Called before a bucket's last list ends.
void restore_bucket_end_state();
u64 inert_draw_count();             // draws skipped because the program is not in the table
u64 skipped_draw_count();           // dkgl review: draws dropped (unknown attrib format, no IBO...)
u64 lost_clear_count();             // clears still pending at frame_begin (must stay 0)
void consume_pending_clear();       // flush step 2 zeroes the recorded glClear bits
// pending target switch queued by bind_framebuffer, emitted at the next draw
bool pending_target_switch();
bool pending_target_offscreen();
u32 pending_target_dk_tex();
u32 pending_target_level();         // mip level attached to the pending offscreen FBO
u32 bound_framebuffer();
void pending_target_consumed();
u32 texture_sampler_combo(u32 gl_id);  // bit0 min_linear | bit1 mag_linear | bit2 mip_linear | bit3 repeat

#if defined(__SWITCH__)
// dkgl_glad.cpp: overwrite the dk_gl_census no-op stubs for the dkgl subset. The
// census installs first (boot); this runs later (T107 init) and wins. No uninstall.
void install_glad();
#endif

}  // namespace dkgl
