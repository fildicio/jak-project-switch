# Jak Project Switch

> **Unofficial, non-commercial fan project.** This is an unofficial Nintendo Switch
> port of the [OpenGOAL project](https://github.com/open-goal/jak-project). It is
> **not** affiliated with, endorsed by, or sponsored by the OpenGOAL team, Naughty Dog,
> Sony Interactive Entertainment, or Nintendo.

OpenGOAL decompiles and recompiles the original Jak and Daxter trilogy (Jak 1 -> Jak 3)
so it can run natively, using GOAL, a custom LISP language developed by Naughty Dog.
This repository adapts that work to run on the Nintendo Switch.

## Attribution and licensing

- The overwhelming majority of this code comes from
  [open-goal/jak-project](https://github.com/open-goal/jak-project), used under the
  ISC License — see [LICENSE](LICENSE).
- Full attribution for OpenGOAL, devkitPro/libnx and other third-party components is in
  [CREDITS.md](CREDITS.md).
- **If you want to support this work, support OpenGOAL instead** — this repository accepts
  no donations and is not sold or monetised in any form.

## No game assets are distributed

This repository contains **no** game data, ROMs, ISOs, textures, audio, or other
copyrighted assets from any Jak and Daxter title. You must extract game data yourself from
your own legally purchased copy of the game. Nothing here is a substitute for buying the
original games.

Jak and Daxter and all related names, characters and trademarks are the property of
Sony Interactive Entertainment and Naughty Dog. Nintendo Switch is a trademark of Nintendo.
Used here only for identification purposes.

## Takedown requests

If you are a rights holder and believe anything in this repository infringes your rights,
please open an issue or contact the repository owner and it will be addressed promptly.



### How It Works

**Prerequisites**

\* Windows PC with [**MSYS2**](https://www.msys2.org/) installed.

\* Legitimate PS2 ISO files: jak1.iso, jak2.iso, and jak3.iso.

\* Files from the latest release of jak-project-switch:

\*[jak1.nro, jak2.nro, jak3.nro](https://github.com/fildicio/jak-project-switch/releases/tag/v.0.3.0)

\* [extractor-windows-x86\_64.zip](https://github.com/fildicio/jak-project-switch/releases/tag/v.0.3.0) (contains extractor.exe and goalc.exe)

**Step 1: File Preparation**

\* Extract the jak-project-switch-main zip.

\* Extract extractor.exe and goalc.exe directly into the root of your jak-project-switch-main folder.

\* Place your ISO files (jak1.iso, jak2.iso, jak3.iso) into the same project root folder.

**Step 2: Extracting Assets from PS2 ISOs (Run for each game)**

Open PowerShell or CMD in your project directory and run extractor.exe for each ISO file to extract, decompile, and compile the assets for the ARM64 architecture:

\* For Jak 1:

.\\extractor.exe "C:\\path\\to\\jak-project-switch\\jak1.iso" --extract --decompile --compile --game jak1 --instruction-set arm64 --proj-path "C:\\path\\to\\jak-project-switch"

\* For Jak 2:

.\\extractor.exe "C:\\path\\to\\jak-project-switch\\jak2.iso" --extract --decompile --compile --game jak2 --instruction-set arm64 --proj-path "C:\\path\\to\\jak-project-switch"

\* For Jak 3:

.\\extractor.exe "C:\\path\\to\\jak-project-switch\\jak3.iso" --extract --decompile --compile --game jak3 --instruction-set arm64 --proj-path "C:\\path\\to\\jak-project-switch"

**Step 3: Packaging Jak 1 with MSYS2**

\* Open MSYS2 Terminal from your Windows Start Menu.

\* Navigate to your project folder:

cd /c/path/to/jak-project-switch

\* Prepare the build directory and copy jak1.nro as gk.nro:

mkdir -p build-switch/game

cp jak1.nro build-switch/game/gk.nro

\* Package the game for the Switch SD card:

./scripts/package-switch.sh build-switch iso\_data/jak1 build-switch/sd-card

\* Copy the generated files from build-switch/sd-card to your Switch SD card (into sdmc:/switch/jak1/).

**Step 4: Packaging Jak II & Jak 3 (Important Fix)**

If you check scripts/build-switch.sh, you will notice this line:

SWITCH\_GAME="${SWITCH\_GAME:-jak1}"

By default, the runtime bakes in jak1. To compile gk.nro properly for Jak II or Jak 3, pass the SWITCH\_GAME environment variable before building:

\* For Jak II: Copy jak2.nro to build-switch/game/gk.nro, then run:

SWITCH\_GAME=jak2 ./scripts/build-switch.sh

./scripts/package-switch.sh build-switch iso\_data/jak2 build-switch/sd-card

\* For Jak 3: Repeat the same steps using jak3.nro and setting SWITCH\_GAME=jak3:

SWITCH\_GAME=jak3 ./scripts/build-switch.sh

./scripts/package-switch.sh build-switch iso\_data/jak3 build-switch/sd-card

**Step 5: Transfer to SD Card**

Transfer the resulting folders from build-switch/sd-card to your Switch SD card under sdmc:/switch/. You can now launch them via Homebrew Launcher or create NSP Forwarders for your home menu!

this guide was written by @fadelmbow
