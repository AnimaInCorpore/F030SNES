"""Run the translated game from boot on a Musashi 68030 model, no Falcon.

    python3 -m tools.ff6x_run build/new --nmis 3000 --every 250

Links a build directory (`make ff6x`-style objects: rt.o, the translated
smk_code_*.o, the C objects) into a flat image at $40000 with vlink, loads it
into the model, replays the runtime's startup without the hardware (banks,
page tables, stacks, the SPC700, SNES reset registers) and runs
`smkx_reset`. Interrupts are delivered by the harness: a V-blank (the
runtime's `rt_vbl`) whenever the game's main loop waits for one (the runtime
sets `rt_idled` at its idle marks and V-blank polls), so the game runs as
fast as the code does and the cycles counted are the work of each frame, not
the wait. Rendering stays off (`FF6X_RENDER=0` in the build).

Used to compare two translations: the WRAM checksum after every N NMIs must
be the same, the cycle counts (Musashi's 68020-style timing: no cache, no
wait states, so relative only) say how much faster one is.

Hardware addresses ($FFFF8xxx, $FFFFFAxx) read as 0 and ignore writes.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import zlib
from pathlib import Path

from tools.emit_bench import find_vasm  # noqa: F401  (same tool discovery)

BASE = 0x40000
HALT = 0x2000                  # `bra.s *`: where a called routine returns to
ROOT = Path(__file__).resolve().parents[1]


def find_vlink() -> str:
    import os
    import shutil
    for cand in (os.environ.get("VLINK"), ROOT.parents[2] / "F030Arcade/third_party/vlink/vlink", shutil.which("vlink")):
        if cand and Path(cand).exists():
            return str(cand)
    raise SystemExit("vlink not found: set $VLINK")


def link_flat(build: Path) -> Path:
    """build/<x>/flat.bin and flat.map from the objects a `make FF6X=build/<x>`
    left there."""
    objs = ["rt.o"] + [n[:-2] + ".o" for n in (build / "sources.txt").read_text().split()] + \
        ["tables.o", "input.o", "snes_ppu.o", "smkx_video.o", "palette.o", "smkx_m7.o", "m7_plan.o",
         "spc700.o", "smkx_apu.o", "smkx_ppu.o", "ppu_dsp_plan.o"]
    out = build / "flat.bin"
    r = subprocess.run([find_vlink(), "-b", "rawbin1", f"-Ttext", hex(BASE), f"-M{build / 'flat.map'}",
                        "-o", str(out)] + [str(build / o) for o in objs], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)
    return out


def symbols(mapfile: Path) -> dict[str, int]:
    syms = {}
    for line in mapfile.read_text().splitlines():
        m = re.match(r"\s+0x([0-9a-f]{8}) (\S+?):", line)
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
    return syms


class Mapper:
    """68030 PC -> what it is: a runtime routine (rt_*, C symbols) or a node of
    a translated 65816 procedure with its 65816 address."""

    def __init__(self, build: Path, syms: dict[str, int]):
        import bisect
        self.bisect = bisect
        labels = {n: a for n, a in syms.items() if re.fullmatch(r"n\d+_\d+", n)}
        addr65, proc = {}, {}
        for src in (build / "sources.txt").read_text().split():
            cur = None
            for line in (build / src).read_text().splitlines():
                if line.startswith("; P"):
                    cur = line[2:].strip()
                m = re.match(r"(n\d+_\d+):\s+; \$([0-9A-F]{6})", line)
                if m:
                    addr65[m.group(1)] = int(m.group(2), 16)
                    proc[m.group(1)] = cur
        self.nodes = sorted((labels[n], n) for n in addr65 if n in labels)
        self.node_addrs = [a for a, _ in self.nodes]
        self.addr65, self.proc = addr65, proc
        self.text_end = max(self.node_addrs)
        rt = sorted((a, n) for n, a in syms.items() if not re.fullmatch(r"[ne]\d+(_\d+)?", n)
                    and not n.startswith(("q", "c", "t", "w", "s", "o")) or n in ("start",))
        self.rt = [(a, n) for a, n in rt if a >= BASE]
        self.rt_addrs = [a for a, _ in self.rt]
        self.first_node = self.node_addrs[0]

    def __call__(self, pc: int):
        """("t", 65816 address, procedure) or ("r", symbol)."""
        if self.first_node <= pc <= self.text_end + 0x4000:
            i = self.bisect.bisect_right(self.node_addrs, pc) - 1
            if i >= 0:
                n = self.nodes[i][1]
                return ("t", self.addr65[n], self.proc[n])
        i = self.bisect.bisect_right(self.rt_addrs, pc) - 1
        return ("r", self.rt[i][1] if i >= 0 else "?")


class CtxRun:
    def __init__(self, build: Path):
        from machine68k import CPUType, Machine, Register as R
        self.R = R
        flat = build / "flat.bin"
        if not flat.exists() or flat.stat().st_mtime < (build / "rt.o").stat().st_mtime:
            link_flat(build)
        self.build = build
        self.sym = symbols(build / "flat.map")
        self.machine = Machine(CPUType.M68030, 16 * 1024)
        self.cpu, self.mem = self.machine.cpu, self.machine.mem
        self.invalid = 0
        self.mem.set_invalid_func(self._invalid)
        self.mem.w_block(0, flat.read_bytes())          # rawbin1 starts at address 0
        self.mem.w16(HALT, 0x60FE)
        self.cycles = 0
        self.last_vbl = 0

    def _invalid(self, mode, width, addr):
        self.invalid += 1
        return 0

    # -- calling and running ----------------------------------------------------------
    def reg(self, name):
        return self.cpu.r_reg(getattr(self.R, name))

    def setreg(self, name, v):
        self.cpu.w_reg(getattr(self.R, name), v & 0xFFFFFFFF)

    def call(self, name: str, sp: int, limit: int = 200_000_000, **regs):
        """Run a routine as a subroutine (return address = HALT)."""
        sp -= 4
        self.mem.w32(sp, HALT)
        self.setreg("A7", sp)
        self.setreg("ISP", sp)
        for k, v in regs.items():
            self.setreg(k, v)
        self.cpu.w_sr(0x2700)
        self.cpu.w_pc(self.sym[name])
        while self.cpu.r_pc() != HALT:
            self.cycles += self.cpu.execute(2000).cycles
            limit -= 2000
            if limit < 0:
                raise RuntimeError(f"{name} did not return (pc {self.cpu.r_pc():X})")

    def boot(self):
        s, mem = self.sym, self.mem
        self.cpu.pulse_reset()
        self.setreg("ISP", 0x30000)
        scratch = 0x30000
        for fn in ("rt_build_banks", "rt_build_pages", "rt_init_stacks"):
            self.call(fn, scratch)
        self.call("smkx_apu_init", scratch)
        mem.w16(HALT + 0x100, 0x4E73)                    # `rte`: the TOS VBL chain the runtime calls last
        mem.w32(s["rt_old_vbl"], HALT + 0x100)
        mem.w32(0x70, s["rt_vbl"])                       # VBL_VECTOR
        mem.w8(s["rt_io"] + 0x2016, 1)                   # a pad in port 1
        mem.w32(s["rt_script_at"], s["smkx_input"])
        wram = s["smkx_wram"]
        self.call("rt_setdbr", scratch, D7=0)
        for r in ("D0", "D1", "D2", "D3"):
            if r != "D3":
                self.setreg(r, 0)
        self.setreg("A5", wram)
        self.setreg("A6", wram + 0x1FF)
        self.setreg("A2", wram)
        self.setreg("D7", 0)
        # the SNES reset state; rt_setdbr left d3/a3/a4 for bank 0 (D3 above
        # is kept: call() ran it on the machine's registers)
        top = s["rt_sstack_top"]
        top -= 4
        mem.w32(top, HALT)
        self.setreg("A7", top)
        self.setreg("ISP", top)
        self.cpu.w_sr(0x2300)
        self.cpu.w_pc(s["smkx_reset"])

    def u32(self, name, off=0):
        return self.mem.r32(self.sym[name] + off)

    def u8(self, name, off=0):
        return self.mem.r8(self.sym[name] + off)

    FRAME_CYCLES = 320_000          # 16 MHz / 50 Hz

    def maybe_vbl(self):
        """A V-blank when the main loop waits for one, or when a frame's worth
        of cycles has passed without one (a game that never idles, or waits
        on something else, still sees the 50 Hz the hardware would give)."""
        if (self.u8("rt_idled") and not self.u8("rt_in_frame")) or \
                self.cycles - self.last_vbl >= self.FRAME_CYCLES:
            # interrupts may be masked at this very instruction (the idle
            # loop raises IPL 7 around its mark): step until they are not,
            # so a chunk size can never alias with such a window
            for _ in range(64):
                if self.vbl():
                    return
                self.cycles += self.cpu.execute(1).cycles

    def vbl(self):
        """Deliver the V-blank as a format-0 exception frame, if unmasked."""
        sr = self.cpu.r_sr()
        if (sr >> 8) & 7 >= 4:
            return False
        self.last_vbl = self.cycles
        sp = self.reg("A7") - 8
        self.mem.w16(sp, sr)
        self.mem.w32(sp + 2, self.cpu.r_pc())
        self.mem.w16(sp + 6, 0x0070)
        self.setreg("A7", sp)
        self.cpu.w_sr((sr | 0x2000) & ~0x0700 & ~0x8000 | 0x0400)
        self.cpu.w_pc(self.mem.r32(0x70))
        self.mem.w32(0x4BA, (self.mem.r32(0x4BA) + 4) & 0xFFFFFFFF)   # _hz_200
        return True

    def profile(self, nmis: int, chunk: int = 300, skip: int = 0):
        """Sample the PC every `chunk` cycles for `nmis` NMIs after `skip`;
        returns (Counter of samples by routine, total samples)."""
        import collections
        m = Mapper(self.build, self.sym)
        tally = collections.Counter()
        proc_tally = collections.Counter()
        total = 0
        target = self.u32("rt_nmis") + skip
        started = False
        while True:
            n = self.u32("rt_nmis")
            if n >= target and not started:
                started, target = True, n + nmis
            if started and n >= target:
                break
            self.maybe_vbl()
            self.cycles += self.cpu.execute(chunk).cycles
            if started:
                kind = m(self.cpu.r_pc())
                tally[kind[:2] if kind[0] == "r" else ("t", kind[2])] += 1
                total += 1
        return tally, total

    def wram_crc(self) -> int:
        w = self.sym["smkx_wram"]
        # the stack page and the runtime's saved registers differ with timing: skip $0000-$01FF? no,
        # the game's stack is deterministic here; the whole 128 KiB counts
        return zlib.crc32(bytes(self.mem.r_block(w, 0x20000)))

    def run_nmis(self, target: int, every: int, chunk: int = 4000, limit_cycles: int = 4_000_000_000):
        """Run until `target` NMIs have run; report (nmis, cycles, crc) each
        `every` NMIs."""
        out = []
        next_report = every
        idle_seen = 0
        start = self.cycles
        while True:
            nmis = self.u32("rt_nmis")
            if nmis >= next_report:
                out.append((nmis, self.cycles - start, self.wram_crc()))
                next_report += every
                if nmis >= target:
                    return out
            # a V-blank when the main loop waits for one (or, failing that,
            # every 8 frames' worth of cycles so a stuck game still moves)
            self.maybe_vbl()
            self.cycles += self.cpu.execute(chunk).cycles
            if self.cycles - start > limit_cycles:
                raise RuntimeError("cycle limit")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("build", type=Path)
    ap.add_argument("--nmis", type=int, default=1000)
    ap.add_argument("--every", type=int, default=250)
    a = ap.parse_args(argv)
    run = CtxRun(a.build)
    run.boot()
    for nmis, cycles, crc in run.run_nmis(a.nmis, a.every):
        print(f"nmis {nmis:6d}  cycles {cycles:12d}  wram crc {crc:08x}")
        sys.stdout.flush()
    print("invalid accesses:", run.invalid)


if __name__ == "__main__":
    main(sys.argv[1:])
