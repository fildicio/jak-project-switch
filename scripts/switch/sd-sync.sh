#!/bin/sh
# sd-sync.sh (AI-assisted): ONE command per SD trip. Pulls every log + replay dump
# into the repo, deploys the newest jak1 NRO if it changed, keeps the dk flag files,
# re-arms the replay dump only if none was captured yet, then ejects.
# Usage: scripts/switch/sd-sync.sh [path/to/gk.nro]
set -e
SD="/Volumes/SWITCH SD/switch/jak1"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NRO="${1:-$ROOT/build-switch-jak1/game/gk.nro}"
[ -d "$SD" ] || { echo "SD not mounted"; exit 1; }
OUT="$ROOT/hw-logs/$(date +%Y%m%d-%H%M)"; mkdir -p "$OUT"
for f in "$SD"/gk_*.txt; do case "$f" in *gk_dk3d.txt|*gk_dkload.txt|*gk_dktfrag.txt|*gk_dkshrub.txt|*gk_dkdump.txt) ;; *) mv "$f" "$OUT/";; esac; done
if ls "$SD"/dk_replay.* >/dev/null 2>&1; then
  mkdir -p "$ROOT/test/deko3d/replay/jak1"; mv "$SD"/dk_replay.* "$ROOT/test/deko3d/replay/jak1/"
  rm -f "$SD/gk_dkdump.txt"; echo "replay dump pulled"
fi
for f in gk_dk3d.txt gk_dkload.txt gk_dktfrag.txt gk_dkshrub.txt; do [ -f "$SD/$f" ] || echo on > "$SD/$f"; done
[ -f "$ROOT/test/deko3d/replay/jak1/dk_replay.dma" ] || echo 300 > "$SD/gk_dkdump.txt"
if [ -f "$NRO" ] && [ "$(md5 -q "$NRO")" != "$(md5 -q "$SD/jak1.nro")" ]; then
  cp -X "$NRO" "$SD/jak1.nro"; echo "deployed $(md5 -q "$SD/jak1.nro")"
else echo "NRO unchanged $(md5 -q "$SD/jak1.nro")"; fi
sync; diskutil eject "/Volumes/SWITCH SD" >/dev/null && echo "ejected"
grep -h "error\|fatal\|replay\|tfrag world\|fps" "$OUT"/*.txt 2>/dev/null | sort | uniq -c | sort -rn | head -15
echo "logs: $OUT"
