"""Processor-flag liveness over the analysed program.

The emitter computes a 65816 flag only where a later instruction can read
it, and preserves a flag across an instruction only where it stays live.
Nodes are (procedure key, node) pairs; flags flow into callees at calls and
from every return back to each caller's continuation. Returns from
procedures whose callers are not all known keep every flag live.
"""

from __future__ import annotations

from . import opcodes as O

C, Z, N, V = 1, 2, 4, 8
ALL = C | Z | N | V

NZ_DEFS = {
    "LDA", "LDX", "LDY", "PLA", "PLX", "PLY", "PLB", "TAX", "TAY", "TXA", "TYA",
    "TSX", "TXY", "TYX", "TSC", "TDC", "AND", "ORA", "EOR", "INC", "DEC", "INX",
    "INY", "DEX", "DEY", "XBA", "PLD",
}


def uses(mn: str) -> int:
    if mn in ("ADC", "SBC", "ROL", "ROR", "BCC", "BCS"):
        return C
    if mn in ("BEQ", "BNE"):
        return Z
    if mn in ("BMI", "BPL"):
        return N
    if mn in ("BVC", "BVS"):
        return V
    if mn == "PHP":
        return ALL
    return 0


def defs(mn: str, mode: str, value: int) -> int:
    if mn in NZ_DEFS:
        return N | Z
    if mn in ("ADC", "SBC", "PLP", "RTI"):
        return ALL
    if mn in ("CMP", "CPX", "CPY", "ASL", "LSR", "ROL", "ROR"):
        return N | Z | C
    if mn == "BIT":
        return Z if mode == O.IMM_M else N | V | Z
    if mn in ("TRB", "TSB"):
        return Z
    if mn in ("CLC", "SEC"):
        return C
    if mn == "CLV":
        return V
    if mn in ("REP", "SEP"):
        out = 0
        if value & 0x01: out |= C
        if value & 0x02: out |= Z
        if value & 0x40: out |= V
        if value & 0x80: out |= N
        return out
    return 0


def compute(prog) -> dict:
    """live_out[(pkey, node)] -> flag mask."""
    procs = prog.procs
    succ: dict = {}
    preds: dict = {}
    use: dict = {}
    kill: dict = {}

    def add(a, b):
        succ.setdefault(a, set()).add(b)
        preds.setdefault(b, set()).add(a)

    def all_callers(key):
        # direct calls and pointer dispatch sites (whose continuation is the
        # "assumed" one) both resume after this procedure returns
        return set(prog.callers.get(key, ())) | set(prog.dyn_callers.get(key, ()))

    entry_node = {}
    for pkey, p in procs.items():
        for st in p.states.get(p.entry, []):
            if st.stack == () and st.esc == 0:
                entry_node[pkey] = (pkey, (p.entry, st.key()))
                break
    always_live = set()

    for pkey, p in procs.items():
        for addr, sts in p.states.items():
            d = p.insns.get(addr)
            for st in sts:
                node = (addr, st.key())
                g = (pkey, node)
                if d is None or node in p.traps:
                    use[g], kill[g] = 0, 0
                    continue
                use[g] = uses(d.mnem)
                kill[g] = defs(d.mnem, d.mode, d.value)
                if p.dynamic.get(node, "").startswith("execptr"):
                    kill[g] = ALL          # the dispatch sets C, V, N and Z
                for kind, dest in p.succ.get(node, []):
                    add(g, (pkey, dest))
                for target, ctx in p.calls.get(node, []):
                    ce = entry_node.get((target, ctx))
                    if ce is not None:
                        add(g, ce)
                if d.mnem in ("RTS", "RTL"):
                    callers = all_callers(pkey)
                    linked = pkey in prog.exit_links or p.escaped_to or not callers
                    if linked:
                        always_live.add(g)
                    for (ckey, cnode, site) in callers:
                        cp = procs[ckey]
                        for (cn, ex), dest in cp.conts.items():
                            if cn == cnode:
                                add(g, (ckey, dest))
    live_in: dict = {g: 0 for g in use}
    live_out: dict = {g: (ALL if g in always_live else 0) for g in use}
    work = list(use)
    in_work = set(work)
    while work:
        g = work.pop()
        in_work.discard(g)
        out = live_out[g]
        for s in succ.get(g, ()):
            out |= live_in.get(s, ALL)
        live_out[g] = out
        new_in = use[g] | (out & ~kill[g])
        if new_in != live_in[g]:
            live_in[g] = new_in
            for pr in preds.get(g, ()):
                if pr not in in_work:
                    in_work.add(pr)
                    work.append(pr)
    return live_out
