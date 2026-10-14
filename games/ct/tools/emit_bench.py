"""A bench for the translator's emitted code: assemble it, run it on a 68030
model and compare the result with a 65816 reference.

    python3 -m tools.emit_bench            # every test
    python3 -m tools.emit_bench -k adc     # tests whose name contains "adc"

It needs vasm (`vasmm68k_mot`, found through $VASM or ../../../F030Arcade/third_party
or $PATH) and the Musashi binding `pip install machine68k`. Nothing here is
the Falcon: the model executes 68030 instructions and counts cycles the way
Musashi does (no cache, no bus wait states), which is enough to check that a
sequence computes what the 65816 instruction does, and to compare two
sequences by instruction and cycle count, not to time them.

`Bench.emit()` runs the real `tools.smkx.emit.Emitter` over a straight-line
procedure built from (mnemonic, mode, operand) triples, with the flag
liveness of each instruction chosen by the caller, so every combination of
live flags can be tried against the reference.
"""

from __future__ import annotations

import itertools
import os
import random
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from tools.smkx import opcodes as O
from tools.smkx.analyze import Decoded, Proc, State, Ctx
from tools.smkx.emit import Emitter
from tools.smkx.liveness import C, Z, N, V

ROOT = Path(__file__).resolve().parents[1]

# The bench's memory map.
CODE = 0x10000
STACK = 0x0F000
WRAM = 0x100000          # a5: the 65816's WRAM ($7E0000-$7FFFFF, 128 KiB)
ROMB = 0x200000          # smkx_rom
RTIO = 0x700000          # rt_io: shadow of $2000-$5FFF (16 KiB)
RTVF = 0x704000          # rt_vf
RTLOG = 0x704004         # rt_logging
RTTRASH = 0x705000       # rt_trash (256 bytes)
PAGR = 0x710000          # biased read page table (see emit.py)
PAGW = 0x790000          # biased write page table
HOST = 0x800000          # host pages for the page-table tests
RAM_KIB = 12 * 1024

SYMBOLS = {
    "smkx_wram": WRAM, "smkx_rom": ROMB, "rt_io": RTIO - 0x2000 + 0x2000, "rt_vf": RTVF,
    "rt_logging": RTLOG, "rt_trash": RTTRASH, "rt_pages_r": PAGR, "rt_pages_w": PAGW,
    "rt_dbr": RTLOG + 2, "rt_lotab": RTLOG + 0x400, "rt_hitab": RTLOG + 0x800, "rt_nmitimen": RTLOG + 1, "rt_slow_n": RTLOG + 0x100, "rt_slow_a0": RTLOG + 0x104,
    "rt_slow_v": RTLOG + 0x108, "rt_slow_reg": RTLOG + 0x10C,
}


def find_vasm() -> str:
    for cand in (os.environ.get("VASM"), ROOT.parents[2] / "F030Arcade/third_party/vasm/vasmm68k_mot",
                 shutil.which("vasmm68k_mot")):
        if cand and Path(cand).exists():
            return str(cand)
    scratch = os.environ.get("VASM_DIR")
    if scratch and (Path(scratch) / "vasmm68k_mot").exists():
        return str(Path(scratch) / "vasmm68k_mot")
    raise SystemExit("vasmm68k_mot not found: set $VASM")


# --- the reference 65816 ---------------------------------------------------------------

def adc(a: int, m: int, c: int, eight: bool):
    """(result, C, V, N, Z) of a binary ADC."""
    bits, mask = (8, 0xFF) if eight else (16, 0xFFFF)
    a &= mask; m &= mask
    r = a + m + c
    res = r & mask
    v = (~(a ^ m) & (a ^ res)) >> (bits - 1) & 1
    return res, r >> bits & 1, v, res >> (bits - 1) & 1, int(res == 0)


def sbc(a: int, m: int, c: int, eight: bool):
    bits, mask = (8, 0xFF) if eight else (16, 0xFFFF)
    return adc(a, ~m & mask, c, eight)


@dataclass
class Result:
    d: list                # d0-d7
    a: list                # a0-a7
    ccr: int
    vf: int
    mem: dict = field(default_factory=dict)
    cycles: int = 0
    insns: int = 0

    @property
    def c(self) -> int: return self.ccr >> 4 & 1
    @property
    def z(self) -> int: return self.ccr >> 2 & 1
    @property
    def n(self) -> int: return self.ccr >> 3 & 1
    @property
    def v(self) -> int: return int(self.vf != 0)


class Bench:
    def __init__(self):
        from machine68k import CPUType, Machine
        self.vasm = find_vasm()
        self.machine = Machine(CPUType.M68030, RAM_KIB)
        self.cpu, self.mem = self.machine.cpu, self.machine.mem
        self.tmp = Path(tempfile.mkdtemp(prefix="emitbench"))

    # -- assembling and running -------------------------------------------------------
    def assemble(self, lines: list[str], extra: str = "") -> bytes:
        equ = "".join(f"{k}\tequ\t${v:X}\n" for k, v in SYMBOLS.items())
        # runtime stand-ins first (behind a jump), the code under test last, so
        # the run ends at the final `bra.s` and no branch has far to go
        src = f"\torg\t${CODE:X}\n{equ}\tbra\tbench_start\n{extra}\n\teven\nbench_start:\n"
        src += "\n".join(lines) + "\n\tbra.s\tbench_end\nbench_end:\tbra.s\tbench_end\n"
        p = self.tmp / "t.s"
        p.write_text(src)
        out = self.tmp / "t.bin"
        r = subprocess.run([self.vasm, "-Fbin", "-m68030", "-quiet", "-nowarn=62", "-o", str(out), str(p)],
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr + "\n" + src)
        return out.read_bytes()

    def run(self, lines: list[str], regs: dict | None = None, mem: dict | None = None,
            extra: str = "", limit: int = 20000, watch: tuple = (), pages=None) -> Result:
        from machine68k import Register as R
        code = self.assemble(lines, extra)
        end = CODE + len(code) - 2                       # the closing `bra.s`
        self.mem.clear_block(0, 0x1000, 0)
        for base, size in ((WRAM, 0x20000), (RTIO, 0x4200)):
            self.mem.clear_block(base, size, 0)
        if pages is not None and getattr(self, "pagemap", None) is not pages:
            pages.build(self.mem)             # the page tables persist between runs
            self.pagemap = pages
        self.mem.w_block(CODE, code)
        for a, b in (mem or {}).items():
            self.mem.w8(a, b & 0xFF)
        self.cpu.pulse_reset()
        self.cpu.w_reg(R.ISP, STACK)
        self.cpu.w_reg(R.A7, STACK)
        self.cpu.w_sr(0x2700)
        for i in range(8):
            self.cpu.w_reg(getattr(R, f"D{i}"), 0)
            self.cpu.w_reg(getattr(R, f"A{i}"), 0) if i < 7 else None
        self.cpu.w_reg(R.A5, WRAM)
        for k, v in (regs or {}).items():
            if k == "ccr":
                self.cpu.w_sr(0x2700 | v)
            elif k == "vf":
                self.mem.w8(RTVF, v)
            else:
                self.cpu.w_reg(getattr(R, k.upper()), v & 0xFFFFFFFF)
        self.cpu.w_pc(CODE)
        cycles = insns = 0
        while self.cpu.r_pc() != end:
            cycles += self.cpu.execute(1).cycles
            insns += 1
            if insns > limit:
                raise RuntimeError(f"runaway at pc {self.cpu.r_pc():X}")
        res = Result([self.cpu.r_reg(getattr(R, f"D{i}")) for i in range(8)],
                     [self.cpu.r_reg(getattr(R, f"A{i}")) for i in range(8)],
                     self.cpu.r_sr() & 0x1F, self.mem.r8(RTVF), cycles=cycles, insns=insns)
        for a in watch:
            res.mem[a] = self.mem.r8(a)
        return res

    # -- the real emitter over a straight-line procedure ---------------------------------
    def emit(self, insns: list[tuple], lives: list[int] | None = None, m8=True, x8=True, dbr=0,
             dp=0, natural: bool = False) -> list[str]:
        """Translate insns ((mnemonic, mode, value) each) with the emitter and
        return the lines of the procedure body, with the entry and node
        labels left in. lives[i] is the flag mask live after insns[i]."""
        addr = 0xC00000
        ctx = Ctx(m8, x8, False, dbr, False, dp)
        p = Proc(entry=addr, ctx=ctx)
        nodes = []
        for i, (mn, mode, val) in enumerate(insns):
            length = 1 + O.FIXED_LEN.get(mode, 0) if mode not in (O.IMM_M, O.IMM_X) else \
                1 + (1 if (m8 if mode == O.IMM_M else x8) else 2)
            raw = bytes(length)
            d = Decoded(addr, mn, mode, length, val, raw)
            st = State(m8, x8, False, dbr, False, (), 0, dp)
            p.states[addr] = [st]
            p.insns[addr] = d
            nodes.append((addr, st.key()))
            addr += length
        for a, b in zip(nodes, nodes[1:]):
            p.succ[a] = [("fall", b)]
        key = (0xC00000, ctx)

        class FakeProg:
            procs = {key: p}
            callers = {}
            rom = bytes(0x10000)

            def decode(self, a, mm8, xx8):
                return p.insns[a]

            def overlay_at(self, a):
                return None

        prog = FakeProg()
        live = {}
        for i, n in enumerate(nodes):
            live[(key, n)] = (lives[i] if lives else V | C | Z | N)
        e = Emitter(prog, live)
        e.run()
        self.emitter = e
        lines = []
        for _, chunk, _, _ in e.chunks:
            lines += chunk
        lines += e.miss_stubs
        return [l for l in lines if not l.startswith(";")]



def _tables() -> str:
    from tools.smkx.__main__ import flag_tables
    text = flag_tables().split("\txdef\tdsp1_sine")[0]
    return "".join(l + "\n" for l in text.splitlines()
                   if not l.startswith(("\tsection", "\txdef", ";")))


# rt_trap for the emitter's dead ends (the entry of a routine, a fall-off at the
# last instruction): the bench just ends there. The CCR/P tables come with it.
TRAPS = "rt_trap:\tbra\tbench_end\n\teven\n" + _tables()


# The runtime's generic helpers as the bench models them. Each keeps every
# register and the CCR but d6 (the result, as rt_rb does), counts the call in
# rt_slow_n and records what it was asked: a byte read answers
# (address & $FF) ^ $A5, a word read the two bytes at a and a + 1 (low
# first); a write leaves the value in rt_slow_v. The register helpers work on
# the shadow array (rt_io) like rt_io_write does.
SLOW = """
rt_rb:	move.w	ccr,-(sp)
	move.l	a0,rt_slow_a0
	addq.l	#1,rt_slow_n
	move.l	a0,d6
	andi.l	#$ff,d6
	eori.b	#$a5,d6
	move.w	(sp)+,ccr
	rts
rt_rw:	move.w	ccr,-(sp)
	move.l	a0,rt_slow_a0
	addq.l	#1,rt_slow_n
	move.l	a0,d6
	addq.b	#1,d6
	eori.b	#$a5,d6
	lsl.w	#8,d6
	move.l	d7,-(sp)
	move.l	a0,d7
	eori.b	#$a5,d7
	move.b	d7,d6
	move.l	(sp)+,d7
	move.w	(sp)+,ccr
	rts
rt_wb:	move.w	ccr,-(sp)
	move.l	a0,rt_slow_a0
	addq.l	#1,rt_slow_n
	move.b	d6,rt_slow_v
	move.w	(sp)+,ccr
	rts
rt_ww:	move.w	ccr,-(sp)
	move.l	a0,rt_slow_a0
	addq.l	#1,rt_slow_n
	move.w	d6,rt_slow_v
	move.w	(sp)+,ccr
	rts
rt_iorb: move.w	ccr,-(sp)
	move.l	a0,rt_slow_a0
	addq.l	#1,rt_slow_n
	movem.l	d7,-(sp)
	move.w	a0,d7
	andi.l	#$ffff,d7
	moveq	#0,d6
	move.b	(rt_io-$2000,d7.l),d6
	movem.l	(sp)+,d7
	move.w	(sp)+,ccr
	rts
rt_iowb: move.w	ccr,-(sp)
	move.l	a0,rt_slow_a0
	addq.l	#1,rt_slow_n
	movem.l	d7,-(sp)
	move.w	a0,d7
	andi.l	#$ffff,d7
	move.b	d6,(rt_io-$2000,d7.l)
	movem.l	(sp)+,d7
	move.w	(sp)+,ccr
	rts
rt_ioww: move.w	ccr,-(sp)
	move.l	a0,rt_slow_a0
	addq.l	#1,rt_slow_n
	movem.l	d7,-(sp)
	move.w	a0,d7
	andi.l	#$ffff,d7
	move.b	d6,(rt_io-$2000,d7.l)
	lsr.w	#8,d6
	move.b	d6,(rt_io-$2000+1,d7.l)
	movem.l	(sp)+,d7
	move.w	(sp)+,ccr
	rts
rt_iorw: move.w	ccr,-(sp)
	move.l	a0,rt_slow_a0
	addq.l	#1,rt_slow_n
	movem.l	d7,-(sp)
	move.w	a0,d7
	andi.l	#$ffff,d7
	move.b	(rt_io-$2000+1,d7.l),d6
	lsl.w	#8,d6
	move.b	(rt_io-$2000,d7.l),d6
	movem.l	(sp)+,d7
	move.w	(sp)+,ccr
	rts
rt_w2180: move.w ccr,-(sp)
	addq.l	#1,rt_slow_n
	move.b	d6,rt_slow_v
	move.w	(sp)+,ccr
	rts
rt_r2180: move.w ccr,-(sp)
	addq.l	#1,rt_slow_n
	move.b	rt_slow_v,d6
	move.w	(sp)+,ccr
	rts
"""


def slow_read(a: int) -> int:
    return (a & 0xFF) ^ 0xA5


def slow_readw(a: int) -> int:
    return slow_read(a) | slow_read(a + 1) << 8


class PageMap:
    """The bench's memory as the page tables describe it: `host` maps a
    24-bit SNES page (address >> 8) to the host address of its first byte;
    pages missing from the map are I/O. Writes to `rom_pages` land in the
    trash page. build() fills rt_pages_r/rt_pages_w in the bench's RAM the
    way rt_build_pages does."""

    def __init__(self, host: dict, rom_pages: set = frozenset()):
        self.host, self.rom_pages = host, set(rom_pages)

    def build(self, mem):
        for p in range(0x10000):
            h = self.host.get(p)
            mem.w32(PAGR + 4 * p, h or 0)
            mem.w32(PAGW + 4 * p, (RTTRASH if p in self.rom_pages else h) or 0)

    def read(self, mem, a: int):
        """(byte, was_slow) at a 24-bit address, as the emitted code sees it."""
        a &= 0xFFFFFF
        h = self.host.get(a >> 8)
        if h is None:
            return None
        return mem.r8(h + (a & 0xFF))
