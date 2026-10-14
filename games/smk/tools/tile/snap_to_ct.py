#!/usr/bin/env python3
"""SMK snapshots (version 3, `SMKXSNAP`: SRAM 2 KiB, a four-plane frame of 28,672 bytes, the
palette blocks) in the layout the shared tile stack reads (F030CT's version 4: SRAM padded to
8 KiB, a `planes` u32 after it, the frame padded to 57,344 bytes, the same palette blocks).
planes is 4: the frame is the Mode 7 path's, in palette slots. Snapshots that already are
version 4 (the runtime writes those when the list compositor's eight-plane screen is shown) are
copied as they are.

    snap_to_ct.py OUTDIR SNAP.BIN...
"""
import struct
import sys
from pathlib import Path

HDR = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 + 1536
PAL_BLOCKS = 2 * (96 + 48 + 256 + 4 + 2)


def convert(data: bytes) -> bytes:
    if data[:8] != b"SMKXSNAP":
        raise SystemExit("not a snapshot")
    version = struct.unpack(">I", data[8:12])[0]
    if version == 4:
        return data
    if version != 3:
        raise SystemExit(f"snapshot version {version}")
    out = bytearray(data[:8] + struct.pack(">I", 4) + data[12:HDR])
    p = HDR
    out += data[p:p + 131072]; p += 131072                     # WRAM
    out += data[p:p + 2048] + bytes(8192 - 2048); p += 2048    # SRAM
    out += struct.pack(">I", 4)                                # planes
    out += data[p:p + 28672] + bytes(57344 - 28672); p += 28672
    out += data[p:p + PAL_BLOCKS]; p += PAL_BLOCKS
    out += data[p:]                                            # OAM segments
    return bytes(out)


def main() -> None:
    out_dir = Path(sys.argv[1])
    out_dir.mkdir(parents=True, exist_ok=True)
    for name in sys.argv[2:]:
        (out_dir / Path(name).name).write_bytes(convert(Path(name).read_bytes()))


if __name__ == "__main__":
    main()
