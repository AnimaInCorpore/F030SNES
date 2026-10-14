"""Static translation of YI's SuperFX (GSU-2) program to C.

    python3 -m tools.smkx.gsu2c ROM OUT.c

The GSU code lives in banks $08-$0B (YI/SuperFX/SuperFXCode_YI.asm); every
CODE_ label there and every entry the 65816 side names is a root. From the
roots the ROM bytes are decoded along control flow. A node is an address
plus the prefix state it is reached with (ALT1/ALT2, B, source and
destination register): YI jumps into the middle of prefixed instructions
with the prefix set in the jump's delay slot, so the state travels with the
flow like the 65816 register widths do.

Each bank becomes one C function over the GSU state (src/gsu.h) with the
registers and flags in locals, so the compiler allocates registers and
drops flag computations nobody reads. Per instruction the code mirrors
src/gsu.c's step() with the prefixes resolved:

- R15 reads are constants (the address after the opcode and its operands).
- Every write to R15 has a delay slot: the next instruction executes before
  the jump takes effect. It is emitted inline on each path; its operand
  bytes, if any, come from the target's stream, as the pipeline fetches
  them (YI uses this: `IWT R15, #L+$01 : LMS R0, ($20)`).
- Static targets are gotos; dynamic ones (`JMP R11`, arithmetic into R15,
  LOOP with an unknown R13) go through a switch over this bank's nodes, and
  leave the function when the target lies elsewhere (the driver, src/
  yix_gsu.c, then calls the right bank or falls back to the interpreter).

The interpreter (src/gsu.c) stays the reference: yix_gsu.c can run both
and compare them run by run (YIX_GSU_CHECK).
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LISTING = ROOT / "third_party/Yoshi-s-Island-Disassembly/YI/SuperFX/SuperFXCode_YI.asm"
BANKS = (0x08, 0x09, 0x0A, 0x0B)

# Prefix state: (alt1, alt2, b, sreg, dreg)
NONE = (0, 0, 0, 0, 0)

BRANCH = {0x05: "1", 0x06: "(s == ov)", 0x07: "(s != ov)", 0x08: "!z", 0x09: "z", 0x0A: "!s",
          0x0B: "s", 0x0C: "!cy", 0x0D: "cy", 0x0E: "!ov", 0x0F: "ov"}


def rom_offset(bank: int, pc: int) -> int:
    """The GSU's ROM view: banks $00-$3F LoROM-style (bit 15 ignored)."""
    return ((bank & 0x3F) << 15) | (pc & 0x7FFF)


def oplen(op: int) -> int:
    if op < 0x10 and op >= 0x05:
        return 2                       # branches
    if 0xA0 <= op <= 0xAF:
        return 2                       # IBT / LMS / SMS
    if op >= 0xF0:
        return 3                       # IWT / LM / SM
    return 1


def is_prefix(op: int, pf) -> bool:
    """Instructions that only set prefix state."""
    alt1, alt2, b, sreg, dreg = pf
    hi = op >> 4
    if op in (0x3D, 0x3E, 0x3F):
        return True
    if hi == 0x2:
        return True                    # WITH
    if hi == 0x1 and not b:
        return True                    # TO
    if hi == 0xB and not b:
        return True                    # FROM
    return False


def after_prefix(op: int, pf):
    """Prefix state after an instruction (branches keep it, like gsu.c)."""
    alt1, alt2, b, sreg, dreg = pf
    n = op & 15
    if op == 0x3D:
        return (1, alt2, 0, sreg, dreg)
    if op == 0x3E:
        return (alt1, 1, 0, sreg, dreg)
    if op == 0x3F:
        return (1, 1, 0, sreg, dreg)
    if op >> 4 == 0x2:
        return (alt1, alt2, 1, n, n)
    if op >> 4 == 0x1 and not b:
        return (alt1, alt2, b, sreg, n)
    if op >> 4 == 0xB and not b:
        return (alt1, alt2, b, n, dreg)
    if 0x05 <= op <= 0x0F:
        return pf
    return NONE


class Unsupported(Exception):
    pass


@dataclass
class Node:
    bank: int
    pc: int
    pf: tuple


class Translator:
    def __init__(self, rom: bytes):
        self.rom = rom
        self.nodes: dict[tuple, int] = {}          # (bank, pc, pf) -> id
        self.key_of: dict[int, tuple] = {}
        self.work: list[tuple] = []
        self.problems: list[str] = []

    def byte(self, bank: int, pc: int) -> int:
        return self.rom[rom_offset(bank, pc)]

    def label(self, bank, pc, pf) -> str:
        key = (bank, pc, pf)
        if key not in self.nodes:
            self.nodes[key] = len(self.nodes)
            self.key_of[self.nodes[key]] = key
            self.work.append(key)
        return f"L{self.nodes[key]}"

    # -- one instruction as C ---------------------------------------------
    def insn(self, bank: int, pc: int, pf, operand_at=None, r15dyn=None):
        """C statements for the instruction at bank:pc with prefix state pf.
        Returns (lines, new_pf, flow, length); flow is None or describes a
        write to R15. operand_at: (bank, pc) of the stream a delay slot
        reads R15 and its operand bytes from (the jump target); r15dyn: the
        C expression of R15 in the delay slot of a dynamic jump."""
        op = self.byte(bank, pc)
        alt1, alt2, b, sreg, dreg = pf
        n = op & 15
        hi = op >> 4
        ln = oplen(op)
        ob, opc = operand_at if operand_at else (bank, pc + 1)
        k = ln - 1
        operand = [self.byte(ob, opc + i) for i in range(k)]
        # R15 as the instruction sees it: the address after its operands
        # (in a delay slot: the jump target, advanced past the operands).
        r15 = (opc + k) & 0xFFFF if operand_at else (pc + ln) & 0xFFFF

        def rv(i):
            if i == 15:
                return f"(uint16_t){r15dyn}" if r15dyn else f"(uint16_t)0x{r15:04X}"
            return f"r{i}"

        src = rv(sreg)
        out: list[str] = []
        new_r15 = None                      # a value (int) or "dyn:<expr>"

        def set_r(i, expr):
            nonlocal new_r15
            if i == 15:
                out.append(f"t15 = (uint16_t)({expr});")
                new_r15 = "dyn"
            else:
                out.append(f"r{i} = (uint16_t)({expr});")

        def set_dr(expr):
            set_r(dreg, expr)

        def sz(v):
            out.append(f"s = ((uint16_t)({v}) & 0x8000) != 0; z = (uint16_t)({v}) == 0;")

        if is_prefix(op, pf):
            return [], after_prefix(op, pf), None, ln
        npf = after_prefix(op, pf)
        if op == 0x00:
            # R15 ends one past the byte after STOP, as in gsu.c
            out.append(f"target = 0x{(r15 + 1) & 0xFFFF:04X}; goto stop;")
        elif op == 0x01:
            pass
        elif op == 0x02:
            out.append(f"g->cbr = 0x{r15 & 0xFFF0:04X};")
        elif op == 0x03:
            out += [f"v = {src};", "cy = v & 1;", "v >>= 1;"]
            set_dr("v"); sz("v")
        elif op == 0x04:
            out += [f"v = {src};", "t = (uint16_t)((v << 1) | cy);", "cy = v >> 15;"]
            set_dr("t"); sz("t")
        elif 0x05 <= op <= 0x0F:
            d = operand[0] - 256 if operand[0] & 0x80 else operand[0]
            target = (r15 + d) & 0xFFFF
            return [f"cond = {BRANCH[op]};"], npf, ("branch", target), ln
        elif hi == 0x1:                     # MOVE (B set)
            set_r(n, src)
        elif hi == 0x3:
            if n < 12:
                out.append(f"ramaddr = r{n};")
                out.append(f"RAMW(ramaddr, {src});")
                if not alt1:
                    out.append(f"RAMW(ramaddr ^ 1, {src} >> 8);")
            elif n == 12:
                out += ["r12--;"]
                sz("r12")
                return out, npf, ("loop",), ln
        elif hi == 0x4:
            if n < 12:
                out.append(f"ramaddr = r{n};")
                if alt1:
                    out.append("v = RAMR(ramaddr);")
                else:
                    out.append("v = (uint16_t)(RAMR(ramaddr) | (RAMR(ramaddr ^ 1) << 8));")
                set_dr("v")
            elif n == 12:
                if not alt1:
                    out.append("gsu_plot(g, (uint8_t)r1, (uint8_t)r2); r1++;")
                else:
                    out.append("v = gsu_rpix(g, (uint8_t)r1, (uint8_t)r2);")
                    set_dr("v"); sz("v")
            elif n == 13:
                out.append(f"v = {src}; v = (uint16_t)((v >> 8) | (v << 8));")
                set_dr("v"); sz("v")
            elif n == 14:
                if not alt1:
                    out.append(f"g->colr = gsu_color(g, (uint8_t){src});")
                else:
                    out.append(f"g->por = (uint8_t){src};")
            else:
                out.append(f"v = (uint16_t)~{src};")
                set_dr("v"); sz("v")
        elif hi == 0x5:
            o = f"{n}" if alt2 else rv(n)
            c = " + cy" if alt1 else ""
            out += [f"o = {o};", f"w = (uint32_t){src} + o{c};",
                    f"ov = ((~({src} ^ o) & (o ^ w)) & 0x8000) != 0;", "cy = w >= 0x10000;"]
            sz("w"); set_dr("w")
        elif hi == 0x6:
            o = f"{n}" if (alt2 and not alt1) else rv(n)
            c = " - !cy" if (alt1 and not alt2) else ""
            out += [f"o = {o};", f"sw = (int32_t){src} - o{c};",
                    f"ov = ((({src} ^ o) & ({src} ^ sw)) & 0x8000) != 0;", "cy = sw >= 0;"]
            sz("sw")
            if not (alt1 and alt2):
                set_dr("sw")
        elif op == 0x70:
            out += ["v = (uint16_t)((r7 & 0xFF00) | (r8 >> 8));",
                    "ov = (v & 0xC0C0) != 0; s = (v & 0x8080) != 0; cy = (v & 0xE0E0) != 0; z = (v & 0xF0F0) != 0;"]
            set_dr("v")
        elif hi == 0x7:
            o = f"{n}" if alt2 else rv(n)
            if alt1:
                o = f"(uint16_t)~{o}"
            out.append(f"v = {src} & {o};")
            sz("v"); set_dr("v")
        elif hi == 0x8:
            o = f"{n}" if alt2 else rv(n)
            if not alt1:
                out.append(f"v = (uint16_t)((int16_t)(int8_t){src} * (int16_t)(int8_t){o});")
            else:
                out.append(f"v = (uint16_t)((uint8_t){src} * (uint8_t){o});")
            sz("v"); set_dr("v")
        elif op == 0x90:
            out += ["RAMW(ramaddr, " + src + ");", "RAMW(ramaddr ^ 1, " + src + " >> 8);"]
        elif 0x91 <= op <= 0x94:
            out.append(f"r11 = (uint16_t)({rv(15)} + {n});")
        elif op == 0x95:
            out.append(f"v = (uint16_t)(int16_t)(int8_t){src};")
            sz("v"); set_dr("v")
        elif op == 0x96:
            out.append(f"cy = {src} & 1;")
            if not alt1:
                out.append(f"v = (uint16_t)((int16_t){src} >> 1);")
            else:
                out.append(f"v = (uint16_t)(((int16_t){src} >> 1) + (((uint32_t){src} + 1) >> 16));")
            sz("v"); set_dr("v")
        elif op == 0x97:
            out += [f"v = (uint16_t)((cy << 15) | ({src} >> 1));", f"cy = {src} & 1;"]
            sz("v"); set_dr("v")
        elif 0x98 <= op <= 0x9D:
            if not alt1:
                out.append(f"t15 = r{n};")
                return out, npf, ("jump", None), ln
            out += [f"pbr = r{n} & 0x7F;", f"t15 = {src};", "g->cbr = t15 & 0xFFF0;"]
            return out, npf, ("ljump",), ln
        elif op == 0x9E:
            out += [f"v = {src} & 0xFF;", "s = (v & 0x80) != 0; z = v == 0;"]
            set_dr("v")
        elif op == 0x9F:
            out.append(f"w = (uint32_t)((int32_t)(int16_t){src} * (int32_t)(int16_t)r6);")
            if alt1:
                out.append("r4 = (uint16_t)w;")
            out += ["v = (uint16_t)(w >> 16);", "cy = (w >> 15) & 1;"]
            sz("v"); set_dr("v")
        elif hi == 0xA:
            if alt1:
                a = operand[0] << 1
                set_r(n, f"RAMR(0x{a:04X}) | (RAMR(0x{a ^ 1:04X}) << 8)")
                out.insert(0, f"ramaddr = 0x{a:04X};")
            elif alt2:
                a = operand[0] << 1
                out += [f"ramaddr = 0x{a:04X};", f"RAMW(0x{a:04X}, {rv(n)});", f"RAMW(0x{a ^ 1:04X}, {rv(n)} >> 8);"]
            else:
                v = operand[0] - 256 if operand[0] & 0x80 else operand[0]
                set_r(n, f"0x{v & 0xFFFF:04X}")
                if n == 15:
                    return out, npf, ("const", v & 0xFFFF), ln
        elif hi == 0xB:                     # MOVES (B set)
            out += [f"v = {rv(n)};", "ov = (v & 0x80) != 0;"]
            sz("v"); set_dr("v")
        elif op == 0xC0:
            out += [f"v = {src} >> 8;", "s = (v & 0x80) != 0; z = v == 0;"]
            set_dr("v")
        elif hi == 0xC:
            o = f"{n}" if alt2 else rv(n)
            opx = "^" if alt1 else "|"
            out.append(f"v = {src} {opx} {o};")
            sz("v"); set_dr("v")
        elif hi == 0xD:
            if n < 15:
                out.append(f"r{n}++;")
                sz(f"r{n}")
            elif not alt2:
                out.append("g->colr = gsu_color(g, ROMB());")
            elif not alt1:
                out.append(f"g->rambr = {src} & 1;")
            else:
                out.append(f"rombr = {src} & 0x7F; SETROMB();")
        elif hi == 0xE:
            if n < 15:
                out.append(f"r{n}--;")
                sz(f"r{n}")
            else:
                if not alt1 and not alt2:
                    set_dr("ROMB()")
                elif alt1 and not alt2:
                    set_dr(f"(ROMB() << 8) | ({src} & 0xFF)")
                elif not alt1:
                    set_dr(f"({src} & 0xFF00) | ROMB()")
                else:
                    set_dr("(uint16_t)(int16_t)(int8_t)ROMB()")
        else:
            if alt1:
                a = operand[0] | operand[1] << 8
                out.append(f"ramaddr = 0x{a:04X};")
                set_r(n, f"RAMR(0x{a:04X}) | (RAMR(0x{a ^ 1:04X}) << 8)")
            elif alt2:
                a = operand[0] | operand[1] << 8
                out += [f"ramaddr = 0x{a:04X};", f"RAMW(0x{a:04X}, {rv(n)});", f"RAMW(0x{a ^ 1:04X}, {rv(n)} >> 8);"]
            else:
                v = operand[0] | operand[1] << 8
                set_r(n, f"0x{v:04X}")
                if n == 15:
                    return out, npf, ("const", v), ln
        if new_r15 == "dyn":
            return out, npf, ("jump", None), ln
        return out, npf, None, ln

    # -- a node and its control flow ---------------------------------------
    def node_code(self, bank: int, pc: int, pf) -> list[str]:
        lines, npf, flow, ln = self.insn(bank, pc, pf)
        nxt = (pc + ln) & 0xFFFF
        if flow is None:
            if self.byte(bank, pc) == 0x00 and not is_prefix(0, pf):
                return lines
            return lines + [f"goto {self.label(bank, nxt, npf)};"]
        # A write to R15: the next instruction is the delay slot.
        kind = flow[0]
        dpc = nxt
        if kind == "branch":
            target = flow[1]
            taken = self.delay(bank, dpc, npf, ("static", bank, target))
            fall = self.delay(bank, dpc, npf, None)
            return lines + ["if (cond) {"] + taken + ["}"] + fall
        if kind == "loop":
            # LOOP: jump to R13 unless R12 reached zero.
            taken = self.delay(bank, dpc, npf, ("dyn", "r13"))
            fall = self.delay(bank, dpc, npf, None)
            return lines + ["if (!z) {"] + taken + ["}"] + fall
        if kind == "const":
            return lines + self.delay(bank, dpc, npf, ("static", bank, flow[1]))
        if kind == "ljump":
            return lines + self.delay(bank, dpc, npf, ("far", "t15"))
        return lines + self.delay(bank, dpc, npf, ("dyn", "t15"))

    def delay(self, bank, dpc, pf, target) -> list[str]:
        """The delay slot at bank:dpc, then the jump (target None: fall
        through after the slot)."""
        op = self.byte(bank, dpc)
        if target is None:
            lines, npf, flow, ln = self.insn(bank, dpc, pf)
            if flow is not None:
                raise Unsupported(f"${bank:02X}:{dpc:04X} writes R15 in a delay slot")
            if op == 0x00:
                return lines
            return lines + [f"goto {self.label(bank, (dpc + ln) & 0xFFFF, npf)};"]
        k = oplen(op) - 1
        if target[0] == "static":
            tb, tpc = target[1], target[2]
            lines, npf, flow, ln = self.insn(bank, dpc, pf, (tb, tpc))
            if flow is not None:
                raise Unsupported(f"${bank:02X}:{dpc:04X} writes R15 in a delay slot")
            if op == 0x00:
                return lines
            return lines + [f"goto {self.label(tb, (tpc + k) & 0xFFFF, npf)};"]
        if k:
            raise Unsupported(f"${bank:02X}:{dpc:04X}: operand bytes in the delay slot of a dynamic jump")
        lines, npf, flow, ln = self.insn(bank, dpc, pf, r15dyn=target[1])
        if flow is not None:
            raise Unsupported(f"${bank:02X}:{dpc:04X} writes R15 in a delay slot")
        pfid = self.pf_id(npf)
        if target[0] == "far":
            return lines + [f"target = {target[1]}; tpf = {pfid}; goto far;"]
        return lines + [f"target = {target[1]}; tpf = {pfid}; goto dispatch;"]

    def pf_id(self, pf) -> int:
        alt1, alt2, b, sreg, dreg = pf
        return alt1 | alt2 << 1 | b << 2 | sreg << 3 | dreg << 7

    # -- driver --------------------------------------------------------------
    def run(self, roots: list[tuple[int, int]]) -> None:
        for bank, pc in roots:
            self.label(bank, pc, NONE)
        self.code: dict[tuple, list[str]] = {}
        while self.work:
            key = self.work.pop()
            bank, pc, pf = key
            try:
                self.code[key] = self.node_code(bank, pc, pf)
            except Unsupported as e:
                self.problems.append(str(e))
                self.code[key] = [f"target = 0x{pc:04X}; tpf = {self.pf_id(pf)}; goto unsupported;"]

    def emit(self, roots: set) -> str:
        """C source: each bank's nodes in address order, cut into chunks of
        about CHUNK nodes at routine labels (gcc copes badly with huge goto
        functions). A goto into another chunk leaves the function with the
        target (fault 5); yix_gsu_enter() finds the chunk and continues."""
        out = [HEADER]
        keys = sorted(self.nodes, key=lambda k: (k[0], k[1], k[2]))
        chunk_of: dict[int, int] = {}
        chunks: list[list] = []
        for k in keys:
            new = not chunks or chunks[-1][0][0] != k[0] or \
                (len(chunks[-1]) >= CHUNK and (k[0], k[1]) in roots and chunks[-1][-1][1] != k[1])
            if new:
                chunks.append([])
            chunks[-1].append(k)
            chunk_of[self.nodes[k]] = len(chunks) - 1
        goto = re.compile(r"goto L(\d+);")
        for ci, ck in enumerate(chunks):
            def jump(m, ci=ci):
                lid = int(m.group(1))
                if chunk_of[lid] == ci:
                    return m.group(0)
                bank, pc, pf = self.key_of[lid]
                return f"{{ target = 0x{pc:04X}; tpf = {self.pf_id(pf)}; goto next; }}"
            out.append(f"static int c{ci}(Gsu *g)")
            out.append("{")
            out.append(PROLOGUE)
            out.append("dispatch:")
            out.append("switch ((uint32_t)target << 12 | tpf) {")
            for k in ck:
                out.append(f"case 0x{k[1] << 12 | self.pf_id(k[2]):X}u: goto L{self.nodes[k]};")
            out.append("default: g->fault = 3; goto leave;")
            out.append("}")
            for k in ck:
                out.append(f"L{self.nodes[k]}: /* ${k[0]:02X}:{k[1]:04X} {k[2]} */")
                out.extend("\t" + goto.sub(jump, l) for l in self.code[k])
            out.append("next:\n\tg->fault = 5;\n\tgoto leave;")
            out.append(EPILOGUE)
            out.append("}")
        out.append("static int (*const chunk_fn[])(Gsu *) = {")
        out.append(",".join(f"c{i}" for i in range(len(chunks))))
        out.append("};")
        out.append("static const uint32_t chunk_start[] = {")
        out.append(",".join(f"0x{ck[0][0]:02X}{ck[0][1]:04X}u" for ck in chunks))
        out.append("};")
        out.append(f"#define NCHUNKS {len(chunks)}")
        out.append(ENTER)
        return "\n".join(out) + "\n"


CHUNK = 250

ENTER = r'''
/* Run translated code from g->pbr:g->r[15] (the next opcode) with the
 * prefix state in g, across chunks, until STOP (returns 0) or a target
 * that was not translated (returns 1; g->r[15] = that target, fault 2 or 3)
 * or a far jump (fault 4). */
int yix_gsu_enter(Gsu *g)
{
    for (;;) {
        uint32_t key = (uint32_t)g->pbr << 16 | g->r[15];
        int lo = 0, hi = NCHUNKS - 1, c = -1;
        while (lo <= hi) {                  /* last chunk starting at or before key */
            int mid = (lo + hi) >> 1;
            if (chunk_start[mid] <= key) {
                c = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (c < 0 || (chunk_start[c] >> 16) != g->pbr) {
            g->fault = 3;
            return 1;
        }
        g->fault = 0;
        chunk_fn[c](g);
        if (!g->g)
            return 0;
        if (g->fault != 5)
            return 1;
    }
}
'''


HEADER = r'''/* Generated by tools/smkx/gsu2c.py from the user's ROM: YI's SuperFX
 * program as C. Do not edit and do not commit. */
#include "gsu.h"

#define RAMR(a) (ram[(uint16_t)(a) & GSU_RAM_MASK])
#define RAMW(a, v) (ram[(uint16_t)(a) & GSU_RAM_MASK] = (uint8_t)(v))
#define ROMB() (romb[r14 & rmask])
#define SETROMB() do { romb = gsu_rom_view(g, rombr, &rmask); } while (0)
'''

PROLOGUE = r'''	uint16_t r0 = g->r[0], r1 = g->r[1], r2 = g->r[2], r3 = g->r[3], r4 = g->r[4],
		r5 = g->r[5], r6 = g->r[6], r7 = g->r[7], r8 = g->r[8], r9 = g->r[9], r10 = g->r[10],
		r11 = g->r[11], r12 = g->r[12], r13 = g->r[13], r14 = g->r[14];
	uint16_t target = g->r[15], t15 = 0, v, t, o, ramaddr = g->ramaddr;
	uint32_t w;
	int32_t sw;
	int z = g->z, s = g->s, cy = g->cy, ov = g->ov, cond;
	unsigned tpf = g->alt1 | g->alt2 << 1 | g->b << 2 | g->sreg << 3 | g->dreg << 7;
	uint8_t pbr = g->pbr, rombr = g->rombr;
	uint8_t *ram = g->ram;
	uint16_t rmask;
	const uint8_t *romb = gsu_rom_view(g, rombr, &rmask);
	(void)t; (void)o; (void)w; (void)sw; (void)cond; (void)t15; (void)pbr;
	goto dispatch;'''

EPILOGUE = r'''stop:
	g->g = 0;
	if (!(g->cfgr & 0x80))
		g->irq = 1;
	g->pipeline = 0x01;
	g->alt1 = g->alt2 = g->b = 0; g->sreg = g->dreg = 0;
	g->r[15] = target;
	goto save;
far:
	g->pbr = pbr;
	g->fault = 4;
	goto leave;
unsupported:
	g->fault = 2;
leave:
	g->alt1 = tpf & 1; g->alt2 = (tpf >> 1) & 1; g->b = (tpf >> 2) & 1;
	g->sreg = (tpf >> 3) & 15; g->dreg = (tpf >> 7) & 15;
	g->r[15] = target;
save:
	g->r[0] = r0; g->r[1] = r1; g->r[2] = r2; g->r[3] = r3; g->r[4] = r4; g->r[5] = r5;
	g->r[6] = r6; g->r[7] = r7; g->r[8] = r8; g->r[9] = r9; g->r[10] = r10; g->r[11] = r11;
	g->r[12] = r12; g->r[13] = r13; g->r[14] = r14;
	g->z = z; g->s = s; g->cy = cy; g->ov = ov;
	g->rombr = rombr; g->ramaddr = ramaddr;
	return g->g;'''


def roots_from_listing(path: Path) -> list[tuple[int, int]]:
    out = []
    for m in re.finditer(r"^CODE_([0-9A-F]{2})([0-9A-F]{4}):", path.read_text(errors="replace"), re.M):
        bank, pc = int(m.group(1), 16), int(m.group(2), 16)
        if bank in BANKS:
            out.append((bank, pc))
    return out


def main() -> None:
    rom = Path(sys.argv[1]).read_bytes()
    t = Translator(rom)
    roots = roots_from_listing(LISTING)
    t.run(roots)
    Path(sys.argv[2]).write_text(t.emit(set(roots)))
    print(f"GSU: {len(t.nodes)} nodes, {len(t.problems)} problems")
    for p in t.problems[:20]:
        print("  " + p)


if __name__ == "__main__":
    main()
