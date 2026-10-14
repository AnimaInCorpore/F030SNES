#!/usr/bin/env python3
"""Reads `lc_model census` output on stdin, prints the snapshot files (one per line, in the
directory given as argument) of the longest run of consecutive frames the model covers.
Consecutive: the frame numbers in the names (Fnnnnnnn.BIN, hex) differ by one."""
import sys

rows = []
for line in sys.stdin:
    parts = line.split()
    if len(parts) >= 2 and parts[0].startswith("F") and parts[0].endswith(".BIN"):
        rows.append((parts[0], parts[1] == "yes"))
best, cur = [], []
for name, ok in rows:
    n = int(name[1:-4], 16)
    if ok and (not cur or n == int(cur[-1][1:-4], 16) + 1):
        cur.append(name)
    elif ok:
        cur = [name]
    else:
        cur = []
    if len(cur) > len(best):
        best = list(cur)
for name in best:
    print(f"{sys.argv[1]}/{name}")
