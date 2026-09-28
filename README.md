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

## Network Logging Configuration

The project supports live network logging via FIX 7s. To enable network logging:

1. Create a file named `gk_log_host.txt` on your SD card
2. Place the IP address of your development machine in this file (e.g., `192.168.1.100`)
3. The system will automatically connect to port 12345 for live logging

### File Location
- **SD Card Path**: `sdmc:/gk_log_host.txt`
- **Example Content**: 
```
192.168.1.100
```

### How It Works
- The system reads the host address from `gk_log_host.txt` during startup
- If a valid IP is found, logs are sent to that machine via TCP on port 12345
- If no file exists or connection fails, logs fall back to SD card logging only