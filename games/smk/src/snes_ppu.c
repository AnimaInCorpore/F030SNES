/* SNES picture composition: see snes_ppu.h for scope.
 *
 * Scanlines 1-224 are visible (row = scanline - 1). HDMA runs at the start
 * of every scanline from 0, before that scanline is composed; IRQ-logged
 * writes take effect from their line. Freestanding: no libc.
 */
#include "ppu_internal.h"

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
        if (off >= 0x8000) return bus->rom[(((bank & 0x3F) << 16) | off) & 0x7FFFF];
        if (off >= 0x6000 && (bank & 0x3F) >= 0x20) return bus->sram[off & 0x7FF];
        return 0;                       /* I/O is not an HDMA source */
    }
    return bus->rom[(((bank & 0x3F) << 16) | off) & 0x7FFFF];
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
            return bus->rom + ((((bank & 0x3F) << 16) | off) & 0x7FFFF);
        }
        *len = 0;
        return 0;
    }
    *len = 0x10000 - off;
    return bus->rom + ((((bank & 0x3F) << 16) | off) & 0x7FFFF);
}

/* ---- optional stage profile (Falcon profile builds) ----------------------- */

#ifdef PPU_PROFILE
uint32_t (*ppu_prof_clock)(void);
uint32_t ppu_prof[PPU_PROF_STAGES];
#endif

/* ---- register state for one frame -------------------------------------- */


/* $2100 + n -> the bit of the DSP stream's field it feeds (ppu_dsp.c) */
#define W (1u << 26)            /* the windows: W12SEL-WOBJLOG and TMW */
static const uint32_t reg_field[] = {
    1u << 0, 1u << 31, 0, 0, 0, 1u << 1, 0, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 6,
    1u << 7, 1u << 8 | 1u << 23, 1u << 9 | 1u << 24, 1u << 10, 1u << 11, 1u << 12, 1u << 13,
    1u << 14, 1u << 15, 0, 0, 0, 0, 0, 1u << 16, 1u << 17, 1u << 18, 1u << 19, 1u << 20,
    1u << 21, 1u << 22, 0, 0, W, W, W, W, W, W, W, W, W, 1u << 25, 0, W, 0,
};
#undef W
_Static_assert(sizeof reg_field / sizeof reg_field[0] == 0x30, "reg_field: one entry per register");

static void reg_write(Regs *s, uint16_t reg, uint8_t v)
{
    uint8_t n = (uint8_t)(reg - 0x2100);
    s->r[n] = v;
    if (n < 0x30) s->dirty |= reg_field[n];
    if (n >= 0x0D && n <= 0x14) {
        int i = n - 0x0D;
        uint16_t old = s->bgofs[i];
        if (i & 1)
            s->bgofs[i] = (uint16_t)((v << 8) | s->bgold);
        else
            s->bgofs[i] = (uint16_t)((v << 8) | (s->bgold & 0xF8) | ((old >> 8) & 7));
        s->bgold = v;
        if (n <= 0x0E) {                /* $210D/$210E also feed Mode 7 */
            s->m7ofs[n - 0x0D] = (uint16_t)((v << 8) | s->m7old);
            s->m7old = v;
        }
    } else if (n >= 0x1B && n <= 0x20) {
        s->m7[n - 0x1B] = (int16_t)((v << 8) | s->m7old);
        s->m7old = v;
    } else if (n == 0x21) {
        s->cgaddr = (uint16_t)(v << 1);
        s->cgflip = 0;
    } else if (n == 0x22) {
        if (!s->cgflip) {
            s->cglatch = v;
            s->cgflip = 1;
        } else {
            s->cgram[s->cgaddr & 0x1FE] = s->cglatch;
            s->cgram[(s->cgaddr & 0x1FE) + 1] = v & 0x7F;
            s->cgaddr += 2;
            s->cgflip = 0;
        }
    }
}

/* ---- HDMA ---------------------------------------------------------------- */

static const uint8_t hdma_len[8] = {1, 2, 2, 4, 4, 4, 2, 4};
static const uint8_t hdma_off[8][4] = {
    {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 1, 1},
    {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
};

static uint8_t abus(const PpuInput *in, uint32_t a)
{
    return in->abus(a & 0xFFFFFF, in->ctx);
}

/* The byte at *a, which then advances within its bank; *p and *len follow it
 * as a host pointer while the region lasts. */
static inline uint8_t hdma_read(const PpuInput *in, uint32_t *a, const uint8_t **p, uint32_t *len)
{
    uint8_t v;
    if (!*len && in->aptr)
        *p = in->aptr(*a & 0xFFFFFF, in->ctx, len);
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
static int win_on;              /* this frame's windows can hide a layer */

static int reg_used(int n)
{
    if (n <= 0x14 || (n >= 0x1A && n <= 0x22) || n == 0x2C)
        return 1;
    return win_on && ((n >= 0x23 && n <= 0x2B) || n == 0x2E);
}

/* Whether windows can hide a layer anywhere in the frame: TMW and a
 * window select enabling a window for the same layer, among the values the
 * frame starts with and the IRQ writes; an HDMA channel writing the
 * selects or TMW counts as yes. Otherwise channels that only move window
 * edges (SMK's race) are not walked. */
static int windows_possible(const PpuInput *in)
{
    uint8_t en = in->io[0x2200 + 0xC];         /* HDMAEN ($420C) */
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
        hdma_line_counter(in, &h[ch], regs);
    }
}

static void hdma_line(const PpuInput *in, Hdma *h, Regs *s)
{
    for (int ch = 0; ch < 8; ch++) {
        Hdma *c = &h[ch];
        if (!c->active)
            continue;
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
                if (c->m7i >= 0) {
                    /* mode 2 into a Mode 7 matrix register: both writes
                     * at once, the state two reg_write calls leave */
                    s->r[c->breg] = q[1];
                    s->dirty |= reg_field[c->breg];
                    s->m7[c->m7i] = (int16_t)(q[1] << 8 | q[0]);
                    s->m7old = q[1];
                } else {
                    for (int i = 0; i < n; i++)
                        reg_write(s, (uint16_t)(0x2100 + ((c->breg + c->off[i]) & 0xFF)), q[i]);
                }
            } else {
                for (int i = 0; i < n; i++) {
                    uint8_t v = hdma_read(in, a, p, len);
                    reg_write(s, (uint16_t)(0x2100 + ((c->breg + c->off[i]) & 0xFF)), v);
                }
            }
        }
        c->count--;
        c->dotransfer = c->repeat ? 1 : 0;
        if (c->count == 0)
            hdma_line_counter(in, c, regs);
    }
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

static void bg_line(const PpuInput *in, const Regs *s, int bg, int bpp, int palbase,
                    int row, Pix *out)
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
    for (int x = 0; x < PPU_W; x++) {
        int px = (x + hofs) & (mapw - 1);
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
void ppu_mode7_setup(const Regs *s, int row, PpuM7Line *l)
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

/* Pixel stage: the reference for the DSP kernel. */
void ppu_m7_pixels(const uint8_t *vram, const PpuM7Line *l, uint8_t *cgidx)
{
    int32_t vx = l->vx, vy = l->vy;
    for (int x = 0; x < PPU_W; x++, vx += l->a, vy += l->c)
        cgidx[x] = ppu_m7_texel(vram, l->sel, vx, vy);
}

static void mode7_line(const PpuInput *in, const Regs *s, int row, Pix *out)
{
    PpuM7Line l;
    uint8_t idx[PPU_W];
    ppu_mode7_setup(s, row, &l);
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
void ppu_obj_line(const PpuInput *in, const Regs *s, int row, uint16_t *out)
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

/* ---- windows ------------------------------------------------------------------ */

/* Where layer (0-3 BG1-4, 4 OBJ) is hidden on the main screen: TMW masks
 * it inside its window area. Window 1 covers WH0 <= x <= WH1, window 2
 * WH2 <= x <= WH3 (empty when left > right); W12SEL/W34SEL/WOBJSEL enable
 * and invert them per layer, WBGLOG/WOBJLOG combine two (OR, AND, XOR,
 * XNOR). Colour windows and colour math are not modelled. Returns the
 * number of spans (at most 3: two windows cut the line into five pieces),
 * each [x, x + len) with len > 0, ascending. */
int ppu_win_spans(const Regs *s, int layer, uint16_t span[3][2])
{
    if (!(s->r[0x2E] & (1 << layer)))
        return 0;
    uint8_t sel = layer == 4 ? s->r[0x25] : (uint8_t)(s->r[0x23 + (layer >> 1)] >> ((layer & 1) * 4));
    int en1 = sel & 2, en2 = sel & 8;
    if (!en1 && !en2)
        return 0;
    int logic = layer == 4 ? s->r[0x2B] & 3 : (s->r[0x2A] >> (layer * 2)) & 3;
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

/* Bit layer set where ppu_win_spans has spans for it (and TM shows it). */
int ppu_win_layers(const Regs *s)
{
    if (!(s->r[0x2E] & s->r[0x2C] & 0x1F))
        return 0;
    int m = 0;
    uint16_t sp[3][2];
    for (int i = 0; i < 5; i++)
        if ((s->r[0x2C] & (1 << i)) && ppu_win_spans(s, i, sp)) m |= 1 << i;
    return m;
}

/* ---- composition --------------------------------------------------------------- */

/* A Mode 7 row for the external pixel stage: only sprites are composed
 * here. Sprites of priority 1-3 cover BG1; priority 0 shows only where BG1
 * is transparent, which takes that one texel. */
static void compose_m7_ext(const PpuInput *in, const Regs *s, int row, uint8_t *out)
{
    static uint16_t obj[PPU_W];
    PpuM7Line l;
    PROF_START();
    ppu_mode7_setup(s, row, &l);
    in->m7sink->m7_row(in->m7sink->ctx, row, &l);
    PROF_MARK(PPU_PROF_M7FEED);
    if (s->r[0x2C] & 0x10) ppu_obj_line(in, s, row, obj);
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
            else if (!ppu_m7_texel(in->vram, l.sel, l.vx + l.a * x, l.vy + l.c * x)) idx = o & 0xFF;
        }
        out[x] = idx < 0 ? PPU_EXT : in->map[idx];
        if (!(x & 15)) {
            if (idx < 0) idx = ppu_m7_texel(in->vram, l.sel, l.vx + l.a * x, l.vy + l.c * x);
            in->hist[idx] += 16;
        }
    }
    PROF_MARK(PPU_PROF_COMPOSE);
}

void ppu_ref_row(const PpuInput *in, const Regs *s, int row, uint8_t *out)
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
        in->hist[0] += 256;                     /* 16 samples of the backdrop */
        return;
    }
    static Pix bg[4][PPU_W];
    static uint16_t obj[PPU_W];
    PROF_START();
    for (int i = 0; i < 4; i++)
        for (int x = 0; x < PPU_W; x++) bg[i][x] = 0;
    if (mode == 7) {
        if (tm & 1) mode7_line(in, s, row, bg[0]);
    } else if (mode == 0) {
        for (int i = 0; i < 4; i++)
            if (tm & (1 << i)) bg_line(in, s, i, 2, i * 32, row, bg[i]);
    } else {
        /* modes 1-6 composed as mode 1 for now */
        if (tm & 1) bg_line(in, s, 0, 4, 0, row, bg[0]);
        if (tm & 2) bg_line(in, s, 1, 4, 0, row, bg[1]);
        if (tm & 4) bg_line(in, s, 2, 2, 0, row, bg[2]);
    }
    PROF_MARK(mode == 7 ? PPU_PROF_M7 : PPU_PROF_BG);
    if (tm & 0x10) ppu_obj_line(in, s, row, obj);
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
        if (!(x & 15)) in->hist[idx] += 16;     /* every 16th pixel, as the DSP */
    }
    PROF_MARK(PPU_PROF_COMPOSE);
}

void ppu_frame_start(const PpuInput *in, PpuFrame *f)
{
    Regs *s = &f->s;
    for (int i = 0; i < 0x100; i++) s->r[i] = in->io[0x100 + i];
    for (int i = 0; i < 8; i++) s->bgofs[i] = in->bgofs[i];
    for (int i = 0; i < 6; i++) s->m7[i] = (int16_t)in->m7[i];
    s->m7ofs[0] = in->m7ofs[0];
    s->m7ofs[1] = in->m7ofs[1];
    for (int i = 0; i < 512; i++) s->cgram[i] = in->cgram[i];
    s->cgflip = 0;
    s->cgaddr = (uint16_t)(s->r[0x21] << 1);
    s->bgold = s->m7old = 0;
    s->dirty = 0xFFFFFFFFu;     /* the write-twice latches: every frame alike,
                                   whatever rendered before (PpuInput has no
                                   latch state) */
    win_on = windows_possible(in);
    hdma_init(in, f->h);
    f->logi = f->segi = 0;
    s->oam = in->oam;
}

void ppu_frame_line(const PpuInput *in, PpuFrame *f, int line)
{
    Regs *s = &f->s;
    while (f->logi < in->nlog && in->log[f->logi].line <= line) {
        reg_write(s, in->log[f->logi].reg, in->log[f->logi].value);
        f->logi++;
    }
    while (f->segi < in->noamseg && in->oamseg[f->segi].line <= line)
        s->oam = in->oamseg[f->segi++].oam;
    PROF_START();
    hdma_line(in, f->h, s);
    PROF_MARK(PPU_PROF_HDMA);
}

void ppu_render_rows(const PpuInput *in, uint8_t *out, PpuRowFn rowfn)
{
    static PpuFrame f;
    ppu_frame_start(in, &f);
    for (int line = 0; line <= PPU_H; line++) {
        ppu_frame_line(in, &f, line);
        if (line >= 1) {
            PROF_START();
            rowfn(in, &f.s, line - 1, out ? out + (line - 1) * PPU_W : 0);
            PROF_MARK(PPU_PROF_M7FEED);     /* (the row function, whole) */
        }
    }
}

void ppu_render(const PpuInput *in, uint8_t *out)
{
    ppu_render_rows(in, out, ppu_ref_row);
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
