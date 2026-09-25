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
 * are invaluable during a debugging session and pure noise during normal play. Hold
 * L3 + R3 + Minus together to flip this; the combo is detected on the render thread, which
 * owns the SDL event pump. One-shot forensic lines (session start/exit, [disp], [net],
 * crash paths) always log regardless of this flag.
 *
 * FIX 41 (AI-assisted): DEFAULTS TO OFF. This used to default to on "so a fresh boot is
 * always measurable", and that decision quietly cost more than every optimisation in this
 * port put together:
 *
 *   - each logged line took ~6 ms on the console (spike dump line timestamps: 341.268,
 *     341.274, 341.280, 341.286, ... - see FIX 40);
 *   - a 2-second report block is ~20 lines, so ~120 ms, which is exactly the 150-260 ms
 *     frames the spike log recorded every 2.05 s with an idle loader;
 *   - the [gfx] heartbeat fired every ~265 ms, and its [chan] mirror in gk_fatal.txt
 *     fsync()s the SD card on every single line for the whole t=8..30 s window - i.e. it
 *     runs a synchronous card flush four times a second during exactly the part of the
 *     boot where the game was freezing;
 *   - the FIX 39 spike dump made it self-amplifying: a frame over 45 ms writes 7 more
 *     lines, which makes the next frame slow too. In a busy scene (Jak 1 Sandover) that is
 *     a permanent tax, which is why performance there got "way worse" after a build whose
 *     only change was more instrumentation.
 *
 * So normal play is now silent, and measurement is opt-in: hold L3+R3+Minus to start a
 * capture. The game the player boots is no longer an instrument.
 */
inline std::atomic<bool> g_switch_diag_enabled{false};

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

/*!
 * FIX 40 -- THE TELEMETRY WAS THE STUTTER. (AI-assisted)
 *
 * Every line written here cost about 6 ms on the console. That is not a guess; the
 * spike dump timestamps its own lines:
 *
 *   [341.268] [spike] 201.3ms frame ... unaccounted 167.8
 *   [341.274] [spike]   [ 3] blit    12.62ms
 *   [341.280] [spike]   [205] merc-l2-pris 2.72ms
 *   [341.286] ... [341.292] ... [341.299] ... [341.310] ... [341.317] [fps] ...
 *
 * Six milliseconds apart, every line. A 2-second report block is ~20 lines, so roughly
 * 120 ms - and the spike log showed exactly that, a 150-260 ms frame arriving every
 * 2.05 seconds with `loader 0.01` and only 32 ms of buckets, the rest unaccounted. The
 * periodic hitch the player feels on the zoomer *is the measurement*, and it has been in
 * every build since the phase telemetry was added.
 *
 * Two things made a single line that expensive:
 *   1. it waited up to 500 ms for the global filesystem lock, which the ISO streaming
 *      thread holds constantly while an area loads;
 *   2. it then did its own unbuffered write() syscall to the SD card.
 *
 * So lines are now formatted into memory and flushed in one write: ~20 syscalls and ~20
 * lock acquisitions per report become one. The FS lock wait drops from 500 ms to 2 ms -
 * if the card is busy we simply keep buffering, which is strictly better than today's
 * behaviour (it waited, then wrote *anyway* without the lock). Forensic lines - crashes,
 * exits, fatal paths - still flush immediately and still wait for the lock, because for
 * those an unflushed line is a lost clue.
 */
inline void switch_run_log_flush_locked(int& fd, char* pending, int& len, long long ms) {
  if (len <= 0) {
    return;
  }
  if (fd < 0) {
    fd = open(SWITCH_LOG_PATH("gk_run_log.txt"), O_WRONLY | O_CREAT | O_APPEND, 0644);
  }
  if (fd >= 0) {
    // FIX 7d: a write() that starts failing (fd smashed by an fsdev race) used to fail
    // silently forever. Drop the fd on failure so the next flush re-opens the file.
    if (write(fd, pending, len) < 0) {
      close(fd);
      fd = -1;
    } else {
      // FIX 7w: fsync is a synchronous flush to the card; keep it on a timer.
      static long long s_last_sync_ms = 0;
      if (ms - s_last_sync_ms >= 1000) {
        s_last_sync_ms = ms;
        fsync(fd);
      }
    }
  }
  len = 0;
}

inline void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void switch_run_logf(const char* fmt, ...) {
  static std::mutex s_mtx;
  static int s_fd = -1;  // opened on first use, kept for the whole session
  static const std::chrono::steady_clock::time_point s_t0 = std::chrono::steady_clock::now();
  // FIX 40: lines live here until a flush. 64 KB is ~400 lines, far more than the ~20 a
  // report block produces, so a busy card never costs us data.
  static char s_pending[64 * 1024];
  static int s_pending_len = 0;
  static long long s_last_flush_ms = 0;
  static int s_dropped = 0;       // lines lost to a full buffer, reported on the next flush
  static bool s_noted_busy = false;  // one LOCKBUSY note per flush window, not per line

  char buf[512];
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - s_t0)
                      .count();
  int n = snprintf(buf, sizeof(buf), "[%lld.%03lld] ", (long long)ms / 1000,
                   (long long)ms % 1000);
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) {
    return;
  }
  n += m;
  if (n > (int)sizeof(buf) - 2) {
    n = (int)sizeof(buf) - 2;
  }
  buf[n++] = '\n';

  // A forensic line must reach the card even if this is the last instruction the process
  // executes, so it is worth blocking for. Everything else is telemetry and is not.
  const bool forensic = strstr(buf, "EXCEPTION") || strstr(buf, "[exit]") ||
                        strstr(buf, "fatal") || strstr(buf, "FATAL") ||
                        strstr(buf, "abort");

  // FIX 41 (AI-assisted): the ordinary path must not touch the filesystem lock at all.
  // FIX 40 buffered the writes but still asked for the lock on every line with a 2 ms
  // timeout, and while an area is streaming that lock is held almost continuously - so a
  // 20-line report could still burn 40 ms of pure waiting. Appending to the buffer needs
  // nothing but the log mutex.
  bool flush_due = false;
  {
    std::lock_guard<std::mutex> lk(s_mtx);
    // FIX 7s: the network channel goes first and outside the fd path. It shares nothing
    // with fsdev, so it keeps reporting even when SD logging is wedged.
    switch_net_log_write(buf, n);
    if (s_pending_len + n <= (int)sizeof(s_pending)) {
      memcpy(s_pending + s_pending_len, buf, n);
      s_pending_len += n;
    } else {
      s_dropped++;
    }
    flush_due = forensic || s_pending_len > (int)sizeof(s_pending) - 1024 ||
                (ms - s_last_flush_ms) >= 2000;
  }
  if (!flush_due) {
    return;
  }

  // FIX 7l -- LOCK ORDER: filesystem lock first, then the log mutex. Callers such as
  // sceOpen() hold the fs lock across their body and then log, so taking them the other
  // way round is an AB-BA deadlock. SWITCH_FS_LOCK() is recursive, so a caller that
  // already owns it simply re-enters. Note the append above only ever takes s_mtx and
  // never waits for anything while holding it, so it cannot participate in a cycle.
  const bool got_fs =
      switch_fs_mutex().try_lock_for(std::chrono::milliseconds(forensic ? 500 : 2));
  if (!got_fs) {
    // The card is busy. Keep buffering; the next line will try again. Only note who is
    // holding it, and only once per flush window, so a freeze is still visible.
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_noted_busy) {
      s_noted_busy = true;
      const char* owner = switch_fs_owner_label().load(std::memory_order_relaxed);
      const long long since = switch_fs_owner_since_ms().load(std::memory_order_relaxed);
      char note[160];
      int nn = snprintf(note, sizeof(note), "[LOCKBUSY owner=%s held_for=%lldms]\n",
                        owner ? owner : "(null)", switch_fs_now_ms() - since);
      if (nn > 0 && s_pending_len + nn <= (int)sizeof(s_pending)) {
        memcpy(s_pending + s_pending_len, note, nn);
        s_pending_len += nn;
      }
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_last_flush_ms = ms;
    s_noted_busy = false;
    if (s_dropped > 0) {
      char note[96];
      int nn = snprintf(note, sizeof(note), "[log] %d lines dropped (buffer full)\n",
                        s_dropped);
      if (nn > 0 && s_pending_len + nn <= (int)sizeof(s_pending)) {
        memcpy(s_pending + s_pending_len, note, nn);
        s_pending_len += nn;
      }
      s_dropped = 0;
    }
    switch_run_log_flush_locked(s_fd, s_pending, s_pending_len, ms);
  }
  switch_fs_mutex().unlock();
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
