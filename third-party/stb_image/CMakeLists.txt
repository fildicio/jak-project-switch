# NOTE: do not `set(CMAKE_CXX_FLAGS ...)` here -- that would clobber the cross-compile
# toolchain flags (e.g. -fPIE/-mcpu for the Switch NRO build), putting const aggregates
# with dynamic relocations into read-only segments and crashing the homebrew loader.
add_library(stb_image stb_image.cpp)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(stb_image PRIVATE -O3)
elseif(MSVC)
  target_compile_options(stb_image PRIVATE /EHsc)
endif()

