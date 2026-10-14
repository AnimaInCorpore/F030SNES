#!/usr/bin/env python3
"""Game frame rate per scene from the status lines of a CT run.

    python3 scripts/ff6x_fps.py LOG                  # list the scenes
    python3 scripts/ff6x_fps.py LOG 9F6-EA6,1C20-2580

The status line (every 50 frames) gives handled frames, Falcon VBLs, NMIs
and the PPU registers ($2100 INIDISP, $2105 BGMODE, $212C TM); the
"SMKX time" line after it the cumulative clocks in 1/38400 s: main-loop
passes and their time without interrupts (marks in tools/smkx/marks.txt:
the menu shell's scheduler and Field_MainLoop), NMI, IRQ, DSP-1, render and
the whole VBL handler.

Without ranges it lists where INIDISP/BGMODE/TM change (the value is the
one seen at the listed frame). With ranges (hex frame numbers) it reports,
per range: game frames per second (one per handled VBL, 50 at most), the
per-frame milliseconds, and the main-loop passes per second with their own
milliseconds; with rendering on, a second line: DSP frames and the VBLs
between them, the 68030's planning, upload and send per DSP frame, and the
pictures shown per second (the "ppudsp" and "renders/ticks" fields; the
planning time is wall time when the work runs in the game's idle time).
Emulated time in the patched Hatari, not Falcon figures.
"""

from __future__ import annotations

import re
import sys


def load(path: str):
    status, times, dsp = {}, {}, {}
    for line in open(path, errors="replace"):
        m = re.search(r"SMKX: frame ([0-9A-F]{8}) vbl ([0-9A-F]{8}) nmi ([0-9A-F]{8}).*? ppu ([0-9A-F]{8})", line)
        if m:
            status[int(m[1], 16)] = (int(m[2], 16), int(m[3], 16), m[4])
            d = re.search(r"ppudsp ([0-9A-F/]+).*?renders/ticks ([0-9A-F]+)/([0-9A-F]+)", line)
            if d and "still running" not in line:
                dsp[int(m[1], 16)] = ([int(x, 16) for x in d[1].strip("/").split("/")], int(d[3], 16),
                                      int(d[2], 16))
        m = re.search(r"SMKX time ((?:[0-9A-F]{8} ?){8})", line)
        if m:
            v = [int(x, 16) for x in m[1].split()]
            times[v[0]] = v
    return status, times, dsp


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    status, times, dsp = load(sys.argv[1])
    if not status:
        raise SystemExit("no status lines")
    if len(sys.argv) < 3:
        last = None
        for f in sorted(status):
            ppu = status[f][2]
            if ppu != last:
                print(f"{f:05X} ppu {ppu}")
                last = ppu
        print(f"{max(status):05X} (last)")
        return
    near = lambda d, k: min(d, key=lambda x: abs(x - k))
    for r in sys.argv[2].split(","):
        a, b = (int(x, 16) for x in r.split("-"))
        a, b = near(status, a), near(status, b)
        ta, tb = near(times, a), near(times, b)
        frames, vbls = b - a, status[b][0] - status[a][0]
        S, T = times[ta], times[tb]
        n = tb - ta
        ms = lambda i: (T[i] - S[i]) / 38.4 / n
        out = (f"{a:05X}-{b:05X} ppu {status[b][2]}: {50 * frames / vbls:.1f} fps; ms per frame: "
               f"NMI {ms(3):.1f}, IRQ {ms(4):.1f}, render {ms(6):.1f}, VBL handler {ms(7):.1f}")
        passes = T[1] - S[1]
        if passes:
            tv = status[tb][0] - status[ta][0]
            out += f"; main loop {50 * passes / tv:.1f}/s, {(T[2] - S[2]) / 38.4 / passes:.1f} ms each"
        print(out)
        if dsp:
            da, db = near(dsp, a), near(dsp, b)
            (pa, ta_, ra), (pb, tb_, rb) = dsp[da], dsp[db]
            n_dsp = pb[0] - pa[0]
            v = status[db][0] - status[da][0]
            if n_dsp > 0 and v > 0:
                per = lambda i: (pb[i] - pa[i]) / 38.4 / n_dsp
                print(f"  pictures {50 * (rb - ra) / v:.1f}/s; DSP frames {n_dsp}, one per {v / n_dsp:.1f} VBLs; "
                      f"per DSP frame: plan {per(3):.1f} ms, upload {per(4):.1f}, send {per(5):.1f}")


if __name__ == "__main__":
    main()
