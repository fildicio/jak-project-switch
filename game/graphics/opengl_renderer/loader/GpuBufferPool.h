#pragma once

#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "third-party/glad/include/glad/glad.h"

/*!
 * FIX 33 (AI-assisted): pooled GL buffer objects for the level loader.
 *
 * On the Switch (Mesa/nouveau) every glBufferData goes through the Tegra
 * suballocator (nouveau_mm). The Sep 20 2026 jak2 crash logs show area
 * transitions dying inside nouveau_mm_allocate: the loader staged a whole new
 * area while all old levels were still resident, and the allocator gave up
 * during a large merc allocation. See SWITCH_PORT_SESSION_NOTES.md.
 *
 * The pool removes buffer allocation churn once warmed up:
 *  - acquire() reuses a previously released buffer of a close-enough size
 *    class instead of glGenBuffers + glBufferData. Reused buffers keep their
 *    original (>= requested) storage. The loader always SubData-fills the
 *    entire logical range before a level becomes drawable, and draw calls
 *    only reference that logical range, so stale tail bytes are never read.
 *  - release() recycles buffers into the pool instead of glDeleteBuffers.
 *    The memory stays owned by the pool and is handed to the next area that
 *    loads, so steady-state GPU memory tracks "live levels", not "levels ever
 *    visited".
 *
 * Buffer objects in GL 3.x core are target-agnostic, so a buffer acquired
 * through GL_ARRAY_BUFFER may later be reused through
 * GL_ELEMENT_ARRAY_BUFFER (and vice versa); acquire() simply binds it to
 * whatever target is asked for.
 *
 * Not thread-safe: only the GL/render thread may touch it (every loader GPU
 * stage runs there, including load_common during init).
 */
class GpuBufferPool {
 public:
  // Buffers are pooled in 256 KB size classes.
  static constexpr GLsizeiptr SIZE_CLASS = 256 * 1024;

  /*!
   * Get a buffer bound to `target` with storage >= `size` bytes. Allocates a
   * new GL buffer if nothing suitable is pooled. The returned buffer is
   * already bound to `target`.
   */
  GLuint acquire(GLenum target, GLsizeiptr size) {
    GLsizeiptr want = size_class_of(size);
    GLsizeiptr best = 0;
    for (auto& [cls, list] : m_free) {
      if (!list.empty() && cls >= want && (best == 0 || cls < best)) {
        best = cls;
      }
    }
    // Don't waste more than 2x the request when reusing an oversized buffer.
    if (best != 0 && best > want * 2) {
      best = 0;
    }
    GLuint id;
    if (best != 0) {
      id = m_free[best].back();
      m_free[best].pop_back();
      if (m_free[best].empty()) {
        m_free.erase(best);
      }
      m_pooled_bytes -= best;
      glBindBuffer(target, id);
    } else {
      glGenBuffers(1, &id);
      glBindBuffer(target, id);
      glBufferData(target, want, nullptr, GL_STATIC_DRAW);
      best = want;
    }
    m_sizes[id] = best;
    return id;
  }

  /*!
   * Return a buffer previously handed out by acquire(). Issues no GL calls.
   * Unknown / zero / already-released ids are ignored (defensive against the
   * old hfrag double-delete bug class).
   */
  void release(GLuint id) {
    if (id == 0) {
      return;
    }
    auto it = m_sizes.find(id);
    if (it == m_sizes.end()) {
      return;
    }
    GLsizeiptr cls = it->second;
    m_sizes.erase(it);
    m_free[cls].push_back(id);
    m_pooled_bytes += cls;
  }

  /*!
   * Delete every pooled (free) buffer. Outstanding (handed-out) buffers stay
   * valid and tracked: when released later they re-enter the (now empty)
   * free lists. Render thread only.
   */
  void clear() {
    for (auto& [cls, list] : m_free) {
      for (GLuint id : list) {
        glDeleteBuffers(1, &id);
      }
    }
    m_free.clear();
    m_pooled_bytes = 0;
  }

  int pooled_buffers() const {
    int n = 0;
    for (auto& [cls, list] : m_free) {
      n += (int)list.size();
    }
    return n;
  }
  size_t pooled_bytes() const { return m_pooled_bytes; }
  int outstanding_buffers() const { return (int)m_sizes.size(); }

 private:
  static GLsizeiptr size_class_of(GLsizeiptr size) {
    GLsizeiptr cls = ((size + SIZE_CLASS - 1) / SIZE_CLASS) * SIZE_CLASS;
    return cls < SIZE_CLASS ? SIZE_CLASS : cls;  // no zero-sized classes
  }

  // outstanding id -> its size class (capacity)
  std::unordered_map<GLuint, GLsizeiptr> m_sizes;
  // size class -> free ids
  std::unordered_map<GLsizeiptr, std::vector<GLuint>> m_free;
  size_t m_pooled_bytes = 0;
};
