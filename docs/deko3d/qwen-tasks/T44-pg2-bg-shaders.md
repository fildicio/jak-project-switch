# T44 — PG.2: jak1 background shaders → std140 + uam rule (P3.3 prep, AI-assisted)

Track D PG.2 · Depends on: — · Inert (headers included by nothing) · One session (~30 min)

## Goal

Convert the 3 GL shader pairs that jak1 Tie3/Shrub need (`shrub`,
`tfrag3_no_tex`, `tie_wind`) into deko3d `.glsl` files, and compile them at build
time with the same uam rule tfrag3 uses. ZERO decisions: a script does the
conversion; the CMake block is given verbatim.

Reviewer pre-checks (2026-10-19): the script reproduces the existing
`tfrag3.{vert,frag}.glsl` byte-for-byte, and all 6 outputs compile with UAM in
the devkitPro image (UAM OK, 512–1280 B each). All 6 use the SAME UBO blocks as
tfrag3 (`VertParams`/`FragParams`), so no C++ change is needed.

## Session rules (hard)

- Do NOT edit shader text by hand. Do NOT read deko3d headers, the plan, or notes.
- Run commands from the repo root, exactly as written.
- If any output differs from "Expected": STOP and paste it in the report.
- Report goes in QWEN_REPORT.md under `## T44`.

## Steps

1. Baseline md5 (BEFORE any edit):
   ```sh
   scripts/ai/check-bg.sh jak1
   ```
   repeat `scripts/ai/check-tail.sh` until `DONE-PASS`. Write down the md5 = **A**.

2. Convert (6 commands):
   ```sh
   S=game/graphics/opengl_renderer/shaders; D=game/graphics/deko3d_renderer/shaders
   for n in shrub tfrag3_no_tex tie_wind; do for s in vert frag; do python3 scripts/ai/pg2-convert-shader.py $S/$n.$s $s $D/$n.$s.glsl && echo "OK $n.$s"; done; done
   ```
   Expected: 6 lines `OK ...`.

3. In `game/CMakeLists.txt`, find the line `    add_dependencies(runtime dk_tfrag3)`
   and insert this block DIRECTLY AFTER it (verbatim, 4-space indent):
   ```cmake

    # PG.2 (AI-assisted, T44): jak1 background shaders for P3.3 (Tie3/Shrub).
    # Same uam + embed rule as tfrag3 above. Inert: headers included by nothing yet.
    set(_BG_SHADERS shrub.vert shrub.frag tfrag3_no_tex.vert tfrag3_no_tex.frag
                    tie_wind.vert tie_wind.frag)
    set(_BG_OUTS "")
    foreach(_sh ${_BG_SHADERS})
        string(REPLACE "." "_" _nm ${_sh})
        string(REGEX MATCH "[a-z]+$" _stage ${_sh})
        add_custom_command(
            OUTPUT ${_TF3_GEN}/${_sh}.dksh ${_TF3_GEN}/${_nm}_dksh.h
            COMMAND python3 ${_TF3_PY}
                    --input ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/${_sh}.glsl
                    --stage ${_stage} --output ${_TF3_GEN}/${_sh}.dksh
            COMMAND ${CMAKE_COMMAND} -DIN=${_TF3_GEN}/${_sh}.dksh
                    -DOUT=${_TF3_GEN}/${_nm}_dksh.h -DNAME=${_nm}_dksh
                    -P ${_TF3_EMBED}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/graphics/deko3d_renderer/shaders/${_sh}.glsl
                    ${_TF3_PY} ${_TF3_EMBED}
            COMMENT "uam ${_sh} (T44, AI-assisted)"
            VERBATIM)
        list(APPEND _BG_OUTS ${_TF3_GEN}/${_sh}.dksh ${_TF3_GEN}/${_nm}_dksh.h)
    endforeach()
    add_custom_target(dk_bg_shaders DEPENDS ${_BG_OUTS})
    add_dependencies(runtime dk_bg_shaders)
   ```

4. Build again:
   ```sh
   scripts/ai/check-bg.sh jak1
   ```
   repeat `scripts/ai/check-tail.sh` until `DONE-PASS`. md5 = **B**.
   Expected: **B == A** (shaders are compiled but no code uses them yet).

5. Prove the 6 shaders were compiled:
   ```sh
   ls build-switch-jak1/game/dksh_gen/dksh/ | grep -cE "^(shrub|tfrag3_no_tex|tie_wind)_(vert|frag)_dksh\.h$"
   ```
   Expected: `6`

6. Report: A, B, the `6`, `git status --short`. Set the README T44 row to 🟡.

## Doubts

- If step 4 fails with a CMake error, paste the first 20 lines of
  `build-switch-jak1/ai-check.log` errors. Do not change the block.
