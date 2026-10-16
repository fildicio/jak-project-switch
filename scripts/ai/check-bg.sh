#!/bin/sh
# check-bg.sh (AI-assisted): start a check-switch.sh build in the BACKGROUND.
# Returns instantly (<1s) — safe under the agent 30s shell cap. Never times out.
# Usage:   scripts/ai/check-bg.sh jak1
# Then:    scripts/ai/check-tail.sh     (repeat every few seconds until DONE-PASS/DONE-FAIL)
set -e
GAME="${1:?usage: check-bg.sh jak1|jak2|jak3}"
case "$GAME" in jak1|jak2|jak3) ;; *) echo "unknown game: $GAME" >&2; exit 1;; esac
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LOG="/tmp/dk-check-$GAME-$(date +%Y%m%d-%H%M%S).log"
PREV="$(ls -t /tmp/dk-check-*.log 2>/dev/null | head -1 || true)"
if [ -n "$PREV" ] && ! grep -q '^PASS\|FAIL' "$PREV" 2>/dev/null \
   && [ -n "$(find "$PREV" -mmin -15 2>/dev/null)" ]; then
  echo "NOTE: $PREV may still be building; two builds at once is slow but works."
fi
nohup "$ROOT/scripts/ai/check-switch.sh" "$GAME" > "$LOG" 2>&1 &
echo "started pid $! game=$GAME"
echo "log: $LOG"
echo "poll with: scripts/ai/check-tail.sh"
