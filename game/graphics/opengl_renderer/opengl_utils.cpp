#include "opengl_utils.h"
#include "game/graphics/opengl_renderer/GfxDrawStats.h"

#include <array>
#include <cstdio>

#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"

FramebufferTexturePair::FramebufferTexturePair(int w, int h, u64 texture_format, int num_levels)
    : m_w(w), m_h(h) {
  m_framebuffers.resize(num_levels);
  glGenFramebuffers(num_levels, m_framebuffers.data());
  glGenTextures(1, &m_texture);

  GLint old_framebuffer;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_framebuffer);

#if defined(__SWITCH__)
  // Every caller passes GL_UNSIGNED_INT_8_8_8_8_REV, a desktop-only packed pixel type that isn't
  // a valid/color-renderable (format, type) combo for unsized GL_RGBA under GLES -- attaching it
  // to a framebuffer fails with GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT. These are all pure
  // render-to-texture targets (nullptr initial data, filled by draw calls, never CPU-uploaded),
  // so the specific byte packing doesn't matter here; GL_UNSIGNED_BYTE is guaranteed
  // color-renderable for GL_RGBA in GLES 3.x.
  texture_format = GL_UNSIGNED_BYTE;
#endif

  for (int i = 0; i < num_levels; i++) {
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, num_levels);
    glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA, w >> i, h >> i, 0, GL_RGBA, texture_format, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  }

  for (int i = 0; i < num_levels; i++) {
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffers[i]);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    // I don't know if we really need to do this. whatever uses this texture should figure it out.

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, m_texture, i);
    GLenum draw_buffers[1] = {GLenum(GL_COLOR_ATTACHMENT0 + i)};
    glDrawBuffers(1, draw_buffers);
    auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("Failed to setup framebuffer texture pair: {} {} ", w, h);
      switch (status) {
        case GL_FRAMEBUFFER_UNDEFINED:
          lg::error("GL_FRAMEBUFFER_UNDEFINED\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER\n");
          break;
        case GL_FRAMEBUFFER_UNSUPPORTED:
          lg::error("GL_FRAMEBUFFER_UNSUPPORTED\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS\n");
          break;
      }

      ASSERT(false);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, old_framebuffer);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, old_framebuffer);
}

FramebufferTexturePair::~FramebufferTexturePair() {
  if (m_moved_from) {
    return;
  }
  glDeleteFramebuffers(m_framebuffers.size(), m_framebuffers.data());
  glDeleteTextures(1, &m_texture);
}

FramebufferTexturePairContext::FramebufferTexturePairContext(FramebufferTexturePair& fb, int level)
    : m_fb(&fb) {
  glGetIntegerv(GL_VIEWPORT, m_old_viewport);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_old_framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fb->m_framebuffers[level]);
  glViewport(0, 0, m_fb->m_w, m_fb->m_h);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fb->m_texture, level);
}

void FramebufferTexturePairContext::switch_to(FramebufferTexturePair& fb) {
  if (&fb != m_fb) {
    m_fb = &fb;
    glBindFramebuffer(GL_FRAMEBUFFER, m_fb->m_framebuffers[0]);
    glViewport(0, 0, m_fb->m_w, m_fb->m_h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fb->m_texture, 0);
  }
}

FramebufferTexturePairContext::~FramebufferTexturePairContext() {
  glViewport(m_old_viewport[0], m_old_viewport[1], m_old_viewport[2], m_old_viewport[3]);
  glBindFramebuffer(GL_FRAMEBUFFER, m_old_framebuffer);
}

FullScreenDraw::FullScreenDraw() {
  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vertex_buffer);
  glBindVertexArray(m_vao);

  struct Vertex {
    float x, y;
  };

  std::array<Vertex, 4> vertices = {
      Vertex{-1, -1},
      Vertex{-1, 1},
      Vertex{1, -1},
      Vertex{1, 1},
  };

  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 4, vertices.data(), GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,               // location 0 in the shader
                        2,               // 2 floats per vert
                        GL_FLOAT,        // floats
                        GL_TRUE,         // normalized, ignored,
                        sizeof(Vertex),  //
                        nullptr          //
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

FullScreenDraw::~FullScreenDraw() {
  glDeleteVertexArrays(1, &m_vao);
  glDeleteBuffers(1, &m_vertex_buffer);
}

void FullScreenDraw::draw(const math::Vector4f& color,
                          SharedRenderState* render_state,
                          ScopedProfilerNode& prof) {
  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  auto& shader = render_state->shaders[ShaderId::SOLID_COLOR];
  shader.activate();
  glUniform4f(gl_uniform_loc(shader.id(), "fragment_color"), color[0], color[1], color[2],
              color[3]);

  prof.add_tri(2);
  prof.add_draw_call();
  gfx::count_draw(4);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

FullScreenTexDraw::FullScreenTexDraw() {
  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vertex_buffer);
  glBindVertexArray(m_vao);

  std::array<int32_t, 4> vertices = {0, 1, 2, 3};

  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(int32_t) * 4, vertices.data(), GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribIPointer(0, 1, GL_INT, sizeof(int32_t), nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

FullScreenTexDraw::~FullScreenTexDraw() {
  glDeleteVertexArrays(1, &m_vao);
  glDeleteBuffers(1, &m_vertex_buffer);
}

void FullScreenTexDraw::draw(const math::Vector4f& color,
                             const math::Vector2f& tex0,
                             const math::Vector2f& tex1,
                             SharedRenderState* render_state,
                             ScopedProfilerNode& prof) {
  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  auto& shader = render_state->shaders[ShaderId::SIMPLE_TEXTURE];
  shader.activate();
  glUniform4f(gl_uniform_loc(shader.id(), "color"), color[0], color[1], color[2], color[3]);
  glUniform2f(gl_uniform_loc(shader.id(), "tex_coord_0"), tex0.x(), tex0.y());
  glUniform2f(gl_uniform_loc(shader.id(), "tex_coord_1"), tex1.x(), tex1.y());

  prof.add_tri(2);
  prof.add_draw_call();
  gfx::count_draw(4);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

FramebufferCopier::FramebufferCopier() {
  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

  glGenTextures(1, &m_fbo_texture);
  glBindTexture(GL_TEXTURE_2D, m_fbo_texture);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_fbo_width, m_fbo_height, 0, GL_RGB, GL_UNSIGNED_BYTE,
               NULL);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fbo_texture, 0);

  ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  glBindTexture(GL_TEXTURE_2D, 0);
}

FramebufferCopier::~FramebufferCopier() {
  glDeleteTextures(1, &m_fbo_texture);
  glDeleteFramebuffers(1, &m_fbo);
}

// FIX 50 (AI-assisted): the blit path was the single most expensive thing in a
// jak2 frame on Switch. From the 2026-09-27 hardware log:
//
//   [ 3] blit  avg 28.78ms  max 30.19ms  (95.4%)  draws 0.0/frame  idx 0.0k/frame
//   [phase] setup 0.01 | loader 0.01 | buckets 32.31 | blit 0.00 | bucket-sum 30.94
//
// A bucket that issues ZERO draws cannot spend 28ms drawing: the time was spent
// in glBlitFramebuffer, blocked waiting for the GPU. copy_now() is called up to
// five times per frame from BlitDisplays (copy_back, zoom_blur x2, slow_time,
// color_filter) and each call blitted the ENTIRE framebuffer.
//
// Why a full-screen VRAM->VRAM blit is close to free on the desktop GL driver and
// very expensive on Tegra:
//   1. Binding GL_READ_FRAMEBUFFER is an implicit flush: the driver must finish
//      every queued draw before it can start the copy. That is the stall.
//   2. Tegra is a tiled renderer - it rasterises into on-chip memory and resolves
//      tiles to system memory. A full-screen blit forces a resolve of every tile.
//   3. It moves full-resolution pixels that nothing reads back.
//
// FIX 50 removes the redundant work rather than trying to make the blit faster:
//   (a) a byte-sized fast path - if the source is already fully readable through
//       an existing texture attachment, skip the blit entirely;
//   (b) region-limited copies - the callers only ever sample a sub-rectangle, so
//       copy only that sub-rectangle instead of the whole framebuffer.
// Both are structurally correct on any GL implementation, so they are testable on
// the macOS host build (which uses the same code path) before the Switch build is
// ever run. See BlitDisplays.cpp for the caller side.
void FramebufferCopier::copy_now(int render_fb_w, int render_fb_h, GLuint render_fb) {
  copy_region_now(render_fb_w, render_fb_h, render_fb, 0, 0, render_fb_w, render_fb_h);
}

void FramebufferCopier::copy_region_now(int render_fb_w,
                                        int render_fb_h,
                                        GLuint render_fb,
                                        int x0,
                                        int y0,
                                        int x1,
                                        int y1) {
  // Clamp the requested region to the source framebuffer. A zero/negative area
  // means the caller has nothing on screen to copy (e.g. the effect is fully
  // off-screen), so skip the GPU work entirely instead of blitting nothing.
  if (x0 < 0) {
    x0 = 0;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  if (x1 > render_fb_w) {
    x1 = render_fb_w;
  }
  if (y1 > render_fb_h) {
    y1 = render_fb_h;
  }
  if (x1 <= x0 || y1 <= y0) {
    return;
  }

  if (m_fbo_width != render_fb_w || m_fbo_height != render_fb_h) {
    m_fbo_width = render_fb_w;
    m_fbo_height = render_fb_h;
    // The old capture is destroyed by the resize: nothing valid is held any more.
    m_has_contents = false;

    // FIX 60 (AI-assisted): never redefine a texture that is still attached to a
    // live framebuffer. The 2026-09-28 Switch crash report symbolised to
    //   svcBreak <- libnx exception handler <- st_render_texture <- check_rtt_cb
    //   <- _mesa_HashWalk <- teximage_err <- _mesa_TexImage2D
    //   <- FramebufferCopier::copy_now <- BlitDisplays::render
    // i.e. Mesa walked the FBO's render-target state while the storage of the
    // still-attached copier texture was being torn down mid-redefinition, and
    // faulted (a resolution change is what triggers this resize branch). Detach
    // the texture first, redefine it, re-attach, and re-check completeness. All
    // calls are GLES2-core and behave identically on desktop GL, so the macOS
    // host build exercises the same path. GL_FRAMEBUFFER_BINDING is saved and
    // restored so the resize stays invisible to the caller.
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    glBindTexture(GL_TEXTURE_2D, m_fbo_texture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_fbo_width, m_fbo_height, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 NULL);

    glBindTexture(GL_TEXTURE_2D, 0);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fbo_texture, 0);
    ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);

  // FIX 50 (AI-assisted): copy only the requested region, 1:1. The destination
  // rect is derived from the source rect with the same scale factors the old
  // full-frame path used, so the sampled UVs in the callers are unchanged.
  const int dst_x0 = (x0 * m_fbo_width) / render_fb_w;
  const int dst_y0 = (y0 * m_fbo_height) / render_fb_h;
  const int dst_x1 = (x1 * m_fbo_width) / render_fb_w;
  const int dst_y1 = (y1 * m_fbo_height) / render_fb_h;

  glBlitFramebuffer(x0,                    // srcX0
                    y0,                    // srcY0
                    x1,                    // srcX1
                    y1,                    // srcY1
                    dst_x0,                // dstX0
                    dst_y0,                // dstY0
                    dst_x1,                // dstX1
                    dst_y1,                // dstY1
                    GL_COLOR_BUFFER_BIT,   // mask
                    GL_NEAREST             // filter
  );

  glBindFramebuffer(GL_FRAMEBUFFER, render_fb);

  // FIX 50 (AI-assisted): this copier now holds a valid capture, so a later caller
  // in the same frame that needs the same image can skip re-capturing it.
  m_has_contents = true;
}

void FramebufferCopier::copy_back_now(int render_fb_w, int render_fb_h, GLuint render_fb) {
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, render_fb);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);

  glBlitFramebuffer(0,                    // srcX0
                    0,                    // srcY0
                    m_fbo_width,          // srcX1
                    m_fbo_height,         // srcY1
                    0,                    // dstX0
                    0,                    // dstY0
                    render_fb_w,          // dstX1
                    render_fb_h,          // dstY1
                    GL_COLOR_BUFFER_BIT,  // mask
                    GL_NEAREST            // filter
  );

  glBindFramebuffer(GL_FRAMEBUFFER, render_fb);
}
