#!/usr/bin/env python3
"""The race census over every mode the menus reach, on the reference interpreter.

    python3 scripts/race_census_runs.py OUTDIR          # all runs, then the summary
    python3 scripts/race_census_runs.py OUTDIR --summary

tools/ref65816.c plays scripted input (REF_INPUT, pad 2 in the upper 16 bits)
and writes snapshots in the runtime's SNAP.BIN format (REF_SNAP_EVERY); its
renderer inputs equal the Falcon's (analysis/race-census.md). Runs:

    gp1        1P Mario GP, 50cc, Mushroom Cup (Mario Circuit 1)
    tt_CK      1P time trial, cup C (0-2), course K (0-4): the 15 open tracks
    p2_M_K     2P: M = 0 GP (cup K), 1 match race (track K = cup*5 + course),
               2 battle (course K)

The menus are found from the game state ($36: 6 driver select, $16 course
select, $08 cup select), not from fixed frames: each screen appears some
frames after the press that leads to it. Every run holds B (accelerate) in
the race; nobody steers. Needs build/ref65816, build/race_census and
build/smkx/smk.sfc.
"""

from __future__ import annotations

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REF = [str(ROOT / "build/ref65816"), str(ROOT / "build/smkx/smk.sfc"), str(ROOT / "assets/dsp1.bin")]
CENSUS = str(ROOT / "build/race_census")
B, A, UP, DOWN, LEFT, RIGHT = 0x8000, 0x0080, 0x0800, 0x0400, 0x0200, 0x0100
P2 = 16                                         # pad 2: the upper 16 bits
WRAM = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 + 256 * 6      # in SNAP.BIN


def script(ev, hold=None):
    out = []
    for f, p in sorted(ev):
        out += [f"{f}:{p:X}", f"{f + 6}:0"]
    if hold:
        out.append(f"{hold[0]}:{hold[1]:X}")
    return ",".join(out)


def run(ev, frames, hold=None, snap_dir=None):
    """The reference with this input; the frames at which $36 was written, with the value."""
    env = dict(os.environ, REF_INPUT=script(ev, hold), REF_WATCH="36", REF_WATCH_FROM="300")
    if snap_dir:
        os.makedirs(snap_dir, exist_ok=True)
        env.update(REF_SNAP_EVERY="100", REF_SNAP_DIR=snap_dir)
    r = subprocess.run(REF + [str(frames)], env=env, capture_output=True, text=True).stdout
    return [(int(l.split("(frame ")[1].split(",")[0]), l.split(" = ")[1][:2])
            for l in r.splitlines() if l.startswith("write")]


def until(ev, state, start):
    """B every 100 frames from start on until $36 reaches state: (events, frame)."""
    for n in range(0, 6):
        trial = ev + [(start + 100 * i, B) for i in range(n)]
        st = run(trial, start + 100 * n + 400)
        hit = [f for f, s in st if s == state and f >= start - 100]
        if hit:
            return trial, hit[0]
    raise SystemExit(f"state ${state} not reached")


def course_keys(s, cup, course):
    """The time trial / match race course list: Left into the cups, Right back."""
    ev = [(s, LEFT)] + [(s + 40 + 40 * i, DOWN) for i in range(cup)] + [(s + 160, RIGHT)]
    ev += [(s + 200 + 40 * i, DOWN) for i in range(course)]
    return ev + [(s + 400, B), (s + 500, B)]


def race_1p(out, tt, cup=0, course=0):
    ev = [(300, B), (350, UP), (400, B)] + ([(450, DOWN)] if tt else []) + [(500, B)]
    ev, f6 = until(ev, "06", 600)
    ev += [(f6 + 60, B), (f6 + 160, B)]                    # the driver, OK
    nxt = [(f, s) for f, s in run(ev, f6 + 800) if f > f6 and s not in ("00", "06")]
    s = nxt[0][0] + 60
    ev += course_keys(s, cup, course) if tt else [(s + 40 * i, DOWN) for i in range(cup)] + [(s + 200, B)]
    run(ev, s + 3000, hold=(s + 700, B), snap_dir=out)


def race_2p(out, mode, k):
    ev = [(300, B), (400, B)] + [(450 + 50 * i, DOWN) for i in range(mode)] + [(600, B)]
    ev, f6 = until(ev, "06", 700)
    ev += [(f6 + 60, B), (f6 + 90, B << P2), (f6 + 160, B), (f6 + 190, B << P2)]   # one pad a frame
    nxt = [(f, s) for f, s in run(ev, f6 + 800) if f > f6 and s not in ("00", "06")]
    s = nxt[0][0] + 60
    if mode == 0:
        ev += [(s + 40 * i, DOWN) for i in range(k)] + [(s + 200, B)]
    elif mode == 1:
        ev += course_keys(s, *divmod(k, 5))
    else:
        ev += [(s + 40 * i, DOWN) for i in range(k)] + [(s + 200, B), (s + 300, B)]
    run(ev, s + 3000, hold=(s + 700, B | B << P2), snap_dir=out)


def layout(txt):
    rows = re.findall(r"^rows +(\d+)- *(\d+) \( *\d+\): (mode \d[^,]*), main ([A-Za-z0-9 ]*)sub ([A-Za-z0-9 ]*)INIDISP \$(..)",
                      txt, re.M)
    return ";".join(f"{a}-{b} {m.split()[1]} [{t.strip()}]" + (" blank" if i.startswith("8") else "")
                    for a, b, m, t, s, i in rows)


def summary(base):
    """Per run: race frames ($36 = 2 or $0E in the snapshot's WRAM), layouts, sprites, VRAM and
    CGRAM changes between race frames 100 frames apart."""
    for d in sorted(glob.glob(f"{base}/*/")):
        name = Path(d).name
        sigs, smax, pages, cg, prev = {}, 0, set(), 0, None
        for f in sorted(glob.glob(f"{d}F*.BIN")):
            b = open(f, "rb").read()
            if b[WRAM + 0x36] not in (0x02, 0x0E):       # not racing ($36: 2 race, $0E battle)
                prev = None
                continue
            t = subprocess.run([CENSUS, f, REF[1]], capture_output=True, text=True).stdout
            sig = layout(t)
            sigs[sig] = sigs.get(sig, 0) + 1
            smax = max([smax] + [int(x) for x in re.findall(r"max (\d+),", t)])
            if prev:
                pages |= {p for p in range(64) if b[12 + p * 1024:12 + (p + 1) * 1024] != prev[12 + p * 1024:12 + (p + 1) * 1024]}
                c0 = 12 + 65536
                cg = max(cg, sum(1 for i in range(256) if b[c0 + 2 * i:c0 + 2 * i + 2] != prev[c0 + 2 * i:c0 + 2 * i + 2]))
            prev = b
        print(f"{name}: {sum(sigs.values())} race frames, sprites/row max {smax}, VRAM pages changed "
              f"{len(pages)} ({' '.join('%04X' % (p * 512) for p in sorted(pages))}), CGRAM entries changed "
              f"per 100 frames max {cg}")
        for s, n in sigs.items():
            print(f"   {n:3d}x {s}")


def main() -> None:
    base = sys.argv[1]
    if "--summary" not in sys.argv:
        race_1p(f"{base}/gp1", tt=False)
        for c in range(3):
            for k in range(5):
                race_1p(f"{base}/tt_{c}{k}", tt=True, cup=c, course=k)
        for mode, ks in ((0, (0, 2)), (1, (0, 7, 12)), (2, (0, 1, 2, 3))):
            for k in ks:
                race_2p(f"{base}/p2_{mode}_{k}", mode, k)
    summary(base)


if __name__ == "__main__":
    main()
