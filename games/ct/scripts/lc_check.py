#!/usr/bin/env python3
"""Checks in-game snapshots (F*.BIN from scripts/lc_snaps.sh) against the host reference, for the list compositor.

    scripts/lc_check.py DIR [-v]

For every snapshot: the planner (build/lc/lc_why, cold) says whether the compositor covers the frame. A covered frame
was drawn by the compositor, which leaves colour math out, so it is compared with the reference rendered the same way
(PPU_SNAP_NOMATH=1) unless LC_REFUSE_MATH is set in the environment (the build with CTX_LC_MATH=1, the default: frames with
colour math are not covered, so the full reference applies); any other frame went the old way and is compared with the full
reference. Prints a line a snapshot
(-v) and the totals; exit status 1 on any difference. Needs build/ppu_snap and build/lc/lc_why (make)."""
import re
import subprocess
import sys
import os
from pathlib import Path

root = Path(__file__).resolve().parents[1]
d = Path(sys.argv[1])
verbose = "-v" in sys.argv
rom = root / "build/ctx/ct.sfc"
ok_tab = root / "build/ctx/oklab555.bin"
snaps = sorted(d.glob("F*.BIN"))
why = subprocess.run([str(root / "build/lc/lc_why"), str(rom)] + [str(s) for s in snaps], capture_output=True, text=True).stdout
rc_of = {}
for line in why.splitlines():
    m = re.match(r"(\S+) rc (-?\d+) why ([0-9a-f]+)", line)
    if m:
        rc_of[m.group(1)] = (int(m.group(2)), m.group(3))
tot = {"covered": 0, "covered_bad": 0, "blank": 0, "other": 0, "other_bad": 0}
words = 0
for s in snaps:
    rc, w = rc_of.get(s.name, (-1, "?"))
    env = dict(os.environ)
    if rc == 0 and "LC_REFUSE_MATH" not in os.environ:
        env["PPU_SNAP_NOMATH"] = "1"
    r = subprocess.run([str(root / "build/ppu_snap"), str(s), str(rom), str(ok_tab)], capture_output=True, text=True, env=env)
    m = re.search(r"(\d+) of 28672 \w+ differ(?: in (\d+) rows)?", r.stdout)
    diff = int(m.group(1)) if m else -1
    kind = "covered" if rc == 0 else "blank" if rc == 1 else "other"
    tot[kind] = tot.get(kind, 0) + 1
    if diff != 0:
        tot["covered_bad" if rc == 0 else "other_bad"] += 1
        words += max(diff, 0)
    if verbose or diff != 0:
        print(f"{s.name} {kind:8s} why {w:>3s} differing {diff}")
print(f"{len(snaps)} snapshots: {tot['covered']} covered by the compositor ({tot['covered_bad']} differ from the no-math reference), "
      f"{tot['blank']} forced blank, {tot['other']} other ({tot['other_bad']} differ from the full reference); {words} differing words in all")
sys.exit(1 if tot["covered_bad"] or tot["other_bad"] else 0)
