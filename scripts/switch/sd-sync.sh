#!/bin/sh
# sd-sync.sh (AI-assisted): ONE command per SD trip. Pulls every log + replay dump
# into the repo, deploys the newest jak1 NRO if it changed, keeps the dk flag files + gk_opengl.txt (opt-out),
# re-arms the replay dump only if none was captured yet, then ejects.
# Usage: scripts/switch/sd-sync.sh [path/to/gk.nro]
set -e
SD="/Volumes/SWITCH SD/switch/jak1"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NRO="${1:-$ROOT/build-switch-jak1/game/gk.nro}"
[ -d "$SD" ] || { echo "SD not mounted"; exit 1; }
OUT="$ROOT/hw-logs/$(date +%Y%m%d-%H%M)"; mkdir -p "$OUT"
for f in "$SD"/gk_*.txt; do case "$f" in *gk_dk3d.txt|*gk_dkload.txt|*gk_dktfrag.txt|*gk_dkshrub.txt|*gk_dktie.txt|*gk_dkdump.txt|*gk_opengl.txt|*gk_dkgl_eyeprobe.txt) ;; *) mv "$f" "$OUT/";; esac; done
# #18: the lg:: log (e.g. "[FAKEISO] failed to find VAGWAD  ITA") is file-only on Switch, under log/
find "$SD" -maxdepth 4 -path "*log*" -name "*.log" -mtime -2 -exec cp -X {} "$OUT/" \; 2>/dev/null || true
# #18: the saved settings (game-language = voices, text-language, subtitle-language; Italian = 4)
find "$SD" -maxdepth 5 -name "pc-settings.gc" -exec cp -X {} "$OUT/" \; 2>/dev/null || true
# #18: which language files are on the SD (voices VAGWAD.<lang>, text/subtitles <N>COMMON/<N>SUBTIT; Italian = 4)
ISO="$SD/data/out/jak1/iso"
[ -d "$ISO" ] && { ls "$ISO" | grep -E '^VAGWAD|^[0-9]+(COMMON|SUBTIT)\.TXT$' | tr '\n' ' '; echo; } > "$OUT/lang_files.txt"
if ls "$SD"/dk_replay.* >/dev/null 2>&1; then
  mkdir -p "$ROOT/test/deko3d/replay/jak1"; mv "$SD"/dk_replay.* "$ROOT/test/deko3d/replay/jak1/"
  rm -f "$SD/gk_dkdump.txt"; echo "replay dump pulled"
fi
# jak1 runs deko3d by default now (TODO #12); old gk_dk*.txt files are harmless.
[ -f "$ROOT/test/deko3d/replay/jak1/dk_replay.dma" ] || echo 300 > "$SD/gk_dkdump.txt"
if [ -f "$NRO" ] && [ "$(md5 -q "$NRO")" != "$(md5 -q "$SD/jak1.nro")" ]; then
  cp -X "$NRO" "$SD/jak1.nro"; echo "deployed $(md5 -q "$SD/jak1.nro")"
else echo "NRO unchanged $(md5 -q "$SD/jak1.nro")"; fi
sync; diskutil eject "/Volumes/SWITCH SD" >/dev/null && echo "ejected"
grep -h "error\|fatal\|replay\|tfrag world\|fps" "$OUT"/*.txt 2>/dev/null | sort | uniq -c | sort -rn | head -15
[ -f "$OUT/lang_files.txt" ] && echo "language files on SD: $(cat "$OUT/lang_files.txt")"
[ -f "$OUT/pc-settings.gc" ] && grep -E "language" "$OUT/pc-settings.gc"
echo "logs: $OUT"
