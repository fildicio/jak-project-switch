#pragma once

#include "common/common_types.h"

/*!
 * FIX 35 (AI-assisted): cheap per-frame draw-call / submitted-index counters.
 * (AI-assisted)
 *
 * The Switch port is draw-call / driver bound, not fill-rate bound (proven on
 * hardware: lowering the resolution changed nothing - see
 * SWITCH_FIX35_AGENT_BRIEF.md). Without per-frame draw counts it is impossible
 * to tell "expensive pixels" from "too many draws", so every glDraw* call site
 * reports itself here and the [buckets] report in OpenGLRenderer.cpp prints
 * per-bucket and per-frame totals every 2 seconds.
 *
 * `indices` is the number of submitted indices (the `count` argument of
 * glDrawElements/glDrawArrays) - for GL_TRIANGLES that is exactly 3x the
 * triangles, for strips/fans it is a very close upper bound. Good enough to
 * rank buckets; cheaper than computing primitive counts per mode.
 *
 * Single-threaded by construction: only the render thread issues draws, so
 * plain inline globals are fine. Cost per draw call: two integer increments.
 */
namespace gfx {

/// number of glDraw* calls since the last frame reset (dispatch_buckets)
inline u32 g_draw_calls = 0;
/// number of indices submitted by those calls
inline u32 g_draw_indices = 0;

/// call immediately before each glDrawElements / glDrawArrays, with its count
inline void count_draw(u32 indices) {
  g_draw_calls++;
  g_draw_indices += indices;
}

/// call immediately before each glMultiDrawElements: one API call submitting
/// `n` draw ranges whose index counts live in `counts[0..n)` (GLsizei/int,
/// matching the buffers the renderers keep). FIX 36: the multidraw path was
/// never counted, which made the TIE buckets read "draws 0.0/frame" while
/// burning 8.7 ms - the report was lying.
inline void count_multidraw(const int* counts, u32 n) {
  g_draw_calls += n;
  u64 total = 0;
  for (u32 i = 0; i < n; i++) {
    total += (u64)counts[i];
  }
  g_draw_indices += (u32)total;
}

// ---------------------------------------------------------------------------
// FIX 36 Task 2 (AI-assisted): sub-phase timers for the background renderers.
//
// The FIX 35 report showed the three TIE buckets burning 8.7 ms/frame while
// the [buckets] counters read "draws 0.0" -- time with no attributed draws.
// Part of that was a counting gap (glMultiDrawElements was never counted,
// fixed alongside this), the rest is CPU work inside the renderers. These
// accumulators break a background renderer's frame into its actual phases:
//   tod      - time-of-day color interp + the 1xN texture re-upload
//   protovis - proto-visibility mask update from GOAL memory
//   cull     - cull_check_all_slow over the vis tree
//   idx      - vis-string -> draw list / multidraw table building
//   upload   - index buffer upload (single-draw path)
//   draw     - the draw submission loop itself
// `frames` counts tree-renders (render_tree calls), so the averages printed
// by the [buckets] report are per tree-render, not per frame -- the
// *proportions* are what matter (SWITCH_FIX36_AGENT_BRIEF.md §3).
// Single-threaded by construction, same as the draw counters.
// ---------------------------------------------------------------------------
enum class BgRenderer : int { TIE = 0, TFRAG = 1, SHRUB = 2, COUNT = 3 };

struct BgSubphaseAcc {
  double tod_ms = 0;
  double protovis_ms = 0;
  double cull_ms = 0;
  double idx_ms = 0;
  double upload_ms = 0;
  double draw_ms = 0;
  u32 frames = 0;
};

inline BgSubphaseAcc g_bg_subphase[(int)BgRenderer::COUNT];

inline void bg_subphase_add(BgRenderer r,
                            double tod,
                            double protovis,
                            double cull,
                            double idx,
                            double upload,
                            double draw) {
  auto& a = g_bg_subphase[(int)r];
  a.tod_ms += tod;
  a.protovis_ms += protovis;
  a.cull_ms += cull;
  a.idx_ms += idx;
  a.upload_ms += upload;
  a.draw_ms += draw;
  a.frames++;
}

}  // namespace gfx
