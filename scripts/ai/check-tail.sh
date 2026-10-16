#!/bin/sh
# check-tail.sh (AI-assisted): bounded poll for the newest dk-check build.
# Waits at most ~20s per call (never hits the 30s agent shell cap), then prints
# the log tail plus a one-word verdict: DONE-PASS / DONE-FAIL / BUILDING.
# Exit codes: 0=PASS, 2=FAIL, 3=still building, 1=no log found.
# Usage: scripts/ai/check-tail.sh [logfile]     (default: newest /tmp/dk-check-*.log)
LOG="${1:-$(ls -t /tmp/dk-check-*.log 2>/dev/null | head -1)}"
[ -z "$LOG" ] && { echo "no /tmp/dk-check-*.log found — run scripts/ai/check-bg.sh first"; exit 1; }
[ -f "$LOG" ] || { echo "no such log: $LOG"; exit 1; }
verdict() {
  if grep -q '^PASS' "$LOG"; then grep '^PASS' "$LOG"; echo DONE-PASS; exit 0
  elif grep -q 'FAIL' "$LOG"; then echo DONE-FAIL; exit 2
  else echo "BUILDING (log: $LOG)"; exit 3; fi
}
i=0
while [ $i -lt 20 ]; do
  grep -q '^PASS\|FAIL' "$LOG" && break
  sleep 1
  i=$((i+1))
done
echo "log: $LOG"
tail -12 "$LOG"
verdict
