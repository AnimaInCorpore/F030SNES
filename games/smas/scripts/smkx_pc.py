#!/usr/bin/env python3
"""Map a text-segment offset of SMKX.TOS back to the translated 65816 node.

    scripts/smkx_pc.py 0001A2B4 [...]

Uses vlink's map (build/smasx/SMASX.map) for label addresses and the
generated sources for each node label's 65816 address comment.
"""

from __future__ import annotations

import bisect
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/smasx"


def load():
    labels = {}
    for line in (OUT / "SMASX.map").read_text(errors="replace").splitlines():
        m = re.match(r"\s*0x([0-9a-fA-F]+)\s+([ne]\d+(?:_\d+)?|start):", line)
        if m:
            labels[m.group(2)] = int(m.group(1), 16)
    addr65 = {}
    proc = {}
    for src in (OUT / "sources.txt").read_text().split():
        current = None
        for line in (OUT / src).read_text().splitlines():
            if line.startswith("; P"):
                current = line[2:].strip()
            m = re.match(r"(n\d+_\d+):\s+; \$([0-9A-F]{6})", line)
            if m:
                addr65[m.group(1)] = int(m.group(2), 16)
                proc[m.group(1)] = current
    return labels, addr65, proc


def main() -> None:
    labels, addr65, proc = load()
    base = labels["start"]
    nodes = sorted((a, n) for n, a in labels.items() if n in addr65)
    keys = [a for a, _ in nodes]
    for arg in sys.argv[1:]:
        pc = base + int(arg, 16)
        i = bisect.bisect_right(keys, pc) - 1
        if i < 0:
            print(f"{arg}: before the translated code (runtime)")
            continue
        a, n = nodes[i]
        print(f"{arg}: {n} +{pc - a} -> 65816 ${addr65[n]:06X} in {proc[n]}")


if __name__ == "__main__":
    main()
