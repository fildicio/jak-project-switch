// T105 (AI-assisted): dkgl textures + FBOs.
// glGenTextures objects are dktexgpu render targets created LAZILY -- the pseudo GL id
// only becomes a real dktexgpu id at framebuffer_texture_2d / resolve_texture_id time
// (the pool seam, TexturePool.cpp, and T106 sampling both go through those). FBO 0 is
// the dk frame target; a bound offscreen FBO is an emitter target switch, no GL work.
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"

#if defined(__SWITCH__)
// same seam pair TexturePool.cpp includes for the T23 check (plain headers)
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"  // dktexgpu::create_render_target_mipped
#include "game/graphics/pipelines/dk_device.h"             // dkdev::is_up
#endif

namespace dkgl {
namespace {

struct TexObj {
  bool live = false;
  int w = 0, h = 0;
  u32 levels = 0;
  u32 min_filter = gl::LINEAR, mag_filter = gl::LINEAR;  // TexturePool.cpp upload defaults
  u32 wrap_s = gl::CLAMP_TO_EDGE, wrap_t = gl::CLAMP_TO_EDGE;
  u32 dk_tex_id = 0;  // dktexgpu id once materialized (0 = not yet)
  bool was_rendered_to = false;  // an offscreen FBO with this texture was unbound
  bool barrier_done = false;     // T106 sent the render-to-texture barrier
  bool rgb = false;  // T112: GL_RGB -- sampled with alpha forced to 1, like GL
};
struct FboObj {
  bool live = false;
  u32 attached_tex = 0;  // GL texture id attached via framebuffer_texture_2d
  u32 level = 0;         // dkgl review: mip level attached (the ocean renders its mips one by one)
};
// dkgl review: sampler parameters set on POOL textures (raw dktexgpu ids). The ocean binds
// its pool textures and sets REPEAT + LINEAR_MIPMAP_LINEAR on them (CommonOceanRenderer);
// without this they would be sampled clamped, without mips.
struct PoolTexParams {
  u32 min_filter = gl::LINEAR, mag_filter = gl::LINEAR;
  u32 wrap_s = gl::CLAMP_TO_EDGE, wrap_t = gl::CLAMP_TO_EDGE;
};
std::unordered_map<u32, PoolTexParams> g_pool_params;

TexObj g_textures[kMaxTextures];
FboObj g_fbos[kMaxFbos];
u32 g_next_tex = 0;  // slot round-robin cursor, like g_next_vao
u32 g_next_fbo = 0;
u32 g_bound_fbo = 0;  // 0 = the dk frame target
u32 g_read_fbo = 0;   // T112: glBlitFramebuffer source (GL_READ_FRAMEBUFFER)
struct PendingTarget {  // T106 flush step 1
  bool active = false;
  bool offscreen = false;
  u32 dk_tex_id = 0;
};
PendingTarget g_pending_target;

u64 g_tex_data_count = 0;       // tex_image_2d with pixels (ocean never does)
u64 g_bad_tex_param_count = 0;  // tex_parameteri on a pname we don't store
u64 g_bad_tex_format_count = 0;
u64 g_rt_no_dims_count = 0;  // materialize before any tex_image_2d
u64 g_bad_draw_buffers_count = 0;
bool g_logged_tex_data = false;
u64 g_unsupported_blit_count = 0;  // T112
u64 g_respecify_count = 0;         // T112: level-0 tex_image_2d with new dims/format

TexObj* tex_by_gl(u32 gl_id) {
  if (gl_id < kTexIdBase + 1 || gl_id > kTexIdBase + kMaxTextures) {
    return nullptr;
  }
  TexObj* t = &g_textures[gl_id - kTexIdBase - 1];
  return t->live ? t : nullptr;
}

FboObj* fbo_by_id(u32 id) {
  if (id == 0 || id > kMaxFbos) {
    return nullptr;
  }
  FboObj* f = &g_fbos[id - 1];
  return f->live ? f : nullptr;
}

// Create the dktexgpu render target on first need. Returns 0 ONLY if the Switch-side
// create fails; on host (no dk device) materializes to a deterministic fake id so
// tests can assert "second resolve -> same id, one creation".
u32 materialize(TexObj& t, u32 gl_id) {
  if (t.dk_tex_id) {
    return t.dk_tex_id;
  }
  if (t.levels == 0 || t.w == 0 || t.h == 0) {
    ++g_rt_no_dims_count;  // no tex_image_2d seen: 1x1x1 placeholder target
  }
  const u32 w = t.w > 0 ? (u32)t.w : 1;
  const u32 h = t.h > 0 ? (u32)t.h : 1;
  const u32 levels = t.levels > 0 ? t.levels : 1;
#if defined(__SWITCH__)
  // T23 activity-check pattern (TexturePool.cpp): only touch dktexgpu when it is real.
  if (dkdev::is_up() && dktexgpu::active()) {
    t.dk_tex_id = dktexgpu::create_render_target_mipped(w, h, levels, t.rgb);
    return t.dk_tex_id;  // 0 = create failed (caller reports false)
  }
#endif
  t.dk_tex_id = kTexIdBase + 0x1000 + (gl_id - kTexIdBase);  // host fake
  return t.dk_tex_id;
}

}  // namespace

// ---- textures -----------------------------------------------------------------
void gen_textures(int n, u32* out) {
  for (int i = 0; i < n; ++i) {
    out[i] = 0;
    for (u32 attempt = 0; attempt < kMaxTextures && out[i] == 0; ++attempt) {
      const u32 slot = g_next_tex;
      g_next_tex = (g_next_tex + 1) % kMaxTextures;
      if (!g_textures[slot].live) {
        g_textures[slot] = TexObj{};
        g_textures[slot].live = true;
        out[i] = kTexIdBase + slot + 1;
      }
    }
    if (out[i] == 0) {
      note_table_full("textures");
    }
  }
}
void delete_textures(int n, const u32* in) {
  for (int i = 0; i < n; ++i) {
    TexObj* t = tex_by_gl(in[i]);
    if (t) {
      *t = TexObj{};  // drops dk_tex_id: the image memory is NOT freed (P8, by design)
    }
    forget_texture_binding(in[i]);
  }
}
void bind_texture(u32 name) {
  // >= kTexIdBase: one of OUR ids; any other nonzero name is a real dktexgpu id that
  // came through the pool path -- record it as-is. 0 unbinds. No GL work.
  set_bound_texture(name);
}
void tex_parameteri(u32 pname, u32 param) {
  const u32 bound = bound_texture(active_unit());
  TexObj* t = tex_by_gl(bound);
  if (!t) {
    if (bound != 0 && bound < kTexIdBase) {
      PoolTexParams& p = g_pool_params[bound];
      if (pname == gl::TEXTURE_MIN_FILTER) {
        p.min_filter = param;
      } else if (pname == gl::TEXTURE_MAG_FILTER) {
        p.mag_filter = param;
      } else if (pname == gl::TEXTURE_WRAP_S) {
        p.wrap_s = param;
      } else if (pname == gl::TEXTURE_WRAP_T) {
        p.wrap_t = param;
      } else {
        ++g_bad_tex_param_count;
      }
    }
    return;
  }
  if (pname == gl::TEXTURE_MIN_FILTER) {
    t->min_filter = param;
  } else if (pname == gl::TEXTURE_MAG_FILTER) {
    t->mag_filter = param;
  } else if (pname == gl::TEXTURE_WRAP_S) {
    t->wrap_s = param;
  } else if (pname == gl::TEXTURE_WRAP_T) {
    t->wrap_t = param;
  } else {
    ++g_bad_tex_param_count;  // stored nowhere, but the call is not an error
  }
}
bool tex_image_2d(int level, u32 internal_format, int w, int h, u32 format, u32 type,
                  const void* data) {
  (void)format;
  TexObj* t = tex_by_gl(bound_texture(active_unit()));
  if (!t || level < 0 || level >= 16) {
    return false;
  }
  const bool rgb = internal_format == gl::RGB;
  if (internal_format != gl::RGBA && internal_format != gl::RGBA8 && !rgb) {
    ++g_bad_tex_format_count;
  }
  if (type != gl::UNSIGNED_INT_8_8_8_8_REV && type != gl::UNSIGNED_BYTE) {
    ++g_bad_tex_format_count;
  }
  if (level == 0) {
    // level 0 fixes the dimensions (mip chain: 128,64,...,1). T112: a later level-0 call
    // with other dims/format RESPECIFIES the texture -- the old dk image is dropped (not
    // freed, P8) and a new one is materialized lazily; FBOs resolve their attachment lazily
    // too (pending_target_dk_tex), so they follow automatically.
    const bool changed = t->levels == 0 || t->w != w || t->h != h || t->rgb != rgb;
    if (changed) {
      if (t->dk_tex_id != 0) {
        ++g_respecify_count;
      }
      t->dk_tex_id = 0;
      t->was_rendered_to = false;
      t->barrier_done = false;
      t->w = w;
      t->h = h;
      t->rgb = rgb;
      t->levels = 1;
    }
  } else if ((u32)level + 1 > t->levels) {
    t->levels = (u32)level + 1;
  }
  if (data) {
    // pixel uploads are NOT part of the ocean surface (the census stub would have
    // logged it); keep recording the mip bookkeeping and ignore the pixels.
    ++g_tex_data_count;
    if (!g_logged_tex_data) {
      std::fprintf(stderr, "[dkgl] tex_image_2d with data unsupported (ocean never does this)\n");
      g_logged_tex_data = true;
    }
  }
  return true;
}

// ---- FBOs ----------------------------------------------------------------------
void gen_framebuffers(int n, u32* out) {
  for (int i = 0; i < n; ++i) {
    out[i] = 0;
    for (u32 attempt = 0; attempt < kMaxFbos && out[i] == 0; ++attempt) {
      const u32 slot = g_next_fbo;
      g_next_fbo = (g_next_fbo + 1) % kMaxFbos;
      if (!g_fbos[slot].live) {
        g_fbos[slot] = FboObj{};
        g_fbos[slot].live = true;
        out[i] = slot + 1;  // ids 1..kMaxFbos, 0 reserved for the frame target
      }
    }
    if (out[i] == 0) {
      note_table_full("framebuffers");
    }
  }
}
void delete_framebuffers(int n, const u32* in) {
  for (int i = 0; i < n; ++i) {
    FboObj* f = fbo_by_id(in[i]);
    if (f) {
      *f = FboObj{};
    }
    if (g_bound_fbo == in[i]) {
      g_bound_fbo = 0;  // deleting the bound FBO falls back to the frame target
    }
  }
}
void bind_framebuffer_target(u32 target, u32 name) {
  if (target == gl::READ_FRAMEBUFFER) {
    g_read_fbo = fbo_by_id(name) ? name : 0;  // blit source only: no target switch
    return;
  }
  if (target != gl::DRAW_FRAMEBUFFER) {
    g_read_fbo = fbo_by_id(name) ? name : 0;  // GL_FRAMEBUFFER binds both
  }
  bind_framebuffer(name);
}
bool blit_framebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                      u32 mask, u32 filter) {
  // DepthCue is the only user: frame (read FBO 0) -> its sample texture, colour, no flip.
  FboObj* f = fbo_by_id(g_bound_fbo);
  TexObj* t = f ? tex_by_gl(f->attached_tex) : nullptr;
  if (g_read_fbo != 0 || !t || mask != gl::COLOR_BUFFER_BIT || sx1 <= sx0 || sy1 <= sy0 ||
      dx1 <= dx0 || dy1 <= dy0 || sx0 < 0 || sy0 < 0 || dx0 < 0 || dy0 < 0) {
    ++g_unsupported_blit_count;
    return false;
  }
  const u32 dk = materialize(*t, f->attached_tex);
  if (dk == 0) {
    ++g_unsupported_blit_count;
    return false;
  }
  const DkImageRect src{(u32)sx0, (u32)sy0, 0, (u32)(sx1 - sx0), (u32)(sy1 - sy0), 1};
  const DkImageRect dst{(u32)dx0, (u32)dy0, 0, (u32)(dx1 - dx0), (u32)(dy1 - dy0), 1};
  t->was_rendered_to = true;  // the first draw sampling it gets the T106 barrier
  t->barrier_done = false;
  if (Emitter* e = emitter()) {
    e->blit_frame_to_tex(dk, src, dst, filter == gl::LINEAR);
  }
  return true;
}
u64 unsupported_blit_count() {
  return g_unsupported_blit_count;
}
void bind_framebuffer(u32 name) {
  // Leaving an offscreen FBO means its texture was rendered to: T106 flushes the
  // render-to-texture barrier before the first draw that samples it. Re-binding the
  // SAME fbo (one render pass, many state groups) is not a leave.
  FboObj* prev = fbo_by_id(g_bound_fbo);
  if (prev && g_bound_fbo != name) {
    if (TexObj* t = tex_by_gl(prev->attached_tex)) {
      t->was_rendered_to = true;
      t->barrier_done = false;  // written again: the next sample needs a new barrier
    }
  }
  g_bound_fbo = fbo_by_id(name) ? name : 0;
  // T106: the switch is QUEUED, not emitted -- it flushes at the next draw, before
  // everything else (the ocean can bind an FBO and set up state before drawing). The
  // attachment is resolved when the switch is consumed (pending_target_dk_tex/level):
  // FramebufferTexturePairContext binds the FBO first and attaches the level after.
  g_pending_target.active = true;
  g_pending_target.offscreen = (g_bound_fbo != 0);
  g_pending_target.dk_tex_id = 0;
}
bool framebuffer_texture_2d(u32 target, u32 attachment, u32 tex_name, int level) {
  (void)target;      // GL_FRAMEBUFFER only, in the ocean
  (void)attachment;  // COLOR_ATTACHMENT0 only, in the ocean
  (void)level;       // mips are created by create_render_target_mipped, not per-level
  FboObj* f = fbo_by_id(g_bound_fbo);
  if (!f) {
    return false;
  }
  if (f->attached_tex != tex_name || f->level != (u32)level) {
    f->attached_tex = tex_name;
    f->level = level > 0 ? (u32)level : 0;
    g_pending_target.active = true;  // same FBO, new attachment: the target changed
    g_pending_target.offscreen = true;
  }
  TexObj* t = tex_by_gl(tex_name);
  if (!t) {
    return false;
  }
  if (t->dk_tex_id == 0) {
    return materialize(*t, tex_name) != 0;  // false only if the dk create failed
  }
  return true;
}
void draw_buffers(int n, const u32* bufs) {
  if (n != 1 || !bufs || bufs[0] != gl::COLOR_ATTACHMENT0) {
    ++g_bad_draw_buffers_count;
  }
}
u32 check_framebuffer_status() {
  return gl::FRAMEBUFFER_COMPLETE;
}

// ---- pool seam + T106 accessors -------------------------------------------------
u32 resolve_texture_id(u32 gl_id) {
  if (gl_id < kTexIdBase) {
    return gl_id;  // not a dkgl pseudo id (real dktexgpu id from the pool path)
  }
  TexObj* t = tex_by_gl(gl_id);
  if (!t) {
    return gl_id;
  }
  materialize(*t, gl_id);
  return t->dk_tex_id != 0 ? t->dk_tex_id : gl_id;
}
u32 materialized_dk_tex(u32 gl_id) {
  TexObj* t = tex_by_gl(gl_id);
  return t ? t->dk_tex_id : 0;
}
bool texture_needs_barrier(u32 gl_id) {
  TexObj* t = tex_by_gl(gl_id);
  return t && t->was_rendered_to && !t->barrier_done;
}
void texture_barrier_sent(u32 gl_id) {
  if (TexObj* t = tex_by_gl(gl_id)) {
    t->barrier_done = true;
  }
}

// ---- T106 seams: queued target switch + sampler combo ---------------------------
bool pending_target_switch() {
  return g_pending_target.active;
}
bool pending_target_offscreen() {
  return g_pending_target.offscreen;
}
u32 pending_target_dk_tex() {
  FboObj* f = fbo_by_id(g_bound_fbo);
  if (!g_pending_target.offscreen || !f) {
    return 0;
  }
  TexObj* t = tex_by_gl(f->attached_tex);
  return t ? materialize(*t, f->attached_tex) : 0;  // lazily, at flush time
}
u32 pending_target_level() {
  FboObj* f = fbo_by_id(g_bound_fbo);
  return (g_pending_target.offscreen && f) ? f->level : 0;
}
u32 bound_framebuffer() {
  return g_bound_fbo;
}
void pending_target_consumed() {
  g_pending_target.active = false;
}
u32 texture_sampler_combo(u32 gl_id) {
  TexObj* t = tex_by_gl(gl_id);
  PoolTexParams pool;
  u32 min_filter, mag_filter, wrap_s;
  if (t) {
    min_filter = t->min_filter;
    mag_filter = t->mag_filter;
    wrap_s = t->wrap_s;
  } else {
    // pool textures: what glTexParameteri set on them, else the TexturePool upload
    // defaults (LINEAR/LINEAR + CLAMP)
    auto it = g_pool_params.find(gl_id);
    if (it != g_pool_params.end()) {
      pool = it->second;
    }
    min_filter = pool.min_filter;
    mag_filter = pool.mag_filter;
    wrap_s = pool.wrap_s;
  }
  u32 combo = 0;
  switch (min_filter) {
    case gl::LINEAR:
    case gl::LINEAR_MIPMAP_NEAREST:
    case gl::LINEAR_MIPMAP_LINEAR:
      combo |= 1;  // min_linear
      break;
    default:
      break;
  }
  if (mag_filter == gl::LINEAR) {
    combo |= 2;
  }
  // Horizon A/B (AI-assisted): gk_dkgl_nomip.txt samples level 0 only (DkMipFilter_None),
  // to test whether the ocean's small mips are black on hardware.
  static const bool s_no_mip = std::fopen("sdmc:/switch/jak1/gk_dkgl_nomip.txt", "r") != nullptr;
  if (!s_no_mip &&
      (min_filter == gl::LINEAR_MIPMAP_LINEAR || min_filter == gl::NEAREST_MIPMAP_LINEAR)) {
    combo |= 4;  // mip_linear
  }
  if (wrap_s == gl::REPEAT) {
    combo |= 8;
  }
  return combo;
}

}  // namespace dkgl
