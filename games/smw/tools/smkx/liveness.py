"""Processor-flag liveness over the analysed program.

The emitter computes a 65816 flag only where a later instruction can read
it, and preserves a flag across an instruction only where it stays live.
Nodes are (procedure key, node) pairs; flags flow into callees at calls and
from every return back to each caller's continuation. Returns from
procedures whose callers are not all known keep every flag live.
"""

from __future__ import annotations

import os

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


# Precise escapes and PHP/PLP pairing (see compute); SMKX_SAFE_LIVENESS=1
# turns them off for A/B checks against tools/ref65816.c.
FAST = os.environ.get("SMKX_SAFE_LIVENESS") != "1"


def php_pairs(prog):
    """Which PLP pulls the P that each PHP pushed, per procedure.

    A forward pass over each procedure's nodes tracks, for every byte of
    the abstract stack, the PHP nodes that may have pushed it. Returns
    (matches, unmatched): matches[php] = the PLP nodes that pull its byte,
    unmatched = PHP nodes whose byte may leave another way (pulled by PLA,
    left on the stack at a return, dropped by TXS, consumed by a callee
    that pulls below its frame). Keys are (pkey, node).
    """
    matches: dict = {}
    unmatched: set = set()
    for pkey, p in prog.procs.items():
        state = {}
        for addr, sts in p.states.items():
            for st in sts:
                state[(addr, st.key())] = st
        ids: dict = {}
        work = []
        for st in p.states.get(p.entry, []):
            node = (p.entry, st.key())
            ids[node] = tuple(frozenset() for _ in st.stack)
            work.append(node)
        while work:
            node = work.pop()
            cur = ids[node]
            d = p.insns.get(node[0])
            mn = d.mnem if d is not None else ""
            for target, ctx in p.calls.get(node, []):
                callee = prog.procs.get((target, ctx))
                if mn in ("JMP", "JML") or callee is None or callee.escapes or callee.escape_exits:
                    for entry in cur:
                        unmatched.update((pkey, php) for php in entry)
                    break
            if node in p.dynamic and not p.calls.get(node):
                for entry in cur:
                    unmatched.update((pkey, php) for php in entry)
            if mn == "PHP":
                out = cur + (frozenset([node]),)
            elif mn == "PLP":
                if cur:
                    for php in cur[-1]:
                        matches.setdefault((pkey, php), set()).add((pkey, node))
                out = cur[:-1]
            else:
                out = cur
            edges = p.succ.get(node, [])
            if not edges and mn in ("RTS", "RTL", "RTI"):
                for entry in out:
                    unmatched.update((pkey, php) for php in entry)
            for kind, dst in edges:
                st = state.get(dst)
                if st is None:
                    continue
                want = len(st.stack)
                o = out
                if len(o) > want:
                    for entry in o[want:]:
                        unmatched.update((pkey, php) for php in entry)
                    o = o[:want]
                elif len(o) < want:
                    o = o + tuple(frozenset() for _ in range(want - len(o)))
                old = ids.get(dst)
                if old is None:
                    new = o
                elif len(old) != len(o):
                    for entry in old + o:
                        unmatched.update((pkey, php) for php in entry)
                    new = old
                else:
                    new = tuple(a | b for a, b in zip(old, o))
                if new != old:
                    ids[dst] = new
                    work.append(dst)
    return matches, unmatched


def compute(prog) -> dict:
    """live_out[(pkey, node)] -> flag mask.

    A return that pulled more than its own frame flows to where the escaped
    caller's callers continue instead of keeping every flag live, and a PHP
    reads only the flags live after its matching PLP (php_pairs). Checked
    against the reference interpreter frame for frame (analysis/smw.md)."""
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

    php_match, php_unmatched = php_pairs(prog) if FAST else ({}, set())
    plp_php: dict = {}
    for php, plps in php_match.items():
        if php not in php_unmatched:
            for plp in plps:
                plp_php.setdefault(plp, set()).add(php)
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
                if d.mnem == "PHP" and g in php_match and g not in php_unmatched:
                    use[g] = 0             # filled from the matching PLPs below
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
                    linked = pkey in prog.exit_links or not callers or (p.escaped_to and not FAST)
                    if linked:
                        always_live.add(g)
                    for ck in (p.escaped_to if FAST else ()):
                        # the return leaves the escaped caller too: it continues
                        # where that caller's callers continue
                        outer = all_callers(ck)
                        if ck in prog.exit_links or not outer or procs[ck].escaped_to:
                            always_live.add(g)
                        for (okey, onode, osite) in outer:
                            for (cn, ex), dest in procs[okey].conts.items():
                                if cn == onode:
                                    add(g, (okey, dest))
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
        if out != live_out[g] and g in plp_php:
            # the flags a PLP restores are read by the PHPs that pushed them
            for php in plp_php[g]:
                use[php] |= out
                if php not in in_work:
                    in_work.add(php)
                    work.append(php)
        live_out[g] = out
        new_in = use[g] | (out & ~kill[g])
        if new_in != live_in[g]:
            live_in[g] = new_in
            for pr in preds.get(g, ()):
                if pr not in in_work:
                    in_work.add(pr)
                    work.append(pr)
    return live_out
