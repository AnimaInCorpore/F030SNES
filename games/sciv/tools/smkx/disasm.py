"""Small 65816 disassembler for ROM or WRAM-program bytes (diagnostics).

    python3 -m tools.smkx.disasm 7E5A90 40 [m8 x8]   # widths default 8-bit M, 16-bit X
"""

from __future__ import annotations

import sys
from pathlib import Path

from . import analyze
from . import opcodes as O

ROOT = Path(__file__).resolve().parents[2]


def reader():
    rom = (ROOT / "build/scivx/sciv.sfc").read_bytes()

    def byte(a):
        i = analyze.rom_index(a)
        if i is None:
            raise KeyError(hex(a))
        return rom[i]
    return byte


def main() -> None:
    addr, n = int(sys.argv[1], 16), int(sys.argv[2], 16)
    m8 = sys.argv[3] != "m0" if len(sys.argv) > 3 else True
    x8 = sys.argv[4] == "x1" if len(sys.argv) > 4 else False
    byte = reader()
    end = addr + n
    while addr < end:
        op = byte(addr)
        mn, mode = O.OPCODES[op]
        k = O.operand_length(mode, m8, x8)
        raw = [byte(addr + i) for i in range(1 + k)]
        v = int.from_bytes(bytes(raw[1:]), "little") if k else 0
        if mn == "REP":
            m8 = m8 and not v & 0x20; x8 = x8 and not v & 0x10
        if mn == "SEP":
            m8 = m8 or bool(v & 0x20); x8 = x8 or bool(v & 0x10)
        opnd = "" if not k else f"${v:0{2 * k}X}"
        if mode in (O.REL8, O.REL16):
            off = v - (0x100 if mode == O.REL8 else 0x10000) if v & (0x80 if mode == O.REL8 else 0x8000) else v
            opnd = f"${(addr & 0xFF0000) | ((addr + 1 + k + off) & 0xFFFF):06X}"
        print(f"${addr:06X}  {' '.join(f'{b:02X}' for b in raw):12} {mn} {mode:8} {opnd}")
        addr += 1 + k


if __name__ == "__main__":
    main()
