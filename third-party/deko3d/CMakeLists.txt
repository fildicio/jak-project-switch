# deko3d 0.5.0 (devkitPro, zlib licence: see LICENSE) built from source for the Switch build,
# replacing the devkita64 image's libdeko3d.a, for the two fixes in PATCHES.md. (AI-assisted, T101)
# generated/ = output of devkitPro's dekotools for source/maxwell/*.def and *.mme (see PATCHES.md).
file(GLOB DEKO3D_SOURCES source/*.cpp source/maxwell/*.cpp)
add_library(deko3d_patched STATIC ${DEKO3D_SOURCES})
target_include_directories(deko3d_patched PUBLIC include)
target_include_directories(deko3d_patched PRIVATE generated source source/maxwell)
target_compile_definitions(deko3d_patched PRIVATE __DK_INTERNAL__ NDEBUG=1)
target_compile_options(deko3d_patched PRIVATE -O2 -fno-rtti -fno-exceptions -Wall)
set_target_properties(deko3d_patched PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
