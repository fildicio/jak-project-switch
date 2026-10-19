#!/usr/bin/env bash
# AI build gate (AI-assisted): docker build + EL0 sysreg audit. Never deploys.
# Usage: scripts/ai/check-switch.sh [jak1|jak2|jak3] [--no-build]
# Prints a short PASS/FAIL summary; full build log in build-switch-<game>/ai-check.log.
set -uo pipefail

GAME=jak1
NO_BUILD=0
for a in "$@"; do
  case "$a" in
    jak1|jak2|jak3) GAME="$a" ;;
    --no-build) NO_BUILD=1 ;;
    *) echo "FAIL: usage: check-switch.sh [jak1|jak2|jak3] [--no-build]"; exit 2 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
BUILD="build-switch-$GAME"
LOG="$BUILD/ai-check.log"
IMAGE="devkitpro/devkita64:latest"
mkdir -p "$BUILD"

if [[ $NO_BUILD -eq 0 ]]; then
  echo "== build $GAME (log: $LOG)"
  if ! docker run --rm -v "$ROOT:/work" -w /work "$IMAGE" \
      env OPENGOAL_PIN_REVISION=aigate SWITCH_GAME="$GAME" BUILD_DIR="/work/$BUILD" JOBS=2 bash scripts/build-switch.sh \
      >"$LOG" 2>&1; then
    echo "FAIL: build. First errors:"
    grep -nE "error:|undefined reference|fatal error" "$LOG" | head -20
    echo "--- last lines:"; tail -15 "$LOG"
    exit 1
  fi
  echo "build ok ($(grep -cE 'warning:' "$LOG") warnings)"
fi

ELF="$BUILD/game/gk"
[[ -f "$ELF" ]] || { echo "FAIL: $ELF missing"; exit 1; }

# The Switch kernel traps EL0 reads of most system registers (cntvct_el0 crashed the
# first [glt] NRO). Allowed = what libnx/libc already use in known-good builds.
echo "== EL0 sysreg audit ($ELF)"
AUDIT=$(docker run --rm -v "$ROOT:/work" -w /work "$IMAGE" bash -c \
  "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump -d --no-show-raw-insn '$ELF' \
   | grep -oE 'mrs[[:space:]]+x[0-9]+, [a-z0-9_]+' | awk '{print \$3}' | sort | uniq -c | sort -rn")
echo "$AUDIT"
ALLOWED='^(cntpct_el0|tpidrro_el0|tpidr_el0|fpcr|fpsr|cntfrq_el0|ctr_el0|dczid_el0|tpidr2_el0|gcspr_el0|s3_3_c2_c5_1|nzcv)$'
BAD=$(echo "$AUDIT" | awk 'NF{print $2}' | grep -vE "$ALLOWED")
if [[ -n "$BAD" ]]; then
  echo "FAIL: forbidden EL0 sysreg read(s): $(echo $BAD)"
  echo "Locate: objdump -d $ELF | grep -n 'mrs.*<reg>' (inside the docker image)"
  exit 1
fi

echo "PASS: $GAME  nro=$BUILD/game/gk.nro  md5=$(md5 -q "$BUILD/game/gk.nro" 2>/dev/null)"
echo "Not deployed. SD deploy is a human step (docs/deko3d/AGENT_KNOWLEDGE_MAP.md §12)."
