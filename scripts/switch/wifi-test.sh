#!/bin/sh
# wifi-test.sh (AI-assisted): test a new jak1 build WITHOUT moving the SD card.
# On the Switch: open the homebrew menu and press Y (netloader). It shows the Switch IP.
# On the Mac:    scripts/switch/wifi-test.sh 192.168.1.XX   (IP is remembered afterwards)
# nxlink (native Mac build in ~/.local/bin, see docs/deko3d/WIFI_TEST.md) sends the NRO; with
# -s it then prints the game's log (game connects back to port 28771). Quit the game, Ctrl+C.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NRO="${NRO:-$ROOT/build-switch-jak1/game/gk.nro}"  # override: NRO=path scripts/switch/wifi-test.sh
NXLINK="$HOME/.local/bin/nxlink"
IPFILE="$HOME/.switch_ip"
[ -n "$1" ] && echo "$1" > "$IPFILE"
[ -f "$IPFILE" ] || { echo "Usage: $0 <switch-ip>  (shown in hbmenu after pressing Y)"; exit 1; }
[ -x "$NXLINK" ] || { echo "nxlink missing: see docs/deko3d/WIFI_TEST.md"; exit 1; }
[ -f "$NRO" ] || { echo "No build at $NRO"; exit 1; }
IP="$(cat "$IPFILE")"
OUT="$ROOT/hw-logs/$(date +%Y%m%d-%H%M)-wifi"; mkdir -p "$OUT"
LOG="$OUT/gk_net_log.txt"
summary() {
  echo; grep -h "error\|fatal\|world pass\|shaders\|fps\| frame " "$LOG" 2>/dev/null \
    | sed -E 's/^\[[0-9.]+\] //' | sort | uniq -c | sort -rn | head -15
  echo "logs: $LOG"
}
trap 'summary; exit 0' INT TERM
echo "sending $(md5 -q "$NRO") to $IP ... (Switch must show the netloader screen, press Y)"
# `script` gives nxlink a pseudo-terminal so its stdout is line-buffered: piped straight
# into tee it is block-buffered (4 KB) and Ctrl+C on a frozen game lost the whole log.
# stdin from /dev/null keeps `script` from putting this terminal in raw mode; perl drops
# the "^D" + backspaces that EOF echoes and the pty's \r.
script -q /dev/null "$NXLINK" -a "$IP" -s "$NRO" < /dev/null 2>&1 \
  | perl -pe 'BEGIN { $| = 1 } s/\^D\x08\x08//g; s/\r//g' \
  | tee "$LOG" | grep --line-buffered "error\|fatal\|world pass\|fps\|Connection\|Sending"
summary
