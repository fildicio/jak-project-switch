# T36a — TFragmentDk skeleton + verts/indices upload (P3.2, AI-assisted)

Track D P3.2 · Depends on: T21 ✅ T22b ✅ T23 ✅ · Inert until T39 · One session (~30 min)
**v2 2026-10-19 (model-fit rewrite):** v1 died 3× on design freedom (include
choices, GL→dk "translation"). v2 gives the COMPLETE file contents verbatim.
Your job is ONLY: create 2 files, add 1 CMake line, build, A/B, report.
Do NOT redesign, do NOT change any include, do NOT read anything not listed.

## Goal

Half 1 of T36. New file pair `TFragmentDk.{h,cpp}` + the CMake line: the
`TreeCacheDk` struct and the vertex/index upload loop. NO time-of-day texture,
NO discard polish (T36b). NO draw code ever (T38).

## Session rules (hard — read twice)

- Time box ~30 min. Over budget → STOP at the last clean compile; report what remains.
- The file contents below are FIXED. Copy them exactly. No edits except where a
  line says `// TODO(T36b)` — there are none in this half.
- FORBIDDEN reads: everything except this file, QWEN_REPORT.md (append-only)
  and your two new files. This task needs NO other file — all facts are here.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T36a`.

## Facts (verified by the reviewer — do not re-derive, do not re-check)

- `dkdev::buffer_pool()` returns `dkpool::BufferPool*` (null when the device
  is down → return false, log one line).
- `pool->acquire(bytes)` returns `dkpool::PoolBuffer{id, cpu, gpu, capacity}`;
  `.ok()` is false on failure → skip that tree, log one line.
- `dkdev::copy_to_gpu(gpu_dst, src, size)` returns `CopyResult{ok, ms}`.
- `switch_run_logf` is printf-like, declared in `game/switch/run_log.h`.
- `tfrag3::TFRAG_GEOS` = 3 (constant in Tfrag3Data.h — already included).
- `TfragTree` fields used: `packed_vertices.vertices` (std::vector of
  `tfrag3::PreloadedVertex`, 32 B each), `unpacked.indices` (std::vector of
  u32), `draws`, `bvh`, `use_strips` — all inside `tfrag3::Level::tfrag_trees`.
- The GL version (`TFragment.cpp` `update_load`) pre-loads vertex buffers from
  the loader; the dk side uploads them itself instead. You do NOT need to read
  that file.

## Step 1 — create `game/graphics/deko3d_renderer/TFragmentDk.h` with EXACTLY:

```cpp
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
```

## Step 2 — create `game/graphics/deko3d_renderer/TFragmentDk.cpp` with EXACTLY:

```cpp
#include "game/graphics/deko3d_renderer/TFragmentDk.h"

#if defined(__SWITCH__)
#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
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
```

## Step 3 — CMake

In `game/CMakeLists.txt`, find the line `        graphics/deko3d_renderer/dk_render_target.cpp`
(8 leading spaces). Add DIRECTLY BELOW it, same indent:

```
        graphics/deko3d_renderer/TFragmentDk.cpp
```

## Step 4 — build + A/B

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS expected: NRO md5 `bb560fa75442dc67dd336826ac5bb154` (new TU, zero
callers → linker strips it; if it MOVES, A/B-attribute: stash only your two new
files + the CMake line, rebuild, compare, restore, rebuild — report both md5s;
STOP only if the A/B does not attribute).

## Report

`## T36a` in QWEN_REPORT.md: files created, build result, both md5s, any
compile error you had to touch (quote the exact error + the one-line fix).

## Doubts

There are none. If it does not compile, quote the exact first error in the
report and stop — do not redesign.

> REVIEWER FIX 2026-10-19: the expected md5 above was rebaselined from
> 67018cd5 (stale, pre-T35b) to bb560fa7 (the T35b-v2 baseline) — the run
> PASSED against the real value. Task done; history note only.
