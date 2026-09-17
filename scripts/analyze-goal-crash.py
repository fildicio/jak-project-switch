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
  5. FIX 27/28 aware: "[klink] obj=... base=EE+0x... size=..." lines in the boot log and
     the crash-time "=== LINK BASES ===" / "=== OBJECTS ===" / "=== SYMBOLS ===" sections
     in gk_fatal.txt get parsed, so pc/lr print as object+section names instead of bare
     EE offsets (and SYMBOLS lines are surfaced verbatim -- they name data targets).

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

# FIX 27 (boot log, chronological): "[klink] obj=gcommon base=EE+0x1234 size=99 seg2base=..."
KLINK_LINE = re.compile(
    r"\[klink\] obj=(\S+) base=EE\+0x([0-9a-f]+) size=(\d+)"
    r"(?: seg2base=EE\+0x([0-9a-f]+) seg2size=(\d+))?"
)
# FIX 28 (gk_fatal.txt, newest-first dump): "EE+0x1234 size=99 obj=gcommon"
LINK_BASES_LINE = re.compile(r"EE\+0x([0-9a-f]+) size=(\d+) obj=(\S+)")
# FIX 28b: "EE+0x18fe04 == *some-global*" (exact) / "EE+0x1937bdc <= some-func+0x2c" (nearest)
SYMBOLS_LINE = re.compile(r"EE\+0x([0-9a-f]+) (==|<=) (\S+?)\+0x([0-9a-f]+)")
OBJECTS_LINE = re.compile(r"(pc_in|lr_in)=(.+)")


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


def parse_klink_bases(path):
    """FIX 27 boot-log lines, in link (chronological) order. Only covers links that happened
    before switch_boot_log latched off at "boot complete" -- level code needs the FIX 28
    crash-time dump instead."""
    out = []
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                m = KLINK_LINE.search(line)
                if m:
                    out.append((int(m.group(2), 16), int(m.group(3)), m.group(1)))
                    if m.group(4) and m.group(5) != "0":
                        out.append((int(m.group(4), 16), int(m.group(5)), m.group(1)))
    except FileNotFoundError:
        pass
    return out


def attach_fix28_sections(path, crashes):
    """Pull the per-crash FIX 28 sections (=== OBJECTS === / === LINK BASES === / ===
    SYMBOLS ===) out of gk_fatal.txt. They can be over a thousand lines each, so the
    bounded window in parse_fatal can't see them; slice on the exception headers instead."""
    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()
    except FileNotFoundError:
        return
    headers = [i for i, l in enumerate(lines) if EXC_HEADER.search(l)]
    for k, block in enumerate(crashes):
        start = headers[k] if k < len(headers) else len(lines)
        end = headers[k + 1] if k + 1 < len(headers) else len(lines)
        section = None
        for l in lines[start:end]:
            if l.startswith("=== OBJECTS"):
                section = "objects"
                block["objects"] = {}
                continue
            if l.startswith("=== LINK BASES"):
                section = "bases"
                block["link_bases"] = []
                continue
            if l.startswith("=== SYMBOLS"):
                section = "symbols"
                block["symbols"] = []
                continue
            if l.startswith("=== "):
                section = None
                continue
            if section == "objects":
                m = OBJECTS_LINE.search(l)
                if m:
                    block["objects"][m.group(1)] = m.group(2).strip()
            elif section == "bases":
                m = LINK_BASES_LINE.search(l)
                if m:
                    block["link_bases"].append(
                        (int(m.group(1), 16), int(m.group(2)), m.group(3)))
            elif section == "symbols":
                block["symbols"].append(l.rstrip("\n"))


class Resolver:
    """EE offset -> "obj+0xoff". FIX 28 crash-time dumps (newest-first) win over FIX 27
    boot-log lines (chronological, so the last covering entry is newest); a level reload
    re-links an object at a new base, which is why newest matters."""

    def __init__(self, fatal_bases=None, klink_bases=None):
        self.fatal_bases = fatal_bases or []
        self.klink_bases = klink_bases or []

    def resolve(self, addr):
        for base, size, name in self.fatal_bases:
            if base <= addr < base + size:
                return f"{name}+0x{addr - base:x}"
        hit = None
        for base, size, name in self.klink_bases:
            if base <= addr < base + size:
                hit = f"{name}+0x{addr - base:x}"  # last covering entry = newest
        return hit


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
    klink_bases = parse_klink_bases(args.boot_log) if args.boot_log else []
    attach_fix28_sections(args.fatal, crashes)

    if not crashes:
        print("no FIX 7n CPU exception blocks found in", args.fatal)
        return 0

    print(f"{len(crashes)} crash(es), {len(boots)} boot(s) in boot log, "
          f"{len(runlog)} [FATAL] breadcrumb(s) in run log, "
          f"{len(klink_bases)} [klink] base line(s)\n")

    callers = []
    lr_names = {}
    for idx, c in enumerate(crashes, 1):
        resolver = Resolver(c.get("link_bases"), klink_bases)
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
        # FIX 27/28 annotations: the crash dump's own OBJECTS section is authoritative;
        # fall back to resolving against the dumped/boot-logged code ranges.
        obj = c.get("objects", {})
        raw_pc, raw_lr = obj.get("pc_in", ""), obj.get("lr_in", "")
        pc_in = raw_pc if raw_pc not in ("", "?") else resolver.resolve(pc_ee)
        lr_in = raw_lr if raw_lr not in ("", "?") else resolver.resolve(lr_ee)
        if pc_in and pc_in != "?":
            print(f"  pc in: {pc_in}")
        if lr_in and lr_in != "?":
            print(f"  lr in: {lr_in}")
            lr_names[lr_ee] = lr_in.split("+")[0]
        if c.get("symbols"):
            keep = []
            for s in c["symbols"]:
                m = SYMBOLS_LINE.search(s)
                if m and (m.group(2) == "==" or int(m.group(1), 16) in (pc_ee, lr_ee)):
                    keep.append(s)
                elif s.startswith("==="):
                    keep.append(s)
            for s in keep[:10]:
                print(f"  sym: {s}")
        if c["regs_ee"]:
            print(f"  {c['regs_ee']}")
        if c["backtrace"]:
            print(f"  {c['backtrace']}")
            bt = re.findall(r"EE\+0x([0-9a-f]+)", c["backtrace"])
            named = []
            for tok in bt[:12]:
                r = resolver.resolve(int(tok, 16))
                if r:
                    named.append(r)
            if named:
                print(f"  backtrace resolved: {' '.join(named)}")
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
            objs = sorted({lr_names[lr] for lr in cl if lr in lr_names})
            print(f"  {span}  x{len(cl)}" + (f"  [{', '.join(objs)}]" if objs else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
