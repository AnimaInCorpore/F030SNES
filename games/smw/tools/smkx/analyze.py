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

from .listing import Listing, Data, Label, Insn

UNKNOWN = None
IN = "IN"          # the data bank the caller had on entry
CTX_CAP = 4        # contexts per entry before the DBR is left to the runtime


def rom_index(addr: int) -> int | None:
    """ROM file offset of a 24-bit CPU address (LoROM), or None for non-ROM
    space. SMW's cartridge is plain LoROM: every bank but $7E/$7F maps ROM
    at $8000-$FFFF, 32 KiB per bank; the lower halves hold WRAM, I/O and,
    in banks $70-$7D, SRAM, all resolved by the runtime."""
    bank, pc = addr >> 16, addr & 0xFFFF
    if bank in (0x7E, 0x7F) or pc < 0x8000:
        return None
    return (((bank & 0x7F) << 15) | (pc & 0x7FFF)) & 0x7FFFF


@dataclass(frozen=True)
class Ctx:
    m8: bool | None
    x8: bool | None
    d: bool | None
    dbr: int | None
    e: bool = False

    def short(self) -> str:
        f = lambda v: "?" if v is None else ("1" if v else "0")
        dbr = "??" if self.dbr is None else ("IN" if self.dbr == IN else f"{self.dbr:02X}")
        return f"m{f(self.m8)}x{f(self.x8)}d{f(self.d)}b{dbr}" + ("e" if self.e else "")


@dataclass
class State:
    m8: bool | None
    x8: bool | None
    d: bool | None
    dbr: int | None
    e: bool
    stack: tuple
    esc: int = 0        # bytes pulled from below this procedure's own pushes

    def key(self):
        return (self.m8, self.x8, self.d, self.dbr, self.e, self.stack, self.esc)


def exit_state(ex: "Ctx", site_dbr) -> "State":
    """A forwarded exit, with the callee's IN bank resolved at the site."""
    return State(ex.m8, ex.x8, ex.d, site_dbr if ex.dbr == IN else ex.dbr, ex.e, ())


def join_state(a: "State", b: "State") -> "State":
    stack = join_stack(a.stack, b.stack) or a.stack
    return State(join_val(a.m8, b.m8), join_val(a.x8, b.x8), join_val(a.d, b.d),
                 join_val(a.dbr, b.dbr), a.e and b.e, stack, max(a.esc, b.esc))


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
        syms = getattr(listing, "symbols", {})
        # SMW's inline jump tables: `JSL ExecutePtr` / `JSL ExecutePtrLong`
        # followed by a dw / dl table indexed by A (see execptr_targets).
        self.execptr = {syms[n]: unit for n, unit in (("ExecutePtr", 2), ("ExecutePtrLong", 3))
                        if n in syms}

    # -- decoding ----------------------------------------------------------
    def decode(self, addr: int, m8, x8) -> Decoded | str:
        idx = rom_index(addr)
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
        idx = rom_index(addr)
        return None if idx is None else self.listing.insn_at.get(idx)

    def aligned(self, addr: int) -> bool:
        """Whether a control transfer lands where the listing has code."""
        idx = rom_index(addr)
        return idx is not None and (idx in self.listing.insn_at or idx in self.mid_entries)

    # -- jump tables ----------------------------------------------------------
    def table_words(self, table_addr: int, unit: int) -> list[int] | None:
        """Entries of the listing's dw (unit 2) or dl (unit 3) block at table_addr."""
        idx = rom_index(table_addr)
        if idx is None:
            return None
        if (idx, unit) in self.table_cache:
            return self.table_cache[(idx, unit)]
        words = None
        items = self.listing.items
        start = self.item_at(idx)
        if start is not None:
            words = []
            j = start
            while j < len(items) and isinstance(items[j], Data) and items[j].unit == unit:
                n = items[j].size // unit
                for k in range(n):
                    o = items[j].offset + k * unit
                    words.append(int.from_bytes(self.rom[o:o + unit], "little"))
                j += 1
                if j < len(items) and isinstance(items[j], Label):
                    break
            if not words:
                words = None
        self.table_cache[(idx, unit)] = words
        return words

    def item_at(self, idx: int):
        if not hasattr(self, "_data_index"):
            self._data_index = {}
            for i, item in enumerate(self.listing.items):
                if isinstance(item, Data):
                    self._data_index.setdefault(item.offset, i)
        return self._data_index.get(idx)

    def table_targets(self, table_addr: int, bank: int) -> list[int] | None:
        """Words of the listing's dw block at table_addr, as bank:word targets."""
        words = self.table_words(table_addr, 2)
        if words is None:
            return None
        return [(bank << 16) | w for w in words]

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
            self.flow(p, None, "entry", entry, State(ctx.m8, ctx.x8, ctx.d, dbr, ctx.e, ()))
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
                shape = (st.m8, st.x8, st.d, st.e, len(st.stack), st.esc)
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
        ctx = Ctx(st.m8, st.x8, st.d, dbr, st.e)
        known = self.contexts.setdefault(target, set())
        if ctx not in known and len(known) >= CTX_CAP:
            ctx = Ctx(st.m8, st.x8, st.d, None, st.e)
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
            p.assumed[node] = Ctx(st.m8, st.x8, st.d, st.dbr, st.e)
            if (node, "assumed") not in p.conts:
                assumed = Ctx(st.m8, st.x8, st.d, IN, st.e)
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
        idx = rom_index(addr)
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

    def tail_call(self, p: Proc, node, st: State, target: int) -> None:
        """A dynamic JMP: the target runs as its own procedure, and its
        returns are this procedure's returns (the 68k keeps one frame)."""
        if st.stack or st.esc:
            p.problems.append(f"${node[0]:06X} dynamic jump with {len(st.stack)} bytes pushed")
        ctx = self.canon(target, st, open_bank=True)
        callee = self.proc(target, ctx)
        tails = p.calls.setdefault(node, [])
        if (target, ctx) not in tails:
            tails.append((target, ctx))
        link = self.exit_links.setdefault((target, ctx), set())
        entry = ((p.entry, p.ctx), st.dbr)
        if entry not in link:
            link.add(entry)
            for ex in list(callee.exits):
                self.finish_exit(p, ("tail", target), exit_state(ex, st.dbr))

    def resume_after_call(self, p: Proc, node, site: int, st: State, ex: Ctx) -> None:
        ins = p.insns[site]
        after = State(ex.m8, ex.x8, ex.d, st.dbr if ex.dbr == IN else ex.dbr, ex.e, st.stack, st.esc)
        dest = self.flow(p, node, "return", site + ins.length, after)
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
        self.finish_exit(caller, ("escape", p.entry), exit_state(ex, cst.dbr))

    def finish_exit(self, p: Proc, node, st: State) -> None:
        ex = Ctx(st.m8, st.x8, st.d, st.dbr, st.e)
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
            for tkey, site_dbr in list(self.exit_links.get((p.entry, p.ctx), ())):
                self.finish_exit(self.procs[tkey], ("tail", p.entry), exit_state(ex, site_dbr))
            for (ckey, cnode, site) in list(self.callers.get((p.entry, p.ctx), ())):
                caller = self.procs[ckey]
                cst = next(o for o in caller.states[site] if o.key() == cnode[1])
                self.resume_after_call(caller, cnode, site, cst, ex)

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
        s = State(st.m8, st.x8, st.d, st.dbr, st.e, st.stack, st.esc)

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
                push(("b", None), ("b", None))
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
                self.flow(p, node, "jump", t, State(True, True, s.d, s.dbr, s.e, s.stack, s.esc))
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
        if mn in ("JMP", "JML", "JSR", "JSL"):
            is_call = mn in ("JSR", "JSL")
            if mode == O.ABS:
                targets = [bank | d.value]
            elif mode == O.LONG:
                targets = [d.value]
            elif mode == O.ABSIX:
                targets = self.table_targets(bank | d.value, bank >> 16)
            else:
                targets = None
            if targets is None:
                p.dynamic[node] = f"{mn} {mode} ${d.value:04X}"
                if (st.m8, st.x8, st.d, st.e) != DISPATCH_WIDTHS:
                    # Every dispatch site the vectors reach without seeding runs
                    # 16-bit and binary; one reached otherwise is a context the
                    # seeds themselves introduced, so it is reported, not seeded.
                    p.problems.append(f"${addr:06X} dispatch in widths m8={st.m8} x8={st.x8} d={st.d}")
                    return
                long_ptr = mode == O.ABSIL
                seeds = self.seeds_for(None if long_ptr else bank >> 16, st)
                for t in seeds:
                    if is_call:
                        self.call(p, node, addr, st, t, open_bank=True)
                    else:
                        self.tail_call(p, node, st, t)
                return
            if mode == O.ABSIX:
                p.dynamic[node] = f"table ${bank | d.value:06X}"
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
                s.m8 = s.x8 = s.d = None
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
            push(("b", 0), ("b", 0))
        elif mn == "PLA":
            pull(1 if s.m8 else 2)
        elif mn in ("PLX", "PLY"):
            pull(1 if s.x8 else 2)
        elif mn == "PLD":
            pull(2)
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


def analyse(listing: Listing, extra: Path | None = None) -> Program:
    rom = listing.rom
    prog = Program(listing)
    prog.seeds = address_taken(listing) | (extra_targets(extra) if extra else set())
    reset = vector(rom, rom_index(0xFFFC))
    nmi = vector(rom, rom_index(0xFFEA))
    irq = vector(rom, rom_index(0xFFEE))
    entries = [
        (reset, Ctx(True, True, False, 0x00, True)),
        # Interrupts arrive in whatever widths the interrupted code had; the
        # handlers establish their own with REP/SEP before any width matters.
        # SMW never sets the decimal flag (no SED, no SEP with bit 3).
        (nmi, Ctx(None, None, False, None, False)),
        (irq, Ctx(None, None, False, None, False)),
    ]
    prog.run(entries)
    return prog
