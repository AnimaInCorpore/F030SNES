/* SuperFX (GSU-2) core; see gsu.h.
 *
 * Pipeline: the byte after the current opcode is already fetched while it
 * executes. During an instruction R15 holds the address after the opcode
 * (after its operands once they are read); unless the instruction wrote
 * R15, R15 advances by one afterwards. A write to R15 (branch, JMP, LOOP,
 * IWT R15, ...) therefore lets the already fetched byte execute first: the
 * delay slot.
 */
#include "gsu.h"

#define SR() (g->r[g->sreg])

static uint8_t bus_read(const Gsu *g, uint32_t addr)
{
    uint32_t bank = (addr >> 16) & 0x7F;
    if (bank < 0x40)            /* LoROM view, bit 15 ignored */
        return g->rom[((bank & 0x3F) << 15) | (addr & 0x7FFF)];
    if (bank < 0x60)            /* linear view */
        return g->rom[addr & 0x1FFFFF];
    return g->ram[addr & GSU_RAM_MASK];
}

static uint8_t fetch(Gsu *g, uint16_t addr)
{
    return bus_read(g, ((uint32_t)g->pbr << 16) | addr);
}

static uint8_t ram_rd(Gsu *g, uint16_t addr)
{
    return g->ram[(((uint32_t)g->rambr << 16) | addr) & GSU_RAM_MASK];
}

static void ram_wr(Gsu *g, uint16_t addr, uint8_t v)
{
    g->ram[(((uint32_t)g->rambr << 16) | addr) & GSU_RAM_MASK] = v;
}

static uint8_t rom_buffer(Gsu *g)
{
    return bus_read(g, ((uint32_t)g->rombr << 16) | g->r[14]);
}

static void reset_prefix(Gsu *g)
{
    g->b = 0;
    g->alt1 = 0;
    g->alt2 = 0;
    g->sreg = 0;
    g->dreg = 0;
}

static void set_r(Gsu *g, unsigned n, uint16_t v)
{
    g->r[n] = v;
    if (n == 15)
        g->r15_written = 1;
}

static void set_dr(Gsu *g, uint16_t v)
{
    set_r(g, g->dreg, v);
}

static void sz(Gsu *g, uint16_t v)
{
    g->s = (v & 0x8000) != 0;
    g->z = v == 0;
}

static uint8_t pipe(Gsu *g)
{
    uint8_t v = g->pipeline;
    g->r[15]++;
    g->pipeline = fetch(g, g->r[15]);
    g->r15_written = 0;
    return v;
}

uint8_t gsu_color(Gsu *g, uint8_t src)
{
    if (g->por & 0x04)                      /* high nibble */
        return (uint8_t)((g->colr & 0xF0) | (src >> 4));
    if (g->por & 0x08)                      /* freeze high */
        return (uint8_t)((g->colr & 0xF0) | (src & 0x0F));
    return src;
}

static unsigned char_number(Gsu *g, uint8_t x, uint8_t y)
{
    unsigned ht = (g->por & 0x10) ? 3 : (unsigned)(((g->scmr >> 4) & 2) | ((g->scmr >> 2) & 1));
    switch (ht) {
    case 0: return ((x & 0xF8u) << 1) + ((y & 0xF8u) >> 3);
    case 1: return ((x & 0xF8u) << 1) + ((x & 0xF8u) >> 1) + ((y & 0xF8u) >> 3);
    case 2: return ((x & 0xF8u) << 1) + (x & 0xF8u) + ((y & 0xF8u) >> 3);
    default: return ((y & 0x80u) << 2) + ((x & 0x80u) << 1) + ((y & 0x78u) << 1) + ((x & 0x78u) >> 3);
    }
}

static unsigned bpp_of(Gsu *g)
{
    unsigned md = g->scmr & 3;
    return 2u << (md - (md >> 1));          /* 2, 4, 4, 8 */
}

static uint32_t char_row(Gsu *g, uint8_t x, uint8_t y, unsigned bpp)
{
    return char_number(g, x, y) * (bpp << 3) + ((uint32_t)g->scbr << 10) + (uint32_t)(y & 7) * 2;
}

static void flush_pixels(Gsu *g, GsuPixelCache *c)
{
    unsigned bpp, n, i;
    uint32_t addr;
    uint8_t x, y;
    if (c->bitpend == 0)
        return;
    x = (uint8_t)(c->offset << 3);
    y = (uint8_t)(c->offset >> 5);
    bpp = bpp_of(g);
    addr = char_row(g, x, y, bpp);
    for (n = 0; n < bpp; n++) {
        uint32_t a = (addr + ((n >> 1) << 4) + (n & 1)) & GSU_RAM_MASK;
        uint8_t data = 0;
        for (i = 0; i < 8; i++)
            data |= (uint8_t)(((c->data[i] >> n) & 1) << i);
        if (c->bitpend != 0xFF)
            data = (uint8_t)((data & c->bitpend) | (g->ram[a] & ~c->bitpend));
        g->ram[a] = data;
    }
    c->bitpend = 0;
}

void gsu_plot(Gsu *g, uint8_t x, uint8_t y)
{
    uint8_t col = g->colr;
    uint16_t off;
    if (!(g->por & 0x01)) {                 /* not transparent-plotting */
        if ((g->scmr & 3) == 3) {
            if (g->por & 0x08) {
                if ((col & 0x0F) == 0)
                    return;
            } else if (col == 0) {
                return;
            }
        } else if ((col & 0x0F) == 0) {
            return;
        }
    }
    if ((g->por & 0x02) && (g->scmr & 3) != 3) {    /* dither */
        if ((x ^ y) & 1)
            col >>= 4;
        col &= 0x0F;
    }
    off = (uint16_t)((y << 5) + (x >> 3));
    if (g->pc[0].offset != off) {
        flush_pixels(g, &g->pc[1]);
        g->pc[1] = g->pc[0];
        g->pc[0].bitpend = 0;
        g->pc[0].offset = off;
    }
    x = (uint8_t)((x & 7) ^ 7);
    g->pc[0].data[x] = col;
    g->pc[0].bitpend |= (uint8_t)(1u << x);
    if (g->pc[0].bitpend == 0xFF) {
        flush_pixels(g, &g->pc[1]);
        g->pc[1] = g->pc[0];
        g->pc[0].bitpend = 0;
    }
}

uint8_t gsu_rpix(Gsu *g, uint8_t x, uint8_t y)
{
    unsigned bpp, n;
    uint32_t addr;
    uint8_t data = 0;
    flush_pixels(g, &g->pc[1]);
    flush_pixels(g, &g->pc[0]);
    bpp = bpp_of(g);
    addr = char_row(g, x, y, bpp);
    x = (uint8_t)((x & 7) ^ 7);
    for (n = 0; n < bpp; n++) {
        uint32_t a = (addr + ((n >> 1) << 4) + (n & 1)) & GSU_RAM_MASK;
        data |= (uint8_t)(((g->ram[a] >> x) & 1) << n);
    }
    return data;
}

const uint8_t *gsu_rom_view(Gsu *g, uint8_t bank, uint16_t *mask)
{
    bank &= 0x7F;
    if (bank < 0x40) {
        *mask = 0x7FFF;
        return g->rom + ((uint32_t)(bank & 0x3F) << 15);
    }
    if (bank < 0x60) {
        *mask = 0xFFFF;
        return g->rom + (((uint32_t)bank << 16) & 0x1FFFFF);
    }
    *mask = GSU_RAM_MASK;
    return g->ram;
}

void gsu_resume_at(Gsu *g, uint16_t pc)
{
    /* the state step() expects before executing the opcode at pc */
    g->pipeline = fetch(g, pc);
    g->r[15] = (uint16_t)(pc + 1);
}

void gsu_init(Gsu *g, const uint8_t *rom, uint8_t *ram)
{
    unsigned i;
    uint8_t *p = (uint8_t *)g;
    for (i = 0; i < sizeof *g; i++)
        p[i] = 0;
    g->rom = rom;
    g->ram = ram;
    g->pipeline = 0x01;         /* NOP */
}

static void step(Gsu *g)
{
    uint8_t op, n;
    uint16_t sr, v;
    int32_t r;
    op = g->pipeline;
    g->pipeline = fetch(g, g->r[15]);
    g->r15_written = 0;
    n = op & 15;
    sr = SR();
    switch (op >> 4) {
    case 0x0:
        switch (n) {
        case 0x0:                           /* STOP */
            if (!(g->cfgr & 0x80))
                g->irq = 1;
            g->g = 0;
            g->pipeline = 0x01;
            reset_prefix(g);
            break;
        case 0x1:                           /* NOP */
            reset_prefix(g);
            break;
        case 0x2:                           /* CACHE */
            if (g->cbr != (g->r[15] & 0xFFF0))
                g->cbr = g->r[15] & 0xFFF0;
            reset_prefix(g);
            break;
        case 0x3:                           /* LSR */
            g->cy = sr & 1;
            v = sr >> 1;
            set_dr(g, v);
            sz(g, v);
            reset_prefix(g);
            break;
        case 0x4:                           /* ROL */
            v = (uint16_t)((sr << 1) | g->cy);
            g->cy = (sr >> 15) & 1;
            set_dr(g, v);
            sz(g, v);
            reset_prefix(g);
            break;
        default: {                          /* branches: no prefix reset */
            int c;
            int8_t d;
            switch (n) {
            case 0x5: c = 1; break;
            case 0x6: c = g->s == g->ov; break;
            case 0x7: c = g->s != g->ov; break;
            case 0x8: c = !g->z; break;
            case 0x9: c = g->z; break;
            case 0xA: c = !g->s; break;
            case 0xB: c = g->s; break;
            case 0xC: c = !g->cy; break;
            case 0xD: c = g->cy; break;
            case 0xE: c = !g->ov; break;
            default: c = g->ov; break;
            }
            d = (int8_t)pipe(g);
            if (c)
                set_r(g, 15, (uint16_t)(g->r[15] + d));
            break;
        }
        }
        break;
    case 0x1:                               /* TO / MOVE */
        if (!g->b) {
            g->dreg = n;
        } else {
            set_r(g, n, sr);
            reset_prefix(g);
        }
        break;
    case 0x2:                               /* WITH */
        g->sreg = g->dreg = n;
        g->b = 1;
        break;
    case 0x3:
        if (n < 12) {                       /* STW / STB (Rn) */
            g->ramaddr = g->r[n];
            ram_wr(g, g->ramaddr, (uint8_t)sr);
            if (!g->alt1)
                ram_wr(g, g->ramaddr ^ 1, (uint8_t)(sr >> 8));
            reset_prefix(g);
        } else if (n == 12) {               /* LOOP */
            g->r[12]--;
            sz(g, g->r[12]);
            if (!g->z)
                set_r(g, 15, g->r[13]);
            reset_prefix(g);
        } else if (n == 13) {               /* ALT1 */
            g->b = 0;
            g->alt1 = 1;
        } else if (n == 14) {               /* ALT2 */
            g->b = 0;
            g->alt2 = 1;
        } else {                            /* ALT3 */
            g->b = 0;
            g->alt1 = g->alt2 = 1;
        }
        break;
    case 0x4:
        if (n < 12) {                       /* LDW / LDB (Rn) */
            g->ramaddr = g->r[n];
            v = ram_rd(g, g->ramaddr);
            if (!g->alt1)
                v |= (uint16_t)(ram_rd(g, g->ramaddr ^ 1) << 8);
            set_dr(g, v);
            reset_prefix(g);
        } else if (n == 12) {
            if (!g->alt1) {                 /* PLOT */
                gsu_plot(g, (uint8_t)g->r[1], (uint8_t)g->r[2]);
                g->r[1]++;
            } else {                        /* RPIX */
                v = gsu_rpix(g, (uint8_t)g->r[1], (uint8_t)g->r[2]);
                set_dr(g, v);
                sz(g, v);
            }
            reset_prefix(g);
        } else if (n == 13) {               /* SWAP */
            v = (uint16_t)((sr >> 8) | (sr << 8));
            set_dr(g, v);
            sz(g, v);
            reset_prefix(g);
        } else if (n == 14) {
            if (!g->alt1)                   /* COLOR */
                g->colr = gsu_color(g, (uint8_t)sr);
            else                            /* CMODE */
                g->por = (uint8_t)sr;
            reset_prefix(g);
        } else {                            /* NOT */
            v = (uint16_t)~sr;
            set_dr(g, v);
            sz(g, v);
            reset_prefix(g);
        }
        break;
    case 0x5: {                             /* ADD / ADC / ADD # / ADC # */
        uint16_t o = g->alt2 ? n : g->r[n];
        r = (int32_t)sr + o + (g->alt1 ? g->cy : 0);
        g->ov = (~(sr ^ o) & (o ^ r) & 0x8000) != 0;
        g->cy = r >= 0x10000;
        v = (uint16_t)r;
        sz(g, v);
        set_dr(g, v);
        reset_prefix(g);
        break;
    }
    case 0x6: {                             /* SUB / SBC / SUB # / CMP */
        uint16_t o = (g->alt2 && !g->alt1) ? n : g->r[n];
        r = (int32_t)sr - o - ((g->alt1 && !g->alt2) ? !g->cy : 0);
        g->ov = ((sr ^ o) & (sr ^ r) & 0x8000) != 0;
        g->cy = r >= 0;
        v = (uint16_t)r;
        sz(g, v);
        if (!(g->alt1 && g->alt2))
            set_dr(g, v);
        reset_prefix(g);
        break;
    }
    case 0x7:
        if (n == 0) {                       /* MERGE */
            v = (uint16_t)((g->r[7] & 0xFF00) | (g->r[8] >> 8));
            g->ov = (v & 0xC0C0) != 0;
            g->s = (v & 0x8080) != 0;
            g->cy = (v & 0xE0E0) != 0;
            g->z = (v & 0xF0F0) != 0;
            set_dr(g, v);
        } else {                            /* AND / BIC / AND # / BIC # */
            uint16_t o = g->alt2 ? n : g->r[n];
            if (g->alt1)
                o = (uint16_t)~o;
            v = sr & o;
            sz(g, v);
            set_dr(g, v);
        }
        reset_prefix(g);
        break;
    case 0x8: {                             /* MULT / UMULT / MULT # / UMULT # */
        uint16_t o = g->alt2 ? n : g->r[n];
        if (!g->alt1)
            v = (uint16_t)((int16_t)(int8_t)sr * (int16_t)(int8_t)o);
        else
            v = (uint16_t)((uint8_t)sr * (uint8_t)o);
        sz(g, v);
        set_dr(g, v);
        reset_prefix(g);
        break;
    }
    case 0x9:
        switch (n) {
        case 0x0:                           /* SBK */
            ram_wr(g, g->ramaddr, (uint8_t)sr);
            ram_wr(g, g->ramaddr ^ 1, (uint8_t)(sr >> 8));
            break;
        case 0x1: case 0x2: case 0x3: case 0x4:     /* LINK #n */
            g->r[11] = (uint16_t)(g->r[15] + n);
            break;
        case 0x5:                           /* SEX */
            v = (uint16_t)(int16_t)(int8_t)sr;
            sz(g, v);
            set_dr(g, v);
            break;
        case 0x6:                           /* ASR / DIV2 */
            g->cy = sr & 1;
            if (!g->alt1)
                v = (uint16_t)((int16_t)sr >> 1);
            else
                v = (uint16_t)(((int16_t)sr >> 1) + (((uint32_t)sr + 1) >> 16));
            sz(g, v);
            set_dr(g, v);
            break;
        case 0x7:                           /* ROR */
            v = (uint16_t)((g->cy << 15) | (sr >> 1));
            g->cy = sr & 1;
            sz(g, v);
            set_dr(g, v);
            break;
        case 0xE:                           /* LOB */
            v = sr & 0xFF;
            g->s = (v & 0x80) != 0;
            g->z = v == 0;
            set_dr(g, v);
            break;
        case 0xF: {                         /* FMULT / LMULT */
            uint32_t res = (uint32_t)((int32_t)(int16_t)sr * (int32_t)(int16_t)g->r[6]);
            if (g->alt1)
                g->r[4] = (uint16_t)res;
            v = (uint16_t)(res >> 16);
            g->cy = (res >> 15) & 1;
            sz(g, v);
            set_dr(g, v);
            break;
        }
        default:                            /* $98-$9D JMP / LJMP Rn */
            if (!g->alt1) {
                set_r(g, 15, g->r[n]);
            } else {
                g->pbr = g->r[n] & 0x7F;
                set_r(g, 15, sr);
                g->cbr = g->r[15] & 0xFFF0;
            }
            break;
        }
        reset_prefix(g);
        break;
    case 0xA:
        if (g->alt1) {                      /* LMS Rn, (yy) */
            g->ramaddr = (uint16_t)(pipe(g) << 1);
            v = (uint16_t)(ram_rd(g, g->ramaddr) | (ram_rd(g, g->ramaddr ^ 1) << 8));
            set_r(g, n, v);
        } else if (g->alt2) {               /* SMS (yy), Rn */
            g->ramaddr = (uint16_t)(pipe(g) << 1);
            ram_wr(g, g->ramaddr, (uint8_t)g->r[n]);
            ram_wr(g, g->ramaddr ^ 1, (uint8_t)(g->r[n] >> 8));
        } else {                            /* IBT Rn, #pp */
            set_r(g, n, (uint16_t)(int16_t)(int8_t)pipe(g));
        }
        reset_prefix(g);
        break;
    case 0xB:                               /* FROM / MOVES */
        if (!g->b) {
            g->sreg = n;
        } else {
            v = g->r[n];
            g->ov = (v & 0x80) != 0;
            sz(g, v);
            set_dr(g, v);
            reset_prefix(g);
        }
        break;
    case 0xC:
        if (n == 0) {                       /* HIB */
            v = sr >> 8;
            g->s = (v & 0x80) != 0;
            g->z = v == 0;
            set_dr(g, v);
        } else {                            /* OR / XOR / OR # / XOR # */
            uint16_t o = g->alt2 ? n : g->r[n];
            v = g->alt1 ? (uint16_t)(sr ^ o) : (uint16_t)(sr | o);
            sz(g, v);
            set_dr(g, v);
        }
        reset_prefix(g);
        break;
    case 0xD:
        if (n < 15) {                       /* INC Rn */
            v = (uint16_t)(g->r[n] + 1);
            set_r(g, n, v);
            sz(g, v);
        } else if (!g->alt2) {              /* GETC */
            g->colr = gsu_color(g, rom_buffer(g));
        } else if (!g->alt1) {              /* RAMB */
            g->rambr = sr & 0x01;
        } else {                            /* ROMB */
            g->rombr = sr & 0x7F;
        }
        reset_prefix(g);
        break;
    case 0xE:
        if (n < 15) {                       /* DEC Rn */
            v = (uint16_t)(g->r[n] - 1);
            set_r(g, n, v);
            sz(g, v);
        } else {
            uint8_t b = rom_buffer(g);
            if (!g->alt1 && !g->alt2)       /* GETB */
                v = b;
            else if (g->alt1 && !g->alt2)   /* GETBH */
                v = (uint16_t)((b << 8) | (sr & 0xFF));
            else if (!g->alt1)              /* GETBL */
                v = (uint16_t)((sr & 0xFF00) | b);
            else                            /* GETBS */
                v = (uint16_t)(int16_t)(int8_t)b;
            set_dr(g, v);
        }
        reset_prefix(g);
        break;
    default:                                /* $F */
        if (g->alt1) {                      /* LM Rn, (xx) */
            g->ramaddr = pipe(g);
            g->ramaddr |= (uint16_t)(pipe(g) << 8);
            v = (uint16_t)(ram_rd(g, g->ramaddr) | (ram_rd(g, g->ramaddr ^ 1) << 8));
            set_r(g, n, v);
        } else if (g->alt2) {               /* SM (xx), Rn */
            g->ramaddr = pipe(g);
            g->ramaddr |= (uint16_t)(pipe(g) << 8);
            ram_wr(g, g->ramaddr, (uint8_t)g->r[n]);
            ram_wr(g, g->ramaddr ^ 1, (uint8_t)(g->r[n] >> 8));
        } else {                            /* IWT Rn, #xxxx */
            v = pipe(g);
            v |= (uint16_t)(pipe(g) << 8);
            set_r(g, n, v);
        }
        reset_prefix(g);
        break;
    }
    if (!g->r15_written)
        g->r[15]++;
    g->r15_written = 0;
}

void gsu_step1(Gsu *g)
{
    step(g);
    g->steps++;
}

void gsu_run(Gsu *g, uint32_t limit)
{
    uint32_t count = 0;
    g->start_pc = g->r[15];
    g->start_pbr = g->pbr;
    g->fault = 0;
    while (g->g) {
        step(g);
        count++;
        if (limit && count >= limit) {
            g->fault = 1;
            break;
        }
    }
    g->steps += count;
    g->last_steps = count;
}

uint8_t gsu_read_reg(Gsu *g, uint16_t addr)
{
    if (addr >= 0x3000 && addr < 0x3020) {
        uint16_t r = g->r[(addr >> 1) & 15];
        return (uint8_t)((addr & 1) ? r >> 8 : r);
    }
    if (addr >= 0x3100 && addr < 0x3300)
        return g->cache[(g->cbr + addr) & 511];
    switch (addr) {
    case 0x3030:
        return (uint8_t)((g->z << 1) | (g->cy << 2) | (g->s << 3) | (g->ov << 4) | (g->g << 5));
    case 0x3031: {
        uint8_t v = (uint8_t)(g->alt1 | (g->alt2 << 1) | (g->b << 4) | (g->irq << 7));
        g->irq = 0;
        return v;
    }
    case 0x3034: return g->pbr;
    case 0x3036: return g->rombr;
    case 0x303B: return 0x04;       /* VCR: GSU-2 */
    case 0x303C: return g->rambr;
    case 0x303E: return (uint8_t)g->cbr;
    case 0x303F: return (uint8_t)(g->cbr >> 8);
    default: return 0;
    }
}

int gsu_write_reg(Gsu *g, uint16_t addr, uint8_t data)
{
    if (addr >= 0x3000 && addr < 0x3020) {
        unsigned n = (addr >> 1) & 15;
        if (addr & 1)
            g->r[n] = (uint16_t)((data << 8) | (g->r[n] & 0xFF));
        else
            g->r[n] = (uint16_t)((g->r[n] & 0xFF00) | data);
        if (addr == 0x301F) {
            g->g = 1;
            return 1;
        }
        return 0;
    }
    if (addr >= 0x3100 && addr < 0x3300) {
        g->cache[(g->cbr + addr) & 511] = data;
        return 0;
    }
    switch (addr) {
    case 0x3030: {
        uint8_t was = g->g;
        g->z = (data >> 1) & 1;
        g->cy = (data >> 2) & 1;
        g->s = (data >> 3) & 1;
        g->ov = (data >> 4) & 1;
        g->g = (data >> 5) & 1;
        if (was && !g->g)
            g->cbr = 0;
        return g->g && !was;
    }
    case 0x3031:
        g->alt1 = data & 1;
        g->alt2 = (data >> 1) & 1;
        g->b = (data >> 4) & 1;
        g->irq = (data >> 7) & 1;
        return 0;
    case 0x3033: g->bramr = data & 1; return 0;
    case 0x3034: g->pbr = data & 0x7F; return 0;
    case 0x3037: g->cfgr = data; return 0;
    case 0x3038: g->scbr = data; return 0;
    case 0x3039: g->clsr = data & 1; return 0;
    case 0x303A: g->scmr = data; return 0;
    default: return 0;
    }
}
