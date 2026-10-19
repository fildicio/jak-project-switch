#pragma once
// deko3d tfrag-level GPU cache (AI-assisted, T36a). The .cpp body is
// #if defined(__SWITCH__) like the other dk files; this header stays plain
// C++ (repo rule: no deko3d includes in headers).
#include <cstdint>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"

// The loader's LevelData (opengl_renderer/loader/common.h) pulls GL headers
// (glad, TexturePool) and must NOT be included from dk code. The dk side
// takes the pure-data tfrag3::Level instead; the seam is wired in T39.
struct LevelData;

namespace dktf {

// One tfrag tree on the dk side (T36a fills the GPU buffers; T36b the texture).
struct TreeCacheDk {
  uint64_t vert_gpu = 0;    // BufferPool GPU addr (0 = none)
  uint32_t vert_count = 0;
  uint64_t idx_gpu = 0;
  uint32_t idx_count = 0;
  int tod_tex_id = -1;      // dktexgpu id, -1 = none (T36b fills it)
  const std::vector<tfrag3::StripDraw>* draws = nullptr;
  const tfrag3::BVH* vis = nullptr;
  bool use_strips = false;
};

// Upload every tfrag tree's vertices + indices to the level-data BufferPool
// (dkdev::buffer_pool()). lev_data is loader_data->level.get() — the caller
// makes that one-line extraction (T39). Skips (one log line each) any tree
// whose upload fails; never crashes. Render thread only.
bool setup_for_level(const tfrag3::Level* lev_data, const char* level_name);

// Drop the cached trees (BufferPool chunks are pooled — nothing to free, FIX 33).
void discard_tree_cache();

}  // namespace dktf
