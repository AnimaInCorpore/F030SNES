#!/usr/bin/env python3
"""Plays a recorded list-compositor stream (build/tile/lc/stream.lcp, written by tools/tile/lc_model with
LC_REC) on falcon/lcplay.s in F030Arcade's patched Hatari (through scripts/run_smkx.py).

  run_lcplay.py check [STREAM]          every frame's page window summed and compared with the
                                        stream's checksum; exit status 1 on any mismatch
  run_lcplay.py bench [STREAM] [PASSES] time per update: the whole stream PASSES times (default 5)
                                        without the vertical-blank wait, then with BLIT, COPY and
                                        DATA records read past one kind at a time, and all three

Emulator timing only: Hatari's blitter, bus and 030 caches are not a Falcon's. The Hatari build
identity is printed beside the figures.
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCADE = ROOT.parents[2] / "F030Arcade/third_party"
VASM = ARCADE / "vasm/vasmm68k_mot"
VLINK = ARCADE / "vlink/vlink"
LC = ROOT / "build/tile/lc"
LOGS = ROOT / "build/smkx"


def build(name: str, stream: Path, defs: list[str]) -> Path:
    LC.mkdir(parents=True, exist_ok=True)
    (LC / "stream.lcp").write_bytes(stream.read_bytes())
    obj, tos = LC / f"{name}.o", LC / f"{name}.TOS"
    subprocess.run([str(VASM), "-Felf", "-m68030", "-quiet", "-nowarn=62", "-nowarn=2058", f"-I{LC}",
                    *defs, "-o", str(obj), str(ROOT / "falcon/tile/lcplay.s")], check=True)
    subprocess.run([str(VLINK), "-b", "ataritos", "-tos-flags", "1", "-o", str(tos), str(obj)],
                   check=True, capture_output=True)
    return tos


def run(tos: Path, tag: str) -> tuple[list[str], str]:
    out = subprocess.run([sys.executable, str(ROOT / "scripts/run_smkx.py"), "--program", str(tos), "--fast",
                          "--timeout", "900", "--tag", tag, "--grep", "LCPLAY"],
                         capture_output=True, text=True, cwd=ROOT).stdout.splitlines()
    log = (LOGS / f"{tag}-stderr.log").read_text(errors="replace")
    m = re.search(r"INFO : (Hatari v[^\n]*)", log)
    return out, m.group(1) if m else "Hatari (version line not found)"


def ticks(lines: list[str]) -> int:
    for line in lines:
        m = re.search(r"ticks\(200Hz\) total (\d+)", line)
        if m:
            return int(m.group(1))
    raise SystemExit("no timing line in the player's output:\n" + "\n".join(lines))


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in ("check", "bench"):
        print(__doc__)
        return 2
    mode = sys.argv[1]
    stream = Path(sys.argv[2]) if len(sys.argv) > 2 else LC / "smk.lcp"
    for f in (VASM, VLINK, stream):
        if not f.exists():
            raise SystemExit(f"missing {f}")
    frames = struct.unpack(">I", stream.read_bytes()[4:8])[0]
    if mode == "check":
        lines, hatari = run(build("lcchk", stream, ["-DCHECK=1", "-DLOOP=0"]), "lcchk")
        print("\n".join(lines))
        print(hatari)
        m = re.search(r"frames (\d+) window checksum mismatches (\d+)", "\n".join(lines))
        return 0 if m and int(m.group(1)) == frames and int(m.group(2)) == 0 else 1
    passes = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    base = ["-DTIMING=1", f"-DPASSES={passes}", "-DCHECK=0", "-DLOOP=0"]
    variants = [("all ops", []), ("no blit", ["-DSKIPBLIT=1"]), ("no copy", ["-DSKIPCOPY=1"]),
                ("no data", ["-DSKIPDATA=1"]), ("nothing drawn", ["-DSKIPBLIT=1", "-DSKIPCOPY=1", "-DSKIPDATA=1"])]
    res, hatari = {}, ""
    for i, (label, extra) in enumerate(variants):
        lines, hatari = run(build(f"lct{i}", stream, base + extra), f"lct{i}")
        res[label] = ticks(lines)
    per = lambda t: t * 5.0 / (passes * frames)          # 200 Hz ticks -> ms per update
    full = res["all ops"]
    print(f"{hatari}; {frames} updates x {passes} passes, no vertical-blank wait")
    print(f"  all ops          {per(full):6.2f} ms per update ({1000 / per(full):.0f} updates/s)")
    print(f"  blitter ops      {per(full - res['no blit']):6.2f} ms (all - no blit)")
    print(f"  restore copies   {per(full - res['no copy']):6.2f} ms (all - no copy)")
    print(f"  data writes      {per(full - res['no data']):6.2f} ms (all - no data)")
    print(f"  loop and Videl   {per(res['nothing drawn']):6.2f} ms (nothing drawn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
