#include "game/graphics/deko3d_renderer/TFragmentDk.h"

#if defined(__SWITCH__)
#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

namespace {

// Per-geom tree caches, same shape as the GL renderer's m_cached_trees.
std::vector<dktf::TreeCacheDk> s_trees[tfrag3::TFRAG_GEOS];

}  // namespace

namespace dktf {

void discard_tree_cache() {
  // FIX 33: BufferPool chunks are pooled, not freed — dropping the cache IS
  // the discard (GL parity: TFragment::discard_tree_cache).
  for (auto& geom : s_trees) {
    geom.clear();
  }
  // T36b note: the time-of-day texture ids die with the cache (dktexgpu slots
  // are monotonic; T38 owns per-frame invalidation, not this function).
}

bool setup_for_level(const tfrag3::Level* lev_data, const char* level_name) {
  discard_tree_cache();
  const char* name = level_name ? level_name : "?";
  if (lev_data == nullptr) {
    switch_run_logf("[dk] tfrag setup: null level (%s)", name);
    return false;
  }
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  if (pool == nullptr) {
    switch_run_logf("[dk] tfrag setup: no buffer pool (%s)", name);
    return false;
  }

  int trees = 0;
  for (int geom = 0; geom < tfrag3::TFRAG_GEOS; ++geom) {
    for (size_t tree_idx = 0; tree_idx < lev_data->tfrag_trees[geom].size(); ++tree_idx) {
      const auto& tree = lev_data->tfrag_trees[geom][tree_idx];
      TreeCacheDk cache;

      // vertices: PreloadedVertex array, 32 bytes each.
      const auto& verts = tree.packed_vertices.vertices;
      if (!verts.empty()) {
        const uint32_t bytes = uint32_t(verts.size() * sizeof(tfrag3::PreloadedVertex));
        dkpool::PoolBuffer pb = pool->acquire(bytes);
        if (!pb.ok()) {
          switch_run_logf("[dk] tfrag setup: verts acquire failed (tree %zu, geom %d)", tree_idx, geom);
          continue;  // skip THIS tree, keep the rest
        }
        if (!dkdev::copy_to_gpu(pb.gpu, verts.data(), bytes).ok) {
          pool->release(pb.id);
          switch_run_logf("[dk] tfrag setup: verts copy failed (tree %zu, geom %d)", tree_idx, geom);
          continue;
        }
        cache.vert_gpu = pb.gpu;
        cache.vert_count = uint32_t(verts.size());
      }

      // indices: u32 each.
      const auto& inds = tree.unpacked.indices;
      if (!inds.empty()) {
        const uint32_t bytes = uint32_t(inds.size() * sizeof(uint32_t));
        dkpool::PoolBuffer pb = pool->acquire(bytes);
        if (!pb.ok()) {
          switch_run_logf("[dk] tfrag setup: indices acquire failed (tree %zu, geom %d)", tree_idx, geom);
          continue;
        }
        if (!dkdev::copy_to_gpu(pb.gpu, inds.data(), bytes).ok) {
          pool->release(pb.id);
          switch_run_logf("[dk] tfrag setup: indices copy failed (tree %zu, geom %d)", tree_idx, geom);
          continue;
        }
        cache.idx_gpu = pb.gpu;
        cache.idx_count = uint32_t(inds.size());
      }

      // time-of-day palette (T36b): one zeroed 8192x1 RGBA8 upload so frame 1
      // has a valid texture; T38 uploads the real interpolated palette later.
      {
        static uint8_t s_tod_zeros[8192 * 4] = {};  // TIME_OF_DAY_COLOR_COUNT x RGBA8
        const uint32_t tod_id = dktexgpu::upload_rgba8(8192, 1, s_tod_zeros);
        if (tod_id != 0) {
          cache.tod_tex_id = int(tod_id);
        }
        // tod_id == 0: failure already logged at the source — leave -1, keep going.
      }
      cache.draws = &tree.draws;
      cache.vis = &tree.bvh;
      cache.use_strips = tree.use_strips;
      s_trees[geom].push_back(cache);
      ++trees;
    }
  }
  switch_run_logf("[dk] tfrag setup: level %s, trees=%d", name, trees);
  return true;
}

}  // namespace dktf
#endif  // __SWITCH__
