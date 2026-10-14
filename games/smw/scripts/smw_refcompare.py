#!/usr/bin/env python3
"""Compare the per-frame WRAM sums a SMKX_SYNC=1 SMKX_SUMS=1 build logged
with those of the reference interpreter (tools/ref65816.c).

    scripts/smw_refcompare.py build/smwx/hatari-stderr.log [SMWX_INPUT]

Runs build/ref65816 for as many frames as the log has and prints the first
frame whose sums differ, with the VBL at which the Falcon reached it (use
it as SMWX_FRAMES for a snapshot, then scripts/smw_wramdiff.py).
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    fal = {}
    for line in Path(sys.argv[1]).read_text(errors="replace").splitlines():
        m = re.search(r"SMKX sum ([0-9A-F]{8}) ([0-9A-F]{8}) ([0-9A-F]{8})", line)
        if m:
            fal[int(m.group(1), 16)] = (m.group(2), int(m.group(3), 16))
    if not fal:
        raise SystemExit("no 'SMKX sum' lines: build with SMWX_SYNC=1 SMWX_SUMS=1")
    frames = max(fal) + 1
    env = dict(__import__("os").environ)
    if len(sys.argv) > 2:
        env["REF_INPUT"] = sys.argv[2]           # the build's SMWX_INPUT
    out = subprocess.run([str(ROOT / "build/ref65816"), str(ROOT / "build/smwx/smw.sfc"),
                          str(ROOT / "third_party/SMWDisX/SMW_U.sym"), str(frames)],
                         capture_output=True, text=True, env=env)
    ref = {}
    for line in out.stdout.splitlines():
        if line.startswith("sum "):
            _, n, s = line.split()
            ref[int(n)] = s
    print(out.stderr.strip())
    common = sorted(set(ref) & set(fal))
    bad = [n for n in common if ref[n] != fal[n][0]]
    print(f"{len(common)} frames compared, {len(bad)} differ")
    if bad:
        n = bad[0]
        print(f"first difference: frame {n} (Falcon VBL {fal[n][1]}), "
              f"reference {ref[n]}, Falcon {fal[n][0]}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
