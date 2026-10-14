"""NEC uPD7725/uPD77C25 reference interpreter and disassembler.

The DSP-1 is a uPD77C25 running a 2048 x 24-bit program with a 1024 x 16-bit
data ROM (assets/dsp1.bin: both big-endian, program first). This model is the
reference the 68030 translation (upd2m68k.py) is checked against.

Registers: accumulators A and B with flags OV0, OV1, Z, C, S0, S1 each; TR,
TRB; K and L feed the multiplier, whose product appears in M (sign and top
15 bits) and N (low 15 bits, then 0) after every instruction; DP (8 bits,
data RAM of 256 words), RP (10 bits, data ROM), PC (11 bits), a four-level
return stack; DR and SR towards the host.

Instruction words (bits 23-22): 00 OP, 01 RT (OP, then return), 10 JP,
11 LD. The ALU's ADC/SBB/SHL1 take their carry from the *other*
accumulator's C flag.

Host side (the SNES): DR and SR follow the chip's handshake. With DRC = 0
the data register moves 16 bits a byte at a time (DRS marks the second
byte); RQM is set when the DSP reads or writes DR and cleared once the host
has moved the (last) byte. The DSP program polls RQM with JRQM/JNRQM; this
model runs the program until it spins in such a poll, which is where the
host side of each access resumes.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

ALU = ["NOP", "OR", "AND", "XOR", "SUB", "ADD", "SBB", "ADC",
       "DEC", "INC", "CMP", "SHR1", "SHL1", "SHL2", "SHL4", "XCHG"]
PSEL = ["RAM", "IDB", "M", "N"]
SRC = ["TRB", "A", "B", "TR", "DP", "RP", "RO", "SGN",
       "DR", "DRNF", "SR", "SIM", "SIL", "K", "L", "MEM"]
DST = ["NON", "A", "B", "TR", "DP", "RP", "DR", "SR",
       "SOL", "SOM", "K", "KLR", "KLM", "L", "TRB", "MEM"]
DPL = ["", "DPINC", "DPDEC", "DPCLR"]

# JP conditions: brch -> (name, flag test). Tests are evaluated on UPD state.
BRANCH = {
    0x100: "JMP", 0x140: "CALL",
    0x080: "JNCA", 0x082: "JCA", 0x084: "JNCB", 0x086: "JCB",
    0x088: "JNZA", 0x08A: "JZA", 0x08C: "JNZB", 0x08E: "JZB",
    0x090: "JNOVA0", 0x092: "JOVA0", 0x094: "JNOVB0", 0x096: "JOVB0",
    0x098: "JNOVA1", 0x09A: "JOVA1", 0x09C: "JNOVB1", 0x09E: "JOVB1",
    0x0A0: "JNSA0", 0x0A2: "JSA0", 0x0A4: "JNSB0", 0x0A6: "JSB0",
    0x0A8: "JNSA1", 0x0AA: "JSA1", 0x0AC: "JNSB1", 0x0AE: "JSB1",
    0x0B0: "JDPL0", 0x0B1: "JDPLN0", 0x0B2: "JDPLF", 0x0B3: "JDPLNF",
    0x0B4: "JNSIAK", 0x0B6: "JSIAK", 0x0B8: "JNSOAK", 0x0BA: "JSOAK",
    0x0BC: "JNRQM", 0x0BE: "JRQM",
}

SR_RQM, SR_USF1, SR_USF0, SR_DRS, SR_DMA, SR_DRC = 1 << 15, 1 << 14, 1 << 13, 1 << 12, 1 << 11, 1 << 10
SR_SOC, SR_SIC, SR_EI, SR_P1, SR_P0 = 1 << 9, 1 << 8, 1 << 7, 1 << 1, 1 << 0
SR_DSP_WRITABLE = 0xFFFF & ~0x907C          # the DSP cannot write RQM, DRS, bits 6-2


def load_firmware(path: Path) -> tuple[list[int], list[int]]:
    data = path.read_bytes()
    if len(data) != 8192:
        raise ValueError(f"{path}: expected 8192 bytes, got {len(data)}")
    program = [int.from_bytes(data[i:i + 3], "big") for i in range(0, 6144, 3)]
    rom = [int.from_bytes(data[6144 + i:6146 + i], "big") for i in range(0, 2048, 2)]
    return program, rom


@dataclass
class Flags:
    ov0: int = 0
    ov1: int = 0
    z: int = 0
    c: int = 0
    s0: int = 0
    s1: int = 0


@dataclass
class UPD:
    program: list
    rom: list
    ram: list = field(default_factory=lambda: [0] * 256)
    a: int = 0
    b: int = 0
    fa: Flags = field(default_factory=Flags)
    fb: Flags = field(default_factory=Flags)
    tr: int = 0
    trb: int = 0
    k: int = 0
    l: int = 0
    m: int = 0
    n: int = 0
    dp: int = 0
    rp: int = 0
    pc: int = 0
    stack: list = field(default_factory=lambda: [0] * 4)
    dr: int = 0
    sr: int = 0
    steps: int = 0

    # -- one instruction ------------------------------------------------------
    def step(self) -> None:
        op = self.program[self.pc]
        self.pc = (self.pc + 1) & 0x7FF
        kind = op >> 22
        if kind in (0, 1):
            self.exec_op(op)
            if kind == 1:
                self.pc = self.stack[0]
                self.stack = self.stack[1:] + [self.stack[3]]
        elif kind == 2:
            self.exec_jp(op)
        else:
            self.write_dst(op & 15, (op >> 6) & 0xFFFF)
        self.multiply()
        self.steps += 1

    def multiply(self) -> None:
        k = self.k - 0x10000 if self.k & 0x8000 else self.k
        l = self.l - 0x10000 if self.l & 0x8000 else self.l
        prod = k * l
        self.m = (prod >> 15) & 0xFFFF
        self.n = (prod << 1) & 0xFFFF

    def read_src(self, src: int) -> int:
        if src == 0: return self.trb
        if src == 1: return self.a
        if src == 2: return self.b
        if src == 3: return self.tr
        if src == 4: return self.dp
        if src == 5: return self.rp
        if src == 6: return self.rom[self.rp & 0x3FF]
        if src == 7: return 0x8000 - self.fa.s1
        if src == 8:
            self.sr |= SR_RQM
            return self.dr
        if src == 9: return self.dr
        if src == 10: return self.sr
        if src in (11, 12): return 0            # serial input: unused by the DSP-1
        if src == 13: return self.k
        if src == 14: return self.l
        return self.ram[self.dp & 0xFF]

    def write_dst(self, dst: int, v: int) -> None:
        v &= 0xFFFF
        if dst == 1: self.a = v
        elif dst == 2: self.b = v
        elif dst == 3: self.tr = v
        elif dst == 4: self.dp = v & 0xFF
        elif dst == 5: self.rp = v & 0x3FF
        elif dst == 6:
            self.dr = v
            self.sr |= SR_RQM
        elif dst == 7: self.sr = (self.sr & ~SR_DSP_WRITABLE) | (v & SR_DSP_WRITABLE)
        elif dst in (8, 9): pass                  # serial output: unused
        elif dst == 10: self.k = v
        elif dst == 11:
            self.k = v
            self.l = self.rom[self.rp & 0x3FF]
        elif dst == 12:
            self.l = v
            self.k = self.ram[(self.dp | 0x40) & 0xFF]
        elif dst == 13: self.l = v
        elif dst == 14: self.trb = v
        elif dst == 15: self.ram[self.dp & 0xFF] = v

    def exec_op(self, op: int) -> None:
        psel = (op >> 20) & 3
        alu = (op >> 16) & 15
        asl = (op >> 15) & 1
        dpl = (op >> 13) & 3
        dphm = (op >> 9) & 15
        rpdcr = (op >> 8) & 1
        src = (op >> 4) & 15
        dst = op & 15
        idb = self.read_src(src)
        if alu:
            p = [self.ram[self.dp & 0xFF], idb, self.m, self.n][psel]
            if asl == 0:
                q, flag, c = self.a, self.fa, self.fb.c
            else:
                q, flag, c = self.b, self.fb, self.fa.c
            r = self.alu(alu, p, q, c, flag)
            if asl == 0:
                self.a = r
            else:
                self.b = r
        self.write_dst(dst, idb)
        if dpl == 1:
            self.dp = (self.dp & 0xF0) | ((self.dp + 1) & 0x0F)
        elif dpl == 2:
            self.dp = (self.dp & 0xF0) | ((self.dp - 1) & 0x0F)
        elif dpl == 3:
            self.dp &= 0xF0
        self.dp ^= dphm << 4
        if rpdcr:
            self.rp = (self.rp - 1) & 0x3FF

    @staticmethod
    def alu(alu: int, p: int, q: int, c: int, flag: Flags) -> int:
        if alu == 1: r = q | p
        elif alu == 2: r = q & p
        elif alu == 3: r = q ^ p
        elif alu == 4: r = q - p
        elif alu == 5: r = q + p
        elif alu == 6: r = q - p - c
        elif alu == 7: r = q + p + c
        elif alu == 8: r, p = q - 1, 1
        elif alu == 9: r, p = q + 1, 1
        elif alu == 10: r = ~q
        elif alu == 11: r = (q >> 1) | (q & 0x8000)
        elif alu == 12: r = (q << 1) | c
        elif alu == 13: r = (q << 2) | 3
        elif alu == 14: r = (q << 4) | 15
        else: r = ((q << 8) | (q >> 8))
        r &= 0xFFFF
        flag.s0 = 1 if r & 0x8000 else 0
        flag.z = 1 if r == 0 else 0
        if alu in (1, 2, 3, 10, 13, 14, 15):
            flag.c = flag.ov0 = flag.ov1 = 0
        elif alu in (4, 5, 6, 7, 8, 9):
            if alu & 1:
                flag.ov0 = 1 if (q ^ r) & (p ^ r) & 0x8000 else 0
                flag.c = 1 if r < q else 0
            else:
                flag.ov0 = 1 if (q ^ r) & (q ^ p) & 0x8000 else 0
                flag.c = 1 if r > q else 0
            if flag.ov0:
                flag.s1 = flag.ov1 ^ (0 if r & 0x8000 else 1)
                flag.ov1 ^= 1
        elif alu == 11:
            flag.c = q & 1
            flag.ov0 = flag.ov1 = 0
        elif alu == 12:
            flag.c = q >> 15
            flag.ov0 = flag.ov1 = 0
        return r

    def condition(self, brch: int) -> bool:
        fa, fb, sr = self.fa, self.fb, self.sr
        table = {
            0x100: True, 0x140: True,
            0x080: not fa.c, 0x082: bool(fa.c), 0x084: not fb.c, 0x086: bool(fb.c),
            0x088: not fa.z, 0x08A: bool(fa.z), 0x08C: not fb.z, 0x08E: bool(fb.z),
            0x090: not fa.ov0, 0x092: bool(fa.ov0), 0x094: not fb.ov0, 0x096: bool(fb.ov0),
            0x098: not fa.ov1, 0x09A: bool(fa.ov1), 0x09C: not fb.ov1, 0x09E: bool(fb.ov1),
            0x0A0: not fa.s0, 0x0A2: bool(fa.s0), 0x0A4: not fb.s0, 0x0A6: bool(fb.s0),
            0x0A8: not fa.s1, 0x0AA: bool(fa.s1), 0x0AC: not fb.s1, 0x0AE: bool(fb.s1),
            0x0B0: (self.dp & 15) == 0, 0x0B1: (self.dp & 15) != 0,
            0x0B2: (self.dp & 15) == 15, 0x0B3: (self.dp & 15) != 15,
            0x0BC: not (sr & SR_RQM), 0x0BE: bool(sr & SR_RQM),
        }
        if brch not in table:
            raise ValueError(f"unsupported branch condition {brch:#05x} at {self.pc - 1:#05x}")
        return table[brch]

    def exec_jp(self, op: int) -> None:
        brch = (op >> 13) & 0x1FF
        na = (op >> 2) & 0x7FF
        if self.condition(brch):
            if brch == 0x140:
                self.stack = [self.pc] + self.stack[:3]
            self.pc = na

    # -- host side ------------------------------------------------------------------
    def waiting(self) -> bool:
        """The next instruction polls RQM and would jump to itself."""
        op = self.program[self.pc]
        if op >> 22 != 2:
            return False
        brch = (op >> 13) & 0x1FF
        na = (op >> 2) & 0x7FF
        return brch in (0x0BC, 0x0BE) and na == self.pc and self.condition(brch)

    def run(self, limit: int = 1_000_000) -> None:
        for _ in range(limit):
            if self.waiting():
                return
            self.step()
        raise RuntimeError(f"DSP did not reach a host poll within {limit} steps (pc {self.pc:#05x})")

    def host_write_dr(self, byte: int) -> None:
        self.run()
        if not self.sr & SR_DRC:
            if not self.sr & SR_DRS:
                self.sr |= SR_DRS
                self.dr = (self.dr & 0xFF00) | byte
            else:
                self.sr &= ~(SR_RQM | SR_DRS)
                self.dr = (byte << 8) | (self.dr & 0xFF)
        else:
            self.sr &= ~SR_RQM
            self.dr = (self.dr & 0xFF00) | byte

    def host_read_dr(self) -> int:
        self.run()
        if not self.sr & SR_DRC:
            if not self.sr & SR_DRS:
                self.sr |= SR_DRS
                return self.dr & 0xFF
            self.sr &= ~(SR_RQM | SR_DRS)
            return self.dr >> 8
        self.sr &= ~SR_RQM
        return self.dr & 0xFF

    def host_read_sr(self) -> int:
        self.run()
        return self.sr >> 8

    # -- convenience for tests ------------------------------------------------------
    def command(self, cmd: int, params: list[int], results: int) -> list[int]:
        self.host_write_dr(cmd)
        for p in params:
            self.host_write_dr(p & 0xFF)
            self.host_write_dr((p >> 8) & 0xFF)
        out = []
        for _ in range(results):
            lo = self.host_read_dr()
            hi = self.host_read_dr()
            out.append(lo | (hi << 8))
        return out


def disassemble(op: int) -> str:
    kind = op >> 22
    if kind in (0, 1):
        psel, alu, asl = (op >> 20) & 3, (op >> 16) & 15, (op >> 15) & 1
        dpl, dphm, rpdcr = (op >> 13) & 3, (op >> 9) & 15, (op >> 8) & 1
        src, dst = (op >> 4) & 15, op & 15
        parts = ["RT" if kind else "OP"]
        if alu:
            parts.append(f"{ALU[alu]} {'AB'[asl]},{PSEL[psel]}")
        if src or dst:
            parts.append(f"MOV {DST[dst]},{SRC[src]}")
        if dpl:
            parts.append(DPL[dpl])
        if dphm:
            parts.append(f"M{dphm:X}")
        if rpdcr:
            parts.append("RPDEC")
        return " | ".join(parts)
    if kind == 2:
        brch, na = (op >> 13) & 0x1FF, (op >> 2) & 0x7FF
        return f"{BRANCH.get(brch, f'J?{brch:03X}')} ${na:03X}"
    return f"LD {DST[op & 15]},#${(op >> 6) & 0xFFFF:04X}"
