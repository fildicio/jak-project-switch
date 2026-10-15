#pragma once

/*!
 * @file gl_time_split.h
 *
 * deko3d migration P0.5 -- the GL/C++ time-split measurement shim. (AI-assisted)
 *
 * Question being answered (DEKO3D_MIGRATION_PLAN.md, Phase 0.5): of the Switch
 * render phase (the FIX 11 "render" bucket between receiving the DMA chain and
 * releasing it), how much is spent inside GL entry points -- i.e. inside Mesa +
 * nouveau -- and how much in the game's own C++ (matrix math, bucket walking,
 * memcpy, state caching)? The split is the go/no-go number for the migration:
 *
 *   - GL-dominated  -> porting the submit path to deko3d attacks most of the
 *                      phase; 30 fps at stock clocks is plausible.
 *   - C++-dominated -> deko3d can only shrink the smaller half; stop before
 *                      Phase 2 and spend the effort elsewhere.
 *
 * Mechanism: glad exposes every GL function as a macro over a glad_ function
 * pointer. The generated header gl_time_split_funcs.h (from
 * scripts/gen-gl-time-split.py, covering exactly the functions listed in
 * docs/deko3d/gl-inventory.json) #undefs each macro and substitutes an inline
 * wrapper of the identical signature. The wrapper is a pure passthrough unless
 * recording() is true, so with the shim off the build is behaviorally
 * identical and pays one predictable branch per call.
 *
 * Recording is bracketed around the render phase by the GL frame loop
 * (game/graphics/pipelines/opengl.cpp): begin_render_phase() after the DMA
 * wait, end_render_phase() where the FIX 11 probe stops its render timer.
 * t_recording is thread-local, so GL calls made by the loader thread (texture
 * uploads, glClientWaitSync probes) are correctly excluded.
 *
 * Kill-switch: the shim only records when
 *   sdmc:/switch/<game>/gk_glt.txt
 * exists (checked once in init()), matching the gk_* convention. The [glt]
 * report goes through switch_diag_logf, so it only prints while the R3+Minus
 * diagnostics capture is enabled -- same gating and 2 s cadence as [fps]
 * (FIX 41/FIX 40: no log traffic during normal play).
 *
 * Everything here is Switch-only; on other platforms this header is inert.
 */

#if defined(__SWITCH__)

#include <cstdint>

#include "common/versions/versions.h"

namespace glt {

//! True when the gk_glt.txt kill-switch file was found at init().
bool enabled();

//! Call once on the render thread after glad is fully initialized (including
//! the FIX 43a manual resolver pass, so the wrappers cover resolved entries
//! too -- they wrap the glad_ pointers, however they were filled).
void init(GameVersion game_version);

//! Render thread: start attributing GL calls to the render phase.
void begin_render_phase();

//! Render thread: stop attributing; fold this frame's counters into the
//! interval accumulators.
void end_render_phase();

//! Print the [glt] report (2 s cadence, diag-gated) given the FIX 11 phase
//! averages/maxima for the same interval, then reset the interval.
void report(double render_avg_ms, double render_max_ms);

//! Per-thread "currently inside the render phase" flag. Set only while the
//! shim is enabled; read by the generated wrappers on every GL call.
extern thread_local bool t_recording;
inline bool recording() { return t_recording; }

//! Monotonic timer: the ARM generic timer count (CNTVCT). ~19.2 MHz on the
//! Switch's T210, read via mrs -- no libc call, ~tens of cycles.
inline uint64_t now() {
  uint64_t v;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}

}  // namespace glt

#endif  // __SWITCH__
