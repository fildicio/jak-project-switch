#!/usr/bin/env bash
# Assemble the SD-card directory from a built NRO and user-extracted game data.
set -euo pipefail

usage() {
  cat >&2 <<EOF
Usage: GAME=jak2 scripts/package-switch.sh [build-dir] [iso-data-dir] [output-dir]

GAME (environment variable, default jak1) selects which game to package. It must match
the SWITCH_GAME the gk.nro was built with (scripts/build-switch.sh); the script verifies
this and refuses to package a mismatched NRO.

Defaults (with GAME=jak1):
  build-dir     ./build-switch
  iso-data-dir  ./iso_data/jak1
  output-dir    ./build-switch/sd-card

Set ZIP=1 to additionally write a clean <game>-switch.zip next to the build dir.
The zip is made from the freshly packaged tree only -- never from an SD-card folder --
so it can never pick up gk_* logs, .DS_Store or __MACOSX junk that accumulates on cards.

The ISO data must come from your own supported PS2 copy and must have been compiled for ARM64.
EOF
  exit 2
}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && usage

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="${GAME:-jak1}"
case "${GAME}" in
  jak1|jak2|jak3) ;;
  *) echo "error: unsupported GAME '${GAME}' (expected jak1, jak2 or jak3)" >&2; exit 1 ;;
esac
BUILD_DIR="${1:-${ROOT}/build-switch}"
ISO_DIR="${2:-${ROOT}/iso_data/${GAME}}"
OUT="${3:-${BUILD_DIR}/sd-card}"
NRO="${BUILD_DIR}/game/gk.nro"
APP="${OUT}/switch/${GAME}"
DATA="${APP}/data"

[[ -f "${NRO}" ]] || {
  echo "error: NRO not found: ${NRO}" >&2
  echo "error: build it with: cmake --build ${BUILD_DIR} --target gk_nro" >&2
  exit 1
}
# The game is baked into the NRO at build time (SWITCH_GAME -> SWITCH_GAME_NAME). A mismatch
# here would silently boot the wrong game's kernel against the wrong data folder.
if ! grep -aq "sdmc:/switch/${GAME}/gk.nro" "${NRO}"; then
  echo "error: ${NRO} was not built for ${GAME} (no 'sdmc:/switch/${GAME}/gk.nro' inside it)" >&2
  echo "error: rebuild it with: SWITCH_GAME=${GAME} bash scripts/build-switch.sh" >&2
  exit 1
fi
[[ -d "${ISO_DIR}" ]] || { echo "error: extracted game data not found: ${ISO_DIR}" >&2; exit 1; }
[[ -f "${ISO_DIR}/buildinfo.json" ]] || {
  echo "error: ${ISO_DIR}/buildinfo.json is missing; run the OpenGOAL extractor first" >&2
  exit 1
}
# The runtime's fake_iso scans <data>/out/${GAME}/iso and its fileio loads <data>/out/${GAME}/obj/*.go
[[ -d "${ROOT}/out/${GAME}/iso" && -d "${ROOT}/out/${GAME}/obj" ]] || {
  echo "error: ${ROOT}/out/${GAME} is missing or incomplete; compile the game with" >&2
  echo "error:   ./build-host/decompiler/extractor <iso-or-folder> --decompile --compile --game ${GAME} --instruction-set arm64" >&2
  exit 1
}

rm -rf "${APP}"
mkdir -p "${DATA}/game/graphics/opengl_renderer" "${DATA}/log"
cp "${NRO}" "${APP}/gk.nro"
cp -R "${ROOT}/game/assets" "${DATA}/game/"
cp -R "${ROOT}/game/graphics/opengl_renderer/shaders" \
  "${DATA}/game/graphics/opengl_renderer/"
# goal_src is NOT copied: on the console it is only read by the jak2/jak3 debug level-editor
# SQL hook (initialize_sql_db), never during normal play; it would waste ~327 MB per game.
if [[ -d "${ROOT}/custom_assets" ]]; then
  cp -R "${ROOT}/custom_assets" "${DATA}/"
fi
# iso_data is NOT copied: the runtime only reads out/${GAME}/{iso,obj} (fake_iso + fileio);
# iso_data is a host-only input for goalc/extractor and would waste 1.4-4.2 GB per game.
# GOAL objects and the rebuilt fake-ISO tree produced by the ARM64 compile step.
mkdir -p "${DATA}/out"
cp -R "${ROOT}/out/${GAME}" "${DATA}/out/${GAME}"

cat > "${APP}/README.txt" <<EOF
OpenGOAL ${GAME} for Nintendo Switch
Switch port made by fildicio (github.com/fildicio/jak-project-switch).
Do not re-upload, mirror or share the pre-compiled binaries without explicit attribution.

Launch gk.nro through hbmenu using full-memory title takeover. Applet mode does not provide enough
memory for the runtime's 128 MiB executable EE arena plus renderer and game data.

Logs:  sdmc:/switch/${GAME}/gk_boot_log.txt, gk_run_log.txt, gk_fatal.txt and gk_stdout.txt
Saves: sdmc:/switch/${GAME}/OpenGOAL/${GAME}/saves

Only use assets extracted from a game copy you legally own. Do not redistribute this data folder.
EOF

if [[ "${ZIP:-0}" == "1" ]]; then
  command -v zip >/dev/null || { echo "error: ZIP=1 but 'zip' was not found" >&2; exit 1; }
  ZIP_PATH="${ZIP_PATH:-${BUILD_DIR}/${GAME}-switch.zip}"
  rm -f "${ZIP_PATH}"
  # -X: no extra file attributes; the tree was just rebuilt by this script, so there is
  # nothing to exclude -- logs/saves only ever exist on a real card, not in this package.
  (cd "${OUT}" && zip -r -X -q "${ZIP_PATH}" switch)
  echo "Clean archive at ${ZIP_PATH}"
fi

echo "Packaged SD-card tree for ${GAME} at ${OUT}"
echo "Copy the contents of that directory to the root of the SD card."
