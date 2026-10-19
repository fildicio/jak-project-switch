# T36b — TFragmentDk time-of-day texture + discard (P3.2, AI-assisted)

Track D P3.2 · Depends on: T36a ✅ · Inert until T39 · One session (~20 min)
**v2 2026-10-19 (model-fit rewrite):** three mechanical edits to the T36a
files, all content given verbatim. No design, no reads outside this file.

## Goal

Half 2 of T36. Upload ONE zeroed time-of-day palette texture per tree in
`setup_for_level` (so T38 has a valid texture on frame 1) and note the discard
side. Per-frame re-interpolation is T38's job, not yours.

## Session rules (hard — read twice)

- Time box ~20 min.
- FORBIDDEN reads: everything except this file, QWEN_REPORT.md (append-only),
  and `game/graphics/deko3d_renderer/TFragmentDk.{h,cpp}` (which T36a created).
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T36b`.

## Facts (verified by the reviewer — do not re-derive)

- `dktexgpu::upload_rgba8(w, h, src)` returns a texture id ≥ 2 on success,
  0 on failure — and on failure it has ALREADY logged (FIX 33): do not log
  again, just leave `tod_tex_id = -1`.
- GL allocates the palette texture as `8192 x 1` RGBA8 uninitialized at level
  load (TIME_OF_DAY_COLOR_COUNT, TFragment.h:176) and fills it per frame.
  The dk side mirrors that: upload 8192x1 of zeros now; T38 uploads real data.
- Zeroing 32 KB: a static zero-initialized array is fine (BSS, not heap).

## Edit 1 — `TFragmentDk.cpp` include list

Add DIRECTLY BELOW the `#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"`
line, same style:

```cpp
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
```

## Edit 2 — `TFragmentDk.cpp` upload block

Find these two consecutive lines inside `setup_for_level`:

```cpp
      cache.draws = &tree.draws;
      cache.vis = &tree.bvh;
```

Insert DIRECTLY ABOVE them, same indent:

```cpp
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
```

## Edit 3 — `TFragmentDk.cpp` discard comment

In `discard_tree_cache()`, directly BELOW the `for` loop closing brace `  }`,
add (same indent as the `for`):

```cpp
  // T36b note: the time-of-day texture ids die with the cache (dktexgpu slots
  // are monotonic; T38 owns per-frame invalidation, not this function).
```

## Edit 4 — build + A/B

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS expected: NRO md5 `bb560fa75442dc67dd336826ac5bb154` (still zero
callers → stripped; a move is NOT a stop: A/B-attribute — stash only
`TFragmentDk.cpp`, rebuild, compare, restore, rebuild — report both md5s; STOP
only if the A/B does not attribute).

## Report

`## T36b` in QWEN_REPORT.md: the 3 edits, build result, both md5s, any compile
error (quote exact error + one-line fix).

## Doubts

There are none. If it does not compile, quote the exact first error and stop.

> REVIEWER FIX 2026-10-19: the expected md5 above was rebaselined from
> 67018cd5 (stale, pre-T35b) to bb560fa7 (the T35b-v2 baseline) — the run
> PASSED against the real value. Task done; history note only.
