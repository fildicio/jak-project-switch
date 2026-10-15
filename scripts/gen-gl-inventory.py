#!/usr/bin/env python3
"""OpenGL call-site inventory for the deko3d migration (DEKO3D_MIGRATION_PLAN.md, P0.2).

Counts every `glXxx(` call under game/graphics/ per file and per function, plus the
shader sources the UAM toolchain will have to compile. Re-run each phase to track the
port burn-down. (AI-assisted)

Usage:
  python3 scripts/gen-gl-inventory.py                # writes docs/deko3d/gl-inventory.{md,json}
  python3 scripts/gen-gl-inventory.py --stdout       # print markdown only
"""

import argparse
import json
import re
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC_ROOT = REPO / "game" / "graphics"
SHADER_ROOT = SRC_ROOT / "opengl_renderer" / "shaders"
OUT_DIR = REPO / "docs" / "deko3d"

# `glFoo(` but not `glm::`, `GLuint`, or our own `gl_helpers(`.
GL_CALL = re.compile(r"\bgl[A-Z]\w*(?=\s*\()")
LINE_COMMENT = re.compile(r"//.*")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
SRC_EXT = {".cpp", ".h", ".hpp", ".cc"}
SHADER_EXT = {".vert", ".frag", ".geom", ".comp", ".glsl"}


def strip_comments(text: str) -> str:
    # Keep newlines so nothing shifts if line numbers are ever added.
    text = BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT.sub("", text)


def scan_sources():
    per_file = {}
    per_func = Counter()
    for path in sorted(SRC_ROOT.rglob("*")):
        if path.suffix not in SRC_EXT or not path.is_file():
            continue
        calls = GL_CALL.findall(strip_comments(path.read_text(errors="replace")))
        if calls:
            per_file[str(path.relative_to(REPO))] = Counter(calls)
            per_func.update(calls)
    return per_file, per_func


def scan_shaders():
    if not SHADER_ROOT.exists():
        return []
    shaders = []
    for path in sorted(SHADER_ROOT.rglob("*")):
        if path.suffix not in SHADER_EXT or not path.is_file():
            continue
        text = path.read_text(errors="replace")
        code = strip_comments(text)
        version = re.search(r"#version\s+(\d+)\s*(\w*)", text)
        shaders.append({
            "file": str(path.relative_to(REPO)),
            "version": " ".join(version.groups()).strip() if version else "none",
            # Default (non-block) uniforms: UAM rejects these, each must move into a UBO.
            "default_uniforms": len(re.findall(r"^\s*uniform\s+(?!\w+\s*\{)", code, re.M)),
            "has_binding_layout": "binding" in code,
            "extensions": sorted(set(re.findall(r"#extension\s+(\w+)", code))),
        })
    return shaders


def build_markdown(per_file, per_func, shaders) -> str:
    total = sum(per_func.values())
    uniform_calls = sum(n for f, n in per_func.items() if f.startswith("glUniform"))
    out = [
        "# GL inventory (deko3d migration P0.2) — generated, do not edit (AI-assisted)",
        "",
        "Regenerate: `python3 scripts/gen-gl-inventory.py`",
        "",
        "## Summary",
        "",
        f"- GL call sites: **{total}** in **{len(per_file)}** files under `game/graphics/`",
        f"- Distinct GL functions: **{len(per_func)}**",
        f"- `glUniform*` call sites (→ UBO migration): **{uniform_calls}**",
        f"- Shader sources: **{len(shaders)}**, default uniforms to migrate: "
        f"**{sum(s['default_uniforms'] for s in shaders)}**, "
        f"already using `binding`: **{sum(s['has_binding_layout'] for s in shaders)}**",
        "",
        "## Per file (port checklist)",
        "",
        "| File | Calls | Distinct |",
        "|---|---:|---:|",
    ]
    for f, c in sorted(per_file.items(), key=lambda kv: -sum(kv[1].values())):
        out.append(f"| `{f}` | {sum(c.values())} | {len(c)} |")
    out += ["", "## Per function", "", "| Function | Calls |", "|---|---:|"]
    for fn, n in per_func.most_common():
        out.append(f"| `{fn}` | {n} |")
    out += ["", "## Shaders", "", "| File | #version | Default uniforms | binding? | Extensions |",
            "|---|---|---:|---|---|"]
    for s in shaders:
        out.append(f"| `{s['file']}` | {s['version']} | {s['default_uniforms']} | "
                   f"{'yes' if s['has_binding_layout'] else 'no'} | {', '.join(s['extensions'])} |")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stdout", action="store_true", help="print markdown instead of writing files")
    args = ap.parse_args()

    per_file, per_func = scan_sources()
    shaders = scan_shaders()
    md = build_markdown(per_file, per_func, shaders)
    if args.stdout:
        print(md, end="")
        return

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "gl-inventory.md").write_text(md)
    data = {
        "total_calls": sum(per_func.values()),
        "files": {f: dict(c.most_common()) for f, c in per_file.items()},
        "functions": dict(per_func.most_common()),
        "shaders": shaders,
    }
    (OUT_DIR / "gl-inventory.json").write_text(json.dumps(data, indent=2) + "\n")
    print(f"wrote {OUT_DIR.relative_to(REPO)}/gl-inventory.{{md,json}}: "
          f"{data['total_calls']} calls, {len(per_file)} files, {len(shaders)} shaders")


if __name__ == "__main__":
    main()
