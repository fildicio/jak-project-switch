#pragma once
// deko3d P3.1, slices 1-2 (T27-T28, AI-assisted): dkrt — a single RGBA8 color
// render target (create/bind/destroy), now with an optional Z24S8 depth
// attachment (T28). This is the deko3d equivalent of the GL FBO colour attachment
// (Fbo.h:12-16, which also carries an optional zbuf_stencil); later slices add
// MSAA resolve and CPU readback.
//
// This header stays PLAIN (no <deko3d.h>, no __SWITCH__ guard, cstdint only) so
// it is host-inspectable like dk_texture_pool.h. The deko3d-dependent
// implementation lives in dk_render_target.cpp, wrapped in `#if defined(__SWITCH__)`.
//
// Inert: no caller outside dk_render_target.cpp invokes dkrt::* yet. The 2D-engine
// blits (P3.1 later slices) will be the first consumer.
//
// Not thread-safe: render-thread only (matches the rest of the deko3d state).
//
// ===== T29b (P3.1 slice 3b): GL Fbo owner inventory (from T29a's doc =====
// docs/deko3d/gl-fbo-inventory.md, 2026-10-18). Pasted here so the dkrt
// author sees exactly which GL framebuffers a P3 renderer must re-create as
// FullTargets. 13 distinct FBO owners across 7 files (excluding the make_fbo
// factory and the gl_time_split_funcs.h timing shim, which is NOT an owner).
//
//   FboState / m_fsr_mid (OpenGLRenderer.h):
//     1  resources.window         (default fb, fbo_id=0)    — letterbox dst
//     2  resources.render_buffer  game_res_w x game_res_h,   — MSAA scene
//                                                   RGBA8 + ZS, msaa?
//     3  resources.resolve_buffer  game_res_w x game_res_h,  — MSAA resolve
//                                                   RGBA8, no depth
//     4  m_fsr_mid                fsr_w x 720, RGBA8, docked  — FSR intermediate
//   opengl_utils.cpp (effect FBOs):
//     5  FramebufferTexturePair  m_framebuffers[]  — mipmap chain, w>>i x h>>i
//     6  FramebufferCopier       m_fbo  m_fbo_w x m_fbo_h  GL_RGB NEAREST
//   SkyBlendGPU (effect FBOs):
//     7  m_framebuffers[0] sky   (32x32, square, RGBA8)
//     8  m_framebuffers[1] clouds (64x64, square, RGBA8)
//   DepthCue (effect FBOs):
//     9  m_ogl.framebuffer_sample_fbo  1x1 -> scene res, GL_RGB LINEAR
//    10  m_ogl.fbo  depth-cue base page 1x1 -> scene res, GL_RGB LINEAR
//   Sprite3_Distort (effect FBO):
//    11  m_distort_ogl.fbo  fbo_w x fbo_h, GL_RGB NEAREST CLAMP — distort src
//   GlowRenderer (effect FBOs):
//    12  m_ogl.probe_fbo  render_fb_w x render_fb_h, RGBA8 + ZS — glow probe
//    13a  m_ogl.downsample_fbos[0]  640x640  (largest level)
//    13b  m_ogl.downsample_fbos[1]  320x320
//    13c  m_ogl.downsample_fbos[2]  160x160
//    13d  m_ogl.downsample_fbos[3]  80x80
//    13e  m_ogl.downsample_fbos[4]  40x40  (smallest level, final glow)
//
// MSAA only on resources.render_buffer; resolve_buffer is its non-MSAA target.
// The "9 in the plan" vs "13 owners" vs "6 glBlitFramebuffer / 7 plan blits":
//   9 = number of glGenFramebuffers call sites (excl. the timing shim); 13 =
//   distinct owners (make_fbo called 3x at runtime + the 5-loop Downsample chain);
//   6 = actual glBlitFramebuffer calls, the 7th "blit" is a glDrawArrays copy.

#include <cstdint>

namespace dkrt {

// Dimensions of the render target, in pixels.
struct Desc {
  uint32_t width;
  uint32_t height;
  // T28 (slice 2): also create a Z24S8 depth attachment (in its own memblock,
  // bound as the depth target of dkCmdBufBindRenderTarget). Default false keeps
  // old callers byte-compatible.
  bool with_depth = false;
};

// RGBA8 (DkImageFormat_RGBA8_Unorm), single mip, no MSAA in this slice.
//   init(d):   create the image memblock + DkImage + DkImageView for a d.width x
//              d.height RGBA8 colour target. When d.with_depth is set, also
//              create a Z24S8 (DkImageFormat_Z24S8) depth image in a SECOND,
//              separate image memblock (the deko3d counterpart of the GL
//              zbuf_stencil renderbuffer). Returns false (and logs exactly one
//              "[dk] rt init failed (w x h, N MB)" line, or one
//              "[dk] rt depth init failed (w x h, N MB)" line for the depth
//              block) if a memblock does not allocate; on depth failure the colour
//              half is torn down too.
//   bind():    record dkCmdBufBindRenderTarget on the current cmd buf (colour
//              target = this view, depth = the Z24S8 view when with_depth, else
//              nullptr).
//   teardown(): destroy the memblock and re-zero all state. Safe to call when
//              init() never ran or failed (valid() == false): a no-op then.
//   valid():   true iff init() completed and has not been torn down.
bool init(const Desc& d);
void bind();
void teardown();
bool valid();
uint32_t width();
uint32_t height();

// ===== T29b (P3.1 slice 3b): FullTarget + FullTargetDesc (AI-assisted) =====
// A "full" render target mirrors the GL Fbo: it OWNS an RGBA8 colour image AND
// (optionally) a Z24S8 depth image, each in its own image memblock, plus the two
// DkImageViews that dkCmdBufBindRenderTarget consumes. It is the deko3d replacement
// for the 13 GL Fbo owners listed above (this slice is the colour + depth target
// only — one FullTarget instance = one such pair; dkrt may host several in its
// pool). MSAA and the resolve blit are T30.
//
// The class stores NO GPU types: it holds only a 1-based pool slot handle plus a
// cached copy of its dims. The real DkImage / DkImageView / DkMemBlock objects live
// in a fixed pool inside dk_render_target.cpp (behind __SWITCH__). That keeps this
// header PLAIN (host-compileable, no <deko3d.h>) like the rest of the file.

// Parameters for one FullTarget (a colour + optional depth target). Mirrors the GL
// Fbo's {width, height} + the T28 with_depth flag; no msaa term (MSAA is T30).
struct FullTargetDesc {
  uint32_t width;
  uint32_t height;
  bool with_depth = false;
};

// A colour + (optional) depth render target the caller owns.
//
//   init(d):   allocate one pool slot for this FullTarget and create its OWN
//              RGBA8 colour image in its own image memblock (plus, when
//              d.with_depth, a second Z24S8 depth image in its own memblock) and
//              its two DkImageViews. Caches the dims for matches()/width()/
//              height(). Returns false (and logs exactly one "[dk] fulltarget
//              init failed (w x h, N MB)" line, or a "[dk] fulltarget depth init
//              failed (w x h, N MB)" line for the depth block) if a memblock does
//              not allocate; on depth failure the colour half is torn down too so
//              nothing is left half-live.
//   matches(w,h): true iff valid() && width() == w && height() == h. Mirrors the
//              GL Fbo::matches (Fbo.h:23-30) WITHOUT the msaa term — MSAA is T30,
//              so a plain (non-MSAA) target matches on size only.
//   bind():     record dkCmdBufBindRenderTarget on the current cmd buf (colour
//              target = this colour view, depth = the Z24S8 view when with_depth,
//              else nullptr), via the same bind_views() helper the free dkrt::bind()
//              uses — so the T28 depth view is never duplicated.
//   teardown(): destroy this FullTarget's memblock(s) and free its pool slot.
//              Safe to call when init() never ran or failed (valid() == false):
//              a no-op then.
//   valid()/width()/height(): state / cached dims of this FullTarget.
class FullTarget {
 public:
  bool init(const FullTargetDesc& d);
  bool matches(uint32_t w, uint32_t h) const;
  void bind();
  void teardown();
  bool valid() const;
  uint32_t width() const;
  uint32_t height() const;

 private:
  // 1-based pool slot owned by this FullTarget; 0 = none (not yet created / torn
  // down). The cached dims mirror what init() created so matches()/width()/
  // height() are plain reads and never reach into the .cpp pool.
  int m_slot = 0;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
};

}  // namespace dkrt
