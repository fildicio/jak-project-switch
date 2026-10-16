# P0.4 (partial): UAM toolchain spike — real repo shader compiles to .dksh (2026-10-15)

Environment: local Docker image `devkitpro/devkita64:latest` (already on this Mac;
no local `/opt/devkitpro` needed). UAM at `/opt/devkitpro/tools/bin/uam` inside the
image. Repo mounted at `/work` (required — `build-switch/CMakeCache.txt` was created
there; a host-path mount makes CMake abort).

## Result

`tfrag3.vert` + `tfrag3.frag` — the core world-terrain shader — compile to `.dksh`
offline after exactly three mechanical diffs. Artifacts: `tfrag3.{vert,frag}.glsl`
(preprocessed inputs, kept here as the record) and `tfrag3.{vert,frag}.dksh`.

## The diffs UAM demands (all three confirmed by compile errors, in order)

1. **Stage flag**: `--stage vert|frag` — Vulkan-style names; `vertex`/`fragment`
   are rejected ("Unrecognized pipeline stage").
2. **Explicit sampler bindings** on *every* sampler, **including vertex-shader
   samplers** (`tfrag3.vert` has `tex_T10`, the time-of-day LUT):
   `layout(binding = N) uniform sampler2D tex_T0;`
   deko3d has 10 sampler slots (0-9) — enough for every repo shader surveyed so far.
3. **No loose uniforms** — the biggest one. Every plain `uniform` is rejected with
   `uniform 'X' in driver constbuf ... not supported`. All uniforms must move into
   std140 UBOs: `layout(std140, binding = N) uniform VertParams { ... };`
   This makes the plan's **PG.2 UBO migration a hard prerequisite** for P1, not an
   optimization. It was already scheduled (R2 mitigation); this spike proves it.

## Preprocessing that must be replicated offline (from `Shader.cpp`)

The runtime regex-replaces tokens before compiling; offline compilation must apply
the same substitutions (jak1 values shown; jak2/3 use the second column):

| Token | jak1 | jak2/jak3 |
|---|---|---|
| `HEIGHT_SCALE` | `1.0` | `0.5` |
| `SCISSOR_HEIGHT` | `448.0` | `416.0` |
| `SCISSOR_ADJUST` | `(512.0/448.0)` | `(512.0/416.0)` |

On Switch the `#version 410 core` line is swapped for `#version 310 es` +
`precision highp float; precision highp int;` — same swap works for UAM.

tfrag3 UBO contents produced by the spike (binding 7, per stage):
- vert: `hvdf_offset, cam_trans, pc_camera (mat4), camera (mat4), fog_constant, fog_min, fog_max, decal (int)`
- frag: `alpha_min, alpha_max, fog_color (vec4), gfx_hack_no_tex (int)`

## Still open in P0.4 (needs more than this Mac)

- `switch-deko3d` package is **not** in the image (no `libdeko3d/`, no
  `include/deko3d.h`) → install via `dkp-pacman` inside the container, then:
- triangle NRO that initializes a deko3d device and clears — **boots on hardware**
  (the gate's remaining item) — plus a photo/log as evidence.
