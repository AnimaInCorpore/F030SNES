#!/usr/bin/env python3
"""YI snapshots (version 5, `SMKXSNAP`, the GSU's 32 KiB RAM, no planes field or palette blocks) in
F030CT's layout (version 4), so that F030CT's own src/snes_ppu.c and tools/lc_model.c can read them:
the first 8 KiB of the GSU RAM as SRAM (the part banks $00-$3F show at $6000), a `planes` u32 = 8
after it, two empty palette blocks after the frame. Modelled on F030SMW's smw_to_ct_snap.py.

CT's HDMA bus is HiROM with 8 KiB SRAM: tables in the GSU RAM above 8 KiB or in YI's LoROM/linear ROM
map are not read right there. This repository's tools/lc/lc_model.c reads the native layout directly
with the SuperFX bus (ppu_abus_yi); this converter is for CT's own tools only.

    smw2_to_ct_snap.py OUTDIR SNAP.BIN...
"""
import struct
import sys
from pathlib import Path

HDR = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 + 1536
PAL_BLOCKS = 2 * (96 + 48 + 256 + 4 + 2)


def convert(data: bytes) -> bytes:
    assert data[:8] == b"SMKXSNAP" and struct.unpack(">I", data[8:12])[0] == 5, "not a version 5 snapshot"
    out = bytearray(data[:HDR])
    out[8:12] = struct.pack(">I", 4)
    p = HDR
    out += data[p:p + 131072]; p += 131072              # WRAM
    out += data[p:p + 8192]; p += 32768                 # the GSU RAM's first 8 KiB as SRAM
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
