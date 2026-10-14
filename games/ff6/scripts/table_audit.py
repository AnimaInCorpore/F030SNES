#!/usr/bin/env python3
"""Jump tables the translator may have cut short.

    python3 scripts/table_audit.py [ROM]            # list the suspects
    python3 scripts/table_audit.py [ROM] C1B8BB ... # dump those tables

The translator reads a JSR/JMP (abs,X) table from the listing's dw block
up to the next label, or from incbin bytes while the words name listed code
(tools/smkx/analyze.py). A label the listing puts inside a table, or
handlers it left as incbin, cut the table short, and the game then stops in
rt_tableoob. This lists each table whose run of ROM words naming listed
code, or whose entries up to the lowest handler above it (all pointing
near the table: handlers the listing left as incbin count), is longer than
what was read. Either can overshoot into a neighbouring table or data, so
check each suspect: the next table's start, the site's bound (CMP/AND
before the ASL/TAX) or the first handler's address; enter the real length
in tools/smkx/tables.txt. Tables listed there, checked
ones included, are left out.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools.smkx import __main__ as M, analyze, listing  # noqa: E402


def main() -> None:
    args = sys.argv[1:]
    rom_path = Path(args.pop(0)) if args and not all(c in "0123456789ABCDEFabcdef" for c in args[0]) \
        else ROOT / "assets/Final Fantasy VI (Japan).zip"
    rom = M.load_rom(rom_path)
    lst = listing.parse_ff6(M.LISTING, rom)
    prog = analyze.analyse(lst, M.EXTRA, M.INTERRUPTS)
    tables = next(o for o in [prog, *vars(prog).values()] if hasattr(o, "table_cache")).table_cache
    code = lst.insn_at
    names = {}
    for name, off in lst.labels.items():
        names.setdefault(off, name)

    def run(idx: int, bank: int, limit: int = 256):
        o = idx
        while o + 2 <= len(rom) and o not in code and (o - idx) // 2 < limit:
            w = struct.unpack_from("<H", rom, o)[0]
            if analyze.rom_index(bank << 16 | w) not in code:
                break
            yield o, w
            o += 2

    def bound(idx: int) -> int:
        # Entries up to the lowest handler above the table, while each word
        # points near it (handlers the listing left as incbin count too).
        t, n, lo = idx & 0xFFFF, 0, 0x10000
        while n < 256 and t + 2 * n < lo:
            w = struct.unpack_from("<H", rom, idx + 2 * n)[0]
            if abs(w - t) > 0x2000:
                return 0
            if w > t:
                lo = min(lo, w)
            n += 1
        return n if t + 2 * n == lo else 0

    if not args:
        for idx, words in sorted(tables.items()):
            bank = idx >> 16 | 0xC0
            if (bank << 16 | idx & 0xFFFF) not in analyze.TABLE_LENGTHS:
                words = words or []             # read as empty: listed too
                n, b = sum(1 for _ in run(idx, bank)), bound(idx)
                if max(n, b) > len(words):
                    print(f"${bank:02X}{idx & 0xFFFF:04X}: read {len(words)}, ROM run {n}, "
                          f"up to the first handler {b or '-'}")
        return
    for a in (int(x, 16) for x in args):
        idx, bank = analyze.rom_index(a), a >> 16
        print(f"${a:06X}: read {len(tables.get(idx) or [])}")
        for k, (o, w) in enumerate(run(idx, bank, 128)):
            label = names.get(o)
            print(f"  {k:3d} ${bank:02X}{o & 0xFFFF:04X} -> {w:04X} "
                  f"{names.get(analyze.rom_index(bank << 16 | w), ''):40s}{'  label ' + label if label else ''}")


if __name__ == "__main__":
    main()
