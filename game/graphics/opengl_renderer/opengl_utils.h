#pragma once

#include "common/math/Vector.h"

#include "game/graphics/pipelines/opengl.h"

struct SharedRenderState;
class ScopedProfilerNode;

/*!
 * This is a wrapper around a framebuffer and texture to make it easier to render to a texture.
 */
class FramebufferTexturePair {
 public:
  FramebufferTexturePair(int w, int h, u64 texture_format, int num_levels = 1);
  ~FramebufferTexturePair();

  GLuint texture() const { return m_texture; }

  void update_texture_size(int w, int h) {
    m_w = w;
    m_h = h;
  }

  void update_texture_unsafe(GLuint texture) { m_texture = texture; }

  FramebufferTexturePair(const FramebufferTexturePair&) = delete;
  FramebufferTexturePair& operator=(const FramebufferTexturePair&) = delete;
  FramebufferTexturePair(FramebufferTexturePair&& other) {
    if (this == &other) {
      return;
    }
    ASSERT(!m_moved_from && !other.m_moved_from);
    other.m_moved_from = true;
    m_w = other.m_w;
    m_h = other.m_h;
    m_texture = other.m_texture;
    m_framebuffers = std::move(other.m_framebuffers);
  }
  int width() const { return m_w; }
  int height() const { return m_h; }

 private:
  friend class FramebufferTexturePairContext;
  std::vector<GLuint> m_framebuffers;
  GLuint m_texture;
  int m_w, m_h;
  bool m_moved_from = false;
};

class FramebufferTexturePairContext {
 public:
  FramebufferTexturePairContext(FramebufferTexturePair& fb, int level = 0);
  ~FramebufferTexturePairContext();

  void switch_to(FramebufferTexturePair& fb);

  FramebufferTexturePairContext(const FramebufferTexturePairContext&) = delete;
  FramebufferTexturePairContext& operator=(const FramebufferTexturePairContext&) = delete;

 private:
  FramebufferTexturePair* m_fb;
  GLint m_old_viewport[4];
  GLint m_old_framebuffer;
};

// draw over the full screen.
// you must set alpha/ztest/etc.
class FullScreenDraw {
 public:
  FullScreenDraw();
  ~FullScreenDraw();
  FullScreenDraw(const FullScreenDraw&) = delete;
  FullScreenDraw& operator=(const FullScreenDraw&) = delete;
  void draw(const math::Vector4f& color, SharedRenderState* render_state, ScopedProfilerNode& prof);

 private:
  GLuint m_vao;
  GLuint m_vertex_buffer;
};

class FullScreenTexDraw {
 public:
  FullScreenTexDraw();
  ~FullScreenTexDraw();
  FullScreenTexDraw(const FullScreenTexDraw&) = delete;
  FullScreenTexDraw& operator=(const FullScreenTexDraw&) = delete;
  void draw(const math::Vector4f& color,
            const math::Vector2f& tex0,
            const math::Vector2f& tex1,
            SharedRenderState* render_state,
            ScopedProfilerNode& prof);

 private:
  GLuint m_vao;
  GLuint m_vertex_buffer;
};

class FramebufferCopier {
 public:
  FramebufferCopier();
  ~FramebufferCopier();
  FramebufferCopier(const FramebufferCopier&) = delete;
  FramebufferCopier& operator=(const FramebufferCopier&) = delete;
  void copy_now(int render_fb_w, int render_fb_h, GLuint render_fb);
  // FIX 50 (AI-assisted): region-limited copy. The callers sample a sub-rectangle
  // of the framebuffer, so copying the whole thing is wasted VRAM bandwidth and a
  // full-screen tile resolve on Tegra. See the implementation for the measurements.
  void copy_region_now(int render_fb_w,
                       int render_fb_h,
                       GLuint render_fb,
                       int x0,
                       int y0,
                       int x1,
                       int y1);
  // FIX 50 (AI-assisted): true when copy_now() with these dimensions would
  // reproduce exactly the contents already in this copier, so the caller can skip
  // it. BlitDisplays runs do_zoom_blur/do_slow_time/apply_color_filter in one
  // frame and several of them captured the same image.
  bool holds(int render_fb_w, int render_fb_h) const {
    return m_fbo_width == render_fb_w && m_fbo_height == render_fb_h && m_has_contents;
  }
  void copy_back_now(int render_fb_w, int render_fb_h, GLuint render_fb);
  u64 texture() const { return m_fbo_texture; }
  int width() const { return m_fbo_width; }
  int height() const { return m_fbo_height; }

 private:
  GLuint m_fbo = 0, m_fbo_texture = 0;
  int m_fbo_width = 640, m_fbo_height = 480;
  // FIX 50 (AI-assisted): whether the texture currently holds a captured image, so
  // a caller can skip a copy that would reproduce what is already there.
  bool m_has_contents = false;
};