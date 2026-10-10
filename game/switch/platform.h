#pragma once
// Switch-specific platform helpers shared by the graphics and kernel layers.
#if defined(__SWITCH__)

/*!
 * FIX 7f -- A/B switch for the handheld resolution override.
 *
 * Set to 0 to behave exactly as the port did BEFORE the resolution fix: report and render
 * at whatever SDL/hbloader set up (1080p), never asking SDL to resize the nwindow
 * swapchain. The 7e run died ~0.35s into GOAL's dispatch loop -- the moment GOAL queries
 * the display size and validates its saved `game-size` -- with no exception and no exit
 * path, which is the signature of a libnx fatalThrow (e.g. a rejected swapchain/vi
 * operation). This toggle settles whether the override is that trigger.
 *
 * 1 = override on (720p handheld / 1080p docked), 0 = native, pre-fix behaviour.
 *
 * FIX 73c (AI-assisted): re-enabled to remove the handheld double-resample
 * (EASU 540->720 -> bilinear 720->1080 swapchain -> vi 1080->720 panel). 7f shipped TWO
 * mitigations at once: this override off, and capping pc_get_active_display_size's probe
 * logging. The uncapped kernel-thread SD writes were later confirmed as the 7c killer and
 * stay capped -- the override was very likely innocent. The remaining hazard of a 720p
 * swapchain is a post-creation resize (boot-time fullscreen application of the saved
 * 1920x1080 display settings, the 2026-09-11 crash class); that is now closed separately:
 * DisplayManager::set_display_mode/set_window_size are no-ops on Switch, so the swapchain
 * size is decided exactly once, here, at window creation. If a dispatch-start fatalThrow
 * returns with this enabled, flip back to 0 -- __wrap_nwindowQueueBuffer will have logged
 * the rejected vi operation in gk_fatal.txt.
 */
#define SWITCH_RES_OVERRIDE 1

namespace switch_platform {

struct DisplaySize {
  int w;
  int h;
};

/*!
 * Process memory usage straight from the kernel (svcGetInfo). `total` is the memory
 * the system granted this process; `used` is current usage. If an intro-time death
 * shows `used` climbing toward `total` right before the log goes silent, the system
 * killed us for memory -- which produces exactly the "fatal screen, no creport"
 * signature we saw on 2026-09-11.
 *
 * DECLARATION ONLY, same rule as get_display_size_for_operation_mode(): no libnx
 * header may be reachable from here (u128 collision with common_types.h).
 */
struct MemInfo {
  unsigned long long total;
  unsigned long long used;
};

MemInfo get_memory_info();

/*!
 * FIX 70 -- thread core diagnostics + pinning (PERF_PLAN_NEXT_AGENT.md step 1).
 *
 * Every thread in the runtime inherits the process core mask and the scheduler
 * is free to juggle them all across cores 0-2 (core 3 is the OS's). These pin
 * the CALLING thread and log a [cores] line to gk_run_log.txt:
 *   switch_pin_current_thread(role, core) - pin + report; call as the first
 *     thing a thread does. Returns the libnx Result (0 = success).
 *   switch_thread_core_report(role)       - report only (current core,
 *     preferred core, thread mask; process mask once per process).
 *   switch_core_diag_periodic(role)       - report at most every 10 s; call
 *     from a loop body. Safe mid-stream: switch_run_logf batches (FIX 40).
 *
 * FIX 70b: pinning is OPT-IN -- it only happens if sdmc:/gk_pin.txt exists.
 * The first hardware test with pinning always-on regressed frame pacing
 * (worst-ever [cam] HITCH rates) while leaving loader stats flat, so the
 * default is every thread floating on the process mask (pre-F70 behavior).
 * The [cores] reports above run unconditionally in both modes; with pinning
 * disabled they simply show where the scheduler put each thread.
 *
 * DECLARATION ONLY, same rule as above: no libnx header from this header.
 * Definitions in switch/platform.cpp (the one TU with <switch.h>).
 */
void switch_thread_core_report(const char* role);
unsigned int switch_pin_current_thread(const char* role, int core);
void switch_core_diag_periodic(const char* role);

/*!
 * FIX 71 -- CPU boost mode during blackout loads (PERF_PLAN_NEXT_AGENT.md step 2).
 *
 * appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad) switches the whole console to the
 * "fast load" performance configuration: CPU 1785 MHz instead of the usual 1020, GPU
 * clamped to its minimum clock. That trade is free while the screen is black and
 * terrible during play, so the scope is deliberately narrow: the loader turns it ON
 * when a blackout load starts and OFF the moment the load finishes
 * (Loader::update_frame_budget + Loader::update_blocking). It is never extended to
 * gameplay or the non-blackout streaming backlog -- unlike FIX 39's loadboost, which
 * only trades resolution, this one would throttle the GPU under the player's feet.
 *
 * Every transition logs one [boost] line (with the boost duration on OFF). No
 * watchdog thread: runtime thread creation is impossible on this console
 * (FIX 34c: ~4 MB free, no room for a stack -- F71 learned this the hard way
 * with an _exit(1) at the first boot blackout), and the OS restores the normal
 * clocks when the process exits anyway. See platform.cpp FIX 71/71b.
 *
 * Called once per frame by the render thread; idempotent and cheap (one atomic
 * exchange) when the state does not change.
 *
 * DECLARATION ONLY, same rule as above: no libnx header from this header.
 */
void switch_set_cpu_boost(bool on);

/*!
 * FIX 92 (AI-assisted): keep the CPU at >= 1785 MHz during play (GPU untouched); logs
 * [clk] lines on change. Call once per frame from the render thread; throttled to 2 s.
 */
void switch_clock_tick();

/*!
 * Size the game should present and render at for the current console operation mode:
 * 1280x720 in handheld, 1920x1080 docked.
 *
 * devkitPro's SDL reports the display mode that hbloader's takeover applet was set up
 * with (1080p), regardless of handheld/docked state -- so ask libnx directly instead.
 *
 * DECLARATION ONLY: libnx's <switch.h> typedefs `u128`, which collides with this
 * project's own `struct u128` from common/common_types.h in any translation unit that
 * ends up with both. So no libnx header may ever be included from this header (or any
 * header reachable from code using common_types.h). The definition lives in
 * switch/platform.cpp, which includes <switch.h> in complete isolation.
 */
DisplaySize get_display_size_for_operation_mode();

// FIX 73c: the size the window/swapchain was actually created with (see platform.cpp).
// Reads must never resize anything -- DECLARATION ONLY, same rule as above.
void set_created_window_size(int w, int h);
DisplaySize get_created_window_size();

/*!
 * FIX 7k -- applet message tracing.
 *
 * The 7j run proved the intro death is NOT a CPU fault (no creport) and NOT a libc exit
 * (neither the _exit nor the __appExit trap fires), even though the very same NRO fires
 * both under emulation. What is left is the process being terminated from outside: `am`
 * asking the applet to quit. Nothing in this codebase has ever registered an applet hook
 * or called appletMainLoop, so that entire channel was invisible. This registers a hook
 * that logs every applet message; if OnExitRequest shows up right before the log stops,
 * the mystery is solved.
 */
void install_applet_hook();

/*!
 * FIX 7p -- pump the applet message queue. MUST be called every frame from the render loop.
 *
 * This port never called appletMainLoop() anywhere, which is a real bug, not just a missing
 * probe: on Switch an applet has to acknowledge system messages (focus change, sleep,
 * operation-mode change, exit request). An applet that never answers is suspended by the OS
 * -- every thread stops at once, which is exactly the "freeze" that has been killing the
 * intro: no CPU exception, no exit path, no lock contention, all logging stopping on the
 * same millisecond, then a libnx-module fatal a few seconds later when the system gives up
 * and terminates the process. It also explains why the applet hook installed in 7k never
 * fired (libnx only dispatches hooks from this call) and why emulators never reproduce it
 * (they do not suspend applets).
 *
 * Returns false when the system has asked us to exit, so the caller can shut down cleanly.
 */
bool applet_pump();

/*!
 * FIX 105 -- true while inside the post-resume SDL-pump grace window (2s from
 * AppletHookType_OnResume).
 *
 * The 2026-09-10 resume-from-suspend crashes (x3) all died in hidGetTouchScreenStates
 * inside SDL's event pump on the first frames after the console woke -- SDL2 re-reads the
 * hid shared memory on every poll and that memory is not settled yet right after resume.
 * While this returns true the caller must NOT call SDL_PollEvent / SDL_PumpEvents.
 * applet_pump() is unaffected (it is a separate libnx call), so exit requests are still
 * honored during the window. The historical workaround ("don't suspend mid-game") stays
 * valid as a fallback; this makes the resume path itself safe.
 */
bool sdl_pump_grace_active();

}  // namespace switch_platform

/*!
 * FIX 7r -- the fatal channel (sdmc:/gk_fatal.txt), declared at global scope because it is
 * deliberately independent of every other logging facility in the port.
 *
 * switch_fatal_channel_open() opens the descriptor once at startup so that the death-time
 * write path is a bare write()+fsync() with no open(), no allocation and no fsdev lookup.
 * switch_fatal_channel_heartbeat() then writes a bounded series of "[chan]" lines around
 * the time of the crash, which validates the instrument itself: if those lines run right up
 * to the moment of death and no trap line follows, the traps genuinely did not fire and the
 * fatal is being raised outside our module.
 */
void switch_fatal_channel_open();

/*!
 * FIX 7s -- bring up the live network log (see switch_net_log_write in run_log.h).
 * Safe to call unconditionally: if the SD card has no gk_log_host.txt, or the listener is
 * not running, or the network is down, it logs why and returns, leaving the SD log as the
 * only channel. Must be called after switch_run_logf's fd exists so the reason is recorded.
 */
void switch_net_log_init();
void switch_fatal_channel_heartbeat(double t, unsigned stage, unsigned iter);

#endif

