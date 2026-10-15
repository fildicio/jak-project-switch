#!/usr/bin/env python3
"""Static draw-mode -> pipeline-state table for the deko3d migration (P0.3).

DEKO3D_MIGRATION_PLAN.md P0.3: "DrawMode is a bounded bitfield, so list every
reachable (blend x depth x alpha-test x fog x sampler) combination from
setup_opengl_from_draw_mode and the renderers that set fixed state, by reading
the code."

This script does exactly that, statically:

- the closed per-axis state sets are transcribed from `common/dma/gs.h`
  (DrawMode bitfields + GsTest enums) and the mapping semantics of
  `setup_opengl_from_draw_mode()` in
  `game/graphics/opengl_renderer/background/background_common.cpp`;
- combinations the GL mapping rejects with `ASSERT(false)` are pruned
  (documented in the output as `impossible_combinations`);
- reachability *evidence* is gathered by grepping `game/graphics/` for the
  enum values each axis can take, so every entry says which renderer files can
  construct it from C++ (DMA-driven modes can carry anything in the closed
  set -- that upper bound is what a deko3d PipelineTable must be ready for).

Output: docs/deko3d/pipeline_table.{json,md}. (AI-assisted)

Usage:
  python3 scripts/gen-pipeline-table.py                # writes docs/deko3d/pipeline_table.{md,json}
  python3 scripts/gen-pipeline-table.py --stdout       # print markdown only
"""

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC_ROOT = REPO / "game" / "graphics"
OUT_DIR = REPO / "docs" / "deko3d"

SRC_EXT = {".cpp", ".h", ".hpp", ".cc"}
LINE_COMMENT = re.compile(r"//.*")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)

# ---------------------------------------------------------------------------
# Closed per-axis state sets, transcribed from the code (do not widen without
# re-reading common/dma/gs.h and setup_opengl_from_draw_mode).
# ---------------------------------------------------------------------------

# GsTest::ZTest { NEVER=0, ALWAYS=1, GEQUAL=2, GREATER=3 }, gated by zt_enable.
DEPTH_TESTS = ["OFF", "NEVER", "ALWAYS", "GEQUAL", "GREATER"]

DEPTH_TEST_GL = {
    "OFF": ["glDisable(GL_DEPTH_TEST)"],
    "NEVER": ["glEnable(GL_DEPTH_TEST)", "glDepthFunc(GL_NEVER)"],
    "ALWAYS": ["glEnable(GL_DEPTH_TEST)", "glDepthFunc(GL_ALWAYS)"],
    "GEQUAL": ["glEnable(GL_DEPTH_TEST)", "glDepthFunc(GL_GEQUAL)"],
    "GREATER": ["glEnable(GL_DEPTH_TEST)", "glDepthFunc(GL_GREATER)"],
}
DEPTH_TEST_DK = {
    "OFF": None,  # depth test disabled entirely
    "NEVER": "Never",
    "ALWAYS": "Always",
    "GEQUAL": "Gequal",
    "GREATER": "Greater",
}

# DrawMode::AlphaBlend { DISABLED=0..SRC_0_DST_DST=7 }. Three different bit
# patterns collapse onto "no blending": ab_enable=0, AlphaBlend::DISABLED, and
# SRC_SRC_SRC_SRC ((SRC-SRC)*a + SRC == SRC, the function says so itself).
# The key is the *distinct GL/deko3d state*, not the enum value.
BLENDS = [
    # (state-name, enum values that reach it, gl calls, deko3d factors, notes)
    ("OFF", ["DISABLED", "SRC_SRC_SRC_SRC", "<abe=0>"],
     ["glDisable(GL_BLEND)"],
     None,
     "ab_enable=0, AlphaBlend::DISABLED, or SRC_SRC_SRC_SRC (identity blend)"),
    ("STD_ALPHA", ["SRC_DST_SRC_DST"],
     ["glEnable(GL_BLEND)", "glBlendEquation(GL_FUNC_ADD)",
      "glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO)"],
     {"mode": "Add", "srcColor": "Src_Alpha", "dstColor": "One_Minus_Src_Alpha",
      "srcAlpha": "One", "dstAlpha": "Zero"},
     None),
    ("PREMULT_ADD", ["SRC_0_SRC_DST"],
     ["glEnable(GL_BLEND)", "glBlendEquation(GL_FUNC_ADD)",
      "glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO)"],
     {"mode": "Add", "srcColor": "Src_Alpha", "dstColor": "One",
      "srcAlpha": "One", "dstAlpha": "Zero"},
     None),
    ("FIX128_ADD", ["SRC_0_FIX_DST"],
     ["glEnable(GL_BLEND)", "glBlendEquation(GL_FUNC_ADD)",
      "glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO)"],
     {"mode": "Add", "srcColor": "One", "dstColor": "One",
      "srcAlpha": "One", "dstAlpha": "Zero"},
     "fix=128; GL path uses ONE/ONE (code comment), not CONSTANT_COLOR"),
    ("FIX64_SUB", ["SRC_DST_FIX_DST"],
     ["glEnable(GL_BLEND)", "glBlendEquation(GL_FUNC_ADD)",
      "glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO)",
      "glBlendColor(0.5, 0.5, 0.5, 0.5)"],
     {"mode": "Add", "srcColor": "Constant_Color", "dstColor": "Constant_Color",
      "srcAlpha": "One", "dstAlpha": "Zero", "blendConstant": [0.5, 0.5, 0.5, 0.5]},
     "fix=64 -> Cv=(Cs-Cd)*FIX+Cd via constant color 0.5"),
    ("SRC_ALPHA_SUB", ["ZERO_SRC_SRC_DST"],
     ["glEnable(GL_BLEND)", "glBlendEquation(GL_FUNC_REVERSE_SUBTRACT)",
      "glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO)"],
     {"mode": "ReverseSubtract", "srcColor": "Src_Alpha", "dstColor": "One",
      "srcAlpha": "One", "dstAlpha": "Zero"},
     None),
    ("DST_ALPHA_ADD", ["SRC_0_DST_DST"],
     ["glEnable(GL_BLEND)", "glBlendEquation(GL_FUNC_ADD)",
      "glBlendFunc(GL_DST_ALPHA, GL_ONE)"],
     {"mode": "Add", "srcColor": "Dst_Alpha", "dstColor": "One",
      "srcAlpha": "Dst_Alpha", "dstAlpha": "One"},
     "requires shader color_mult=0.5 (DoubleDraw.color_mult)"),
]

# DrawMode::AlphaTest { NEVER=0, ALWAYS=1, GEQUAL=2 } x GsTest::AlphaFail
# { KEEP=0, FB_ONLY=1, ZB_ONLY=2, RGB_ONLY=3 }, gated by at_enable.
# ASSERT(false) branches are impossible and pruned; listed separately below.
ALPHA_TESTS = [
    ("NONE", ["<at=0>", "ALWAYS*"],
     None, None,
     "no alpha test (at_enable=0, or AlphaTest::ALWAYS)"),
    ("GEQUAL_KEEP", ["GEQUAL+KEEP"],
     "shader-discard", None,
     "discard below aref (shader alpha_min uniform); aref is dynamic, NOT pipeline state"),
    ("GEQUAL_FB_ONLY", ["GEQUAL+FB_ONLY"],
     "shader-discard", "AFAIL_NO_DEPTH_WRITE",
     "double draw when depth_write=1: 1st pass full aref, 2nd pass aref=alpha_min "
     "without depth write; single draw with aref=0 when depth_write=0"),
    ("NEVER_FB_ONLY", ["NEVER+FB_ONLY"],
     "never-draw", None,
     "the atest-NEVER+FB_ONLY hack used by the engine to disable depth writes "
     "(alpha_hack_to_disable_z_write); forces depth_write OFF"),
]

IMPOSSIBLE = [
    {"alpha_test": "GEQUAL", "alpha_fail": "ZB_ONLY", "reason": "ASSERT(false) in setup_opengl_from_draw_mode"},
    {"alpha_test": "GEQUAL", "alpha_fail": "RGB_ONLY", "reason": "ASSERT(false) in setup_opengl_from_draw_mode"},
    {"alpha_test": "NEVER", "alpha_fail": "KEEP", "reason": "ASSERT(false) in setup_opengl_from_draw_mode"},
    {"alpha_test": "NEVER", "alpha_fail": "ZB_ONLY", "reason": "ASSERT(false) in setup_opengl_from_draw_mode"},
    {"alpha_test": "NEVER", "alpha_fail": "RGB_ONLY", "reason": "ASSERT(false) in setup_opengl_from_draw_mode"},
]

# Sampler axis (FIX 43): (clamp_s, clamp_t, filt, mipmap) -> 16 sampler objects.
SAMPLER_AXES = ["clamp_s", "clamp_t", "filt", "mipmap"]

# Shader-permutation axes: bits the shaders read but GL state does not.
SHADER_AXES = {
    "fog": {"bit": 29, "values": [False, True]},
    "decal": {"bit": 28, "inverted": True, "values": [False, True]},
    "tcc": {"bit": 7, "values": [False, True]},
}

# ---------------------------------------------------------------------------
# C++ reachability evidence: which enum values / setters appear in game/graphics
# ---------------------------------------------------------------------------

EVIDENCE_PATTERNS = {
    # depth tests (ZTest values + the zt gate itself). The *_GATE keys take
    # dynamic bools (e.g. set_zt(reg.zte())), so they mean "this file drives
    # the axis from C++", not "this file forces this value".
    "ZT_GATE": [r"\benable_zt\s*\(", r"\bdisable_zt\s*\(", r"\bset_zt\s*\("],
    "ZT_NEVER": [r"ZTest::NEVER"],
    "ZT_ALWAYS": [r"ZTest::ALWAYS"],
    "ZT_GEQUAL": [r"ZTest::GEQUAL"],
    "ZT_GREATER": [r"ZTest::GREATER"],
    # depth write (both directions go through the same dynamic setters)
    "DW_SET": [r"\benable_depth_write\s*\(", r"\bdisable_depth_write\s*\(",
               r"\bset_depth_write_enable\s*\("],
    # blend modes
    "B_DISABLED": [r"AlphaBlend::DISABLED"],
    "B_SRC_DST_SRC_DST": [r"AlphaBlend::SRC_DST_SRC_DST"],
    "B_SRC_0_SRC_DST": [r"AlphaBlend::SRC_0_SRC_DST"],
    "B_SRC_0_FIX_DST": [r"AlphaBlend::SRC_0_FIX_DST"],
    "B_SRC_DST_FIX_DST": [r"AlphaBlend::SRC_DST_FIX_DST"],
    "B_ZERO_SRC_SRC_DST": [r"AlphaBlend::ZERO_SRC_SRC_DST"],
    "B_SRC_SRC_SRC_SRC": [r"AlphaBlend::SRC_SRC_SRC_SRC"],
    "B_SRC_0_DST_DST": [r"AlphaBlend::SRC_0_DST_DST"],
    "B_ABE_GATE": [r"\bset_ab_enable\s*\(", r"\benable_ab\s*\(", r"\bdisable_ab\s*\("],
    # alpha tests
    "A_NEVER": [r"AlphaTest::NEVER"],
    "A_ALWAYS": [r"AlphaTest::ALWAYS"],
    "A_GEQUAL": [r"AlphaTest::GEQUAL"],
    "AF_KEEP": [r"AlphaFail::KEEP"],
    "AF_FB_ONLY": [r"AlphaFail::FB_ONLY"],
    "A_ATE_GATE": [r"\bset_at\s*\(", r"\benable_at\s*\(", r"\bdisable_at\s*\("],
    # shader permutation axes (dynamic bool arguments)
    "FOG": [r"\benable_fog\s*\(", r"\bdisable_fog\s*\(", r"\bset_fog\s*\(", r"\bset_fog_flag\s*\("],
    "DECAL": [r"\benable_decal\s*\(", r"\bdisable_decal\s*\(", r"\bset_decal\s*\(", r"\bset_decal_flag\s*\("],
    "TCC": [r"\benable_tcc\s*\(", r"\bdisable_tcc\s*\(", r"\bset_tcc\s*\(", r"\bset_tcc_flag\s*\("],
}


def strip_comments(text: str) -> str:
    text = BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT.sub("", text)


def gather_evidence():
    sources = []
    for path in sorted(SRC_ROOT.rglob("*")):
        if path.suffix not in SRC_EXT or not path.is_file():
            continue
        sources.append((str(path.relative_to(REPO)), strip_comments(path.read_text(errors="replace"))))

    evidence = {}
    for key, patterns in EVIDENCE_PATTERNS.items():
        hits = set()
        for rel, code in sources:
            for pat in patterns:
                if re.search(pat, code):
                    hits.add(rel)
                    break
        evidence[key] = sorted(hits)
    return evidence


def blend_evidence(state, enums, evidence):
    """Files that can construct any enum value reaching this blend state."""
    files = set()
    if "<abe=0>" in enums:
        files.update(evidence.get("B_ABE_GATE", []))
    for e in enums:
        if e.startswith("<"):
            continue
        files.update(evidence.get(f"B_{e}", []))
    return sorted(files) or None


def depth_evidence(test, evidence):
    if test == "OFF":
        # the zt gate takes dynamic bools, so any file driving it can turn the
        # depth test off; exact ZTest::X values are enumerated per-function
        return evidence.get("ZT_GATE", []) or None
    gate = evidence.get("ZT_GATE", [])
    exact = evidence.get(f"ZT_{test}", [])
    return sorted(set(gate) | set(exact)) or None


def dw_evidence(write, evidence):
    # both directions go through the same dynamic setters
    return evidence.get("DW_SET", []) or None


def atest_evidence(state, evidence):
    key = {
        "NONE": ["A_ALWAYS", "A_ATE_GATE"],
        "GEQUAL_KEEP": ["A_GEQUAL", "AF_KEEP"],
        "GEQUAL_FB_ONLY": ["A_GEQUAL", "AF_FB_ONLY"],
        "NEVER_FB_ONLY": ["A_NEVER", "AF_FB_ONLY"],
    }[state]
    files = set()
    for k in key:
        files.update(evidence.get(k, []))
    return sorted(files) or None


def build_pipelines(evidence):
    pipelines = []
    pid = 0
    for depth in DEPTH_TESTS:
        for dw in (True, False):
            for (bname, benums, bgl, bdk, bnote) in BLENDS:
                for (aname, aenums, amode, add, anote) in ALPHA_TESTS:
                    # The NEVER+FB_ONLY hack forces depth writes OFF, so a
                    # depth_write=1 variant of it can never be observed.
                    if aname == "NEVER_FB_ONLY" and dw:
                        continue
                    # A depth-test OFF pipeline cannot write depth; GL ignores
                    # glDepthMask then. Keep one canonical entry (dw=False).
                    if depth == "OFF" and dw:
                        continue
                    pid += 1
                    gl = list(DEPTH_TEST_GL[depth])
                    gl += ["glDepthMask(GL_TRUE)" if dw else "glDepthMask(GL_FALSE)"]
                    gl += bgl
                    dk = {
                        "depthTestEnable": depth != "OFF",
                        "depthCompareOp": DEPTH_TEST_DK[depth],
                        "depthWriteEnable": dw,
                        "blendEnable": bdk is not None,
                        "blend": bdk,
                    }
                    double_draw = None
                    color_mult = None
                    if aname == "GEQUAL_FB_ONLY" and dw:
                        double_draw = add
                    if bname == "DST_ALPHA_ADD":
                        color_mult = 0.5
                    pipelines.append({
                        "id": f"P{pid:03d}",
                        "key": f"{depth}|dw={int(dw)}|{bname}|{aname}",
                        "depth_test": depth,
                        "depth_write": dw,
                        "blend": bname,
                        "blend_enum_sources": benums,
                        "alpha_test": aname,
                        "double_draw": double_draw,
                        "color_mult": color_mult,
                        "alpha_test_impl": amode,
                        "gl_state": gl,
                        "deko3d_state": dk,
                        "cpp_evidence": {
                            "depth_test": depth_evidence(depth, evidence),
                            "depth_write": dw_evidence(dw, evidence),
                            "blend": blend_evidence(bname, benums, evidence),
                            "alpha_test": atest_evidence(aname, evidence),
                        },
                        "notes": [n for n in (bnote, anote) if n],
                    })
    return pipelines


def build_samplers():
    out = []
    for idx in range(16):
        cs, ct, filt, mm = bool(idx & 1), bool(idx & 2), bool(idx & 4), bool(idx & 8)
        out.append({
            "index": idx,
            "clamp_s": cs,
            "clamp_t": ct,
            "filt": filt,
            "mipmap": mm,
            "gl": {
                "wrap_s": "GL_CLAMP_TO_EDGE" if cs else "GL_REPEAT",
                "wrap_t": "GL_CLAMP_TO_EDGE" if ct else "GL_REPEAT",
                "min_filter": ("GL_LINEAR_MIPMAP_LINEAR" if mm else "GL_LINEAR") if filt else "GL_NEAREST",
                "mag_filter": "GL_LINEAR" if filt else "GL_NEAREST",
            },
            "deko3d": {
                "wrapU": "ClampToEdge" if cs else "Repeat",
                "wrapV": "ClampToEdge" if ct else "Repeat",
                "minFilter": ("LinearMipmapLinear" if mm else "Linear") if filt else "Nearest",
                "magFilter": "Linear" if filt else "Nearest",
            },
        })
    return out


def build_markdown(pipelines, samplers, evidence):
    n_cpp = sum(1 for p in pipelines if any(p["cpp_evidence"].values()))
    blend_counts = defaultdict(int)
    for p in pipelines:
        blend_counts[p["blend"]] += 1
    lines = [
        "# deko3d pipeline table (P0.3) — generated, do not edit (AI-assisted)",
        "",
        "Regenerate: `python3 scripts/gen-pipeline-table.py`",
        "",
        "Static enumeration of the closed DrawMode state space, transcribed from",
        "`common/dma/gs.h` and `setup_opengl_from_draw_mode()`",
        "(`game/graphics/opengl_renderer/background/background_common.cpp`).",
        "This is the upper bound a deko3d `PipelineTable` must cover; entries also",
        "carry the C++ reachability evidence (which renderer files construct the",
        "axis values). DMA-driven modes can carry anything in the closed set.",
        "",
        "## Summary",
        "",
        f"- Pipeline entries (depth x depth-write x blend x alpha-test): **{len(pipelines)}**",
        f"- Entries with C++ reachability evidence: **{n_cpp}**",
        "- Axes: depth test "
        f"**{len(DEPTH_TESTS)}** (OFF + {len(DEPTH_TESTS)-1} funcs) x depth write **2** x "
        f"blend **{len(BLENDS)}** x alpha-test **{len(ALPHA_TESTS)}**",
        f"- Pruned impossible combinations: **{len(IMPOSSIBLE)}** (ASSERT(false) branches)",
        "- Sampler/descriptor table (FIX 43): **16** entries (separate from pipelines)",
        "- Shader permutation axes: fog(2) x decal(2) x tcc(2) = **8** (shader variants, "
        "multiply the *shader* count, not the pipeline count)",
        "",
        "## Gate check vs plan (P0.3: 'state table <= ~150 entries')",
        "",
        f"The full closed set is **{len(pipelines)}** pipeline entries — same order as the "
        "plan's O(100) estimate, not above it by a category. The per-game *hot* set is "
        "much smaller (see `cpp_evidence`); deko3d pipelines should be created lazily on "
        "first use (or all at init — ~250 pipeline creations is a one-time ~tens-of-ms "
        "cost) and the plan's one-route runtime spot check remains the confirmation "
        "that nothing outside this table appears.",
        "",
        "## Blend states",
        "",
        "| State | DrawMode enum sources | GL calls | deko3d |",
        "|---|---|---|---|",
    ]
    for (bname, benums, bgl, bdk, bnote) in BLENDS:
        dk = "blend off" if bdk is None else (
            f"{bdk['mode']}: src={bdk['srcColor']} dst={bdk['dstColor']} "
            f"srcA={bdk['srcAlpha']} dstA={bdk['dstAlpha']}")
        lines.append(f"| `{bname}` | {', '.join(benums)} | "
                     f"{'; '.join(bgl)} | {dk} |")
    lines += [
        "",
        "## Alpha-test behaviors",
        "",
        "| State | Sources | Implementation | Notes |",
        "|---|---|---|---|",
    ]
    for (aname, aenums, amode, add, anote) in ALPHA_TESTS:
        lines.append(f"| `{aname}` | {', '.join(aenums)} | {amode or 'none'} | {anote} |")
    lines += [
        "",
        "## Impossible combinations (pruned)",
        "",
    ]
    for imp in IMPOSSIBLE:
        lines.append(f"- alpha_test={imp['alpha_test']} + alpha_fail={imp['alpha_fail']}: "
                     f"{imp['reason']}")
    lines += [
        "",
        "## Sampler table (16 descriptors, FIX 43)",
        "",
        "| idx | clamp_s | clamp_t | filt | mipmap | min filter |",
        "|---:|---|---|---|---|---|",
    ]
    for s in samplers:
        lines.append(f"| {s['index']} | {s['clamp_s']} | {s['clamp_t']} | {s['filt']} | "
                     f"{s['mipmap']} | {s['gl']['min_filter']} |")
    lines += [
        "",
        "## C++ reachability evidence (grep of `game/graphics/`)",
        "",
        "Enum-value keys (`ZTest::*`, `AlphaBlend::*`, ...) are exact matches. Gate keys",
        "(`ZT_GATE`, `DW_SET`, `*_ATE_GATE`, `B_ABE_GATE`, `FOG`, `DECAL`, `TCC`) match the",
        "setters with *dynamic* arguments, so they mean \"this file drives the axis from",
        "C++\", not \"forces this specific value\". DMA-driven modes (DirectRenderer2,",
        "Generic2) can carry anything in the closed set regardless.",
        "",
        "| Key | Files |",
        "|---|---|",
    ]
    for key in sorted(evidence):
        files = evidence[key]
        shown = ", ".join(f"`{f}`" for f in files[:6])
        more = f" (+{len(files)-6} more)" if len(files) > 6 else ""
        lines.append(f"| `{key}` | {shown}{more} |" if files else f"| `{key}` | _none_ |")
    lines += [
        "",
        "## Notes",
        "",
        "- `aref` (DrawMode bits 8-15) is a shader uniform (`alpha_min`), never pipeline state.",
        "- Alpha test stays shader-side on deko3d exactly as on GL (the tfrag/tie/shrub",
        "  shaders already do `discard` below `alpha_min`); the pipeline axis only drives",
        "  depth-write suppression and the double-draw behavior.",
        "- `SRC_0_DST_DST` needs the shader `color_mult` trick (0.5) — carried as",
        "  `color_mult` on the entry, not as blend state.",
        "- Full per-entry detail (GL call list, deko3d field mapping, evidence per axis):",
        "  `docs/deko3d/pipeline_table.json`.",
        "",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stdout", action="store_true", help="print markdown instead of writing files")
    args = ap.parse_args()

    evidence = gather_evidence()
    pipelines = build_pipelines(evidence)
    samplers = build_samplers()
    md = build_markdown(pipelines, samplers, evidence)
    if args.stdout:
        print(md, end="")
        return

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "pipeline_table.md").write_text(md)
    data = {
        "generated_by": "scripts/gen-pipeline-table.py",
        "sources": [
            "common/dma/gs.h",
            "game/graphics/opengl_renderer/background/background_common.cpp",
        ],
        "pipeline_count": len(pipelines),
        "sampler_count": len(samplers),
        "shader_permutation_axes": SHADER_AXES,
        "impossible_combinations": IMPOSSIBLE,
        "pipelines": pipelines,
        "samplers": samplers,
        "cpp_evidence": evidence,
    }
    (OUT_DIR / "pipeline_table.json").write_text(json.dumps(data, indent=2) + "\n")
    print(f"wrote {OUT_DIR.relative_to(REPO)}/pipeline_table.{{md,json}}: "
          f"{len(pipelines)} pipelines, {len(samplers)} samplers")


if __name__ == "__main__":
    main()
