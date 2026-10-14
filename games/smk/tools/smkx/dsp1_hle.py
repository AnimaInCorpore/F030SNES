"""High-level DSP-1 routines, exact against the firmware (tools/smkx/upd7725.py).

The hybrid DSP-1 keeps the chip's own program (translated) for everything
but its hot loops, which run natively and leave the chip in exactly the
state the firmware would: registers, flags, data RAM, program counter. The
Python versions here are the reference for the C/68030 ones
(src/dsp1_hle.c), and dsp1_hle.check() proves them against the interpreter
on a recorded host stream.

Raster ($0A, uPD $35F): each line, from the loop head at $364 to the first
output at $384, is

    Vs'   = Vs + 1                         RAM[$09]
    t     = M(RAM[$49], Vs) + RAM[$C8]     M(k, l) = k*l >> 15, 16 bits
    c, e  = inverse(normalize(t, 7))       $03A, $0D3
    p     = M(RAM[$4C], c)                 TR
    C1    = clip(normalize(p, e + RAM[$CC]))                $077
    C2    = clip(normalize(M(RAM[$4B], p), e + RAM[$CC] + RAM[$CB]))
    RAM[$3F] = RAM[$2F] = C1,  RAM[$3E] = RAM[$2E] = C2
    out1  = M(RAM[$7F], C1)                A at $384

and the firmware then writes out1, M(RAM[$7E], C2), M(RAM[$6F], C1),
M(RAM[$6E], C2) to the host.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from .upd7725 import UPD, Flags, load_firmware

RASTER_LOOP, RASTER_OUT = 0x364, 0x384


def s16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def mul(k: int, l: int) -> tuple[int, int]:
    """The multiplier: M and N from K and L (upd7725.UPD.multiply)."""
    prod = s16(k) * s16(l)
    return (prod >> 15) & 0xFFFF, (prod << 1) & 0xFFFF


class Alu:
    """Flag effects of the ALU ops the hot paths use, per accumulator."""

    @staticmethod
    def add(flag: Flags, q: int, p: int) -> int:
        r = (q + p) & 0xFFFF
        flag.s0, flag.z = r >> 15, int(r == 0)
        flag.ov0 = 1 if (q ^ r) & (p ^ r) & 0x8000 else 0
        flag.c = 1 if r < q else 0
        if flag.ov0:
            flag.s1 = flag.ov1 ^ (0 if r & 0x8000 else 1)
            flag.ov1 ^= 1
        return r

    @staticmethod
    def sub(flag: Flags, q: int, p: int) -> int:
        r = (q - p) & 0xFFFF
        flag.s0, flag.z = r >> 15, int(r == 0)
        flag.ov0 = 1 if (q ^ r) & (q ^ p) & 0x8000 else 0
        flag.c = 1 if r > q else 0
        if flag.ov0:
            flag.s1 = flag.ov1 ^ (0 if r & 0x8000 else 1)
            flag.ov1 ^= 1
        return r

    @staticmethod
    def logic(flag: Flags, r: int) -> int:
        r &= 0xFFFF
        flag.s0, flag.z = r >> 15, int(r == 0)
        flag.c = flag.ov0 = flag.ov1 = 0
        return r

    @staticmethod
    def incdec(flag: Flags, q: int, d: int) -> int:
        return Alu.add(flag, q, 1) if d > 0 else Alu.sub(flag, q, 1)


def run_to(upd: UPD, pc: int, limit: int = 100_000) -> None:
    for _ in range(limit):
        if upd.pc == pc:
            return
        upd.step()
    raise RuntimeError(f"did not reach {pc:#05x}")


def raster_line_reference(upd: UPD) -> None:
    """The firmware itself, from $364 to $384 (for comparison)."""
    upd.step()
    run_to(upd, RASTER_OUT)


def check(firmware: Path, log: Path, hle) -> int:
    """Replay the stream twice, once with `hle` replacing $364..$384, and
    compare every host read and the complete chip state at every line."""
    program, rom = load_firmware(firmware)
    ref, hyb = UPD(program, rom), UPD(program, rom)
    raw = log.read_bytes()
    words = struct.unpack(f">{len(raw) // 2}H", raw[: len(raw) // 2 * 2])
    lines = diffs = 0

    def advance(u: UPD, use_hle: bool) -> None:
        """Run to the next host poll, taking the HLE path at $364."""
        nonlocal lines
        for _ in range(1_000_000):
            if u.waiting():
                return
            if use_hle and u.pc == RASTER_LOOP:
                hle(u)
                lines += 1
                continue
            u.step()
        raise RuntimeError("no host poll")

    def state(u: UPD):
        f = lambda fl: (fl.ov0, fl.ov1, fl.z, fl.c, fl.s0, fl.s1)  # noqa: E731
        return (u.pc, u.a, u.b, u.tr, u.trb, u.k, u.l, u.m, u.n, u.dp, u.rp, u.dr, u.sr,
                tuple(u.stack), f(u.fa), f(u.fb), tuple(u.ram))

    for i, w in enumerate(words):
        for u, h in ((ref, False), (hyb, True)):
            advance(u, h)
        if state(ref) != state(hyb):
            diffs += 1
            if diffs <= 5:
                a, b = state(ref), state(hyb)
                names = "pc a b tr trb k l m n dp rp dr sr stack fa fb ram".split()
                what = [n for n, x, y in zip(names, a, b) if x != y]
                print(f"access {i}: state differs in {what}")
                for n, x, y in zip(names, a, b):
                    if x != y and n != "ram":
                        print(f"    {n}: firmware {x}  hle {y}")
                if "ram" in what:
                    print("    ram:", [(j, hex(x), hex(y)) for j, (x, y) in enumerate(zip(a[-1], b[-1])) if x != y][:6])
            # resynchronise so later differences are reported separately
            hyb.__dict__.update({k: (list(v) if isinstance(v, list) else v) for k, v in ref.__dict__.items()
                                 if k not in ("fa", "fb")})
            hyb.fa, hyb.fb = Flags(**ref.fa.__dict__), Flags(**ref.fb.__dict__)
        write, sr, byte = w & 0x8000, w & 0x4000, w & 0xFF
        for u in (ref, hyb):
            if write:
                u.host_write_dr(byte)
            else:
                got = u.host_read_sr() if sr else u.host_read_dr()
                if got != byte:
                    raise SystemExit(f"access {i}: {'hle' if u is hyb else 'firmware'} read {got:#04x}, log {byte:#04x}")
    print(f"{len(words)} accesses, {lines} raster lines by the HLE, states differing: {diffs}")
    return diffs


def state_words(u: UPD) -> list[int]:
    """The layout tools/dsp1_hle_test.c reads: 27 words, then RAM."""
    fl = lambda f: [0xFF if x else 0 for x in (f.ov0, f.ov1, f.z, f.c, f.s0, f.s1)]  # noqa: E731
    return ([u.tr, u.trb, u.dr, u.sr, u.pc] + list(u.stack) + fl(u.fa) + fl(u.fb)
            + [u.a, u.b, u.k, u.l, u.dp, u.rp] + list(u.ram))


def vectors(firmware: Path, log: Path, out: Path, entry: int = RASTER_LOOP,
            exits: tuple[int, ...] = (RASTER_OUT, 0x392)) -> int:
    """Every pass through a region of the stream: the state at `entry` and
    the firmware's state where it hands back (one of `exits`, or a host
    poll)."""
    program, rom = load_firmware(firmware)
    u = UPD(program, rom)
    raw = log.read_bytes()
    words = struct.unpack(f">{len(raw) // 2}H", raw[: len(raw) // 2 * 2])
    rows = []

    def advance() -> None:
        for _ in range(1_000_000):
            if u.waiting():
                return
            if u.pc == entry:
                before = state_words(u)
                u.step()
                for _ in range(100_000):
                    if u.pc in exits or u.waiting():
                        break
                    u.step()
                rows.append(before + state_words(u))
                continue
            u.step()
        raise RuntimeError("no host poll")

    for w in words:
        advance()
        if w & 0x8000:
            u.host_write_dr(w & 0xFF)
        elif w & 0x4000:
            u.host_read_sr()
        else:
            u.host_read_dr()
    with out.open("wb") as f:
        f.write(struct.pack("<I", len(rows)))
        for r in rows:
            f.write(struct.pack(f"<{len(r)}H", *r))
    print(f"wrote {len(rows)} vectors for ${entry:03X} to {out}")
    return len(rows)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware", type=Path)
    ap.add_argument("log", type=Path)
    ap.add_argument("--reference", action="store_true", help="check the harness with the firmware itself")
    ap.add_argument("--vectors", type=Path, help="write region vectors for tools/dsp1_hle_test.c")
    ap.add_argument("--region", default="raster", help="the region's name in tools/smkx/upd2c.py")
    args = ap.parse_args()
    if args.vectors:
        from .upd2c import REGIONS
        entry, exits = REGIONS[args.region]
        vectors(args.firmware, args.log, args.vectors, entry, exits)
        return
    raise SystemExit(check(args.firmware, args.log, raster_line_reference if args.reference else raster_line) != 0)


OR, AND, XOR, SUB, ADD, DEC, INC, CMP = 1, 2, 3, 4, 5, 8, 9, 10


def alu_a(u: UPD, op: int, p: int) -> None:
    u.a = UPD.alu(op, p, u.a, u.fb.c, u.fa)


def alu_b(u: UPD, op: int, p: int) -> None:
    u.b = UPD.alu(op, p, u.b, u.fa.c, u.fb)


def m_of(u: UPD) -> int:
    return mul(u.k, u.l)[0]


def call(u: UPD, ret: int) -> None:
    u.stack = [ret] + u.stack[:3]


def ret(u: UPD) -> None:
    u.stack = u.stack[1:] + [u.stack[3]]


def dp_inc(u: UPD) -> None:
    u.dp = (u.dp & 0xF0) | ((u.dp + 1) & 0x0F)


def dp_dec(u: UPD) -> None:
    u.dp = (u.dp & 0xF0) | ((u.dp - 1) & 0x0F)


def normalize(u: UPD, ret_to: int) -> None:
    """$03A: A normalized, B -= shifts (B = 0 for A = 0); DP cleared."""
    call(u, ret_to)
    u.dp = 0
    idb = u.a
    alu_a(u, AND, idb)
    u.l = idb
    if u.fa.z:
        alu_b(u, XOR, u.b)                          # 041
    else:
        while True:                                  # 03D-03E
            idb = u.a
            alu_a(u, ADD, idb)
            u.l = idb
            dp_inc(u)
            if u.fa.ov0:
                break
        u.a = u.l                                    # 03F
        dp_dec(u)
        alu_b(u, SUB, u.dp)                          # 040
    u.dp &= 0xF0
    ret(u)


def inverse(u: UPD, ret_to: int) -> None:
    """$0D3: A, B = inverse of the normalized coefficient, exponent 1 - B."""
    call(u, ret_to)
    alu_a(u, AND, u.a)                               # 0D3
    u.dp &= 0xF0
    if u.fa.z:                                       # 0F2-0F4
        u.a, u.b = 0x7FFF, 0x002F
        u.dp &= 0xF0
        ret(u)
        return
    if u.fa.s0:                                      # 0D6-0D9
        alu_a(u, CMP, u.ram[u.dp])
        dp_inc(u)
        alu_a(u, INC, 1)
        if u.fa.ov0:
            alu_a(u, DEC, 1)
    u.rp = 0x64                                      # 0DA
    idb = u.a                                        # 0DB
    alu_b(u, CMP, u.ram[u.dp])
    u.k, u.l = idb, u.rom[u.rp]
    u.rp = (u.rp - 1) & 0x3FF
    idb = u.rom[u.rp]                                # 0DC
    alu_b(u, INC, 1)
    u.a = idb
    u.rp = (u.rp - 1) & 0x3FF
    idb = u.rom[u.rp]                                # 0DD
    alu_a(u, ADD, m_of(u))
    u.tr = idb
    idb = u.a                                        # 0DE
    alu_a(u, XOR, idb)
    u.rp = idb & 0x3FF
    idb = u.rom[u.rp]                                # 0DF
    alu_b(u, INC, 1)
    u.l = idb
    idb = u.k                                        # 0E0
    alu_a(u, SUB, m_of(u))
    u.trb = idb
    alu_a(u, SUB, u.tr)                              # 0E1
    for _ in range(2):                               # 0E2-0E6, 0EA-0EE
        idb = u.a
        alu_a(u, XOR, idb)
        u.k = idb
        idb = u.trb if _ == 0 else None
        alu_a(u, OR, m_of(u))
        if _ == 0:
            u.k = idb
        alu_a(u, ADD, u.a)
        if u.fa.ov0:
            u.a = 0x7FFF
        if _ == 0:                                   # 0E7-0E9
            idb = u.a
            alu_a(u, XOR, idb)
            u.l = idb
            alu_a(u, SUB, m_of(u))
            alu_a(u, SUB, u.tr)
    if u.dp & 0x0F:                                  # 0F0-0F1
        alu_a(u, CMP, u.ram[u.dp])
        alu_a(u, INC, 1)
    u.dp &= 0xF0
    ret(u)


def denormalize(u: UPD, ret_to: int) -> None:
    """$077: normalize, then scale by ROM[$31 + B - 15] or clip."""
    call(u, ret_to)
    normalize(u, 0x078)
    u.rp = 0x0F                                      # 078-079
    alu_b(u, SUB, u.rp)
    if u.fb.s0:                                      # 07B-07F
        u.rp = 0x31
        alu_b(u, ADD, u.rp)
        idb = u.b
        alu_b(u, XOR, idb)
        u.rp = idb & 0x3FF
        idb = u.a
        alu_a(u, XOR, idb)
        u.k, u.l = idb, u.rom[u.rp]
        alu_a(u, ADD, m_of(u))
    elif not u.fb.z:                                 # 081-085
        alu_a(u, AND, u.a)
        u.a = 0x8001 if u.fa.s0 else 0x7FFF
        alu_b(u, XOR, u.b)
    else:                                            # 086
        alu_b(u, XOR, u.b)
    ret(u)


def raster_line(u: UPD) -> None:
    """$364-$383 at value level; leaves the chip as the firmware does at $384."""
    u.dp = 0x09                                      # 364
    u.b = 0x0001                                     # 365
    idb = u.ram[u.dp]                                # 366
    alu_b(u, ADD, idb)
    u.l, u.k = idb, u.ram[(u.dp | 0x40) & 0xFF]
    idb = u.b                                        # 367
    alu_a(u, OR, m_of(u))
    u.ram[u.dp] = idb
    dp_dec(u)
    u.dp ^= 0xC0
    alu_a(u, ADD, u.ram[u.dp])                       # 368
    u.b = 0x0007                                     # 369
    normalize(u, 0x36B)                              # 36A
    inverse(u, 0x36C)                                # 36B
    u.dp = 0x4C                                      # 36C
    idb = u.a                                        # 36D
    alu_a(u, XOR, idb)
    u.l, u.k = idb, u.ram[(u.dp | 0x40) & 0xFF]
    u.dp ^= 0x80
    alu_b(u, ADD, u.ram[u.dp])                       # 36E
    idb = u.b                                        # 36F
    alu_a(u, OR, m_of(u))
    u.trb = idb
    u.tr = u.a                                       # 370
    denormalize(u, 0x372)                            # 371
    u.dp = 0x3F                                      # 372
    u.ram[u.dp] = u.a                                # 373
    u.dp ^= 0x10
    u.ram[u.dp] = u.a                                # 374
    u.dp = 0x4B                                      # 375
    u.a = u.tr                                       # 376
    idb = u.a                                        # 377
    alu_a(u, XOR, idb)
    u.l, u.k = idb, u.ram[(u.dp | 0x40) & 0xFF]
    u.dp ^= 0x80
    idb = u.trb                                      # 378
    alu_a(u, OR, m_of(u))
    u.b = idb
    alu_b(u, ADD, u.ram[u.dp])                       # 379
    denormalize(u, 0x37B)                            # 37A
    u.dp = 0x3E                                      # 37B
    u.ram[u.dp] = u.a                                # 37C
    u.dp ^= 0x10
    idb = u.a                                        # 37D
    alu_a(u, XOR, idb)
    u.ram[u.dp] = idb
    u.dp = 0x46                                      # 37E
    u.dp = u.ram[u.dp] & 0xFF                        # 37F
    if u.dp & 0x0F:                                  # 380: the other output path
        u.pc = 0x392
        return
    u.dp = 0x3F                                      # 381
    idb = u.ram[u.dp]                                # 382
    u.l, u.k = idb, u.ram[(u.dp | 0x40) & 0xFF]
    dp_dec(u)
    idb = u.ram[u.dp]                                # 383
    alu_a(u, OR, m_of(u))
    u.l, u.k = idb, u.ram[(u.dp | 0x40) & 0xFF]
    dp_inc(u)
    u.dp ^= 0x10
    u.m, u.n = mul(u.k, u.l)
    u.pc = RASTER_OUT


if __name__ == "__main__":
    main()
