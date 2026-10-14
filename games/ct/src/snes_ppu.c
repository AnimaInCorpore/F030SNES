/* SNES picture composition: see snes_ppu.h for scope.
 *
 * Scanlines 1-224 are visible (row = scanline - 1). HDMA runs at the start
 * of every scanline from 0, before that scanline is composed; IRQ-logged
 * writes take effect from their line. Freestanding: no libc.
 */
#include "snes_ppu.h"

void ppu_init(void)
{
    /* Colour comes from the adaptive palette (palette.h); nothing to build. */
}

uint8_t ppu_abus_hirom(uint32_t a, void *ctx)
{
    const PpuBus *bus = (const PpuBus *)ctx;
    uint32_t bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F)
        return bus->wram[((bank - 0x7E) << 16) | off];
    if ((bank & 0x7F) < 0x40) {
        if (off < 0x2000) return bus->wram[off];
        if (off >= 0x8000) return bus->rom[(((bank & 0x3F) << 16) | off) & 0x3FFFFF];
        if (off >= 0x6000 && (bank & 0x3F) >= 0x20) return bus->sram[off & 0x1FFF];
        return 0;                       /* I/O is not an HDMA source */
    }
    return bus->rom[(((bank & 0x3F) << 16) | off) & 0x3FFFFF];
}

/* The same map as host pointers: WRAM and ROM are contiguous to the end of
 * their region within the bank; I/O and SRAM go through ppu_abus_hirom. */
const uint8_t *ppu_aptr_hirom(uint32_t a, void *ctx, uint32_t *len)
{
    const PpuBus *bus = (const PpuBus *)ctx;
    uint32_t bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) {
        *len = 0x10000 - off;
        return bus->wram + (((bank - 0x7E) << 16) | off);
    }
    if ((bank & 0x7F) < 0x40) {
        if (off < 0x2000) {
            *len = 0x2000 - off;
            return bus->wram + off;
        }
        if (off >= 0x8000) {
            *len = 0x10000 - off;
            return bus->rom + ((((bank & 0x3F) << 16) | off) & 0x3FFFFF);
        }
        *len = 0;
        return 0;
    }
    *len = 0x10000 - off;
    return bus->rom + ((((bank & 0x3F) << 16) | off) & 0x3FFFFF);
}

/* LoROM (SMW: 512 KiB ROM, 2 KiB SRAM): ROM at b:$8000 is (b & $7F) * 32 KiB, banks $40-$6F mirror
 * it in their low half, $70-$7D are SRAM below $8000. HDMA reads its tables through these. */
uint8_t ppu_abus_lorom(uint32_t a, void *ctx)
{
    const PpuBus *bus = (const PpuBus *)ctx;
    uint32_t bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F)
        return bus->wram[((bank - 0x7E) << 16) | off];
    if (off >= 0x8000)
        return bus->rom[(((bank & 0x7F) << 15) | (off & 0x7FFF)) & 0x7FFFF];
    if ((bank & 0x7F) < 0x40)
        return off < 0x2000 ? bus->wram[off] : 0;   /* I/O is not an HDMA source */
    if ((bank & 0x7F) >= 0x70)
        return bus->sram[off & 0x7FF];
    return bus->rom[(((bank & 0x7F) << 15) | off) & 0x7FFFF];
}

const uint8_t *ppu_aptr_lorom(uint32_t a, void *ctx, uint32_t *len)
{
    const PpuBus *bus = (const PpuBus *)ctx;
    uint32_t bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) {
        *len = 0x10000 - off;
        return bus->wram + (((bank - 0x7E) << 16) | off);
    }
    if (off >= 0x8000) {
        *len = 0x10000 - off;
        return bus->rom + ((((bank & 0x7F) << 15) | (off & 0x7FFF)) & 0x7FFFF);
    }
    if ((bank & 0x7F) < 0x40) {
        if (off < 0x2000) {
            *len = 0x2000 - off;
            return bus->wram + off;
        }
        *len = 0;
        return 0;
    }
    if ((bank & 0x7F) >= 0x70) {
        *len = 0;
        return 0;
    }
    *len = 0x8000 - off;
    return bus->rom + ((((bank & 0x7F) << 15) | off) & 0x7FFFF);
}

/* ---- optional stage profile (Falcon profile builds) ----------------------- */

#ifdef PPU_PROFILE
uint32_t (*ppu_prof_clock)(void);
uint32_t ppu_prof[PPU_PROF_STAGES];
#define PROF_START() uint32_t prof_t = ppu_prof_clock()
#define PROF_MARK(i) do { uint32_t prof_u = ppu_prof_clock(); \
                          ppu_prof[i] += prof_u - prof_t; prof_t = prof_u; } while (0)
#else
#define PROF_START() do {} while (0)
#define PROF_MARK(i) do {} while (0)
#endif

/* ---- register state for one frame -------------------------------------- */

typedef PpuRegs Regs;

/* changed: set when the register state a row is composed with differs
 * (HDMA often rewrites the same values every line; the latches alone are
 * not state a row sees). */
static void reg_write(Regs *s, uint16_t reg, uint8_t v)
{
    uint8_t n = (uint8_t)(reg - 0x2100);
    if (ppu_replay_nocg && (n == 0x2D || n == 0x30 || n == 0x31 || n == 0x32))
        return;                         /* colour math is left out too: its registers keep the frame-start values */
    if (s->r[n] != v) {
        s->r[n] = v;
        s->changed = 1;
    }
    if (n >= 0x0D && n <= 0x14) {
        int i = n - 0x0D;
        uint16_t old = s->bgofs[i], nv;
        if (i & 1)
            nv = (uint16_t)((v << 8) | s->bgold);
        else
            nv = (uint16_t)((v << 8) | (s->bgold & 0xF8) | ((old >> 8) & 7));
        if (nv != old) {
            s->bgofs[i] = nv;
            s->changed = 1;
        }
        s->bgold = v;
        if (n <= 0x0E) {                /* $210D/$210E also feed Mode 7 */
            nv = (uint16_t)((v << 8) | s->m7old);
            if (nv != s->m7ofs[n - 0x0D]) {
                s->m7ofs[n - 0x0D] = nv;
                s->changed = 1;
            }
            s->m7old = v;
        }
    } else if (n >= 0x1B && n <= 0x20) {
        int16_t nv = (int16_t)((v << 8) | s->m7old);
        if (nv != s->m7[n - 0x1B]) {
            s->m7[n - 0x1B] = nv;
            s->changed = 1;
        }
        s->m7old = v;
    } else if (n == 0x32) {
        for (int k = 0; k < 3; k++)
            if ((v >> (5 + k)) & 1) {
                if (s->fixed[k] != (v & 31)) s->changed = 1;
                s->fixed[k] = v & 31;
            }
    } else if (n == 0x21) {
        s->cgaddr = (uint16_t)(v << 1);
        s->cgflip = 0;
    } else if (n == 0x22) {
        if (!s->cgflip) {
            s->cglatch = v;
            s->cgflip = 1;
        } else {
            uint8_t *c = &s->cgram[s->cgaddr & 0x1FE];
            if (c[0] != s->cglatch || c[1] != (v & 0x7F)) {
                c[0] = s->cglatch;
                c[1] = v & 0x7F;
                if (!ppu_replay_nocg) s->changed = 1;
                s->cgwrites++;
            }
            s->cgaddr += 2;
            s->cgflip = 0;
        }
    }
}

/* ---- HDMA ---------------------------------------------------------------- */

typedef struct {
    uint8_t active, dotransfer, repeat, count;
    uint8_t n, ind, breg;      /* bytes per unit, indirect, B-bus register */
    int8_t m7i;                /* mode 2 into $211B-$2120: that m7[] index */
    const uint8_t *off;        /* the unit's register offsets */
    uint32_t table;            /* A-bus address of the table pointer */
    uint32_t indirect;         /* A-bus address of indirect data */
    const uint8_t *tp, *ip;    /* host pointers at table / indirect, */
    uint32_t tlen, ilen;       /* valid for this many more bytes */
    uint8_t idem, have;        /* repeating a unit changes nothing; last is valid */
    int next;                  /* the line of its next transfer */
    uint8_t last[4];           /* the unit written last */
} Hdma;

/* Registers whose writes are not a plain store: write-twice pairs (a unit
 * that writes each of them twice in a row leaves the same value whatever
 * the latch held), and data ports that advance an address. */
static int reg_twice(int n) { return (n >= 0x0D && n <= 0x14) || (n >= 0x1B && n <= 0x20); }
static int reg_port(int n) { return n == 0x04 || n == 0x18 || n == 0x19 || n == 0x22 || n == 0x80; }

/* A channel's unit equal to its last one may be skipped when idem, as long
 * as no channel writes a write-twice register halfway (it would see the
 * shared latch the skipped unit leaves) and no IRQ write came in between. */
static int hdma_skip_ok;

static const uint8_t hdma_len[8] = {1, 2, 2, 4, 4, 4, 2, 4};
static const uint8_t hdma_off[8][4] = {
    {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 1, 1},
    {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
};

/* Set when an HDMA table or indirect data byte was read from RAM (WRAM, SRAM, the registers' region; conservative):
 * a replay that read ROM only depends on the registers and the IRQ log, which lc_plan uses to keep its result. */
int ppu_replay_ram;

static uint8_t abus(const PpuInput *in, uint32_t a)
{
    return in->abus(a & 0xFFFFFF, in->ctx);
}

/* The byte at *a, which then advances within its bank; *p and *len follow it
 * as a host pointer while the region lasts. */
static inline uint8_t hdma_read(const PpuInput *in, uint32_t *a, const uint8_t **p, uint32_t *len)
{
    uint8_t v;
    if (!*len) {
        uint32_t bank = (*a >> 16) & 0x7F, off = *a & 0xFFFF;
        if (((*a >> 16) & 0xFE) == 0x7E || (off < 0x8000 && (bank < 0x40 || (bank >= 0x70 && bank < 0x7E))))
            ppu_replay_ram = 1;
        if (in->aptr) *p = in->aptr(*a & 0xFFFFFF, in->ctx, len);
    }
    if (*len) {
        v = *(*p)++;
        (*len)--;
    } else {
        v = abus(in, *a);
    }
    *a = (*a & 0xFF0000) | ((*a + 1) & 0xFFFF);
    if (!(*a & 0xFFFF)) *len = 0;       /* wrapped within the bank */
    return v;
}

static void hdma_line_counter(const PpuInput *in, Hdma *h, const uint8_t *regs)
{
    uint8_t c = hdma_read(in, &h->table, &h->tp, &h->tlen);
    if (c == 0) {
        h->active = 0;
        return;
    }
    h->repeat = c & 0x80;
    h->count = c & 0x7F;
    if (c == 0x80) {                    /* 128 lines, repeat */
        h->count = 128;
        h->repeat = 0x80;
    }
    if (regs[0] & 0x40) {               /* indirect: 16-bit address, bank $43x7 */
        uint8_t lo = hdma_read(in, &h->table, &h->tp, &h->tlen);
        uint8_t hi = hdma_read(in, &h->table, &h->tp, &h->tlen);
        h->indirect = ((uint32_t)regs[7] << 16) | ((uint32_t)hi << 8) | lo;
        h->ilen = 0;
    }
    h->dotransfer = 1;
}

/* Registers the renderer reads ($2100 + n): a channel that writes none of
 * them (windows, colour math) changes nothing here and is not walked. */
int ppu_walk_all;
static int win_on;              /* this frame's windows can hide a layer */
static int math_on;             /* this frame's colour math can change a pixel */

/* The list compositor (F030SMW's src/smkx_lc.c) draws frames with the palette of the frame start and shifted by a few
 * pixels: ppu_replay_nocg leaves colour math (TS, CGWSEL, CGADSUB, COLDATA) and CGRAM writes out (an HDMA gradient writing only CGRAM then costs nothing and does not
 * make rows differ), ppu_replay_dx subtracts dx from every BG horizontal scroll the callback sees (the frame's own
 * and the HDMA's). */
int ppu_replay_nocg;
int ppu_replay_dx;

static int reg_used(int n)
{
    if (ppu_replay_nocg && (n == 0x21 || n == 0x22))
        return 0;
    if (n <= 0x14 || (n >= 0x1A && n <= 0x22) || n == 0x2C)
        return 1;
    if ((ppu_walk_all || math_on) && n <= 0x33)
        return 1;
    return win_on && ((n >= 0x23 && n <= 0x2B) || n == 0x2E);
}

/* Whether colour math can change a pixel anywhere in the frame: CGADSUB
 * names a layer and CGWSEL does not say never, among the frame's start
 * values and IRQ writes; an HDMA channel writing CGWSEL or CGADSUB counts
 * as yes. Then channels on the sub screen, windows and COLDATA are walked
 * too (the planner's palette for colour math, dsp_math_palette). */
static int math_possible(const PpuInput *in)
{
    uint8_t en = in->io[0x420C - 0x2000];
    for (int ch = 0; ch < 8; ch++) {
        if (!((en >> ch) & 1)) continue;
        const uint8_t *regs = in->io + (0x4300 - 0x2000) + ch * 16;
        int mode = regs[0] & 7;
        for (int i = 0; i < hdma_len[mode]; i++) {
            int n = (regs[1] + hdma_off[mode][i]) & 0xFF;
            if (n == 0x30 || n == 0x31) return 1;
        }
    }
    uint8_t cgwsel = in->io[0x130], cgadsub = in->io[0x131];
    int on = ((cgwsel >> 4) & 3) != 3 && (cgadsub & 0x3F);
    for (int i = 0; i < in->nlog; i++) {
        if (in->log[i].reg == 0x2130) cgwsel = in->log[i].value;
        else if (in->log[i].reg == 0x2131) cgadsub = in->log[i].value;
        else continue;
        on |= ((cgwsel >> 4) & 3) != 3 && (cgadsub & 0x3F);
    }
    return on;
}

/* Whether windows can hide a layer anywhere in the frame (F030SMK): TMW
 * and a window select enabling a window for the same layer, among the
 * values the frame starts with and the IRQ writes; an HDMA channel writing
 * the selects or TMW counts as yes. Otherwise channels that only move
 * window edges are not walked. */
static int windows_possible(const PpuInput *in)
{
    uint8_t en = in->io[0x420C - 0x2000];
    for (int ch = 0; ch < 8; ch++) {
        if (!((en >> ch) & 1)) continue;
        const uint8_t *regs = in->io + (0x4300 - 0x2000) + ch * 16;
        int mode = regs[0] & 7;
        for (int i = 0; i < hdma_len[mode]; i++) {
            int n = (regs[1] + hdma_off[mode][i]) & 0xFF;
            if ((n >= 0x23 && n <= 0x25) || n == 0x2E) return 1;
        }
    }
    uint8_t tmw = in->io[0x12E], w12 = in->io[0x123], w34 = in->io[0x124], wobj = in->io[0x125];
    for (int i = 0; i < in->nlog; i++) {
        uint16_t r = in->log[i].reg;
        uint8_t v = in->log[i].value;
        if (r == 0x212E) tmw |= v;
        else if (r == 0x2123) w12 |= v;
        else if (r == 0x2124) w34 |= v;
        else if (r == 0x2125) wobj |= v;
    }
    int layers = ((w12 & 0x0A) ? 1 : 0) | ((w12 & 0xA0) ? 2 : 0) | ((w34 & 0x0A) ? 4 : 0) |
                 ((w34 & 0xA0) ? 8 : 0) | ((wobj & 0x0A) ? 16 : 0);
    return (tmw & layers) != 0;
}

static void hdma_init(const PpuInput *in, Hdma *h)
{
    uint8_t en = in->io[0x420C - 0x2000];
    int skip_ok = 1;
    for (int ch = 0; ch < 8; ch++) {
        const uint8_t *regs = in->io + (0x4300 - 0x2000) + ch * 16;
        h[ch].active = (en >> ch) & 1;
        if (!h[ch].active)
            continue;
        int used = 0, mode = regs[0] & 7;
        for (int i = 0; i < hdma_len[mode]; i++) used |= reg_used((regs[1] + hdma_off[mode][i]) & 0xFF);
        if (!used) {
            h[ch].active = 0;
            continue;
        }
        h[ch].n = hdma_len[mode];
        h[ch].ind = (regs[0] & 0x40) != 0;
        h[ch].breg = regs[1];
        h[ch].off = hdma_off[mode];
        h[ch].m7i = (mode == 2 && regs[1] >= 0x1B && regs[1] <= 0x20) ? (int8_t)(regs[1] - 0x1B) : -1;
        h[ch].table = ((uint32_t)regs[4] << 16) | ((uint32_t)regs[3] << 8) | regs[2];
        h[ch].tlen = 0;
        h[ch].ilen = 0;
        h[ch].have = 0;
        h[ch].next = 0;
        int idem = 1, twice = 0;
        for (int i = 0; i < h[ch].n; i++) {
            int r = (regs[1] + h[ch].off[i]) & 0xFF;
            if (reg_port(r)) idem = 0;
            if (reg_twice(r)) {
                twice = 1;
                if (i + 1 < h[ch].n && ((regs[1] + h[ch].off[i + 1]) & 0xFF) == r) i++;
                else idem = 0;
            }
        }
        h[ch].idem = (uint8_t)idem;
        if (twice && !idem) skip_ok = 0;
        hdma_line_counter(in, &h[ch], regs);
    }
    hdma_skip_ok = skip_ok;
}

/* The channels whose next event is this line: a transfer, then the lines
 * it holds. A repeat entry transfers on every line of its count; any other
 * transfers once and holds for its count, so it is next due at the line
 * after (its table is read ahead, which changes nothing a row sees).
 * Returns the next line any channel is due, or PPU_H + 1. */
static int hdma_events(const PpuInput *in, Hdma *h, Regs *s, int line, int lim)
{
    int due = PPU_H + 1;
    for (int ch = 0; ch < 8; ch++) {
        Hdma *c = &h[ch];
        int skip = 0;
        if (!c->active)
            continue;
        if (c->next != line) {
            if (c->next < due) due = c->next;
            continue;
        }
        const uint8_t *regs = in->io + (0x4300 - 0x2000) + ch * 16;
        if (c->dotransfer) {
            int n = c->n;
            uint32_t *a = c->ind ? &c->indirect : &c->table;
            const uint8_t **p = c->ind ? &c->ip : &c->tp;
            uint32_t *len = c->ind ? &c->ilen : &c->tlen;
            if (*len > (uint32_t)n && ((*a & 0xFFFF) + (uint32_t)n) <= 0xFFFF) {
                /* the unit lies in one host region and one bank: straight
                 * from the pointer */
                const uint8_t *q = *p;
                *p = q + n;
                *len -= (uint32_t)n;
                *a += (uint32_t)n;
                int same = c->have && hdma_skip_ok;
                for (int i = 0; same && i < n; i++) same = q[i] == c->last[i];
                if (same && c->idem) {
                    /* the unit it wrote last line: nothing changes, and nothing for as many lines as the entry goes
                     * on repeating it, up to the next IRQ write (which makes the next transfer a real one) */
                    if (c->repeat && c->count > 1) {
                        int maxk = c->count - 1;
                        if (maxk > (int)(*len / (uint32_t)n)) maxk = (int)(*len / (uint32_t)n);
                        if (maxk > lim - line - 1) maxk = lim - line - 1;
                        const uint8_t *u = *p;
                        int k = 0;
                        while (k < maxk) {
                            int eq = 1;
                            for (int i = 0; i < n; i++) eq &= u[i] == c->last[i];
                            if (!eq) break;
                            u += n;
                            k++;
                        }
                        if (k > 0) {
                            *p = u;
                            *len -= (uint32_t)k * (uint32_t)n;
                            *a += (uint32_t)k * (uint32_t)n;
                            c->count = (uint8_t)(c->count - k);
                            skip = k;
                        }
                    }
                } else if (c->m7i >= 0) {
                    /* mode 2 into a Mode 7 matrix register: both writes
                     * at once, the state two reg_write calls leave */
                    int16_t nv = (int16_t)(q[1] << 8 | q[0]);
                    if (s->r[c->breg] != q[1] || s->m7[c->m7i] != nv) s->changed = 1;
                    s->r[c->breg] = q[1];
                    s->m7[c->m7i] = nv;
                    s->m7old = q[1];
                } else {
                    for (int i = 0; i < n; i++)
                        reg_write(s, (uint16_t)(0x2100 + ((c->breg + c->off[i]) & 0xFF)), q[i]);
                }
                for (int i = 0; i < n; i++) c->last[i] = q[i];
                c->have = 1;
            } else {
                for (int i = 0; i < n; i++) {
                    uint8_t v = hdma_read(in, a, p, len);
                    reg_write(s, (uint16_t)(0x2100 + ((c->breg + c->off[i]) & 0xFF)), v);
                }
                c->have = 0;
            }
        }
        if (c->repeat) {
            c->count--;
            if (c->count == 0)
                hdma_line_counter(in, c, regs);
            c->next = line + 1 + skip;
        } else {
            c->next = line + c->count;
            c->dotransfer = 0;
            hdma_line_counter(in, c, regs);
        }
        if (c->active && c->next < due) due = c->next;
    }
    return due;
}

/* ---- background layers ----------------------------------------------------- */

/* Pixel of one layer: CGRAM index in bits 0-7, priority in bit 8,
 * 0 = transparent. */
typedef uint16_t Pix;

static inline uint16_t vram_word(const uint8_t *vram, uint32_t word)
{
    word &= 0x7FFF;
    return (uint16_t)(vram[word * 2] | (vram[word * 2 + 1] << 8));
}

/* Offset-per-tile (BG modes 2, 4 and 6), as snes9x 1.63 draws it
 * (gfx.cpp, DrawBackgroundOffset): BG3's map, scrolled by BG3's offsets,
 * holds a word per tile column; bit 13 applies it to BG1, bit 14 to BG2.
 * Modes 2 and 6 take a horizontal word from BG3's scroll row and a
 * vertical one from the row voffoff = 8 lines below; mode 4 takes one word,
 * vertical when bit 15 is set. A horizontal word replaces the scroll's
 * coarse part (the fine part stays the register's), a vertical word the
 * whole vertical scroll. The leftmost tile column is never offset. */
static uint32_t opt_word(const PpuInput *in, const Regs *s, int left, int voffoff, uint16_t *v)
{
    uint8_t sc3 = s->r[0x09];
    uint32_t b0 = (uint32_t)(sc3 & 0xFC) << 8;
    uint32_t b1 = (sc3 & 1) ? b0 + 1024 : b0;
    uint32_t b2 = (sc3 & 2) ? b1 + 1024 : b0;
    uint32_t b3 = (sc3 & 1) ? b2 + 1024 : b2;
    int big3 = (s->r[0x05] >> 6) & 1;          /* BG3's tile size */
    int mask2 = big3 ? 0x3FF : 0x1FF, shift2 = big3 ? 4 : 3;
    int hoff = s->bgofs[4] & 0x3FF, voff = s->bgofs[5] & 0x3FF;
    int hrow = voff >> shift2, vrow = (voff + voffoff) >> shift2;
    uint32_t s1 = ((hrow & 0x20) ? b2 : b0) + (uint32_t)(hrow & 0x1F) * 32;
    uint32_t s2 = ((hrow & 0x20) ? b3 : b1) + (uint32_t)(hrow & 0x1F) * 32;
    uint32_t sv = ((vrow & 0x20) ? b2 : b0) + (uint32_t)(vrow & 0x1F) * 32;
    int col = ((hoff + left - 1) & mask2) >> 3;
    uint32_t e;
    if (!big3) e = col > 31 ? s2 + (uint32_t)(col & 31) : s1 + (uint32_t)col;
    else e = col > 63 ? s2 + (uint32_t)((col >> 1) & 31) : s1 + (uint32_t)(col >> 1);
    uint16_t h = vram_word(in->vram, e);
    if (voffoff) {
        *v = vram_word(in->vram, e + sv - s1);
    } else if (h & 0x8000) {
        *v = h;
        h = 0;
    } else {
        *v = 0;
    }
    return h;
}

void ppu_opt_slivers(const PpuInput *in, const PpuRegs *s, int bg, uint32_t out[33])
{
    int mode = s->r[0x05] & 7, voffoff = mode == 4 ? 0 : 8;
    int fine = s->bgofs[bg * 2] & 7;
    uint16_t en = (uint16_t)(0x2000 << bg);
    out[0] = 0;
    for (int k = 1; k < 33; k++) {
        uint16_t vw, hw = (uint16_t)opt_word(in, s, 8 * k - fine, voffoff, &vw);
        uint32_t w = 0;
        if (hw & en) w |= 1u << 23 | (hw & 0x3FFu);
        if (vw & en) w |= 1u << 22 | (uint32_t)(vw & 0x3FF) << 10;
        out[k] = w;
    }
}

static void bg_line(const PpuInput *in, const Regs *s, int bg, int bpp, int palbase,
                    int row, Pix *out, int opt, int voffoff)
{
    uint8_t sc = s->r[0x07 + bg];
    uint32_t mapbase = (uint32_t)(sc & 0xFC) << 8;
    int wide = sc & 1, tall = (sc >> 1) & 1;
    uint8_t nba = s->r[0x0B + (bg >> 1)];
    uint32_t chrbase = (uint32_t)((bg & 1) ? (nba >> 4) : (nba & 15)) << 12;
    int big = (s->r[0x05] >> (4 + bg)) & 1;
    int tile = big ? 16 : 8;
    int hofs = s->bgofs[bg * 2] & 0x3FF, vofs = s->bgofs[bg * 2 + 1] & 0x3FF;
    int y = (row + 1 + vofs);
    int mapw = (wide ? 64 : 32) * tile, maph = (tall ? 64 : 32) * tile;
    y &= maph - 1;
    int words = bpp * 4;                /* words per 8x8 character */
    int y0 = y;
    uint16_t en = (uint16_t)(0x2000 << bg);
    for (int x = 0; x < PPU_W; x++) {
        int hx = hofs;
        y = y0;
        if (opt && x >= 8 - (hofs & 7)) {
            uint16_t vw, hw = (uint16_t)opt_word(in, s, x - ((x + hofs) & 7), voffoff, &vw);
            if (vw & en) y = (row + 1 + vw) & (maph - 1);
            if (hw & en) hx = (hw & ~7) | (hofs & 7);
        }
        int px = (x + hx) & (mapw - 1);
        int tx = px / tile, ty = y / tile;
        uint32_t scr = mapbase;
        if (tx >= 32) { scr += 0x400; tx -= 32; }
        if (ty >= 32) { scr += wide ? 0x800 : 0x400; ty -= 32; }
        uint16_t e = vram_word(in->vram, scr + ty * 32 + tx);
        int fx = px & (tile - 1), fy = y & (tile - 1);
        if (e & 0x4000) fx = tile - 1 - fx;
        if (e & 0x8000) fy = tile - 1 - fy;
        uint32_t chr = e & 0x3FF;
        if (big) {
            if (fx >= 8) chr += 1;
            if (fy >= 8) chr += 16;
        }
        fx &= 7; fy &= 7;
        uint32_t addr = chrbase + (chr & 0x3FF) * words;
        int bit = 7 - fx, c = 0;
        for (int p = 0; p < bpp; p += 2) {
            uint16_t w = vram_word(in->vram, addr + (uint32_t)(p / 2) * 8 + fy);
            c |= ((w >> bit) & 1) << p;
            c |= ((w >> (bit + 8)) & 1) << (p + 1);
        }
        if (c == 0) {
            out[x] = 0;
            continue;
        }
        int pal = (e >> 10) & 7;
        int idx = bpp == 8 ? c : palbase + pal * (1 << bpp) + c;
        out[x] = (Pix)((idx & 0xFF) | ((e & 0x2000) ? 0x100 : 0) | 0x200);
    }
}

static int clip13(int v)
{
    return (v & 0x2000) ? (v | ~0x3FF) : (v & 0x3FF);
}

static int sext13(int v)
{
    v &= 0x1FFF;
    return (v & 0x1000) ? v - 0x2000 : v;
}

/* Frame planner side: the line's start coordinates and steps. */
static void mode7_setup(const Regs *s, int row, PpuM7Line *l)
{
    int sel = s->r[0x1A];
    int a = s->m7[0], b = s->m7[1], c = s->m7[2], d = s->m7[3];
    int x0 = sext13(s->m7[4]), y0 = sext13(s->m7[5]);
    int hofs = sext13(s->m7ofs[0]), vofs = sext13(s->m7ofs[1]);
    int y = row + 1;
    if (sel & 2) y = 255 - y;
    int vx = ((a * clip13(hofs - x0)) & ~63) + ((b * clip13(vofs - y0)) & ~63) +
             ((b * y) & ~63) + (x0 << 8);
    int vy = ((c * clip13(hofs - x0)) & ~63) + ((d * clip13(vofs - y0)) & ~63) +
             ((d * y) & ~63) + (y0 << 8);
    if (sel & 1) {                      /* x flip: start at x = 255, step back */
        vx += a * 255;
        vy += c * 255;
        a = -a;
        c = -c;
    }
    l->vx = vx;
    l->vy = vy;
    l->a = a;
    l->c = c;
    l->sel = (uint8_t)sel;
}

/* One Mode 7 texel: the CGRAM index at texel coordinates (vx, vy) >> 8. */
static inline uint8_t m7_texel(const uint8_t *vram, uint8_t sel, int32_t vx, int32_t vy)
{
    int tx = vx >> 8, ty = vy >> 8;
    uint8_t tile;
    if (((tx | ty) & ~1023) && (sel & 0x80)) {
        if (!(sel & 0x40)) return 0;
        tile = 0;
    } else {
        tile = vram[(((ty & 1023) >> 3) * 128 + ((tx & 1023) >> 3)) * 2];
    }
    return vram[(tile * 64 + (ty & 7) * 8 + (tx & 7)) * 2 + 1];
}

/* Pixel stage: the reference for the DSP kernel. */
void ppu_m7_pixels(const uint8_t *vram, const PpuM7Line *l, uint8_t *cgidx)
{
    int32_t vx = l->vx, vy = l->vy;
    for (int x = 0; x < PPU_W; x++, vx += l->a, vy += l->c)
        cgidx[x] = m7_texel(vram, l->sel, vx, vy);
}

static void mode7_line(const PpuInput *in, const Regs *s, int row, Pix *out)
{
    PpuM7Line l;
    uint8_t idx[PPU_W];
    mode7_setup(s, row, &l);
    ppu_m7_pixels(in->vram, &l, idx);
    for (int x = 0; x < PPU_W; x++)
        out[x] = idx[x] ? (Pix)(idx[x] | 0x200) : 0;
}

/* ---- sprites ----------------------------------------------------------------- */

static const uint8_t obj_sizes[8][2][2] = {   /* [obsel size][small/large] = w, h */
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

/* out: CGRAM index in bits 0-7, priority 0-3 in bits 8-9, bit 10 = opaque. */
static void obj_line(const PpuInput *in, const Regs *s, int row, uint16_t *out)
{
    for (int x = 0; x < PPU_W; x++)
        out[x] = 0;
    uint8_t obsel = s->r[0x01];
    uint32_t base = (uint32_t)(obsel & 7) << 13;
    uint32_t gap = (uint32_t)(((obsel >> 3) & 3) + 1) << 12;
    const uint8_t (*sz)[2] = obj_sizes[obsel >> 5];
    int list[32], n = 0;
    for (int i = 0; i < 128 && n < 32; i++) {
        const uint8_t *e = s->oam + i * 4;
        uint8_t hi = (s->oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int w = sz[hi >> 1][0], h = sz[hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        int dy = (row - e[1]) & 0xFF;
        if (dy >= h) continue;
        if (x <= -w || x >= 256) continue;
        list[n++] = i;
    }
    /* Slivers (8-pixel tile rows) are fetched from the last sprite in range
     * backwards; beyond 34 the rest are dropped. */
    int slivers = 0;
    for (int k = n - 1; k >= 0; k--) {
        int i = list[k];
        const uint8_t *e = s->oam + i * 4;
        uint8_t hi = (s->oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int w = sz[hi >> 1][0], h = sz[hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        int dy = (row - e[1]) & 0xFF;
        uint8_t attr = e[3];
        if (attr & 0x80) dy = h - 1 - dy;
        int tcol0 = e[2] & 15, trow0 = e[2] >> 4;
        int table = attr & 1;
        int pal = 128 + ((attr >> 1) & 7) * 16;
        int pri = (attr >> 4) & 3;
        for (int col = 0; col < w / 8; col++) {
            int sx = x + col * 8;
            if (sx <= -8 || sx >= 256) continue;
            if (++slivers > 34) goto done;
            int c = (attr & 0x40) ? (w / 8 - 1 - col) : col;
            int chr = (((trow0 + (dy >> 3)) & 15) << 4) | ((tcol0 + c) & 15);
            uint32_t addr = base + (table ? gap : 0) + (uint32_t)chr * 16;
            uint16_t p01 = vram_word(in->vram, addr + (dy & 7));
            uint16_t p23 = vram_word(in->vram, addr + 8 + (dy & 7));
            for (int px = 0; px < 8; px++) {
                int X = sx + px;
                if (X < 0 || X >= 256) continue;
                int bit = (attr & 0x40) ? px : 7 - px;
                int v = ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1) |
                        (((p23 >> bit) & 1) << 2) | (((p23 >> (bit + 8)) & 1) << 3);
                if (v)
                    out[X] = (uint16_t)((pal + v) | (pri << 8) | 0x400);
            }
        }
    }
done:
    return;
}

/* ---- composition --------------------------------------------------------------- */

/* ---- windows ------------------------------------------------------------- */

/* Where layer (0-3 BG1-4, 4 OBJ) is hidden on the main screen (F030SMK):
 * TMW masks it inside its window area. Window 1 covers WH0 <= x <= WH1,
 * window 2 WH2 <= x <= WH3 (empty when left > right); W12SEL/W34SEL/
 * WOBJSEL enable and invert them per layer, WBGLOG/WOBJLOG combine two
 * (OR, AND, XOR, XNOR). Colour windows and colour math are not modelled.
 * Returns the number of spans (at most 3: two windows cut the line into
 * five pieces), each [x, x + len) with len > 0, ascending. */
/* The same for a mask register's layers (TMW or TSW); layer 5 is the
 * colour window (WOBJSEL bits 7-4, WOBJLOG bits 3-2), on whatever on is. */
static int win_spans(const PpuRegs *s, int layer, uint8_t on, uint16_t span[3][2])
{
    if (layer < 5 && !(on & (1 << layer)))
        return 0;
    uint8_t sel = layer == 5 ? (uint8_t)(s->r[0x25] >> 4)
                : layer == 4 ? s->r[0x25] : (uint8_t)(s->r[0x23 + (layer >> 1)] >> ((layer & 1) * 4));
    int en1 = sel & 2, en2 = sel & 8;
    if (!en1 && !en2)
        return 0;
    int logic = layer == 5 ? (s->r[0x2B] >> 2) & 3
              : layer == 4 ? s->r[0x2B] & 3 : (s->r[0x2A] >> (layer * 2)) & 3;
    int l1 = s->r[0x26], r1 = s->r[0x27], l2 = s->r[0x28], r2 = s->r[0x29];
    /* the points where either window's membership changes */
    int cut[6] = {0, 256, 256, 256, 256, 256}, nc = 1;
    int pts[4] = {l1, r1 + 1, l2, r2 + 1};
    for (int i = 0; i < 4; i++) {
        int v = pts[i];
        if (v <= 0 || v >= 256) continue;
        int k = nc;
        while (k > 0 && cut[k - 1] > v) { cut[k] = cut[k - 1]; k--; }
        if (k > 0 && cut[k - 1] == v) { for (int j = k; j < nc; j++) cut[j] = cut[j + 1]; continue; }
        cut[k] = v;
        nc++;
    }
    cut[nc] = 256;
    int n = 0;
    for (int i = 0; i < nc; i++) {
        int x = cut[i];
        int a = (l1 <= x && x <= r1) ^ (sel & 1), b = (l2 <= x && x <= r2) ^ ((sel >> 2) & 1);
        int in;
        if (!en2) in = a;
        else if (!en1) in = b;
        else if (logic == 0) in = a | b;
        else if (logic == 1) in = a & b;
        else if (logic == 2) in = a ^ b;
        else in = !(a ^ b);
        if (!in) continue;
        if (n && span[n - 1][0] + span[n - 1][1] == x)
            span[n - 1][1] = (uint16_t)(span[n - 1][1] + cut[i + 1] - x);
        else {
            span[n][0] = (uint16_t)x;
            span[n][1] = (uint16_t)(cut[i + 1] - x);
            n++;
        }
    }
    return n;
}

int ppu_win_spans(const PpuRegs *s, int layer, uint16_t span[3][2])
{
    return win_spans(s, layer, s->r[0x2E], span);
}

/* Bit layer set where ppu_win_spans has spans for it (and TM shows it). */
int ppu_win_spans_on(const PpuRegs *s, int layer, uint8_t on, uint16_t span[3][2])
{
    return win_spans(s, layer, on, span);
}

int ppu_win_layers(const PpuRegs *s)
{
    if (!(s->r[0x2E] & s->r[0x2C] & 0x1F))
        return 0;
    int m = 0;
    uint16_t sp[3][2];
    for (int i = 0; i < 5; i++)
        if ((s->r[0x2C] & (1 << i)) && ppu_win_spans(s, i, sp)) m |= 1 << i;
    return m;
}

/* ---- composition ------------------------------------------------------- */

/* A Mode 7 row for the external pixel stage: only sprites are composed
 * here. Sprites of priority 1-3 cover BG1; priority 0 shows only where BG1
 * is transparent, which takes that one texel. */
static void compose_m7_ext(const PpuInput *in, const Regs *s, int row, uint8_t *out)
{
    static uint16_t obj[PPU_W];
    PpuM7Line l;
    PROF_START();
    mode7_setup(s, row, &l);
    in->m7sink->m7_row(in->m7sink->ctx, row, &l);
    PROF_MARK(PPU_PROF_M7FEED);
    if (!(s->r[0x2C] & 0x10) && !ppu_win_layers(s)) {
        /* No sprites: the row is the kernel's, and only every 8th pixel is
         * sampled (the loop below per pixel cost the intro ~0.4 s a frame
         * on the 68030). */
        for (int x = 0; x < PPU_W; x++) out[x] = PPU_EXT;
        for (int x = 0; x < PPU_W; x += 8)
            in->hist[m7_texel(in->vram, l.sel, l.vx + l.a * x, l.vy + l.c * x)] += 8;
        PROF_MARK(PPU_PROF_COMPOSE);
        return;
    }
    if (s->r[0x2C] & 0x10) obj_line(in, s, row, obj);
    else for (int x = 0; x < PPU_W; x++) obj[x] = 0;
    PROF_MARK(PPU_PROF_OBJ);
    static uint8_t hide[PPU_W];                 /* windows: BG1 hidden */
    for (int x = 0; x < PPU_W; x++) hide[x] = 0;
    for (int i = 0, wl = ppu_win_layers(s); wl; i++, wl >>= 1) {
        if (!(wl & 1)) continue;
        uint16_t sp[3][2];
        for (int k = 0, n = ppu_win_spans(s, i, sp); k < n; k++)
            for (int x = sp[k][0]; x < sp[k][0] + sp[k][1]; x++) {
                if (i == 4) obj[x] = 0;
                else if (i == 0) hide[x] = 1;
            }
    }
    for (int x = 0; x < PPU_W; x++) {
        uint16_t o = obj[x];
        int idx = -1;
        if (hide[x]) idx = (o & 0x400) ? o & 0xFF : 0;
        else if (o & 0x400) {
            if (o & 0x300) idx = o & 0xFF;
            else if (!m7_texel(in->vram, l.sel, l.vx + l.a * x, l.vy + l.c * x)) idx = o & 0xFF;
        }
        out[x] = idx < 0 ? PPU_EXT : in->map[idx];
        if (!(x & 7)) {
            if (idx < 0) idx = m7_texel(in->vram, l.sel, l.vx + l.a * x, l.vy + l.c * x);
            in->hist[idx] += 8;
        }
    }
    PROF_MARK(PPU_PROF_COMPOSE);
}

uint32_t ppu_sub_cfg(const PpuRegs *s, const uint8_t *cgram)
{
    uint8_t cgwsel = s->r[0x30], cgadsub = s->r[0x31], ts = s->r[0x2D] & 0x1F;
    if ((s->r[0x00] & 0x80) || (s->r[0x05] & 7) == 7) return 0;
    if (!(cgwsel & 2) || !ts || (cgwsel & 0xF0) || (cgadsub & 0x80) || !(cgadsub & 0x3F)) return 0;
    if (!(cgadsub & s->r[0x2C] & 0x1F) && !(cgadsub & 0x20)) return 0;   /* no shown layer takes part */
    uint32_t v = ts | (uint32_t)(cgadsub & 0x3F) << 8 | 0x10000;
    if ((cgadsub & 0x20) && !(cgadsub & 0x40) && !cgram[0] && !(cgram[1] & 0x7F)) v |= 0x20000;
    if (!(cgadsub & 0x40)) v |= 0x40000;
    return v;
}

/* The BG layers of a row in layers (TM | TS bits), by mode: bits per pixel
 * of BG1-4 (0: not shown in that mode) and offset-per-tile, as snes9x 1.63
 * sets them up (gfx.cpp, RenderScreen). Modes 5 and 6 are hi-res on the
 * SNES; here they are drawn at 256 pixels, with their layer formats. */
static const uint8_t mode_bpp[7][4] = {
    {2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0}, {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0},
};

static void bg_layers(const PpuInput *in, const Regs *s, int row, uint8_t layers, Pix bg[4][PPU_W])
{
    int mode = s->r[0x05] & 7;
    int opt = mode == 2 || mode == 4 || mode == 6, voffoff = mode == 4 ? 0 : 8;
    for (int i = 0; i < 4; i++) {
        int bpp = mode_bpp[mode][i];
        if (bpp && (layers & (1 << i)))
            bg_line(in, s, i, bpp, mode == 0 ? i * 32 : 0, row, bg[i], opt && i < 2, voffoff);
    }
}

static int front(int mode, int bg3top, Pix bg[4][PPU_W], const uint16_t *obj, uint8_t layers,
                 uint8_t hid[5][PPU_W], int x, int *src);

/* A row with the sub screen (ppu_sub_cfg): both screens through front(),
 * then the checkerboard. */
static void compose_sub_row(const PpuInput *in, const Regs *s, int row, uint8_t *out, uint32_t sub)
{
    static Pix bg[4][PPU_W];
    static uint16_t obj[PPU_W];
    static uint8_t hid[5][PPU_W];
    int mode = s->r[0x05] & 7, bg3top = (s->r[0x05] & 8) != 0;
    uint8_t tm = s->r[0x2C] & 0x1F, ts = (uint8_t)(sub & 0x1F), both = tm | ts;
    for (int i = 0; i < 4; i++)
        for (int x = 0; x < PPU_W; x++) bg[i][x] = 0;
    bg_layers(in, s, row, both, bg);
    if (both & 0x10) obj_line(in, s, row, obj);
    else for (int x = 0; x < PPU_W; x++) obj[x] = 0;
    for (int i = 0; i < 5; i++) {
        for (int x = 0; x < PPU_W; x++) hid[i][x] = 0;
        uint16_t sp[3][2];
        for (int k = 0, n = ppu_win_spans(s, i, sp); k < n; k++)
            for (int x = sp[k][0]; x < sp[k][0] + sp[k][1]; x++) hid[i][x] = 1;
    }
    for (int x = 0; x < PPU_W; x++) {
        int ms, ss;
        int mi = front(mode, bg3top, bg, obj, tm, hid, x, &ms);
        int si = front(mode, bg3top, bg, obj, ts, hid, x, &ss);
        int idx = mi;
        if (ss != 5) {
            int en = ms == 5 ? (sub >> 13) & 1 : ms == 4 ? ((sub >> 12) & 1) && mi >= 192 : (sub >> (8 + ms)) & 1;
            if (ms == 5 && (sub & 0x20000)) idx = si;
            else if (en && !((x + row) & 1)) {
                if (!(sub & 0x40000)) idx = si;             /* halved: the plain average */
                else if (in->subreloc && in->subreloc[si >> 4] != 0xFF)
                    idx = in->subreloc[si >> 4] * 16 + (si & 15);
            }
        }
        out[x] = in->map[idx];
        in->hist[idx]++;
    }
}

static void compose_row(const PpuInput *in, const Regs *s, int row, uint8_t *out)
{
    uint8_t inidisp = s->r[0x00];
    int mode = s->r[0x05] & 7;
    uint8_t tm = s->r[0x2C];
    if (!(inidisp & 0x80) && mode == 7 && (tm & 1) && in->m7sink) {
        compose_m7_ext(in, s, row, out);
        return;
    }
    if (in->m7sink) in->m7sink->cpu_row(in->m7sink->ctx, row);
    if (inidisp & 0x80) {
        /* forced blank: the runtime blacks the palette; pixels keep the
         * backdrop's slot */
        for (int x = 0; x < PPU_W; x++) out[x] = in->map[0];
        return;
    }
    uint32_t sub = ppu_sub_cfg(s, in->cgram);
    if (sub) {
        compose_sub_row(in, s, row, out, sub);
        return;
    }
    static Pix bg[4][PPU_W];
    static uint16_t obj[PPU_W];
    int m7sample = mode == 7 && (tm & 1);
    PROF_START();
    for (int i = 0; i < 4; i++)
        for (int x = 0; x < PPU_W; x++) bg[i][x] = 0;
    if (mode == 7) {
        if (tm & 1) mode7_line(in, s, row, bg[0]);
    } else {
        bg_layers(in, s, row, tm, bg);
    }
    PROF_MARK(mode == 7 ? PPU_PROF_M7 : PPU_PROF_BG);
    if (tm & 0x10) obj_line(in, s, row, obj);
    else for (int x = 0; x < PPU_W; x++) obj[x] = 0;
    PROF_MARK(PPU_PROF_OBJ);
    for (int i = 0, wl = ppu_win_layers(s); wl; i++, wl >>= 1) {
        if (!(wl & 1)) continue;
        uint16_t sp[3][2];
        for (int k = 0, n = ppu_win_spans(s, i, sp); k < n; k++)
            for (int x = sp[k][0]; x < sp[k][0] + sp[k][1]; x++) {
                if (i == 4) obj[x] = 0;
                else bg[i][x] = 0;
            }
    }

    int bg3top = (s->r[0x05] & 8) != 0;
    for (int x = 0; x < PPU_W; x++) {
        int idx = 0;                     /* backdrop */
        uint16_t o = obj[x];
        int op = (o & 0x400) ? (o >> 8) & 3 : -1;
#define BGP(i, hi) ((bg[i][x] & 0x200) && (((bg[i][x] >> 8) & 1) == (hi)))
#define OBJ(p) (op == (p))
        if (mode == 7) {
            if (OBJ(3)) idx = o & 0xFF;
            else if (OBJ(2)) idx = o & 0xFF;
            else if (OBJ(1)) idx = o & 0xFF;
            else if (bg[0][x] & 0x200) idx = bg[0][x] & 0xFF;
            else if (OBJ(0)) idx = o & 0xFF;
        } else if (mode == 0) {
            if (OBJ(3)) idx = o & 0xFF;
            else if (BGP(0, 1)) idx = bg[0][x] & 0xFF;
            else if (BGP(1, 1)) idx = bg[1][x] & 0xFF;
            else if (OBJ(2)) idx = o & 0xFF;
            else if (BGP(0, 0)) idx = bg[0][x] & 0xFF;
            else if (BGP(1, 0)) idx = bg[1][x] & 0xFF;
            else if (OBJ(1)) idx = o & 0xFF;
            else if (BGP(2, 1)) idx = bg[2][x] & 0xFF;
            else if (BGP(3, 1)) idx = bg[3][x] & 0xFF;
            else if (OBJ(0)) idx = o & 0xFF;
            else if (BGP(2, 0)) idx = bg[2][x] & 0xFF;
            else if (BGP(3, 0)) idx = bg[3][x] & 0xFF;
        } else if (mode >= 2) {
            /* modes 2-6 (snes9x: OBJ 16/12/8/4, BG1 15/7, BG2 11/3) */
            if (OBJ(3)) idx = o & 0xFF;
            else if (BGP(0, 1)) idx = bg[0][x] & 0xFF;
            else if (OBJ(2)) idx = o & 0xFF;
            else if (BGP(1, 1)) idx = bg[1][x] & 0xFF;
            else if (OBJ(1)) idx = o & 0xFF;
            else if (BGP(0, 0)) idx = bg[0][x] & 0xFF;
            else if (OBJ(0)) idx = o & 0xFF;
            else if (BGP(1, 0)) idx = bg[1][x] & 0xFF;
        } else {
            if (bg3top && BGP(2, 1)) idx = bg[2][x] & 0xFF;
            else if (OBJ(3)) idx = o & 0xFF;
            else if (BGP(0, 1)) idx = bg[0][x] & 0xFF;
            else if (BGP(1, 1)) idx = bg[1][x] & 0xFF;
            else if (OBJ(2)) idx = o & 0xFF;
            else if (BGP(0, 0)) idx = bg[0][x] & 0xFF;
            else if (BGP(1, 0)) idx = bg[1][x] & 0xFF;
            else if (OBJ(1)) idx = o & 0xFF;
            else if (!bg3top && BGP(2, 1)) idx = bg[2][x] & 0xFF;
            else if (OBJ(0)) idx = o & 0xFF;
            else if (BGP(2, 0)) idx = bg[2][x] & 0xFF;
        }
#undef BGP
#undef OBJ
        /* The slot comes from the frame-start CGRAM: colours HDMA rewrites
         * mid-frame keep their index's slot (a known limitation). */
        out[x] = in->map[idx];
        if (!m7sample) in->hist[idx]++;
        else if (!(x & 7)) in->hist[idx] += 8;
    }
    PROF_MARK(PPU_PROF_COMPOSE);
}

typedef uint32_t __attribute__((may_alias)) u32a;

/* n bytes (a multiple of 4), long by long when both sides are aligned:
 * the byte loops cost the 68030 most of the replay's start */
static void copy4(uint8_t *d, const uint8_t *s, int n)
{
    if (((uintptr_t)d | (uintptr_t)s) & 3) {
        for (int i = 0; i < n; i++) d[i] = s[i];
        return;
    }
    for (int i = 0; i < n; i += 4) *(u32a *)(void *)(d + i) = *(const u32a *)(const void *)(s + i);
}

static void replay(const PpuInput *in, PpuRowFn fn, void *ctx, int every)
{
    static Regs s;
    static Hdma h[8];
    copy4(s.r, in->io + 0x100, 0x100);
    for (int i = 0; i < 8; i++) s.bgofs[i] = in->bgofs[i];
    for (int i = 0; i < 6; i++) s.m7[i] = (int16_t)in->m7[i];
    s.m7ofs[0] = in->m7ofs[0];
    s.m7ofs[1] = in->m7ofs[1];
    /* The write-twice latches start clear: not left over from the last
     * replay (the planner and the composer replay the same frame). */
    s.bgold = s.m7old = 0;
    for (int k = 0; k < 3; k++) s.fixed[k] = in->io[0x1F0 + k] & 31;
    copy4(s.cgram, in->cgram, 512);
    s.cgflip = 0;
    s.cgwrites = 0;
    s.cgaddr = (uint16_t)(s.r[0x21] << 1);
    win_on = windows_possible(in);
    math_on = ppu_replay_nocg ? 0 : math_possible(in);
    hdma_init(in, h);
    int logi = 0, segi = 0;
    s.oam = in->oam;
    s.changed = 1;
    int hdma = PPU_H + 1;                /* the next line a channel is due */
    for (int ch = 0; ch < 8; ch++)
        if (h[ch].active) hdma = 0;
    for (int line = 0; line <= PPU_H; line++) {
        if (!every && !s.changed && line > 1) {
            /* nothing to report until the next log write, OAM segment or
             * HDMA line: jump there (a full replay calls fn every line) */
            int next = hdma;
            if (logi < in->nlog && in->log[logi].line < next) next = in->log[logi].line;
            if (segi < in->noamseg && in->oamseg[segi].line < next) next = in->oamseg[segi].line;
            if (next > line) line = next > PPU_H ? PPU_H + 1 : next;
            if (line > PPU_H) break;
        }
        while (logi < in->nlog && in->log[logi].line <= line) {
            reg_write(&s, in->log[logi].reg, in->log[logi].value);
            logi++;
            for (int ch = 0; ch < 8; ch++) h[ch].have = 0;
        }
        while (segi < in->noamseg && in->oamseg[segi].line <= line) {
            s.oam = in->oamseg[segi++].oam;
            s.changed = 1;
        }
        if (line == hdma) {
            PROF_START();
            hdma = hdma_events(in, h, &s, line, logi < in->nlog ? in->log[logi].line : PPU_H + 1);
            PROF_MARK(PPU_PROF_HDMA);
        }
        if (line >= 1 && (every || s.changed || line == 1)) {
            if (ppu_replay_dx) {
                static Regs t;
                t = s;
                for (int b = 0; b < 4; b++) t.bgofs[b * 2] = (uint16_t)((s.bgofs[b * 2] - ppu_replay_dx) & 0x3FF);
                /* the windows' edges move with the picture: [left, right] + dx, cut to the row (an empty one stays empty,
                 * one moved off the row becomes empty) */
                for (int w = 0x26; w <= 0x28; w += 2) {
                    int l = s.r[w], r = s.r[w + 1];
                    if (l <= r) {
                        l += ppu_replay_dx;
                        r += ppu_replay_dx;
                        if (l > 255 || r < 0) l = 1, r = 0;
                        else {
                            if (l < 0) l = 0;
                            if (r > 255) r = 255;
                        }
                        t.r[w] = (uint8_t)l;
                        t.r[w + 1] = (uint8_t)r;
                    }
                }
                fn(ctx, line - 1, &t);
            } else {
                fn(ctx, line - 1, &s);
            }
            s.changed = 0;
        }
    }
}

void ppu_replay(const PpuInput *in, PpuRowFn fn, void *ctx)
{
    replay(in, fn, ctx, 1);
}

void ppu_replay_changes(const PpuInput *in, PpuRowFn fn, void *ctx)
{
    ppu_replay_ram = 0;
    replay(in, fn, ctx, 0);
}

typedef struct {
    const PpuInput *in;
    uint8_t *out;
} RenderCtx;

static void render_row(void *ctx, int row, const PpuRegs *s)
{
    const RenderCtx *c = (const RenderCtx *)ctx;
    compose_row(c->in, s, row, c->out + row * PPU_W);
}

void ppu_render(const PpuInput *in, uint8_t *out)
{
    RenderCtx c = {in, out};
    ppu_replay(in, render_row, &c);
}

void ppu_m7_setup(const PpuRegs *s, int row, PpuM7Line *l)
{
    mode7_setup(s, row, l);
}

void ppu_planar8(const uint8_t *chunky, uint16_t *planar)
{
    for (int i = 0; i < PPU_W * PPU_H; i += 16) {
        uint16_t p[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int k = 0; k < 16; k++) {
            uint8_t c = chunky[i + k];
            uint16_t bit = (uint16_t)(0x8000 >> k);
            for (int b = 0; b < 8; b++)
                if (c & (1 << b)) p[b] |= bit;
        }
        for (int b = 0; b < 8; b++) *planar++ = p[b];
    }
}

void ppu_planar(const uint8_t *chunky, uint16_t *planar)
{
    for (int i = 0; i < PPU_W * PPU_H; i += 16) {
        uint16_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (int k = 0; k < 16; k++) {
            uint8_t c = chunky[i + k];
            uint16_t bit = (uint16_t)(0x8000 >> k);
            if (c & 1) p0 |= bit;
            if (c & 2) p1 |= bit;
            if (c & 4) p2 |= bit;
            if (c & 8) p3 |= bit;
        }
        *planar++ = p0;
        *planar++ = p1;
        *planar++ = p2;
        *planar++ = p3;
    }
}

/* ---- the full colour output (reference only) -------------------------------- */

/* The front pixel of the layers in mask at x: its CGRAM index; *src 0-3
 * BG1-4, 4 OBJ, 5 the backdrop (index 0). hid: per layer, where windows hide
 * it on this screen. The order is compose_row's. */
static int front(int mode, int bg3top, Pix bg[4][PPU_W], const uint16_t *obj, uint8_t layers,
                 uint8_t hid[5][PPU_W], int x, int *src)
{
    uint16_t o = ((layers & 0x10) && !hid[4][x]) ? obj[x] : 0;
    int op = (o & 0x400) ? (o >> 8) & 3 : -1;
#define ON(i) (((layers >> (i)) & 1) && !hid[i][x])
#define BGP(i, hi) (ON(i) && (bg[i][x] & 0x200) && (((bg[i][x] >> 8) & 1) == (hi)))
#define TAKE(i) do { *src = (i); return bg[i][x] & 0xFF; } while (0)
#define TAKEO() do { *src = 4; return o & 0xFF; } while (0)
    if (mode == 7) {
        if (op >= 1) TAKEO();
        if (ON(0) && (bg[0][x] & 0x200)) TAKE(0);
        if (op == 0) TAKEO();
    } else if (mode == 0) {
        if (op == 3) TAKEO();
        if (BGP(0, 1)) TAKE(0);
        if (BGP(1, 1)) TAKE(1);
        if (op == 2) TAKEO();
        if (BGP(0, 0)) TAKE(0);
        if (BGP(1, 0)) TAKE(1);
        if (op == 1) TAKEO();
        if (BGP(2, 1)) TAKE(2);
        if (BGP(3, 1)) TAKE(3);
        if (op == 0) TAKEO();
        if (BGP(2, 0)) TAKE(2);
        if (BGP(3, 0)) TAKE(3);
    } else if (mode >= 2) {
        if (op == 3) TAKEO();
        if (BGP(0, 1)) TAKE(0);
        if (op == 2) TAKEO();
        if (BGP(1, 1)) TAKE(1);
        if (op == 1) TAKEO();
        if (BGP(0, 0)) TAKE(0);
        if (op == 0) TAKEO();
        if (BGP(1, 0)) TAKE(1);
    } else {
        if (bg3top && BGP(2, 1)) TAKE(2);
        if (op == 3) TAKEO();
        if (BGP(0, 1)) TAKE(0);
        if (BGP(1, 1)) TAKE(1);
        if (op == 2) TAKEO();
        if (BGP(0, 0)) TAKE(0);
        if (BGP(1, 0)) TAKE(1);
        if (op == 1) TAKEO();
        if (!bg3top && BGP(2, 1)) TAKE(2);
        if (op == 0) TAKEO();
        if (BGP(2, 0)) TAKE(2);
    }
#undef ON
#undef BGP
#undef TAKE
#undef TAKEO
    *src = 5;
    return 0;
}

typedef struct {
    const PpuInput *in;
    uint32_t *out;
    uint16_t *out565;               /* ppu_render_565: this instead of out */
} RgbCtx;

static void mark_spans(const PpuRegs *s, int layer, uint8_t on, uint8_t *m)
{
    uint16_t sp[3][2];
    for (int k = 0, n = win_spans(s, layer, on, sp); k < n; k++)
        for (int x = sp[k][0]; x < sp[k][0] + sp[k][1]; x++) m[x] = 1;
}

/* One row: main and sub screen, then per pixel the colour window's clip
 * and colour math (CGWSEL 7-6 clip: 1 outside the colour window, 2 inside,
 * 3 always; 5-4 math: 0 always, 1 inside, 2 outside, 3 never; bit 1 the
 * sub screen, else the fixed colour; CGADSUB 7 subtract, 6 half, 5-0 the
 * layers, OBJ only with palettes 4-7). Half is not applied where the pixel
 * was clipped or where the fixed colour stands in for a transparent sub
 * screen. Brightness as the Falcon's palette scales it. */
static void rgb_row(void *ctx, int row, const PpuRegs *s)
{
    const RgbCtx *c = (const RgbCtx *)ctx;
    const PpuInput *in = c->in;
    uint32_t *out = c->out ? c->out + row * PPU_W : 0;
    uint16_t *o565 = c->out565 ? c->out565 + row * PPU_W : 0;
    uint8_t inidisp = s->r[0x00];
    if (inidisp & 0x80) {
        for (int x = 0; x < PPU_W; x++) {
            if (out) out[x] = 0;
            else o565[x] = 0;
        }
        return;
    }
    int mode = s->r[0x05] & 7, bg3top = (s->r[0x05] & 8) != 0;
    uint8_t tm = s->r[0x2C] & 0x1F, ts = s->r[0x2D] & 0x1F, both = tm | ts;
    static Pix bg[4][PPU_W];
    static uint16_t obj[PPU_W];
    static uint8_t hm[5][PPU_W], hs[5][PPU_W], cw[PPU_W];
    for (int i = 0; i < 4; i++)
        for (int x = 0; x < PPU_W; x++) bg[i][x] = 0;
    if (mode == 7) {
        if (both & 1) mode7_line(in, s, row, bg[0]);
    } else {
        bg_layers(in, s, row, both, bg);
    }
    if (both & 0x10) obj_line(in, s, row, obj);
    else for (int x = 0; x < PPU_W; x++) obj[x] = 0;
    for (int i = 0; i < 5; i++)
        for (int x = 0; x < PPU_W; x++) hm[i][x] = hs[i][x] = 0;
    for (int x = 0; x < PPU_W; x++) cw[x] = 0;
    for (int i = 0; i < 5; i++) {
        mark_spans(s, i, s->r[0x2E], hm[i]);
        mark_spans(s, i, s->r[0x2F], hs[i]);
    }
    mark_spans(s, 5, 0, cw);
    uint8_t cgwsel = s->r[0x30], cgadsub = s->r[0x31];
    int clipmode = cgwsel >> 6, mathmode = (cgwsel >> 4) & 3, bright = inidisp & 15;
    for (int x = 0; x < PPU_W; x++) {
        int ms, ss;
        int mi = front(mode, bg3top, bg, obj, tm, hm, x, &ms);
        int si = front(mode, bg3top, bg, obj, ts, hs, x, &ss);
        uint16_t mc = (uint16_t)(s->cgram[mi * 2] | s->cgram[mi * 2 + 1] << 8);
        int v[3] = {mc & 31, (mc >> 5) & 31, (mc >> 10) & 31};
        int inw = cw[x];
        int clip = clipmode == 3 || (clipmode == 2 && inw) || (clipmode == 1 && !inw);
        int mathok = mathmode == 0 || (mathmode == 1 && inw) || (mathmode == 2 && !inw);
        if (clip) v[0] = v[1] = v[2] = 0;
        int en = ms == 5 ? (cgadsub >> 5) & 1 : ms == 4 ? ((cgadsub >> 4) & 1) && mi >= 192 : (cgadsub >> ms) & 1;
        if (mathok && en) {
            int w[3], half = (cgadsub >> 6) & 1;
            if ((cgwsel & 2) && ss != 5) {
                uint16_t sc = (uint16_t)(s->cgram[si * 2] | s->cgram[si * 2 + 1] << 8);
                w[0] = sc & 31; w[1] = (sc >> 5) & 31; w[2] = (sc >> 10) & 31;
            } else {
                for (int k = 0; k < 3; k++) w[k] = s->fixed[k];
                if (cgwsel & 2) half = 0;       /* the fixed colour for a transparent sub screen */
            }
            if (clip) half = 0;
            for (int k = 0; k < 3; k++) {
                int r = (cgadsub & 0x80) ? v[k] - w[k] : v[k] + w[k];
                if (r < 0) r = 0;
                if (half) r >>= 1;
                if (r > 31) r = 31;
                v[k] = r;
            }
        }
        if (o565) {
            /* the Videl's RGB565: brightness on the 5-bit components,
             * green widened to 6 bits */
            int r = v[0] * (bright + 1) >> 4, g = v[1] * (bright + 1) >> 4, b = v[2] * (bright + 1) >> 4;
            o565[x] = (uint16_t)(r << 11 | (g << 1 | g >> 4) << 5 | b);
            continue;
        }
        uint32_t rgb = 0;
        for (int k = 0; k < 3; k++)
            rgb = rgb << 8 | (uint32_t)(v[k] * 255 / 31 * (bright + 1) / 16);
        out[x] = rgb;
    }
}

void ppu_render_rgb(const PpuInput *in, uint32_t *out)
{
    int walk = ppu_walk_all;
    ppu_walk_all = 1;                           /* windows and colour math registers too */
    RgbCtx c = {in, out, 0};
    ppu_replay(in, rgb_row, &c);
    ppu_walk_all = walk;
}

void ppu_render_565(const PpuInput *in, uint16_t *out)
{
    int walk = ppu_walk_all;
    ppu_walk_all = 1;
    RgbCtx c = {in, 0, out};
    ppu_replay(in, rgb_row, &c);
    ppu_walk_all = walk;
}

/* The frame loop with another row composer, and the reference's own pieces: what src/ppu_fast.c (the
 * fast row composer, kept as the CPU fallback) builds on. */
#include "ppu_internal.h"

typedef struct {
    const PpuInput *in;
    uint8_t *out;
    PpuComposeFn fn;
} RowsCtx;

static void rows_cb(void *ctx, int row, const PpuRegs *s)
{
    const RowsCtx *c = (const RowsCtx *)ctx;
    c->fn(c->in, s, row, c->out + row * PPU_W);
}

void ppu_render_rows(const PpuInput *in, uint8_t *out, PpuComposeFn fn)
{
    RowsCtx c = {in, out, fn};
    ppu_replay(in, rows_cb, &c);
}

void ppu_ref_row(const PpuInput *in, const Regs *s, int row, uint8_t *out)
{
    compose_row(in, s, row, out);
}

void ppu_obj_line(const PpuInput *in, const Regs *s, int row, uint16_t *out)
{
    obj_line(in, s, row, out);
}
