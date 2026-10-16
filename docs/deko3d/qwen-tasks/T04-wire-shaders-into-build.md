# T04 — Build the deko3d shaders with the Switch NRO

**Goal:** every Switch build compiles the deko3d shaders and makes the generated headers
includable as `#include "dksh/splash_vert.h"`.

**Edit (1 file):** `game/CMakeLists.txt`, inside the existing `if(NINTENDO_SWITCH)` block
that links `deko3d nx` (around line 371). Add after the `target_link_libraries` line:

```cmake
    # deko3d migration P1.3 (AI-assisted): GLSL -> .dksh (UAM) -> embedded headers.
    include(${CMAKE_SOURCE_DIR}/cmake/Deko3dShaders.cmake)
    set(DK_SHADER_GEN ${CMAKE_CURRENT_BINARY_DIR}/dksh_gen/dksh)
    dk_add_shaders(DK_SHADER_HEADERS ${DK_SHADER_GEN}
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/splash.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/splash.frag)
    add_custom_target(dk_shaders DEPENDS ${DK_SHADER_HEADERS})
    add_dependencies(runtime dk_shaders)
    target_include_directories(runtime PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/dksh_gen)
```
Check `CMAKE_SOURCE_DIR` points to the repo root in this project (look at how other
`cmake/` files are included at the top level); use the same style.

Nothing else changes. Desktop builds must not see any of this (it's inside the Switch `if`).

**Done-check:**
```sh
scripts/ai/check-switch.sh jak1 && ls build-switch-jak1/game/dksh_gen/dksh/
```
Expected: `PASS: jak1 ...` and `splash_frag.h splash_vert.h` (+ the `.dksh` files).
Build takes several minutes. If it fails, paste the FAIL block.
