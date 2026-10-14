#!/usr/bin/env python3
"""Plays a walker stream (build/lc/smw.lcw, tools/lc/lcd_model with LCD_REC) on falcon/lcwalk.s in the
patched Hatari (through scripts/run_smkx.py).

  run_lcwalk.py check [STREAM]          every frame's window checksum against the stream's; status 1 on a mismatch
  run_lcwalk.py shot N [STREAM]         the Videl's picture of frame N (1-based) in a Hatari screenshot against the
                                        reference picture with the stream's palette (tools/lc/shot_compare.py)
  run_lcwalk.py bench [STREAM] [PASSES] time per frame: all ops, then STORE and COPY records read past one at
                                        a time, then both (the loop and the stream parse only)

Emulator timing only (Hatari's 030, caches and bus are not a Falcon's); the build is printed beside it."""
from __future__ import annotations
import re, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCADE = ROOT.parents[2] / "F030Arcade/third_party"
LC = ROOT / "build/lcw"
LOGS = ROOT / "build/smwx"


def build(name, stream, defs):
    LC.mkdir(parents=True, exist_ok=True)
    (LC / "stream.lcw").write_bytes(stream.read_bytes())
    subprocess.run([str(ARCADE / "vasm/vasmm68k_mot"), "-Felf", "-m68030", "-quiet", "-nowarn=62", "-nowarn=2058",
                    f"-I{LC}", *defs, "-o", str(LC / f"{name}.o"), str(ROOT / "falcon/lcwalk.s")], check=True)
    subprocess.run([str(ARCADE / "vlink/vlink"), "-b", "ataritos", "-tos-flags", "1", "-o", str(LC / f"{name}.TOS"),
                    str(LC / f"{name}.o")], check=True, capture_output=True)
    return LC / f"{name}.TOS"


def run(tos, tag):
    out = subprocess.run([sys.executable, str(ROOT / "scripts/run_smkx.py"), "--program", str(tos), "--fast",
                          "--timeout", "900", "--tag", tag, "--grep", "LCWALK"], capture_output=True, text=True,
                         cwd=ROOT).stdout
    log = (LOGS / f"{tag}-stderr.log").read_text(errors="replace")
    m = re.search(r"INFO : (Hatari v[^\n]*)", log)
    return out, (m.group(1) if m else "Hatari (version not found)")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("check", "bench", "shot"):
        print(__doc__); return 2
    if sys.argv[1] == "shot":
        n = int(sys.argv[2])
        stream = Path(sys.argv[3]) if len(sys.argv) > 3 else ROOT / "build/lc/smw.lcw"
        tos = build(f"shot{n}", stream, [f"-DSTOPAT={n}", "-DLOOP=0"])
        png = LC / f"shot{n}.png"
        png.unlink(missing_ok=True)
        subprocess.run([sys.executable, str(ROOT / "scripts/run_smkx.py"), "--program", str(tos), "--fast", "--timeout", "30",
                        "--tag", f"shot{n}", "--grep", "NONE", "--screenshot", str(png)], capture_output=True, cwd=ROOT)
        window = (ROOT / "build/lc/window.txt").read_text().split()
        ppm = LC / f"ref{n}.ppm"
        env = dict(__import__("os").environ, LCD_PPM=str(n), LCD_PPM_FILE=str(ppm), LC_SMW="1")
        subprocess.run([str(ROOT / "build/lc/lcd_model"), str(ROOT / "build/smwx/smw.sfc"), *window], env=env,
                       capture_output=True, cwd=ROOT, check=True)
        return subprocess.run([sys.executable, str(ROOT / "tools/lc/shot_compare.py"), str(png), str(ppm)], cwd=ROOT).returncode
    stream = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "build/lc/smw.lcw"
    frames = struct.unpack(">I", stream.read_bytes()[4:8])[0]
    if sys.argv[1] == "check":
        out, hat = run(build("wchk", stream, ["-DCHECK=1", "-DLOOP=0"]), "wchk")
        print(out, hat)
        m = re.search(r"frames (\d+) window checksum mismatches (\d+)", out)
        return 0 if m and int(m[1]) == frames and int(m[2]) == 0 else 1
    passes = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    base = ["-DTIMING=1", f"-DPASSES={passes}", "-DCHECK=0", "-DLOOP=0"]
    var = [("all", []), ("no store", ["-DSKIPSTORE=1"]), ("no copy", ["-DSKIPCOPY=1"]),
           ("neither", ["-DSKIPSTORE=1", "-DSKIPCOPY=1"])]
    res = {}
    for i, (lab, extra) in enumerate(var):
        out, hat = run(build(f"wt{i}", stream, base + extra), f"wt{i}")
        m = re.search(r"ticks\(200Hz\) total (\d+)", out)
        if not m: raise SystemExit("no timing line:\n" + out)
        res[lab] = int(m[1])
    per = lambda t: t * 5.0 / (passes * frames) * 1000  # 200 Hz ticks -> microseconds per frame
    print(f"{hat}; {frames} frames x {passes} passes, no vertical-blank wait")
    print(f"  all            {per(res['all']):8.0f} us per frame")
    print(f"  STORE records  {per(res['all'] - res['no store']):8.0f} us (all - no store)")
    print(f"  COPY records   {per(res['all'] - res['no copy']):8.0f} us (all - no copy)")
    print(f"  loop + parse   {per(res['neither']):8.0f} us (both skipped)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
