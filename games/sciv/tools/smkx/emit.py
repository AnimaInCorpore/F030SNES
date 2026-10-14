"""Emit vasm (Motorola syntax) 68030 source for the analysed SMK program.

Register mapping
    d0.w  C (A in d0.b, B in bits 8-15)       d1.w  X        d2.w  Y
    d3.l  DBR << 16                           d4/d5 flag save/merge
    d6/d7 scratch, runtime-call value         a0/a1 scratch, runtime-call address
    a2    D, as a host pointer into WRAM (a5 + D)
    a3    host address of DBR:$8000           a4    host address of DBR:$0000
    a5    WRAM ($7E0000)                      a6    S, as a host pointer into WRAM
    a7    68k return addresses (a shadow of the 65816 frames)

Direct page: where the analysis knows D, `dp` operands are static bank-0
addresses (WRAM, or the registers when D points at $2100/$4300); elsewhere
they go through a2, which every TCD and PLD keeps current.

Flags: 65816 C lives in the 68k X flag, N/Z in the CCR, V in the byte
rt_vf ($FF set, 0 clear): nearly every 68k instruction clears the CCR's V,
while the 65816 changes V only in ADC, SBC, BIT, CLV, PLP, REP/SEP and
RTI, so keeping it in the CCR cost a save around most instructions
(from F030SMK, 2026-09-24). A flag is computed
only where liveness says a later instruction can read it, and preserved
across an instruction only where it stays live. SNES memory keeps the ROM's
little-endian layout; 16-bit loads rotate with ror.w #8, which leaves X alone,
and 16-bit ROM loads read the prebuilt big-endian word view instead.

Every node the analysis could not translate, and every control transfer it
could not resolve, becomes a call to rt_trap with the 65816 address, so a
wrong assumption stops the program instead of running on.
"""

from __future__ import annotations

from . import analyze
from . import opcodes as O
from .analyze import IN, rom_index, INTERRUPT_DISPATCH, dbr_class
from .liveness import C, Z, N, V, ALL, defs as flag_defs

X68 = {C: 0x10, Z: 0x04, N: 0x08, V: 0x02}


def m68(mask: int) -> int:
    out = 0
    for f, bit in X68.items():
        if mask & f:
            out |= bit
    return out


def sz(eight: bool) -> str:
    return "b" if eight else "w"


def hx(v: int) -> str:
    return f"${v:X}"


class Unsupported(Exception):
    pass


class Loc:
    """Where a memory operand lives.

    kind "host": ea (byte access); rom marks read-only ROM (writes are
                 dropped). 16-bit reads load the little-endian word and
                 rotate it (the 68030 reads misaligned words; CT's 4 MiB ROM
                 leaves no room for SMK's big-endian word view).
    kind "rt":   prep computes the 24-bit SNES address into a0; the runtime
                 performs the access (I/O, DSP-1, SRAM, unmapped, dynamic).
    """

    def __init__(self, kind, ea=None, rom=False, prep=None, note=""):
        self.kind, self.ea, self.rom, self.prep, self.note = kind, ea, rom, prep or [], note


CHUNK_LINES = 12000


# Emitter optimisations that can be switched off for bisecting a fault:
# SMKX_EMIT_OFF=guard,paged,plain,plb,w2180,adc,rmw,bitfield (comma separated).
import os as _os
def _wide_index() -> frozenset:
    """Instruction addresses whose `dp,x` / `dp,y` index is a full 16-bit address
    (tools/smkx/wide_index.txt): the access is resolved at run time."""
    from pathlib import Path
    p = Path(__file__).resolve().parent / "wide_index.txt"
    out = set()
    if p.exists():
        for line in p.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                out.add(int(line.lstrip("$"), 16) & 0x7FFFFF)
    return frozenset(out)


WIDE_INDEX = _wide_index()
OFF = frozenset(x for x in _os.environ.get("SMKX_EMIT_OFF", "").split(",") if x)

# Registers whose write is only the store into the shadow array rt_io
# (falcon/smkx_rt.s: rt_io_write's .w21/.w42 tables send them to .out).
# tools/emit_tests.py checks these sets against the runtime's tables.
PLAIN21 = frozenset([0x2100, 0x2101] + list(range(0x2105, 0x210D)) + [0x211A] + list(range(0x2123, 0x2132))
                    + [0x2133])
PLAIN42 = frozenset([0x4201, 0x4202, 0x4204, 0x4205, 0x4207, 0x4208, 0x4209, 0x420A, 0x420C, 0x420D])
PLAIN43 = frozenset(range(0x4300, 0x4380))         # DMA channel registers
SHADOW_R = frozenset(list(range(0x4218, 0x4220)) + list(range(0x4300, 0x4380)))   # read from the shadow
HW_REGS = frozenset(list(range(0x2100, 0x2200)) + list(range(0x4200, 0x4380)))     # never memory in a system bank
# The operand kinds looked up in the page tables. The others (`io`, `dp-io`: a
# register picked by an index or D on the registers) are I/O nearly always.
PAGED_NOTES = frozenset([] if "paged" in OFF else ["ptr", "indexed-pointer", "dbr-io"])


class Emitter:
    def __init__(self, prog, live_out):
        self.prog = prog
        self.live = live_out
        self.lines: list[str] = []
        self.pid: dict = {}
        self.exit_ids: dict = {}
        self.table_len: dict = {}            # dispatch table label -> entries
        self.tables: list[str] = []
        self.state_of: dict = {}
        self.unsupported: list[str] = []
        self.probes: set[int] = set()
        self.marks: dict[int, str] = {}
        self.natives: dict[int, tuple[int, str]] = {}
        self.miss_stubs: list[str] = []
        self.ovl_checks: list[str] = []
        self.cur_stubs: list[str] = []
        self.cur_pid = 0
        self.nlbl = 0
        for i, key in enumerate(sorted(prog.procs, key=lambda k: (k[0], k[1].short()))):
            self.pid[key] = i

    # -- labels -------------------------------------------------------------
    def exit_id(self, ctx) -> int:
        if ctx not in self.exit_ids:
            self.exit_ids[ctx] = len(self.exit_ids)
        return self.exit_ids[ctx]

    def entry_label(self, key) -> str:
        return f"e{self.pid[key]}"

    def node_label(self, key, node) -> str:
        return f"n{self.pid[key]}_{self.node_index[key][node]}"

    # -- driver -------------------------------------------------------------
    def run(self) -> dict:
        """Emit the program as several source files of whole procedures.

        vasm sizes branches per section and merges same-named sections within
        a file, so one file per ~CHUNK_LINES keeps branch sizing local. Each
        file carries the tables of its own procedures; smk_tables.s holds the
        pointer-dispatch table and the vector exports.
        """
        prog = self.prog
        self.node_index = {}
        self.order = {}
        self.preds = {}
        self.pred_src = {}
        for key, p in prog.procs.items():
            nodes = []
            for addr in sorted(p.states):
                for st in p.states[addr]:
                    node = (addr, st.key())
                    nodes.append(node)
                    self.state_of[(key, node)] = st
            self.order[key] = nodes
            self.node_index[key] = {n: i for i, n in enumerate(nodes)}
            pc = {}
            for src, edges in p.succ.items():
                for kind, dst in edges:
                    pc[dst] = pc.get(dst, 0) + 1
            self.preds[key] = pc
            srcs = {}
            for src, edges in p.succ.items():
                for kind, dst in edges:
                    srcs.setdefault(dst, []).append(src)
            self.pred_src[key] = srcs
        files: dict = {}
        chunk: list[str] = []
        chunk_tables: list[int] = [0, 0]      # start of this chunk in self.tables / pending
        keys = sorted(prog.procs, key=lambda k: self.pid[k])
        self.chunks = []                      # (name, lines, table_start, pending_start)

        def flush():
            if not chunk:
                return
            name = f"smk_code_{len(self.chunks):03d}.s"
            self.chunks.append((name, list(chunk), chunk_tables[0], chunk_tables[1]))
            chunk.clear()
            chunk_tables[0] = len(self.tables)
            chunk_tables[1] = len(getattr(self, "pending_tables", []))

        for key in keys:
            self.lines = chunk
            self.emit_proc(key)
            if len(chunk) >= CHUNK_LINES:
                flush()
        flush()
        return files

    def vector_exports(self) -> list[str]:
        """smkx_reset/nmi/irq: the procedures analysed from the ROM vectors."""
        import struct
        rom = self.prog.rom
        out = []
        for name, off in (("smkx_reset", 0x7FFC), ("smkx_nmi", 0x7FEA), ("smkx_irq", 0x7FEE)):
            addr = struct.unpack_from("<H", rom, off)[0]
            keys = [k for k in self.prog.procs if k[0] == addr]
            if len(keys) != 1:
                raise SystemExit(f"{name}: {len(keys)} procedures at ${addr:04X}")
            out.append(f"{name}:\tjmp\t{self.entry_label(keys[0])}")
        return out

    def emit_proc(self, key) -> None:
        p = self.prog.procs[key]
        out = self.lines
        out.append(f"; {p.name}")
        nodes = self.order[key]
        entry_done = False
        stubs: list[str] = []
        self.cur_stubs, self.cur_pid, self.nlbl = stubs, self.pid[key], 0
        for i, node in enumerate(nodes):
            label = self.node_label(key, node)
            st = self.state_of[(key, node)]
            if not entry_done and node[0] == p.entry and st.stack == () and st.esc == 0:
                out.append(f"\txdef\t{self.entry_label(key)}")
                out.append(f"{self.entry_label(key)}:")
                entry_done = True
            out.append(f"{label}:\t\t\t\t; ${node[0]:06X}")
            if node[0] in self.probes:
                # Flag- and register-neutral: pea/addq on sp leave the CCR alone.
                out += [f"\tpea\t{hx(node[0])}", "\tjsr\trt_probe", "\taddq.l\t#4,sp"]
            if node[0] in self.marks:
                # the hook preserves every register and the CCR
                out += [f"\txref\trt_mark_{self.marks[node[0]]}", f"\tjsr\trt_mark_{self.marks[node[0]]}"]
            if node[0] in self.natives:
                out += self.native_hook(key, p, node, st, stubs)
            nxt = nodes[i + 1] if i + 1 < len(nodes) else None
            try:
                body = self.emit_node(key, p, node, st, nxt)
            except Unsupported as e:
                self.unsupported.append(f"{p.name} ${node[0]:06X}: {e}")
                body = [f"\tpea\t{hx(node[0])}", "\tjmp\trt_unsupported"]
            out.extend(body)
        out.extend(stubs)
        if not entry_done:
            out.append(f"\txdef\t{self.entry_label(key)}")
            out.append(f"{self.entry_label(key)}:")
            out.append(f"\tpea\t{hx(p.entry)}")
            out.append("\tjmp\trt_trap")

    def native_hook(self, key, p, node, st, stubs) -> list[str]:
        """A native routine (natives.txt) at a routine's entry: rt_native_<name>
        gets the address of a stub doing this context's RTL (the routine's
        own RTL node, whose context after PLP is the caller's) and the M, X
        and D bits PHP would push; it returns here when it declines. Contexts
        without a single matching RTL node keep the translation alone."""
        rtl, name = self.natives[node[0]]
        if st.stack != () or st.esc != 0 or st.m8 is None or st.x8 is None:
            return []
        ends = {self.state_of[(key, n)].ctx() for n in self.order[key]
                if n[0] == rtl and self.state_of[(key, n)].m8 == st.m8
                and self.state_of[(key, n)].x8 == st.x8}
        if len(ends) != 1:
            return []
        label = f"{self.node_label(key, node)}_nat"
        bits = (0x20 if st.m8 else 0) | (0x10 if st.x8 else 0) | (0x08 if st.d else 0)
        stubs += [f"{label}:\t\t\t\t; RTL ${rtl:06X} after rt_native_{name}",
                  f"\tlea\t{self.exit_id(ends.pop())}.w,a0", "\taddq.l\t#3,a6", "\trts"]
        # pea leaves the CCR alone; the routine keeps every register but a0
        return [f"\txref\trt_native_{name}", f"\tpea\t{label}", f"\tpea\t{hx(bits)}.w",
                f"\tjsr\trt_native_{name}"]

    # -- helpers --------------------------------------------------------------
    def succ(self, p, node, kind=None):
        return [d for k, d in p.succ.get(node, []) if kind is None or k == kind]

    def goto(self, key, node, nxt, out) -> None:
        if node is None:
            return
        if node != nxt:
            out.append(f"\tbra\t{self.node_label(key, node)}")

    def emit_node(self, key, p, node, st, nxt) -> list[str]:
        addr = node[0]
        if node in p.traps:
            return [f"\tpea\t{hx(addr)}", "\tjmp\trt_trap"]
        d = self.prog.decode(addr, st.m8, st.x8) if addr in p.insns else None
        if d is None or isinstance(d, str):
            return [f"\tpea\t{hx(addr)}", "\tjmp\trt_trap"]
        live = self.live.get((key, node), ALL)
        mn = d.mnem
        out: list[str] = []
        ctl = self.control(key, p, node, st, d, nxt, live)
        if ctl is not None:
            return ctl
        lines, clob, produced = self.op(key, p, node, st, d, live)
        clob &= ~V                  # the 65816 V lives in rt_vf, not in the CCR
        need = flag_defs(mn, d.mode, d.value) & live
        if need & ~produced:
            raise Unsupported(f"{mn} leaves {need & ~produced:#x} unproduced")
        through = live & ~flag_defs(mn, d.mode, d.value)
        save = through & clob
        if save:
            out.append("\tmove.w\tccr,d5")
        out.extend(lines)
        if save:
            if need or through & ~clob:
                # Flags the lines produced, and live flags they left alone
                # (a DEX that keeps the carry must not lose it here).
                keep = m68(need | (through & ~clob))
                if (key, node) in getattr(self, "fused_cmp", ()):
                    # the fused branch reads the borrow from the 68k C bit
                    keep = (keep & ~0x10) | 0x01
                out.append("\tmove.w\tccr,d4")
                out.append(f"\tandi.w\t#{hx(keep)},d4")
                out.append(f"\tandi.w\t#{hx(m68(save))},d5")
                out.append("\tor.w\td4,d5")
            out.append("\tmove.w\td5,ccr")
        falls = self.succ(p, node, "fall")
        self.goto(key, falls[0] if falls else None, nxt, out)
        if not falls and mn not in ("STP",):
            out.append(f"\tpea\t{hx(addr)}")
            out.append("\tjmp\trt_trap")
        return out

    # -- memory operands ---------------------------------------------------------
    def host_or_rt(self, snes: int, idx: str | None, word: bool) -> Loc:
        """Classify a static 24-bit address (plus an optional index register)."""
        bank, off = snes >> 16, snes & 0xFFFF
        ix = f",{idx}.l" if idx else ""
        wram = None
        if bank in (0x7E, 0x7F):
            wram = ((bank - 0x7E) << 16) | off
        elif (bank & 0x7F) < 0x40 and off < 0x2000:
            wram = off
        if wram is not None:
            if wram < 0x8000:
                return Loc("host", f"({hx(wram)},a5{ix})")
            return Loc("host", f"(smkx_wram+{hx(wram)}{ix})")
        ri = rom_index(snes)
        if ri is not None:
            return Loc("host", f"(smkx_rom+{hx(ri)}{ix})", rom=True)
        if idx is None and (bank & 0x7F) < 0x40 and (off < 0x6000 or (bank & 0x3F) < 0x20):
            # A fixed register (or the DSP-1 in banks $00-$1F): direct entry.
            loc = Loc("io", prep=[f"\tmovea.w\t#{hx(off)},a0"], note="io")
            loc.reg = off
            return loc
        prep = [f"\tlea\t({hx(snes)}{ix}),a0"] if idx else [f"\tlea\t{hx(snes)},a0"]
        return Loc("rt", prep=prep, note="io")

    def loc(self, st, d) -> Loc:
        mode, v = d.mode, d.value
        word_access = None
        if mode in (O.DP, O.DPX, O.DPY):
            idx = {O.DP: None, O.DPX: "d1", O.DPY: "d2"}[mode]
            ix = f",{idx}.l" if idx else ""
            base = self.dp_base(st, v)
            if idx and (d.addr & 0x7FFFFF) in WIDE_INDEX:
                if base is not None:
                    return Loc("rt", prep=[f"\tlea\t({hx(base)},{idx}.l),a0"], note="dp-io")
                # D not known here: its value is in rt_dreg (kept by every PLD/TCD)
                return Loc("rt", prep=["\tmoveq\t#0,d6", "\tmove.w\trt_dreg,d6",
                                      f"\tlea\t({hx(v)},{idx}.l),a0", "\tadda.l\td6,a0"], note="dp-io")
            if base is None:
                return Loc("host", f"({hx(v)},a2{ix})")
            if base + 0xFF < 0x2000:
                return Loc("host", f"({hx(base)},a5{ix})")
            if idx:
                # D on the registers (CT's $7E3000 overlay: D = $2100, the
                # register picked by X): the address at run time, through
                # the generic helpers (lea keeps the CCR).
                return Loc("rt", prep=[f"\tlea\t({hx(base)},{idx}.l),a0"], note="dp-io")
            return self.host_or_rt(base, None, False)
        if mode in (O.SR, O.SRIY):
            if mode == O.SR:
                return Loc("host", f"({hx(v)},a6)")
            prep = ["\tmoveq\t#0,d6", f"\tmove.w\t({hx(v)},a6),d6", "\tror.w\t#8,d6", "\tmovea.l\td6,a0"]
            prep.append("\tlea\t(0,a0,d3.l),a0" if not isinstance(st.dbr, int)
                        else f"\tadda.l\t#{hx(st.dbr << 16)},a0")
            prep.append("\tlea\t(0,a0,d2.l),a0")
            return Loc("rt", prep=prep, note="ptr")
        if mode in (O.ABS, O.ABSX, O.ABSY):
            idx = {O.ABS: None, O.ABSX: "d1", O.ABSY: "d2"}[mode]
            ix = f",{idx}.l" if idx else ""
            dbr = st.dbr
            if idx and v < 0x100 and (not isinstance(dbr, int) or (dbr & 0x7F) < 0x40):
                # `$0000,x`: the index register holds a full address, which may
                # be ROM, I/O or WRAM; resolve bank:base+index at runtime.
                prep = [f"\tlea\t({hx(v)},{idx}.l),a0"]
                prep.append("\tlea\t(0,a0,d3.l),a0" if not isinstance(dbr, int)
                            else f"\tadda.l\t#{hx(dbr << 16)},a0")
                return Loc("rt", prep=prep, note="indexed-pointer")
            if isinstance(dbr, int):
                return self.host_or_rt((dbr << 16) | v, idx, False)
            # Bank known only at runtime.
            if v < 0x2000:
                return Loc("host", f"({hx(v)},a4{ix})")
            if v >= 0x8000:
                return Loc("host", f"({hx(v - 0x8000)},a3{ix})")
            prep = [f"\tlea\t({hx(v)}{ix}),a0", "\tlea\t(0,a0,d3.l),a0"] if idx else \
                   [f"\tlea\t{hx(v)}.w,a0", "\tlea\t(0,a0,d3.l),a0"]
            loc = Loc("rt", prep=prep, note="dbr-io")
            if not idx and v == 0x217F:
                loc.wm217f = True           # the WRAM port trick (read/write)
            if not idx:
                loc.dbr_reg = v             # rt_d{r,w}{b,w}<reg> when there is one
                if v in HW_REGS and "guard" not in OFF:
                    loc.guard_reg = v       # a register in a system bank, memory in any other
            return loc
        if mode in (O.LONG, O.LONGX):
            if mode == O.LONGX and ((v >> 16) & 0x7F) < 0x40 and (v & 0xFFFF) < 0x100:
                # `lda $010000,x` (Super Castlevania IV's script readers): X is a full
                # 16-bit address in the bank, ROM above $8000 and WRAM below
                # $2000, so the access is resolved at run time (as `$0000,x`).
                return Loc("rt", prep=[f"\tlea\t({hx(v)},d1.l),a0"], note="indexed-pointer")
            return self.host_or_rt(v, "d1" if mode == O.LONGX else None, False)
        if mode in (O.DPI, O.DPIY, O.DPIX):
            src = self.dp_ea(st, v, "d1" if mode == O.DPIX else None)
            prep = ["\tmoveq\t#0,d6", f"\tmove.w\t{src},d6", "\tror.w\t#8,d6", "\tmovea.l\td6,a0"]
            prep.append("\tlea\t(0,a0,d3.l),a0" if not isinstance(st.dbr, int)
                        else f"\tadda.l\t#{hx(st.dbr << 16)},a0")
            if mode == O.DPIY:
                prep.append("\tlea\t(0,a0,d2.l),a0")
            return Loc("rt", prep=prep, note="ptr")
        if mode in (O.DPIL, O.DPILY):
            prep = ["\tmoveq\t#0,d6", f"\tmove.b\t{self.dp_ea(st, v + 2)},d6", "\tswap\td6",
                    f"\tmove.w\t{self.dp_ea(st, v)},d6", "\tror.w\t#8,d6", "\tmovea.l\td6,a0"]
            if mode == O.DPILY:
                prep.append("\tlea\t(0,a0,d2.l),a0")
            return Loc("rt", prep=prep, note="ptr")
        raise Unsupported(f"addressing mode {mode}")

    def dp_base(self, st, v: int):
        """The bank-0 address of direct-page offset v when D is known."""
        return (st.dp + v) & 0xFFFF if isinstance(st.dp, int) else None

    def dp_ea(self, st, v: int, idx: str | None = None) -> str:
        """Host operand of a pointer kept in direct page (must be WRAM)."""
        ix = f",{idx}.l" if idx else ""
        base = self.dp_base(st, v)
        if base is None:
            return f"({hx(v)},a2{ix})"
        if base + 0xFF >= 0x2000:
            raise Unsupported("direct-page pointer outside WRAM")
        return f"({hx(base)},a5{ix})"

    # The multiplier and divider ($4202-$4206 in, $4214-$4217 out) are plain
    # arithmetic: the results are loaded from the register shadow (rt_io,
    # $2000-based), the operands stored there, and the writes that start an
    # operation call a short runtime routine (all registers and the CCR
    # kept) instead of the whole register dispatch. CT's battle effects
    # multiply per colour and per scanline.
    MATH_OUT = range(0x4214, 0x4218)
    # The same through an unknown data bank: runtime entries that test the
    # bank (a system bank: the register) and fall back to rt_rb/rt_wb.
    DBR_MATH = {("r", True, r) for r in range(0x4214, 0x4218)} | \
               {("r", False, 0x4214), ("r", False, 0x4216)} | \
               {("w", True, r) for r in range(0x4202, 0x4207)} | {("w", False, 0x4202)}

    def dbr_math(self, loc: Loc, rw: str, eight: bool) -> str | None:
        reg = getattr(loc, "dbr_reg", None)
        if (rw, eight, reg) in self.DBR_MATH:
            return f"rt_d{rw}{'b' if eight else 'w'}{reg:X}"
        return None

    # -- reads and writes -------------------------------------------------------------
    # A memory operand the analysis could not place (pointers, an unknown data
    # bank, an index register holding an address) is looked up at run time in
    # the runtime's page tables (rt_pages_r / rt_pages_w: one long per
    # 256-byte page of the address space, the host address of the page, 0 for
    # I/O; index = address >> 8 & $FFFF, so an address that wrapped past bank
    # $FF finds its page too). A 0 entry, or a word on the last byte of its
    # page, takes the generic helper out of line (the stub). Nothing in the
    # sequence touches X (the carry): rt_rb and its kin kept every register
    # and the CCR; this uses d7 (or d6), a1 and the flags but X.

    def new_label(self) -> str:
        self.nlbl += 1
        return f"q{self.cur_pid}_{self.nlbl}"

    def page_lookup(self, table: str, sc: str, slow: str) -> list[str]:
        """a1 = the host page of the address in a0, sc = its low byte."""
        # Bit-field offsets count from bit 31. Extract address bits 23..8,
        # ignoring the upper byte exactly as the old rotate/mask did. X is
        # untouched; these intermediate N/Z/C flags are dead. Keep ANDI for
        # the low byte: replacing a single mask with BFEXTU costs more work.
        index = [f"\tbfextu\t{sc}{{8:16}},{sc}"] if "bitfield" not in OFF else \
                [f"\tror.l\t#8,{sc}", f"\tandi.l\t#$ffff,{sc}"]
        return [f"\tmove.l\ta0,{sc}"] + index + [
                f"\tmovea.l\t({table},{sc}.l*4),a1", "\tcmpa.w\t#0,a1", f"\tbeq\t{slow}",
                f"\tmove.l\ta0,{sc}", f"\tandi.l\t#$ff,{sc}"]

    def paged_read(self, loc: Loc, eight: bool, dst: str) -> list[str]:
        s = sz(eight)
        slow, done = self.new_label(), self.new_label()
        lines = list(loc.prep) + self.page_lookup("rt_pages_r", "d7", slow)
        if eight:
            lines.append(f"\tmove.b\t(a1,d7.l),{dst}")
        else:
            lines += ["\tcmpi.b\t#$ff,d7", f"\tbeq\t{slow}", f"\tmove.w\t(a1,d7.l),{dst}", f"\tror.w\t#8,{dst}"]
        lines.append(f"{done}:")
        # the generic helper leaves the value in d6 and N/Z alone
        self.cur_stubs += [f"{slow}:", f"\tjsr\trt_r{s}", f"\tmove.{s}\td6,{dst}" if dst != "d6" else f"\ttst.{s}\td6",
                           f"\tbra\t{done}"]
        return lines

    def paged_write(self, loc: Loc, eight: bool, src: str, restore: bool = True) -> list[str]:
        s = sz(eight)
        slow, done = self.new_label(), self.new_label()
        sc = "d6" if src == "d7" else "d7"          # the scratch register is never the value
        lines = list(loc.prep) + self.page_lookup("rt_pages_w", sc, slow)
        if eight:
            lines.append(f"\tmove.b\t{src},(a1,{sc}.l)")
        else:
            lines += [f"\tcmpi.b\t#$ff,{sc}", f"\tbeq\t{slow}", f"\tror.w\t#8,{src}",
                      f"\tmove.w\t{src},(a1,{sc}.l)"]
            if restore:
                lines.append(f"\tror.w\t#8,{src}")
        lines.append(f"{done}:")
        self.cur_stubs += [f"{slow}:"] + ([] if src == "d6" else [f"\tmove.{s}\t{src},d6"]) + \
            [f"\tjsr\trt_w{s}", f"\tbra\t{done}"]
        return lines

    def guarded(self, loc: Loc, inner: list[str], generic: list[str]) -> list[str]:
        """A hardware register reached through an unknown data bank: a system
        bank (bit 6 of the bank clear: d3 is bank << 16) means the register,
        any other bank means WRAM or ROM at that offset, through the generic
        path out of line."""
        gen, done = self.new_label(), self.new_label()
        self.cur_stubs += [f"{gen}:"] + generic + [f"\tbra\t{done}"]
        return ["\tbtst\t#22,d3", f"\tbne\t{gen}"] + inner + [f"{done}:"]

    def io_loc(self, loc: Loc) -> Loc:
        io = Loc("io", prep=[f"\tmovea.w\t#{hx(loc.guard_reg)},a0"], note="io")
        io.reg = loc.guard_reg
        return io

    def read(self, loc: Loc, eight: bool, dst: str) -> tuple[list[str], bool]:
        """Load into dst's low byte/word. Returns (lines, nz_valid)."""
        s = sz(eight)
        reg = getattr(loc, "reg", None)
        if loc.kind == "io" and reg in self.MATH_OUT and (eight or reg + 1 in self.MATH_OUT):
            ea = f"(rt_io+{hx(reg - 0x2000)})"
            if eight:
                return [f"\tmove.b\t{ea},{dst}"], True
            return [f"\tmove.w\t{ea},{dst}", f"\tror.w\t#8,{dst}"], True
        if loc.kind == "io" and reg in SHADOW_R and (eight or reg + 1 in SHADOW_R) and "plain" not in OFF:
            ea = f"(rt_io+{hx(reg - 0x2000)})"          # rt_io_read's .shadow
            if eight:
                return [f"\tmove.b\t{ea},{dst}"], True
            return [f"\tmove.w\t{ea},{dst}", f"\tror.w\t#8,{dst}"], True
        if loc.kind == "io" and reg == 0x2180 and eight and "w2180" not in OFF:
            return ["\tjsr\trt_r2180"] + ([f"\tmove.b\td6,{dst}"] if dst != "d6" else ["\ttst.b\td6"]), True
        call = self.dbr_math(loc, "r", eight)
        if call:
            lines = [f"\tjsr\t{call}"]
            return lines + ([f"\tmove.{s}\td6,{dst}"] if dst != "d6" else [f"\ttst.{s}\td6"]), True
        if getattr(loc, "wm217f", False) and not eight:
            lines = ["\tjsr\trt_rw217f"]
            return lines + ([f"\tmove.w\td6,{dst}"] if dst != "d6" else ["\ttst.w\td6"]), True
        if loc.kind == "host":
            if eight:
                return [f"\tmove.b\t{loc.ea},{dst}"], True
            return [f"\tmove.w\t{loc.ea},{dst}", f"\tror.w\t#8,{dst}"], True
        if getattr(loc, "guard_reg", None) is not None:
            inner, _ = self.read(self.io_loc(loc), eight, dst)
            return self.guarded(loc, inner, self.generic_read(loc, eight, dst)), True
        if loc.kind == "rt" and loc.note in PAGED_NOTES:
            return self.paged_read(loc, eight, dst), True
        return self.generic_read(loc, eight, dst), True

    def generic_read(self, loc: Loc, eight: bool, dst: str) -> list[str]:
        s = sz(eight)
        lines = list(loc.prep) + [f"\tjsr\trt_{'io' if loc.kind == 'io' else ''}r{s}"]
        if dst != "d6":
            return lines + [f"\tmove.{s}\td6,{dst}"]
        return lines + [f"\ttst.{s}\td6"]

    def write(self, loc: Loc, eight: bool, src: str, restore: bool = True) -> list[str]:
        """Store src. A 16-bit store to host memory swaps the register and
        swaps it back; restore=False leaves it swapped (a temporary that is
        dead afterwards). The last swap also leaves N and Z of the true
        value, which read-modify-write instructions rely on."""
        s = sz(eight)
        reg = getattr(loc, "reg", None)
        if loc.kind == "io" and reg is not None:
            ea = f"(rt_io+{hx(reg - 0x2000)})"
            if eight and reg in (0x4202, 0x4204, 0x4205):
                return [f"\tmove.b\t{src},{ea}"]
            if not eight and reg == 0x4204:
                return [f"\tror.w\t#8,{src}", f"\tmove.w\t{src},{ea}", f"\tror.w\t#8,{src}"]
            call = {(True, 0x4203): "rt_wrmpyb", (True, 0x4206): "rt_wrdivb",
                    (False, 0x4202): "rt_wrmpyw"}.get((eight, reg))
            if call:
                lines = [] if src == "d6" else [f"\tmove.{s}\t{src},d6"]
                return lines + [f"\tjsr\t{call}"]
            inline = self.plain_write(loc, reg, eight, src, restore)
            if inline is not None:
                return inline
            if reg == 0x2180 and eight and "w2180" not in OFF:
                return ([] if src == "d6" else [f"\tmove.b\t{src},d6"]) + ["\tjsr\trt_w2180"]
        if getattr(loc, "wm217f", False) and not eight:
            return ([] if src == "d6" else [f"\tmove.w\t{src},d6"]) + ["\tjsr\trt_ww217f"]
        call = self.dbr_math(loc, "w", eight)
        if call:
            return ([] if src == "d6" else [f"\tmove.{s}\t{src},d6"]) + [f"\tjsr\t{call}"]
        if loc.kind == "host":
            if loc.rom:
                return ["\tnop\t\t\t\t; write to ROM ignored"]
            if eight:
                return [f"\tmove.b\t{src},{loc.ea}"]
            return [f"\tror.w\t#8,{src}", f"\tmove.w\t{src},{loc.ea}"] + \
                ([f"\tror.w\t#8,{src}"] if restore else [])
        if getattr(loc, "guard_reg", None) is not None:
            inner = self.write(self.io_loc(loc), eight, src, restore)
            return self.guarded(loc, inner, self.generic_write(loc, eight, src))
        if loc.kind == "rt" and loc.note in PAGED_NOTES:
            return self.paged_write(loc, eight, src, restore)
        return self.generic_write(loc, eight, src)

    def generic_write(self, loc: Loc, eight: bool, src: str) -> list[str]:
        s = sz(eight)
        lines = list(loc.prep)
        if src != "d6":
            lines.append(f"\tmove.{s}\t{src},d6")
        lines.append(f"\tjsr\trt_{'io' if loc.kind == 'io' else ''}w{s}")
        return lines

    def plain_write(self, loc: Loc, reg: int, eight: bool, src: str, restore: bool):
        """A write to a register that only lands in the shadow array: one
        store. $21xx registers are logged while an IRQ runs (rt_logging), so
        they test that first and leave for the generic path when it is set;
        $42xx/$43xx never are."""
        if "plain" in OFF:
            return None
        ea = f"(rt_io+{hx(reg - 0x2000)})"
        def plain(r):
            return r in PLAIN21 or r in PLAIN42 or r in PLAIN43
        if eight:
            if reg == 0x4200:                     # NMITIMEN: the shadow and the NMI enable byte
                return [f"\tmove.b\t{src},{ea}", f"\tmove.b\t{src},rt_nmitimen"]
            if not plain(reg):
                return None
            store = [f"\tmove.b\t{src},{ea}"]
        else:
            if not (plain(reg) and plain(reg + 1)):
                return None
            store = [f"\tror.w\t#8,{src}", f"\tmove.w\t{src},{ea}"] + ([f"\tror.w\t#8,{src}"] if restore else [])
        if reg in PLAIN21 or (not eight and reg + 1 in PLAIN21):
            # the 16-bit form of a $21xx pair: both must be logged-or-not alike (they are: both plain)
            slow, done = self.new_label(), self.new_label()
            self.cur_stubs += [f"{slow}:"] + self.generic_write(loc, eight, src) + [f"\tbra\t{done}"]
            return ["\ttst.b\trt_logging", f"\tbne\t{slow}"] + store + [f"{done}:"]
        return store

    # -- non-control instructions --------------------------------------------------
    def op(self, key, p, node, st, d, live):
        mn, mode, v = d.mnem, d.mode, d.value
        m8, x8 = st.m8, st.x8
        NZ = N | Z
        reg = {"A": "d0", "X": "d1", "Y": "d2"}

        def need_width(flag, what):
            if flag is None:
                raise Unsupported(f"{mn} with unknown {what} width")
            return flag

        def operand(eight: bool, dst: str = "d7"):
            if mode in (O.IMM_M, O.IMM_X):
                return [f"\tmove.{sz(eight)}\t#{hx(v)},{dst}"], True
            return self.read(self.loc(st, d), eight, dst)

        c_live_through = bool(live & C) and not (flag_defs(mn, mode, v) & C)

        if mn in ("LDA", "LDX", "LDY"):
            r = reg[mn[2]]
            eight = need_width(m8 if mn == "LDA" else x8, "register")
            if mode in (O.IMM_M, O.IMM_X):
                return [f"\tmove.{sz(eight)}\t#{hx(v)},{r}"], NZ | V, NZ
            lines, _ = self.read(self.loc(st, d), eight, r)
            return lines, NZ | V, NZ

        if mn in ("STA", "STX", "STY", "STZ"):
            eight = need_width(m8 if mn in ("STA", "STZ") else x8, "register")
            loc = self.loc(st, d)
            if mn == "STZ":
                if loc.kind == "host" and not loc.rom:
                    return [f"\tclr.{sz(eight)}\t{loc.ea}"], NZ | V, 0
                # io and rt both go through write()
                return ["\tmoveq\t#0,d6"] + self.write(loc, eight, "d6"), NZ | V, 0
            return self.write(loc, eight, reg[mn[2]]), NZ | V, 0

        if mn in ("TAX", "TAY", "TXA", "TYA", "TXY", "TYX"):
            src, dst = reg[mn[1]], reg[mn[2]]
            eight = need_width(m8 if dst == "d0" else x8, "register")
            return [f"\tmove.{sz(eight)}\t{src},{dst}"], NZ | V, NZ
        if mn in ("TCS", "TXS"):
            # rt_setsp also switches the 68k return stack when S moves to
            # another stack region (CT's menu tasks are coroutines).
            src = "d0" if mn == "TCS" else "d1"
            return ["\tmoveq\t#0,d7", f"\tmove.w\t{src},d7", "\tjsr\trt_setsp"], 0, 0
        if mn in ("TSC", "TSX"):
            dst = "d0" if mn == "TSC" else "d1"
            eight = False if mn == "TSC" else need_width(x8, "index")
            return ["\tmove.l\ta6,d7", "\tsub.l\ta5,d7", f"\tmove.{sz(eight)}\td7,{dst}"], ALL, NZ
        if mn == "TDC":
            # D = a2 - a5 without SUB, which would overwrite X, where the 65816
            # carry lives (FF6's `ADC #ptr : shorta0 : LDA bank : ADC #^base`
            # lost it here: shorta0 is TDC, unknown D)
            return ["\tmovea.l\ta2,a1", "\tsuba.l\ta5,a1", "\tmove.l\ta1,d7", "\tmove.w\td7,d0"], NZ | V, NZ
        if mn == "TCD":
            return ["\tmoveq\t#0,d7", "\tmove.w\td0,d7", "\tmove.w\td7,rt_dreg", "\tlea\t(0,a5,d7.l),a2",
                    "\ttst.w\td0"], NZ | V, NZ

        if mn in ("AND", "ORA", "EOR"):
            eight = need_width(m8, "accumulator")
            s = sz(eight)
            op = {"AND": "and", "ORA": "or", "EOR": "eor"}[mn]
            if mode == O.IMM_M:
                return [f"\t{op}i.{s}\t#{hx(v)},d0"], NZ | V, NZ
            lines, _ = operand(eight)
            return lines + [f"\t{op}.{s}\td7,d0"], NZ | V, NZ

        if mn in ("ADC", "SBC"):
            eight = need_width(m8, "accumulator")
            if st.d is None:
                raise Unsupported("ADC/SBC with unknown decimal mode")
            lines, _ = operand(eight)
            s = sz(eight)
            vout = ["\tsvs\trt_vf"] if live & V else []     # Scc leaves the CCR alone
            if not st.d:
                folded = self.folded_carry(key, p, node)     # CLC : ADC and SEC : SBC
                if folded:
                    # The carry in is known, so the plain add or subtract does:
                    # one instruction instead of a CCR edit and the extended one
                    # (which needs the Z bit preset to give a Z of its own).
                    if mode in (O.IMM_M, O.IMM_X):
                        ins = ("add" if mn == "ADC" else "sub")
                        lines = [f"\t{ins}q.{s}\t#{v},d0" if 1 <= v <= 8 else f"\t{ins}i.{s}\t#{hx(v)},d0"]
                    else:
                        lines = lines + [f"\t{'add' if mn == 'ADC' else 'sub'}.{s}\td7,d0"]
                    # (a 68k subtract leaves the borrow in X: the 65816 carry inverted)
                    fix = ["\teori\t#$10,ccr"] if mn == "SBC" and live & C else []
                    return lines + vout + fix, ALL, ALL
                zpre = ["\tori\t#$04,ccr"] if live & Z else []   # addx/subx only ever clear Z
                if mn == "ADC":
                    return lines + zpre + [f"\taddx.{s}\td7,d0"] + vout, ALL, ALL
                return lines + ["\teori\t#$10,ccr"] + zpre + [f"\tsubx.{s}\td7,d0"] + vout + \
                    (["\teori\t#$10,ccr"] if live & C else []), ALL, ALL
            bcd = "abcd" if mn == "ADC" else "sbcd"
            flip = [] if mn == "ADC" else ["\teori\t#$10,ccr"]
            # V after a decimal ADC/SBC: approximated by the binary-mode
            # overflow of the same operands and carry, computed on copies.
            # Emitted only where liveness says V can be read.
            vpre = []
            if live & V:
                bin_op = "addx" if mn == "ADC" else "subx"
                vpre = ["\tmove.w\tccr,d4", f"\tmove.{s}\td0,d6"] + flip + \
                       ["\tori\t#$04,ccr", f"\t{bin_op}.{s}\td7,d6", "\tsvs\trt_vf",
                        "\tmove.w\td4,ccr"]
            if eight:
                body = lines + vpre + flip + ["\tori\t#$04,ccr", f"\t{bcd}\td7,d0"] + flip + ["\ttst.b\td0"]
                return body, ALL, C | N | Z | (V if vpre else 0)
            body = lines + vpre + flip + ["\tori\t#$04,ccr", f"\t{bcd}\td7,d0", "\tror.w\t#8,d7",
                                          "\tror.w\t#8,d0", f"\t{bcd}\td7,d0", "\tror.w\t#8,d0"] + flip
            return body, ALL, C | N | Z | (V if vpre else 0)

        if mn in ("CMP", "CPX", "CPY"):
            r = {"CMP": "d0", "CPX": "d1", "CPY": "d2"}[mn]
            eight = need_width(m8 if mn == "CMP" else x8, "register")
            s = sz(eight)
            if mode in (O.IMM_M, O.IMM_X):
                lines = [f"\tcmpi.{s}\t#{hx(v)},{r}"]
            else:
                lines, _ = operand(eight)
                lines = lines + [f"\tcmp.{s}\td7,{r}"]
            if live & C and not self.fused_branch(key, p, node):
                lines += ["\tmove.w\tccr,d7", "\tmove.b\t(rt_cmpfix,d7.w),d7", "\tmove.w\td7,ccr"]
            return lines, ALL, ALL

        if mn == "BIT":
            eight = need_width(m8, "accumulator")
            s = sz(eight)
            if mode == O.IMM_M:
                if v and "bitfield" not in OFF:
                    low = (v & -v).bit_length() - 1
                    width = v.bit_length() - low
                    if width == 1:
                        # Immediate BIT changes Z only. BTST also keeps N
                        # and X, avoiding a flag merge when N is live.
                        return [f"\tbtst\t#{low},d0"], Z, Z
                # A multi-bit BFTST would be smaller but is slower than this
                # MOVE/ANDI pair on the patched Hatari 68030 timing model.
                return [f"\tmove.{s}\td0,d7", f"\tandi.{s}\t#{hx(v)},d7"], NZ | V, Z
            lines, _ = operand(eight)
            if live & (N | V):
                return lines + [f"\tjsr\trt_bit{8 if eight else 16}"], NZ | V, N | V | Z
            return lines + [f"\tand.{s}\td0,d7"], NZ | V, Z

        if mn in ("INC", "DEC", "INX", "INY", "DEX", "DEY"):
            up = mn in ("INC", "INX", "INY")
            if mn in ("INC", "DEC") and mode != O.IMP:
                eight = need_width(m8, "accumulator")
                loc = self.loc(st, d)
                lines, _ = self.read(loc, eight, "d7")
                lines += self.step_reg("d7", eight, up, c_live_through)
                lines += self.write(loc, eight, "d7", restore=bool(live & NZ))
                if loc.kind != "host" and live & NZ:
                    lines.append(f"\ttst.{sz(eight)}\td7")
                return lines, NZ | V | (0 if c_live_through else C), NZ
            r = {"INC": "d0", "DEC": "d0", "INX": "d1", "DEX": "d1", "INY": "d2", "DEY": "d2"}[mn]
            eight = need_width(m8 if r == "d0" else x8, "register")
            return self.step_reg(r, eight, up, c_live_through), NZ | V | (0 if c_live_through else C), NZ

        if mn in ("ASL", "LSR", "ROL", "ROR"):
            eight = need_width(m8, "accumulator")
            s = sz(eight)
            ins = {"ASL": "asl", "LSR": "lsr", "ROL": "roxl", "ROR": "roxr"}[mn]
            if mode == O.IMP:
                return [f"\t{ins}.{s}\t#1,d0"], ALL, ALL
            loc = self.loc(st, d)
            lines, _ = self.read(loc, eight, "d7")
            lines.append(f"\t{ins}.{s}\t#1,d7")
            lines += self.write(loc, eight, "d7", restore=bool(live & (N | Z)))
            if loc.kind != "host" and live & (N | Z):
                lines.append(f"\ttst.{s}\td7")
            return lines, ALL, ALL

        if mn in ("TSB", "TRB"):
            eight = need_width(m8, "accumulator")
            s = sz(eight)
            loc = self.loc(st, d)
            lines, _ = self.read(loc, eight, "d7")
            lines.append(f"\tmove.{s}\td7,d4")          # original value, for Z
            if mn == "TSB":
                lines.append(f"\tor.{s}\td0,d7")
            else:
                lines += [f"\tmove.{s}\td0,d6", f"\tnot.{s}\td6", f"\tand.{s}\td6,d7"]
            lines += self.write(loc, eight, "d7")
            lines.append(f"\tand.{s}\td0,d4")
            return lines, NZ | V, Z

        if mn == "XBA":
            return ["\tror.w\t#8,d0", "\ttst.b\td0"], NZ | V, NZ

        # Stack.
        def stack_width(w):
            # An interrupt handler entered in the interrupted code's widths
            # saves and restores registers in them; the analysis counts
            # such pushes as 16-bit, and so does the code (the VBL
            # restores the real registers after the handler anyway).
            if w is None and (p.ctx.m8 is None or p.ctx.x8 is None):
                return False
            return need_width(w, "register")
        if mn in ("PHA", "PHX", "PHY"):
            r = reg[mn[2]]
            eight = stack_width(m8 if r == "d0" else x8)
            return self.push_reg(r, eight), NZ | V, 0
        if mn in ("PLA", "PLX", "PLY"):
            r = reg[mn[2]]
            eight = stack_width(m8 if r == "d0" else x8)
            return self.pull_reg(r, eight, key, p, node, st), NZ | V, NZ
        if mn == "PHB":
            return ["\tsubq.l\t#1,a6", "\tmove.l\td3,d7", "\tswap\td7", "\tmove.b\td7,1(a6)"], NZ | V, 0
        if mn == "PLB" and "plb" in OFF:
            lines = ["\tmove.b\t1(a6),d7", "\taddq.l\t#1,a6"] + self.esc_fix(key, p, node, st, 1)
            return lines + ["\tjsr\trt_setdbr", "\ttst.b\td7"], NZ | V, NZ
        if mn == "PLB":
            # rt_setdbr's work without the call (it saved the CCR and d7 on the
            # stack): d7 = the bank, the tables give a4 = bank:$0000 and
            # a3 = bank:$8000. Only X-safe instructions (the carry lives in X).
            lines = ["\tmoveq\t#0,d7", "\tmove.b\t1(a6),d7", "\taddq.l\t#1,a6"] + self.esc_fix(key, p, node, st, 1)
            lines += ["\tmove.b\td7,rt_dbr", "\tmove.l\td7,d3", "\tswap\td3",
                      "\tmovea.l\t(rt_lotab,d7.w*4),a4", "\tmovea.l\t(rt_hitab,d7.w*4),a3", "\ttst.b\td7"]
            return lines, NZ | V, NZ
        if mn == "PHK":
            return ["\tsubq.l\t#1,a6", f"\tmove.b\t#{hx(node[0] >> 16)},1(a6)"], NZ | V, 0
        if mn == "PEA":
            return ["\tsubq.l\t#2,a6", f"\tmove.w\t#{hx(((v & 0xFF) << 8) | (v >> 8))},1(a6)"], NZ | V, 0
        if mn == "PER":
            t = ((node[0] + 3) + (v - 0x10000 if v & 0x8000 else v)) & 0xFFFF
            return ["\tsubq.l\t#2,a6", f"\tmove.w\t#{hx(((t & 0xFF) << 8) | (t >> 8))},1(a6)"], NZ | V, 0
        if mn == "PEI":
            return ["\tmove.w\t" + f"{self.dp_ea(st, v)},d7", "\tsubq.l\t#2,a6", "\tmove.w\td7,1(a6)"], NZ | V, 0
        if mn == "PHD":
            return ["\tmovea.l\ta2,a1", "\tsuba.l\ta5,a1", "\tmove.l\ta1,d7", "\tror.w\t#8,d7", "\tsubq.l\t#2,a6",
                    "\tmove.w\td7,1(a6)"], NZ | V, 0
        if mn == "PLD":
            lines = ["\tmoveq\t#0,d7", "\tmove.w\t1(a6),d7", "\taddq.l\t#2,a6"]
            lines += self.esc_fix(key, p, node, st, 2)
            return lines + ["\tror.w\t#8,d7", "\tmove.w\td7,rt_dreg", "\tlea\t(0,a5,d7.l),a2", "\ttst.w\td7"], \
                NZ | V, NZ
        if mn == "PHP":
            bits = (0x20 if m8 else 0) | (0x10 if x8 else 0) | (0x08 if st.d else 0)
            return ["\tmove.w\tccr,d7", "\tmove.b\t(rt_ccr2p,d7.w),d6", f"\tori.b\t#{hx(bits)},d6",
                    "\tandi.b\t#$BF,d6", "\tmove.b\trt_vf,d4", "\tandi.b\t#$40,d4", "\tor.b\td4,d6",
                    "\tsubq.l\t#1,a6", "\tmove.b\td6,1(a6)", "\tmove.w\td7,ccr"], 0, 0
        if mn == "PLP":
            lines = ["\tmoveq\t#0,d7", "\tmove.b\t1(a6),d7", "\taddq.l\t#1,a6"]
            lines += self.esc_fix(key, p, node, st, 1)
            if len(st.stack) == 0:
                lines.append("\tmove.b\td7,rt_lastp")     # for width_split
            lines += self.width_fix(key, p, node, st)
            lines += ["\tbtst\t#6,d7", "\tsne\trt_vf", "\tmove.b\t(rt_p2ccr,d7.w),d7", "\tmove.w\td7,ccr"]
            return lines, ALL, ALL

        # Processor state.
        if mn == "REP" or mn == "SEP":
            lines = []
            mask = 0
            if v & 0x01: mask |= 0x10
            if v & 0x02: mask |= 0x04
            if v & 0x80: mask |= 0x08
            if mask:
                lines.append(f"\tandi\t#{hx(0xFF & ~mask)},ccr" if mn == "REP" else f"\tori\t#{hx(mask)},ccr")
            if v & 0x40:
                lines.append("\tsf\trt_vf" if mn == "REP" else "\tst\trt_vf")
            if mn == "SEP" and v & 0x10 and st.x8 is not True:
                lines += ["\tmove.w\tccr,d7", "\tandi.w\t#$FF,d1", "\tandi.w\t#$FF,d2", "\tmove.w\td7,ccr"]
            if v & 0x04:
                lines.append("\tjsr\t" + ("rt_cli" if mn == "REP" else "rt_sei"))
            return lines, 0, flag_defs(mn, mode, v)
        if mn in ("CLC", "SEC"):
            if self.folded_carry(key, p, node):
                return [], 0, C             # the ADC/SBC right behind it needs no carry in X
            return (["\tandi\t#$EF,ccr"] if mn == "CLC" else ["\tori\t#$10,ccr"]), 0, C
        if mn == "CLV":
            return ["\tsf\trt_vf"], 0, V
        if mn in ("CLD", "SED", "XCE", "NOP"):
            return [], 0, 0
        if mn == "SEI":
            return ["\tjsr\trt_sei"], 0, 0
        if mn == "CLI":
            return ["\tjsr\trt_cli"], 0, 0
        if mn in ("MVN", "MVP"):
            dst, src = v & 0xFF, v >> 8
            return [f"\tmove.l\t#{hx(src << 16)},d6", f"\tmove.l\t#{hx(dst << 16)},d7",
                    f"\tjsr\trt_{mn.lower()}"], ALL, 0
        raise Unsupported(mn)

    def step_reg(self, r: str, eight: bool, up: bool, keep_x: bool) -> list[str]:
        s = sz(eight)
        if not keep_x:
            return [f"\t{'addq' if up else 'subq'}.{s}\t#1,{r}"]
        table = "rt_inc8" if up else "rt_dec8"
        if eight:
            if r == "d0":
                return ["\tmove.w\td0,d6", "\tandi.w\t#$FF,d6", f"\tmove.b\t({table},d6.w),d0"]
            if r == "d7":
                return ["\tandi.w\t#$FF,d7", f"\tmove.b\t({table},d7.w),d7"]
            return [f"\tmove.b\t({table},{r}.w),{r}"]
        return [f"\tmovea.l\t{r},a1", f"\t{'addq' if up else 'subq'}.w\t#1,a1", f"\tmove.w\ta1,{r}"]

    def push_reg(self, r: str, eight: bool) -> list[str]:
        if eight:
            return ["\tsubq.l\t#1,a6", f"\tmove.b\t{r},1(a6)"]
        return ["\tsubq.l\t#2,a6", f"\tror.w\t#8,{r}", f"\tmove.w\t{r},1(a6)", f"\tror.w\t#8,{r}"]

    def pull_reg(self, r: str, eight: bool, key, p, node, st) -> list[str]:
        n = 1 if eight else 2
        if eight:
            lines = [f"\tmove.b\t1(a6),{r}", "\taddq.l\t#1,a6"]
        else:
            lines = [f"\tmove.w\t1(a6),{r}", "\taddq.l\t#2,a6"]
        lines += self.esc_fix(key, p, node, st, n)
        if not eight:
            lines.append(f"\tror.w\t#8,{r}")
        else:
            lines.append(f"\ttst.b\t{r}")
        return lines

    def esc_fix(self, key, p, node, st, n: int) -> list[str]:
        """A pull that removes this procedure's return frame drops the
        matching 68k return address."""
        depth = len(st.stack)
        below = max(0, n - depth)
        if not below or st.switched:
            return []       # (after TCS/TXS the 68k stack is rt_setsp's business)
        frame = self.frame_size(key)
        before = st.esc
        after = st.esc + below
        if frame is None:
            raise Unsupported("pull below a return frame of unknown size")
        if before < frame <= after:
            return ["\taddq.l\t#4,sp"]
        return []

    def frame_size(self, key):
        kinds = {self.prog.procs[ck].insns[site].mnem for ck, cn, site in self.prog.callers.get(key, ())}
        if kinds == {"JSR"}:
            return 2
        if kinds == {"JSL"}:
            return 3
        if not kinds:
            return 2          # pointer dispatch sites are all JSR (abs,x)
        return None

    def width_fix(self, key, p, node, st) -> list[str]:
        """PLP switching the index registers to 8 bits clears their high bytes."""
        out = []
        for kind, dst in p.succ.get(node, []):
            nst = self.state_of.get((key, dst))
            if nst is not None and nst.x8 and st.x8 is not True:
                out = ["\tandi.w\t#$FF,d1", "\tandi.w\t#$FF,d2"]
        return out

    def folded_carry(self, key, p, node) -> bool:
        """True for a CLC whose only successor is a binary ADC (or a SEC and a
        SBC), and for that ADC/SBC: the pair is emitted as a plain add or
        subtract. The second instruction must be reachable only through the
        first, so nothing arrives with another carry: one predecessor edge,
        not an entry, not the continuation of a call, and no hook (probe,
        mark, native routine) at either."""
        if "adc" in OFF:
            return False
        cache = self.__dict__.setdefault("_folded", {})
        if (key, node) in cache:
            return cache[(key, node)]
        mn = p.insns[node[0]].mnem if node[0] in p.insns else None
        pair = None
        if mn in ("CLC", "SEC"):
            edges = p.succ.get(node, [])
            if len(edges) == 1 and edges[0][0] == "fall":
                pair = (node, edges[0][1])
        elif mn in ("ADC", "SBC"):
            srcs = self.pred_src[key].get(node, [])
            if len(srcs) == 1 and len(p.succ.get(srcs[0], [])) == 1:
                pair = (srcs[0], node)
        ok = False
        if pair is not None:
            first, second = pair
            a, b = p.insns.get(first[0]), p.insns.get(second[0])
            st = self.state_of.get((key, second))
            ok = (a is not None and b is not None and st is not None
                  and (a.mnem, b.mnem) in (("CLC", "ADC"), ("SEC", "SBC"))
                  and first[0] + a.length == second[0] and self.preds[key].get(second, 0) == 1
                  and second[0] != p.entry and st.d is False
                  and second not in p.conts.values()
                  and not {first[0], second[0]} & (self.probes | set(self.marks) | set(self.natives))
                  and second not in p.dynamic and first not in p.dynamic)
            cache[(key, first)] = cache[(key, second)] = ok
        cache[(key, node)] = ok
        return ok

    def fused_branch(self, key, p, node) -> bool:
        """CMP immediately followed by BCC/BCS that alone consumes the carry."""
        falls = self.succ(p, node, "fall")
        if len(falls) != 1:
            return False
        b = falls[0]
        if self.preds[key].get(b, 0) != 1:
            return False
        bi = p.insns.get(b[0])
        if bi is None or bi.mnem not in ("BCC", "BCS"):
            return False
        if self.live.get((key, b), ALL) & C:
            return False
        idx = self.node_index[key]
        if idx.get(b) != idx[node] + 1:
            return False
        self.fused = getattr(self, "fused", set())
        self.fused.add((key, b))
        self.fused_cmp = getattr(self, "fused_cmp", set())
        self.fused_cmp.add((key, node))
        return True

    # -- control transfers ----------------------------------------------------------
    def control(self, key, p, node, st, d, nxt, live):
        mn, mode = d.mnem, d.mode
        addr = node[0]
        cond = {"BEQ": "beq", "BNE": "bne", "BMI": "bmi", "BPL": "bpl"}
        if mn in ("BVS", "BVC"):
            # V is the byte rt_vf; testing it clobbers N and Z (the carry
            # stays in X), so they are saved where still live.
            out = []
            taken = self.succ(p, node, "branch")
            fall = self.succ(p, node, "fall")
            op = "bne" if mn == "BVS" else "beq"
            if live & (N | Z):
                out += ["\tmove.w\tccr,d5", "\ttst.b\trt_vf"]
                if taken:
                    out += [f"\t{'beq' if mn == 'BVS' else 'bne'}.s\t.vf", "\tmove.w\td5,ccr",
                            f"\tbra\t{self.node_label(key, taken[0])}", ".vf:"]
                out.append("\tmove.w\td5,ccr")
            else:
                out.append("\ttst.b\trt_vf")
                if taken:
                    out.append(f"\t{op}\t{self.node_label(key, taken[0])}")
            self.goto(key, fall[0] if fall else None, nxt, out)
            return out
        if mn in cond or mn in ("BCC", "BCS"):
            out = []
            taken = self.succ(p, node, "branch")
            fall = self.succ(p, node, "fall")
            if mn in cond:
                op = cond[mn]
            elif (key, node) in getattr(self, "fused", set()):
                op = "bcs" if mn == "BCC" else "bcc"          # 68k C is the 65816 borrow
            else:
                out += ["\tmove.w\tccr,d7", "\tmove.b\t(rt_x2c,d7.w),d7", "\tmove.w\td7,ccr"]
                op = "bcc" if mn == "BCC" else "bcs"
            if taken:
                out.append(f"\t{op}\t{self.node_label(key, taken[0])}")
            self.goto(key, fall[0] if fall else None, nxt, out)
            return out
        if mn in ("BRA", "BRL"):
            out = []
            self.goto(key, self.succ(p, node)[0] if self.succ(p, node) else None, nxt, out)
            return out or ["\t; bra falls through"]
        split = self.width_split(key, p, node, st)
        if split is not None:
            return split
        if node in p.dynamic and p.dynamic[node].startswith("ramcode"):
            return self.ramcode(key, p, node, st, d, nxt, live)
        if mn == "JML" and node in p.dynamic and p.dynamic[node].startswith("ramjml"):
            # The JML an engine stored at the trampoline ($0500/$0504 in CT, $1500/$1504 in FF6): rt_ramjml resolves it
            # (or an RTI stored there) to a translated handler.
            out = [f"\tmovea.w\t#{hx(d.value & 0xFFFF)},a0", "\tjsr\trt_ramjml"]
            out = self.guard(out, live)
            out.append("\tjmp\t(a0)")
            return out
        if mn in ("JMP", "JML"):
            if mode in (O.ABS, O.LONG):
                s = self.succ(p, node, "jump")
                if not s:
                    return [f"\tpea\t{hx(addr)}", "\tjmp\trt_trap"]
                out = self.ovl_guard(addr, s[0][0])
                if out:
                    out = self.guard(out, live)
                self.goto(key, s[0], nxt, out)
                return out or ["\t; jmp falls through"]
            if node in p.dynamic and p.dynamic[node].startswith("table"):
                return self.table_jump(key, p, node, d)
            if node in p.dynamic and p.dynamic[node].startswith("percall"):
                # the return address is on the 65816 stack already (PER)
                out = self.pointer_target(st, d, node)
                out.append(f"\tmove.w\t#{self.dispatch_ctx(st)},d6")
                out.append("\tjsr\trt_dispatch")
                out = self.guard(out, live)
                out.append("\tjsr\t(a0)")
                self.continuation(key, p, node, nxt, out, assumed=st)
                return out
            return self.dynamic_jump(key, p, node, st, d)
        if mn in ("JSR", "JSL"):
            return self.call(key, p, node, st, d, nxt, live)
        if mn == "PLP":
            split = self.plp_split(key, p, node, st, nxt)
            if split is not None:
                return split
        if mn in ("RTS", "RTL", "RTI"):
            out = []
            ex = self.exit_ctx_of(p, node, st)
            out.append(f"\tlea\t{self.exit_id(ex)}.w,a0")
            n = {"RTS": 2, "RTL": 3, "RTI": 4}[mn]
            out.append(f"\taddq.l\t#{n},a6")
            out.append("\trts")
            return out
        if mn in ("BRK", "COP", "STP", "WDM", "WAI"):
            return [f"\tpea\t{hx(addr)}", "\tjmp\trt_trap"]
        return None

    def ramcode(self, key, p, node, st, d, nxt, live):
        """A JSR/JSL/JMP/JML into code the game built in WRAM: rt_ramcode
        interprets it up to its RTS/RTL (which pulls the frame). A jump
        returns from this procedure through that RTS."""
        target = int(p.dynamic[node].split("$")[1], 16)
        if st.m8 is None or st.x8 is None:
            raise Unsupported("WRAM code with unknown widths")
        body = [f"\tmove.l\t#{hx(target)},a0", f"\tmove.w\t#{int(bool(st.m8)) | 2 * int(bool(st.x8))},d6",
                "\tjsr\trt_ramcode"]
        if d.mnem in ("JSR", "JSL"):
            out = self.guard(self.push_return(st, d, node, live) + body, live)
            falls = self.succ(p, node, "return")
            self.goto(key, falls[0] if falls else None, nxt, out)
            return out
        after = self.state_of[(key, node)]
        from .analyze import State
        ex = State(**vars(after)).ctx()
        ex = type(ex)(ex.m8, ex.x8, ex.d, ex.dbr, ex.e, None)
        return body + [f"\tlea\t{self.exit_id(ex)}.w,a0", "\trts"]

    def ovl_guard(self, site: int, target: int) -> list[str]:
        """Entering a WRAM program (analyze.WRAM_OVERLAYS) from outside it:
        check that WRAM still holds the bytes it was translated from."""
        ov = self.prog.overlay_at(target)
        if ov is None or self.prog.overlay_at(site) is not None:
            return []
        start, data = ov
        label = f"o{len(self.ovl_checks)}_{target:06X}"
        self.ovl_checks.append(label)
        chunk = data[target - start:target - start + 8].ljust(8, b"\0")
        self.tables.append(f"{label}:\tdc.b\t" + ",".join(f"${b:02X}" for b in chunk))
        return [f"\tmove.l\t#{hx(target)},a0", f"\tlea\t{label},a1", "\tjsr\trt_ovlcheck"]

    def exit_ctx_of(self, p, node, st):
        return st.ctx()

    def push_return(self, st, d, node, live) -> list[str]:
        """Write the 65816 return address JSR/JSL would push."""
        addr = node[0]
        if d.mnem == "JSL":
            ret = (addr + 3) & 0xFFFF
            return ["\tsubq.l\t#3,a6", f"\tmove.b\t#{hx(addr >> 16)},3(a6)",
                    f"\tmove.w\t#{hx(((ret & 0xFF) << 8) | (ret >> 8))},1(a6)"]
        ret = (addr + 2) & 0xFFFF
        return ["\tsubq.l\t#2,a6", f"\tmove.w\t#{hx(((ret & 0xFF) << 8) | (ret >> 8))},1(a6)"]

    def continuation(self, key, p, node, nxt, out, assumed=None) -> None:
        if (node, "yield") in p.conts:
            # A coroutine yield (analyze.YIELD_ROUTINES): resumed from the
            # scheduler with any exit id.
            dst = p.conts[(node, "yield")]
            self.cont_table(key, node, lambda ex, dst=dst: dst, out)
            return
        conts = {ex: dst for (n, ex), dst in p.conts.items() if n == node and ex not in ("assumed", "yield", "ramcode")}
        if assumed is not None:
            dst = p.conts.get((node, "assumed"))
            if dst is None:
                out += [f"\tpea\t{hx(node[0])}", "\tjmp\trt_noreturn"]
                return
            st = assumed
            from .analyze import open_exit
            def pick(ex, st=st, conts=conts, dst=dst):
                k = open_exit(ex)
                if k in conts:
                    return conts[k]
                if (ex.m8, ex.x8, ex.d, ex.e) == (st.m8, st.x8, st.d, st.e):
                    return dst
                return None
            self.cont_table(key, node, pick, out)
            return
        dests = set(conts.values())
        if not dests:
            out += [f"\tpea\t{hx(node[0])}", "\tjmp\trt_noreturn"]
            return
        if len(dests) == 1:
            self.goto(key, next(iter(dests)), nxt, out)
            return
        site = self.state_of[(key, node)]
        def pick(ex, conts=conts, site=site):
            if ex in conts:
                return conts[ex]
            if ex.dbr == IN or ex.dp == IN:
                # "Bank / D unchanged": what this call site had, which is how
                # the analysis resolved the exit when it built the continuation.
                from .analyze import Ctx
                here = Ctx(ex.m8, ex.x8, ex.d, site.dbr if ex.dbr == IN else ex.dbr, ex.e,
                           site.dp if ex.dp == IN else ex.dp)
                if here in conts:
                    return conts[here]
                same = {d for c, d in conts.items() if (c.m8, c.x8, c.d, c.e) == (ex.m8, ex.x8, ex.d, ex.e)}
                if len(same) == 1:
                    return same.pop()
            if ex.m8 is None or ex.x8 is None:
                # An escaped return whose widths come from a P byte its
                # caller pushed: the analysis resolved them at this site
                # (escape_into); the rest of the context picks the entry.
                same = {d for c, d in conts.items() if (c.d, c.e, c.dbr, c.dp) == (ex.d, ex.e, ex.dbr, ex.dp)
                        and (ex.m8 is None or c.m8 == ex.m8) and (ex.x8 is None or c.x8 == ex.x8)}
                if len(same) == 1:
                    return same.pop()
            return None
        self.cont_table(key, node, pick, out)

    def cont_table(self, key, node, pick, out) -> None:
        label = f"c{self.pid[key]}_{self.node_index[key][node]}"
        self.pending_tables = getattr(self, "pending_tables", [])
        self.pending_tables.append((label, key, pick))
        out.append(f"\tjmp\t([{label},a0.l*4])")

    def call(self, key, p, node, st, d, nxt, live):
        """JSR/JSL. Flags live into the callee survive the frame push and the
        target computation: they are saved in d5 and restored before the jump."""
        out = self.push_return(st, d, node, live)
        mode = d.mode
        targets = p.calls.get(node, [])
        if node in p.dynamic and p.dynamic[node].startswith("patched"):
            # The operand the game stored (analyze.PATCHED_CALLS): compared
            # byte by byte in WRAM (cmpi keeps the 68k X, the 65816 carry),
            # the translation of the target called; another value stops.
            opnd = (node[0] + 1) & 0xFFFF
            ea = (lambda o: f"({hx(o)},a5)") if opnd < 0x7FFF else (lambda o: f"(smkx_wram+{hx(o)})")
            base = f"pt{self.pid[key]}_{self.node_index[key][node]}"
            for i, (t, ctx) in enumerate(targets):
                out += [f"\tcmpi.b\t#{hx(t & 0xFF)},{ea(opnd)}", f"\tbne\t{base}_{i}",
                        f"\tcmpi.b\t#{hx((t >> 8) & 0xFF)},{ea(opnd + 1)}", f"\tbne\t{base}_{i}",
                        f"\tlea\t{self.entry_label((t, ctx))},a0", f"\tbra\t{base}_go", f"{base}_{i}:"]
            out += [f"\tpea\t{hx(node[0])}", "\tjmp\trt_trap", f"{base}_go:"]
            out = self.guard(out, live)
            out.append("\tjsr\t(a0)")
            self.continuation(key, p, node, nxt, out)
            return out
        if mode in (O.ABS, O.LONG):
            if not targets:
                return [f"\tpea\t{hx(node[0])}", "\tjmp\trt_trap"]
            t, ctx = targets[0]
            alt = p.conts.get((node, "ramcode"))
            if alt is not None:
                # WRAM still holding the translated bytes: the translation;
                # otherwise the interpreter (analyze: "ramcode" continuation).
                ov = self.prog.overlay_at(t)
                label = f"o{len(self.ovl_checks)}_{t:06X}"
                self.ovl_checks.append(label)
                chunk = ov[1][t - ov[0]:t - ov[0] + 8].ljust(8, b"\0")
                self.tables.append(f"{label}:\tdc.b\t" + ",".join(f"${b:02X}" for b in chunk))
                other = f"{label}_int"
                out = ["\tmove.w\tccr,d5"] + out + [
                    f"\tmove.l\t#{hx(t)},a0", f"\tlea\t{label},a1", "\tjsr\trt_ovlmatch",
                    "\ttst.l\td7", f"\tbne\t{other}", "\tmove.w\td5,ccr",
                    f"\tjsr\t{self.entry_label((t, ctx))}"]
                self.continuation(key, p, node, None, out)     # never falls into the other path
                out += [f"{other}:", "\tmove.w\td5,ccr", f"\tmove.l\t#{hx(t)},a0",
                        f"\tmove.w\t#{int(bool(st.m8)) | 2 * int(bool(st.x8))},d6", "\tjsr\trt_ramcode",
                        f"\tbra\t{self.node_label(key, alt)}"]
                return out
            out = self.guard(out + self.ovl_guard(node[0], t), live)
            out.append(f"\tjsr\t{self.entry_label((t, ctx))}")
            self.continuation(key, p, node, nxt, out)
            return out
        if node in p.dynamic and p.dynamic[node].startswith("table"):
            table = self.dispatch_table(key, p, node, d, targets, calls=True)
            out = self.guard(out + self.table_bound(table, node), live)
            out.append(f"\tjsr\t([{table},d1.l*2])")
            self.continuation(key, p, node, nxt, out)
            return out
        out += self.pointer_target(st, d, node)
        out.append(f"\tmove.w\t#{self.dispatch_ctx(st)},d6")
        out.append("\tjsr\trt_dispatch")
        out = self.guard(out, live)
        out.append("\tjsr\t(a0)")
        self.continuation(key, p, node, nxt, out, assumed=st)
        return out

    def guard(self, body: list[str], live: int) -> list[str]:
        if not live:
            return body
        return ["\tmove.w\tccr,d5"] + body + ["\tmove.w\td5,ccr"]

    def pointer_target(self, st, d, node) -> list[str]:
        """a0 = the 24-bit target of an indirect JMP/JSR."""
        v, mode = d.value, d.mode
        bank = node[0] & 0xFF0000
        if mode == O.ABSIX:
            if v < 0x2000 and ((bank >> 16) & 0x7F) < 0x40:
                # The table address is in the index register (e.g. JSR ($0000,x)
                # over a ROM list): read the pointer through the runtime.
                return [f"\tlea\t({hx(bank | v)},d1.l),a0", "\tjsr\trt_rw",
                        "\tandi.l\t#$FFFF,d6", f"\tori.l\t#{hx(bank)},d6", "\tmovea.l\td6,a0"]
            src = self.host_or_rt(bank | v, "d1", True)
            if src.kind != "host":
                raise Unsupported("indirect jump through I/O")
            return ["\tmoveq\t#0,d6", f"\tmove.w\t{src.ea},d6", "\tror.w\t#8,d6",
                    "\tmovea.l\td6,a0", f"\tadda.l\t#{hx(bank)},a0"]
        if mode == O.ABSI:
            return ["\tmoveq\t#0,d6", f"\tmove.w\t({hx(v)},a5),d6", "\tror.w\t#8,d6",
                    "\tmovea.l\td6,a0", f"\tadda.l\t#{hx(bank)},a0"]
        if mode == O.ABSIL:
            return ["\tmoveq\t#0,d6", f"\tmove.b\t({hx(v + 2)},a5),d6", "\tswap\td6",
                    f"\tmove.w\t({hx(v)},a5),d6", "\tror.w\t#8,d6", "\tmovea.l\td6,a0"]
        raise Unsupported(f"indirect mode {mode}")

    def dispatch_ctx(self, st) -> int:
        # bits 0-2 widths and decimal, bit 3 the interrupt trampolines, bits
        # 4-7 the site's static data bank class (analyze.dbr_class)
        return (1 if st.m8 else 0) | (2 if st.x8 else 0) | (4 if st.d else 0) | \
            ((dbr_class(st.dbr) << 4) if analyze.DISPATCH_BANK else 0)

    def width_split(self, key, p, node, st) -> list[str] | None:
        """analyze.step's branch on the widths of the P byte the last PLP
        pulled from below the procedure's entry (rt_lastp)."""
        edges = {k: dst for k, dst in p.succ.get(node, []) if k.startswith("plpw")}
        if not edges:
            return None
        label = f"w{self.pid[key]}_{self.node_index[key][node]}"
        out = ["\tmove.w\tccr,d5", "\tmoveq\t#0,d7", "\tmove.b\trt_lastp,d7"]
        if st.x8 is not True:
            out += ["\tbtst\t#4,d7", f"\tbeq.s\t{label}_x", "\tandi.w\t#$FF,d1", "\tandi.w\t#$FF,d2",
                    f"{label}_x:"]
        out += (["\tbfextu\td7{26:2},d7"] if "bitfield" not in OFF else
                ["\tlsr.w\t#4,d7", "\tandi.w\t#3,d7"])
        out += ["\tmove.w\td5,ccr", f"\tjmp\t([{label},d7.w*4])"]
        row = []
        for m in (0, 1):
            for x in (0, 1):
                dst = edges.get(f"plpw{m}{x}")
                row.append(self.node_label(key, dst) if dst is not None else "rt_plpwidth")
        self.tables.append(f"{label}:\tdc.l\t" + ",".join(row))
        return out

    def plp_split(self, key, p, node, st, nxt) -> list[str] | None:
        """PLP of a P byte pushed elsewhere: branch on its M and X bits to
        the successor analysed in those widths."""
        edges = {k: dst for k, dst in p.succ.get(node, []) if k.startswith("plp")}
        if not edges:
            return None
        out = ["\tmoveq\t#0,d7", "\tmove.b\t1(a6),d7", "\taddq.l\t#1,a6"]
        out += self.esc_fix(key, p, node, st, 1)
        label = f"s{self.pid[key]}_{self.node_index[key][node]}"
        if st.x8 is not True:
            out += ["\tbtst\t#4,d7", f"\tbeq.s\t{label}_x", "\tandi.w\t#$FF,d1", "\tandi.w\t#$FF,d2",
                    f"{label}_x:"]
        out += (["\tbfextu\td7{26:2},d6"] if "bitfield" not in OFF else
                ["\tmove.w\td7,d6", "\tlsr.w\t#4,d6", "\tandi.w\t#3,d6"])
        out += ["\tbtst\t#6,d7", "\tsne\trt_vf", "\tmove.b\t(rt_p2ccr,d7.w),d7", "\tmove.w\td7,ccr", f"\tjmp\t([{label},d6.w*4])"]
        row = []
        for m in (0, 1):
            for x in (0, 1):
                dst = edges.get(f"plp{m}{x}")
                row.append(self.node_label(key, dst) if dst is not None else "rt_plpwidth")
        self.tables.append(f"{label}:\tdc.l\t" + ",".join(row))
        return out

    def dispatch_table(self, key, p, node, d, targets, calls: bool) -> str:
        label = f"t{self.pid[key]}_{self.node_index[key][node]}"
        bank = node[0] & 0xFF0000
        words = self.prog.table_targets(bank | d.value, bank >> 16) or []
        entries = []
        by_target = {}
        if calls:
            for t, ctx in targets:
                by_target.setdefault(t, self.entry_label((t, ctx)))
        else:
            for dst in self.succ(p, node, "jump"):
                by_target.setdefault(dst[0], self.node_label(key, dst))
        for t in words:
            entries.append(by_target.get(t, "rt_badtable"))
        self.table_len[label] = len(entries)
        self.tables.append(f"{label}:")
        for i in range(0, len(entries), 8):
            self.tables.append("\tdc.l\t" + ",".join(entries[i:i + 8]))
        return label

    def table_bound(self, table, node) -> list[str]:
        """X past the table's end (a command the analysis never saw): stop
        with the site and X instead of jumping through the next table."""
        stub = f"{table}_oob"
        self.tables += [f"{stub}:\tpea\t{hx(node[0])}", "\tjmp\trt_tableoob"]
        return [f"\tcmpi.l\t#{2 * self.table_len[table]},d1", f"\tbcc.l\t{stub}"]

    def table_jump(self, key, p, node, d):
        table = self.dispatch_table(key, p, node, d, [], calls=False)
        return self.table_bound(table, node) + [f"\tjmp\t([{table},d1.l*2])"]

    def dynamic_jump(self, key, p, node, st, d):
        out = self.pointer_target(st, d, node)
        out.append(f"\tmove.w\t#{self.dispatch_ctx(st)},d6")
        out.append("\tjsr\trt_dispatch")
        out = self.guard(out, self.live.get((key, node), ALL))
        out.append("\tjmp\t(a0)")
        return out

    def finish_cont_tables(self) -> None:
        by_id = sorted(self.exit_ids.items(), key=lambda kv: kv[1])
        for label, key, pick in getattr(self, "pending_tables", []):
            row = []
            miss = f"{label}_miss"
            for ex, i in by_id:
                dst = pick(ex)
                row.append(self.node_label(key, dst) if dst is not None else miss)
            self.tables.append(f"{label}:")
            for i in range(0, len(row), 8):
                self.tables.append("\tdc.l\t" + ",".join(row[i:i + 8]))
            # The 65816 call site, for rt_badexit's report.
            site = int(label[1:].split("_")[1])
            node = self.order[key][site]
            self.miss_stubs.append(f"{miss}:\tpea\t{hx(node[0])}")
            self.miss_stubs.append("\tjmp\trt_badexit")

    def finish_dispatch(self) -> None:
        """Runtime dispatch: (context << 24 | address) -> entry, sorted."""
        entries = {}
        for key, p in self.prog.procs.items():
            for node, desc in p.dynamic.items():
                if desc.startswith("table"):
                    continue
                st = self.state_of[(key, node)]
                code = INTERRUPT_DISPATCH if desc.startswith("ramjml") else self.dispatch_ctx(st)
                for t, ctx in p.calls.get(node, []):
                    entries.setdefault((code << 24) | t, self.entry_label((t, ctx)))
        self.tables.append("smkx_dispatch:")
        for k in sorted(entries):
            self.tables.append(f"\tdc.l\t{hx(k)},{entries[k]}")
        self.tables.append(f"smkx_dispatch_count\tequ\t{len(entries)}")


HEADER = ["; Generated by tools/smkx from the pinned SMK disassembly and the",
          "; user's ROM. Do not edit and do not commit.",
          "\tinclude\t'smkx_rt.i'"]


def emit(prog, live_out, probes=frozenset(), marks=None, natives=None) -> tuple[dict, "Emitter"]:
    e = Emitter(prog, live_out)
    e.probes = set(probes)
    e.marks = dict(marks or {})
    e.natives = dict(natives or {})
    e.run()
    n_tables = len(e.tables)
    pending = list(getattr(e, "pending_tables", []))
    files = {}
    bounds = [(c[2], c[3]) for c in e.chunks] + [(n_tables, len(pending))]
    # Resolve continuation tables per chunk now that every exit id exists.
    for i, (name, lines, t0, p0) in enumerate(e.chunks):
        t1, p1 = bounds[i + 1]
        e.pending_tables = pending[p0:p1]
        saved = e.tables
        e.tables = list(saved[t0:t1])
        e.miss_stubs = []
        e.finish_cont_tables()
        body = HEADER + ["\tsection\ttext,code"] + lines + e.miss_stubs + \
            ["\tsection\tdata,data", "\tcnop\t0,4"] + e.tables
        files[name] = "\n".join(body) + "\n"
        e.tables = saved
    e.tables = []
    e.finish_dispatch()
    exports = ["\txdef\tsmkx_reset,smkx_nmi,smkx_irq",
               "\txdef\tsmkx_dispatch,smkx_dispatch_count,smkx_exit_count"]
    tail = HEADER + exports + ["\tsection\ttext,code"] + e.vector_exports() + ["\tsection\tdata,data", "\tcnop\t0,4"] + e.tables
    tail.append(f"smkx_exit_count\tequ\t{len(e.exit_ids)}")
    files["smk_tables.s"] = "\n".join(tail) + "\n"
    return files, e
