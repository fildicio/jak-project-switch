#!/usr/bin/env bash
# Assemble the SD-card directory from NRO files (built or downloaded) and your own
# extracted game data.
#
# Usage:
#   scripts/package-switch.sh              # package EVERY game that is ready
#   scripts/package-switch.sh jak2         # package just one game
#   scripts/package-switch.sh all          # same as no argument
#
# A game is "ready" when BOTH of these exist:
#   - an NRO in <build-dir>/game/:  jakN.nro  (or gk.nro built for that game)
#   - extracted data:  iso_data/jakN/  and  out/jakN/   (from the extractor)
# Games that are not ready are skipped with a note saying exactly what is missing,
# so you can run this one command at any time -- it simply does everything possible.
#
# Optional positional arguments (rarely needed):
#   [build-dir]     default ./build-switch
#   [iso-data-dir]  default ./iso_data/<game>   (single-game mode only)
#   [output-dir]    default <build-dir>/sd-card
#
# The game name is baked into each NRO at build time (it contains the string
# sdmc:/switch/<game>/gk.nro), so the script always knows which game an NRO is --
# it is impossible to package the wrong game by mistake.
#
# Compatible with old-style invocations: GAME=jak2 scripts/package-switch.sh ...
set -euo pipefail

usage() {
  cat >&2 <<EOF
Usage: scripts/package-switch.sh [game|all] [build-dir] [iso-data-dir] [output-dir]

  no argument / all   package every game that has an NRO and extracted data
  jak1 | jak2 | jak3  package just that game

Examples:
  scripts/package-switch.sh              # everything that is ready
  scripts/package-switch.sh jak2         # just Jak II

Game data must come from your own supported PS2 copy, extracted with:
  extractor.exe <iso> --extract --decompile --compile --game <game> \\
      --instruction-set arm64 --proj-path <repo root>
EOF
  exit 2
}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && usage

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Which game to package: leading positional argument wins, then the GAME=
# environment variable (old style), then "package everything ready".
GAME="${GAME:-}"
MODE_ALL=0
if [[ "${1:-}" == "jak1" || "${1:-}" == "jak2" || "${1:-}" == "jak3" ]]; then
  GAME="$1"
  shift
elif [[ "${1:-}" == "all" ]]; then
  MODE_ALL=1
  shift
fi
if [[ -z "${GAME}" ]]; then
  MODE_ALL=1
fi
case "${GAME}" in
  ""|jak1|jak2|jak3) ;;
  *) echo "error: unsupported game '${GAME}' (expected jak1, jak2 or jak3)" >&2; exit 1 ;;
esac

BUILD_DIR="${1:-${ROOT}/build-switch}"
ISO_DIR_ARG="${2:-}"
OUT="${3:-${BUILD_DIR}/sd-card}"
GAME_DIR="${BUILD_DIR}/game"

# Read the game name back out of an NRO (baked in at build time).
nro_game() {
  local nro="$1" g
  for g in jak1 jak2 jak3; do
    if grep -aq "sdmc:/switch/${g}/gk.nro" "$nro"; then
      echo "$g"
      return 0
    fi
  done
  return 1
}

# The NRO for a game: prefer the game-named file, fall back to gk.nro if that is
# what the file actually contains (no manual renaming needed, ever).
find_nro() {
  local game="$1"
  if [[ -f "${GAME_DIR}/${game}.nro" ]]; then
    echo "${GAME_DIR}/${game}.nro"
    return 0
  fi
  if [[ -f "${GAME_DIR}/gk.nro" ]]; then
    if [[ "$(nro_game "${GAME_DIR}/gk.nro" || true)" == "${game}" ]]; then
      echo "${GAME_DIR}/gk.nro"
      return 0
    fi
  fi
  return 1
}

# Copy one game's full SD-card tree. Assumes everything was already validated.
package_one() {
  local game="$1" nro="$2" iso_dir="$3" out="$4"
  local app="${out}/switch/${game}"
  local data="${app}/data"

  rm -rf "${app}"
  mkdir -p "${data}/game/graphics/opengl_renderer" "${data}/log" "${data}/iso_data"
  cp "${nro}" "${app}/gk.nro"
  cp -R "${ROOT}/game/assets" "${data}/game/"
  cp -R "${ROOT}/game/graphics/opengl_renderer/shaders" \
    "${data}/game/graphics/opengl_renderer/"
  cp -R "${ROOT}/goal_src" "${data}/"
  if [[ -d "${ROOT}/custom_assets" ]]; then
    cp -R "${ROOT}/custom_assets" "${data}/"
  fi
  cp -R "${iso_dir}" "${data}/iso_data/${game}"
  # GOAL objects and the rebuilt fake-ISO tree produced by the ARM64 compile step.
  mkdir -p "${data}/out"
  cp -R "${ROOT}/out/${game}" "${data}/out/${game}"

  cat > "${app}/README.txt" <<EOF
OpenGOAL ${game} for Nintendo Switch

Launch gk.nro through hbmenu using full-memory title takeover. Applet mode does not provide enough
memory for the runtime's 128 MiB executable EE arena plus renderer and game data.

Logs:  sdmc:/switch/${game}/gk_boot_log.txt, gk_run_log.txt, gk_fatal.txt and gk_stdout.txt
Saves: sdmc:/switch/${game}/OpenGOAL/${game}/saves

Only use assets extracted from a game copy you legally own. Do not redistribute this data folder.
EOF
  echo "  packaged ${game}  (NRO: ${nro})"
}

# Check that a game's extracted data exists; print a friendly explanation if not.
# Returns 0 = ready, 1 = not ready (message already printed).
data_ready() {
  local game="$1"
  if [[ ! -f "${ROOT}/iso_data/${game}/buildinfo.json" ]]; then
    echo "  skipped ${game}: no iso_data/${game} -- run the extractor for it first (README Step 2)"
    return 1
  fi
  if [[ ! -d "${ROOT}/out/${game}/iso" || ! -d "${ROOT}/out/${game}/obj" ]]; then
    echo "  skipped ${game}: iso_data/${game} exists but out/${game} is missing/incomplete -- re-run the extractor with --compile (README Step 2)"
    return 1
  fi
  return 0
}

PACKAGED=""

if [[ "${MODE_ALL}" -eq 0 ]]; then
  # ---------- single-game mode ----------
  NRO="$(find_nro "${GAME}" || true)"
  if [[ -z "${NRO}" ]]; then
    if [[ -f "${GAME_DIR}/gk.nro" ]]; then
      echo "error: ${GAME_DIR}/gk.nro is a $(nro_game "${GAME_DIR}/gk.nro" || echo 'unknown') build, and there is no ${GAME}.nro alongside it" >&2
      echo "error: build a matching NRO with:  SWITCH_GAME=${GAME} bash scripts/build-switch.sh" >&2
    else
      echo "error: no NRO found in ${GAME_DIR}/ (expected ${GAME}.nro or gk.nro)" >&2
      echo "error: build one with:  SWITCH_GAME=${GAME} bash scripts/build-switch.sh" >&2
      echo "error: or download jak<game>.nro from the releases into ${GAME_DIR}/" >&2
    fi
    exit 1
  fi
  NRO_GAME="$(nro_game "${NRO}" || true)"
  if [[ -z "${NRO_GAME}" ]]; then
    echo "error: cannot tell which game ${NRO} is (not an OpenGOAL Switch NRO?)" >&2
    exit 1
  fi
  if [[ "${NRO_GAME}" != "${GAME}" ]]; then
    echo "error: ${NRO} is a ${NRO_GAME} build, but ${GAME} was requested" >&2
    echo "error: build a matching NRO with:  SWITCH_GAME=${GAME} bash scripts/build-switch.sh" >&2
    exit 1
  fi

  ISO_DIR="${ISO_DIR_ARG:-${ROOT}/iso_data/${GAME}}"
  # Catch swapped arguments (e.g. passing iso_data/jak2 while packaging jak1).
  if [[ -n "${ISO_DIR_ARG}" ]]; then
    base="$(basename "${ISO_DIR}")"
    if [[ "${base}" != "${GAME}" && ( "${base}" == "jak1" || "${base}" == "jak2" || "${base}" == "jak3" ) ]]; then
      echo "error: iso-data-dir is '${ISO_DIR}' but the NRO is a ${GAME} build" >&2
      echo "error: drop the iso-data-dir argument -- the default iso_data/${GAME} is used automatically" >&2
      exit 1
    fi
  fi
  if [[ ! -d "${ISO_DIR}" || ! -f "${ISO_DIR}/buildinfo.json" ]]; then
    echo "error: extracted game data not found: ${ISO_DIR} -- run the extractor first (README Step 2)" >&2
    exit 1
  fi
  if [[ ! -d "${ROOT}/out/${GAME}/iso" || ! -d "${ROOT}/out/${GAME}/obj" ]]; then
    echo "error: ${ROOT}/out/${GAME} is missing or incomplete; re-run the extractor with --compile" >&2
    exit 1
  fi

  echo "Packaging ${GAME} into ${OUT}/switch/${GAME} ..."
  package_one "${GAME}" "${NRO}" "${ISO_DIR}" "${OUT}"
  PACKAGED="${GAME}"
else
  # ---------- package-everything mode ----------
  echo "Looking for games to package in ${GAME_DIR} ..."
  echo ""
  for g in jak1 jak2 jak3; do
    NRO="$(find_nro "${g}" || true)"
    if [[ -z "${NRO}" ]]; then
      echo "  skipped ${g}: no NRO -- put ${g}.nro in ${GAME_DIR}/ (from the releases, or build it)"
      continue
    fi
    if ! data_ready "${g}"; then
      continue
    fi
    package_one "${g}" "${NRO}" "${ROOT}/iso_data/${g}" "${OUT}"
    PACKAGED="${PACKAGED} ${g}"
  done
  echo ""
  if [[ -z "${PACKAGED}" ]]; then
    echo "error: nothing could be packaged -- fix the notes above and run this script again" >&2
    exit 1
  fi
fi

echo "-----------------------------------------------------------"
echo "Done. Packaged:${PACKAGED}"
echo ""
echo "Copy the 'switch' folder to the ROOT of your SD card:"
echo "   ${OUT}/switch   ->   sdmc:/switch"
echo "(each game ends up at sdmc:/switch/<game> and appears in hbmenu)"
