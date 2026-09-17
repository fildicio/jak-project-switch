# Nintendo Switch (Jak 1 homebrew)

This target builds the Jak 1 runtime as an ARM64/GLES 3.1 `.nro`. It does **not** include Sony or
Naughty Dog assets. Use only a supported PlayStation 2 disc that you legally own, and do not
redistribute the packaged `data` directory.

Jak 1 **boots and is playable** on real hardware. It is still experimental: expect frame-rate drops
and the crashes listed under [Known issues](#known-issues).

---

## Quick start (players)

You need three things: a homebrew-capable Switch, the `gk.nro`, and game data that **you** extract
from **your own** PS2 disc. The data is never distributed — not by this project, not by anyone.

### What you need

- A homebrew-capable Switch running a current Atmosphère/libnx environment.
- An SD card with roughly **4 GB free** for the game data.
  - **FAT32 works** — no single file in the package exceeds the 4 GB limit. exFAT is fine too, but
    it is more prone to corruption on Switch; FAT32 is the safer choice.
- Your own legally-owned **PS2 Jak and Daxter disc** (or an ISO you dumped from it). Retail PAL,
  NTSC and NTSC-J builds are supported, including Greatest Hits. PS3/PS4/PS5 re-releases are not.
- A PC (Windows, Linux or macOS) to run the extractor once.

### Steps

1. **Get `gk.nro`** — download it from the **Releases** page of this repository, or build it
   yourself with [step 2](#2-build-the-nro) below.
2. **Build this fork's desktop tools** on your PC — see
   [step 0](#0-build-this-forks-desktop-tools). Upstream OpenGOAL binaries **cannot** be used.
3. **Extract and compile your game data for ARM64** — see
   [step 1](#1-extract-and-compile-your-game-data-for-arm64). This is mandatory: the desktop
   (x86) output will **not** run on Switch.
4. **Assemble the SD-card tree** — see [step 3](#3-assemble-and-install-the-sd-card-tree).
5. **Copy the contents** of `build-switch/sd-card` to the root of your SD card.
6. **Launch it with full-memory title takeover**, explained below.

### Where do the files go? (SD card layout)

Copy the **contents of `build-switch/sd-card/`** — that is, the `switch` folder inside it — to the
**root of your SD card**. Do not copy the `sd-card` folder itself.

You should end up with exactly this:

```text
sdmc:/switch/jak1/gk.nro                  <- the NRO
sdmc:/switch/jak1/data/                   <- keep the folder, named "data"
sdmc:/switch/jak1/data/game/...
sdmc:/switch/jak1/data/goal_src/...
sdmc:/switch/jak1/data/out/jak1/...       <- ARM64-compiled game code
sdmc:/switch/jak1/data/iso_data/jak1/...  <- your extracted disc
sdmc:/switch/jak1/OpenGOAL/               <- created on first launch (settings + saves)
```

Common mistakes:

- ❌ Emptying `data/` and putting its contents next to `gk.nro`. Keep `data` as a folder.
- ❌ Copying the `sd-card` folder to the SD card, giving `sdmc:/sd-card/switch/jak1/`.
- ❌ Renaming `switch/jak1` or `data`. **These paths are compiled into the binary** and are not
  configurable — rename anything and nothing loads.
- ❌ Shipping only `gk.nro` with no `data/`. The NRO contains no game content whatsoever.

`data/iso_data/jak1/buildinfo.json` must exist. If it doesn't, the extractor never finished, and
the game will exit straight back to hbmenu.

### Launching (important)

The runtime reserves a 128 MiB executable EE arena on top of renderer and game memory, which is far
more than applet mode allows. You **must** use *title takeover*:

1. Hold **R** and launch any installed retail game from the Switch home menu.
2. hbmenu opens with that game's full memory allocation.
3. Select **OpenGOAL Jak 1**.

If you open hbmenu the usual way (the Album icon) you get applet mode, and the game will exit
straight back to hbmenu.

### Recommended settings

Set these in the in-game **Options** menu on first launch. They are saved to
`sdmc:/switch/jak1/OpenGOAL/` and persist.

- **Resolution: 720p**
- **Frame rate: 30 FPS**

Higher settings will run, but expect drops. Frame-rate optimisation is still a work in progress.

## Known issues

- **Sentinel Beach — the seagull cutscene crashes the game.**
- **Misty Island — the ambush sequence crashes the game.**
- Frame rate drops in demanding areas; 720p/30 FPS is the recommended configuration.
- Jak 2 and Jak 3 are **not supported** on Switch. Jak 1 only.

Please check this list before reporting a bug.

## Reporting bugs

Open an issue **on this repository** — not on upstream OpenGOAL, and not in the OpenGOAL Discord.
They do not maintain this port and cannot help with it.

Include:

- What you were doing and where (level, cutscene, exact spot).
- `sdmc:/gk_boot_log.txt` and `sdmc:/gk_stdout.txt`.
- Any crash report from `sdmc:/atmosphere/crash_reports/`.
- Your Atmosphère version, and whether you were docked or handheld.

---

## Status and requirements

- Jak 1 only.
- A homebrew-capable Switch with a current Atmosphère/libnx environment.
- Launch through hbmenu **with full-memory title takeover** (hold `R` while opening an installed
  title). Applet mode is not supported: the runtime reserves a 128 MiB executable EE arena in
  addition to renderer and game memory.
- devkitPro's `switch-dev`, `switch-sdl2`, and `switch-mesa` packages.
- CMake and Ninja (or set `CMAKE_GENERATOR="Unix Makefiles"`).
- A normal desktop OpenGOAL build for extracting the disc and compiling GOAL code to ARM64.

On Debian-derived systems, after installing devkitPro pacman using the official instructions:

```sh
sudo dkp-pacman -S switch-dev switch-sdl2 switch-mesa
```

On Windows, run the commands below from the devkitPro MSYS2 shell. The toolchain supports Windows,
Linux and macOS hosts.

## 0. Build this fork's desktop tools

> [!IMPORTANT]
> **You must build the tools from this repository.** The `--instruction-set arm64` option does not
> exist in upstream OpenGOAL, so official OpenGOAL releases and binaries **cannot** produce Switch
> data. Downloading a prebuilt OpenGOAL will not work.

All commands below are **bash**. On Windows use the **devkitPro MSYS2 shell** (or Git Bash for the
non-build steps) — the devkitPro installer provides it, so there is nothing extra to install.

### 0a. Install the prerequisites first

This step is not optional. If you skip it you will get `task: command not found`, the build will
never run, and every later step will fail with `No such file or directory`.

**Windows**

1. Install **Visual Studio 2022** with the **"Desktop development with C++"** workload. This is
   required even if you use another editor — it supplies the Windows SDKs.
2. Install [Scoop](https://scoop.sh/), then:
   ```sh
   scoop install git llvm nasm python task ninja cmake
   ```

**Linux / macOS** — see [linux.md](linux.md) / [macos.md](macos.md). You need `cmake`, `ninja`, a
C++ toolchain, and [`task`](https://taskfile.dev/installation/).

Verify before continuing — every one of these must print a version:

```sh
task --version
cmake --version
ninja --version
```

### 0b. Clone and build

```sh
git clone https://github.com/fildicio/jak-project-switch.git
cd jak-project-switch
task gen-cmake-release
task build-release
```

Expect this to take 10–30 minutes.

<details>
<summary>Without <code>task</code> (plain CMake)</summary>

`task` is only a convenience wrapper. The equivalent on Windows is:

```sh
cmake --preset=Release-windows-clang
cmake --build ./out/build/Release --parallel 8
```

Run `cmake --list-presets` to see the presets available for your platform.

</details>

This produces the extractor. **Its location differs by platform:**

| Platform | Extractor path |
| --- | --- |
| Linux / macOS | `./build/decompiler/extractor` |
| Windows | `./out/build/Release/bin/extractor.exe` |

Substitute the right one in the commands below. `bash: ./build/decompiler/extractor: No such file
or directory` means either the build has not been run, or you are not in the repository directory,
or you are on Windows and need the `out/build/Release/bin` path.

## 1. Extract and compile your game data for ARM64

First complete [step 0](#0-build-this-forks-desktop-tools). Then run the extractor with the ARM64
backend, **from the repository directory**:

```sh
# Linux / macOS
./build/decompiler/extractor /path/to/JAK_AND_DAXTER.iso \
  --extract --decompile --compile --game jak1 --instruction-set arm64

# Windows (MSYS2 / Git Bash)
./out/build/Release/bin/extractor.exe "C:/JAK_AND_DAXTER.iso" \
  --extract --decompile --compile --game jak1 --instruction-set arm64
```

> [!IMPORTANT]
> **`--decompile` is not optional.** Each flag runs one stage and nothing else. Without it the
> extractor jumps straight from unpacking the ISO to compiling, and the compile fails with
> `Input file decompiler_out/jak1/textures/tpage-dir.txt does not exist.` — that file is produced
> by the decompilation stage.

> [!TIP]
> In bash, write Windows paths with **forward** slashes and in quotes: `"C:/JAK_AND_DAXTER.iso"`.
> A backslash path like `"C:\JAK_AND_DAXTER.iso"` gets mangled by escaping, and `/c:/...` is not
> valid — the MSYS form is `/c/JAK_AND_DAXTER.iso`.

If the disc is already extracted and validated in `iso_data/jak1`, compile that directory instead
(adjust the executable path for your platform as above):

```sh
./build/decompiler/extractor ./iso_data/jak1 --folder --decompile --compile \
  --game jak1 --instruction-set arm64
```

The default x86 compile output cannot execute on Switch. Re-run the compile step without
`--instruction-set arm64` before returning to a desktop x86 runtime.

## 2. Build the NRO

```sh
export DEVKITPRO=/opt/devkitpro   # C:/devkitPro in the MSYS2 shell
./scripts/build-switch.sh
```

The result is `build-switch/game/gk.nro`. The CMake target can also be invoked directly:

```sh
cmake -S . -B build-switch -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/Switch.cmake \
  -DDEVKITPRO="$DEVKITPRO" -DCMAKE_BUILD_TYPE=Release \
  -DSTATICALLY_LINK=ON -DBUILD_TESTING=OFF
cmake --build build-switch --target gk_nro
```

## 3. Assemble and install the SD-card tree

The script expects `gk.nro` at `build-switch/game/gk.nro` (where step 2 puts it). If you
downloaded the NRO from the Releases page instead of building it, place it there first:

```sh
mkdir -p build-switch/game
cp /path/to/gk.nro build-switch/game/

./scripts/package-switch.sh build-switch iso_data/jak1 build-switch/sd-card
```

Copy the **contents** of `build-switch/sd-card` to the root of the Switch SD card. The resulting
layout starts with:

```text
sdmc:/switch/jak1/gk.nro
sdmc:/switch/jak1/data/game/assets/...
sdmc:/switch/jak1/data/game/graphics/opengl_renderer/shaders/...
sdmc:/switch/jak1/data/iso_data/jak1/...
```

Launch `OpenGOAL Jak 1` from full-memory hbmenu (hold **R** while opening an installed game).

> [!WARNING]
> **Never redistribute `build-switch/sd-card` or a zip of it.** `data/iso_data/jak1/` and
> `data/out/jak1/` are built from the game's own code and assets — sharing them is distributing
> copyrighted Sony/Naughty Dog material. Only `gk.nro` may be shared.

## Troubleshooting

**`bash: task: command not found`**

You skipped the prerequisites. `task` is [Taskfile](https://taskfile.dev/), installed on Windows via
`scoop install git llvm nasm python task ninja cmake` — and you also need Visual Studio 2022 with
the "Desktop development with C++" workload. See [step 0a](#0a-install-the-prerequisites-first).

Nothing gets built until this is fixed, so every later step will fail with
`No such file or directory`.

**`Input file decompiler_out/jak1/textures/tpage-dir.txt does not exist.`**

You skipped the decompilation stage. The extractor's `--extract`, `--decompile`, `--compile` and
`--play` flags each run **only** that one stage, so `--extract --compile` unpacks the ISO and then
tries to compile with nothing decompiled. `tpage-dir.txt` and everything else under
`decompiler_out/` is produced by `--decompile`.

Re-run the [step 1](#1-extract-and-compile-your-game-data-for-arm64) command with all three flags.
The ISO is already unpacked, so you can skip straight to the remaining stages:

```sh
# adjust the executable path for your platform
./build/decompiler/extractor ./iso_data/jak1 --folder --decompile --compile \
  --game jak1 --instruction-set arm64
```

**`fatal error: 'unistd.h' file not found` while building on Windows**

This was a bug in this fork that broke every Windows build. It is fixed — update your clone and
build again:

```bash
git pull
task build-release
```

If you downloaded the source as a ZIP instead of cloning, download it again.

**`bash: ./build/decompiler/extractor: No such file or directory`**

Three possible causes, in order of likelihood:

1. You are not in the repository directory. If your prompt shows `~`, you are in your home folder —
   `cd` into the cloned `jak-project-switch` first.
2. You have not built the tools yet. Run [step 0](#0-build-this-forks-desktop-tools); `build/` does
   not exist until the project is compiled.
3. **You are on Windows**, where the binary is at `./out/build/Release/bin/extractor.exe`, not
   `./build/decompiler/extractor`.

Also make sure you are not using an upstream OpenGOAL download — it has no `--instruction-set`
option and cannot build Switch data.

**The extractor cannot find my ISO**

In bash, use forward slashes and quotes: `"C:/JAK_AND_DAXTER.iso"`. Backslashes are escape
characters, and `/c:/...` is invalid — the MSYS equivalent is `/c/JAK_AND_DAXTER.iso`.

**The game exits instantly back to hbmenu**

- You launched in applet mode. Use title takeover: hold **R** while opening an installed game.
- `data/iso_data/jak1/buildinfo.json` is missing — the extractor did not finish.
- The data was compiled for x86. Re-run the compile step with `--instruction-set arm64`.

**It crashes shortly after launching, or assets look wrong**

- The data was compiled for a different game version than the disc you extracted.
- The SD card copy was incomplete. Re-copy the whole `switch/jak1` folder and check free space.

**It crashes in Sentinel Beach or Misty Island**

- Known issue, see [Known issues](#known-issues). Not a problem with your setup.

**Poor frame rate**

- Set 720p and 30 FPS in the in-game Options menu. Performance work is ongoing.

**Saves**

- Stored in `sdmc:/switch/jak1/OpenGOAL/jak1/saves`. Back this folder up before replacing `gk.nro`.

## Diagnostics

Early boot diagnostics are written to:

- `sdmc:/gk_boot_log.txt`
- `sdmc:/gk_stdout.txt`

Settings and saves use `sdmc:/switch/jak1/OpenGOAL/`. If the NRO immediately returns to hbmenu,
check that it was launched through title takeover, that `data/iso_data/jak1/buildinfo.json` exists,
and that the assets were compiled with `--instruction-set arm64`.
