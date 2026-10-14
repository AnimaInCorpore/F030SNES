"""Emit vasm (Motorola syntax) 68030 source for the analysed SMK program.

Register mapping
    d0.w  C (A in d0.b, B in bits 8-15)       d1.w  X        d2.w  Y
    d3.l  DBR << 16                           d4/d5 flag save/merge
    d6/d7 scratch, runtime-call value         a0/a1 scratch, runtime-call address
    a3    host address of DBR:$8000           a4    host address of DBR:$0000
    a5    WRAM ($7E0000)                      a6    S, as a host pointer into WRAM
    a7    68k return addresses (a shadow of the 65816 frames)

Flags: 65816 C lives in the 68k X flag, N/Z in the CCR, V in the byte
rt_vf ($FF set, 0 clear): nearly every 68k instruction clears the CCR's V,
while the 65816 changes V only in ADC, SBC, BIT, CLV, PLP, REP/SEP and
RTI, so keeping it in the CCR cost a save around most instructions. A flag is computed
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

    kind "host": ea (byte access) and optionally ea_w (big-endian word view).
    kind "rt":   prep computes the 24-bit SNES address into a0; the runtime
                 performs the access (I/O, DSP-1, SRAM, unmapped, dynamic).
    """

    def __init__(self, kind, ea=None, ea_w=None, prep=None, note=""):
        self.kind, self.ea, self.ea_w, self.prep, self.note = kind, ea, ea_w, prep or [], note


CHUNK_LINES = 12000


class Emitter:
    inline_reads = True                 # rt_rb/rt_rw's page-table path at the call site

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
        self.miss_stubs: list[str] = []
        self.outline: list[str] = []        # out-of-line paths, after the procedure
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
            nxt = nodes[i + 1] if i + 1 < len(nodes) else None
            outline_at = len(self.outline)
            try:
                body = self.emit_node(key, p, node, st, nxt)
            except Unsupported as e:
                self.unsupported.append(f"{p.name} ${node[0]:06X}: {e}")
                body = [f"\tpea\t{hx(node[0])}", "\tjmp\trt_unsupported"]
                del self.outline[outline_at:]
            out.extend(body)
        if not entry_done:
            out.append(f"\txdef\t{self.entry_label(key)}")
            out.append(f"{self.entry_label(key)}:")
            out.append(f"\tpea\t{hx(p.entry)}")
            out.append("\tjmp\trt_trap")
        out.extend(self.outline)
        self.outline = []

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
            self.ccr_saves = getattr(self, "ccr_saves", {})
            self.ccr_saves[save] = self.ccr_saves.get(save, 0) + 1
            if not hasattr(self, "save_by_mnem"):
                import collections
                self.save_by_mnem = collections.Counter()
            self.save_by_mnem[(mn, save)] += 1
        if save:
            out.append("\tmove.w\tccr,d5")
        out.extend(lines)
        if save:
            if need:
                # live flags the instruction leaves alone are still in the CCR
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
            loc = Loc("host", f"(smkx_wram+{hx(wram)}{ix})")
            if idx and bank == 0x7F:
                # base + index may carry out of bank $7F into bank $80 (low
                # RAM, I/O, ROM), past the end of smkx_wram: see cross_split.
                loc.cross = (off, idx, snes)
            return loc
        ri = rom_index(snes)
        if ri is not None:
            ea_w = f"(smkx_romw+{hx(2 * ri)}{',' + idx + '.l*2' if idx else ''})"
            return Loc("host", f"(smkx_rom+{hx(ri)}{ix})", ea_w)
        if idx is None and (bank & 0x7F) < 0x40 and (off < 0x6000 or (bank & 0x3F) < 0x20):
            # A fixed register (or the DSP-1 in banks $00-$1F): direct entry.
            loc = Loc("io", prep=[f"\tmovea.w\t#{hx(off)},a0"], note="io")
            loc.addr = off
            return loc
        prep = [f"\tlea\t({hx(snes)}{ix}),a0"] if idx else [f"\tlea\t{hx(snes)},a0"]
        return Loc("rt", prep=prep, note="io")

    def loc(self, st, d) -> Loc:
        mode, v = d.mode, d.value
        word_access = None
        if mode in (O.DP, O.DPX, O.DPY):
            idx = {O.DP: None, O.DPX: "d1", O.DPY: "d2"}[mode]
            ix = f",{idx}.l" if idx else ""
            return Loc("host", f"({hx(v)},a5{ix})")
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
                loc = Loc("rt", prep=prep, note="indexed-pointer")
                if isinstance(dbr, int):
                    loc.lowfast = (v, idx)
                return loc
            if idx and v >= 0xFF00:
                # base + index carries into the next bank on the 65816: SMK
                # stores its OAM size bits with `STA $FFFF,Y` (data bank
                # $80), i.e. into $81:0400 = low RAM, not into ROM. The
                # 24-bit sum goes to the runtime (found with tools/ref65816.c).
                prep = [f"\tlea\t({hx(v)},{idx}.l),a0"]
                prep.append("\tlea\t(0,a0,d3.l),a0" if not isinstance(dbr, int)
                            else f"\tadda.l\t#{hx(dbr << 16)},a0")
                return Loc("rt", prep=prep, note="bank-crossing")
            if isinstance(dbr, int):
                return self.short_index(st, self.host_or_rt((dbr << 16) | v, idx, False))
            # Bank known only at runtime.
            if v < 0x2000:
                return Loc("host", f"({hx(v)},a4{ix})")
            if v >= 0x8000:
                return Loc("host", f"({hx(v - 0x8000)},a3{ix})")
            prep = [f"\tlea\t({hx(v)}{ix}),a0", "\tlea\t(0,a0,d3.l),a0"] if idx else \
                   [f"\tlea\t{hx(v)}.w,a0", "\tlea\t(0,a0,d3.l),a0"]
            loc = Loc("rt", prep=prep, note="dbr-io")
            if not idx and 0x6000 <= v < 0x8000:
                loc.dsp = v            # the DSP-1 in banks $00-$1F/$80-$9F
            return loc
        if mode in (O.LONG, O.LONGX):
            if mode == O.LONGX and (v & 0xFFFF) < 0x100 and ((v >> 16) & 0x7F) < 0x40:
                # `$bb0000,x`: X holds a full offset in bank bb, which may be
                # ROM ($8000 up) although the base lies in the low-RAM mirror
                # (SMK $85E608 reads its record lists this way).
                loc = Loc("rt", prep=[f"\tlea\t({hx(v)},d1.l),a0"], note="indexed-pointer")
                loc.lowfast = (v & 0xFFFF, "d1")
                return loc
            return self.short_index(st, self.host_or_rt(v, "d1" if mode == O.LONGX else None, False))
        if mode in (O.DPI, O.DPIY, O.DPIX):
            src = f"({hx(v)},a5,d1.l)" if mode == O.DPIX else f"({hx(v)},a5)"
            prep = ["\tmoveq\t#0,d6", f"\tmove.w\t{src},d6", "\tror.w\t#8,d6", "\tmovea.l\td6,a0"]
            prep.append("\tlea\t(0,a0,d3.l),a0" if not isinstance(st.dbr, int)
                        else f"\tadda.l\t#{hx(st.dbr << 16)},a0")
            if mode == O.DPIY:
                prep.append("\tlea\t(0,a0,d2.l),a0")
            return Loc("rt", prep=prep, note="ptr")
        if mode in (O.DPIL, O.DPILY):
            prep = ["\tmoveq\t#0,d6", f"\tmove.b\t({hx(v + 2)},a5),d6", "\tswap\td6",
                    f"\tmove.w\t({hx(v)},a5),d6", "\tror.w\t#8,d6", "\tmovea.l\td6,a0"]
            if mode == O.DPILY:
                prep.append("\tlea\t(0,a0,d2.l),a0")
            return Loc("rt", prep=prep, note="ptr")
        raise Unsupported(f"addressing mode {mode}")

    @staticmethod
    def short_index(st, loc: Loc) -> Loc:
        """An 8-bit index cannot carry a base up to $FEFF out of its bank."""
        cross = getattr(loc, "cross", None)
        if cross is not None and st.x8 is True and cross[0] <= 0xFEFF:
            loc.cross = None
        return loc

    def cross_split(self, loc: Loc, eight: bool, fast: list[str], generic: list[str]) -> list[str]:
        """A bank-$7F access indexed by a 16-bit register: the host access
        while base + index (both bytes of a word) stays in the bank, else the
        runtime with the full 24-bit sum. SMK's $81FCD4 reads its 128x128
        map, `LDA $7F5000,X`, with X = $F216 for a kart off the map's top:
        on the SNES that is $80:4216, the divider's result (found with
        tools/ref65816.c). The runtime path lies out of line, after the
        procedure; cmpi/bhi leave X (the carry)."""
        base, idx, _ = loc.cross
        self.split_n = getattr(self, "split_n", 0) + 1
        g, x = f"xg{self.split_n}", f"xr{self.split_n}"
        limit = (0xFFFF if eight else 0xFFFE) - base
        self.outline += [f"{g}:"] + generic + [f"\tbra\t{x}"]
        return [f"\tcmpi.w\t#{hx(limit)},{idx}", f"\tbhi\t{g}"] + fast + [f"{x}:"]

    @staticmethod
    def cross_parts(loc: Loc) -> tuple[Loc, Loc]:
        _, idx, snes = loc.cross
        return (Loc("host", loc.ea, loc.ea_w),
                Loc("rt", prep=[f"\tlea\t({hx(snes)},{idx}.l),a0"], note="bank-crossing"))

    # Registers the emitter reaches without rt_io_read/rt_io_write (the hot
    # ones of SMK's race, counted with SMKX_IOSTATS). Plain: shadow bytes
    # with no side effect, read or written inline (moves leave X alone).
    PLAIN_W = {0x4202: "rt_io+$2202", 0x4204: "rt_io+$2204", 0x4205: "rt_io+$2205",
               0x4209: "rt_io+$2209", 0x420A: "rt_io+$220A", 0x420C: "rt_io+$220C",
               **{a: f"rt_io+${a - 0x2000:04X}" for a in range(0x4300, 0x4380)}}
    PLAIN_R = {0x4214: "rt_io+$2214", 0x4215: "rt_io+$2215", 0x4216: "rt_io+$2216",
               0x4217: "rt_io+$2217", 0x2140: "rt_apu_out", 0x2141: "rt_apu_out+1",
               0x2142: "rt_apu_out+2", 0x2143: "rt_apu_out+3"}
    CALL_R = {0x4211: "rt_r4211", 0x4212: "rt_r4212"}
    CALL_W = {0x4200: "rt_w4200", 0x420B: "rt_w420b",
              0x4203: "rt_w4203", 0x4206: "rt_w4206", 0x2122: "rt_w2122", 0x2140: "rt_wapu0",
              0x2141: "rt_wapu1", 0x2142: "rt_wapu2", 0x2143: "rt_wapu3"}

    def direct_read(self, a: int, eight: bool, dst: str):
        """Lines loading register a (and a+1) into dst, or None."""
        if 0x6000 <= a < 0x8000:                      # DSP-1: data or status register
            sr = a >= 0x7000
            if eight:
                call = "rt_rdsp_sr" if sr else "rt_rdsp"
            elif (a + 1 >= 0x7000) == sr and a + 1 < 0x8000:
                call = "rt_rdsp_srw" if sr else "rt_rdspw"
            else:
                return None
            lines = [f"\tjsr\t{call}"]
            s = sz(eight)
            lines.append(f"\tmove.{s}\td6,{dst}" if dst != "d6" else f"\ttst.{s}\td6")
            return lines
        if a in self.CALL_R and eight:
            return [f"\tjsr\t{self.CALL_R[a]}", f"\tmove.b\td6,{dst}" if dst != "d6" else "\ttst.b\td6"]
        if a in self.PLAIN_R and (eight or a + 1 in self.PLAIN_R):
            if eight:
                return [f"\tmove.b\t{self.PLAIN_R[a]},{dst}"]
            return [f"\tmove.w\t{self.PLAIN_R[a]},{dst}", f"\tror.w\t#8,{dst}"]
        return None

    def direct_write_byte(self, a: int, src: str) -> list[str] | None:
        """Lines storing src's low byte to register a, or None."""
        if 0x6000 <= a < 0x7000:
            call = "rt_wdsp"
        elif a in self.PLAIN_W:
            return [f"\tmove.b\t{src},{self.PLAIN_W[a]}"]
        elif a in self.CALL_W:
            call = self.CALL_W[a]
        else:
            return None
        return ([] if src == "d6" else [f"\tmove.b\t{src},d6"]) + [f"\tjsr\t{call}"]

    def direct_write(self, a: int, eight: bool, src: str) -> list[str] | None:
        lo = self.direct_write_byte(a, src)
        if lo is None or eight:
            return lo
        if 0x6000 <= a < 0x6FFF:                      # the DSP-1's data register:
            # both bytes in one call (a native command takes the word)
            return ([] if src == "d6" else [f"\tmove.w\t{src},d6"]) + ["\tjsr\trt_wdspw"]
        # 16 bits: the low byte to a, then the high byte to a + 1
        hi = self.direct_write_byte(a + 1, "d6")
        if hi is None:
            hi = [f"\tmovea.w\t#{hx(a + 1)},a0", "\tjsr\trt_iowb"]
        # ror, not lsr: X holds the 65816 carry
        return lo + [f"\tmove.w\t{src},d6", "\tror.w\t#8,d6"] + hi

    def bank_split(self, direct: list[str], generic: list[str]) -> list[str]:
        """direct where the data bank (d3 = bank << 16) is $00-$1F or $80-$9F
        (bits 5 and 6 clear: the DSP-1's banks), generic elsewhere. btst
        changes only Z, so the carry in X survives."""
        self.split_n = getattr(self, "split_n", 0) + 1
        g, x = f".bg{self.split_n}", f".bx{self.split_n}"
        return ["\tbtst\t#21,d3", f"\tbne.s\t{g}", "\tbtst\t#22,d3", f"\tbne.s\t{g}"] + direct + \
               [f"\tbra.s\t{x}", f"{g}:"] + generic + [f"{x}:"]

    def low_split(self, loc: Loc, eight: bool, fast: list[str], generic: list[str]) -> list[str]:
        """An indexed pointer into a low bank (`$00xx,X` with a known data
        bank, `$bb0000,X`): offset base + index in a0; below $2000 (both
        bytes of a word) it is the low-RAM mirror at a5 and fast runs, else
        the runtime resolves it (generic). cmpa/bcc leave X (the carry)."""
        base, idx = loc.lowfast
        self.split_n = getattr(self, "split_n", 0) + 1
        g, x = f".lg{self.split_n}", f".lx{self.split_n}"
        limit = 0x2000 if eight else 0x1FFF
        return [f"\tlea\t({hx(base)},{idx}.l),a0", f"\tcmpa.l\t#{hx(limit)},a0", f"\tbcc.s\t{g}"] + \
            fast + [f"\tbra.s\t{x}", f"{g}:"] + generic + [f"{x}:"]

    def read(self, loc: Loc, eight: bool, dst: str) -> tuple[list[str], bool]:
        """Load into dst's low byte/word. Returns (lines, nz_valid)."""
        s = sz(eight)
        if getattr(loc, "cross", None) is not None:
            host, rt = self.cross_parts(loc)
            fast, nz = self.read(host, eight, dst)
            generic, _ = self.read(rt, eight, dst)
            return self.cross_split(loc, eight, fast, generic), nz
        if loc.kind == "rt" and getattr(loc, "lowfast", None) is not None:
            gen = Loc("rt", prep=loc.prep, note=loc.note)
            generic, _ = self.read(gen, eight, dst)
            if eight:
                fast = [f"\tmove.b\t(0,a5,a0.l),{dst}"]
            else:
                fast = [f"\tmove.w\t(0,a5,a0.l),{dst}", f"\tror.w\t#8,{dst}"]
            return self.low_split(loc, eight, fast, generic), True
        if getattr(loc, "dsp", None) is not None:
            direct = self.direct_read(loc.dsp, eight, dst)
            if direct is not None:
                gen = Loc("rt", prep=loc.prep, note=loc.note)
                generic, _ = self.read(gen, eight, dst)
                return self.bank_split(direct, generic), True
        if loc.kind == "io" and getattr(loc, "addr", None) is not None:
            lines = self.direct_read(loc.addr, eight, dst)
            if lines is not None:
                return lines, True
        if loc.kind == "host":
            if eight:
                return [f"\tmove.b\t{loc.ea},{dst}"], True
            if loc.ea_w:
                return [f"\tmove.w\t{loc.ea_w},{dst}"], True
            return [f"\tmove.w\t{loc.ea},{dst}", f"\tror.w\t#8,{dst}"], True
        if loc.kind == "rt" and self.inline_reads:
            # rt_rb/rt_rw's page-table path inline: the 24-bit address in a0,
            # its page's host address from rt_pages_r (0: I/O), a word within
            # one page. bfextu, cmpa and andi leave X (the carry); d7 and a1
            # are scratch. The rest goes to the runtime as before.
            self.split_n = getattr(self, "split_n", 0) + 1
            g, x = f".pg{self.split_n}", f".px{self.split_n}"
            lines = list(loc.prep) + ["\tmove.l\ta0,d7", "\tbfextu\td7{8:16},d7",
                                      "\tmovea.l\t(rt_pages_r,d7.l*4),a1", "\tcmpa.w\t#0,a1",
                                      f"\tbeq.s\t{g}", "\tmove.l\ta0,d7", "\tandi.l\t#$FF,d7"]
            if eight:
                lines.append(f"\tmove.b\t(a1,d7.l),{dst}")
            else:
                lines += ["\tcmpi.b\t#$FF,d7", f"\tbeq.s\t{g}", f"\tmove.w\t(a1,d7.l),{dst}",
                          f"\tror.w\t#8,{dst}"]
            lines += [f"\tbra.s\t{x}", f"{g}:", f"\tjsr\trt_r{s}"]
            lines.append(f"\tmove.{s}\td6,{dst}" if dst != "d6" else f"\ttst.{s}\td6")
            lines.append(f"{x}:")
            return lines, True
        lines = list(loc.prep) + [f"\tjsr\trt_{'io' if loc.kind == 'io' else ''}r{s}"]
        if dst != "d6":
            lines.append(f"\tmove.{s}\td6,{dst}")
            return lines, True
        lines.append(f"\ttst.{s}\td6")
        return lines, True

    def write(self, loc: Loc, eight: bool, src: str) -> list[str]:
        s = sz(eight)
        if getattr(loc, "cross", None) is not None:
            host, rt = self.cross_parts(loc)
            return self.cross_split(loc, eight, self.write(host, eight, src), self.write(rt, eight, src))
        if loc.kind == "rt" and getattr(loc, "lowfast", None) is not None:
            gen = Loc("rt", prep=loc.prep, note=loc.note)
            generic = self.write(gen, eight, src)
            if eight:
                fast = [f"\tmove.b\t{src},(0,a5,a0.l)"]
            else:
                fast = [f"\tror.w\t#8,{src}", f"\tmove.w\t{src},(0,a5,a0.l)", f"\tror.w\t#8,{src}"]
            return self.low_split(loc, eight, fast, generic)
        if getattr(loc, "dsp", None) is not None:
            direct = self.direct_write(loc.dsp, eight, src)
            if direct is not None:
                gen = Loc("rt", prep=loc.prep, note=loc.note)
                return self.bank_split(direct, self.write(gen, eight, src))
        if loc.kind == "io" and getattr(loc, "addr", None) is not None:
            lines = self.direct_write(loc.addr, eight, src)
            if lines is not None:
                return lines
        if loc.kind == "host":
            if loc.ea_w is not None:
                return ["\tnop\t\t\t\t; write to ROM ignored"]
            if eight:
                return [f"\tmove.b\t{src},{loc.ea}"]
            return [f"\tror.w\t#8,{src}", f"\tmove.w\t{src},{loc.ea}", f"\tror.w\t#8,{src}"]
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
                if loc.kind == "host" and loc.ea_w is None and getattr(loc, "cross", None) is None:
                    return [f"\tclr.{sz(eight)}\t{loc.ea}"], NZ | V, 0
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
        if mn == "TDC":
            return ["\tmoveq\t#0,d7", "\tmove.w\td7,d0"], NZ | V, NZ

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
                if mn == "ADC":
                    return lines + ["\tori\t#$04,ccr", f"\taddx.{s}\td7,d0"] + vout, ALL, ALL
                return lines + ["\teori\t#$10,ccr", "\tori\t#$04,ccr", f"\tsubx.{s}\td7,d0"] + vout + \
                    ["\teori\t#$10,ccr"], ALL, ALL
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
            return ["\tmove.w\t" + f"({hx(v)},a5),d7", "\tsubq.l\t#2,a6", "\tmove.w\td7,1(a6)"], NZ | V, 0
        if mn == "PHD":
            return ["\tsubq.l\t#2,a6", "\tclr.w\t1(a6)"], NZ | V, 0
        if mn == "PLD":
            return ["\taddq.l\t#2,a6", "\ttst.w\t-1(a6)"], NZ | V, NZ
        if mn == "PHP":
            bits = (0x20 if m8 else 0) | (0x10 if x8 else 0) | (0x08 if st.d else 0)
            return ["\tmove.w\tccr,d7", "\tmove.b\t(rt_ccr2p,d7.w),d6", f"\tori.b\t#{hx(bits)},d6",
                    "\tandi.b\t#$BF,d6", "\tmove.b\trt_vf,d4", "\tandi.b\t#$40,d4", "\tor.b\td4,d6",
                    "\tsubq.l\t#1,a6", "\tmove.b\td6,1(a6)", "\tmove.w\td7,ccr"], 0, 0
        if mn == "PLP":
            lines = ["\tmoveq\t#0,d7", "\tmove.b\t1(a6),d7", "\taddq.l\t#1,a6"]
            lines += self.esc_fix(key, p, node, st, 1)
            lines += self.width_fix(key, p, node, st)
            lines += ["\tbtst\t#6,d7", "\tsne\trt_vf",
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
                lines.append("\tsf\trt_vf" if mn == "REP" else "\tst\trt_vf")
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
        if not below:
            return []
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
        if mn in ("JMP", "JML"):
            if mode in (O.ABS, O.LONG):
                out = []
                s = self.succ(p, node, "jump")
                if not s:
                    return [f"\tpea\t{hx(addr)}", "\tjmp\trt_trap"]
                self.goto(key, s[0], nxt, out)
                return out or ["\t; jmp falls through"]
            if node in p.dynamic and p.dynamic[node].startswith("table"):
                return self.table_jump(key, p, node, d, live)
            return self.dynamic_jump(key, p, node, st, d)
        if mn in ("JSR", "JSL"):
            return self.call(key, p, node, st, d, nxt, live)
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

    def exit_ctx_of(self, p, node, st):
        from .analyze import Ctx
        return Ctx(st.m8, st.x8, st.d, st.dbr, st.e)

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
                return (ex.m8, ex.x8, ex.d, ex.e) == (st.m8, st.x8, st.d, st.e) and ex.dbr in (IN, st.dbr)
            self.cont_table(key, node, lambda ex: dst if accept(ex) else None, out)
            return
        dests = set(conts.values())
        if not dests:
            out += [f"\tpea\t{hx(node[0])}", "\tjmp\trt_noreturn"]
            return
        if len(dests) == 1:
            self.goto(key, next(iter(dests)), nxt, out)
            return
        site_dbr = self.state_of[(key, node)].dbr
        def pick(ex, conts=conts, site_dbr=site_dbr):
            if ex in conts:
                return conts[ex]
            if ex.dbr == IN:
                # "Bank unchanged": the bank this call site had, which is how
                # the analysis resolved the exit when it built the continuation.
                from .analyze import Ctx
                here = Ctx(ex.m8, ex.x8, ex.d, site_dbr, ex.e)
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
        out = self.push_return(st, d, node, live)
        mode = d.mode
        targets = p.calls.get(node, [])
        if mode in (O.ABS, O.LONG):
            if not targets:
                return [f"\tpea\t{hx(node[0])}", "\tjmp\trt_trap"]
            t, ctx = targets[0]
            out = self.guard(out, live)
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
            if src.kind != "host" or getattr(src, "cross", None) is not None:
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
        return (1 if st.m8 else 0) | (2 if st.x8 else 0) | (4 if st.d else 0)

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
        with the site and X instead of jumping through the next table (from
        F030CT, where the listing had cut tables short)."""
        stub = f"{table}_oob"
        self.tables += [f"{stub}:\tpea\t{hx(node[0])}", "\tjmp\trt_tableoob"]
        return [f"\tcmpi.l\t#{2 * self.table_len[table]},d1", f"\tbcc.l\t{stub}"]

    def table_jump(self, key, p, node, d, live):
        table = self.dispatch_table(key, p, node, d, [], calls=False)
        # cmpi leaves X (the carry); N/Z/C live into the target are saved
        return self.guard(self.table_bound(table, node), live) + [f"\tjmp\t([{table},d1.l*2])"]

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


def emit(prog, live_out, probes=frozenset(), marks=None) -> tuple[dict, "Emitter"]:
    e = Emitter(prog, live_out)
    e.probes = set(probes)
    e.marks = dict(marks or {})
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
