# Cross-compilation toolchain for Nintendo Switch homebrew (NRO) via devkitPro's devkitA64.
# Flags mirror a hardware-proven devkitPro Makefile build (Nazi Zombies Portable NX port).

# Prefer an explicit -DDEVKITPRO=... (this bundled MSYS2 cmake does not reliably inherit the
# DEVKITPRO env var from a plain shell export), but fall back to the env var if it is set.
if(NOT DEFINED DEVKITPRO)
  if(DEFINED ENV{DEVKITPRO})
    set(DEVKITPRO "$ENV{DEVKITPRO}")
  else()
    message(FATAL_ERROR "Pass -DDEVKITPRO=C:/devkitPro (env var DEVKITPRO is not visible here)")
  endif()
endif()

# NOTE: file(TO_CMAKE_PATH ...) is unsafe here -- this cmake build treats ':' as a path-list
# separator (POSIX-style), so it silently splits "C:/devkitPro" into "C" and "/devkitPro".
string(REPLACE "\\" "/" DEVKITPRO "${DEVKITPRO}")
set(DEVKITA64 "${DEVKITPRO}/devkitA64")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# devkitPro uses .exe names under Windows/MSYS2 and extension-less names on Linux/macOS.
# Do not hard-code either spelling: doing so made the port impossible to configure on two of
# devkitPro's three supported host platforms (the pushed PR head shipped .exe-only and could
# only configure under MSYS2).
if(CMAKE_HOST_WIN32)
  set(SWITCH_HOST_EXE_SUFFIX ".exe")
else()
  set(SWITCH_HOST_EXE_SUFFIX "")
endif()

set(SWITCH_TOOLCHAIN_BIN "${DEVKITA64}/bin")
set(CMAKE_C_COMPILER   "${SWITCH_TOOLCHAIN_BIN}/aarch64-none-elf-gcc${SWITCH_HOST_EXE_SUFFIX}")
set(CMAKE_CXX_COMPILER "${SWITCH_TOOLCHAIN_BIN}/aarch64-none-elf-g++${SWITCH_HOST_EXE_SUFFIX}")
set(CMAKE_ASM_COMPILER "${SWITCH_TOOLCHAIN_BIN}/aarch64-none-elf-gcc${SWITCH_HOST_EXE_SUFFIX}")
set(CMAKE_AR           "${SWITCH_TOOLCHAIN_BIN}/aarch64-none-elf-gcc-ar${SWITCH_HOST_EXE_SUFFIX}")
set(CMAKE_RANLIB       "${SWITCH_TOOLCHAIN_BIN}/aarch64-none-elf-gcc-ranlib${SWITCH_HOST_EXE_SUFFIX}")

# -mcpu implies -march; a separately-specified bare "-march=armv8-a" (no +crc) here would win
# over -mcpu's +crc and silently disable CRC32 intrinsics (hit in common/util/crc32.h).
set(SWITCH_ARCH_FLAGS "-mtune=cortex-a57 -mtp=soft -fPIE -mcpu=cortex-a57+crc+fp+simd")

# newlib's sys/param.h doesn't define __BYTE_ORDER (glibc/BSD convention some third-party libs,
# e.g. fpng, rely on) -- AArch64 on Switch is always little-endian.
set(CMAKE_C_FLAGS_INIT   "${SWITCH_ARCH_FLAGS} -D__SWITCH__ -D__BYTE_ORDER=1234")
set(CMAKE_CXX_FLAGS_INIT "${SWITCH_ARCH_FLAGS} -D__SWITCH__ -D__BYTE_ORDER=1234")
set(CMAKE_ASM_FLAGS_INIT "${SWITCH_ARCH_FLAGS}")

# PIE link hygiene: libnx processes NRO relocations (including packed RELR) at load time
# while segments already carry their final permissions, so a relocation targeting a
# read-only page is an instant data abort before main. The strict -z text from libnx's
# switch.specs turns that into a link-time error ("read-only segment has dynamic
# relocations") -- the correct fix is compiling every TU with -fPIE (see
# third-party/stb_image), NOT silencing it with -z notext.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-specs=${DEVKITPRO}/libnx/switch.specs ${SWITCH_ARCH_FLAGS} -L${DEVKITPRO}/libnx/lib -L${DEVKITPRO}/portlibs/switch/lib")

set(CMAKE_FIND_ROOT_PATH "${DEVKITA64}/aarch64-none-elf" "${DEVKITPRO}/libnx" "${DEVKITPRO}/portlibs/switch")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

include_directories(SYSTEM "${DEVKITPRO}/libnx/include" "${DEVKITPRO}/portlibs/switch/include")
link_directories("${DEVKITPRO}/libnx/lib" "${DEVKITPRO}/portlibs/switch/lib")

set(NINTENDO_SWITCH TRUE)
