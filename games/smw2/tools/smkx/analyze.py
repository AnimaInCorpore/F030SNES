"""Static discovery of the SMK program from the ROM vectors.

Procedures are analysed per entry context: the entry address plus the M, X,
decimal and data-bank state the caller established. Within a procedure every
instruction carries one abstract machine state:

    m8, x8   register widths (True = 8-bit), None when unknown
    d        decimal flag
    dbr      data bank register value, None when unknown
    e        emulation mode (reset code only)
    stack    what this procedure pushed so far, innermost last; each entry
             is ("b", value-or-None) for a byte or ("p", (m8, x8, d)) for
             the P register pushed by PHP

Calls are summarised: a JSR/JSL continues with the callee's exit widths,
decimal flag and DBR. Pulling below the procedure's own pushes is recorded as
a stack escape. Every decoded instruction that starts where the listing has
an instruction must have the listing's length, which checks the inferred M/X
at each immediate.
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field

from . import opcodes as O
from pathlib import Path

from .listing import Listing, Data, Label, Insn, yi_offset

UNKNOWN = None
IN = "IN"          # the data bank the caller had on entry
CTX_CAP = 4        # contexts per entry before the DBR is left to the runtime


def rom_index(addr: int) -> int | None:
    """ROM file offset of a 24-bit CPU address, or None for non-ROM space.
    YI's SuperFX cartridge maps its 2 MiB ROM LoROM-style at $8000-$FFFF of
    banks $00-$3F and linearly in banks $40-$5F (listing.yi_offset); the
    rest (WRAM, I/O, the GSU's RAM at $70-$71 and at $6000-$7FFF) is
    resolved by the runtime."""
    return yi_offset(addr)


# Code the reset routine copies from ROM into WRAM and runs there: the
# interrupt trampolines (YI_ROMHeaderVectorRAMCodeBlock, $00813F -> $0100)
# and YI_MainRAMCodeBlock ($00C000-$00FFFF -> $7EC000), which holds the
# NMI and IRQ handlers and the routines that start the SuperFX (the 65816
# may not touch ROM while the GSU runs). Nothing writes into either copy,
# so it is decoded from the ROM.
RAM_CODE = ((0x7E0100, 0x00813F, 0x10), (0x7EC000, 0x00C000, 0x4000))


def code_index(addr: int) -> int | None:
    """ROM offset of the bytes an instruction fetch at addr reads."""
    bank, pc = addr >> 16, addr & 0xFFFF
    if (bank & 0x7F) < 0x40 and pc < 0x2000:
        addr = 0x7E0000 | pc                  # low WRAM mirror
    for ram, rom, n in RAM_CODE:
        if ram <= addr < ram + n:
            return yi_offset(rom + addr - ram)
    return yi_offset(addr)


# PLD sites whose value the analysis cannot see (A is not tracked):
# reset's `LDA #$00 : PHA : PLB : PHA : PHA : PLD`.
KNOWN_D = {0x00800C: 0x0000}

# Routines that read inline arguments after their JSL and return behind
# them (they add the argument size to the return address on the stack).
INLINE_ARGS = {0x00BE39: 8, 0x00BE71: 6}


@dataclass(frozen=True)
class Ctx:
    m8: bool | None
    x8: bool | None
    d: bool | None
    dbr: int | None
    e: bool = False
    dp: int | None = 0          # direct page register D (IN: the caller's)

    def short(self) -> str:
        f = lambda v: "?" if v is None else ("1" if v else "0")
        dbr = "??" if self.dbr is None else ("IN" if self.dbr == IN else f"{self.dbr:02X}")
        dp = "" if self.dp == 0 else ("D??" if self.dp is None else ("DIN" if self.dp == IN else f"D{self.dp:04X}"))
        return f"m{f(self.m8)}x{f(self.x8)}d{f(self.d)}b{dbr}" + dp + ("e" if self.e else "")


@dataclass
class State:
    m8: bool | None
    x8: bool | None
    d: bool | None
    dbr: int | None
    e: bool
    stack: tuple
    esc: int = 0        # bytes pulled from below this procedure's own pushes
    dp: int | None = 0  # D: a value, IN (the entry's) or None (unknown)

    def key(self):
        return (self.m8, self.x8, self.d, self.dbr, self.e, self.stack, self.esc, self.dp)


def exit_state(ex: "Ctx", site_dbr, site_dp) -> "State":
    """A forwarded exit, with the callee's IN bank and D resolved at the site."""
    return State(ex.m8, ex.x8, ex.d, site_dbr if ex.dbr == IN else ex.dbr, ex.e, (),
                 dp=site_dp if ex.dp == IN else ex.dp)


def join_state(a: "State", b: "State") -> "State":
    stack = join_stack(a.stack, b.stack) or a.stack
    return State(join_val(a.m8, b.m8), join_val(a.x8, b.x8), join_val(a.d, b.d),
                 join_val(a.dbr, b.dbr), a.e and b.e, stack, max(a.esc, b.esc), join_val(a.dp, b.dp))


def join_val(a, b):
    return a if a == b else None


def join_stack(a: tuple, b: tuple):
    if len(a) != len(b):
        return None
    out = []
    for x, y in zip(a, b):
        if x == y:
            out.append(x)
        elif x[0] == y[0]:
            out.append((x[0], None))
        else:
            out.append(("b", None))
    return tuple(out)


@dataclass
class Decoded:
    addr: int
    mnem: str
    mode: str
    length: int
    value: int
    raw: bytes


@dataclass
class Proc:
    """One procedure entered at `entry` in context `ctx`.

    Nodes are (address, State.key()) pairs: an address reached in two
    distinct states is analysed, and later translated, once per state.
    """
    entry: int
    ctx: Ctx
    states: dict = field(default_factory=dict)       # addr -> list[State]
    state_keys: dict = field(default_factory=dict)   # addr -> set of State.key()
    widened: dict = field(default_factory=dict)      # (addr, shape) -> representative
    widen_count: int = 0
    insns: dict = field(default_factory=dict)        # addr -> Decoded
    succ: dict = field(default_factory=dict)         # node -> list[(kind, node)]
    exits: dict = field(default_factory=dict)        # Ctx -> set of nodes
    calls: dict = field(default_factory=dict)        # node -> list[(target, Ctx)]
    conts: dict = field(default_factory=dict)        # (node, exit Ctx) -> node
    dynamic: dict = field(default_factory=dict)      # node -> description
    assumed: dict = field(default_factory=dict)      # dispatch node -> Ctx its handlers must return in
    traps: dict = field(default_factory=dict)        # node -> reason
    problems: list = field(default_factory=list)
    escapes: list = field(default_factory=list)      # (node, mnemonic, bytes below entry)
    escaped_to: set = field(default_factory=set)     # callers this procedure returns out of
    escape_exits: set = field(default_factory=set)   # (exit Ctx, bytes pulled below entry)
    inline: dict = field(default_factory=dict)       # call node -> inline argument bytes after it
    ret_at: dict = field(default_factory=dict)       # PER/PEA + JMP (ptr) node -> where the callee returns
    unpark: set = field(default_factory=set)         # RTS/RTL that returns through a parked return address

    @property
    def name(self) -> str:
        return f"P{self.entry:06X}_{self.ctx.short()}"

    def all_states(self, addr: int) -> list:
        return self.states.get(addr, [])


STATE_CAP = 8
OAM_RESET = 0x7F8000          # OAMResetRoutine, built by the reset code
OAM_RESET_RTL = 2 + 3 * 128   # offset of its RTL
DISPATCH_WIDTHS = (False, False, False, False)   # m8, x8, d, e at pointer dispatch sites
ESC_CAP = 16       # bytes a procedure may pull from below its entry frame
STACK_CAP = 64     # deeper abstract stacks are a loop that pushes more than it pulls


class Program:
    def __init__(self, listing: Listing):
        self.listing = listing
        self.rom = listing.rom
        self.procs: dict[tuple[int, Ctx], Proc] = {}
        self.callers: dict[tuple[int, Ctx], set] = {}
        self.work: list = []
        self.table_cache: dict[int, list[int] | None] = {}
        self.mid_entries = {off for _, off, _ in listing.mid_labels}
        self.glitch_entries: set[int] = set()
        self.exit_links: dict = {}
        self.dyn_callers: dict = {}
        self.seeds: set[int] = set()
        self.contexts: dict[int, set] = {}
        self.table_entries: set[int] = set()
        self.seed_width_cache: dict[int, tuple] = {}
        self.stack_jumps: dict[int, tuple] = {}
        self.ptr_jumps: dict[int, tuple] = {}
        self.table_at: dict[int, list] = {}
        self.decimal_calls: set[int] = set()
        self.inferred: dict[int, int] = {}    # pointer jump site -> table the listing names
        self.multi_bank: set[int] = set()     # entries analysed with the data bank left to the runtime
        self.multi_dp: set[int] = set()       # ... and with D left to the runtime
        syms = getattr(listing, "symbols", {})
        # SMW's inline jump tables: `JSL ExecutePtr` / `JSL ExecutePtrLong`
        # followed by a dw / dl table indexed by A (see execptr_targets).
        self.execptr = {syms[n]: unit for n, unit in (("ExecutePtr", 2), ("ExecutePtrLong", 3))
                        if n in syms}
        # YI: CODE_008365 is SMW's ExecutePtr, instruction for instruction.
        if "CODE_008365" in syms:
            self.execptr[syms["CODE_008365"]] = 2

    # -- decoding ----------------------------------------------------------
    def decode(self, addr: int, m8, x8) -> Decoded | str:
        idx = code_index(addr)
        if idx is None:
            return f"code outside ROM at ${addr:06X}"
        op = self.rom[idx]
        mnem, mode = O.OPCODES[op]
        n = O.operand_length(mode, m8, x8)
        if n is None:
            return f"${addr:06X} {mnem} immediate with unknown {'M' if mode == O.IMM_M else 'X'}"
        raw = self.rom[idx:idx + 1 + n]
        value = int.from_bytes(raw[1:], "little") if n else 0
        return Decoded(addr, mnem, mode, 1 + n, value, raw)

    def listing_insn(self, addr: int):
        idx = code_index(addr)
        return None if idx is None else self.listing.insn_at.get(idx)

    def aligned(self, addr: int) -> bool:
        """Whether a control transfer lands where the listing has code."""
        idx = code_index(addr)
        return idx is not None and (idx in self.listing.insn_at or idx in self.mid_entries)

    # -- jump tables ----------------------------------------------------------
    def table_block(self, table_addr: int, unit: int):
        """(k, entries): the listing's dw (unit 2) or dl (unit 3) block that
        starts k bytes after table_addr (k > 0 for `JSR (DATA_x-$01,x)`:
        tables addressed below their first entry, whose indices the code
        never produces)."""
        idx = code_index(table_addr)
        if idx is None:
            return None
        if (idx, unit) in self.table_cache:
            return self.table_cache[(idx, unit)]
        out = None
        items = self.listing.items
        for k in range(0, 4 * unit + 1):
            start = self.item_at(idx + k)
            if start is not None:
                break
        if start is not None:
            words = []
            j = start
            while j < len(items) and isinstance(items[j], Data) and items[j].unit == unit:
                n = items[j].size // unit
                for e in range(n):
                    o = items[j].offset + e * unit
                    words.append(int.from_bytes(self.rom[o:o + unit], "little"))
                j += 1
                if j < len(items) and isinstance(items[j], Label):
                    break
            if words:
                out = (k, words)
        self.table_cache[(idx, unit)] = out
        return out

    def table_words(self, table_addr: int, unit: int) -> list[int] | None:
        """Entries of the table at table_addr, index 0 at table_addr."""
        blk = self.table_block(table_addr, unit)
        if blk is None:
            return None
        k, words = blk
        return words if k == 0 else None

    def table_entries_any(self, table_addr: int, unit: int) -> list[int] | None:
        """Entries of the table at or just after table_addr."""
        blk = self.table_block(table_addr, unit)
        return None if blk is None else blk[1]

    def item_at(self, idx: int):
        if not hasattr(self, "_data_index"):
            self._data_index = {}
            for i, item in enumerate(self.listing.items):
                if isinstance(item, Data):
                    self._data_index.setdefault(item.offset, i)
        return self._data_index.get(idx)

    def table_targets(self, table_addr: int, bank: int, site: int | None = None) -> list[int] | None:
        """A (abs,x) jump table as bank:word targets, indexed by the byte
        offset X (None where no entry starts). When the table is addressed
        far below its start (`JMP (DATA_x-!Define_...,x)`), the label in the
        listing's operand at `site` locates it."""
        blk = self.table_block(table_addr, 2)
        if blk is None and site is not None:
            li = self.listing_insn(site)
            m = re.search(r"\b(DATA_[0-9A-F]{6})\b", li.operand) if li is not None else None
            sym = m and getattr(self.listing, "symbols", {}).get(m.group(1))
            if sym is not None and code_index(sym) is not None and code_index(table_addr) is not None:
                k = code_index(sym) - code_index(table_addr)
                inner = self.table_block(sym, 2)
                if 0 <= k < 0x1000 and inner is not None and inner[0] == 0:
                    blk = (k, inner[1])
        if blk is None:
            return None
        k, words = blk
        out = [None] * (k + 2 * len(words))
        for j, w in enumerate(words):
            out[k + 2 * j] = (bank << 16) | w
        return out

    def ptr_targets(self, site: int) -> list[int]:
        """Targets of a pointer jump listed in jumps.txt."""
        tables, unit, _ = self.ptr_jumps[site]
        if unit == 0:
            return list(tables)                  # "= $T1,$T2,...": explicit targets
        if isinstance(tables, tuple):
            cols = [self.table_entries_any(t, 1) or [] for t in tables]
            return [lo | mid << 8 | hi << 16 for lo, mid, hi in zip(*cols)]
        words = self.table_entries_any(tables, unit) or []
        return [(site & 0xFF0000) | w if unit == 2 else w for w in words]

    def infer_ptr(self, p: Proc, site: int, mode: str) -> list[int]:
        """`LDA.w DATA_x-K,y : STA $00 [: SEP] : JMP ($xxxx)`: the targets are
        the dw table the listing names in the load (reported in
        self.inferred for review). Only for JMP (abs) through a 16-bit
        pointer in the site's bank."""
        if mode != O.ABSI:
            return []
        a = site
        for _ in range(6):
            prev = [d for d in p.insns.values() if d.addr + d.length == a]
            if len(prev) != 1:
                return []
            d = prev[0]
            a = d.addr
            if d.mnem == "LDA" and d.mode in (O.ABSX, O.ABSY):
                li = self.listing_insn(a)
                m = re.search(r"\b(DATA_[0-9A-F]{6})\b", li.operand) if li is not None else None
                sym = m and getattr(self.listing, "symbols", {}).get(m.group(1))
                if sym is None:
                    return []
                words = self.table_entries_any(sym, 2) or []
                self.inferred[site] = sym
                return [(site & 0xFF0000) | w for w in words]
        return []

    def execptr_targets(self, site: int, length: int, unit: int) -> list[int] | None:
        """The table after a JSL ExecutePtr(Long) at site: 16-bit entries in
        the site's bank, or 24-bit entries."""
        words = self.table_words(site + length, unit)
        if words is None:
            return None
        if unit == 2:
            return [(site & 0xFF0000) | w for w in words]
        return words

    # -- driver -----------------------------------------------------------------
    def proc(self, entry: int, ctx: Ctx) -> Proc:
        key = (entry, ctx)
        p = self.procs.get(key)
        if p is None:
            p = Proc(entry, ctx)
            self.procs[key] = p
            dbr = IN if ctx.dbr is None else ctx.dbr
            dp = IN if ctx.dp is None else ctx.dp
            self.flow(p, None, "entry", entry, State(ctx.m8, ctx.x8, ctx.d, dbr, ctx.e, (), dp=dp))
        return p

    def run(self, entries: list[tuple[int, Ctx]]) -> None:
        for addr, ctx in entries:
            self.proc(addr, ctx)
        while self.work:
            key, addr, st = self.work.pop()
            self.step(self.procs[key], addr, st)

    def flow(self, p: Proc, src, kind: str, addr: int, st: State):
        """Record an edge to (addr, st).

        An address holds up to STATE_CAP exact states. Beyond that, each
        state shape (widths, decimal, emulation, stack depth, escape) gets
        one representative that absorbs later states of that shape by
        joining only the data bank and pushed values.
        """
        if len(st.stack) > STACK_CAP:
            if src is not None:
                p.traps[src] = f"stack grows past {STACK_CAP} bytes toward ${addr:06X}"
            return None
        if st.esc > ESC_CAP:
            if src is not None:
                p.traps[src] = f"pulls {st.esc} bytes below its entry frame toward ${addr:06X}"
            return None
        keys = p.state_keys.setdefault(addr, set())
        key = st.key()
        if key not in keys:
            states = p.states.setdefault(addr, [])
            if len(states) < STATE_CAP:
                states.append(st)
                keys.add(key)
                self.work.append(((p.entry, p.ctx), addr, st))
            else:
                shape = (st.m8, st.x8, st.d, st.e, len(st.stack), st.esc, st.dp)
                rep = p.widened.get((addr, shape))
                w = st if rep is None else join_state(rep, st)
                if rep is None or w.key() != rep.key():
                    p.widened[(addr, shape)] = w
                    if w.key() not in keys:
                        states.append(w)
                        keys.add(w.key())
                        self.work.append(((p.entry, p.ctx), addr, w))
                    p.widen_count += 1
                st = w
                key = st.key()
        if src is not None:
            edges = p.succ.setdefault(src, [])
            edge = (kind, (addr, key))
            if edge not in edges:
                edges.append(edge)
        return (addr, key)

    def canon(self, target: int, st: State, open_bank: bool = False) -> Ctx:
        dbr = None if open_bank or st.dbr == IN else st.dbr
        dp = None if st.dp == IN else st.dp
        # YI sets the decimal flag only inside SED ... CLD windows that call
        # nothing (checked in the listing), so D is clear at every call.
        if st.d is not False:
            self.decimal_calls.add(target)
        if target in self.multi_bank:
            dbr = None
        if target in self.multi_dp:
            dp = None
        ctx = Ctx(st.m8, st.x8, False, dbr, st.e, dp)
        known = self.contexts.setdefault(target, set())
        if ctx not in known and len(known) >= CTX_CAP:
            ctx = Ctx(st.m8, st.x8, st.d, None, st.e, dp)
            if ctx not in known and len(known) >= 2 * CTX_CAP:
                ctx = Ctx(st.m8, st.x8, st.d, None, st.e, None)
        known.add(ctx)
        return ctx

    def call(self, p: Proc, node, site: int, st: State, target: int, open_bank: bool = False) -> None:
        ctx = self.canon(target, st, open_bank)
        if not self.aligned(target):
            self.glitch_entries.add(target)
        callee = self.proc(target, ctx)
        entries = p.calls.setdefault(node, [])
        if (target, ctx) not in entries:
            entries.append((target, ctx))
        for ex, esc in list(callee.escape_exits):
            self.escape_into(callee, (p.entry, p.ctx), node, site, ex, esc)
        if open_bank:
            # Escapes from a dispatched handler still leave through this site.
            self.dyn_callers.setdefault((target, ctx), set()).add(((p.entry, p.ctx), node, site))
            # A pointer dispatch: continue as if the handler returns in the
            # widths, decimal mode and bank it was called with. The emitted
            # site checks the exit the handler actually took.
            p.assumed[node] = Ctx(st.m8, st.x8, st.d, st.dbr, st.e, st.dp)
            if (node, "assumed") not in p.conts:
                assumed = Ctx(st.m8, st.x8, st.d, IN, st.e, IN)
                self.resume_after_call(p, node, site, st, assumed)
                p.conts[(node, "assumed")] = p.conts[(node, assumed)]
            return
        self.callers.setdefault((target, ctx), set()).add(((p.entry, p.ctx), node, site))
        for ex in list(callee.exits):
            self.resume_after_call(p, node, site, st, ex)

    def seeds_for(self, bank: int | None, st: State) -> list[int]:
        """Seeds in the site's bank whose own entry widths match the site."""
        out = []
        for t in sorted(self.seeds):
            if bank is not None and t >> 16 != bank:
                continue
            if t in self.table_entries:
                continue
            m8, x8 = self.seed_widths(t)
            if (m8 is None or m8 == st.m8) and (x8 is None or x8 == st.x8):
                out.append(t)
        return out

    def seed_widths(self, addr: int):
        """Entry widths a seed's own code implies: the size the listing gives
        its first M- or X-width immediate before any REP/SEP sets that flag."""
        if addr in self.seed_width_cache:
            return self.seed_width_cache[addr]
        m8 = x8 = None
        m_set = x_set = False
        idx = code_index(addr)
        for _ in range(48):
            li = self.listing.insn_at.get(idx) if idx is not None else None
            if li is None:
                break
            mnem, mode = O.OPCODES[li.opcode]
            if mnem in ("REP", "SEP"):
                v = li.raw[1]
                m_set |= bool(v & 0x20)
                x_set |= bool(v & 0x10)
            elif mode == O.IMM_M and not m_set and m8 is None:
                m8 = li.size == 2
            elif mode == O.IMM_X and not x_set and x8 is None:
                x8 = li.size == 2
            if mnem in ("RTS", "RTL", "RTI", "JMP", "JML", "BRA", "BRL") or (m_set or m8 is not None) and (x_set or x8 is not None):
                break
            idx += li.size
        self.seed_width_cache[addr] = (m8, x8)
        return m8, x8

    def tail_call(self, p: Proc, node, st: State, target: int, keep_bank: bool = False) -> None:
        """A dynamic JMP: the target runs as its own procedure, and its
        returns are this procedure's returns (the 68k keeps one frame)."""
        if st.stack or st.esc:
            p.problems.append(f"${node[0]:06X} dynamic jump with {len(st.stack)} bytes pushed")
        ctx = self.canon(target, st, open_bank=not keep_bank)
        callee = self.proc(target, ctx)
        tails = p.calls.setdefault(node, [])
        if (target, ctx) not in tails:
            tails.append((target, ctx))
        link = self.exit_links.setdefault((target, ctx), set())
        entry = ((p.entry, p.ctx), st.dbr, st.dp)
        if entry not in link:
            link.add(entry)
            for ex in list(callee.exits):
                self.finish_exit(p, ("tail", target), exit_state(ex, st.dbr, st.dp))

    def resume_after_call(self, p: Proc, node, site: int, st: State, ex: Ctx) -> None:
        ins = p.insns[site]
        after = State(ex.m8, ex.x8, ex.d, st.dbr if ex.dbr == IN else ex.dbr, ex.e, st.stack, st.esc,
                      st.dp if ex.dp == IN else ex.dp)
        ret = p.ret_at.get(node, site + ins.length + p.inline.get(node, 0))
        dest = self.flow(p, node, "return", ret, after)
        p.conts[(node, ex)] = dest

    def escape_into(self, p: Proc, ckey, cnode, site: int, ex: "Ctx", esc: int) -> None:
        """An escaped return of p leaves the caller at (ckey, site) when it
        pulled exactly the caller's return frame plus the caller's pushes."""
        caller = self.procs[ckey]
        frame = 3 if caller.insns[site].mnem == "JSL" else 2
        cst = next(o for o in caller.states[site] if o.key() == cnode[1])
        if esc != frame + len(cst.stack):
            p.problems.append(f"returns {esc} bytes below a {frame}-byte frame at ${site:06X} "
                              f"(caller had pushed {len(cst.stack)})")
            return
        p.escaped_to.add(ckey)
        st = exit_state(ex, cst.dbr, cst.dp)
        # A caller that had already pulled its own return address (a parked
        # return, see unpark) is left as well: the escape continues upward.
        st.esc = cst.esc
        self.finish_exit(caller, ("escape", p.entry), st)

    def finish_exit(self, p: Proc, node, st: State) -> None:
        ex = Ctx(st.m8, st.x8, st.d, st.dbr, st.e, st.dp)
        if st.esc:
            # The procedure pulled its return address and, beyond it, what
            # the caller had pushed before calling: this return leaves the
            # caller as well.
            key = (p.entry, p.ctx)
            if (ex, st.esc) in p.escape_exits:
                return
            p.escape_exits.add((ex, st.esc))
            sites = list(self.callers.get(key, ())) + list(self.dyn_callers.get(key, ()))
            for (ckey, cnode, site) in sites:
                self.escape_into(p, ckey, cnode, site, ex, st.esc)
            return
        new = ex not in p.exits
        p.exits.setdefault(ex, set()).add(node)
        if new:
            for tkey, site_dbr, site_dp in list(self.exit_links.get((p.entry, p.ctx), ())):
                self.finish_exit(self.procs[tkey], ("tail", p.entry), exit_state(ex, site_dbr, site_dp))
            for (ckey, cnode, site) in list(self.callers.get((p.entry, p.ctx), ())):
                caller = self.procs[ckey]
                cst = next(o for o in caller.states[site] if o.key() == cnode[1])
                self.resume_after_call(caller, cnode, site, cst, ex)

    def falls_into(self, p: Proc, src: int, dst: int) -> bool:
        """Whether dst is reached only by falling through from src."""
        for n, edges in p.succ.items():
            for kind, (a, _) in edges:
                if a == dst and not (n[0] == src and kind == "fall"):
                    return False
        return True

    # -- one instruction ----------------------------------------------------------
    def step(self, p: Proc, addr: int, st: State) -> None:
        node = (addr, st.key())
        d = self.decode(addr, st.m8, st.x8)
        if isinstance(d, str):
            p.problems.append(d)
            p.traps[node] = d
            return
        prev = p.insns.get(addr)
        if prev is not None and prev.length != d.length:
            p.problems.append(f"${addr:06X} decodes to {prev.length} and {d.length} bytes in two states")
        p.insns[addr] = d
        li = self.listing_insn(addr)
        if li is not None and li.size != d.length:
            p.problems.append(f"${addr:06X} {d.mnem}: decoded {d.length} bytes, listing {li.size} "
                              f"(m8={st.m8} x8={st.x8})")
        mn, mode = d.mnem, d.mode
        bank = addr & 0xFF0000
        nxt = addr + d.length
        s = State(st.m8, st.x8, st.d, st.dbr, st.e, st.stack, st.esc, st.dp)

        def push(*entries):
            s.stack = s.stack + tuple(entries)

        def pull(n: int):
            vals = []
            below = 0
            for _ in range(n):
                if s.stack:
                    vals.append(s.stack[-1])
                    s.stack = s.stack[:-1]
                else:
                    below += 1
                    vals.append(("b", None))
            if below:
                p.escapes.append((node, mn, below))
                s.esc += below
            return vals

        # Control flow.
        if mode in (O.REL8, O.REL16):
            if mode == O.REL8:
                off = d.value - 0x100 if d.value & 0x80 else d.value
            else:
                off = d.value - 0x10000 if d.value & 0x8000 else d.value
            target = bank | ((nxt + off) & 0xFFFF)
            if mn == "PER":
                push(("b", target >> 8 & 0xFF), ("b", target & 0xFF))
                self.flow(p, node, "fall", nxt, s)
                return
            self.flow(p, node, "branch", target, State(**vars(s)))
            if mn not in ("BRA", "BRL"):
                self.flow(p, node, "fall", nxt, s)
            return
        if mn in ("JSL", "JSR", "JML", "JMP") and mode in (O.ABS, O.LONG) and \
                ((bank | d.value) if mode == O.ABS else d.value) in self.execptr:
            target = (bank | d.value) if mode == O.ABS else d.value
            unit = self.execptr[target]
            if mn != "JSL":
                # `PHK : PER $0003 : JML ExecutePtr` is a JSL written out:
                # the pushed return address points at the JML's last byte.
                per = p.insns.get(addr - 3)
                phk = p.insns.get(addr - 4)
                if mn != "JML" or not per or per.mnem != "PER" or per.value != 3 or \
                        not phk or phk.mnem != "PHK" or len(s.stack) < 3:
                    p.problems.append(f"${addr:06X} {mn} to ExecutePtr without PHK/PER $0003")
                    p.traps[node] = f"{mn} into ExecutePtr"
                    return
                s.stack = s.stack[:-3]
            if st.x8 is not True:
                p.problems.append(f"${addr:06X} JSL ExecutePtr with X width {st.x8}")
            targets = self.execptr_targets(addr, d.length, unit)
            if targets is None:
                p.traps[node] = "ExecutePtr without a table"
                p.problems.append(f"${addr:06X} JSL ExecutePtr: no table in the listing")
                return
            p.dynamic[node] = f"execptr{unit} ${addr + d.length:06X}"
            # `dw $0000` / `dl 0` mark unused slots
            targets = [t for t in targets if rom_index(t) is not None]
            self.table_entries.update(targets)
            for t in targets:
                if not self.aligned(t):
                    self.glitch_entries.add(t)
                self.flow(p, node, "jump", t, State(True, True, s.d, s.dbr, s.e, s.stack, s.esc, s.dp))
            return
        if mn == "JSL" and OAM_RESET <= d.value <= OAM_RESET + OAM_RESET_RTL:
            # Code the reset routine builds in WRAM: `LDA #$F0`, then
            # `STA $0201+4k` for k = 0..127, then RTL. Run natively
            # (rt_oamreset); callers enter at the LDA or at a STA.
            off = d.value - OAM_RESET
            if off != 0 and (off - 2) % 3:
                p.traps[node] = f"JSL into the middle of OAMResetRoutine (+{off})"
                return
            if st.m8 is not True or not (isinstance(st.dbr, int) and (st.dbr & 0x7F) < 0x40):
                p.problems.append(f"${addr:06X} OAMResetRoutine with m8={st.m8} dbr={st.dbr}")
            p.dynamic[node] = f"oamreset {0 if off == 0 else (off - 2) // 3} {int(off == 0)}"
            self.flow(p, node, "fall", nxt, s)
            return
        if mn == "JSL" and d.value in INLINE_ARGS:
            p.inline[node] = INLINE_ARGS[d.value]
        if mn in ("JMP", "JML", "JSR", "JSL"):
            is_call = mn in ("JSR", "JSL")
            if mode == O.ABS:
                targets = [bank | d.value]
            elif mode == O.LONG:
                targets = [d.value]
            elif mode == O.ABSIX:
                targets = self.table_targets(bank | d.value, bank >> 16, addr)
                if targets is not None:
                    self.table_at[addr] = targets
            else:
                targets = None
            if targets is None:
                # A jump through a pointer: its targets come from jumps.txt;
                # an unlisted target stops the program at runtime with its
                # address. `PER/PEA ret-1 : JMP (ptr)` is a call.
                p.dynamic[node] = f"{mn} {mode} ${d.value:04X}"
                cst = st
                if not is_call and len(s.stack) >= 2 and \
                        all(e[0] == "b" and isinstance(e[1], int) for e in s.stack[-2:]):
                    p.ret_at[node] = bank | ((((s.stack[-2][1] << 8) | s.stack[-1][1]) + 1) & 0xFFFF)
                    cst = State(st.m8, st.x8, st.d, st.dbr, st.e, s.stack[:-2], st.esc, st.dp)
                call_like = is_call or node in p.ret_at
                tlist = self.ptr_targets(addr) if addr in self.ptr_jumps else self.infer_ptr(p, addr, mode)
                keep = addr in self.ptr_jumps and self.ptr_jumps[addr][2]
                if not tlist:
                    p.problems.append(f"${addr:06X} {mn} through a pointer without targets (jumps.txt)")
                for t in tlist:
                    if code_index(t) is None:
                        continue
                    tst = cst
                    if keep:
                        tst = State(cst.m8, cst.x8, cst.d, t >> 16, cst.e, cst.stack, cst.esc, cst.dp)
                    if call_like:
                        self.call(p, node, addr, tst, t, open_bank=True)
                    else:
                        self.tail_call(p, node, tst, t, keep_bank=keep)
                if call_like and (node, "assumed") not in p.conts:
                    p.assumed[node] = Ctx(cst.m8, cst.x8, cst.d, cst.dbr, cst.e, cst.dp)
                    assumed = Ctx(cst.m8, cst.x8, cst.d, IN, cst.e, IN)
                    self.resume_after_call(p, node, addr, cst, assumed)
                    p.conts[(node, "assumed")] = p.conts[(node, assumed)]
                return
            if mode == O.ABSIX:
                p.dynamic[node] = f"table ${bank | d.value:06X}"
                targets = [t for t in targets if t is not None]
                self.table_entries.update(targets)
            if is_call:
                for t in targets:
                    self.call(p, node, addr, st, t)
            else:
                for t in targets:
                    if not self.aligned(t):
                        self.glitch_entries.add(t)
                    self.flow(p, node, "jump", t, State(**vars(s)))
            return
        if mn in ("RTS", "RTL") and addr in self.stack_jumps and len(s.stack) >= (2 if mn == "RTS" else 3):
            # The routine pushed a table entry minus one and "returns" to
            # it: a jump inside this procedure (stack_jumps.txt).
            table, unit, dbr_is_bank = self.stack_jumps[addr]
            n = 2 if mn == "RTS" else 3
            if len(s.stack) < n:
                p.traps[node] = f"stack jump with {len(s.stack)} bytes pushed"
                p.problems.append(f"${addr:06X} {mn} stack jump with {len(s.stack)} bytes pushed")
                return
            words = self.table_entries_any(table, unit)
            if not words:
                p.traps[node] = "stack jump without a table"
                return
            p.dynamic[node] = f"stackjump {n}"
            targets = []
            for w in words:
                if w is None:
                    continue
                t = (bank | ((w + 1) & 0xFFFF)) if unit == 2 else ((w & 0xFF0000) | ((w + 1) & 0xFFFF))
                if rom_index(t) is None and code_index(t) is None:
                    continue
                targets.append(t)
            p.stack_targets = getattr(p, "stack_targets", {})
            p.stack_targets[node] = targets
            rest = s.stack[:-n]
            for t in sorted(set(targets)):
                if not self.aligned(t):
                    self.glitch_entries.add(t)
                dbr = (t >> 16) if dbr_is_bank else s.dbr
                self.flow(p, node, "jump", t, State(s.m8, s.x8, s.d, dbr, s.e, rest, s.esc, s.dp))
            return
        if mn in ("RTS", "RTL") and s.esc and len(s.stack) == s.esc:
            # `PLA : STA $00 ... LDA $00 : PHA : RTS`: the routine parked
            # its return address and pushed it back; an ordinary return
            # (the emitter keeps the 68k return address in a slot).
            p.unpark.add(node)
            self.finish_exit(p, node, State(s.m8, s.x8, s.d, s.dbr, s.e, (), 0, s.dp))
            return
        if mn in ("RTS", "RTL", "RTI"):
            if s.stack:
                p.problems.append(f"${addr:06X} {mn} with {len(s.stack)} bytes still pushed")
            self.finish_exit(p, node, s)
            return
        if mn in ("BRK", "COP", "STP", "WDM", "WAI"):
            p.traps[node] = f"reached {mn}"
            return

        # Mode and register-state changes.
        if mn == "REP":
            if d.value & 0x20: s.m8 = False
            if d.value & 0x10: s.x8 = False
            if d.value & 0x08: s.d = False
            if s.e:
                s.m8 = s.x8 = True
        elif mn == "SEP":
            if d.value & 0x20: s.m8 = True
            if d.value & 0x10: s.x8 = True
            if d.value & 0x08: s.d = True
        elif mn == "CLD":
            s.d = False
        elif mn == "SED":
            s.d = True
        elif mn == "XCE":
            carry = None
            for back in (1, 2):
                prev = p.insns.get(addr - back)
                if prev is not None and prev.addr + prev.length == addr:
                    if prev.mnem == "CLC" or prev.mnem == "REP" and prev.value & 1:
                        carry = 0
                    elif prev.mnem == "SEC" or prev.mnem == "SEP" and prev.value & 1:
                        carry = 1
            if carry == 0:
                s.e = False
            elif carry == 1:
                s.e, s.m8, s.x8 = True, True, True
            else:
                p.problems.append(f"${addr:06X} XCE with unknown carry")
        elif mn == "PHP":
            push(("p", (s.m8, s.x8, s.d)))
        elif mn == "PLP":
            (v,) = pull(1)
            if v[0] == "p" and v[1] is not None:
                s.m8, s.x8, s.d = v[1]
            else:
                s.m8 = s.x8 = None
                s.d = False            # see canon(): D is clear outside SED ... CLD
        elif mn == "PHK":
            push(("b", bank >> 16))
        elif mn == "PHB":
            push(("b", s.dbr))
        elif mn == "PLB":
            (v,) = pull(1)
            s.dbr = v[1] if v[0] == "b" else None
        elif mn == "PEA":
            push(("b", d.value >> 8), ("b", d.value & 0xFF))
        elif mn == "PEI":
            push(("b", None), ("b", None))
        elif mn == "PHA":
            push(*([("b", None)] * (1 if s.m8 else 2)))
        elif mn in ("PHX", "PHY"):
            push(*([("b", None)] * (1 if s.x8 else 2)))
        elif mn == "PHD":
            push(("dh", s.dp), ("dl", s.dp))
        elif mn == "PLA":
            pull(1 if s.m8 else 2)
        elif mn in ("PLX", "PLY"):
            pull(1 if s.x8 else 2)
        elif mn == "PLD":
            lo, hi = pull(2)
            if addr in KNOWN_D:
                s.dp = KNOWN_D[addr]
            elif lo[0] == "dl" and hi[0] == "dh" and lo[1] == hi[1]:
                s.dp = lo[1]                      # PHD ... PLD
            elif lo[0] == hi[0] == "b" and isinstance(lo[1], int) and isinstance(hi[1], int):
                s.dp = hi[1] << 8 | lo[1]         # PEA #$xxxx : PLD
            else:
                s.dp = None
        elif mn == "TCD":
            prev = p.insns.get(addr - 3)
            if prev is not None and prev.mnem == "LDA" and prev.mode == O.IMM_M and prev.length == 3 \
                    and self.falls_into(p, addr - 3, addr):
                s.dp = prev.value                 # LDA.w #$xxxx : TCD
            else:
                s.dp = None
        elif mn in ("TCS", "TXS"):
            s.stack = ()
        if mn in ("PHA", "PLA") and s.m8 is None or mn in ("PHX", "PHY", "PLX", "PLY") and s.x8 is None:
            p.problems.append(f"${addr:06X} {mn} with unknown width")
        self.flow(p, node, "fall", nxt, s)


def vector(rom: bytes, offset: int) -> int:
    return struct.unpack_from("<H", rom, offset)[0]


IDENT = re.compile(r"(?<![A-Za-z0-9_$%])\.?[A-Za-z_][A-Za-z0-9_]*")


def address_taken(listing: Listing) -> set[int]:
    """Code addresses the listing uses as data: dw/dl elements and immediates
    that name a label placed on an instruction."""
    syms = getattr(listing, "symbols", {})
    code = set(listing.insn_at)
    out: set[int] = set()

    def scan(text: str, glob: str):
        for m in IDENT.finditer(text):
            name = m.group()
            if name.startswith("."):
                name = f"{glob}_{name[1:]}"
            a = syms.get(name)
            if a is not None and rom_index(a) in code:
                out.add(a)
    glob = ""
    for item in listing.items:
        if isinstance(item, Label):
            glob = getattr(item, "glob", item.name)
        elif isinstance(item, Data) and item.words:
            for w in item.words:
                scan(w, glob)
        elif isinstance(item, Insn) and "#" in item.operand:
            scan(item.operand.split("#", 1)[1], glob)
    return out


def extra_targets(path: Path) -> set[int]:
    """Runtime-discovered dispatch targets, one $BBAAAA per line."""
    out: set[int] = set()
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                out.add(int(line.lstrip("$"), 16))
    return out


def read_jumps(path: Path) -> tuple[dict, dict]:
    """jumps.txt: (stack jumps, pointer jumps), site -> (table, unit, dbr).
    A pointer jump's table is an address or three (lo/mid/hi byte tables)."""
    stack: dict[int, tuple] = {}
    ptr: dict[int, tuple] = {}
    if path.exists():
        for line in path.read_text().splitlines():
            f = line.split("#", 1)[0].split()
            if not f:
                continue
            site = int(f[0].lstrip("$"), 16)
            if f[2] == "=":                      # explicit targets
                entry = (tuple(int(t.lstrip("$"), 16) for t in f[3].split(",")), 0, "dbr" in f[4:])
                ptr[site] = entry
                continue
            tables = tuple(int(t.lstrip("$"), 16) for t in f[2].split("/"))
            entry = (tables if len(tables) > 1 else tables[0], int(f[3]), "dbr" in f[4:])
            (stack if f[1] == "stack" else ptr)[site] = entry
    return stack, ptr


def analyse(listing: Listing, extra: Path | None = None, jumps: Path | None = None) -> Program:
    """Analyse from the vectors. Entries reached with more than one data
    bank (or D) are analysed once with it left to the runtime; since that
    changes what their callees see, passes repeat until no entry changes."""
    multi_bank: set[int] = set()
    multi_dp: set[int] = set()
    for _ in range(8):
        prog = analyse_once(listing, extra, jumps, multi_bank, multi_dp)
        banks: dict[int, set] = {}
        dps: dict[int, set] = {}
        for (entry, ctx) in prog.procs:
            banks.setdefault(entry, set()).add(ctx.dbr)
            dps.setdefault(entry, set()).add(ctx.dp)
        more_bank = {a for a, v in banks.items() if len(v) > 1} - multi_bank
        more_dp = {a for a, v in dps.items() if len(v) > 1} - multi_dp
        if not more_bank and not more_dp:
            break
        multi_bank |= more_bank
        multi_dp |= more_dp
    return prog


def analyse_once(listing: Listing, extra, jumps, multi_bank, multi_dp) -> Program:
    rom = listing.rom
    prog = Program(listing)
    prog.multi_bank, prog.multi_dp = set(multi_bank), set(multi_dp)
    if jumps is not None:
        prog.stack_jumps, prog.ptr_jumps = read_jumps(jumps)
    prog.seeds = address_taken(listing) | (extra_targets(extra) if extra else set())
    reset = vector(rom, rom_index(0xFFFC))
    nmi = vector(rom, rom_index(0xFFEA))
    irq = vector(rom, rom_index(0xFFEE))
    entries = [
        (reset, Ctx(True, True, False, 0x00, True)),
        # Interrupts arrive in whatever widths the interrupted code had; the
        # handlers establish their own with REP/SEP before any width matters.
        # The 65816 clears D when it takes an interrupt (YI's score code
        # does use SED).
        (nmi, Ctx(None, None, False, None, False, None)),
        (irq, Ctx(None, None, False, None, False, None)),
    ]
    prog.run(entries)
    return prog
