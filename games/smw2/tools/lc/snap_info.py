#!/usr/bin/env python3
"""One line of PPU registers and the HDMA channels of each YI snapshot (version 5): BGMODE (bit 4-7 = 16x16 tiles), BG maps and
character bases, TM/TS, windows, colour math, mosaic, OBSEL, then every enabled HDMA channel (control, destination register,
table). snap_info.py SNAP.BIN..."""
import sys


def info(fn):
    d = open(fn, "rb").read()
    io = d[12 + 65536 + 512 + 544:12 + 65536 + 512 + 544 + 0x6000]
    en = io[0x220C]
    print(fn.split("/")[-1], "HDMAEN", hex(en), "BGMODE", hex(io[0x105]), "BGSC", [hex(io[0x107 + i]) for i in range(4)],
          "NBA", hex(io[0x10B]), hex(io[0x10C]), "TM", hex(io[0x12C]), "TS", hex(io[0x12D]), "W12", hex(io[0x123]),
          "W34", hex(io[0x124]), "WOBJ", hex(io[0x125]), "TMW", hex(io[0x12E]), "TSW", hex(io[0x12F]), "CGWSEL", hex(io[0x130]),
          "CGADSUB", hex(io[0x131]), "MOSAIC", hex(io[0x106]), "OBSEL", hex(io[0x101]))
    for c in range(8):
        if en >> c & 1:
            b = 0x2300 + c * 16
            print("  ch", c, "ctrl", hex(io[b]), "dest $21%02x" % io[b + 1], "table %02x:%02x%02x" % (io[b + 4], io[b + 3], io[b + 2]),
                  "indirect bank", hex(io[b + 7]))


for f in sys.argv[1:]:
    info(f)
