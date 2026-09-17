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
 */
#define SWITCH_RES_OVERRIDE 0

/*!
 * FIX 30 -- report the operation-mode size as the *active display size* to GOAL.
 *
 * This is the other half of what SWITCH_RES_OVERRIDE used to gate, split out because the
 * two halves have completely different risk profiles:
 *
 *  - SWITCH_RES_OVERRIDE asks SDL to resize hbloader's nwindow swapchain. That is the
 *    operation FIX 7f suspected of the fatalThrow-shaped death. The hypothesis was
 *    REFUTED (7f died identically with it compiled out, and before GOAL touched any
 *    display code) but the swapchain is still left alone here -- there is nothing to gain
 *    from resizing it, since the present is a cheap blit.
 *
 *  - This flag only changes the number pc_get_active_display_size() hands to GOAL, which
 *    GOAL uses to pick the DEFAULT `game-size` (pckernel-h.gc reset-graphics) and as the
 *    fallback when a saved `game-size` is not a supported resolution
 *    (pckernel-common.gc). A saved, supported choice is still honoured, so unlike the
 *    reverted FIX 16 this never overrides the user's Game Resolution setting -- it only
 *    stops the *default* being 1080p on a 720p panel.
 *
 * `game-size` drives the off-screen render FBO (pc_set_game_resolution ->
 * Gfx::g_global_settings.game_res_w/h), so this is the real throughput lever: 1280x720 is
 * 44% of the fragment work of 1920x1080.
 */
#define SWITCH_GAME_RES_FOR_MODE 1

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

