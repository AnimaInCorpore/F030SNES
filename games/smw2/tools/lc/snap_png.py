#!/usr/bin/env python3
"""Draws the Falcon's frame of YI snapshots (version 5) as PNG files: the eight planes with CGRAM as it was at the
frame's start, brightness ignored (the viewer for scenes the host renderer shows black because INIDISP ends in
forced blank). snap_png.py OUTDIR SNAP.BIN..."""
import struct
import sys
from pathlib import Path

HDR = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 + 1536 + 131072 + 32768


def convert(data: bytes):
    cg = data[12 + 65536:12 + 65536 + 512]
    planar = data[HDR:HDR + 57344]
    rows = []
    for y in range(224):
        row = bytearray()
        for g in range(16):
            w = struct.unpack(">8H", planar[(y * 16 + g) * 16:(y * 16 + g) * 16 + 16])
            for k in range(16):
                v = 0
                for b in range(8):
                    v |= ((w[b] >> (15 - k)) & 1) << b
                c = cg[v * 2] | cg[v * 2 + 1] << 8
                row += bytes(((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31))
        rows.append(bytes(row))
    return b"P6\n256 224\n255\n" + b"".join(rows)


if __name__ == "__main__":
    import subprocess
    out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    for n in sys.argv[2:]:
        ppm = out / (Path(n).stem + ".ppm")
        ppm.write_bytes(convert(Path(n).read_bytes()))
        subprocess.run(["magick", str(ppm), str(ppm.with_suffix(".png"))], check=True)
        ppm.unlink()
