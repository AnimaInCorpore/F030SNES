#!/usr/bin/env python3
"""Snapshots of SMK's tile-mode screens from the reference interpreter (tools/ref65816.c), for the
list compositor's host checks (analysis/list-compositor-smk.md), and a census of what they show.

    python3 scripts/tile_snaps.py run OUTDIR FRAMES EVERY    # 1P GP to the race start; EVERY 1 = every frame
    python3 scripts/tile_snaps.py census DIR                  # distinct row layouts in DIR's snapshots
    python3 scripts/tile_snaps.py window DIR LO HI OUTDIR [STEP]   # frames LO..HI (every STEP-th) in the shared layout

`run` plays the menus as scripts/race_census_runs.py does (title, mode select, driver select,
cup, course; every screen found from the game state) and holds B in the race, which starts at
about frame 1,307. Needs build/ref65816, build/race_census and build/smkx/smk.sfc.
"""
from __future__ import annotations

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import scripts.race_census_runs as R  # noqa: E402


def run(out: str, frames: int, every: int) -> None:
    ev = [(300, R.B), (350, R.UP), (400, R.B), (500, R.B)]
    ev, f6 = R.until(ev, "06", 600)
    ev += [(f6 + 60, R.B), (f6 + 160, R.B)]
    nxt = [(f, s) for f, s in R.run(ev, f6 + 800) if f > f6 and s not in ("00", "06")]
    s = nxt[0][0] + 60
    ev += [(s + 200, R.B)]
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, REF_INPUT=R.script(ev, (s + 700, R.B)), REF_SNAP_EVERY=str(every), REF_SNAP_DIR=out)
    subprocess.run(R.REF + [str(frames)], env=env, capture_output=True, check=True)
    print(f"{len(glob.glob(out + '/F*.BIN'))} snapshots in {out}")


def census(d: str) -> None:
    groups: dict = {}
    for f in sorted(glob.glob(d + "/F*.BIN")):
        t = subprocess.run([R.CENSUS, f, R.REF[1]], capture_output=True, text=True).stdout
        rows = re.findall(r"^rows +(\d+)- *(\d+) \( *\d+\): (mode \d[^,]*), main ([A-Za-z0-9 ]*)sub ([A-Za-z0-9 ]*)INIDISP \$(..)",
                          t, re.M)
        sig = ";".join(f"{m.split()[1]}[{mm.strip()}]" + ("B" if i.startswith("8") else "") for a, b, m, mm, s, i in rows)
        ws = sorted(set(re.findall(r"windows (\$..) (\$..) (\$..) \[(.*?)\]", t)))
        n = int(re.search(r"F0*([0-9A-F]+)\.BIN", f).group(1), 16)
        groups.setdefault((sig, tuple(ws)), []).append(n)
    for (sig, ws), v in groups.items():
        print(f"{len(v):5d} frames {v[0]}-{v[-1]}: {sig[:80]}  windows {ws[:2]}")


def window(d: str, lo: int, hi: int, out: str, step: int = 1) -> None:
    sel = [f for f in sorted(glob.glob(d + "/F*.BIN")) if lo <= int(f[-11:-4], 16) <= hi and int(f[-11:-4], 16) % step == 0]
    subprocess.run([sys.executable, str(ROOT / "tools/tile/snap_to_ct.py"), out] + sel, check=True)
    print(f"{len(sel)} snapshots converted to {out}")


if __name__ == "__main__":
    c = sys.argv[1]
    if c == "run":
        run(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    elif c == "census":
        census(sys.argv[2])
    elif c == "window":
        window(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], int(sys.argv[6]) if len(sys.argv) > 6 else 1)
