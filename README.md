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

---

## Setup guide

**What you need**

- A Windows PC with [**MSYS2**](https://www.msys2.org/) installed.
- Your own PS2 ISO files (`jak1.iso`, `jak2.iso`, `jak3.iso`) — only for the games you own.
- From the [latest release](https://github.com/fildicio/jak-project-switch/releases/tag/v.0.3.0):
  the three `.nro` files (`jak1.nro`, `jak2.nro`, `jak3.nro`) and
  `extractor-windows-x86_64.zip` (contains `extractor.exe` and `goalc.exe`).

**Step 1 — Put everything in one folder**

1. Unzip the `jak-project-switch` source zip (or clone this repo) — this is your **project folder**.
2. Unzip `extractor-windows-x86_64.zip` into the project folder (so `extractor.exe` sits next to `README.md`).
3. Put your ISO files in the project folder too.
4. Put the `.nro` files into `build-switch/game/` (create the folder if needed). Don't rename them — `jak2.nro` stays `jak2.nro`.

**Step 2 — Extract each game (run once per game)**

Open CMD or PowerShell **in the project folder** and run **one command per game**:

```
.\extractor.exe jak1.iso --extract --decompile --compile --game jak1 --instruction-set arm64 --proj-path .
.\extractor.exe jak2.iso --extract --decompile --compile --game jak2 --instruction-set arm64 --proj-path .
.\extractor.exe jak3.iso --extract --decompile --compile --game jak3 --instruction-set arm64 --proj-path .
```

(If an ISO is somewhere else, give its full path instead, e.g. `.\extractor.exe "C:\games\jak2.iso" ...`)

This takes a while (often 30+ minutes per game). When it finishes you will have an
`iso_data\jakN` folder and an `out\jakN` folder in the project folder — **both are needed**.

**Step 3 — Package for the SD card (one command, run it any time)**

Open the **MSYS2** terminal from the Windows Start Menu, then:

```
cd /c/path/to/jak-project-switch
./scripts/package-switch.sh
```

That's it. The script automatically finds **every game you have both an `.nro` and
extracted data for**, and packages each one into `build-switch/sd-card/switch/jakN/`.
Games that aren't ready yet are simply skipped with a note telling you exactly what's
missing — so you can re-run the same single command after extracting another game and
it will pick up the new one.

Want just one game? `./scripts/package-switch.sh jak2`. The script reads the game name
out of the NRO itself, so it can never package the wrong game by mistake.

**Step 4 — Copy to the SD card**

Copy the whole `build-switch/sd-card/switch` folder to the root of your SD card (it
contains `jak1`, `jak2`, … — one folder per game you packaged). Launch them from the
Homebrew Menu — hold **R** on a game to enter hbmenu with full memory (title
takeover). Applet mode does not have enough RAM.

Logs (if something goes wrong) are written to `sdmc:/switch/jakN/gk_boot_log.txt`,
`gk_run_log.txt`, `gk_fatal.txt` and `gk_stdout.txt`.

---

## For developers: building the NRO yourself

You don't need this if you use the release `.nro` files. If you changed C++ code, build with:

```
SWITCH_GAME=jak2 bash scripts/build-switch.sh
```

- **Never edit the script to change the game** — always pass `SWITCH_GAME=jakN` before the command.
- The build also saves a copy as `build-switch/game/jakN.nro` so switching games never
  overwrites your only NRO. Switching games triggers a full rebuild — that is expected.
- Then package exactly like Step 3 above: `./scripts/package-switch.sh jak2`.
- Requires devkitPro (`devkitA64`, `switch-sdl2`, `switch-mesa`) with `DEVKITPRO` set
  (default `/opt/devkitpro`).

this guide was written by @fadelmbow
