"""Translate the DSP-1's uPD77C25 program to 68030 code.

The generated routine runs the chip's program from a resume point until it
polls the host (a JRQM on itself), then returns. Its semantics are those of
upd7725.UPD, which the Hatari harness compares it against word for word.

Register use while it runs (the runtime saves and restores the caller's):
    d0.w A    d1.w B    d2.w K    d3.w L
    d4   scratch        d5.w DP   d6.w RP (kept in 0-$3FF)   d7 IDB / scratch
    a0   data RAM (256 words)     a1 data ROM (1024 words, big-endian)
    a3   state block (upd_* offsets below)
Flags live in the state block as bytes (0 or $FF) and are stored only where
the flag liveness below says a later instruction can read them.
M and N are formed from K and L where an ALU operation selects them; the chip
computes the product at the end of every instruction from the same K and L,
and nothing else reads M or N.
"""

from __future__ import annotations

from .upd7725 import BRANCH, SR_DSP_WRITABLE, disassemble

# State block layout (offsets from a3).
ST = {
    "tr": 0, "trb": 2, "dr": 4, "sr": 6, "pc": 8, "stack": 10,   # stack: 4 words
    "a_ov0": 18, "a_ov1": 19, "a_z": 20, "a_c": 21, "a_s0": 22, "a_s1": 23,
    "b_ov0": 24, "b_ov1": 25, "b_z": 26, "b_c": 27, "b_s0": 28, "b_s1": 29,
    "a": 30, "b": 32, "k": 34, "l": 36, "dp": 38, "rp": 40,
}
STATE_SIZE = 42
FLAGS = ["ov0", "ov1", "z", "c", "s0", "s1"]
BIT = {f"{acc}_{f}": 1 << (i + 6 * j) for j, acc in enumerate("ab") for i, f in enumerate(FLAGS)}
ALLF = (1 << 12) - 1

COND = {
    0x080: ("a_c", False), 0x082: ("a_c", True), 0x084: ("b_c", False), 0x086: ("b_c", True),
    0x088: ("a_z", False), 0x08A: ("a_z", True), 0x08C: ("b_z", False), 0x08E: ("b_z", True),
    0x090: ("a_ov0", False), 0x092: ("a_ov0", True), 0x094: ("b_ov0", False), 0x096: ("b_ov0", True),
    0x098: ("a_ov1", False), 0x09A: ("a_ov1", True), 0x09C: ("b_ov1", False), 0x09E: ("b_ov1", True),
    0x0A0: ("a_s0", False), 0x0A2: ("a_s0", True), 0x0A4: ("b_s0", False), 0x0A6: ("b_s0", True),
    0x0A8: ("a_s1", False), 0x0AA: ("a_s1", True), 0x0AC: ("b_s1", False), 0x0AE: ("b_s1", True),
}


def fields(op: int) -> dict:
    return dict(kind=op >> 22, psel=(op >> 20) & 3, alu=(op >> 16) & 15, asl=(op >> 15) & 1,
                dpl=(op >> 13) & 3, dphm=(op >> 9) & 15, rpdcr=(op >> 8) & 1,
                src=(op >> 4) & 15, dst=op & 15, brch=(op >> 13) & 0x1FF, na=(op >> 2) & 0x7FF,
                imm=(op >> 6) & 0xFFFF)


def flag_use_def(op: int) -> tuple[int, int]:
    f = fields(op)
    use = dfn = 0
    if f["kind"] == 2:
        c = COND.get(f["brch"])
        if c:
            use |= BIT[c[0]]
        return use, dfn
    if f["kind"] == 3:
        return use, dfn
    if f["src"] == 7:
        use |= BIT["a_s1"]
    alu = f["alu"]
    if alu:
        x = "ab"[f["asl"]]
        other = "ab"[1 - f["asl"]]
        if alu in (6, 7, 12):
            use |= BIT[f"{other}_c"]
        dfn |= BIT[f"{x}_s0"] | BIT[f"{x}_z"] | BIT[f"{x}_c"] | BIT[f"{x}_ov0"] | BIT[f"{x}_ov1"]
        if alu in (4, 5, 6, 7, 8, 9):
            # OV1 toggles and S1 updates only on an overflow: both are read.
            use |= BIT[f"{x}_ov1"] | BIT[f"{x}_s1"]
            dfn |= BIT[f"{x}_s1"]
    return use, dfn


def successors(pc: int, op: int, returns: list[int]) -> list[int]:
    f = fields(op)
    nxt = (pc + 1) & 0x7FF
    if f["kind"] == 1:
        return list(returns)
    if f["kind"] == 2:
        if f["brch"] == 0x100:
            return [f["na"]]
        if f["brch"] == 0x140:
            return [f["na"]]
        return [f["na"], nxt]
    return [nxt]


def liveness(program: list[int]) -> list[int]:
    returns = sorted({(pc + 1) & 0x7FF for pc, op in enumerate(program)
                      if op >> 22 == 2 and fields(op)["brch"] == 0x140})
    n = len(program)
    succ = [successors(pc, op, returns) for pc, op in enumerate(program)]
    ud = [flag_use_def(op) for op in program]
    live_in = [0] * n
    live_out = [0] * n
    changed = True
    while changed:
        changed = False
        for pc in range(n - 1, -1, -1):
            out = 0
            for s in succ[pc]:
                out |= live_in[s]
            use, dfn = ud[pc]
            new_in = use | (out & ~dfn)
            if out != live_out[pc] or new_in != live_in[pc]:
                live_out[pc], live_in[pc] = out, new_in
                changed = True
    return live_out


# Native hot paths (src/dsp1_hle.c): at these loop heads the generated code
# calls the glue in falcon/smkx_upd.s, which runs the C routine on the state
# block and returns the program counter to continue at in d7 (0: the native
# path is off, run the firmware as usual).
from .upd2c import REGIONS  # noqa: E402

HOOKS = {entry: (f"upd_hle_{name}_glue", exits) for name, (entry, exits) in REGIONS.items()}


def hx(v: int) -> str:
    return f"${v:X}"


class Translator:
    def __init__(self, program: list[int]):
        self.program = program
        self.live = liveness(program)
        self.out: list[str] = []

    def emit(self, *lines: str) -> None:
        self.out.extend("\t" + l if not l.endswith(":") else l for l in lines)

    def label(self, pc: int) -> str:
        return f"u{pc:03X}"

    def run(self) -> str:
        self.out = ["; Generated by tools/smkx/upd2m68k.py from assets/dsp1.bin. Do not commit.",
                    "\txdef\tupd_code_table", "\tsection\ttext,code"]
        for pc, op in enumerate(self.program):
            self.out.append(f"{self.label(pc)}:\t\t\t\t; {pc:03X} {disassemble(op)}")
            if pc in HOOKS:
                # the region returns where the firmware continues (an exit, a
                # host poll, a return out of it); 0 = the native path is off
                glue, _ = HOOKS[pc]
                self.emit(f"jsr\t{glue}", "tst.w\td7", f"beq.s\t.run{pc:03X}",
                          "jmp\t([upd_code_table,d7.w*4])")
                self.out.append(f".run{pc:03X}:")
            self.instruction(pc, op)
        self.out += ["\tsection\tdata,data", "\tcnop\t0,4", "upd_code_table:"]
        for pc in range(0, len(self.program), 8):
            self.out.append("\tdc.l\t" + ",".join(self.label(p) for p in range(pc, pc + 8)))
        return "\n".join(self.out) + "\n"

    # -- one instruction ------------------------------------------------------------
    def instruction(self, pc: int, op: int) -> None:
        f = fields(op)
        live = self.live[pc]
        nxt = self.label((pc + 1) & 0x7FF)
        if f["kind"] == 3:
            self.emit(f"move.w\t#{hx(f['imm'])},d7")
            self.dst(f["dst"])
            return
        if f["kind"] == 2:
            self.jump(pc, f)
            return
        self.op(f, live)
        if f["kind"] == 1:
            # return: pop the four-level stack (the bottom entry repeats)
            self.emit("move.w\tupd_stack(a3),d7",
                      "move.l\tupd_stack+2(a3),upd_stack(a3)",
                      "move.w\tupd_stack+6(a3),upd_stack+4(a3)",
                      "jmp\t([upd_code_table,d7.w*4])")

    def jump(self, pc: int, f: dict) -> None:
        brch, na = f["brch"], f["na"]
        target = self.label(na)
        if brch == 0x100:
            self.emit(f"bra\t{target}")
        elif brch == 0x140:
            self.emit("move.l\tupd_stack+2(a3),upd_stack+4(a3)",
                      "move.w\tupd_stack(a3),upd_stack+2(a3)",
                      f"move.w\t#{hx((pc + 1) & 0x7FF)},upd_stack(a3)",
                      f"bra\t{target}")
        elif brch in (0x0BC, 0x0BE):
            want_set = brch == 0x0BE
            if na == pc:
                # a host poll: hand control back until the host moves DR
                self.emit("btst\t#7,upd_sr(a3)",
                          f"{'beq' if want_set else 'bne'}.s\t.go{pc:03X}",
                          f"move.w\t#{hx(pc)},upd_pc(a3)",
                          "jmp\tupd_yield",
                          f".go{pc:03X}:")
            else:
                self.emit("btst\t#7,upd_sr(a3)", f"{'bne' if want_set else 'beq'}\t{target}")
        elif brch in COND:
            flag, want = COND[brch]
            self.emit(f"tst.b\tupd_{flag}(a3)", f"{'bne' if want else 'beq'}\t{target}")
        elif brch in (0x0B0, 0x0B1, 0x0B2, 0x0B3):
            self.emit("move.b\td5,d7", "andi.b\t#$0F,d7")
            if brch in (0x0B2, 0x0B3):
                self.emit("cmpi.b\t#$0F,d7")
            self.emit(f"{'beq' if brch in (0x0B0, 0x0B2) else 'bne'}\t{target}")
        else:
            self.emit(f"move.w\t#{hx(pc)},upd_pc(a3)", "jmp\tupd_trap")

    def src(self, src: int) -> None:
        """IDB -> d7."""
        e = self.emit
        if src == 0: e("move.w\tupd_trb(a3),d7")
        elif src == 1: e("move.w\td0,d7")
        elif src == 2: e("move.w\td1,d7")
        elif src == 3: e("move.w\tupd_tr(a3),d7")
        elif src == 4: e("move.w\td5,d7")
        elif src == 5: e("move.w\td6,d7")
        elif src == 6: e("move.w\t(a1,d6.w*2),d7")
        elif src == 7:
            e("move.w\t#$8000,d7", "tst.b\tupd_a_s1(a3)", "beq.s\t*+4", "subq.w\t#1,d7")
        elif src == 8: e("move.w\tupd_dr(a3),d7", "ori.w\t#$8000,upd_sr(a3)")
        elif src == 9: e("move.w\tupd_dr(a3),d7")
        elif src == 10: e("move.w\tupd_sr(a3),d7")
        elif src in (11, 12): e("moveq\t#0,d7")
        elif src == 13: e("move.w\td2,d7")
        elif src == 14: e("move.w\td3,d7")
        else: e("move.w\t(a0,d5.w*2),d7")

    def dst(self, dst: int) -> None:
        e = self.emit
        if dst == 0: return
        if dst == 1: e("move.w\td7,d0")
        elif dst == 2: e("move.w\td7,d1")
        elif dst == 3: e("move.w\td7,upd_tr(a3)")
        elif dst == 4: e("move.w\td7,d5", "andi.w\t#$FF,d5")
        elif dst == 5: e("move.w\td7,d6", "andi.w\t#$3FF,d6")
        elif dst == 6: e("move.w\td7,upd_dr(a3)", "ori.w\t#$8000,upd_sr(a3)")
        elif dst == 7:
            e(f"andi.w\t#{hx(0xFFFF & ~SR_DSP_WRITABLE)},upd_sr(a3)",
              f"andi.w\t#{hx(SR_DSP_WRITABLE)},d7", "or.w\td7,upd_sr(a3)")
        elif dst in (8, 9): return
        elif dst == 10: e("move.w\td7,d2")
        elif dst == 11: e("move.w\td7,d2", "move.w\t(a1,d6.w*2),d3")
        elif dst == 12:
            e("move.w\td7,d3", "move.w\td5,d4", "ori.w\t#$40,d4", "move.w\t(a0,d4.w*2),d2")
        elif dst == 13: e("move.w\td7,d3")
        elif dst == 14: e("move.w\td7,upd_trb(a3)")
        else: e("move.w\td7,(a0,d5.w*2)")

    def op(self, f: dict, live: int) -> None:
        e = self.emit
        alu, asl, psel = f["alu"], f["asl"], f["psel"]
        needs_idb = f["dst"] != 0 or (alu and psel == 1) or f["src"] in (8,)
        if needs_idb:
            self.src(f["src"])
        if alu:
            # Arithmetic with carry and the overflow update use d7; the
            # destination still needs the bus value.
            keep = f["dst"] != 0 and alu in (4, 5, 6, 7, 8, 9)
            if keep:
                e("move.w\td7,-(sp)")
            self.alu(alu, asl, psel, live)
            if keep:
                e("move.w\t(sp)+,d7")
        self.dst(f["dst"])
        if f["dpl"] == 1:
            e("move.b\td5,d7", "addq.b\t#1,d7", "andi.b\t#$0F,d7", "andi.b\t#$F0,d5", "or.b\td7,d5")
        elif f["dpl"] == 2:
            e("move.b\td5,d7", "subq.b\t#1,d7", "andi.b\t#$0F,d7", "andi.b\t#$F0,d5", "or.b\td7,d5")
        elif f["dpl"] == 3:
            e("andi.b\t#$F0,d5")
        if f["dphm"]:
            e(f"eori.b\t#{hx(f['dphm'] << 4)},d5")
        if f["rpdcr"]:
            e("subq.w\t#1,d6", "andi.w\t#$3FF,d6")

    def operand(self, psel: int) -> str:
        """p -> a data register (d7 already holds IDB)."""
        e = self.emit
        if psel == 1:
            return "d7"
        if psel == 0:
            e("move.w\t(a0,d5.w*2),d4")
            return "d4"
        e("move.w\td2,d4", "muls.w\td3,d4", "add.l\td4,d4")      # d4.w = N, high word = M
        if psel == 2:
            e("swap\td4")
        return "d4"

    def store(self, name: str, cond: str, live: int) -> None:
        if live & BIT[name]:
            self.emit(f"s{cond}\tupd_{name}(a3)")

    def alu(self, alu: int, asl: int, psel: int, live: int) -> None:
        e = self.emit
        x = "ab"[asl]
        acc = ("d0", "d1")[asl]
        other_c = f"upd_{'ab'[1 - asl]}_c(a3)"
        need = lambda f: bool(live & BIT[f"{x}_{f}"])
        ov1_s1 = need("ov1") or need("s1")

        def zero_flags(c_too=True):
            for fl in ("ov0", "ov1") + (("c",) if c_too else ()):
                if need(fl):
                    e(f"clr.b\tupd_{x}_{fl}(a3)")

        if alu in (1, 2, 3):
            p = self.operand(psel)
            op = {1: "or", 2: "and", 3: "eor"}[alu]
            e(f"{op}.w\t{p},{acc}")
            self.store(f"{x}_s0", "mi", live)
            self.store(f"{x}_z", "eq", live)
            zero_flags()
        elif alu in (10, 13, 14, 15):
            if alu == 10:
                e(f"not.w\t{acc}")
            elif alu == 13:
                e(f"lsl.w\t#2,{acc}", f"ori.w\t#3,{acc}")
            elif alu == 14:
                e(f"lsl.w\t#4,{acc}", f"ori.w\t#$F,{acc}")
            else:
                e(f"ror.w\t#8,{acc}")
            e(f"tst.w\t{acc}")
            self.store(f"{x}_s0", "mi", live)
            self.store(f"{x}_z", "eq", live)
            zero_flags()
        elif alu == 11:
            e(f"asr.w\t#1,{acc}")
            self.store(f"{x}_c", "cs", live)
            e(f"tst.w\t{acc}")
            self.store(f"{x}_s0", "mi", live)
            self.store(f"{x}_z", "eq", live)
            zero_flags(False)
        elif alu == 12:
            e(f"tst.b\t{other_c}", "sne\td4", "add.b\td4,d4",          # X = the other accumulator's C
              f"roxl.w\t#1,{acc}")
            self.store(f"{x}_c", "cs", live)
            e(f"tst.w\t{acc}")
            self.store(f"{x}_s0", "mi", live)
            self.store(f"{x}_z", "eq", live)
            zero_flags(False)
        elif alu in (4, 5, 8, 9):
            # ADD/SUB/INC/DEC: the 68k C and V equal the chip's C and OV0.
            if alu in (8, 9):
                e(f"{'subq' if alu == 8 else 'addq'}.w\t#1,{acc}")
            else:
                p = self.operand(psel)
                e(f"{'sub' if alu == 4 else 'add'}.w\t{p},{acc}")
            self.arith_flags(x, acc, live)
        else:
            # ADC/SBB: the chip's C is r < q (add) or r > q (subtract), and
            # OV0 follows its formula; both computed explicitly.
            p = self.operand(psel)
            if p != "d4":
                e(f"move.w\t{p},d4")
            e("move.l\td4,-(sp)", f"move.w\t{acc},-(sp)")            # (sp) = q, 2(sp).w = p
            e(f"tst.b\t{other_c}", "sne\td7", "add.b\td7,d7")         # X = carry in
            e(f"{'addx' if alu == 7 else 'subx'}.w\td4,{acc}")
            # C
            if need("c"):
                e(f"cmp.w\t(sp),{acc}")
                self.store(f"{x}_c", "cs" if alu == 7 else "hi", live)
            # OV0 = (q^r)&(p^r) (add) or (q^r)&(q^p) (subtract), bit 15
            e("move.w\t(sp),d7", f"eor.w\t{acc},d7")                 # q ^ r
            if alu == 7:
                e(f"eor.w\t{acc},d4", "and.w\td4,d7")               # & (p ^ r)
            else:
                e("eor.w\td4,(sp)", "and.w\t(sp),d7")               # & (q ^ p)
            e("addq.l\t#6,sp")
            e("tst.w\td7")
            e("smi\td4")
            if need("ov0") or ov1_s1:
                e(f"move.b\td4,upd_{x}_ov0(a3)")
            e(f"tst.w\t{acc}")
            self.store(f"{x}_s0", "mi", live)
            self.store(f"{x}_z", "eq", live)
            if ov1_s1:
                self.overflow_update(x, acc, from_reg="d4")

    def arith_flags(self, x: str, acc: str, live: int) -> None:
        e = self.emit
        need = lambda f: bool(live & BIT[f"{x}_{f}"])
        ov1_s1 = need("ov1") or need("s1")
        e("move.w\tccr,d4")
        if need("c"):
            e("move.w\td4,ccr")
            self.store(f"{x}_c", "cs", live)
        if need("ov0") or ov1_s1:
            e("move.w\td4,ccr")
            e(f"svs\tupd_{x}_ov0(a3)")
        e("move.w\td4,ccr")
        self.store(f"{x}_s0", "mi", live)
        self.store(f"{x}_z", "eq", live)
        if ov1_s1:
            e("move.w\td4,ccr", "svs\td4")
            self.overflow_update(x, acc, from_reg="d4")

    def overflow_update(self, x: str, acc: str, from_reg: str) -> None:
        """On OV0: S1 = OV1 ^ (result >= 0), then OV1 = !OV1."""
        e = self.emit
        self.n_labels = getattr(self, "n_labels", 0) + 1
        skip = f".nov{self.n_labels}"
        e(f"tst.b\t{from_reg}", f"beq.s\t{skip}")
        e(f"tst.w\t{acc}", "spl\td7", f"move.b\tupd_{x}_ov1(a3),d4", "eor.b\td4,d7",
          f"move.b\td7,upd_{x}_s1(a3)", f"not.b\tupd_{x}_ov1(a3)")
        e(f"{skip}:")


def translate(program: list[int]) -> str:
    text = Translator(program).run()
    glue = ",".join(sorted({g for g, _ in HOOKS.values()}))
    return text.replace("\tsection\ttext,code",
                        "\tinclude\t'upd_equ.i'\n\txref\tupd_yield,upd_trap\n\txref\t" + glue
                        + "\n\tsection\ttext,code", 1)


def equates() -> str:
    lines = [f"upd_{k}\tequ\t{v}" for k, v in ST.items() if k not in ("a", "b", "k", "l", "dp", "rp")]
    lines += [f"upd_{k}_saved\tequ\t{v}" for k, v in ST.items() if k in ("a", "b", "k", "l", "dp", "rp")]
    lines.append(f"upd_state_size\tequ\t{STATE_SIZE}")
    return "\n".join(lines) + "\n"


def main() -> None:
    import shutil
    import sys
    from pathlib import Path
    from .upd7725 import load_firmware
    firmware, outdir = Path(sys.argv[1]), Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)
    program, _ = load_firmware(firmware)
    (outdir / "upd_code.s").write_text(translate(program))
    (outdir / "upd_equ.i").write_text(equates())
    shutil.copyfile(firmware, outdir / "dsp1.bin")


if __name__ == "__main__":
    main()
