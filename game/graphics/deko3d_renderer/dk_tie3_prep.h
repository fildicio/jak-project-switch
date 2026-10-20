#pragma once
// Tie3 ground-work header (T41a 1/2, AI-assisted): the verified vertex-format
// table for tfrag3::PreloadedVertex as consumed by Tie3.
//
// The header is deko3d-free (per repo rule: deko3d.h only in .cpp under
// __SWITCH__) and host-compilable: no <deko3d.h>, no <gl*.h>, no Tfrag3Data.h.
// All offsets and sizes are plain ints, encoded the same way as
// PipelineTable.h (small plain-int tables, deko3d enum NAMES in comments only,
// never as code symbols).
//
// Source of truth (cited file:line for EVERY constant below):
//   - vertex struct: common/custom_data/Tfrag3Data.h:87-111
//       struct PreloadedVertex {
//         float x, y, z;        // @ 0,  4,  8
//         u8    r, g, b, a;     // @12, 13, 14, 15
//         float s, t;           // @16, 20
//         u32   nor;            // @24               (GL_INT_2_10_10_10_REV)
//         u16   color_index;    // @28
//       };
//     static_assert(sizeof(PreloadedVertex) == 32) at Tfrag3Data.h:111.
//   - the GL bind code we mirror:
//       game/graphics/opengl_renderer/background/Tie3.cpp:133-138  loc 0 (pos)
//       game/graphics/opengl_renderer/background/Tie3.cpp:141-146  loc 1 (uv)
//       game/graphics/opengl_renderer/background/Tie3.cpp:149-153  loc 2 (color_index, int attrib)
//       game/graphics/opengl_renderer/background/Tie3.cpp:156-161  loc 3 (packed normal)
//       game/graphics/opengl_renderer/background/Tie3.cpp:164-169  loc 4 (envmap tint RGBA)
//
// VERDICT on the vertex struct:
//   Tie3 streams the EXACT same `tfrag3::PreloadedVertex` as TFragment
//   (TFragment.cpp:320-336 uses the same type and the same five attrib lines).
//   5 attrib locations, 1 single VBO (no second buffer -> no separate table),
//   32 B stride, GL_FALSE/TRUE per call site. So the table below is shared
//   with TFragmentDk (T36a/T60). The deko3d port already uses it
//   (TFragmentDk.cpp, dk_tfrag_record.cpp).
//
// Tie3-only notes (Tie3.h:Tree / Tie3.h:151-176): the wind path adds an
// INSTANCED second index buffer (`wind_indices`) and an instanced wind index
// buffer (`wind_vertex_index_buffer`). Those are INDEX buffers, not a second
// vertex buffer, so they do NOT need a new vertex table. Wind matrix
// animation is a per-instance UBO concern for T60+; not covered here.
//
// No pipeline ids, no shader list, no GPU calls (T41b covers the rest).

#include <cstdint>

namespace dktie {

// Plain-int encoding of the 5 GL formats used at Tie3.cpp:135-166.
// deko3d DkVtxAttribSize/DkVtxAttribType enum NAMES appear in comments only (not as symbols),
// following PipelineTable.h's "deko3d enum names in comments only" rule.
enum class VtxFormat : uint8_t {
  Float3,   // GL_FLOAT × 3            — DkVtxAttribSize_3x32 + DkVtxAttribType_Float
  Float2,   // GL_FLOAT × 2            — DkVtxAttribSize_2x32 + DkVtxAttribType_Float
  U16x2,    // GL_UNSIGNED_SHORT × 2   — DkVtxAttribSize_2x16 + DkVtxAttribType_Uint (glVertexAttribIPointer)
  Int2_10,  // GL_INT_2_10_10_10_REV   — DkVtxAttribSize_10_10_10_2 + DkVtxAttribType_Snorm (normalized)
  U8x4,     // GL_UNSIGNED_BYTE × 4    — DkVtxAttribSize_4x8 + DkVtxAttribType_Unorm (normalized)
};

// One vertex-attribute binding (one row per enabled location).
// Plain ints only; deko3d names go in VtxFormat comments above.
struct VtxAttrib {
  uint32_t location;      // shader attribute location (0..4)
  uint32_t offset;        // byte offset inside PreloadedVertex
  uint32_t stride;        // vertex stride in bytes (always kVertexStride = 32)
  uint8_t  components;     // number of components (1..4)
  uint8_t  format;         // VtxFormat (plain-int; one of the 5 above)
  bool     normalized;     // GL_TRUE for the packed/normalized slots
  bool     integer;        // true = glVertexAttribIPointer, false = glVertexAttribPointer
};

// Single vertex stride for every enabled attrib.
// Citation: sizeof(tfrag3::PreloadedVertex) in Tie3.cpp:137,145,152,160,168
// and the static_assert on Tfrag3Data.h:111.
constexpr uint32_t kVertexStride = 32;

// Number of attrib locations Tie3 enables (Tie3.cpp:127-131).
constexpr uint32_t kNumAttribs = 5;

// The 5 attrib bindings, in the same order Tie3.cpp:133-170 issues them.
//
// Field offsets (Tfrag3Data.h:87-110, sizeof 32):
//   x @0,  r @12, s @16, nor @24, color_index @28
//
// Tie3.cpp:133-138  loc 0  pos   vec3 float,    GL_FALSE, offset(x)         = 0
// Tie3.cpp:141-146  loc 1  uv    vec2 float,    GL_FALSE, offset(s)         = 16
// Tie3.cpp:149-153  loc 2  cidx  u16 × 2 (int), int attrib, offset(color_index) = 28
// Tie3.cpp:156-161  loc 3  nor   packed 10.10.10.2 (u32), GL_TRUE,  offset(nor) = 24
// Tie3.cpp:164-169  loc 4  tint  u8 × 4, GL_TRUE,         offset(r)        = 12
//
// Note on the GL code's inline comments ("// 1 values per vert" at loc 2 and
// "// 3 values per vert" at loc 3): those comments disagree with the actual
// argument passed to the GL call; we follow the GL ARGUMENT (2 for loc 2,
// 4 for loc 3), since deko3d reads components per the same packing the GL
// call used.
inline constexpr VtxAttrib kAttribs[kNumAttribs] = {
  // location, offset, stride, components, format,     normalized, integer
  { 0u,     0u,       kVertexStride, 3u, (uint8_t)VtxFormat::Float3,   false, false },
  { 1u,    16u,       kVertexStride, 2u, (uint8_t)VtxFormat::Float2,   false, false },
  { 2u,    28u,       kVertexStride, 2u, (uint8_t)VtxFormat::U16x2,    false, true  },
  { 3u,    24u,       kVertexStride, 4u, (uint8_t)VtxFormat::Int2_10,  true,  false },
  { 4u,    12u,       kVertexStride, 4u, (uint8_t)VtxFormat::U8x4,     true,  false },
};

// Host-side sanity: 5 bindings, all at the 5 known field offsets.
// (Reviewed separately by the reviewer; not asserted at compile time to keep
// the header dependency-free — the offsets are documented above and the
// GL-side calls are the ultimate ground truth.)

// ============================================================================
// T41b 2/2 (AI-assisted): pipeline-table ids for Tie3's draw modes + shader set.
//
// Source of truth (cited file:line for every constant below):
//   - Tie3 draw paths:
//       Tie3.cpp:133-169  attrib bind
//       Tie3.cpp:595-711  base + envmap base pass (shader picked at :602/:651)
//       Tie3.cpp:602       ETIE_BASE vs TFRAG3 on `use_envmap`
//       Tie3.cpp:714-800   envmap 2nd pass (ShaderId::ETIE at :719,:755)
//       Tie3.cpp:995-1070  wind pass (ShaderId::TIE_WIND at :1003,:1031)
//   - The 4 axes that decide which kPipelines[] entry a Tie3 draw hits:
//       background_common.cpp:144-164  depth_test + depth_op (NEVER/ALWAYS/
//          GEQUAL/GREATER, or OFF when get_zt_enable()==0)
//       background_common.cpp:169-216  blend: 7 cases (OFF, STD_ALPHA,
//          PREMULT_ADD, FIX128_ADD, FIX64_SUB, SRC_ALPHA_SUB, DST_ALPHA_ADD)
//       background_common.cpp:224-257  alpha_test (ALWAYS/GEQUAL/NEVER) +
//          alpha_fail (KEEP/FB_ONLY) -> NEVER+FB_ONLY = the z-write disable hack
//       background_common.cpp:259-263  depth_write (with the alpha-hack override
//          at :223,:248-251)
//       background_common.cpp:268-278  setup_tfrag_shader: every Tie3 draw goes
//          through this, so all 4 axes are live for Tie3.
//   - Deko3d side that maps the 4 axes to a kPipelines entry:
//       dk_state.cpp:244-262 find_for_draw_mode scans kPipelines matching the
//          4 axes. 224 entries in PipelineTable.inc:3-227 cover the closed
//          state space from common/dma/gs.h (DrawMode) -- exactly what Tie3's
//          draw.mode can produce because the modes are DMA-driven.
//   - Tie3's draw-mode source:
//       Tfrag3Data.h:196-202  struct StripDraw { DrawMode mode; }
//       Tfrag3Data.h:242-247  struct InstancedStripDraw { DrawMode mode; }
//       (both `tree.draws` and `tree.wind_draws` carry a DrawMode each.)
//
// VERDICT on pipeline-table ids:
//   Tie3 draws through setup_tfrag_shader -> setup_opengl_from_draw_mode,
//   which exercises all 4 axes (depth_test 5, depth_write 2, blend 7,
//   alpha_test 4). Tie3's draw.mode is a DMA-loaded DrawMode (Tfrag3Data.h:197,
//   :243), so any of the 224 closed-set entries can appear at runtime. The
//   set of reachable ids is therefore ALL of P001..P224 (no subset, no GAP).
//   T41a noted the vertex table is already shared with TFragment; the pipeline
//   table is too.
//
// VERDICT on shaders (task command 4: `grep -rn 'tie_wind'
//   game/graphics/opengl_renderer/`):
//   - The STRING "tie_wind" occurs once:
//       Shader.cpp:202  at(ShaderId::TIE_WIND) = {"tie_wind", version};
//     (registration only -- every shader is registered there).
//   - The ENUM ShaderId::TIE_WIND is referenced at ONE site only:
//       Tie3.cpp:1003  `auto shader_id = ShaderId::TIE_WIND;`
//       Tie3.cpp:1031  (inside render_tree_wind, the instanced wind loop)
//     No other file in opengl_renderer/ uses it (grep-confirmed). So tie_wind
//     IS required by Tie3 -- it is the shader of the instanced wind pass
//     (tree.wind_draws / wind_vertex_index_buffer), distinct from the tfrag3
//     base pass and not shared with Hfrag or Shrub.

// Tie3 needs the full 4-shader set (no tfrag3-only "pair" suffices; the
// envmap branch and the wind path are separate code paths in Tie3.cpp).
// The ids below are ShaderId ordinals from game/graphics/opengl_renderer/Shader.h:
//   TFRAG3=6      Shader.h:53
//   ETIE_BASE=30  Shader.h:77
//   ETIE=31       Shader.h:78
//   TIE_WIND=40   Shader.h:87
// Deko3d-side GLSL pairs live under
//   game/graphics/deko3d_renderer/shaders/ :
//     tfrag3.vert.glsl / tfrag3.frag.glsl       (TFRAG3)
//     tie_wind.vert.glsl / tie_wind.frag.glsl   (TIE_WIND)
//   (tfrag3 and tie_wind are built to .dksh at build time: game/CMakeLists.txt
//   _BG_SHADERS, T44. ETIE / ETIE_BASE have no deko3d .glsl yet -- flagged as a follow-up,
//   outside T41b scope; the GL side splits them into one etie.vert / etie_base
//   uniform set per Tie3.cpp:47-57.)

// --- kPipelineIds: the kPipelines[] ids Tie3's draw modes map to. -----------
// Tie3's draw.mode is a DMA-loaded DrawMode and goes through
// setup_tfrag_shader (background_common.cpp:268) -> setup_opengl_from_draw_mode
// (:107), which sets all 4 axes (depth_test/depth_op :144-164, blend :169-216,
// alpha_test :224-257, depth_write :259-263). Those 4 axes are exactly what
// find_for_draw_mode (dk_state.cpp:244) matches against the 224-entry
// kPipelines[] table (PipelineTable.inc:3-227, count asserted at :247). Since the
// closed DrawMode state space equals the 224 entries and Tie3's modes are
// DMA-driven, Tie3 can reach ANY of P001..P224: it is the full set, not a subset,
// and there is no draw mode with no table entry (no GAP). We encode the set as
// [first, count) instead of listing 224 ids (which would be noise).
//   P001 = PipelineTable.inc:4 ({P001,...}) ; P224 = PipelineTable.inc:227.
inline constexpr uint32_t kPipelineIdsFirst = 0u;    // P001 (PipelineTable.inc:4)
inline constexpr uint32_t kPipelineIdsCount = 224u;  // P001..P224
// Convenience last id (matches PipelineTable.inc:227).
inline constexpr uint32_t kPipelineIdsLast = kPipelineIdsFirst + kPipelineIdsCount - 1u;

// --- kShadersNeeded: the ShaderIds Tie3 binds (full 4-shader set). ----------
// Ordinals are the ShaderId values from Shader.h: TFRAG3=6 (:53),
// ETIE_BASE=30 (:77), ETIE=31 (:78), TIE_WIND=40 (:87).
// A tfrag3-only pair is NOT enough: Tie3.cpp:602 switches base pass between
// TFRAG3 and ETIE_BASE on use_envmap, :719/:755 run the ETIE envmap 2nd pass,
// and :1003/:1031 run the TIE_WIND instanced wind pass.
enum class Tie3Shader : uint8_t {
  TFRAG3    = 6,   // Shader.h:53  -> deko3d tfrag3.{vert,frag}.glsl
  ETIE_BASE = 30,  // Shader.h:77  -> GL etie_base uniforms (Tie3.cpp:53-57)
  ETIE      = 31,  // Shader.h:78  -> GL etie uniforms (Tie3.cpp:47-51)
  TIE_WIND  = 40,  // Shader.h:87  -> deko3d tie_wind.{vert,frag}.glsl
};

// The 4 shaders Tie3 binds, in source order (Tie3.cpp:45-57 init, :602/:651
// base, :719/:755 envmap 2nd pass, :1003/:1031 wind).
inline constexpr Tie3Shader kShadersNeeded[4] = {
  Tie3Shader::TFRAG3,    // Tie3.cpp:45, :602, :651
  Tie3Shader::ETIE_BASE, // Tie3.cpp:53-57, :602, :651
  Tie3Shader::ETIE,      // Tie3.cpp:47-51, :719, :755
  Tie3Shader::TIE_WIND,  // Tie3.cpp:1003, :1031
};
inline constexpr uint32_t kNumShadersNeeded = 4u;

// Host-side sanity (reviewer-checked, not asserted at compile time, to keep the
// header dependency-free): 4 shaders, 224 pipeline ids, both grounded above.

}  // namespace dktie
