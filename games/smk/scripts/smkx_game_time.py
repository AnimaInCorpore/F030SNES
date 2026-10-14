#!/usr/bin/env python3
"""Game-code time per frame from the "SMKX time" lines of a run.

    python3 scripts/smkx_game_time.py [build/smkx/hatari-stderr.log] [--state 2]

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
    ap.add_argument("log", nargs="?", type=Path, default=ROOT / "build/smkx/hatari-stderr.log")
    ap.add_argument("--state", type=lambda v: int(v, 0), default=2)
    args = ap.parse_args()
    state = None
    pts = []
    for line in args.log.read_bytes().decode("latin1").splitlines():
        m = re.search(r"SMKX: frame ([0-9A-F]{8}) nmi ([0-9A-F]{8}) state ([0-9A-F]{8})", line)
        if m:
            r = re.search(r"renders/ticks ([0-9A-F]{8})", line)
            state = (int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16),
                     int(r.group(1), 16) if r else 0)
            continue
        m = re.match(r"SMKX time ([0-9A-F]{8})((?: [0-9A-F]{8}){7,16})", line.strip())
        if m and state and state[2] == args.state:
            v = [int(x, 16) for x in m.group(2).split()]
            pts.append((int(m.group(1), 16), state[1], v, state[3]))
    if len(pts) < 2:
        raise SystemExit(f"fewer than two timing lines in state {args.state}")
    (f0, n0, a, r0), (f1, n1, b, r1) = pts[0], pts[-1]
    d = [y - x for x, y in zip(a, b)]
    frames, nmis, mains = f1 - f0, n1 - n0, d[0]
    print(f"state {args.state}, frames {f0}..{f1}: {frames} frames, {nmis} NMIs, {mains} main-loop frames")
    print(f"emulated time, {hatari_identity()}")
    per = lambda x, n: x * UNIT_MS / n if n else 0.0  # noqa: E731
    print(f"  main loop       {per(d[1], mains):7.2f} ms per main-loop frame (interrupts excluded)")
    print(f"  NMI             {per(d[2], nmis):7.2f} ms per NMI")
    print(f"  IRQs            {per(d[3], nmis):7.2f} ms per NMI frame")
    print(f"  DSP-1 firmware  {per(d[4], mains):7.2f} ms per main-loop frame (inside the above)")
    print(f"  rendering       {d[5] * UNIT_MS:9.1f} ms in total, {r1 - r0} renders, "
          f"{per(d[5], r1 - r0):7.1f} ms per render")
    # per main-loop frame: its own time plus the interrupts of the VBLs it spans
    game = (d[1] + d[2] + d[3]) * UNIT_MS / mains if mains else 0.0
    print(f"  game code       {game:7.2f} ms per main-loop frame (main loop + NMI + IRQs of its VBLs)")
    print(f"  DSP-1 firmware  {per(d[4], mains):7.2f} ms per main-loop frame")
    if len(d) >= 10:
        print(f"  native raster   {d[7]} lines, {per(d[8], d[7]) * 1000:7.1f} us per line, "
              f"{per(d[8], mains):7.2f} ms per main-loop frame")
        print(f"  firmware runs   {d[9]} ({d[9] / mains if mains else 0:.0f} per main-loop frame), "
              f"{per(d[4] - d[8], d[9]) * 1000:7.1f} us each outside the native path")
    print(f"  => at most {1000.0 / game if game else 0:5.1f} game frames per second with no rendering at all")
    if len(d) >= 12:
        print(f"  waiting         {per(d[11], mains):7.2f} ms per main-loop frame at the NMI wait")
    if len(d) >= 16:
        n = r1 - r0
        print(f"  per picture: palette {per(d[12], n):6.1f} ms, samples {per(d[13], n):5.1f} ms, "
              f"uploads {per(d[14], n):5.1f} ms, stream {per(d[15], n):6.1f} ms (68030)")
    if len(d) >= 11 and d[10]:
        secs = d[10] / 50.0
        print(f"  over {d[10]} VBLs ({secs:.1f} s at 50 Hz): {mains / secs:5.2f} main-loop frames/s, "
              f"{(r1 - r0) / secs:5.2f} frames shown/s ({1000.0 * secs / (r1 - r0) if r1 > r0 else 0:.0f} ms apart)")


if __name__ == "__main__":
    main()
