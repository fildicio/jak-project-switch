# T37 — tfrag3 UBO structs: std140 mirror + pure fill (P3.2, AI-assisted)

Track D P3.2 · Depends on: — (independent; T20 ring exists) · Inert until T38 · One session

## Goal

New host-testable header `deko3d_renderer/dk_tfrag_ubo.h`: the C++ mirrors of
the spike shaders' std140 uniform blocks, with `static_assert` offset checks, and
PURE fill functions from `GoalBackgroundCameraData` (no GL, no deko3d — the
arithmetic is ported from the GL path so dk and GL produce identical UBO bytes).

## Context

- `game/graphics/deko3d_renderer/shaders/tfrag3.vert.glsl:5-14` —
  `layout(std140, binding = 7) uniform VertParams`:
  `hvdf_offset, cam_trans` (vec4s), `pc_camera, camera` (mat4s), `fog_constant,
  fog_min, fog_max` (floats), `decal` (int).
- `shaders/tfrag3.frag.glsl:5-10` — `FragParams`: `alpha_min, alpha_max`
  (floats), `fog_color` (vec4), `gfx_hack_no_tex` (int).
- GL arithmetic source: `first_tfrag_draw_setup` + the draw-time uniform sets in
  `game/graphics/opengl_renderer/background/background_common.cpp` (grep the
  function; it computes hvdf_offset/fog from `GoalBackgroundCameraData`,
  background_common.h:9) and `TFragment.cpp:525` (`decal`) + the alpha_min/max
  double-draw at TFragment.cpp:548-551. Port the MATH, not the GL calls.
- `game/graphics/pipelines/dk_device.h:70-73` — the T20 UBO ring you will
  alloc() from in T38 (align 0x100); this task only defines the byte layout.

## std140 layout you must assert (vec3 pads to 16; mat4 = 64)

```cpp
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
};                                 // sizeof 176, alloc rounded to 0x100
struct FragParams {          // binding 7, fragment stage
  float alpha_min;                 // @0
  float alpha_max;                 // @4
  math::Vector4f fog_color;        // @8
  int32_t gfx_hack_no_tex;         // @24
};                                 // sizeof 28, alloc rounded to 0x100
static_assert(offsetof(VertParams, pc_camera) == 32);
static_assert(offsetof(VertParams, fog_constant) == 160);
static_assert(offsetof(VertParams, decal) == 172);
static_assert(offsetof(FragParams, fog_color) == 8);
static_assert(offsetof(FragParams, gfx_hack_no_tex) == 24);
```
Verify `sizeof(math::Matrix4f) == 64` and that Vector4f is 16 B trivially
copyable (common/math/Vector.h) — if not, use plain `float[16]`/`float[4]`
members instead and say so.

## Steps

1. Write the header (plain, host-compilable, `#pragma once`, namespace `dktf`).
2. Pure fill functions:
   ```cpp
   void fill_vert(VertParams* out, const GoalBackgroundCameraData& cam, bool decal);
   void fill_frag(FragParams* out, float alpha_min, float alpha_max,
                  const math::Vector4f& fog_color, bool hack_no_tex);
   ```
   `fill_vert` ports `first_tfrag_draw_setup`'s hvdf/fog math (fog_constant/
   min/max from the camera data — read the GL function and mirror term for term;
   if a term is not in `GoalBackgroundCameraData`, report where it comes from).
   `fill_frag` defaults: alpha_min/-max from the current DrawMode double-draw
   (TFragment.cpp:548-551) — default args `-10.f / 255.f` per the GL AFAIL path,
   overridden per draw in T38.
3. Host compile check is part of done-check: the header is included by nothing on
   Switch yet; it MUST compile on host (clang syntax-only, command below).

## Rules

- ONE new file: `dk_tfrag_ubo.h`. No .cpp, no CMake edit (header-only).
- No GL/deko3d includes in it. `background_common.h` include for the camera
  struct is allowed (pure data) — if it drags GL types in, forward-declare and
  take the members you need as plain floats instead; report which way you went.

## Done-check

```sh
xcrun clang++ -fsyntax-only -std=c++17 -I. \
  -include game/graphics/deko3d_renderer/dk_tfrag_ubo.h -x c++ /dev/null && echo HOST-OK
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

Paste HOST-OK + DONE-PASS + NRO md5 (`727feb1c…` — header with no TU including
it cannot change the NRO; changed → explain).

## Doubts

- If GL `first_tfrag_draw_setup` sets uniforms not present in VertParams
  (shader-version drift), list them — do not invent struct fields.
