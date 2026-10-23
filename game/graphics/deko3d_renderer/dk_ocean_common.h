/*!
 * dk_ocean_common.h - deko3d ocean common recorder (T93, AI-assisted)
 *
 * Verbatim copy of the pure-CPU decode half of
 * game/graphics/opengl_renderer/ocean/CommonOceanRenderer.h + .cpp:
 * init_for_near / kick_from_near / handle_near_* / init_for_mid /
 * kick_from_mid / handle_mid_adgif / reverse_indices.
 *
 * Differences from the GL header (and the ONLY differences):
 *  - class renamed CommonOceanRenderer -> CommonRecorder, wrapped in
 *    namespace dkocean (the _PS2 ocean files call these through a recorder)
 *  - BucketRenderer.h include and the flush_near/flush_mid declarations are
 *    dropped: the deko3d flush is recorded by a separate file
 *  - the GL constructor/destructor (GL buffer setup/teardown) are dropped; the
 *    deko3d constructor sizes the decode buffers instead (T96 fix: the first
 *    build shipped it empty, the vectors stayed null, and the first console
 *    ocean kick wrote through null pointers - data abort far=0x0)
 *  - the m_ogl struct (GL handles) is dropped
 *  - reverse_indices gets a declaration here (in the GL tree it is only
 *    defined in the .cpp) because the deko3d flush uses it too
 *  - friend struct OceanRecorderTestPeer: host tests read private decode
 *    state (test/test_dk_ocean_common.cpp); no runtime effect
 *
 * Plain C++ header: no deko3d includes here (see dk_device.h comment).
 */

#pragma once

#include "common/math/Vector.h"
#include "common/common_types.h"

#include <vector>

namespace dkocean {

// Free function, copied verbatim from the GL .cpp (defined in dk_ocean_common.cpp).
// Declared before the class: CommonRecorder::reverse_bucket_indices uses it.
void reverse_indices(u32* indices, u32 count);

class CommonRecorder {
 public:
  CommonRecorder();

  void init_for_near();
  void kick_from_near(const u8* data);

  void init_for_mid();
  void kick_from_mid(const u8* data);

  // T95 (AI-assisted): read access for the deko3d flush recorder
  // (dk_ocean_flush.cpp). GL flush_near/flush_mid read these members
  // directly; the layout of Vertex is part of the GPU contract (32 B).
  struct Vertex {
    math::Vector<float, 3> xyz;
    math::Vector<u8, 4> rgba;
    math::Vector<float, 3> stq;
    u8 fog;
    u8 pad[3];
  };
  static_assert(sizeof(Vertex) == 32);

  static constexpr int NUM_BUCKETS = 3;

  // T96 (AI-assisted): decode buffer sizing. GL's CommonOceanRenderer ctor
  // resizes m_vertices + 3x m_indices to 4096*10 (~1.8 MB per recorder, 3.5 MB
  // across near+mid - too much for the ~4 MB Switch heap headroom). Start
  // small and double on demand (ensure_capacity), capped at GL's design max.
  static constexpr u32 kInitialCapacity = 8192;
  static constexpr u32 kMaxCapacity = 4096 * 10;

  const Vertex* vertices_data() const { return m_vertices.data(); }
  u32 next_free_vertex() const { return m_next_free_vertex; }
  const u32* indices_data(int bucket) const { return m_indices[bucket].data(); }
  u32 next_free_index(int bucket) const { return m_next_free_index[bucket]; }
  // T95: GL flush_mid reverses the envmap bucket's indices before the draw
  // (the high-poly-wins trick); the dk driver does it at capture time.
  void reverse_bucket_indices(int bucket) {
    reverse_indices(m_indices[bucket].data(), m_next_free_index[bucket]);
  }
  // Raw tbp of the envmap texture the mid adgif picked (0 until the first
  // handle_mid_adgif with a TEX0_1; GL flush looks it up in the pool).
  u32 envmap_tex() const { return m_envmap_tex; }

 private:
  void handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop);
  void handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop);

  void handle_near_adgif(const u8* data, u32 offset, u32 count);

  void handle_mid_adgif(const u8* data, u32 offset);

  // T96 (AI-assisted): grow m_vertices and the current bucket's index buffer so
  // the next kick can write extra_verts / extra_indices more entries. Returns
  // false (growing nothing) when that would exceed kMaxCapacity; callers drop
  // the kick instead of writing out of bounds.
  bool ensure_capacity(u32 extra_verts, u32 extra_indices);

  enum VertexBucket {
    RGB_TEXTURE = 0,
    ALPHA = 1,
    ENV_MAP = 2,
  };
  u32 m_current_bucket = VertexBucket::RGB_TEXTURE;

  // (T95) the Vertex struct + NUM_BUCKETS moved to the public section above;
  // the members below are unchanged.

  std::vector<Vertex> m_vertices;
  u32 m_next_free_vertex = 0;

  std::vector<u32> m_indices[NUM_BUCKETS];
  u32 m_next_free_index[NUM_BUCKETS] = {0};

  u32 m_envmap_tex = 0;

  friend struct OceanRecorderTestPeer;  // host tests read private decode state
};

// (T95) moved up above the class - CommonRecorder::reverse_bucket_indices calls it.

}  // namespace dkocean

