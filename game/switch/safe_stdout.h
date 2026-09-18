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
#include <unistd.h>

#include "common/util/FsLock.h"
#include "game/switch/log_paths.h"

/*!
 * funopen() write callback. Signature is fixed by newlib (devkitA64 takes a size_t count).
 * Returns @p n unconditionally: see (2) above.
 */
inline int switch_safe_stdout_write(void* cookie, const char* buf, size_t n) {
  if (n == 0) {
    return 0;
  }
  const int fd = (int)(intptr_t)cookie;
  if (fd >= 0) {
    // Ordered against run_log, the ISO thread and every other fsdev user. The lock is
    // recursive, so a printf() issued from code already holding it is safe.
    SWITCH_FS_LOCK();
    ssize_t written = write(fd, buf, n);
    (void)written;
  }
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
  // Line buffered: one fsdev write per line instead of one per character, while still
  // getting each line out promptly (abort() does not flush, and losing the message that
  // explains a crash is how this port lost days of debugging).
  setvbuf(f, nullptr, _IOLBF, 0);
  stdout = f;
  stderr = f;
}

#endif
