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

}  // namespace gfx
