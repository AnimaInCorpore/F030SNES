#!/usr/bin/env python3
"""Run FF6X.TOS in the patched Hatari until a byte of the 65816's WRAM takes a
value it should not, then print the 68030 registers and the translated
65816 site.

    scripts/hatari_break.py 0001 "!= 0"       # DP $01 must stay 0
    scripts/hatari_break.py 1127 "> 8" --timeout 600

Uses the debugger's conditional breakpoint on the host address of the byte
(the runtime prints it at start: "SMKX: WRAM at $..."); stdin feeds the
debugger prompt. Build with FRAMES=0 (no limit) or a large one.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import smkx_pc  # noqa: E402

HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
TOS = ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"
PROGRAM = ROOT / "build/ff6x/FF6X.TOS"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("offset", help="WRAM offset, hex (0001 = $7E0001), or 'vbl' with cond '> N': stop after N VBLs")
    ap.add_argument("cond", help='condition on the byte, e.g. "! 0" (the debugger has = ! < > only)')
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--wram", help="host address of WRAM if it changed (hex)")
    ap.add_argument("--extra", default="", help="more debugger commands after the break, ';'-separated; WRAM+HEX is the host address of that WRAM offset")
    args = ap.parse_args()
    wram = int(args.wram, 16) if args.wram else None
    if wram is None:
        r = subprocess.run([sys.executable, "scripts/run_smkx.py", "--fast", "--timeout", "120", "--grep", "WRAM at"],
                           cwd=ROOT, capture_output=True, text=True, env={**__import__("os").environ})
        m = re.search(r"WRAM at \$([0-9A-F]{8})", r.stdout)
        wram = int(m.group(1), 16)
    addr = None if args.offset == "vbl" else wram + int(args.offset, 16)
    script = ROOT / "build/ff6x/break.ini"
    script.write_text("hi cpu 64\n" + (f"b VBL {args.cond}\n" if addr is None else f"b (${addr:08X}).b {args.cond}\n"))
    out = ROOT / "build/ff6x"
    cmd = [str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14", "--ttram", "0", "--tos", str(TOS),
           "--fast-boot", "true", "--cpu-exact", "true", "--compatible", "true", "--patch-tos", "true",
           "--monitor", "rgb", "--sound", "off", "--natfeats", "true", "--confirm-quit", "off", "--alert-level", "fatal",
           "--fast-forward", "true", "--parse", str(script), str(PROGRAM)]
    log = out / "break.log"
    with log.open("w") as f:
        proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=f, stderr=subprocess.STDOUT, text=True)
    t0 = time.monotonic()
    hit = False
    while time.monotonic() - t0 < args.timeout and proc.poll() is None:
        text = log.read_text(errors="replace")
        if "condition(s) matched" in text or "Breakpoint" in text and "matched" in text:
            hit = True
            break
        time.sleep(0.5)
    if hit:
        for c in ["r", "hi 48", f"m ${wram + 0xF0:08X} 16", *[re.sub(r"WRAM\+([0-9A-Fa-f]+)", lambda m: f"${wram + int(m.group(1), 16):08X}", x)
                                                                 for x in args.extra.split(";") if x], "q"]:
            proc.stdin.write(c + "\n")
            proc.stdin.flush()
            time.sleep(0.5)
        time.sleep(1)
    if proc.poll() is None:
        proc.kill()
    text = log.read_text(errors="replace")
    print(text[-6000:])
    # text base: the host address of smkx_wram minus its link-map offset
    mp = (ROOT / "build/ff6x/FF6X.map").read_text(errors="replace")
    off = int(re.search(r"0x([0-9a-f]+) smkx_wram:", mp).group(1), 16)
    base = wram - off
    print(f"text base ${base:08X}; runtime offset = pc - base (scripts/smkx_pc.py maps it to the 65816 site)")
    return 0 if hit else 1


if __name__ == "__main__":
    sys.exit(main())
