#!/usr/bin/env python3
"""Run FF6X.TOS until it stops at a pointer dispatch to an untranslated
target, add that target to the reporting site's list in
tools/smkx/dispatch_targets.txt, rebuild and run again.

    scripts/ff6x_seed_loop.py --frames 12000 --rounds 10

Stops at the first run that ends differently (frame limit, trap, timeout)
and prints how it ended. Uses the patched Hatari through run_smkx.py.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import smkx_pc  # noqa: E402

TARGETS = ROOT / "tools/smkx/dispatch_targets.txt"
MISS = re.compile(r"untranslated target \(ctx<<24\|addr\) \$([0-9A-F]{8})")
SITE = re.compile(r" from \+([0-9A-F]{8})")


def site_of(offset: int) -> int | None:
    """The 65816 address of the translated node at a text offset."""
    import bisect
    labels, addr65, _ = smkx_pc.load()
    nodes = sorted((a, n) for n, a in labels.items() if n in addr65)
    keys = [a for a, _ in nodes]
    i = bisect.bisect_right(keys, labels["start"] + offset) - 1
    return addr65[nodes[i][1]] if i >= 0 else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=12000)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--timeout", type=int, default=5000)
    ap.add_argument("make_args", nargs="*", help="extra make variables, e.g. FF6X_INPUT=3300:1000")
    args = ap.parse_args()
    for r in range(args.rounds):
        subprocess.run(["make", "ff6x", f"FF6X_FRAMES={args.frames}", "FF6X_RENDER=0", *args.make_args], cwd=ROOT,
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run = subprocess.run([sys.executable, "scripts/run_smkx.py", "--program", "build/ff6x/FF6X.TOS", "--fast",
                              "--timeout", str(args.timeout)], cwd=ROOT, capture_output=True, text=True)
        log = run.stdout + run.stderr
        (ROOT / "build/ff6x/seed-loop.log").write_text(log)
        m = MISS.search(log)
        frames = [l for l in log.splitlines() if l.startswith("SMKX: frame")]
        if not m:
            print(f"round {r}: no dispatch miss; last: {frames[-1][:160] if frames else '-'}")
            for l in log.splitlines():
                if any(k in l for k in ("TRAP", "exception", "unexpected", "UNSUPPORTED", "limit", "WRAM", "timeout")):
                    print("   ", l[:200])
            return 0
        target = int(m.group(1), 16) & 0xFFFFFF
        after = log[m.end():]
        s = SITE.search(after)
        site = site_of(int(s.group(1), 16)) if s else None
        if site is None:
            print(f"round {r}: miss ${target:06X} at an unknown site; stopping")
            return 1
        with TARGETS.open("a") as f:
            f.write(f"${site:06X}: ${target:06X}   # reported by the runtime (scripts/ff6x_seed_loop.py)\n")
        print(f"round {r}: ${site:06X} -> ${target:06X} ({frames[-1][6:40] if frames else ''})", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
