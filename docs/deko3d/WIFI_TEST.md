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

## On-demand frame grab (AI-assisted)

`R3 + Plus` held together in game arms a one-shot readback of the next presented frame;
two frames later the game streams it as raw RGBA8 over TCP **28773** to the same host
the live log uses (the nxlink Mac, or `gk_log_host.txt`). No SD writes, nothing
allocated until the combo is pressed, one visible hitch per grab (the send is
synchronous -- it is meant to be pressed while posing a scene).

On the Mac, start the receiver first:

    python3 scripts/switch/frame-receiver.py            # saves hw-logs/frames/*.png
    python3 scripts/switch/frame-receiver.py outdir     # or a custom dir

Expected log lines on the game side: `[grab] R3+Plus: frame grab armed`, then
`[grab] frame N armed WxH` and `[grab] frame WxH (NNN KB) sent to <ip>`.
