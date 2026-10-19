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
  // T30 (slice 4): sample count. 1 = 1x (single sample, the default and the
  // baseline path), 2/4/8 = multisampled (a 2DMS colour image is created and a
  // 1x resolve sibling is available for resolve()). Any other value is rejected
  // by init() with a one-line [dk] log (see the .cpp). Default 1 keeps old
  // callers byte-compatible (1x is the DkMsMode_1x the defaults already set).
  uint32_t samples = 1;
};

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
// Fbo's {width, height} + the T28 with_depth flag + the T30 sample count.
struct FullTargetDesc {
  uint32_t width;
  uint32_t height;
  bool with_depth = false;
  // T30 (slice 4): sample count (1/2/4/8). 1 = single-sample colour image and no
  // resolve sibling (the baseline path); 2/4/8 creates a 2DMS colour image plus a
  // 1x resolve sibling in its own memblock. Any other value is rejected by
  // init() with a one-line [dk] log. Default 1 keeps old callers byte-compatible.
  uint32_t samples = 1;
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
//   matches(w,h,msaa): true iff valid() && width() == w && height() == h && the
//              cached sample count == msaa. Mirrors the GL Fbo::matches
//              (Fbo.h:23-30): it now also matches on the msaa term (T30a
//              slice 4a). A single-sample target (samples == 1) matches
//              msaa == 1; a 2DMS target (2/4/8 samples) matches that count.
//   bind():     record dkCmdBufBindRenderTarget on the current cmd buf (colour
//              target = this colour view, depth = the Z24S8 view when with_depth,
//              else nullptr), via the same bind_views() helper the free dkrt::bind()
//              uses — so the T28 depth view is never duplicated.
//   teardown(): destroy this FullTarget's memblock(s) and free its pool slot.
//              a no-op then.
//              T30b (P3.1 slice 4b): teardown() ALSO SWEEPS the entire g_fulltargetPool:
//              every OTHER still-live slot is freed (its colour + depth + resolve-sibling
//              memblocks, via the same fulltarget_free_slot()), so a single teardown call
//              settles all pooled targets and no other FullTarget leaks a memblock. The
//              number of other slots swept is logged as "[dk] rt teardown swept N pooled
//              target(s)". There is no lock: the pool is touched only from the one render
//              thread, matching every other accessor. This settles the ownership note left
//              open in T29b.
//   valid()/width()/height(): state / cached dims of this FullTarget.
class FullTarget {
 public:
  bool init(const FullTargetDesc& d);
  bool matches(uint32_t w, uint32_t h, uint32_t msaa) const;
  void bind();
  void teardown();
  bool valid() const;
  uint32_t width() const;
  uint32_t height() const;
  // T30b (P3.1 slice 4b): the 1x RESOLVE SIBLING + resolve(), only meaningful
  // when samples > 1. The MSAA colour image is rendered into; the sibling is a
  // plain 2D RGBA8 (DkMsMode_1x) image in its own memblock, created by init()
  // alongside the MSAA colour image. resolved_view() returns it as an OPAQUE
  // const void* (T31/T38 cast it to DkImageView*); the .h stays deko3d-free
  // (no <deko3d.h>), exactly like the other plain getters. Only valid for a live
  // 2DMS target (samples > 1); for a 1x target (no sibling) it returns nullptr.
  const void* resolved_view() const;
  // Only valid for a live 2DMS target (samples > 1) that init() created
  // successfully; for a 1x target or a non-live target it logs a one-line "[dk]"
  // and returns false (never crash). resolve() records dkCmdBufBarrier(full) then
  // dkCmdBufResolveImage from the MSAA colour image into the 1x resolve sibling,
  // mirroring the existing barrier+blit precedent in dk_device.cpp.
  bool resolve();

 private:
  // 1-based pool slot owned by this FullTarget; 0 = none (not yet created / torn
  // down). The cached dims mirror what init() created so matches()/width()/
  // height() are plain reads and never reach into the .cpp pool.
  int m_slot = 0;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  // T30a (slice 4a): cached sample count (1/2/4/8). A plain 1x target has
  // samples == 1; a 2DMS colour image has samples 2/4/8. matches() compares
  // this against its msaa argument, mirroring the GL Fbo::matches msaa term.
  uint32_t m_samples = 1;
};

// ===== T31a (P3.1 slice 5a): blit() core (AI-assisted) ======================
// Half 1 of T31: a single plain-signature wrapper that composes the deko3d
// blit flags and records a full barrier (when asked) + dkCmdBufBlitImage on
// the current cmd buf. Mirrors the barrier-then-blit order of the existing
// splash precedent in dk_device.cpp (L606-613).
//
// This header stays PLAIN (no <deko3d.h>): src/dst are passed as OPAQUE
// const void* (in practice a DkImageView* from FullTarget::resolved_view() or a
// view built by the caller); the .cpp casts them to DkImageView const*.
struct BlitRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t w = 0;
  uint32_t h = 0;
};

// Filter for the blit: Nearest -> no filter bit, Linear -> DkBlitFlag_FilterLinear.
enum class BlitFilter {
  Nearest,
  Linear,
};

// Record, on the current cmd buf (dkdev::cmd_handle()), a colour blit from
// src into dst:
//   - null src or dst view  -> one "[dk] rt blit null view" line, return false
//     (never crash);
//   - with_barrier == true  -> dkCmdBufBarrier(Full, 0) FIRST (3D->2D mixing),
//     then the blit (same order as the splash precedent);
//   - the DkImageRects are built with z=0, depth=1 (a single slice);
//   - flags = (Linear ? 1U<<4 : 0) | (flip_x ? 1U<<0 : 0) | (flip_y ? 1U<<1 : 0)
//     i.e. DkBlitFlag_FilterLinear / FlipX / FlipY; FilterNearest sets no bit;
//   - factor is 0 (deko3d blits are always colour; no mask parameter exists).
// Inert: no caller yet (T31b grounds the inventory, T38/T39 wire it).
//
// T31b (P3.1 slice 5b) grounds the wrapper in the REAL game blits. There are
// exactly 6 glBlitFramebuffer call sites in game/graphics/opengl_renderer/ (5
// colour + 1 depth) -- the gl-fbo-inventory "7 blits" guess is off by one; 6 is
// the real count (BlitDisplays.cpp has none; gl_time_split_funcs.h:759 is the
// timing wrapper, not a call site). Flip = Y-inversion / negative-width rect,
// read ONLY from what the code shows: none of these 6 sites flip (every coord
// is positive), so flip_x/flip_y stay false at call time. Columns:
// site | src -> dst | filter | flip | stretch.
//   1. OpenGLRenderer.cpp:2225 -- MSAA resolve: render_fbo (full 0,0,w,h) ->
//      resolve_buffer (full 0,0,w,h); GL_LINEAR; flip none; stretch NO.
//   2. opengl_utils.cpp:340 -- FramebufferCopier::copy: render region
//      (x0,y0,x1,y1) -> scaled dst (dst_x0..dst_y1, rescaled by
//      m_fbo_width/render_fb_w); GL_NEAREST; flip none; stretch YES.
//   3. opengl_utils.cpp:363 -- FramebufferCopier::copy_back_now: m_fbo
//      (m_fbo_width x m_fbo_height) -> render_fb (render_fb_w x render_fb_h);
//      GL_NEAREST; flip none; stretch YES.
//   4. sprite/Sprite3_Distort.cpp:624 -- distort: render_fb (full) ->
//      m_distort_ogl.fbo (fbo_width x fbo_height); GL_NEAREST; flip none;
//      stretch YES.
//   5. sprite/GlowRenderer.cpp:552 -- glow probe: render_fb (full) ->
//      m_ogl.probe_fbo (probe_fbo_w x probe_fbo_h); GL_NEAREST; flip none;
//      stretch YES. // OUT OF SCOPE: depth blit (mask GL_DEPTH_BUFFER_BIT; the
//      deko3d blit above is colour-only, so this row is NOT grounded here).
//   6. DepthCue.cpp:553 -- depth cue: render_fb sub-region (draw_offset +
//      draw_region) -> m_ogl.framebuffer_sample_fbo (sample_w x sample_h);
//      GL_NEAREST; flip none; stretch YES.
// No site needs stretch AND flip at once, so no row carries a "// T38: verify"
// note (the deko3d flag bits compose per the header enum, but no site exercises
// the combined case); the depth row 5 is OUT OF SCOPE -- do not implement.
bool blit(const void* src, const BlitRect& src_rect, const void* dst,
           const BlitRect& dst_rect, BlitFilter filter, bool flip_x,
           bool flip_y, bool with_barrier = true);


// ===== T32b (P3.1 slice 6b): readback_rgba8() (AI-assisted) ===============
// Half 2 of T32: the blocking CPU readback of an RGBA8 colour render
// target. Half 1 (T32a) owns, lazily, a dedicated DkQueue + its DkCmdBuf +
// a CPU-visible staging memblock + a command-memory memblock; this function
// is the first consumer of those resources (do not recreate anything).
//
// This header stays PLAIN (no <deko3d.h>): `view` is passed as an opaque
// const void* (in practice a DkImageView* from FullTarget::resolved_view());
// the .cpp casts it to DkImageView const* and drives the deko3d copy.
//
// On the source it performs, mirroring submit_upload_cmds / copy_to_gpu in
// dk_device.cpp, on the dedicated readback queue:
//   1. guards: null view/out or w==0 / h==0 -> one "[dk] rt readback bad
//      args" line and return false (never crash);
//   2. ensure_readback_resources(w,h) (T32a) -> false returns false;
//   3. a FRESH command slice: dkCmdBufClear + dkCmdBufAddMemory on the
//      dedicated readback cmd buffer, then dkCmdBufCopyImageToBuffer from the
//      source view (DkImageRect{0,0,0,w,h,1}) into
//      DkCopyBuf{staging_gpu_addr, rowLength=w, imageHeight=h}, copy flags 0
//      (the Flip/Blit bits are "only for BlitImage and CopyDataToImage", so
//      CopyImageToBuffer takes 0);
//   4. finish list -> dkQueueSubmitCommands -> dkQueueSignalFence(flush=true)
//      -> dkFenceWait(-1), the same submit/signal/wait ordering as
//      submit_upload_cmds;
//   5. memcpy the w*h*4 bytes from the CPU-visible staging into `out`; true.
//
// `out` must hold at least w*h*4 bytes. The caller must ensure the source is
// not being written (a completed frame). Inert: no caller yet (T40/T4x wire
// it). Not thread-safe: render-thread only, like the rest of dkrt.
bool readback_rgba8(const void* view, uint32_t w, uint32_t h, uint8_t* out);


}  // namespace dkrt
