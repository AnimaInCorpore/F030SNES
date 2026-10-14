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
    log = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/yix/hatari-stderr.log"
    lines = log.read_text(errors="replace").splitlines()
    try:
        start = lines.index("SMKX samples")
    except ValueError:
        raise SystemExit("no samples in the log (build with SMKX_FRAMES set)")
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

    rt_labels = []
    for line in (ROOT / "build/yix/YIX.map").read_text(errors="replace").splitlines():
        m = re.match(r"\s*0x([0-9a-f]+)\s+(rt_\w+):", line)
        if m and int(m.group(1), 16) < first_code:
            rt_labels.append((int(m.group(1), 16), m.group(2)))
    rt_labels.sort()
    rt_keys = [a for a, _ in rt_labels]

    def rt_name(off: int) -> str:
        i = bisect.bisect_right(rt_keys, base + off) - 1
        return rt_labels[i][1] if i >= 0 else "?"

    by_rt = collections.Counter()
    by_proc = collections.Counter()
    in_rt = 0
    total = 0
    for line in lines[start + 1:]:
        m = re.fullmatch(r"([0-9A-F]{8}) ([0-9A-F]{8})", line.strip())
        if not m:
            continue
        pc, caller = int(m.group(1), 16), int(m.group(2), 16)
        total += 1
        if base + pc < first_code:
            in_rt += 1
            by_rt[rt_name(pc)] += 1
            by_proc["[rt] " + where(caller)] += 1
        else:
            by_proc[where(pc)] += 1
    print(f"{total} samples, {in_rt} in the runtime")
    print("runtime routines:")
    for name, n in by_rt.most_common(10):
        print(f"{n:6} {100 * n / total:5.1f}%  {name}")
    print("by procedure (runtime samples charged to the last generic memory call):")
    for name, n in by_proc.most_common(25):
        print(f"{n:6} {100 * n / total:5.1f}%  {name}")


if __name__ == "__main__":
    main()
