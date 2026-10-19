#!/usr/bin/env python3
# pg2-convert-shader.py (AI-assisted): GL shader -> deko3d std140 .glsl, the exact
# edits made by hand for tfrag3 (reproduces shaders/tfrag3.*.glsl byte-for-byte).
# Usage: pg2-convert-shader.py IN.vert|IN.frag vert|frag OUT.glsl
import re, sys
VBLOCK = """layout(std140, binding = 7) uniform VertParams {
  vec4 hvdf_offset;
  vec4 cam_trans;
  mat4 pc_camera;
  mat4 camera;
  float fog_constant;
  float fog_min;
  float fog_max;
  int decal;
};
"""
FBLOCK = """layout(std140, binding = 7) uniform FragParams {
  float alpha_min;
  float alpha_max;
  vec4 fog_color;
  int gfx_hack_no_tex;
};
"""
def conv(src, stage):
    block = VBLOCK if stage == "vert" else FBLOCK
    names = re.findall(r"\b(\w+);", block)
    lines = src.split("\n")
    assert lines[0].startswith("#version 410"), lines[0]
    out = ["#version 310 es", "precision highp float;", "precision highp int;"]
    rest = lines[1:]
    if rest[0].strip() != "":
        rest = [""] + rest  # no blank line after #version: add one (layout unchanged otherwise)
    out += [rest[0]] + block.rstrip("\n").split("\n") + ["", ""]
    for l in rest[1:]:
        m = re.match(r"\s*uniform\s+(\w+)\s+(\w+)\s*;", l)
        if m and m.group(1) != "sampler2D":
            assert m.group(2) in names, "uniform not in block: " + l
            continue
        if m:
            l = "layout(binding = 0) " + l.strip()
        # same token swaps as gen-dksh.py / Shader.cpp (jak1 values)
        l = l.replace("SCISSOR_ADJUST", "(512.0/448.0)").replace("SCISSOR_HEIGHT", "448.0")
        l = l.replace("HEIGHT_SCALE", "1.0")
        out.append(l)
    res = "\n".join(out)
    assert not re.search(r"^\s*uniform\s+(?!sampler2D)", res, re.M)
    assert "SCISSOR" not in res and "HEIGHT_SCALE" not in res
    return res
def survey(paths):
    """--survey: one line per shader, never writes files (T45)."""
    import os
    vn = re.findall(r"\b(\w+);", VBLOCK); fn = re.findall(r"\b(\w+);", FBLOCK)
    for p in sorted(paths):
        name = os.path.basename(p); stage = "vert" if p.endswith(".vert") else "frag"
        src = open(p).read()
        uni = re.findall(r"^\s*uniform\s+(\w+)\s+(\w+)\s*(?:\[[^\]]*\])?\s*;", src, re.M)
        miss = [n for t, n in uni if t != "sampler2D" and n not in (vn if stage == "vert" else fn)]
        blocks = re.findall(r"^\s*(?:layout\s*\([^)]*\)\s*)?uniform\s+\w+\s*\{", src, re.M)
        tag = "OK" if not miss and not blocks and src.startswith("#version 410") else "NEEDS-BLOCK"
        if not src.startswith("#version 410"): tag = "ODD-VERSION"
        print(f"{tag:12} {name:32} missing={' '.join(miss) or '-'}{' has-uniform-block' if blocks else ''}")
if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "--survey":
    survey(sys.argv[2:]); sys.exit(0)
if __name__ == "__main__":
    src, stage, dst = sys.argv[1:4]
    res = conv(open(src).read(), stage)  # convert first: never leave an empty file on failure
    open(dst, "w").write(res)
