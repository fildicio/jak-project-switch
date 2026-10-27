// T104a (AI-assisted): host golden checks on the dkgl shadow state. No emitter is
// installed, so every entry point only touches the shadow. NOTE: relies on gtest
// declaration order -- Defaults runs first against the pristine static shadow.
#include "game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

using namespace dkgl;

TEST(DkglState, Defaults) {
  set_emitter(nullptr);
  const auto& sh = shadow();
  EXPECT_FALSE(sh.blend);
  EXPECT_TRUE(sh.depth_write);
  EXPECT_EQ(sh.depth_func, 0x0201u);  // GL_LESS
  EXPECT_EQ(sh.blend_src_rgb, 1u);    // GL_ONE
  EXPECT_EQ(sh.blend_dst_rgb, 0u);    // GL_ZERO
  EXPECT_EQ(sh.blend_eq_rgb, 0x8006u);
  EXPECT_EQ(sh.restart_index, 0xFFFF'FFFFu);
  EXPECT_EQ(draw_count(), 0u);
}

TEST(DkglState, Caps) {
  set_emitter(nullptr);
  enable(0x0BE2u);  // GL_BLEND
  EXPECT_TRUE(shadow().blend);
  EXPECT_TRUE(is_enabled(0x0BE2u));
  enable(0x0B71u);  // GL_DEPTH_TEST
  EXPECT_TRUE(shadow().depth_test);
  EXPECT_TRUE(is_enabled(0x0B71u));
  disable(0x0BE2u);
  EXPECT_FALSE(is_enabled(0x0BE2u));
  // unknown cap: no crash, no change
  const auto before = shadow();
  enable(0xBAD0u);
  disable(0xBAD1u);
  EXPECT_FALSE(is_enabled(0xBAD0u));
  EXPECT_EQ(shadow().blend, before.blend);
  EXPECT_EQ(shadow().depth_test, before.depth_test);
}

TEST(DkglState, BlendFuncs) {
  set_emitter(nullptr);
  blend_func_separate(0x0302u, 0x0303u, 1u, 0x0303u);
  EXPECT_EQ(shadow().blend_src_rgb, 0x0302u);
  EXPECT_EQ(shadow().blend_dst_rgb, 0x0303u);
  EXPECT_EQ(shadow().blend_src_a, 1u);
  EXPECT_EQ(shadow().blend_dst_a, 0x0303u);
  blend_func(0x0306u, 0u);
  EXPECT_EQ(shadow().blend_src_rgb, 0x0306u);
  EXPECT_EQ(shadow().blend_dst_rgb, 0u);
  EXPECT_EQ(shadow().blend_src_a, 0x0306u);
  EXPECT_EQ(shadow().blend_dst_a, 0u);
}

TEST(DkglState, DepthViewportClear) {
  set_emitter(nullptr);
  depth_func(0x0203u);  // GL_LEQUAL
  depth_mask(false);
  viewport(0, 0, 512, 448);
  clear_color(0.1f, 0.2f, 0.3f, 0.4f);
  primitive_restart_index(0xFFFFu);
  const auto& sh = shadow();
  EXPECT_EQ(sh.depth_func, 0x0203u);
  EXPECT_FALSE(sh.depth_write);
  EXPECT_EQ(sh.vp_w, 512);
  EXPECT_EQ(sh.vp_h, 448);
  EXPECT_FLOAT_EQ(sh.clear_color[2], 0.3f);
  EXPECT_EQ(sh.restart_index, 0xFFFFu);
}

TEST(DkglState, FrameBeginKeepsObjects) {
  set_emitter(nullptr);
  u32 vao = 0;
  gen_vertex_arrays(1, &vao);
  bind_vertex_array(vao);
  enable_vertex_attrib_array(0);
  u32 buf = 0;
  gen_buffers(1, &buf);
  ASSERT_NE(buf, 0u);
  frame_begin();
  EXPECT_NE(buffer(buf), nullptr);      // object tables survive the frame
  EXPECT_NE(bound_vao(), nullptr);      // binding survives too (GL semantics)
  EXPECT_TRUE(bound_vao()->attribs[0].enabled);
  begin_bucket();  // dirty-all: consumed by the T106 emit path
}

// ---- T104b: buffers / VAO / counters -------------------------------------------

namespace {
// Records ring_alloc calls; every other emitter virtual appends to a log string.
// ring slices come from the mock's own arena so tests can prove renaming (the
// shadow's memcpy lands in the arena and must never overwrite the first slice).
class MockEmitter : public Emitter {
 public:
  u8 arena[4096];
  u32 arena_used = 0;
  bool fail_ring_alloc = false;
  struct RingCall {
    u32 bytes, align;
    void* cpu;
    u64 gpu;
  };
  std::vector<RingCall> ring_calls;
  std::string log;

  MockEmitter() {
    memset(arena, 0xAB, sizeof(arena));
  }

  bool ring_alloc(u32 bytes, u32 align, void** cpu, u64* gpu) override {
    (void)align;
    if (fail_ring_alloc || arena_used + bytes > sizeof(arena)) {
      return false;
    }
    *cpu = arena + arena_used;
    *gpu = 0x1000'0000ull + arena_used;
    arena_used += (bytes + 15u) & ~15u;
    ring_calls.push_back({bytes, align, *cpu, *gpu});
    return true;
  }
  bool ubo_alloc(u32 bytes, void** cpu, u64* gpu) override {
    return ring_alloc(bytes, 0x100, cpu, gpu);
  }
  u32 persistent_calls = 0;
  bool persistent_alloc(u32 bytes, u32 align, void** cpu, u64* gpu) override {
    ++persistent_calls;
    return ring_alloc(bytes, align, cpu, gpu);
  }
  void bind_idx_buffer(u64, DkIdxFormat) override { log += "idx;"; ++idx_binds; }
  struct UboBind {
    DkStage stage;
    u64 gpu;
    u32 bytes;
  };
  std::vector<UboBind> ubo_binds;
  void bind_ubo(DkStage stage, u32, u64 gpu, u32 bytes) override {
    log += "ubo;";
    ubo_binds.push_back({stage, gpu, bytes});
  }
  void bind_tex(u32 unit, u32 dk_tex_id, u32 combo) override {
    log += "tex;";
    tex_binds.push_back({unit, dk_tex_id, combo});
  }
  void set_primitive_restart(bool, u32) override { log += "restart;"; ++restart_sets; }
  u32 frame_target_calls = 0;
  std::vector<u32> offscreen_ids;
  void bind_frame_target() override {
    log += "frame;";
    ++frame_target_calls;
  }
  std::vector<u32> offscreen_levels;
  void bind_offscreen_target(u32 id, u32 level) override {
    log += "offscreen;";
    offscreen_ids.push_back(id);
    offscreen_levels.push_back(level);
  }
  std::vector<std::pair<u32, std::string>> shader_binds;
  std::vector<DkDepthStencilState> depth_states;
  std::vector<DkBlendState> blend_states;
  std::vector<DkViewport> viewport_states;
  u32 barrier_count = 0;
  u32 idx_binds = 0;
  u32 restart_sets = 0;
  u32 vtx_formats = 0;
  u32 draws_a = 0, draws_e = 0;
  std::vector<std::array<u32, 3>> tex_binds;
  void bind_shaders(u32 program_id, const char* program_name) override {
    log += "shaders(" + std::to_string(program_id) + ":" + program_name + ");";
    shader_binds.push_back({program_id, program_name});
  }
  void set_depth(const DkDepthStencilState& st) override {
    log += "depth;";
    depth_states.push_back(st);
  }
  void set_blend(const DkBlendState& st) override {
    log += "blend;";
    blend_states.push_back(st);
  }
  void set_viewport(const DkViewport& vp) override {
    log += "viewport;";
    viewport_states.push_back(vp);
  }
  u32 last_vtx_bufs = 0;
  void set_vtx_format(const DkVtxAttribState*, u32, const DkVtxBufferState*, const u64*,
                      const u32*, u32 n_bufs) override {
    log += "vtx;";
    ++vtx_formats;
    last_vtx_bufs = n_bufs;
  }
  void barrier_before_sample() override {
    log += "barrier;";
    ++barrier_count;
  }
  void draw_arrays(DkPrimitive, u32, u32) override {
    log += "draw_a;";
    ++draws_a;
  }
  void draw_elements(DkPrimitive, u32, u32) override {
    log += "draw_e;";
    ++draws_e;
  }
  void clear(bool, bool, const float[4]) override { log += "clear;"; }
};
}  // namespace

TEST(DkglState, BufferDataRenames) {
  MockEmitter mock;
  set_emitter(&mock);
  u32 buf = 0;
  gen_buffers(1, &buf);
  bind_buffer(gl::ARRAY_BUFFER, buf);
  const u8 first[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const u8 second[16] = {0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
                         0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF, 0x00};
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(first), first, 0));
  const u8* first_bytes = (const u8*)buffer(buf)->cpu;
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(second), second, 0));
  // two buffer_data on the SAME name -> two ring slices with distinct cpu pointers
  ASSERT_EQ(mock.ring_calls.size(), 2u);
  EXPECT_NE(mock.ring_calls[0].cpu, mock.ring_calls[1].cpu);
  EXPECT_NE(buffer(buf)->cpu, (const void*)first_bytes);
  // renaming: the second upload must not have touched the first slice
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(first_bytes[i], first[i]) << "byte " << i << " of the first slice";
  }
  // the buffer object now serves the second slice's bytes
  EXPECT_EQ(((const u8*)buffer(buf)->cpu)[0], 0xF1);
  set_emitter(nullptr);
}

TEST(DkglState, AttribCapturesArrayBindingAtCallTime) {
  set_emitter(nullptr);
  u32 vao = 0;
  gen_vertex_arrays(1, &vao);
  bind_vertex_array(vao);
  u32 b1 = 0, b2 = 0;
  gen_buffers(1, &b1);
  gen_buffers(1, &b2);
  bind_buffer(gl::ARRAY_BUFFER, b1);
  vertex_attrib_pointer(0, 3, gl::FLOAT, false, 32, (void*)0);
  bind_buffer(gl::ARRAY_BUFFER, b2);
  vertex_attrib_pointer(1, 4, gl::FLOAT, false, 32, (void*)12);
  // the ARRAY_BUFFER binding at CALL time is captured, not the one at draw time
  const auto& a = bound_vao()->attribs;
  EXPECT_EQ(a[0].buffer, b1);
  EXPECT_EQ(a[1].buffer, b2);
  EXPECT_EQ(a[0].size, 3);
  EXPECT_EQ(a[1].size, 4);
  EXPECT_EQ(a[1].offset, 12u);
}

TEST(DkglState, ElementBufferIsVaoState) {
  set_emitter(nullptr);
  u32 va = 0, vb = 0;
  gen_vertex_arrays(1, &va);
  gen_vertex_arrays(1, &vb);
  ASSERT_NE(va, 0u);
  ASSERT_NE(vb, 0u);
  ASSERT_NE(va, vb);
  bind_vertex_array(va);
  bind_buffer(gl::ELEMENT_ARRAY_BUFFER, 9);
  EXPECT_EQ(bound_vao()->element_buffer, 9u);
  bind_vertex_array(vb);
  EXPECT_EQ(bound_vao()->element_buffer, 0u);  // vb never bound an element buffer
  bind_vertex_array(va);
  EXPECT_EQ(bound_vao()->element_buffer, 9u);  // va still remembers it
}

TEST(DkglState, BufferDataFailMarksBufferInvalid) {
  MockEmitter mock;
  mock.fail_ring_alloc = true;
  set_emitter(&mock);
  u32 buf = 0;
  gen_buffers(1, &buf);
  bind_buffer(gl::ARRAY_BUFFER, buf);
  EXPECT_FALSE(buffer_data(gl::ARRAY_BUFFER, 64, "data", 0));
  EXPECT_FALSE(buffer(buf)->valid);  // draws using it are skipped, never crash
  set_emitter(nullptr);
}

TEST(DkglState, CpuPointerAttribCounted) {
  set_emitter(nullptr);
  const u64 before = cpu_pointer_attrib_count();
  bind_buffer(gl::ARRAY_BUFFER, 0);  // no array buffer bound
  vertex_attrib_pointer(0, 3, gl::FLOAT, false, 32, (void*)64);
  EXPECT_EQ(bound_vao()->attribs[0].buffer, 0u);  // slot stays invalid
  EXPECT_EQ(cpu_pointer_attrib_count(), before + 1);
}

TEST(DkglState, PrimitiveRestartCapUnknownCapsPendingClear) {
  set_emitter(nullptr);
  enable(gl::PRIMITIVE_RESTART);
  EXPECT_TRUE(shadow().primitive_restart);
  EXPECT_TRUE(is_enabled(gl::PRIMITIVE_RESTART));
  disable(gl::PRIMITIVE_RESTART);
  EXPECT_FALSE(is_enabled(gl::PRIMITIVE_RESTART));
  // SCISSOR_TEST (and any other cap): counted, answered false, never crashes
  const u64 before = unknown_cap_count();
  enable(gl::SCISSOR_TEST);
  disable(gl::SCISSOR_TEST);
  EXPECT_FALSE(is_enabled(gl::SCISSOR_TEST));
  EXPECT_EQ(unknown_cap_count(), before + 3);
  // glClear bits are recorded and die with the frame if never flushed
  clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
  clear(gl::COLOR_BUFFER_BIT);
  EXPECT_EQ(pending_clear_mask(), gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
  frame_begin();
  EXPECT_EQ(pending_clear_mask(), 0u);
}

// ---- T105: textures / FBO / programs -------------------------------------------

namespace {
// build+link a program whose shaders carry the given hint name (GL Shader wrappers
// set the hint right before creating each stage's shader)
u32 build_hinted_program(const char* name) {
  hint_program_name(name);
  const u32 vs = create_shader(gl::VERTEX_SHADER);
  const u32 fs = create_shader(gl::FRAGMENT_SHADER);
  const u32 p = create_program();
  attach_shader(p, vs);
  attach_shader(p, fs);
  link_program(p);
  return p;
}
}  // namespace

TEST(DkglState, ProgramHintIdentity) {
  set_emitter(nullptr);
  const u32 p = build_hinted_program("ocean_common");
  ASSERT_NE(p, 0u);
  ASSERT_NE(program_table(p), nullptr) << "ocean_common must be in the T103 table";
  EXPECT_STREQ(program_table(p)->program_name, "ocean_common");
  // inert: name not in the T103 table -> draws nothing, but nothing crashes
  const u32 p2 = build_hinted_program("solid_color");
  EXPECT_EQ(program_table(p2), nullptr);
  // GL status plumbing: the Shader ctor only reads COMPILE/LINK status
  int status = 0;
  get_shaderiv(1, gl::COMPILE_STATUS, &status);
  EXPECT_EQ(status, 1);
  get_programiv(p, gl::LINK_STATUS, &status);
  EXPECT_EQ(status, 1);
  char logbuf[8];
  logbuf[0] = 1;
  int loglen = -1;
  get_shader_info_log(1, sizeof(logbuf), &loglen, logbuf);
  EXPECT_EQ(loglen, 0);
  EXPECT_EQ(logbuf[0], '\0');
}

TEST(DkglState, UniformShadowCombinedLayout) {
  set_emitter(nullptr);
  const u32 p = build_hinted_program("ocean_common");
  ASSERT_NE(program_table(p), nullptr);
  use_program(p);  // glUniform* resolves against the BOUND program (GL semantics)
  const int bucket = get_uniform_location(p, "bucket");
  const int color_mult = get_uniform_location(p, "color_mult");
  ASSERT_GE(bucket, 0);
  ASSERT_GE(color_mult, 0);
  const u8* s = program_uniform_shadow(p);
  ASSERT_NE(s, nullptr);
  // 'bucket' is in BOTH stages (vert offset 0, frag offset 48): one call, both copies
  program_uniform_clear_dirty(p);
  uniform1i(bucket, 7);
  int vert_bucket = 0, frag_bucket = 0;
  memcpy(&vert_bucket, s + 0, 4);                       // vert bucket @ 0
  memcpy(&frag_bucket, s + 16 + 48, 4);                 // frag bucket @ vert.block_size+48
  EXPECT_EQ(vert_bucket, 7);
  EXPECT_EQ(frag_bucket, 7);
  EXPECT_TRUE(program_uniform_dirty(p));
  // frag-only uniform lands at vert.block_size + its offset
  program_uniform_clear_dirty(p);
  uniform1f(color_mult, 2.5f);
  float frag_color_mult = 0.f;
  memcpy(&frag_color_mult, s + 16, 4);  // frag color_mult @ vert.block_size + 0
  EXPECT_FLOAT_EQ(frag_color_mult, 2.5f);
  EXPECT_TRUE(program_uniform_dirty(p));
  // sampler pseudo-location: exposed so GL code runs, but a no-op
  const int texloc = get_uniform_location(p, "tex_T1");
  EXPECT_EQ(texloc, (int)(0x8000'0000u | 1u));
  program_uniform_clear_dirty(p);
  uniform1i(texloc, 3);
  EXPECT_FALSE(program_uniform_dirty(p));  // sampler writes never dirty the shadow
  EXPECT_EQ(vert_bucket, 7);               // and nothing changed
  // unknown uniform -> -1
  EXPECT_EQ(get_uniform_location(p, "nosuch_uniform"), -1);
}

TEST(DkglState, RenderTargetLazyMaterialize) {
  MockEmitter mock;
  set_emitter(&mock);
  u32 tex = 0;
  gen_textures(1, &tex);
  ASSERT_GE(tex, kTexIdBase + 1);
  bind_texture(tex);  // records on the current unit; no GL work
  // the mip chain is 8 glTexImage2D calls, 128..1 (FramebufferTexturePair)
  int dim = 128;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(tex_image_2d(gl::RGBA8, dim, dim, gl::RGBA, gl::UNSIGNED_INT_8_8_8_8_REV, nullptr));
    dim /= 2;
  }
  u32 fbo = 0;
  gen_framebuffers(1, &fbo);
  ASSERT_NE(fbo, 0u);
  // T106: a target switch is QUEUED here and emitted by the next draw, so nothing
  // reaches the emitter yet (see DrawFlushScriptedOceanOrder for the emission).
  bind_framebuffer(fbo);
  EXPECT_TRUE(pending_target_switch());
  EXPECT_TRUE(pending_target_offscreen());
  EXPECT_EQ(pending_target_dk_tex(), 0u);  // nothing attached yet
  EXPECT_TRUE(mock.offscreen_ids.empty());
  ASSERT_TRUE(framebuffer_texture_2d(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, tex, 0));
  EXPECT_TRUE(mock.offscreen_ids.empty());  // attaching does not emit either
  EXPECT_EQ(check_framebuffer_status(), gl::FRAMEBUFFER_COMPLETE);
  // re-binding the complete FBO materializes the target id lazily (at queue time)
  bind_framebuffer(fbo);
  const u32 dk_id = pending_target_dk_tex();
  EXPECT_EQ(dk_id, kTexIdBase + 0x1000 + (tex - kTexIdBase));  // host fake-id scheme
  // the pool seam resolves to the SAME id, twice, with no extra emitter traffic
  EXPECT_EQ(resolve_texture_id(tex), dk_id);
  EXPECT_EQ(resolve_texture_id(tex), dk_id);
  EXPECT_TRUE(mock.offscreen_ids.empty());
  // re-binding the same FBO is not a leave: no barrier yet
  EXPECT_FALSE(texture_needs_barrier(tex));
  bind_framebuffer(0);  // back to the dk frame target (queued)
  EXPECT_TRUE(pending_target_switch());
  EXPECT_FALSE(pending_target_offscreen());
  EXPECT_EQ(mock.frame_target_calls, 0u);  // still queued, not emitted
  EXPECT_TRUE(texture_needs_barrier(tex));
  texture_barrier_sent(tex);
  EXPECT_FALSE(texture_needs_barrier(tex));
  pending_target_consumed();  // test hygiene: don't leak the queue into the next test
  set_emitter(nullptr);
}

TEST(DkglState, ResolvePassthroughForNonDkglIds) {
  set_emitter(nullptr);
  EXPECT_EQ(resolve_texture_id(777u), 777u);  // a real dktexgpu id from the pool path
  EXPECT_EQ(resolve_texture_id(0u), 0u);
  EXPECT_EQ(resolve_texture_id(kTexIdBase - 1), kTexIdBase - 1);
  // dkgl-range id that was never generated: passthrough, no crash
  const u32 weird = kTexIdBase + kMaxTextures + 1;
  EXPECT_EQ(resolve_texture_id(weird), weird);
}

// ---- T106: draw flush emission ---------------------------------------------------

TEST(DkglState, DrawFlushScriptedOceanOrder) {
  MockEmitter mock;
  set_emitter(&mock);
  frame_begin();
  begin_bucket();

  // VAO + VBO setup (two buffer_data calls, both ring-renamed)
  u32 vao = 0;
  gen_vertex_arrays(1, &vao);
  bind_vertex_array(vao);
  u32 vbo = 0, ibo = 0;
  gen_buffers(1, &vbo);
  gen_buffers(1, &ibo);
  bind_buffer(gl::ARRAY_BUFFER, vbo);
  const float vdata[16] = {0.f};
  const u32 idata[6] = {0};
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(vdata), vdata, 0));
  bind_buffer(gl::ELEMENT_ARRAY_BUFFER, ibo);  // VAO state (T104b)
  ASSERT_TRUE(buffer_data(gl::ELEMENT_ARRAY_BUFFER, sizeof(idata), idata, 0));
  enable_vertex_attrib_array(0);
  vertex_attrib_pointer(0, 4, gl::FLOAT, false, 16, (void*)0);
  enable_vertex_attrib_array(1);
  vertex_attrib_i_pointer(1, 1, gl::UNSIGNED_INT, 16, (void*)0);

  // program + per-pass state
  const u32 p = build_hinted_program("ocean_common");
  use_program(p);
  blend_func_separate(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA, gl::ONE,
                      gl::ONE_MINUS_SRC_ALPHA);
  enable(gl::BLEND);
  enable(gl::DEPTH_TEST);
  depth_mask(true);
  viewport(0, 0, 128, 128);
  enable(gl::PRIMITIVE_RESTART);
  primitive_restart_index(0xFFFF'FFFFu);

  // the ocean RT (128x128, 8 mips)
  u32 tex = 0;
  gen_textures(1, &tex);
  bind_texture(tex);
  ASSERT_TRUE(tex_image_2d(gl::RGBA8, 128, 128, gl::RGBA, gl::UNSIGNED_INT_8_8_8_8_REV, nullptr));
  u32 fbo = 0;
  gen_framebuffers(1, &fbo);
  bind_framebuffer(fbo);
  ASSERT_TRUE(framebuffer_texture_2d(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, tex, 0));
  bind_framebuffer(fbo);
  bind_texture(0);  // the RT is not sampled during its own render pass

  // ---- draw 1: offscreen pass (clear + arrays) ----------------------------------
  clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
  draw_arrays(gl::TRIANGLES, 0, 3);
  ASSERT_EQ(mock.offscreen_ids.size(), 1u);  // queued switch flushed at the draw
  // clear ordering: emitted BEFORE draw 1's bind_shaders
  const size_t clear_pos = mock.log.find("clear;");
  ASSERT_NE(clear_pos, std::string::npos);
  EXPECT_LT(clear_pos, mock.log.find("shaders("));

  // ---- back to the frame target; draw 2 + 3 sample the RT ------------------------
  bind_framebuffer(0);
  uniform4f(get_uniform_location(p, "fog_color"), 1.f, 2.f, 3.f, 4.f);
  bind_texture(tex);
  draw_elements(gl::TRIANGLE_STRIP, 3, gl::UNSIGNED_INT, (void*)8);  // draw 2
  EXPECT_EQ(mock.frame_target_calls, 1u);
  draw_elements(gl::TRIANGLE_STRIP, 3, gl::UNSIGNED_INT, (void*)8);  // draw 3 (same)

  // 1. distinct UBO slices on EVERY draw (3 draws x vert+frag pair)
  ASSERT_EQ(mock.ubo_binds.size(), 6u);
  EXPECT_EQ(mock.ubo_binds[0].stage, DkStage_Vertex);
  EXPECT_EQ(mock.ubo_binds[1].stage, DkStage_Fragment);
  EXPECT_NE(mock.ubo_binds[0].gpu, mock.ubo_binds[2].gpu);  // draw 1 vs draw 2
  EXPECT_NE(mock.ubo_binds[2].gpu, mock.ubo_binds[4].gpu);  // draw 2 vs draw 3
  // one slice; the frag block starts at the vert block size rounded up to the 0x100
  // UBO alignment (dkgl review: +16 would be an unaligned UBO address on the GPU)
  EXPECT_EQ(mock.ubo_binds[1].gpu - mock.ubo_binds[0].gpu, 0x100ull);

  // 2. diffing: unchanged state is not re-emitted (draw 2 switched targets, which
  // re-derives viewport + vtx format, but depth/blend/program stay put)
  EXPECT_EQ(mock.blend_states.size(), 1u);
  EXPECT_EQ(mock.depth_states.size(), 1u);
  EXPECT_EQ(mock.shader_binds.size(), 1u);
  EXPECT_EQ(mock.viewport_states.size(), 2u);  // offscreen + frame target
  EXPECT_EQ(mock.idx_binds, 1u);               // only the indexed draws
  EXPECT_EQ(mock.draws_a, 1u);
  EXPECT_EQ(mock.draws_e, 2u);

  // 3. barrier: exactly ONE before the first sampling bind, none for the re-bind
  EXPECT_EQ(mock.barrier_count, 1u);
  ASSERT_EQ(mock.tex_binds.size(), 2u);  // draw 2 and draw 3
  EXPECT_GT(mock.tex_binds[0][1], 0u);   // the materialized dk id
  const size_t barrier_pos = mock.log.find("barrier;");
  EXPECT_LT(barrier_pos, mock.log.find("tex;"));

  // 5. inert program: skipped + counted (no UBO slice, no draw)
  const u64 inert_before = inert_draw_count();
  use_program(build_hinted_program("solid_color"));
  draw_arrays(gl::TRIANGLES, 0, 3);
  EXPECT_EQ(inert_draw_count(), inert_before + 1);
  EXPECT_EQ(mock.ubo_binds.size(), 6u);
  EXPECT_EQ(mock.draws_a, 1u);

  // 6. separate alpha blend factors reach the DkBlendState verbatim
  ASSERT_EQ(mock.blend_states.size(), 1u);
  EXPECT_EQ(mock.blend_states[0].srcColorBlendFactor, DkBlendFactor_SrcAlpha);
  EXPECT_EQ(mock.blend_states[0].dstColorBlendFactor, DkBlendFactor_InvSrcAlpha);
  EXPECT_EQ(mock.blend_states[0].srcAlphaBlendFactor, DkBlendFactor_One);
  EXPECT_EQ(mock.blend_states[0].dstAlphaBlendFactor, DkBlendFactor_InvSrcAlpha);

  // 7. no lost clear in the scripted run (delta-based: older tests may leave one),
  // and begin_bucket re-emits everything
  const u64 lost_before = lost_clear_count();
  frame_begin();
  EXPECT_EQ(lost_clear_count(), lost_before);
  begin_bucket();
  use_program(p);
  draw_elements(gl::TRIANGLE_STRIP, 3, gl::UNSIGNED_INT, (void*)8);  // draw 4
  EXPECT_EQ(mock.blend_states.size(), 2u);
  EXPECT_EQ(mock.depth_states.size(), 2u);
  EXPECT_EQ(mock.shader_binds.size(), 2u);
  EXPECT_EQ(mock.ubo_binds.size(), 8u);
  set_emitter(nullptr);
}

// ---- dkgl review (AI-assisted): regression tests for the fixes found before hardware --------

TEST(DkglState, ReviewMultiBufferVao) {
  // OceanTexture_PC: positions from a static VBO + per-frame data from a dynamic VBO.
  MockEmitter mock;
  set_emitter(&mock);
  frame_begin();
  begin_bucket();
  u32 vao = 0, vb[2] = {0, 0};
  gen_vertex_arrays(1, &vao);
  bind_vertex_array(vao);
  gen_buffers(2, vb);
  const float d[8] = {0.f};
  bind_buffer(gl::ARRAY_BUFFER, vb[0]);
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(d), d, 0));
  enable_vertex_attrib_array(0);
  vertex_attrib_pointer(0, 2, gl::FLOAT, false, 8, (void*)0);
  bind_buffer(gl::ARRAY_BUFFER, vb[1]);
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(d), d, 0));
  enable_vertex_attrib_array(1);
  vertex_attrib_pointer(1, 4, gl::UNSIGNED_BYTE, true, 4, (void*)0);
  use_program(build_hinted_program("ocean_texture"));
  draw_arrays(gl::TRIANGLES, 0, 3);
  EXPECT_EQ(mock.draws_a, 1u);
  EXPECT_EQ(mock.last_vtx_bufs, 2u);
  set_emitter(nullptr);
}

TEST(DkglState, ReviewMipLevelTarget) {
  // FramebufferTexturePairContext(fb, level): bind the FBO, THEN attach that mip level.
  MockEmitter mock;
  set_emitter(&mock);
  frame_begin();
  begin_bucket();
  u32 tex = 0, fbo = 0;
  gen_textures(1, &tex);
  bind_texture(tex);
  for (int i = 0, dim = 128; i < 8; ++i, dim /= 2) {
    tex_image_2d(gl::RGBA, dim, dim, gl::RGBA, gl::UNSIGNED_BYTE, nullptr);
  }
  bind_texture(0);
  gen_framebuffers(1, &fbo);
  bind_framebuffer(fbo);
  ASSERT_TRUE(framebuffer_texture_2d(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, tex, 3));
  u32 vao = 0, vbo = 0;
  gen_vertex_arrays(1, &vao);
  bind_vertex_array(vao);
  gen_buffers(1, &vbo);
  bind_buffer(gl::ARRAY_BUFFER, vbo);
  const float d[16] = {0.f};
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(d), d, 0));
  enable_vertex_attrib_array(0);
  vertex_attrib_pointer(0, 4, gl::FLOAT, false, 16, (void*)0);
  use_program(build_hinted_program("ocean_texture_mipmap"));
  draw_arrays(gl::TRIANGLE_STRIP, 0, 4);
  ASSERT_EQ(mock.offscreen_levels.size(), 1u);
  EXPECT_EQ(mock.offscreen_levels[0], 3u);
  EXPECT_EQ(mock.offscreen_ids[0], materialized_dk_tex(tex));
  bind_framebuffer(0);
  set_emitter(nullptr);
}

TEST(DkglState, ReviewPoolTextureParams) {
  // CommonOceanRenderer sets REPEAT + LINEAR_MIPMAP_LINEAR on a pool texture it bound.
  set_emitter(nullptr);
  const u32 pool_id = 777;
  EXPECT_EQ(texture_sampler_combo(pool_id), 3u);  // pool defaults: linear, clamp, no mip
  active_texture(gl::TEXTURE0);
  bind_texture(pool_id);
  tex_parameteri(gl::TEXTURE_WRAP_S, gl::REPEAT);
  tex_parameteri(gl::TEXTURE_WRAP_T, gl::REPEAT);
  tex_parameteri(gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
  tex_parameteri(gl::TEXTURE_MAG_FILTER, gl::LINEAR);
  EXPECT_EQ(texture_sampler_combo(pool_id), 1u | 2u | 4u | 8u);
  bind_texture(0);
}

TEST(DkglState, ReviewPersistentBuffersOutsideFrame) {
  // init-time GL_STATIC_DRAW quads must not live in the per-frame ring
  MockEmitter mock;
  set_emitter(&mock);
  frame_end();
  u32 vbo = 0;
  gen_buffers(1, &vbo);
  bind_buffer(gl::ARRAY_BUFFER, vbo);
  const float d[4] = {0.f};
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(d), d, 0));
  EXPECT_EQ(mock.persistent_calls, 1u);
  frame_begin();
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(d), d, 0));
  EXPECT_EQ(mock.persistent_calls, 1u);  // inside a frame: ring renaming
  set_emitter(nullptr);
}

TEST(DkglState, ReviewGetIntegervAndDepthWrite) {
  MockEmitter mock;
  set_emitter(&mock);
  frame_begin();
  begin_bucket();
  viewport(1, 2, 300, 400);
  int vp[4] = {-1, -1, -1, -1};
  get_integerv(gl::VIEWPORT, vp);
  EXPECT_EQ(vp[0], 1);
  EXPECT_EQ(vp[3], 400);
  u32 fbo = 0;
  gen_framebuffers(1, &fbo);
  bind_framebuffer(fbo);
  int bound = -1;
  get_integerv(gl::FRAMEBUFFER_BINDING, &bound);
  EXPECT_EQ(bound, (int)fbo);
  bind_framebuffer(0);
  // GL writes no depth while the depth test is off, whatever glDepthMask says
  u32 vao = 0, vbo = 0;
  gen_vertex_arrays(1, &vao);
  bind_vertex_array(vao);
  gen_buffers(1, &vbo);
  bind_buffer(gl::ARRAY_BUFFER, vbo);
  const float d[16] = {0.f};
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(d), d, 0));
  enable_vertex_attrib_array(0);
  vertex_attrib_pointer(0, 4, gl::FLOAT, false, 16, (void*)0);
  use_program(build_hinted_program("ocean_envmap"));
  disable(gl::DEPTH_TEST);
  depth_mask(true);
  draw_arrays(gl::TRIANGLES, 0, 3);
  ASSERT_FALSE(mock.depth_states.empty());
  EXPECT_EQ(mock.depth_states.back().depthWriteEnable, 0u);
  set_emitter(nullptr);
}

TEST(DkglState, ReviewIntegerByteAttribAndPackedStride) {
  // CommonOceanRenderer: fog = glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE) -- every ocean
  // draw was skipped before (unknown format = black ocean on hardware).
  // OceanTexture_PC: glVertexAttribPointer(..., stride 0) = tightly packed.
  MockEmitter mock;
  set_emitter(&mock);
  frame_begin();
  begin_bucket();
  u32 vao = 0, vbo = 0;
  gen_vertex_arrays(1, &vao);
  bind_vertex_array(vao);
  gen_buffers(1, &vbo);
  bind_buffer(gl::ARRAY_BUFFER, vbo);
  const float d[32] = {0.f};
  ASSERT_TRUE(buffer_data(gl::ARRAY_BUFFER, sizeof(d), d, 0));
  enable_vertex_attrib_array(0);
  vertex_attrib_pointer(0, 2, gl::FLOAT, true, 0, (void*)0);
  EXPECT_EQ(bound_vao()->attribs[0].stride, 8u);
  enable_vertex_attrib_array(3);
  vertex_attrib_i_pointer(3, 4, gl::UNSIGNED_BYTE, 8, (void*)4);
  EXPECT_TRUE(bound_vao()->attribs[3].integer);
  const u64 skipped = skipped_draw_count();
  use_program(build_hinted_program("ocean_common"));
  draw_arrays(gl::TRIANGLES, 0, 3);
  EXPECT_EQ(skipped_draw_count(), skipped);
  EXPECT_EQ(mock.draws_a, 1u);
  set_emitter(nullptr);
}

TEST(DkglState, ReviewPrimitiveRestartEnums) {
  // GL_PRIMITIVE_RESTART is 0x8F9D; this glad redefines it as GL_PRIMITIVE_RESTART_FIXED_INDEX
  // (0x8D69) on Switch. The first draft used 0x8558: the ocean's restart was never enabled and
  // its strips joined into screen-wide triangles (70 ms of GPU per frame on hardware).
  set_emitter(nullptr);
  disable(gl::PRIMITIVE_RESTART);
  enable(0x8D69u);
  EXPECT_TRUE(is_enabled(gl::PRIMITIVE_RESTART));
  disable(0x8F9Du);
  EXPECT_FALSE(is_enabled(0x8D69u));
}

TEST(DkglState, ReviewEnoughFramebuffersForTheOcean) {
  // FramebufferTexturePair(128, 128, fmt, 8) alone asks for 8 FBOs; the ocean needs 11+.
  set_emitter(nullptr);
  u32 ids[16] = {};
  gen_framebuffers(16, ids);
  for (u32 id : ids) {
    EXPECT_NE(id, 0u);  // 0 would silently mean "the frame target"
  }
  delete_framebuffers(16, ids);
}
