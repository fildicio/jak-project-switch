#!/usr/bin/env python3
"""Summarize [glt] lines from a Switch gk_run_log.txt (deko3d migration P0.5). (AI-assisted)

The raw [glt] "render" number includes the FIX 13 deferred window glClear,
which is where the EGL swapchain acquire blocks. Under vsync that clear soaks
up whatever is left of the vblank interval (run 3, 2026-10-17: corr -0.98 vs
real work), so it is vsync WAIT, not GL work. This script splits it out:

    busy   = cpp + (gl - glClear)    real CPU-side work per frame
    p/3,p/5 = cpp + (gl - glClear)/k  deko3d projections on the busy time

Gate-P0 (DEKO3D_MIGRATION_PLAN.md): projection <= 14 ms on >= 2 routes.

usage: python3 scripts/analyze-glt.py path/to/gk_run_log.txt [...]
"""
import re
import statistics
import sys

HEAD = re.compile(
    r"\[([\d.]+)\] \[glt\] (\d+)f/\d+s \| render ([\d.]+) \(max ([\d.]+)\) \| "
    r"gl ([\d.]+)ms \(([\d.]+)%\) cpp ([\d.]+)ms .*?\| (\d+) calls/f")
GATE_MS = 14.0


def fn_ms(top, name):
    m = re.search(r"\b" + name + r" (\d+)c ([\d.]+)ms", top)
    return float(m.group(2)) if m else 0.0


def pct(values, q):
    v = sorted(values)
    return v[min(len(v) - 1, int(round(q * (len(v) - 1))))]


def analyze(path):
    lines = open(path, errors="replace").read().splitlines()
    rows = []
    for i, line in enumerate(lines):
        m = HEAD.search(line)
        if not m:
            continue
        t, frames, render, rmax, gl, _share, cpp, calls = m.groups()
        frames = int(frames)
        top = lines[i + 1] if i + 1 < len(lines) and "top GL" in lines[i + 1] else ""
        clear = fn_ms(top, "glClear") / frames
        tex = (fn_ms(top, "glTexImage2D") + fn_ms(top, "glTexSubImage2D")) / frames
        work_gl = max(0.0, float(gl) - clear)
        cpp = float(cpp)
        rows.append({
            "t": float(t), "frames": frames, "render": float(render), "max": float(rmax),
            "clear": clear, "gl_work": work_gl, "tex": tex, "cpp": cpp,
            "busy": cpp + work_gl, "p3": cpp + work_gl / 3, "p5": cpp + work_gl / 5,
            "calls": int(calls),
        })
    if not rows:
        print(f"{path}: no [glt] report lines (was gk_glt.txt present?)")
        return

    print(f"== {path}: {len(rows)} windows, {sum(r['frames'] for r in rows)} frames")
    print("     t  frm render  clear glwork   tex   cpp  busy   p/3   p/5 calls/f")
    for r in rows:
        print("%6.1f %4d %6.2f %6.2f %6.2f %5.2f %5.2f %5.2f %5.2f %5.2f %6d" % (
            r["t"], r["frames"], r["render"], r["clear"], r["gl_work"], r["tex"],
            r["cpp"], r["busy"], r["p3"], r["p5"], r["calls"]))
    for key, label in (("busy", "busy (real work)"), ("clear", "glClear (vsync wait)"),
                       ("gl_work", "GL work excl. clear"), ("cpp", "C++"),
                       ("tex", "tex uploads"), ("p3", "proj gl/3"), ("p5", "proj gl/5")):
        vals = [r[key] for r in rows]
        print("%-22s p50 %6.2f  p95 %6.2f  max %6.2f" % (
            label, statistics.median(vals), pct(vals, 0.95), max(vals)))
    if len(rows) > 2:
        xs = [r["busy"] for r in rows]
        ys = [r["clear"] for r in rows]
        mx, my = statistics.mean(xs), statistics.mean(ys)
        den = (sum((a - mx) ** 2 for a in xs) * sum((b - my) ** 2 for b in ys)) ** 0.5
        if den:
            corr = sum((a - mx) * (b - my) for a, b in zip(xs, ys)) / den
            print("corr(busy, glClear) = %.3f  (near -1 => glClear is vsync wait)" % corr)
    p95 = pct([r["p3"] for r in rows], 0.95)
    print("Gate-P0 (conservative gl/3, p95): %.2f ms -> %s" % (
        p95, "PASS" if p95 <= GATE_MS else "MISS"))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    for p in sys.argv[1:]:
        analyze(p)
