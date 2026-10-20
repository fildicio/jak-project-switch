#!/bin/sh
# wifi-test.sh (AI-assisted): test a new jak1 build WITHOUT moving the SD card.
# On the Switch: open the homebrew menu and press Y (netloader). It shows the Switch IP.
# On the Mac:    scripts/switch/wifi-test.sh 192.168.1.XX   (IP is remembered afterwards)
# The NRO is sent over wifi with nxlink; the game streams its log back to this Mac
# (port 9000). Play, quit the game, then press Ctrl+C here to save the log summary.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NRO="$ROOT/build-switch-jak1/game/gk.nro"
IPFILE="$HOME/.switch_ip"
[ -n "$1" ] && echo "$1" > "$IPFILE"
[ -f "$IPFILE" ] || { echo "Usage: $0 <switch-ip>  (shown in hbmenu after pressing Y)"; exit 1; }
IP="$(cat "$IPFILE")"
[ -f "$NRO" ] || { echo "No build at $NRO"; exit 1; }
OUT="$ROOT/hw-logs/$(date +%Y%m%d-%H%M)-wifi"; mkdir -p "$OUT"
LOG="$OUT/gk_net_log.txt"

nc -lk 9000 > "$LOG" &
NC=$!
summary() {
  kill $NC 2>/dev/null || true
  echo; grep -h "error\|fatal\|world pass\|shaders\|fps" "$LOG" 2>/dev/null | sort | uniq -c | sort -rn | head -15
  echo "logs: $LOG"
}
trap 'summary; exit 0' INT TERM

echo "sending $(md5 -q "$NRO") to $IP ..."
docker run --rm -v "$ROOT/build-switch-jak1/game:/w" devkitpro/devkita64 \
  /opt/devkitpro/tools/bin/nxlink -a "$IP" /w/gk.nro
echo "sent. Game is booting; live log below. Quit the game, then press Ctrl+C."
tail -f "$LOG" | grep --line-buffered "error\|fatal\|world pass\|shaders\|fps"
