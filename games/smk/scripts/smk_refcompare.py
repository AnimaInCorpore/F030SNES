#!/usr/bin/env python3
"""Compare the WRAM sums of a SMKX_SUMS=1 run with tools/ref65816.c's.

    python3 scripts/smk_refcompare.py build/smkx/ref.txt build/smkx/ref-stderr.log

Both print one sum per frame, after the frame's IRQs and before its NMI
("sum NMIS SUM" / "SMKX sum NMIS SUM FRAMES"). Prints the first frame that
differs; exits non-zero if any does. Then, to find the bytes:
build/ref65816 ... N+1 ref.bin (WRAM at NMI N), a Falcon run with
SMKX_FRAMES=N+1 (SNAP.BIN at the same checkpoint), and REF_WATCH=offsets
REF_WATCH_FROM=N-1 on the reference for the writing instructions.
"""

import re
import sys


def main() -> int:
    ref = {}
    for line in open(sys.argv[1]):
        m = re.match(r"sum (\d+) ([0-9A-F]{8})", line)
        if m:
            ref[int(m.group(1))] = m.group(2)
    compared = same = 0
    first = None
    for line in open(sys.argv[2], errors="replace"):
        m = re.match(r"SMKX sum ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8})", line)
        if not m:
            continue
        n, s, frame = int(m.group(1), 16), m.group(2), int(m.group(3), 16)
        if n not in ref:
            continue
        compared += 1
        if ref[n] == s:
            same += 1
        elif first is None:
            first = (n, frame, s, ref[n])
    print(f"{compared} frames compared, {same} equal")
    if first:
        print(f"first difference at NMI {first[0]} (Falcon frame {first[1]}): Falcon {first[2]}, reference {first[3]}")
    return 0 if compared and not first else 1


if __name__ == "__main__":
    sys.exit(main())
