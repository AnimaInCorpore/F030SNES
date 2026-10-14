#!/usr/bin/env python3
"""Pictures a second of game runs (build/ff6x/TAG-stderr.log), by slices of game frames.

    scripts/tc_bench/picrate.py [-s SLICE] TAG [TAG...]

Per slice and run: pictures drawn by the DSP a second (drawn), pictures skipped because no row changed
(skip), rows drawn as whole pictures a second (rows, partial rendering), pictures that differed from the
last one (chgd: the pictures a viewer can tell apart, per second). Whole-run totals last. The ppudsp counters are documented in
src/smkx_ppu.h (PPU_ST_*): frames [0], plan time [3], skipped [12], rows [14]. Emulated time in the patched
Hatari (write its identity beside any figure); one game frame is 1/50 s (the game holds 50 fps in almost every scene).
"""
import re, sys

def load(f):
    d = {}
    for l in open(f, errors="replace"):
        m = re.search(r"SMKX: frame ([0-9A-F]{8}) vbl ([0-9A-F]{8}).*?ppudsp ([0-9A-F/]+)", l)
        if m:
            d[int(m[1], 16)] = (int(m[2], 16), [int(x, 16) for x in m[3].strip("/").split("/")])
    return d

def near(d, k):
    return min(d, key=lambda x: abs(x - k))

args = sys.argv[1:]
step = 1000
if args and args[0] == "-s":
    step = int(args[1]); args = args[2:]
runs = {t: load(f"build/ff6x/{t}-stderr.log") for t in args}
print("frames      " + "  ".join(f"{t:>26s}" for t in args))
print("            " + "  ".join("  drawn  skip  rows  chgd" for t in args))
last = min(max(d) for d in runs.values())
for a in range(0, last - step + 1, step):
    line = f"{a:5d}-{a + step:5d} "
    for t, d in runs.items():
        (va, pa), (vb, pb) = d[near(d, a)], d[near(d, a + step)]
        secs = max((vb - va) / 50, 1e-9)
        n = pb[0] - pa[0]
        sk = (pb[12] - pa[12]) / secs if len(pb) > 12 else 0
        rw = (pb[14] - pa[14]) / 224 / secs if len(pb) > 14 else n / secs
        ch = (pb[17] - pa[17]) / secs if len(pb) > 17 else n / secs
        line += f"  {n / secs:6.2f} {sk:5.1f} {rw:5.2f} {ch:5.2f}"
    print(line)
tot = f"total       "
for t, d in runs.items():
    k = near(d, last)
    v, p = d[k]
    secs = v / 50
    tot += f"  {p[0] / secs:6.2f} {p[12] / secs if len(p) > 12 else 0:5.1f} {p[14] / 224 / secs if len(p) > 14 else p[0] / secs:5.2f} {p[17] / secs if len(p) > 17 else p[0] / secs:5.2f}"
print(tot)
