#pragma once

/*!
 * @file safe_stdout.h
 * FIX 7t: a stdout for the Switch port that can never kill the game.
 *
 * THE BUG THIS FIXES (the intro freeze, finally caught on 2026-09-11 by the live network
 * log -- the SD log could never show it, because the SD log is the very thing that dies):
 *
 *     mc_print<>                      game/kernel/common/kmemcard.cpp:122
 *     fmt::v11::detail::fwrite_all    third-party/fmt/include/fmt/format-inl.h:79
 *     __cxa_throw                     <-- fwrite() to stdout came up short
 *     std::terminate                  <-- nobody catches it
 *     diagAbortWithResult -> __libnx_exit -> abort -> _exit(1)
 *
 * fmt's fwrite_all() throws system_error("cannot write to file") whenever fwrite() writes
 * fewer bytes than asked. stdout was redirected to a plain SD file (main.cpp) and left
 * *unbuffered*, so every print became a direct fsdev write on whichever thread printed.
 * fsdev is not thread-safe -- this port has a whole race catalogue about it in FsLock.h --
 * and the kernel thread's [MC] print lands in the middle of the ISO thread streaming
 * VAGWAD.ENG. One short write later the game aborts. That is the entire "intro freeze":
 * not audio, not EGL, not the applet, and not a crash at all -- a debug print.
 *
 * Note the SD card was demonstrably healthy 25ms earlier (run-log lines at t=14.163 were
 * still being fsync'd successfully), which is why "the card is wedged" was never the
 * answer, and why the death always appeared to happen "during audio": the VAG stream is
 * simply the other party in the race.
 *
 * THE FIX: install stdout as a funopen() stream whose write function
 *   1. serialises against every other fsdev user via SWITCH_FS_LOCK(), and
 *   2. *always reports success*, even if the underlying write fails.
 *
 * (2) is the important half. It makes it structurally impossible for any of the ~767
 * printf/fmt::print call sites in the kernel, overlord and sce layers to ever again turn a
 * transient I/O hiccup into an uncaught exception and a dead process. A debug print that
 * cannot be delivered must be dropped, never fatal.
 */

#if defined(__SWITCH__)

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <mutex>

#include "common/util/FsLock.h"
#include "game/switch/log_paths.h"

/*!
 * FIX 67 (AI-assisted): the write callback used to do the SD write synchronously, on
 * whichever thread printed, under the filesystem lock. That was fine when stdout only
 * carried boot messages - but the loader's telemetry prints per FRAME while an area
 * streams in (`Loader::update slow setup`, `stage texture took`, the `live=` line), and
 * the hardware logs from the FIX 66 build showed what that costs:
 *
 *   jak2 `ruins` load window (131 loader-heavy frames, 9.33 s):
 *     slow setup ........ 11.5 ms/frame average
 *     stage texture .....  5.6 ms/frame   (real upload work)
 *     mip drain .........  1.2 ms/frame   (rate 2)
 *     remainder ......... ~4.7 ms/frame   <- the SD writes for those print lines,
 *                                            racing the ISO thread's reads on the
 *                                            same card
 *
 * jak1 was worse (its fake_iso layer has no read-ahead): the [cam] telemetry recorded
 * sustained 60-78 ms hitches (f=1848-1879, 2026-10-03 gk_run_log) while an area streamed
 * in - a print-write landing behind a burst of level reads stalls the render thread for
 * several frames at a time. Every print was one unbuffered fsdev write; fsdev writes to
 * FAT32 go through the FS service and the card itself, not through any OS page cache.
 *
 * THE FIX is the one run_log already proved (FIX 40/41 there): buffer the bytes under a
 * small log mutex, and only touch the card when a flush is due - at most every 2 s, when
 * the buffer nears capacity, or when the line itself looks forensic. A flush asks for the
 * filesystem lock with try_lock_for(2 ms) and simply keeps buffering if the card is busy;
 * forensic lines are worth blocking for (500 ms budget) so a dying process still gets its
 * last words out.
 *
 * TRADE-OFF, recorded here because FIX 7t spent days on this: line buffering used to mean
 * a hard abort() lost at most the line it died inside of. With a buffer, a hard abort()
 * that prints nothing recognizable loses everything still pending - up to 2 s of
 * telemetry. Accepted because (a) gk_fatal's crash report is written synchronously to
 * gk_boot_log by its own path and is the primary crash record, (b) the keyword scan below
 * catches the abort/terminate/EXCEPTION/fatal families that precede the common deaths, and
 * (c) the alternative is the measured multi-millisecond per-frame stall above, which is
 * not a trade at all. Lines lost to a full buffer are counted and reported on the next
 * flush, exactly like run_log.
 */
namespace {
constexpr int kStdoutPendingBytes = 64 * 1024;  // matches run_log's FIX 40 sizing

// memmem is a GNU extension and its newlib availability is not worth betting a build on;
// chunks are line-sized, so a direct scan is effectively free.
bool stdout_contains(const char* hay, size_t n, const char* needle) {
  const size_t m = strlen(needle);
  if (m == 0 || n < m) {
    return false;
  }
  for (size_t i = 0; i + m <= n; i++) {
    if (hay[i] == needle[0] && memcmp(hay + i, needle, m) == 0) {
      return true;
    }
  }
  return false;
}

bool stdout_chunk_is_forensic(const char* buf, size_t n) {
  // Same families run_log treats as worth blocking for, plus the C++ ones that reach
  // stdout via fmt before anyone can catch them. Cheap scans; chunks are line-sized.
  static const char* kKeywords[] = {"EXCEPTION", "[exit]",    "fatal", "FATAL",
                                    "abort",     "terminate", "gk_fatal"};
  for (const char* kw : kKeywords) {
    if (stdout_contains(buf, n, kw)) {
      return true;
    }
  }
  return false;
}
}  // namespace

/*!
 * funopen() write callback. Signature is fixed by newlib (devkitA64 takes a size_t count).
 * Returns @p n unconditionally: see (2) above.
 */
inline int switch_safe_stdout_write(void* cookie, const char* buf, size_t n) {
  if (n == 0) {
    return 0;
  }
  const int fd = (int)(intptr_t)cookie;
  if (fd < 0) {
    return (int)n;
  }

  static std::mutex s_mtx;
  static const std::chrono::steady_clock::time_point s_t0 =
      std::chrono::steady_clock::now();
  static char s_pending[kStdoutPendingBytes];
  static int s_pending_len = 0;
  static long long s_last_flush_ms = 0;
  static int s_dropped = 0;  // lines lost to a full buffer, reported on the next flush
  static long long s_retry_after_ms = 0;  // failed-flush backoff: card busy, don't retry yet

  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - s_t0)
                           .count();
  const bool forensic = stdout_chunk_is_forensic(buf, n);

  bool flush_due = false;
  {
    // FIX 41 discipline (from run_log): the append takes nothing but the log mutex and
    // never waits for anything while holding it, so it cannot participate in a lock
    // cycle. The filesystem lock is only ever asked for AFTER this scope ends.
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_pending_len + (int)n <= kStdoutPendingBytes) {
      memcpy(s_pending + s_pending_len, buf, n);
      s_pending_len += (int)n;
    } else {
      s_dropped++;
    }
    // The backoff matters more here than in run_log: FIX 41 measured that the FS lock is
    // held almost continuously while an area streams, and the loader prints several lines
    // PER FRAME then. Without it, every line after a failed flush would pay the full
    // 2 ms try_lock_for timeout -- several ms per frame, the very stall this fix removes.
    // With it, a busy card costs at most one 2 ms attempt per 250 ms of wall time.
    flush_due = (forensic || s_pending_len > kStdoutPendingBytes - 4096 ||
                 (ms - s_last_flush_ms) >= 2000) &&
                (forensic || ms >= s_retry_after_ms);
  }
  if (!flush_due) {
    return (int)n;
  }

  // FIX 7l LOCK ORDER (same rule as run_log): filesystem lock first, then the log mutex.
  // SWITCH_FS_LOCK() is recursive, so a caller that already owns it simply re-enters --
  // and try_lock_for() on a recursive mutex owned by this thread succeeds immediately.
  const bool got_fs =
      switch_fs_mutex().try_lock_for(std::chrono::milliseconds(forensic ? 500 : 2));
  if (!got_fs) {
    // The card is busy (usually: the ISO thread is streaming a level). Keep buffering and
    // back off (250 ms) so the per-line 2 ms attempt cannot become a per-frame tax while
    // the lock is held almost continuously. Forensic lines bypass the backoff above; the
    // gk_fatal path also has its own synchronous report file as backstop.
    std::lock_guard<std::mutex> lk(s_mtx);
    s_retry_after_ms = ms + 250;
    return (int)n;
  }
  {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_last_flush_ms = ms;
    s_retry_after_ms = 0;
    if (s_dropped > 0) {
      char note[96];
      int nn = snprintf(note, sizeof(note), "[stdout] %d lines dropped (buffer full)\n",
                        s_dropped);
      if (nn > 0 && s_pending_len + nn <= kStdoutPendingBytes) {
        memcpy(s_pending + s_pending_len, note, nn);
        s_pending_len += nn;
      }
      s_dropped = 0;
    }
    // FIX 7t rule (2) still governs: a write that fails or comes up short is swallowed,
    // never surfaced -- the undelivered tail is dropped, not retried, not fatal.
    const char* p = s_pending;
    int left = s_pending_len;
    while (left > 0) {
      ssize_t w = write(fd, p, (size_t)left);
      if (w <= 0) {
        break;
      }
      p += w;
      left -= (int)w;
    }
    s_pending_len = 0;
  }
  switch_fs_mutex().unlock();
  return (int)n;
}

/*!
 * Point stdout (and stderr) at the safe sink. Call once, early in main().
 * Falls back to leaving the stream alone if anything here fails -- never fatal.
 */
inline void switch_install_safe_stdout() {
  const int fd = open(SWITCH_LOG_PATH("gk_stdout.txt"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return;
  }
  FILE* f = funopen((const void*)(intptr_t)fd, nullptr, switch_safe_stdout_write, nullptr,
                    nullptr);
  if (!f) {
    close(fd);
    return;
  }
  // Line buffered: the callback receives whole lines, which is what the FIX 67 forensic
  // keyword scan expects (and keeps the pending buffer aligned to real log lines).
  setvbuf(f, nullptr, _IOLBF, 0);
  stdout = f;
  stderr = f;
}

#endif
