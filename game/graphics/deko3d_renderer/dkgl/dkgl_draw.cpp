// T106 (AI-assisted): dkgl draw emission.
// draw_arrays/draw_elements (dkgl_state.cpp) funnel into flush_pending, which turns
// the lazy shadow state into a minimal DIFFED command stream through the Emitter:
// only what changed since the last draw is re-emitted (begin_bucket resets the cache),
// but UNIFORMS are snapshotted into a FRESH UBO slice on EVERY draw (brief lesson:
// the GL side may rewrite uniforms between draws that share all other state).
//
// Blend enable: DkBlendState has no enable bit (deko3d 0.5.0) -- a disabled GL blend
// is encoded as factors ONE/ZERO with op ADD (result = src), the canonical mapping.
#include <cstdio>
#include <cstring>

#include "game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"

namespace dkgl {
namespace {

// ---- last-emitted cache (diffing) ----------------------------------------------
struct EmitCache {
  bool valid = false;  // false until the first draw / after begin_bucket
  u32 program = 0;
  DkDepthStencilState depth{};
  DkBlendState blend{};
  DkViewport vp{};
  const void* vao = nullptr;  // bound_vao() identity
  u64 vtx_gpu[kMaxAttribs] = {};
  u32 vtx_bytes[kMaxAttribs] = {};
  u32 n_vtx_bufs = 0;
  DkIdxFormat idx_fmt = DkIdxFormat_Uint32;
  u64 idx_gpu = 0;
  bool restart_on = false;
  u32 restart_index = 0xFFFF'FFFFu;
};
EmitCache g_cache;
u64 g_inert_draw_count = 0;
u64 g_bad_blend_factor_count = 0;
u64 g_bad_depth_func_count = 0;
u64 g_bad_prim_count = 0;
u64 g_bad_attrib_count = 0;
u64 g_vtx_stride_mismatch_count = 0;
u64 g_missing_element_buffer_count = 0;
bool g_logged_bad_attrib = false;

// ---- maps (spellings verbatim from deko3d.h 0.5.0; check-dk.py validates) ------
bool blend_factor_map(u32 gl, DkBlendFactor* out) {
  switch (gl) {
    case gl::ZERO:
      *out = DkBlendFactor_Zero;
      return true;
    case gl::ONE:
      *out = DkBlendFactor_One;
      return true;
    case gl::SRC_COLOR:
      *out = DkBlendFactor_SrcColor;
      return true;
    case gl::ONE_MINUS_SRC_COLOR:
      *out = DkBlendFactor_InvSrcColor;
      return true;
    case gl::SRC_ALPHA:
      *out = DkBlendFactor_SrcAlpha;
      return true;
    case gl::ONE_MINUS_SRC_ALPHA:
      *out = DkBlendFactor_InvSrcAlpha;
      return true;
    case gl::DST_ALPHA:
      *out = DkBlendFactor_DstAlpha;
      return true;
    case gl::ONE_MINUS_DST_ALPHA:
      *out = DkBlendFactor_InvDstAlpha;
      return true;
    case gl::DST_COLOR:
      *out = DkBlendFactor_DstColor;
      return true;
    case gl::ONE_MINUS_DST_COLOR:
      *out = DkBlendFactor_InvDstColor;
      return true;
    case gl::CONST_COLOR:
      *out = DkBlendFactor_ConstColor;
      return true;
    default:
      return false;
  }
}
DkBlendOp blend_op_map(u32 eq, bool* ok) {
  *ok = true;
  switch (eq) {
    case gl::FUNC_ADD:
      return DkBlendOp_Add;
    case gl::FUNC_SUBTRACT:
      return DkBlendOp_Sub;
    case gl::FUNC_REVERSE_SUBTRACT:
      return DkBlendOp_RevSub;
    default:
      *ok = false;  // counter + treat as Add
      return DkBlendOp_Add;
  }
}
DkCompareOp depth_func_map(u32 func, bool* ok) {
  *ok = true;
  switch (func) {
    case gl::NEVER:
      return DkCompareOp_Never;
    case gl::LESS:
      return DkCompareOp_Less;
    case gl::EQUAL:
      return DkCompareOp_Equal;
    case gl::LEQUAL:
      return DkCompareOp_Lequal;
    case gl::GREATER:
      return DkCompareOp_Greater;
    case gl::NOTEQUAL:
      return DkCompareOp_NotEqual;
    case gl::GEQUAL:
      return DkCompareOp_Gequal;
    case gl::ALWAYS:
      return DkCompareOp_Always;
    default:
      *ok = false;  // counter + Always (test disabled anyway)
      return DkCompareOp_Always;
  }
}
bool prim_map(u32 mode, DkPrimitive* out) {
  switch (mode) {
    case gl::TRIANGLES:
      *out = DkPrimitive_Triangles;
      return true;
    case gl::TRIANGLE_STRIP:
      *out = DkPrimitive_TriangleStrip;
      return true;
    case gl::LINES:
      *out = DkPrimitive_Lines;
      return true;
    case gl::TRIANGLE_FAN:
      *out = DkPrimitive_TriangleFan;
      return true;
    default:
      return false;
  }
}
bool idx_fmt_map(u32 type, DkIdxFormat* out) {
  switch (type) {
    case gl::UNSIGNED_INT:
      *out = DkIdxFormat_Uint32;
      return true;
    case gl::UNSIGNED_SHORT:
      *out = DkIdxFormat_Uint16;
      return true;
    default:
      return false;
  }
}
// GL (size,type,normalized) -> DkVtxAttribSize + DkVtxAttribType. Field order of
// DkVtxAttribState is {buffer, isFixed, offset, size, type, isBgra} (recipe #2).
// NOTE: the Nx32 sizes are NOT contiguous values (1x32=0x12, 2x32=0x04, 3x32=0x02,
// 4x32=0x01) -- explicit switch, never arithmetic.
bool attrib_map(int size, u32 type, bool normalized, bool integer, DkVtxAttribSize* sz,
                DkVtxAttribType* ty) {
  if (type == gl::FLOAT) {
    switch (size) {
      case 1:
        *sz = DkVtxAttribSize_1x32;
        break;
      case 2:
        *sz = DkVtxAttribSize_2x32;
        break;
      case 3:
        *sz = DkVtxAttribSize_3x32;
        break;
      case 4:
        *sz = DkVtxAttribSize_4x32;
        break;
      default:
        return false;
    }
    *ty = DkVtxAttribType_Float;
    return true;
  }
  // dkgl review: integer attribs (glVertexAttribIPointer) read as Uint, normalized ones as
  // Unorm, plain ones as Uscaled (GL converts them to float without normalizing).
  // CommonOceanRenderer's fog is an IPointer of 4 u8 (all its draws were skipped before).
  const DkVtxAttribType int_ty =
      integer ? DkVtxAttribType_Uint : (normalized ? DkVtxAttribType_Unorm : DkVtxAttribType_Uscaled);
  if (type == gl::UNSIGNED_INT) {
    switch (size) {
      case 1:
        *sz = DkVtxAttribSize_1x32;
        break;
      case 2:
        *sz = DkVtxAttribSize_2x32;
        break;
      case 3:
        *sz = DkVtxAttribSize_3x32;
        break;
      case 4:
        *sz = DkVtxAttribSize_4x32;
        break;
      default:
        return false;
    }
    *ty = integer ? DkVtxAttribType_Uint : DkVtxAttribType_Uscaled;
    return true;
  }
  if (type == gl::UNSIGNED_BYTE) {
    switch (size) {
      case 1:
        *sz = DkVtxAttribSize_1x8;
        break;
      case 2:
        *sz = DkVtxAttribSize_2x8;
        break;
      case 3:
        *sz = DkVtxAttribSize_3x8;
        break;
      case 4:
        *sz = DkVtxAttribSize_4x8;
        break;
      default:
        return false;
    }
    *ty = int_ty;
    return true;
  }
  if (type == gl::INT_2_10_10_10_REV) {
    *sz = DkVtxAttribSize_10_10_10_2;
    *ty = DkVtxAttribType_Snorm;  // brief pitfall: the packed format is SNORM in dk
    return true;
  }
  return false;
}

}  // namespace

void reset_emit_cache() {
  g_cache = EmitCache{};
}
u64 inert_draw_count() {
  return g_inert_draw_count;
}
u64 skipped_draw_count() {
  return g_bad_attrib_count + g_missing_element_buffer_count + g_bad_prim_count;
}

void flush_pending(u32 mode, int first, int count, u32 idx_type, u64 idx_off, bool indexed) {
  Emitter* e = emitter();
  if (!e) {
    return;  // shadow-only mode (host tests without a mock): nothing to emit
  }
  const ShadowState& sh = shadow();

  // 1. pending target switch (queued by bind_framebuffer): emits FIRST, and the new
  // target's size differs -> the viewport + vtx format must be re-derived.
  if (pending_target_switch()) {
    if (pending_target_offscreen()) {
      e->bind_offscreen_target(pending_target_dk_tex(), pending_target_level());
    } else {
      e->bind_frame_target();
    }
    pending_target_consumed();
    g_cache.vp.width = 0;  // force viewport re-emit (target size changed)
    g_cache.vao = nullptr;
    g_cache.n_vtx_bufs = 0;  // force vtx-format re-emit
  }

  // 2. pending clear, BEFORE any draw state (the ocean clears right after binding)
  if (pending_clear_mask()) {
    const u32 mask = pending_clear_mask();
    e->clear((mask & gl::COLOR_BUFFER_BIT) != 0, (mask & gl::DEPTH_BUFFER_BIT) != 0,
             sh.clear_color);
    consume_pending_clear();
  }

  // 3. program: inert (name not in the T103 table) -> skip the whole draw, counted
  const u32 prog = bound_program();
  const ProgramUniformTable* table = program_table(prog);
  if (!table) {
    ++g_inert_draw_count;
    return;
  }
  if (!g_cache.valid || g_cache.program != prog) {
    e->bind_shaders(prog, table->program_name);
    g_cache.program = prog;
  }

  // 4. depth (GL-style depth: device runs DepthMinusOneToOne, clears 0.0 -- P5)
  bool ok = true;
  const DkCompareOp cmp = depth_func_map(sh.depth_func, &ok);
  if (!ok) {
    ++g_bad_depth_func_count;
  }
  DkDepthStencilState depth{};
  depth.depthTestEnable = sh.depth_test ? 1u : 0u;
  // GL never writes depth while GL_DEPTH_TEST is off (dkgl review)
  depth.depthWriteEnable = (sh.depth_test && sh.depth_write) ? 1u : 0u;
  depth.stencilTestEnable = 0;
  depth.depthCompareOp = cmp;
  if (!g_cache.valid || std::memcmp(&g_cache.depth, &depth, sizeof(depth)) != 0) {
    e->set_depth(depth);
    g_cache.depth = depth;
  }

  // 5. blend (enable encoded as ONE/ZERO factors; needs T101's patched deko3d for the
  // separate alpha factor to actually reach the GPU)
  bool ok_s = true, ok_d = true, ok_sa = true, ok_da = true, ok_op = true, ok_opa = true;
  DkBlendFactor src, dst, srca, dsta;
  if (sh.blend) {
    ok_s = blend_factor_map(sh.blend_src_rgb, &src);
    ok_d = blend_factor_map(sh.blend_dst_rgb, &dst);
    ok_sa = blend_factor_map(sh.blend_src_a, &srca);
    ok_da = blend_factor_map(sh.blend_dst_a, &dsta);
    if (!ok_s || !ok_d || !ok_sa || !ok_da) {
      ++g_bad_blend_factor_count;
      src = dst = srca = dsta = DkBlendFactor_One;  // safe fallback: as-if disabled
    }
  } else {
    src = DkBlendFactor_One;   // disabled GL blend == src replaces dst
    dst = DkBlendFactor_Zero;
    srca = DkBlendFactor_One;
    dsta = DkBlendFactor_Zero;
  }
  const DkBlendOp op_rgb = blend_op_map(sh.blend_eq_rgb, &ok_op);
  const DkBlendOp op_a = blend_op_map(sh.blend_eq_a, &ok_opa);
  if (!ok_op || !ok_opa) {
    ++g_bad_blend_factor_count;
  }
  DkBlendState blend{};
  blend.colorBlendOp = op_rgb;
  blend.srcColorBlendFactor = src;
  blend.dstColorBlendFactor = dst;
  blend.alphaBlendOp = op_a;
  blend.srcAlphaBlendFactor = srca;
  blend.dstAlphaBlendFactor = dsta;
  if (!g_cache.valid || std::memcmp(&g_cache.blend, &blend, sizeof(blend)) != 0) {
    e->set_blend(blend);
    g_cache.blend = blend;
  }

  // 6. viewport
  const DkViewport vp{(float)sh.vp_x, (float)sh.vp_y, (float)sh.vp_w, (float)sh.vp_h, 0.f,
                      1.f};
  if (!g_cache.valid || std::memcmp(&g_cache.vp, &vp, sizeof(vp)) != 0) {
    e->set_viewport(vp);
    g_cache.vp = vp;
  }

  // 7. vtx format, from the CURRENT VAO; attribs captured their buffer at
  // glVertexAttribPointer time (T104b). dkgl review: one DkVtxBufferState per DISTINCT
  // buffer (OceanTexture_PC: static positions + dynamic data in one VAO); the attribs of
  // one buffer must share its stride.
  const VaoObj* vao = bound_vao();
  DkVtxAttribState attribs[kMaxAttribs];
  DkVtxBufferState vbufs[kMaxAttribs];
  u32 buf_names[kMaxAttribs];
  u64 buf_gpu[kMaxAttribs];
  u32 buf_bytes[kMaxAttribs];
  u32 n_bufs = 0;
  u32 n_attribs = 0;
  for (u32 i = 0; i < kMaxAttribs; ++i) {
    const auto& a = vao->attribs[i];
    if (!a.enabled) {
      continue;
    }
    DkVtxAttribSize sz;
    DkVtxAttribType ty;
    if (!attrib_map(a.size, a.type, a.normalized, a.integer, &sz, &ty)) {
      ++g_bad_attrib_count;
      if (!g_logged_bad_attrib) {
        g_logged_bad_attrib = true;
        std::fprintf(stderr, "[dkgl] draw skipped: attrib %u size %d type 0x%x norm %d int %d\n",
                     i, a.size, a.type, (int)a.normalized, (int)a.integer);
      }
      return;  // unknown attrib format: skip the draw (never guess a layout)
    }
    const BufferObj* bo = buffer(a.buffer);
    if (!bo || !bo->valid || bo->cpu == nullptr) {
      ++g_bad_attrib_count;
      return;  // attrib points at a dead/failed buffer: skip the draw
    }
    u32 b = 0;
    while (b < n_bufs && buf_names[b] != a.buffer) {
      ++b;
    }
    if (b == n_bufs) {
      buf_names[b] = a.buffer;  // first attrib of this buffer fixes its stride
      buf_gpu[b] = bo->gpu;
      buf_bytes[b] = bo->bytes;
      vbufs[b] = DkVtxBufferState{a.stride, 0};
      ++n_bufs;
    } else if (a.stride != vbufs[b].stride) {
      ++g_vtx_stride_mismatch_count;  // counter + FIRST stride (spec rule)
    }
    attribs[n_attribs++] = DkVtxAttribState{b, 0, (u32)a.offset, sz, ty, 0};
  }
  if (n_attribs == 0 || n_bufs == 0) {
    ++g_bad_attrib_count;
    return;  // nothing to draw without a vertex layout
  }
  bool vtx_same = g_cache.valid && g_cache.vao == (const void*)vao && g_cache.n_vtx_bufs == n_bufs;
  for (u32 b = 0; vtx_same && b < n_bufs; ++b) {
    vtx_same = g_cache.vtx_gpu[b] == buf_gpu[b] && g_cache.vtx_bytes[b] == buf_bytes[b];
  }
  if (!vtx_same) {
    e->set_vtx_format(attribs, n_attribs, vbufs, buf_gpu, buf_bytes, n_bufs);
    g_cache.vao = vao;
    g_cache.n_vtx_bufs = n_bufs;
    for (u32 b = 0; b < n_bufs; ++b) {
      g_cache.vtx_gpu[b] = buf_gpu[b];
      g_cache.vtx_bytes[b] = buf_bytes[b];
    }
  }

  // 8. indexed draws bind the CURRENT VAO's element buffer
  DkIdxFormat idx_fmt = DkIdxFormat_Uint32;
  if (indexed) {
    const BufferObj* ib = buffer(vao->element_buffer);
    if (!ib || !ib->valid || ib->cpu == nullptr) {
      ++g_missing_element_buffer_count;
      return;
    }
    if (!idx_fmt_map(idx_type, &idx_fmt)) {
      ++g_bad_attrib_count;
      return;
    }
    if (!g_cache.valid || g_cache.idx_gpu != ib->gpu || g_cache.idx_fmt != idx_fmt) {
      e->bind_idx_buffer(ib->gpu, idx_fmt);
      g_cache.idx_gpu = ib->gpu;
      g_cache.idx_fmt = idx_fmt;
    }
  }

  // 9. samplers: one per unit with a bound texture; an RT that was rendered to gets
  // its render-to-texture barrier exactly once (before the first sampling draw)
  for (u32 unit = 0; unit < kMaxTexUnits; ++unit) {
    const u32 gl_id = bound_texture(unit);
    if (gl_id == 0) {
      continue;
    }
    u32 dk_id;
    if (gl_id >= kTexIdBase) {
      dk_id = materialized_dk_tex(gl_id);
      if (dk_id == 0) {
        dk_id = resolve_texture_id(gl_id);  // materialize on demand
      }
    } else {
      dk_id = gl_id;  // pool path: the binding IS a raw dktexgpu id
    }
    if (texture_needs_barrier(gl_id)) {
      e->barrier_before_sample();
      texture_barrier_sent(gl_id);
    }
    e->bind_tex(unit, dk_id, texture_sampler_combo(gl_id));
  }

  // 10. uniforms -- snapshot EVERY draw into a FRESH UBO slice (never reuse within a
  // frame): one slice carries both stage blocks. dkgl review: a UBO address must be
  // 0x100-aligned (DK_UNIFORM_BUF_ALIGNMENT), so the frag block starts at the vert block
  // size rounded up to 0x100, not right after it (the shadow keeps the packed layout).
  const u32 vs_bytes = table->vert.block_size;
  const u32 fs_bytes = table->frag.block_size;
  const u32 fs_off = (vs_bytes + 0xFFu) & ~0xFFu;
  if (vs_bytes + fs_bytes > 0) {
    void* cpu = nullptr;
    u64 gpu = 0;
    if (e->ubo_alloc(fs_off + fs_bytes, &cpu, &gpu)) {
      const u8* shadow_bytes = program_uniform_shadow(prog);
      u8* dst = (u8*)cpu;
      if (shadow_bytes) {
        std::memcpy(dst, shadow_bytes, vs_bytes);
        std::memcpy(dst + fs_off, shadow_bytes + vs_bytes, fs_bytes);
      } else {
        std::memset(dst, 0, fs_off + fs_bytes);
      }
      if (vs_bytes) {
        e->bind_ubo(DkStage_Vertex, 7, gpu, vs_bytes);
      }
      if (fs_bytes) {
        e->bind_ubo(DkStage_Fragment, 7, gpu + fs_off, fs_bytes);
      }
    }
    program_uniform_clear_dirty(prog);
  }

  // 11. primitive restart
  if (!g_cache.valid || g_cache.restart_on != sh.primitive_restart ||
      g_cache.restart_index != sh.restart_index) {
    e->set_primitive_restart(sh.primitive_restart, sh.restart_index);
    g_cache.restart_on = sh.primitive_restart;
    g_cache.restart_index = sh.restart_index;
  }

  // 12. the draw
  DkPrimitive prim;
  if (!prim_map(mode, &prim)) {
    ++g_bad_prim_count;
    return;
  }
  if (indexed) {
    e->draw_elements(prim, (u32)count, (u32)idx_off);
  } else {
    e->draw_arrays(prim, (u32)first, (u32)count);
  }
  g_cache.valid = true;
}

}  // namespace dkgl
