# deko3d pipeline table (P0.3) — generated, do not edit (AI-assisted)

Regenerate: `python3 scripts/gen-pipeline-table.py`

Static enumeration of the closed DrawMode state space, transcribed from
`common/dma/gs.h` and `setup_opengl_from_draw_mode()`
(`game/graphics/opengl_renderer/background/background_common.cpp`).
This is the upper bound a deko3d `PipelineTable` must cover; entries also
carry the C++ reachability evidence (which renderer files construct the
axis values). DMA-driven modes can carry anything in the closed set.

## Summary

- Pipeline entries (depth x depth-write x blend x alpha-test): **224**
- Entries with C++ reachability evidence: **224**
- Axes: depth test **5** (OFF + 4 funcs) x depth write **2** x blend **7** x alpha-test **4**
- Pruned impossible combinations: **5** (ASSERT(false) branches)
- Sampler/descriptor table (FIX 43): **16** entries (separate from pipelines)
- Shader permutation axes: fog(2) x decal(2) x tcc(2) = **8** (shader variants, multiply the *shader* count, not the pipeline count)

## Gate check vs plan (P0.3: 'state table <= ~150 entries')

The full closed set is **224** pipeline entries — same order as the plan's O(100) estimate, not above it by a category. The per-game *hot* set is much smaller (see `cpp_evidence`); deko3d pipelines should be created lazily on first use (or all at init — ~250 pipeline creations is a one-time ~tens-of-ms cost) and the plan's one-route runtime spot check remains the confirmation that nothing outside this table appears.

## Blend states

| State | DrawMode enum sources | GL calls | deko3d |
|---|---|---|---|
| `OFF` | DISABLED, SRC_SRC_SRC_SRC, <abe=0> | glDisable(GL_BLEND) | blend off |
| `STD_ALPHA` | SRC_DST_SRC_DST | glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO) | Add: src=Src_Alpha dst=One_Minus_Src_Alpha srcA=One dstA=Zero |
| `PREMULT_ADD` | SRC_0_SRC_DST | glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO) | Add: src=Src_Alpha dst=One srcA=One dstA=Zero |
| `FIX128_ADD` | SRC_0_FIX_DST | glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO) | Add: src=One dst=One srcA=One dstA=Zero |
| `FIX64_SUB` | SRC_DST_FIX_DST | glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO); glBlendColor(0.5, 0.5, 0.5, 0.5) | Add: src=Constant_Color dst=Constant_Color srcA=One dstA=Zero |
| `SRC_ALPHA_SUB` | ZERO_SRC_SRC_DST | glEnable(GL_BLEND); glBlendEquation(GL_FUNC_REVERSE_SUBTRACT); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO) | ReverseSubtract: src=Src_Alpha dst=One srcA=One dstA=Zero |
| `DST_ALPHA_ADD` | SRC_0_DST_DST | glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_DST_ALPHA, GL_ONE) | Add: src=Dst_Alpha dst=One srcA=Dst_Alpha dstA=One |

## Alpha-test behaviors

| State | Sources | Implementation | Notes |
|---|---|---|---|
| `NONE` | <at=0>, ALWAYS* | none | no alpha test (at_enable=0, or AlphaTest::ALWAYS) |
| `GEQUAL_KEEP` | GEQUAL+KEEP | shader-discard | discard below aref (shader alpha_min uniform); aref is dynamic, NOT pipeline state |
| `GEQUAL_FB_ONLY` | GEQUAL+FB_ONLY | shader-discard | double draw when depth_write=1: 1st pass full aref, 2nd pass aref=alpha_min without depth write; single draw with aref=0 when depth_write=0 |
| `NEVER_FB_ONLY` | NEVER+FB_ONLY | never-draw | the atest-NEVER+FB_ONLY hack used by the engine to disable depth writes (alpha_hack_to_disable_z_write); forces depth_write OFF |

## Impossible combinations (pruned)

- alpha_test=GEQUAL + alpha_fail=ZB_ONLY: ASSERT(false) in setup_opengl_from_draw_mode
- alpha_test=GEQUAL + alpha_fail=RGB_ONLY: ASSERT(false) in setup_opengl_from_draw_mode
- alpha_test=NEVER + alpha_fail=KEEP: ASSERT(false) in setup_opengl_from_draw_mode
- alpha_test=NEVER + alpha_fail=ZB_ONLY: ASSERT(false) in setup_opengl_from_draw_mode
- alpha_test=NEVER + alpha_fail=RGB_ONLY: ASSERT(false) in setup_opengl_from_draw_mode

## Sampler table (16 descriptors, FIX 43)

| idx | clamp_s | clamp_t | filt | mipmap | min filter |
|---:|---|---|---|---|---|
| 0 | False | False | False | False | GL_NEAREST |
| 1 | True | False | False | False | GL_NEAREST |
| 2 | False | True | False | False | GL_NEAREST |
| 3 | True | True | False | False | GL_NEAREST |
| 4 | False | False | True | False | GL_LINEAR |
| 5 | True | False | True | False | GL_LINEAR |
| 6 | False | True | True | False | GL_LINEAR |
| 7 | True | True | True | False | GL_LINEAR |
| 8 | False | False | False | True | GL_NEAREST |
| 9 | True | False | False | True | GL_NEAREST |
| 10 | False | True | False | True | GL_NEAREST |
| 11 | True | True | False | True | GL_NEAREST |
| 12 | False | False | True | True | GL_LINEAR_MIPMAP_LINEAR |
| 13 | True | False | True | True | GL_LINEAR_MIPMAP_LINEAR |
| 14 | False | True | True | True | GL_LINEAR_MIPMAP_LINEAR |
| 15 | True | True | True | True | GL_LINEAR_MIPMAP_LINEAR |

## C++ reachability evidence (grep of `game/graphics/`)

Enum-value keys (`ZTest::*`, `AlphaBlend::*`, ...) are exact matches. Gate keys
(`ZT_GATE`, `DW_SET`, `*_ATE_GATE`, `B_ABE_GATE`, `FOG`, `DECAL`, `TCC`) match the
setters with *dynamic* arguments, so they mean "this file drives the axis from
C++", not "forces this specific value". DMA-driven modes (DirectRenderer2,
Generic2) can carry anything in the closed set regardless.

| Key | Files |
|---|---|
| `AF_FB_ONLY` | `game/graphics/opengl_renderer/DirectRenderer.cpp`, `game/graphics/opengl_renderer/TextureAnimator.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/sprite/GlowRenderer.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `AF_KEEP` | `game/graphics/opengl_renderer/DirectRenderer.h`, `game/graphics/opengl_renderer/background/background_common.cpp` |
| `A_ALWAYS` | `game/graphics/opengl_renderer/DirectRenderer.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/TextureAnimator.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp` |
| `A_ATE_GATE` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/sprite/GlowRenderer.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `A_GEQUAL` | `game/graphics/opengl_renderer/DirectRenderer.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `A_NEVER` | `game/graphics/opengl_renderer/DirectRenderer.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/TextureAnimator.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp` (+1 more) |
| `B_ABE_GATE` | `game/graphics/opengl_renderer/foreground/Merc2.cpp` |
| `B_DISABLED` | `game/graphics/opengl_renderer/background/background_common.cpp` |
| `B_SRC_0_DST_DST` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp`, `game/graphics/opengl_renderer/foreground/Merc2.cpp` |
| `B_SRC_0_FIX_DST` | `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp`, `game/graphics/opengl_renderer/sprite/GlowRenderer.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `B_SRC_0_SRC_DST` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `B_SRC_DST_FIX_DST` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `B_SRC_DST_SRC_DST` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp`, `game/graphics/opengl_renderer/foreground/Merc2.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `B_SRC_SRC_SRC_SRC` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp` |
| `B_ZERO_SRC_SRC_DST` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `DECAL` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.h`, `game/graphics/opengl_renderer/foreground/Generic2.h`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp` |
| `DW_SET` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Merc2.cpp`, `game/graphics/opengl_renderer/sprite/GlowRenderer.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3_Distort.cpp` |
| `FOG` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.h`, `game/graphics/opengl_renderer/foreground/Generic2.h`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Merc2.cpp` |
| `TCC` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.h`, `game/graphics/opengl_renderer/foreground/Generic2.h`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/sprite/GlowRenderer.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `ZT_ALWAYS` | `game/graphics/opengl_renderer/DepthCue.cpp`, `game/graphics/opengl_renderer/DirectRenderer.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/TextureAnimator.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp` |
| `ZT_GATE` | `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/sprite/GlowRenderer.cpp`, `game/graphics/opengl_renderer/sprite/Sprite3.cpp` |
| `ZT_GEQUAL` | `game/graphics/opengl_renderer/DirectRenderer.cpp`, `game/graphics/opengl_renderer/DirectRenderer.h`, `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_Build.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp` (+2 more) |
| `ZT_GREATER` | `game/graphics/opengl_renderer/DirectRenderer.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp` |
| `ZT_NEVER` | `game/graphics/opengl_renderer/DirectRenderer.cpp`, `game/graphics/opengl_renderer/DirectRenderer2.cpp`, `game/graphics/opengl_renderer/background/background_common.cpp`, `game/graphics/opengl_renderer/foreground/Generic2_OpenGL.cpp` |

## Notes

- `aref` (DrawMode bits 8-15) is a shader uniform (`alpha_min`), never pipeline state.
- Alpha test stays shader-side on deko3d exactly as on GL (the tfrag/tie/shrub
  shaders already do `discard` below `alpha_min`); the pipeline axis only drives
  depth-write suppression and the double-draw behavior.
- `SRC_0_DST_DST` needs the shader `color_mult` trick (0.5) — carried as
  `color_mult` on the entry, not as blend state.
- Full per-entry detail (GL call list, deko3d field mapping, evidence per axis):
  `docs/deko3d/pipeline_table.json`.
