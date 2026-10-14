"""Check src/dsp1_native.c against the DSP-1 interpreter (tools/smkx/upd7725.py).

    python3 -m tools.smkx.dsp1_native assets/dsp1.bin [--log build/smkx/DSP1LOG.BIN]
        [--subs N] [--cmds N]

1. Subroutines: random registers and RAM into each native subroutine and
   into the firmware from its entry to its return; the values later code
   reads must agree.
2. Commands: random parameters and RAM through the host interface of the
   interpreter and through dsp1n_exec; results and the whole data RAM must
   agree.
3. A recorded host log (SMKX_DSP1_LOG): replayed with the native commands
   in place of the firmware, through the same host protocol the runtime
   emulates (falcon/smkx_upd.s); every logged read must come out again.

The C file is compiled for the host with DSP1N_TEST (build/libdsp1n.*).
"""

from __future__ import annotations

import argparse
import ctypes
import random
import struct
import subprocess
import sys
from pathlib import Path

from .upd7725 import UPD, Flags, load_firmware, SR_RQM, SR_DRS, SR_DRC

ROOT = Path(__file__).resolve().parents[2]
SDK = "/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk"


class Regs(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint16) for n in ("a", "b", "k", "l", "tr", "trb", "dp", "rp", "ov1", "s1")]


def build() -> ctypes.CDLL:
    out = ROOT / "build" / ("libdsp1n.dylib" if sys.platform == "darwin" else "libdsp1n.so")
    out.parent.mkdir(exist_ok=True)
    cmd = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-shared", "-fPIC", "-DDSP1N_TEST",
           f"-I{ROOT / 'src'}", "-o", str(out), str(ROOT / "src/dsp1_native.c")]
    if sys.platform == "darwin" and Path(SDK).exists():
        cmd[1:1] = ["-isysroot", SDK]
    subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(str(out))
    lib.dsp1n_params.restype = ctypes.c_int
    lib.dsp1n_exec.restype = ctypes.c_int
    return lib


U16x256 = ctypes.c_uint16 * 256
U16x1024 = ctypes.c_uint16 * 1024
U16x16 = ctypes.c_uint16 * 16

# What a caller reads after each subroutine (besides the RAM the checks
# compare as a whole): tools/smkx firmware notes in src/dsp1_native.c.
SUB_OUT = {
    0x03A: ("a", "b", "ov1", "s1"), 0x039: ("a", "b", "ov1", "s1"), 0x042: ("a", "b"),
    0x077: ("a", "ov1", "s1"), 0x087: ("a", "b", "ov1"), 0x0D3: ("a", "b", "s1"),
    0x09F: (), 0x132: ("a", "b"), 0x123: ("a", "b"),
}


def run_sub(upd: UPD, entry: int, limit: int = 100000) -> None:
    upd.stack = [0x7FF, 0x7FF, 0x7FF, 0x7FF]
    upd.pc = entry
    for _ in range(limit):
        if upd.pc == 0x7FF:
            return
        upd.step()
    raise RuntimeError(f"subroutine ${entry:03X} did not return")


def check_subs(lib, program, rom, n: int, rnd: random.Random) -> int:
    crom = U16x1024(*rom)
    bad = 0
    for entry, outs in SUB_OUT.items():
        fails = 0
        for i in range(n):
            regs = {k: rnd.randrange(0x10000) for k in ("a", "b", "k", "l", "tr", "trb")}
            if i % 4 == 0:                   # small values and edges too
                regs["a"] = rnd.choice([0, 1, 0xFFFF, 0x8000, 0x7FFF, 0x4000, 0xC000,
                                        rnd.randrange(-64, 64) & 0xFFFF])
                regs["b"] = rnd.randrange(-40, 40) & 0xFFFF
            ram = [rnd.randrange(0x10000) for _ in range(256)]
            if entry == 0x09F:
                for j in range(3):
                    ram[0xF4 + j] = rnd.randrange(-20, 40) & 0xFFFF
            upd = UPD(program, rom, ram=list(ram))
            upd.a, upd.b, upd.k, upd.l, upd.tr, upd.trb = (regs[k] for k in ("a", "b", "k", "l", "tr", "trb"))
            upd.dp, upd.rp = rnd.randrange(256), rnd.randrange(0x400)
            if entry == 0x09F:               # as project calls it ($3CD-$3CF)
                upd.a, upd.dp = 0, 0xF6
            ov1, s1 = rnd.randrange(2), rnd.randrange(2)
            upd.fa = Flags(ov1=ov1, s1=s1)
            dp0, rp0 = upd.dp, upd.rp
            run_sub(upd, entry)
            t = Regs(regs["a"], regs["b"], regs["k"], regs["l"], regs["tr"], regs["trb"], dp0, rp0, ov1, s1)
            cram = U16x256(*ram)
            lib.dsp1n_test_sub(entry, ctypes.byref(t), cram, crom)
            want = {"a": upd.a, "b": upd.b, "ov1": upd.fa.ov1, "s1": upd.fa.s1}
            diff = [k for k in outs if getattr(t, k) != want[k]]
            if list(cram) != upd.ram:
                diff.append("ram")
            if diff:
                fails += 1
                if fails <= 3:
                    print(f"  ${entry:03X} in a={regs['a']:04X} b={regs['b']:04X}: "
                          + ", ".join(f"{k} {getattr(t, k) if k != 'ram' else '':} want {want.get(k, '')}"
                                      for k in diff))
        print(f"subroutine ${entry:03X}: {n} cases, {fails} differ")
        bad += fails
    return bad


def fresh(program, rom, ram=None) -> UPD:
    upd = UPD(program, rom, ram=list(ram) if ram else [0] * 256)
    upd.run()
    return upd


def check_cmds(lib, program, rom, n: int, rnd: random.Random, cmds) -> int:
    crom = U16x1024(*rom)
    bad = 0
    for cmd in cmds:
        np = lib.dsp1n_params(cmd)
        fails = 0
        for i in range(n):
            ram = [rnd.randrange(0x10000) for _ in range(256)]
            if i % 2:                        # plausible exponents and camera
                for j in range(0xF0, 0x100):
                    ram[j] = rnd.randrange(-8, 24) & 0xFFFF
            params = [rnd.randrange(0x10000) if i % 3 else rnd.randrange(-2000, 2000) & 0xFFFF
                      for _ in range(np)]
            upd = fresh(program, rom, ram)
            cin = (ctypes.c_uint16 * max(np, 1))(*params)
            out = U16x16()
            cram = U16x256(*ram)
            nout = lib.dsp1n_exec(cmd, cin, out, cram, crom)
            want = upd.command(cmd, params, nout)
            got = list(out)[:nout]
            upd.run()                        # to the next command poll
            if got != want or list(cram) != upd.ram:
                fails += 1
                if fails <= 3:
                    rd = [j for j in range(256) if cram[j] != upd.ram[j]]
                    print(f"  ${cmd:02X} {['%04X' % p for p in params]}: out {['%04X' % v for v in got]} "
                          f"want {['%04X' % v for v in want]}; RAM differs at {['%02X' % j for j in rd[:8]]}")
        print(f"command ${cmd:02X}: {n} cases, {fails} differ")
        bad += fails
    return bad


class NativeHost:
    """The host protocol around native commands, as falcon/smkx_upd.s runs
    it: the interpreter until a native command byte arrives at the command
    poll, then parameters collected, dsp1n_exec, results served, and the
    interpreter left at its command poll ($004, B $C0, SR $8400, DR $0080)."""

    def __init__(self, lib, program, rom):
        self.lib, self.upd = lib, fresh(program, rom)
        self.crom = U16x1024(*rom)
        self.idle = self.upd.pc
        self.mode = 0                        # 0 firmware, 1 parameters, 2 results
        self.sr = 0
        self.natives = 0

    def write(self, byte: int) -> None:
        u = self.upd
        if self.mode == 0:
            u.run()
            if u.pc == self.idle and u.sr & SR_DRC and self.lib.dsp1n_params(byte) >= 0:
                self.cmd, self.need = byte, self.lib.dsp1n_params(byte)
                self.params, self.lo = [], None
                self.sr = SR_RQM             # the firmware read it; SR = 0 at $007
                self.natives += 1
                self.mode = 1
                if self.need == 0:
                    self._exec()
                return
            u.host_write_dr(byte)
            return
        if self.mode == 1:
            if self.lo is None:
                self.lo = byte
                self.sr |= SR_DRS
            else:
                self.params.append(self.lo | byte << 8)
                self.lo = None
                self.sr &= ~SR_DRS
                if len(self.params) == self.need:
                    self._exec()
            return
        raise RuntimeError("host write while results are pending")

    def _exec(self) -> None:
        cin = (ctypes.c_uint16 * max(self.need, 1))(*self.params)
        out = U16x16()
        cram = U16x256(*self.upd.ram)
        n = self.lib.dsp1n_exec(self.cmd, cin, out, cram, self.crom)
        self.upd.ram = list(cram)
        self.out, self.oi, self.hi = list(out)[:n], 0, False
        self.mode = 2
        self.sr = SR_RQM
        if not n:
            self._finish()

    def _finish(self) -> None:
        u = self.upd
        u.pc, u.b, u.sr, u.dr = self.idle, 0x00C0, SR_RQM | SR_DRC, 0x0080
        self.mode = 0

    def read_dr(self) -> int:
        if self.mode == 0:
            return self.upd.host_read_dr()
        if self.mode == 1:
            raise RuntimeError("DR read while parameters are pending")
        w = self.out[self.oi]
        if not self.hi:
            self.hi = True
            self.sr |= SR_DRS
            return w & 0xFF
        self.hi = False
        self.sr &= ~SR_DRS
        self.oi += 1
        if self.oi == len(self.out):
            self._finish()
        return w >> 8

    def read_sr(self) -> int:
        if self.mode == 0:
            return self.upd.host_read_sr()
        return self.sr >> 8


def check_log(lib, program, rom, log: Path) -> int:
    raw = log.read_bytes()
    words = struct.unpack(f">{len(raw) // 2}H", raw[: len(raw) // 2 * 2])
    host = NativeHost(lib, program, rom)
    bad = 0
    for i, w in enumerate(words):
        write, sr, byte = w & 0x8000, w & 0x4000, w & 0xFF
        if write:
            host.write(byte)
            continue
        got = host.read_sr() if sr else host.read_dr()
        if got != byte:
            bad += 1
            if bad <= 5:
                print(f"  access {i}: {'SR' if sr else 'DR'} logged ${byte:02X}, native ${got:02X}")
    print(f"log {log.name}: {len(words)} accesses, {host.natives} native commands, {bad} reads differ")
    return bad


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware", type=Path)
    ap.add_argument("--log", type=Path)
    ap.add_argument("--subs", type=int, default=3000)
    ap.add_argument("--cmds", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    program, rom = load_firmware(args.firmware)
    lib = build()
    rnd = random.Random(args.seed)
    cmds = [c for c in range(0x40) if lib.dsp1n_params(c) >= 0]
    bad = check_subs(lib, program, rom, args.subs, rnd) if args.subs else 0
    bad += check_cmds(lib, program, rom, args.cmds, rnd, cmds) if args.cmds else 0
    if args.log:
        bad += check_log(lib, program, rom, args.log)
    print("all equal" if not bad else f"{bad} differences")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
