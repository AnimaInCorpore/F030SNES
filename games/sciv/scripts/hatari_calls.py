#!/usr/bin/env python3
"""Log WRAM at each call of a translated 65816 address, in the patched Hatari.

    scripts/hatari_calls.py C0046C --dump F0:16 --count 12

Sets a debugger breakpoint on the translated node of the 65816 address (the
first context that has one), and at each stop prints `--dump OFFSET:LENGTH`
of the 65816's WRAM (offsets are hex from $7E0000) and continues. The
runtime prints the host address of WRAM at start ("SMKX: WRAM at $...").
Build with SCIVX_FRAMES=0 or a large limit.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import smkx_pc  # noqa: E402

HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
TOS = ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"
PROGRAM = ROOT / "build/scivx/SCIVX.TOS"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", help="65816 address, hex")
    ap.add_argument("--dump", action="append", default=[], help="OFFSET:LENGTH of WRAM, hex (repeatable)")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--after", type=int, default=0, help="only from this VBL on")
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()
    r = subprocess.run([sys.executable, "scripts/run_smkx.py", "--fast", "--timeout", "120", "--grep", "WRAM at"],
                       cwd=ROOT, capture_output=True, text=True)
    wram = int(re.search(r"WRAM at \$([0-9A-F]{8})", r.stdout).group(1), 16)
    mp = (ROOT / "build/scivx/SCIVX.map").read_text(errors="replace")
    base = wram - int(re.search(r"0x([0-9a-f]+) smkx_wram:", mp).group(1), 16)
    labels, addr65, _ = smkx_pc.load()
    want = int(args.addr, 16)
    node = next(n for n, a in sorted(addr65.items(), key=lambda kv: labels[kv[0]]) if a == want)
    pc = base + labels[node]
    ini = ROOT / "build/scivx/calls.ini"
    ini.write_text(f"b pc = ${pc:08X}" + (f" && VBL > {args.after}" if args.after else "") + "\n")
    cmd = [str(HATARI), "--machine", "falcon", "--dsp", "emu", "--memsize", "14", "--ttram", "0", "--tos", str(TOS),
           "--fast-boot", "true", "--cpu-exact", "true", "--compatible", "true", "--patch-tos", "true",
           "--monitor", "rgb", "--sound", "off", "--natfeats", "true", "--confirm-quit", "off", "--alert-level", "fatal",
           "--fast-forward", "true", "--parse", str(ini), str(PROGRAM)]
    log = ROOT / "build/scivx/calls.log"
    f = log.open("w")
    proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=f, stderr=subprocess.STDOUT, text=True)
    seen = 0
    t0 = time.monotonic()
    pos = 0
    try:
        while seen < args.count and time.monotonic() - t0 < args.timeout and proc.poll() is None:
            text = log.read_text(errors="replace")
            n = text.count("condition(s) matched")
            if n > seen:
                time.sleep(0.3)
                for d in args.dump:
                    off, ln = (int(x, 16) for x in d.split(":"))
                    proc.stdin.write(f"m ${wram + off:08X} {ln}\n")
                proc.stdin.write("c\n")
                proc.stdin.flush()
                seen = n
                time.sleep(0.5)
            time.sleep(0.2)
    finally:
        if proc.poll() is None:
            proc.kill()
    text = log.read_text(errors="replace")
    frames = re.findall(r"VBL=(\d+)", text)
    for m in re.finditer(r"^([0-9A-F]{8}): ((?:[0-9a-f]{2} ){1,16})", text, re.M):
        print(f"{int(m.group(1), 16) - wram:05X}: {m.group(2)}")
    print("VBLs:", frames[:args.count])
    return 0


if __name__ == "__main__":
    sys.exit(main())
