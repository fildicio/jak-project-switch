# T03 — `cmake/Deko3dShaders.cmake`: the UAM build rule

**Goal:** a CMake function that compiles `.vert`/`.frag` files with UAM and embeds each
result as a header via `cmake/EmbedBinary.cmake` (T02). This is plan item P1.3.

**Create (1 new file):** `cmake/Deko3dShaders.cmake` (~50 lines).

**API:**
```cmake
# dk_add_shaders(<out_headers_var> <gen_dir> <shader files...>)
```
For each source `S` (absolute path):
- stage = `vert` if the extension is `.vert`, `frag` if `.frag`, else `FATAL_ERROR`.
- name = `<basename>_<stage>` (e.g. `splash_vert`); dksh = `${gen_dir}/<name>.dksh`;
  header = `${gen_dir}/<name>.h`.
- `add_custom_command(OUTPUT <header>`
  - `COMMAND ${DEVKITPRO}/tools/bin/uam --stage <stage> -o <dksh> <S>`
  - `COMMAND ${CMAKE_COMMAND} -DIN=<dksh> -DOUT=<header> -DNAME=<name> -P ${_DK_SHADERS_DIR}/EmbedBinary.cmake`
  - `DEPENDS <S> ${_DK_SHADERS_DIR}/EmbedBinary.cmake`
  - `COMMENT "uam <name>" VERBATIM)`
- Append the header to the output list; `set(<out_headers_var> ... PARENT_SCOPE)`.
- `file(MAKE_DIRECTORY ${gen_dir})` once.
- If `DEVKITPRO` is empty, `FATAL_ERROR` with a clear message.

The project requires only CMake 3.10, so `CMAKE_CURRENT_FUNCTION_LIST_DIR` is not
allowed. At file scope (top of the file, outside the function) write
`set(_DK_SHADERS_DIR ${CMAKE_CURRENT_LIST_DIR})` and use `_DK_SHADERS_DIR` inside.

**Do not** touch `game/CMakeLists.txt` (that is T04).

**Done-check:** a throwaway project in `/tmp` (delete it after):
```sh
mkdir -p /tmp/dkt && cat > /tmp/dkt/CMakeLists.txt <<EOF
cmake_minimum_required(VERSION 3.16)
project(dkt NONE)
include($PWD/cmake/Deko3dShaders.cmake)
dk_add_shaders(H \${CMAKE_BINARY_DIR}/gen $PWD/game/graphics/deko3d_renderer/shaders/splash.vert)
message(STATUS "headers: \${H}")
EOF
cmake -S /tmp/dkt -B /tmp/dkt/b -DDEVKITPRO=/opt/devkitpro | grep headers; rm -rf /tmp/dkt
```
Expected: `headers: .../gen/splash_vert.h` (configure only; UAM isn't on the Mac).
