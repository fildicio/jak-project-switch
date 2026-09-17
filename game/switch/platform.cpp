#include "game/switch/platform.h"

#if defined(__SWITCH__)
// Include libnx ALONE in this translation unit: <switch.h> typedefs `u128`, which
// conflicts with the project's own `struct u128` (common/common_types.h). No header
// that pulls in common_types.h, directly or transitively, may appear before this, and
// none are needed for the definitions below.
#include <switch.h>
#include <stdio.h>  // snprintf for the __appExit trap below (plain libc header, no u128 clash)
#include <fcntl.h>   // FIX 7n: raw open() for the lock-free exception log
#include <unistd.h>  // FIX 7n: write/fsync/close for the lock-free exception log
// FIX 28: resident linked-object code ranges. Self-contained header (<atomic>/<stdint>/
// <string.h> only) so it cannot reintroduce the u128 include-order problem.
#include "game/switch/link_bases.h"
// FIX 7s: BSD sockets for the live network log. libnx routes these through its bsd:u
// driver; all plain libc/POSIX headers, no u128 clash.
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

// FIX 7e -- _exit() interposition (declared here, defined below outside the namespace).
// switch_run_logf is an inline function defined in game/switch/run_log.h; run_log.h pulls
// common headers that must not appear before <switch.h> in this TU (u128 conflict), so we
// use a plain declaration and let the linker resolve the inline definition from another TU.
void switch_run_logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// FIX 7g: the dispatch breadcrumb from run_log.h. Declared by hand for the same reason --
// run_log.h cannot be included in this TU. <atomic> is a plain std header with no u128
// collision, so it is safe here. The inline variable in run_log.h has external linkage,
// so this declaration resolves to the very same object.
#include <atomic>
extern std::atomic<unsigned int> g_switch_goal_stage;

// FIX 8b: the periodic-diagnostics toggle, same hand-declared treatment as above. Note we
// bind to the inline *variable* rather than to run_log.h's inline switch_diag_enabled():
// an inline function whose every call site is inlined need not be emitted out-of-line by
// any TU, so a hand declaration of it can fail to link. Inline variables always are.
extern std::atomic<bool> g_switch_diag_enabled;

static bool switch_diag_enabled() {
  return g_switch_diag_enabled.load(std::memory_order_relaxed);
}

namespace switch_platform {

MemInfo get_memory_info() {
  MemInfo result = {0, 0};
  u64 total = 0;
  u64 used = 0;
  // InfoType_TotalMemorySize = 6, InfoType_UsedMemorySize = 7; CUR_PROCESS_HANDLE = 0xffff8001.
  // (FIX 7d: 7c used 18/19 off a wrong comment -- 18 is UserExceptionContextAddr -- and both
  // queries failed, which is why the 7c run logged mem_used=0KB mem_total=0KB.)
  svcGetInfo(&total, (InfoType)6, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, (InfoType)7, CUR_PROCESS_HANDLE, 0);
  result.total = total;
  result.used = used;
  return result;
}

DisplaySize get_display_size_for_operation_mode() {
  if (appletGetOperationMode() == AppletOperationMode_Handheld) {
    return {1280, 720};
  }
  return {1920, 1080};
}

// FIX 7k -- see platform.h. The cookie must outlive the hook, hence file scope.
static AppletHookCookie s_applet_hook_cookie;

static const char* applet_hook_name(AppletHookType type) {
  switch (type) {
    case AppletHookType_OnFocusState:
      return "OnFocusState";
    case AppletHookType_OnOperationMode:
      return "OnOperationMode";
    case AppletHookType_OnPerformanceMode:
      return "OnPerformanceMode";
    case AppletHookType_OnExitRequest:
      return "OnExitRequest";
    case AppletHookType_OnResume:
      return "OnResume";
    case AppletHookType_OnCaptureButtonShortPressed:
      return "OnCaptureButtonShortPressed";
    case AppletHookType_OnAlbumScreenShotTaken:
      return "OnAlbumScreenShotTaken";
    default:
      return "Unknown";
  }
}

static void applet_hook_cb(AppletHookType type, void* /*param*/) {
  // Applet messages are rare (focus changes, exit requests), so logging every one cannot
  // become the write storm that killed the 7c run.
  switch_run_logf("[applet] hook %s (%d) focus=%d opmode=%d", applet_hook_name(type), (int)type,
                  (int)appletGetFocusState(), (int)appletGetOperationMode());
}

void install_applet_hook() {
  appletHook(&s_applet_hook_cookie, applet_hook_cb, nullptr);
  switch_run_logf("[applet] hook installed: focus=%d opmode=%d perf=%d",
                  (int)appletGetFocusState(), (int)appletGetOperationMode(),
                  (int)appletGetPerformanceMode());
}

bool applet_pump() {
  // No I/O on the common path -- this runs every frame. Only genuine state *changes* are
  // logged, and only a bounded number of them, so this can never become a write storm
  // (the 7c lesson).
  static int s_last_focus = -1;
  static int s_last_opmode = -1;
  static int s_log_budget = 24;

  const bool keep_running = appletMainLoop();

  const int focus = (int)appletGetFocusState();
  const int opmode = (int)appletGetOperationMode();
  if ((focus != s_last_focus || opmode != s_last_opmode) && s_log_budget > 0) {
    s_log_budget--;
    switch_run_logf("[applet] state change: focus=%d (was %d) opmode=%d (was %d)", focus,
                    s_last_focus, opmode, s_last_opmode);
  }
  s_last_focus = focus;
  s_last_opmode = opmode;

  if (!keep_running) {
    // The system asked us to exit. Previously we never saw this message at all, so the OS
    // eventually terminated the process instead -- which is what the fatal reports were.
    switch_run_logf("[applet] appletMainLoop() returned false -- system requested exit");
  }
  return keep_running;
}

}  // namespace switch_platform

// FIX 7e -- exit interposition.
//
// Why: the 7b/7d "silent death" ~0.5-2s into the kernel dispatch loop leaves a
// fatal_reports entry (Result 0x1159) whose only possible trigger is hidExit() inside
// __appExit() -- i.e. an exit path RAN -- while every atexit-registered probe stayed
// silent even though the run-log fd was demonstrably alive until 0.2s before death.
// newlib's exit() calls _exit() after the atexit list; anything that calls _exit()
// directly (or exit() whose handler list hangs before the early handlers) skips them.
//
// _exit is a strong symbol in its own archive member (libsysbase ..._exit.o), so this
// definition wins the link and the member is simply not pulled. We hand off to libnx's
// weak __libnx_exit (init.o) afterwards so teardown behaves exactly as before.
//
// The logged lr is the caller's return address: symbolize with the session's printed
// symbol anchor (load_base = anchor_ptr - nm_addr_of_anchor_in_this_ELF).
//
// Known blind spot: if the caller already holds SWITCH_FS_LOCK(), this log line
// deadlocks instead of printing -- which itself shows up as "[exit] _exit missing +
// fatal report present" and is equally diagnostic.
extern "C" void __libnx_exit(int);

extern "C" __attribute__((noreturn)) void _exit(int rc) {
  switch_run_logf("[exit] _exit(%d) lr=%p", rc, __builtin_return_address(0));
  __libnx_exit(rc);
  __builtin_unreachable();
}

// FIX 7g -- libnx abort interposition. THE prime suspect after 7e/7f.
//
// 7d/7e/7f all die ~0.5s into GOAL's dispatch loop with: no CPU exception (no creport),
// no libc exit (the 7e _exit trap never fired), and a contentless fatal report. The only
// remaining mechanism that produces that combination is libnx killing the process itself.
//
// libnx routes essentially every internal "impossible" Result through
// diagAbortWithResult(), whose default implementation is literally
// `svcBreak(BreakReason_Panic, &res, sizeof(res))` -- and it is a WEAK symbol, so this
// strong definition simply replaces it with zero link risk. Its callers inside libnx are
// exactly our suspect list: hid.o, pad.o, applet.o, sm.o, framebuffer.o, default_window.o,
// thread.o, virtmem.o, newlib.o, env.o, init.o.
//
// We log the Result and the caller's return address, then reproduce the default behaviour
// byte for byte (same svcBreak) so the failure mode is unchanged.
//
// Decoding the Result: module = res & 0x1FF, description = (res >> 9) & 0x1FFF.
// e.g. module 202 = HID, 21 = libnx itself, 2 = kernel/FS-adjacent.
//
// svcBreak is declared here by hand rather than via <switch.h>'s inline wrapper to keep
// this definition self-contained; the libnx stub in svc.o provides it.
extern "C" Result svcBreak(u32 breakReason, uintptr_t address, uintptr_t size);

// Defined further down; forward-declared so the abort trap can use it. FIX 7q: the abort
// trap used to report only through switch_run_logf(), which takes the fs lock and can be
// swallowed whole if fsdev is wedged or the lock is held -- which would explain why this
// trap has never been seen to fire despite the tombstone proving an abort happened.
static void switch_exc_write(const char* data, int len);
static char s_abort_buf[512];

extern "C" __attribute__((noreturn)) void diagAbortWithResult(Result res) {
  // Lock-free channel first: this must survive conditions that kill the normal log.
  int an = snprintf(s_abort_buf, sizeof(s_abort_buf),
                    "\n=== FIX 7q diagAbortWithResult ===\nres=0x%08x module=%u desc=%u\n"
                    "lr=%p anchor(get_memory_info)=%p stage=%u\n",
                    (unsigned)res, (unsigned)(res & 0x1FF), (unsigned)((res >> 9) & 0x1FFF),
                    __builtin_return_address(0), (void*)&switch_platform::get_memory_info,
                    g_switch_goal_stage.load(std::memory_order_relaxed));
  if (an > 0) {
    switch_exc_write(s_abort_buf, an);
  }
  switch_run_logf("[fatal] diagAbortWithResult res=0x%08x (module=%u desc=%u) lr=%p stage=%u",
                  (unsigned)res, (unsigned)(res & 0x1FF), (unsigned)((res >> 9) & 0x1FFF),
                  __builtin_return_address(0),
                  g_switch_goal_stage.load(std::memory_order_relaxed));
  Result tmp = res;
  svcBreak(0, (uintptr_t)&tmp, sizeof(tmp));
  __builtin_unreachable();
}

// FIX 7h -- __appExit() interposition. THE decisive trap for the remaining hypothesis.
//
// Chain of evidence: every death leaves an all-zero 0x1159 fatal tombstone; the 0x1159
// aborts fire only when the HID sharedmem pointer is NULL, which happens only after
// hidExit(); hidExit's single caller is __appExit+0x34 (verified by disassembling the
// deployed binary). __appExit runs from __libnx_exit (body: __appExit ->
// envGetExitFuncPtr -> __nx_exit), and __libnx_exit is reachable from _exit (7e trap --
// never fired), from exit() via the atexit list (7e "[exit] atexit begin" -- never
// fired), or from a DIRECT __libnx_exit/__appExit call. Whatever the arm, __appExit
// runs at every death -- so this override is guaranteed to observe the killer's call
// chain, which no higher-level trap could do.
//
// __appExit is a WEAK symbol in libnx.a(init.o); this strong definition wins with zero
// link risk. Body: log + frame-pointer walk + bounded stack scan FIRST (while fsdev is
// still alive), then a faithful replica of libnx's default teardown, verified against
// the shipped 7g ELF at 0xad8bc0:
//   __nx_win_exit(); fsdevUnmountAll(); fsExit(); timeExit(); hidExit(); appletExit();
//   smExit();  (tail call)
// __nx_win_exit is the only function here that switch.h doesn't already declare
// (libnx's default_window.c); the rest (fsdevUnmountAll/fsExit/timeExit/hidExit/
// appletExit/smExit) come from the libnx headers aggregated by <switch.h> above.
extern "C" void __nx_win_exit();

extern "C" void __appExit() {
  // 1) caller return address + frame-pointer chain (ours + any FP-keeping callers).
  char walk[160];
  int off = 0;
  void* fp = __builtin_frame_address(0);
  for (int i = 0; i < 8 && fp; i++) {
    void** f = (void**)fp;
    void* ret = f[1];
    off += snprintf(walk + off, sizeof(walk) - off, "%s%p", i ? " " : "", ret);
    if (off >= (int)sizeof(walk) - 24) {
      break;
    }
    void* next = f[0];
    if ((uintptr_t)next <= (uintptr_t)fp || ((uintptr_t)next & 7)) {
      break;  // FP chain broken (libnx/newlib -O2 code omits frame pointers)
    }
    fp = next;
  }
  walk[off < (int)sizeof(walk) ? off : (int)sizeof(walk) - 1] = 0;
  switch_run_logf("[exit] __appExit entered lr=%p walk=%s", __builtin_return_address(0),
                  walk);

  // 2) bounded stack scan: the exiting thread's stack, up 4KB, collecting words that
  // look like code addresses (inside +/-256MB of this module). Symbolize offline with
  // gk.<ver>.elf nm + the session's symbol anchor line. This recovers return addresses
  // even when intermediate frames had no frame pointers.
  {
    const uintptr_t anchor = (uintptr_t)&switch_platform::get_memory_info;
    const uintptr_t lo = anchor - 0x10000000ull;
    const uintptr_t hi = anchor + 0x10000000ull;
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    char scan[420];
    int so = 0;
    int found = 0;
    for (int i = 0; i < 512 && found < 16; i++) {
      uintptr_t v = *(volatile uintptr_t*)(sp + (uintptr_t)i * 8);
      if (v >= lo && v < hi && (v & 3) == 0 && v != anchor) {
        so += snprintf(scan + so, sizeof(scan) - so, "%s%p", found ? " " : "", (void*)v);
        found++;
        if (so >= (int)sizeof(scan) - 24) {
          break;
        }
      }
    }
    scan[so < (int)sizeof(scan) ? so : (int)sizeof(scan) - 1] = 0;
    if (found) {
      switch_run_logf("[exit] stack scan: %s", scan);
    }
  }

  // 3) faithful teardown (the libnx default; see comment above).
  __nx_win_exit();
  fsdevUnmountAll();
  fsExit();
  timeExit();
  hidExit();
  appletExit();
  smExit();
}


// ---------------------------------------------------------------------------------------
// FIX 7n -- in-process CPU exception handler.
//
// By 7m we had trapped every *userspace* death path and none of them ever fired:
//   _exit()              (7e)  -- silent
//   diagAbortWithResult()(7g)  -- silent
//   __appExit()          (7h)  -- silent
//   [LOCKBUSY] fs-lock   (7m)  -- never printed, so nothing was wedging the filesystem lock
// yet the run log still stops dead mid-line-rate logging and the console shows a fatal
// after a ~5s pause. A process that dies without running a single line of its own exit
// code is a process the *kernel* stopped: a CPU exception.
//
// libnx can deliver those to us. `__libnx_exception_handler` is a weak symbol, and the
// entry stub needs a dedicated stack (`__nx_exception_stack`) plus `__nx_exception_ignoredebug`
// so the handler still runs when Atmosphere is watching the process. Defining all three
// here makes the faulting thread describe its own death -- exact PC, LR, fault address and
// ESR -- straight to the SD, before Atmosphere ever gets involved.
//
// Constraints inside this handler (it runs on a fault, on a tiny alternate stack):
//   - no locks. switch_run_logf takes mutexes, and the faulting thread may already hold
//     them; that would turn the crash into a hang. We open/write/close a raw fd instead.
//   - no allocation, no fmt, no iostreams. Fixed static buffers only.
//   - no large stack frames -- hence the static scratch buffer.
extern "C" {
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1;
}

static char s_exc_buf[2048];

// FIX 7r -- a persistently-open fd for the fatal channel.
//
// Every "this trap never fired" conclusion in this investigation rests on the assumption
// that switch_exc_write() actually reaches the SD card at death time. That assumption has
// never been tested. open() at death time goes through fsdev, which allocates, takes
// locks, and does IPC to the fs service -- all of which can fail precisely in the
// situation we are trying to report on. So: open the file once, at startup, and keep the
// descriptor. At death time the write path is then a bare write()+fsync() syscall pair.
static std::atomic_int s_fatal_fd{-1};

void switch_fatal_channel_open() {
  if (s_fatal_fd.load(std::memory_order_relaxed) >= 0) {
    return;
  }
  int fd = open("sdmc:/gk_fatal.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    s_fatal_fd.store(fd, std::memory_order_relaxed);
  }
}

static void switch_exc_write(const char* data, int len) {
  // Preferred path: the descriptor opened at startup. No allocation, no open(), no
  // directory walk -- just the two syscalls needed to get bytes onto the card.
  int fd = s_fatal_fd.load(std::memory_order_relaxed);
  if (fd >= 0) {
    ssize_t ignored = write(fd, data, (size_t)len);
    (void)ignored;
    fsync(fd);
    return;
  }
  // Fallback: open per call. Closed immediately so a half-initialised or smashed fsdev
  // state cannot leave us holding anything.
  fd = open("sdmc:/gk_fatal.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    ssize_t ignored = write(fd, data, (size_t)len);
    (void)ignored;
    fsync(fd);
    close(fd);
  }
}

// FIX 7r -- heartbeat on the fatal channel itself.
//
// Called from the graphics heartbeat. Writes a line to gk_fatal.txt for a bounded window
// that straddles the death (~15s). If the file ends with a heartbeat at ~14.9s and no
// trap line, the channel was demonstrably alive right up to the death and the traps
// really did not fire -- the abort is outside our module. If the heartbeats stop early,
// the channel is the problem and every previous negative result must be re-examined.
void switch_fatal_channel_heartbeat(double t, unsigned stage, unsigned iter) {
  if (!switch_diag_enabled()) {
    return;
  }
  if (t < 8.0 || t > 30.0) {
    return;
  }
  static char buf[160];
  int n = snprintf(buf, sizeof(buf), "[chan] t=%.3f stage=%u iter=%u\n", t, stage, iter);
  if (n > 0) {
    switch_exc_write(buf, n);
  }
}

// FIX 7s -- live network log.
//
// Rationale: every previous iteration cost a physical SD-card round trip, which is the
// real reason this investigation has taken so many hardware runs. A TCP stream to the
// development machine turns a test into "press A and watch", and is a strictly better
// instrument besides: it shares nothing with fsdev, so it keeps reporting under the exact
// conditions that can silence the SD log.
//
// Design constraints, all learned the hard way in this port:
//   - The socket driver's buffers are charged to our heap and the hardware runs with only
//     ~4MB free (mem_used=3261548KB of 3265536KB), so the config below is deliberately
//     tiny rather than socketInitializeDefault()'s ~1MB+.
//   - connect() must never block boot. It is done non-blocking with a 2s poll; on any
//     failure we simply run without the network channel and the SD log stays the fallback.
//   - TCP_NODELAY is essential: without it Nagle coalesces lines and the last moments
//     before death -- the only part we care about -- sit in a buffer and are lost.
//   - Sends must never stall the caller (switch_run_logf runs on the gfx and kernel
//     threads), hence a short SO_SNDTIMEO and a best-effort, drop-on-failure send.
static std::atomic_int s_net_fd{-1};

void switch_net_log_init() {
  // Host is read from the SD card so the listener address can change without a rebuild.
  // Format: "192.168.0.30:9000" (port optional).
  char host[64] = "192.168.0.30";
  int port = 9000;
  int cf = open("sdmc:/gk_log_host.txt", O_RDONLY);
  if (cf >= 0) {
    char raw[64] = {0};
    ssize_t got = read(cf, raw, sizeof(raw) - 1);
    close(cf);
    if (got > 0) {
      // Trim whitespace/newlines, then split off an optional ":port".
      char* end = raw + got;
      while (end > raw && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) {
        *--end = '\0';
      }
      char* colon = strchr(raw, ':');
      if (colon) {
        *colon = '\0';
        int p = atoi(colon + 1);
        if (p > 0 && p < 65536) {
          port = p;
        }
      }
      if (raw[0]) {
        snprintf(host, sizeof(host), "%s", raw);
      }
    }
  }

  static const SocketInitConfig cfg = {
      0x1000,               // tcp_tx_buf_size
      0x1000,               // tcp_rx_buf_size
      0x4000,               // tcp_tx_buf_max_size
      0x4000,               // tcp_rx_buf_max_size
      0,                    // udp_tx_buf_size
      0,                    // udp_rx_buf_size
      1,                    // sb_efficiency
      2,                    // num_bsd_sessions
      BsdServiceType_User,  // bsd_service_type
  };
  Result rc = socketInitialize(&cfg);
  if (R_FAILED(rc)) {
    switch_run_logf("[net] socketInitialize failed rc=0x%08x -- SD log only", (unsigned)rc);
    return;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    switch_run_logf("[net] socket() failed errno=%d -- SD log only", errno);
    return;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u16)port);
  addr.sin_addr.s_addr = inet_addr(host);

  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  bool connected = false;
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
    connected = true;
  } else if (errno == EINPROGRESS) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLOUT)) {
      int soerr = 0;
      socklen_t len = sizeof(soerr);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) {
        connected = true;
      }
    }
  }

  if (!connected) {
    switch_run_logf("[net] connect to %s:%d failed errno=%d -- SD log only", host, port, errno);
    close(fd);
    return;
  }

  fcntl(fd, F_SETFL, flags);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000;  // 100ms: never stall a logging thread on a stalled network
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  s_net_fd.store(fd, std::memory_order_release);
  switch_run_logf("[net] connected to %s:%d -- live log active", host, port);
}

void switch_net_log_write(const char* data, int len) {
  int fd = s_net_fd.load(std::memory_order_acquire);
  if (fd < 0 || len <= 0) {
    return;
  }
  // Best effort. A dropped line is acceptable; a blocked logging thread is not, and a
  // failed send must not take the channel down permanently (the next line retries).
  ssize_t ignored = send(fd, data, (size_t)len, 0);
  (void)ignored;
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
  // Anchor for offline symbolization: subtract this from pc/lr after looking the symbol up
  // with nm on the archived gk.<fix>.elf, exactly like the "symbol anchor" line in the run log.
  const uintptr_t anchor = (uintptr_t)&switch_platform::get_memory_info;
  int n = 0;
  n += snprintf(s_exc_buf + n, sizeof(s_exc_buf) - n,
                "\n=== FIX 7n CPU EXCEPTION ===\n"
                "error_desc=0x%x esr=0x%x far=0x%llx\n"
                "pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx\n"
                "anchor(get_memory_info)=0x%llx pstate=0x%x\n",
                (unsigned)ctx->error_desc, (unsigned)ctx->esr,
                (unsigned long long)ctx->far.x, (unsigned long long)ctx->pc.x,
                (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x,
                (unsigned long long)ctx->fp.x, (unsigned long long)anchor,
                (unsigned)ctx->pstate);
  n += snprintf(s_exc_buf + n, sizeof(s_exc_buf) - n, "pc_off=0x%llx lr_off=0x%llx\n",
                (unsigned long long)(ctx->pc.x - anchor),
                (unsigned long long)(ctx->lr.x - anchor));
  for (int i = 0; i < 29 && n < (int)sizeof(s_exc_buf) - 64; i++) {
    n += snprintf(s_exc_buf + n, sizeof(s_exc_buf) - n, "X%02d=0x%llx%s", i,
                  (unsigned long long)ctx->cpu_gprs[i].x, (i % 4 == 3) ? "\n" : " ");
  }
  if (n < (int)sizeof(s_exc_buf) - 2) {
    s_exc_buf[n++] = '\n';
  }

  // Bounded stack scan for return addresses inside this module (+/-256MB of the anchor),
  // same trick as the 7h exit trap: -O3 drops frame pointers, so this is what recovers the
  // call chain. Guarded by the fault address so we never fault again inside the handler.
  const uintptr_t lo = anchor - 0x10000000ull;
  const uintptr_t hi = anchor + 0x10000000ull;
  uintptr_t sp = (uintptr_t)ctx->sp.x;
  if (sp && (sp & 7) == 0) {
    int found = 0;
    n += snprintf(s_exc_buf + n, sizeof(s_exc_buf) - n, "stack:");
    for (int i = 0; i < 768 && found < 24 && n < (int)sizeof(s_exc_buf) - 32; i++) {
      uintptr_t v = *(volatile uintptr_t*)(sp + (uintptr_t)i * 8);
      if (v >= lo && v < hi && (v & 3) == 0) {
        n += snprintf(s_exc_buf + n, sizeof(s_exc_buf) - n, " 0x%llx",
                      (unsigned long long)(v - anchor));
        found++;
      }
    }
    n += snprintf(s_exc_buf + n, sizeof(s_exc_buf) - n, "\n");
  }
  switch_exc_write(s_exc_buf, n);

  // FIX 25 -- GOAL (EE arena) decode of the same dump.
  //
  // The 2026-09-17 crash triage (6 CPU exceptions across several boots; see
  // SWITCH_PORT_SESSION_NOTES.md) had to reconstruct all of this by hand on the host by
  // matching pc/lr against the [ee_runner] rw=/rx= lines of every boot in gk_boot_log.txt.
  // Doing it here makes every future crash self-locating: pc/lr/sp and every register get
  // their EE-arena offsets, and the GOAL stack (which lives inside the arena) gets a
  // backtrace of EE return addresses.
  //
  // The two arena globals are declared by hand instead of including game/runtime.h for the
  // same u128-ordering reason as switch_run_logf above (see the include block comment at the
  // top of this file). Variable mangling ignores the pointee type, so `unsigned char*`
  // declarations link against the `u8*` definitions in game/runtime.cpp.
  extern unsigned char* g_ee_main_mem;       // u8* g_ee_main_mem (game/runtime.h)
  extern unsigned char* g_ee_main_mem_exec;  // u8* g_ee_main_mem_exec (game/runtime.h)
  const uintptr_t ee_rw = (uintptr_t)g_ee_main_mem;
  const uintptr_t ee_rx = (uintptr_t)g_ee_main_mem_exec;
  // EE_MAIN_MEM_SIZE (128 MB) from common/goal_constants.h, restated by hand (see above).
  const uintptr_t ee_size = 0x8000000ull;
  if (ee_rw && ee_rx) {
    static char ee_buf[4096];
    int m = 0;
    m += snprintf(ee_buf + m, sizeof(ee_buf) - m,
                  "=== EE (GOAL) DECODE ===\n"
                  "rw=0x%llx rx=0x%llx size=0x%llx\n"
                  "pc_ee=0x%llx lr_ee=0x%llx sp_ee=0x%llx\n",
                  (unsigned long long)ee_rw, (unsigned long long)ee_rx,
                  (unsigned long long)ee_size,
                  (unsigned long long)(ctx->pc.x >= ee_rx ? ctx->pc.x - ee_rx : 0),
                  (unsigned long long)(ctx->lr.x >= ee_rx ? ctx->lr.x - ee_rx : 0),
                  (unsigned long long)(ctx->sp.x >= ee_rw ? ctx->sp.x - ee_rw : 0));
    // Registers, annotated with their EE offset when they point into the arena. GOAL
    // pointers (objects, symbol-table entries, code) are all arena-relative, so this turns
    // the raw dump into "which GOAL things were live at the crash".
    m += snprintf(ee_buf + m, sizeof(ee_buf) - m, "regs_ee:");
    for (int i = 0; i < 29 && m < (int)sizeof(ee_buf) - 48; i++) {
      uintptr_t v = (uintptr_t)ctx->cpu_gprs[i].x;
      if (v >= ee_rx && v - ee_rx < ee_size) {
        m += snprintf(ee_buf + m, sizeof(ee_buf) - m, " X%02d=EE+0x%llx", i,
                      (unsigned long long)(v - ee_rx));
      } else if (v >= ee_rw && v - ee_rw < ee_size) {
        m += snprintf(ee_buf + m, sizeof(ee_buf) - m, " X%02d=rw+0x%llx", i,
                      (unsigned long long)(v - ee_rw));
      }
    }
    if (m < (int)sizeof(ee_buf) - 2) {
      ee_buf[m++] = '\n';
    }
    // GOAL backtrace: the GOAL stack lives inside the arena (call_goal_on_stack), so words
    // on it that point into the executable alias are GOAL return addresses. Same
    // guarded-read discipline as the module stack scan above.
    if (sp && (sp & 7) == 0) {
      int found = 0;
      m += snprintf(ee_buf + m, sizeof(ee_buf) - m, "goal_backtrace:");
      for (int i = 0; i < 768 && found < 24 && m < (int)sizeof(ee_buf) - 32; i++) {
        uintptr_t v = *(volatile uintptr_t*)(sp + (uintptr_t)i * 8);
        if (v >= ee_rx && v - ee_rx < ee_size && ((v - ee_rx) & 3) == 0) {
          m += snprintf(ee_buf + m, sizeof(ee_buf) - m, " EE+0x%llx",
                        (unsigned long long)(v - ee_rx));
          found++;
        }
      }
      if (m < (int)sizeof(ee_buf) - 2) {
        ee_buf[m++] = '\n';
      }
    }
    switch_exc_write(ee_buf, m);
  }

  // FIX 28 -- name the code, not just the offsets.
  //
  // FIX 25 turned every crash into EE offsets, but the 2026-09-17/18 triage then stalled on
  // "which object is 0x1937bdc in?": no symbol map ships with the port, and switch_boot_log()
  // latches off at "boot complete" -- right after the title screen -- so level-time links
  // (exactly where these crashes live) were never logged at all. FIX 27 (jak1/klink.cpp)
  // records every object's final code range in a resident ring (game/switch/link_bases.h);
  // use it now: annotate pc/lr and dump the whole ring so any offset in the dump (backtrace,
  // registers) can be resolved on the host afterwards.
  if (ee_rw && ee_rx) {
    static char obj_buf[512];
    int ob = 0;
    unsigned pc_off = (unsigned)(ctx->pc.x >= ee_rx ? ctx->pc.x - ee_rx : 0);
    unsigned lr_off = (unsigned)(ctx->lr.x >= ee_rx ? ctx->lr.x - ee_rx : 0);
    ob = snprintf(obj_buf, sizeof(obj_buf), "=== OBJECTS ===\npc_in=");
    const SwitchLinkBaseRecord* rec = switch_link_bases_find(pc_off);
    ob += snprintf(obj_buf + ob, sizeof(obj_buf) - ob, rec ? "%s+0x%x\n" : "?\n",
                   rec ? rec->name : "", rec ? pc_off - rec->base : 0);
    ob += snprintf(obj_buf + ob, sizeof(obj_buf) - ob, "lr_in=");
    rec = switch_link_bases_find(lr_off);
    ob += snprintf(obj_buf + ob, sizeof(obj_buf) - ob, rec ? "%s+0x%x\n" : "?\n",
                   rec ? rec->name : "", rec ? lr_off - rec->base : 0);
    switch_exc_write(obj_buf, ob);

    // Full ring dump, newest first (a level reload's later record is the live one).
    uint32_t lb_total = switch_link_bases_total().load(std::memory_order_relaxed);
    uint32_t lb_first = lb_total > 1024 ? lb_total - 1024 : 0;
    static char lb_buf[8192];
    int lb = snprintf(lb_buf, sizeof(lb_buf), "=== LINK BASES (%u of %u) ===\n",
                      lb_total - lb_first, lb_total);
    for (uint32_t i = lb_total; i-- > lb_first;) {
      const SwitchLinkBaseRecord& e = switch_link_bases_table()[i & 1023];
      int w = snprintf(lb_buf + lb, sizeof(lb_buf) - lb, "EE+0x%x size=%u obj=%s\n", e.base,
                       e.size, e.name);
      if (w < 0 || (int)sizeof(lb_buf) - lb - w < 96) {
        switch_exc_write(lb_buf, lb);
        lb = 0;
        w = snprintf(lb_buf, sizeof(lb_buf), "EE+0x%x size=%u obj=%s\n", e.base, e.size, e.name);
      }
      if (w > 0) {
        lb += w;
      }
    }
    if (lb > 0) {
      switch_exc_write(lb_buf, lb);
    }
  }

  // FIX 28b -- GOAL symbol resolution (jak1 layout only; jak2/3/x use different parallel
  // name tables and the Switch port ships jak1 today).
  //
  // This is what can name DATA targets like EE+0x18fe04: if a global object's address is a
  // symbol value, it prints as an exact match. For code, "nearest symbol at-or-below" gives
  // function names. The symbol globals live in game/kernel/common/kscheme.cpp as Ptr<u32>;
  // Ptr<u32> is a single-u32 wrapper and C++ variable mangling ignores the type, so these
  // hand-written layout-compatible declarations link -- same discipline as g_ee_main_mem.
  // Layout (common/goal_constants.h + jak1/kscheme.h): 8-byte symbol entries {u32 value} in
  // [SymbolTable2, LastSymbol); name of symbol `sym` is SymInfo{u32 hash, Ptr<String> str}
  // at sym + SYM_INFO_OFFSET; String is {u32 len, char data[]}.
  {
    extern int g_game_version;  // GameVersion g_game_version; Jak1 = 1 (common/versions)
    if (ee_rw && ee_rx && g_game_version == 1) {
      struct PtrU32 { unsigned offset; };
      extern PtrU32 SymbolTable2;  // Ptr<u32> (game/kernel/common/kscheme.cpp)
      extern PtrU32 LastSymbol;
      const unsigned SYM_INFO_OFFSET = 16384u * 8u - 4u;  // jak1 (goal_constants.h)
      unsigned first = SymbolTable2.offset;
      unsigned last = LastSymbol.offset;
      if (first >= 16 && last > first && last <= 0x8000000 && last - first <= 16384u * 8u * 2u) {
        // Interesting addresses: pc/lr/far, every arena-pointing register, and the GOAL
        // backtrace (stack rescan, same discipline as FIX 25).
        struct SymTarget {
          unsigned addr;
          unsigned best_val;
          unsigned best_sym;
        };
        static SymTarget targets[40];
        int n_targets = 0;
        auto add_target = [&](unsigned a) {
          if (a && a < 0x8000000 && n_targets < (int)(sizeof(targets) / sizeof(targets[0]))) {
            for (int i = 0; i < n_targets; i++) {
              if (targets[i].addr == a) {
                return;
              }
            }
            targets[n_targets].addr = a;
            targets[n_targets].best_val = 0;
            targets[n_targets].best_sym = 0;
            n_targets++;
          }
        };
        add_target((unsigned)(ctx->pc.x >= ee_rx ? ctx->pc.x - ee_rx : 0));
        add_target((unsigned)(ctx->lr.x >= ee_rx ? ctx->lr.x - ee_rx : 0));
        if (ctx->far.x >= ee_rw && ctx->far.x - ee_rw < ee_size) {
          add_target((unsigned)(ctx->far.x - ee_rw));
        }
        for (int i = 0; i < 29; i++) {
          uintptr_t v = (uintptr_t)ctx->cpu_gprs[i].x;
          if (v >= ee_rx && v - ee_rx < ee_size) {
            add_target((unsigned)(v - ee_rx));
          } else if (v >= ee_rw && v - ee_rw < ee_size) {
            add_target((unsigned)(v - ee_rw));
          }
        }
        if (sp && (sp & 7) == 0) {
          int found = 0;
          for (int i = 0; i < 768 && found < 24; i++) {
            uintptr_t v = *(volatile uintptr_t*)(sp + (uintptr_t)i * 8);
            if (v >= ee_rx && v - ee_rx < ee_size && ((v - ee_rx) & 3) == 0) {
              add_target((unsigned)(v - ee_rx));
              found++;
            }
          }
        }
        // One pass over all symbols. They are sorted by NAME, not value, so "nearest
        // below" per target needs the full scan; all reads are bounded by the arena.
        for (unsigned sym = first; sym + 8 <= last; sym += 8) {
          unsigned value = *(volatile unsigned*)(ee_rw + sym);
          if (!value || value >= 0x8000000) {
            continue;
          }
          for (int t = 0; t < n_targets; t++) {
            if (value <= targets[t].addr && value > targets[t].best_val) {
              targets[t].best_val = value;
              targets[t].best_sym = sym;
            }
          }
        }
        static char sym_buf[4096];
        int sb = snprintf(sym_buf, sizeof(sym_buf), "=== SYMBOLS (jak1, %d targets) ===\n",
                          n_targets);
        for (int t = 0; t < n_targets && sb < (int)sizeof(sym_buf) - 96; t++) {
          const SymTarget& tg = targets[t];
          if (!tg.best_val) {
            continue;
          }
          unsigned strp = 0;
          if (tg.best_sym + SYM_INFO_OFFSET + 8 <= 0x8000000) {
            strp = *(volatile unsigned*)(ee_rw + tg.best_sym + SYM_INFO_OFFSET + 4);
          }
          char name[64];
          name[0] = '\0';
          int have_name = 0;
          if (strp >= 8 && strp + 8 < 0x8000000) {
            unsigned len = *(volatile unsigned*)(ee_rw + strp);
            if (len > 0 && len < 4096 && strp + 4 + len < 0x8000000) {
              unsigned copy = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
              const volatile char* src = (const volatile char*)(ee_rw + strp + 4);
              unsigned k;
              for (k = 0; k < copy; k++) {
                char c = src[k];
                if (c < 0x20 || c > 0x7e) {
                  break;
                }
                name[k] = c;
              }
              name[k] = '\0';
              have_name = k > 0;
            }
          }
          sb += snprintf(sym_buf + sb, sizeof(sym_buf) - sb, "EE+0x%x %s %s+0x%x\n", tg.addr,
                         tg.addr == tg.best_val ? "==" : "<=",
                         have_name ? name : "sym@EE+0x0?", tg.addr - tg.best_val);
        }
        switch_exc_write(sym_buf, sb);
      }
    }
  }

  // Also drop a one-line breadcrumb in the main run log so the timeline stays unified.
  // This one may block briefly, but by now the evidence is already safely on the card.
  switch_run_logf(
      "[FATAL] CPU exception desc=0x%x far=0x%llx pc_off=0x%llx lr_ee=0x%llx -- see "
      "gk_fatal.txt",
      (unsigned)ctx->error_desc, (unsigned long long)ctx->far.x,
      (unsigned long long)(ctx->pc.x - anchor),
      (unsigned long long)(ee_rx && ctx->lr.x >= ee_rx && ctx->lr.x - ee_rx < ee_size
                               ? ctx->lr.x - ee_rx
                               : 0));

  // Hand back to the system so Atmosphere still produces its own report/error screen.
  svcBreak(0 /*BreakReason_Panic*/, 0, 0);
  for (;;) {
  }
}


// ---------------------------------------------------------------------------------------
// FIX 7o -- trap the actual killer.
//
// 7n finally identified the death: Atmosphere wrote a *fatal_report* (not a crash report)
// with Result 0x1159 and all-zero registers. All-zero means fatalThrow() was called from
// userspace with nothing but a Result -- it is not a CPU exception, which is exactly why
// the 7n exception handler correctly stayed silent.
//
// Disassembling the binary found every fatalThrow call site, and all three live in Mesa's
// egl_switch.c: switch_swap_buffers, switch_st_framebuffer_validate and
// switch_create_window_surface. In switch_swap_buffers the sequence is literally
//
//     bl nwindowQueueBuffer ; cbnz w0, <+0xe0> ... <+0xe0>: bl fatalThrow
//
// so a failed present is an instant, context-free kill. 0x1159 = MAKERESULT(Module_Libnx,
// LibnxError_NotInitialized), and nwindowQueueBuffer returns exactly that from two places:
// a failed nwindowIsValid(nw) (bad/torn-down window), or `nw->cur_slot != slot` (queueing a
// buffer that was never dequeued). Those are very different bugs, so we must distinguish.
//
// --wrap (set in game/CMakeLists.txt) is the only way in: both symbols are strong in
// libnx.a/libEGL, so they cannot be overridden the way the weak hooks in 7e/7g/7h/7n were.
extern "C" {
void __real_fatalThrow(Result res) __attribute__((noreturn));
Result __real_nwindowQueueBuffer(NWindow* nw, s32 slot, const void* fence);
}

// Remembers the last nwindow rejection so the fatal trap can report the cause and the
// effect together, even though they happen in different functions.
static volatile u32 s_last_nwq_res = 0;
static volatile s32 s_last_nwq_slot = 0;
static volatile s32 s_last_nwq_cur_slot = 0;
static volatile u32 s_last_nwq_magic = 0;
static volatile u32 s_last_nwq_valid = 0;
static volatile u64 s_nwq_calls = 0;

extern "C" Result __wrap_nwindowQueueBuffer(NWindow* nw, s32 slot, const void* fence) {
  s_nwq_calls++;
  Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
  if (R_FAILED(rc)) {
    // Record only. Logging here would be a per-frame SD write on the render thread -- the
    // exact mistake 7m had to undo. The fatal trap below prints it, once.
    s_last_nwq_res = rc;
    s_last_nwq_slot = slot;
    s_last_nwq_cur_slot = nw ? nw->cur_slot : -999;
    s_last_nwq_magic = nw ? nw->magic : 0;
    s_last_nwq_valid = (nw && nwindowIsValid(nw)) ? 1 : 0;
  }
  return rc;
}

extern "C" void __wrap_fatalThrow(Result res) {
  const uintptr_t anchor = (uintptr_t)&switch_platform::get_memory_info;
  const uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  static char buf[1024];
  int n = snprintf(buf, sizeof(buf),
                   "\n=== FIX 7o fatalThrow TRAP ===\n"
                   "res=0x%x (%u-%04u)\n"
                   "caller_lr=0x%llx anchor=0x%llx lr_off=0x%llx\n"
                   "nwindowQueueBuffer: calls=%llu last_res=0x%x slot=%d cur_slot=%d "
                   "magic=0x%x is_valid=%u\n",
                   (unsigned)res, 2000u + R_MODULE(res), R_DESCRIPTION(res),
                   (unsigned long long)lr, (unsigned long long)anchor,
                   (unsigned long long)(lr - anchor), (unsigned long long)s_nwq_calls,
                   (unsigned)s_last_nwq_res, (int)s_last_nwq_slot, (int)s_last_nwq_cur_slot,
                   (unsigned)s_last_nwq_magic, (unsigned)s_last_nwq_valid);
  switch_exc_write(buf, n);
  switch_run_logf("[FATAL] fatalThrow res=0x%x lr_off=0x%llx nwq_last=0x%x slot=%d cur=%d valid=%u",
                  (unsigned)res, (unsigned long long)(lr - anchor), (unsigned)s_last_nwq_res,
                  (int)s_last_nwq_slot, (int)s_last_nwq_cur_slot, (unsigned)s_last_nwq_valid);
  __real_fatalThrow(res);
}

// FIX 7q -- threadExit() interposition. THE call site of the 0x1159 abort.
//
// The 7p fatal_report is Result 0x1159 = MAKERESULT(Module_Libnx=345,
// LibnxError_NotInitialized=8) raised through diagAbortWithResult -> svcBreak(Panic),
// which is exactly why Atmosphere records a *fatal_report* with an all-zero register
// context instead of a crash_report: it is a deliberate break, not a CPU exception.
//
// Disassembling every libnx object that can raise that specific Result narrows it to two
// families: the hid* accessors (abort when the HID sharedmem pointer is NULL, i.e. after
// hidExit -- but the 7h __appExit trap proves hidExit never runs) and threadExit(). The
// threadExit prologue matches byte for byte:
//
//     mrs x0, tpidrro_el0          ; TLS base
//     ldr x20, [x0, #488]          ; ThreadVars.thread_ptr  (TLS+0x1E0 +8)
//     cbz x20, +0x9c               ; -> mov w0,#0x1159 ; bl diagAbortWithResult
//
// So: some thread calls threadExit() with ThreadVars.thread_ptr == NULL. libnx leaves
// thread_ptr NULL for any thread it did not create through threadCreate() -- most
// notably the *main* thread. This wrapper runs before the check and records who.
extern "C" __attribute__((noreturn)) void __real_threadExit(void);

static std::atomic_int s_thread_exit_logged{0};

extern "C" __attribute__((noreturn)) void __wrap_threadExit(void) {
  const uintptr_t anchor = (uintptr_t)&switch_platform::get_memory_info;
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);

  // ThreadVars sits in the last 0x20 bytes of the 0x200-byte TLS block: magic at +0x1E0,
  // handle at +0x1E4, thread_ptr at +0x1E8.
  volatile u8* tls = (volatile u8*)armGetTls();
  u32 magic = *(volatile u32*)(tls + 0x1E0);
  u32 handle = *(volatile u32*)(tls + 0x1E4);
  void* thread_ptr = *(void* volatile*)(tls + 0x1E8);

  const bool fatal = (thread_ptr == nullptr) || (magic != 0x21545624u /* '!TV$' */);

  if (fatal) {
    // This thread is about to abort inside __real_threadExit. Last chance to name it.
    static char buf[768];
    int n = snprintf(buf, sizeof(buf),
                     "\n=== FIX 7q threadExit WILL ABORT (0x1159) ===\n"
                     "thread_ptr=%p magic=0x%08x (want 0x21545624) handle=0x%08x\n"
                     "caller_lr=%p lr_off=0x%llx anchor(get_memory_info)=%p\n"
                     "tls=%p stage=%u\n",
                     thread_ptr, (unsigned)magic, (unsigned)handle, (void*)lr,
                     (unsigned long long)(lr - anchor), (void*)anchor, (void*)tls,
                     g_switch_goal_stage.load(std::memory_order_relaxed));
    if (n > 0) {
      switch_exc_write(buf, n);
    }
    switch_run_logf("[FATAL] threadExit thread_ptr=NULL? %d magic=0x%08x lr_off=0x%llx",
                    (int)(thread_ptr == nullptr), (unsigned)magic,
                    (unsigned long long)(lr - anchor));
  } else if (s_thread_exit_logged.fetch_add(1, std::memory_order_relaxed) < 24) {
    // Budgeted census of healthy exits, so the log shows which threads die near the end.
    switch_run_logf("[thread] exit ok handle=0x%08x lr_off=0x%llx", (unsigned)handle,
                    (unsigned long long)(lr - anchor));
  }

  __real_threadExit();
}

// FIX 7r -- the last two bypasses.
//
// --wrap only rewrites *undefined* references, so two paths could still reach the system
// without passing any trap:
//   1. fatalThrowWithPolicy() -- fatalThrow() is only a thin shim over it, so anything
//      calling the worker directly skipped __wrap_fatalThrow entirely.
//   2. svcBreak() -- only reachable through our diagAbortWithResult override *if* the
//      caller went through diagAbortWithResult; a direct svcBreak was invisible.
// Between these two and the existing traps, every documented way for this process to ask
// the system to kill it is now instrumented.
extern "C" Result __real_fatalThrowWithPolicy(Result err, u32 type);
extern "C" Result __real_svcBreak(u32 breakReason, uintptr_t address, uintptr_t size);

extern "C" Result __wrap_fatalThrowWithPolicy(Result err, u32 type) {
  const uintptr_t anchor = (uintptr_t)&switch_platform::get_memory_info;
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  static char buf[384];
  int n = snprintf(buf, sizeof(buf),
                   "\n=== FIX 7r fatalThrowWithPolicy ===\nres=0x%08x module=%u desc=%u "
                   "policy=%u\nlr=%p lr_off=0x%llx stage=%u\n",
                   (unsigned)err, (unsigned)(err & 0x1FF), (unsigned)((err >> 9) & 0x1FFF),
                   (unsigned)type, (void*)lr, (unsigned long long)(lr - anchor),
                   g_switch_goal_stage.load(std::memory_order_relaxed));
  if (n > 0) {
    switch_exc_write(buf, n);
  }
  switch_run_logf("[FATAL] fatalThrowWithPolicy res=0x%08x policy=%u lr_off=0x%llx",
                  (unsigned)err, (unsigned)type, (unsigned long long)(lr - anchor));
  return __real_fatalThrowWithPolicy(err, type);
}

extern "C" Result __wrap_svcBreak(u32 breakReason, uintptr_t address, uintptr_t size) {
  const uintptr_t anchor = (uintptr_t)&switch_platform::get_memory_info;
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  // A break carries its payload by pointer; for the abort path that payload is the Result.
  unsigned payload = 0;
  if (address != 0 && size >= sizeof(unsigned)) {
    payload = *(volatile unsigned*)address;
  }
  static char buf[384];
  int n = snprintf(buf, sizeof(buf),
                   "\n=== FIX 7r svcBreak ===\nreason=%u payload=0x%08x size=%llu\n"
                   "lr=%p lr_off=0x%llx stage=%u\n",
                   (unsigned)breakReason, payload, (unsigned long long)size, (void*)lr,
                   (unsigned long long)(lr - anchor),
                   g_switch_goal_stage.load(std::memory_order_relaxed));
  if (n > 0) {
    switch_exc_write(buf, n);
  }
  switch_run_logf("[FATAL] svcBreak reason=%u payload=0x%08x lr_off=0x%llx",
                  (unsigned)breakReason, payload, (unsigned long long)(lr - anchor));
  return __real_svcBreak(breakReason, address, size);
}

#endif
