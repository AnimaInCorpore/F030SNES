#!/usr/bin/env python3
"""SMAS snapshots (version 4, `SMKXSNAP`, SRAM 2 KiB, no planes field or palette blocks) in
F030CT's layout, so that F030CT's src/snes_ppu.c and tools/lc_model.c can read them: SRAM
padded to 8 KiB, a `planes` u32 = 8 after it, two empty palette blocks after the frame.
The same conversion as games/smw/tools/lc/smw_to_ct_snap.py and games/dkc/tools/lc_check/dkc_to_ct_snap.py (the formats are identical).

    smas_to_ct_snap.py OUTDIR SNAP.BIN...
"""
import struct
import sys
from pathlib import Path

HDR = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 + 1536
PAL_BLOCKS = 2 * (96 + 48 + 256 + 4 + 2)


def convert(data: bytes) -> bytes:
    out = bytearray(data[:HDR])
    p = HDR
    out += data[p:p + 131072]; p += 131072              # WRAM
    out += data[p:p + 2048] + bytes(8192 - 2048); p += 2048   # SRAM
    out += struct.pack(">I", 8)                         # planes
    out += data[p:p + 57344]; p += 57344                # the Falcon's frame
    out += bytes(PAL_BLOCKS)
    out += data[p:]                                     # OAM segments
    return bytes(out)


def main() -> None:
    out_dir = Path(sys.argv[1])
    out_dir.mkdir(parents=True, exist_ok=True)
    for name in sys.argv[2:]:
        (out_dir / Path(name).name).write_bytes(convert(Path(name).read_bytes()))


if __name__ == "__main__":
    main()
