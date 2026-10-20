#!/usr/bin/env python3
"""deko3d lint for the Switch port (AI-assisted).

Catches what the compiler can't:
  1. NAMES   every dkXxx / DkXxx / DK_XXX identifier must exist in the vendored deko3d 0.5.0
             headers (docs/deko3d-ref/include). Catches invented API names in code AND docs.
  2. PITFALL known silent-GPU-bug patterns (see .clinerules/deko3d-recipes.md "Pitfalls").

Usage:
  python3 scripts/ai/check-dk.py                 # files changed vs HEAD (+ untracked) under game/ .clinerules/ docs/deko3d/
  python3 scripts/ai/check-dk.py FILE [FILE...]  # explicit files
Exit 0 = "dk-lint: PASS", 1 = "dk-lint: FAIL" (errors). Warnings never fail.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HDRS = [os.path.join(ROOT, "docs/deko3d-ref/include", h) for h in ("deko3d.h", "deko3d.hpp")]
NAME_RE = re.compile(r"\b(dk[A-Z]\w*|Dk[A-Z]\w*|DK_[A-Z0-9_]+)\b")
SCAN_DIRS = ("game/", ".clinerules/", "docs/deko3d/")
SCAN_EXT = (".cpp", ".h", ".hpp", ".md")
# History logs quote wrong names on purpose (what was tried and caught): not scanned by default.
LOG_FILES = ("QWEN_REPORT.md", "qwen-tasks/README.md")
# deko3d code in this repo: GL calls are forbidden here (AI_ROLES.md review rule).
DK_CODE_RE = re.compile(r"(deko3d_renderer/|pipelines/dk_)")


PROJ_DEF_RE = re.compile(
    r"(?:\b(?:struct|class|enum(?:\s+class)?|using|typedef)\s+|#define\s+)(Dk\w+|dk\w+|DK_\w+)")


def known_names():
    names = set()
    for h in HDRS:
        with open(h, encoding="utf-8", errors="replace") as f:
            names.update(NAME_RE.findall(f.read()))
    return names


def project_names():
    """Dk-prefixed types this repo defines itself (DkSyncData, DkDisplay, DkStates...)."""
    names = set()
    for d, _, fs in os.walk(os.path.join(ROOT, "game")):
        for fn in fs:
            if fn.endswith((".h", ".cpp")):
                with open(os.path.join(d, fn), encoding="utf-8", errors="replace") as f:
                    names.update(PROJ_DEF_RE.findall(f.read()))
    # gtest suites and env vars defined by the host tests (TEST(DkRing, ...), getenv("DK_...")).
    for fn in os.listdir(os.path.join(ROOT, "test")):
        if fn.endswith(".cpp"):
            with open(os.path.join(ROOT, "test", fn), encoding="utf-8", errors="replace") as f:
                t = f.read()
            names.update(re.findall(r"\bTEST(?:_F)?\((Dk\w+),", t))
            names.update(re.findall(r"getenv\(\"(DK_\w+)\"\)", t))
    # Build macros defined in CMake (e.g. target_compile_definitions(... DK_HOST_REPLAY=1)).
    for d, _, fs in os.walk(ROOT):
        if os.path.relpath(d, ROOT).startswith(("build", "third-party", ".git", "out")):
            continue
        if "CMakeLists.txt" in fs:
            with open(os.path.join(d, "CMakeLists.txt"), encoding="utf-8", errors="replace") as f:
                names.update(re.findall(r"\b(DK_[A-Z0-9_]+)=", f.read()))
    return names


def name_ok(n, names, proj):
    if n in names or n in proj or "Xxx" in n:
        return True
    if n.endswith("_") and any(k.startswith(n) for k in names):
        return True  # enum family prefix, e.g. "DkBlendFactor_*"
    return n.endswith("s") and (n[:-1] in names or n[:-1] in proj)  # plural in prose


def changed_files():
    out = subprocess.run(["git", "-C", ROOT, "diff", "--name-only", "HEAD"],
                         capture_output=True, text=True).stdout.split()
    out += subprocess.run(["git", "-C", ROOT, "ls-files", "--others", "--exclude-standard"],
                          capture_output=True, text=True).stdout.split()
    return sorted({f for f in out if f.startswith(SCAN_DIRS) and f.endswith(SCAN_EXT)
                   and "deko3d-ref" not in f and not f.endswith(LOG_FILES) and os.path.exists(os.path.join(ROOT, f))})


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def lint(path, names, proj, errors, warnings):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
        raw = f.read()
    lines = raw.split("\n")
    for i, line in enumerate(lines, 1):
        if "dk-lint: ok" in line:
            continue
        for n in NAME_RE.findall(line):
            if not name_ok(n, names, proj):
                errors.append(f"{path}:{i}: NAME '{n}' is not in deko3d.h/deko3d.hpp")
    if path.endswith(".md") or not DK_CODE_RE.search(path):
        return
    code = strip_comments(raw)
    clines = code.split("\n")
    for i, line in enumerate(clines, 1):
        if re.search(r"\bgl[A-Z]\w*\s*\(", line):
            errors.append(f"{path}:{i}: PITFALL GL call in deko3d code")
        if re.search(r"std::thread\b(?!::)", line):
            warnings.append(f"{path}:{i}: PITFALL new std::thread (runtime threads need reviewer OK)")
    if "dkCmdBufBlitImage" in code and "dkCmdBufBarrier" not in code:
        warnings.append(f"{path}: PITFALL BlitImage (2D engine) without any dkCmdBufBarrier "
                        "(3D clear/draw -> 2D blit needs a barrier)")
    if "dkCmdBufClear" in code and not re.search(r"dkFenceWait|dkQueueWaitIdle|submit_upload_cmds", code):
        warnings.append(f"{path}: PITFALL dkCmdBufClear but no dkFenceWait/dkQueueWaitIdle "
                        "(cmd memory may still be read by the GPU)")
    if "DkMemBlockMaker" in code and not re.search(r"DK_MEMBLOCK_ALIGNMENT|0x1000|0xFFF", code):
        warnings.append(f"{path}: PITFALL DkMemBlockMaker without DK_MEMBLOCK_ALIGNMENT "
                        "(sizes must be multiples of 0x1000)")
    if "dkCmdBufBindUniformBuffer" in code and "0x100" not in code \
            and "DK_UNIFORM_BUF_ALIGNMENT" not in code:
        warnings.append(f"{path}: PITFALL UBO binds but no 0x100/DK_UNIFORM_BUF_ALIGNMENT "
                        "alignment anywhere (UBO addresses must be 256-byte aligned)")


def main():
    files = sys.argv[1:] or changed_files()
    names = known_names()
    proj = project_names()
    errors, warnings = [], []
    for p in files:
        lint(os.path.relpath(os.path.abspath(p), ROOT) if os.path.exists(p) else p,
             names, proj, errors, warnings)
    for w in warnings:
        print("warning:", w)
    for e in errors:
        print("error:", e)
    print(f"dk-lint: {'FAIL' if errors else 'PASS'} ({len(files)} files, "
          f"{len(errors)} errors, {len(warnings)} warnings)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
