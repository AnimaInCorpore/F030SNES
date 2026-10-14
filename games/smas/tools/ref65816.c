/* Reference run of the game on an interpreted 65816, with the hardware
 * model of the Falcon runtime (falcon/smkx_rt.s) mirrored register for
 * register: the same memory map, I/O shadows, DMA, APU handshake model,
 * multiply/divide, IRQ chain and interrupt conventions. Its purpose is to
 * check the translation: with the runtime built with SMKX_SYNC=1, both
 * deliver a VBL only when the game's main loop has finished its frame, so
 * the WRAM after N frames must be equal on both sides.
 *
 *   build/ref65816 smas.sfc SYMFILE|- FRAMES [wram.bin]
 *
 * Prints one line per frame ("sum NMIS SUM", the checkpoint the runtime
 * logs with SMKX_SUMS=1: after the frame's IRQs, before its NMI). With a
 * fourth argument it stops at the checkpoint of frame FRAMES-1 and writes
 * WRAM there.
 *
 * Game-specific facts (SMW): the main loop waits in GameLoop for LagFlag
 * ($10); the stack page above $0110 is excluded from the sum.
 *
 * SMAS has no frame-synchronised main loop here: with "-" as the symbol file
 * the VBL is delivered after a fixed number of instructions (REF_FRAME_INSNS,
 * default 30000, or at once at a WAI), as on hardware. REF_SNAP_DIR=dir
 * writes the runtime's SNAP.BIN (version 4, falcon/smkx_rt.s snap_blocks) for
 * every REF_SNAP_EVERY-th frame from REF_SNAP_FROM on (and every frame from
 * a to b with REF_SNAP_WINDOW=a:b; REF_SNAP_BASE=n is added to the number in
 * the file names, so that runs of different games share a directory), from the frame
 * handler's state: register shadows at the VBL's start, the IRQ chain's
 * writes logged with their line, OAM per segment. The Falcon's own frame in
 * it is zero: these are the renderer's inputs of real game frames (the ROM
 * on an interpreted 65816), not frames of the translation. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_SIZE 0x200000          /* SMAS: 2 MiB LoROM */
#define LAGFLAG 0x10
#define STACK_LO 0x0111           /* sum skips $0111-$01FF */
#define STACK_HI 0x01FF

static uint8_t rom[ROM_SIZE], wram[0x20000], sram[0x8000], io[0x6000];
static uint8_t vram[0x10000], cgram[512], oam[544];

/* ---- hardware state (names as in smkx_rt.s) ---------------------------- */
static uint16_t vmadd, vminc = 1, cgadd, oamadd;
static uint8_t vminc_high, cgflip, cglatch, oamlatch, m7old, m7b, bgold;
static int16_t m7a;
static int32_t mpy;
static uint32_t wmadd;
static uint8_t nmitimen, rdnmi, timeup, iflag, hvb_toggle;
static uint8_t apu_out[4] = {0xAA, 0xBB, 0, 0}, apu_in[4], apu_state, apu_index;
static uint16_t irqline;
static uint32_t apuwrites, dmabytes;
/* what the runtime keeps for the renderer (snapshots) */
static uint16_t bgofs[8], m7r[6], m7ofs[2];
static uint16_t bgofs_start[16 + 6 + 2];    /* bgofs, m7, m7ofs as at the frame's start (contiguous in the runtime) */
static uint8_t io_start[0x6000], cgram_start[512];
#define IRQLOG_MAX 256
#define OAMSEG_MAX 9
static struct { uint16_t line, reg; uint8_t value; } irqlog[IRQLOG_MAX];
static unsigned nirqlog, noamseg;
static struct { uint16_t line; uint8_t oam[544]; } oamseg[OAMSEG_MAX];
static int logging;
static int wai_hit;
/* free-running mode: pseudo time inside the frame, in instructions, for $4210/$4212 polls */
static int free_run;
static int nmi_flag_taken;
static uint64_t frame_base;
static long frame_budget = 30000;

/* ---- CPU ------------------------------------------------------------------ */
typedef struct {
    uint16_t a, x, y, s, d, pc;
    uint8_t dbr, pbr;
    uint8_t n, v, m, xf, dec, i, z, c, e;
} Cpu;
static Cpu cpu;
static uint64_t insns;
static int stopped;
static uint32_t stop_pc;
static const char *stop_why;

static void fail(const char *why)
{
    if (!stopped) {
        stopped = 1;
        stop_why = why;
        stop_pc = (uint32_t)cpu.pbr << 16 | cpu.pc;
    }
}

/* ---- memory map (LoROM, as rt_resolve / rt_build_banks) ------------------- */
static uint8_t io_read(uint32_t a);
static void io_write(uint32_t a, uint8_t v);

static uint8_t rd(uint32_t a)
{
    a &= 0xFFFFFF;
    uint32_t bank = a >> 16, off = a & 0xFFFF, b7 = bank & 0x7F;
    if (bank == 0x7E || bank == 0x7F)
        return wram[((bank - 0x7E) << 16) | off];
    if (off >= 0x8000)
        return rom[((b7 << 15) | (off & 0x7FFF)) & (ROM_SIZE - 1)];
    if (b7 < 0x40) {
        if (off < 0x2000) return wram[off];
        return io_read(a);
    }
    if (b7 >= 0x70)
        return sram[off & 0x1FFF];         /* 8 KiB mirrored through the bank: SMAS's copy detection writes $70:2000 and reads $70:0000 */
    return rom[((b7 << 15) | off) & (ROM_SIZE - 1)];   /* $40-$6F: the bank mirrored */
}

/* REF_WATCH=addr[,addr...] (WRAM offsets, hex) from frame REF_WATCH_FROM on:
 * every write to them is printed with the instruction's address. */
static uint32_t watch[16];
static int nwatch;
static long watch_from = -1, cur_frame;
static uint32_t insn_pc;

static void watch_write(uint32_t w, uint8_t v)
{
    if (cur_frame < watch_from) return;
    for (int i = 0; i < nwatch; i++)
        if (watch[i] == w)
            printf("write $%05X = %02X at $%06X (frame %ld, A=%04X X=%04X Y=%04X P=%c%c S=%04X"
                   " stack %02X %02X %02X %02X %02X %02X %02X %02X)\n", w, v,
                   insn_pc, cur_frame, cpu.a, cpu.x, cpu.y, cpu.m ? 'm' : '-', cpu.xf ? 'x' : '-', cpu.s,
                   wram[cpu.s + 1], wram[cpu.s + 2], wram[cpu.s + 3], wram[cpu.s + 4],
                   wram[cpu.s + 5], wram[cpu.s + 6], wram[cpu.s + 7], wram[cpu.s + 8]);
}

static void wr(uint32_t a, uint8_t v)
{
    a &= 0xFFFFFF;
    uint32_t bank = a >> 16, off = a & 0xFFFF, b7 = bank & 0x7F;
    if (nwatch) {
        uint32_t w = bank == 0x7E || bank == 0x7F ? ((bank - 0x7E) << 16) | off
                   : (b7 < 0x40 && off < 0x2000) ? off : 0xFFFFFFFF;
        if (w != 0xFFFFFFFF) watch_write(w, v);
    }
    if (bank == 0x7E || bank == 0x7F) {
        wram[((bank - 0x7E) << 16) | off] = v;
        return;
    }
    if (off >= 0x8000)
        return;                                          /* ROM */
    if (b7 < 0x40) {
        if (off < 0x2000) wram[off] = v;
        else io_write(a, v);
        return;
    }
    if (b7 >= 0x70)
        sram[off & 0x1FFF] = v;
}

/* ---- I/O (rt_io_read / rt_io_write) ------------------------------------------- */
static void vram_step(void) { vmadd = (uint16_t)((vmadd + vminc) & 0x7FFF); }

static uint8_t io_read(uint32_t a)
{
    uint32_t off = a & 0xFFFF;
    if (off >= 0x6000) return 0;
    if (off >= 0x2134 && off < 0x2137) return (uint8_t)(mpy >> ((off - 0x2134) * 8));
    if (off == 0x2139 || off == 0x213A) {
        uint8_t v = vram[(uint32_t)vmadd * 2 + (off - 0x2139)];
        if ((off == 0x213A) == (vminc_high != 0)) vram_step();
        return v;
    }
    if (off == 0x213E) return 0x01;
    if (off == 0x213F) return 0x03;
    if (off >= 0x2140 && off < 0x2180) return apu_out[off & 3];
    if (off == 0x2180) {
        uint8_t v = wram[wmadd & 0x1FFFF];
        wmadd++;
        return v;
    }
    if (off == 0x4210) {
        uint8_t v = 2;
        if (free_run && !rdnmi && !nmi_flag_taken &&
            (long)(insns - frame_base) >= frame_budget * 224 / 262) rdnmi = 1;   /* V-blank began */
        if (rdnmi) { v |= 0x80; rdnmi = 0; nmi_flag_taken = 1; }
        return v;
    }
    if (off == 0x4211) {
        uint8_t v = timeup ? 0x80 : 0;
        timeup = 0;
        return v;
    }
    if (off == 0x4016) return 1;             /* a pad in port 1 whatever was written (rt_io_read's .joyser) */
    if (off == 0x4212) {
        if (free_run) {                       /* V-blank in the last 38 lines of 262, H-blank in a line's last quarter */
            long pos = (long)(insns - frame_base), line = frame_budget / 262 ? frame_budget / 262 : 1;
            return (uint8_t)((pos >= frame_budget * 224 / 262 ? 0x80 : 0) | (pos % line >= line * 3 / 4 ? 0x40 : 0));
        }
        hvb_toggle = (uint8_t)~hvb_toggle;
        return hvb_toggle & 0xC0;
    }
    return off >= 0x2000 ? io[off - 0x2000] : 0;
}

static void apu_write(int port, uint8_t v)
{
    apu_in[port] = v;
    if (apu_state == 0) {
        if (port == 0 && v == 0xCC) {
            apu_out[0] = v;
            apu_state = 1;
            apu_index = 0xFF;
        }
    } else if (apu_state == 1) {
        if (port != 0) return;
        apu_out[0] = v;
        if ((uint8_t)(apu_index + 1) == v) {
            apu_index = v;
            return;
        }
        apu_index = 0xFF;
        if (apu_in[1] == 0) apu_state = 2;
    } else {
        apu_out[port] = v;
        if (port == 1 && v == 0xFF) {
            apu_state = 0;
            apu_out[0] = 0xAA; apu_out[1] = 0xBB; apu_out[2] = 0; apu_out[3] = 0;
        }
    }
}

static void dma(uint8_t mask);

static void io_write(uint32_t a, uint8_t v)
{
    uint32_t off = a & 0xFFFF;
    if (off >= 0x6000 || off < 0x2000) return;
    io[off - 0x2000] = v;
    if (logging && off >= 0x2100 && off < 0x2200 && off != 0x2104 && off != 0x2118 && off != 0x2119 &&
        nirqlog < IRQLOG_MAX) {
        irqlog[nirqlog].line = irqline;
        irqlog[nirqlog].reg = (uint16_t)off;
        irqlog[nirqlog++].value = v;
    }
    if (off >= 0x2100 && off < 0x2200) {
        uint32_t n = off - 0x2100;
        switch (n) {
        case 0x02: case 0x03:
            oamadd = (uint16_t)((((io[0x103] & 1) << 8) | io[0x102]) * 2);
            return;
        case 0x04:
            if (oamadd < 0x200) {
                if (oamadd & 1) {
                    oam[oamadd - 1] = oamlatch;
                    oam[oamadd] = v;
                } else {
                    oamlatch = v;
                }
            } else {
                oam[oamadd & 0x21F] = v;
            }
            oamadd = (uint16_t)((oamadd + 1) & 0x3FF);
            return;
        case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
            {   /* as rt_io_write's .bgh/.bgv: HOFS keeps three bits of the previous high byte */
                unsigned idx = n - 0x0D;
                if (!(idx & 1)) bgofs[idx] = (uint16_t)(v << 8 | (bgold & 0xF8) | ((bgofs[idx] >> 8) & 7));
                else bgofs[idx] = (uint16_t)(v << 8 | bgold);
                bgold = v;
                if (idx < 2) {
                    m7ofs[idx] = (uint16_t)(v << 8 | m7old);
                    m7old = v;
                }
            }
            return;
        case 0x15: {
            static const uint8_t inc[4] = {1, 32, 128, 128};
            vminc = inc[v & 3];
            vminc_high = (v & 0x80) != 0;
            return;
        }
        case 0x16: vmadd = (uint16_t)((vmadd & 0xFF00) | v); return;
        case 0x17: vmadd = (uint16_t)((vmadd & 0x00FF) | v << 8); return;
        case 0x18: case 0x19:
            vram[((uint32_t)vmadd * 2 + (n - 0x18)) & 0xFFFF] = v;
            if ((n == 0x19) == (vminc_high != 0)) vram_step();
            return;
        case 0x1B:
            m7a = (int16_t)(v << 8 | m7old);
            m7r[0] = (uint16_t)m7a;
            m7old = v;
            mpy = (int32_t)m7a * (int8_t)m7b;
            return;
        case 0x1C:
            m7r[1] = (uint16_t)(v << 8 | m7old);
            m7old = v;
            m7b = v;
            mpy = (int32_t)m7a * (int8_t)m7b;
            return;
        case 0x1D: case 0x1E: case 0x1F: case 0x20:
            m7r[n - 0x1B] = (uint16_t)(v << 8 | m7old);
            m7old = v;
            return;
        case 0x21:
            cgadd = (uint16_t)(v * 2);
            cgflip = 0;
            return;
        case 0x22:
            cgflip = (uint8_t)~cgflip;
            if (cgflip) {
                cglatch = v;
            } else {
                cgram[cgadd & 0x1FE] = cglatch;
                cgram[(cgadd & 0x1FE) + 1] = v & 0x7F;
                cgadd = (uint16_t)(cgadd + 2);
            }
            return;
        case 0x80:
            wram[wmadd & 0x1FFFF] = v;
            wmadd++;
            return;
        case 0x81: wmadd = (wmadd & 0x1FF00) | v; return;
        case 0x82: wmadd = (wmadd & 0x100FF) | (uint32_t)v << 8; return;
        case 0x83: wmadd = (wmadd & 0x0FFFF) | (uint32_t)(v & 1) << 16; return;
        default:
            if (n >= 0x40 && n < 0x80) {
                apuwrites++;
                apu_write(n & 3, v);
            }
            return;
        }
    }
    switch (off) {
    case 0x4200: nmitimen = v; return;
    case 0x4203: {
        uint16_t p = (uint16_t)(io[0x2202] * v);
        io[0x2216] = (uint8_t)p;
        io[0x2217] = (uint8_t)(p >> 8);
        return;
    }
    case 0x4206: {
        uint16_t dvd = (uint16_t)(io[0x2204] | io[0x2205] << 8);
        if (v) {
            uint16_t q = dvd / v, r = dvd % v;
            io[0x2214] = (uint8_t)q; io[0x2215] = (uint8_t)(q >> 8);
            io[0x2216] = (uint8_t)r; io[0x2217] = (uint8_t)(r >> 8);
        } else {
            io[0x2214] = 0xFF; io[0x2215] = 0xFF;
            io[0x2216] = io[0x2204]; io[0x2217] = io[0x2205];
        }
        return;
    }
    case 0x420B: dma(v); return;
    default: return;
    }
}

/* General DMA, byte by byte: the semantics every fast path of rt_dma keeps. */
static void dma(uint8_t mask)
{
    static const uint8_t pat[8][4] = {
        {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
        {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
    };
    for (int ch = 0; ch < 8; ch++) {
        if (!(mask >> ch & 1)) continue;
        uint8_t *r = io + 0x2300 + ch * 16;
        uint32_t count = (uint32_t)(r[5] | r[6] << 8);
        if (!count) count = 0x10000;
        dmabytes += count;
        uint32_t bank = (uint32_t)r[4] << 16;
        uint16_t addr = (uint16_t)(r[2] | r[3] << 8);
        uint8_t ctl = r[0];
        for (uint32_t k = 0; k < count; k++) {
            uint32_t b = 0x2100 + (uint8_t)(r[1] + pat[ctl & 7][k & 3]);
            if (ctl & 0x80) wr(bank | addr, io_read(0x800000 | b));
            else io_write(0x800000 | b, rd(bank | addr));
            if (!(ctl & 0x08)) addr = (uint16_t)(addr + ((ctl & 0x10) ? -1 : 1));
        }
        r[2] = (uint8_t)addr;
        r[3] = (uint8_t)(addr >> 8);
        r[5] = r[6] = 0;
    }
}

/* ---- CPU helpers ------------------------------------------------------------- */
static inline uint8_t getp(void)
{
    return (uint8_t)(cpu.n << 7 | cpu.v << 6 | cpu.m << 5 | cpu.xf << 4 | cpu.dec << 3 |
                     cpu.i << 2 | cpu.z << 1 | cpu.c);
}

static void setp(uint8_t p)
{
    cpu.n = p >> 7 & 1; cpu.v = p >> 6 & 1; cpu.dec = p >> 3 & 1; cpu.i = p >> 2 & 1;
    cpu.z = p >> 1 & 1; cpu.c = p & 1;
    cpu.m = p >> 5 & 1; cpu.xf = p >> 4 & 1;
    if (cpu.e) cpu.m = cpu.xf = 1;
    if (cpu.xf) { cpu.x &= 0xFF; cpu.y &= 0xFF; }
}

static inline uint8_t fetch8(void)
{
    uint8_t v = rd((uint32_t)cpu.pbr << 16 | cpu.pc);
    cpu.pc++;
    return v;
}

static inline uint16_t fetch16(void) { uint16_t l = fetch8(); return (uint16_t)(l | fetch8() << 8); }
static inline uint32_t fetch24(void) { uint32_t l = fetch16(); return l | (uint32_t)fetch8() << 16; }

static inline uint8_t rd0(uint16_t a) { return rd(a); }
static inline uint16_t rd16_0(uint16_t a) { return (uint16_t)(rd(a) | rd((uint16_t)(a + 1)) << 8); }

static void push8(uint8_t v)
{
    wr(cpu.s, v);
    cpu.s = cpu.e ? (uint16_t)(0x100 | ((cpu.s - 1) & 0xFF)) : (uint16_t)(cpu.s - 1);
}

static uint8_t pull8(void)
{
    cpu.s = cpu.e ? (uint16_t)(0x100 | ((cpu.s + 1) & 0xFF)) : (uint16_t)(cpu.s + 1);
    return rd(cpu.s);
}

static void push16(uint16_t v) { push8((uint8_t)(v >> 8)); push8((uint8_t)v); }
static uint16_t pull16(void) { uint16_t l = pull8(); return (uint16_t)(l | pull8() << 8); }

/* Effective addresses: the 24-bit address of the operand's first byte, and
 * whether the second byte wraps inside bank 0 (direct page, stack). */
typedef struct { uint32_t a; int bank0; } Ea;

static inline uint16_t dp_addr(uint8_t o) { return (uint16_t)(cpu.d + o); }

static uint8_t rdm(Ea e) { return rd(e.a); }
static uint16_t rdm16(Ea e)
{
    uint32_t a2 = e.bank0 ? (uint16_t)(e.a + 1) : (e.a + 1) & 0xFFFFFF;
    return (uint16_t)(rd(e.a) | rd(a2) << 8);
}
static void wrm(Ea e, uint8_t v) { wr(e.a, v); }
static void wrm16(Ea e, uint16_t v)
{
    uint32_t a2 = e.bank0 ? (uint16_t)(e.a + 1) : (e.a + 1) & 0xFFFFFF;
    wr(e.a, (uint8_t)v);
    wr(a2, (uint8_t)(v >> 8));
}

enum { M_IMP, M_IMM_M, M_IMM_X, M_IMM8, M_DP, M_DPX, M_DPY, M_DPI, M_DPIX, M_DPIY, M_DPIL, M_DPILY,
       M_ABS, M_ABSX, M_ABSY, M_LONG, M_LONGX, M_SR, M_SRIY };

static Ea ea(int mode)
{
    Ea e = {0, 0};
    uint32_t dbr = (uint32_t)cpu.dbr << 16;
    switch (mode) {
    case M_DP: e.a = dp_addr(fetch8()); e.bank0 = 1; break;
    case M_DPX: e.a = (uint16_t)(dp_addr(fetch8()) + cpu.x); e.bank0 = 1; break;
    case M_DPY: e.a = (uint16_t)(dp_addr(fetch8()) + cpu.y); e.bank0 = 1; break;
    case M_DPI: e.a = dbr | rd16_0(dp_addr(fetch8())); break;
    case M_DPIX: e.a = dbr | rd16_0((uint16_t)(dp_addr(fetch8()) + cpu.x)); break;
    case M_DPIY: e.a = ((dbr | rd16_0(dp_addr(fetch8()))) + cpu.y) & 0xFFFFFF; break;
    case M_DPIL: {
        uint16_t p = dp_addr(fetch8());
        e.a = rd16_0(p) | (uint32_t)rd0((uint16_t)(p + 2)) << 16;
        break;
    }
    case M_DPILY: {
        uint16_t p = dp_addr(fetch8());
        e.a = ((rd16_0(p) | (uint32_t)rd0((uint16_t)(p + 2)) << 16) + cpu.y) & 0xFFFFFF;
        break;
    }
    case M_ABS: e.a = dbr | fetch16(); break;
    case M_ABSX: e.a = ((dbr | fetch16()) + cpu.x) & 0xFFFFFF; break;
    case M_ABSY: e.a = ((dbr | fetch16()) + cpu.y) & 0xFFFFFF; break;
    case M_LONG: e.a = fetch24(); break;
    case M_LONGX: e.a = (fetch24() + cpu.x) & 0xFFFFFF; break;
    case M_SR: e.a = (uint16_t)(cpu.s + fetch8()); e.bank0 = 1; break;
    case M_SRIY: e.a = ((dbr | rd16_0((uint16_t)(cpu.s + fetch8()))) + cpu.y) & 0xFFFFFF; break;
    default: fail("bad addressing mode"); break;
    }
    return e;
}

/* Operand value for an ALU instruction (width w8: 8-bit). */
static uint16_t operand(int mode, int w8)
{
    if (mode == M_IMM_M || mode == M_IMM_X)
        return w8 ? fetch8() : fetch16();
    Ea e = ea(mode);
    return w8 ? rdm(e) : rdm16(e);
}

static inline void nz8(uint8_t v) { cpu.n = v >> 7; cpu.z = v == 0; }
static inline void nz16(uint16_t v) { cpu.n = v >> 15; cpu.z = v == 0; }

static void adc(uint16_t v)
{
    if (cpu.m) {
        uint8_t a = (uint8_t)cpu.a;
        int r;
        if (cpu.dec) {
            r = (a & 0x0F) + (v & 0x0F) + cpu.c;
            if (r > 9) r += 6;
            r = (a & 0xF0) + (v & 0xF0) + (r > 0x0F ? 0x10 : 0) + (r & 0x0F);
            cpu.v = (~(a ^ v) & (a ^ r) & 0x80) != 0;
            if (r > 0x9F) r += 0x60;
        } else {
            r = a + (v & 0xFF) + cpu.c;
            cpu.v = (~(a ^ v) & (a ^ r) & 0x80) != 0;
        }
        cpu.c = r > 0xFF;
        cpu.a = (uint16_t)((cpu.a & 0xFF00) | (r & 0xFF));
        nz8((uint8_t)r);
    } else {
        uint16_t a = cpu.a;
        int r;
        if (cpu.dec) {
            r = (a & 0x000F) + (v & 0x000F) + cpu.c;
            if (r > 0x0009) r += 0x0006;
            r = (a & 0x00F0) + (v & 0x00F0) + (r > 0x000F ? 0x0010 : 0) + (r & 0x000F);
            if (r > 0x009F) r += 0x0060;
            r = (a & 0x0F00) + (v & 0x0F00) + (r > 0x00FF ? 0x0100 : 0) + (r & 0x00FF);
            if (r > 0x09FF) r += 0x0600;
            r = (a & 0xF000) + (v & 0xF000) + (r > 0x0FFF ? 0x1000 : 0) + (r & 0x0FFF);
            cpu.v = (~(a ^ v) & (a ^ r) & 0x8000) != 0;
            if (r > 0x9FFF) r += 0x6000;
        } else {
            r = a + v + cpu.c;
            cpu.v = (~(a ^ v) & (a ^ r) & 0x8000) != 0;
        }
        cpu.c = r > 0xFFFF;
        cpu.a = (uint16_t)r;
        nz16((uint16_t)r);
    }
}

static void sbc(uint16_t v)
{
    if (cpu.dec) {
        /* decimal subtract: SMW never sets D; kept simple and exact per nibble */
        if (cpu.m) {
            uint8_t a = (uint8_t)cpu.a;
            int r = (a & 0x0F) - (v & 0x0F) - !cpu.c;
            if (r < 0) r = ((r - 6) & 0x0F) - 0x10;
            r = (a & 0xF0) - (v & 0xF0) + r;
            int bin = a - (v & 0xFF) - !cpu.c;
            cpu.v = ((a ^ v) & (a ^ bin) & 0x80) != 0;
            cpu.c = bin >= 0;
            if (r < 0) r -= 0x60;
            cpu.a = (uint16_t)((cpu.a & 0xFF00) | (r & 0xFF));
            nz8((uint8_t)r);
        } else {
            uint16_t a = cpu.a;
            int r = (a & 0x000F) - (v & 0x000F) - !cpu.c;
            if (r < 0) r = ((r - 6) & 0x000F) - 0x0010;
            r = (a & 0x00F0) - (v & 0x00F0) + r;
            if (r < 0) r = ((r - 0x60) & 0x00FF) - 0x0100;
            r = (a & 0x0F00) - (v & 0x0F00) + r;
            if (r < 0) r = ((r - 0x600) & 0x0FFF) - 0x1000;
            r = (a & 0xF000) - (v & 0xF000) + r;
            int bin = a - v - !cpu.c;
            cpu.v = ((a ^ v) & (a ^ bin) & 0x8000) != 0;
            cpu.c = bin >= 0;
            if (r < 0) r -= 0x6000;
            cpu.a = (uint16_t)r;
            nz16((uint16_t)r);
        }
        return;
    }
    adc(cpu.m ? (uint16_t)(~v & 0xFF) : (uint16_t)~v);
}

static void cmp(uint16_t reg, uint16_t v, int w8)
{
    if (w8) {
        int r = (reg & 0xFF) - (v & 0xFF);
        cpu.c = r >= 0;
        nz8((uint8_t)r);
    } else {
        int r = reg - v;
        cpu.c = r >= 0;
        nz16((uint16_t)r);
    }
}

/* Read-modify-write on memory or A: op 0 ASL 1 ROL 2 LSR 3 ROR 4 INC 5 DEC 6 TSB 7 TRB */
static uint16_t rmw_val(int op, uint16_t v, int w8)
{
    uint16_t top = w8 ? 0x80 : 0x8000, all = w8 ? 0xFF : 0xFFFF;
    uint16_t r = v;
    switch (op) {
    case 0: cpu.c = (v & top) != 0; r = (uint16_t)((v << 1) & all); break;
    case 1: { int c = cpu.c; cpu.c = (v & top) != 0; r = (uint16_t)(((v << 1) | c) & all); break; }
    case 2: cpu.c = v & 1; r = (uint16_t)(v >> 1); break;
    case 3: { int c = cpu.c; cpu.c = v & 1; r = (uint16_t)((v >> 1) | (c ? top : 0)); break; }
    case 4: r = (uint16_t)((v + 1) & all); break;
    case 5: r = (uint16_t)((v - 1) & all); break;
    case 6: cpu.z = (v & cpu.a & all) == 0; return (uint16_t)(v | (cpu.a & all));
    case 7: cpu.z = (v & cpu.a & all) == 0; return (uint16_t)(v & ~(cpu.a & all) & all);
    }
    if (w8) nz8((uint8_t)r); else nz16(r);
    return r;
}

static void rmw_mem(int op, int mode)
{
    Ea e = ea(mode);
    if (cpu.m) wrm(e, (uint8_t)rmw_val(op, rdm(e), 1));
    else wrm16(e, rmw_val(op, rdm16(e), 0));
}

static void rmw_a(int op)
{
    if (cpu.m) cpu.a = (uint16_t)((cpu.a & 0xFF00) | rmw_val(op, cpu.a & 0xFF, 1));
    else cpu.a = rmw_val(op, cpu.a, 0);
}

static void ld(uint16_t *reg, uint16_t v, int w8)
{
    if (w8) { *reg = (uint16_t)((*reg & (reg == &cpu.a ? 0xFF00 : 0)) | (v & 0xFF)); nz8((uint8_t)v); }
    else { *reg = v; nz16(v); }
}

static void st(int mode, uint16_t v, int w8)
{
    Ea e = ea(mode);
    if (w8) wrm(e, (uint8_t)v); else wrm16(e, v);
}

static void branch(int cond)
{
    int8_t o = (int8_t)fetch8();
    if (cond) cpu.pc = (uint16_t)(cpu.pc + o);
}

static void bit(uint16_t v, int imm)
{
    if (cpu.m) {
        cpu.z = (v & cpu.a & 0xFF) == 0;
        if (!imm) { cpu.n = v >> 7 & 1; cpu.v = v >> 6 & 1; }
    } else {
        cpu.z = (v & cpu.a) == 0;
        if (!imm) { cpu.n = v >> 15 & 1; cpu.v = v >> 14 & 1; }
    }
}

static void set_x8(void) { if (cpu.xf) { cpu.x &= 0xFF; cpu.y &= 0xFF; } }

static int alu_col(uint8_t op)
{
    switch (op & 0x1F) {
    case 0x01: return M_DPIX; case 0x03: return M_SR; case 0x05: return M_DP;
    case 0x07: return M_DPIL; case 0x09: return M_IMM_M; case 0x0D: return M_ABS;
    case 0x0F: return M_LONG; case 0x11: return M_DPIY; case 0x12: return M_DPI;
    case 0x13: return M_SRIY; case 0x15: return M_DPX; case 0x17: return M_DPILY;
    case 0x19: return M_ABSY; case 0x1D: return M_ABSX; case 0x1F: return M_LONGX;
    }
    return -1;
}

/* ---- one instruction ----------------------------------------------------------- */
/* REF_CALLS_FROM=frame: print every JSR/JSL/RTS/RTL from that frame on, with S. */
static long calls_from = -1;

static void step(void)
{
    insn_pc = (uint32_t)cpu.pbr << 16 | cpu.pc;
    if (calls_from >= 0 && cur_frame >= calls_from) {
        uint8_t o = rd(insn_pc);
        if (o == 0x20 || o == 0x22 || o == 0xFC || o == 0x60 || o == 0x6B)
            printf("call %06X %s S=%04X\n", insn_pc,
                   o == 0x20 ? "JSR" : o == 0x22 ? "JSL" : o == 0xFC ? "JSRX" : o == 0x60 ? "RTS" : "RTL", cpu.s);
    }
    uint8_t op = fetch8();
    insns++;
    int m8 = cpu.m, x8 = cpu.xf;
    int col = alu_col(op);
    if (col >= 0 && op != 0x89) {
        int grp = op >> 5;             /* ORA AND EOR ADC STA LDA CMP SBC */
        if (grp == 4) { st(col, cpu.a, m8); return; }
        uint16_t v = operand(col, m8);
        switch (grp) {
        case 0: if (m8) { cpu.a = (uint16_t)(cpu.a | (v & 0xFF)); nz8((uint8_t)cpu.a); } else { cpu.a |= v; nz16(cpu.a); } return;
        case 1: if (m8) { cpu.a = (uint16_t)((cpu.a & 0xFF00) | (cpu.a & v & 0xFF)); nz8((uint8_t)cpu.a); } else { cpu.a &= v; nz16(cpu.a); } return;
        case 2: if (m8) { cpu.a = (uint16_t)(cpu.a ^ (v & 0xFF)); nz8((uint8_t)cpu.a); } else { cpu.a ^= v; nz16(cpu.a); } return;
        case 3: adc(v); return;
        case 5: ld(&cpu.a, v, m8); return;
        case 6: cmp(cpu.a, v, m8); return;
        case 7: sbc(v); return;
        }
    }
    switch (op) {
    /* read-modify-write */
    case 0x06: rmw_mem(0, M_DP); return;   case 0x16: rmw_mem(0, M_DPX); return;
    case 0x0E: rmw_mem(0, M_ABS); return;  case 0x1E: rmw_mem(0, M_ABSX); return;
    case 0x0A: rmw_a(0); return;
    case 0x26: rmw_mem(1, M_DP); return;   case 0x36: rmw_mem(1, M_DPX); return;
    case 0x2E: rmw_mem(1, M_ABS); return;  case 0x3E: rmw_mem(1, M_ABSX); return;
    case 0x2A: rmw_a(1); return;
    case 0x46: rmw_mem(2, M_DP); return;   case 0x56: rmw_mem(2, M_DPX); return;
    case 0x4E: rmw_mem(2, M_ABS); return;  case 0x5E: rmw_mem(2, M_ABSX); return;
    case 0x4A: rmw_a(2); return;
    case 0x66: rmw_mem(3, M_DP); return;   case 0x76: rmw_mem(3, M_DPX); return;
    case 0x6E: rmw_mem(3, M_ABS); return;  case 0x7E: rmw_mem(3, M_ABSX); return;
    case 0x6A: rmw_a(3); return;
    case 0xE6: rmw_mem(4, M_DP); return;   case 0xF6: rmw_mem(4, M_DPX); return;
    case 0xEE: rmw_mem(4, M_ABS); return;  case 0xFE: rmw_mem(4, M_ABSX); return;
    case 0x1A: rmw_a(4); return;
    case 0xC6: rmw_mem(5, M_DP); return;   case 0xD6: rmw_mem(5, M_DPX); return;
    case 0xCE: rmw_mem(5, M_ABS); return;  case 0xDE: rmw_mem(5, M_ABSX); return;
    case 0x3A: rmw_a(5); return;
    case 0x04: rmw_mem(6, M_DP); return;   case 0x0C: rmw_mem(6, M_ABS); return;
    case 0x14: rmw_mem(7, M_DP); return;   case 0x1C: rmw_mem(7, M_ABS); return;

    /* BIT */
    case 0x89: bit(m8 ? fetch8() : fetch16(), 1); return;
    case 0x24: bit(operand(M_DP, m8), 0); return;
    case 0x34: bit(operand(M_DPX, m8), 0); return;
    case 0x2C: bit(operand(M_ABS, m8), 0); return;
    case 0x3C: bit(operand(M_ABSX, m8), 0); return;

    /* loads and stores of X, Y; STZ */
    case 0xA2: ld(&cpu.x, operand(M_IMM_X, x8), x8); return;
    case 0xA6: ld(&cpu.x, operand(M_DP, x8), x8); return;
    case 0xB6: ld(&cpu.x, operand(M_DPY, x8), x8); return;
    case 0xAE: ld(&cpu.x, operand(M_ABS, x8), x8); return;
    case 0xBE: ld(&cpu.x, operand(M_ABSY, x8), x8); return;
    case 0xA0: ld(&cpu.y, operand(M_IMM_X, x8), x8); return;
    case 0xA4: ld(&cpu.y, operand(M_DP, x8), x8); return;
    case 0xB4: ld(&cpu.y, operand(M_DPX, x8), x8); return;
    case 0xAC: ld(&cpu.y, operand(M_ABS, x8), x8); return;
    case 0xBC: ld(&cpu.y, operand(M_ABSX, x8), x8); return;
    case 0x86: st(M_DP, cpu.x, x8); return;  case 0x96: st(M_DPY, cpu.x, x8); return;
    case 0x8E: st(M_ABS, cpu.x, x8); return;
    case 0x84: st(M_DP, cpu.y, x8); return;  case 0x94: st(M_DPX, cpu.y, x8); return;
    case 0x8C: st(M_ABS, cpu.y, x8); return;
    case 0x64: st(M_DP, 0, m8); return;      case 0x74: st(M_DPX, 0, m8); return;
    case 0x9C: st(M_ABS, 0, m8); return;     case 0x9E: st(M_ABSX, 0, m8); return;

    /* compares of X, Y */
    case 0xE0: cmp(cpu.x, operand(M_IMM_X, x8), x8); return;
    case 0xE4: cmp(cpu.x, operand(M_DP, x8), x8); return;
    case 0xEC: cmp(cpu.x, operand(M_ABS, x8), x8); return;
    case 0xC0: cmp(cpu.y, operand(M_IMM_X, x8), x8); return;
    case 0xC4: cmp(cpu.y, operand(M_DP, x8), x8); return;
    case 0xCC: cmp(cpu.y, operand(M_ABS, x8), x8); return;

    /* register increments and transfers */
    case 0xE8: cpu.x = (uint16_t)(cpu.x + 1); if (x8) { cpu.x &= 0xFF; nz8((uint8_t)cpu.x); } else nz16(cpu.x); return;
    case 0xC8: cpu.y = (uint16_t)(cpu.y + 1); if (x8) { cpu.y &= 0xFF; nz8((uint8_t)cpu.y); } else nz16(cpu.y); return;
    case 0xCA: cpu.x = (uint16_t)(cpu.x - 1); if (x8) { cpu.x &= 0xFF; nz8((uint8_t)cpu.x); } else nz16(cpu.x); return;
    case 0x88: cpu.y = (uint16_t)(cpu.y - 1); if (x8) { cpu.y &= 0xFF; nz8((uint8_t)cpu.y); } else nz16(cpu.y); return;
    case 0xAA: ld(&cpu.x, cpu.a, x8); return;
    case 0xA8: ld(&cpu.y, cpu.a, x8); return;
    case 0x8A: ld(&cpu.a, cpu.x, m8); return;
    case 0x98: ld(&cpu.a, cpu.y, m8); return;
    case 0x9B: ld(&cpu.y, cpu.x, x8); return;
    case 0xBB: ld(&cpu.x, cpu.y, x8); return;
    case 0xBA: ld(&cpu.x, cpu.s, x8); return;
    case 0x9A: cpu.s = cpu.e ? (uint16_t)(0x100 | (cpu.x & 0xFF)) : cpu.x; return;
    case 0x5B: cpu.d = cpu.a; nz16(cpu.d); return;
    case 0x7B: cpu.a = cpu.d; nz16(cpu.a); return;
    case 0x1B: cpu.s = cpu.e ? (uint16_t)(0x100 | (cpu.a & 0xFF)) : cpu.a; return;
    case 0x3B: cpu.a = cpu.s; nz16(cpu.a); return;
    case 0xEB: cpu.a = (uint16_t)(cpu.a >> 8 | cpu.a << 8); nz8((uint8_t)cpu.a); return;

    /* flags */
    case 0x18: cpu.c = 0; return;  case 0x38: cpu.c = 1; return;
    case 0x58: cpu.i = 0; iflag = 0; return;  case 0x78: cpu.i = 1; iflag = 1; return;
    case 0xB8: cpu.v = 0; return;
    case 0xD8: cpu.dec = 0; return;  case 0xF8: cpu.dec = 1; return;
    case 0xC2: { uint8_t v = fetch8(); setp((uint8_t)(getp() & ~v)); if (v & 4) iflag = 0; return; }
    case 0xE2: { uint8_t v = fetch8(); setp((uint8_t)(getp() | v)); if (v & 4) iflag = 1; return; }
    case 0xFB: {
        uint8_t c = cpu.c;
        cpu.c = cpu.e;
        cpu.e = c;
        if (cpu.e) { cpu.m = cpu.xf = 1; set_x8(); cpu.s = (uint16_t)(0x100 | (cpu.s & 0xFF)); }
        return;
    }

    /* stack */
    case 0x48: if (m8) push8((uint8_t)cpu.a); else push16(cpu.a); return;
    case 0xDA: if (x8) push8((uint8_t)cpu.x); else push16(cpu.x); return;
    case 0x5A: if (x8) push8((uint8_t)cpu.y); else push16(cpu.y); return;
    case 0x68: if (m8) { cpu.a = (uint16_t)((cpu.a & 0xFF00) | pull8()); nz8((uint8_t)cpu.a); } else { cpu.a = pull16(); nz16(cpu.a); } return;
    case 0xFA: if (x8) { cpu.x = pull8(); nz8((uint8_t)cpu.x); } else { cpu.x = pull16(); nz16(cpu.x); } return;
    case 0x7A: if (x8) { cpu.y = pull8(); nz8((uint8_t)cpu.y); } else { cpu.y = pull16(); nz16(cpu.y); } return;
    case 0x8B: push8(cpu.dbr); return;
    case 0xAB: cpu.dbr = pull8(); nz8(cpu.dbr); return;
    case 0x4B: push8(cpu.pbr); return;
    case 0x0B: push16(cpu.d); return;
    case 0x2B: cpu.d = pull16(); nz16(cpu.d); return;
    case 0x08: push8(getp()); return;
    case 0x28: setp(pull8()); return;          /* PLP: the runtime keeps its I flag */
    case 0xF4: push16(fetch16()); return;
    case 0xD4: push16(rd16_0(dp_addr(fetch8()))); return;
    case 0x62: { uint16_t o = fetch16(); push16((uint16_t)(cpu.pc + o)); return; }

    /* branches and jumps */
    case 0x10: branch(!cpu.n); return;  case 0x30: branch(cpu.n); return;
    case 0x50: branch(!cpu.v); return;  case 0x70: branch(cpu.v); return;
    case 0x90: branch(!cpu.c); return;  case 0xB0: branch(cpu.c); return;
    case 0xD0: branch(!cpu.z); return;  case 0xF0: branch(cpu.z); return;
    case 0x80: branch(1); return;
    case 0x82: { uint16_t o = fetch16(); cpu.pc = (uint16_t)(cpu.pc + o); return; }
    case 0x4C: cpu.pc = fetch16(); return;
    case 0x5C: { uint32_t t = fetch24(); cpu.pc = (uint16_t)t; cpu.pbr = (uint8_t)(t >> 16); return; }
    case 0x6C: cpu.pc = rd16_0(fetch16()); return;
    case 0x7C: { uint16_t b = fetch16(); uint32_t p = (uint32_t)cpu.pbr << 16 | (uint16_t)(b + cpu.x);
                 cpu.pc = (uint16_t)(rd(p) | rd((p & 0xFF0000) | (uint16_t)(p + 1)) << 8); return; }
    case 0xDC: { uint16_t b = fetch16(); cpu.pc = rd16_0(b); cpu.pbr = rd0((uint16_t)(b + 2)); return; }
    case 0x20: { uint16_t t = fetch16(); push16((uint16_t)(cpu.pc - 1)); cpu.pc = t; return; }
    case 0xFC: { uint16_t b = fetch16(); push16((uint16_t)(cpu.pc - 1));
                 uint32_t p = (uint32_t)cpu.pbr << 16 | (uint16_t)(b + cpu.x);
                 cpu.pc = (uint16_t)(rd(p) | rd((p & 0xFF0000) | (uint16_t)(p + 1)) << 8); return; }
    case 0x22: { uint32_t t = fetch24(); push8(cpu.pbr); push16((uint16_t)(cpu.pc - 1));
                 cpu.pc = (uint16_t)t; cpu.pbr = (uint8_t)(t >> 16); return; }
    case 0x60: cpu.pc = (uint16_t)(pull16() + 1); return;
    case 0x6B: cpu.pc = (uint16_t)(pull16() + 1); cpu.pbr = pull8(); return;
    case 0x40: fail("RTI outside an interrupt"); return;

    /* block moves */
    case 0x54: case 0x44: {
        uint8_t dst = fetch8(), src = fetch8();
        cpu.dbr = dst;
        wr((uint32_t)dst << 16 | cpu.y, rd((uint32_t)src << 16 | cpu.x));
        int d = op == 0x54 ? 1 : -1;
        cpu.x = (uint16_t)(cpu.x + d); cpu.y = (uint16_t)(cpu.y + d);
        set_x8();
        cpu.a--;
        if (cpu.a != 0xFFFF) cpu.pc = (uint16_t)(cpu.pc - 3);
        return;
    }

    case 0xEA: return;
    case 0x42: fetch8(); return;             /* WDM */
    case 0x00: fail("BRK"); return;
    case 0x02: fail("COP"); return;
    case 0xCB: wai_hit = 1; return;               /* the next VBL comes at once */
    case 0xDB: fail("STP"); return;
    default: fail("unimplemented opcode"); return;
    }
}

/* ---- interrupts, as the runtime delivers them --------------------------------------
 * `subq.l #4,a6 : jsr handler`, the handler's RTI `addq.l #4,a6 : rts`, then
 * the VBL handler's movem restores every 68k register: nothing is pushed,
 * and the interrupted state comes back whole. The I flag of the runtime
 * (rt_iflag) is left as the handler set it. */
static uint32_t symbol(const char *path, const char *name);

static void interrupt(uint16_t vector_off)
{
    Cpu saved = cpu;
    uint16_t vec = (uint16_t)(rom[vector_off] | rom[vector_off + 1] << 8);
    cpu.s = (uint16_t)(cpu.s - 4);
    cpu.pbr = 0;
    cpu.pc = vec;
    cpu.dec = 0;
    for (uint64_t n = 0; !stopped; n++) {
        uint8_t op = rd((uint32_t)cpu.pbr << 16 | cpu.pc);
        if (op == 0x40) {                      /* RTI */
            cpu = saved;
            return;
        }
        step();
        if (n > 20000000) fail("interrupt handler does not return");
    }
}


/* ---- the frame handler's state for the renderer, and SNAP.BIN (free-running mode) ----- */
static void oam_segment(void)
{
    if (noamseg >= OAMSEG_MAX) return;
    oamseg[noamseg].line = irqline;
    memcpy(oamseg[noamseg].oam, oam, 544);
    noamseg++;
}

/* rt_frame_start: the shadows the renderer reads, the scroll latches, CGRAM, OAM from line 0 */
static void frame_start(void)
{
    memcpy(io_start + 0x100, io + 0x100, 0x100);
    memcpy(io_start + 0x2200, io + 0x2200, 0x20);
    memcpy(io_start + 0x2300, io + 0x2300, 0x80);
    memcpy(bgofs_start, bgofs, 16);
    memcpy(bgofs_start + 8, m7r, 12);
    memcpy(bgofs_start + 14, m7ofs, 4);
    nirqlog = 0;
    memcpy(cgram_start, cgram, 512);
    noamseg = 0;
    irqline = 0;
    oam_segment();
}

static void put_be(FILE *f, uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; i--) fputc((int)(v >> (i * 8)) & 255, f);
}

static long snap_base;                      /* REF_SNAP_BASE: added to the frame number in the file name */

static void write_snapshot(const char *dir, long frame)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/F%07lX.BIN", dir, frame + snap_base);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite("SMKXSNAP", 1, 8, f);
    put_be(f, 4, 4);
    fwrite(vram, 1, sizeof vram, f);
    fwrite(cgram_start, 1, 512, f);
    fwrite(oam, 1, 544, f);
    fwrite(io_start, 1, sizeof io_start, f);
    for (int i = 0; i < 16; i++) put_be(f, bgofs_start[i], 2);
    put_be(f, nirqlog, 4);
    for (int i = 0; i < IRQLOG_MAX; i++) {
        put_be(f, i < (int)nirqlog ? irqlog[i].line : 0, 2);
        put_be(f, i < (int)nirqlog ? irqlog[i].reg : 0, 2);
        fputc(i < (int)nirqlog ? irqlog[i].value : 0, f);
        fputc(0, f);
    }
    fwrite(wram, 1, sizeof wram, f);
    fwrite(sram, 1, 0x800, f);
    for (int i = 0; i < 256 * 224; i++) fputc(0, f);        /* the Falcon's frame: not part of this run */
    put_be(f, noamseg, 4);
    for (int i = 0; i < OAMSEG_MAX; i++) {
        put_be(f, i < (int)noamseg ? oamseg[i].line : 0, 2);
        put_be(f, 0, 2);
        fwrite(i < (int)noamseg ? oamseg[i].oam : (const uint8_t[544]){0}, 1, 544, f);
    }
    fclose(f);
}

static uint32_t wram_sum(void)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < 0x20000; i += 4) {
        uint32_t w = (uint32_t)wram[i] << 24 | wram[i + 1] << 16 | wram[i + 2] << 8 | wram[i + 3];
        if (i >= (STACK_LO & ~3u) && i <= STACK_HI) {
            /* the stack page: only the bytes below $0111 */
            w = 0;
            for (uint32_t k = 0; k < 4; k++)
                if (i + k < STACK_LO) w |= (uint32_t)wram[i + k] << (24 - 8 * k);
        }
        s += w;
    }
    return s;
}

static uint8_t *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*n + 1);
    if (!d || fread(d, 1, (size_t)*n, f) != (size_t)*n) { perror(path); exit(2); }
    fclose(f);
    return d;
}

static uint32_t symbol(const char *path, const char *name)
{
    long n;
    char *t = (char *)load(path, &n);
    t[n] = 0;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        unsigned a;
        char s[256];
        if (sscanf(l, "%x %255s", &a, s) == 2 && !strcmp(s, name)) {
            free(t);
            return a;
        }
    }
    fprintf(stderr, "symbol %s not found\n", name);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s rom.sfc SMW_U.sym FRAMES [wram.bin]\n", argv[0]);
        return 2;
    }
    long n;
    uint8_t *r = load(argv[1], &n);
    if (n != ROM_SIZE) { fprintf(stderr, "%s: %ld bytes\n", argv[1], n); return 2; }
    memcpy(rom, r, ROM_SIZE);
    free_run = !strcmp(argv[2], "-");
    uint32_t gameloop = free_run ? 0 : symbol(argv[2], "GameLoop");
    long frames = strtol(argv[3], NULL, 0);
    const char *w = getenv("REF_WATCH");
    for (; w && *w && nwatch < 16; nwatch++) {
        char *end;
        watch[nwatch] = (uint32_t)strtoul(w, &end, 16);
        w = *end ? end + 1 : end;
    }
    if (getenv("REF_WATCH_FROM")) watch_from = strtol(getenv("REF_WATCH_FROM"), NULL, 0);
    if (getenv("REF_CALLS_FROM")) calls_from = strtol(getenv("REF_CALLS_FROM"), NULL, 0);

    /* reset: emulation mode, S = $01FF, registers 0 (the runtime's start) */
    memset(&cpu, 0, sizeof cpu);
    cpu.e = 1; cpu.m = cpu.xf = 1;
    cpu.s = 0x1FF;
    cpu.pc = (uint16_t)(rom[0x7FFC] | rom[0x7FFD] << 8);
    /* REF_INPUT: the runtime's SMWX_INPUT ("FRAME:PAD,..."), by game frame as a
     * SMKX_SYNC=1 build reads it: from that frame on, PAD in $4218/$4219. */
    long in_frame[4096];
    unsigned in_pad[4096];
    int nin = 0, in_at = 0;
    unsigned pad = 0;
    for (const char *q = getenv("REF_INPUT"); q && *q && nin < 4096;) {
        char *end;
        in_frame[nin] = strtol(q, &end, 10);
        if (*end != ':') break;
        in_pad[nin++] = (unsigned)strtoul(end + 1, &end, 16);
        q = *end ? end + 1 : end;
    }
    long nmis = 0;
    if (free_run) {
        const char *snapdir = getenv("REF_SNAP_DIR");
        long snap_every = getenv("REF_SNAP_EVERY") ? strtol(getenv("REF_SNAP_EVERY"), NULL, 0) : 1;
        long snap_from = getenv("REF_SNAP_FROM") ? strtol(getenv("REF_SNAP_FROM"), NULL, 0) : 1;
        long budget = getenv("REF_FRAME_INSNS") ? strtol(getenv("REF_FRAME_INSNS"), NULL, 0) : 30000;
        frame_budget = budget;
        /* REF_SNAP_WINDOW=a:b: also every frame from a to b (consecutive frames for the list compositor's models) */
        long win_a = -1, win_b = -1;
        if (getenv("REF_SNAP_WINDOW")) sscanf(getenv("REF_SNAP_WINDOW"), "%ld:%ld", &win_a, &win_b);
        if (getenv("REF_SNAP_BASE")) snap_base = strtol(getenv("REF_SNAP_BASE"), NULL, 0);
        long progress = getenv("REF_PROGRESS") ? strtol(getenv("REF_PROGRESS"), NULL, 0) : 0;
        for (long vbl = 1; !stopped && vbl <= frames; vbl++) {
            wai_hit = 0;
            frame_base = insns;
            nmi_flag_taken = 0;
            for (long k = 0; k < budget && !stopped && !wai_hit; k++) step();
            if (stopped) break;
            cur_frame = vbl;
            frame_start();
            while (in_at < nin && in_frame[in_at] <= vbl - 1) pad = in_pad[in_at++];
            io[0x2218] = (uint8_t)pad;
            io[0x2219] = (uint8_t)(pad >> 8);
            irqline = 0;
            for (int left = 7; left >= 0; left--) {
                if (!(nmitimen & 0x30) || iflag) break;
                uint16_t line = (uint16_t)((((io[0x220A] & 1) << 8) | io[0x2209]) + 1);
                if (line <= irqline || line >= 262) break;
                irqline = line;
                timeup = 1;
                logging = 1;
                interrupt(0x7FEE);
                logging = 0;
                oam_segment();
            }
            if (snapdir && ((vbl >= snap_from && (vbl - snap_from) % snap_every == 0) || (vbl >= win_a && vbl <= win_b)))
                write_snapshot(snapdir, vbl);
            if (progress && vbl % progress == 0)
                fprintf(stderr, "frame %ld: pc $%06X insns %llu inidisp $%02X nmitimen $%02X dma %u apu %u\n", vbl,
                        (uint32_t)cpu.pbr << 16 | cpu.pc, (unsigned long long)insns, io[0x100], nmitimen, dmabytes, apuwrites);
            if (nmitimen & 0x80) {
                rdnmi = 1;
                nmis++;
                interrupt(0x7FEA);
            }
        }
        if (stopped) {
            fprintf(stderr, "stopped at $%06X: %s after %llu instructions, %ld frames\n",
                    stop_pc, stop_why, (unsigned long long)insns, cur_frame);
            return 1;
        }
        fprintf(stderr, "%ld frames, %llu instructions, dma %u, apu writes %u\n", frames,
                (unsigned long long)insns, dmabytes, apuwrites);
        return 0;
    }
    while (!stopped && nmis < frames) {
        /* the main loop's wait: LDA LagFlag : BEQ GameLoop with LagFlag 0 */
        if (((uint32_t)cpu.pbr << 16 | cpu.pc) == gameloop && wram[LAGFLAG] == 0 &&
            (nmitimen & 0x80)) {
            /* one VBL: the pad latch, the IRQ chain in line order, the
             * checkpoint, the NMI */
            while (in_at < nin && in_frame[in_at] <= nmis) pad = in_pad[in_at++];
            io[0x2218] = (uint8_t)pad;
            io[0x2219] = (uint8_t)(pad >> 8);
            irqline = 0;
            for (int left = 7; left >= 0; left--) {
                if (!(nmitimen & 0x30) || iflag) break;
                uint16_t line = (uint16_t)((((io[0x220A] & 1) << 8) | io[0x2209]) + 1);
                if (line <= irqline || line >= 262) break;
                irqline = line;
                timeup = 1;
                interrupt(0x7FEE);
            }
            printf("sum %ld %08X\n", nmis, wram_sum());
            if (nmis == frames - 1 && argc > 4)
                break;                         /* dump at this checkpoint */
            rdnmi = 1;
            nmis++;
            cur_frame = nmis;
            interrupt(0x7FEA);
            continue;
        }
        step();
    }
    if (stopped) {
        fprintf(stderr, "stopped at $%06X: %s after %llu instructions, %ld frames\n",
                stop_pc, stop_why, (unsigned long long)insns, nmis);
        return 1;
    }
    fprintf(stderr, "%ld frames, %llu instructions, GameMode $%02X, dma %u, apu writes %u\n",
            nmis, (unsigned long long)insns, wram[0x100], dmabytes, apuwrites);
    if (argc > 4) {
        FILE *f = fopen(argv[4], "wb");
        if (!f) { perror(argv[4]); return 2; }
        fwrite(wram, 1, sizeof wram, f);
        fclose(f);
    }
    return 0;
}
