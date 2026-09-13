# Nintendo Switch (Jak 1 homebrew)

This target builds the Jak 1 runtime as an ARM64/GLES 3.1 `.nro`. It does **not** include Sony or
Naughty Dog assets. Use only a supported PlayStation 2 disc that you legally own, and do not
redistribute the packaged `data` directory.

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

Launch `OpenGOAL Jak 1` from full-memory hbmenu.

## Diagnostics

Early boot diagnostics are written to:

- `sdmc:/gk_boot_log.txt`
- `sdmc:/gk_stdout.txt`

Settings and saves use `sdmc:/switch/jak1/OpenGOAL/`. If the NRO immediately returns to hbmenu,
check that it was launched through title takeover, that `data/iso_data/jak1/buildinfo.json` exists,
and that the assets were compiled with `--instruction-set arm64`.
