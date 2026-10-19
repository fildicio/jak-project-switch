# T37a — dk_tfrag_ubo.h: std140 structs + asserts (P3.2, AI-assisted)

Track D P3.2 · Depends on: — (independent) · Inert until T38 · One session (~30 min)
Split from T37 (halving pass, 2026-10-18).

## Goal

Half 1 of T37. ONE new header `game/graphics/deko3d_renderer/dk_tfrag_ubo.h`:
the C++ mirrors of the tfrag3 shaders' std140 uniform blocks with
static_assert offset checks. NO fill functions (T37b). No .cpp, no CMake.

## Session rules (hard — read twice)

- Time box ~30 min. Read each listed range ONCE; re-reading = looping → STOP.
- FORBIDDEN reads: `deko3d.h`/`deko3d.hpp`, `docs/deko3d-ref/**`, session
  notes, the plan.
- Build command is exactly `scripts/ai/check-bg.sh jak1` — jak1, never jak2.
- Report goes in QWEN_REPORT.md under `## T37a`.

## Read (once, exactly these)

1. `game/graphics/deko3d_renderer/shaders/tfrag3.vert.glsl` L1-25 — the
   `VertParams` block (binding 7).
2. `game/graphics/deko3d_renderer/shaders/tfrag3.frag.glsl` L1-15 — `FragParams`.
3. `grep -n 'Matrix4f\|Vector4f' common/math/*.h | head -20` — confirm
   `sizeof(Matrix4f)==64` and Vector4f is 16 B trivially copyable (read the
   two struct definitions only).

## The exact header body (write this — it is already std140-correct)

```cpp
#pragma once
#include "common/math/Vector.h"   // adjust to the real header from read 3
#include <cstdint>
#include <cstddef>
namespace dktf {
struct VertParams {          // binding 7, vertex stage
  math::Vector4f hvdf_offset;      // @0
  math::Vector4f cam_trans;        // @16
  math::Matrix4f pc_camera;        // @32
  math::Matrix4f camera;           // @96
  float fog_constant;              // @160
  float fog_min;                   // @164
  float fog_max;                   // @168
  int32_t decal;                   // @172
};                                 // sizeof 176, ring alloc rounds to 0x100
struct FragParams {          // binding 7, fragment stage
  float alpha_min;                 // @0
  float alpha_max;                 // @4
  math::Vector4f fog_color;        // @8
  int32_t gfx_hack_no_tex;         // @24
};                                 // sizeof 28, ring alloc rounds to 0x100
static_assert(offsetof(VertParams, pc_camera) == 32);
static_assert(offsetof(VertParams, fog_constant) == 160);
static_assert(offsetof(VertParams, decal) == 172);
static_assert(offsetof(FragParams, fog_color) == 8);
static_assert(offsetof(FragParams, gfx_hack_no_tex) == 24);
}
```

If read 3 shows the math types do NOT satisfy the sizes, switch those members
to plain `float[16]` / `float[4]` (same offsets) and SAY SO in the report.

## Done-check (host check FIRST, then the build)

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_tfrag_ubo.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

Paste HOST-OK + DONE-PASS + NRO md5 `bb560fa75442dc67dd336826ac5bb154`
(a move is NOT a stop: A/B-attribute — stash only your files, rebuild, compare, restore, rebuild — and report both md5s; STOP only if the A/B does not attribute) (a header nothing includes cannot change the NRO). Changed → STOP.

## Doubts

- If the .glsl blocks show MORE members than the structs above (shader drift),
   list them — do not invent fields.

> REVIEWER FIX 2026-10-19: expected md5 rebaselined 67018cd5 → bb560fa7
> (stale pre-T35b value). Run PASSED against the real baseline.
