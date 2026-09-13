#pragma once

/*!
 * @file FsLock.h
 * Global filesystem serialization for the Switch port.
 *
 * newlib's fsdev/devoptab layer (the sdmc: backend) keeps unsynchronized global state: the
 * descriptor table, the per-device cache and the fsp-srv session are all shared, and none of it
 * is guarded. On PC every stdio call lands in a thread-safe libc, so the runtime happily does
 * file I/O from several threads at once:
 *
 *   - the GOAL kernel thread, through sceOpen/sceRead (game/sce/sif_ee.cpp)
 *   - the overlord/ISO thread, streaming DGOs (the fake_iso.cpp files under game/overlord)
 *   - the logging sink and the boot trace
 *
 * On Switch those races corrupt fsdev's internal state. Observed symptoms: a boot freeze inside
 * kopen() with no exception report (the kernel thread's fopen colliding with the overlord's
 * in-flight reads) and truncated save banks (a kernel-side fwrite racing overlord reads).
 *
 * A single global recursive mutex serializes every entry into that layer. Recursive because the
 * higher-level helpers nest (read_binary_file -> open_file). This is a no-op everywhere else.
 */

#if defined(__SWITCH__)

#include <atomic>
#include <chrono>
#include <mutex>

/*!
 * FIX 7m -- the lock is now timed and self-identifying.
 *
 * Every intro death so far has been a freeze in which *every* thread that touches the card
 * stops at the same instant, leaving no fault and no exit. That is what a wedged filesystem
 * lock looks like, but nothing recorded who was holding it, so each run only showed where the
 * victims stopped -- never the culprit. The mutex now remembers the label of its current
 * owner and when it was taken, and the run log can refuse to block on it (see run_log.h).
 */
inline std::recursive_timed_mutex& switch_fs_mutex() {
  static std::recursive_timed_mutex mtx;
  return mtx;
}

inline std::atomic<const char*>& switch_fs_owner_label() {
  static std::atomic<const char*> label{"(none)"};
  return label;
}

inline std::atomic<long long>& switch_fs_owner_since_ms() {
  static std::atomic<long long> since{0};
  return since;
}

inline long long switch_fs_now_ms() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

class SwitchFsLockGuard {
 public:
  explicit SwitchFsLockGuard(const char* label) : m_label(label) {
    switch_fs_mutex().lock();
    // Only the outermost acquisition on this thread names the owner; recursive re-entry
    // must not overwrite it (or clear it on the inner release).
    m_outermost = (s_depth++ == 0);
    if (m_outermost) {
      switch_fs_owner_label().store(label, std::memory_order_relaxed);
      switch_fs_owner_since_ms().store(switch_fs_now_ms(), std::memory_order_relaxed);
    }
  }
  ~SwitchFsLockGuard() {
    if (--s_depth == 0) {
      switch_fs_owner_label().store("(none)", std::memory_order_relaxed);
      switch_fs_owner_since_ms().store(0, std::memory_order_relaxed);
    }
    switch_fs_mutex().unlock();
  }
  SwitchFsLockGuard(const SwitchFsLockGuard&) = delete;
  SwitchFsLockGuard& operator=(const SwitchFsLockGuard&) = delete;

 private:
  const char* m_label;
  bool m_outermost{false};
  static inline thread_local int s_depth = 0;
};

#define SWITCH_FS_LOCK() SwitchFsLockGuard _switch_fs_lock_guard(__func__)

#else

#define SWITCH_FS_LOCK() \
  do {                   \
  } while (0)

#endif
