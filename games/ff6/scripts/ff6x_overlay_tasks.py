#!/usr/bin/env python3
"""The cutscene program's task code pointers, for tools/smkx/dispatch_targets.txt.

The program at $7E5000 (title, opening credits, ...) has a task scheduler
like the menu's: its `JMP ($1B)` runs the code pointer of every active
task, and a task is made by `LDY #near XTask : JSR CreateTask`. The listing
does not tie the site to those addresses, so this finds them: the labels
that `LDY #imm : JSR CreateTask` names in the program's bytes.

    python3 scripts/ff6x_overlay_tasks.py SITE      # SITE = the JMP ($1B), hex
"""

import contextlib
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools.smkx import __main__ as M, listing  # noqa: E402


def main() -> None:
    site = int(sys.argv[1], 16)
    with contextlib.redirect_stdout(io.StringIO()):
        rom = M.load_rom(ROOT / "assets/Final Fantasy VI (Japan).zip")
    lst = listing.parse_ff6(M.LISTING, rom)
    # `LDY #imm16 : JSR CreateTask` in the program's own bytes
    from tools.smkx import analyze, ff6_lz
    start, src = analyze.WRAM_OVERLAYS[0]
    data = ff6_lz.decompress(lambda a: rom[analyze.rom_index(a)], src)
    create = next(a for a, name, _ in lst.wram_refs if name == "CreateTask")
    pat = re.compile(rb"\xa0(..)\x20" + re.escape(bytes([create & 0xFF, create >> 8 & 0xFF])), re.S)
    targets = sorted({0x7E0000 | int.from_bytes(m.group(1), "little") for m in pat.finditer(data)})
    print(f"# The cutscene program's task scheduler (${site:06X}, `JMP ($1B)`): the code pointers given")
    print("# to CreateTask (scripts/ff6x_overlay_tasks.py).")
    line = f"${site:06X}:"
    for t in targets:
        if len(line) > 70:
            print(line)
            line = f"${site:06X}:"
        line += f" ${t:06X}"
    print(line)


if __name__ == "__main__":
    main()
