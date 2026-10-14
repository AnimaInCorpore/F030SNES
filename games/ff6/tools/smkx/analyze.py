"""Static discovery of the program from the ROM vectors.

Procedures are analysed per entry context: the entry address plus the M, X,
decimal, data-bank and direct-page state the caller established. Within a
procedure every instruction carries one abstract machine state:

    m8, x8   register widths (True = 8-bit), None when unknown
    d        decimal flag
    dbr      data bank register value, None when unknown
    e        emulation mode (reset code only)
    dp       direct page register D: a value, IN (the caller's) or None
    stack    what this procedure pushed so far, innermost last; each entry
             is ("b", value-or-None) for a byte, ("p", (m8, x8, d)) for
             the P register pushed by PHP, or ("dh"/"dl", dp) for the two
             bytes PHD pushes while D is not a known value

Calls are summarised: a JSR/JSL continues with the callee's exit widths,
decimal flag and DBR. Pulling below the procedure's own pushes is recorded as
a stack escape. Every decoded instruction that starts where the listing has
an instruction must have the listing's length, which checks the inferred M/X
at each immediate.
"""

from __future__ import annotations

import os
import re
import struct
from dataclasses import dataclass, field

from . import opcodes as O
from pathlib import Path

from .listing import Listing, Data, Label, Insn

UNKNOWN = None
IN = "IN"          # the data bank the caller had on entry
CTX_CAP = 4        # contexts per entry before the DBR is left to the runtime
# Static data-bank tracking beyond the shared translator (both cost code size:
# every routine is translated once per bank it is entered with; see
# analysis/translator-speedup.md):
#   PHA_CONST     `LDA #imm : PHA : PLB` gives the bank (CT's way to set DBR)
#   DISPATCH_BANK a pointer-dispatch site that knows its bank hands it to the
#                 handlers (one entry per bank class, see dbr_class)
PHA_CONST = os.environ.get("SMKX_PHA_CONST", "0") == "1"
DISPATCH_BANK = os.environ.get("SMKX_DISPATCH_BANK", "0") == "1"


ROM_MASK = 0x3FFFFF   # HiROM, 4 MiB (Chrono Trigger)


def rom_index(addr: int) -> int | None:
    """ROM file offset of a 24-bit CPU address (HiROM), or None for non-ROM
    space: banks $40-$7D and $C0-$FF map 64 KiB each, the system banks
    ($00-$3F, $80-$BF) their upper halves."""
    bank, pc = addr >> 16, addr & 0xFFFF
    if bank in (0x7E, 0x7F):
        return None
    if (bank & 0x7F) < 0x40 and pc < 0x8000:
        return None
    return (((bank & 0x3F) << 16) | pc) & ROM_MASK


# Calls whose operand the game patches before they run (self-modifying code
# in a WRAM program): site -> the targets it stores. The translation reads
# the operand at run time and calls the translation of the one there.
PATCHED_CALLS: dict[int, tuple[int, ...]] = {
    # F030CT, $7E3000 program, a shared helper's callback: `LDA #$45B0 : STA $35FD`
    # ($7E3CDB) or `LDA #$9B46 : STA $35FD` ($7E86E9) before JSR $0000.
    # 0x7E35FC: (0x7E45B0, 0x7E9B46),
}

# Programs the game decompresses into WRAM and runs there: (WRAM address, the
# compressed block's CPU address). Translated at their WRAM addresses from
# the bytes tools/smkx/ff6_lz.py rebuilds; the emitter guards every entry from
# outside with a check of WRAM. FF6 has one: the cutscene program (title,
# opening credits, the floating continent, the World of Ruin scene), stored
# at $C2686C and decompressed to $7E5000 by DecompCutsceneProg ($C26828).
# (F030CT's is the intro at $7E3000, its block found through a pointer.)
WRAM_OVERLAYS: list[tuple[int, int]] = [
    (0x7E5000, 0xC2686C),
]


def wram_code(addr: int) -> bool:
    """An address in WRAM, where the game generates code (CT's Mode 7 copy
    routine at $7E3000): run by the runtime's interpreter (rt_ramcode)."""
    bank, pc = addr >> 16, addr & 0xFFFF
    return bank in (0x7E, 0x7F) or ((bank & 0x7F) < 0x40 and pc < 0x2000)


def wram_trampoline(addr: int) -> bool:
    """Code the engines write into low WRAM: FF6's NMI and IRQ vectors jump
    to $001500 / $001504 (CT's: $000500 / $000504), where each engine stores
    a JML to its handler."""
    bank, pc = addr >> 16, addr & 0xFFFF
    return (bank == 0x7E or (bank & 0x7F) < 0x40) and pc in (0x1500, 0x1504)


@dataclass(frozen=True)
class Ctx:
    m8: bool | None
    x8: bool | None
    d: bool | None
    dbr: int | None
    e: bool
    dp: int | None

    def short(self) -> str:
        f = lambda v: "?" if v is None else ("1" if v else "0")
        dbr = "??" if self.dbr is None else ("IN" if self.dbr == IN else f"{self.dbr:02X}")
        dp = "" if self.dp == 0 else "D????" if self.dp is None else \
            ("DIN" if self.dp == IN else f"D{self.dp:04X}")
        return f"m{f(self.m8)}x{f(self.x8)}d{f(self.d)}b{dbr}" + ("e" if self.e else "") + dp


@dataclass
class State:
    m8: bool | None
    x8: bool | None
    d: bool | None
    dbr: int | None
    e: bool
    stack: tuple
    esc: int = 0        # bytes pulled from below this procedure's own pushes
    dp: object = 0      # D: an int, IN (the entry's) or None
    pesc: int | None = None   # unknown widths are the P byte pulled this far below the entry
    outer: tuple | None = None  # the stack a TXS to a fresh one left, until a TXS restores it
    switched: bool = False      # S was set to another stack (TCS/TXS): pulls below are not escapes

    def key(self):
        return (self.m8, self.x8, self.d, self.dbr, self.e, self.stack, self.esc, self.dp, self.pesc,
                self.outer, self.switched)

    def ctx(self) -> "Ctx":
        return Ctx(self.m8, self.x8, self.d, self.dbr, self.e, self.dp)


def exit_state(ex: "Ctx", site_dbr, site_dp) -> "State":
    """A forwarded exit, with the callee's IN bank and D resolved at the site."""
    return State(ex.m8, ex.x8, ex.d, site_dbr if ex.dbr == IN else ex.dbr, ex.e, (),
                 dp=site_dp if ex.dp == IN else ex.dp)


# Data banks a pointer-dispatch site may hand on to its handlers as a static
# bank. The runtime dispatch key carries the class in bits 4-7 of its context
# byte (emit.dispatch_ctx), so one handler exists per (target, class).
DBR_CLASSES = (0x00, 0x7E, 0x7F, 0x80)


def dbr_class(dbr) -> int:
    """0 for an unknown bank, else 1 + the index in DBR_CLASSES."""
    return 1 + DBR_CLASSES.index(dbr) if isinstance(dbr, int) and dbr in DBR_CLASSES else 0


def open_exit(ex: "Ctx") -> "Ctx":
    """A handler's exit as a pointer-dispatch site continues from it: in the
    handler's widths and, when it leaves a static bank, that bank; D and any
    other bank are left to the runtime registers."""
    return Ctx(ex.m8, ex.x8, ex.d, ex.dbr if DISPATCH_BANK and dbr_class(ex.dbr) else None, ex.e, None)


def join_state(a: "State", b: "State") -> "State":
    stack = join_stack(a.stack, b.stack) or a.stack
    return State(join_val(a.m8, b.m8), join_val(a.x8, b.x8), join_val(a.d, b.d),
                 join_val(a.dbr, b.dbr), a.e and b.e, stack, max(a.esc, b.esc),
                 join_val(a.dp, b.dp), outer=a.outer if a.outer == b.outer else None,
                 switched=a.switched or b.switched)


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
    escape_exits: set = field(default_factory=set)   # (exit Ctx, bytes pulled below entry, pesc)

    @property
    def name(self) -> str:
        return f"P{self.entry:06X}_{self.ctx.short()}"

    def all_states(self, addr: int) -> list:
        return self.states.get(addr, [])


STATE_CAP = 8
DP_IO = 0x1F01          # D values from here on reach past low WRAM
# Calls that yield a coroutine: CT's menu tasks call these to sleep; the
# scheduler later resumes the task on its own stack right after the call
# (PLD PLP PLY PLX RTS restore D, P, Y and X), from any context. FF6's menu
# tasks (ExecTasks, $C311B0) run to completion in each pass: none.
YIELD_ROUTINES: set[int] = set()     # F030CT: {0xC2821E, 0xC2822B}, MenuTask_Sleep(Frames)
BANK_SEEDING = False
PLP_SPLIT = False       # branch on every unknown pulled P (exploded CT's node count)
# Instructions whose meaning does not depend on the M and X widths.
WIDTH_FREE = {"BRA", "BRL", "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ", "JMP", "JML",
              "RTS", "RTL", "RTI", "PLD", "PLB", "PHB", "PHD", "PHK", "PHP", "PLP", "REP", "SEP",
              "CLC", "SEC", "CLI", "SEI", "CLD", "SED", "CLV", "NOP", "XCE", "TCD", "TDC", "TCS",
              "TSC", "XBA", "WAI", "STP", "BRK", "COP", "WDM", "PEA", "PEI", "PER"}     # seed unlisted dispatch sites with the bank's address-taken labels
INTERRUPT_DISPATCH = 8   # dispatch context code of the WRAM interrupt trampolines
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
        self.interrupt_seeds: set[int] = set()
        self.site_targets: dict[int, set[int]] = {}
        self.ret_at: dict = {}     # (proc key, node) -> return address of a PER-pushed call
        self.rom_tables: dict[int, int] = {}    # jump tables read from incbin bytes
        self.overlays: list[tuple[int, bytes]] = []
        self.bad_table_targets: set[int] = set()   # jump table words that name no listed code

    # -- decoding ----------------------------------------------------------
    def overlay_at(self, addr: int):
        for start, data in self.overlays:
            if start <= addr < start + len(data):
                return start, data
        return None

    def decode(self, addr: int, m8, x8) -> Decoded | str:
        idx = rom_index(addr)
        if idx is None:
            ov = self.overlay_at(addr)
            if ov is None:
                return f"code outside ROM at ${addr:06X}"
            mem, idx = ov[1], addr - ov[0]
        else:
            mem = self.rom
        op = mem[idx]
        mnem, mode = O.OPCODES[op]
        n = O.operand_length(mode, m8, x8)
        if n is None:
            return f"${addr:06X} {mnem} immediate with unknown {'M' if mode == O.IMM_M else 'X'}"
        raw = mem[idx:idx + 1 + n]
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
    def table_targets(self, table_addr: int, bank: int) -> list[int] | None:
        """Words of the listing's dw block at table_addr, as bank:word targets."""
        idx = rom_index(table_addr)
        if idx is None:
            ov = self.overlay_at(table_addr)
            if ov is None:
                return None
            # A table inside a WRAM program: words while they point into it.
            start, data = ov
            out = []
            o = table_addr - start
            while o + 2 <= len(data) and len(out) < 64:
                t = (bank << 16) | data[o] | data[o + 1] << 8
                if self.overlay_at(t) is None:
                    break
                out.append(t)
                o += 2
            return out or None
        if idx in self.table_cache:
            words = self.table_cache[idx]
        elif TABLE_LENGTHS.get((bank << 16) | (idx & 0xFFFF)):
            words = self.rom_table(idx, bank)
            self.table_cache[idx] = words
        else:
            words = None
            items = self.listing.items
            for i, item in enumerate(items):
                if isinstance(item, Data) and item.offset == idx and item.unit == 2:
                    words = []
                    j = i
                    while j < len(items) and isinstance(items[j], Data) and items[j].unit == 2:
                        n = items[j].size // 2
                        words.extend(struct.unpack_from(f"<{n}H", self.rom, items[j].offset))
                        j += 1
                        if j < len(items) and isinstance(items[j], Label):
                            break
                    break
            if words is None:
                words = self.rom_table(idx, bank)
            self.table_cache[idx] = words
        if words is None:
            return None
        targets = [(bank << 16) | w for w in words]
        # Entries that name no listed code (FF6 tables start with `.addr 0`
        # for "none", which the game tests for before it dispatches) would
        # be decoded as code from wherever they point.
        bad = [t for t in targets if not self.aligned(t)]
        if bad:
            self.bad_table_targets.update(bad)
            targets = [t for t in targets if self.aligned(t)]
        return targets

    def rom_table(self, idx: int, bank: int) -> list[int] | None:
        """A jump table the listing keeps as incbin bytes: words from the ROM
        up to the next label or equate, while each names listed code in the
        bank (at most 128). A table whose handlers the listing did not decode
        stops early that way, and a dw table at a label inside it:
        tools/smkx/tables.txt gives the length of both kinds."""
        import bisect
        n = TABLE_LENGTHS.get((bank << 16) | (idx & 0xFFFF))
        if n:
            words = list(struct.unpack_from(f"<{n}H", self.rom, idx))
            self.rom_tables[(bank << 16) | (idx & 0xFFFF)] = n
            return words
        if not hasattr(self, "_bounds"):
            offs = set(self.listing.labels.values())
            offs |= {rom_index(a) for a in getattr(self.listing, "symbols", {}).values()} - {None}
            self._bounds = sorted(offs)
        k = bisect.bisect_right(self._bounds, idx)
        end = self._bounds[k] if k < len(self._bounds) else len(self.rom)
        words = []
        o = idx
        while o + 2 <= end and len(words) < 128:
            w = struct.unpack_from("<H", self.rom, o)[0]
            if rom_index((bank << 16) | w) not in self.listing.insn_at:
                break
            words.append(w)
            o += 2
        if words:
            self.rom_tables[(bank << 16) | (idx & 0xFFFF)] = len(words)
        return words or None

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
                shape = (st.m8, st.x8, st.d, st.e, len(st.stack), st.esc)
                # (D and the data bank are joined, like pushed values)
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
        if open_bank and not DISPATCH_BANK:
            dbr = None
        elif open_bank:
            # A dispatch site that knows its bank runs its handlers in it
            # (a handler entered from another site with another bank has an
            # entry of its own: the dispatch key includes the bank class).
            dbr = st.dbr if dbr_class(st.dbr) else None
        else:
            dbr = None if st.dbr == IN else st.dbr
        # D is passed on only when it points outside low WRAM (the PPU or
        # DMA registers): anywhere else the runtime's D register (a2)
        # reaches direct page as fast as a fixed address would.
        dp = st.dp if isinstance(st.dp, int) and st.dp >= DP_IO and not open_bank else None
        ctx = Ctx(st.m8, st.x8, st.d, dbr, st.e, dp)
        known = self.contexts.setdefault(target, set())
        if ctx not in known and len(known) >= CTX_CAP:
            # Too many contexts: first leave the bank, then D, to the runtime.
            ctx = Ctx(st.m8, st.x8, st.d, None, st.e, dp)
            if ctx not in known and len([k for k in known if k.dbr is None]) >= CTX_CAP:
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
        for ex, esc, pesc in list(callee.escape_exits):
            self.escape_into(callee, (p.entry, p.ctx), node, site, ex, esc, pesc)
        if open_bank:
            # Escapes from a dispatched handler still leave through this site.
            self.dyn_callers.setdefault((target, ctx), set()).add(((p.entry, p.ctx), node, site))
            # A pointer dispatch: continue as if the handler returns in the
            # widths, decimal mode and bank it was called with. The emitted
            # site checks the exit the handler actually took.
            # Handlers may return with another data bank or D (the caller
            # usually restores them with PLB/PLD): the continuation takes
            # both from the runtime registers.
            p.assumed[node] = Ctx(st.m8, st.x8, st.d, None, st.e, None)
            if (node, "assumed") not in p.conts:
                assumed = Ctx(st.m8, st.x8, st.d, None, st.e, None)
                self.resume_after_call(p, node, site, st, assumed)
                p.conts[(node, "assumed")] = p.conts[(node, assumed)]
            # and in every context a handler is known to return in
            for ex in list(callee.exits):
                self.resume_after_call(p, node, site, st, open_exit(ex))
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
            if li is None or not li.traced:
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
        entry = ((p.entry, p.ctx), st.dbr, st.dp)
        if entry not in link:
            link.add(entry)
            for ex in list(callee.exits):
                self.finish_exit(p, ("tail", target), exit_state(ex, st.dbr, st.dp))

    def resume_after_call(self, p: Proc, node, site: int, st: State, ex: Ctx) -> None:
        ins = p.insns[site]
        ret = self.ret_at.get((p.entry, p.ctx, node))
        stack = st.stack
        if ret is None:
            ret = site + ins.length
        else:
            stack = stack[:-2]      # the PER-pushed return address is the call's frame
        after = State(ex.m8, ex.x8, ex.d, st.dbr if ex.dbr == IN else ex.dbr, ex.e, stack, st.esc,
                      st.dp if ex.dp == IN else ex.dp, outer=st.outer, switched=st.switched)
        dest = self.flow(p, node, "return", ret, after)
        p.conts[(node, ex)] = dest

    def escape_into(self, p: Proc, ckey, cnode, site: int, ex: "Ctx", esc: int, pesc=None) -> None:
        """An escaped return of p leaves the caller at (ckey, site) when it
        pulled exactly the caller's return frame plus the caller's pushes.
        Widths p left unknown because it pulled P from below its entry
        (pesc bytes down) are the P byte the caller pushed there."""
        caller = self.procs[ckey]
        frame = 3 if caller.insns[site].mnem == "JSL" else 2
        cst = next(o for o in caller.states[site] if o.key() == cnode[1])
        if esc != frame + len(cst.stack):
            p.problems.append(f"returns {esc} bytes below a {frame}-byte frame at ${site:06X} "
                              f"(caller had pushed {len(cst.stack)})")
            return
        p.escaped_to.add(ckey)
        if pesc is not None and pesc >= frame:
            v = cst.stack[len(cst.stack) - 1 - (pesc - frame)]
            if v[0] == "p" and v[1] is not None:
                m8, x8, dec = v[1]
                ex = Ctx(ex.m8 if ex.m8 is not None else m8, ex.x8 if ex.x8 is not None else x8,
                         dec if ex.m8 is None and ex.x8 is None else ex.d, ex.dbr, ex.e, ex.dp)
        self.finish_exit(caller, ("escape", p.entry), exit_state(ex, cst.dbr, cst.dp))

    def finish_exit(self, p: Proc, node, st: State) -> None:
        if st.switched:
            # A return on another stack (a resumed menu task): it lands in
            # that task's yield continuation, which takes any exit.
            return
        ex = st.ctx()
        if st.esc:
            # The procedure pulled its return address and, beyond it, what
            # the caller had pushed before calling: this return leaves the
            # caller as well.
            key = (p.entry, p.ctx)
            pesc = st.pesc if st.m8 is None or st.x8 is None else None
            if (ex, st.esc, pesc) in p.escape_exits:
                return
            p.escape_exits.add((ex, st.esc, pesc))
            sites = list(self.callers.get(key, ())) + list(self.dyn_callers.get(key, ()))
            for (ckey, cnode, site) in sites:
                self.escape_into(p, ckey, cnode, site, ex, st.esc, pesc)
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
            for (ckey, cnode, site) in list(self.dyn_callers.get((p.entry, p.ctx), ())):
                caller = self.procs[ckey]
                cst = next(o for o in caller.states[site] if o.key() == cnode[1])
                self.resume_after_call(caller, cnode, site, cst, open_exit(ex))

    def const_a(self, p: Proc, addr: int):
        """The 16-bit value of C before the instruction at addr, when the
        straight-line code before it loads a constant: `LDA #imm` (16-bit),
        or `LDA #hi : XBA : LDA #lo` (8-bit), or `TDC` of a known D is not
        followed. None otherwise."""
        def before(a):
            for back in (1, 2, 3, 4):
                prev = p.insns.get(a - back)
                if prev is not None and prev.addr + prev.length == a:
                    return prev
            return None
        prev = before(addr)
        if prev is None or prev.mnem != "LDA" or prev.mode != O.IMM_M:
            return None
        if prev.length == 3:
            return prev.value
        xba = before(prev.addr)
        hi = before(xba.addr) if xba is not None and xba.mnem == "XBA" else None
        if hi is None or hi.mnem != "LDA" or hi.mode != O.IMM_M or hi.length != 2:
            return None
        return hi.value << 8 | prev.value

    def const_a_imm(self, p: Proc, addr: int, m8):
        """The value A holds before the instruction at addr when the one
        right before it is `LDA #imm` in the current width (an 8-bit load
        leaves the high byte alone, so only the 8-bit value is returned in
        8-bit mode). Only straight-line code counts: a label between the two
        instructions would let another path arrive with another A."""
        if m8 is None:
            return None
        for back in (2, 3):
            prev = p.insns.get(addr - back)
            if prev is None or prev.addr + prev.length != addr:
                continue
            if prev.mnem != "LDA" or prev.mode != O.IMM_M or prev.length != back:
                return None
            if (back == 2) != bool(m8):
                return None
            if self.is_branch_target(addr):
                return None
            return prev.value
        return None

    def is_branch_target(self, addr: int) -> bool:
        """True when the listing puts a label (named or anonymous) on addr:
        another path may arrive there with other register contents."""
        idx = rom_index(addr)
        li = self.listing.insn_at.get(idx) if idx is not None else None
        return bool(idx is not None and (self.listing.label_of(idx) or (li is not None and li.anon)))

    # -- one instruction ----------------------------------------------------------
    def step(self, p: Proc, addr: int, st: State) -> None:
        node = (addr, st.key())
        if st.pesc is not None and (st.m8 is None or st.x8 is None) and not st.e:
            # Widths from a P byte this procedure pulled from below its
            # entry (it leaves through its caller's frame). At the first
            # instruction that needs them, branch at runtime on that byte
            # (the runtime keeps the last P a PLP pulled) to this address
            # in each pair of widths.
            peek = self.decode(addr, True, True)
            if isinstance(peek, str) or peek.mnem not in WIDTH_FREE:
                for m8 in ((st.m8,) if st.m8 is not None else (False, True)):
                    for x8 in ((st.x8,) if st.x8 is not None else (False, True)):
                        t = State(**vars(st))
                        t.m8, t.x8, t.pesc = m8, x8, None
                        self.flow(p, node, f"plpw{int(m8)}{int(x8)}", addr, t)
                return
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
        if li is not None and li.traced and li.size != d.length:
            p.problems.append(f"${addr:06X} {d.mnem}: decoded {d.length} bytes, listing {li.size} "
                              f"(m8={st.m8} x8={st.x8})")
        mn, mode = d.mnem, d.mode
        bank = addr & 0xFF0000
        nxt = addr + d.length
        s = State(st.m8, st.x8, st.d, st.dbr, st.e, st.stack, st.esc, st.dp, st.pesc, st.outer, st.switched)

        def push(*entries):
            s.stack = s.stack + tuple(entries)

        def pull(n: int):
            vals = []
            below = 0
            for _ in range(n):
                if s.stack:
                    vals.append(s.stack[-1])
                    s.stack = s.stack[:-1]
                elif s.switched:
                    # another stack (the menu scheduler resuming a task):
                    # values it holds, not this procedure's frame
                    vals.append(("sw", None))
                else:
                    vals.append(("esc", s.esc + below))
                    below += 1
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
        if mn in ("JML", "JSL", "JMP") and mode == O.LONG and wram_trampoline(d.value):
            # JML $000500: the JML an engine stored there. Its targets, the
            # interrupt handlers, run as tail calls in this procedure's context.
            if mn != "JML" or st.stack or st.esc:
                p.traps[node] = f"{mn} into the WRAM trampoline with a frame"
                return
            p.dynamic[node] = f"ramjml ${d.value & 0xFFFF:04X}"
            for t in sorted(self.interrupt_seeds):
                self.tail_call(p, node, st, t)
            return
        if mn == "JSR" and mode == O.ABS and addr in PATCHED_CALLS:
            p.dynamic[node] = f"patched ${addr + 1:06X}"
            for t in PATCHED_CALLS[addr]:
                self.call(p, node, addr, st, t)
            return
        if mn in ("JMP", "JML", "JSR", "JSL") and mode in (O.ABS, O.LONG) and \
                wram_code((bank | d.value) if mode == O.ABS else d.value) and \
                self.overlay_at((bank | d.value) if mode == O.ABS else d.value) is None:
            # Code the game built in WRAM: interpreted by the runtime, which
            # leaves widths and the bank alone but may change D.
            target = (bank | d.value) if mode == O.ABS else d.value
            p.dynamic[node] = f"ramcode ${target:06X}"
            after = State(**vars(s))
            after.dp = None
            if mn in ("JSR", "JSL"):
                self.flow(p, node, "return", nxt, after)
            else:
                if s.stack:
                    p.problems.append(f"${addr:06X} {mn} into WRAM code with {len(s.stack)} bytes pushed")
                self.finish_exit(p, node, after)
            return
        if mn in ("JMP", "JML", "JSR", "JSL"):
            is_call = mn in ("JSR", "JSL")
            if mode == O.ABS:
                targets = [bank | d.value]
            elif mode == O.LONG:
                targets = [d.value]
            elif mode == O.ABSIX:
                # a per-site list (dispatch_targets.txt) wins over the table: for
                # tables with entries the site never dispatches to
                targets = sorted(self.site_targets[addr]) if addr in self.site_targets \
                    else self.table_targets(bank | d.value, bank >> 16)
            else:
                targets = None
            if targets is None:
                p.dynamic[node] = f"{mn} {mode} ${d.value:04X}"
                if st.m8 is None or st.x8 is None or st.d is not False or st.e:
                    # The dispatch table is keyed by the site's widths; seeds
                    # are analysed in them, so they must be known.
                    p.problems.append(f"${addr:06X} dispatch in widths m8={st.m8} x8={st.x8} d={st.d}")
                    return
                long_ptr = mode == O.ABSIL
                if addr in self.site_targets:
                    seeds = sorted(self.site_targets[addr])
                elif BANK_SEEDING:
                    seeds = self.seeds_for(None if long_ptr else bank >> 16, st)
                else:
                    # Targets are listed per site (dispatch_targets.txt) as the
                    # runtime reports them; seeding every address-taken label of
                    # the bank multiplied CT's contexts.
                    seeds = []
                    p.problems.append(f"${addr:06X} dispatch site without listed targets")
                per = None
                if not is_call and len(s.stack) >= 2 and s.stack[-1][0] == "b" and s.stack[-2][0] == "b" \
                        and isinstance(s.stack[-1][1], int) and isinstance(s.stack[-2][1], int):
                    # `PER ret-1 : ... : JMP (ptr)` is a call that returns
                    # to ret (CT's menu state dispatcher at $C2828E).
                    per = bank | ((s.stack[-2][1] << 8 | s.stack[-1][1]) + 1) & 0xFFFF
                    self.ret_at[(p.entry, p.ctx, node)] = per
                    p.dynamic[node] = f"percall {mn} {mode} ${d.value:04X}"
                    cs = State(**vars(st))
                    cs.stack = st.stack[:-2]
                for t in seeds:
                    if is_call:
                        self.call(p, node, addr, st, t, open_bank=True)
                    elif per is not None:
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
                if mode in (O.ABS, O.LONG) and self.overlay_at(targets[0]) is not None and \
                        self.overlay_at(addr) is None and (node, "ramcode") not in p.conts:
                    # A call into a WRAM program from outside: the game may
                    # have built other code there since (CT's menu and save
                    # code build MVN stubs at $7E9690). When WRAM no longer
                    # holds the translated bytes, rt_ramcode runs it and the
                    # caller continues here, bank and D from the runtime.
                    after = State(**vars(s))
                    after.dbr, after.dp = None, None
                    p.conts[(node, "ramcode")] = self.flow(p, node, "return", nxt, after)
                if len(targets) == 1 and targets[0] in YIELD_ROUTINES and (node, "yield") not in p.conts:
                    after = State(**vars(s))
                    after.dbr = None
                    p.conts[(node, "yield")] = self.flow(p, node, "return", nxt, after)
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
            s.pesc = None
            if v[0] == "p" and v[1] is not None:
                s.m8, s.x8, s.d = v[1]
            else:
                # CT sets decimal mode only between a SED and a CLD in one
                # routine, so a P pushed elsewhere never has D set.
                s.m8 = s.x8 = None
                s.d = False
                if v[0] == "esc":
                    s.pesc = v[1]       # resolved at the escaped caller (escape_into)
                elif v[0] == "sw":
                    s.pesc = -1         # only from the pulled byte at runtime (width_split)
            if PLP_SPLIT and (s.m8 is None or s.x8 is None) and not s.e:
                # A P byte pushed elsewhere: the emitted PLP branches on the
                # pulled M and X bits to one successor per width pair.
                for m8 in ((s.m8,) if s.m8 is not None else (False, True)):
                    for x8 in ((s.x8,) if s.x8 is not None else (False, True)):
                        t = State(**vars(s))
                        t.m8, t.x8 = m8, x8
                        self.flow(p, node, f"plp{int(m8)}{int(x8)}", nxt, t)
                return
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
            # `LDA #imm : PHA : PLB` is how CT sets the data bank: with the
            # constant known, the PLB gives the bank statically and the code
            # after it needs no runtime bank (a3/a4/d3) for its operands.
            c = self.const_a_imm(p, addr, s.m8) if PHA_CONST else None
            if c is None:
                push(*([("b", None)] * (1 if s.m8 else 2)))
            elif s.m8:
                push(("b", c & 0xFF))
            else:
                push(("b", c >> 8 & 0xFF), ("b", c & 0xFF))
        elif mn in ("PHX", "PHY"):
            push(*([("b", None)] * (1 if s.x8 else 2)))
        elif mn == "PHD":
            if isinstance(s.dp, int):
                push(("b", s.dp >> 8), ("b", s.dp & 0xFF))
            else:
                push(("dh", s.dp), ("dl", s.dp))
        elif mn == "PLA":
            pull(1 if s.m8 else 2)
        elif mn in ("PLX", "PLY"):
            pull(1 if s.x8 else 2)
        elif mn == "PLD":
            lo, hi = pull(2)
            if lo[0] == "b" and hi[0] == "b" and isinstance(lo[1], int) and isinstance(hi[1], int):
                s.dp = hi[1] << 8 | lo[1]
            elif lo[0] == "dl" and hi[0] == "dh" and lo[1] == hi[1]:
                s.dp = lo[1]
            else:
                s.dp = None
        elif mn == "TCD":
            s.dp = self.const_a(p, addr)
        elif mn in ("TCS", "TXS"):
            # `LDX #imm : TXS` switches to a fresh stack (the menu NMI's
            # $1DFD), `LDX abs : TXS` (or LDA/TCS) switches back to the
            # S it saved: the pushes before the switch are known again.
            prev = None
            for back in (2, 3):
                q = p.insns.get(addr - back)
                if q is not None and q.addr + q.length == addr:
                    prev = q
            src = "LDX" if mn == "TXS" else "LDA"
            if prev is not None and prev.mnem == src and prev.mode in (O.IMM_X, O.IMM_M):
                s.outer, s.stack, s.switched = s.stack, (), False
            elif prev is not None and prev.mnem == src and prev.mode == O.ABS and s.outer is not None \
                    and s.stack == ():
                s.stack, s.outer, s.switched = s.outer, None, False
            else:
                s.stack, s.outer, s.switched = (), None, True
        if mn in ("PHA", "PLA") and s.m8 is None or mn in ("PHX", "PHY", "PLX", "PLY") and s.x8 is None:
            p.problems.append(f"${addr:06X} {mn} with unknown width")
        self.flow(p, node, "fall", nxt, s)


def vector(rom: bytes, offset: int) -> int:
    return struct.unpack_from("<H", rom, offset)[0]


IDENT = re.compile(r"(?<![A-Za-z0-9_$%!])[A-Za-z_][A-Za-z0-9_]*")


def address_taken(listing: Listing) -> set[int]:
    """Code addresses the listing uses as data: dw/dl elements and immediates
    that name a label or equate placed on an instruction."""
    if hasattr(listing, "taken"):        # ca65 listings (parse_ff6) work it out from the linker's references
        return set(listing.taken)
    syms = getattr(listing, "symbols", {})
    code = set(listing.insn_at)
    out: set[int] = set()

    def scan(text: str):
        for m in IDENT.finditer(text):
            a = syms.get(m.group())
            if a is not None and rom_index(a) in code:
                out.add(a)
    for item in listing.items:
        if isinstance(item, Data) and item.words:
            for w in item.words:
                scan(w)
        elif isinstance(item, Insn) and "#" in item.operand:
            scan(item.operand.split("#", 1)[1])
    return out


def table_lengths(path: Path) -> dict[int, int]:
    """Jump table lengths the listing cannot tell: "$BBAAAA count" per line."""
    out = {}
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.split("#")[0].strip()
            if line:
                a, n = line.split()
                out[int(a.lstrip("$"), 16)] = int(n)
    return out


TABLE_LENGTHS = table_lengths(Path(__file__).resolve().parent / "tables.txt")


def extra_targets(path: Path) -> set[int]:
    """Runtime-discovered dispatch targets, one $BBAAAA per line (lines of
    the form "$SITE: targets" are per-site lists, see site_targets)."""
    out: set[int] = set()
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line and ":" not in line:
                out.add(int(line.lstrip("$"), 16))
    return out


def pointer_idiom_sites(listing: Listing) -> dict[int, set[int]]:
    """Dispatch sites of the form `LDA table,X : STA ptr : ... : JMP (ptr)`
    (or JML [ptr]): the targets are the table's words, read from the ROM
    while they name listed code in the site's bank (at most 64)."""
    rom = listing.rom
    insns = sorted(listing.insn_at.items())
    out: dict[int, set[int]] = {}
    # Runs of word-sized data items (`.addr` lists), which a label ends: the
    # tables the listing knows the length of, by their first byte.
    runs: dict[int, list[int]] = {}
    cur: list | None = None
    for item in listing.items:
        if isinstance(item, Data) and item.unit == 2 and item.size % 2 == 0:
            if cur is None:
                cur = runs.setdefault(item.offset, [])
            cur.extend(struct.unpack_from(f"<{item.size // 2}H", rom, item.offset))
        else:
            cur = None
    for k, (off, ins) in enumerate(insns):
        if ins.mnem not in ("JMP", "JML") or ins.mode not in (O.ABSI, O.ABSIL):
            continue
        ptr = ins.raw[1] | ins.raw[2] << 8
        bank = 0xC00000 | (off & 0x3F0000)
        table = None
        for back in range(1, 9):
            if k - back < 0:
                break
            _, st = insns[k - back]
            if st.mnem == "STA" and st.mode in (O.DP, O.ABS) and \
                    (st.raw[1] if st.mode == O.DP else st.raw[1] | st.raw[2] << 8) == ptr:
                for b2 in range(1, 5):
                    if k - back - b2 < 0:
                        break
                    _, ld = insns[k - back - b2]
                    if ld.mnem == "LDA" and ld.mode in (O.ABSX, O.LONGX):
                        v = int.from_bytes(ld.raw[1:], "little")
                        table = v if ld.mode == O.LONGX else bank | v
                        break
                break
        if table is None or rom_index(table) is None:
            continue
        targets = set()
        t = rom_index(table)
        if t in runs:               # a table the listing keeps as dw items: all of it (FF6's event commands: 203)
            targets = {bank | w for w in runs[t] if rom_index(bank | w) in listing.insn_at}
            if targets:
                out[0xC00000 | off] = targets
            continue
        while len(targets) < 64 and t + 2 <= len(rom):
            w = rom[t] | rom[t + 1] << 8
            if rom_index(bank | w) not in listing.insn_at:
                break
            targets.add(bank | w)
            t += 2
        if targets:
            out[0xC00000 | off] = targets
    return out


def site_targets(path: Path) -> dict[int, set[int]]:
    """Per-site dispatch targets: "$SITE: $T1 $T2 ..." (several lines may
    name the same site)."""
    out: dict[int, set[int]] = {}
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if ":" in line:
                site, rest = line.split(":", 1)
                out.setdefault(int(site.strip().lstrip("$"), 16), set()).update(
                    int(t.lstrip("$"), 16) for t in rest.split())
    return out


def analyse(listing: Listing, extra: Path | None = None, interrupts: Path | None = None) -> Program:
    rom = listing.rom
    prog = Program(listing)
    prog.seeds = address_taken(listing) | (extra_targets(extra) if extra else set())
    prog.interrupt_seeds = extra_targets(interrupts) if interrupts else set()
    prog.site_targets = site_targets(extra) if extra else {}
    for site, ts in pointer_idiom_sites(listing).items():
        prog.site_targets.setdefault(site, set()).update(ts)
    from . import ff6_lz
    for start, src in WRAM_OVERLAYS:
        prog.overlays.append((start, ff6_lz.decompress(lambda a: rom[rom_index(a)], src)))
    reset = vector(rom, 0xFFFC)
    nmi = vector(rom, 0xFFEA)
    irq = vector(rom, 0xFFEE)
    entries = [
        (reset, Ctx(True, True, False, 0x00, True, 0)),
        # Interrupts arrive in whatever widths, bank and D the interrupted
        # code had; the handlers establish their own before any of it
        # matters. The 65816 clears the decimal flag on an interrupt.
        (nmi, Ctx(None, None, False, None, False, None)),
        (irq, Ctx(None, None, False, None, False, None)),
    ]
    prog.run(entries)
    return prog
