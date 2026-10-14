#!/usr/bin/env python3
"""The picture the patched Hatari's Videl shows at a frame, against the host reference of the same frame.

    scripts/lc_shot.py FRAME [DIR] [make variables...]

Builds FF6X.TOS with FF6X_FRAMES=FRAME FF6X_HOLD=1 (at the frame limit the runtime writes SNAP.BIN and stays, its video up), runs it in
F030Arcade's patched Hatari (own copy in DIR, default build/lc/shot), waits for "holding the picture", takes a screenshot
through the command FIFO and quits Hatari (its own process only). Then the screenshot's picture (found by its borders) is
compared with the reference render of SNAP.BIN (build/ppu_snap's PPM): for the compositor's frames that checks HSCROLL, the line
offset and the base address of the ring window as the Videl shows them, not only the page bytes.
Emulated; the Hatari build identity is printed."""
import re
import stat
import subprocess
import sys
import time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hatari = root.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
tos = root.parents[2] / "F030Arcade/third_party/tos/tos404.img"
args = [a for a in sys.argv[1:] if "=" not in a]
extra = [a for a in sys.argv[1:] if "=" in a]
frame = int(args[0])
d = Path(args[1]) if len(args) > 1 else root / "build/lc/shot"
d.mkdir(parents=True, exist_ok=True)
subprocess.run(["make", "ff6x", f"FF6X_FRAMES={frame}", "FF6X_HOLD=1", "FF6X_SNAP_EVERY=0", *extra], cwd=root, check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for f in d.glob("*.png"):
    f.unlink()
for f in ("SNAP.BIN", "run.log", "trace.log", "hold.fifo"):
    (d / f).unlink(missing_ok=True)
(d / "FF6X.TOS").write_bytes((root / "build/ff6x/FF6X.TOS").read_bytes())
(d / "FF6X.TOS").chmod(0o755)
fifo = d / "hold.fifo"
log = d / "run.log"
cmd = [str(hatari), "--machine", "falcon", "--dsp", "emu", "--memsize", "14", "--ttram", "0", "--tos", str(tos), "--fast-boot", "true",
       "--cpu-exact", "true", "--compatible", "true", "--patch-tos", "true", "--monitor", "rgb", "--sound", "off", "--natfeats", "true",
       "--confirm-quit", "off", "--alert-level", "fatal", "--fast-forward", "true", "--statusbar", "false", "--borders", "false",
       "--trace", "event", "--trace-file", str(d / "trace.log"), "--cmd-fifo", str(fifo), "--screenshot-dir", str(d),
       "--screenshot-format", "png", str(d / "FF6X.TOS")]
with log.open("w") as err:
    proc = subprocess.Popen(cmd, cwd=root, stdout=err, stderr=subprocess.STDOUT)
t0 = time.monotonic()
try:
    while time.monotonic() - t0 < 3000:
        text = log.read_text(errors="replace")
        if "holding the picture" in text or proc.poll() is not None:
            break
        time.sleep(1)
    time.sleep(2)                        # the Videl shows the held picture for a few frames
    with fifo.open("w") as f:
        f.write(f"hatari-debug screenshot {d / 'shot.png'}\n")
    time.sleep(2)
    with fifo.open("w") as f:
        f.write("hatari-debug quit\n")
    proc.wait(timeout=20)
finally:
    if proc.poll() is None:
        proc.kill()
m = re.search(r"INFO : (Hatari v[^\n]*)", log.read_text(errors="replace"))
print(m.group(1) if m else "Hatari (version line not found)")
shot = d / "shot.png"
if not shot.exists() or not (d / "SNAP.BIN").exists():
    sys.exit("no screenshot or no SNAP.BIN in " + str(d))
# the snapshot's picture (planes, CGRAM, INIDISP) as the Falcon's palette shows it
from PIL import Image, ImageChops
snap = (d / "SNAP.BIN").read_bytes()
off = 12 + 65536
cgram = snap[off:off + 512]
off += 512 + 544
io = snap[off:off + 0x6000]
off += 0x6000 + 16 + 12 + 4 + 4 + 256 * 6 + 131072 + 8192
planes = int.from_bytes(snap[off:off + 4], "big")
assert planes == 8, f"snapshot has {planes} planes"
pl = snap[off + 4:off + 4 + 57344]
bright = 0 if io[0x100] & 0x80 else io[0x100] & 15
pal = []
for i in range(256):
    c = cgram[i * 2] | cgram[i * 2 + 1] << 8
    pal.append(tuple(round(((c >> (5 * k)) & 31) * 63 * bright / (31 * 15)) * 255 // 63 for k in range(3)))
exp = Image.new("RGB", (256, 224))
px = []
for g in range(224 * 16):
    w = pl[g * 16:g * 16 + 16]
    words = [w[b * 2] << 8 | w[b * 2 + 1] for b in range(8)]
    for bit in range(15, -1, -1):
        v = 0
        for b in range(8):
            v |= (words[b] >> bit & 1) << b
        px.append(pal[v])
exp.putdata(px)
exp.save(d / "ref.png")
img = Image.open(shot).convert("RGB")
W, H = img.size
print("screenshot", W, "x", H)
best = None
for oy in range(0, H - 224 + 1):
    for ox in range(0, W - 256 + 1):
        diff = ImageChops.difference(img.crop((ox, oy, ox + 256, oy + 224)), exp).convert("L")
        n = 256 * 224 - diff.point(lambda v: 255 if v <= 4 else 0).histogram()[255]    # pixels off by more than 4 (grey of the difference)
        if best is None or n < best[0]:
            best = (n, ox, oy)
print(f"best placement ({best[1]},{best[2]}): {best[0]} of {256 * 224} pixels differ from the snapshot's picture (Hatari expands the 6-bit palette its own way)")
sys.exit(0 if best[0] == 0 else 1)
