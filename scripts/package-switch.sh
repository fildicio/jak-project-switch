#!/usr/bin/env bash
# Assemble the SD-card directory from a built NRO and user-extracted Jak 1 data.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: scripts/package-switch.sh [build-dir] [iso-data-dir] [output-dir]

Defaults:
  build-dir     ./build-switch
  iso-data-dir  ./iso_data/jak1
  output-dir    ./build-switch/sd-card

The ISO data must come from your own supported PS2 copy and must have been compiled for ARM64.
EOF
  exit 2
}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && usage

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT}/build-switch}"
ISO_DIR="${2:-${ROOT}/iso_data/jak1}"
OUT="${3:-${BUILD_DIR}/sd-card}"
NRO="${BUILD_DIR}/game/gk.nro"
APP="${OUT}/switch/jak1"
DATA="${APP}/data"

[[ -f "${NRO}" ]] || { echo "error: NRO not found: ${NRO}" >&2; exit 1; }
[[ -d "${ISO_DIR}" ]] || { echo "error: extracted game data not found: ${ISO_DIR}" >&2; exit 1; }
[[ -f "${ISO_DIR}/buildinfo.json" ]] || {
  echo "error: ${ISO_DIR}/buildinfo.json is missing; run the OpenGOAL extractor first" >&2
  exit 1
}
# The runtime's fake_iso scans <data>/out/jak1/iso and its fileio loads <data>/out/jak1/obj/*.go
[[ -d "${ROOT}/out/jak1/iso" && -d "${ROOT}/out/jak1/obj" ]] || {
  echo "error: ${ROOT}/out/jak1 is missing or incomplete; compile the game with" >&2
  echo "error:   ./build-host/decompiler/extractor <iso-or-folder> --decompile --compile --game jak1 --instruction-set arm64" >&2
  exit 1
}

rm -rf "${APP}"
mkdir -p "${DATA}/game/graphics/opengl_renderer" "${DATA}/log" "${DATA}/iso_data"
cp "${NRO}" "${APP}/gk.nro"
cp -R "${ROOT}/game/assets" "${DATA}/game/"
cp -R "${ROOT}/game/graphics/opengl_renderer/shaders" \
  "${DATA}/game/graphics/opengl_renderer/"
cp -R "${ROOT}/goal_src" "${DATA}/"
if [[ -d "${ROOT}/custom_assets" ]]; then
  cp -R "${ROOT}/custom_assets" "${DATA}/"
fi
cp -R "${ISO_DIR}" "${DATA}/iso_data/jak1"
# GOAL objects and the rebuilt fake-ISO tree produced by the ARM64 compile step.
mkdir -p "${DATA}/out"
cp -R "${ROOT}/out/jak1" "${DATA}/out/jak1"

cat > "${APP}/README.txt" <<'EOF'
OpenGOAL Jak 1 for Nintendo Switch

Launch gk.nro through hbmenu using full-memory title takeover. Applet mode does not provide enough
memory for the runtime's 128 MiB executable EE arena plus renderer and game data.

Logs:  sdmc:/gk_boot_log.txt and sdmc:/gk_stdout.txt
Saves: sdmc:/switch/jak1/OpenGOAL/jak1/saves

Only use assets extracted from a game copy you legally own. Do not redistribute this data folder.
EOF

echo "Packaged SD-card tree at ${OUT}"
echo "Copy the contents of that directory to the root of the SD card."
