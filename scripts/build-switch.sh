#!/usr/bin/env bash
# Build the Jak 1 runtime as a Nintendo Switch NRO.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build-switch}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"

if [[ ! -x "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-g++" && \
      ! -x "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-g++.exe" ]]; then
  echo "error: devkitA64 was not found under DEVKITPRO=${DEVKITPRO}" >&2
  echo "Install the devkitPro switch-dev group and set DEVKITPRO if it is not /opt/devkitpro." >&2
  exit 1
fi

for lib in libSDL2.a libEGL.a libglapi.a libdrm_nouveau.a; do
  if [[ ! -f "${DEVKITPRO}/portlibs/switch/lib/${lib}" ]]; then
    echo "error: missing ${DEVKITPRO}/portlibs/switch/lib/${lib}" >&2
    echo "Install the devkitPro Switch SDL2/Mesa portlibs (switch-sdl2 and switch-mesa)." >&2
    exit 1
  fi
done

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G "${GENERATOR}" \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchains/Switch.cmake" \
  -DDEVKITPRO="${DEVKITPRO}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTATICALLY_LINK=ON \
  -DBUILD_TESTING=OFF

cmake --build "${BUILD_DIR}" --target gk_nro --parallel "${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

echo "Built ${BUILD_DIR}/game/gk.nro"
