#pragma once

/*!
 * @file run_log.h
 * FIX 7: whole-session SD-card trace for the Switch port.
 *
 * boot_log.h (gk_boot_log.txt) latches itself off at "boot complete" because most of
 * its call sites are per-frame renderer hooks. But the post-FIX6 intro deaths are
 * silent: the process exits cleanly ~15-30s into the intro (Sony logo shown, no
 * title screen), which leaves no Atmosphère creport at all -- only the post-mortem
 * 0x1159 HID teardown screen. Everything after "boot complete" was a blind zone.
 *
 * run_log (gk_run_log.txt) stays armed for the whole session. Discipline:
 *   - events and a 2s kernel heartbeat only, never per-frame;
 *   - one fd opened lazily, one mutex, ordered against other fsdev users via the
 *     shared SWITCH_FS_LOCK() (newlib's fsdev layer is not thread-safe);
 *   - no-op on every other platform.
 */

#if defined(__SWITCH__)

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common/util/FsLock.h"
#include "game/switch/log_paths.h"

/*!
 * FIX 7s -- live network log. Defined in game/switch/platform.cpp.
 *
 * Every hardware iteration so far has cost a full SD-card round trip (build, copy, eject,
 * run, re-insert, read), which is why this investigation has burned so many runs. This
 * streams the same lines to a TCP listener on the development machine instead, so a test
 * is "press A and watch". It is also a strictly better instrument: it does not touch fsdev,
 * so it survives the exact conditions that can silence the SD log.
 *
 * No-op when the connection was not established, so the SD log remains the fallback.
 */
void switch_net_log_write(const char* data, int len);

/*!
 * FIX 7g -- dispatch breadcrumbs.
 *
 * 7d/7e/7f all died ~0.5s into the GOAL dispatch loop, i.e. within ~30 iterations. Logging
 * every iteration would mean ~180 fsync'd writes/second on the kernel thread -- precisely
 * the fsdev write storm that killed the 7c run. Instead the kernel thread only stores a
 * stage code (one relaxed atomic store, no I/O), and the gfx thread -- the writer that has
 * survived every run so far -- prints it with its existing 250ms heartbeat. Cost on the
 * kernel thread: a few stores per iteration. Resolution at death: 250ms.
 */
enum SwitchGoalStage : unsigned int {
  SWITCH_STAGE_NOT_STARTED = 0,
  SWITCH_STAGE_LOOP_TOP = 1,
  SWITCH_STAGE_LISTENER_WAIT = 2,
  SWITCH_STAGE_LISTENER_PROCESS = 3,
  SWITCH_STAGE_GOAL_CALL = 4,   // inside call_goal_on_stack -- GOAL code is running
  SWITCH_STAGE_GOAL_RETURNED = 5,
  SWITCH_STAGE_CLEAR_PENDING = 6,
  SWITCH_STAGE_LOOP_SLEEP = 7,
};

inline std::atomic<unsigned int> g_switch_goal_stage{SWITCH_STAGE_NOT_STARTED};
inline std::atomic<unsigned long long> g_switch_goal_iter{0};

inline void switch_goal_stage(unsigned int stage) {
  g_switch_goal_stage.store(stage, std::memory_order_relaxed);
}

inline void switch_goal_tick() {
  g_switch_goal_iter.fetch_add(1, std::memory_order_relaxed);
}

/*!
 * FIX 7j -- audio-thread probe window.
 *
 * The 7i run proved the VAG file open and the audio device init both succeed; death comes
 * ~130ms after the stream is armed, i.e. around the first time 989snd actually mixes the
 * streamed voice. The IOP thread arms a small counter when it keys the voice on, and the
 * audio callback logs only that many callbacks -- enough to see whether the audio thread
 * survives the first mixes, without turning the 48kHz callback into a write storm.
 */
inline std::atomic<int> g_switch_audio_probe{0};

inline void switch_audio_probe_arm(int n) {
  g_switch_audio_probe.store(n, std::memory_order_relaxed);
}

inline bool switch_audio_probe_take() {
  int v = g_switch_audio_probe.load(std::memory_order_relaxed);
  if (v <= 0) {
    return false;
  }
  g_switch_audio_probe.store(v - 1, std::memory_order_relaxed);
  return true;
}

/*!
 * FIX 8b -- runtime toggle for the periodic diagnostics.
 *
 * The heartbeats ([gfx] alive + its [chan] mirror) and the [vag]/[snd]/[MC] breadcrumbs
 * are invaluable during a debugging session and pure noise (plus a little SD/net
 * traffic) during normal play. Hold L3 + R3 + Minus together to flip this; the combo is
 * detected on the render thread, which owns the SDL event pump. One-shot forensic lines
 * (session start/exit, [disp], [net], crash paths) always log regardless of this flag.
 * Defaults to enabled so a fresh boot is always measurable.
 */
inline std::atomic<bool> g_switch_diag_enabled{true};

inline bool switch_diag_enabled() {
  return g_switch_diag_enabled.load(std::memory_order_relaxed);
}

inline void switch_set_diag_enabled(bool enabled) {
  g_switch_diag_enabled.store(enabled, std::memory_order_relaxed);
}

#define switch_diag_logf(...)      \
  do {                             \
    if (switch_diag_enabled()) {   \
      switch_run_logf(__VA_ARGS__); \
    }                              \
  } while (0)

inline void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void switch_run_logf(const char* fmt, ...) {
  static std::mutex s_mtx;
  static int s_fd = -1;  // opened on first use, kept for the whole session
  static const std::chrono::steady_clock::time_point s_t0 = std::chrono::steady_clock::now();

  // FIX 7l -- LOCK ORDER. This used to take s_mtx first and SWITCH_FS_LOCK() second, while
  // callers such as sceOpen() (game/sce/sif_ee.cpp) hold SWITCH_FS_LOCK() across their whole
  // body and then log -- i.e. the exact opposite order. That is an AB-BA deadlock:
  //
  //   gfx/ISO thread : switch_run_logf -> holds s_mtx -> waits for the fs lock
  //   EE thread      : sceOpen         -> holds fs lock -> waits for s_mtx
  //
  // Both threads stop forever, and so does every other thread that later logs or touches
  // the card. That is the intro "death": no CPU fault, no creport, no exit path, the last
  // frame frozen on screen for ~5s until the system throws its own fatal. It only fired
  // once the first VAG stream started, because that is the first moment several threads log
  // and read the SD at full rate.
  //
  // The global order is now uniformly "filesystem lock, then log mutex", matching the rule
  // log.cpp already documents. SWITCH_FS_LOCK() is recursive, so a caller that already owns
  // it simply re-enters.
  // FIX 7m -- the log must never be a victim of the freeze it is supposed to diagnose.
  //
  // 7l fixed the genuine AB-BA inversion here (s_mtx before the fs lock, while sceOpen holds
  // the fs lock and then logs), but the intro still froze -- with every thread, including the
  // audio callback, stopping at the same instant. That is someone wedging the filesystem lock
  // itself. So instead of blocking on it forever, wait half a second; if that fails, write the
  // line anyway and name the owner. A freeze then becomes a log line instead of silence.
  const bool got_fs =
      switch_fs_mutex().try_lock_for(std::chrono::milliseconds(500));  // recursive: re-entry ok
  std::lock_guard<std::mutex> lk(s_mtx);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - s_t0)
                      .count();
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "[%lld.%03lld] ", (long long)ms / 1000,
                   (long long)ms % 1000);
  if (!got_fs) {
    const char* owner = switch_fs_owner_label().load(std::memory_order_relaxed);
    const long long since = switch_fs_owner_since_ms().load(std::memory_order_relaxed);
    n += snprintf(buf + n, sizeof(buf) - n - 1, "[LOCKBUSY owner=%s held_for=%lldms] ",
                  owner ? owner : "(null)", switch_fs_now_ms() - since);
  }
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) {
    if (got_fs) {
      switch_fs_mutex().unlock();
    }
    return;
  }
  n += m;
  if (n > (int)sizeof(buf) - 2) {
    n = (int)sizeof(buf) - 2;
  }
  buf[n++] = '\n';
  // FIX 7s: the network channel goes FIRST and outside the fd path. It shares nothing with
  // fsdev -- no fs lock, no SD card, no filesystem state -- so it keeps reporting even when
  // the SD logging is wedged, which is the failure mode that would invalidate every
  // "the trap never fired" conclusion drawn from gk_run_log.txt so far.
  switch_net_log_write(buf, n);
  if (s_fd < 0) {
    s_fd = open(SWITCH_LOG_PATH("gk_run_log.txt"), O_WRONLY | O_CREAT | O_APPEND, 0644);
  }
  if (s_fd >= 0) {
    // FIX 7d: a write() that starts failing (fd smashed by an fsdev race, as reconstructed
    // from the 7c post-mortem) used to fail silently forever -- the log went permanently
    // dark while the probes kept "running". Drop the fd on failure so the next line lazily
    // re-opens the file instead.
    if (write(s_fd, buf, n) < 0) {
      close(s_fd);
      s_fd = -1;
    } else {
      // FIX 7w -- frame-rate cleanup. This used to fsync() every single line. An fsync is a
      // synchronous flush all the way to the SD card, and it happens while holding the
      // global filesystem lock, so every log line stalled the ISO/streaming threads too.
      // That was a defensible price while the bug under investigation was a hard process
      // exit (an unflushed line is a lost clue), but the intro crash is fixed and the
      // network log -- which is written above, before this fd path, and is unaffected by
      // fsdev -- is now the primary channel. So flush on a timer instead: a crash loses at
      // most one second of the SD log, and the common path is a plain buffered write.
      static long long s_last_sync_ms = 0;
      if (ms - s_last_sync_ms >= 1000) {
        s_last_sync_ms = ms;
        fsync(s_fd);
      }
    }
  }
  if (got_fs) {
    switch_fs_mutex().unlock();
  }
}

#else

#define switch_run_logf(...) \
  do {                       \
  } while (0)

#define switch_diag_logf(...) \
  do {                        \
  } while (0)

// FIX 8b: off-Switch the toggle does not exist, but call sites in shared code (kmemcard's
// [MC] formatter) must keep their original always-on behaviour.
inline bool switch_diag_enabled() {
  return true;
}

inline void switch_set_diag_enabled(bool) {}

#define switch_goal_stage(...) \
  do {                         \
  } while (0)

#define switch_goal_tick(...) \
  do {                        \
  } while (0)

#define switch_audio_probe_arm(...) \
  do {                              \
  } while (0)

#define switch_audio_probe_take() false

#endif
