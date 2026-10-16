#include "game/graphics/opengl_renderer/gl_time_split.h"

/*!
 * @file gl_time_split.cpp
 *
 * deko3d migration P0.5 -- runtime half of the GL time-split shim. (AI-assisted)
 * The design lives in gl_time_split.h; this file owns the accumulators and the
 * [glt] report. Everything is Switch-only.
 */

#if defined(__SWITCH__)

#include <cstdio>
#include <unistd.h>  // access(), F_OK

// kFuncCount and the wrapper table; also re-includes gl_time_split.h (guarded).
#include "game/graphics/opengl_renderer/gl_time_split_funcs.h"

#include "game/switch/run_log.h"

namespace glt {

namespace {
// set once in init(); kill-switch file found?
bool s_enabled = false;
// CNTFRQ (ticks per second of now()); read once in init()
uint64_t s_freq_hz = 1;
// interval accumulators, owned by the render thread between reports
int s_int_frames = 0;
double s_int_gl_ms = 0;
uint64_t s_int_calls = 0;
double s_frame_max_gl_ms = 0;
// per-function totals across the report interval (kFuncCount copies)
uint64_t s_int_ticks[kFuncCount] = {0};
uint64_t s_int_funccalls[kFuncCount] = {0};

// render-phase time (from the FIX 11 stats) weighted by recorded frames, so the
// window can span several 2 s [fps] intervals
double s_int_render_ms = 0;
double s_int_render_max = 0;
int s_int_reports = 0;
int s_frames_at_last_report = 0;

// The kill-switch file alone must be enough to get numbers: the first fixed
// hardware run (2026-10-17) recorded 437 s of play and printed nothing because
// these lines were behind the R3+Minus diag gate. When diag is off, print on
// our own, but only every kQuietReports intervals (5 x 2 s = 10 s), since each SD
// line costs ~6 ms on the console (FIX 40) and would skew the render phase
// it is measuring.
constexpr int kQuietReports = 5;

double ticks_to_ms(uint64_t ticks) {
  return (double)ticks * 1000.0 / (double)s_freq_hz;
}
}  // namespace

// defined here, declared in the generated header; written only while recording
uint64_t g_ticks[kFuncCount] = {0};
uint32_t g_calls[kFuncCount] = {0};
thread_local bool t_recording = false;

bool enabled() {
  return s_enabled;
}

void init(GameVersion game_version) {
  static bool s_done = false;
  if (s_done) {
    return;
  }
  s_done = true;

  uint64_t freq;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  s_freq_hz = freq ? freq : 1;

  // Kill-switch, gk_* convention: measurement only runs when this file exists.
  char path[128];
  snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_glt.txt",
           game_version_names[game_version]);
  s_enabled = access(path, F_OK) == 0;
  // One boot-time line, and only when measuring: normal play stays silent
  // (FIX 41), and a missing kill-switch file is the default state, not news.
  if (s_enabled) {
    switch_run_logf("[glt] GL time-split shim ENABLED (%s)", path);
  }
}

void begin_render_phase() {
  // t_recording is only ever set while the shim is enabled, so the generated
  // wrappers stay one-branch passthroughs otherwise.
  if (!s_enabled) {
    return;
  }
  t_recording = true;
}

void end_render_phase() {
  if (!t_recording) {
    return;
  }
  t_recording = false;

  // fold this frame's counters (single-threaded: only the recording thread
  // writes them, and recording just stopped)
  double frame_gl_ms = 0;
  uint64_t frame_calls = 0;
  for (int i = 0; i < kFuncCount; i++) {
    frame_gl_ms += ticks_to_ms(g_ticks[i]);
    frame_calls += g_calls[i];
    s_int_ticks[i] += g_ticks[i];
    s_int_funccalls[i] += g_calls[i];
    g_ticks[i] = 0;
    g_calls[i] = 0;
  }

  // A starved iteration (FIX 14: 40 ms DMA timeout, nothing rendered) makes no
  // GL calls; keep it out of the averages exactly like the FIX 11 stats keep
  // starved frames out, or it would dilute the per-frame numbers.
  if (frame_calls == 0) {
    return;
  }

  s_int_frames++;
  s_int_gl_ms += frame_gl_ms;
  s_int_calls += frame_calls;
  if (frame_gl_ms > s_frame_max_gl_ms) {
    s_frame_max_gl_ms = frame_gl_ms;
  }
}

void report(double render_avg_ms, double render_max_ms) {
  if (!s_enabled) {
    return;
  }

  // Fold this 2 s [fps] interval in. render_avg_ms is per presented frame, so
  // weight it by the frames recorded since the previous call.
  const int new_frames = s_int_frames - s_frames_at_last_report;
  s_frames_at_last_report = s_int_frames;
  s_int_render_ms += render_avg_ms * (double)new_frames;
  if (render_max_ms > s_int_render_max) {
    s_int_render_max = render_max_ms;
  }
  s_int_reports++;

  // R3+Minus diag on: every 2 s, like [fps]. Off: every kQuietReports
  // intervals, so the kill-switch file alone produces numbers.
  const int window = switch_diag_enabled() ? 1 : kQuietReports;
  if (s_int_reports < window || s_int_frames == 0) {
    return;
  }

  const double n = (double)s_int_frames;
  const double render_avg = s_int_render_ms / n;
  const double gl_avg = s_int_gl_ms / n;
  const double calls_avg = (double)s_int_calls / n;
  const double us_per_call = s_int_calls ? s_int_gl_ms * 1000.0 / (double)s_int_calls : 0.0;
  const double cpp_avg = render_avg > gl_avg ? render_avg - gl_avg : 0.0;
  const double gl_share = render_avg > 0.0 ? 100.0 * gl_avg / render_avg : 0.0;

  // Projection: what the render phase would look like if the deko3d submit
  // path drove GL-side cost down to 1/3 (conservative) or 1/5 (target) of the
  // Mesa/nouveau numbers. The divisor is an ASSUMPTION until Phase 2 measures
  // it; both bounds are printed so the decision does not hinge on one guess.
  const double proj3 = cpp_avg + gl_avg / 3.0;
  const double proj5 = cpp_avg + gl_avg / 5.0;

  // Not switch_diag_logf: gk_glt.txt is the opt-in for these lines.
  switch_run_logf(
      "[glt] %df/%.0fs | render %.2f (max %.1f) | gl %.2fms (%.1f%%) cpp %.2fms | gl max "
      "%.2f/f | %.0f calls/f %.2fus/call | proj gl/3=%.2f gl/5=%.2f | @30 %s @60 %s",
      s_int_frames, 2.0 * s_int_reports, render_avg, s_int_render_max, gl_avg, gl_share,
      cpp_avg, s_frame_max_gl_ms, calls_avg, us_per_call, proj3, proj5,
      proj5 <= 33.33 ? "ok" : "MISS",
      // 14 ms = Gate-P0's 60 fps budget (16.67 ms vblank minus the ~2.7 ms
      // the rest of the frame needs), straight from the migration plan.
      proj5 <= 14.0 ? "ok" : "MISS");

  // top offenders by accumulated GL time (names ordered by ID = inventory rank)
  int idx[kFuncCount];
  for (int i = 0; i < kFuncCount; i++) {
    idx[i] = i;
  }
  for (int i = 1; i < kFuncCount; i++) {  // insertion sort, kFuncCount is ~117
    const int v = idx[i];
    int j = i - 1;
    while (j >= 0 && s_int_ticks[idx[j]] < s_int_ticks[v]) {
      idx[j + 1] = idx[j];
      j--;
    }
    idx[j + 1] = v;
  }
  char top[512];
  top[0] = '\0';
  size_t len = 0;
  for (int r = 0; r < 8 && r < kFuncCount; r++) {
    const int i = idx[r];
    if (s_int_ticks[i] == 0) {
      break;
    }
    int w = snprintf(top + len, sizeof(top) - len, " %s%s %lldc %.2fms", r ? "| " : "",
                     kNames[i], (long long)s_int_funccalls[i], ticks_to_ms(s_int_ticks[i]));
    if (w <= 0 || (size_t)w >= sizeof(top) - len) {
      break;
    }
    len += (size_t)w;
  }
  switch_run_logf("[glt] top GL (calls total_ms):%s", top);

  // reset the window
  s_int_frames = 0;
  s_frames_at_last_report = 0;
  s_int_reports = 0;
  s_int_render_ms = 0;
  s_int_render_max = 0;
  s_int_gl_ms = 0;
  s_int_calls = 0;
  s_frame_max_gl_ms = 0;
  for (int i = 0; i < kFuncCount; i++) {
    s_int_ticks[i] = 0;
    s_int_funccalls[i] = 0;
  }
}

}  // namespace glt

#endif  // __SWITCH__
