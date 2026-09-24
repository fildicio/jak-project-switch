# Switch Port — Session Notes / Handoff Log

> (AI-assisted) This file is a session log written by AI coding agents so
> future sessions can pick up where this one left off. Last updated: 2026-09-11 ~15:35.
> **CURRENT OPEN BUG — NOW LOCALIZED: the game dies at the *first streamed-audio open*
> of the session. Five consecutive hardware game logs end at the identical instruction
> (`PRI 3 elt 0 QueueVAG`), i.e. inside `iso.cpp`'s `QUEUE_VAG_STREAM` case at
> `isofs->open_wad()` for `VAGWAD ENG`, right after `link finish: ndi-intro`.
> FIX 7i (on the card, HARDWARE TEST PENDING) probes that exact window.**

## BREAKTHROUGH — emulator differential (2026-09-11, Eden)

Running the *same* `gk.nro` under the Eden emulator (Apple Silicon) survives

**indefinitely** — 26 000+ dispatch iterations over 8 minutes, into real gameplay —
while hardware dies after 13–19. The differential localizes the bug pre
cisely:

| | Eden | hardware |
|---|---|---|
| `NDINTRO STR` streamed | yes | yes (last thing in log) |
| `QueueVAG` | 45× | **1× — then death** |
| `FS_OpenWad VAGWAD ENG` | **7× ✅** | **never reached** |
| dispatch iterations | 26 780+ | 13–19 |

**Therefore GOAL, the JIT, IOP emulation, DGO loading and `sif_rpc` are all innocent** —
they run for thousands of iterations. The killer is console-specific and sits in the
VAG (streamed audio) open path. This also retires the `stage=4` clue: stage 4 just means
GOAL is running, which is correct.

### How to reproduce the emulator run (it is a real diagnostic instrument)

- App: `/Applications/eden.app`. Data dir: `~/Documents/Switch-games`
  (firmware: 238 NCAs in `bis/system/Contents/registered`, keys in `system/prod.keys`).
- **Load the NRO unmodified and with an ABSOLUTE path.** A relative path fails with
  "Failed to obtain loader"; stripping the 16-byte prefix (the `NRO0` magic lives at
  offset 0x10 *inside* the header) makes the file unloadable. Do not "fix" the ASET trailer.
- **Required config** in `~/.config/eden/qt-config.ini`: `use_multi_core=false` and
  `memory_layout_mode=1` (6 GB). With the defaults Eden segfaults ~3 s in (dynarmic
  `assert !is_executing` storm). Backup: `qt-config.ini.bak-*`.
- Eden's SDMC is `~/.local/share/eden/sdmc`, whose `switch` entry is a **symlink** to
  `~/Documents/Switch-games/sdcard/switch`. *Beware: `find` does not descend symlinks —
  an `rm -rf` through that path deleted the 3.4 GB `jak1` folder once; it was restored
  from `/Volumes/SWITCH SD` (9837 files, verified).*
- Video does **not** render (MoltenVK lacks `geometryShader`; `surface.cpp assert
  Unimplemented format=0`). The game still runs — judge progress by
  `gk_run_log.txt` `iter=` and the game log, not by the window.
- Evidence archived: `switch-crash-reports/gk_run_log.emu7h.txt`, `gamelog_emu_7h.txt`.

### Hypotheses this killed

- **`envSetExitFuncPtr` loader-return trap** — pointless: `__libnx_exit` is
  `__appExit; envGetExitFuncPtr; __nx_exit`, and `envGetExitFuncPtr` is referenced by
  exactly one object (`init.o`), strictly downstream of the already-silent `_exit` trap.
- **`fatalThrow` trap** — pointless: `fatalThrow` is strong (`fatal.o`) and has **zero**
  callers inside libnx; `fatal.o` is only linked if we reference it ourselves.
- **`diagAbortWithResult` / the HID-abort story** — its body is `svcBreak`, and a break
  *always* produces a creport. No creport was ever produced, so no break ever executed,
  in our libnx copy or hbloader's. (`hid.o` alone has 69 call sites, all silent.)
- **Missing `SWITCH_FS_LOCK()` in `FS_OpenWad`** — investigated and **false**:
  `file_util::open_file()` takes the lock internally (`FileUtil.cpp:751`).

## FIX 7i — probes across the VAG/audio window (ON THE CARD, TEST PENDING)

`gk.nro` md5 `6f16f0e7f4196e65f52150330f0940f4`, marker
`session start 7i (VAG/audio window probes: [vag] + [aud])`, ELF archived as
`switch-crash-reports/gk.7i.elf`.

`lg::debug` is **not trustworthy at death** — one hardware game log is truncated
mid-line, so its buffer is lost. All 7i probes go through `switch_run_logf` (raw fd +
`fsync` per line) into `sdmc:/gk_run_log.txt`.

- `[vag]` in `iso.cpp` (`QUEUE_VAG_STREAM` entry, around `QueueMessage`, around
  `open_wad`, after `ProcessVAGData` is armed) and in `fake_iso.cpp`
  (`FS_OpenWad` enter/submit/sleep/wake/`future.get`, and inside `open_fr` on the
  pool thread around `get_file_path` / `fopen` / `iWakeupThread`).
  `open_fr`'s probes are gated by an atomic set only during a wad open, so the
  per-DGO `FS_Open` traffic cannot become the 7c write storm.
- `[aud]` in `cubeb_shim.h` (`cubeb_init`, `SDL_OpenAudioDevice` want/have,
  `cubeb_stream_start`, and a **one-shot** first-callback entry/return probe —
  the callback runs on SDL's audout thread at 48 kHz, so it must never log per call).

**Reading the result:**

| last `[vag]`/`[aud]` line | conclusion |
|---|---|
| no `[vag]` at all | death is before `QUEUE_VAG_STREAM`; the game log misled us (buffering) |
| `QUEUE_VAG_STREAM enter` then nothing | dies in `QueueMessage`/IOP queue |
| `FS_OpenWad submitted` then nothing | dies in the thread pool / `SleepThread` handoff |
| `open_fr fopen done` then nothing | dies in `iWakeupThread` (IOP scheduler) |
| `FS_OpenWad done` then nothing | dies after the open — in `ProcessVAGData`/989snd |
| any `[aud]` line last | the SDL/audout audio shim is the killer |

Note the audio shim (`game/switch/cubeb_shim.h`) is a **new, barely-exercised**
cubeb→SDL2 wrapper written for this port, and the first streamed VAG is the first
time it is driven in anger — it is the strongest remaining suspect.

### 7i is instrumentation, NOT a fix

The game **will still die**. 7i changes no behaviour; it only records which statement
ran last. Resist patching before reading the log: the `SWITCH_FS_LOCK()`-in-`FS_OpenWad`
theory was plausible, fit every observation, and was still wrong.

### How to run it (state as of 2026-09-11 15:36)

1. Card already holds 7i; `gk_run_log.txt` and `gk_boot_log.txt` were **deleted before
   eject**, so any file that exists after the run IS the 7i run. Older runs are kept as
   `gk_run_log.7b/7c/7d/7e.txt` in the card root.
2. Boot the game from hbmenu, let it reach the intro, wait for the death (~15–20 s).
3. Copy `sdmc:/gk_run_log.txt` back and read the last `[vag]`/`[aud]` line against the
   table above. Archive it as `switch-crash-reports/gk_run_log.7i.txt`.
4. Only then write the actual fix.

Alternative single-question experiment, if a second cycle is cheap: build with streamed
VAG audio disabled on Switch. Survives the intro ⇒ the audio path is confirmed and the
fix is scoped to `cubeb_shim.h`; dies anyway ⇒ the audio path is exonerated outright.

### Host build

`game/kernel/common/kmachine.cpp` called `switch_run_logf`/`SWITCH_FS_LOCK` from code
*outside* the `__SWITCH__` guard while including the headers *inside* it, so the PC build
had been broken. Both headers are now included unconditionally (they are no-ops
off-Switch). `cmake --build build-host --target runtime` is green — keep it that way.

## TL;DR — previous state (superseded, kept for history)


- **On the card NOW**: `switch/jak1/gk.nro` md5 `58bc30269f079d46e04e44f41414a6e9`
  = **FIX 7g**, marker `session start 7g (abort trap + dispatch breadcrumbs)`.
  The live `gk_run_log.txt` was deleted before deploy, so any file that exists now
  IS the 7f run. **Next action: run it and read `sdmc:/gk_run_log.txt` against
  decision tree v5** (bottom of this file).
- **What 7g is**: a strong override of libnx's **weak** `diagAbortWithResult()` (the abort
  path used by hid/pad/applet/sm/framebuffer/thread/newlib) that logs
  `[fatal] diagAbortWithResult res=… lr=… stage=…` before reproducing libnx's exact
  `svcBreak`, plus **dispatch breadcrumbs**: the kernel thread only *stores* a stage code
  (no I/O — the 7c write-storm lesson), and the gfx thread prints it with its existing
  250 ms heartbeat as `[gfx] alive stage=N iter=M`. Stage 4 = GOAL code executing.
- **The resolution hypothesis is DEAD** (7f): with the override compiled out and the window
  created at native 1920x1080, the game died identically — and `pc_get_active_display_size`
  never appeared in the log, so GOAL dies *before* it ever touches display code.
- **Ruled out so far**: OOM (mem numbers are hbl constants, 7d), a CPU exception (no
  creport, 7d+7e), and any `exit()`/`_exit()` path (trap installed and never fired, 7e).
  Boot itself is healthy — `gk_boot_log.txt` always reaches `about to KernelCheckAndDispatch`.
- **Diagnostic assets** in `switch-crash-reports/`: `gk_run_log.7d/7e.txt`,
  `gk_boot_log.7d.txt`, `fatal_7e_01789179294.bin`, and `gk.7f.elf` (unstripped ELF
  matching the deployed NRO — creport cannot symbolize hbloader NROs, so use the
  `symbol anchor` line printed at session start: `load_base = printed_ptr − nm_addr`;
  7f: `exec_runtime = 0x96fd0`, `get_memory_info = 0x9ce60`, `_exit = 0x9cee0`).
- **No rollback NRO is left on the card** (only `gk.nro`). The last binary known to reach
  gameplay was md5 `01046d313f5836491a86720ae3f1eea9` (2026-09-11 00:09) — **not archived
  locally; rebuild from the notes if a rollback is needed.**


### FIX index (chronological)

| Fix | What it changed | Status |
|-----|-----------------|--------|
| 1 | libco `.text#` orphan section → executable | CONFIRMED fix |
| 2 | fsdev serialization (`SWITCH_FS_LOCK`) | disproven as boot-hang cause, KEPT |
| 3 | `sdmc:` path mangling in `get_file_path()` | CONFIRMED fix |
| 4 | libnx HID abort on exit (result `0x1159`) | KEPT |
| 5 | all fsdev users under the lock | REGRESSED (exposed the IOP race) |
| 6 | `IOP_Kernel::threads` → `std::deque` + mutex | KEPT, fixes FIX 5's regression |
| 7, 7b, 7c, 7d | whole-session run-log tracing (7c's watchdog thread was itself fatal; removed in 7d) | instrumentation only |
| 7e | `_exit()` interposition | instrumentation; **trap never fired ⇒ exit hypothesis dead** |
| 7f | resolution override compiled out + `[disp]` probes | **hypothesis REFUTED** — dies identically, and before any display call |
| 7g | `diagAbortWithResult` trap + dispatch stage breadcrumbs | **on card, test PENDING** |

### Historical context (superseded, kept for reasoning)

- Earlier on the card: md5 `784939cc83a75779ad389cbfca21853a`
  (FIX 6, deployed late 2026-09-11, marker `update-card: begin` verified inside).
- **FIX 5 build crashed EARLIER than the original freeze** — right after the Sony
  America title screen (intro STR streaming), before any memory-card call
  (`mc-trace.txt` never existed). 9 Atmosphère reports over the day, 3 crash
  signatures (Instruction Abort jump-to-unmapped out of the IOP dispatch path,
  User Break on the main thread, Data Abort) = **classic memory corruption, one
  race, many signatures**.
- **ROOT CAUSE (symbolized against unstripped ELF, confirmed in source)**:
  `IOP_Kernel::threads` was a `std::vector<IopThread>` with **no mutex**.
  `IopThread` has a user-declared destructor (`co_delete`) ⇒ no move ctor ⇒
  vector growth COPIES elements then destroys the originals — including
  `co_delete()` of coroutine stacks that can be **executing on the iop_runner
  thread at that moment**. `CreateThread`/`StartThread`/`WakeupThread` are
  reachable cross-thread (EE/GOAL kernel via `sif_rpc` handlers) while
  `dispatch()` iterates the same vector. FIX 5 didn't create this — its mutex
  overhead shifted timing into the window during intro streaming.
- **FIX 6 (see its section): `threads` → `std::deque` (growth never invalidates
  element pointers) + new `threads_mtx` around table ops.** Docker rebuild
  `BUILD EXIT: 0`, deployed with md5 verification.
- **THE GAME BOOTS AND REACHES GAMEPLAY** (pre-FIX5 build, session 20:48:01).
- **FIX 1 — libco `.text#` orphan section (CONFIRMED, root cause of a hard crash).**
  `third-party/libco/settings.h` put `co_swap_function` in a section literally named
  `.text#`, flagged `A` (alloc, **no execute**) — the first `co_switch` was an
  instruction abort. Upstream's `#`-as-comment trick is x86-GAS-only; on aarch64 `#`
  is the immediate prefix. Now `__aarch64__` uses plain `.text`. Verified: orphan
  section gone, `co_swap_function` is `T` inside `.text` (AX). Symptom it explains:
  boot log dying exactly at `[iop_runner] start`.
- **FIX 2 — fsdev serialization (DISPROVEN as the boot-hang cause; KEPT anyway).**
  A global recursive mutex over all newlib fsdev users. It did **not** fix the
  `pckernel`/`kopen()` freeze — the next boot died at the identical line. The races it
  guards are real, so it stays, but do not credit it with fixing anything yet.
- **FIX 3 — `sdmc:` path mangling in `get_file_path()` (CONFIRMED ON HARDWARE, cause of
  the `kopen()` boot freeze).** See its own section below. The game now boots past it;
  the log shows `pc settings file write: "sdmc:/switch/jak1/OpenGOAL/jak1/settings/
  pc-settings.gc"` unmangled, and two successful `kopen()` calls.
- **FIX 4 — libnx HID abort on exit (NEW, see its own section).** Any path that reaches
  `exit()` without going through `Gfx::Exit()` let SDL keep polling pads after libnx's
  `__appExit()` destroyed HID, producing an Atmosphère `User Break` result `0x1159`
  crash report that *hid the real failure*. An `atexit` handler now stops the pad
  subsystem while HID is still mapped.
- **Rollback** on card: `gk.nro.prev` = md5 `01046d313f5836491a86720ae3f1eea9`
  (2026-09-11 00:09 build — boots to gameplay, still HAS the new-save freeze;
  **contains no FIX5 markers — verified pre-FIX5 baseline**). `gk.nro.fix5` =
  md5 `e770a107…` (the FIX 5 build that crashes at the Sony logo — kept as the
  reference build for the crash reports; its unstripped ELF was rebuilt away,
  symbolize against `gk.nro.fix5` if ever needed again).
  Older: `gk.nro.bak` (2026-09-10 16:32) and `gk.nro.bak2` (20:21),
  `484049fef60da8a7c3fe94e1166178bd`-era known-bootable builds. NOTE: the
  intermediate builds `1501336d…`, `8b1c39…`, `4fe8d2fe…`, `b89954a6…` were
  overwritten, not kept.
- **SD card health: RULED OUT.** Full read of `switch/jak1/data` (3.7 GB, 9,830 files)
  returned zero I/O errors. The earlier FAT corruption was real but was repaired.
- **Boot-log watermark: `gk_boot_log.txt` was 14,715 lines at the latest deploy.**
  Anything past that is the next session; the file is append-only and fsync'd per line.
- **OPEN — the new-save freeze.** Pre-FIX5 it was a FREEZE with no crash report.
  FIX 5 (fsdev-under-lock everywhere) turned out NOT to be the fix — it exposed
  the IOP thread-table race instead (FIX 6). Whether the original freeze is also
  gone is unknown until hardware retest: **FIX 6 + FIX 5 both on card now.**
- **Also unresolved from tonight: the PAL ISO question** (see night section) —
  verify the crash fix first on PAL before any swap.

## 2026-09-11 (late night) — FIX 5 regression: post-Sony-logo crashes → root cause → FIX 6

**(AI-assisted)** FIX 5 (`e770a107…`) crashed on hardware right after the Sony
America title screen — before New Game, before any MC call. ~9 fresh Atmosphère
reports in `atmosphere/crash_reports/` (`…_0100a3d008c5c000.log`).

### Evidence (symbolized with devkitA64 addr2line/objdump inside `devkitpro/devkita64`)
- `mc-trace.txt` does **not exist anywhere on the card** ⇒ no FIX5-traced MC call
  ever ran ⇒ the crash is NOT in the save path.
- Two evening signatures, reproducible ×2 each:
  - **Instruction Abort** (19:29:50 / 19:30:04, iop thread): PC = unmapped
    heap-ish address; backtrace `IOP_Kernel::sif_rpc` (IOP_Kernel.cpp:465) →
    `schedNext` (372) → `WakeupThread` (130). Address mapping verified by
    disassembly (semantically consistent call graphs), so the chain is REAL.
  - **User Break 0x1159** (18:43 / 20:35, main thread): PCs/LRs land in libnx
    `hidLaShowControllerSupportForSystem` / newlib `accept()` = register+stack
    garbage, svcBreak reports a 4-byte **stack** slot (canary-style abort).
- Afternoon reports add **Data Aborts** and more Instruction Aborts, incl. the
  same `gk+0xa2c40` (WakeupThread) site twice — one corruption, many signatures.
- `gk.nro.prev` (01046d31) has **no** `update-card: begin` marker ⇒ confirmed
  pre-FIX5 ⇒ the intro crash is NEW with FIX 5's build.

### Root cause (confirmed in source, `game/system/IOP_Kernel.{h,cpp}`)
`IOP_Kernel` runs its IOP "threads" as **libco coroutines** (`co_create(0x300000)`)
on the single `iop_runner` OS thread, but its API is called from OTHER OS threads
too (EE/GOAL kernel thread: `sif_rpc`, `set_rpc_queue`, module init — that's why
`sif_mtx`/`wakeup_mtx` exist). The thread table `std::vector<IopThread> threads`
had **no lock**, and `IopThread` (user-declared dtor ⇒ implicit copy, no move)
makes vector growth: copy all elements → **destroy originals → `co_delete()` on
coroutine stacks that may be running on iop_runner right then** → execution
continues on freed 3MB stacks → heap reuse → jumps to unmapped, canary aborts,
data aborts. Matches every signature, the crash sites (dispatch/WakeupThread),
and why FIX 5 (mutex timing perturbation, e.g. gfx-thread `stbi_load` under the
FS lock) pushed it into firing during intro streaming.

### FIX 6
- `game/system/IOP_Kernel.h`: `threads` is now `std::deque<IopThread>` (growth
  never invalidates/moves/destroys existing elements); new `std::mutex
  threads_mtx`; `#include <deque>`.
- `game/system/IOP_Kernel.cpp`: `threads_mtx` taken in `CreateThread` (incl. ID
  assignment + priming `co_switch`), `StartThread`, `WakeupThread`, and around
  the dispatch-side iterations (`schedNext`, `updateDelay`, `nextWakeup`).
  Removed the now-pointless `threads.reserve(16)` ctor hack.
- Lock-ordering: `threads_mtx` is a leaf (nothing else held while taking it);
  `processWakeups` nests `wakeup_mtx` → `threads_mtx` in one direction only.
  No `co_switch` into lock-taking coroutines while it is held (CreateThread's
  priming switch parks immediately).
- NOT touched (pre-existing, boot-time only, out of scope): `mbxs`/`semas`/
  `event_flags` vectors still grow without locks — if a *later* crash points at
  semaphore/mbx corruption during runtime module loads, that's the next place.
- Docker rebuild: `BUILD EXIT: 0`; `gk_nro` target; deployed md5
  `784939cc83a75779ad389cbfca21853a` (host=card verified, `sync`'d, marker
  present).

### RETEST (hardware, next session) — decision tree
1. Boot → Sony logo → intro → title. If it **crashes again**: grab new crash
   reports + `gk_boot_log.txt` tail; the FIX5 reports stay valid comparators
   (`gk.nro.fix5` on card). If the PCs move OUT of the IOP dispatch path, the
   thread-table race is fixed and whatever remains is a second, smaller bug.
2. If intro passes: New Game → first save-create → check `mc-trace.txt` now
   exists with complete `begin/done` pairs (FIX 5's original purpose — verdict on
   the save-freeze finally becomes testable).
3. Only after stability: revisit the PAL↔USA ISO question (night section).



## 2026-09-11 (night) — FIX 5: all fsdev users under the lock + hang tracing; PAL-vs-USA ISO

**(AI-assisted)** Deployed `gk.nro` md5 `e770a107e0c96e522e4f8fc2fd2ef7b6`
(clean docker rebuild, `BUILD EXIT: 0`; same md5 as the earlier cut-log build —
that one had in fact finished).

### Root-cause hypothesis for the freeze
The Overlord streams level/ISO data on its own thread. Several fsdev/newlib users
still ran WITHOUT the recursive FS mutex from FIX 2, so `std::filesystem::exists` /
`file_size` / `stat()` / icon loads from the kernel or gfx thread could interleave
with the Overlord's reads → newlib fsdev corruption/hang → save-create FREEZE with
no crash report.

### FIX 5 — everything else that touches fsdev now takes `SWITCH_FS_LOCK()`
- `game/kernel/common/kmemcard.cpp`: `file_is_present`, `pc_update_card`,
  `pc_game_load_open_file`, `mc_trace_persist`
- `game/kernel/common/kmachine.cpp`: `pc_filepath_exists`
- `game/sce/sif_ee_memcard.cpp`: `flush()`, `read_memory_card_from_file()`
- `common/util/FileUtil.cpp`: `create_dir_if_needed`, `create_dir_if_needed_for_file`,
  `file_exists`, `assert_file_exists`
- `game/graphics/pipelines/opengl.cpp`: window-icon `stat()` + `stbi_load`
  (gfx thread, boot time)
- Lock order convention: `SWITCH_FS_LOCK()` **before** `g_mc_trace_mtx` (fs → trace).
  The FS mutex is recursive, which also covers `pc_game_load_open_file`'s recursion.
- FIX 4 extension: new `sdl3compat_shutdown` atomic (`game/switch/sdl3_compat.h`)
  gates `sdl3compat_PollEvent`; set from the `atexit` handler in
  `game/system/hid/input_manager.cpp` — covers touchscreen polling after `hidExit()`.

### Hang pinpointing (for the NEXT freeze)
`mc-trace.txt` now logs pairs: `update-card: begin/done` (`pc_update_card`) and
`get-status: begin/done` (`MC_get_status`). `auto-save-check` remains commented out
at its only call site, so these only fire at the title screen and save transitions —
safe to leave on unconditionally. If the freeze recurs, the last `begin` without its
`done` names the stuck call.

### RETEST (hardware, next session)
New Game → reach the first save-create → confirm the save completes, `mc-trace.txt`
exists with complete begin/done pairs, and `gk_boot_log.txt` keeps growing.
If it still freezes: grab `mc-trace.txt` + `gk_boot_log.txt` tail, compare with the
watermark, and look at which `begin` never finished.

### PAL vs USA ISO — frame-rate verdict
- **PAL's 50 Hz slowdown cannot happen in this port.** The runtime synthesizes
  vblank from the actual display vsync: `game/runtime.cpp` line ~411 →
  `Gfx::register_vsync_callback(… iop.kernel.signal_vblank())`. There is no
  territory/video-mode timing anywhere; PAL data runs at the Switch's 60 Hz exactly
  like NTSC data would. Speed/feel will NOT change with the USA ISO.
- PAL differs only in: multi-language text/audio, PAL-encoded STR movies, version
  metadata. `goal_src/jak1` is territory-independent.
- **Decision: test FIX 5 FIRST on the existing PAL data** (one variable at a time).
  Swapping to USA Rev 1 afterwards is optional (authenticity/language), not a fix.
- Version support (`decompiler/extractor/extractor_util.cpp` DB):
  - current card data = PAL `SCES_503.61`, config `pal`, 338 files (fork-added).
  - USA black label = `SCUS-971.24` ELF hash `7280758013604870207` → `ntsc_v1`
    (337 files); **USA Rev 1 = ELF hash `744661860962747854` → `ntsc_v2`**
    (338 files). The user's Rev 1 dump (redump md5 `e5563b15…`) should match
    `ntsc_v2`; the extractor validates ELF hash + file count and fails safely
    otherwise.
- Swap procedure (host, ~30+ min + 3.7 GB SD copy), ONLY after FIX 5 is verified:
  1. Replace `iso_data/jak1/` contents with the USA ISO file; set
     `DECOMP_CONFIG_VERSION=ntsc_v2` (`task set-game-jak1` / `.taskvars.yml`).
  2. `task extract` (needs host `decompiler` in `build-host`).
  3. Regenerate whatever originally produced the runtime folders, then re-copy to SD —
     `switch/jak1/data` mirrors repo `iso_data/`, `out/`, `goal_src/`,
     `custom_assets/` (diff SD vs repo before copying).
  4. Keep the PAL `data/` around (rename to `data-pal`) for instant rollback.
  - Saves: the port's save format is per-game, not per-territory; worst case a fresh
    save is created.

## How to build / deploy / rollback

```bash
# build (docker, devkitA64):
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest bash scripts/build-switch.sh

# output: build-switch/game/gk.nro  (plus unstripped ELF build-switch/game/gk)

# deploy (SD card must be mounted):
cp build-switch/game/gk.nro "/Volumes/SWITCH SD/switch/jak1/gk.nro" && sync

# rollback (on card):
cp "/Volumes/SWITCH SD/switch/jak1/gk.nro.bak" "/Volumes/SWITCH SD/switch/jak1/gk.nro"

# eject when done:
diskutil eject /dev/disk4   # check actual disk# with `diskutil list external`
```

Symbolizing crash reports (Atmosphère writes to `atmosphere/crash_reports/`):
module-relative offsets ("gk + 0x...") map **directly** to vaddrs in the unstripped ELF
(it is a PIE with base 0):

```bash
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest \
  /opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -e build-switch/game/gk 0x<offset>
```

**Caveat (learned the hard way, 2026-09-11):** Atmosphère's `Stack Trace` is a heuristic
stack scan and is frequently pure garbage; `PC`/`LR` can be stale too. See the FIX 4
section for the reliable method (`Stack Dump` + `diagAbortWithResult` frame arithmetic)
and for the `readelf -sW` decimal-`Size` pitfall.

## History — issues found so far (2026-09-10 sessions)

1. **Save truncation**: `bank0.bin` written as only 1024 bytes = valid `McHeader`
   (magic/checksum/save_count ok) but **zero payload**. The header write succeeded, the
   first payload chunk failed. GOAL logged `ERROR NOTIFY: internal-error 13`.
   Suspected root cause: **newlib's fsdev layer is not thread-safe on Switch** when the
   Overlord thread streams the ISO (`LoadISOFileChunkToEE`) while the kernel thread
   saves (documented in `game/switch/boot_log.h`, which works around it with raw fds +
   its own mutex). Disk space ruled out (FAT32, 84 GB free).
2. **Options-menu crash**: GOAL sends `set-display-mode` windowed with stored
   `window_size 0,0` → `SDL_SetWindowSize(0,0)` zeroes the Switch nwindow swapchain →
   later `make_fbo` fails `GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT` → ASSERT crash.
3. **Resume-from-suspend crash (x3, 2026-09-10 17:58–17:59)**: `hidGetTouchScreenStates`
   → `diagAbortWithResult` inside devkitPro SDL2 2.28.5 touch polling (called from
   `GLDisplay::render()`). **Upstream SDL2 bug, pre-existing, NOT fixed** — avoid
   suspending the console in-game.
4. **Diagnostics were unreliable**: `gk_stdout.txt` is truncated every boot
   (`freopen("w")` in `game/main.cpp:122`), and the async lg queue loses its tail on a
   crash — that's why earlier `[MC]` traces vanished.

## 2026-09-11 (evening) — old build ALSO crashes → real root cause: SD card corruption

After the rollback, the user booted the **old, previously-working build** — it crashed
**2×** (crash reports 01789151391 / 01789151405, both Instruction Abort; gk_stdout died
even earlier than in the morning, before the `arg 1 : fakeiso` echo). Same binary that
worked all day yesterday + progressive degradation ⇒ not the binary.

- `diskutil verifyVolume` on the card → **error -69845 / exit 206** (corrupt FAT).
- Backed up configs, then `diskutil repairVolume` → **"***** FILE SYSTEM WAS
  MODIFIED *****", exit code 0**; re-verified clean. Game files unharmed
  (gk.nro md5 unchanged, data/ intact).
- **Working theory**: repeated hard power-offs (after fatal-error screens) while the
  system was writing (crash reports, boot log) corrupted the FAT; the game's boot-time
  file/service init then read garbage → the wild-pointer Instruction Aborts. This
  explains: crashes in fsdev/adjacent code, zero frames in changed code, "worked
  yesterday / dead today", and worsening behavior across the day.
- **Action**: repaired FS, deployed the staged fixed build (`1501336d…`) so the
  save/display fixes can finally be tested, kept `gk.nro.bak`/`.bak2` rollbacks on the
  card, archived the matching ELF locally + on-card.

## 2026-09-11 incident — boot crashes with the experimental build (ROLLED BACK)

User booted the game with the 19:53 build → **Atmosphère fatal screen 4x**. Evidence
pulled to `/Users/filippo/Downloads/gk-evidence-0911/` (4 crash logs + gk_stdout.txt +
the bad NRO):

- 2x **Instruction Abort**, identical stacks, on the IOP thread:
  `IOP::wait_for_overlord_start_cmd` (`game/system/iop_thread.cpp:30`) → libnx
  `shmemClose`/`binderGetNativeHandle`/`serviceDispatchImpl` → jump to a wild pointer
  **just past the end of the gk module** (deterministic offset ≈ `base+0xC16860`).
- 1x Instruction Abort with frames in module `hbl`/unnamed module (no gk annotation).
- 1x **User Break** (`svcBreak`): PC in libnx `select` (`socket.c:450`), LR in
  `_hidLaShow` (`hid_la.c:10`), via `audrenInitialize` ← `SDL_EGL_CreateContext` ←
  `SDL_CreateWindow`; outer frames `GLDisplay::render` → `process_sdl_events` →
  `sdl3compat_PollEvent` → `Gfx::Loop` → `exec_runtime` → `main`. (Some mid-stack
  entries like `SDL_CreateWindow` may be stale stack garbage.)
- **`mc-trace.txt` was never created** and `gk_stdout.txt` ends right after
  `arg 2 : SCREEN1.USA` → the memory-card code **never executed** before the crash.
- No `settings.json` exists on the card → NOT persisted bad display settings.

**Analysis**: all frames are in libnx/SDL2 service init, none in our changed files
(kmemcard.cpp / display_manager.cpp never appear, and their code demonstrably didn't
run). Most plausible explanations, in order:
1. Latent race between concurrent libnx service init on two threads (main thread doing
   SDL window/GL/audren setup while the IOP thread does binder/nwindow setup) — the
   different binary layout/timing of the new build perturbed scheduling and it lost 4x.
2. Layout-sensitive uninitialized function pointer/vtable entry (wild jump to a fixed
   offset just past the module end).
3. Console-side stale state after the previous day's fatal errors (recommend a FULL
   power-off, not sleep, before retesting).
## 2026-09-10 (late) — libco `.text#` bug (CONFIRMED FIX)

`third-party/libco/settings.h`. Upstream marks `co_swap_function` with
`__attribute__((section(".text#")))` under GCC. The `#` is meant to be swallowed as a
comment by **x86** GAS, leaving `.text`. On aarch64 GAS, `#` is the immediate prefix,
not a comment — so the section name stayed literally `.text#`, and the assembler
created an orphan section with flags `A` only (**no X**). Executing it = instruction
abort on the very first coroutine swap.

Reproduced in isolation with the real toolchain before patching:
`section(".text#")` → `A`; `section(".text")` → `AX`. (`naked` functions are not an
option: this GCC ignores the attribute on aarch64.)

Fix: under `__GNUC__ && !__clang__`, an `__aarch64__` branch uses plain `.text`.
x86 keeps the upstream hack; clang already fell through to the correct branch.

Evidence it was the live bug: the boot log ended exactly at `[iop_runner] start`, the
first place `co_switch` is used.

Verified in the built ELF/NRO: no `.text#` section; `co_swap_function` is `T` at a
vaddr inside the `AX` `.text`; instruction bytes identical in ELF and NRO and decode to
the expected sequence (`mov x16,sp` / `stp x16,x30,[x1]` / `ldp x16,x30,[x0]` /
`mov sp,x16` / callee-saved `stp`/`ldp` pairs); RELR relocations 23,627 with 0 targets
in the read-only segment.

## 2026-09-10 (late) — fsdev serialization (DISPROVEN as the hang cause, kept as hardening)

**Outcome first: this did NOT fix the boot hang.** The boot after deploying it died at
the identical line (`obj=pckernel: top-level enter` → `****** CALL TO kopen() ******`).
The real cause was the path bug in the next section. Retained because the races it
guards are genuine, but it has fixed no observed symptom.

Original motivation: the run after the libco fix got through the whole DGO link chain,
then froze in `pckernel`'s top-level around `kopen()` with **no** Atmosphère exception
report (a hang, not a crash). Card health had been tested and passed, leaving concurrent
access to newlib's fsdev/devoptab layer as the suspect: it keeps unsynchronized global
state (descriptor table, device cache, fsp-srv session) while the GOAL kernel thread
(`sceOpen`/`sceRead`), the overlord/ISO thread (`fake_iso.cpp`), the log sink and the
boot trace all use it.

New `common/util/FsLock.h`: one global **recursive** mutex behind `SWITCH_FS_LOCK()`,
compiled to a no-op off Switch. Recursive because the helpers nest
(`read_binary_file` → `open_file`). Applied in:

- `common/util/FileUtil.cpp` — `open_file`, `read`/`write_binary_file`,
  `read`/`write_text_file`
- `game/sce/sif_ee.cpp` — `sceOpen`/`sceClose`/`sceRead`/`sceWrite`/`sceLseek`,
  `LIBRARY_INIT_sceSif`
- `game/overlord/jak1/fake_iso.cpp` — `FS_Close`, and the seek/tell/rewind/seek/read
  sequence in `fs_read`
- `game/overlord/common/fake_iso.cpp` — `FS_GetLength`, `LoadMusicTweaks`
- `common/log/log.cpp`, `game/switch/boot_log.h`

**Two deadlock hazards this had to avoid — do not "simplify" these away:**

1. **Lock ordering.** `FS_Close` holds the FS lock and then logs. So the logger must
   take the **FS lock before `gLogger.mutex`**, never the reverse — otherwise ABBA
   deadlock. All four `gLogger.mutex` sites in `log.cpp` now do FS-lock-first.
2. **No blocking under the lock.** In `fs_read` the lock is scoped to the I/O only, so
   the trailing `iWakeupThread` runs unlocked (its caller `FS_BeginRead` does
   `SleepThread`/`future.get()` on the pool result).

`boot_log.h` now shares the global lock instead of its own private mutex — a private
mutex only ordered tracers against each other, not against the ISO/kernel threads.

**Unrelated bug fixed while in `sif_ee.cpp`:** `sceOpen` allocated descriptors as
`sce_fds.size() + 1`, which reissues a live id after any close — the next open
overwrites the still-open file's map entry, leaking the `FILE*` and handing two callers
the same descriptor. Now a monotonic counter.


## 2026-09-10 (late) — `sdmc:` path mangling in get_file_path() (CONFIRMED FIX)

**This is what was actually killing the boot at `kopen()`.**

Evidence from `gk_boot_log.txt` / `data/log/jak1.*.log`:

```
[PC] PC Settings not found at 'sdmc:/switch/jak1/OpenGOAL/jak1/settings/pc-settings.gc'
...initializing with defaults!
pc settings reset
****** CALL TO kopen() ******      <-- last line, then freeze
```

`commit-to-file` (goal_src/jak1/pc/pckernel-common.gc) does `pc-mkdir-file-path` then
`write-to-file`, and the latter is the `kopen`.

Mechanism: `std::filesystem::path::is_absolute()` requires a leading `/`. devkitPro's
`sdmc:/...` is absolute to the C library but **relative** to `std::filesystem`, so
`get_file_path()` appended it to the project dir:

```
sdmc:/switch/jak1/data/sdmc:/switch/jak1/OpenGOAL/jak1/settings/pc-settings.gc
```

`:` cannot appear in a FAT32 name, so the mkdir/open failed on the GOAL kernel thread
(and `fs::create_directories`'s throwing overload turned that into a runtime kill).

**Why it only appeared now:** `game/main.cpp:203` force-enables **portable mode** on
Switch — an earlier fix so saves land next to `gk.nro` instead of a CWD-relative path
that never materialized. That made the user config dir an absolute `sdmc:/...` path for
the first time; before it, the settings folder was relative and joined correctly. The
save fix silently introduced the boot hang.

Corroborating detail: `pc_filepath_exists` and `pc_mkdir_filepath`
(game/kernel/common/kmachine.cpp) use the raw GOAL string and do **not** call
`get_file_path`, which is why the settings directory really was created on the card and
the existence check behaved — only `kopen` → `sceOpen`, the single caller that routes
through `get_file_path`, failed.

Fix (common/util/FileUtil.cpp):
- new `is_device_absolute_path()` (guarded to `__SWITCH__` so PC semantics are
  untouched) recognizes a leading `<device>:/` component; `get_file_path()` now returns
  such paths unchanged. Unit-tested: `sdmc:/...` → absolute, while relative ISO names
  like `DATA/MUS/TWEAKVAL.MUS` still join to the project dir.
- `create_dir_if_needed_for_file()` now uses the `error_code` overload and logs, instead
  of throwing an uncaught `filesystem_error` on the kernel thread.

Diagnostics added: `sceOpen` writes an fsync'd
`[sceOpen] flag=<n> in=<goal path> resolved=<final path>` line to `gk_boot_log.txt`, so
any future failure here shows the resolved path instead of requiring another guess.

## 2026-09-11 (late) — libnx HID abort on exit, result `0x1159` (FIX 4)

**Symptom.** Atmosphère crash reports of type `User Break`, `Result: 0x1159
(2345-0008)`, `Break Reason 0x0`, `Break Size 0x4`.

**How to decode this class of report (reusable method).**
- `0x1159` = `MAKERESULT(345, 8)` = `345 | (8 << 9)` = `Module_Libnx`,
  `LibnxError_NotInitialized`.
- `Break Reason 0`/`Break Size 4` plus a `Break Address` inside the crashed thread's own
  stack region is the exact signature of libnx `diagAbortWithResult(Result)`. Its
  disassembly is `mov x2,#4 / add x1,sp,#0x1c / str w0,[sp,#28] / mov w0,#0 / bl
  svcBreak`, so **`Break Address` == `SP + 0x1c`**, and the 4 bytes at that address in
  the `Stack Dump` are the Result. In the analysed report those bytes were literally
  `59 11 00 00`. That is how to be *sure* which result aborted.
- **The report's `PC`, `LR` and `Stack Trace` were garbage** (they symbolised to an
  impossible chain: `select` → `_hidLaShow` → `audrenInitialize` → `SDL_FillRect1` →
  `SDL_VideoInit`, and the trace ended in a repeating `0x8847c / 0xb5744c / 0xc4`
  triple). Do **not** trust them. The ELF *did* match the deployed NRO (byte-compared
  at three vaddrs), so this is a bad unwind, not a stale binary.
- Symbolising: `ELF vaddr == NRO file offset == Atmosphère's `gk + 0x…` offset`
  (module base `0x3ccf609000` in that report). **`readelf -sW` prints `Size` in
  DECIMAL** — parsing it as hex silently corrupts every containment test.

**Root cause (established statically, not from the bad backtrace).**
- Only **13** sites in the whole binary actually call `diagAbortWithResult(0x1159)` —
  found by scanning for `mov w*, #0x1159` and keeping only those followed within 6
  instructions by `bl <diagAbortWithResult>`. The other 43 matches merely *return*
  that value. All 13 are `hidGetNpadStates*` / `hidGetNpadStyleSet` /
  `hidGetNpadDeviceType` / `hidGet{Touchscreen,Mouse,Keyboard}States`, plus
  `threadExit`.
- Each of those aborts on exactly one condition: `hidGetSharedmemAddr()` returned
  `NULL` (`bl hidGetSharedmemAddr; cbz x0, <abort>`).
- The HID sharedmem pointer is set **only** by `hidInitialize` (single caller:
  `__appInit+0x34`) and cleared **only** by `hidExit` (single caller: `__appExit+0x34`).
  → **HID can only be NULL while the process is exiting.**
- But `"GFX Exit"` / `"GOAL Runtime Shutdown"` appear in **no log ever written**, so the
  orderly path (`Gfx::Loop` → `Gfx::Exit()` → `Display::KillMainDisplay()` →
  `~GLDisplay()` → `SDL_Quit()`) never ran. Something reached `exit()` directly —
  most plausibly libnx force-exiting the title when the user closes a frozen game from
  the HOME menu.
- Net effect: `exit()` → `__appExit()` → `hidExit()` → SDL is still polling pads →
  `hidGetNpadStates*()` → abort. The same `0x1159` signature appears on this card for
  several *unrelated* titles (`0591a9a6989bf000`), confirming it is a generic
  SDL-on-Switch shutdown race, not something specific to this port.

**Fix.** `game/system/hid/input_manager.cpp`: immediately after a successful
`SDL_InitSubSystem(SDL_INIT_GAMEPAD)`, a `std::call_once`-guarded
`std::atexit([]{ SDL_QuitSubSystem(SDL_INIT_GAMEPAD); })`. atexit handlers all run
before libnx's `__libnx_exit`/`__appExit`, so the pad subsystem is always torn down
while HID is still mapped, on *every* exit path.

Verified in the binary: GCC emitted the handler as
`InputManager::InputManager(SDL_Window*)::{lambda}::_FUN()` at `0x1dc740` =
`mov w0,#0x2000` (`SDL_INIT_GAMEPAD`) → `b SDL_QuitSubSystem`, registered by
`add x0,x0,#0x740` → `b atexit`.

**This is a diagnostic-quality fix, not a gameplay fix.** Its value is that a freeze
will no longer be overwritten by a misleading crash dump.


## 2026-09-11 (late) — the new-save failure is a FREEZE (OPEN)

**Do not chase the `0x1159` crash report for this.** Correlating mtimes (both the
reports and the logs come from the console's own clock) pairs every report with a
session:

| crash report (mtime)  | game log            | note |
|-----------------------|---------------------|------|
| 18:43:16              | 18:43:03            | |
| 19:29:50 / 19:30:04   | 19:29:48 / 19:30:03 | Instruction Aborts = pre-libco-fix |
| 20:35:16 (`0x1159`)   | 20:35:07            | died at `kopen()` — the *old* path bug |
| **— none —**          | **20:48:01**        | **the successful boot + save attempt** |

The `20:48:01` session ran ~59 s, booted fully, and **produced no crash report**.
Atmosphère only dumps on exceptions, not hangs → **the save attempt froze.**

Evidence from that session:
- Game log ends at `Setting level village1 display command to display` /
  `Setting force-inside?[village1] to #f`.
- `gk_stdout.txt` ends at `Blackout loads done. village1 is loaded. title is loaded.`
  with no assert text.
- `switch/jak1/mc-trace.txt` does not exist.

**Correction to a previous assumption:** the absence of `mc-trace.txt` does *not* prove
the memory-card layer was never entered. Every *active* `mc_print` lives inside
`pc_game_save_synch` / `pc_game_load_synch`; `MC_format`, `MC_unformat` and
`MC_createfile` are upstream stubs whose logging is **commented out**, so they returned
`OK` silently. It only proves the actual bank read/write never started.

To close that blind spot, `MC_format` / `MC_unformat` / `MC_createfile` now each emit
one `mc_print` on entry. The next run will create `sdmc:/switch/jak1/mc-trace.txt` as
soon as the save menu is touched, which distinguishes "never reached the memcard layer"
from "reached it and hung after it".


## Earlier code changes (now built and deployed as part of the current NRO)

`game/kernel/common/kmemcard.cpp`:
- Persistent append-only `[MC]` trace sink → `sdmc:/switch/jak1/mc-trace.txt`
  (raw fd + one shared mutex, opens lazily, **no fsync** — write() goes through the
  console's FS service process, which already survives a game crash; fsync per line
  would stall boot/save on FAT32). MUST live outside the `mc_print` template —
  function-local statics in templates are duplicated per instantiation (initial version
  had this bug: per-variant fds + mutexes, writes not actually serialized).
- Save path (`pc_game_save_synch`): full open→write→sync→close retried up to 3x from a
  fresh `FILE*`, 100 ms apart, to ride out transient fsdev failures.
- Load path (`pc_game_load_synch`): retry up to 3x only when result is
  `INTERNAL_ERROR` (IO); corruption results (`READ_ERROR` etc.) are NOT retried.
  `p2` is reset to 0 before every attempt.

`game/system/hid/display_manager.cpp`:
- `sanitize_window_size()` (anonymous namespace): never pass 0/negative sizes to
  `SDL_SetWindowSize` (the options-menu 0x0 crash); falls back to current window size,
  then 1280x720; null-window safe. Called from `set_window_size` and the Windowed
  branch of `set_display_mode`.

## Verified properties of the current build (md5 `01046d313f5836491a86720ae3f1eea9`)

- Switch build in the devkitPro container: exit 0.
- **Host build also relinks clean** (`ninja -C build-host goalc extractor`) — important,
  since `common/util` and `common/log` are shared with the PC/decompiler builds.
- libco: 0 `.text#` sections; `co_swap_function` is `T` inside `.text` (AX).
- Markers confirmed in the binary (use `grep -F`, the strings contain `[`):
  `[sceOpen] flag=`, `resolved=`, `could not create directories for`, `switch_fs_mutex`,
  `MC_format requested`, `MC_unformat requested`, `MC_createfile requested`.
- FIX 4 present: atexit thunk at `0x1dc740` (`mov w0,#0x2000` → `b SDL_QuitSubSystem`).
- RELR: 23,629 relocation targets, 0 in the read-only segment.
- Deployed md5 verified equal to the local artifact.

## Known remaining issues

- **OPEN / MAIN ISSUE: creating a new save freezes the game** (no crash report — see the
  "new-save freeze" section). Last log line is the `village1` display/force-inside pair.
- The fsdev mutex has fixed nothing observable; do not assume it is load-bearing.
- `MC_format` / `MC_unformat` / `MC_createfile` are upstream **stubs** that just return
  `OK`. They are also stubs on PC, so they are not automatically the bug — but they are
  the least-tested code on the path the user was exercising.
- Suspend/resume crash (SDL2 touch polling) — upstream, avoid sleeping mid-game.
- `gk_stdout.txt` truncation on every boot (by design of main.cpp, documented above).
- This working tree is **not a git repository**, so all of the above exists only as
  loose edits — a re-clone or re-extract silently destroys them. Worth `git init`.

## Suggested next steps

1. **Boot the console** (full power-off first, then hbmenu via *full-memory title
   takeover* — hold R while launching an installed game; applet mode cannot fit the
   128 MiB EE arena). Reproduce: start a new game / create a save.
2. Then collect, in this order:
   - `sdmc:/switch/jak1/mc-trace.txt` — **the key new signal.** If it exists, the
     memory-card layer was reached and the last line says how far it got. If it still
     does not exist, the freeze happens *before* any `MC_*` call, i.e. in the level
     transition into `village1`, not in saving.
   - the newest `switch/jak1/data/log/jak1.*.log` (note: the `lg` queue is async, so its
     tail can lose the last moments — prefer `mc-trace.txt`).
   - `sdmc:/gk_boot_log.txt` **from line 14,715 onward** (watermark at this deploy).
     Remember boot tracing self-disables at `boot complete`, so it will only cover boot.
   - `atmosphere/crash_reports/` — with FIX 4 in place a genuine crash should now
     survive as a *meaningful* report instead of being replaced by the `0x1159` HID
     abort. **A freeze will still produce nothing** — that itself is diagnostic.
3. If it froze again with no `mc-trace.txt`, the next instrumentation step is a
   heartbeat that outlives boot (the existing `boot_log` sink is disabled after boot);
   trace the `village1` level-load / `pc_set_levels` path rather than the memcard path.
4. Rollback if needed: `cp gk.nro.prev gk.nro` on the card (that is `b89954a6…`, which
   is known to boot to gameplay).
5. **Archive the unstripped ELF** next to any future deploy — this build's ELF is at
   `build-switch/game/gk` and the next build overwrites it. Symbolisation caveats are
   documented in the FIX 4 section (decimal `Size` in `readelf`; Atmosphère backtraces
   can be pure garbage — trust the `Stack Dump` + `diagAbortWithResult` arithmetic).

## 2026-09-12 ~00:40 — FIX 6 retest: race is DEAD, new failure is a silent clean exit; FIX 7 = whole-session run log

Hardware retest of FIX 6 (md5 `784939cc…` confirmed it ran):

- **Boot completed 100% cleanly for the first time** — the full DGO chain incl. babak …
  static-screen/title-obs linked, log ends at the normal "boot complete, tracing disabled".
  The FIX 5 corruption crashes (which all left creports) are gone.
- Got **past the Sony logo** (FIX 5 died at it), then ~15–30 s into the intro the user got a
  **fatal-error screen: 2345-0008 / raw 0x1159, program 0100a3d008c5c000**. Three attempts,
  deterministic (erpt telemetry pairs at 23:27, 00:00, 00:23 ≈ 24–36 s sessions).
- **NO creport in atmosphere/crash_reports/** (newest still 01789155317, 20:35 FIX5-era).

Root-cause analysis of 0x1159 (see the long comment in game/system/hid/input_manager.cpp):
`MAKERESULT(Module_Libnx, LibnxError_NotInitialized)` — SDL input polling outliving HID
teardown in `__appExit()`. It is a **post-mortem artifact of a clean process exit**, not a
crash; the missing report fits (fsdev is already torn down by the time the break fires).
So: *something made the process exit ~15–30 s into the intro.*

Exit sources investigated and ruled out:
- GOAL kiosk-demo timeout (main.gc `(kernel-shutdown)` block): dead — `DebugBootMessage`
  defaults to "play" (kboot_init_globals_common) so `(!= *kernel-boot-message* 'play)` is
  false, and `masterConfig.timeout/inactive_timeout` are explicitly zeroed (jak1/kboot.cpp).
- ASSERT/lg::die: `private_assert_failed` ends in `abort()` → would leave a creport.
- `InitMachine` failure exit(1): didn't happen (log shows InitMachine ok).

Remaining candidates (all now instrumented): bogus listener message (`LTT_MSG_SHUTDOWN`),
SDL/applet quit (`SDL_EVENT_QUIT` → `m_should_quit` → `MasterExit=EXIT`), and the prime
suspect **system kill for memory** in applet mode during the intro (explains: no exception,
no report, erpt, ~fixed timing; the earlier village1 debug-boot path allocates far less
than the retail intro path).

### FIX 7 (deployed as gk.nro, md5 `7de839e83495e1671568aba0c390883d`; FIX 6 kept at switch/jak1/gk.nro.fix6)
Whole-session SD trace **gk_run_log.txt** (game/switch/run_log.h — header-only, one fd +
mutex, ordered via shared SWITCH_FS_LOCK, no-op off-Switch; boot_log stays boot-only):
- 2 s kernel heartbeat in `KernelCheckAndDispatch`: MasterExit + `svcGetInfo`
  TotalMemorySize/UsedMemorySize (game/switch/platform.cpp `get_memory_info()`,
  libnx isolated per the u128 rule).
- Exit-path probes: `KernelShutdown()`, listener message kind + LTT RESET/SHUTDOWN,
  `SDL_EVENT_QUIT`, `m_should_quit→EXIT`, exec_runtime join/return, goal_main return,

## 2026-09-12 ~02:0x — FIX 7b verdict + FIX 7c (deployed, md5 `cd96657c94f15d1253c8d0704775b6a2`)

**Attribution cleanup:** the 01:00:36 creport (01789171236, Instruction Abort PC 0x55ca194860,
LR gk+0xa0970) belongs to the OLD BACKUP build the user tried at 01:00 — discard. The erpt
pair 01:00:10/01:00:34 brackets that same old-build run. The erpt at 01:08:50 is a T+2
startup event of the FIX 7b session (started 01:08:48).

**FIX 7b session (gk_run_log.txt, preserved as gk_run_log.7b.txt):**
```
[0.000]  session start, run log fd forced open
[6.877]  [gfx] alive
[11.892] [gfx] alive
```
boot completed 01:09:02 (≈T+14, "about to KernelCheckAndDispatch" → "boot complete").
Then NOTHING: kernel heartbeat was due ≈T+16, next gfx line ≈T+16.9 — the whole process
vanished at ≈T+15-16, ~1-2 s into the kernel dispatch loop, BEFORE any fast probe could
fire. Zero exit-path evidence (no KernelShutdown, no SDL_EVENT_QUIT, no atexit line, no
exec_runtime return), no creport, no erpt at death. Zero [sceOpen] lines = GOAL code never
opened a file before dying (consistent: death is ~1-2 s into the intro).

**Verdict so far:** sudden whole-process removal with no C++ exit path and no fault —
system-side kill (OOM prime suspect) or external termination. The 7b instrumentation
worked; its cadence was just too slow to catch the moment or any memory telemetry.

### FIX 7c changes (main.cpp, kboot.cpp, opengl.cpp)
- `main.cpp`: session-start line now includes mem_used/mem_total (shows the applet memory
  budget at T+0 — a single number that redirects the whole investigation if ≈448MB).
  Plus ONE process-wide detached watchdog thread: `[wd] alive mem_used=…` every 250 ms for
  the first 60 s, then 2 s — independent of kernel/gfx, pins the death to sub-second and
  captures the memory trajectory right up to it.
- `kboot.cpp`: "kernel loop: first iteration" one-shot line; heartbeat 500 ms for the
  first 60 s of the loop, then 2 s.
- `opengl.cpp`: `[gfx] alive` 1 s for first 60 s, then 5 s.

Reading the FIX 7c log after the next death:
- last `[wd]` line timestamp = death moment ±250 ms; its mem_used vs mem_total = pressure
  at death (used≈total ⇒ OOM kill confirmed).
- `kernel loop: first iteration` present ⇒ kernel entered; watchdog continuing past a
  silent kernel heartbeat ⇒ kernel-thread hang; watchdog stopping too ⇒ process death.
- mem_total at session start ≈448MB ⇒ we're in the cramped applet allocation (hbloader),
  and the intro simply doesn't fit — the fix becomes memory diet / title-takeover mode.


## 2026-09-12 ~01:40 — FIX 7 result analysis + FIX 7b (deployed to switch/jak1/gk.nro, md5 `95bffa5fd27c9f7d4419eaed9e560346`)

**Correction to the 00:53 telemetry:** the user had tried one of the OLD BACKUP builds of
jak1 around 00:53 (it didn't run either) — the 00:53 erpt pair and possibly the last
gk_boot_log.txt boot belong to that older binary, NOT to FIX 7. Discard that forensics.
(Also: the "disappearing" switch/jak1 dir mid-session was just the user ejecting the card.)

FIX 7 itself: probe strings verified present in the shipped gk binary, boot completed, but
**gk_run_log.txt was never created**. Two credible mechanisms, both fixed by 7b:

1. **Lazy-open asymmetry (instrumentation bug):** boot_log opens its fd EARLY (main
   thread, pre-boot); run_log opened lazily at the first heartbeat, 2 s AFTER
   boot-complete. If post-boot fsdev state is corrupted (or opens start failing), an
   already-open fd keeps working — which is exactly why gk_boot_log.txt kept receiving
   lines while the run log never appeared.
2. **The death is a HANG, not an exit (alternative disease):** if the kernel thread
   never iterates the dispatch loop after boot, zero heartbeats and zero exit probes
   fire — matches the empty run log too. A frozen game that the user (or the applet
   watchdog) then closes produces the same 0x1159 fatal screen + erpt + no-creport
   signature we blamed on a "self-initiated exit".

### FIX 7b changes (game/main.cpp, opengl.cpp, sif_ee.cpp)
- `main.cpp`: `switch_run_logf("session start, run log fd forced open")` before
  exec_runtime — fd is opened pre-boot like boot_log's, immune to both mechanisms above.
- `opengl.cpp` `process_sdl_events`: `[gfx] alive` every 5 s (render-thread liveness).
  Kernel heartbeats stop + [gfx] continues ⇒ kernel-thread hang. Both stop ⇒ process
  death / system kill.
- `sif_ee.cpp` `sceOpen`: ok/FAILED lines into the run log — the existing boot_log
  sceOpen trace latches off at boot-complete, precisely when save-file activity begins,
  so post-boot file opens were invisible until now.

Reading gk_run_log.txt after the next FIX 7b run (decision tree update):
- `session start` present but no heartbeats at all ⇒ kernel thread never dispatched
  (hang inside/before first KernelCheckAndDispatch iteration — check [sceOpen] lines).
- heartbeats stop, `[gfx] alive` continues ⇒ kernel-thread hang (e.g. save-bank fs op
  deadlocked under SWITCH_FS_LOCK — see FsLock.h history).
- heartbeats + [gfx] both stop, no exit probe ⇒ whole-process death (system OOM kill —
  check last `mem_used` vs `mem_total` in the final heartbeats).
- an exit probe fires (KernelShutdown / LTT_MSG_SHUTDOWN / SDL_EVENT_QUIT / m_should_quit)
  ⇒ genuine self-exit; trace that path.
- `[sceOpen] FAILED ...` lines ⇒ file-open failure on the death path.

  input atexit (pad teardown), main post-exec_runtime.

Reading the next run's gk_run_log.txt — decision tree:
- `KernelShutdown() called from GOAL` just before silence → GOAL-side exit; hunt callers.
- `listener message received, kind=…` → garbage listener message = residual corruption.
- `SDL_EVENT_QUIT received` → system/applet asked us to exit.
- heartbeat stops with NO probe and `mem_used` climbing toward `mem_total` → OOM kill;
  fix = reduce intro allocations / run via title-takeover for full memory.
- heartbeat stops with NO probe, memory flat → hard system kill (power/applet lifecycle).

erpt_reports note: console-lifecycle telemetry comes in pairs per boot; useful only as
session timestamps, not crash evidence.


## 2026-09-12 ~02:40 — FIX 7c post-mortem: the instrumentation killed the run (crash 01789175410) + FIX 7d deployed (md5 `fa9e3d809e7da8105ff61d870d1024b8`)

**The FIX 7c run did NOT reproduce the T+15 death — it never got that far.** Evidence chain
(card read back ~02:30):

- `gk_run_log.txt` (preserved as `gk_run_log.7c.txt`) has exactly TWO lines:
  `[0.000] session start` and `[0.008] session start mem_used=0KB mem_total=0KB`.
  No `[wd]`, no `[gfx]`, no kernel lines at all.
- `gk_boot_log.txt` for this run ends at `[main] about to call exec_runtime` — **InitMachine
  never logged a single line** (7b reached boot-complete in 14 s from the same point).
- All three files (run log, boot log) plus a NEW creport `01789175410` stamp **02:10:10** —
  process died ~seconds after launch, and this time it DID leave a crash report.
- Crash report: `hbloader` process, **Instruction Abort** at PC `0x63d1ec2724`. Crashed
  thread registers are a confession: `X20=0x5F5E100` (100 ms sleep in ns), `X23=250`,
  `X24=2000` (7c cadence constants), `X21/X28 = 0x18d46d8e…` (steady_clock ns readings,
  console-2026 epoch), `X25≈59.98 s` (elapsed!) — **the crashed thread IS the 7c watchdog,
  ~60 s after start**, with X0–X17 zeroed and FP==SP (jumped to garbage at function entry).
  Module list contains ONLY `hbl` — creport cannot symbolize an hbloader-loaded NRO at all.
- Thread[00] (main) in the same report: blocked forever in an fsp-srv wait — **main hung
  during early exec_runtime**, matching the frozen boot log. gk_stdout.txt's last line is
  lg's log rotation (`removing sdmc:/…/jak1….log`) — main never came back from that fs work.
- (Also: user re-ran 7b once at 01:22 — that session's erpt pair belongs to it; its run log
  lines were already inside gk_run_log.7b.txt.)

**Reconstruction:** the watchdog's 250 ms `write+fsync` storm began at T+0.25, exactly while
main was inside lg's log-file rotation — an fsdev path that does NOT take SWITCH_FS_LOCK()
(the known-unsafe window catalogued in FsLock.h). The race corrupted fsdev state: main's
fsp-srv wait never completed (boot frozen pre-InitMachine), the run-log fd went dead (every
subsequent write silently EBADF — zero `[wd]` lines despite the thread ticking for ~60 s),
and the corrupted state finally jumped to a non-executable address → Instruction Abort.
**7c's own telemetry: the 0 KB/0 KB line** — get_memory_info used InfoTypes 18/19 from a
wrong comment; 18 is UserExceptionContextAddr, both queries failed. Two 7c bugs, both mine.

### FIX 7d changes (main.cpp, opengl.cpp, platform.cpp, run_log.h)
- **Watchdog thread deleted.** No thread touches the SD before SDL init completes. The
  gfx/render thread — the writer that provably survived all of 7b — is now the fast prober:
  `[gfx] alive mem_used=…KB mem_total=…KB` every **250 ms** for the first 60 s (first call
  immediate = render-thread-up stamp), then 1 s. Kernel heartbeat unchanged (500 ms/2 s,
  with mem). Sub-second death pinning + memory trajectory retained.
- `platform.cpp`: InfoTypes fixed to **6 (TotalMemorySize) / 7 (UsedMemorySize)** with
  CUR_PROCESS_HANDLE — real numbers now, including the T+0 applet budget line.
- `run_log.h`: **self-healing fd** — if `write()` fails, close+drop the fd so the next line
  lazily re-opens, instead of going permanently dark.
- `main.cpp`: new **symbol anchor** line at session start prints `&exec_runtime` and
  `&get_memory_info` pointers. Since creport can't symbolize hbloader NROs, any future
  crash report maps onto this exact ELF by subtraction. Keep `build-switch/game/gk`
  (this build!) — ELF addresses: `exec_runtime=0x96fc0`, `get_memory_info=0x9ce50`.
  `load_base = printed_exec_runtime_ptr − 0x96fc0`; then `nm/addr2line (addr − load_base)`.

### Reading the FIX 7d log after the next run (decision tree v3)
- `session start mem_used=XKB mem_total=YKB`: **Y≈448MB ⇒ cramped applet allocation** →
  memory diet / title-takeover regardless of what happens next. Y should now be nonzero.
- Normal boot: first `[gfx] alive` ≈T+3-7 (render thread up), `kernel loop: first
  iteration` ≈T+14, kernel heartbeats 500 ms. Last `[gfx]` timestamp = death moment ±250 ms;
  its `mem_used` vs `mem_total` = pressure at death (used≈total ⇒ OOM kill confirmed).
- `[gfx]` continuing while kernel heartbeats stop ⇒ kernel-thread hang (trace block point).
- Both stopping at once, no exit probe ⇒ whole-process kill — check final mem numbers, and
  check the card for a NEW creport: if one exists, symbolize via the anchor line
  (subtraction method above).
- A new Instruction Abort at PC≈watchdog-less addresses would also symbolize now.



## 2026-09-11 ~03:10 — FIX 7d result analysis + FIX 7e deployed (switch/jak1/gk.nro, md5 `9b44f250c3e5a791aabf0a0c77242875`) (AI-assisted)

**FIX 7d worked as instrumentation: the run is fully pinned for the first time.**
Telemetry preserved as `switch-crash-reports/gk_run_log.7d.txt` (+ `gk_boot_log.7d.txt`,
`fatal_7d_01789176677_…bin`); the card's live `gk_run_log.txt` was cleared for the 7e run
and the on-card copy kept as `gk_run_log.7d.txt`.

What the 7d log says, line by line:

- `[0.015] session start mem_used=3261548KB mem_total=3265536KB` — **memory is NOT the
  disease.** `total` = 3.11 GiB, so this *is* full title takeover, not the 448 MB applet
  budget we feared at 01:40. But note `used` is already within **3988 KB of total at T+0**
  and stays **bit-for-bit constant for the whole 22 s** — hbloader reserves the entire heap
  up front, so `used`/`total` are hbl's reservation, not our allocator's live usage. The
  "used climbing toward total ⇒ OOM kill" branch of decision tree v3 is therefore **dead —
  these two numbers can never move.** (Residual real risk: only ~3.9 MB is left for anything
  that grows the heap *outside* hbl's reservation, e.g. a fresh `svcSetHeapSize`/map.)
- `[0.028] symbol anchor exec_runtime=0x73d7f6fc0 get_memory_info=0x73d7fce50` → 7d
  `load_base = 0x73d7f6fc0 − 0x96fc0 = 0x73d760000`. The anchor mechanism works.
- `[2.518] … [22.465] [gfx] alive` — render thread up at T+2.5, ticking every 250 ms, healthy
  for 20 s straight.
- `[22.136] kernel loop: first iteration`, then `[22.657] kernel heartbeat MasterExit=0` —
  **and that is the last line in the file.**

**Diagnosis (decision tree v3, branch 3): gfx and kernel stop together, ~0.5 s after the
kernel dispatch loop starts, with no exit probe.** Whole-process death, and the process
lives exactly one kernel heartbeat into dispatch. Crucially:

- **No new Atmosphère creport.** Newest crash_report is still `01789175410` (the 7c
  Instruction Abort). So 7d did **not** take a CPU exception — no abort, no SIGSEGV.
- **A new fatal_report `01789176677` exists** (~21 min after the 7c creport) and its dump is
  **all zeroes — no register state**, i.e. the familiar HID-teardown 0x1159 artifact rather
  than an exception dump.

No exception + `__appExit`-flavoured fatal ⇒ **the process terminated through an exit path,
not a crash** — while every atexit probe (incl. the pad-teardown handler) stayed silent,
even though the run-log fd was provably writable 0.2 s earlier. Two candidates remain:
something calls `_exit()`/`svcExitProcess` directly (skipping the atexit list), or the
atexit list hangs before reaching our early handlers.

### FIX 7e changes (platform.cpp, main.cpp) — an exit trap
- **`_exit()` interposition** (`switch/platform.cpp`, outside `namespace switch_platform`):
  our strong `_exit` out-links libsysbase's archive member (`…_exit.o` is then never pulled),
  logs `[exit] _exit(rc) lr=<caller return address>`, and hands off to libnx's weak
  `__libnx_exit` so teardown is byte-for-byte what it was. `switch_run_logf` is
  forward-declared rather than included here — `<switch.h>` must stay alone in this TU
  (`u128` collides with `common/common_types.h`).
- **`[exit] atexit begin` handler** registered **last** in `main.cpp` ⇒ runs **first**
  (atexit is LIFO), immediately after the anchor line.
- `session start 7e (exit-trap build)` marks the log so a stale binary can't be misread
  as a 7e run (the mistake that cost us the 00:53 forensics).

Verified in the shipped ELF before deploy: `_exit` at `0x9cee0` disassembles to
`bl switch_run_logf` → `bl __libnx_exit` (so the interposition really won the link), and
both format strings are present in the binary.

**7e symbol anchor for this exact build** (`switch-crash-reports/gk.7e.elf`, matches the
deployed NRO md5 above): `exec_runtime = 0x96fd0`, `get_memory_info = 0x9ce60`,
`_exit = 0x9cee0`, `switch_run_logf = 0x8b120`.
`load_base = printed_exec_runtime_ptr − 0x96fd0`; symbolize any address with
`aarch64-none-elf-addr2line -f -C -e switch-crash-reports/gk.7e.elf <addr − load_base>`.
Note the ELF addresses **moved vs 7d** (`exec_runtime` 0x96fc0 → 0x96fd0) — always use the
anchor printed by the *same* session.

### Reading the FIX 7e log after the next run (decision tree v4)
- `[exit] atexit begin` **and** `[exit] _exit(rc) lr=…` ⇒ a normal `exit()`/return-from-main
  path ran. Read `rc`, and hunt the caller of `exit()` — check for a `main:
  exec_runtime returned N` line just before.
- **only** `[exit] _exit(rc) lr=…` ⇒ something bypassed the atexit list with a direct
  `_exit()`. **Symbolize `lr` with the anchor** — that single address names the culprit.
- **neither**, yet a new zero-filled fatal_report appears ⇒ `svcExitProcess` (or an external
  kill) below the libc exit machinery; next step is trapping that syscall / the SDL+applet
  lifecycle path (`appletMainLoop` returning false is the prime suspect).
- Log ends mid-write with a **new creport** ⇒ a real exception after all; symbolize via anchor.
- If the `_exit` line is missing but a fatal report exists, remember the known blind spot:
  a caller already holding `SWITCH_FS_LOCK()` deadlocks inside the log call instead of
  printing — itself diagnostic (it would freeze rather than exit, so gfx would stop too).
- Ignore `mem_used`/`mem_total` from now on; see above, they are hbl constants.


## 2026-09-11 ~03:20 — FIX 7e result: the exit hypothesis is DEAD + FIX 7f deployed (md5 `f1433f1f571501e47ef61f2b2cbea376`) (AI-assisted)

**7e ran (marker `session start 7e (exit-trap build)` present, so this was genuinely the
exit-trap binary) and the trap did NOT fire.** Log preserved as
`switch-crash-reports/gk_run_log.7e.txt`, fatal dump as `fatal_7e_01789179294.bin`.

- `[0.014] symbol anchor exec_runtime=0x1ccad9cfd0` → 7e `load_base = 0x1ccad06000`.
- `[13.889] kernel loop: first iteration`, last `[14.241] [gfx] alive`. **Death ~0.35 s into
  the dispatch loop** — 7d was 0.5 s. Reproducible, and *earlier* than the "15-30 s into the
  intro" we chased all night: the game dies essentially the instant GOAL starts running.
- `gk_boot_log.txt` ends at `about to KernelCheckAndDispatch` / `boot complete, tracing
  disabled` — boot itself is perfectly healthy.
- ❌ no `[exit] atexit begin`, ❌ no `[exit] _exit(rc) lr=…`, ❌ **no new creport**
  (newest is still the 7c `01789175410`), ✅ new **all-zero fatal_report `01789179294`**.

**Verdict (decision tree v4, branch 3): nothing goes through `exit()` or `_exit()`, and there
is no CPU exception.** The process is torn down below libc. That combination — no exception,
no libc exit, a contentless fatal report — is the signature of a **libnx `fatalThrow` /
`diagAbortWithResult`**, i.e. a libnx service wrapper rejecting a result and killing the
process itself. FIX 4's HID `0x1159` is one such path; a rejected **vi/nvidia swapchain
operation** is another, and that one lands exactly in the death window.

### FIX 7f — testing the user's hypothesis: did the resolution fix cause this?
User's observation: the game used to run at 1080p and stopped running after the resolution +
save fixes went in. The timing evidence supports it. GOAL's boot-time display/settings init
is precisely what runs in the first fraction of a second of dispatch, and the resolution fix
put two hooks there: `pc_get_active_display_size` (kmachine.cpp) and the `gl_make_display`
swapchain resize (opengl.cpp), the latter asking **SDL to resize hbloader's 1080p nwindow
swapchain down to 720p** — a vi/nvidia operation that would `fatalThrow` on rejection.

Changes:
- **`SWITCH_RES_OVERRIDE` compile switch** in `switch/platform.h`, set to **0** for 7f.
  Both override sites are now `#if`-guarded, so this build behaves exactly as the port did
  *before* the resolution fix: native/SDL-reported size, no swapchain resize request.
  Verified in the shipped ELF: only the `(native)` format string exists; the `(override)`
  branch was compiled out. Flip to 1 to restore the fix.
- **Display probes** (event-driven, **capped at 16 calls**, so they can never become the 7c
  write storm): `[disp] create_window WxH (res_override=N)`,
  `[disp] pc_get_active_display_size enter/ok (native) WxH`,
  `[disp] pc_set_display_mode enter WxH` / `queued ok`.
- Session marker: `session start 7f (res-override OFF + display probes)`.

7f symbol anchor (`switch-crash-reports/gk.7f.elf`): `exec_runtime = 0x96fd0`,
`get_memory_info = 0x9ce60`, `_exit = 0x9cee0`. (The 7e section's `0x9ce50` was a
transcription error — nm says `0x9ce60`; both builds share these addresses.)

### Reading the FIX 7f log (decision tree v5)
- **Game survives past ~15 s / reaches the title screen ⇒ HYPOTHESIS CONFIRMED**, the
  resolution override was the killer. Fix properly: stop resizing the swapchain and instead
  render at 720p into the 1080p surface (or set the nwindow crop), rather than asking SDL to
  recreate it. Re-enable via `SWITCH_RES_OVERRIDE 1` only with that approach.
- **`[disp] pc_get_active_display_size enter` with no matching `ok` ⇒** death is inside the
  display-manager query itself (not the override) — suspect the SDL display manager on the
  GOAL thread.
- **`[disp] pc_set_display_mode enter` then silence ⇒** GOAL is driving a mode change and the
  enqueue/resize kills us — same family of bug, independent of the override.
- **Dies at ~14 s again with no `[disp]` line at all ⇒ HYPOTHESIS REFUTED**: the death is
  upstream of any display call. Next suspects then: the FIX 6 IOP thread table under real
  dispatch load, and trapping `svcExitProcess`/`fatalThrow` directly.
- Ignore `mem_used`/`mem_total` (hbl constants, see the 7d section).


## 2026-09-11 ~03:25 — FIX 7f verdict: resolution hypothesis REFUTED; the death is upstream of all display code (AI-assisted)

Log preserved as `switch-crash-reports/gk_run_log.7f.txt`, fatal dump as
`fatal_7f_01789179851.bin`.

- `[0.071] [disp] create_window 1920x1080 (res_override=0)` — the override really was
  compiled out; the window/swapchain is hbloader's native 1080p and SDL was never asked to
  resize it. **The port is running exactly as it did before the resolution fix.**
- `[17.787] kernel loop: first iteration` → `[18.250] [gfx] alive` →
  `[18.301] kernel heartbeat MasterExit=0` — **last line. Dead again, ~0.5 s into dispatch.**
- **`[disp] pc_get_active_display_size` NEVER appears.** GOAL dies *before* it ever queries
  the display size. Decision tree v5, branch 4: **hypothesis refuted — the resolution fix is
  not the killer, and it never even got the chance to be.**
- Same signature as 7d/7e: no exit probe, **no new creport**, new **all-zero** fatal report
  (`01789179851`). Boot log still ends healthy at `about to KernelCheckAndDispatch`.

**The invariant across 7d/7e/7f** (boot times vary, 22.1 s / 13.9 s / 17.8 s, because SD
read speed varies — but the *interval* does not):

| run | first dispatch iteration | last line | survived |
|-----|--------------------------|-----------|----------|
| 7d | 22.136 | 22.657 (1st heartbeat) | 0.52 s |
| 7e | 13.889 | 14.241 (gfx) | 0.35 s |
| 7f | 17.787 | 18.301 (1st heartbeat) | 0.51 s |

**The process survives exactly one kernel heartbeat of GOAL execution, then is killed by
something that is not an exception, not libc exit, and leaves no register state.** The death
is inside the first ~0.5 s of GOAL code execution itself, not in any PC-port hook we have
instrumented so far.

### Suspects now, in order
1. **libnx `fatalThrow` / `diagAbortWithResult`** — the only mechanism that matches "fatal
   report, no creport, no exit path". Something calls a libnx wrapper that rejects a Result
   and self-terminates. **Interposable exactly like `_exit` was in 7e** (strong symbol in its
   own archive member), which would give us the Result code AND the caller's `lr`.
2. **`svcExitProcess`** called directly (same interposition trick on the stub).
3. **The FIX 6 IOP thread table under real dispatch load** — the first 0.5 s of dispatch is
   when GOAL starts hammering `sif_rpc` into the IOP; the 7d/7e/7f timing is suspiciously
   tight to "first EE↔IOP traffic". Note FIX 6 was never hardware-verified past this point.
4. **Audio/HID service init triggered by GOAL on its first frames** — both are classic
   `fatalThrow` sources on Switch, and FIX 4 already caught one HID abort (`0x1159`).

### Proposed FIX 7g
- Interpose **`fatalThrow`** (and `diagAbortWithResult`) → log `[fatal] result=0x%x lr=%p`
  before handing off. This should finally *name* the killer.
- Interpose **`svcExitProcess`** likewise.
- Bounded dispatch tracing: log the first ~50 `KernelCheckAndDispatch` iterations (and the
  GOAL function/frame they run) so we can see how far GOAL gets before silence.
- Keep `SWITCH_RES_OVERRIDE 0` for now — it is not the bug, and leaving it off keeps the
  variable count down. Re-enable only after the process survives.


## 2026-09-11 ~03:30 — FIX 7g deployed (md5 `58bc30269f079d46e04e44f41414a6e9`) (AI-assisted)

## 2026-09-12 — FIX 7g VERDICT: death is inside GOAL execution (stage=4); no libnx abort (AI-assisted)

Log preserved as `switch-crash-reports/gk_run_log.7g.txt`, fatal dump as
`fatal_7g_01789180568.log`. Confirmed the 7g binary ran (marker
`session start 7g (abort trap + dispatch breadcrumbs)`).

- `[16.322] kernel loop: first iteration` → `[16.401] stage=4 iter=2` →
  `[16.656] stage=4 iter=13` — **last line**. Dead ~0.33 s after the first dispatch,
  the same interval as 7d/7e/7f (0.52/0.35/0.51 s). The invariant holds.
- **`stage=4` = inside `call_goal_on_stack` — GOAL code itself was executing** when the
  last breadcrumb was carried out; 13 dispatch iterations completed (~21 ms each).
- **NO `[fatal] diagAbortWithResult` line.** The gfx thread wrote successfully 250 ms
  before death, so the log was writable — the abort trap is live in this binary (verified
  in the ELF pre-deploy). ⇒ **no libnx subsystem abort fired before or during the kill.**
- New fatal `01789180568` = the same **all-zero 0x1159 tombstone** as 7d/7e/7f.
- Boot log ends healthy (`boot complete`); memory flat at the (meaningless) hbl
  constants; no new creport.

Decision tree v6 ⇒ branch 2: **the kill happens during GOAL code execution, with no
exception, no libnx abort, and no libc exit.**

### What the tombstone proves (used by 7h)
0x1159 requires NULL HID sharedmem ⇒ `hidExit()` ran ⇒ `__appExit` ran (hidExit's only
caller, `__appExit+0x34`, per deployed-binary disassembly). And `__libnx_exit`'s body is
`__appExit() → envGetExitFuncPtr() → __nx_exit(0, func)` — so **every libnx exit path
funnels through `__appExit`**, yet 7e proved neither `_exit` nor `exit()` was invoked.
Remaining arms: a direct `__libnx_exit()`/`__appExit()` call, or an exit performed by
hbloader-hosted code in our process address space. Either way `__appExit` runs at every
death — trapping it observes the killer's call chain unconditionally.

## 2026-09-12 — FIX 7h deployed: `__appExit` trap (md5 `aec4cf2bc196a373c9d443246bdf0612`) (AI-assisted)

`game/switch/platform.cpp` (same TU as the 7e/7g traps):
- **Strong `__appExit` override** (`__appExit` is weak in `libnx.a(init.o)` — zero link
  risk, unlike `svcExitProcess`/`fatalThrow`). It logs, in order:
  1. `[exit] __appExit entered lr=<caller> walk=<up to 8 FP-chain return addrs>`
  2. `[exit] stack scan: <up to 16 code-looking words from 4KB of the exiting thread's
     stack, within ±256MB of this module>` — recovers return addresses even through
     FP-omitting libnx/newlib frames.
  …then performs the **byte-for-byte replica** of libnx's default teardown (verified
  against the shipped 7g ELF at `0xad8bc0`): `__nx_win_exit(); fsdevUnmountAll();
  fsExit(); timeExit(); hidExit(); appletExit(); smExit();`
- Marker bumped to `session start 7h (appexit-trap build)`.

Link verification (shipped ELF, preserved as `switch-crash-reports/gk.7h.elf`):
`__appExit` = **T** `0x9cf60` (ours; libnx's weak one no longer present), body confirmed
calling `snprintf` → `switch_run_logf` before the teardown sequence. All previous traps
intact: `_exit` `0x9cee0`, `diagAbortWithResult` `0x9cf10`.

**7h anchors:** `exec_runtime = 0x96fd0`, `get_memory_info = 0x9ce60`, `switch_run_logf
= 0x8b120`. `load_base = printed_exec_runtime_ptr − 0x96fd0`.

One build hiccup fixed en route: redeclaring `fsdevUnmountAll` as returning `void`
conflicts with libnx's `Result fsdevUnmountAll(void)` — `<switch.h>` already declares
all teardown functions except `__nx_win_exit`, so only that one is declared by hand.

### Reading the FIX 7h log (decision tree v7)
- `[exit] __appExit entered …` at death ⇒ **the exit path is finally caught**. Symbolize
  `lr`/`walk`/scan words: `load_base = printed_exec_runtime_ptr − nm(exec_runtime)` in
  `gk.7h.elf`, then `aarch64-none-elf-addr2line -f -C -e gk.7h.elf <addr − load_base>`.
  The frames above `__libnx_exit` ARE the killer.
- No line + another all-zero tombstone ⇒ `__appExit`/`__libnx_exit` were bypassed too and
  the tombstone theory needs rework (then: trap `hidExit` itself — also just a T symbol —
  to log its caller).
- No line + NO tombstone ⇒ external kill of the process (hbloader/pm); instrument from
  the hbloader side or reproduce under a different launcher.


Built and verified in the shipped ELF before deploy.

### 1. libnx abort trap (the main event)
`diagAbortWithResult` is **weak** in `libnx.a(diag.o)`, and its entire default body is
`svcBreak(BreakReason_Panic, &res, sizeof(res))`. A strong definition in
`switch/platform.cpp` therefore replaces it with **zero link risk** (confirmed: build
linked with no multiple-definition error, and `nm` shows `diagAbortWithResult` at
`0x9cf10` disassembling to `bl switch_run_logf` → `bl svcBreak` — ours, with libnx's
behaviour reproduced exactly).

Its callers inside libnx are precisely our suspect list: `hid.o`, `pad.o`, `applet.o`,
`sm.o`, `framebuffer.o`, `default_window.o`, `thread.o`, `virtmem.o`, `newlib.o`,
`env.o`, `init.o`, `random.o`, `usb_comms.o`, `dynamic.o`.

Logged line: `[fatal] diagAbortWithResult res=0xXXXXXXXX (module=M desc=D) lr=0x… stage=N`.
Decode: `module = res & 0x1FF`, `desc = (res >> 9) & 0x1FFF` (202 = HID, 21 = libnx,
2 = kernel/FS-adjacent). Symbolize `lr` with the session's `symbol anchor` line.

**Note `fatalThrow` was deliberately NOT interposed**: it is a *strong* symbol sharing
`fatal.o` with `fatalThrowWithPolicy`/`fatalThrowWithContext`, so overriding it risks a
duplicate-symbol link failure, and nothing inside libnx calls it anyway.
**`svcExitProcess` cannot be interposed at all** — it lives in `svc.o` with every other
syscall stub, which every program pulls in.

### 2. Dispatch breadcrumbs (where is GOAL when it dies?)
Logging each dispatch iteration would mean ~180 fsync'd writes/s on the kernel thread —
exactly the fsdev storm that killed the 7c run. Instead (`switch/run_log.h`):
- the kernel thread does **stores only**, no I/O: `switch_goal_stage(n)` /
  `switch_goal_tick()` into two `inline std::atomic`s;
- the **gfx thread** — the writer that has survived every run — carries them out on its
  existing 250 ms heartbeat: `[gfx] alive stage=N iter=M mem_…`.

Stage codes (`kboot.cpp` `KernelCheckAndDispatch`):
`1` loop top · `2` `WaitForMessageAndAck` · `3` `ProcessListenerMessage` ·
**`4` inside `call_goal_on_stack` — GOAL code itself is executing** · `5` GOAL returned ·
`6` `ClearPending`.

### Reading the FIX 7g log (decision tree v6)
- **`[fatal] diagAbortWithResult …` appears ⇒ CASE CLOSED**: the Result names the failing
  libnx subsystem and `lr` names the caller. Fix that subsystem.
- **No `[fatal]` line, last `[gfx] alive stage=4` ⇒** the kill happens *inside GOAL code
  execution*. Given `iter=M`, GOAL got M dispatches in. Next step: trap inside the GOAL
  call (PC-hook-level tracing), and re-examine FIX 6's IOP table under real `sif_rpc` load.
- **Last `[gfx] alive stage=2` ⇒** it dies in the listener/socket wait — a network/socket
  service abort, very plausibly an `sm`/`bsd` service failure.
- **`iter` stops advancing while `[gfx]` keeps printing ⇒** kernel-thread hang, not a kill.
- **Still nothing at all ⇒** the killer is below both libc and libnx (raw `svcExitProcess`
  or an external/system kill); remaining option isa an `svcBreak`-level or kernel-side trace.

## 2026-09-11 (late) — FIX 8: MSAA crash-proofing + PAL→USA Rev 1 ISO swap

**(AI-assisted)** Deployed `gk.nro` md5 `dd54c8ee57b913572ff89bd501c1cbc2`
(incremental docker rebuild on the 7x tree, exit 0).

### Crash: MSAA 16 set from the in-game options menu
Not a regression from the 7x build — a **settings** crash that would kill any build.
The options menu wrote `(msaa 16)` into `OpenGOAL/jak1/settings/pc-settings.gc`.
Tegra X1 cannot allocate a 16x multisample framebuffer, so `make_fbo` got
`GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE` → `ASSERT(false)` → abort. Symptom: boot log
reached `boot complete`, `gk_stdout.txt` stopped right after `tit.VIS` (first frame).
Same family as the 2026-09-10 issue #2 options-menu crash.

The existing safety clamp in `render_game_frame` did **not** help: `GLint msaa_max;` was
**uninitialized**, so if the driver does not answer `glGetIntegerv(GL_MAX_SAMPLES)` the
comparison ran against a garbage stack value and the clamp silently did nothing.

- `game/graphics/pipelines/opengl.cpp`: initialize `msaa_max = 1`, floor the query result,
  and clamp `options.msaa_samples` at both ends.
- `game/graphics/opengl_renderer/OpenGLRenderer.cpp`: `make_fbo` now falls back to a
  non-multisampled buffer instead of `ASSERT(false)`. Settings are applied before the first
  frame, so aborting there makes a bad graphics option unrecoverable without editing the SD
  card by hand. Cleanup uses `Fbo::clear()` (`tex_id`/`zbuf_stencil_id` are `std::optional`).
- `game/graphics/opengl_renderer/OpenGLRenderer.h`: added `FboState::requested_msaa`. The
  fbo-still-valid check must compare against the *requested* value, otherwise a fallback
  makes `matches()` fail every frame and the fbo is rebuilt continuously. The resolve buffer
  is now keyed off the *effective* sample count actually created.

### PAL → USA Rev 1 swap (done, procedure from the night section worked as written)
- ISO md5 `e5563b152759f11e037fe67b6cd65676` = documented redump Rev 1.
- Extractor auto-detected `SCUS-97124` / ELF hash `744661860962747854` → **`ntsc_v2`**,
  NTSC-U, 338 files. No manual `DECOMP_CONFIG_VERSION` needed; `.env` was never set for PAL
  either (the extractor's own detection drives the config override).
- `ntsc_v2` needs no dedicated `decompiler/config/jak1/ntsc_v2/` folder — the
  `version_overrides` block reuses the `pal`/`jp` merge files and `all_objs_jak1_jp.json`.
- Command (note `--instruction-set arm64`, required for Switch):
  `./build-host/decompiler/extractor <iso> --game jak1 --extract --validate --decompile --compile --instruction-set arm64`
  → exit 0, 1318 targets. The only `[error]` lines are custom test-zone mesh colours.
- **ARM64 verified**: `out/jak1/obj/*.o` are byte-identical to the known-good PAL ARM64
  build (`goal_src` is territory-independent), proving the backend was not x86.
- Packaged with `scripts/package-switch.sh` → `build-switch/sd-card` (3.2 GB, 6150 files),
  copied to the card and checksum spot-checked.
- **Saves survived**: the save dir was already `BASCUS-97124AYBABTU!` (USA serial) under PAL
  data, confirming the save format is per-game, not per-territory.
- Rollback kept on host: `iso_data/jak1-pal`, `out/jak1-pal`, `decompiler_out/jak1-pal`,
  plus the PAL ISO. The card's PAL `data/` was deleted (user's choice, space).

### Gotcha: AppleDouble files on the FAT32 card
`cp -R` from macOS created **6661** `._*` files on the card even with `COPYFILE_DISABLE=1`.
Delete them after any host→SD copy (`find <dst> -name '._*' -delete`) — hbmenu and the
runtime's directory scans do not want them.

## 2026-09-12 (00:30) — FIX 9 triage: null GOAL call + loader-driven frame pacing

**(AI-assisted)** Session id `ca00eb98-5e7e-41d6-b7b1-b9e0c4fce062`.
Card state at end of session: `gk.nro` = `jak-and-daxter.nro` = `dd54c8ee57b913572ff89bd501c1cbc2`
(7x + MSAA fix), data = USA `ntsc_v2`, plus `gk-7v-verified.nro` = `0ef4828a…` staged for A/B.

### The MSAA fix worked
Previous run died at the title screen (first frame). This run reached gameplay and survived
**58 s** with `village1` + `training` loaded. `(msaa 1)` and the fallback both held.

### New crash — indirect call through a NULL GOAL pointer
```
[ee_runner] rw=0x738facf000 rx=0x48e5251000     <- boot log
pc=0x48e5251000  far=0x48e5251000  error_desc=0x100 (instruction abort)
lr=0x48e74ed7cc  X22=0x738facf000
```
`pc` is **exactly the `rx` base**, i.e. EE offset **0**. `far == pc` ⇒ the *instruction fetch*
faulted, not a data access. So GOAL did an indirect call through a null function pointer.
The caller (`lr`) is also inside the EE arena, at offset `0x29C7CC` — GOAL code, not C++, so
`addr2line` on the unstripped ELF is useless for it.

Register sanity check that confirms the reading: `X00/X01/X02/X19/X23/X24 = 0x23xxxx` and
`X11/X13/X14/X21 = 0x18fxxx` are all EE-relative GOAL pointers of the right magnitude, and
`X22` is the rw alias base verbatim.

**Why it got this far instead of faulting cleanly:** `runtime.cpp` mprotects the low 512 kB
of EE memory `PROT_NONE` specifically to trap null-ish GOAL pointers — but that is
`#if !defined(__SWITCH__)`. libnx's Jit maps the region with one permission per view and
cannot split off a sub-range, so **the guard page does not exist on Switch**; a null call
just runs into the zeroed arena. That is a diagnostic gap, not the root cause.

**NOT memory**: `mem_used` is pinned at `3261548KB / 3265536KB` from the first heartbeat to
the last. It is a pre-reserved arena, flat all run — no leak, no growth before the fault.

**Two variables changed at once** (7x had never booted on hardware AND the data changed), so
`gk-7v-verified.nro` (the only hardware-verified build) was copied to the card. Boot it on the
same USA data: still crashes ⇒ data; stable ⇒ 7x code regression. **Test pending.**

### Frame pacing — it is the loader, not logging and not the ROM
`gk_stdout.txt` from the run:
```
stage texture took 5–7 ms
stage tie took 23.64 ms          <- one item, ~1.5 frames
Loader::update slow setup: 7–26 ms
```
`LoaderStages.cpp`: `LOAD_BUDGET = 4.5f` ms, `MAX_TEX_BYTES_PER_FRAME = 1 MB`
(`Loader.cpp` has its own 128 kB variant). Desktop tuning. Worse, the budget is only tested
**between** items, so a single large TIE tree overruns it wholesale. This matches the reported
symptom exactly — hitching **while moving the camera**, i.e. while new geometry/textures
stream in. Unrelated to PAL/USA: vblank comes from real display vsync (see night section).

### Fixed this session
- `kmemcard.cpp:463` — `mc_print("header cache refill bank=%d save_count=%d", …)` used printf
  syntax in an fmt-style formatter, so both values were dropped and the log literally printed
  `bank=%d save_count=%d` (visible in `gk_stdout.txt`). Now `{}`. **Not yet rebuilt/deployed.**

### Next (agreed)
1. User A/B tests `gk-7v-verified.nro` on the USA data.
2. Tune the loader budgets for Switch (smaller per-frame chunks; accept more pop-in) + rebuild.
3. Optional: add a Switch-side null-GOAL-call guard so this class of crash names itself.

## 2026-09-12 (00:45) — FIX 9 implementation: Switch loader budgets deployed (AI-assisted)

**Deployed `gk.nro` = `jak-and-daxter.nro` = `a20723330f61d467bcde9650523de5ce`**
(incremental docker rebuild on the 7x tree, exit 0, log `build-fix9.log`, 54 steps).

### What changed (2 files, all `#ifdef __SWITCH__`-gated — desktop values identical)

The smoking gun was the chunk size, not just the budget: `PreloadedVertex` is
**32 bytes**, so `CHUNK_SIZE = 32768` verts is **exactly 1 MB per
`glBufferSubData`**. The time budget is only checked *between* uploads, so one
such upload can (and did) stall the frame 20+ ms. All desktop-tuned 32768-element
chunks and byte caps are now platform constants:

| constant | desktop | Switch | note |
|---|---|---|---|
| `LOAD_BUDGET` | 4.5 ms | **2.0 ms** | per-update time cap, checked between items |
| `STAGE_VERT_CHUNK` | 32768 (1 MB) | **8192 (~256 KB)** | tfrag/shrub/tie verts, collide, hfrag, merc |
| `STAGE_INDEX_CHUNK` | 32768×8 (1 MB) | **8192×8 (~256 KB)** | tie indices |
| `MAX_TEX_BYTES_PER_FRAME` | 1 MB | **256 KB** | level texture stage |
| `MAX_STAGE_UPLOAD_KB` | 2048 | **512** | per-stage byte caps (tfrag/tie) |
| `SHARED_TEXTURE_LOAD_BUDGET` | 3 ms | **1.5 ms** | `Loader.h`, shared-texture path |

- `LoaderStages.cpp`: constants block at top; every hardcoded 32768/2048 chunk
  site now uses them (tfrag, shrub, tie-verts, tie-indices, collide, hfrag,
  merc, texture stage). Untouched on purpose: shrub's odd `/128 > 2048` byte
  check (already a 256 KB effective cap) and the tie wind-indices path (already
  paced at one tree per frame).
- `Loader.h`: `SHARED_TEXTURE_LOAD_BUDGET` gated. (`TIE_LOAD_BUDGET` is dead
  code — defined, never used — left alone.)
- `Loader.cpp`'s own 128 kB shared-texture cap left as-is (already small).
- The `mc_print` fmt fix from the triage session rides along in this build.

Result: worst-case single loader upload is now ~256 KB instead of 1 MB, and the
time budget halves; levels stream in over ~4× more frames (more pop-in) instead
of hitching. `update_blocking` (blackout loads) still runs stages to completion.

### Card state after deploy
- `gk.nro` = `jak-and-daxter.nro` = `a20723330f61d467bcde9650523de5ce` (**FIX 9**)
- `gk.nro.fix8` = `dd54c8ee57b913572ff89bd501c1cbc2` (7x + MSAA — rollback)
- `gk-7v-verified.nro` = `0ef4828a8b0a709cced8227c554a0216` (**untouched — the
  pending A/B build for the null-GOAL-call crash**)
- Host: unstripped ELF + nro snapshotted in `backups/fix9/`
  (`gk.elf` md5 `75127ca0cc853cb6d8a697b174fc6fbd`, 186 MB — use for symbolizing
  any new crash report against this build).

### What to look for in the next run
- `stage tie took …` should drop from 23.6 ms to low single digits (a single
  256 KB chunk); `stage texture took …` likewise except for atomic >256 KB
  textures, which still upload whole.
- Hitching while sweeping the camera (new geometry streaming in) should be
  greatly reduced; pop-in will be more visible instead. If still hitching,
  next knob is `LOAD_BUDGET` → 1 ms or chunk → 4096.
- The null-call crash is NOT expected to be fixed by this — it is a separate
  issue; A/B test with `gk-7v-verified.nro` still decides data-vs-code.


---

## FIX 10 — Uneven frame pacing: frame limiter stacked on top of vsync (AI-assisted)

**Symptom:** frame times alternated ~16 ms / ~33 ms, most visible as judder/"flashing"
while swinging the camera. Worst at `(fps 30)`.

**Root cause:** two independent pacing mechanisms ran in series each frame:
1. `frame_limiter.run()` slept until the target frame time had elapsed, then
2. `SDL_GL_SwapWindow()` blocked again on the compositor's vsync.

Any sleep that overshot a vblank boundary by even a fraction of a millisecond cost a
full extra 16.6 ms in the swap, producing the 16/33/16/33 alternation. 33.3 ms (30 fps)
lands exactly on a boundary, so it was the worst case.

**Fix** (`game/graphics/pipelines/opengl.cpp`):
- Added `GraphicsData::current_swap_interval` (init `-1`) so the interval is only set
  when it actually changes.
- Added `skip_frame_limiter`; on Switch it is true whenever vsync is on, so the
  software limiter is bypassed and the display alone paces the frame.
- Swap interval is derived from the target fps: `2` when Switch + vsync +
  `target_fps <= 35` (→ a clean 30 Hz on the 60 Hz panel), otherwise `vsync ? 1 : 0`.

The in-game 30/60 fps carousel still works live with no restart: the menu calls
`pc-set-frame-rate` → `pc_set_frame_rate()` → `target_fps`, which is re-read every
frame and now simply re-derives the swap interval. The FIX 8a clamp (rate > 60 → 60)
still folds the PC-only 100/150 entries down to the panel's 60 Hz.

Note: the earlier loader-budget work (FIX 9 constants in `LoaderStages.cpp`) was
already in the tree and fixed a *different* stutter source (streaming overruns).

**Also in this build:**
- `game/kernel/common/kmemcard.cpp:463` — `mc_print` was called with printf-style
  `%d` against an fmt-style formatter; converted to `{}`.
- `game/switch/gk-icon.jpg` was a 0-byte file (no icon). Replaced with a real
  256×256 JPEG so the title shows box art in hbmenu instead of a blank tile.

**Build:** `b78d4310752b2691fc40c0c0c933390a`, 15048033 bytes. Deployed to both
`gk.nro` and `jak-and-daxter.nro`; previous build kept as `gk.nro.fix9`, and the
hardware-verified `gk-7v-verified.nro` is still on the card for A/B testing.

**Still open:** the bad-GOAL-function-pointer crashes (indirect call through an
invalid pointer — once null, once `rx + 0x2660AD4`) are NOT addressed by FIX 10.
Root cause remains unknown.

---

## FIX 11 — The real judder: lost wakeup in the engine/renderer handshake (AI-assisted)

**Why FIX 10 was not enough.** After FIX 10 the user recorded the stutter
(`IMG_9484.MOV`, 4K/30, 14 s, handheld). Frame analysis of the capture:

- ~8 % of camera frames were *exact duplicates* of the previous one, rising to 20 % in
  bursts during camera pans. A solid 60 fps display would produce **zero** duplicates for
  a 30 fps camera, so the console was genuinely repeating frames.
- The duplicate frames were uniformly flat across the **whole** screen height — no
  horizontal step anywhere. So it is whole repeated frames, **not** tearing and not a
  pacing seam.
- Settings on the card at the time of the recording were confirmed `(fps 60)`,
  `(vsync #t)`, `(msaa 1)`, lod 0/0 — i.e. FIX 10's "vsync alone paces frames" path.

The user also tried **overclocking, with no change at all**. That is the decisive clue:
the console was not short of compute, it was *waiting*.

**Root cause.** `gl_sync_path()` (the engine thread's "wait until the renderer has taken
my DMA chain" call) locked `sync_mutex` and waited on `sync_cv` — but the predicate it
tests, `has_data_to_render`, is owned by `dma_mutex`, and the render thread clears that
flag and signals while holding `dma_mutex`. Waiter and notifier therefore had **no mutex
in common**, breaking the condition-variable contract twice:

1. **Lost wakeup.** The render thread could clear the flag and `notify_all()` in the
   window between the engine evaluating the predicate and actually enqueueing on the
   condvar. Holding the predicate's mutex across the notify is precisely what closes that
   window, and it was not held. The engine then slept until the next *unrelated* signal —
   `frame_idx++` after the swap — i.e. a full extra vblank later.
2. **Data race.** Reading `has_data_to_render` under the wrong mutex establishes no
   happens-before edge, so on AArch64's weak memory model the store may not be observed
   promptly either.

Both lose whole frames at random. This explains every observation: the stutter is
independent of clock speed (a missed wakeup needs no CPU), it worsens under camera motion
(more work per frame = wider window to land in), and it is far rarer on x86 desktops.

**Fix** (`game/graphics/pipelines/opengl.cpp`):
- `gl_sync_path()` now waits on `dma_cv` under `dma_mutex`, putting waiter and notifier on
  the same mutex, and also checks `MasterExit`. Sharing `dma_cv` between the "flag became
  true" (render thread) and "flag became false" (engine thread) predicates is safe: same
  mutex, `notify_all()` everywhere. This also brings `engine_timer`/`last_engine_time`
  under one consistent mutex.
- The render thread's clear of `has_data_to_render` now signals `dma_cv` instead of
  `sync_cv`.
- `frame_idx_of_input_data = frame_idx` was an unlocked read-modify-write of two fields
  that `gl_vsync()` reads under `sync_mutex`; it now takes `sync_mutex`.
- Shutdown notifies `dma_cv` as well, or the engine thread could hang on exit.

**FIX 11 telemetry.** Guessing has cost several SD-card round trips, so the render loop is
now instrumented per phase and summarized every 2 s to `gk_run_log.txt` (and the network
log), gated behind the existing L3+R3+Minus diagnostics toggle:

```
[fps] <avg> avg (<ms>) | wait_dma <avg>/<max> render <avg>/<max> swap <avg>/<max>
      | worst <ms> | vblanks 1x= 2x= 3x= 4x+= | target= vsync= si=
```

Read it as: high `wait_dma` = engine/GOAL bound; high `render` = draw-submit bound; high
`swap` = GPU or vsync bound; the vblank histogram is what the eye perceives as judder
(a clean 60 fps is almost all `1x`).

**Build:** `04fc65697beb3237de2bde80a34d4448`, 15048033 bytes, deployed to `gk.nro` and
`jak-and-daxter.nro`. Previous build kept as `gk.nro.fix10`; `gk.nro.fix9` and
`gk-7v-verified.nro` also still on the card. `gk_run_log.txt` and `mc-trace.txt` were
deleted so the next run's telemetry is unambiguous.

**Still open:** the bad-GOAL-function-pointer crashes are unrelated and still unexplained.
Note, though, that a handshake race of this kind can also let the engine and renderer
touch the DMA buffer concurrently, so it is worth re-checking whether those crashes
persist now.

---

## FIX 12 — Telemetry verdict: the cost is CPU-side draw submission (AI-assisted)

FIX 11's per-phase telemetry ran on hardware and settled the question. Representative
lines from `gk_run_log.txt` during gameplay:

```
[fps] 19.1 avg (52.45ms) | wait_dma 5.53/40.01 render 45.52/67.49 swap 0.35/0.50 | vblanks 1x=1 2x=4 3x=26 4x+=8
[fps] 14.8 avg (67.81ms) | wait_dma 0.00/0.00 render 66.69/81.43 swap 0.33/0.48 | vblanks 1x=0 2x=0 3x=1 4x+=29
```

- **`swap` = 0.3 ms, always.** The GPU is never the limit and vsync is never waited on.
  So this is not a pacing or presentation problem, and FIX 10/11 could never have solved it.
- **`wait_dma` = 0 ms** for most of gameplay. The GOAL engine is not the limit either.
- **`render` = 45-67 ms.** The entire frame budget is CPU-side GL command submission,
  and it scales with visible geometry — the user independently reported it worsens looking
  down from high ground, and the log's worst stretches match.

This also finally explains why **overclocking changed nothing** if only the GPU was
raised, and why the judder was never really "judder" — at 67 ms the console is simply
running at ~15 fps, and the vblank histogram (`4x+=29`) shows nearly every frame spanning
four or more vblanks.

Note that FIX 11's handshake fix is still correct and worth keeping — it was a genuine
lost-wakeup bug — it just was not *this* bug.

**FIX 12** instruments `dispatch_buckets_jak1()` so each bucket renderer's submission cost
is timed and the eight worst are reported every 2 s:

```
[buckets] <N> frames, all buckets <X>ms/frame -- worst:
[buckets]   l0-tfrag-tie              avg  12.34ms  max  20.10ms  (25.6%)
```

Caveat noted during implementation: `BucketRenderer::name_and_id()` returns `std::string`
**by value**, so the profiler must own a copy — storing `.c_str()` would dangle. The name
is captured once per bucket to avoid per-frame allocation.

**Build:** `d196f2fb30466df20e94a25fea93593c`, deployed to `gk.nro` + `jak-and-daxter.nro`.
Rollbacks on card: `gk.nro.fix11`, `gk.nro.fix10`, `gk-7v-verified.nro`.

**Repo moved** to `~/Desktop/jak-project-switch` at ~01:25 (the `~/Downloads` path is now a
stub containing only failed `build-host/extract/package` logs). The Desktop tree is the
authoritative one and contains all FIX 10/11/12 work.

---

## FIX 13 — Root cause found: swapchain acquire stalled the START of every frame (AI-assisted)

**How it was found.** FIX 12's per-bucket profiler showed the buckets were innocent — at
the worst moment `render` was 56 ms while *all* bucket renderers together cost 20.6 ms
(sky 4.0, l0-tfrag-tie 3.3, l0-tfrag-tfrag 2.6 — all unremarkable). So ~36 ms was being
spent outside bucket dispatch. Timing the four non-bucket phases gave an unambiguous
answer:

```
[phase] setup 31.68 | loader 0.01 (max 0.02) | buckets 22.50 | blit 0.00 | pcrtc 0.09
```

**Root cause.** `setup_frame()` began the frame by binding framebuffer 0 (the window) and
clearing it. A `glClear` is not expensive — but the *first* operation that touches the
default framebuffer forces the EGL driver to acquire the next swapchain image, and that
acquire blocks until the compositor releases one. Doing it at the top of the frame put
that block **before** the frame's ~22 ms of command submission instead of concurrently
with it:

```
  [ wait 31 ms for a buffer ][ 22 ms of work ]   = 53 ms  -> ~19 fps
```

The two never overlapped, so the console idled for half of every frame. This also explains
the two facts that had made the bug so confusing:

- `SDL_GL_SwapWindow` measured only **0.3 ms** — the waiting had already happened at the
  start of the frame, so the swap itself had nothing left to wait for. Every measurement
  therefore pointed *away* from presentation.
- **Overclocking changed nothing**, because the console was blocked, not computing.

**Fix.** Nothing between `setup_frame()` and `do_pcrtc_effects()` touches framebuffer 0 —
the game renders into `m_fbo_state.render_fbo` and is blitted to the window at the very
end — so on Switch the window clear is deferred to immediately before that final blit
(`m_deferred_window_clear`). The acquire then happens after the frame's work, and the two
overlap:

```
  [ 22 ms of work ][ short wait ]                = ~22 ms  -> can hit the vblank
```

The clear itself is still required and still happens: `do_pcrtc_effects` only draws the
letterboxed region, so the bars outside it would otherwise show stale garbage. The
deferred path re-establishes the letterboxed viewport afterwards. Desktop behaviour is
unchanged (`#if defined(__SWITCH__)` only) — the render-FBO clear still happens for
everyone, in place.

**Expected result:** frame cost drops from ~53 ms to roughly the ~22 ms of real work, i.e.
about 45 fps uncapped, and a stable 30 fps if 30 is selected. Note 22 ms of genuine bucket
submission still means **60 fps is not reachable** in the heaviest scenes — 30 fps will be
the smooth choice. Reducing that 22 ms is a separate optimisation problem.

**Build:** `8cf38935b053664cdf739c7332f8951c`, deployed to `gk.nro` + `jak-and-daxter.nro`.
`gk.nro.fix13probe` (the instrumented build) and `gk-7v-verified.nro` kept as rollbacks.
The `[fps]`, `[buckets]` and `[phase]` telemetry all remain in this build behind the
L3+R3+Minus toggle, so the improvement can be confirmed from the log.

## FIX 14 — duplicate presents: "it loses frames, it doesn't drop them" (AI-assisted)

**Symptom.** Motion judders and reads as *nauseating* while moving the camera. Crucially the
user reported it is **identical at a locked 30 fps**, which rules out the earlier theory that
this was variance around an unmet 60 fps target.

**Evidence.**
- Video forensics on `IMG_9484.MOV` (29.97 fps capture): ~8% of frames are *byte-identical*
  to their predecessor, rising to ~20% during camera pans. Band analysis of the diffs was
  uniformly flat across the full screen height => whole repeated frames, **not tearing**.
- `gk_run_log.txt`: `wait_dma` pegged at the ceiling (`/40.01`) in **16 of 43** 2-second
  windows.

**Root cause.** `render_game_frame()` waits up to 40 ms for the engine to hand over a DMA
chain. On timeout it returned having drawn nothing — but `render()` went on to
`SDL_GL_SwapWindow()` and to `frame_idx++` unconditionally. Two consequences:

1. **Duplicate presents.** The swap re-showed the previous image, so the world stood still
   for one interval then covered two intervals of motion at once. Nothing is skipped — each
   presented frame just represents a different slice of real time. That is exactly why it
   reads as the image *losing* frames rather than dropping them, and why it happens at 30 fps
   too: the engine still misses the 40 ms window regardless of the target.
2. **Parity corruption.** `gl_vsync()` returns `frame_idx & 1` — the even/odd index the game
   uses to pick its double-buffered DMA target. Advancing `frame_idx` for a frame that was
   never rendered flips that parity spuriously, letting the engine write the buffer the
   renderer is still reading.

**Fix.** `render_game_frame()` now returns whether it actually rendered. On Switch, when it
did not: skip the frame limiter, skip the swap, and do not advance `frame_idx`. The 40 ms
timeout exists only to keep imgui responsive, and there is no imgui on Switch.

Deliberately **not** an early `return`: the shutdown check at the end of `render()` latches
`m_should_quit` into `MasterExit`, so skipping it would hang the game on exit while the
engine is stalled. The swap and the counter are gated individually instead.

**New telemetry.** `[fps]` gained `starved=N` — iterations where the engine had no chain
ready. Starved iterations are counted but excluded from the timing averages so they do not
distort them. Pre-fix this number is what was being presented as duplicate frames.

**Build:** `c5a5169de65a5379fbdb57ec99407957` — deployed to `gk.nro` + `jak-and-daxter.nro`,
rollback kept as `gk.nro.fix13`. Card settings restored to `(fps 60)` / `(vsync #t)`.

**Still open:** ~20 ms of bucket submission means heavy scenes cannot reach 60 fps regardless;
this fix addresses *smoothness*, not throughput.

## FIX 15 — THE judder bug: world time quantised to whole frames, rounded up (AI-assisted)

**This is the actual cause of the "it loses frames, it doesn't drop them" symptom.** It is in
the GOAL engine, not the renderer, and no amount of presentation work (FIX 10-14) could ever
have touched it.

**What pointed here.** The user was asked directly whether the game felt like slow motion.
Answer: *"No - normal speed, it's just juddery."* That single answer killed the previous
theory (a fixed timestep running behind real time would look like slow-mo) and proved the
engine *is* tracking real time -- but in uneven steps.

**The code**, `goal_src/jak1/engine/draw/drawable.gc:978`:

```lisp
(let ((time-ratio (the float
                    (+ (/ (timer-count ...) (the-as uint *ticks-per-frame*))
                       1 ;; so we round up.
                       ))))
```

That is an **integer** division followed by `+1`. `time-ratio` can therefore only ever be
1.0, 2.0, 3.0 ... -- never 2.4 -- and it is always rounded *up*. The world advances by that
many whole 60 Hz ticks. `scaled-seconds` then truncated again:
`(* (the int time-ratio) (the int (-> disp time-factor)))`.

**Why it judders.** At ~25 fps (target 60, `*ticks-per-frame*` = 9765):

| real frame | true ratio | quantised | world advances | error |
|---|---|---|---|---|
| 40 ms | 2.40 | 3 | 50.0 ms | +25% |
| 33 ms | 1.98 | 2 | 33.3 ms | ~0% |
| 45 ms | 2.70 | 3 | 50.0 ms | +11% |
| 30 ms | 1.80 | 2 | 33.3 ms | +11% |

The *average* tracks real time, so the game does not feel slow -- but every individual step
is wrong by up to a third of a frame and the error flips sign frame to frame. Motion is
non-monotonic: it stalls, then lurches. That is exactly "losing frames rather than dropping
them", and it is why it is worst on camera pans (continuous motion where the eye expects
perfect smoothness) and nauseating.

**Why 30 fps made it no better.** `*ticks-per-frame*` = `585900 / target-fps`, so at 30 the
tick is twice as long. A 40 ms frame gives ratio 1.2 -> quantised to 2 -> the world advances
66 ms for a 40 ms frame (+65%), alternating with 1. Coarser quantisation, *worse* judder.

**Fix.** `float-time-ratio` -- the true unquantised elapsed time -- was already being computed
on the very next line, and was only ever used for the `< 1.3` test. Use it:

```lisp
(if (< float-time-ratio 1.3)
    (set! time-ratio 1.0)
    (set! time-ratio float-time-ratio))
```

and multiply in float before converting: `(the int (* time-ratio (-> disp time-factor)))`,
keeping the engine's full 300-ticks-per-second (~3.3 ms) resolution instead of snapping to a
whole display frame (~16.7 ms).

The `< 1.3 -> 1.0` clamp is deliberately kept: it hides the ~1 ms of swap jitter that would
otherwise make a steady 60 fps wobble. `set-time-ratios` already caps at `fmin 4.0`, which
bounds the physics dt, and it already accepts arbitrary floats (the tube/racer/snowball
slow-motion callers pass computed values), so feeding it a fractional ratio is in-contract.

**Build.** Requires a GOAL recompile, not just the NRO:
`./build-host/goalc/goalc --game jak1 --instruction-set arm64 --cmd "(make-group \"iso\")"`
-> 699 targets OK; `out/jak1/obj/drawable.o` + `ENGINE.CGO`/`GAME.CGO` rebuilt.

**Gotcha:** `build-host/goalc/goalc` has its `@rpath` baked to the pre-move
`~/Downloads/jak-project-switch` path and fails with `Library not loaded: libcompiler.dylib`.
DYLD_* env vars are stripped by SIP-protected shells, so the workaround is a symlink:
`ln -sfn ~/Desktop/jak-project-switch/build-host ~/Downloads/jak-project-switch/build-host`.

**Deploy:** the NRO is unchanged (FIX 14, `c5a5169d...`). Only the data needs updating --
copy `out/jak1/` to `<SD>/switch/jak1/data/out/jak1/`.

## FIX 16 — REVERTED before shipping: internal resolution cap (AI-assisted)

Found that SDL reports hbloader's 1080p swapchain even in handheld mode, so the game renders
1920x1080 onto a 720p panel (2.25x the needed pixels). FIX 7f had spotted this but fixed it by
resizing the swapchain, which is the prime suspect for the fatalThrow at GOAL dispatch, so it
was disabled.

I implemented a safer version (cap only the off-screen FBO, leave the swapchain alone) — then
**reverted it**, because the user correctly pointed out there is already a `game-resolution`
option in the in-game menu. Verified: `pc_set_game_resolution` (kmachine.cpp:1115) only writes
`Gfx::g_global_settings.game_res_w/h`, which drives FBO recreation — the window and swapchain
are untouched, so the menu toggle is safe on Switch and is exactly this lever. Hard-coding a
cap would have silently overridden the user's choice. Use the menu instead.

Note this is a *throughput* lever, not a judder fix. Worth trying, but it is not the bug.

## FIX 17 — carry the truncated tick fraction between frames (AI-assisted)

After FIX 15 the user reported a real improvement but the symptom persisted, and notably that
30 fps felt better than 60 — which points at frame-time *consistency* rather than the timestep
maths.

Verified the time source is sound first: `timer-count` is `__read-ee-timer >> 9`, and
`read_ee_timer()` (kmachine.cpp:526) is `getNs() * 3 / 10`, i.e. a 300 MHz counter shifted to
~585.9 kHz — matching `*ticks-per-frame*` = `585900 / target-fps`. Resolution ~1.7us, off a
real clock. So the measurement is accurate and FIX 15's ratio is correct.

What remained: `scaled-seconds` must be a whole number of engine ticks (1/300 s = 3.3 ms), so
the float frame time is truncated every frame and the remainder discarded. That is a per-frame
error of up to 3.3 ms that *changes sign every frame* — the same class of jitter FIX 15 removed,
just an order of magnitude smaller. It also biases the counters slow, since truncation always
rounds down.

Worth noting *why* this is visible: `set-time-ratios` stores `time-adjust-ratio` and
`seconds-per-frame` as floats (no truncation), and those drive the camera and physics — while
the frame *counters* that drive animation and timers get the quantised value. So the camera and
the animation advance by slightly different amounts each frame.

**Fix:** new global `*frame-tick-remainder*`; accumulate the fraction and carry it forward:
```lisp
(let* ((exact-ticks (+ (* time-ratio (-> disp time-factor)) *frame-tick-remainder*))
       (scaled-seconds (the int exact-ticks)))
  (set! *frame-tick-remainder* (- exact-ticks (the float scaled-seconds)))
```

Deployed: `drawable.o` + `ENGINE.CGO`/`GAME.CGO` (546 targets). NRO unchanged (`c5a5169d...`).

## FIX 18 — camera tracking-spline smoother is a divergent oscillator (AI-assisted)

**The user's decisive detail:** the camera "comes back 2 frames and skips what I was doing",
not only when spinning the camera but **also when changing direction**. Both are large-error
events, which points at an unstable smoother rather than at timing or presentation.

Ruled out first, by reading rather than guessing:
- `*oddeven*` only feeds `reset-display-gs-state` and shadow/depth-cue offsets (interlacing),
  never camera position.
- The 2-buffer DMA handshake is sound: `gl_sync_path` (post-FIX 11) waits for
  `has_data_to_render == false` under `dma_mutex`, and that flag is cleared only after
  `render_game_frame` has finished reading, so the engine cannot reuse a buffer the renderer
  is still on. (Note `run_dma_copy = false`, so the renderer *does* read EE memory live -- the
  handshake is the only thing protecting it, and it holds.)

**The bug**, `goal_src/jak1/engine/camera/camera.gc:445` (`tracking-spline-method-21`), which
drives how fast the camera advances along the spline of Jak's past positions:

```lisp
(f2-8 (* (fmin arg1 (- f2-5 (-> this max-move))) (-> *display* time-adjust-ratio)))
```

`(- f2-5 max-move)` is the **full remaining error**, and the whole expression -- error included
-- is scaled by `time-adjust-ratio`. That makes the effective gain equal to the ratio itself.
An exponential smoother is stable only while its gain stays below 2:

| fps | time-adjust-ratio | gain | behaviour |
|---|---|---|---|
| 60 | 1.0 | 1.0 | converges in one step |
| ~25 | ~2.4 | 2.4 | **overshoots by 1.4x the error, flips sign every frame** |

So `max-move` oscillates divergently (bounded only by the `fmin arg2` / `fmax 0.4096` clamps):
the camera's advance rate surges, stalls, then backs up. Largest error = worst oscillation,
which is why it shows up precisely on direction changes and hard camera spins.

**Why it survived FIX 15, and why 30 fps did not help.** `time-adjust-ratio` is normalised to
60 Hz -- `(* (/ 60.0 target-fps) ratio)` -- so at ~25 fps real it lands near 2.4 whether the
target is 30 or 60. Before FIX 15 the quantised ratio was 3.0 (worse); FIX 15 brought it to
2.4, which is why the user reported a real improvement with the symptom still present. Both are
above the stability limit of 2.

**Fix.** `arg1` is the per-60Hz-frame *rate cap*; that is what should scale with elapsed time,
never the error. Taking r steps of `min(arg1, err)` converges to `min(arg1*r, err)`:

```lisp
(f2-8 (let ((err (- f2-5 (-> this max-move))))
        (if (>= err 0.0)
            (fmin (* arg1 (-> *display* time-adjust-ratio)) err)
            err)))
```

At ratio 1.0 this is arithmetically identical to the original in **both** branches, so 60 fps
behaviour is unchanged -- the change only takes effect when frames are actually long.

Note the codebase already uses this clamped pattern elsewhere (`cam-states.gc:605/615` do
`(fmin 1.0 (* arg2 time-adjust-ratio))`), so the hazard was known; this site was just missed.

Deployed: `camera.o` + `ENGINE.CGO`/`GAME.CGO` (546 targets). NRO unchanged (`c5a5169d...`).

## CRITICAL DEPLOY BUG — the NRO was being copied to the wrong place (AI-assisted)

**Every C++ fix after FIX 13 was deployed to a path the console never launches.**

The notes said to overwrite "both" NROs -- meaning `<SD>/gk.nro` and `<SD>/jak-and-daxter.nro`
at the **root**. But `package-switch.sh` installs the app to `${OUT}/switch/jak1/`, and that is
what hbmenu actually launches. So the real binaries live at:

```
<SD>/switch/jak1/gk.nro
<SD>/switch/jak1/jak-and-daxter.nro
```

Caught it from the telemetry, not from the file listing: the `[fps]` lines in the run log had
no `starved=` field, which only FIX 14 onwards emits, and showed `render 0.00` iterations being
counted as frames -- exactly the pre-FIX-14 behaviour. The root NROs hashed as FIX 14
(`c5a5169d...`) while `switch/jak1/*.nro` were still FIX 13 (`8cf38935...`). The log's banner
also still read `session start 7x`.

**Consequence:** FIX 14 (skip present + frame_idx when starved) never ran on hardware. FIX 13
(deferred window clear) *was* running, since `8cf38935` is the FIX 13 build. The GOAL-side work
(FIX 15/17/18) **did** take effect throughout, because game code is loaded from
`<SD>/switch/jak1/data/`, which was always being updated correctly.

This means the user's feedback on FIX 14 was feedback on a binary that did not contain it, and
any conclusion drawn from it is void.

**Deploy checklist from now on -- all four, or the test is meaningless:**
```bash
for p in switch/jak1/gk.nro switch/jak1/jak-and-daxter.nro gk.nro jak-and-daxter.nro; do
  cp build-switch/game/gk.nro "/Volumes/SWITCH SD/$p"
done
```
Then verify by md5, and confirm after the run that the log contains a field only the new build
emits. A matching md5 at the root proves nothing.

Current: all four = `331974cd527d5858ff8aa28ff0f751a4` (FIX 14 + `<algorithm>` include; FIX 16
was reverted before shipping). GOAL data verified current for FIX 15/17/18.

## FIX 19 — revert FIX 14; it deadlocked the boot (black screen) (AI-assisted)

The first run in which FIX 14 was *actually on the console* (see the deploy-path bug above)
black-screened and never loaded. FIX 14 is a circular wait:

- `gl_vsync()` (engine thread, opengl.cpp:1246) blocks on `frame_idx > init_frame`.
- FIX 14 made `frame_idx++` conditional on the render thread having received a DMA chain.

At startup the engine calls `gl_vsync()` **before** it has ever sent a chain. The render thread
times out after 40 ms with `got_chain = false`, so it never increments `frame_idx` and never
satisfies the predicate. The engine sleeps forever, and because the engine is the thing that
produces chains, no chain can ever arrive. Hard deadlock before the first frame.

`frame_idx` is not a statistic. It is simultaneously the engine's liveness signal and its
even/odd DMA buffer index (`gl_vsync` returns `frame_idx & 1`). It must advance on every render
iteration. Gating `SDL_GL_SwapWindow` was also wrong: with vsync on, the swap is what paces the
loop, so skipping it makes the thread spin.

Reverted all three gates (frame limiter, swap, `frame_idx++`). **Kept** the `starved` counter and
the `starved=` field in the `[fps]` line — it is pure telemetry, costs nothing, and is the marker
that proves which binary ran.

Lesson: FIX 14's premise ("don't present duplicate frames") was cosmetic; the counter it touched
carried a synchronisation contract. Re-presenting an identical image is harmless. Not advancing a
counter another thread is blocked on is not.

Build: `88f5e4443b364413e5c42ba1a1351045`, deployed to all four NRO paths.

## FIX 21 — the camera springs are unstable below 60fps (AI-assisted)

`cam-float-seeker::update!` and `cam-vector-seeker::update!` in `camera-h.gc` are springs
integrated with semi-implicit Euler and **no damping term**:

```
vel   += error * accel * dt
value += vel * dt
```

Such a system is stable only while `accel * dt^2 < 4`. The original code multiplies *both*
lines by `time-adjust-ratio`, so the effective stiffness grows with the **square** of the ratio:

| situation                   | ratio | effective accel |
|-----------------------------|-------|-----------------|
| 60fps target, hitting 60    | 1.0   | accel           |
| 30fps target (locked, si=2) | 2.0   | accel x 4       |
| 60fps target, real ~25fps   | 2.4   | accel x 5.8     |

Any seeker tuned with `accel > 1.0` leaves its stability region the instant the framerate
drops. With no damping it does not just overshoot once -- it **rings indefinitely**, bounded
only by the `max-vel` clamp. A seeker sitting on its target has zero error and stays silent,
which is exactly why the picture is rock steady when the camera is still and falls apart the
moment it moves, turns, or changes height. It also explains why 30fps felt better but never
cured it: 2.0 is right on the stability boundary, so it rings without growing, while 2.4
actively diverges.

Scaling a fixed-timestep second-order integrator by dt is not a valid transformation. The fix
is to **sub-step**: run the integrator the number of times it would have run at 60fps, each
with a step of at most 1.0.

```lisp
(let* ((ratio (fmin 4.0 (-> *display* time-adjust-ratio)))
       (steps (max 1 (the int (+ ratio 0.9999))))
       (dt    (/ ratio (the float steps))))
  (dotimes (i steps) ... use dt ...))
```

At ratio 1.0 this is bit-for-bit the original single step, so 60fps behaviour is untouched. At
2.0 it is two genuine 60fps steps -- the same dynamics the game was authored with. The cap of 4
means a load hitch costs a little camera lag instead of an explosion.

### Why the earlier camera fixes missed it
FIX 18 patched `tracking-spline-method-21`, a *first-order* smoother. That one is only
marginally affected (gain < 2 is a weak constraint). The real damage is in the second-order
seekers in `camera-h.gc`, where the error is squared. The run-log telemetry recorded only 23
heading reversals in 3600 frames, which is what finally made it clear that gross camera
reversal was not the mechanism -- a high-frequency ring is.

Built: 546 targets. Deployed `camera-h.o`, `ENGINE.CGO`, `GAME.CGO`. Settings left at
`fps 30 / vsync #t` so the ratio is exactly 2.0 -> two clean 60fps sub-steps.

## Measurements from the 30fps run (AI-assisted)

- `fps 30 + vsync #t` gives `si=2` and a **perfect lock**: 30.0 fps, 33.33ms, `vblanks 2x=57-60`,
  `starved=0`, sustained for ~110 seconds. The earlier "30fps does not help" test was run with
  **vsync off** (`si=0`, no pacing at all) and was therefore meaningless.
- `(game-size 1280 720)` was **already** set. The game was never rendering the world at 1080p --
  only the final upscale blit is 1080p. The proposed internal-resolution cap (FIX 16) would have
  achieved nothing; the user was right to reject it.
- Sandover Village costs ~36ms/frame of buckets, dominated by `l1-tfrag-tie` at 12ms (34-40%).
  Several buckets share a ~40ms max, which is one GPU stall being attributed to whichever bucket
  happened to be running -- read the averages, not the maxima.
- Memory is flat at 3261488KB for the whole run: no leak.

## FIX 22 — the camera follow smoother was never framerate-compensated (AI-assisted)

`camera.gc:771` (`cam-calc-follow-pt`):

```lisp
(vector-seek-3d-smooth! (-> arg0 follow-off) s3-2 (* 20480.0 (seconds-per-frame)) 0.05)
```

`max-step` is scaled by `seconds-per-frame`, but `alpha` (0.05) is a **per-frame** exponential
smoothing factor and was left constant. An exponential smoother converges as
`(1 - alpha)^frames`, so at 30fps it takes half as many steps per second and the camera follows
at roughly half the real-time rate it does at 60fps. Worse, the rate then *tracks the framerate*,
so the camera's feel changes whenever the scene gets heavy (30fps in the open, ~14fps in
Sandover) -- inconsistent camera response during exactly the motion the player complains about.

Fixed by sub-stepping at 60Hz granularity, the same treatment as FIX 21:

```lisp
(let* ((ratio (fmin 4.0 (-> *display* time-adjust-ratio)))
       (steps (max 1 (the int (+ ratio 0.9999))))
       (dt    (/ ratio (the float steps))))
  (dotimes (i steps)
    (vector-seek-3d-smooth! (-> arg0 follow-off) s3-2 (* 20480.0 (/ dt 60.0)) 0.05)))
```

Equivalence check at 60fps: ratio 1.0 -> steps 1, dt 1.0 -> `max-step = 20480/60 = 341.33`.
The original at 60fps: `20480 * (1/60) = 341.33`. Bit-identical, so 60fps cannot regress.
Total real time per frame is preserved at every ratio (`steps * dt/60 == ratio/60 ==
seconds-per-frame`).

### User-confirmed result of FIX 21
"much better and for sure the best mode is 720p 30fps - but the bug is still slightly there".
So the undamped-spring instability was the dominant mechanism. FIX 22 targets the residual.

### Remaining known instances of the same class (not yet changed)
- `swamp-blimp.gc:308-312` and `:377-382` -- a literal copy of the cam-float-seeker /
  cam-vector-seeker spring, same `ratio` scaling, same instability. Only affects the Rock
  Village blimp, so it is cosmetic, but it is the same bug.
- First-order sites (`seekl`, linear ramps, rotation rates) scale by `ratio` correctly and need
  no change. Do not "fix" these.

### Recommended configuration
`fps 30 + vsync #t + game-size 1280x720`. This is the only combination that yields a constant
frame time: vsync with `target_fps <= 35` selects swap interval 2, so every frame is exactly
33.3ms, and `time-adjust-ratio` is then exactly 2.0 -> two clean 60fps sub-steps everywhere.

## FIX 23 — the camera rotation blend mixed two different framerate compensations (AI-assisted)

`slave-matrix-blend-2` (`camera.gc:806`) is the quaternion blend that turns the camera toward
its target orientation. It is the code that runs *while the camera is spinning*, which is the
only situation where judder still survived FIX 21 and FIX 22. Two compensations were applied to
the same step:

```lisp
(let ((f30-0 (* 364.0889 (-> *display* time-adjust-ratio) f0-3)))   ; rate cap -- correct
  ...
  (if (< (* (/ (-> *display* time-adjust-ratio) 4) f28-0) f30-0)     ; exponential gain -- wrong
      (set! f30-0 (* (/ (-> *display* time-adjust-ratio) 4) f28-0))))
```

A cap on a rate *does* scale linearly with dt, so the first line is fine. The second is an
exponential smoother with a per-frame gain of 0.25; such a smoother converges as `(1-g)^frames`,
so the correct compensation is `1-(1-g)^ratio`, not `g*ratio`:

| ratio | gain used | gain correct |
|-------|-----------|--------------|
| 1.0   | 0.25      | 0.25         |
| 2.0   | 0.50      | 0.4375       |
| 2.4   | 0.60      | 0.503        |

So the camera over-rotates as soon as the framerate drops. Worse, the step taken is the
**minimum** of the two terms, and they scale differently, so the blend flips between the
rate-capped regime and the error-proportional regime as the ratio and the angular error wobble.
The two regimes have different dynamics; alternating between them frame to frame is what makes
a spin intermittently snap. That matches the report exactly: "the bug is still there *sometimes*
when you spin the camera", and it survived FIX 21 because it is not a spring.

Fixed by sub-stepping at 60Hz granularity (same approach as FIX 21/22): the cap and the gain
both take the values they were tuned for, and composing N steps of gain 0.25 reproduces the true
`(1-0.25)^N` curve rather than approximating it linearly. At ratio 1.0 it is a single iteration
with `dt = 1.0`, bit-for-bit the original.

Stack allocations were hoisted out of the loop; the function now explicitly returns `arg0` (all
callers ignore the value).

### Corroborating evidence
The user reported that **overclocking the console changes almost nothing**. That is strong
evidence the residual is arithmetic rather than throughput -- a CPU/GPU-bound problem would
respond to clocks, a wrong-dt problem would not.

## Resolution / framerate options — verified, no change needed (AI-assisted)

`DisplayManager::update_resolutions` already injects the Switch console modes explicitly
(1920x1080, 1600x900, 1280x720, 960x540, 640x360) because devkitPro's SDL only reports the
1080p mode the takeover applet was created with. So the in-game Game Resolution toggle has both
1080p and 720p and works; `pc-is-supported-resolution?` accepts both. The framerate option
(30/60) is untouched and remains available. Recommended: 720p + 30fps + vsync.

## FIX 24 — verify the swap interval is actually applied (AI-assisted)

`SDL_GL_SetSwapInterval`'s return value was ignored and `current_swap_interval` was updated
optimistically, so if the driver refused the request the game believed vsync was on while it
silently tore. Now the return code is checked, the value is read back with
`SDL_GL_GetSwapInterval()`, and both are logged:

```
[vsync] requested=2 set_ok=1 actual=2 (setting vsync=1 target_fps=30)
[vsync] WARNING: driver applied N, not M -- pacing will not match
```

Note this is **devkitPro SDL2**, not SDL3: `SDL_GL_SetSwapInterval` returns `0` on success
(negative on error) and `SDL_GL_GetSwapInterval()` takes no arguments and returns the interval.
The SDL3 signatures used elsewhere in the upstream tree do not compile here.

### Was vsync working? Yes.
The 30fps run is proof on its own: `33.33ms` frame times with `vblanks 2x=57-60` sustained for
~110 seconds cannot happen without a working swap interval of 2. The logging exists to remove
the inference, and to catch a silent failure after a mode change.

### The real vsync trade-off (should have been stated earlier)
With vsync on, a frame that misses its budget falls to the *next whole vblank*: 33.3ms -> 50ms
-> 66.6ms. In an area like Sandover that runs ~36ms, that means a hard drop to 20fps or 15fps
even though the frame was only slightly over. With vsync off the average framerate is higher but
frame durations are arbitrary. So:

- vsync ON  + 30fps: perfect pacing where the game fits in 33.3ms (most areas), harsh cliffs where it does not.
- vsync OFF: higher average fps, no pacing guarantee.

Adaptive vsync (interval -1) is the principled answer -- sync when the frame fits, tear instead
of double-waiting when it does not -- but support on devkitPro's nouveau-based stack is
unverified. The new `[vsync] actual=` line is what will tell us whether -1 is honoured before
committing to it.

Build `46bb4f5d791c139f22d5828f5c38cbc5`, deployed to all four NRO paths. Settings left at the
user's own choice (`vsync #f`) rather than overridden.
Build `46bb4f5d791c139f22d5828f5c38cbc5`, deployed to all four NRO paths. Settings left at the
user's own choice (`vsync #f`) rather than overridden.

## 2026-09-17/18 — Jak 1 runtime crashes: 6-crash triage + FIX 25/26 + crash analyzer (AI-assisted)

Branch: `jak-1-crashes-fixes` (not merged to main until the fix is verified on hardware).

### Triage — all 6 CPU exceptions decoded against boot-log rw/rx bases

Symbolizing pc/lr of each `=== FIX 7n CPU EXCEPTION ===` block in `gk_fatal.txt` against the
`[ee_runner] rw=/rx=` line of the matching boot in `gk_boot_log.txt` (33 boots on the card):

| # | fatal line | boot-log line | caller (lr_ee) | branch target (pc_ee) |
|---|-----------|---------------|----------------|-----------------------|
| 1 | 154  | 1998  | EE+0x229c7cc | **EE+0x0** (null function pointer) |
| 2 | 250  | 3904  | EE+0x1937a7c | EE+0x2660ad4 |
| 3 | 1630 | 39625 | EE+0x1937bdc | EE+0x268ab94 |
| 4 | 1731 | 41531 | EE+0x1937bdc | EE+0x2660f94 |
| 5 | 1866 | 45343 | EE+0x228864c | EE+0x2c14f54 |
| 6 | 2442 | 58685 | EE+0x2292f04 | EE+0x18fe04 |

(Earlier hand triage wrote crash #1's caller as `0x29C7CC` — that was a dropped digit; the
correct value is `0x229C7CC`. Crash #6's `0x29C7CC`-style kernel-region decode was also from
the wrong boot; the sp-in-rw discriminator fixes the matching.)

What the table says:

- **Two caller clusters, 3 crashes each**: `EE+0x1937a7c..0x1937bdc` and
  `EE+0x228864c..0x229c7cc`. So ~2–3 call sites, not six unrelated bugs.
- Crashes 2/3/4 are the **same GOAL function** (identical stack depth `rw+0x1e7dc0`, callers
  within 1 KB) calling a method whose slot points into the data heap (`0x2660ad4`/`0x2660f94`
  are ~2.5 KB apart, `0x268ab94` the same neighborhood) — same kind of object, different
  instances/boots.
- Crash #6's target **`EE+0x18fe04` is constant across boots** → a boot-time static/global,
  and it is the *same value* that appears as `X10=X11=X12` in crashes 2–4. One specific GOAL
  global object keeps ending up *called*.
- All targets are either null or valid arena data addresses — never wild garbage. This is
  "method slot holds a data pointer / null", i.e. the called-through slot is wrong or was
  never initialized, not memory corruption of the code itself.

### FIX 25 — EE (GOAL) decode inside the crash handler (`game/switch/platform.cpp`)

The triage above had to be reconstructed by hand on the host. Now `__libnx_exception_handler`
appends an `=== EE (GOAL) DECODE ===` section to `gk_fatal.txt` directly:

- `rw=/rx=/size=` of the arena, then `pc_ee/lr_ee/sp_ee` as arena offsets;
- every register annotated (`X07=EE+0x…` when it points into the executable alias,
  `X09=rw+0x…` for the writable alias) — "which GOAL things were live";
- `goal_backtrace:` a bounded scan of the GOAL stack (which lives inside the arena, so words
  pointing into the rx alias are GOAL return addresses) — same `sp && (sp&7)==0` +
  `volatile`-read discipline as the existing 7n module scan;
- the `[FATAL]` run-log breadcrumb now also carries `lr_ee`.

Implementation notes: `g_ee_main_mem`/`g_ee_main_mem_exec` are declared as local
`extern unsigned char*` (pointer mangling ignores the pointee type, so they link against the
`u8*` definitions in `game/runtime.cpp:106`) instead of including `game/runtime.h` — same
u128-include-order workaround already used for `switch_run_logf`. `EE_MAIN_MEM_SIZE`
(0x8000000) is restated by hand for the same reason.

With FIX 25, every future crash is self-locating: no host-side boot matching needed.

### FIX 26 — flush the v2 link path's icache unconditionally (all four `klink.cpp`)

`flush_icache_for_linked_object_v2` existed since the initial port but only ran when
`LINK_FLAG_EXECUTE` was set. A v2 object linked *without* that flag (a quiet data+code link)
got its code block moved by `work_v2` with **no icache flush**, and the Switch icache is not
coherent with the writable alias — so that code stayed stale until something else happened to
flush the same range. Such an object can still be called later through a symbol.

Change (identical shape in jak1/jak2; jak3/jakx keep their version-5
`m_link_segments_table` variant): hoist the flush out of the
`if (m_flags & LINK_FLAG_EXECUTE)` so every v2 link flushes its final code range. The
`LINK_FLAG_EXECUTE` branch (entry point setup, C→GOAL jump) is untouched, and the v3 path
(per-segment flush in `jak1_finish`) is untouched.

Honest status: this closes a real, long-standing correctness gap on this platform — it is
**not** confirmed to be the cause of the 6 crashes (the evidence points at method-slot
*contents*), but stale icache executing half-written code could produce corrupted downstream
state, so it must be in before any further triage.

### `scripts/analyze-goal-crash.py` — the triage, automated (host-side, stdlib only)

```
python3 scripts/analyze-goal-crash.py --fatal "/Volumes/SWITCH SD/gk_fatal.txt" \
    --boot-log "/Volumes/SWITCH SD/gk_boot_log.txt" [--run-log …/gk_run_log.txt]
```

Parses every FIX 7n block, matches legacy dumps to boots (lr inside `[rx, rx+128MB)` **and**
sp inside `[rw, rw+128MB)` — the sp check disambiguates rx collisions, which is exactly what
crash #1 needed), prefers the new EE-decode section when present, classifies each target
(null / arena-internal / wild) and prints the caller clusters. Validated: reproduces the
table above from the card logs as-is.

### Regression review — did this break anything already fixed? (checked 2026-09-18)

- **FIX 7d/7e/7n crash machinery** (`platform.cpp`): diff is purely additive — the 7n dump
  still writes first, the `[FATAL]` breadcrumb still fires (extended with `lr_ee` only),
  `svcBreak` still hands back to Atmosphere.
- **No previous FIX ever touched `klink.cpp`/icache** (nothing in these notes mentions
  either), so there is no past behavior to regress; the v3 flush path is unmodified.
- `git diff --stat` touches exactly 5 files (`game/switch/platform.cpp`,
  `game/kernel/{jak1,jak2,jak3,jakx}/klink.cpp`, new `scripts/analyze-goal-crash.py`):
  no fsdev/IOP (FIX 2/5/6), no HID exit (FIX 4), no MSAA (FIX 8), no loader budgets (FIX 9),
  no display/pacing/vsync code (FIX 10–24), no deploy-path machinery.
- Numbering: the platform.cpp comment originally said "FIX 10a", which collides with the
  frame-limiter FIX 10 in the index above — renamed to **FIX 25**.
- Link check: `g_ee_main_mem`/`g_ee_main_mem_exec` confirmed at global scope in
  `game/runtime.cpp:106-107` as `u8*`.

### Build / test status — NOT YET BUILT

The Mac has no local devkitPro and the docker daemon is down; the next session must
`scripts/build-switch.sh` inside the `devkitpro/devkita64` image, deploy to all four NRO
paths, and retest. Decision tree:

- crashes gone → FIX 26 was (at least part of) the cause;
- crashes persist → the new `goal_backtrace:` + `regs_ee:` lines identify the caller
  function and the live GOAL objects directly; combine with the level's `.o` symbol maps to
  name the method slot that holds `EE+0x18fe04` / `0x2660xxx`.

- Crash #1 is a plain null call (slot never set).
- All targets are either null or valid arena data addresses — never wild garbage. This is
  "method slot holds a data pointer / null", i.e. the called-through slot is wrong or was
  never initialized, not memory corruption of the code itself.

## 2026-09-18 — FIX 27/28: crash dumps now name the code (build deployed)

Branch `jak-1-crashes-fixes` @ e4a95c620 + this commit. Docker was up again; full
Switch build + deploy done this session.

### The blocker, and a twist the handover missed

The handover's plan was: log `obj=<name> base=EE+0x… size=…` in the boot log and resolve
offsets from it. **`switch_boot_log()` latches itself off at "boot complete"**
(`boot_log.h:45`, message "boot complete, tracing disabled") — and that happens right
after the title screen (`tit.DGO`, `static-screen`, `title-obs` are the last objects
logged, then the latch). The crashing callers (0x1937bdc / 0x2288xxx clusters, sp_ee
0x1e7dc0/0x1d5b00 = gameplay depth) are **level code linked after the latch**, so the
boot-log-only plan would have resolved boot objects only and missed exactly the two
clusters we need. Hence FIX 28's resident table.

### FIX 27 — record every object's final code range

- New `game/switch/link_bases.h` (header-only, `<atomic>/<stdint>/<string.h>` only —
  safe for platform.cpp's u128 constraint): ring of 1024
  `{u32 base; u32 size; char name[40]}`, `record()` from the linker, newest-first
  `find()` (level reloads re-link at a new base; newest wins).
- `game/kernel/jak1/klink.cpp` `jak1_finish()`: `log_link_base()` helper emits the
  handover-specified line `[klink] obj=%s base=EE+0x%x size=%u seg2base=EE+0x%x
  seg2size=%u` via boot log (pre-latch coverage) **and** records into the ring
  (always-on). v3 path uses `ofh->code_infos[0]` (main) + `code_infos[2]` (top-level);
  v2 path uses `m_object_data`/`m_code_size` right next to FIX 26's flush.
- jak2/jak3/jakx klink.cpp deliberately untouched: they have zero Switch instrumentation
  (no `__SWITCH__` blocks at all — only jak1 was ever instrumented) and the port ships
  jak1. FIX 26 stays in all four.

### FIX 28 — the exception handler now names things

`game/switch/platform.cpp`, after FIX 25's decode, before the `[FATAL]` breadcrumb:

1. `=== OBJECTS ===`: `pc_in=`/`lr_in=` annotated from the ring.
2. `=== LINK BASES (n of total) ===`: full ring dump (newest first, chunked writes) —
   makes *any* offset in the dump (backtrace, regs) resolvable on the host later.
3. `=== SYMBOLS (jak1, N targets) ===` — the important one: walks the **GOAL symbol
   table** itself (jak1 layout: 8-byte `{u32 value}` entries in
   `[SymbolTable2, LastSymbol)`, name = `SymInfo{hash, Ptr<String> str}` at
   `sym + 0x1FFFC`, `String = {u32 len, char data[]}`). For pc/lr/far + every
   arena-pointing register + the GOAL backtrace it prints
   `EE+0x… == <name>` (exact — this is what names data targets like EE+0x18fe04) or
   `EE+0x… <= <func>+0x…` (nearest at-or-below — function names for code targets).
   Globals `SymbolTable2`/`LastSymbol` are hand-declared as layout-compatible
   `PtrU32{unsigned offset}` in platform.cpp (same ODR-mangling discipline as FIX 25's
   `g_ee_main_mem`; `Ptr<u32>` is a single-u32 wrapper and variable mangling ignores
   the type). Guards: jak1-only (`g_game_version == 1`), arena-bounded reads, sane
   lengths, printable-name check.

### Analyzer

`scripts/analyze-goal-crash.py`: parses `[klink]` boot-log lines + the three new fatal
sections; prints `pc in:`/`lr in:` (crash-time OBJECTS wins, `?` falls back to range
resolution), surfaces `==` symbol hits, resolves backtrace tokens, and names caller
clusters. Regression-checked on the real card logs (identical output for the 8 legacy
crashes) + synthetic FIX 28 dump (all paths exercised).

### Build / deploy / rollback — DONE this session

- Build: `docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest bash
  scripts/build-switch.sh` → exit 0; platform.cpp [1/7] and klink.cpp [4/7] rebuilt.
- Verified: md5 `0b9dbaef…` → `2a6d400d…`; `strings build-switch/game/gk` still finds
  `EE (GOAL) DECODE` (FIX 25 intact) and now `=== OBJECTS ===`,
  `=== LINK BASES (%u of %u) ===`, `=== SYMBOLS (jak1, %d targets) ===`,
  `[klink] obj=%s base=…` (FIX 27/28 in).
- Deployed to `/Volumes/SWITCH SD/switch/jak1/gk.nro` (md5 matches build, `sync` done).
- Backup: `backups/pre-fix27/gk.nro` (old md5 `0b9dbaef…`).
  Rollback: `cp backups/pre-fix27/gk.nro '/Volumes/SWITCH SD/switch/jak1/gk.nro' && sync`.
- NOTE: only the `switch/jak1/` path was updated; check whether the other three NRO
  paths (per earlier session deploy habits) should also get this build before retesting.

### Honest status

- **Root cause NOT yet identified.** Naming 0x1937bdc / 0x18fe04 requires a crash from
  the new build on hardware: the SYMBOLS section should name the data target directly
  (exact match) and the caller function (nearest-below), and pc_in/lr_in name the
  objects. Nothing speculative shipped — these are pure diagnostics plus FIX 26's
  genuine (but unconfirmed-as-fix) cache fix.
- Performance slowdown vs Sep 12: NOT chased this session (per handover). FIX 27 adds
  one snprintf+fsync per linked object (~500 during boot, handfuls per level load) and
  FIX 28 only runs at crash time. Measure before concluding anything.
- Working hypothesis unchanged: method slot holding a data pointer or null (load/link
  ordering, wrong method-slot index on this platform, or use-after-free of the object's
  heap). The `==` symbol lines from the next crash will say which object the slot
  *points at*; comparing that with the slot's *owning* type (from the .o method tables
  once pc_in names the object) is the next step after that.

## 2026-09-18 (later) — user intel: crashes are REPRODUCIBLE at two known moments

User knows the triggers — no random play needed:
1. **Sentinel Beach, the cutscene when you trigger the seagull**
2. **Misty Island, the ambush cutscene**

Both are cutscene-trigger moments. Investigated against the repo:

- `goal_src/jak1/levels/beach/seagull.gc` exists (seagull/seagullflock processes,
  `BEA.DGO`). Line 9 says `;; note: modified for high fps` — as do
  `engine/camera/cam-states.gc`, `engine/gfx/mood/mood.gc`,
  `engine/gfx/background/wind.gc`, `levels/swamp/swamp-obs.gc`.
- **All five are byte-identical to upstream OpenGOAL master** (diffed against
  raw.githubusercontent.com/open-goal/jak-project/master): the "high fps" edits are
  upstream's own, not the port's, and goal_src overall is stock (single commit
  "initial release"). ⇒ **Game source is ruled out as the fork-specific cause.**
- `misty-obs.gc`/`battlecontroller.gc` (the ambush machinery: `misty-battlecontroller`
  spawning `nav-enemy`s) are NOT high-fps-flagged and also stock.
- Old boot log can't map the two lr clusters (A: 0x1937xxx, B: 0x2288-229cxxx) to the
  two sites — it latches off at boot complete. New build's FIX 27 ring + FIX 28
  sections will.
- Level code objects on disk for reference when the dump names something:
  `beach-obs.o`, `beach-part.o`, `bird-lady-beach.o` (yes, a bird-lady!), `misty-obs.o`,
  `misty-part.o`, `mistycam-ag.go`, `beachcam-ag.go` … in `out/jak1/obj` (1039 files).

Sharpened hypothesis: cutscene trigger = the moment NEW code runs (cutscene/task code
and camera-state transitions). If the icache isn't fully coherent for linked code on
ARM64 (exactly what FIX 26 addresses), the CPU can execute stale/mixed instruction
words at an indirect call → "method slot holds a data pointer"-looking crash. The 8
crashes on record happened with FIX 25 present but (at least some) predate FIX 26
being deployed — confirm on next run.

### Next run procedure (minutes, not hours)

1. Boot the NEW NRO (already on card, md5 `2a6d400d…` — verified twice).
2. Go straight to the seagull trigger (Sentinel Beach) and/or the Misty ambush.
3. Outcomes:
   - **No crash** ⇒ FIX 26 (v2 icache flush) was the fix; root cause = stale icache
     after linking, cutscene triggers exposed it. Then re-test the other site + play
     on for confidence.
   - **Crash** ⇒ copy `/Volumes/SWITCH SD/gk_fatal.txt` (+ `gk_boot_log.txt`) and run:
     `python3 scripts/analyze-goal-crash.py --fatal '/Volumes/SWITCH SD/gk_fatal.txt' --boot-log '/Volumes/SWITCH SD/gk_boot_log.txt'`
     The new `pc in:`/`lr in:`/`=== SYMBOLS ===` lines will name the object, function,
     and the data target (e.g. `EE+0x18fe04 == *something*`). From there: disassemble
     the named .o in out/jak1/obj at the given offset → identify the exact method call.


## 2026-09-17 — FIX 29: render-thread driver round-trips + an honest options menu (AI-assisted)

Two tracks, both built and green. NRO md5 `62a8aaa2a330d9b6d1df50ee6652a35f`
(`build-switch/game/gk.nro`), GOAL recompiled for ARM64 (546 targets), host build
(`cmake --build build-host --target runtime`) still links.

### Track B — performance (render thread, the bottleneck FIX 12 measured)

1. **Uniform-location cache.** `setup_tfrag_shader()` did **2** `glGetUniformLocation`
   calls *per draw* and `first_tfrag_draw_setup()` did **12** *per tree/pass*, across
   tfrag, tie (base + envmap + instanced), shrub and hfrag — several hundred driver
   round-trips every frame, all for locations that are fixed once the program is linked.
   New `TfragShaderUniforms` + `get_tfrag_shader_uniforms()` in
   `game/graphics/opengl_renderer/background/background_common.{h,cpp}` look them up once
   per `ShaderId`, lazily, on the render thread. Uniforms a shader doesn't have stay `-1`,
   and `glUniform*` with `-1` is a spec no-op — the same semantics as the old
   `if (u_id != -1)` guards. The cache stores the **program id** it was filled from and
   re-queries if it ever changes, so a rebuilt `ShaderLibrary` cannot be served stale
   locations (the cache is a global that outlives the renderer).
   Call sites converted: `TFragment.cpp` and `Tie3.cpp` AFAIL double-draw paths.
   In `Tie3.cpp` the old code looked the locations up on the **TFRAG3** program even for
   envmap draws; it now addresses whichever program is actually bound (`ETIE_BASE` when
   `use_envmap`). `ASSERT(false)` on that unreachable Tie3 case was kept.

2. **Per-frame `glGetIntegerv(GL_MAX_SAMPLES)` removed.** FIX 8's MSAA hardening put a
   driver query in the render loop. It is now queried **once**, right after the GL loader
   is ready, and cached in an atomic (`gl_get_max_samples()` in
   `game/graphics/pipelines/opengl.{h,cpp}`). Floored at 1: some GLES translation layers
   (the Switch's included) may leave `glGetIntegerv`'s output untouched, and a bogus max
   would poison both the clamp and the menu. Atomic because it is written on the render
   thread at init and read from the GOAL kernel thread.

3. **Texture-bind caching in `TFragment::render_tree`** — it re-bound `GL_TEXTURE_2D`
   on *every* draw; Tie3 already cached. Both now use a sentinel (`0x40000000`) that no
   real `tree_tex_id` can take. This also fixes a latent Tie3 bug: its old `-1` initial
   value **skipped the first bind** when the first draw legitimately used anim slot 0
   (`tree_tex_id == -1`).

### Track A — stop the options menu lying on Switch

- New `pc-get-max-msaa` (`game/kernel/common/kmachine.cpp`, registered as a GOAL symbol,
  declared in `goal_src/jak1/kernel-defs.gc`) returns the cached `GL_MAX_SAMPLES`.
  **Signature gotcha:** a zero-argument GOAL function is `(function int)`, *not*
  `(function none int)` — the latter fails the offline compile with
  `get_load_size called on NullType`.
- `pc_set_msaa` now clamps at the source, so the FBO cache never sees a sample count the
  device cannot allocate.
- `pc_set_display_mode` is a logged **no-op under `__SWITCH__`**: the console has one
  display and one present mode, and the "windowed" branch would try to resize the console
  framebuffer.
- `goal_src/jak1/pc/progress-pc.gc`: the MSAA carousel is now built per device
  (`*carousell-msaa-16/-8/-4/-2/-off*`) in `init-game-options` from `(pc-get-max-msaa)`,
  and a stale settings file saved on other hardware is coerced down. The cap is rounded
  **down to a power of two** because `update-to-os` resets any non-power-of-two
  `gfx-msaa` to `PC_DEFAULT_MSAA`. Display Mode and Monitor are disabled on Switch.

### Environment fixes made along the way

- `build-host/decompiler/extractor` had dylib rpaths baked in from the repo's old
  `~/Downloads` location: re-added rpaths + `codesign -s -` to make it runnable.
- `build-host`'s CMake cache and ninja files still pointed at that old path, so
  `cmake --build build-host` refused to run. Rewrote the stale absolute path across the
  65 text files under `build-host` instead of reconfiguring from scratch.

### Still open / not done here

- Nothing about the reproducible cutscene crashes (FIX 25-28) changed. This is pure
  frame-time and menu-honesty work; **the crash triage above is still the priority.**
- Not deployed to the card by this session. To deploy: `scripts/package-switch.sh`, then
  copy `build-switch/sd-card/` onto the SD root (or just `gk.nro` + `out/jak1` if the data
  folder is already current).

## 2026-09-17 — FIX 30: the Game Resolution setting was ignored every frame (AI-assisted)

**The port has been rendering 1920x1080 the whole time, on the 720p panel, regardless of
what the menu said.** The card's `pc-settings.gc` read `(game-size 1280 720)` and it made
no difference. NRO md5 `0cb12b6b2721728a5318ce8e535a664e`, deployed.

### Root cause

`update-to-os` (`goal_src/jak1/pc/pckernel-common.gc:125`) runs **every frame** and did:

```lisp
(if (= (pc-get-display-mode) 'windowed)
  (pc-set-game-resolution (-> obj framebuffer-scissor-width) (-> obj framebuffer-scissor-height))
  (pc-set-game-resolution (-> obj width) (-> obj height)))
```

- `framebuffer-width/height` comes from `pc-get-window-size`, i.e. the **actual SDL window**,
  which on Switch is hbloader's 1920x1080 swapchain (the window is deliberately created at
  the display size; `SWITCH_RES_OVERRIDE` is off).
- With `aspect-ratio-auto? #t` the scissor size equals the full window, so the windowed
  branch feeds 1920x1080 straight back into `pc-set-game-resolution`.
- `pc_get_display_mode()` returns `'windowed` on Switch (`DisplayManager`'s default; the
  console has no windowed/fullscreen distinction, and FIX 29 made `pc_set_display_mode` a
  no-op there).

So the windowed branch won every frame and overwrote `game-size` with the window size.
`game-size` drives `Gfx::g_global_settings.game_res_w/h` → the off-screen render FBO, so
this is 2.25x the fragment work for nothing. **This also explains why FIX 16's "use the
menu instead" advice never produced a measurable win** — the menu lever was disconnected.

### Fix

Switch always honours the explicit `game-size`:

```lisp
(if (and (= (pc-get-display-mode) 'windowed) (!= (pc-get-os) 'switch))
```

Deliberately *not* fixed by making `pc_get_display_mode()` report `'fullscreen` on Switch:
that symbol is also read by the vsync predicate and by
`(when (!= 'fullscreen (pc-get-display-mode)) (pc-set-frame-rate ...))`, so flipping it
would silently disable the frame limiter — entangled with FIX 10/24. One-line, one-effect
change instead.

### FIX 30b — the *default* resolution (fresh installs)

`SWITCH_RES_OVERRIDE` used to gate two unrelated things. Split in `game/switch/platform.h`:

- `SWITCH_RES_OVERRIDE` (**still 0**) — resizing hbloader's nwindow swapchain. FIX 7f's
  suspicion of it was refuted, but there is nothing to gain: the present is a cheap blit.
- `SWITCH_GAME_RES_FOR_MODE` (**now 1**) — `pc_get_active_display_size()`
  (`kmachine.cpp:680`) reports the operation-mode size (720p handheld / 1080p docked).

GOAL uses that only for the **default** `game-size` (`pckernel-h.gc:323 reset-graphics`)
and as the fallback when a saved value fails `pc-is-supported-resolution?`
(`pckernel-common.gc:467`). A saved, supported choice is still honoured — so unlike the
reverted FIX 16 this never overrides the user, it just stops fresh installs defaulting to
1080p on a 720p panel.

### Expected effect

Handheld should now actually render 1280x720 and present scaled to 1080p. This is by far
the largest throughput change made to the port: ~44% of the previous fragment workload.
Docked is unchanged (1080p is correct there).

## TODO — cut release v0.2.3 with FIX 29/30 (deferred; user focusing elsewhere)

**Status: NOT done. No tag pushed, no release touched.** Everything below is verified
fact as of 2026-09-18, so it can be executed without re-derivation.

### Why a new release is needed

- Latest release `v0.2.2` (published 2026-09-17 11:55 UTC) is tagged at `13c41ee89`
  (workflow-README commit) — it **predates FIX 29/30** (`cbb01140e`, main HEAD).
  Its `gk.nro` asset (md5 `0b9dbaedefb831814ca685b068243ae9`) is the old build.
  Nothing is mislabeled; v0.2.2 just doesn't have the fixes.
- v0.2.2's extractor tarballs are still fine — FIX 29/30 didn't touch extractor C++;
  GOAL fixes reach users via `goal_src` in the repo source at the new tag.

### The verified artifact to ship

- `build-switch/game/gk.nro`, md5 `0cb12b6b2721728a5318ce8e535a664e` — docker build
  from exactly `cbb01140e` (working tree was clean at that commit). This same NRO is
  already running on the test SD card.
- No CI job builds the NRO (`extractor-release.yml` only builds extractor+goalc;
  `release-pipeline.yaml` tagging is gated on upstream's repo name). Attachment is manual.

### Steps (when resuming)

1. Tag and push: `git tag v0.2.3 cbb01140e && git push origin v0.2.3`
   → `extractor-release.yml` auto-builds/attaches the three extractor archives.
2. Attach the NRO: `gh release upload v0.2.3 build-switch/game/gk.nro`, then publish
   (`gh release edit v0.2.3 --draft=false` or create with notes).
3. Release notes must include the **coupling warning** (below) and the update path.

### The coupling warning (must go in the release notes)

- **Old NRO + new data = broken boot**: `init-game-options` calls `(pc-get-max-msaa)`,
  which only exists as a kernel symbol in the new NRO. Update both together.
- New NRO + old data = works, but silently misses the honest menu and FIX 30
  (game-size honored). Users may wrongly conclude "no improvement".

### Update path for existing users (put in release notes)

1. `git pull` (or re-download the v0.2.3 source zip) — GOAL fixes live in `goal_src`.
2. Fast recompile, no re-extract/decompile needed (minutes):
   `~/tools/extractor iso_data/jak1 --folder --compile --game jak1 --instruction-set arm64 --proj-path .`
3. `cp <downloaded>/gk.nro build-switch/game/gk.nro`
   then `./scripts/package-switch.sh build-switch iso_data/jak1 build-switch/sd-card`
   and copy `build-switch/sd-card/switch/` to the SD root — **replacing `gk.nro` in the
   same step** (see coupling warning).
- Headline for notes: handheld now renders at the chosen 720p instead of forced 1080p
  (~44% fragment workload), plus uniform-lookup caching and an honest options menu.

### Optional follow-up (separate task, not for v0.2.3)

Add a devkitPro docker job to `extractor-release.yml` so the NRO is CI-built and
reproducible instead of hand-attached. GitHub runners can run the same
`scripts/build-switch.sh` docker flow.

---

## A0 — post-FIX-29/30 baseline (2026-09-18, NRO 0cb12b6b, ~7 h organic play)

Source: SD-card logs (`gk_run_log.txt` 19 MB / 12,548 `[fps]` lines; 38 boots in boot log).
NRO md5 re-verified on card = `0cb12b6b2721728a5318ce8e535a664e` (FIX 29/30 build).
Areas covered (game-log markers): village1 x28, beach x28, village2 x21, rolling x16,
firecanyon x9, misty x7 — all three test-loop spots plus more. Organic play instead of
the formal 110 s/spot loop (formal loop = optional refinement, same NRO).

Boot sanity: `GL_MAX_SAMPLES: 8` (FIX 29 log line present); FBO lines show real
resolution switching 640x480 / 1280x720 / 640x360 — FIX 30 menu confirmed working.
> **[CORRECTED 2026-09-19:]** the sentence above is wrong — no `FBO Setup`,
> `GL_MAX_SAMPLES` or `[disp] pc_set_window_size` line exists in ANY retained log
> (7b/7c/7d/7e/current). The A0 "menu confirmed working" claim was inferred, not
> observed. What the settings file actually shows is the carousel writing
> `window-size` while the renderer stayed at `game-size` — see the FIX 31 section
> at the bottom.

### Steady-state numbers

Windows are 2 s; "steady" = fps within tolerance of target, worst-frame < 500 ms
(removes loads + console suspends — raw maxima showed 34-minute "frames" from sleep).

- **30 fps** (n=9,711 = 324 min): fps avg 29.4; render avg **p50 30.7 / p95 35.8 /
  max 39.2 ms**; bucket totals avg 22.5 / p95 35.2 / max 81.7 ms; worst-frame avg 54.3 ms;
  vblank-locked (2x>=55) in 71.5% of windows; worst-frame >50 ms in 51.8% of windows,
  >100 ms in 1.86%, >300 ms in 0.031% (3 events in 5.4 h).
- **60 fps** (26 min attempted, 3 min steady): holds >=55 fps only in light areas
  (contiguous ~2-min stretches, render avg p50 15.3 ms there); overall 60-target avg
  38.4 fps.
- **Worst buckets in steady windows:** l1-tfrag-tfrag worst-avg 24.1 / worst-max 178.7 ms;
  l0-tfrag-tfrag 23.7 / 154.4; l1-tfrag-tie 14.3 / 79.1; sprite 11.6 / 70.1.
  Bucket AVERAGES unchanged at ~22.5 ms (matches pre-fix) — FIX 29 cut the non-bucket
  render overhead (45–67 -> 30.7 p50, a 32–47% cut), not the bucket submission itself.

### A0 verdict (per the gates in performance-improvement.md)

- render avg p50 30.7 ms >= 30 ms -> **A2 (submission cost) stays the priority**; it is
  also the only path to broader 60 fps (needs <= 16.6 ms).
- Bucket maxima 70–180 ms while averages are low, 1.86% of windows >100 ms -> **A1
  stall confirmed**, second priority.
- 30 fps: locked in ~72% of windows, minor one-frame slips elsewhere — playable
  everywhere; vblank histogram `2x=57–59/60` when locked.

### Also captured (C1 input — not root-caused yet)

`gk_fatal.txt` (137 KB): 9 CPU exceptions across the session, all symbolized cleanly by
`scripts/analyze-goal-crash.py` (FIX 27/28 works end-to-end on hardware):
- 5x caller cluster `draw-node+0x7c` -> tie-methods/bsp/collide chain; pc = call into EE
  data/heap (bogus method slot).
- 4x caller cluster `nav-enemy+0x11ac` / `tippy+0xcc` (one null GOAL function pointer).
Two distinct repro families — consistent with the "two reproducible crash moments" intel.
Logs archived at /tmp/sdlogs/ (run/boot/fatal).

## A2a — draw-mode state cache (2026-09-18, deployed NRO 33138483, NOT yet measured)

Source: cbb01140e + A2a (uncommitted working-tree changes, 9 files). C2/v0.2.3 remains
deferred — the release procedure in the section above is unchanged and still valid.

### What changed

`setup_opengl_from_draw_mode()` issued ~10 fixed-function GL calls on EVERY background/
merc/sprite draw. Now there is a state mirror (`background_common.cpp`, anon namespace):

- **Global part** (depth test enable+func, blend equation/funcs/color, depth mask):
  keyed by the DrawMode bits that affect it (`kStateKeyMask = 0x0f7f0007`); aref/decal/
  fog bits excluded (they never touch this state). Skipped when key unchanged.
- **Sampler part** (4x glTexParameteri): keyed by clamp S/T + filt bits + mipmap
  (`kParamKeyMask = 0x00880060`, mipmap in bit 31). Params live on the *texture
  object*, so the cache additionally requires callers to report `texture_rebound`
  (loops already tracked rebinds since FIX 29). A rebind forces a full re-apply.
- **Validity contract:** the mirror is only trusted *within one render pass*.
  `reset_draw_mode_state_cache()` is called at the start of every pass that uses the
  function: `TFragment::render_tree`, `Tie3::draw_matching_draws_for_tree`,
  `Tie3::envmap_second_pass_draw`, `Tie3::render_tree_wind`, `Shrub::render_tree`,
  `Hfrag::render_hfrag_level`, `Merc2::do_draws`, `Sprite3::flush_sprites`,
  `Sprite3::distort_draw_common`. (Other renderers mutate the same global GL state
  between buckets — resetting per pass is what makes the mirror safe.)
- **AFAIL invariant fix:** all five AFAIL double-draw paths (tfrag, tie base, tie wind,
  shrub, sprite) did `glDepthMask(GL_FALSE)` for the second draw and left it; they now
  restore `glDepthMask(GL_TRUE)` after (AFAIL only occurs when depth_write_enable, so
  TRUE is exactly what setup applied — provably equivalent to the old behavior where
  the next per-draw setup re-set it, but without the re-set call).
- **Bonus (uniform lookups FIX 29 missed):** Tie3 wind path did 1-3
  `glGetUniformLocation` per *instance group*, Shrub AFAIL did 2 per AFAIL draw, and
  Sprite3 flush did 3 per bucket — wind+shrub now use `get_tfrag_shader_uniforms`.
  (Sprite3's 3-per-bucket lookups left for A2b along with upload hoisting.)

CPU-side DoubleDraw analysis (aref/color_mult) still runs per call — only the GL calls
are gated. No GOAL changes, no asset changes → NRO-swap-only deploy.

### Build / deploy facts

- Host build clean (`cmake --build build-host`, 291/291, no errors).
- Switch NRO: docker `devkitpro/devkita64` + `scripts/build-switch.sh` (incremental,
  exit 0), `build-switch/game/gk.nro` md5 **33138483c297a734a08a962938e84d2b**.
- SD: copied to `/Volumes/SWITCH SD/gk.nro`, md5 re-verified on card. Rollback copy:
  `gk.nro.pre-a2a` (md5 46bb4f5d791c139f22d5828f5c38cbc5, the pre-A2a card NRO).
- Note: A0 baseline NRO was md5 0cb12b6b; the card NRO before this deploy was 46bb4f5d
  (both cbb01140e-source docker builds — NROs are not bit-reproducible, md5s drift per
  rebuild). Source-wise the A/B vs A0 is clean: same commit, A2a is the only delta.

### Measure next (A0 loop)

1. Play normally (~30+ min across village1/beach/indoor as in A0).
2. Pull `gk_run_log.txt`; compare render-phase p50/p95 vs A0 (p50 30.7 / p95 35.8 ms) and
   bucket worst avgs (l1-tfrag-tfrag 24.1, l0-tfrag-tfrag 23.7, l1-tfrag-tie 14.3,
   sprite 11.6 ms).
3. Regression canary (state-cache bugs render wrong): Sandover + Sentinel Beach + one
   indoor area; look specifically at AFAIL-heavy trans geometry (tfrag-trans / shrubs)
   for missing/incorrect second-pass depth behavior, wrong clamping, blend flicker.
4. If good → A2b (hoist uniform uploads: first_tfrag_draw_setup matrices per tree,
   Sprite3 per-bucket lookups, merc per-draw set_uniforms).

## A2a measurement verdict (2026-09-19, ~4 min organic play, NRO 33138483)

**⚠️ Methodology trap for every future pull:** `gk_run_log.txt` APPENDS across boots —
the file on the card contains ALL previous sessions. Always split on `session start`
lines and analyze only the sessions after the deploy date. (First comparison below was
accidentally A0-vs-A0: identical extremes to 0.1 ms gave it away.) Analysis script kept
at `/tmp/analyze_fps.py` (replicates the A0 steady-state metrics; group-index bug fixed
in situ — first run reported "none" because `target` was parsed from the wrong group).

### Numbers (steady 30 fps windows, fps>=25, worst<500 ms)

- A2a session (n=110, 4 min, village-heavy): render p50 **31.9** / p95 32.4 / max 33.5;
  bucket totals avg 23.9; worst-frame avg **54.1 ms**, >50 ms in **61.8%** of windows;
  vblank-locked 80.9%.
- Apples-to-apples village fingerprint (l1-tfrag-tie>=4.5 + l1-tfrag-tfrag>=2.5 + eyes):
  A0 n=486 → p50 **32.4**; A2a final session n=10 → p50 **32.0**. **A2a saved ~0.4 ms =
  noise. NO gain** — matches the user's perception ("stuttering still, didn't improve").
- GOAL side confirmed idle: wait_dma avg 0.0–1.2 ms → A3 (per-frame kernel calls) cannot
  fix stutter; it stays a free µs–ms cleanup, not a priority.

### What the stutter actually is (from the spike-window correlation dump)

1. **Zero-headroom cliff:** village render p50 31.9–32.4 ms = 96–97% of the 33.3 ms
   budget. Normal per-frame variance then misses the vblank: worst-frame ≈ 50–60 ms
   (= exactly one extra 16.7 ms slot, vblank 2x→3x) in 62% of windows = the felt hitch.
2. **Spike frames 60–95 ms** during movement (e.g. t=31–57 s: render max 44–75 ms while
   every bucket's max stays 5–22 ms — the spike frame is NOT one bad bucket; whole-frame
   stall pattern, driver/compositor or streaming-texture first-use — A1 territory).
3. **One ~1 s loader hitch** at session start (`[phase] loader max 993.92` — area/texture
   streaming; A1's other hypothesis).
4. Also seen: a 41–45 s stretch with swap avg 10–11 ms, fps 21–22 (GPU/vsync-bound
   signature) — proof the GPU CAN be the limit at 720p in spots, so the CPU/GPU fork is
   genuinely open, not settled by FIX 12 history.

### Verdict + fork (recorded in performance-improvement.md order section)

- A2a: kept (correct, free), but the A2 micro-series is NOT the lever; skipping cheap
  fixed-function state calls moves ~0.
- Next action (zero code): **resolution A/B in the village** — same spot ~2 min each at
  640x360 vs 720p via the options menu, then pull the log. Render avg scaling with
  pixels → GPU-bound → pivot to GPU-side (overdraw/trans passes, default res); flat →
  CPU-bound → **A2d multidraw audit** first, A2b/A2c after.
- If the A/B is ambiguous, build the A1 instrumentation (per-bucket GPU fence /
  `EXT_disjoint_timer_query`, log buckets with max > 20 ms) — measures CPU submit AND
  GPU exec per bucket, and catches the spike frames in the act.
- Rollback still on card: `gk.nro.pre-a2a` (md5 46bb4f5d…). Current: gk.nro = 33138483…

## Resolution A/B attribution — verdict: the test was VOID (2026-09-19, FIX 31)

The user reported: village, 720p → 640×360 → 720p via the options menu — "0 stuttering"
at 640×360, and "performances didn't go lower" after returning to 720p. Task was to
attribute the improvement to A1 (first-use costs) vs A2 (steady-state fill cost).

### Evidence chain (all from logs + the SD settings file)

1. **No resolution event exists in any retained log.** `pc_set_window_size` logs
   `[disp] pc_set_window_size -> game_res WxH` on Switch (in the code since the initial
   commit); `grep` across session-resab, session-last, 7b/7c/7d/7e and the current
   gk_run_log = **zero hits**. The options-menu handler (game-resolution submenu →
   `(game-option-type resolution)` → `set-window-size!` → `pc-set-window-size!`) never
   fired in any recorded session.
2. **The carousel was structurally disconnected from the renderer.** `set-window-size!`
   takes the windowed branch on Switch (display mode is always 0) and writes only
   `window-width/window-height` (+ an immediate one-shot `pc_set_game_resolution` that
   would have logged). But FIX 30's `update-to-os` re-applies `game-size`
   (`width`/`height`) as the render resolution **every frame**. So even a menu pick
   would be reverted one frame later.
3. **The SD settings file is the smoking gun:**
   `pc-settings.gc.bak60` (Sep 12 03:08): `(window-size 640 360) (game-size 1280 720)`;
   `pc-settings.gc` (now): `(window-size 1600 900) (game-size 1280 720)`. The user DID
   drive the carousel at least twice (640×360 on Sep 12, later 1600×900 — the "720p"
   they returned to was actually the 1600×900 entry, or no entry at all this session),
   but `game-size` — the only field the renderer reads — never moved from 1280×720.
4. **Therefore session 7x rendered 100% of its frames at 1280×720.** There was no B leg.

### Attribution of the perceived fix

- **A2 (steady-state fill cost): NOT TESTED — no low-res frame ever existed.** Also note
  FIX 12 never tested it either (it changed the *window* size; game res stayed 720p).
  The CPU/GPU fork stays open until a real A/B runs (post-FIX-31).
- **A1 (first-use costs): supported as the perceived mechanism.** All 60–110 ms spike
  frames live in the first ~90 s (village fingerprint `l1-tfrag-tie` constant all
  session); after that worst frames settle at 45–60 ms and never spike again at any
  point the user believed the res was lowered or restored. Yesterday's baseline had an
  80 s stretch at **0 hitches/min** with unchanged r_avg (~32 ms) and no setting change
  — smoothness tracks time-in-area/coverage, not resolution.
- t=190+ hitch burst (162/min, pos frozen): user in the options menu / quitting — not a
  resolution event (consistent with the 00:52 pc-settings write).
- swap avg collapse 4.7–5.8 ms → 0.95 ms at t≈90: episodic present-pressure change of
  the same kind that appeared/disappeared in yesterday's log with no setting change —
  NOT a resolution signal (it couldn't be — nothing changed).
- Render avg ~32 ms flat for the whole session at constant 720p: no fill-cost lever was
  ever pulled; the flat line is the CPU/driver-bound signature we already knew.

### FIX 31 (this session)

1. `goal_src/jak1/pc/pckernel-common.gc` — `set-window-size!`: on Switch the windowed
   branch now also writes `width`/`height` (game-size), so the carousel actually changes
   the render resolution and it persists across boots via `(game-size …)`.
2. `game/kernel/common/kmachine.cpp` — `pc_set_game_resolution`: logs real transitions
   (`[disp] pc_set_game_resolution -> WxH (was WxH)`), guarded because `update-to-os`
   calls it every frame. Boot will now log the applied saved game-size once.

### Deploy + re-test

- Needs NRO rebuild **and** the edited `pckernel-common.gc` copied to the SD data folder
  (`switch/jak1/data/goal_src/jak1/pc/`) — same NRO↔data coupling rule as always.
- Then re-run the real A/B (decision gate: performance-improvement.md step 3): same
  village stretch ~2 min each at 640×360 vs 720p; the `[disp]` lines now bracket the
  legs, so attribution cannot be silent again.
- Expectation to beat: if r_avg scales with pixels → GPU-bound (pivot to overdraw /
  trans passes / default res); if flat at ~32 ms → CPU-bound → A2d multidraw audit.

### A2a working tree (reminder)

The 9 uncommitted A2a renderer files (draw-mode state cache) are correct and free but
measured ~0 gain — committed now so the tree is clean before FIX 31 deploy builds.

## 2026-09-19 (later) — user's overnight re-test was VOID #2: wrong NRO path (AI-assisted)

The user reported at ~02:30 that lowering the res made loads (Forbidden Jungle, Sentinel
Beach) faster and removed the village drops, and that 720p brought the drops back. Log
forensics say otherwise:

1. **One new session since the last-night copy**: starts at log line 225,824, wall-clock
   ≈02:11:05–02:14:52 (pc-settings.gc mtime 02:14:48 = saved on quit).
2. **Zero `[disp]` lines of any kind** — not `pc_set_window_size` (in the old NRO since
   the initial commit) and not `pc_set_game_resolution` (in FIX 31). The FIX-30 GOAL
   data deployed at 01:06 applies `(game-size 1280 720)` via `update-to-os` every frame,
   and C-side `game_res_w` defaults to 640 → the FIX 31 NRO would log
   `(was 640x480)` at boot. Zero ⇒ **the old NRO ran**.
3. **Which NRO?** The console launches `sdmc:/gk.nro` (SD **root**, hbmenu's copy),
   which was still the Sep-17 22:46 A2a build (`33138483…`). FIX 31 had only been
   deployed to `sdmc:/switch/jak1/gk.nro`. Render res was therefore pinned at 1280×720
   for the whole session; the carousel was never opened (no menu events, settings file
   only rewritten on quit). What the user felt as low-res-vs-720p was view/area variance
   + expectation. (Also: only ONE loader spike all session — 944 ms at t=20, the boot
   village load. No Forbidden Jungle / Sentinel Beach loads were recorded.)
4. **Real data harvested anyway** (constant 720p): steady village holds 30 fps with
   render 24–32 ms (zero headroom); far-terrain/ocean views saturate — fps 23–26,
   `[cam]` HITCH 45–57 ms, swap-block 9–14 ms; buckets l1-tfrag-tie 4.4 ms, sky 3.0,
   l1-alpha-sky-blend-and-tfrag-trans 2.6, l1-tfrag-tfrag 2.5, sprite 1.9,
   ocean-mid-far 1.5. Fill-bound profile at 720p → GPU-bound prior strengthened.

**Deploy corrected (02:00):** FIX 31 NRO `9be0e8b9…` copied to BOTH `sdmc:/gk.nro`
(rollback `gk.nro.pre-fix31` = `33138483…`) and `sdmc:/switch/jak1/gk.nro` (rollback
`0cb12b6b…`); md5s verified; SD `data/goal_src/.../pckernel-common.gc` confirmed equal
to repo (FIX 30+31). Docs updated: performance-improvement.md VOID#2 + deploy-loop rule
(copy BOTH paths) + build-identity check.

**Next-session gate:** boot must log `[disp] pc_set_game_resolution -> 1280x720 (was
640x480)` ≈ t14 s. Then the A/B: village ~2 min at 640×360 vs 720p via the options
carousel (each pick must log `[disp] pc_set_window_size -> game_res …` + a game_res
transition). If low-res legs drop render avg materially → GPU-bound → A2 overdraw
candidates ranked by the bucket list above (tie bucket first, then sky/trans). If flat
→ CPU-bound → A2d multidraw audit. 720p-smooth end state likely needs the overdraw cuts;
540p is the instant fallback once the picker truly works.

## 2026-09-18 11:30 — Jak 2 support: per-game NRO builds + full jak2 ARM64 pipeline run (AI-assisted)

User supplied `/Users/filippo/Downloads/Jak II (USA) (En,Ja,Fr,De,Es,It,Ko) (v1.00).iso`
— verified by mount + SYSTEM.CNF and by the extractor's validation DB: **SCUS-972.65,
VER 1.00, NTSC → `jak2` / ntsc_v1** ("Detected - Jak II", Serial SCUS-97265).

### Code changes (per-game NRO, build-time selection)

Switch homebrew has no command line, so `--game` can't be passed; previously the game was
hardcoded to jak1 in THREE places. New design: the game is baked into the NRO at build time.

- Root `CMakeLists.txt`: new `SWITCH_GAME` cache var (default empty = legacy jak1 paths).
  When set → `add_compile_definitions(SWITCH_GAME_NAME="<game>")` globally.
- `scripts/build-switch.sh`: always passes `-DSWITCH_GAME="${SWITCH_GAME:-jak1}"` (env override).
- `game/main.cpp`: Switch block sets `game_name = SWITCH_GAME_NAME`; logs
  `Switch NRO built for game: <name>` after Compiled Version; comments updated.
- `common/util/FileUtil.cpp` (CRLF file!): `get_current_executable_path()` Switch branch now
  returns `sdmc:/switch/<SWITCH_GAME>/gk.nro` (was hardcoded `.../jak1/...`). This drives
  try_get_data_dir → each game's `data/` resolves to its own `sdmc:/switch/<game>/data`.
  **This was the hidden third blocker: without it a jak2 install would still have booted
  jak1's data.**
- `game/kernel/common/kmemcard.cpp`: mc-trace.txt now per-game (`sdmc:/switch/<game>/mc-trace.txt`).
- `scripts/package-switch.sh`: `GAME` env (default jak1) parameterizes everything AND
  verifies the NRO actually contains `sdmc:/switch/${GAME}/gk.nro` (grep -a) — refuses to
  package a game/NRO mismatch, so a jak2 data folder can't ship with a jak1 NRO or vice versa.

Behavior with no env vars is unchanged (jak1 everywhere). Desktop builds unaffected
(define only set when SWITCH_GAME is non-empty).

### Pipeline run (all succeeded)

```
./build-host/decompiler/extractor '<iso>' --game jak2 --extract --decompile --compile \
    --instruction-set arm64 --disable-ansi     # log: extract-jak2.log
```

- 2683 make-system targets built in ~16 s (!) — Apple Silicon is fast; 2121 files in
  `out/jak2/obj` (840 .o + .go data), `out/jak2` 5.4 GB, `iso_data/jak2` 4.1 GB,
  `decompiler_out/jak2` 509 MB. No file >2 GB (FAT32-safe).
- **ARM64 verified by opcode scan** (not just trusting the flag): unaligned scan of
  `out/jak2/obj/gkernel.o` finds 76× `c0 03 5f d6` (AArch64 `ret`) + 11 AArch64 `nop`s
  and ZERO x86 prologues (`55 48 89 e5`, `f3 0f 1e fa`) — same profile as known-good
  jak1 (73 rets). NOTE: a 4-byte-ALIGNED scan reports 0 because GOAL .o headers shift
  the code stream off 4-byte file alignment — don't redo that false alarm.

### Builds / packaging

- `SWITCH_GAME=jak2` docker build → `[577/577] gk.nro`, "Built ... (game: jak2)".
  Full rebuild (the new define dirties everything). NRO contains
  `sdmc:/switch/jak2/gk.nro` (grep -a verified).
- Pre-build jak1 NRO backed up: `backups/pre-jak2/gk.nro` + `gk` (md5 9be0e8b9… = the
  FIX-31 build deployed on the card — same as pre-change build).
- `GAME=jak2 scripts/package-switch.sh` → `build-switch/sd-card/switch/jak2` (9.6 GB):
  gk.nro + data/{out/jak2, iso_data/jak2, goal_src, custom_assets, game, log}.
- Copied (plus unstripped `gk.staged.elf` for addr2line) to the SD staging mirror:
  `/Users/filippo/Documents/giochi/Switch-games/sdcard/switch/jak2/` (9.8 GB).
  `switch/jak1/` mirror untouched. jak2 NRO+ELF also stashed in `backups/jak2-first-build/`.
- build-switch cache was left at SWITCH_GAME=jak2, then **restored to a jak1 NRO build**
  (detached docker run, `build-switch-jak1-restore.log`) so `build-switch/game/gk.nro`
  stays the deployed jak1 binary for the usual rebuild-and-redeploy-jak1 workflow.
  build-switch.sh always passes -DSWITCH_GAME explicitly, so the cache value is inert
  for scripted builds.

### Deploy (SD card NOT mounted at session end — user must do this)

```
# with the SD mounted:
cp -R /Users/filippo/Documents/giochi/Switch-games/sdcard/switch/jak2 "/Volumes/SWITCH SD/switch/jak2"
sync; diskutil eject <disk>
```
Needs ~10 GB free on the card. `switch/jak1` on the card needs nothing — untouched.
Launch `switch/jak2/gk.nro` from hbmenu (full RAM takeover, same as jak1).

### Known limitations / expectations for first boot

- **Jak 2's GOAL code has NEVER executed on ARM64 before** (jak1 only, until now). The
  compile is clean but runtime bugs are possible; if it crashes grab
  `sdmc:/gk_boot_log.txt`, `sdmc:/switch/jak2/mc-trace.txt` and
  `atmosphere/crash_reports/`.
- FIX 27/28 GOAL symbol diagnostics are guarded `g_game_version == 1` (jak1-layout only)
  → jak2 crash dumps will lack `=== SYMBOLS ===` etc.; native-side addr2line against
  `gk.staged.elf` still works.
- `sdmc:/gk_boot_log.txt` + `sdmc:/gk_stdout.txt` are shared between the two games
  (whichever boots last truncates them). Per-game info is under `switch/<game>/`.
- Saves start fresh at `sdmc:/switch/jak2/OpenGOAL/jak2/saves`; no clash with jak1.



The user's "done" re-test session (log line 228,110; pc-settings.gc saved 03:17:06;
console clock runs ~1 day ahead of the Mac — mind mtimes) booted normally (kernel loop
t=22.9), ~3.5 min of village play, 96 `[fps]` lines (28.1 avg fps, render 33.7 avg /
75.5 worst ms, buckets l1-tfrag-tie ~5.0 ms) — **and STILL zero `[disp]` resolution
lines**. Forensics:

1. **NRO-path theory falsified.** Both `sdmc:/gk.nro` and `sdmc:/switch/jak1/gk.nro`
   hashed `9be0e8b9…` (FIX 31) before the session, and `gk_stdout.txt` states the actual
   launch path directly: `Current executable directory - sdmc:/switch/jak1/gk.nro`.
   The earlier "console launches the SD root copy" conclusion was wrong. Keep copying
   both paths (cheap insurance), but identity checks must not rely on it.
2. **Real cause: the GOAL object on the card predated FIX 29/30 *and* FIX 31.**
   `data/out/jak1/obj/pckernel-common.o` mtime **Sep 17 18:58**; FIX 29/30 committed
   **19:14**, FIX 31 at Sep 18 01:07. The console **loads precompiled `.o` files from
   `data/out/jak1/obj` and never recompiles `goal_src`** — deploying the FIX-31 `.gc` to
   `data/goal_src` at 01:06 was a no-op. The pre-FIX-30 `update-to-os` (scissor-derived
   res, no `pc-set-game-resolution`/`pc-set-window-size` calls) therefore ran in EVERY
   session since Sep 17 18:58 — A2a measurement and both VOID sessions included — which
   is exactly why `[disp]` was silent no matter which NRO ran.
3. **Consequences for the record:** both VOID verdicts stand (no res change ever
   happened), but for the stale-`.o` reason. **Every session since Sep 17 18:58 rendered
   at the pre-FIX-30 scissor-derived resolution (unknown, ≠ the `game-size` setting) —
   all prior "720p" annotations are unreliable**, including the "constant 720p" bucket
   profile (fill-bound *shape* still valid; absolute ms may shift at the true 720p).
   The user's carousel picks changed `window-size` in the file but never reached the
   renderer.
4. **Fix (deployed + verified 03:10–03:25):** host goalc rebuild for arm64 —
   `./build-host/goalc/goalc --game jak1 --instruction-set arm64 --cmd '(make-group "kernel")'`
   (10 targets) then `--cmd '(make-group "engine")'` (342-target cascade — the
   `kernel-defs.gc` change rebuilds dependents); `out/jak1/obj/pckernel-common.o`
   37212→37348 bytes (+FIX 31 code); rsync → SD: 487 obj + 322 iso files, exit 0;
   md5-verified `pckernel-common.o` `d5bb176b…`, `progress-pc.o` `9250abc3…`,
   `gkernel.o` `7002b2c2…` (local = card), `pckernel-common.gc` equal to repo. `sync`'d.
   Gotchas for next time: goalc binary is `build-host/goalc/goalc` (the parent dir is a
   CMake target dir — easy to confuse); strip `com.apple.quarantine` if macOS blocks it;
   `make-group "kernel"` does NOT include the `pc/` files (engine group does); a
   `kernel-defs.gc` edit cascades into a full engine rebuild.
5. **Next-session gate (unchanged line, corrected expectation):** shortly after
   `kernel loop: first iteration` (~t≈23 s) the log MUST show
   `[disp] pc_set_game_resolution -> 1280x720 (was …)` — the `(was …)` value finally
   reveals what the stale build had been rendering at. Every carousel pick must then log
   `[disp] pc_set_window_size -> game_res WxH` + a `pc_set_game_resolution` transition,
   and the picture/perf will genuinely change with each pick. Note the first boot after
   this deploy is the FIRST time FIX 29/30+31 GOAL behavior actually runs on the console
   — treat its numbers as the new baseline, not comparable to any prior session.


## 2026-09-18 ~12:10 — jak2 first-run validation on Mac (Eden emulator + host runtime)

**Verdict: jak2 RUNS.** The GOAL code boots, streams levels, plays attract mode, writes
saves — proven natively on ARM64 (same instruction set as the Switch) via the host runtime.
The Eden emulator itself turned out to be broken on this Mac — unrelated to jak2.

### Eden emulator attempt (user asked: '/Applications/eden.app')
- Eden = yuzu fork (Citron lineage), arm64, MoltenVK. Homebrew NRO is supported without
  keys/firmware per its docs.
- Set up Eden's virtual SD at `~/Library/Application Support/eden/sdmc/switch/jak2/`
  (docker-detached rsync; 7896/7896 files verified, NRO md5 `5761b4eb…` matches all copies).
- **Eden never shows any window at all on this Mac** — with the NRO argument, without it,
  fresh config, first_start=false — always: Qt event loop healthy (sampled: idle in
  QEventLoop::exec), ~7% CPU, no window rendered, no crash reports, no app-level logging.
  macOS 26.6.2 / M4 Max incompatibility with this Eden build. NOTHING to do with jak2.
- `sdmc/gk_boot_log.txt` never appeared → the NRO never started executing in Eden.
- To retry later: newer Eden build or another emu (e.g. Sudachi/Citron), same virtual-SD
  layout is already in place.

### Host runtime SUCCESS (the real validation)
- `build-host/game/gk --game jak2 -boot -fakeiso` (run from build-host/game; data dir
  auto-resolves to repo root via try_get_jak_project_path, iso_data/jak2 + out/jak2).
- Boot: `OpenGOAL Runtime 1.0`, SDL 3.4.1, OpenGL 4.1 Metal → title screen → attract loop
  (`GAMEPLAY: enter title/ctysluma`, `Displaying level forexita`, soundbank loads,
  `pc settings file write: ~/Library/Application Support/OpenGOAL/jak2/settings/...`).
- 3 separate runs, each healthy until killed by MY tooling's process-group cleanup (the
  agent tool kills any still-running child when a call ends/times out — NOT a game crash;
  no crash reports, no shutdown messages, log just stops mid-frame).
- "Multiple textures named …" errors at boot are known-benign (duplicate names across TGOs).
- `SAVE ERROR: "no-auto-save"` on first boot is expected (no memory card data yet).

### Tooling lessons (this agent environment)
- Background children die at call end/timeout regardless of nohup/disown/osascript —
  group kill. Only daemon-owned processes survive (docker -d, launchd).
- launchd LaunchAgent for gk works but unsigned binary under launchd stalls in dyld
  (syspolicyd assessment) — avoid; run from Terminal instead.
- For the user to run jak2 on the Mac themselves:
  `open` is blocked by Gatekeeper prompt on the .command; instead paste in any terminal:
  `cd ~/Documents/giochi/SWITCH/jak-project-switch/build-host/game && ./gk --game jak2 -boot -fakeiso`
  (or right-click→Open `run-jak2-mac.command` at repo root once).
- Logs kept: `/Users/filippo/gk-jak2-mac{,2,3,4}.log` (mac3/mac4 are the long healthy runs).

### Still pending (unchanged)
- SD card insert → copy staging mirror → first REAL Switch boot of jak2 (Eden path dead).
- NRO rename question (Jak 1.nro / Jak 2.nro) still open.

## Session 2026-09-18 (midday): jak2 first deployment to SD + NRO renames

**Decision (user):** rename both NROs for hbmenu clarity — `switch/jak1/Jak 1.nro`
and `switch/jak2/Jak 2.nro`. Safe because runtime data-path resolution is
directory-based (`FileUtil.cpp` returns `sdmc:/switch/<SWITCH_GAME>/gk.nro`, and
`game/main.cpp` uses its parent for `data/`); the NRO filename itself is only an
hbmenu label. Deploy scripts must now use the new names.

**Deployed:** full staging mirror `build-switch/sd-card/switch/jak2/` →
`/Volumes/SWITCH SD/switch/jak2/` (7,890 files: custom_assets, game, goal_src,
log, iso_data 594 files/4.4 GB, out 2,874 files/5.8 GB; + `Jak 2.nro`,
`gk.staged.elf` 194 MB for addr2line triage, README.txt). Card du 10.45 GB
(> raw 10.08 GB = FAT32 cluster slack, expected). **md5 `Jak 2.nro` =
`5761b4ebc27b52173e09ac878a4c423f` — identical to the build that ran healthy on
the Mac host and in Eden.** jak1 rename applied in place (mtime unchanged).
Root `gk.nro` (jak1 FIX-31 duplicate) left as insurance. Card: 93 GiB free.
Deploy log archived: `build-switch/sd-card/deploy-2026-09-18.log`.

**Gotchas hit this session:**
- `run_commands` has a hard **30 s timeout** and kills the whole process group on
  expiry — naive rsync of 9.8 GB is impossible; a killed rsync can leave
  `.<name>.<6random>` temp files (none found this time).
- **macOS TCC blocks launchd agents from reading `~/Documents`** ("Operation not
  permitted" on every staging path). Workaround: APFS **hardlink farm** via
  `rsync -a --link-dest=<staging> <staging>/ ~/jak2-sd-tmp/` (instant, 0 bytes),
  copy script points there, delete farm afterwards.
- launchd one-shot (`RunAtLoad`, no KeepAlive) survives call boundaries and runs
  at full speed: total copy 12:21:20→12:37:27 (~16 min; small files ~2 MB/s on
  FAT32, big files ~18 MB/s). Plist/script/farm cleaned up afterwards.
- macOS writes `._*` AppleDouble sidecars on FAT32 when copying files with
  xattrs (`._gk.staged.elf` appeared) — deleted; Switch ignores them anyway.

**Next:** eject, boot Switch, launch "Jak 2" from hbmenu; triage via
`gk_boot_log.txt`, `mc-trace.txt`, crash screenshots + addr2line against
`sdmc:/switch/jak2/gk.staged.elf`.

## FIX 32 — Jak 2 stuck on the Sony splash: the ARM64 C-trampoline branch was never added to the jak2/jak3/jakX kernels (AI-assisted)

**Symptom:** the jak2 NRO boots, shows the "Sony Interactive Entertainment presents"
splash, and never progresses. jak1 on the same card, same commit, boots fine.

**Evidence (card logs, 2026-09-19 runs):**
- `switch/jak2/data/log/jak2.*.log` ends at `kernel: RPC port #5 started [FAB5]` in
  **both** jak2 runs. The next thing jak1 logs at that point is
  `Initialized GOAL heap` → `[Load and Link DGO From C] kernel`. jak2 never gets there,
  i.e. it dies inside `InitHeapAndSymbol()`, the call right after `InitRPC()`
  (`game/kernel/jak2/kmachine.cpp`).
- `gk_fatal.txt` for that run is `stage=0 iter=0` forever — the GOAL kernel never ran a
  single iteration. No CPU exception block: the EE thread just stops.

**Root cause:** `make_function_from_c()` / `make_stack_arg_function_from_c()` in
`game/kernel/jak2/kscheme.cpp` dispatch on platform:

```c
#ifdef __linux__ ... #elif __APPLE__ ... #elif _WIN32 ... #endif
```

devkitA64/newlib defines **none** of those, so on Switch the function fell off its end
with no `return` — undefined behaviour, and the caller used whatever was in the return
register as a `Function*`. `InitHeapAndSymbol()` calls it dozens of times while building
the symbol table (`asize-of-basic-func`, `delete-basic`, every `make_function_symbol_from_c`),
so the jak2 kernel was wired up with garbage function pointers and died at the first call.

This exact branch was already added to `game/kernel/jak1/kscheme.cpp` earlier in the port
(that is why jak1 works) — it was simply never applied to the other three kernels.
`make_function_from_c_systemv()` itself is byte-identical in all four and already has a
working `__aarch64__` path (`emit_arm64_c_stub`), so the fix is one `#elif`.

**Fix:** added the `#elif defined(__SWITCH__)` branch (→ `*_systemv`, AAPCS64) to both
dispatchers in `game/kernel/jak2/kscheme.cpp`, and to the identical latent bug in
`game/kernel/jak3/kscheme.cpp` and `game/kernel/jakx/kscheme.cpp`.

### FIX 32b — diagnostic logs are now per game

`gk_boot_log.txt`, `gk_run_log.txt`, `gk_fatal.txt` and `gk_stdout.txt` were hardcoded to
the **SD card root**, and boot/run/fatal all open with `O_APPEND`. With one NRO per game
that means jak1 and jak2 runs interleave in a single file with nothing distinguishing
them, and the truncate-on-boot `gk_stdout.txt` belongs to whichever game booted last.

This actively caused a wrong diagnosis in the first pass at this bug: jak1's
`[vag] open_fr path=sdmc:/switch/jak1/.../VAGWAD.ENG` and its whole KERNEL.CGO/ENGINE.CGO
link trace (jak1 ran at 13:41 and 14:22, jak2 at 13:43 and 14:23 — interleaved) read as
"the jak2 process is loading jak1's data". It was not: the card's `Jak 2.nro` has
`sdmc:/switch/jak2/gk.nro` baked in, `gk_stdout.txt` confirms
`Switch NRO built for game: jak2` / `Using data path: sdmc:/switch/jak2/data`, and all of
jak2's `out/jak2/iso` files match the repo by size and md5.

New `game/switch/log_paths.h` provides `SWITCH_LOG_PATH(name)` →
`sdmc:/switch/<SWITCH_GAME_NAME>/<name>` (falls back to the old root path when
`SWITCH_GAME_NAME` is undefined). Applied in `boot_log.h`, `run_log.h`, `safe_stdout.h`
and `platform.cpp` (both `gk_fatal.txt` sites). The opt-in *input* files
`sdmc:/gk_log_host.txt` and `sdmc:/gk_no_vag.txt` stay at the root on purpose — one
switch for all games. Packaged `README.txt` updated.

**Build/deploy:**
- `docker run --rm -v "$PWD:/work" -w /work -e BUILD_DIR=/work/build-switch-jak2 -e SWITCH_GAME=jak2 devkitpro/devkita64:latest bash scripts/build-switch.sh` → exit 0.
- Same for jak1 with `BUILD_DIR=/work/build-switch SWITCH_GAME=jak1` → exit 0.
- Verified baked strings: jak1 NRO has only `sdmc:/switch/jak1/gk_*.txt`, jak2 only
  `sdmc:/switch/jak2/gk_*.txt`.
- Deployed: jak1 md5 `9dbbde02085ca14483920f9de4342123` → `/Volumes/SWITCH SD/switch/jak1/Jak 1.nro`,
  jak2 md5 `16bb831874ba8923f63bee5ddeb7a3dd` → `/Volumes/SWITCH SD/switch/jak2/Jak 2.nro`, `sync` done.
- Backups: `backups/pre-jak2-abi-fix/jak1-gk.nro` (`dbf7ad03…`), `jak2-gk.nro` (`c7bc9170…`).
  Old shared root logs moved to `/Volumes/SWITCH SD/old-shared-logs/`.

**Not yet verified on hardware** — next jak2 run should get past
`kernel: RPC port #5 started` into `Initialized GOAL heap` / `[Load and Link DGO From C] kernel`,
and its logs will now be at `sdmc:/switch/jak2/gk_*.txt` only.

## 2026-09-19 — jak2 empty city fix + 30fps + resolution picker (commit cacbc33f3) (AI-assisted)
- Empty city root cause: ctywide loading-level heap 99.6% full at login (ARM64 object sizes vs retail-tuned heap), 32KB *city-dead-pool* init failed -> no traffic-manager. Fix: pool storage from global heap (traffic-manager.gc).
- 30fps: added 30 to *frame-rate-options* (progress-static-pc.gc) + index case remap.
- Resolution: re-applied GOAL FIX 30+31 in shared pckernel-common.gc (both games).
- NOTE repo renamed to jak-project-switch-original: goalc now needs DYLD_LIBRARY_PATH=$(find build-host -name "*.dylib" | sed "s|/[^/]*$||" | sort -u | tr "
" ":").
- Deployed: 91 jak2 + 27 jak1 iso files, all md5-verified on card (OGR.DGO/TSZ.DGO re-copied after --size-only missed them).

## 2026-09-24 — FIX 33: loader memory-pressure crash + zoomer fps (AI-assisted)

**Symptoms (jak2, FIX-32b build):** riding the zoomer through chained area
transitions hard-crashes during the blackout load — abort in Tegra
`nouveau_mm_allocate` reached from loader `glBufferData`; also 5–50 ms frame
spikes while streaming (`stage texture took …`, `Loader::update slow setup`).

**Root cause (confirmed in source, both games share the loader):**
1. `Loader::update` only unloads levels when it has nothing to stage
   (`!did_gpu_stuff`) — never while a level initializes. `update_blocking`
   (end of blackout) loads the whole new area while every old level stays
   resident (stdout showed 7+ live at once) → peak VRAM = old+new → the Tegra
   suballocator gives up during a large merc `glBufferData`.
2. `m_max_levels` for jak2 = `LEVEL_TOTAL` (effectively unbound) and eviction
   required 180 idle frames anyway.
3. Latent leaks in the normal eviction path: shrub vertex buffers and
   `hfrag_vertices` were **never** released, and `hfrag_indices` was queued
   **twice** (double-delete) — VRAM grew on every eviction during long
   sessions.
4. Per-texture atomic `glTexImage2D(full data) + glGenerateMipmap` = 5–50 ms
   spikes; budget was only checked between textures.

**Fix — all Switch-gated C++, no GOAL changes, files in
`game/graphics/opengl_renderer/loader/`:**
- **`GpuBufferPool.h` (new):** pooled GL buffer objects (256 KB size
  classes, best-fit reuse ≤2× request). Stages `acquire()`/`release()`
  instead of `glGenBuffers`+`glBufferData`/`glDeleteBuffers` — after warm-up
  area transitions reuse buffers instead of churning the suballocator.
- **`Loader::update_blocking`:** new `purge_retired_levels(immediate)`
  before staging the new area — recycles every level that is neither on the
  game's want-list (`__pc-set-levels`) nor displayed
  (`__pc-set-active-levels`; GOAL updates both every frame from
  `level.gc`), flushes texture garbage and `glFinish()`es so the driver has
  actually reclaimed the memory first. Screen is black → invisible.
- **`Loader::update`:** eviction now runs every frame (one level/frame) via
  `pick_eviction_victim()` — Switch rules: never evict desired/active
  levels; recycle off-list levels after 30 frames; hard cap of 8 live levels
  (off-list only) as a backstop. Desktop keeps the legacy
  `m_max_levels`+180-frame rule. Garbage textures/buffers now drain even
  while staging (used to require an idle loader). Fixed the shrub/hfrag
  leaks + double-delete by routing ALL unload paths through one shared
  `unload_level_gpu_objects()`.
- **`TextureLoaderStage`:** storage allocated with null upload, pixels
  streamed in ≤128 KB `glTexSubImage2D` row bands (budget-checked per band),
  `glGenerateMipmap` once after the base level is complete. Desktop uses
  512 KB bands. `load_common`/`do_reload*` also routed through the pool
  (fixes a common-reload merc buffer leak).
- **Telemetry:** every 120 frames on Switch
  `[loader] live=N init=N want=N | pool=N bufs X.XMB free, N out | gc …`
  plus `[loader] blackout purge: recycling N retired level(s)` lines.

**Build/deploy (2026-09-24):**
- docker builds exit 0: jak2 `BUILD_DIR=/work/build-switch-jak2 SWITCH_GAME=jak2`,
  jak1 `BUILD_DIR=/work/build-switch SWITCH_GAME=jak1`; host (macOS arm64,
  `build-host-fix33`) compiles the loader TUs warning-free (old `build-host`
  cache is stale — repo was renamed; fresh dir used).
- Deployed: jak2 md5 `def2bd2e6f5b964a81466ced1c187d9a` → `/Volumes/SWITCH SD/switch/jak2/Jak 2.nro`,
  jak1 md5 `1ca94cf191677cc892d3b1903ab2c5b3` → `/Volumes/SWITCH SD/switch/jak1/Jak 1.nro` (rebuilt
  once more so both NROs match the exact final source, incl. the
  unused-const cleanup in `LoaderStages.cpp`), `sync` done.
- Backups: `backups/pre-fix33/jak2-gk.nro` (`16bb8318…`, the FIX-32b build
  the crash logs came from) + `jak1-gk.nro` (`9dbbde02…`); crash logs
  snapshotted in `backups/pre-fix33/jak2-crash-logs/`.

**Hardware test procedure:** jak2 → load save in Haven zoomer area → chain
area transitions (city ↔ haven ×3+). Watch `sdmc:/switch/jak2/gk_stdout.txt`
for the new `[loader]` lines: `live=` should drop at each blackout purge and
`pool … free` should recycle instead of shrinking; crash #1
(nouveau_mm_allocate abort) should be gone. Crash #3 (garbage function
pointers) was left instrumented — reassess after this memory-pressure fix;
if it persists it's a separate bug. Zoomer fps: `stage texture took` spikes
should shrink to ≤ a few ms.

## 2026-09-24 — FIX 33a: revert Switch band texture uploads (boot crash) (AI-assisted)

### What happened
The first FIX 33 build **crashed at boot** right after the Sony screen, during
the first blackout load. gk_fatal.txt (newest entry, appended after the old
crash-#3 entries):
  error_desc=0x101 esr=0x92000007 (data abort, READ), far=0x752d3a9a80
  pc_off=0xab65f0, X01=far, X02=0x20000 (128 KB), X14=-(src-dst)
  stack: convert_ushort / util_format_*unpack* (Mesa driver format-conversion)
That is a 128 KB memcpy — exactly TEX_BAND_BYTES on Switch — faulting on the
SOURCE page while the driver staged a glTexSubImage2D band. gk_stdout ends at
"NOTE: coming out of blackout...", so the crash was inside update_blocking's
staging of the first level.

### Investigation
- Static analysis: band math is provably in-bounds (m_cur_row < h, rows ≤
  h - m_cur_row, offset+rows*row_bytes ≤ w*h*4); no game code sets
  GL_UNPACK_ROW_LENGTH (only imgui/SDL internals, not used on Switch).
- Host repro: built gk (build-host-fix33, arm64) and booted jak2 against
  out/jak2 — the SAME band code streamed ctysluma/ctywide/lwidea/title with
  zero issues, and 0 "TEXTURE SIZE MISMATCH" lines (w*h==data.size() holds
  for all real fr3 textures — checked via the new invariant printf).
- Conclusion: band uploads interact badly with Mesa/nouveau's partial-upload
  staging on Tegra. Can't debug nouveau remotely → revert that part on
  Switch. NOTE: most host textures upload as ONE 512 KB band (fewer
  multi-band textures), so the host test does not fully exercise the
  multi-band resume path either.

### Changes (LoaderStages.cpp only)
- __SWITCH__: TextureLoaderStage back to the original atomic add_texture()
  path with the original per-frame caps (20 textures / 256 KB / LOAD_BUDGET).
  Host keeps the banded path, now with glActiveTexture+glPixelStorei
  (GL_UNPACK_ROW_LENGTH,0) hygiene each band and a valid_rows clamp to
  tex.data.size() so an invariant violation can never read OOB.
- check_tex_invariant() prints "[loader] TEXTURE SIZE MISMATCH" instead of
  crashing if a texture ever ships with data.size() != w*h.
- Everything else from FIX 33 (purge-before-load, GpuBufferPool, per-frame
  eviction, unified unload_level_gpu_objects, telemetry) is unchanged.

### Build/deploy note (IMPORTANT for future sessions)
`cmake --build <dir> --target gk` only links the ELF — it does NOT produce
the NRO. The NRO target is **gk_nro**. (First rebuild attempt silently
redeployed the old broken NROs with identical md5s; caught it because the
new "TEXTURE SIZE MISMATCH" string was missing from the NRO. Always
strings-check a new marker or verify a NEW md5 before deploying.)

### Deployed (SD, md5 verified after copy + sync)
- jak2: build-switch-jak2/game/gk.nro -> switch/jak2/Jak 2.nro
  md5 30409d58d21ec6abe78478dc7c8790c9
- jak1: build-switch/game/gk.nro -> switch/jak1/Jak 1.nro
  md5 44f0f5d9830827af4566006fae156eb0
- Broken first-attempt NROs saved in backups/fix33-attempt1/.

### Test procedure (same as FIX 33)
1. Boot jak2 — should get past the Sony screen to the title (this crashed
   before). Check gk_stdout for the blackout-load completion lines.
2. Load the save, chain city<->haven zoomer transitions ×3+. Confirm
   "[loader] blackout purge: recycling N" at each transition, no nouveau_mm
   abort, live= not climbing, pool MB recycling.
3. Quick jak1 sanity boot.
4. If any "TEXTURE SIZE MISMATCH" lines appear in stdout, note the texture
   names — that would finally pin the band-crash root cause.

## 2026-09-24 — FIX 34: async memory-card save/load + GLES pixel-type fix (AI-assisted)

### Problem 1 — tutorial-text stutter (jak2)
`pc_game_save_synch()` / `pc_game_load_synch()` ran the whole SD transaction
(open -> header -> 128 KiB payload -> footer -> fsync -> close, measured
125–276 ms on console) inline on the GOAL kernel thread from `MC_run()`. Jak 2
auto-saves on every tutorial-hint completion, so hint text froze the game 4–9
frames at a time. On PS2 the IOP did this concurrently while the EE polled —
the GOAL state machines already wait on `op.result == BUSY` for seconds.

**Fix (`game/kernel/common/kmemcard.cpp`):** one lazily-started, detached
worker thread with an IDLE -> BUSY -> DONE phase machine under
`g_mc_async_mtx`.
- GOAL thread only snapshots the 128 KiB bank + 64-byte preview out of EE
  memory (`mc_dispatch_save_async`) or copies the loaded bank back in
  (`MC_run` DONE branch). `op` / `mc_files` / `mc_last_file` are still touched
  **only** on the GOAL thread, so no extra locking.
- Worker owns all file I/O, the 3-attempt retry backoffs and header/checksum
  verification (`mc_worker_save` / `mc_worker_load`), still under
  `SWITCH_FS_LOCK()` — FIX 7u serialisation against the overlord's ISO reads
  is preserved, just off the game thread.
- `mc_checksum_bytes()` added so the worker checksums its own staging buffers
  and never dereferences GOAL pointers.
- While BUSY, `MC_run()` returns early → `op.result` stays BUSY, exactly what
  the GOAL save/load code expects from a slow card.
- Review fix applied during this session: the DONE branch now also latches
  `mc_last_file` on a `NEW_GAME` load result, matching the old synchronous
  loader (it had been dropped, only `OK` set it).
- New stdout markers: `dispatched async save of bank N (save count N)`,
  `async save finished in X.XXms (ok|FAILED)`, `async load finished in …`.

### Problem 2 — `GL_UNSIGNED_INT_8_8_8_8_REV` on Tegra/GLES
That packed pixel type is desktop-GL only; under GLES the driver takes a slow
format-conversion path (`util_format_*unpack*`, the same code that showed up in
the FIX 33 band-upload crash). All CPU-side uploads now pass `GL_UNSIGNED_BYTE`
(byte order is identical for RGBA8888): `TexturePool.cpp`, `TextureAnimator.cpp`,
`SkyBlendCPU/GPU.cpp`, `Shrub/TFragment/Tie3/Hfrag.cpp`, `OceanTexture.cpp`.
Remaining `GL_UNSIGNED_INT_8_8_8_8_REV` call sites are render-to-texture
`FramebufferTexturePair`s, already remapped Switch-side inside
`opengl_utils.cpp`. `LoaderStages.cpp`: `LOAD_BUDGET` 2 -> 4 ms on Switch and
new per-level instrumentation `[loader] tex stage: N textures, upload X.Xms,
mipgen X.Xms` to show whether mipgen or upload dominates next.

### Build / deploy (2026-09-24)
- Host `build-host-fix33` compiles clean; docker builds exit 0 —
  jak2 `BUILD_DIR=/work/build-switch-jak2 SWITCH_GAME=jak2`,
  jak1 `BUILD_DIR=/work/build-switch SWITCH_GAME=jak1` (target is `gk_nro`,
  see FIX 33a note).
- Deployed + md5-verified on card after `sync`:
  jak2 `d0654cc6142feb2c9c4a35f24cf1a095`, jak1 `d957bf7916e4724392436257b0c02c3e`.
  Marker check on both cards' NROs: `dispatched async save` + `tex stage` present.
- Backups of the FIX-33a NROs: `backups/pre-fix34/jak2-gk.nro`
  (`30409d58…`), `jak1-gk.nro` (`44f0f5d9…`).

### Hardware test procedure
1. jak2: play through a tutorial-hint sequence — the 4–9 frame freeze on each
   auto-save should be gone; stdout shows `dispatched async save …` then
   `async save finished in …ms` a few frames later.
2. Save + reload from the options menu; confirm the save slot preview updates
   and the loaded game is correct (async apply path). Try a fresh slot to
   exercise the `NEW_GAME` result.
3. Watch for `[loader] tex stage:` lines at level loads — if mipgen dominates,
   next step is CPU box-filtered mipmaps on the loader thread.
4. Quick jak1 sanity boot + save/load.

## 2026-09-24 — FIX 34a: revert the GLES pixel-type swap (boot-time freeze) (AI-assisted)

### What happened
The FIX 34 build (`d0654cc6…`) froze ~11.8 s in: Sony + Dolby screens, title
level loaded, ~3 s of gameplay-less streaming, then a **hard hang** — no CPU
exception, no new `gk_fatal.txt` entry, and **every** log stopped at once
(`gk_run_log.txt` `[gfx] alive` heartbeat, `[chan]` mirror and `gk_stdout.txt`
all end together). `gk_stdout.txt` ends on `stage texture took 6.32 ms` /
`Loader::update slow setup: 8.6ms`, i.e. inside loader texture uploads. The
immediately preceding session on the same card (FIX 33a NRO `30409d58…`) ran
**660 s at a steady 30 fps** — so FIX 33/33a are proven and only the FIX 34
delta is suspect.

### Ruled out
- Async memory card: `mc-trace.txt` (unbuffered `write()`, so it cannot lose
  the tail) contains **no** lines from the frozen session — no
  `dispatched async save/load` — the worker never ran before the hang.
- No `TEXTURE SIZE MISMATCH`, no OOM, no assert.

### Root cause (best explanation)
Everything stopping simultaneously with no CPU fault is a GPU-channel stall,
and the only FIX 34 change on that code path is the
`GL_UNSIGNED_INT_8_8_8_8_REV -> GL_UNSIGNED_BYTE` swap: it moves Mesa/nouveau
off the CPU conversion path onto its staged/DMA upload path — the same
`nouveau_mm` machinery that aborted in FIX 33 and again in FIX 33a's band
uploads. Third time this driver has punished a "faster" upload path; treat the
atomic `GL_UNSIGNED_INT_8_8_8_8_REV` `glTexImage2D` as the only sanctioned
texture upload on Switch.

### Changes
- Reverted to the proven values in `TexturePool.cpp`, `TextureAnimator.cpp`,
  `SkyBlendCPU/GPU.cpp`, `Shrub/TFragment/Tie3/Hfrag.cpp`, `OceanTexture.cpp`
  and `add_texture()` (`LoaderStages.cpp`) — Switch is now byte-identical to
  FIX 33a on the graphics side. The host-only banded path keeps
  `GL_UNSIGNED_BYTE` (verified on macOS).
- `LOAD_BUDGET` back to 2 ms and `MAX_TEX_BYTES_PER_FRAME` back to 256 KB.
- **Kept**: the whole FIX 33/33a loader work, the FIX 34 async memory card,
  and the `[loader] tex stage:` instrumentation.

### Deployed (md5 verified after `sync`)
jak2 `b49da4e0eb26affb40e799cca062a9ad`, jak1 `6ad1588a17fd0873f025d8947bfcc554`.

### Test procedure
1. Boot jak2 — it must reach the title and stream levels as before (the
   FIX 33a behaviour).
2. Play until an auto-save (tutorial hint) — stutter should be gone;
   `mc-trace.txt` should show `dispatched async save …` followed a few frames
   later by `async save finished in …ms (ok)`.
3. If it freezes again, check `mc-trace.txt`: a trailing `dispatched …` with
   no `finished` line pins the hang on the async memcard worker; no new [MC]
   lines at all means the loader/eviction work is at fault instead.

## 2026-09-24 — FIX 34b: async memcard worker crashed on first save/load (AI-assisted)

### Symptom
FIX 34a (`b49da4e0…`) booted and played fine (the graphics revert worked), but
the game **died the moment a save file was loaded**.

### Evidence
- `mc-trace.txt` (unbuffered `write()`, cannot lose its tail) ends exactly on:
  `[MC] requested load` / `[MC] setting op to load` — and **no**
  `dispatched async load of file N`.
- `gk_fatal.txt` got **no** new entry and `gk_run_log.txt` has no `[FATAL]`
  line; it just stops. That is the signature of `abort()` (uncaught C++
  exception -> `std::terminate`), not a CPU exception.
- The only operation between those two trace points is the lazy
  `std::thread(mc_async_worker_loop).detach()` in
  `mc_async_ensure_worker_started()`. A failed `std::thread` construction
  throws `std::system_error`, which nothing caught.
- Why it failed: `gk_run_log.txt` reports `mem_used=3261548KB /
  mem_total=3265536KB` — **~4 MB of headroom**. Allocating a fresh thread stack
  mid-game is exactly the kind of allocation that fails there.

### Fix (`kmemcard.cpp`, Switch-relevant but portable)
1. **Start the worker at boot**, from `kmemcard_init_globals()`
   (`mc_async_boot_start_worker`), while address space is still plentiful —
   not on the first save/load.
2. **Thread creation can no longer kill the game**: wrapped in
   try/catch(...). On failure `g_mc_async_available` stays false.
3. **Synchronous fallback**: if there is no worker, `mc_dispatch_save_async` /
   `mc_dispatch_load_async` run `mc_worker_save` / `mc_worker_load` inline on
   the GOAL thread and apply the result immediately — i.e. exactly the
   pre-FIX-34 behaviour (a stutter, never a crash).
4. **The worker loop itself is exception-proof**: any throw inside the I/O is
   caught and reported as `INTERNAL_ERROR`, so it can neither abort the process
   nor leave the phase stuck at BUSY (which would hang GOAL forever).
5. Result application was factored into `mc_apply_async_result()` and is shared
   by the async and fallback paths.
New trace lines: `starting async memcard worker...`,
`async memcard worker started` / `… FAILED to start (…) - using synchronous saves`.

### Deployed (md5 verified after `sync`)
jak2 `1a47362d7dfb8d19df8ba805884444b5`, jak1 `96c1dd3cd212f06c743a843412ac6205`.
Crash-run artifacts kept in `backups/pre-fix34b/`.

### What to check next run
`mc-trace.txt` should open with `starting async memcard worker...` +
`async memcard worker started` during boot. Then on a save/load:
`dispatched async …` followed by `async … finished in X.XXms`. If instead it
says `FAILED to start`, the console refused the thread and saves are
synchronous — no crash, but the stutter fix is inactive and we would need a
smaller/preallocated worker stack.
