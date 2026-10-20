# Wifi test loop (AI-assisted)

Test a jak1 build without moving the SD card: `scripts/switch/wifi-test.sh <switch-ip>`.

- On the Switch: homebrew menu → press **Y** (netloader screen shows the IP).
- The script sends `build-switch-jak1/game/gk.nro` with **nxlink**, then `nxlink -s` prints
  the game's log. The game connects back to the Mac on port 28771 when it was launched by
  nxlink (`switch_net_log_init`, `__nxlink_host`). A `sdmc:/gk_log_host.txt` file overrides it.
- Quit the game, press Ctrl+C: a summary is printed, the log is in `hw-logs/<time>-wifi/`.

nxlink must run natively (Docker Desktop cannot reach the LAN). One-time install:

    mkdir -p ~/.local/src/nxlink ~/.local/bin && cd ~/.local/src/nxlink
    curl -sfLO https://raw.githubusercontent.com/switchbrew/switch-tools/master/src/nxlink.c
    clang -O2 -w -o ~/.local/bin/nxlink nxlink.c -lz

Still needs the SD in the Mac: replay dumps (`dk_replay.*`) and crash files (`gk_fatal.txt`).
