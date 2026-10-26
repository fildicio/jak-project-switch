# T103 — dkgl shader generator: 5 ocean programs → ES310 GLSL + std140 UBO tables

**Depends on:** —. **Category:** D dkgl (brief: `docs/deko3d/DKGL_HYBRID_BRIEF.md`, item 1).
**Size:** medium (1 session). **Law:** zero design freedom. The script below is pasted
verbatim; the GLSL outputs are GENERATED, never hand-edited. Anything not covered by a
table → STOP, paste the problem in the report, change nothing else.

## Goal

`scripts/ai/dkgl_gen_shaders.py` reads the 5 GL ocean shader pairs and emits dkgl-ready
GLSL ES 310 + a C++ uniform-offset table:

| GL source (read-only) | generated (commit) |
|---|---|
| `game/graphics/opengl_renderer/shaders/ocean_common.{vert,frag}` | `game/graphics/deko3d_renderer/shaders/dkgl/ocean_common.{vert,frag}.glsl` |
| `ocean_envmap.{vert,frag}` | `…/dkgl/ocean_envmap.{vert,frag}.glsl` |
| `ocean_envmap_haze.{vert,frag}` | `…/dkgl/ocean_envmap_haze.{vert,frag}.glsl` |
| `ocean_texture.{vert,frag}` | `…/dkgl/ocean_texture.{vert,frag}.glsl` |
| `ocean_texture_mipmap.{vert,frag}` | `…/dkgl/ocean_texture_mipmap.{vert,frag}.glsl` |

plus `game/graphics/deko3d_renderer/dkgl/dkgl_uniform_table.inc`.

Transformations (each mirrors existing code — cite in comments):
1. **jak1 substitutions** — exactly what `Shader::Shader` does (Shader.cpp:60-67):
   vert: `HEIGHT_SCALE`→`1.0`, `SCISSOR_HEIGHT`→`448.0`, `SCISSOR_ADJUST`→`(512.0 / 448.0)`;
   frag: `SCISSOR_HEIGHT`→`448.0`.
2. **Header swap** — `#version 410[^\r\n]*` → `#version 310 es\nprecision highp float;\nprecision highp int;`
   (same regex + replacement as Shader.cpp:88-103, the Switch GLES path).
3. **Uniform lift** — every `uniform <T> <name>;` with T ∈ {float,int,uint,unsigned int,vec2,vec3,vec4,mat4}
   is REMOVED and becomes a member of a stage block injected right after the precision header:
   `layout(std140, binding = 7) uniform VertParams { <members, original order/names/types> };`
   (frag: `FragParams`). Binding 7 in BOTH stages is intentional — deko3d binds uniforms
   per stage, so the same slot number is legal in vert and frag (tfrag3 uses binding 7 in
   both stages; see `game/graphics/deko3d_renderer/shaders/tfrag3.{vert,frag}.glsl`).
4. **std140 offsets (conservative, deterministic)** — every member occupies a 16-byte slot
   (mat4 = 4 slots = 64 B), offsets are multiples of 16, block size = sum of slots.
   Over-alignment is conformant std140 and keeps the host test trivial. Emit the offsets
   into the `.inc` table. If any block exceeds 256 → the script FAILS (the per-draw UBO
   ring slice is 0x100, recipe #3; `DK_UNIFORM_BUF_MAX_SIZE` is 0x10000).
5. **Sampler rewrite** — `uniform sampler2D tex_T<N>;` →
   `layout(binding = <N>) uniform highp sampler2D tex_T<N>;`
   (binding = texture unit = the digit suffix; `Shader` binds `tex_T<i>` to unit `i`,
   Shader.cpp:136-142). Any sampler NOT named `tex_T<0-7>` → script FAILS.
6. **No flip-Y.** The dk device is created with `DkDeviceFlags_OriginLowerLeft`
   (GL-style origin; grep `OriginLowerLeft` in `game/graphics/pipelines/dk_device.cpp`), so
   GL vertex math maps 1:1 and offscreen textures keep GL's row-0-at-bottom layout. The
   brief's "negate gl_Position.y" pitfall came from the other team's UpperLeft setup and
   DOES NOT apply here — recorded as a reviewer decision. Do NOT add a flip.

## Create 1 — `scripts/ai/dkgl_gen_shaders.py` (verbatim)

```python
#!/usr/bin/env python3
"""T103 (AI-assisted): dkgl ocean shader generator.

Transforms the GL ocean shaders for the dkgl layer (docs/deko3d/DKGL_HYBRID_BRIEF.md):
jak1 substitutions + GLSL ES header (both mirroring Shader.cpp), default uniforms lifted
into one std140 block per stage (binding 7), explicit sampler bindings, and a C++ table
name -> offset/size. Generated GLSL + .inc are committed; run with --check in the build
to fail on drift.
"""
import argparse, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "game/graphics/opengl_renderer/shaders"
OUT_GL = ROOT / "game/graphics/deko3d_renderer/shaders/dkgl"
OUT_INC = ROOT / "game/graphics/deko3d_renderer/dkgl/dkgl_uniform_table.inc"

PROGRAMS = ["ocean_common", "ocean_envmap", "ocean_envmap_haze",
            "ocean_texture", "ocean_texture_mipmap"]

HEIGHT_SCALE = "1.0"        # jak1 (Shader.cpp)
SCISSOR_HEIGHT = "448.0"    # jak1 (Shader.cpp)
VERSION_RE = re.compile(r"#version 410[^\r\n]*")
GLES_HEADER = "#version 310 es\nprecision highp float;\nprecision highp int;"
UNIFORM_RE = re.compile(
    r"^[ \t]*uniform[ \t]+(float|int|uint|unsigned int|vec2|vec3|vec4|mat4)[ \t]+"
    r"([A-Za-z_]\w*)[ \t]*;", re.M)
SAMPLER_RE = re.compile(
    r"^[ \t]*uniform[ \t]+sampler2D[ \t]+(tex_T(\d+))[ \t]*;", re.M)
# conservative std140: every member a 16-byte slot; mat4 four slots
SLOT = {"float": 16, "int": 16, "uint": 16, "unsigned int": 16,
        "vec2": 16, "vec3": 16, "vec4": 16, "mat4": 64}


def subst(src: str, stage: str) -> str:
    src = re.sub("SCISSOR_HEIGHT", SCISSOR_HEIGHT, src)
    if stage == "vert":
        src = re.sub("HEIGHT_SCALE", HEIGHT_SCALE, src)
        src = re.sub(r"SCISSOR_ADJUST", "(512.0 / 448.0)", src)
    return src


def transform(src: str, stage: str, prog: str):
    src = subst(src, stage)
    members = []  # (type, name)

    def lift(m):
        members.append((m.group(1), m.group(2)))
        return ""

    src = UNIFORM_RE.sub(lift, src)

    def sampler(m):
        n = int(m.group(2))
        if not 0 <= n <= 7:
            sys.exit(f"{prog}.{stage}: sampler {m.group(1)} binding {n} out of range")
        return f"layout(binding = {n}) uniform highp sampler2D {m.group(1)};"

    src = SAMPLER_RE.sub(sampler, src)
    block, table, off = [], [], 0
    for t, n in members:
        block.append(f"  {t} {n};")
        table.append((n, off, SLOT[t]))
        off += SLOT[t]
    if off > 256:
        sys.exit(f"{prog}.{stage}: block size {off} > 256")
    if VERSION_RE.search(src) is None:
        sys.exit(f"{prog}.{stage}: no #version 410 line")
    name = "VertParams" if stage == "vert" else "FragParams"
    if block:
        header = GLES_HEADER + "\nlayout(std140, binding = 7) uniform " + name + " {\n" \
                 + "\n".join(block) + "\n};"
    else:
        header = GLES_HEADER
    src = VERSION_RE.sub(lambda _: header, src, count=1)
    return src, table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="regenerate to memory and diff the committed files")
    args = ap.parse_args()
    inc = ["// GENERATED by scripts/ai/dkgl_gen_shaders.py (T103, AI-assisted) -- DO NOT EDIT.",
           "#pragma once", "", '#include "common/common_types.h"', "", "namespace dkgl {", "",
           "struct UniformSlot { const char* name; u32 offset; u32 size; };",
           "struct StageUniformTable { const UniformSlot* slots; u32 count; u32 block_size; };",
           "struct ProgramUniformTable { const char* program_name; StageUniformTable vert;"
           " StageUniformTable frag; };", ""]
    tables, progs = [], []
    for p in PROGRAMS:
        stage_inc = {}
        for stage in ("vert", "frag"):
            src = (SRC / f"{p}.{stage}").read_text(encoding="utf-8")
            out, table = transform(src, stage, p)
            dst = OUT_GL / f"{p}.{stage}.glsl"
            if args.check:
                have = dst.read_text(encoding="utf-8") if dst.exists() else ""
                if have != out:
                    sys.exit(f"drift: {dst} differs from generator output")
            else:
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_text(out, encoding="utf-8", newline="\n")
            sym = f"kSlots_{p}_{stage}"
            tables.append(f"inline constexpr UniformSlot {sym}[] = {{")
            rows = [f'    {{"{n}", {o}, {s}}},' for n, o, s in table]
            tables += rows if rows else ["    // empty stage"]
            tables.append("};")
            stage_inc[stage] = (f"    {{ {sym}, {len(table)}, "
                                f"{sum(s for _, _, s in table)} }},")
        progs.append(f'    {{ "{p}",')
        progs.append("      // vert")
        progs.append("      " + stage_inc["vert"])
        progs.append("      // frag")
        progs.append("      " + stage_inc["frag"] + " },")
    inc += tables + ["", "inline constexpr ProgramUniformTable kProgramUniformTables[] = {"]
    inc += progs + ["};", "", "}  // namespace dkgl", ""]
    inc_text = "\n".join(inc)
    if args.check:
        have = OUT_INC.read_text(encoding="utf-8") if OUT_INC.exists() else ""
        if have != inc_text:
            sys.exit(f"drift: {OUT_INC} differs from generator output")
        print("dkgl_gen_shaders: PASS (generated files up to date)")
        return
    OUT_INC.parent.mkdir(parents=True, exist_ok=True)
    OUT_INC.write_text(inc_text, encoding="utf-8", newline="\n")
    print(f"dkgl_gen_shaders: wrote {len(PROGRAMS)} programs to {OUT_GL} + {OUT_INC}")


if __name__ == "__main__":
    main()
```

Run it once (`python3 scripts/ai/dkgl_gen_shaders.py`), commit the 10 `.glsl` + the `.inc`.

## Edit 2 — `game/CMakeLists.txt` (dksh build)

In the Switch shader block (the one with the `dk_add_shaders(...)` splash-shader call,
line ~408, and the `_BG_SHADERS` list at ~464): append the 10 generated files to the SAME
list/flow the existing `tfrag3.*.glsl` / `_BG` shaders go through, so uam compiles them to
dksh and `dk_add_shaders` embeds byte arrays named after each file. Mirror the existing
loop verbatim; the entries are `dkgl/ocean_common.vert.glsl`, `dkgl/ocean_common.frag.glsl`,
`dkgl/ocean_envmap.vert.glsl`, `dkgl/ocean_envmap.frag.glsl`,
`dkgl/ocean_envmap_haze.vert.glsl`, `dkgl/ocean_envmap_haze.frag.glsl`,
`dkgl/ocean_texture.vert.glsl`, `dkgl/ocean_texture.frag.glsl`,
`dkgl/ocean_texture_mipmap.vert.glsl`, `dkgl/ocean_texture_mipmap.frag.glsl`
(paths relative to whatever base the other entries use). If the mechanism at lines 400-470
differs from this description → STOP, paste the block in the report.

## Create 3 — `test/test_dkgl_shader_gen.cpp` + `test/CMakeLists.txt` row

Host gtest, ~90 lines, no hardware. Include the generated
`game/graphics/deko3d_renderer/dkgl/dkgl_uniform_table.inc` (relative include path, same
style the game code uses) and read the generated GLSL files (build the path the way an
existing host test that reads repo files does — if none exists, STOP and say so).

Checks (plain asserts, no GLSL parser), for each of the 10 generated files:
1. First line is exactly `#version 310 es`; the string `#version 410` does NOT appear;
   `precision highp float;` appears exactly once.
2. Zero matches of regex `^[ \t]*uniform[ \t]+(float|int|uint|vec2|vec3|vec4|mat4)[ \t]`
   (no loose default uniforms left outside the blocks).
3. Every line containing `sampler2D` also contains `layout(binding =`.

Table checks:
4. `kProgramUniformTables` has 5 entries; names are exactly the 5 program names.
5. `ocean_common` frag: `count == 4`; slot names are the set
   {`color_mult`, `alpha_mult`, `fog_color`, `bucket`}; every offset % 16 == 0;
   `block_size <= 256`.
6. Every program: `vert.block_size + frag.block_size <= 256` (one UBO ring slice holds
   both stages, T106).

`test/CMakeLists.txt`: add `test_dkgl_shader_gen` exactly like the nearest existing host
gtest row.

## Checks

- `python3 scripts/ai/dkgl_gen_shaders.py --check` → PASS.
- Host build + `test_dkgl_shader_gen` PASS (existing host test flow).
- `python3 scripts/ai/check-dk.py` → PASS (the `.inc` has no dk names; run it anyway).
- The 10 `.dksh` compiles (uam) happen in the Switch build and are validated in T107 via
  `check-switch.sh`. Do NOT run check-switch for this task.

## Report

Report: files created (names + line counts), the exact `game/CMakeLists.txt` diff, gtest
output tail, check-dk output, any STOP conditions hit.
