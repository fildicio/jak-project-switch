# T67 — P3.3 prep: ETIE / ETIE_BASE deko3d shaders + EtieParams struct (AI-assisted)

Track D PG.2/P3.3 · Depends on: — · Inert (generated headers included by nothing) · One session (~25 min)

## Goal

Tie3 needs 4 shaders (see `dk_tie3_prep.h` kShadersNeeded). TFRAG3 and TIE_WIND are
already converted; ETIE and ETIE_BASE are the last two. A script converts them, CMake
compiles them with UAM, and a C++ struct mirrors their new uniform block. ZERO decisions.

Reviewer trial 2026-10-20: all 4 outputs compile with UAM (1024–1280 B), the jak1 build
PASSES with md5 `ce12b1ecc3975f6a9bbf50d8266987d8` (unchanged: nothing includes the
headers yet), and the struct passes host compile + dk-lint.

## Session rules (hard)

- Do NOT edit shader text by hand. Do NOT edit `scripts/ai/pg2-convert-shader.py`.
- Copy the code blocks below EXACTLY. No other edits.
- Run commands from the repo root. If any output differs from "Expected": STOP and report it.
- Report goes in QWEN_REPORT.md under `## T67`.

## Steps

1. Convert (4 commands in one loop; note `--etie` on the vertex shaders only):
   ```sh
   S=game/graphics/opengl_renderer/shaders; D=game/graphics/deko3d_renderer/shaders
   for n in etie etie_base; do python3 scripts/ai/pg2-convert-shader.py $S/$n.vert vert $D/$n.vert.glsl --etie && python3 scripts/ai/pg2-convert-shader.py $S/$n.frag frag $D/$n.frag.glsl && echo "OK $n"; done
   md5 -r $D/etie*.glsl
   ```
   Expected: `OK etie`, `OK etie_base`, then exactly these 4 md5s:
   ```
   732e5bf4ee16b334c4a7c499dc6728d8 game/graphics/deko3d_renderer/shaders/etie.frag.glsl
   9d38f487a1d50b694c48faddde42c29a game/graphics/deko3d_renderer/shaders/etie.vert.glsl
   8a0506ce3863e135b9f6b29bfe709119 game/graphics/deko3d_renderer/shaders/etie_base.frag.glsl
   05e5d3e203dd9e7da0eb70217134e327 game/graphics/deko3d_renderer/shaders/etie_base.vert.glsl
   ```

2. `game/CMakeLists.txt`: find these 2 lines
   ```cmake
    set(_BG_SHADERS shrub.vert shrub.frag tfrag3_no_tex.vert tfrag3_no_tex.frag
                    tie_wind.vert tie_wind.frag)
   ```
   and replace them with these 3 lines (only the `)` moves down):
   ```cmake
    set(_BG_SHADERS shrub.vert shrub.frag tfrag3_no_tex.vert tfrag3_no_tex.frag
                    tie_wind.vert tie_wind.frag
                    etie.vert etie.frag etie_base.vert etie_base.frag)
   ```

3. `game/graphics/deko3d_renderer/dk_tie3_prep.h`, two edits:
   a. Find the line `#include <cstdint>` and put `#include <cstddef>` on a new line
      DIRECTLY ABOVE it (`offsetof` lives in `<cstddef>`).
   b. Find the LAST line of the file, `}  // namespace dktie`, and insert this block
      DIRECTLY ABOVE it (keep the blank line at the end of the block):
   ```cpp
// --- T67 (AI-assisted): C++ mirror of the EtieParams UBO (binding 8, vertex). ---
// Written by scripts/ai/pg2-convert-shader.py --etie into etie.vert.glsl and
// etie_base.vert.glsl. std140: vec4 = 16 B, mat4 = 4 column vec4s = 64 B.
// GL sets these in Tie3.cpp (etie uniforms); etie_base ignores envmap_tod_tint.
struct EtieParams {
  float persp0[4];           // @0
  float persp1[4];           // @16
  float cam_no_persp[16];    // @32  column-major, like GL glUniformMatrix4fv
  float envmap_tod_tint[4];  // @96
};                           // sizeof 112
static_assert(offsetof(EtieParams, persp1) == 16, "std140 EtieParams");
static_assert(offsetof(EtieParams, cam_no_persp) == 32, "std140 EtieParams");
static_assert(offsetof(EtieParams, envmap_tod_tint) == 96, "std140 EtieParams");
static_assert(sizeof(EtieParams) == 112, "std140 EtieParams");
inline constexpr uint32_t kEtieParamsBinding = 8u;

   ```

4. Checks:
   ```sh
   xcrun clang++ -fsyntax-only -std=c++17 -Wall -I. -include game/graphics/deko3d_renderer/dk_tie3_prep.h -x c++ /dev/null && echo HOST-OK
   python3 scripts/ai/check-dk.py
   ```
   Expected: `HOST-OK` and `dk-lint: PASS ...`.

5. Build: `scripts/ai/check-bg.sh jak1`, then repeat `scripts/ai/check-tail.sh` until
   `DONE-PASS`. Expected md5: **`ce12b1ecc3975f6a9bbf50d8266987d8`**.

6. Prove the 4 shaders were compiled:
   ```sh
   ls build-switch-jak1/game/dksh_gen/dksh/ | grep -cE "^(etie|etie_base)_(vert|frag)_dksh\.h$"
   ```
   Expected: `4`

7. Report: the step 1 md5s, HOST-OK/lint line, build md5, the `4`, `git status --short`.
   Set the README T67 row to 🟡.

## Why (for the report reader)

ETIE's vertex shaders use 4 uniforms (`persp0`, `persp1`, `cam_no_persp`, `envmap_tod_tint`)
that the shared tfrag3 block `VertParams` (binding 7) doesn't have. deko3d/UAM rejects loose
uniforms, so `--etie` puts them in a second std140 block `EtieParams` at binding 8.
The C++ struct must have the same byte layout; the static_asserts prove it at compile time.
