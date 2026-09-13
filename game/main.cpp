/*!
 * @file main.cpp
 * Main for the game. Launches the runtime.
 */

#define STBI_WINDOWS_UTF8

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "runtime.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/dialogs.h"
#include "common/util/os.h"
#include "common/util/term_util.h"
#include "common/util/unicode_util.h"
#include "common/versions/versions.h"

#include "game/common/game_common_types.h"
#include "graphics/gfx_test.h"

#include "third-party/CLI11.hpp"

#if defined(__SWITCH__)
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include "game/switch/boot_log.h"
#include "game/switch/platform.h"
#include "game/switch/run_log.h"
#include "game/switch/safe_stdout.h"

static void boot_log_main(const char* msg) {
  switch_boot_log(msg);
}
#endif

#ifdef _WIN32
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

/*!
 * Set up logging system to log to file.
 * @param verbose : should we print debug-level messages to stdout?
 */
void setup_logging(const std::string& game_name, bool verbose, bool disable_ansi_colors) {
  lg::set_file(game_name);
  if (verbose) {
    lg::set_file_level(lg::level::debug);
    lg::set_stdout_level(lg::level::debug);
    lg::set_flush_level(lg::level::debug);
  } else {
    lg::set_file_level(lg::level::debug);
    lg::set_stdout_level(lg::level::info);
    lg::set_flush_level(lg::level::warn);
  }
#if defined(__SWITCH__)
  // Confirmed via boot-order tracing: the very first vprintf(stdout)/fflush(stdout) issued from a
  // background SystemThread (the EE thread's first Msg() call in InitParms) hangs indefinitely on
  // this platform, even though the same calls succeed fine from the main thread earlier in boot.
  // The kernel runs almost entirely off the EE thread from here on, so keep logging file-only.
  lg::set_stdout_level(lg::level::off);
#endif
  if (disable_ansi_colors) {
    lg::disable_ansi_colors();
  }
  lg::initialize();
}

std::string game_arg_documentation() {
  // clang-format off
  std::string output = fmt::format(fmt::emphasis::bold, "Game Args (passed through to the game runtime after '--')\n");
  output += fmt::format(fmt::fg(fmt::color::gray), "Order matters, some args will negate others (see kmachine.cpp for details)\n");
  output += fmt::format(fmt::fg(fmt::color::gray), "Args with `*` are not well supported\n\n");
  // Common args
  output += fmt::format(fmt::emphasis::bold, "Common:\n");
  output += "  -cd          * Use the DVD drive for everything. This is how the game runs in retail\n";
  output += "  -cddata      * Use the DVD drive for everything but IOP modules\n";
  output += "  -deviso      * One of two modes for testing without the need for DVDs\n";
  output += "  -fakeiso       The other of two modes for testing without the need for DVDs\n";
  output += "  -boot          Used to set GOAL up for running the game in retail mode\n";
  output += "  -debug         Used to set GOAL up for debugging/development\n";
  output += "  -debug-mem     Used to set up GOAL in debug mode, but not to load debug-segments\n";
  output += "  -nokernel      An added mode to allow booting without a KERNEL.CGO for testing\n";
  output += "  -nosound       An added mode to allow booting without sound for testing\n";
  output += "  -level [name]  Used to inform the game to boot a specific level the default level is `#f`\n";
  // Jak 1 Related
  output += fmt::format(fmt::emphasis::bold | fmt::fg(fmt::color::orange), "Jak 1:\n");
  output += "  -demo          Boot the game in demo mode\n";
  // Jak 2 only
  output += fmt::format(fmt::emphasis::bold | fmt::fg(fmt::color::purple), "Jak 2:\n");
  output += "  -demo          Boot the game in demo mode\n";
  output += "  -kiosk         Boot the game in kiosk demo mode\n";
  output += "  -preview       Boot the game in preview demo mode\n";
  output += "  -debug-boot    Used to boot the game in retail mode, but with debug segments\n";
  output += "  -user [name]   Specify the debugging username, the default is `unknown`\n";
  output += "  -art [name]    Specify the art-group name to set `DebugBootArtGroup`, there is no default\n";
  // clang-format on
  return output;
}

/*!
 * Entry point for the game.
 */
int main(int argc, char** argv) {
#if defined(__SWITCH__)
  boot_log_main("[main] entered\n");
  // newlib resolves the locale's __mbtowc through the reent struct when formatting; on this
  // toolchain it reads back null until the locale is set explicitly, and every raw printf() in
  // the kernel/overlord layers then branches through a null pointer inside _svfiprintf_r.
  setlocale(LC_ALL, "C");
  boot_log_main("[main] locale initialized\n");
  // Confirmed via boot-order tracing: real stdout hangs indefinitely on the first write/flush
  // issued from any thread other than main (see setup_logging()'s lg::set_stdout_level(off) for
  // the lg:: side of this). Plenty of code throughout the kernel/overlord/sce layers still calls
  // raw printf()/fprintf(stdout, ...) directly though, bypassing lg:: entirely -- redirecting the
  // actual FILE* stdout points at to a plain SD card file up front fixes all of those call sites
  // in one place instead of hunting each one down.
  //
  // FIX 7t: this used to be freopen()+setvbuf(_IONBF), i.e. an *unbuffered* plain file, so
  // every print became a direct fsdev write on whichever thread printed. fsdev is not
  // thread-safe (see FsLock.h), so a print racing the ISO thread could come up short --
  // and a short fwrite makes fmt throw system_error, which nothing catches, which aborts
  // the process. That was the intro "freeze": see game/switch/safe_stdout.h for the full
  // stack. The replacement serialises on SWITCH_FS_LOCK() and never reports failure.
  switch_install_safe_stdout();
  boot_log_main("[main] stdout redirected to sdmc:/gk_stdout.txt (safe sink)\n");
#endif
  ArgumentGuard u8_guard(argc, argv);
#if defined(__SWITCH__)
  boot_log_main("[main] ArgumentGuard constructed\n");
#endif

  // CLI flags
  bool show_version = false;
  std::string game_name = "jak1";
  bool verbose_logging = false;
  bool disable_avx2 = false;
  bool disable_display = false;
  bool enable_profiling = false;
  bool enable_portable = false;
  bool disable_save_location_override = false;
  std::string profile_until_event = "";
  std::string gpu_test = "";
  std::string gpu_test_out_path = "";
  int port_number = -1;
  fs::path project_path_override;
  fs::path user_config_dir_override;
  std::vector<std::string> game_args;
#if defined(__SWITCH__)
  // CLI11 is skipped entirely on Switch (see below), which normally supplies these via
  // `-- -boot -fakeiso -debug` (see Taskfile.yml's boot-game task, and kmachine.cpp's
  // -fakeiso/-boot handling) -- without them the kernel never actually boots into the game.
  // -debug is deliberately omitted: it sets MasterDebug, which makes InitMachine() call
  // InitGoalProto() to open a DECI2 handshake expecting a real goalc REPL to connect -- nothing
  // will ever connect to a standalone .nro, so that stalls boot forever.
  game_args = {"-boot", "-fakeiso"};
#endif
#if !defined(__SWITCH__)
  // Switch homebrew has no meaningful command line -- it's launched from a menu, not a shell --
  // so there's nothing for CLI11 to parse here. The variables above already carry the right
  // defaults for that case, so just skip the whole CLI11 app entirely rather than feeding it an
  // effectively-empty argv.
  CLI::App app{"OpenGOAL Game Runtime"};
  app.add_flag("--version", show_version, "Display the built revision");
  app.add_option("-g,--game", game_name, "The game name: 'jak1' or 'jak2'");
  app.add_flag("-v,--verbose", verbose_logging, "Enable verbose logging on stdout");
  app.add_flag(
      "--port", port_number,
      "Specify port number for listener connection (default is 8112 for Jak 1 and 8113 for Jak 2)");
  app.add_flag("--no-avx2", disable_avx2, "Disable AVX2 for testing");
  app.add_flag("--no-display", disable_display, "Disable video display");
  app.add_flag("--profile", enable_profiling, "Enables profiling immediately from startup");
  app.add_flag("--portable", enable_portable,
               "Save settings and saves relative to the game's executable, takes precedence over "
               "--config-path");
  app.add_flag("--disable_save_location_override", disable_save_location_override,
               "If --config-path is provided along with this flag, saves will still be loaded and "
               "stored to the default location");
  app.add_option("--profile-until-event", profile_until_event,
                 "Stops recording profile events once an event with this name is seen");
  app.add_option("--gpu-test", gpu_test,
                 "Tests for minimum graphics requirements.  Valid Options are: [opengl]");
  app.add_option("--gpu-test-out-path", gpu_test_out_path,
                 "Where to store the gpu test result file");
  app.add_option("--proj-path", project_path_override,
                 "Specify the location of the 'data/' folder");
  app.add_option("--config-path", user_config_dir_override,
                 "Override the location where all user configuration and saves are saved");
  app.footer(game_arg_documentation());
  app.add_option("Game Args", game_args,
                 "Remaining arguments (after '--') that are passed-through to the game itself");
  define_common_cli_arguments(app);
  app.allow_extras();
  CLI11_PARSE(app, argc, argv);
#else
  bool _cli_flag_disable_ansi = false;
  boot_log_main("[main] CLI11 skipped on Switch\n");
  // CLI11 is also what would supply --portable, so force it here: Switch has no
  // HOME/XDG/APPDATA env vars, and without the override the user config dir resolves
  // to a CWD-relative path that never materializes -- every save attempt fails with
  // the in-game "PLEASE CHECK THE MEMORY CARD (PS2)" error. Portable mode puts
  // settings and saves next to the gk.nro, at sdmc:/switch/jak1/OpenGOAL/...
  enable_portable = true;
#endif

  // Log the version the game is compiled against so we don't have to guess
  lg::info("Compiled Version: {}", build_revision());
#if defined(__SWITCH__)
  boot_log_main("[main] lg::info(Compiled Version) done\n");
#endif

  // Override the user's config dir, potentially (either because it was explicitly provided
  // or because it's portable mode)
  if (enable_portable) {
    lg::info("Portable mod enabled");
    user_config_dir_override = fs::path(file_util::get_current_executable_path()).parent_path();
  }
  if (!user_config_dir_override.empty()) {
    lg::info("Overriding config directory with: {}", user_config_dir_override.string());
    file_util::override_user_config_dir(user_config_dir_override, !disable_save_location_override);
  }

  if (show_version) {
    lg::print("{}", build_revision());
    return 0;
  }

  if (!gpu_test.empty() && !gpu_test_out_path.empty()) {
    const auto output = tests::run_gpu_test(gpu_test);
    json data = output;
    try {
      file_util::write_text_file(gpu_test_out_path, data.dump(2));
    } catch (std::exception& e) {
      return 1;
    }
    return 0;
  }

  prof().set_enable(enable_profiling);
  prof().set_waiting_for_event(profile_until_event);

  // Create struct with all non-kmachine handled args to pass to the runtime
  GameLaunchOptions game_options;
  game_options.disable_display = disable_display;
  game_options.game_version = game_name_to_version(game_name);
  game_options.server_port =
      port_number == -1 ? DECI2_PORT - 1 + (int)game_options.game_version : port_number;

  // Figure out if the CPU has AVX2 to enable higher performance AVX2 versions of functions.
  setup_cpu_info();
#if defined(__SWITCH__)
  boot_log_main("[main] setup_cpu_info done\n");
#endif
  // If the CPU doesn't have AVX, GOAL code won't work and we exit -- except on Switch, where (as
  // with Apple Silicon) SIMD goes through sse2neon.h's translation layer instead of real AVX, so
  // the presence of hardware AVX was never the right thing to gate on here in the first place.
#if !defined(__SWITCH__)
  if (!get_cpu_info().has_avx) {
// Check if we are on a modern enough version of macOS so that AVX can be
// emulated via rosetta
#ifdef __APPLE__
    auto macos_major_version = get_macos_major_version();
    if (macos_major_version < 15.0) {
      lg::info(
          "Your CPU does not support AVX. But the newer version of Rosetta supports it, update to "
          "atleast Sequoia to run OpenGOAL!");
      dialogs::create_error_message_dialog(
          "Unmet Requirements",
          "Your CPU does not support AVX. But the newer version of Rosetta supports it, update to "
          "atleast Sequoia to run OpenGOAL!");
      return -1;
    }
#else
    lg::info("Your CPU does not support AVX, which is required for OpenGOAL.");
    dialogs::create_error_message_dialog(
        "Unmet Requirements", "Your CPU does not support AVX, which is required for OpenGOAL.");
    return -1;
#endif
  }
#endif  // !defined(__SWITCH__)

  // set up file paths for resources. This is the full repository when developing, and the data
  // directory (a subset of the full repo) in release versions
  if (project_path_override.empty()) {
    lg::info("No project path provided, looking for data/ folder in current directory");
    if (!file_util::setup_project_path({})) {
      return 1;
    }
  } else if (!file_util::setup_project_path(project_path_override)) {
    return 1;
  }

  if (disable_avx2) {
    // for debugging the non-avx2 code paths, there's a flag to manually disable.
    lg::info("Note: AVX2 code has been manually disabled.");
    get_cpu_info().has_avx2 = false;
  }

#ifndef __AVX2__
  if (get_cpu_info().has_avx2) {
    // printf("Note: your CPU supports AVX2, but this build was not compiled with AVX2 support\n");
    get_cpu_info().has_avx2 = false;
  }
#endif

  if (get_cpu_info().has_avx2) {
    lg::info("AVX2 mode enabled");
  } else {
    lg::info("AVX2 mode disabled");
  }

  try {
    setup_logging(game_name, verbose_logging, _cli_flag_disable_ansi);
  } catch (const std::exception& e) {
    lg::error("Failed to setup logging: {}", e.what());
    return 1;
  }

  bool force_debug_next_time = false;
  // always start with an empty arg, as internally kmachine starts at `1` not `0`
  std::vector<const char*> arg_ptrs = {""};
  for (auto& str : game_args) {
    arg_ptrs.push_back(str.data());
  }

  while (true) {
    if (force_debug_next_time) {
      // I'd like to check and not add duplicates, unfortunately since the game
      // cares about ordering...that's likely error prone if the user passed args in the wrong order
      // ie. -debug -boot (we'd skip adding things, but the order would be wrong).
      game_args.push_back("-boot");
      game_args.push_back("-debug");
      force_debug_next_time = false;
      arg_ptrs = {""};  // see above for rationale
      for (auto& str : game_args) {
        arg_ptrs.push_back(str.data());
      }
    }

    // run the runtime in a loop so we can reset the game and have it restart cleanly
    lg::info("OpenGOAL Runtime {}.{}", versions::GOAL_VERSION_MAJOR, versions::GOAL_VERSION_MINOR);
    try {
      MasterExit = RuntimeExitStatus::RUNNING;
#if defined(__SWITCH__)
      boot_log_main("[main] about to call exec_runtime\n");
      // FIX 7b: force the run-log fd open NOW, before boot. boot_log proved an early-opened
      // fd keeps working even when later opens fail (lazy open after boot-complete was
      // silently blind in the first FIX 7 run: probes ran, gk_run_log.txt never appeared).
      switch_run_logf("session start 7x (fps: cache memcard headers, stop 0.5MB/frame SD reads)");
      // FIX 7s: connect the live log before anything interesting happens, so the whole
      // boot is visible on the development machine and no SD round trip is needed.
      switch_net_log_init();
      // FIX 7r: open the fatal channel now, while fsdev is known-good, so the death-time
      // write path needs no open(). Every "the trap never fired" conclusion so far depends
      // on this channel working at death time, which has never actually been verified.
      switch_fatal_channel_open();
      switch_platform::install_applet_hook();
      // FIX 7d: the 7c watchdog thread is GONE. Its 250ms write+fsync storm started at
      // T+0.25 -- exactly while main was inside lg's log-file rotation, an fsdev path
      // that does not take SWITCH_FS_LOCK() (see FsLock.h's race catalogue). Result:
      // main hung forever in an fsp-srv wait before InitMachine ever logged, the run-log
      // fd went dead (every write silently EBADF -- see run_log.h), and ~60s later the
      // corrupted state detonated as an Instruction Abort inside the watchdog thread
      // itself (creport 01789175410). The gfx/render thread -- the writer that provably
      // survived all of FIX 7b -- carries the fast probe now (opengl.cpp). No thread in
      // this process touches the SD before SDL init completes.
      {
        // get_memory_info was also querying the wrong InfoTypes in 7c (18/19; 18 is
        // UserExceptionContextAddr) -- fixed in platform.cpp, so this T+0 budget line is
        // real now.
        const auto mem0 = switch_platform::get_memory_info();
        switch_run_logf("session start mem_used=%lluKB mem_total=%lluKB",
                        (unsigned long long)mem0.used / 1024,
                        (unsigned long long)mem0.total / 1024);
        // FIX 7d: creport cannot symbolize an NRO loaded by hbloader (tonight's module
        // list contains only "hbl"). Print two known symbols so any future crash report
        // maps onto this exact ELF by subtraction: load_base = printed_ptr - nm_addr.
        switch_run_logf("symbol anchor exec_runtime=%p get_memory_info=%p",
                        (void*)&exec_runtime, (void*)&switch_platform::get_memory_info);
        // FIX 7e: registered last => runs FIRST (atexit is LIFO). The 7d run died with a
        // fatal_reports 0x1159 entry (post-exit HID artifact => __appExit ran => an exit
        // path executed) yet NONE of the exit probes logged -- including the pad-teardown
        // atexit registered back at input-manager init, even though the gfx thread wrote
        // successfully 0.2s earlier. So the exit either bypasses the atexit list entirely
        // (direct _exit/svcExitProcess -- trapped in platform.cpp now) or something in the
        // list hangs before reaching the early handlers. This line tells the two apart:
        //   "[exit] atexit begin" seen + "[exit] _exit(...)" seen  -> full exit() path
        //   only "[exit] _exit(...)"                              -> direct _exit() caller
        //   neither                                                -> svcExitProcess / external kill
        std::atexit([] { switch_run_logf("[exit] atexit begin -- exit() was called"); });
      }
#endif
      auto exit_status = exec_runtime(game_options, arg_ptrs.size(), arg_ptrs.data());
#if defined(__SWITCH__)
      switch_run_logf("main: exec_runtime returned %d", (int)exit_status);
#endif
      switch (exit_status) {
        case RuntimeExitStatus::EXIT:
          return 0;
        case RuntimeExitStatus::RESTART_RUNTIME:
        case RuntimeExitStatus::RUNNING:
          break;
        case RuntimeExitStatus::RESTART_IN_DEBUG:
          force_debug_next_time = true;
          break;
      }
    } catch (std::exception& ex) {
      lg::error("Unexpected exception occurred - {}", ex.what());
      throw ex;
    }
  }
  return 0;
}
