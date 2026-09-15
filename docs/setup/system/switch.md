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
2. **Extract and compile your game data for ARM64** on your PC — see
   [step 1](#1-extract-and-compile-your-game-data-for-arm64). This is mandatory: the desktop
   (x86) output will **not** run on Switch.
3. **Assemble the SD-card tree** — see [step 3](#3-assemble-and-install-the-sd-card-tree).
4. **Copy the contents** of `build-switch/sd-card` to the root of your SD card.
5. **Launch it with full-memory title takeover**, explained below.

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

## 1. Extract and compile your game data for ARM64

First build the desktop tools using the normal OpenGOAL instructions. Then run the extractor with
the ARM64 backend (replace the executable path if your host build places it elsewhere):

```sh
./build/decompiler/extractor /path/to/JAK_AND_DAXTER.iso \
  --extract --compile --game jak1 --instruction-set arm64
```

If the disc is already extracted and validated in `iso_data/jak1`, compile that directory instead:

```sh
./build/decompiler/extractor ./iso_data/jak1 --folder --compile \
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

```sh
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
