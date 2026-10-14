#!/usr/bin/env python3
"""The port's snapshots against snes9x.

    python3 scripts/s9x_compare.py SNAPDIR OUTDIR [--range B A] [--sheet out.png N]

Per snapshot, build/ppu_rgb draws the Falcon's picture and our reference
(src/snes_ppu.c's ppu_render_rgb) into OUTDIR; build/s9x_ref runs snes9x's
core headless, pairs the snapshot with the game frame whose work RAM
matches, and keeps the frame from B (default 120) before to A (120) after
it that is closest to our reference: a snapshot's registers are its frame
start's while its work RAM is copied a few frames later, and the port's
NMI-driven effects (fades, wipes) run ahead of or behind its main loop.
The offset says by how much. Colours within 8 (of 255) per channel count
as equal: the emulators round differently. Pictures of nearly one colour
(black between scenes) match any such frame and are counted apart.

--sheet writes the N snapshots that differ most as a picture sheet:
Falcon | reference | snes9x.
"""
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "build/snes9x/src/libretro/snes9x_libretro.dylib"


def differ(a, b):
    r, g, bl = ImageChops.difference(a, b).split()
    m = ImageChops.lighter(ImageChops.lighter(r, g), bl).point(lambda v: 255 if v > 8 else 0)
    return m.histogram()[255]


def flat(im):
    """Nearly one colour: 99% of the pixels the most common one."""
    colours = im.getcolors(256 * 224)
    return max(n for n, _ in colours) >= 0.99 * 256 * 224


def main():
    args = sys.argv[1:]
    snapdir, out = Path(args[0]), Path(args[1])
    out.mkdir(parents=True, exist_ok=True)
    before, after, sheet = 120, 120, None
    if "--range" in args:
        i = args.index("--range")
        before, after = int(args[i + 1]), int(args[i + 2])
    if "--sheet" in args:
        i = args.index("--sheet")
        sheet, nsheet = args[i + 1], int(args[i + 2])
    snaps = sorted(snapdir.glob("F*.BIN"))
    fal = {}
    for snap in snaps:
        both = out / f"{snap.stem}_both.ppm"
        subprocess.run([str(ROOT / "build/ppu_rgb"), str(ROOT / "build/ff6x/ff6.sfc"), str(snap), str(both)],
                       check=True, capture_output=True)
        im = Image.open(both).convert("RGB")
        fal[snap.stem] = im.crop((0, 0, 256, 224))
        im.crop((256, 0, 512, 224)).save(out / f"{snap.stem}_ref.ppm")
        both.unlink()
    res = subprocess.run([str(ROOT / "build/s9x_ref"), str(CORE), str(ROOT / "build/ff6x/ff6.sfc"), str(out),
                          "-range", str(before), str(after), *map(str, snaps)],
                         check=True, capture_output=True, text=True)
    rows = []
    for line in res.stdout.splitlines():
        m = re.match(r"(\S+)\.BIN frame (-?\d+): work RAM differs in (\d+) .*closest frame ([+-]\d+): (-?\d+) pixels", line)
        if not m:
            continue
        stem = m.group(1)
        ref = Image.open(out / f"{stem}_ref.ppm").convert("RGB")
        s9 = Image.open(out / f"{stem}_s9x.ppm").convert("RGB")
        rows.append(dict(stem=stem, frame=int(m.group(2)), wram=int(m.group(3)), off=int(m.group(4)),
                         ref=int(m.group(5)), fal=differ(fal[stem], s9), flat=flat(ref),
                         pics=(fal[stem], ref, s9)))
    real = [r for r in rows if not r["flat"]]
    print(f"{len(rows)} snapshots paired with snes9x by work RAM "
          f"(median {sorted(r['wram'] for r in rows)[len(rows) // 2]} of 131072 bytes differ); "
          f"{len(rows) - len(real)} pictures of nearly one colour left out")
    one = 256 * 224 // 100
    print(f"  of {len(real)}: reference = snes9x (<= 1% of the pixels) {sum(r['ref'] <= one for r in real)}, "
          f"Falcon = snes9x {sum(r['fal'] <= one for r in real)}")
    offs = sorted(r["off"] for r in real)
    print(f"  offset of the closest frame from the work RAM match: median {offs[len(offs) // 2]:+d}, "
          f"{sum(abs(o) > 8 for o in offs)} beyond 8 frames")
    for r in sorted(real, key=lambda r: -max(r["ref"], r["fal"]))[:15]:
        print(f"  {r['stem']}: reference {r['ref']:5d}, Falcon {r['fal']:5d} pixels differ "
              f"(frame {r['frame']}{r['off']:+d})")
    if sheet:
        worst = sorted(real, key=lambda r: -max(r["ref"], r["fal"]))[:nsheet]
        W, H, pad = 256, 224, 6
        img = Image.new("RGB", (3 * (W + pad) + pad, len(worst) * (H + pad + 12) + pad), (30, 30, 30))
        d = ImageDraw.Draw(img)
        for i, r in enumerate(worst):
            y = pad + i * (H + pad + 12)
            d.text((pad, y), f"{r['stem']}  Falcon ({r['fal']}) | reference ({r['ref']}) | snes9x "
                             f"(frame {r['frame']}{r['off']:+d})", fill=(220, 220, 220))
            for k, im in enumerate(r["pics"]):
                img.paste(im, (pad + k * (W + pad), y + 12))
        img.save(sheet)


if __name__ == "__main__":
    main()
