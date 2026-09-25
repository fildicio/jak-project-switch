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
// `frames` counts tree-renders (render_tree calls). Since FIX 37 Task 0 the
// [buckets] report divides the sums by the window's *frame* count (per-frame
// sums -- dividing by a.frames read ~49x too small on the tie line);
// `frames` itself feeds the "(N tree-renders/frame)" figure.
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

// ---------------------------------------------------------------------------
// FIX 37 Task 0 (AI-assisted): time-of-day cache telemetry.
// interp_time_of_day + glTexSubImage2D used to run for every tie/tfrag/shrub
// tree every frame (~56 tree-renders, ~12 ms/frame) even though the itimes
// are usually bit-identical between frames and tree.colors never changes.
// The renderers now cache the last itimes per tree (tod_valid /
// tod_last_itimes, invalidated on texture (re)creation and discard) and skip
// both the interpolation and the upload when nothing changed. These counters
// make the win visible in the report:
//   [tod] trees R/T recomputed, X ms   -- R/T and X are per frame.
// R stays in the low single digits while time-of-day is stationary and only
// spikes on level load, debug toggles, or fast-moving time of day.
// ---------------------------------------------------------------------------

/// tree-renders that had to recompute + re-upload their tod colors
inline u32 g_tod_recomputed = 0;
/// total tie/tfrag/shrub tree-renders
inline u32 g_tod_total = 0;
/// wall time spent in the recomputes (whole tod phase of the tree-renders)
inline double g_tod_recompute_ms = 0;

// ---------------------------------------------------------------------------
// FIX 37 Task 0b (AI-assisted): per-frame recompute budget (Switch only).
//
// The cache above only pays off while the itimes are stationary. If the
// in-game clock advances every frame, every tree invalidates every frame and
// we are back to ~12 ms/frame. The day/night cycle is slow, so refreshing a
// few trees per frame (round-robin, oldest-first by construction: the trees
// that miss the budget stay invalid and are retried next frame) is visually
// indistinguishable and bounds the cost.
//
// A tree whose texture was never uploaded (tod_valid == false) is NOT subject
// to the budget - it would otherwise sample garbage.
// ---------------------------------------------------------------------------
#ifdef __SWITCH__
constexpr u32 kTodRecomputeBudget = 8;  // tree-renders per frame
#else
constexpr u32 kTodRecomputeBudget = 0;  // 0 = unlimited (desktop)
#endif

/// recomputes still allowed this frame; reset by tod_begin_frame()
inline u32 g_tod_budget_left = 0;
/// tree-renders that wanted a refresh but were deferred by the budget
inline u32 g_tod_deferred = 0;

inline void tod_begin_frame() {
  g_tod_budget_left = kTodRecomputeBudget;
}

/// true if this tree-render may refresh its tod colors this frame
inline bool tod_take_budget(bool must_refresh) {
  if (must_refresh || kTodRecomputeBudget == 0) {
    return true;  // never defer a tree that has no valid texture yet
  }
  if (g_tod_budget_left == 0) {
    g_tod_deferred++;
    return false;
  }
  g_tod_budget_left--;
  return true;
}

}  // namespace gfx
