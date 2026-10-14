"""Emit vasm (Motorola syntax) 68030 source for the analysed SMK program.

Register mapping
    d0.w  C (A in d0.b, B in bits 8-15)       d1.w  X        d2.w  Y
    d3.l  DBR << 16                           d4/d5 flag save/merge
    d6/d7 scratch, runtime-call value         a0/a1 scratch, runtime-call address
    a2    host address of D (rt_setdp)        a3    host address of DBR:$8000
    a4    host address of DBR:$0000           a5    WRAM ($7E0000)
    a6    S, as a host pointer into WRAM
    a7    68k return addresses (a shadow of the 65816 frames)

Flags: 65816 C lives in the 68k X flag, N/Z in the CCR and V in the byte
rt_v (0 or $FF): only ADC, SBC, BIT, CLV, REP/SEP and PLP set V and only
BVC/BVS and PHP read it, so no other instruction has to preserve it (in the
CCR, keeping V alive cost half of all nodes a CCR save). A flag is computed
only where liveness says a later instruction can read it, and preserved
across an instruction only where it stays live. SNES memory keeps the ROM's
little-endian layout; 16-bit loads rotate with ror.w #8, which leaves X alone,
and 16-bit ROM loads read the prebuilt big-endian word view instead.

Every node the analysis could not translate, and every control transfer it
could not resolve, becomes a call to rt_trap with the 65816 address, so a
wrong assumption stops the program instead of running on.
"""

from __future__ import annotations

from . import opcodes as O
from .analyze import IN, rom_index
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

    kind "host": ea (byte access); rom marks the ROM image (writes ignored).
    kind "rt":   prep computes the 24-bit SNES address into a0; the runtime
                 performs the access (I/O, DSP-1, SRAM, unmapped, dynamic).
    """

    def __init__(self, kind, ea=None, ea_w=None, prep=None, note="", rom=False):
        self.kind, self.ea, self.ea_w, self.prep, self.note = kind, ea, ea_w, prep or [], note
        self.rom = rom


CHUNK_LINES = 12000
# Debug switch (SMKX_CHECK_STACK=1 in the environment of the translator):
# every call site records S before the call and compares it after the
# return; a difference stops the program at the 65816 site (rt_stackbad).
import os
CHECK_STACK = os.environ.get("SMKX_CHECK_STACK") == "1"


class Emitter:
    def __init__(self, prog, live_out):
        self.prog = prog
        self.live = live_out
        self.lines: list[str] = []
        self.pid: dict = {}
        self.exit_ids: dict = {}
        self.tables: list[str] = []
        self.state_of: dict = {}
        self.unsupported: list[str] = []
        self.probes: set[int] = set()
        self.miss_stubs: list[str] = []
        self.ccr_saves: dict = {}          # flag mask kept across a node -> count
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
        for name, off in (("smkx_reset", 0xFFFC), ("smkx_nmi", 0xFFEA), ("smkx_irq", 0xFFEE)):
            addr = struct.unpack_from("<H", rom, rom_index(off))[0]
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
            nxt = nodes[i + 1] if i + 1 < len(nodes) else None
            try:
                body = self.emit_node(key, p, node, st, nxt)
            except Unsupported as e:
                self.unsupported.append(f"{p.name} ${node[0]:06X}: {e}")
                body = [f"\tpea\t{hx(node[0])}", "\tjmp\trt_unsupported"]
            out.extend(body)
        if not entry_done:
            out.append(f"\txdef\t{self.entry_label(key)}")
            out.append(f"{self.entry_label(key)}:")
            out.append(f"\tpea\t{hx(p.entry)}")
            out.append("\tjmp\trt_trap")

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
        need = flag_defs(mn, d.mode, d.value) & live
        if need & ~produced:
            raise Unsupported(f"{mn} leaves {need & ~produced:#x} unproduced")
        # V lives in rt_v: never merged through the CCR
        vneed = need & V
        need &= ~V
        clob &= ~V
        through = live & ~flag_defs(mn, d.mode, d.value) & ~V
        save = through & clob
        if save:
            self.ccr_saves[save] = self.ccr_saves.get(save, 0) + 1
            out.append("\tmove.w\tccr,d5")
        out.extend(lines)
        if vneed and mn in ("ADC", "SBC", "BIT"):
            out.append("\tsvs\trt_v")
        if save:
            if need:
                # flags the node produced, and live flags it leaves alone
                # (F030CT: the carry kept across DEX/INC was dropped)
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
        # The GSU's RAM: banks $70-$71, and its first 8 KiB at $6000-$7FFF
        # of banks $00-$3F (YI keeps its sprite tables and, with D = $7960,
        # its direct page there).
        if bank in (0x70, 0x71):
            return Loc("host", f"(rt_gsuram+{hx(((bank - 0x70) << 16) | off)}{ix})")
        if (bank & 0x7F) < 0x40 and 0x6000 <= off < 0x8000:
            return Loc("host", f"(rt_gsuram+{hx(off - 0x6000)}{ix})")
        ri = rom_index(snes)
        if ri is not None:
            # YI's 2 MiB ROM has no big-endian word view (it would not fit
            # in 14 MB): 16-bit reads rotate like WRAM reads.
            return Loc("host", f"(smkx_rom+{hx(ri)}{ix})", rom=True)
        if idx is None and (bank & 0x7F) < 0x40 and off < 0x6000:
            # A fixed register: direct entry.
            return Loc("io", prep=[f"\tmovea.w\t#{hx(off)},a0"], note="io")
        prep = [f"\tlea\t({hx(snes)}{ix}),a0"] if idx else [f"\tlea\t{hx(snes)},a0"]
        return Loc("rt", prep=prep, note="io")

    def dp_addr(self, st, v: int, idx: str | None) -> Loc:
        """A direct-page location: bank 0, D + v (+ index)."""
        if isinstance(st.dp, int) and st.dp != IN:
            base = (st.dp + v) & 0xFFFF
            if idx is None or base + 0xFF <= 0xFFFF:
                return self.host_or_rt(base, idx, False)
        # D known only at runtime: a2 is its host address (rt_setdp). YI's
        # direct pages are WRAM ($0000) or the GSU RAM mirror ($7960);
        # I/O pages ($2100, $4300) only occur where D is known.
        ix = f",{idx}.l" if idx else ""
        return Loc("host", f"({hx(v)},a2{ix})")

    def dp_word(self, st, v: int, idx: str | None, dst: str) -> list[str]:
        """dst.w = the little-endian word at the direct-page location."""
        loc = self.dp_addr(st, v, idx)
        if loc.kind == "host":
            return loc.prep + [f"\tmove.w\t{loc.ea},{dst}", f"\tror.w\t#8,{dst}"]
        lines = list(loc.prep) + ["\tjsr\trt_rw"]
        if dst != "d6":
            lines.append(f"\tmove.w\td6,{dst}")
        return lines

    def loc(self, st, d) -> Loc:
        mode, v = d.mode, d.value
        word_access = None
        if mode in (O.DP, O.DPX, O.DPY):
            idx = {O.DP: None, O.DPX: "d1", O.DPY: "d2"}[mode]
            return self.dp_addr(st, v, idx)
        if mode == O.SR:
            return Loc("host", f"({hx(v)},a6)")
        if mode == O.SRIY:
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
            if v >= 0x6000:
                # DBR:$6000-$7FFF: the GSU RAM mirror in the system banks,
                # ROM, GSU RAM or WRAM elsewhere; rt_setdbr keeps its host
                # address in rt_dbr6k.
                return Loc("host", f"({hx(v - 0x6000)},a0{ix})", prep=["\tmovea.l\trt_dbr6k,a0"])
            prep = [f"\tlea\t({hx(v)}{ix}),a0", "\tlea\t(0,a0,d3.l),a0"] if idx else \
                   [f"\tlea\t{hx(v)}.w,a0", "\tlea\t(0,a0,d3.l),a0"]
            return Loc("rt", prep=prep, note="dbr-io")
        if mode in (O.LONG, O.LONGX):
            if mode == O.LONGX and ((v >> 16) & 0x7F) < 0x40 and (v & 0xFFFF) < 0x100:
                # `LDA.l $bb00xx,x` with X holding an address (F030SMK): the
                # bank's low mirror is only the base; resolve at runtime
                return Loc("rt", prep=[f"\tlea\t({hx(v)},d1.l),a0"], note="long-pointer")
            return self.host_or_rt(v, "d1" if mode == O.LONGX else None, False)
        if mode in (O.DPI, O.DPIY, O.DPIX):
            word = self.dp_word(st, v, "d1" if mode == O.DPIX else None, "d6")
            prep = ["\tmoveq\t#0,d6"] + word + ["\tandi.l\t#$FFFF,d6", "\tmovea.l\td6,a0"]
            prep.append("\tlea\t(0,a0,d3.l),a0" if not isinstance(st.dbr, int)
                        else f"\tadda.l\t#{hx(st.dbr << 16)},a0")
            if mode == O.DPIY:
                prep.append("\tlea\t(0,a0,d2.l),a0")
            return Loc("rt", prep=prep, note="ptr")
        if mode in (O.DPIL, O.DPILY):
            bank = self.dp_addr(st, v + 2, None)
            if bank.kind == "host":
                prep = ["\tmoveq\t#0,d6"] + bank.prep + [f"\tmove.b\t{bank.ea},d6"]
            else:
                prep = list(bank.prep) + ["\tjsr\trt_rb", "\tandi.l\t#$FF,d6"]
            prep += ["\tswap\td6", "\tmove.l\td6,d7"] + self.dp_word(st, v, None, "d6") + \
                    ["\tmove.w\td6,d7", "\tmovea.l\td7,a0"]
            if mode == O.DPILY:
                prep.append("\tlea\t(0,a0,d2.l),a0")
            return Loc("rt", prep=prep, note="ptr")
        raise Unsupported(f"addressing mode {mode}")

    def read(self, loc: Loc, eight: bool, dst: str) -> tuple[list[str], bool]:
        """Load into dst's low byte/word. Returns (lines, nz_valid)."""
        s = sz(eight)
        if loc.kind == "host":
            if eight:
                return loc.prep + [f"\tmove.b\t{loc.ea},{dst}"], True
            if loc.ea_w:
                return loc.prep + [f"\tmove.w\t{loc.ea_w},{dst}"], True
            return loc.prep + [f"\tmove.w\t{loc.ea},{dst}", f"\tror.w\t#8,{dst}"], True
        lines = list(loc.prep) + [f"\tjsr\trt_{'io' if loc.kind == 'io' else ''}r{s}"]
        if dst != "d6":
            lines.append(f"\tmove.{s}\td6,{dst}")
            return lines, True
        lines.append(f"\ttst.{s}\td6")
        return lines, True

    def write(self, loc: Loc, eight: bool, src: str) -> list[str]:
        s = sz(eight)
        if loc.kind == "host":
            if loc.rom:
                return ["\tnop\t\t\t\t; write to ROM ignored"]
            if eight:
                return loc.prep + [f"\tmove.b\t{src},{loc.ea}"]
            return loc.prep + [f"\tror.w\t#8,{src}", f"\tmove.w\t{src},{loc.ea}", f"\tror.w\t#8,{src}"]
        lines = list(loc.prep)
        if src != "d6":
            lines.append(f"\tmove.{s}\t{src},d6")
        lines.append(f"\tjsr\trt_{'io' if loc.kind == 'io' else ''}w{s}")
        return lines

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
                    return loc.prep + [f"\tclr.{sz(eight)}\t{loc.ea}"], NZ | V, 0
                # io and rt both go through write()
                return ["\tmoveq\t#0,d6"] + self.write(loc, eight, "d6"), NZ | V, 0
            return self.write(loc, eight, reg[mn[2]]), NZ | V, 0

        if mn in ("TAX", "TAY", "TXA", "TYA", "TXY", "TYX"):
            src, dst = reg[mn[1]], reg[mn[2]]
            eight = need_width(m8 if dst == "d0" else x8, "register")
            return [f"\tmove.{sz(eight)}\t{src},{dst}"], NZ | V, NZ
        if mn == "TCS":
            return ["\tmoveq\t#0,d7", "\tmove.w\td0,d7", "\tlea\t(0,a5,d7.l),a6"], NZ | V, 0
        if mn == "TXS":
            return ["\tlea\t(0,a5,d1.l),a6"], 0, 0
        if mn in ("TSC", "TSX"):
            dst = "d0" if mn == "TSC" else "d1"
            eight = False if mn == "TSC" else need_width(x8, "index")
            return ["\tmove.l\ta6,d7", "\tsub.l\ta5,d7", f"\tmove.{sz(eight)}\td7,{dst}"], ALL, NZ
        if mn == "TCD":
            return ["\tmove.w\td0,rt_dreg", "\tjsr\trt_setdp", "\ttst.w\td0"], NZ | V, NZ
        if mn == "TDC":
            return ["\tmove.w\trt_dreg,d0"], NZ | V, NZ

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
            if not st.d:
                if mn == "ADC":
                    return lines + ["\tori\t#$04,ccr", f"\taddx.{s}\td7,d0"], ALL, ALL
                return lines + ["\teori\t#$10,ccr", "\tori\t#$04,ccr", f"\tsubx.{s}\td7,d0",
                                "\teori\t#$10,ccr"], ALL, ALL
            bcd = "abcd" if mn == "ADC" else "sbcd"
            flip = [] if mn == "ADC" else ["\teori\t#$10,ccr"]
            # V after a decimal ADC/SBC: approximated by the binary-mode
            # overflow of the same operands and carry, computed on copies.
            # Emitted only where liveness says V can be read.
            vpre, vpost = [], []
            if live & V:
                bin_op = "addx" if mn == "ADC" else "subx"
                vpre = ["\tmove.w\tccr,d4", f"\tmove.{s}\td0,d6"] + flip + \
                       ["\tori\t#$04,ccr", f"\t{bin_op}.{s}\td7,d6", "\tmove.w\tccr,d6",
                        "\tmove.w\td4,ccr"]
                vpost = ["\tmove.w\tccr,d4", "\tandi.w\t#$1D,d4", "\tandi.w\t#$02,d6",
                         "\tor.w\td6,d4", "\tmove.w\td4,ccr"]
            if eight:
                body = lines + vpre + flip + ["\tori\t#$04,ccr", f"\t{bcd}\td7,d0"] + flip + ["\ttst.b\td0"]
                return body + vpost, ALL, C | N | Z | (V if vpost else 0)
            body = lines + vpre + flip + ["\tori\t#$04,ccr", f"\t{bcd}\td7,d0", "\tror.w\t#8,d7",
                                          "\tror.w\t#8,d0", f"\t{bcd}\td7,d0", "\tror.w\t#8,d0"] + flip
            return body + vpost, ALL, C | N | Z | (V if vpost else 0)

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
                lines += self.write(loc, eight, "d7")
                if not eight or loc.kind != "host":
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
            lines += self.write(loc, eight, "d7")
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
        if mn in ("PHA", "PHX", "PHY"):
            r = reg[mn[2]]
            eight = need_width(m8 if r == "d0" else x8, "register")
            return self.push_reg(r, eight), NZ | V, 0
        if mn in ("PLA", "PLX", "PLY"):
            r = reg[mn[2]]
            eight = need_width(m8 if r == "d0" else x8, "register")
            return self.pull_reg(r, eight, key, p, node, st), NZ | V, NZ
        if mn == "PHB":
            return ["\tsubq.l\t#1,a6", "\tmove.l\td3,d7", "\tswap\td7", "\tmove.b\td7,1(a6)"], NZ | V, 0
        if mn == "PLB":
            lines = ["\tmove.b\t1(a6),d7", "\taddq.l\t#1,a6"] + self.esc_fix(key, p, node, st, 1)
            lines += ["\tjsr\trt_setdbr", "\ttst.b\td7"]
            return lines, NZ | V, NZ
        if mn == "PHK":
            return ["\tsubq.l\t#1,a6", f"\tmove.b\t#{hx(node[0] >> 16)},1(a6)"], NZ | V, 0
        if mn == "PEA":
            return ["\tsubq.l\t#2,a6", f"\tmove.w\t#{hx(((v & 0xFF) << 8) | (v >> 8))},1(a6)"], NZ | V, 0
        if mn == "PER":
            t = ((node[0] + 3) + (v - 0x10000 if v & 0x8000 else v)) & 0xFFFF
            return ["\tsubq.l\t#2,a6", f"\tmove.w\t#{hx(((t & 0xFF) << 8) | (t >> 8))},1(a6)"], NZ | V, 0
        if mn == "PEI":
            return self.dp_word(st, v, None, "d7") + ["\tror.w\t#8,d7", "\tsubq.l\t#2,a6",
                                                      "\tmove.w\td7,1(a6)"], ALL, 0
        if mn == "PHD":
            return ["\tmove.w\trt_dreg,d7", "\tror.w\t#8,d7", "\tsubq.l\t#2,a6", "\tmove.w\td7,1(a6)"], NZ | V, 0
        if mn == "PLD":
            lines = ["\tmove.w\t1(a6),d7", "\taddq.l\t#2,a6"] + self.esc_fix(key, p, node, st, 2)
            return lines + ["\tror.w\t#8,d7", "\tmove.w\td7,rt_dreg", "\tjsr\trt_setdp", "\ttst.w\td7"], NZ | V, NZ
        if mn == "PHP":
            bits = (0x20 if m8 else 0) | (0x10 if x8 else 0) | (0x08 if st.d else 0)
            return ["\tmove.w\tccr,d7", "\tmove.b\t(rt_ccr2p,d7.w),d6", f"\tori.b\t#{hx(bits)},d6",
                    "\tandi.b\t#$BF,d6", "\tmove.b\trt_v,d4", "\tandi.b\t#$40,d4", "\tor.b\td4,d6",
                    "\tsubq.l\t#1,a6", "\tmove.b\td6,1(a6)", "\tmove.w\td7,ccr"], 0, 0
        if mn == "PLP":
            lines = ["\tmoveq\t#0,d7", "\tmove.b\t1(a6),d7", "\taddq.l\t#1,a6"]
            lines += self.esc_fix(key, p, node, st, 1)
            lines += self.width_fix(key, p, node, st)
            lines += ["\tbtst\t#6,d7", "\tsne\trt_v",
                      "\tmove.b\t(rt_p2ccr,d7.w),d7", "\tmove.w\td7,ccr"]
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
                lines.append("\tsf\trt_v" if mn == "REP" else "\tst\trt_v")
            if mn == "SEP" and v & 0x10 and st.x8 is not True:
                lines += ["\tmove.w\tccr,d7", "\tandi.w\t#$FF,d1", "\tandi.w\t#$FF,d2", "\tmove.w\td7,ccr"]
            if v & 0x04:
                lines.append("\tjsr\t" + ("rt_cli" if mn == "REP" else "rt_sei"))
            return lines, 0, flag_defs(mn, mode, v)
        if mn == "CLC":
            return ["\tandi\t#$EF,ccr"], 0, C
        if mn == "SEC":
            return ["\tori\t#$10,ccr"], 0, C
        if mn == "CLV":
            return ["\tsf\trt_v"], 0, V
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
        if not below:
            return []
        frame = self.frame_size(key)
        before = st.esc
        after = st.esc + below
        if frame is None:
            raise Unsupported("pull below a return frame of unknown size")
        if before < frame <= after:
            if p.unpark:
                # parked: the matching RTS/RTL pushes it back (see analyze)
                return [f"\tmove.l\t(sp)+,{self.park_slot(key)}"]
            return ["\taddq.l\t#4,sp"]
        return []

    def park_slot(self, key) -> str:
        slot = f"q{self.pid[key]}"
        self.park_slots = getattr(self, "park_slots", set())
        self.park_slots.add(slot)
        return slot

    def frame_size(self, key, seen=None):
        """Bytes of the return frame this procedure was entered with: from
        its direct and pointer-dispatch callers, and for a procedure entered
        by a tail jump, from the procedures that jumped to it."""
        seen = set() if seen is None else seen
        if key in seen:
            return 0
        seen.add(key)
        sizes = set()
        prog = self.prog
        for ck, cn, site in list(prog.callers.get(key, ())) + list(prog.dyn_callers.get(key, ())):
            cp = prog.procs[ck]
            mn = cp.insns[site].mnem
            sizes.add(3 if mn == "JSL" else 2)        # JSR, or PER/PEA + JMP (ptr)
        for tkey, _, _ in prog.exit_links.get(key, ()):
            f = self.frame_size(tkey, seen)
            if f:
                sizes.add(f)
        if len(sizes) == 1:
            return sizes.pop()
        if not sizes:
            return 2
        return None

    def width_fix(self, key, p, node, st) -> list[str]:
        """PLP switching the index registers to 8 bits clears their high bytes."""
        out = []
        for kind, dst in p.succ.get(node, []):
            nst = self.state_of.get((key, dst))
            if nst is not None and nst.x8 and st.x8 is not True:
                out = ["\tandi.w\t#$FF,d1", "\tandi.w\t#$FF,d2"]
        return out

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
        if mn in ("BVC", "BVS"):
            # V is rt_v; N and Z may be live across the branch
            taken = self.succ(p, node, "branch")
            fall = self.succ(p, node, "fall")
            op = "bne" if mn == "BVS" else "beq"
            if not live & (N | Z):
                out = ["\ttst.b\trt_v"]
                if taken:
                    out.append(f"\t{op}\t{self.node_label(key, taken[0])}")
                self.goto(key, fall[0] if fall else None, nxt, out)
                return out
            tl = f".v{self.pid[key]}_{self.node_index[key][node]}"
            out = ["\tmove.w\tccr,d7", "\ttst.b\trt_v", f"\t{op}.s\t{tl}", "\tmove.w\td7,ccr"]
            if fall:
                out.append(f"\tbra\t{self.node_label(key, fall[0])}")
            else:
                out += [f"\tpea\t{hx(node[0])}", "\tjmp\trt_trap"]
            out += [f"{tl}:", "\tmove.w\td7,ccr"]
            if taken:
                out.append(f"\tbra\t{self.node_label(key, taken[0])}")
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
        if mn in ("JMP", "JML"):
            if mode in (O.ABS, O.LONG):
                out = []
                s = self.succ(p, node, "jump")
                if not s:
                    return [f"\tpea\t{hx(addr)}", "\tjmp\trt_trap"]
                self.goto(key, s[0], nxt, out)
                return out or ["\t; jmp falls through"]
            if node in p.dynamic and p.dynamic[node].startswith("table"):
                return self.table_jump(key, p, node, d)
            if node in p.ret_at:
                return self.call(key, p, node, st, d, nxt, live)      # PER/PEA pushed the return
            return self.dynamic_jump(key, p, node, st, d)
        if mn in ("JSL", "JML") and p.dynamic.get(node, "").startswith("execptr"):
            return self.execptr(key, p, node, st, d)
        if mn == "JSL" and p.dynamic.get(node, "").startswith("oamreset"):
            _, k, lda = p.dynamic[node].split()
            out = (["\tmove.b\t#$F0,d0"] if lda == "1" else []) + [f"\tmoveq\t#{k},d7", "\tjsr\trt_oamreset"]
            out = self.guard(out, live)
            falls = self.succ(p, node, "fall")
            self.goto(key, falls[0] if falls else None, nxt, out)
            return out
        if mn in ("JSR", "JSL"):
            return self.call(key, p, node, st, d, nxt, live)
        if mn in ("RTS", "RTL") and p.dynamic.get(node, "").startswith("stackjump"):
            return self.stack_jump(key, p, node, st, mn)
        if mn in ("RTS", "RTL", "RTI"):
            out = []
            if node in p.unpark:
                frame = self.frame_size(key)
                if frame is not None and st.esc >= frame:
                    out.append(f"\tmove.l\t{self.park_slot(key)},-(sp)")
                st = type(st)(st.m8, st.x8, st.d, st.dbr, st.e, (), 0, st.dp)
            ex = self.exit_ctx_of(p, node, st)
            out.append(f"\tlea\t{self.exit_id(ex)}.w,a0")
            n = {"RTS": 2, "RTL": 3, "RTI": 4}[mn]
            out.append(f"\taddq.l\t#{n},a6")
            out.append("\trts")
            return out
        if mn in ("BRK", "COP", "STP", "WDM", "WAI"):
            return [f"\tpea\t{hx(addr)}", "\tjmp\trt_trap"]
        return None

    def exit_ctx_of(self, p, node, st):
        from .analyze import Ctx
        return Ctx(st.m8, st.x8, st.d, st.dbr, st.e, st.dp)

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
        conts = {ex: dst for (n, ex), dst in p.conts.items() if n == node and ex != "assumed"}
        if assumed is not None:
            dst = p.conts.get((node, "assumed"))
            if dst is None:
                out += [f"\tpea\t{hx(node[0])}", "\tjmp\trt_noreturn"]
                return
            st = assumed
            def accept(ex, st=st):
                return (ex.m8, ex.x8, ex.d, ex.e) == (st.m8, st.x8, st.d, st.e) and ex.dbr in (IN, st.dbr) \
                    and ex.dp in (IN, st.dp)
            self.cont_table(key, node, lambda ex: dst if accept(ex) else None, out)
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
                # "Bank/D unchanged": what this call site had, which is how
                # the analysis resolved the exit when it built the continuation.
                from .analyze import Ctx
                dbr = site.dbr if ex.dbr == IN else ex.dbr
                dp = site.dp if ex.dp == IN else ex.dp
                for here in (Ctx(ex.m8, ex.x8, ex.d, dbr, ex.e, dp), Ctx(ex.m8, ex.x8, ex.d, dbr, ex.e, ex.dp),
                             Ctx(ex.m8, ex.x8, ex.d, ex.dbr, ex.e, dp)):
                    if here in conts:
                        return conts[here]
                same = {d for c, d in conts.items() if (c.m8, c.x8, c.d, c.e) == (ex.m8, ex.x8, ex.d, ex.e)}
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
        out = [] if node in p.ret_at else self.push_return(st, d, node, live)
        mode = d.mode
        targets = p.calls.get(node, [])
        if mode in (O.ABS, O.LONG):
            if not targets:
                return [f"\tpea\t{hx(node[0])}", "\tjmp\trt_trap"]
            t, ctx = targets[0]
            out = self.guard(out, live)
            out.append(f"\tjsr\t{self.entry_label((t, ctx))}")
            out = self.stack_check(key, node, out)
            self.continuation(key, p, node, nxt, out)
            return out
        if node in p.dynamic and p.dynamic[node].startswith("table"):
            table = self.dispatch_table(key, p, node, d, targets, calls=True)
            out = self.guard(out, live)
            out.append(f"\tjsr\t([{table},d1.l*4])")
            out = self.stack_check(key, node, out)
            self.continuation(key, p, node, nxt, out)
            return out
        out += self.pointer_target(st, d, node)
        out.append(f"\tmove.w\t#{self.dispatch_ctx(st)},d6")
        out.append("\tjsr\trt_dispatch")
        out = self.guard(out, live)
        out.append("\tjsr\t(a0)")
        out = self.stack_check(key, node, out)
        self.continuation(key, p, node, nxt, out, assumed=st)
        return out

    def stack_check(self, key, node, out: list[str]) -> list[str]:
        """With CHECK_STACK: S before the call into a slot of this site,
        compared after the return (flags and a0 kept)."""
        if not CHECK_STACK or node in self.prog.procs[key].ret_at:
            return out
        slot = f"k{self.pid[key]}_{self.node_index[key][node]}"
        self.check_slots = getattr(self, "check_slots", [])
        self.check_slots.append(slot)
        i = next(i for i, l in enumerate(out) if l.startswith("\tjsr\t") and "rt_dispatch" not in l)
        # S before the return address is pushed (the site's first line)
        return ([f"\tmove.l\ta6,{slot}"] + out[:i] + [out[i]] +
                ["\tmove.w\tccr,-(sp)", f"\tcmpa.l\t{slot},a6", f"\tbeq.s\t{slot}_ok",
                 f"\tpea\t{hx(node[0])}", "\tjmp\trt_stackbad", f"{slot}_ok:", "\tmove.w\t(sp)+,ccr"] +
                out[i + 1:])

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
            src = self.host_or_rt(v, None, True)
            if src.kind != "host":
                raise Unsupported("indirect jump through I/O")
            return ["\tmoveq\t#0,d6", f"\tmove.w\t{src.ea},d6", "\tror.w\t#8,d6",
                    "\tmovea.l\td6,a0", f"\tadda.l\t#{hx(bank)},a0"]
        if mode == O.ABSIL:
            src = self.host_or_rt(v, None, True)
            hi = self.host_or_rt(v + 2, None, True)
            if src.kind != "host" or hi.kind != "host":
                raise Unsupported("indirect jump through I/O")
            return ["\tmoveq\t#0,d6", f"\tmove.b\t{hi.ea},d6", "\tswap\td6",
                    f"\tmove.w\t{src.ea},d6", "\tror.w\t#8,d6", "\tmovea.l\td6,a0"]
        raise Unsupported(f"indirect mode {mode}")

    def dispatch_ctx(self, st) -> int:
        return (1 if st.m8 else 0) | (2 if st.x8 else 0) | (4 if st.d else 0)

    def dispatch_table(self, key, p, node, d, targets, calls: bool) -> str:
        label = f"t{self.pid[key]}_{self.node_index[key][node]}"
        bank = node[0] & 0xFF0000
        words = self.prog.table_at.get(node[0]) or self.prog.table_targets(bank | d.value, bank >> 16) or []
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
        self.tables.append(f"{label}:")
        for i in range(0, len(entries), 8):
            self.tables.append("\tdc.l\t" + ",".join(entries[i:i + 8]))
        return label

    def execptr(self, key, p, node, st, d):
        """`JSL ExecutePtr` / `JSL ExecutePtrLong` with the table after it:
        what the routine leaves behind, then a jump to the entry A selects.

        ExecutePtr: $00-$01 = entry, $02 = the site's bank, $03 = Y; C = the
        entry (B its high byte). ExecutePtrLong: $00-$02 = entry, $02-$04
        the table pointer's high bytes, $05 = Y; C = the entry's bank and
        high byte. X and Y keep their values, the widths end at 8 bits,
        carry and overflow clear, N/Z from Y."""
        addr = node[0]
        unit = int(p.dynamic[node][7])
        table_addr = addr + d.length
        words = self.prog.execptr_targets(addr, d.length, unit) or []
        by_target = {}
        for dst in self.succ(p, node, "jump"):
            by_target.setdefault(dst[0], self.node_label(key, dst))
        label = f"t{self.pid[key]}_{self.node_index[key][node]}"
        self.tables.append(f"{label}:")
        entries = [by_target.get(t, "rt_badtable") for t in words]
        for i in range(0, len(entries), 8):
            self.tables.append("\tdc.l\t" + ",".join(entries[i:i + 8]))
        ri = rom_index(table_addr)
        ret = addr + 3                      # the return address JSL (or PER) pushes
        n = len(words)
        out = ["\taddq.l\t#3,a6"] if d.mnem == "JML" else []    # PHK/PER's bytes
        out += ["\tmoveq\t#0,d7", "\tmove.b\td0,d7"]
        out += [f"\tcmpi.w\t#{n},d7", f"\tbcc\t{label}_bad"]

        def dp(k):
            loc = self.dp_addr(st, k, None)
            if loc.kind != "host" or loc.prep:
                raise Unsupported("ExecutePtr with a direct page outside host memory")
            return loc.ea
        # entries are little-endian in the ROM image (no word view)
        if unit == 2:
            out += [f"\tmove.w\t(smkx_rom+{hx(ri)},d7.l*2),d0", "\tror.w\t#8,d0",   # entry k: rom[T+2k]
                    "\tmove.w\td0,d6", "\tror.w\t#8,d6", f"\tmove.w\td6,{dp(0)}",
                    f"\tmove.b\t#{hx(addr >> 16)},{dp(2)}", f"\tmove.b\td2,{dp(3)}"]
        else:
            out += ["\tmove.l\td7,d6", "\tadd.l\td6,d6", "\tadd.l\td7,d6",   # 3k
                    f"\tmove.w\t(smkx_rom+{hx(ri)},d6.l),d0",                     # lo:mid (LE)
                    f"\tmove.w\td0,{dp(0)}",
                    f"\tmove.w\t(smkx_rom+{hx(ri + 1)},d6.l),d0",                 # mid << 8 | bank
                    f"\tmove.b\td0,{dp(2)}",
                    "\tror.w\t#8,d0",                                              # C = bank << 8 | mid
                    f"\tmove.b\t#{hx((ret >> 8) & 0xFF)},{dp(3)}", f"\tmove.b\t#{hx(addr >> 16)},{dp(4)}",
                    f"\tmove.b\td2,{dp(5)}"]
        out += ["\tandi.w\t#$FF,d1", "\tandi.w\t#$FF,d2",
                "\tandi\t#$EC,ccr", "\ttst.b\td2", f"\tjmp\t([{label},d7.l*4])"]
        out += [f"{label}_bad:", "\tmove.l\td7,d1", f"\tpea\t{hx(addr)}", "\tjmp\trt_badtable"]
        return out

    def stack_jump(self, key, p, node, st, mn):
        """RTS/RTL to a table entry the routine pushed (jumps.txt): the
        target is read back from the 65816 stack and looked up in a table
        of this site's destinations (rt_stackjump)."""
        label = f"s{self.pid[key]}_{self.node_index[key][node]}"
        dests = {}
        for dst in self.succ(p, node, "jump"):
            dests.setdefault(dst[0], self.node_label(key, dst))
        self.tables.append(f"{label}:")
        self.tables.append(f"\tdc.w\t{len(dests)}")
        for t in sorted(dests):
            self.tables.append(f"\tdc.l\t{hx(t)},{dests[t]}")
        n = 3 if mn == "RTL" else 2
        out = ["\tmove.w\tccr,d5"]
        if mn == "RTL":
            out += ["\tmoveq\t#0,d6", "\tmove.b\t3(a6),d6", "\tswap\td6"]
        else:
            out += [f"\tmove.l\t#{hx(node[0] & 0xFF0000)},d6"]
        out += ["\tmove.w\t1(a6),d7", "\tror.w\t#8,d7", "\taddq.w\t#1,d7", "\tmove.w\td7,d6",
                f"\taddq.l\t#{n},a6", f"\tlea\t{label},a0", f"\tpea\t{hx(node[0])}", "\tjmp\trt_stackjump"]
        return out

    def table_jump(self, key, p, node, d):
        table = self.dispatch_table(key, p, node, d, [], calls=False)
        return [f"\tjmp\t([{table},d1.l*4])"]

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
                code = self.dispatch_ctx(st)
                for t, ctx in p.calls.get(node, []):
                    entries.setdefault((code << 24) | t, self.entry_label((t, ctx)))
        self.tables.append("smkx_dispatch:")
        for k in sorted(entries):
            self.tables.append(f"\tdc.l\t{hx(k)},{entries[k]}")
        self.tables.append(f"smkx_dispatch_count\tequ\t{len(entries)}")


HEADER = ["; Generated by tools/smkx from the pinned SMK disassembly and the",
          "; user's ROM. Do not edit and do not commit.",
          "\tinclude\t'smkx_rt.i'"]


def emit(prog, live_out, probes=frozenset()) -> tuple[dict, "Emitter"]:
    e = Emitter(prog, live_out)
    e.probes = set(probes)
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
        slots = [l.split(",")[1] for l in lines if l.startswith("\tmove.l\ta6,k")]
        slots += sorted({l.split(",")[1] for l in lines if l.startswith("\tmove.l\t(sp)+,q")})
        body = HEADER + ["\tsection\ttext,code"] + lines + e.miss_stubs + \
            ["\tsection\tdata,data", "\tcnop\t0,4"] + e.tables + \
            (["\tsection\tbss,bss", "\tcnop\t0,4"] + [f"{x}:\tds.l\t1" for x in slots] if slots else [])
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
