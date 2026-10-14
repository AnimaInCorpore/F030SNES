#!/usr/bin/env python3
"""Port snapshots against snes9x run on the same scripted input, by NMI count.

    python3 scripts/s9x_lock.py "K:PAD,K:PAD,..." [frames] [snap_every]

Builds SCIVX with SCIVX_INPUT (keyed by the NMIs the game has had), runs it in
Hatari (F*.BIN snapshots), runs snes9x (build/s9x_ref -wram) with S9X_INPUT
keyed the same way, and prints the game's state variables (WRAM $3A frame
counter, $32 main state, $1C00 scene step, $1E84 BG mode shadow) for both at
equal NMI counts.
"""
import glob, os, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
inp = sys.argv[1] if len(sys.argv) > 1 else ""
frames = sys.argv[2] if len(sys.argv) > 2 else "5000"
every = sys.argv[3] if len(sys.argv) > 3 else "250"
for f in glob.glob(str(ROOT / "build/scivx/F*.BIN")):
    os.remove(f)
subprocess.run(["make", "scivx", f"SCIVX_FRAMES={frames}", f"SCIVX_SNAP_EVERY={every}", f"SCIVX_INPUT={inp}"],
               cwd=ROOT, check=True, capture_output=True)
subprocess.run([sys.executable, "scripts/run_smkx.py", "--program", "build/scivx/SCIVX.TOS", "--fast",
                "--timeout", "900", "--tag", "lock"], cwd=ROOT, capture_output=True)
log = (ROOT / "build/scivx/lock-stderr.log").read_text(errors="replace")
fr2n = {int(m.group(1), 16): int(m.group(2), 16)
        for m in re.finditer(r"SMKX: frame ([0-9A-F]{8}) vbl [0-9A-F]{8} nmi ([0-9A-F]{8})", log)}
off = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 + 256 * 6


def var(w):
    g = lambda a: w[a] | w[a + 1] << 8
    return "3A=%04x 32=%04x 1C00=%04x 1E84=%02x" % (g(0x3A), g(0x32), g(0x1C00), w[0x1E84])


port = {}
for f in sorted(glob.glob(str(ROOT / "build/scivx/F*.BIN"))):
    fr = int(Path(f).stem[1:], 16)
    n = fr2n.get(fr)
    if n is not None:
        port[n] = (fr, var(Path(f).read_bytes()[off:off + 131072]))
ks = sorted(port)
(ROOT / "build/s9x_w").mkdir(exist_ok=True)
env = dict(os.environ, S9X_INPUT=inp)
subprocess.run([str(ROOT / "build/s9x_ref"), str(ROOT / "build/snes9x/src/libretro/snes9x_libretro.dylib"),
                str(ROOT / "build/rom/Super Castlevania IV (USA).sfc"), str(ROOT / "build/s9x_w"), "-wram",
                *map(str, ks)], env=env, capture_output=True)
for k in ks:
    p = ROOT / f"build/s9x_w/w{k:06d}.bin"
    s = var(p.read_bytes()) if p.exists() else "-"
    flag = "" if s == port[k][1] else "   <-- differs"
    print(f"K={k:5} port(frame {port[k][0]:5}) {port[k][1]} | s9x {s}{flag}")
