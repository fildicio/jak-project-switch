# T36 — TFragmentDk level setup: vertices, indices, time-of-day texture (P3.2, AI-assisted)

Track D P3.2 · Depends on: T21 ✅ T22b ✅ T23 ✅ · Inert until T39 · One session

## Goal

New file pair `deko3d_renderer/TFragmentDk.{h,cpp}` (knowledge-map §16 recipe:
new file, never edit the GL one): `setup_for_level()` copies the level's tfrag3
GPU data into deko3d resources — packed vertices + indices into the T21
BufferPool, the time-of-day palette as an RGBA8 texture via T22b — and caches
the per-tree state the draw task (T38/T39) will consume. NO draw code here.

## Context (read before writing)

- `game/graphics/opengl_renderer/background/TFragment.cpp:280-345` — the GL
  `setup_for_level` loop you are mirroring: `lev_data->tfrag_trees[geom]`,
  `tree.packed_vertices.vertices` (u32 count at L296), indices
  `tree.unpacked.indices` (u32 each, L341), `tree.colors` (PackedTimeOfDay,
  `color_count` at L295), `tree.draws`, `tree.bvh`, `use_strips` (L306).
- `common/custom_data/Tfrag3Data.h:87-104` — `PreloadedVertex`:
  `x,y,z` float @0, `r,g,b,a` u8 @12, `s,t` float @16, `nor` u32 @24,
  `color_index` u16 @28, sizeof = 32 (verify with a `static_assert` in the .h —
  if it is not 32, STOP and report).
- `game/graphics/deko3d_renderer/dk_buffer_pool.h` — BufferPool acquire
  (FIX 33 contract: skip-and-log when null or !ok).
- `game/graphics/deko3d_renderer/dk_texture_gpu.h` — `upload_rgba8(...)`
  signature + the slot/id contract (dk_id mapping like T23's `GpuTexture::dk_id`).
- `game/graphics/opengl_renderer/background/background_common.h:97` —
  `make_multidraws_from_vis_string` (you only NOTE it here; T38 calls it).

## API cheat-sheet (verified)

- Uploads: `dkdev::copy_to_gpu(gpu_dst, src, size)` (dk_device.h:59) into a
  BufferPool acquire — blocking, render-thread-only, already reviewed T18/T21.
- Time-of-day texture: GL uploads `colors` as a `color_count x 1` RGBA8 2D
  texture (grep `time_of_day_texture` in TFragment.cpp for the exact
  glTexImage2D call + `TIME_OF_DAY_COLOR_COUNT` cap = 8192, TFragment.h:176) —
  dk equivalent: `dktexgpu::upload_rgba8` with w=color_count, h=1, then keep the
  returned slot/id.
- NO deko3d calls in this task at all (BufferPool + dktexgpu are the whole GPU
  surface). Vertex attrib arrays are T38.

## Steps

1. `TFragmentDk.h`: Switch-only, plain where possible:
   ```cpp
   struct TreeCacheDk {            // one tfrag tree on the dk side
     uint64_t vert_gpu = 0;        // BufferPool GPU addr (0 = none)
     uint32_t vert_count = 0;
     uint64_t idx_gpu = 0;
     uint32_t idx_count = 0;
     int tod_tex_id = -1;          // dktexgpu id, -1 = none
     // pointers into loader-owned data (valid while the level is loaded)
     const std::vector<tfrag3::StripDraw>* draws = nullptr;
     const tfrag3::BVH* vis = nullptr;
     bool use_strips = false;
   };
   bool setup_for_level(const LevelData* loader_data, const char* level_name);
   void discard_tree_cache();
   ```
2. .cpp: loop geom 0..2 / trees like the GL loop; per tree:
   - acquire `verts * 32` bytes from BufferPool (32 = sizeof(PreloadedVertex)),
     `copy_to_gpu` the `packed_vertices.vertices` data (contiguous vector).
   - acquire `indices * 4` bytes, copy `unpacked.indices`.
   - build the tod texture via `interp_time_of_day`-produced data? NO — the GL
     path uploads `colors.color_count` PACKED entries after interpolation each
     frame (FIX 37 cache, TFragment.h:122-123). For setup, only allocate/track;
     the per-frame upload is T38. So here: store `tod_last_itimes` invalid +
     `tod_tex_id` uploaded ONCE with the initial palette (colors at itimes=first)
     so T38 has a valid texture on frame 1. Read GL TFragment.cpp around the
     `time_of_day_texture` upload + `interp_time_of_day` (background_common.h)
     and mirror the initial upload exactly.
3. `discard_tree_cache()` releases nothing (BufferPool chunks are keyed/pooled —
   FIX 33) but re-zeros the vector; keep the GL parity comment.
4. Failure policy: any pool !ok / upload failure → one `[dk] tfrag setup: <what>
   failed (tree i, geom g)` line, skip THAT tree, keep the rest. Never crash.

## Rules

- New files only + ONE CMake edit: add `TFragmentDk.cpp` to the deko3d_renderer
  list in game/CMakeLists.txt (grep `dk_render_target.cpp` for the list).
- The .h/.cpp must not include GL headers except shared plain ones
  (`common/custom_data/Tfrag3Data.h`, `background_common.h` is GL-dir but its
  `TfragRenderSettings`/`GoalBackgroundCameraData` (L9, L31) are pure data —
  include it, call nothing GL from it).
- No callers yet — dk_renderer stays untouched (T39 wires it).

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`74635895…`): new TU with zero callers → stripped.
Changed → explain.

## Doubts

- If `LevelData`/`loader_data` types pull GL headers in transitively, report the
  include chain and switch to forward declarations instead of guessing.
