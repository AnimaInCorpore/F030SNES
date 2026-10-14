"""Register liveness over the DSP-1 program (uPD77C25).

    python3 -m tools.smkx.upd_live assets/dsp1.bin [pc ...]

Prints the registers, flags and return-stack slots live on entry to each
given program counter (default $000, where every command ends): the state
a native command (src/dsp1_native.c) must leave exactly as the firmware
would. Data RAM is not tracked here; native commands keep it exact.
Semantics follow tools/smkx/upd7725.py.
"""

from __future__ import annotations

import sys
from pathlib import Path

from .upd7725 import load_firmware

REGS = ["A", "B", "TR", "TRB", "K", "L", "DP", "RP", "DR", "SR",
        "fa.ov0", "fa.ov1", "fa.z", "fa.c", "fa.s0", "fa.s1",
        "fb.ov0", "fb.ov1", "fb.z", "fb.c", "fb.s0", "fb.s1",
        "st0", "st1", "st2", "st3"]
R = {n: 1 << i for i, n in enumerate(REGS)}
FLAGS = ("ov0", "ov1", "z", "c", "s0", "s1")
COND = {
    0x080: "fa.c", 0x082: "fa.c", 0x084: "fb.c", 0x086: "fb.c",
    0x088: "fa.z", 0x08A: "fa.z", 0x08C: "fb.z", 0x08E: "fb.z",
    0x090: "fa.ov0", 0x092: "fa.ov0", 0x094: "fb.ov0", 0x096: "fb.ov0",
    0x098: "fa.ov1", 0x09A: "fa.ov1", 0x09C: "fb.ov1", 0x09E: "fb.ov1",
    0x0A0: "fa.s0", 0x0A2: "fa.s0", 0x0A4: "fb.s0", 0x0A6: "fb.s0",
    0x0A8: "fa.s1", 0x0AA: "fa.s1", 0x0AC: "fb.s1", 0x0AE: "fb.s1",
    0x0B0: "DP", 0x0B1: "DP", 0x0B2: "DP", 0x0B3: "DP",
    0x0BC: "SR", 0x0BE: "SR",
}
SRC = {0: "TRB", 1: "A", 2: "B", 3: "TR", 4: "DP", 5: "RP", 6: "RP", 7: "fa.s1",
       8: "DR", 9: "DR", 10: "SR", 13: "K", 14: "L", 15: "DP"}


def m(*names: str) -> int:
    v = 0
    for n in names:
        v |= R[n]
    return v


def dst_use_def(dst: int) -> tuple[int, int]:
    """(use, def) of a destination write."""
    if dst == 1: return 0, m("A")
    if dst == 2: return 0, m("B")
    if dst == 3: return 0, m("TR")
    if dst == 4: return 0, m("DP")
    if dst == 5: return 0, m("RP")
    if dst == 6: return m("SR"), m("DR")          # also sets RQM: SR partly
    if dst == 7: return m("SR"), 0                # writable bits only: not a kill
    if dst == 10: return 0, m("K")
    if dst == 11: return m("RP"), m("K", "L")
    if dst == 12: return m("DP"), m("K", "L")
    if dst == 13: return 0, m("L")
    if dst == 14: return 0, m("TRB")
    if dst == 15: return m("DP"), 0
    return 0, 0


def alu_use_def(alu: int, asl: int) -> tuple[int, int]:
    acc = "B" if asl else "A"
    f = "fb" if asl else "fa"
    other_c = "fa.c" if asl else "fb.c"
    use, dfn = m(acc), m(acc, f + ".s0", f + ".z")
    if alu in (1, 2, 3, 10, 13, 14, 15, 11, 12):
        dfn |= m(f + ".c", f + ".ov0", f + ".ov1")
        if alu == 12:
            use |= m(other_c)
    else:                                          # arithmetic
        dfn |= m(f + ".c", f + ".ov0")
        use |= m(f + ".ov1", f + ".s1")            # ov1 toggles, s1 set on overflow only
        dfn |= m(f + ".ov1")
        if alu in (6, 7):
            use |= m(other_c)
    return use, dfn


def succ_and_ud(pc: int, op: int, returns: list[int]):
    """Successors, and a transfer function in -> out for this instruction."""
    kind = op >> 22
    nxt = (pc + 1) & 0x7FF
    if kind == 2:
        brch = (op >> 13) & 0x1FF
        na = (op >> 2) & 0x7FF
        if brch == 0x100:
            return [na], 0, 0, None
        if brch == 0x140:                          # CALL: push
            return [na], 0, 0, "call"
        return [na, nxt], m(COND[brch]), 0, None
    if kind == 3:
        u, d = dst_use_def(op & 15)
        return [nxt], u, d, None
    psel = (op >> 20) & 3
    alu = (op >> 16) & 15
    asl = (op >> 15) & 1
    dpl = (op >> 13) & 3
    dphm = (op >> 9) & 15
    rpdcr = (op >> 8) & 1
    src = (op >> 4) & 15
    dst = op & 15
    use = m(SRC[src]) if src in SRC else 0
    dfn = 0
    if alu:
        use |= [m("DP"), 0, m("K", "L"), m("K", "L")][psel]
        au, ad = alu_use_def(alu, asl)
        use |= au
        dfn |= ad
    du, dd = dst_use_def(dst)
    # the destination write happens after the ALU reads its operands
    use |= du & ~dfn
    dfn |= dd
    if dpl or dphm:
        use |= m("DP") if not (dd & m("DP")) else 0
    if rpdcr:
        use |= m("RP") if not (dd & m("RP")) else 0
    if kind == 1:
        return returns, use, dfn, "ret"
    return [nxt], use, dfn, None


def analyse(program: list[int]) -> list[int]:
    """live_in per program counter."""
    n = len(program)
    calls = [pc for pc, op in enumerate(program) if op >> 22 == 2 and (op >> 13) & 0x1FF == 0x140]
    returns = sorted({(pc + 1) & 0x7FF for pc in calls})
    info = [succ_and_ud(pc, op, returns) for pc, op in enumerate(program)]
    st = [R["st0"], R["st1"], R["st2"], R["st3"]]
    live_in = [0] * n
    changed = True
    while changed:
        changed = False
        for pc in range(n - 1, -1, -1):
            succ, use, dfn, stk = info[pc]
            out = 0
            for s in succ:
                out |= live_in[s]
            if stk == "call":                      # s0' = ret, s1' = s0, s2' = s1, s3' = s2
                base = out & ~(st[0] | st[1] | st[2] | st[3])
                sl = base
                if out & st[1]: sl |= st[0]
                if out & st[2]: sl |= st[1]
                if out & st[3]: sl |= st[2]
                new_in = sl
            elif stk == "ret":                     # jump to s0; s0' = s1, s1' = s2, s2' = s3, s3' = s3
                base = (use | (out & ~dfn)) & ~(st[0] | st[1] | st[2] | st[3])
                sl = base | st[0]
                if out & st[0]: sl |= st[1]
                if out & st[1]: sl |= st[2]
                if out & (st[2] | st[3]): sl |= st[3]
                new_in = sl
            else:
                new_in = use | (out & ~dfn)
            if new_in != live_in[pc]:
                live_in[pc] = new_in
                changed = True
    return live_in


def names(mask: int) -> str:
    return " ".join(n for n in REGS if mask & R[n]) or "(none)"


def main() -> None:
    program, _ = load_firmware(Path(sys.argv[1]))
    live = analyse(program)
    for a in (sys.argv[2:] or ["0"]):
        pc = int(a, 16)
        print(f"${pc:03X}: {names(live[pc])}")


if __name__ == "__main__":
    main()
