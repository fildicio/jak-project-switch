# T25 — UAM shader build rule: tfrag3 .glsl → .dksh in-CMake (P3.0, executes PG.2's vehicle)

First P3 task (after T24 ✅ + P2.3 soak). The uam-spike proved the compile
(docs/deko3d/uam-spike/README.md); this task makes it a build rule so every P3.2+
renderer compiles its shaders the same way. **Inert**: the generated .dksh bytes are
embedded but referenced by nothing yet.

## Verified anchors (do not re-grep unless something fails)
- UAM binary inside the build image: `/opt/devkitpro/tools/bin/uam` (spike README L4).
  Stage flag is `--stage vert|frag` (NOT vertex/fragment — rejected).
- Token substitutions (jak1): `HEIGHT_SCALE`→`1.0`, `SCISSOR_HEIGHT`→`448.0`,
  `SCISSOR_ADJUST`→`(512.0/448.0)`; `#version 410 core`→`#version 310 es` +
  `precision highp float; precision highp int;` (spike README L33–40).
- Existing shader embed pattern: `game/CMakeLists.txt:383–384` lists
  `graphics/deko3d_renderer/shaders/splash.{vert,frag}` — READ that whole block
  (≈L360–400) and extend the same pattern; dk_state.cpp entered at :101.
- Reference outputs: `docs/deko3d/uam-spike/tfrag3.vert.dksh` + `.frag.dksh`
  (and the preprocessed `.glsl` inputs beside them).

## Rules
- STOP rule as always (last 5 lines of any failure, no self-fixing).
- Only the Switch build config gets the rule (host builds must not invoke uam).

## Edits
1. Copy the spike inputs to the shaders dir: `game/graphics/deko3d_renderer/shaders/
   tfrag3.vert.glsl` + `tfrag3.frag.glsl` (from `docs/deko3d/uam-spike/`).
2. NEW `scripts/gen-dksh.py`: args `--input X.glsl --stage vert|frag --output X.dksh
   [--uam PATH]` (default `/opt/devkitpro/tools/bin/uam`). It applies the token
   substitutions + version swap to a temp file, runs uam, writes the .dksh. If uam is
   missing, print one `[dk] uam not found at PATH` line and exit non-zero.
3. `game/CMakeLists.txt`: extend the splash shader block with two custom commands
   (vert + frag) calling `python3 scripts/gen-dksh.py` into
   `${CMAKE_CURRENT_BINARY_DIR}/.../tfrag3.{vert,frag}.dksh` and generating C headers
   with the bytes (`tfrag3_vert_dksh.h` / `tfrag3_frag_dksh.h`, same embed style the
   splash block uses). Wire like dk_state.cpp:101 (Switch-only source group).

## Done-check (background wrappers only)
`scripts/ai/check-bg.sh jak1` + `scripts/ai/check-tail.sh` → DONE-PASS.
**Determinism gate:** `md5 build-switch-jak1/.../tfrag3.vert.dksh` and `...frag.dksh`
must EQUAL the md5s of the spike's recorded artifacts — paste both pairs in the report.
If they differ, STOP and report (do not tweak tokens to force a match).
Inert grep: no includes of the new headers outside CMake-generated land.
QWEN_REPORT `## T25` (files + md5 pairs + last 5 log lines). README row → 🟡.
No commit, no deploy.
