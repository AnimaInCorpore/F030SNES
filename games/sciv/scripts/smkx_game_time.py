#!/usr/bin/env python3
"""Game-code time per frame from the "SMKX time" lines of a run.

    python3 scripts/smkx_game_time.py [build/scivx/hatari-stderr.log] [--state 2]

Each status line (every 50 frames) is followed by cumulative counters in
1/38400 s: main-loop frames and their time without interrupts (marks in
tools/smkx/marks.txt), NMI, IRQ, DSP-1 (the translated firmware, inside
the others), rendering and the whole VBL handler. The report takes the
first and last pair while the main state equals --state (2 = race) and
divides the differences. Emulated time in the patched Hatari, not Falcon
figures.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smkx_render_profile import hatari_identity  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
UNIT_MS = 1000.0 / 38400.0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", type=Path, default=ROOT / "build/scivx/hatari-stderr.log")
    ap.add_argument("--state", type=lambda v: int(v, 0), default=2)
    args = ap.parse_args()
    state = None
    pts = []
    for line in args.log.read_bytes().decode("latin1").splitlines():
        m = re.search(r"SMKX: frame ([0-9A-F]{8}) nmi ([0-9A-F]{8}) state ([0-9A-F]{8})", line)
        if m:
            state = (int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16))
            continue
        m = re.match(r"SMKX time ([0-9A-F]{8})((?: [0-9A-F]{8}){7})", line.strip())
        if m and state and state[2] == args.state:
            v = [int(x, 16) for x in m.group(2).split()]
            pts.append((int(m.group(1), 16), state[1], v))
    if len(pts) < 2:
        raise SystemExit(f"fewer than two timing lines in state {args.state}")
    (f0, n0, a), (f1, n1, b) = pts[0], pts[-1]
    d = [y - x for x, y in zip(a, b)]
    frames, nmis, mains = f1 - f0, n1 - n0, d[0]
    print(f"state {args.state}, frames {f0}..{f1}: {frames} VBLs, {nmis} NMIs, {mains} main-loop frames")
    print(f"emulated time, {hatari_identity()}")
    per = lambda x, n: x * UNIT_MS / n if n else 0.0  # noqa: E731
    print(f"  main loop       {per(d[1], mains):7.2f} ms per main-loop frame (interrupts excluded)")
    print(f"  NMI             {per(d[2], nmis):7.2f} ms per NMI")
    print(f"  IRQs            {per(d[3], nmis):7.2f} ms per NMI frame")
    print(f"  DSP-1 firmware  {per(d[4], mains):7.2f} ms per main-loop frame (inside the above)")
    print(f"  rendering       {d[5] * UNIT_MS:9.1f} ms in total")
    game = per(d[1], mains) + per(d[2], nmis) + per(d[3], nmis)
    print(f"  game code       {game:7.2f} ms per game frame (main loop + NMI + IRQs)")
    print(f"  => at most {1000.0 / game:5.1f} game frames per second with no rendering at all")


if __name__ == "__main__":
    main()
