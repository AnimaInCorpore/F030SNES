"""Recursive-descent 65816 disassembler for the Super Castlevania IV ROM.

    python3 -m tools.sciv_trace "assets/Super Castlevania IV (USA).sfc" build/sciv_trace

There is no source listing for this game, so code is found by tracing from
the vectors, following branches, jumps and calls, with the M/X widths
tracked through REP/SEP/PHP/PLP/XCE. Three kinds of result are kept apart:

  code      reached by a direct path (vectors, JSR/JSL/JMP/JML/branches)
  table     indirect-jump table entries found by a heuristic (reported
            separately: to be confirmed, then moved to a target list)
  problems  places the tracer could not resolve (undetermined immediate
            width, the same address decoded with two lengths, ...)

Writes disasm.txt (a listing with labels and .db runs for the uncovered
bytes), coverage.txt and problems.txt.
"""

from __future__ import annotations

import sys
from collections import defaultdict
from pathlib import Path

from .smkx import opcodes as O

ROM_SIZE = 0x100000


def rom_index(addr: int) -> int | None:
    """LoROM: banks 00-3F/80-BF map $8000-$FFFF; 40-6F/C0-FF are full banks."""
    bank, off = addr >> 16, addr & 0xFFFF
    if bank >= 0x80:
        bank -= 0x80
    if bank < 0x40:
        if off < 0x8000:
            return None
        i = bank * 0x8000 + (off - 0x8000)
    elif 0x40 <= bank < 0x70 and off >= 0x8000:
        i = (bank - 0x40) * 0x8000 + (off - 0x8000)  # mirror of the same banks
    else:
        return None
    return i % ROM_SIZE if i < ROM_SIZE * 2 else None


def canon(addr: int) -> int:
    """One name per ROM byte: bank $80+ becomes $00+ (the game uses both)."""
    return addr & 0x7FFFFF if addr >> 16 >= 0x80 else addr


class Trace:
    """Calls are traced with the caller's M/X/E and the code after the call
    continues with the states the callee returns in, found by running the
    callee: a routine such as `sep #$20 / ... / rts` changes the widths."""

    def __init__(self, rom: bytes, stops=frozenset(), ptrs_in=None):
        self.rom = rom
        self.ptrs_in = ptrs_in or {}                # RAM pointer address -> targets stored by `lda #imm / sta` (previous pass)
        self.ptrs: dict[int, set] = defaultdict(set)
        self.stops = stops                          # other tables' bases: a table ends there
        self.insn: dict[int, tuple] = {}            # canon addr -> (len, mn, mode, opnd, target)
        self.states: dict[int, set] = defaultdict(set)
        self.labels: dict[int, str] = {}
        self.problems: list[str] = []
        self.tables: dict[int, list[int]] = {}      # site -> guessed targets
        self.table_sites: dict[int, int] = {}       # site -> table address
        self.work: list[tuple] = []
        self.summary: dict[tuple, set] = defaultdict(set)   # (entry, m, x, e) -> return states
        self.pending: dict[tuple, list] = defaultdict(list)  # callee key -> continuations
        self.started: set = set()
        self.reason: dict[int, str] = {}

    def byte(self, a: int) -> int | None:
        i = rom_index(a)
        return None if i is None else self.rom[i]

    def label(self, a: int, kind: str) -> None:
        a = canon(a)
        old = self.labels.get(a)
        if old is None or (old.startswith("loc_") and kind == "sub"):
            self.labels[a] = f"{kind}_{a:06X}"

    def push(self, a, m, x, e, ps, why, dbr, ctx) -> None:
        if rom_index(a) is None:
            self.problems.append(f"{why}: target ${a:06X} is not ROM")
            return
        self.reason.setdefault(canon(a), why)
        self.work.append((a, m, x, e, ps, why, dbr, ctx))

    def start(self, a, m, x, e, ps, why, dbr) -> tuple:
        """Begin a routine (call target or vector); returns its context key."""
        key = (canon(a), m, x, e)
        if key not in self.started:
            self.started.add(key)
            self.push(a, m, x, e, ps, why, dbr, key)
        return key

    def call(self, target, nxt, m, x, e, ps, why, dbr, ctx) -> None:
        if rom_index(target) is None:
            self.problems.append(f"{why}: call target ${target:06X} is not ROM")
            self.push(nxt, m, x, e, ps, why, dbr, ctx)
            return
        key = self.start(target, m, x, e, ps, why, dbr)
        cont = (nxt, ps, why, dbr, ctx)
        self.pending[key].append(cont)
        for rm, rx, re_ in list(self.summary[key]):
            self.push(nxt, rm, rx, re_, ps, why, dbr, ctx)

    def returned(self, ctx, m, x, e) -> None:
        if ctx is None or (m, x, e) in self.summary[ctx]:
            return
        self.summary[ctx].add((m, x, e))
        for nxt, ps, why, dbr, cctx in self.pending[ctx]:
            self.push(nxt, m, x, e, ps, why, dbr, cctx)

    def run(self) -> None:
        while self.work:
            self.walk(*self.work.pop())

    def walk(self, a, m, x, e, ps, why, dbr, ctx) -> None:
        carry = None          # known carry from an immediately preceding CLC/SEC
        hist: list = []       # recent (addr, record) on this straight path
        while True:
            key = (m, x, e, ctx)
            ca = canon(a)
            if key in self.states[ca]:
                return
            self.states[ca].add(key)
            op = self.byte(a)
            if op is None:
                self.problems.append(f"${a:06X}: ran off the ROM ({why})")
                return
            mn, mode = O.OPCODES[op]
            n = O.operand_length(mode, m, x)
            if n is None:
                self.problems.append(f"${a:06X}: {mn} immediate width unknown (m={m} x={x}) via {why}")
                return
            raw = [self.byte(a + 1 + i) for i in range(n)]
            if None in raw:
                self.problems.append(f"${a:06X}: operand leaves the ROM")
                return
            v = int.from_bytes(bytes(raw), "little") if n else 0
            length = 1 + n
            target = None
            bank = a & 0xFF0000
            nxt = bank | ((a + length) & 0xFFFF)
            if mode in (O.REL8, O.REL16):
                sign = 0x80 if mode == O.REL8 else 0x8000
                off = v - 2 * sign if v & sign else v
                target = bank | ((a + length + off) & 0xFFFF)
            elif mn in ("JSR", "JMP") and mode == O.ABS:
                target = bank | v
            elif mn in ("JSL", "JML") and mode == O.LONG:
                target = v
            prev = self.insn.get(ca)
            rec = (length, mn, mode, v, target)
            if prev and prev[0] != length:
                self.problems.append(f"${a:06X}: decoded as {prev[1]} (len {prev[0]}) and {mn} (len {length})")
            self.insn[ca] = rec

            carry_now, carry = carry, None
            if mn == "CLC": carry = 0
            elif mn == "SEC": carry = 1
            elif mn == "XCE":
                if carry_now == 0: e = 0
                elif carry_now == 1: e, m, x = 1, 1, 1
                else: e = None; self.problems.append(f"${a:06X}: XCE with unknown carry")
            elif mn == "REP" and e != 1:
                if v & 0x20: m = 0
                if v & 0x10: x = 0
            elif mn == "SEP" and e != 1:
                if v & 0x20: m = 1
                if v & 0x10: x = 1
            elif mn == "PHP": ps = (ps + ((m, x),))[-6:]
            elif mn == "PLP":
                if ps: (m, x), ps = ps[-1], ps[:-1]
                else: m = x = None
            if mn == "PLB":
                p = hist[-1][1] if hist else None
                p2 = hist[-2][1] if len(hist) > 1 else None
                if p and p[1] == "PHK": dbr = bank >> 16
                elif p and p[1] == "PHA" and p2 and p2[1] == "LDA" and p2[2] == O.IMM_M and p2[0] == 2:
                    dbr = p2[3] & 0xFF
                else: dbr = None
            elif mn in ("MVN", "MVP"): dbr = v & 0xFF
            if mn == "STA" and mode in (O.DP, O.ABS) and v < 0x2000 and hist:
                q = hist[-1][1]
                if q[1] == "LDA" and q[2] == O.IMM_M and q[0] == 3 and q[3] >= 0x8000:
                    self.ptrs[v].add(bank | q[3])
            hist = (hist + [(ca, rec)])[-10:]

            if mode in (O.REL8, O.REL16):
                self.label(target, "loc")
                if mn in ("BRA", "BRL"):
                    a = target; why = f"branch from ${ca:06X}"; continue
                self.push(target, m, x, e, ps, f"branch from ${ca:06X}", dbr, ctx)
            elif mn in ("JSR", "JSL"):
                if mode == O.ABSIX:
                    ents = self.indirect(a, v, bank, e, hist, dbr)
                    if ents:
                        for tg in ents:
                            self.label(tg, "sub")
                            self.call(tg, nxt, m, x, e, ps, f"table call at ${ca:06X}", dbr, ctx)
                        return
                else:
                    self.label(target, "sub")
                    self.call(target, nxt, m, x, e, ps, f"call from ${ca:06X}", dbr, ctx)
                    return
            elif mn in ("JMP", "JML"):
                if target is not None:
                    self.label(target, "loc")
                    a = target; why = f"jump from ${ca:06X}"; continue
                for tg in self.indirect(a, v, bank, e, hist, dbr):
                    self.label(tg, "loc")
                    self.push(tg, m, x, e, ps, f"table jump at ${ca:06X}", dbr, ctx)
                return
            elif mn in ("RTS", "RTL"):
                self.returned(ctx, m, x, e)
                return
            elif mn in ("RTI", "BRK", "COP", "STP", "WDM"):
                return
            a = nxt

    def plausible(self, a, m, x) -> bool:
        """Speculative straight-line decode of a guessed table entry."""
        for _ in range(40):
            op = self.byte(a)
            if op is None or op in (0x00, 0x02, 0x42, 0xDB, 0x40):
                return False
            mn, mode = O.OPCODES[op]
            n = O.operand_length(mode, m, x)
            if n is None:
                return True
            raw = [self.byte(a + 1 + i) for i in range(n)]
            if None in raw:
                return False
            v = int.from_bytes(bytes(raw), "little") if n else 0
            bank = a & 0xFF0000
            if mn == "REP": m, x = (0 if v & 0x20 else m), (0 if v & 0x10 else x)
            if mn == "SEP": m, x = (1 if v & 0x20 else m), (1 if v & 0x10 else x)
            if mn in ("JSR", "JMP") and mode == O.ABS and (v < 0x8000 or self.byte(bank | v) is None):
                return False
            if mn in ("JSL", "JML") and mode == O.LONG and rom_index(v) is None:
                return False
            if mn in ("RTS", "RTL", "JMP", "JML", "BRA", "BRL"):
                return True
            a = bank | ((a + 1 + n) & 0xFFFF)
        return True

    def indirect(self, site, v, bank, e, hist, dbr) -> list[int]:
        """Find the table behind an indirect jump/call; return its entries.

        (abs,x): the table is at bank:abs. (abs) / [abs] on a RAM pointer
        (`lda table,x / sta $00 / jmp ($0000)`, with 3-byte entries and a
        second load/store for the bank in the [abs] case): the table is the
        indexed load that fed the store. Lengths are guessed; every guess goes
        to tables.txt for checking.
        """
        site = canon(site)
        op = self.insn[site]
        tab = None
        stride = 2
        if op[2] == O.ABSIX:
            tab = bank | v
        elif op[2] in (O.ABSI, O.ABSIL) and v < 0x2000:
            stride = 2 if op[2] == O.ABSI else 3
            for k in range(len(hist) - 1, -1, -1):
                r = hist[k][1]
                if r[1] == "STA" and r[3] == v and r[2] in (O.DP, O.ABS):
                    for j in range(k - 1, -1, -1):
                        q = hist[j][1]
                        if q[1] == "LDA" and q[2] in (O.LONGX, O.ABSX, O.ABSY):
                            tab = q[3] if q[2] == O.LONGX else (((dbr << 16) if dbr is not None else bank) | q[3])
                            break
                        if q[1] in ("JSR", "JSL", "JMP", "JML"):
                            break
                    break
            if tab is not None and stride == 3:
                # the load of the low word is the one whose store hit v; entries start there
                pass
        if tab is None or (op[2] == O.ABSIX and v < 0x2000):
            if op[2] in (O.ABSIX, O.ABSI) and v < 0x2000 and v in self.ptrs_in:
                ents = sorted(self.ptrs_in[v])       # a pointer in WRAM, set by `lda #routine / sta $xx`
                self.tables[site] = ents
                self.table_sites[site] = v
                return ents
        if tab is None:
            self.problems.append(f"${site:06X}: indirect {op[1]} through ${v:04X}: no table found")
            return []
        self.table_sites[site] = tab
        tb = tab & 0xFF0000
        ents = []
        t = tab
        while len(ents) < 256:
            lo, hi = self.byte(t), self.byte(t + 1)
            if lo is None or hi is None:
                break
            w = lo | hi << 8
            if stride == 3:
                b = self.byte(t + 2)
                if b is None or b >= 0x20:
                    break                            # not a ROM bank of this 1 MiB LoROM
                tgt = (b << 16) | w
            else:
                tgt = (tb if tb else bank) | w
            if w < 0x8000 or not self.plausible(tgt, None if e is None else (1 if e else 0) if False else 0 if e == 0 else 1, 0 if e == 0 else 1):
                break
            if canon(t) in self.insn or (canon(t) in self.labels and t != tab) or (t != tab and canon(t) in self.stops):
                break                                # ran into code or the next table
            ents.append(tgt)
            t += stride
        self.tables[site] = ents
        if not ents:
            self.problems.append(f"${site:06X}: indirect {op[1]} table at ${tab:06X} has no plausible entries")
        return ents


def fmt(a, rec, labels):
    length, mn, mode, v, target = rec
    if mode in (O.REL8, O.REL16, O.ABS) and target is not None and mn != "PEA":
        o = labels.get(canon(target), f"${target:06X}")
    else:
        o = ""
        if mode == O.IMP: o = ""
        elif mode in (O.IMM_M, O.IMM_X, O.IMM8): o = f"#${v:0{2 * (length - 1)}X}"
        elif mode == O.LONG and target is not None: o = labels.get(canon(target), f"${target:06X}")
        elif mode == O.BLK: o = f"${v & 0xFF:02X},${v >> 8:02X}"
        else:
            w = f"${v:0{2 * (length - 1)}X}"
            o = {O.DP: w, O.DPX: w + ",x", O.DPY: w + ",y", O.DPI: f"({w})", O.DPIX: f"({w},x)",
                 O.DPIY: f"({w}),y", O.DPIL: f"[{w}]", O.DPILY: f"[{w}],y", O.ABS: w, O.ABSX: w + ",x",
                 O.ABSY: w + ",y", O.ABSI: f"({w})", O.ABSIX: f"({w},x)", O.ABSIL: f"[{w}]",
                 O.LONG: w, O.LONGX: w + ",x", O.SR: w + ",s", O.SRIY: f"({w},s),y",
                 O.REL8: w, O.REL16: w}.get(mode, w)
    return f"{mn.lower():4} {o}".rstrip()


def run_trace(rom: bytes) -> Trace:
    """Trace from the vectors until the table and pointer findings settle."""
    stops, ptrs = frozenset(), {}
    for _ in range(4):          # later passes know where tables end and which routines RAM pointers get
        t = Trace(rom, stops, ptrs)
        vec = lambda o: rom[0x7F00 + o] | rom[0x7F00 + o + 1] << 8
        # native: COP E4, BRK E6, ABORT E8, NMI EA, IRQ EE; emulation: RESET FC
        for name, off, kind in (("reset", 0xFC, "reset"), ("nmi", 0xEA, "int"), ("irq", 0xEE, "int"),
                                ("cop", 0xE4, "int"), ("brk", 0xE6, "int")):
            w = vec(off)
            t.labels.setdefault(w, f"{name}")
            if kind == "reset":
                t.start(w, 1, 1, 1, (), "reset vector", None)
            else:
                t.start(w, None, None, 0, (), f"{name} vector", None)
        t.run()
        stops = frozenset(canon(a) for a in t.table_sites.values() if a >= 0x2000)
        for k, v in t.ptrs.items():
            ptrs.setdefault(k, set()).update(v)

    return t


def main() -> None:
    rom = Path(sys.argv[1]).read_bytes()
    if len(rom) == ROM_SIZE + 512:
        rom = rom[512:]
    assert len(rom) == ROM_SIZE, len(rom)
    out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
    t = run_trace(rom)

    # listing
    covered = bytearray(ROM_SIZE)
    lines = []
    addrs = sorted(t.insn)
    prev_end = None
    for a in addrs:
        rec = t.insn[a]
        i = rom_index(a)
        for k in range(rec[0]):
            covered[i + k] = 1
    # one linear pass over every ROM byte, bank by bank
    for bank in range(0x20):
        base = bank * 0x8000
        a = bank << 16 | 0x8000
        end = a + 0x8000
        lines.append(f"\n; ---- bank ${bank:02X} (ROM ${base:05X}) ----")
        run = []
        def flush():
            while run:
                chunk, run[:] = run[:16], run[16:]
                lines.append("        .db " + ",".join(f"${b:02X}" for b in chunk))
        while a < end:
            if a in t.insn:
                flush()
                if a in t.labels:
                    lines.append(f"{t.labels[a]}:")
                rec = t.insn[a]
                raw = " ".join(f"{rom[rom_index(a) + k]:02X}" for k in range(rec[0]))
                lines.append(f"  {a:06X}  {raw:12}  {fmt(a, rec, t.labels)}")
                a += rec[0]
            else:
                if a in t.labels:
                    flush(); lines.append(f"{t.labels[a]}:   ; (label inside data)")
                run.append(rom[rom_index(a)])
                a += 1
        flush()
    (out / "disasm.txt").write_text("\n".join(lines) + "\n")
    ncode = sum(covered)
    per = [sum(covered[b * 0x8000:(b + 1) * 0x8000]) for b in range(0x20)]
    cov = [f"code bytes {ncode} of {ROM_SIZE} ({100 * ncode / ROM_SIZE:.1f}%), "
           f"{len(t.insn)} instructions, {len(t.labels)} labels",
           f"indirect tables guessed: {len(t.tables)} ({sum(len(v) for v in t.tables.values())} entries)"]
    cov += [f"bank {b:02X}: {p:5} / 32768 ({100 * p / 32768:5.1f}%)" for b, p in enumerate(per)]
    (out / "coverage.txt").write_text("\n".join(cov) + "\n")
    tl = [f"site ${s:06X} table ${t.table_sites[s]:06X}: " + " ".join(f"${e:06X}" for e in v)
          for s, v in sorted(t.tables.items())]
    (out / "tables.txt").write_text("\n".join(tl) + "\n")
    (out / "reasons.txt").write_text("\n".join(f"${a:06X} {w}" for a, w in sorted(t.reason.items())) + "\n")
    (out / "problems.txt").write_text("\n".join(sorted(set(t.problems))) + "\n")
    print("\n".join(cov[:2]), f"\nproblems: {len(set(t.problems))}")


if __name__ == "__main__":
    main()
