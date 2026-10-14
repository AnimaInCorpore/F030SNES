#!/usr/bin/env python3
"""Run build/ctx/CTX.TOS in F030Arcade's patched Hatari and print its log.

The runtime reports over NatFeats (NF_STDERR). It quits by itself after the
frame limit it was built with, or at the first trap; this script then stops
Hatari. Use only the patched Hatari named in AGENTS.md.
"""

from __future__ import annotations

import argparse
import os
import stat
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
TOS = ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"
PROGRAM = ROOT / "build/ctx/CTX.TOS"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--screenshot", type=Path)
    ap.add_argument("--fast", action="store_true", help="--fast-forward on")
    ap.add_argument("--program", type=Path, default=PROGRAM)
    ap.add_argument("--tag", default="hatari", help="prefix of the log, trace and FIFO files")
    ap.add_argument("--grep", default="SMKX", help="print log lines containing this")
    args = ap.parse_args()
    hatari = HATARI if HATARI.is_file() else HATARI.with_suffix(".exe")      # the same patched build, on Windows
    if not hatari.is_file():
        raise SystemExit(f"patched F030Arcade Hatari missing: {HATARI}")
    out = ROOT / "build/ctx"
    fifo = out / f"{args.tag}.fifo"
    trace = out / f"{args.tag}-trace.log"
    log = out / f"{args.tag}-stderr.log"
    windows = os.name == "nt"               # no FIFOs: Hatari is stopped by its PID instead
    program = args.program
    if windows:
        # The Windows build takes no program path: the program goes into a
        # GEMDOS drive's AUTO folder and the drive's directory is the argument.
        import shutil
        drive = out / f"{args.tag}-drive"
        (drive / "AUTO").mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.program, drive / "AUTO" / "CTX.PRG")
        program = drive
    if fifo.exists() and not windows:
        if not stat.S_ISFIFO(fifo.stat().st_mode):
            raise SystemExit(f"refusing to replace non-FIFO {fifo}")
        fifo.unlink()
    for f in (trace, log):
        if f.exists():
            f.unlink()
    command = [
        str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14",
        "--ttram", "0", "--tos", str(TOS), "--fast-boot", "true",
        "--cpu-exact", "true", "--compatible", "true", "--patch-tos", "true",
        "--monitor", "rgb", "--sound", "off", "--natfeats", "true",
        "--confirm-quit", "off", "--alert-level", "fatal",
        "--fast-forward", "true" if args.fast else "false",
        "--trace", "event", "--trace-file", str(trace),
        *(["--debug-except", "none"] if windows else ["--cmd-fifo", str(fifo)]),
        "--screenshot-dir", str(out), str(program),
    ]
    command[0] = str(hatari)
    with log.open("w") as err:
        proc = subprocess.Popen(command, cwd=ROOT, stdout=err, stderr=subprocess.STDOUT)
    started = time.monotonic()
    status = 1
    try:
        while time.monotonic() - started < args.timeout:
            text = trace.read_text(errors="replace") if trace.exists() else ""
            if "Program termination" in text or proc.poll() is not None:
                status = 0
                break
            time.sleep(0.5)
        else:
            print(f"timeout after {args.timeout:.0f}s", file=sys.stderr)
        if args.screenshot and proc.poll() is None and not windows:
            with fifo.open("w") as f:
                f.write(f"hatari-debug screenshot {args.screenshot}\n")
            time.sleep(1.0)
        if proc.poll() is None and windows:
            proc.terminate()                # this PID only (other sessions run Hatari too)
            proc.wait(timeout=10)
        if proc.poll() is None and fifo.exists():
            with fifo.open("w") as f:
                f.write("hatari-debug quit\n")
            proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
    lines = [l for l in log.read_text(errors="replace").splitlines() if args.grep in l]
    print("\n".join(lines))
    return status


if __name__ == "__main__":
    sys.exit(main())
