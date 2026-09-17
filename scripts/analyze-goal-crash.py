#!/usr/bin/env python3
"""
analyze-goal-crash.py -- triage gk_fatal.txt CPU exceptions from the Switch port. (AI-assisted)

Reconstructs what the 2026-09-17 triage did by hand:

  1. Parses every "=== FIX 7n CPU EXCEPTION ===" block in gk_fatal.txt (pc/lr/sp/fp).
  2. Parses every boot's "[ee_runner] rw=... rx=..." line from gk_boot_log.txt.
  3. For legacy dumps (built before FIX 25) matches each crash to its boot by requiring
     lr to land inside that boot's executable alias [rx, rx+128MB); new dumps carry an
     "=== EE (GOAL) DECODE ===" section and are read directly.
  4. Prints per-crash EE offsets (pc_ee is the branch target, lr_ee is the GOAL caller,
     sp_ee the GOAL stack) plus a caller-cluster summary: Jak 1 crashes so far collapse
     into a handful of call sites calling through method slots that hold data-heap
     pointers (pc_ee ~0x1900000-0x2C00000) or null.

Usage:
  python3 scripts/analyze-goal-crash.py --fatal /path/to/gk_fatal.txt \
      --boot-log /path/to/gk_boot_log.txt \
      [--run-log /path/to/gk_run_log.txt]

The logs are written to the root of the SD card, so copy them off the card first (or point
these options straight at the mounted card).

Stdlib only; run on the host against copies of the SD-card logs.
"""

import argparse
import re
import sys

EE_SIZE = 0x8000000  # EE_MAIN_MEM_SIZE (128 MB), common/goal_constants.h

EXC_HEADER = re.compile(r"=== FIX 7n CPU EXCEPTION ===")
REGS_LINE = re.compile(r"pc=0x([0-9a-f]+) lr=0x([0-9a-f]+) sp=0x([0-9a-f]+) fp=0x([0-9a-f]+)")
BOOT_LINE = re.compile(r"\[ee_runner\] rw=0x([0-9a-f]+) rx=0x([0-9a-f]+)")
EE_DECODE = re.compile(
    r"rw=0x([0-9a-f]+) rx=0x([0-9a-f]+) size=0x([0-9a-f]+)\n"
    r"pc_ee=0x([0-9a-f]+) lr_ee=0x([0-9a-f]+) sp_ee=0x([0-9a-f]+)"
)
RUNLOG_FATAL = re.compile(r"\[FATAL\] CPU exception desc=0x([0-9a-f]+) far=0x([0-9a-f]+)")


def parse_fatal(path):
    crashes = []
    with open(path, "r", errors="replace") as f:
        lines = f.readlines()
    i = 0
    while i < len(lines):
        if EXC_HEADER.search(lines[i]):
            block = {"fatal_line": i + 1, "pc": None, "lr": None, "sp": None, "fp": None,
                     "decode": None, "backtrace": None, "regs_ee": None}
            j = i
            while j < len(lines) and j < i + 60:
                m = REGS_LINE.search(lines[j])
                if m and block["pc"] is None:
                    block["pc"] = int(m.group(1), 16)
                    block["lr"] = int(m.group(2), 16)
                    block["sp"] = int(m.group(3), 16)
                    block["fp"] = int(m.group(4), 16)
                # the decode section spans two lines, so search a two-line window
                d = EE_DECODE.search("".join(lines[j:j + 2]))
                if d and block["decode"] is None:
                    block["decode"] = {
                        "rw": int(d.group(1), 16),
                        "rx": int(d.group(2), 16),
                        "pc_ee": int(d.group(4), 16),
                        "lr_ee": int(d.group(5), 16),
                        "sp_ee": int(d.group(6), 16),
                    }
                if lines[j].startswith("goal_backtrace:"):
                    block["backtrace"] = lines[j].strip()
                if lines[j].startswith("regs_ee:"):
                    block["regs_ee"] = lines[j].strip()
                if lines[j].startswith("=== ") and j > i and not lines[j].startswith("=== EE"):
                    break
                j += 1
            if block["pc"] is not None:
                crashes.append(block)
            i = j
        else:
            i += 1
    return crashes


def parse_boots(path):
    boots = []
    try:
        with open(path, "r", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                m = BOOT_LINE.search(line)
                if m:
                    boots.append({"line": lineno, "rw": int(m.group(1), 16),
                                  "rx": int(m.group(2), 16)})
    except FileNotFoundError:
        pass
    return boots


def parse_runlog_fatals(path):
    out = []
    try:
        with open(path, "r", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                m = RUNLOG_FATAL.search(line)
                if m:
                    out.append({"line": lineno, "far": int(m.group(2), 16)})
    except FileNotFoundError:
        pass
    return out


def match_boot(crash, boots):
    """Legacy matching: the caller lr must be inside the boot's executable alias, and the
    GOAL stack pointer (which lives inside the arena's writable alias) must be inside that
    same boot's writable alias. The sp check disambiguates rx collisions."""
    hits = []
    for b in boots:
        if b["rx"] <= crash["lr"] < b["rx"] + EE_SIZE:
            if b["rw"] <= crash["sp"] < b["rw"] + EE_SIZE:
                hits.append(b)
    return hits


def classify(pc_ee):
    if pc_ee == 0:
        return "null GOAL function pointer (call to EE+0)"
    if 0 < pc_ee < EE_SIZE:
        return "call INTO the EE arena data/heap (method slot holds a data pointer?)"
    return "call to a wild address outside the EE arena"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fatal", required=True, help="path to gk_fatal.txt")
    ap.add_argument("--boot-log", default=None, help="path to gk_boot_log.txt")
    ap.add_argument("--run-log", default=None, help="path to gk_run_log.txt")
    args = ap.parse_args()

    crashes = parse_fatal(args.fatal)
    boots = parse_boots(args.boot_log) if args.boot_log else []
    runlog = parse_runlog_fatals(args.run_log) if args.run_log else []

    if not crashes:
        print("no FIX 7n CPU exception blocks found in", args.fatal)
        return 0

    print(f"{len(crashes)} crash(es), {len(boots)} boot(s) in boot log, "
          f"{len(runlog)} [FATAL] breadcrumb(s) in run log\n")

    callers = []
    for idx, c in enumerate(crashes, 1):
        if c["decode"]:
            d = c["decode"]
            rx, lr_ee, pc_ee, sp_ee = d["rx"], d["lr_ee"], d["pc_ee"], d["sp_ee"]
            src = f"ee-decode rx=0x{rx:x}"
        else:
            hits = match_boot(c, boots)
            if len(hits) != 1:
                print(f"crash #{idx} (fatal line {c['fatal_line']}): "
                      f"{len(hits)} boot candidates, ambiguous -- skipping")
                for h in hits:
                    print(f"    boot at boot-log line {h['line']}: rx=0x{h['rx']:x} "
                          f"lr_ee would be 0x{c['lr'] - h['rx']:x}")
                continue
            rx = hits[0]["rx"]
            lr_ee = c["lr"] - rx
            pc_ee = (c["pc"] - rx) if c["pc"] >= rx else (c["pc"] - hits[0]["rw"])
            sp_ee = (c["sp"] - hits[0]["rw"]) if c["sp"] >= hits[0]["rw"] else 0
            src = f"boot-log line {hits[0]['line']} rx=0x{rx:x}"
        callers.append(lr_ee)
        print(f"crash #{idx} (fatal line {c['fatal_line']}) [{src}]")
        print(f"  pc(GOAL branch target) EE+0x{pc_ee:x}   <- {classify(pc_ee)}")
        print(f"  lr(GOAL caller)         EE+0x{lr_ee:x}")
        print(f"  sp(GOAL stack)          rw+0x{sp_ee:x}")
        if c["regs_ee"]:
            print(f"  {c['regs_ee']}")
        if c["backtrace"]:
            print(f"  {c['backtrace']}")
        print()

    # Cluster callers: crashes within 64 KB of each other are likely the same call site
    # (same object, possibly the same function).
    if callers:
        print("caller clusters (lr_ee, grouped within 64 KB):")
        clusters = []
        for lr in sorted(callers):
            if clusters and lr - clusters[-1][-1] < 0x10000:
                clusters[-1].append(lr)
            else:
                clusters.append([lr])
        for cl in clusters:
            span = f"EE+0x{cl[0]:x}" if cl[0] == cl[-1] else f"EE+0x{cl[0]:x}..EE+0x{cl[-1]:x}"
            print(f"  {span}  x{len(cl)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
