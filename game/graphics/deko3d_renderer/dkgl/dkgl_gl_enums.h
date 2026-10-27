#pragma once
// T104b (AI-assisted): GL enum values used by the dkgl layer, as plain u32.
// Values are the stable GL ABI constants (glad/gl.h). Host tests use these too.
#include "common/common_types.h"

namespace dkgl::gl {
constexpr u32 NO_ERROR = 0;
constexpr u32 FALSE = 0, TRUE = 1;
// blend factors
constexpr u32 ZERO = 0, ONE = 1;
constexpr u32 SRC_COLOR = 0x0300, ONE_MINUS_SRC_COLOR = 0x0301;
constexpr u32 SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr u32 DST_ALPHA = 0x0304, ONE_MINUS_DST_ALPHA = 0x0305;
constexpr u32 DST_COLOR = 0x0306, ONE_MINUS_DST_COLOR = 0x0307;
constexpr u32 CONST_COLOR = 0x8001;  // T106 blend factor map (brief)
// blend equations
constexpr u32 FUNC_ADD = 0x8006, FUNC_SUBTRACT = 0x800A, FUNC_REVERSE_SUBTRACT = 0x800B;
// caps
constexpr u32 BLEND = 0x0BE2, DEPTH_TEST = 0x0B71, SCISSOR_TEST = 0x0C11;
// dkgl review (AI-assisted): GL_PRIMITIVE_RESTART is 0x8F9D (0x8558 is the NV extension
// enum), and this glad redefines it as GL_PRIMITIVE_RESTART_FIXED_INDEX (0x8D69) under some
// configurations -- accept both, or the ocean's strips join into screen-wide triangles.
constexpr u32 PRIMITIVE_RESTART = 0x8F9D, PRIMITIVE_RESTART_FIXED_INDEX = 0x8D69;
// depth funcs
constexpr u32 NEVER = 0x0200, LESS = 0x0201, EQUAL = 0x0202, LEQUAL = 0x0203;
constexpr u32 GREATER = 0x0204, NOTEQUAL = 0x0205, GEQUAL = 0x0206, ALWAYS = 0x0207;
// vertex types
constexpr u32 UNSIGNED_BYTE = 0x1401, INT = 0x1404, UNSIGNED_INT = 0x1405, FLOAT = 0x1406;
constexpr u32 UNSIGNED_SHORT = 0x1403;  // T106: draw_elements idx format (ocean uses it)
constexpr u32 INT_2_10_10_10_REV = 0x8D9F, UNSIGNED_INT_2_10_10_10_REV = 0x8368;
// primitives
constexpr u32 LINES = 0x0001, TRIANGLES = 0x0004, TRIANGLE_STRIP = 0x0005, TRIANGLE_FAN = 0x0006;
// buffer targets
constexpr u32 ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893;
// clear bits
constexpr u32 COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x0100;
// formats
constexpr u32 RGBA = 0x1908, RGBA8 = 0x8058, RGB = 0x1907;
constexpr u32 UNSIGNED_INT_8_8_8_8_REV = 0x8367, UNSIGNED_BYTE_TYPE = UNSIGNED_BYTE;
// textures
constexpr u32 TEXTURE_2D = 0x0DE1;
constexpr u32 TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800;
constexpr u32 TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803;
constexpr u32 NEAREST = 0x2600, LINEAR = 0x2601;
constexpr u32 NEAREST_MIPMAP_NEAREST = 0x2700, NEAREST_MIPMAP_LINEAR = 0x2702;
constexpr u32 LINEAR_MIPMAP_NEAREST = 0x2701, LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr u32 REPEAT = 0x2901, CLAMP_TO_EDGE = 0x812F;
// FBO
constexpr u32 FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0;
constexpr u32 FRAMEBUFFER_COMPLETE = 0x8CD5;
// shader stages / status / units (T105)
constexpr u32 VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30;
constexpr u32 COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82;
constexpr u32 TEXTURE0 = 0x84C0;
// glGetIntegerv (dkgl review)
constexpr u32 VIEWPORT = 0x0BA2, FRAMEBUFFER_BINDING = 0x8CA6;
constexpr u32 CURRENT_PROGRAM = 0x8B8D;  // DirectRenderer::flush_pending
// T113 shadow (AI-assisted): stencil cap + ops
constexpr u32 STENCIL_TEST = 0x0B90;
constexpr u32 KEEP = 0x1E00, REPLACE = 0x1E01, INCR = 0x1E02, DECR = 0x1E03;
constexpr u32 INVERT = 0x150A, INCR_WRAP = 0x8507, DECR_WRAP = 0x8508;
// T112 depth-cue (AI-assisted): read/draw FBO split + blit + buffer usage
constexpr u32 READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9;
constexpr u32 STATIC_DRAW = 0x88E4;
// TODO #5 (AI-assisted): Merc2 bone matrices (glBindBufferRange on GL_UNIFORM_BUFFER)
constexpr u32 UNIFORM_BUFFER = 0x8A11, UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34;
}  // namespace dkgl::gl