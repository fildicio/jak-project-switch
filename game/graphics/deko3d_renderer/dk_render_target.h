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

}  // namespace dkrt
