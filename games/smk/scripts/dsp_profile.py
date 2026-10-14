#!/usr/bin/env python3
"""Where the DSP spends its cycles: Hatari's DSP profiler over a program run.

    python3 scripts/dsp_profile.py PROGRAM.TOS STOP_OFFSET [--listing build/smkx/ppu.lst]
                                   [--idle 51,6a,6e] [--top 60] [--start OFFSET]

Profiling starts with the emulator; it is read at a CPU breakpoint on the
program's text offset STOP_OFFSET (hex, e.g. PPUTEST's sound_release from a
vlink -M map), because Hatari evaluates the profile only when it enters the
debugger. With --start, profiling is switched on only at a first breakpoint
on that offset (PPUTEST's pt_prof_start in PT_PROF builds: the snapshot's
frames without its VRAM upload). The text base is the one TOS gives SMKX programs ($1F0C4).

Prints the hottest DSP addresses with their listing text, as shares of all
cycles outside the idle addresses (hex, comma-separated: by default the
bootstrap's wait at $0051 and ppu.asm's host-port waits). Emulated cycles
(patched Hatari), for finding hot loops, not for Falcon timing.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEXT_BASE = 0x1F0C4


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("program", type=Path)
    ap.add_argument("stop", type=lambda v: int(v, 16))
    ap.add_argument("--listing", type=Path, default=ROOT / "build/smkx/ppu.lst")
    ap.add_argument("--idle", default="51,6a,6e")
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--tag", default="dprof")
    ap.add_argument("--start", type=lambda v: int(v, 16),
                    help="text offset where profiling starts (default: from the emulator's start)")
    args = ap.parse_args()
    tmp = Path(tempfile.mkdtemp())
    (tmp / "bp.txt").write_text("dspprofile cycles 8000\nc\n")
    stop = f"b pc = ${TEXT_BASE + args.stop:x} :once :file {tmp}/bp.txt\n"
    if args.start is None:
        (tmp / "start.txt").write_text("dspprofile on\n" + stop)
    else:
        (tmp / "on.txt").write_text("dspprofile on\nc\n")
        (tmp / "start.txt").write_text(f"b pc = ${TEXT_BASE + args.start:x} :once :file {tmp}/on.txt\n" + stop)
    subprocess.run([sys.executable, str(ROOT / "scripts/run_smkx.py"), "--program", str(args.program),
                    "--tag", args.tag, "--grep", "PPUTEST", "--fast", "--timeout", "1500",
                    "--parse", str(tmp / "start.txt")], capture_output=True)
    log = (ROOT / f"build/smkx/{args.tag}-stderr.log").read_text(errors="replace").splitlines()
    rows, on = [], False
    for line in log:
        if line.startswith("addr:"):
            on = True
            continue
        m = re.match(r"0x([0-9a-f]+)\s+[\d.]+%\s+(\d+)", line)
        if on and m:
            rows.append((int(m.group(2)), int(m.group(1), 16)))
    idle = {int(x, 16) for x in args.idle.split(",") if x}
    rows = [(c, a) for c, a in rows if a not in idle]
    if not rows:
        raise SystemExit("no profile (did the breakpoint hit?)")
    text: dict[int, list[str]] = {}
    for line in args.listing.read_text(errors="replace").splitlines():
        m = re.match(r"\s*\d+\s+P:([0-9A-F]{4})\s+(?:P:[0-9A-F]{4}\s+)?(?:[0-9A-F]{6}\s+)?(.*)$", line)
        if m:
            text.setdefault(int(m.group(1), 16), []).append(re.sub(r"\s+", " ", m.group(2))[:48])
    rows.sort(reverse=True)
    total = sum(c for c, _ in rows)
    print(f"{total} cycles outside the idle addresses")
    acc = 0
    for c, a in rows[:args.top]:
        acc += c
        print(f"{a:04X} {100 * c / total:5.2f}% {' | '.join(text.get(a, ['?']))[:100]}")
    print(f"top {args.top}: {100 * acc / total:.1f}%")


if __name__ == "__main__":
    main()
