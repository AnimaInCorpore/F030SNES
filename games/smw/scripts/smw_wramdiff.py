#!/usr/bin/env python3
"""Compare WRAM of a Falcon snapshot (SNAP.BIN, format in src/snes_ppu.h)
with a reference dump from tools/ref65816.c, naming addresses by the
SMWDisX symbols.

    scripts/smw_wramdiff.py build/smwx/SNAP.BIN ref.bin [max]
"""

import bisect
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SYM = ROOT / "third_party/SMWDisX/SMW_U.sym"
# snapshot layout: magic+version 12, vram 65536, cgram 512, oam 544,
# io 0x6000, scroll/mode 7 32, nlog 4, log 256*6, then wram
WRAM_AT = 12 + 65536 + 512 + 544 + 0x6000 + 32 + 4 + 256 * 6


def ram_symbols():
    out = []
    for line in SYM.read_text().splitlines():
        parts = line.split()
        if len(parts) != 2 or line.startswith(";") or parts[1].startswith(":"):
            continue
        a = int(parts[0], 16)
        bank, off = a >> 16, a & 0xFFFF
        if bank in (0x7E, 0x7F):
            out.append((((bank - 0x7E) << 16) | off, parts[1]))
        elif bank == 0 and off < 0x2000:
            out.append((off, parts[1]))
    out.sort()
    return out


def main():
    snap = Path(sys.argv[1]).read_bytes()
    ref = Path(sys.argv[2]).read_bytes()
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    fal = snap[WRAM_AT:WRAM_AT + 0x20000]
    syms = ram_symbols()
    keys = [a for a, _ in syms]
    diffs = [i for i in range(0x20000) if fal[i] != ref[i] and not (0x111 <= i <= 0x1FF)]
    print(f"{len(diffs)} bytes differ (stack page excluded)")
    for i in diffs[:limit]:
        j = bisect.bisect_right(keys, i) - 1
        name = f"{syms[j][1]}+{i - syms[j][0]:X}" if j >= 0 else "?"
        bank = 0x7E + (i >> 16)
        print(f"  ${bank:02X}:{i & 0xFFFF:04X} {name:34} falcon {fal[i]:02X} ref {ref[i]:02X}")


if __name__ == "__main__":
    main()
