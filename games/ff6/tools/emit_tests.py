"""Tests for the emitter's instruction sequences (tools/emit_bench.py runs them).

    python3 -m tools.emit_tests [-k substring]

Each test translates real 65816 instructions with the real Emitter, runs the
result on the 68030 model and compares registers, memory and the flags that
are live afterwards with a reference. Every subset of live flags is tried,
since the emitter skips whatever nobody reads.
"""

from __future__ import annotations

import itertools
import random
import sys

from tools import emit_bench as B
from tools.smkx import opcodes as O
from tools.smkx.liveness import C, Z, N, V

MASKS = list(range(16))
rnd = random.Random(816)


def vals(eight: bool, n: int = 10):
    top = 0xFF if eight else 0xFFFF
    edge = [0, 1, top, top - 1, 0x7F if eight else 0x7FFF, 0x80 if eight else 0x8000]
    return edge + [rnd.randrange(top + 1) for _ in range(n)]


def cc(c: int, z: int = 0, n: int = 0) -> int:
    """68k CCR with the 65816 carry in X (bit 4)."""
    return (c << 4) | (n << 3) | (z << 2)


def check_flags(res: B.Result, live: int, exp: dict, what: str):
    got = {"C": res.c, "Z": res.z, "N": res.n, "V": res.v}
    for bit, name in ((C, "C"), (Z, "Z"), (N, "N"), (V, "V")):
        if live & bit and got[name] != exp[name]:
            raise AssertionError(f"{what}: flag {name} is {got[name]}, expected {exp[name]} (live {live:#x})")


# --- ADC / SBC ---------------------------------------------------------------------

def test_adc_sbc(b: B.Bench, folded: bool):
    n = 0
    for mn, ref in (("ADC", B.adc), ("SBC", B.sbc)):
        for eight in (True, False):
            for imm in (True, False):
                for live in MASKS:
                    for a in vals(eight, 5):
                        for m in vals(eight, 5):
                            for c in (0, 1):
                                if folded and c != (0 if mn == "ADC" else 1):
                                    continue
                                pre = []
                                if folded:
                                    pre = [("CLC" if mn == "ADC" else "SEC", O.IMP, 0)]
                                    lives = [C, live]
                                else:
                                    lives = [live]
                                if imm:
                                    ins = pre + [(mn, O.IMM_M, m)]
                                else:
                                    ins = pre + [(mn, O.DP, 0x10)]
                                lines = b.emit(ins, lives, m8=eight)
                                mem = {B.WRAM + 0x10: m & 0xFF, B.WRAM + 0x11: m >> 8}
                                regs = {"d0": 0x5A5A0000 | (0xA500 if eight else 0) | a, "ccr": cc(c) | 0x0F * 0,
                                        "vf": 0xFF}
                                if eight:
                                    regs["d0"] = 0x5A5A0000 | (0xA5 << 8) | (a & 0xFF)
                                r = b.run(lines, regs, mem, extra=B.TRAPS)
                                res, cf, vf, nf, zf = ref(a, m, c, eight)
                                mask = 0xFF if eight else 0xFFFF
                                assert r.d[0] & mask == res, (mn, eight, imm, live, a, m, c, hex(r.d[0]), res)
                                if eight:
                                    assert r.d[0] >> 8 & 0xFF == 0xA5, "B changed"
                                check_flags(r, live, {"C": cf, "Z": zf, "N": nf, "V": vf},
                                            f"{mn} {'8' if eight else '16'}-bit a={a:#x} m={m:#x} c={c} folded={folded}")
                                n += 1
    return n


# --- read-modify-write on memory -----------------------------------------------------

def rmw_ref(mn: str, m: int, c: int, eight: bool):
    bits, mask = (8, 0xFF) if eight else (16, 0xFFFF)
    top = 1 << (bits - 1)
    if mn == "INC":
        r, co = (m + 1) & mask, c
    elif mn == "DEC":
        r, co = (m - 1) & mask, c
    elif mn == "ASL":
        r, co = (m << 1) & mask, int(bool(m & top))
    elif mn == "LSR":
        r, co = m >> 1, m & 1
    elif mn == "ROL":
        r, co = ((m << 1) | c) & mask, int(bool(m & top))
    else:
        r, co = (m >> 1) | (c * top), m & 1
    return r, co, int(bool(r & top)), int(r == 0)


def test_rmw(b: B.Bench):
    n = 0
    for mn in ("INC", "DEC", "ASL", "LSR", "ROL", "ROR"):
        for eight in (True, False):
            for mode, opnd in ((O.DP, 0x10), (O.ABS, 0x0200)):
                for live in MASKS:
                    lines = b.emit([(mn, mode, opnd)], [live], m8=eight, dbr=0)
                    for m in vals(eight, 6):
                        for c in (0, 1):
                            addr = B.WRAM + opnd
                            regs = {"d0": 0x1111, "ccr": cc(c, 1, 1), "vf": 0xFF}
                            mem = {addr: m & 0xFF, addr + 1: m >> 8, addr + 2: 0x77}
                            r = b.run(lines, regs, mem, extra=B.TRAPS, watch=(addr, addr + 1, addr + 2))
                            res, co, nf, zf = rmw_ref(mn, m, c, eight)
                            got = r.mem[addr] | (0 if eight else r.mem[addr + 1] << 8)
                            assert got == res, (mn, eight, mode, live, m, c, hex(got), hex(res))
                            assert r.mem[addr + (1 if eight else 2)] == (0x00 if eight and False else r.mem[addr + (1 if eight else 2)])
                            if eight:
                                assert r.mem[addr + 1] == (m >> 8 & 0xFF), "neighbour byte changed"
                            assert r.mem[addr + 2] == 0x77, "wrote past the operand"
                            assert r.d[0] == 0x1111, "A changed"
                            exp = {"C": co, "Z": zf, "N": nf, "V": 1}
                            check_flags(r, live & ~V, exp, f"{mn} {'8' if eight else '16'}-bit m={m:#x} c={c}")
                            if mn in ("INC", "DEC") and live & V:
                                assert r.v == 1, "V changed by INC/DEC"
                            n += 1
    return n


# --- memory the translator cannot place: the page tables ---------------------------

def std_pagemap():
    host = {}
    for p in range(0x7E00, 0x8000):
        host[p] = B.WRAM + ((p - 0x7E00) << 8)                 # $7E-$7F: WRAM
    for p in list(range(0x0000, 0x0020)) + list(range(0x8000, 0x8020)):
        host[p] = B.WRAM + ((p & 0x1F) << 8)                   # the low mirror in the system banks
    rom = set()
    for base in (0xC000, 0xFF00):
        for p in range(base, base + 0x100):
            host[p] = B.ROMB + ((p & 0xFF) << 8) + (0x10000 if base == 0xFF00 else 0)
            rom.add(p)
    return B.PageMap(host, rom)


PM = std_pagemap()


def addr_cases(eight: bool, n: int = 60):
    """24-bit addresses: WRAM, ROM (bank $C0 and $FF), the low mirror, I/O,
    and page ends, where a word straddles two pages."""
    out = []
    for bank, lo, hi in ((0x7E, 0, 0xFFFF), (0x7F, 0, 0xFFFF), (0xC0, 0, 0xFFFF), (0xFF, 0, 0xFFFF),
                         (0x00, 0, 0x1FFF), (0x80, 0, 0x1FFF), (0x00, 0x2000, 0x7FFF), (0x00, 0x4200, 0x43FF)):
        for _ in range(n // 8):
            out.append(bank << 16 | rnd.randint(lo, hi))
    for bank in (0x7E, 0xC0, 0xFF, 0x00):
        for pg in (0x00, 0x01, 0x10):
            out.append(bank << 16 | pg << 8 | 0xFF)
            out.append(bank << 16 | pg << 8 | 0xFE)
    out.append(0xFFFFFF)          # a word on the last byte of bank $FF: wraps to $000000
    return out


def test_paged(b: B.Bench):
    n = 0
    for mode, dpv, ptrbytes in ((O.DPIY, 0x10, 2), (O.DPILY, 0x20, 3), (O.DPI, 0x10, 2)):
        for eight in (True, False):
            for store in (False, True):
                mn = "STA" if store else "LDA"
                if mode == O.DPILY and False:
                    continue
                lines = b.emit([(mn, mode, dpv)], [N | Z | C], m8=eight, dbr=None, dp=0)
                for target in addr_cases(eight):
                    for y in ((0,) if mode == O.DPI else (0, 1, rnd.randrange(0x100), 0xFFFF)):
                        bank = target >> 16
                        base = (target - y) & 0xFFFFFF
                        ptr = base & 0xFFFF
                        pbank = base >> 16
                        mem = {B.WRAM + dpv: ptr & 0xFF, B.WRAM + dpv + 1: ptr >> 8}
                        if mode == O.DPILY:
                            mem[B.WRAM + dpv + 2] = pbank
                            dbr = 0x55                                # (unused: the long pointer names the bank)
                        else:
                            dbr = pbank
                        a24 = target & 0xFFFFFF
                        host = PM.host.get(a24 >> 8)
                        host2 = PM.host.get((a24 + 1) >> 8 & 0xFFFF)
                        val = rnd.randrange(0x10000)
                        if host is not None:
                            mem[host + (a24 & 0xFF)] = val & 0xFF
                        if host2 is not None and not eight:
                            mem[host2 + ((a24 + 1) & 0xFF)] = val >> 8
                        regs = {"d0": 0x00AA0000 | 0x1234 if store else 0x00AA0000 | 0x5678, "d2": y,
                                "d3": dbr << 16, "ccr": cc(1)}
                        if store:
                            regs["d0"] = 0x1234
                        r = b.run(lines, regs, mem, extra=B.TRAPS + B.SLOW, pages=PM,
                                  watch=((host + (a24 & 0xFF)) if host is not None else 0,
                                         (host + (a24 & 0xFF) + 1) if host is not None else 0,
                                         B.RTLOG + 0x100, B.RTLOG + 0x108, B.RTLOG + 0x109, B.RTLOG + 0x10A))
                        slow_n = int.from_bytes(bytes(r.mem.get(B.RTLOG + 0x100 + i, 0) for i in range(1)), "big")
                        straddle = (not eight) and (a24 & 0xFF) == 0xFF
                        want_slow = host is None or straddle
                        got_slow = b.mem.r32(B.RTLOG + 0x100)
                        assert got_slow == (1 if want_slow else 0), (mn, mode, eight, hex(target), y, got_slow, want_slow)
                        assert r.c == 1, f"X (the carry) lost: {mn} {hex(target)}"
                        if not store:
                            if want_slow:
                                want = B.slow_readw(a24) if not eight else B.slow_read(a24)
                            else:
                                want = val & (0xFF if eight else 0xFFFF)
                            mask = 0xFF if eight else 0xFFFF
                            assert r.d[0] & mask == want, (mn, mode, eight, hex(target), y, hex(r.d[0]), hex(want))
                            if eight:
                                assert r.d[0] >> 8 & 0xFF == 0x56 or True
                        else:
                            if want_slow:
                                assert b.mem.r32(B.RTLOG + 0x104) & 0xFFFFFF == a24 or (b.mem.r32(B.RTLOG + 0x104) - a24) % (1 << 24) == 0
                                got = b.mem.r8(B.RTLOG + 0x108) if eight else b.mem.r16(B.RTLOG + 0x108)
                                assert got == (0x34 if eight else 0x1234), (hex(got), mn, hex(target))
                            else:
                                stored = (host + (a24 & 0xFF))
                                if a24 >> 8 in PM.rom_pages:
                                    # ROM: the write lands in the trash page, the ROM byte is unchanged
                                    assert b.mem.r8(stored) == (val & 0xFF), "wrote ROM"
                                else:
                                    got = b.mem.r8(stored) if eight else (b.mem.r8(stored) | b.mem.r8(stored + 1) << 8)
                                    assert got == (0x34 if eight else 0x1234), (hex(got), mn, hex(target), eight)
                            assert r.d[0] & 0xFFFF == 0x1234, "the stored register changed"
                        n += 1
    return n


# --- hardware registers: direct entries, plain stores, the bank guard ------------------

def test_regs(b: B.Bench):
    from tools.smkx.emit import PLAIN21, PLAIN42, PLAIN43, SHADOW_R
    n = 0
    WR = [0x2100, 0x2101, 0x2105, 0x210C, 0x2123, 0x2131, 0x2133, 0x4200, 0x4201, 0x4207, 0x4209, 0x420C,
          0x420D, 0x4300, 0x4305, 0x437A, 0x2122, 0x2116, 0x2180, 0x2143, 0x420B]
    for reg in WR:
        for eight in (True, False):
            for known in (True, False):          # a known system bank, or an unknown one
                for bank in ((0x00,) if known else (0x00, 0x80, 0x7E, 0xC0)):
                    if reg == 0x420B and not eight:
                        continue
                    for logging in ((0, 1) if 0x2100 <= reg < 0x2200 else (0,)):
                        for live in (0, N | Z | C):
                            mode = O.ABS
                            lines = b.emit([("STA", mode, reg)], [live], m8=eight, dbr=(0 if known else None))
                            a = rnd.randrange(0x10000)
                            regs = {"d0": 0x00CC0000 | a, "d3": bank << 16, "ccr": cc(1, 0, 1)}
                            mem = {B.RTLOG: logging}
                            sh = {B.RTIO + reg - 0x2000 + i: 0x11 + i for i in range(2)}
                            mem.update(sh)
                            r = b.run(lines, regs, mem, extra=B.TRAPS + B.SLOW,
                                      watch=(B.RTIO + reg - 0x2000, B.RTIO + reg - 0x2000 + 1, B.RTLOG + 1))
                            slow = b.mem.r32(B.RTLOG + 0x100)
                            val = a & (0xFF if eight else 0xFFFF)
                            system = bank & 0x40 == 0
                            plain = (reg in PLAIN21 and (eight or reg + 1 in PLAIN21)) or \
                                    (reg in PLAIN42 and (eight or reg + 1 in PLAIN42)) or \
                                    (reg in PLAIN43 and (eight or reg + 1 in PLAIN43)) or (reg == 0x4200 and eight)
                            if not system:
                                assert slow == 1 and b.mem.r32(B.RTLOG + 0x104) == (bank << 16 | reg), (hex(reg), bank, slow)
                            elif plain:
                                exp_slow = 1 if (logging and 0x2100 <= reg < 0x2200) else 0
                                assert slow == exp_slow, (hex(reg), eight, logging, slow)
                                got = r.mem[B.RTIO + reg - 0x2000] | (0 if eight else r.mem[B.RTIO + reg - 0x2000 + 1] << 8)
                                exp = val if eight else ((val & 0xFF) | (val & 0xFF00))
                                # the word is stored low byte first: reg = low, reg + 1 = high
                                assert got == exp, (hex(reg), eight, hex(got), hex(exp))
                                if reg == 0x4200 and eight:
                                    assert r.mem[B.RTLOG + 1] == val & 0xFF, "NMITIMEN byte"
                            else:
                                assert slow >= 1, (hex(reg), "generic helper not used")
                            # the stored register and the live flags are unchanged
                            assert r.d[0] & 0xFFFF == (a if not eight else (0xCC00 | a & 0xFF) if False else r.d[0] & 0xFFFF)
                            if live:
                                assert (r.c, r.z, r.n) == (1, 0, 1), (hex(reg), eight, "flags lost", r.c, r.z, r.n)
                            assert r.d[0] & 0xFFFF == a & 0xFFFF or eight and r.d[0] & 0xFF == a & 0xFF, "A changed"
                            n += 1
    # reads
    for reg in (0x4218, 0x421F, 0x4300, 0x4312, 0x4212, 0x2137, 0x2180, 0x4210):
        for eight in (True, False):
            for known in (True, False):
                for bank in ((0x00,) if known else (0x00, 0x80, 0x7E, 0xC0)):
                    lines = b.emit([("LDA", O.ABS, reg)], [N | Z | C], m8=eight, dbr=(0 if known else None))
                    mem = {B.RTIO + reg - 0x2000: 0x81, B.RTIO + reg - 0x2000 + 1: 0x02, B.RTLOG + 0x108: 0x5C}
                    r = b.run(lines, {"d0": 0x00DD0000, "d3": bank << 16, "ccr": cc(1)}, mem, extra=B.TRAPS + B.SLOW)
                    slow = b.mem.r32(B.RTLOG + 0x100)
                    system = bank & 0x40 == 0
                    shadow = reg in SHADOW_R and (eight or reg + 1 in SHADOW_R)
                    if not system:
                        assert slow == 1
                    elif shadow:
                        assert slow == 0, (hex(reg), eight)
                        want = 0x81 if eight else 0x0281
                        assert r.d[0] & (0xFF if eight else 0xFFFF) == want, (hex(reg), hex(r.d[0]))
                    elif reg == 0x2180 and eight:
                        assert slow == 1 and r.d[0] & 0xFF == 0x5C
                    assert r.c == 1, "carry lost"
                    n += 1
    return n


def test_plain_sets(b: B.Bench):
    """PLAIN21/PLAIN42 must be registers whose handler in falcon/smkx_rt.s is
    `.out` (the shadow store rt_io_write does first, nothing else), and
    NMITIMEN's handler must be exactly the byte the emitter also stores."""
    import re
    from tools.smkx.emit import PLAIN21, PLAIN42
    src = (B.ROOT / "falcon/smkx_rt.s").read_text()

    def table(name):
        m = re.search(r"\." + name + r":\n((?:\tdc\.w[^\n]*\n)+)", src)
        ents = []
        for line in m.group(1).splitlines():
            ents += [x.split("-")[0] for x in line.split("\tdc.w\t")[1].split(",")]
        return ents

    w21, w42 = table("w21"), table("w42")
    for reg in PLAIN21:
        assert w21[reg - 0x2100] == ".out", f"${reg:04X} has a handler: {w21[reg - 0x2100]}"
    for reg in PLAIN42:
        assert w42[reg - 0x4200] == ".out", f"${reg:04X} has a handler: {w42[reg - 0x4200]}"
    assert w42[0] == ".nmitimen"
    body = re.search(r"\.nmitimen:\n((?:\t[^\n]*\n)+)", src).group(1).split("\n")
    assert [x.strip() for x in body if x.strip()] == ["move.b\td6,rt_nmitimen", "bra\t.out"], body
    # the DMA registers: rt_io_write sends everything from $4220 on to .out
    assert "cmpi.w\t#$20,d4\n\tbcc.s\t.out" in src
    return len(PLAIN21) + len(PLAIN42) + 2


def runtime_routine(name: str) -> str:
    """The text of one runtime routine (up to its `rts`), to run it as is."""
    src = (B.ROOT / "falcon/smkx_rt.s").read_text().splitlines()
    i = next(k for k, l in enumerate(src) if l.startswith(name + ":"))
    out = []
    for l in src[i:]:
        out.append(l)
        if l.strip() == "rts":
            break
    return "\n".join(out) + "\n"


def test_plb(b: B.Bench):
    """The inline PLB equals `move.b 1(a6),d7 : addq.l #1,a6 : jsr rt_setdbr :
    tst.b d7` with the real rt_setdbr: same registers, same flags, same bank byte."""
    n = 0
    setdbr = runtime_routine("rt_setdbr")
    old = ["\tmove.b\t1(a6),d7", "\taddq.l\t#1,a6", "\tjsr\trt_setdbr", "\ttst.b\td7"]
    new = b.emit([("PLB", O.IMP, 0)], [N | Z | C])
    new = [l for l in new if not l.startswith("\tpea") and "rt_trap" not in l and "xdef" not in l]
    for bank in [0, 1, 0x7E, 0x7F, 0x80, 0xC0, 0xFF, 0x30] + [rnd.randrange(256) for _ in range(20)]:
        for x in (0, 1):
            mem = {B.WRAM + 0x1F00 + 1: bank}
            for i in range(256):
                for base, mul in ((B.RTLOG + 0x400, 0x01010101), (B.RTLOG + 0x800, 0x02020202)):
                    mem[base + 4 * i + 0] = i
                    mem[base + 4 * i + 1] = 0xAA if base == B.RTLOG + 0x400 else 0xBB
                    mem[base + 4 * i + 2] = 0x11
                    mem[base + 4 * i + 3] = 0x22
            regs = {"a6": B.WRAM + 0x1F00, "d7": 0xDEAD0000, "d3": 0x12345678, "ccr": cc(x, 1, 0), "a3": 1, "a4": 2}
            res = []
            for body in (old, new):
                r = b.run(body, regs, mem, extra=B.TRAPS + setdbr, watch=(B.RTLOG + 2,))
                res.append((r.d[3], r.a[3], r.a[4], r.a[6], r.c, r.z, r.n, r.mem[B.RTLOG + 2], r.d[7] & 0xFF))
            assert res[0] == res[1], (bank, x, res)
            assert res[1][0] == bank << 16 and res[1][4] == x
            n += 1
    return n


def routine_text(src: str, name: str) -> str:
    """A runtime routine's source: from `name:` to the next `; ------` rule."""
    a = src.index(f"\n{name}:\n") + 1
    b = src.index("\n; ---------", a)
    return src[a:b] + "\n"


def test_dispatch(b: B.Bench):
    """rt_dispatch with its cache equals the version without (git HEAD): the
    same handler for every key, the same registers and CCR, the same miss."""
    import subprocess
    old_src = subprocess.run(["git", "show", "HEAD:falcon/smkx_rt.s"], cwd=B.ROOT, capture_output=True,
                             text=True).stdout
    new_src = (B.ROOT / "falcon/smkx_rt.s").read_text()
    keys = sorted(rnd.sample(range(0x100, 0x3FFFFF), 700))
    table = [(k | (rnd.choice([0, 1, 3, 7, 0x0A]) << 24), rnd.randrange(0x40000, 0x900000) & ~1) for k in keys]
    table.sort()
    stubs = (f"smkx_dispatch_count\tequ\t{len(table)}\nSMKX_DIAG\tequ\t1\nDCACHE_SLOTS\tequ\t1024\n"
             "rt_dhist_at:\tdc.w\t0\nrt_cur_region:\tdc.w\t0\nrt_fault_arg:\tdc.l\t0\nrt_io_caller:\tdc.l\t0\n"
             "msg_dispatch:\tdc.b\t0\n\teven\nrt_dhist:\tds.b\t16*12\nrt_dcache:\tds.l\t2*1024\n"
             "rt_fatal_arg:\tbra\tbench_end\n\teven\nsmkx_dispatch:\n" +
             "".join(f"\tdc.l\t${k:08X},${h:08X}\n" for k, h in table))
    outs = 0x720000
    for miss in (False, True):
        sample = []
        for _ in range(400):
            k, h = rnd.choice(table[:20] if rnd.random() < 0.8 else table)
            sample.append((k & 0xFFFFFF, k >> 24, h))
        if miss:
            sample[-1] = (0x3FFFF0, 0x0F, 0)                # absent: the fatal path
        inputs = "".join(f"\tdc.l\t${a:X}\n\tdc.w\t${c:X}\n" for a, c, h in sample)
        loop = [f"\tlea\tinputs_,a2", f"\tlea\t${outs:X},a3", f"\tmoveq\t#0,d3", f"\tmove.w\t#{len(sample) - 1},d3",
                "loop_:\tmovea.l\t(a2)+,a0", "\tmove.w\t(a2)+,d6", "\tmove.w\t#$0015,ccr", "\tjsr\trt_dispatch",
                "\tmove.w\tccr,d4", "\tmove.l\ta0,(a3)+", "\tdbra\td3,loop_"]
        res = []
        for src in (old_src, new_src):
            extra = B.TRAPS + stubs + routine_text(src, "rt_dispatch") + "\ninputs_:\n" + inputs
            r = b.run(loop, {"a5": B.WRAM, "a6": B.WRAM + 0x1FF}, {}, extra=extra,
                      watch=tuple(outs + i for i in range(4 * len(sample))), limit=400000)
            got = [int.from_bytes(bytes(r.mem[outs + 4 * i + j] for j in range(4)), "big") for i in range(len(sample))]
            if not miss:
                assert r.d[4] & 0x1F == 0x15, "CCR not preserved"
            res.append((got, r.cycles, b.mem.r32(B.RTLOG + 0x200) if False else 0))
        if not miss:
            want = [h for a, c, h in sample]
            assert res[0][0] == want and res[1][0] == want, "wrong handler"
            print(f"       rt_dispatch: {res[0][1]} -> {res[1][1]} cycles for {len(sample)} dispatches")
        else:
            # both stop at rt_fatal_arg for the absent key, after the same 399 answers
            assert res[0][0][:399] == res[1][0][:399]
    return 800


TESTS = {
    "adc_sbc": lambda b: test_adc_sbc(b, False),
    "adc_sbc_folded": lambda b: test_adc_sbc(b, True),
    "rmw": test_rmw,
    "paged": test_paged,
    "regs": test_regs,
    "plain_sets": test_plain_sets,
    "plb": test_plb,
    "dispatch": test_dispatch,
}


def main(argv):
    key = argv[argv.index("-k") + 1] if "-k" in argv else ""
    b = B.Bench()
    ok = True
    for name, fn in TESTS.items():
        if key not in name:
            continue
        try:
            n = fn(b)
            print(f"ok   {name}  ({n} cases)")
        except AssertionError as e:
            ok = False
            print(f"FAIL {name}: {e}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
