# jak-project-switch — unofficial Nintendo Switch port of OpenGOAL

> [!IMPORTANT]
> **This is an unofficial fork of [open-goal/jak-project](https://github.com/open-goal/jak-project).**
> It is **not** affiliated with, endorsed by, or supported by the OpenGOAL team, Naughty Dog,
> Sony Interactive Entertainment, or Nintendo.
>
> **Do not open issues about this fork on the upstream OpenGOAL repository, and do not ask for
> support with this fork in the OpenGOAL Discord.** Report problems here instead.

This fork adds an experimental, native **Nintendo Switch homebrew** target (`gk.nro`) for **Jak 1**,
built with devkitPro's `devkitA64` toolchain against ARM64 / GLES 3.1. Everything else in this
repository is upstream OpenGOAL, which does the actual heavy lifting: the GOAL decompiler, the
GOAL compiler, and the C++ runtime.

See [CREDITS.md](CREDITS.md) for attribution and [LICENSE](LICENSE) for the ISC license that
covers the upstream code.

## Legal

> [!WARNING]
> **No game assets are included or distributed by this project.** You must supply your own
> legally purchased PS2 copy of the game and extract the data from it yourself. OpenGOAL supports
> retail PAL, NTSC and NTSC-J PS2 builds (including Greatest Hits), but *not* the later PS3/PS4/PS5
> re-releases.
>
> Do not redistribute the packaged `data` directory or any extracted asset. Running homebrew on a
> Nintendo Switch requires a homebrew-capable console; setting that up is out of scope for this
> repository and no help with it will be provided here.

## Status

Jak 1 **boots and is playable** on real hardware. It is still experimental — expect frame-rate
drops and some crashes. Known issues:

- **Sentinel Beach** — the seagull cutscene crashes the game.
- **Misty Island** — the ambush sequence crashes the game.
- Frame rate drops in demanding areas. **720p / 30 FPS is the recommended configuration**, set in
  the in-game Options menu.

This is a hobby port and comes with no warranty or support commitment of any kind.

Jak 2 and Jak 3 are **not** supported on the Switch target. On desktop (Windows/Linux/macOS) this
fork behaves like upstream OpenGOAL.

See [docs/setup/system/switch.md](/docs/setup/system/switch.md) for setup, recommended settings,
troubleshooting, and how to report bugs.

**Bugs go [here](../../issues), not to upstream OpenGOAL or their Discord.** Include
`sdmc:/gk_boot_log.txt` and `sdmc:/gk_stdout.txt`.

## Install (players)

> [!IMPORTANT]
> **The `.nro` on its own does nothing.** It contains no game content at all. You must extract the
> data from your own PS2 disc on a PC first — there is no way around this step.

### What you need

- A homebrew-capable Switch (current Atmosphère). Setting that up is out of scope here.
- An SD card with about **4 GB free**. FAT32 is fine and is the safer choice.
- **Your own PS2 Jak and Daxter disc**, or an ISO you dumped from it. Retail PAL, NTSC and NTSC-J
  are supported, including Greatest Hits. PS3/PS4/PS5 re-releases are not.
- A **PC** (Windows, Linux or macOS) to run the extractor once.

### Steps

> [!WARNING]
> **The tools must be built from this repository.** `--instruction-set arm64` does not exist in
> upstream OpenGOAL, so official OpenGOAL downloads cannot produce Switch data.
>
> All commands are **bash**. On Windows use the **devkitPro MSYS2 shell** or Git Bash — not
> PowerShell or CMD.

1. **Download `gk.nro`** from the [Releases](../../releases) page.
2. **On your PC**, clone and build this fork's desktop tools:
   ```sh
   git clone https://github.com/fildicio/jak-project-switch.git
   cd jak-project-switch
   task gen-cmake-release
   task build-release
   ```
3. **Extract and compile your disc for ARM64**, from inside that folder:
   ```sh
   # Linux / macOS
   ./build/decompiler/extractor /path/to/JAK_AND_DAXTER.iso \
     --extract --compile --game jak1 --instruction-set arm64

   # Windows (MSYS2 / Git Bash) -- note the different path
   ./out/build/Release/bin/extractor.exe "C:/JAK_AND_DAXTER.iso" \
     --extract --compile --game jak1 --instruction-set arm64
   ```
   The default x86 output will **not** run on Switch.
4. **Assemble the SD-card tree:**
   ```sh
   ./scripts/package-switch.sh build-switch iso_data/jak1 build-switch/sd-card
   ```
5. **Copy the contents of `build-switch/sd-card/`** (the `switch` folder inside it) to the **root of
   your SD card**, so you end up with:
   ```text
   sdmc:/switch/jak1/gk.nro
   sdmc:/switch/jak1/data/...
   ```
   Keep `data` as a folder, and do not rename anything — **these paths are compiled into the
   binary**.
6. **Launch with full-memory title takeover:** hold **R** while opening any installed game, then
   pick **OpenGOAL Jak 1** in hbmenu. Opening hbmenu from the Album icon gives applet mode, which
   does not have enough memory and will drop you straight back out.

Set **720p / 30 FPS** in the in-game Options menu on first launch.

Full instructions, SD layout details, troubleshooting and bug reporting:
**[docs/setup/system/switch.md](/docs/setup/system/switch.md)**

## Building from source (developers)

### Nintendo Switch target

See **[docs/setup/system/switch.md](/docs/setup/system/switch.md)** — this is the document specific
to this fork. It covers the devkitPro packages, extracting and compiling your game data for ARM64,
and packaging the `.nro`.

### Desktop targets (needed first)

You need a normal desktop build to extract the disc and compile GOAL code, even when targeting the
Switch.

- [Windows](/docs/setup/system/windows.md)
- [Linux](/docs/setup/system/linux.md)
- [macOS](/docs/setup/system/macos.md)
- [Docker](/docs/setup/system/docker.md)

Editor setup: [Visual Studio](/docs/setup/dev/vs.md) · [VS Code](/docs/setup/dev/vscode.md) · [Zed](/docs/setup/dev/zed.md)

#### Extract assets

Select the game and version, then extract:

```sh
task set-game-jak1
task set-decomp-ntscv1   # or e.g. `task set-decomp-pal`
task extract
```

Place your ISO's contents in `iso_data/jak1` first. Run `task --list` for the other options.

#### Build and run

```sh
task repl        # then run (mi) at the `g >` prompt to build the game
task boot-game   # in a second terminal
```

To attach the REPL to a running game, run `(lt)` after a successful `(mi)`.

> [!NOTE]
> If you are not using the default game version, `(mi)` may fail with something like
> `Input file iso_data/jak1/MUS/TWEAKVAL.MUS does not exist.` The decompiler keys its input/output
> folders off the `gameName` config field (e.g. `iso_data/jak1_pal`). See the `gameVersionFolder`
> field documented in [`goal_src/user/README.md`](./goal_src/user/README.md).

## Support / donations

**This fork does not accept donations, and never will.** Please don't offer them here.

Almost all of the engineering in this repository is upstream's work — this fork only adds a Switch
target on top of it. If this port was useful to you and you want to give something back, give it to
the people who actually built the thing:

- **OpenGOAL** — https://github.com/open-goal/jak-project · https://opengoal.dev
- **devkitPro** (the `devkitA64` toolchain and `libnx`) — https://devkitpro.org

Check their own pages for current ways to support them. Contributing bug reports, fixes, or
documentation upstream is worth at least as much as money.

## About upstream OpenGOAL

OpenGOAL decompiles the original Jak and Daxter trilogy — over 98% of which was written in GOAL, a
custom LISP created by Naughty Dog — into readable GOAL source, then recompiles it natively with a
purpose-built compiler. The result is a native application, not an emulator or transpiler.

Upstream project: https://github.com/open-goal/jak-project · Docs: https://opengoal.dev

A technical overview of the components in this repository is available in
[docs/project-overview.md](/docs/project-overview.md).
