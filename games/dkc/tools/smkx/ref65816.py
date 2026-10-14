"""A reference 65816 interpreter with the Falcon runtime's hardware model.

It runs the user's ROM from reset the way DKCX.TOS does, but instruction by
instruction: the same bank mapping, the same I/O shadow and port behaviour
(PPU ports, DMA, multiply/divide, WRAM port, pads), the same APU handshake
model (falcon/smkx_rt.s, rt_apu_write, with DKC's line) and the same NMI
taken at WAI (rt_wai). Where the runtime depends on time (a VBL setting the
$4210 flag), the interpreter never sets it; the rest is deterministic, so
WRAM at a game-frame boundary must equal the Falcon's DUMP.BIN there
(tools/smkx/refcmp.py). DMA is the plain byte-by-byte transfer, which the
runtime's fast paths must reproduce.

Game frames are counted as the runtime counts them: frame handlers the
NMI starts (rt_nmi_frames). Scripted pad input is keyed by that count.
"""

from __future__ import annotations

import struct

# NMI of DKC (CODE_80A96C) as rt_wai performs it
NMI_LAG, NMI_RETURN, INIDISP_COPY = 0xA980, 0xA98A, 0x051A

DMA_PATTERNS = [(0, 0, 0, 0), (0, 1, 0, 1), (0, 0, 0, 0), (0, 0, 1, 1),
                (0, 1, 2, 3), (0, 1, 0, 1), (0, 0, 0, 0), (0, 0, 1, 1)]
VMINC = (1, 32, 128, 128)


class Stop(Exception):
    pass


class Machine:
    def __init__(self, rom: bytes, script=None, dispatch=None):
        self.rom = rom
        self.rom_mask = len(rom) - 1
        self.wram = bytearray(0x20000)
        self.sram = bytearray(0x800)
        self.vram = bytearray(0x10000)
        self.cgram = bytearray(512)
        self.oam = bytearray(544)
        self.io = bytearray(0x6000)          # shadow of $2000-$7FFF
        self.oamadd = self.vmadd = self.cgadd = 0
        self.vminc, self.vminc_high = 1, False
        self.oamlatch = self.cglatch = 0
        self.vprefetch = 0                      # VRAM read prefetch (rt_vprefetch)
        self.cgflip = False
        self.m7old = self.bgold = 0
        self.m7a = self.m7b = 0
        self.mpy = 0
        self.wmadd = 0
        self.nmitimen = 0
        self.rdnmi = False
        self.timeup = False
        self.hvb_toggle = 0
        self.apu_out = bytearray(b"\xAA\xBB\x00\x00")
        self.apu_in = bytearray(4)
        self.apu_state = 0
        self.apu_index = 0
        self.script = sorted(script or [])  # (game frame, pad word)
        self.script_i = 0
        self.script_pad = 0
        self.game_frames = 0
        self.dispatch = dispatch            # optional: check handler targets
        self.watch = {}                     # address -> callback(addr, old, new, cpu)
        # CPU
        self.a = self.x = self.y = 0
        self.s = 0x1FF
        self.d = 0
        self.dbr = 0
        self.pbr = 0
        self.pc = 0
        self.p = 0x34                       # M X I
        self.e = True
        self.steps = 0
        self.frame_hook = None              # called with the game frame number
        self.pc_hook = {}                   # 24-bit PC -> callback(cpu)

    # ---- bus --------------------------------------------------------------
    def read(self, addr: int) -> int:
        bank, off = (addr >> 16) & 0xFF, addr & 0xFFFF
        if off >= 0x8000 and bank < 0x7E or bank >= 0x80:
            if off >= 0x8000 or bank >= 0xC0:
                return self.rom[(((bank & 0x3F) << 16) | off) & self.rom_mask]
        elif off < 0x2000 and bank < 0x40:
            return self.wram[off]
        addr &= 0xFFFFFF
        if bank == 0x7E or bank == 0x7F:
            return self.wram[addr - 0x7E0000]
        if (bank & 0x7F) < 0x40:
            if off < 0x2000:
                return self.wram[off]
            if off >= 0x8000:
                return self.rom[(((bank & 0x3F) << 16) | off) & self.rom_mask]
            if off >= 0x6000:
                if (bank & 0x3F) >= 0x20:
                    return self.sram[off & 0x7FF]
                return 0
            return self.io_read(off)
        return self.rom[(((bank & 0x3F) << 16) | off) & self.rom_mask]

    def write(self, addr: int, v: int) -> None:
        addr &= 0xFFFFFF
        bank, off = addr >> 16, addr & 0xFFFF
        if bank == 0x7E or bank == 0x7F:
            i = addr - 0x7E0000
            if i in self.watch:
                self.watch[i](i, self.wram[i], v, self)
            self.wram[i] = v
            return
        if (bank & 0x7F) < 0x40:
            if off < 0x2000:
                if off in self.watch:
                    self.watch[off](off, self.wram[off], v, self)
                self.wram[off] = v
            elif off < 0x6000:
                self.io_write(off, v)
            elif off < 0x8000 and (bank & 0x3F) >= 0x20:
                self.sram[off & 0x7FF] = v
        # ROM: writes dropped

    def read16(self, addr: int) -> int:
        return self.read(addr) | self.read((addr + 1) & 0xFFFFFF) << 8

    def write16(self, addr: int, v: int) -> None:
        self.write(addr, v & 0xFF)
        self.write((addr + 1) & 0xFFFFFF, (v >> 8) & 0xFF)

    # ---- I/O (rt_io_read / rt_io_write) ----------------------------------------
    def vprefetch_load(self) -> None:
        """Reads return a latch loaded when the address is written and on
        the read that steps it, before the step: after setting the address
        the first word comes twice, which is why games read once first."""
        i = (self.vmadd * 2) & 0xFFFF
        self.vprefetch = self.vram[i] | self.vram[i + 1] << 8

    def io_read(self, a: int) -> int:
        a &= 0xFFFF
        if a >= 0x6000:
            return 0
        if a < 0x2140:
            if a < 0x2134:
                return self.io[a - 0x2000] if a >= 0x2000 else 0
            if a < 0x2137:
                return (self.mpy >> (8 * (a - 0x2134))) & 0xFF
            if a == 0x2139:
                v = self.vprefetch & 0xFF
                if not self.vminc_high:
                    self.vprefetch_load()
                    self.vmadd = (self.vmadd + self.vminc) & 0x7FFF
                return v
            if a == 0x213A:
                v = self.vprefetch >> 8
                if self.vminc_high:
                    self.vprefetch_load()
                    self.vmadd = (self.vmadd + self.vminc) & 0x7FFF
                return v
            if a == 0x213E:
                return 1
            if a == 0x213F:
                return 3
            return self.io[a - 0x2000]
        if a < 0x2180:
            return self.apu_out[a & 3]
        if a == 0x2180:
            v = self.wram[self.wmadd & 0x1FFFF]
            self.wmadd += 1
            return v
        if a == 0x4210:
            v = 2
            if self.rdnmi:
                v |= 0x80
                self.rdnmi = False
            return v
        if a == 0x4211:
            if self.timeup:
                self.timeup = False
                return 0x80
            return 0
        if a == 0x4212:
            self.hvb_toggle ^= 0xFF
            return self.hvb_toggle & 0xC0
        if a == 0x4016:
            return 1
        if a == 0x4017:
            return 0
        return self.io[a - 0x2000] if a >= 0x2000 else 0

    def io_write(self, a: int, v: int) -> None:
        a &= 0xFFFF
        if a >= 0x6000 or a < 0x2000:
            return
        self.io[a - 0x2000] = v
        if 0x2100 <= a < 0x2200:
            self.w21(a, v)
        elif 0x4200 <= a < 0x4220:
            self.w42(a, v)

    def w21(self, a: int, v: int) -> None:
        r = a - 0x2100
        io = self.io
        if r in (2, 3):
            self.oamadd = (((io[0x103] & 1) << 8) | io[0x102]) * 2
        elif r == 4:
            o = self.oamadd
            if o < 0x200:
                if o & 1:
                    self.oam[o - 1] = self.oamlatch
                    self.oam[o] = v
                else:
                    self.oamlatch = v
            else:
                self.oam[o & 0x21F] = v
            self.oamadd = (o + 1) & 0x3FF
        elif r == 0x15:
            self.vminc = VMINC[v & 3]
            self.vminc_high = bool(v & 0x80)
        elif r == 0x16:
            self.vmadd = (self.vmadd & 0xFF00) | v
            self.vprefetch_load()
        elif r == 0x17:
            self.vmadd = (self.vmadd & 0x00FF) | v << 8
            self.vprefetch_load()
        elif r == 0x18:
            i = self.vmadd * 2
            if i >= 0x10000:
                raise Stop(f"VRAM word address {self.vmadd:#06x} beyond 64 KiB")
            self.vram[i] = v
            if not self.vminc_high:
                self.vmadd = (self.vmadd + self.vminc) & 0x7FFF
        elif r == 0x19:
            i = self.vmadd * 2 + 1
            if i >= 0x10000:
                raise Stop(f"VRAM word address {self.vmadd:#06x} beyond 64 KiB")
            self.vram[i] = v
            if self.vminc_high:
                self.vmadd = (self.vmadd + self.vminc) & 0x7FFF
        elif r in (0x0D, 0x0F, 0x11, 0x13, 0x0E, 0x10, 0x12, 0x14):
            if r in (0x0D, 0x0E):
                self.m7old = v
            self.bgold = v
        elif r == 0x1B:
            self.m7a = (v << 8) | self.m7old
            self.m7old = v
            self.mpyres()
        elif r == 0x1C:
            self.m7b = v
            self.m7old = v
            self.mpyres()
        elif 0x1D <= r <= 0x20:
            self.m7old = v
        elif r == 0x21:
            self.cgadd = v * 2
            self.cgflip = False
        elif r == 0x22:
            self.cgflip = not self.cgflip
            if self.cgflip:
                self.cglatch = v
            else:
                i = self.cgadd & 0x1FE
                self.cgram[i] = self.cglatch
                self.cgram[i + 1] = v & 0x7F
                self.cgadd += 2
        elif 0x40 <= r < 0x80:
            self.apu_write(r & 3, v)
        elif r == 0x80:
            self.wram[self.wmadd & 0x1FFFF] = v
            self.wmadd += 1
        elif r == 0x81:
            self.wmadd = (self.wmadd & 0xFFFF00) | v
        elif r == 0x82:
            self.wmadd = (self.wmadd & 0xFF00FF) | v << 8
        elif r == 0x83:
            self.wmadd = (self.wmadd & 0x00FFFF) | (v & 1) << 16

    def mpyres(self) -> None:
        a = self.m7a - 0x10000 if self.m7a & 0x8000 else self.m7a
        b = self.m7b - 0x100 if self.m7b & 0x80 else self.m7b
        self.mpy = (a * b) & 0xFFFFFFFF

    def w42(self, a: int, v: int) -> None:
        io = self.io
        if a == 0x4200:
            self.nmitimen = v
        elif a == 0x4203:
            p = io[0x2202] * v
            io[0x2216], io[0x2217] = p & 0xFF, p >> 8
        elif a == 0x4206:
            n = io[0x2204] | io[0x2205] << 8
            if v:
                q, r = n // v, n % v
                io[0x2214], io[0x2215] = q & 0xFF, q >> 8
                io[0x2216], io[0x2217] = r & 0xFF, r >> 8
            else:
                io[0x2214] = io[0x2215] = 0xFF
                io[0x2216], io[0x2217] = io[0x2204], io[0x2205]
        elif a == 0x420B:
            self.dma(v)

    def apu_write(self, port: int, v: int) -> None:
        self.apu_in[port] = v
        out = self.apu_out
        if self.apu_state == 0:
            if port == 0 and v == 0xCC:
                out[0] = v
                self.apu_state = 1
                self.apu_index = 0xFF
        elif self.apu_state == 1:
            if port == 0:
                out[0] = v
                if (self.apu_index + 1) & 0xFF == v:
                    self.apu_index = v
                else:
                    self.apu_index = 0xFF
                    if self.apu_in[1] == 0:
                        self.apu_state = 2
        else:
            out[port] = v
            if port == 0 and not v & 1:
                out[1] = 0xFF

    def dma(self, mask: int) -> None:
        io = self.io
        for ch in range(8):
            if not mask & (1 << ch):
                continue
            r = 0x2300 + ch * 16                 # $43x0 - $2000
            params, bbus = io[r], io[r + 1]
            src = io[r + 2] | io[r + 3] << 8 | io[r + 4] << 16
            n = io[r + 5] | io[r + 6] << 8
            n = n or 0x10000
            pat = DMA_PATTERNS[params & 7]
            fixed, dec, btoa = params & 8, params & 0x10, params & 0x80
            for i in range(n):
                breg = 0x2100 + ((bbus + pat[i & 3]) & 0xFF)
                if btoa:
                    self.write(src, self.io_read(breg))
                else:
                    self.io_write(breg, self.read(src))
                if not fixed:
                    lo = (src + (-1 if dec else 1)) & 0xFFFF
                    src = (src & 0xFF0000) | lo
            io[r + 2], io[r + 3] = src & 0xFF, (src >> 8) & 0xFF
            io[r + 5] = io[r + 6] = 0

    # ---- the NMI at WAI (rt_wai) ---------------------------------------------
    def wai(self) -> None:
        self.rdnmi = False
        if not self.io[0x100] & 0x80:
            # VBlank starts: the PPU reloads the OAM address (rt_wai)
            self.oamadd = (((self.io[0x103] & 1) << 8) | self.io[0x102]) * 2
        self.io[0x100] = 0x8F                    # forced blank
        h = self.wram[0x1C] | self.wram[0x1D] << 8
        if h == NMI_LAG:
            self.io[0x100] = self.wram[INIDISP_COPY]
            return
        if h == NMI_RETURN:
            return
        self.game_frames += 1
        while self.script_i < len(self.script) and self.script[self.script_i][0] <= self.game_frames:
            self.script_pad = self.script[self.script_i][1]
            self.script_i += 1
        self.io[0x2218] = self.script_pad & 0xFF
        self.io[0x2219] = self.script_pad >> 8
        if self.frame_hook:
            self.frame_hook(self.game_frames)
        # CODE_80A96C: REP #$30, PHA/PHX/PHY (after the 4-byte interrupt
        # frame), SEP #$20, LDA #$8F, REP #$20, JMP ($001C)
        self.a = (self.a & 0xFF00) | 0x8F
        self.s = (self.s - 10) & 0xFFFF
        self.p = (self.p & ~0x30 & 0xFF) | 0x80  # m16 x16, N from LDA #$8F
        self.p &= ~0x02 & 0xFF
        self.e = False
        self.pbr = 0x80
        self.pc = h

    # ---- CPU ------------------------------------------------------------------
    def run(self, until_frame: int, max_steps: int = 10**10) -> None:
        """Execute until the NMI that would start game frame until_frame."""
        self.frame_limit = until_frame
        step = self.step
        try:
            while self.steps < max_steps:
                step()
        except _FrameReached:
            return

    def reset(self) -> None:
        self.e = True
        self.p = 0x34
        self.pbr = self.dbr = 0
        self.d = 0
        self.s = 0x1FF
        self.pc = self.rom[0xFFFC] | self.rom[0xFFFD] << 8

    # flags
    C, Z, I, D, X, M, V, N = 1, 2, 4, 8, 16, 32, 64, 128

    def m8(self):
        return self.e or self.p & 0x20

    def x8(self):
        return self.e or self.p & 0x10

    def fetch8(self):
        v = self.read(self.pbr << 16 | self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        return v

    def fetch16(self):
        v = self.fetch8()
        return v | self.fetch8() << 8

    def fetch24(self):
        v = self.fetch16()
        return v | self.fetch8() << 16

    def push8(self, v):
        self.write(self.s, v & 0xFF)
        self.s = (self.s - 1) & 0xFFFF
        if self.e:
            self.s = 0x100 | (self.s & 0xFF)

    def pull8(self):
        self.s = (self.s + 1) & 0xFFFF
        if self.e:
            self.s = 0x100 | (self.s & 0xFF)
        return self.read(self.s)

    def push16(self, v):
        self.push8(v >> 8)
        self.push8(v)

    def pull16(self):
        v = self.pull8()
        return v | self.pull8() << 8

    def setnz(self, v, eight):
        p = self.p & ~0x82
        if eight:
            v &= 0xFF
            if not v:
                p |= 2
            p |= v & 0x80
        else:
            v &= 0xFFFF
            if not v:
                p |= 2
            if v & 0x8000:
                p |= 0x80
        self.p = p

    def set_p(self, v):
        self.p = v & 0xFF
        if self.e:
            self.p |= 0x30
        if self.p & 0x10:
            self.x &= 0xFF
            self.y &= 0xFF

    def ea(self, mode):
        return EA[mode](self)

    def load(self, mode, eight):
        if mode == "imm":
            return self.fetch8() if eight else self.fetch16()
        a = EA[mode](self)
        return self.read(a) if eight else self.read16(a)

    def step(self):
        if self.pc_hook:
            h = self.pc_hook.get(self.pbr << 16 | self.pc)
            if h:
                h(self)
        self.steps += 1
        self.ipc = self.pbr << 16 | self.pc
        op = self.fetch8()
        OPS[op](self)


class _FrameReached(Exception):
    pass


# ---- effective addresses (24-bit) -----------------------------------------------
def _f8(c):
    v = c.read(c.pbr << 16 | c.pc)
    c.pc = (c.pc + 1) & 0xFFFF
    return v


def _f16(c):
    return _f8(c) | _f8(c) << 8


def _ea_dpil(c):
    p = (c.d + _f8(c)) & 0xFFFF
    return c.read16(p) | c.read((p + 2) & 0xFFFF) << 16


EA = {
    "dp": lambda c: (c.d + _f8(c)) & 0xFFFF,
    "dpx": lambda c: (c.d + _f8(c) + c.x) & 0xFFFF,
    "dpy": lambda c: (c.d + _f8(c) + c.y) & 0xFFFF,
    "abs": lambda c: c.dbr << 16 | _f16(c),
    "absx": lambda c: ((c.dbr << 16 | _f16(c)) + c.x) & 0xFFFFFF,
    "absy": lambda c: ((c.dbr << 16 | _f16(c)) + c.y) & 0xFFFFFF,
    "long": lambda c: _f16(c) | _f8(c) << 16,
    "longx": lambda c: ((_f16(c) | _f8(c) << 16) + c.x) & 0xFFFFFF,
    "dpi": lambda c: c.dbr << 16 | c.read16((c.d + _f8(c)) & 0xFFFF),
    "dpiy": lambda c: ((c.dbr << 16 | c.read16((c.d + _f8(c)) & 0xFFFF)) + c.y) & 0xFFFFFF,
    "dpix": lambda c: c.dbr << 16 | c.read16((c.d + _f8(c) + c.x) & 0xFFFF),
    "dpil": _ea_dpil,
    "dpily": lambda c: (_ea_dpil(c) + c.y) & 0xFFFFFF,
    "sr": lambda c: (c.s + _f8(c)) & 0xFFFF,
    "sriy": lambda c: ((c.dbr << 16 | c.read16((c.s + _f8(c)) & 0xFFFF)) + c.y) & 0xFFFFFF,
}


# ---- instruction table --------------------------------------------------------
OPS = [None] * 256


def _alu(name, modes):
    for op, mode in modes.items():
        eaf = EA.get(mode)

        def f(cpu, mode=mode, name=name, eaf=eaf):
            e8 = cpu.e or cpu.p & 0x20
            if eaf is None:
                v = _f8(cpu) if e8 else _f16(cpu)
            else:
                a = eaf(cpu)
                v = cpu.read(a) if e8 else cpu.read16(a)
            mask = 0xFF if e8 else 0xFFFF
            a = cpu.a & mask
            if name == "ORA":
                r = a | v
            elif name == "AND":
                r = a & v
            elif name == "EOR":
                r = a ^ v
            elif name == "ADC":
                r = _adc(cpu, a, v, e8)
            elif name == "SBC":
                r = _sbc(cpu, a, v, e8)
            elif name == "CMP":
                _cmp(cpu, a, v, e8)
                return
            elif name == "LDA":
                r = v
            cpu.a = (cpu.a & ~mask & 0xFFFF) | (r & mask)
            cpu.setnz(r, e8)
        OPS[op] = f


def _adc(cpu, a, v, e8):
    c = cpu.p & 1
    if cpu.p & 8:                      # decimal
        if e8:
            lo = (a & 0xF) + (v & 0xF) + c
            if lo > 9:
                lo += 6
            r = (a & 0xF0) + (v & 0xF0) + (lo if lo <= 0xF else (lo & 0xF) + 0x10)
            ov = (~(a ^ v) & (a ^ r) & 0x80)
            if r > 0x9F:
                r += 0x60
            cpu.p = (cpu.p & ~0x41) | (1 if r > 0xFF else 0) | (0x40 if ov else 0)
            return r & 0xFF
        r = 0
        cc = c
        for sh in (0, 4, 8, 12):
            d = ((a >> sh) & 0xF) + ((v >> sh) & 0xF) + cc
            cc = 0
            if d > 9:
                d += 6
                cc = 1
            r |= (d & 0xF) << sh
        ov = (~(a ^ v) & (a ^ r) & 0x8000)
        cpu.p = (cpu.p & ~0x41) | cc | (0x40 if ov else 0)
        return r & 0xFFFF
    if e8:
        r = a + v + c
        ov = (~(a ^ v) & (a ^ r) & 0x80)
        cpu.p = (cpu.p & ~0x41) | (1 if r > 0xFF else 0) | (0x40 if ov else 0)
        return r & 0xFF
    r = a + v + c
    ov = (~(a ^ v) & (a ^ r) & 0x8000)
    cpu.p = (cpu.p & ~0x41) | (1 if r > 0xFFFF else 0) | (0x40 if ov else 0)
    return r & 0xFFFF


def _sbc(cpu, a, v, e8):
    if cpu.p & 8:
        c = cpu.p & 1
        digits = 2 if e8 else 4
        r = 0
        borrow = 1 - c
        for k in range(digits):
            d = ((a >> (4 * k)) & 0xF) - ((v >> (4 * k)) & 0xF) - borrow
            borrow = 0
            if d < 0:
                d += 10
                borrow = 1
            r |= (d & 0xF) << (4 * k)
        mask = 0xFF if e8 else 0xFFFF
        sign = 0x80 if e8 else 0x8000
        vb = (v ^ mask)
        bin_r = a + vb + c
        ov = (~(a ^ vb) & (a ^ bin_r) & sign)
        cpu.p = (cpu.p & ~0x41) | (0 if borrow else 1) | (0x40 if ov else 0)
        return r & mask
    mask = 0xFF if e8 else 0xFFFF
    return _adc_bin(cpu, a, v ^ mask, e8)


def _adc_bin(cpu, a, v, e8):
    saved = cpu.p & 8
    cpu.p &= ~8
    r = _adc(cpu, a, v, e8)
    cpu.p |= saved
    return r


def _cmp(cpu, a, v, e8):
    mask = 0xFF if e8 else 0xFFFF
    r = (a - v) & mask
    cpu.p = (cpu.p & ~1) | (1 if a >= v else 0)
    cpu.setnz(r, e8)


ALU_MODES = {
    "ORA": {0x09: "imm", 0x05: "dp", 0x15: "dpx", 0x0D: "abs", 0x1D: "absx", 0x19: "absy", 0x0F: "long",
            0x1F: "longx", 0x12: "dpi", 0x11: "dpiy", 0x01: "dpix", 0x07: "dpil", 0x17: "dpily", 0x03: "sr",
            0x13: "sriy"},
}
for name, base in (("ORA", 0x00), ("AND", 0x20), ("EOR", 0x40), ("ADC", 0x60), ("LDA", 0xA0), ("CMP", 0xC0),
                   ("SBC", 0xE0)):
    _alu(name, {op + base: m for op, m in ALU_MODES["ORA"].items()})


def _sta_modes():
    return {0x85: "dp", 0x95: "dpx", 0x8D: "abs", 0x9D: "absx", 0x99: "absy", 0x8F: "long", 0x9F: "longx",
            0x92: "dpi", 0x91: "dpiy", 0x81: "dpix", 0x87: "dpil", 0x97: "dpily", 0x83: "sr", 0x93: "sriy"}


def _store(op, mode, reg):
    eaf = EA[mode]

    def f(cpu):
        eight = cpu.m8() if reg in ("a", "z") else cpu.x8()
        a = eaf(cpu)
        v = 0 if reg == "z" else getattr(cpu, reg)
        if eight:
            cpu.write(a, v & 0xFF)
        else:
            cpu.write16(a, v)
    OPS[op] = f


for op, mode in _sta_modes().items():
    _store(op, mode, "a")
for op, mode in {0x86: "dp", 0x96: "dpy", 0x8E: "abs"}.items():
    _store(op, mode, "x")
for op, mode in {0x84: "dp", 0x94: "dpx", 0x8C: "abs"}.items():
    _store(op, mode, "y")
for op, mode in {0x64: "dp", 0x74: "dpx", 0x9C: "abs", 0x9E: "absx"}.items():
    _store(op, mode, "z")


def _ldxy(op, mode, reg):
    def f(cpu):
        e8 = cpu.x8()
        v = cpu.load(mode, e8)
        setattr(cpu, reg, v)
        cpu.setnz(v, e8)
    OPS[op] = f


for op, mode in {0xA2: "imm", 0xA6: "dp", 0xB6: "dpy", 0xAE: "abs", 0xBE: "absy"}.items():
    _ldxy(op, mode, "x")
for op, mode in {0xA0: "imm", 0xA4: "dp", 0xB4: "dpx", 0xAC: "abs", 0xBC: "absx"}.items():
    _ldxy(op, mode, "y")


def _cpxy(op, mode, reg):
    def f(cpu):
        e8 = cpu.x8()
        v = cpu.load(mode, e8)
        _cmp(cpu, getattr(cpu, reg), v, e8)
    OPS[op] = f


for op, mode in {0xE0: "imm", 0xE4: "dp", 0xEC: "abs"}.items():
    _cpxy(op, mode, "x")
for op, mode in {0xC0: "imm", 0xC4: "dp", 0xCC: "abs"}.items():
    _cpxy(op, mode, "y")


def _bit(op, mode):
    def f(cpu):
        e8 = cpu.m8()
        v = cpu.load(mode, e8)
        mask = 0xFF if e8 else 0xFFFF
        r = cpu.a & mask & v
        p = cpu.p & ~2
        if not r:
            p |= 2
        if mode != "imm":
            top = 0x80 if e8 else 0x8000
            p = (p & ~0xC0) | (0x80 if v & top else 0) | (0x40 if v & (top >> 1) else 0)
        cpu.p = p
    OPS[op] = f


for op, mode in {0x89: "imm", 0x24: "dp", 0x34: "dpx", 0x2C: "abs", 0x3C: "absx"}.items():
    _bit(op, mode)


def _rmw(op, mode, kind):
    def f(cpu):
        e8 = cpu.m8()
        mask = 0xFF if e8 else 0xFFFF
        top = 0x80 if e8 else 0x8000
        if mode == "acc":
            v = cpu.a & mask
        else:
            a = cpu.ea(mode)
            v = cpu.read(a) if e8 else cpu.read16(a)
        c = cpu.p & 1
        if kind == "ASL":
            cpu.p = (cpu.p & ~1) | (1 if v & top else 0)
            r = (v << 1) & mask
        elif kind == "LSR":
            cpu.p = (cpu.p & ~1) | (v & 1)
            r = v >> 1
        elif kind == "ROL":
            cpu.p = (cpu.p & ~1) | (1 if v & top else 0)
            r = ((v << 1) | c) & mask
        elif kind == "ROR":
            cpu.p = (cpu.p & ~1) | (v & 1)
            r = (v >> 1) | (top if c else 0)
        elif kind == "INC":
            r = (v + 1) & mask
        elif kind == "DEC":
            r = (v - 1) & mask
        elif kind in ("TSB", "TRB"):
            p = cpu.p & ~2
            if not (cpu.a & mask & v):
                p |= 2
            cpu.p = p
            r = (v | (cpu.a & mask)) if kind == "TSB" else (v & ~(cpu.a & mask) & mask)
        if mode == "acc":
            cpu.a = (cpu.a & ~mask & 0xFFFF) | r
        elif e8:
            cpu.write(a, r)
        else:
            cpu.write16(a, r)
        if kind not in ("TSB", "TRB"):
            cpu.setnz(r, e8)
    OPS[op] = f


for kind, base in (("ASL", 0x00), ("ROL", 0x20), ("LSR", 0x40), ("ROR", 0x60)):
    for off, mode in {0x0A: "acc", 0x06: "dp", 0x16: "dpx", 0x0E: "abs", 0x1E: "absx"}.items():
        _rmw(base + off, mode, kind)
for op, mode in {0x1A: "acc", 0xE6: "dp", 0xF6: "dpx", 0xEE: "abs", 0xFE: "absx"}.items():
    _rmw(op, mode, "INC")
for op, mode in {0x3A: "acc", 0xC6: "dp", 0xD6: "dpx", 0xCE: "abs", 0xDE: "absx"}.items():
    _rmw(op, mode, "DEC")
for op, mode in {0x04: "dp", 0x0C: "abs"}.items():
    _rmw(op, mode, "TSB")
for op, mode in {0x14: "dp", 0x1C: "abs"}.items():
    _rmw(op, mode, "TRB")


def _branch(op, cond):
    def f(cpu):
        o = cpu.fetch8()
        if cond(cpu.p):
            cpu.pc = (cpu.pc + (o - 256 if o & 0x80 else o)) & 0xFFFF
    OPS[op] = f


_branch(0x10, lambda p: not p & 0x80)
_branch(0x30, lambda p: p & 0x80)
_branch(0x50, lambda p: not p & 0x40)
_branch(0x70, lambda p: p & 0x40)
_branch(0x80, lambda p: True)
_branch(0x90, lambda p: not p & 1)
_branch(0xB0, lambda p: p & 1)
_branch(0xD0, lambda p: not p & 2)
_branch(0xF0, lambda p: p & 2)


def _brl(cpu):
    o = cpu.fetch16()
    cpu.pc = (cpu.pc + (o - 0x10000 if o & 0x8000 else o)) & 0xFFFF


OPS[0x82] = _brl


def _incdec_reg(op, reg, d):
    def f(cpu):
        e8 = cpu.m8() if reg == "a" else cpu.x8()
        mask = 0xFF if e8 else 0xFFFF
        v = (getattr(cpu, reg) + d) & mask
        if reg == "a":
            cpu.a = (cpu.a & ~mask & 0xFFFF) | v
        else:
            setattr(cpu, reg, v)
        cpu.setnz(v, e8)
    OPS[op] = f


_incdec_reg(0xE8, "x", 1)
_incdec_reg(0xC8, "y", 1)
_incdec_reg(0xCA, "x", -1)
_incdec_reg(0x88, "y", -1)


def _transfer(op, src, dst):
    def f(cpu):
        if dst == "a":
            e8 = cpu.m8()
            mask = 0xFF if e8 else 0xFFFF
            v = getattr(cpu, src) & mask
            cpu.a = (cpu.a & ~mask & 0xFFFF) | v
        else:
            e8 = cpu.x8()
            mask = 0xFF if e8 else 0xFFFF
            v = cpu.a & mask if src == "a" else getattr(cpu, src) & mask
            setattr(cpu, dst, v)
        cpu.setnz(v, e8)
    OPS[op] = f


_transfer(0xAA, "a", "x")
_transfer(0xA8, "a", "y")
_transfer(0x8A, "x", "a")
_transfer(0x98, "y", "a")
_transfer(0x9B, "x", "y")
_transfer(0xBB, "y", "x")


def _tsx(cpu):
    e8 = cpu.x8()
    cpu.x = cpu.s & (0xFF if e8 else 0xFFFF)
    cpu.setnz(cpu.x, e8)


def _txs(cpu):
    cpu.s = (0x100 | (cpu.x & 0xFF)) if cpu.e else cpu.x


def _tcs(cpu):
    cpu.s = (0x100 | (cpu.a & 0xFF)) if cpu.e else cpu.a


def _tsc(cpu):
    cpu.a = cpu.s
    cpu.setnz(cpu.a, False)


def _tcd(cpu):
    cpu.d = cpu.a
    cpu.setnz(cpu.d, False)


def _tdc(cpu):
    cpu.a = cpu.d
    cpu.setnz(cpu.a, False)


def _xba(cpu):
    cpu.a = ((cpu.a >> 8) | (cpu.a << 8)) & 0xFFFF
    cpu.setnz(cpu.a & 0xFF, True)


OPS[0xBA], OPS[0x9A], OPS[0x1B], OPS[0x3B], OPS[0x5B], OPS[0x7B], OPS[0xEB] = \
    _tsx, _txs, _tcs, _tsc, _tcd, _tdc, _xba


def _push(op, fn):
    OPS[op] = fn


def _pha(cpu):
    if cpu.m8():
        cpu.push8(cpu.a)
    else:
        cpu.push16(cpu.a)


def _pla(cpu):
    if cpu.m8():
        v = cpu.pull8()
        cpu.a = (cpu.a & 0xFF00) | v
        cpu.setnz(v, True)
    else:
        cpu.a = cpu.pull16()
        cpu.setnz(cpu.a, False)


def _phxy(reg):
    def f(cpu):
        if cpu.x8():
            cpu.push8(getattr(cpu, reg))
        else:
            cpu.push16(getattr(cpu, reg))
    return f


def _plxy(reg):
    def f(cpu):
        if cpu.x8():
            v = cpu.pull8()
        else:
            v = cpu.pull16()
        setattr(cpu, reg, v)
        cpu.setnz(v, cpu.x8())
    return f


def _php(cpu):
    cpu.push8(cpu.p | (0x30 if cpu.e else 0))


def _plp(cpu):
    cpu.set_p(cpu.pull8())


def _phb(cpu):
    cpu.push8(cpu.dbr)


def _plb(cpu):
    cpu.dbr = cpu.pull8()
    cpu.setnz(cpu.dbr, True)


def _phk(cpu):
    cpu.push8(cpu.pbr)


def _phd(cpu):
    cpu.push16(cpu.d)


def _pld(cpu):
    cpu.d = cpu.pull16()
    cpu.setnz(cpu.d, False)


def _pea(cpu):
    cpu.push16(cpu.fetch16())


def _pei(cpu):
    p = (cpu.d + cpu.fetch8()) & 0xFFFF
    cpu.push16(cpu.read16(p))


def _per(cpu):
    o = cpu.fetch16()
    cpu.push16((cpu.pc + (o - 0x10000 if o & 0x8000 else o)) & 0xFFFF)


for op, fn in {0x48: _pha, 0x68: _pla, 0xDA: _phxy("x"), 0xFA: _plxy("x"), 0x5A: _phxy("y"), 0x7A: _plxy("y"),
               0x08: _php, 0x28: _plp, 0x8B: _phb, 0xAB: _plb, 0x4B: _phk, 0x0B: _phd, 0x2B: _pld, 0xF4: _pea,
               0xD4: _pei, 0x62: _per}.items():
    OPS[op] = fn


def _flag(op, fn):
    OPS[op] = fn


def _rep(cpu):
    cpu.set_p(cpu.p & ~cpu.fetch8())


def _sep(cpu):
    cpu.set_p(cpu.p | cpu.fetch8())


def _xce(cpu):
    c = cpu.p & 1
    cpu.p = (cpu.p & ~1) | (1 if cpu.e else 0)
    cpu.e = bool(c)
    if cpu.e:
        cpu.set_p(cpu.p | 0x30)
        cpu.s = 0x100 | (cpu.s & 0xFF)


OPS[0xC2], OPS[0xE2], OPS[0xFB] = _rep, _sep, _xce
for op, clear, bit in ((0x18, True, 1), (0x38, False, 1), (0x58, True, 4), (0x78, False, 4), (0xB8, True, 0x40),
                       (0xD8, True, 8), (0xF8, False, 8)):
    def f(cpu, clear=clear, bit=bit):
        cpu.p = (cpu.p & ~bit) if clear else (cpu.p | bit)
    OPS[op] = f


def _jmp_abs(cpu):
    cpu.pc = cpu.fetch16()


def _jml(cpu):
    t = cpu.fetch24()
    cpu.pbr, cpu.pc = t >> 16, t & 0xFFFF


def _jmp_ind(cpu):
    cpu.pc = cpu.read16(cpu.fetch16())


def _jmp_indx(cpu):
    a = (cpu.fetch16() + cpu.x) & 0xFFFF
    cpu.pc = cpu.read16(cpu.pbr << 16 | a)


def _jml_ind(cpu):
    a = cpu.fetch16()
    t = cpu.read16(a) | cpu.read((a + 2) & 0xFFFF) << 16
    cpu.pbr, cpu.pc = t >> 16, t & 0xFFFF


def _jsr(cpu):
    t = cpu.fetch16()
    cpu.push16((cpu.pc - 1) & 0xFFFF)
    cpu.pc = t


def _jsl(cpu):
    lo = cpu.fetch16()
    cpu.push8(cpu.pbr)
    bank = cpu.fetch8()
    cpu.push16((cpu.pc - 1) & 0xFFFF)
    cpu.pbr, cpu.pc = bank, lo


def _jsr_indx(cpu):
    a = cpu.fetch16()
    cpu.push16((cpu.pc - 1) & 0xFFFF)
    cpu.pc = cpu.read16(cpu.pbr << 16 | ((a + cpu.x) & 0xFFFF))


def _rts(cpu):
    cpu.pc = (cpu.pull16() + 1) & 0xFFFF


def _rtl(cpu):
    cpu.pc = (cpu.pull16() + 1) & 0xFFFF
    cpu.pbr = cpu.pull8()


def _rti(cpu):
    cpu.set_p(cpu.pull8())
    cpu.pc = cpu.pull16()
    if not cpu.e:
        cpu.pbr = cpu.pull8()


def _wai(cpu):
    if cpu.game_frames + 1 >= cpu.frame_limit and \
            (cpu.wram[0x1C] | cpu.wram[0x1D] << 8) not in (NMI_LAG, NMI_RETURN):
        cpu.pc = (cpu.pc - 1) & 0xFFFF        # stay on the WAI
        raise _FrameReached()
    cpu.wai()


def _stp(cpu):
    raise Stop(f"STP at {cpu.pbr:02X}:{(cpu.pc - 1) & 0xFFFF:04X}")


def _brk(cpu):
    raise Stop(f"BRK/COP at {cpu.pbr:02X}:{(cpu.pc - 1) & 0xFFFF:04X}")


def _nop(cpu):
    pass


def _wdm(cpu):
    cpu.fetch8()


def _mvn(cpu, step=1):
    dst, src = cpu.fetch8(), cpu.fetch8()
    cpu.dbr = dst
    while True:
        cpu.write(dst << 16 | cpu.y, cpu.read(src << 16 | cpu.x))
        mask = 0xFF if cpu.x8() else 0xFFFF
        cpu.x = (cpu.x + step) & mask
        cpu.y = (cpu.y + step) & mask
        cpu.a = (cpu.a - 1) & 0xFFFF
        if cpu.a == 0xFFFF:
            break


def _mvp(cpu):
    _mvn(cpu, -1)


for op, fn in {0x4C: _jmp_abs, 0x5C: _jml, 0x6C: _jmp_ind, 0x7C: _jmp_indx, 0xDC: _jml_ind, 0x20: _jsr,
               0x22: _jsl, 0xFC: _jsr_indx, 0x60: _rts, 0x6B: _rtl, 0x40: _rti, 0xCB: _wai, 0xDB: _stp,
               0x00: _brk, 0x02: _brk, 0xEA: _nop, 0x42: _wdm, 0x54: _mvn, 0x44: _mvp}.items():
    OPS[op] = fn

assert all(OPS), [hex(i) for i, f in enumerate(OPS) if f is None]


def load_script(spec: str) -> list[tuple[int, int]]:
    out = []
    for item in filter(None, (x.strip() for x in spec.split(","))):
        f, p = item.split(":")
        out.append((int(f), int(p, 16)))
    return out
