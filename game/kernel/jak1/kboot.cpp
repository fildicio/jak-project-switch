/*!
 * @file kboot.cpp
 * GOAL Boot.  Contains the "main" function to launch GOAL runtime
 * DONE!
 */

#include "kboot.h"

#include <chrono>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <thread>

#include "common/common_types.h"
#include "common/log/log.h"
#include "common/util/Timer.h"

#include "game/common/game_common_types.h"
#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/ksocket.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kmachine.h"
#include "game/sce/libscf.h"
#include "game/switch/platform.h"
#include "game/switch/run_log.h"

using namespace ee;

#if defined(__SWITCH__)
#include <fcntl.h>
#include <unistd.h>
#include "game/switch/boot_log.h"

static void boot_log_km(const char* msg) {
  switch_boot_log(msg);
}
#endif

namespace jak1 {
VideoMode BootVideoMode;

void kboot_init_globals() {}

/*!
 * Launch the GOAL Kernel (EE).
 * DONE!
 * See InitParms for launch argument details.
 * @param argc : argument count
 * @param argv : argument list
 * @return 0 on success, otherwise failure.
 *
 * CHANGES:
 * Added InitParms call to handle command line arguments
 * Removed hard-coded debug mode disable
 * Renamed from `main` to `goal_main`
 * Add call to sceDeci2Reset when GOAL shuts down.
 */
s32 goal_main(int argc, const char* const* argv) {
#if defined(__SWITCH__)
  boot_log_km("[goal_main] entered\n");
#endif
  // Initialize global variables based on command line parameters
  // This call is not present in the retail version of the game
  // but the function is, and it likely goes here.
  InitParms(argc, argv);
#if defined(__SWITCH__)
  boot_log_km("[goal_main] InitParms done\n");
#endif

  // Initialize CRC32 table for string hashing
  init_crc();

  // NTSC V1, NTSC v2, PAL CD Demo, PAL Retail
  // Set up game configurations
  masterConfig.aspect = (u16)sceScfGetAspect();
  masterConfig.language = (u16)sceScfGetLanguage();
  masterConfig.inactive_timeout = 0;  // demo thing
  masterConfig.timeout = 0;           // demo thing
  masterConfig.volume = 100;

  // Set up language configuration
  if (masterConfig.language == SCE_SPANISH_LANGUAGE) {
    masterConfig.language = (u16)Language::Spanish;
  } else if (masterConfig.language == SCE_FRENCH_LANGUAGE) {
    masterConfig.language = (u16)Language::French;
  } else if (masterConfig.language == SCE_GERMAN_LANGUAGE) {
    masterConfig.language = (u16)Language::German;
  } else if (masterConfig.language == SCE_ITALIAN_LANGUAGE) {
    masterConfig.language = (u16)Language::Italian;
  } else if (masterConfig.language == SCE_JAPANESE_LANGUAGE) {
    // Note: this case was added so it is easier to test Japanese fonts.
    masterConfig.language = (u16)Language::Japanese;
  } else {
    // pick english by default, if language is not supported.
    masterConfig.language = (u16)Language::English;
  }

  // Set up aspect ratio override in demo
  if (!strcmp(DebugBootMessage, "demo") || !strcmp(DebugBootMessage, "demo-shared")) {
    masterConfig.aspect = SCE_ASPECT_FULL;
  }

  // In retail game, disable debugging modes, and force on DiskBoot
  // MasterDebug = 0;
  // DiskBoot = 1;
  // DebugSegment = 0;

  // Launch GOAL!
#if defined(__SWITCH__)
  boot_log_km("[goal_main] about to InitMachine\n");
#endif
  if (InitMachine() >= 0) {    // init kernel
#if defined(__SWITCH__)
    boot_log_km("[goal_main] InitMachine ok, about to KernelCheckAndDispatch\n");
    switch_boot_log_finish();
#endif
    KernelCheckAndDispatch();  // run kernel
#if defined(__SWITCH__)
    boot_log_km("[goal_main] KernelCheckAndDispatch returned\n");
    switch_run_logf("goal_main: KernelCheckAndDispatch returned, MasterExit=%d",
                    (int)MasterExit);
#endif
    ShutdownMachine();         // kernel died, we should too.
  } else {
#if defined(__SWITCH__)
    boot_log_km("[goal_main] InitMachine FAILED\n");
#endif
    fprintf(stderr, "InitMachine failed\n");
    exit(1);
  }

  return 0;
}

/*!
 * Main loop to dispatch the GOAL kernel.
 */
void KernelCheckAndDispatch() {
  u64 goal_stack = u64(g_ee_main_mem) + EE_MAIN_MEM_SIZE - GOAL_STACK_TOP_OFFSET;

  while (MasterExit == RuntimeExitStatus::RUNNING) {
#if defined(__SWITCH__)
    {
      // FIX 7: post-boot blind zone. Three intro deaths (2026-09-11) left no creport
      // at all -- the process exits silently. A slow heartbeat shows how far the
      // kernel got, and memory pressure reveals a system-side OOM kill.
      // FIX 7c: the 7b run died ~1s after entering this loop, before the first 2s
      // heartbeat could fire. Log loop entry once, then beat at 500ms for the first
      // 60s of the loop before relaxing to 2s.
      static bool s_logged_loop_entry = false;
      static const auto s_loop_t0 = std::chrono::steady_clock::now();
      if (!s_logged_loop_entry) {
        s_logged_loop_entry = true;
        switch_run_logf("kernel loop: first iteration");
      }
      static auto s_last_beat = std::chrono::steady_clock::now();
      const auto now = std::chrono::steady_clock::now();
      const long long loop_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - s_loop_t0).count();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_beat).count() >=
          (loop_ms < 60000 ? 500 : 2000)) {
        s_last_beat = now;
        const auto mem = switch_platform::get_memory_info();
        switch_run_logf("kernel heartbeat MasterExit=%d mem_used=%lluKB mem_total=%lluKB",
                        (int)MasterExit, (unsigned long long)mem.used / 1024,
                        (unsigned long long)mem.total / 1024);
      }
    }
#endif
    // FIX 7g: breadcrumbs. Stores only -- the gfx thread does the writing (run_log.h).
    switch_goal_tick();
    switch_goal_stage(SWITCH_STAGE_LOOP_TOP);
    // try to get a message from the listener, and process it if needed
    switch_goal_stage(SWITCH_STAGE_LISTENER_WAIT);
    Ptr<char> new_message = WaitForMessageAndAck();
    if (new_message.offset) {
      switch_goal_stage(SWITCH_STAGE_LISTENER_PROCESS);
      ProcessListenerMessage(new_message);
    }

    // remember the old listener function
    auto old_listener = ListenerFunction->value;
    // dispatch the kernel
    //(**kernel_dispatcher)();

    Timer kernel_dispatch_timer;
    if (MasterUseKernel) {
      // use the GOAL kernel. Stage 4 means GOAL code itself is executing: if the last
      // [gfx] line of a dead run says stage=4, the killer is inside GOAL/its PC hooks.
      switch_goal_stage(SWITCH_STAGE_GOAL_CALL);
      call_goal_on_stack(Ptr<Function>(kernel_dispatcher->value), goal_stack, s7.offset,
                         g_ee_main_mem);
      switch_goal_stage(SWITCH_STAGE_GOAL_RETURNED);
    } else {
      // use a hack to just run the listener function if there's no GOAL kernel.
      if (ListenerFunction->value != s7.offset) {
        auto result = call_goal_on_stack(Ptr<Function>(ListenerFunction->value), goal_stack,
                                         s7.offset, g_ee_main_mem);
#ifdef __linux__
        cprintf("%ld\n", result);
#else
        cprintf("%lld\n", result);
#endif
        ListenerFunction->value = s7.offset;
      }
    }

    auto time_ms = kernel_dispatch_timer.getMs();
    if (time_ms > 50) {
      lg::print("Kernel dispatch time: {:.3f} ms\n", time_ms);
    }

    switch_goal_stage(SWITCH_STAGE_CLEAR_PENDING);
    ClearPending();

    // if the listener function changed, it means the kernel ran it, so we should notify compiler.
    if (MasterDebug && ListenerFunction->value != old_listener) {
      SendAck();
    }

    if (time_ms < 4) {
      std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }
  }
#if defined(__SWITCH__)
  switch_run_logf("KernelCheckAndDispatch loop ended, MasterExit=%d", (int)MasterExit);
#endif
}

/*!
 * Stop running the GOAL Kernel.
 * DONE, EXACT
 */
void KernelShutdown() {
#if defined(__SWITCH__)
  // FIX 7: this is the GOAL-side clean-shutdown request (kernel-shutdown symbol).
  // If an intro death is preceded by this line, the game itself asked to exit.
  switch_run_logf("KernelShutdown() called from GOAL -- clean shutdown requested");
#endif
  MasterExit = RuntimeExitStatus::EXIT;  // GOAL Kernel Dispatch loop will stop now.
}
}  // namespace jak1
