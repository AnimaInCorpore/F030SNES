#!/usr/bin/env python3
"""Summarise the per-render profile lines of an SMKX_PROFILE=1 run.

    python3 scripts/smkx_render_profile.py [build/smkx/hatari-stderr.log]

Each render is composed twice from the same inputs: through the DSP Mode 7
path, then by the CPU alone (src/smkx_video.c). Times are emulated time
in the patched Hatari (1/38400 s units from _hz_200 and MFP Timer C), not
Falcon measurements; the Hatari build is printed with them.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
UNIT_MS = 1000.0 / 38400.0
FIELDS = ["palette", "dsp upload", "dsp ppu_render", "dsp wait", "dsp overlay",
          "cpu ppu_render", "cpu planar", "m7 rows", "port stalls", "differing words"]
STAGES = ["hdma", "bg layers", "mode 7 (C)", "sprites", "compose", "feed DSP"]
NFIELDS = 23


def hatari_identity() -> str:
    try:
        rev = subprocess.run(["git", "-C", str(HATARI.parents[2]), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        rev = "?"
    sha = hashlib.sha256(HATARI.read_bytes()).hexdigest() if HATARI.exists() else "missing"
    return f"patched Hatari {rev}, binary SHA-256 {sha[:8]}…{sha[-4:]}"


def main() -> None:
    log = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/smkx/hatari-stderr.log"
    rows = []
    for line in log.read_bytes().decode("latin1").splitlines():
        m = re.match(r"SMKX prof ([0-9A-F]{8})((?: [0-9A-F]{8}){%d})" % NFIELDS, line.strip())
        if m:
            rows.append((int(m.group(1), 16), [int(v, 16) for v in m.group(2).split()]))
    if not rows:
        raise SystemExit(f"{log}: no 'SMKX prof' lines (build with SMKX_PROFILE=1)")
    print(f"{len(rows)} renders; emulated time, {hatari_identity()}")
    groups = {"Mode 7 frames": [r for r in rows if r[1][7] > 0],
              "other frames": [r for r in rows if r[1][7] == 0]}
    for name, sel in groups.items():
        if not sel:
            continue
        n = len(sel)
        avg = [sum(r[1][i] for r in sel) / n for i in range(NFIELDS)]
        dsp = avg[0] + avg[1] + avg[2] + avg[3] + avg[4]
        cpu = avg[0] + avg[5] + avg[6]
        print(f"\n{name}: {n} (frames {sel[0][0]}..{sel[-1][0]})")
        for i in range(7):
            print(f"  {FIELDS[i]:16} {avg[i] * UNIT_MS:9.1f} ms")
        print(f"  {'m7 rows':16} {avg[7]:9.1f}")
        print(f"  {'port stalls':16} {avg[8]:9.0f} polls")
        print(f"  DSP path total   {dsp * UNIT_MS:9.1f} ms   (palette + upload + render + wait + overlay)")
        print(f"  CPU path total   {cpu * UNIT_MS:9.1f} ms   (palette + render + planar)")
        if avg[7]:
            print(f"  speed-up         {cpu / dsp:9.2f} x")
        print(f"  {'ppu_render stage':16} {'DSP path':>12} {'CPU only':>12}")
        for k, name in enumerate(STAGES):
            print(f"  {name:16} {avg[10 + k] * UNIT_MS:9.1f} ms {avg[16 + k] * UNIT_MS:9.1f} ms")
        if avg[7]:
            print(f"  kernel alone     {avg[22] * UNIT_MS:9.1f} ms   ({avg[22] * UNIT_MS * 1000 / avg[7]:.0f} us per Mode 7 row,"
                  f" incl. planning and host-port feeding)")
        bad = [r[0] for r in sel if r[1][9]]
        print(f"  frames where DSP and CPU planes differ: {len(bad)}" + (f" {bad[:8]}" if bad else ""))


if __name__ == "__main__":
    main()
