#!/usr/bin/env python3
"""The menu's task code pointers, for tools/smkx/dispatch_targets.txt.

ExecTasks ($C3120x, `JMP (zTaskCodePtr)`) runs the code pointer of every
active task, and a task is made by `LDY #near XTask : JSR CreateTask` (or
InitTask). Nothing in the listing ties the site to those addresses, so this
finds them: the immediate of the last `LDY #` before each call of CreateTask,
InitTask, and the routines that store a code pointer straight into
wTaskProp::CodePtr.

    python3 scripts/ff6x_menu_tasks.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools.smkx import __main__ as M, listing  # noqa: E402


def main() -> None:
    print("ROM check:", file=sys.stderr)          # load_rom reports on stdout: keep stdout for the list
    import contextlib, io
    with contextlib.redirect_stdout(io.StringIO()):
        rom = M.load_rom(ROOT / "assets/Final Fantasy VI (Japan).zip")
    lst = listing.parse_ff6(M.LISTING, rom)
    names = {}
    for name, off in lst.labels.items():
        names.setdefault(off, name)
    site = 0xC00000 | lst.labels["ExecTasks.@1203"]
    creators = {lst.labels["CreateTask"], lst.labels["InitTask"]}
    ins = sorted(lst.insn_at.items())
    targets = {}
    for k, (off, i) in enumerate(ins):
        if i.mnem == "JSR" and i.mode == "abs" and (0xC30000 | i.value) & 0x3FFFFF in creators:
            for back in range(1, 8):
                j = ins[k - back][1]
                if j.mnem == "LDY" and j.mode == "imm_x":
                    targets.setdefault(0xC30000 | j.value, []).append(0xC00000 | off)
                    break
            else:
                print(f"# no LDY # before the call at ${0xC00000 | off:06X}", file=sys.stderr)
    print(f"# ExecTasks (${site:06X}): the code pointers given to CreateTask / InitTask")
    print(f"# (scripts/ff6x_menu_tasks.py).")
    line = f"${site:06X}:"
    for t in sorted(targets):
        if len(line) > 70:
            print(line)
            line = f"${site:06X}:"
        line += f" ${t:06X}"
    print(line)
    for t in sorted(targets):
        print(f"#   ${t:06X} {names.get(t & 0x3FFFFF, '')}", file=sys.stderr)


if __name__ == "__main__":
    main()
