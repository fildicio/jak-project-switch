#!/usr/bin/env bash
# review-pack (AI-assisted): one short report for the reviewer, so it reads this instead of
# whole files. Usage: scripts/ai/review-pack.sh [base-ref]   (default HEAD)
set -u
cd "$(dirname "$0")/../.."
BASE="${1:-HEAD}"

echo "== tasks waiting for review (README rows marked coder done)"
grep -E "^\| T[0-9]+[a-z]? .*\| 🟡" docs/deko3d/qwen-tasks/README.md | cut -c1-160 || echo "(none)"

echo; echo "== files changed vs $BASE"
git --no-pager diff --stat "$BASE" -- . ':!docs/deko3d/qwen-tasks/QWEN_REPORT.md' | tail -25
git ls-files --others --exclude-standard | grep -vE "^(build|out)/" | sed 's/^/new: /'

echo; echo "== deko3d lint"
python3 scripts/ai/check-dk.py | tail -12

echo; echo "== last Switch build per game"
for g in jak1 jak2 jak3; do
  L="build-switch-$g/ai-check.log"
  if [ -f "$L" ]; then
    printf '%s: %s  (log %s)\n' "$g" "$(md5 -q build-switch-$g/game/gk.nro 2>/dev/null || md5sum build-switch-$g/game/gk.nro | cut -c1-32)" "$(date -r "$L" '+%m-%d %H:%M')"
  fi
done

echo; echo "== host replay test"
if [ -x build-host/goalc-test ]; then
  ./build-host/goalc-test --gtest_filter='DkTfragReplay.*' 2>&1 | grep -E "dk-replay:|SKIPPED \]|FAILED  \]|PASSED  \]" | head -3
else
  echo "(build-host/goalc-test not built)"
fi

echo; echo "== newest QWEN_REPORT sections"
grep -nE "^## T[0-9]+" docs/deko3d/qwen-tasks/QWEN_REPORT.md | tail -5
