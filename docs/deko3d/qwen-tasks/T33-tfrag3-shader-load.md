# T33 — tfrag3 shader pair: embed + load into the code memblock (P3.2, AI-assisted)

Track D P3.2 · Depends on: T25 ✅ (build rule) + T24 ✅ · Inert until T39 · One session

## Goal

Load the T25-built tfrag3 .dksh pair as `DkShader` objects, exactly the way the
splash pair is loaded (T05), into the EXISTING 64 KB code memblock — appended
0x100-aligned after the splash shaders. Expose the handles through
`dk_device_internal.h` so the T35/T38 TUs can bind them without touching
dk_device.cpp again.

## Context

- `game/graphics/pipelines/dk_device.cpp` — the T05 pattern: `s_codeMem`
  (kCodePool = 64 KB, L75), `dkShaderMakerDefaults` + `dkShaderInitialize`
  (grep `s_splashVsh` for the exact init code), `#include "dksh/splash_vert.h"`
  style embedding (L22-23).
- The generated headers: `tfrag3_vert_dksh.h` / `tfrag3_frag_dksh.h` in
  `${_TF3_GEN}` (game/CMakeLists.txt:406, 420). FIRST verify the include path
  actually used by the splash headers:
  `find build-switch-jak1 -name 'tfrag3_*_dksh.h' -o -name 'splash_vert.h'`
  and match the splash include style exactly (`dksh/…` or bare name). If the
  tfrag3 headers are NOT next to the splash ones, say so and stop.
- `game/graphics/deko3d_renderer/shaders/tfrag3.vert.glsl` — the source you are
  loading; note `layout(std140, binding = 7)` VertParams and vertex-shader
  sampler `tex_T10` (binding 0): T38 must bind a texture to the VERTEX stage too.

## API cheat-sheet (verified)

- `dkShaderMakerDefaults(&maker, codeMem, codeOffset)` then `dkShaderInitialize`
  (deko3d.h:1335; struct at L293: codeMem, control=NULL, codeOffset, programId=0).
- `DK_SHADER_CODE_ALIGNMENT` = 0x100 (grep to confirm) — tfrag3 code goes at
  `align_up(splash_code_end, DK_SHADER_CODE_ALIGNMENT)`. The .dksh header struct
  in the embedded headers exposes the code byte array + size — mirror how
  dk_device.cpp slices the splash ones (grep `splash_vert` usage in the file).
- Bind (NOT this task — T38): `dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask,
  shaders, 2)` (deko3d.h:1260, used at dk_device.cpp:202-203).

## Steps

1. In dk_device.cpp: include the two generated headers; add `DkShader
   s_tfrag3Vsh, s_tfrag3Fsh;` + `bool s_have_tfrag3 = false;` near the splash
   shader state (L108-112 region).
2. After the splash shader init succeeds, append both tfrag3 shaders into
   `s_codeMem` at 0x100-aligned offsets. Size check: splash pair + tfrag3 pair
   must fit kCodePool; if not, one `[dk] tfrag3 shader code overflow (N/64 KB)`
   line and skip (device stays up, splash unaffected) — do NOT grow the pool
   silently.
3. Expose in `dk_device_internal.h` (T22a pattern): `DkShader tfrag3_vsh();
   DkShader tfrag3_fsh();` + `bool tfrag3_shaders_loaded();` — implement as
   one-liners in dk_device.cpp's internals section (L640+).
4. `shutdown()`: nothing new to destroy (DkShader has no destroy; the memblock
   already goes in the teardown list) — but re-zero `s_have_tfrag3`.

## Rules

- Only `dk_device.cpp` + `dk_device_internal.h`.
- Check the memblock arithmetic with a `static_assert`-style comment (sizes are
  runtime, so an explicit bounds check + `[dk]` log instead).
- Every dkXxx result that CAN fail, checked. Never crash.

## Done-check

```sh
scripts/ai/check-bg.sh jak1 && scripts/ai/check-tail.sh
```

DONE-PASS + NRO md5 unchanged (`08fc9510…`): the load code RUNS at boot but
produces no new GPU work, and the new getters are uncalled → expect IDENTICAL
md5 only if gc-sections strips the getters; if md5 changes, verify the diff is
only the added init code (code bytes copied into a memblock, no NRO-visible
behavior change) and say so explicitly in the report.

## Doubts

- If the embedded tfrag3 headers declare different symbol names than the splash
  ones (they come from the same EmbedBinary.cmake rule, so they should match),
  quote both in your report before adapting.
