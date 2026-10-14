#!/usr/bin/env python3
"""Read WRAM and registers from a runtime snapshot (SNAP.BIN, TRAP.BIN,
Fnnnnnnn.BIN; layout in src/snes_ppu.h).

    scripts/ctx_snap.py build/ctx/TRAP.BIN 7E3000 40    # hex dump of WRAM
"""

from __future__ import annotations

import sys
from pathlib import Path

VRAM, CGRAM, OAM, IO = 0x10000, 512, 544, 0x6000
HEAD = 12
WRAM_AT = HEAD + VRAM + CGRAM + OAM + IO + 32 + 4 + 256 * 6


def wram(data: bytes) -> bytes:
    return data[WRAM_AT:WRAM_AT + 0x20000]


def main() -> None:
    data = Path(sys.argv[1]).read_bytes()
    addr = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x7E0000
    n = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x100
    w = wram(data)
    off = (addr - 0x7E0000) if addr >= 0x7E0000 else addr & 0xFFFF
    for i in range(0, n, 16):
        row = w[off + i:off + i + 16]
        print(f"${0x7E0000 + off + i:06X}: " + " ".join(f"{b:02X}" for b in row))


if __name__ == "__main__":
    main()
