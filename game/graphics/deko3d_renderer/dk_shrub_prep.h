#pragma once
// Shrub prep 1/2 (T42a, AI-assisted): the verified vertex-format table for
// tfrag3::ShrubGpuVertex as consumed by the GL Shrub renderer, PLUS the
// instancing/divisor layout (which vertex buffer carries per-instance attribs
// and its divisor).
//
// The header is deko3d-free (repo rule: <deko3d.h> only in .cpp under
// __SWITCH__) and host-compilable: no <deko3d.h>, no <gl*.h>, no Tfrag3Data.h.
// All offsets/sizes are plain ints, encoded the T41a style (small plain-int
// tables, deko3d enum NAMES in comments only, never as code symbols).
//
// Source of truth (cited file:line for EVERY constant below):
//   - vertex struct: common/custom_data/Tfrag3Data.h:158-167
//       struct ShrubGpuVertex {
//         float       x, y, z;     // @ 0,   4,   8
//         float       s, t;        // @12,  16
//         u32         pad0;        // @20
//         u16         color_index; // @24
//         u16         pad1;        // @26
//         u8          rgba_base[3];// @28,  29, 30
//         u8          pad2;        // @31
//       };
//     static_assert(sizeof(ShrubGpuVertex) == 32) at Tfrag3Data.h:167.
//   - the GL bind code we mirror (game/graphics/opengl_renderer/background/Shrub.cpp):
//       Shrub.cpp:117-123  loc 0  glVertexAttribPointer  (pos, 3 floats,  GL_FALSE, offset x)
//       Shrub.cpp:125-131  loc 1  glVertexAttribPointer  (uv,  2 floats,  GL_FALSE, offset s)
//       Shrub.cpp:133-139  loc 2  glVertexAttribPointer  (rgba_base, 3 u8, GL_TRUE,  offset rgba_base)
//       Shrub.cpp:141-146  loc 3  glVertexAttribIPointer (color_index, 1 u16, offset color_index)
//       Shrub.cpp:149      single_draw_index_buffer / index_buffer GL_BUFFER setup
//       Shrub.cpp:408/432  glDrawElements (GL_TRIANGLE_STRIP)         (single-draw path)
//       Shrub.cpp:413/437  glMultiDrawElements (GL_TRIANGLE_STRIP)    (batched path)
//
// VERDICT (divisor / instancing):
//   Shrub's GL renderer is NON-instanced.  Command 1 (grep for
//   glVertexAttribPointer | glVertexAttribIPointer | glVertexAttribDivisor |
//   glDrawElementsInstanced | glMultiDrawElementsInstanced in Shrub.cpp)
//   returns ONLY the four glVertexAttrib* calls above -- NO
//   glVertexAttribDivisor, NO gl*Instanced, NO indirect-draw call.  The only
//   "instance"-ish data is Shrub.h:44 `instance_info` (a
//   std::vector<tfrag3::TieWindInstance>*) and Shrub.h:52 `perf.wind_draws`,
//   but `grep wind|Instance|Divisor Shrub.cpp` shows `wind_draws` is only ever
//   set to 0 (Shrub.cpp:283) and `instance_info` is never read by any draw.
//   So there is NO per-instance vertex buffer and NO non-zero divisor in Shrub.
//   (NOTE to reviewer: the task preamble says "Shrub is the first instanced
//    renderer" -- that does NOT match the GL code; Shrub is non-instanced.
//    The real instanced wind path lives in Tie3/Tie, see T41a + TieTree in
//    Tfrag3Data.h:365-380. Flagged as the primary doubt in QWEN_REPORT, not
//    invented into this header.)
//
// --- kAttribs: the 4 attributes Shrub streams, all on ONE VBO (index 0). ---
// Single vertex stride for every attrib == sizeof(ShrubGpuVertex) == 32.
#include <cstdint>

namespace dkshrub {

// Single vertex stride, in bytes (sizeof(tfrag3::ShrubGpuVertex),
// Tfrag3Data.h:167 static_assert == 32).
inline constexpr uint32_t kVertexStride = 32;
// Plain-int encoding of the 4 GL formats used at Shrub.cpp:117-145.
// deko3d DkVtxAttribSize/DkVtxAttribType enum NAMES appear in comments only
// (not as symbols), following the T41a / PipelineTable.h rule.
enum class VtxFormat : uint8_t {
  Float3,  // GL_FLOAT x 3, GL_FALSE      - DkVtxAttribSize_3x32  + DkVtxAttribType_Float
  Float2,  // GL_FLOAT x 2, GL_FALSE      - DkVtxAttribSize_2x32  + DkVtxAttribType_Float
  U8x3,    // GL_UNSIGNED_BYTE x 3, GL_TRUE
              // (normalized)               - DkVtxAttribSize_3x8   + DkVtxAttribType_Unorm
  U16x1,   // GL_UNSIGNED_SHORT x 1 (vertexAttribIPointer, integer)
              //                        - DkVtxAttribSize_1x16    + DkVtxAttribType_Uint
};

// One vertex-attribute binding (one row per enabled location).
// Plain ints only; deko3d names go in VtxFormat comments above.
struct VtxAttrib {
  uint32_t location;     // shader attribute location (0..3)
  uint32_t offset;       // byte offset inside ShrubGpuVertex
  uint32_t stride;       // vertex stride in bytes (always kVertexStride = 32)
  uint8_t  components;    // number of components (1..4)
  uint8_t  format;        // VtxFormat (plain-int; one of the 4 above)
  bool     normalized;    // GL_TRUE for the rgba_base slot (loc 2)
  bool     integer;       // true = glVertexAttribIPointer, false = glVertexAttribPointer
};

// The 4 attributes Shrub streams (Shrub.cpp:117,125,133,141), in GL location
// order. All at divisor 0, all on VBO index 0 (the single streamed buffer).
inline constexpr VtxAttrib kAttribs[4] = {
  // loc   off  stride comp fmt      norm int   GL call (Shrub.cpp)
  {   0,     0,    32,   3,  (uint8_t)VtxFormat::Float3,  false, false },  // Shrub.cpp:117 x
  {   1,    12,    32,   2,  (uint8_t)VtxFormat::Float2,  false, false },  // Shrub.cpp:125 s
  {   2,    28,    32,   3,  (uint8_t)VtxFormat::U8x3,   true,  false },  // Shrub.cpp:133 rgba_base
  {   3,    24,    32,   1,  (uint8_t)VtxFormat::U16x1,  false, true  },  // Shrub.cpp:141 color_index
};
inline constexpr uint32_t kNumAttribs = 4;

// --- kInstanceLayout: the per-instance (divisor) descriptor for Shrub. -------
//
// deko3d models instancing PER BUFFER, not per attrib: a DkVtxBufferState
// (deko3d.h:1111-1115) carries {stride, divisor}; the task's pasted Facts
// confirm divisor is "per-BUFFER, not per-attrib".  An instance draw is
// dkCmdBufDrawIndexed (deko3d.h:1308) with instanceCount > 1.
//
// VERDICT: Shrub has NO per-instance stream.  Command 1 found no
// glVertexAttribDivisor / gl*Instanced call in Shrub.cpp.  There is exactly
// ONE vertex buffer (index 0) carrying ALL 4 attribs at divisor 0
// (per-vertex).  So the divisor layout is the trivial "no instancing" case,
// NOT an invented one.  We record it explicitly rather than leaving it implicit:
struct InstanceLayout {
  uint32_t vbo_index;            // index of the buffer that WOULD carry per-instance
                                 // data; 0 == "no separate per-instance buffer".
  uint32_t divisor;              // DkVtxBufferState.divisor value; 0 == per-vertex
                                 // (the only value that appears in Shrub's GL code).
  uint32_t per_instance_attribs; // count of attribs that would carry divisor > 0;
                                 // 0 here = every attrib is per-vertex.
};

// Shrub is non-instanced: the single VBO (index 0) carries all attribs at
// divisor 0.  Rationale cites Shrub.cpp:408/413/432/437 (plain glDrawElements /
// glMultiDrawElements) -- no divisor, no Instanced, no Indirect (command 1).
inline constexpr InstanceLayout kInstanceLayout{0u, 0u, 0u};

// Index-buffer / primitive note for the deko3d port (Step 2 of the task):
//   - Shrub draws an INDEX buffer (Shrub.cpp:149-153 binds an index buffer;
//     the index data is tree.indices / single_draw_index_buffer; u32).
//     deko3d: bind with dkCmdBufBindIdxBuffer (deko3d.h:1277) using
//     DkIdxFormat_Uint32 (deko3d.h:1140), and draw DkPrimitive_TriangleStrip
//     (deko3d.h:1124) -- the GL primitive is GL_TRIANGLE_STRIP throughout.
//   - There are TWO draw paths in Shrub.cpp (Step 3, "merged / non-merged"):
//       (a) singledraw path  : glDrawElements  (Shrub.cpp:408, 432), used when
//           render_state->no_multidraw.  Each ShrubDraw is its own draw call.
//       (b) multidraw path   : glMultiDrawElements (Shrub.cpp:413, 437), the
//           DEFAULT.  Many draws are batched into ONE call via a count buffer
//           (m_cache.multidraw_count_buffer) + index-offset buffer
//           (m_cache.multidraw_index_offset_buffer).
//     deko3d mapping:
//       (a) singledraw  -> dkCmdBufDrawIndexed (deko3d.h:1308), instanceCount = 1.
//       (b) multidraw   -> there is NO single deko3d call for glMultiDrawElements
//           (no per-draw count/offset array).  The deko3d port can EITHER
//           (b1) loop dkCmdBufDrawIndexed once per ShrubDraw (instanceCount = 1),
//           OR (b2) build a DkDrawIndexedIndirectData stream and call
//           dkCmdBufDrawIndexedIndirect (deko3d.h:1309, data type
//           DkDrawIndexedIndirectData at deko3d.h:1151) -- but that requires the
//           index-offset stream to be re-packed on the CPU.  The CHEAPER
//           port-side choice is (b2) ONLY if a multi-draw indirect buffer is
//           cheap to build; otherwise (a)/(b1) is simplest.  RECOMMEND (a):
//           it keeps one draw call per ShrubDraw, matches the no_multidraw GL
//           path already exercised, and needs no extra GPU buffer.  This is a
//           reviewer design call -- flagged in QWEN_REPORT.
//   - Note: the instanced wind path (InstancedStripDraw / TieWindInstance,
//     Tfrag3Data.h:240-262 / 365-380) belongs to Tie3, not Shrub; do not
//     pull it into Shrub's port.  Shrub's `instance_info` field is dead.
//
// ============================================================================
// T42b (P3.3): kPipelineIds (alpha-blended foliage) + kShadersNeeded.
//
// HOW Shrub selects a pipeline:
//   Each ShrubDraw (Tfrag3Data.h:228-240) carries a DMA-loaded DrawMode
//   `mode` (Tfrag3Data.h:229), assigned on load at Shrub.cpp:101
//   (m_trees[..].draws = &tree.static_draws).  render_tree (Shrub.cpp:374-417)
//   runs setup_tfrag_shader(render_state, draw.mode, ShaderId::SHRUB)
//   (Shrub.cpp:399), which dispatches to background_common.cpp:268
//   setup_tfrag_shader -> setup_opengl_from_draw_mode (background_common.cpp:107).
//   That function programs the SAME 4 axes find_for_draw_mode matches against
//   the 224-entry kPipelines[] table (PipelineTable.inc:3-227; count asserted at
//   PipelineTable.inc:247 static_assert == 224): depth-test/op (144-164),
//   depth-write (259-263), alpha-test (224-257) and BLEND (168-216).
//
// "VERIFY which" -- the alpha-blend modes that ARE transparent foliage:
//   The blend is chosen from DrawMode::get_alpha_blend() (common/dma/gs.h:450)
//   over DrawMode::AlphaBlend (common/dma/gs.h:419-426), inside the
//   `if (mode.get_ab_enable() && alpha_blend != DISABLED)` guard
//   (background_common.cpp:169).  The two modes that are PLAIN alpha blend
//   (no fixed constant, no reverse-subtract) are the foliage/leaf modes:
//
//     SRC_DST_SRC_DST = 1  (gs.h:421)
//        -> glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
//                               GL_ONE, GL_ZERO)            (bg_common.cpp:176-179)
//       == dkpipe::Blend::StdAlpha  (PipelineTable.h:29: "GL_FUNC_ADD;
//           src=SrcAlpha dst=OneMinusSrcAlpha srcA=One dstA=Zero")
//
//     SRC_0_SRC_DST   = 2  (gs.h:422)
//        -> glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO)
//                                                               (bg_common.cpp:180-183)
//       == dkpipe::Blend::PremulAdd (PipelineTable.h:30: "GL_FUNC_ADD;
//           src=SrcAlpha dst=One srcA=One dstA=Zero")
//
//   The remaining AlphaBlend values are NOT foliage alpha (cited for the
//   "verify which" decision):
//     SRC_0_FIX_DST   = 3 -> gl(ONE,ONE) Fix128Add        (bg_common.cpp:184-187)
//     SRC_DST_FIX_DST = 4 -> gl(CONST,CONST) Fix64Sub     (bg_common.cpp:188-195)
//     SRC_SRC_SRC_SRC = 6 -> should_enable_blend=false     (bg_common.cpp:172-175)
//     (the SrcAlphaSub / DstAlphaAdd reverse/additive modes, PipelineTable.h:33-34,
//     are specular/highlight effects, never transparent foliage.)
//
//   => the alpha-blended-foliage pipeline set is exactly StdAlpha U PremulAdd
//      = 32 + 32 = 64 entries of kPipelines[].  This is a SUBSET, NOT the full
//      224 (contrast T41b/Tie3, which is the full DMA-driven set).  We list the
//      64 foliage ids explicitly rather than as [first,count).
//
// FULL-reachable note (T41b precedent, reported not force-fitted):
//   Like Tie3, Shrub's mode is DMA-driven, so in principle ANY of P001..P224 is
//   reachable (kPipelineIdsFirst=0 / kPipelineIdsCount=224, see T41b).  But the
//   FOLIAGE-relevant transparent-blend subset the task names is StdAlpha+PremulAdd
//   (64 ids below).  Both facts are recorded so the reviewer can decide; we do NOT
//   force-fit, per the Doubts section.
//
// GAP check (Step 4 "report any draw mode with no table entry"):
//   kPipelines[] has EXACTLY 224 entries (PipelineTable.inc:247 static_assert ==
//   224) and PipelineTable.h:51-57 covers every (depth_test,depth_op,depth_write,
//   blend) combination setup_opengl_from_draw_mode can emit.  Therefore there is NO
//   draw mode with no table entry -- no GAP, exactly as T41b concluded.
//
// PipelineDesc field order for the line citations below (PipelineTable.h:51-57):
//   { id, depth_test, depth_op, depth_write, blend }.
// Index N below is the kPipelines[] position, == (numeric id - 1), since
// P001 is index 0 (PipelineTable.inc:4).
//
// --- kPipelineIdsStdAlpha: the 32 StdAlpha foliage ids (GL_SRC_DST_SRC_DST). ------
enum class ShrubBlend : uint8_t {
  StdAlpha,  // SRC_DST_SRC_DST=1 (gs.h) -> bg_common.cpp:176-179 -> PipelineTable.h:29
  PremulAdd,  // SRC_0_SRC_DST=2   (gs.h) -> bg_common.cpp:180-183 -> PipelineTable.h:30
};

inline constexpr uint32_t kPipelineIdsStdAlpha[32] = {
  4u, 5u, 6u, 7u,          // P005-P008  (inc l8-l11)   Compare::Always,   dw=false
  31u, 32u, 33u,           // P032-P034  (inc l35-l37)  Compare::Never,    dw=true
  53u, 54u, 55u, 56u,      // P054-P057  (inc l57-l60)  Compare::Never,    dw=false
  80u, 81u, 82u,           // P081-P083  (inc l84-l86)  Compare::Always,   dw=true
  102u, 103u, 104u, 105u,  // P103-P106  (inc l106-l109)Compare::Always,   dw=false
  129u, 130u, 131u,        // P130-P132  (inc l133-l135)Compare::Gequal,   dw=true
  151u, 152u, 153u, 154u,  // P152-P155  (inc l155-l158)Compare::Gequal,   dw=false
  178u, 179u, 180u,        // P179-P181  (inc l182-l184)Compare::Greater,  dw=true
  200u, 201u, 202u, 203u,  // P201-P204  (inc l204-l207)Compare::Greater,  dw=false
};
inline constexpr uint32_t kNumPipelineIdsStdAlpha = 32u;

// --- kPipelineIdsPremulAdd: the 32 PremulAdd foliage ids (GL_SRC_0_SRC_DST) -------
inline constexpr uint32_t kPipelineIdsPremulAdd[32] = {
  8u, 9u, 10u, 11u,        // P009-P012  (inc l12-l15)   Compare::Always,   dw=false
  34u, 35u, 36u,           // P035-P037  (inc l38-l40)   Compare::Never,    dw=true
  57u, 58u, 59u, 60u,      // P058-P061  (inc l61-l64)   Compare::Never,    dw=false
  83u, 84u, 85u,           // P084-P086  (inc l87-l89)   Compare::Always,   dw=true
  106u, 107u, 108u, 109u,  // P107-P110  (inc l110-l113) Compare::Always,   dw=false
  132u, 133u, 134u,        // P133-P135  (inc l136-l138) Compare::Gequal,   dw=true
  155u, 156u, 157u, 158u,  // P156-P159  (inc l159-l162) Compare::Gequal,   dw=false
  181u, 182u, 183u,        // P182-P184  (inc l185-l187) Compare::Greater,  dw=true
  204u, 205u, 206u, 207u,  // P205-P208  (inc l208-l211) Compare::Greater,  dw=false
};
inline constexpr uint32_t kNumPipelineIdsPremulAdd = 32u;

// Combined foliage count (task's "kPipelineIds"): StdAlpha U PremulAdd = 64.
inline constexpr uint32_t kPipelineIdsFoliageCount =
    kNumPipelineIdsStdAlpha + kNumPipelineIdsPremulAdd;  // 64

// --- kShadersNeeded: the ShaderId(s) Shrub binds ----------------------------------
// Shrub binds EXACTLY ONE shader, ShaderId::SHRUB (Shader.h:64, ordinal 17):
//   - init_shaders(ShaderId::SHRUB) at Shrub.cpp:15 ("decal" uniform).
//   - the draw: setup_tfrag_shader(render_state, draw.mode, ShaderId::SHRUB)
//     at Shrub.cpp:399, and the AFAIL_NO_DEPTH_WRITE 2nd draw at Shrub.cpp:425-427
//     also uses ShaderId::SHRUB (alpha_min/alpha_max uniforms).
//   No second shader and no envmap/wind 2nd pass (those are Tie3's, per T41b).
// deko3d side: shrub.{vert,frag}.glsl in deko3d_renderer/shaders/ are already compiled to
// .dksh at build time (game/CMakeLists.txt _BG_SHADERS, T44). Reviewer fix 2026-10-20.
enum class ShrubShader : uint8_t {
  SHRUB = 17,  // Shader.h:64 -> deko3d shrub.{vert,frag}.glsl (built by _BG_SHADERS)
};

inline constexpr ShrubShader kShadersNeeded[1] = {ShrubShader::SHRUB};
inline constexpr uint32_t kNumShadersNeeded = 1u;

// Host-side sanity (no compile assert; the reviewer re-verifies, not a build gate):
// 224 pipelines total, 32 StdAlpha + 32 PremulAdd foliage ids (64), 1 shader (SHRUB).
// All values grounded in the file:line citations above.

}  // namespace dkshrub

