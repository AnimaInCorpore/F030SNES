#!/usr/bin/env python3
"""Aggregate the VBL samples SMKX.TOS prints at its frame limit.

Each sample is (interrupted PC, last runtime call site), both text offsets.
Samples inside the runtime are charged to the translated call site that
entered it, so the table ranks 65816 procedures by where time went.
"""

from __future__ import annotations

import collections
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import smkx_pc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", type=Path, default=ROOT / "build/ctx/hatari-stderr.log")
    ap.add_argument("--timer", action="store_true",
                    help="the 200 Hz timer-C samples (every context, NMI and IRQs included)")
    ap.add_argument("--context", choices=("all", "main", "vbl"), default="all",
                    help="timer samples: only the main loop, or only the VBL handler")
    ap.add_argument("--top", type=int, default=10, help="runtime routines to list")
    ap.add_argument("--pairs", type=int, default=0,
                    help="also list the top (runtime routine, calling procedure) pairs")
    ap.add_argument("--map", type=Path, default=None,
                    help="the link map of the program that ran (default build/ctx/CTX.map)")
    args = ap.parse_args()
    if args.map:
        smkx_pc.MAP = args.map
    lines = args.log.read_text(errors="replace").splitlines()
    section = "SMKX tsamples" if args.timer else "SMKX samples"
    try:
        start = lines.index(section)
    except ValueError:
        raise SystemExit(f"no '{section}' in the log (build with SMKX_FRAMES set)")
    labels, addr65, proc = smkx_pc.load()
    base = labels["start"]
    nodes = sorted((a, n) for n, a in labels.items() if n in addr65)
    keys = [a for a, _ in nodes]
    first_code = keys[0]
    import bisect

    def where(off: int) -> str:
        pc = base + off
        i = bisect.bisect_right(keys, pc) - 1
        if i < 0:
            return "runtime"
        return f"{proc[nodes[i][1]]}"

    # Every symbol of the map names the code after it: the runtime, the
    # included DSP-1 glue (smkx_upd.s), the translated DSP-1 firmware
    # (upd_code.o) and the C objects, which all lie outside the translated
    # 65816 procedures. (Only rt_* labels made those samples look like
    # rt_print_hex8 or the last translated procedure.)
    last_code = max(keys)
    rt_labels = []
    for line in smkx_pc.MAP.read_text(errors="replace").splitlines():
        m = re.match(r"\s*0x([0-9a-f]+)\s+([A-Za-z_.]\w*):", line)
        if m and " abs," in line:
            continue                            # equates (SRAM_SIZE, SMKX_FRAMES), not code
        if m and m.group(2) not in addr65:
            a = int(m.group(1), 16)
            if a < first_code or a > last_code:
                rt_labels.append((a, m.group(2)))
    rt_labels.sort()
    rt_keys = [a for a, _ in rt_labels]

    def rt_name(off: int) -> str:
        i = bisect.bisect_right(rt_keys, base + off) - 1
        return rt_labels[i][1] if i >= 0 else "?"

    contexts = collections.Counter()
    by_rt = collections.Counter()
    by_proc = collections.Counter()
    pairs = collections.Counter()
    in_rt = 0
    total = 0
    for line in lines[start + 1:]:
        if line.startswith("SMKX samples"):
            break
        m = re.fullmatch(r"([0-9A-F]{8}) ([0-9A-F]{8})", line.strip())
        if not m:
            continue
        pc, caller = int(m.group(1), 16), int(m.group(2), 16)
        in_vbl = bool(pc & 0x80000000)          # timer samples: inside the VBL handler
        pc &= 0x7FFFFFFF
        contexts[in_vbl] += 1
        if args.context == "main" and in_vbl or args.context == "vbl" and not in_vbl:
            continue
        total += 1
        if base + pc < first_code or base + pc > last_code and rt_name(pc) != nodes[-1][1]:
            in_rt += 1
            by_rt[rt_name(pc)] += 1
            by_proc["[rt] " + where(caller)] += 1
            pairs[(rt_name(pc), where(caller))] += 1
        else:
            by_proc[where(pc)] += 1
    if args.timer:
        n = sum(contexts.values()) or 1
        print(f"contexts: main loop {100 * contexts[False] / n:.1f}%, VBL handler {100 * contexts[True] / n:.1f}%")
    print(f"{total} samples, {in_rt} in the runtime")
    print("runtime routines:")
    for name, n in by_rt.most_common(args.top):
        print(f"{n:6} {100 * n / total:5.1f}%  {name}")
    print("by procedure (runtime samples charged to the last generic memory call):")
    for name, n in by_proc.most_common(25):
        print(f"{n:6} {100 * n / total:5.1f}%  {name}")
    if args.pairs:
        print("runtime routine <- calling procedure:")
        for (r, c), n in pairs.most_common(args.pairs):
            print(f"{n:6} {100 * n / total:5.1f}%  {r} <- {c}")


if __name__ == "__main__":
    main()
