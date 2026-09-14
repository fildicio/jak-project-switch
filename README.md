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

Experimental. Jak 1 only. Expect bugs, crashes, and missing features. This is a hobby port and
comes with no warranty or support commitment of any kind.

Jak 2 and Jak 3 are **not** supported on the Switch target. On desktop (Windows/Linux/macOS) this
fork behaves like upstream OpenGOAL.

## Building

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

## About upstream OpenGOAL

OpenGOAL decompiles the original Jak and Daxter trilogy — over 98% of which was written in GOAL, a
custom LISP created by Naughty Dog — into readable GOAL source, then recompiles it natively with a
purpose-built compiler. The result is a native application, not an emulator or transpiler.

Upstream project: https://github.com/open-goal/jak-project · Docs: https://opengoal.dev

A technical overview of the components in this repository is available in
[docs/project-overview.md](/docs/project-overview.md).
