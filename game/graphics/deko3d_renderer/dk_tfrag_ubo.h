#pragma once
#include "common/math/Vector.h"  // Vector4f/Matrix4f verified 16/64 B, trivially copyable
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
