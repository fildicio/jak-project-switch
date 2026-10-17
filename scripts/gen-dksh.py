#!/usr/bin/env python3
"""scripts/gen-dksh.py -- compile a GLSL shader to a deko3d .dksh via UAM.

T25 (AI-assisted). This is the PG.2 UAM vehicle: every P3.2+ renderer will
compile its shaders with this script instead of going through the runtime
Shader.cpp path. It mirrors the preprocessing Shader.cpp does at shader-load
time (jak1 values -- see game/graphics/opengl_renderer/Shader.cpp:56-90),
then hands the result to UAM.

Before invoking UAM it applies, in order, the same substitutions Shader.cpp
makes (so a raw repo shader compiles the same way the game runs it):
  1. HEIGHT_SCALE      -> 1.0            (jak1; jak2/jak3 -> 0.5)
  2. SCISSOR_HEIGHT    -> 448.0          (jak1; jak2/jak3 -> 416.0)
  3. SCISSOR_ADJUST    -> (512.0/448.0)  (jak1; jak2/jak3 -> (512.0/416.0))
  4. "#version 410<...>" -> "#version 310 es\nprecision highp float;\n
                            precision highp int;"  (the Switch GLES swap,
                            Shader.cpp:85-89).

The tfrag3 spike inputs in game/.../shaders/ are *already* preprocessed
(their #version line is already "310 es" and no HEIGHT_SCALE/SCISSOR_* token
remains), so for them these substitutions are a clean no-op: UAM produces a
byte-identical .dksh to the recorded spike artifacts
(docs/deko3d/uam-spike/tfrag3.{vert,frag}.dksh).

If UAM is missing it prints one line and exits non-zero (no silent fallback):
    [dk] uam not found at <path>

Usage:
    gen-dksh.py --input X.glsl --stage vert|frag --output X.dksh [--uam PATH]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

# jak1 values (Shader.cpp:56-58). jak2/jak3 would use 0.5 / 416.0 / (512.0/416.0).
HEIGHT_SCALE = "1.0"
SCISSOR_HEIGHT = "448.0"
SCISSOR_ADJUST = "(512.0/448.0)"

# Shader.cpp:85-87 -- the #version 410 line (incl. any trailing " core\r") swapped
# for the GLSL-ES header. [^\r\n]* mirrors the C++ "[^\r\n]*" tail-consume.
VERSION_410_RE = re.compile(r"#version 410[^\r\n]*")
GLES_HEADER = "#version 310 es\nprecision highp float;\nprecision highp int;"


def preprocess(src):
    """Apply the four Shader.cpp substitutions (order matches Shader.cpp).

    A lambda is used as the replacement so re.sub never interprets backrefs /
    backslashes in the replacement text -- the literal string is substituted verbatim.
    """

    def _lit(text):
        return lambda _m: text

    src = re.sub(r"HEIGHT_SCALE", _lit(HEIGHT_SCALE), src)
    src = re.sub(r"SCISSOR_HEIGHT", _lit(SCISSOR_HEIGHT), src)
    src = re.sub(r"SCISSOR_ADJUST", _lit(SCISSOR_ADJUST), src)
    src = VERSION_410_RE.sub(_lit(GLES_HEADER), src)
    return src


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compile a GLSL shader to a deko3d .dksh via UAM (T25, AI-assisted)."
    )
    parser.add_argument("--input", required=True, help="path to the .glsl shader source")
    parser.add_argument(
        "--stage", required=True, choices=["vert", "frag"], help="UAM pipeline stage"
    )
    parser.add_argument("--output", required=True, help="path to write the compiled .dksh")
    parser.add_argument(
        "--uam",
        default="/opt/devkitpro/tools/bin/uam",
        help="path to the uam binary (devkitpro image default)",
    )
    args = parser.parse_args(argv)

    # Hard requirement: uam must exist. Never fall back / never guess another path.
    if not os.path.isfile(args.uam):
        print("[dk] uam not found at " + args.uam, file=sys.stderr)
        return 1

    with open(args.input, "r", encoding="utf-8") as f:
        src = f.read()
    pre = preprocess(src)

    # Write the preprocessed source to a temp file, then have UAM compile it.
    tmp = None
    try:
        with tempfile.NamedTemporaryFile(
            "w", suffix=".glsl", delete=False, encoding="utf-8"
        ) as f:
            f.write(pre)
            tmp = f.name
        # uam writes the .dksh to --output; its exit code is the result.
        return subprocess.call(
            [args.uam, "--stage", args.stage, "-o", args.output, tmp]
        )
    finally:
        if tmp and os.path.exists(tmp):
            os.unlink(tmp)


if __name__ == "__main__":
    sys.exit(main())
