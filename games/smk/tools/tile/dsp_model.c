/* Host model of the DSP renderer (falcon/dsp/ppu.asm): reads the word
 * stream of ppu_dsp.h and composes eight-plane rows the way the DSP does,
 * in plane words rather than pixels.
 *
 * Per row and layer the tiles become a strip of 8-pixel slivers: a byte
 * per output plane (the tile's planes, then the bits of the palette base
 * as 00/FF) and an opaque mask, split by the tile's priority bit. A
 * 16-pixel group of a layer is three consecutive sliver bytes shifted by
 * the layer's fine scroll. Sprites are laid into plane words over each
 * other (a lower OAM index wins), with two planes for their priority.
 * The groups are then composed front to back: each pass takes the pixels
 * no earlier pass covered. The window build ANDs each layer's pass with
 * its keep mask: per group, the pixels its main-screen windows leave. On
 * rows that add the sub screen (its SUB slot, ppu_sub_cfg) it composes the
 * sub screen's layers the same way and merges: the checkerboard's pixels
 * whose main layer takes part in colour math and whose sub pixel is
 * opaque (on rows that add without halving only those of the relocated
 * CGRAM blocks, their planes 4-7 set to the target block), and every
 * black-backdrop pixel over an opaque sub pixel with bit 17. */
#include "dsp_model.h"

static const uint8_t obj_sizes[8][2][2] = {
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

static const uint8_t *vram;
static uint8_t *touched;

static uint32_t vword(uint32_t a)
{
    a &= 0x7FFF;
    touched[a >> 8] = 1;
    return (uint32_t)(vram[a * 2] | vram[a * 2 + 1] << 8);
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

/* A layer's row: 33 slivers from the 8-pixel boundary at or left of the
 * first pixel. */
typedef struct {
    uint8_t plane[8][34];
    uint8_t mlo[34], mhi[34];
    int fine;
} Strip;

static void strip_build(const uint32_t *slot, int bg, int bpp, int palbase, int row, Strip *st,
                        const uint32_t *opt)
{
    uint32_t sc = slot[DSP_SLOT_SC + bg];
    uint32_t nba = slot[DSP_SLOT_NBA + (bg >> 1)];
    uint32_t mapbase = (sc & 0xFC) << 8;
    int wide = sc & 1, tall = (sc >> 1) & 1;
    uint32_t chrbase = ((bg & 1) ? (nba >> 4) & 15 : nba & 15) << 12;
    int big = (slot[DSP_SLOT_BGMODE] >> (4 + bg)) & 1;
    int tile = big ? 16 : 8;
    int hofs = slot[DSP_SLOT_OFS + bg * 2] & 0x3FF, vofs = slot[DSP_SLOT_OFS + bg * 2 + 1] & 0x3FF;
    int mapw = (wide ? 64 : 32) * tile, maph = (tall ? 64 : 32) * tile;
    int y = (row + 1 + vofs) & (maph - 1);
    int words = bpp * 4;
    int px0 = hofs & (mapw - 1);
    st->fine = px0 & 7;
    int y0 = y;
    for (int k = 0; k < 33; k++) {
        /* offset-per-tile (ppu_opt_slivers): the sliver's own coarse H
         * scroll and V scroll */
        int hc = px0 & ~7;
        y = y0;
        if (opt && (opt[k] & (1u << 23))) hc = (int)(opt[k] & 0x3F8);
        if (opt && (opt[k] & (1u << 22))) y = (row + 1 + (int)((opt[k] >> 10) & 0x3FF)) & (maph - 1);
        int px = (hc + 8 * k) & (mapw - 1);
        int tx = px / tile, ty = y / tile;
        uint32_t scr = mapbase;
        if (tx >= 32) { scr += 0x400; tx -= 32; }
        if (ty >= 32) { scr += wide ? 0x800 : 0x400; ty -= 32; }
        uint32_t e = vword(scr + (uint32_t)(ty * 32 + tx));
        int half = (px >> 3) & 1, fy = y & (tile - 1);
        if (e & 0x4000) half ^= 1;
        if (e & 0x8000) fy = tile - 1 - fy;
        uint32_t chr = e & 0x3FF;
        if (big) {
            if (half) chr += 1;
            if (fy >= 8) chr += 16;
        }
        fy &= 7;
        uint32_t addr = chrbase + (chr & 0x3FF) * (uint32_t)words + (uint32_t)fy;
        uint8_t m = 0;
        for (int p = 0; p < bpp; p += 2) {
            uint32_t w = vword(addr + (uint32_t)(p / 2) * 8);
            uint8_t lo = (uint8_t)w, hi = (uint8_t)(w >> 8);
            if (e & 0x4000) { lo = rev8(lo); hi = rev8(hi); }
            st->plane[p][k] = lo;
            st->plane[p + 1][k] = hi;
            m |= lo | hi;
        }
        int base = palbase + (int)((e >> 10) & 7) * (1 << bpp);
        for (int p = bpp; p < 8; p++) st->plane[p][k] = ((base >> p) & 1) ? 0xFF : 0;
        st->mhi[k] = (e & 0x2000) ? m : 0;
        st->mlo[k] = (e & 0x2000) ? 0 : m;
    }
}

/* A Mode 7 row (the Mode 7 program): the row's segments sampled as the
 * DSP does, from the packed coordinates, into layer 0's strip of 8-pixel
 * slivers: eight plane bytes, the opaque mask low, fine scroll 0. Returns
 * the words the entry took. */
static int m7_strip(const uint32_t *e, Strip *st)
{
    uint8_t px[256];
    int at = 0, x = 0, nseg = (int)e[at++];
    for (int k = 0; k < nseg; k++) {
        uint32_t kc = e[at++];
        int kind = (int)(kc >> 16), count = (int)(kc & 0xFFFF);
        if (kind == 3) {
            for (int i = 0; i < count && x < 256; i++) px[x++] = 0;
            continue;
        }
        /* a1:a0 = v << 24 | (u + $800000); step x1:x0; 48-bit adds */
        uint64_t a = ((uint64_t)(e[at] & 0xFFFFFF) << 24) | (e[at + 1] & 0xFFFFFF);
        uint64_t d = ((uint64_t)(e[at + 2] & 0xFFFFFF) << 24) | (e[at + 3] & 0xFFFFFF);
        at += 4;
        for (int i = 0; i < count && x < 256; i++) {
            uint32_t v = (uint32_t)(a >> 24) & 0xFFFFFF, u = ((uint32_t)a & 0xFFFFFF) - 0x800000;
            uint32_t tv = (v >> 8) & 1023, tu = (u >> 8) & 1023, tile = 0;
            if (kind == 1) tile = vword((tv >> 3) * 128 + (tu >> 3)) & 0xFF;
            px[x++] = (uint8_t)(vword(tile * 64 + (tv & 7) * 8 + (tu & 7)) >> 8);
            a = (a + d) & 0xFFFFFFFFFFFFULL;
        }
    }
    while (x < 256) px[x++] = 0;
    for (int k = 0; k < 34; k++) {
        uint8_t m = 0;
        for (int q = 0; q < 8; q++) {
            uint8_t b = 0;
            if (k < 32)
                for (int i = 0; i < 8; i++) b |= (uint8_t)(((px[k * 8 + i] >> q) & 1) << (7 - i));
            st->plane[q][k] = b;
            m |= b;
        }
        st->mlo[k] = m;
        st->mhi[k] = 0;
    }
    st->fine = 0;
    return at;
}

/* The layer's plane word for group g from a byte strip. */
static uint32_t group(const uint8_t *b, int fine, int g)
{
    uint32_t t = (uint32_t)b[2 * g] << 16 | (uint32_t)b[2 * g + 1] << 8 | b[2 * g + 2];
    return (t >> (8 - fine)) & 0xFFFF;
}

/* Sprite row: 18 groups, group 0 is x = -16..-1. Planes 0-7, mask, and the
 * two priority bits. */
typedef struct {
    uint32_t w[11][18];
} ObjRow;

static void obj_place(ObjRow *o, int sx, const uint8_t *bytes)
{
    int pos = sx + 16, g = pos >> 4, off = pos & 15;
    uint32_t m = (uint32_t)bytes[8] << (24 - off);
    uint32_t mh = m >> 16, ml = m & 0xFFFF;
    for (int q = 0; q < 11; q++) {
        uint32_t v = (uint32_t)bytes[q] << (24 - off);
        o->w[q][g] = (o->w[q][g] & ~mh) | ((v >> 16) & mh);
        o->w[q][g + 1] = (o->w[q][g + 1] & ~ml) | (v & ml);
    }
}

static void obj_row(const uint32_t *slot, const uint32_t *spr, int nspr, int row, ObjRow *o)
{
    for (int q = 0; q < 11; q++)
        for (int g = 0; g < 18; g++) o->w[q][g] = 0;
    uint32_t obsel = slot[DSP_SLOT_OBSEL];
    uint32_t base = (obsel & 7) << 13;
    uint32_t gap = (((obsel >> 3) & 3) + 1) << 12;
    const uint8_t (*sz)[2] = obj_sizes[(obsel >> 5) & 7];
    int list[32], n = 0;
    for (int i = 0; i < nspr && n < 32; i++) {
        uint32_t w0 = spr[i * 2];
        int large = (w0 >> 17) & 1;
        int w = sz[large][0], h = sz[large][1];
        int x = (int)(w0 & 0x1FF);
        if (x >= 256) x -= 512;
        int dy = (row - (int)((w0 >> 9) & 0xFF)) & 0xFF;
        if (dy >= h) continue;
        if (x <= -w || x >= 256) continue;
        list[n++] = i;
    }
    int slivers = 0;
    for (int k = n - 1; k >= 0; k--) {
        uint32_t w0 = spr[list[k] * 2], w1 = spr[list[k] * 2 + 1];
        int large = (w0 >> 17) & 1;
        int w = sz[large][0], h = sz[large][1];
        int x = (int)(w0 & 0x1FF);
        if (x >= 256) x -= 512;
        int dy = (row - (int)((w0 >> 9) & 0xFF)) & 0xFF;
        uint32_t attr = (w1 >> 8) & 0xFF;
        if (attr & 0x80) dy = h - 1 - dy;
        int tcol0 = w1 & 15, trow0 = (w1 >> 4) & 15;
        int pal = 128 + (int)((attr >> 1) & 7) * 16;
        int pri = (attr >> 4) & 3;
        for (int col = 0; col < w / 8; col++) {
            int sx = x + col * 8;
            if (sx <= -8 || sx >= 256) continue;
            if (++slivers > 34) return;
            int c = (attr & 0x40) ? (w / 8 - 1 - col) : col;
            uint32_t chr = (uint32_t)(((trow0 + (dy >> 3)) & 15) << 4 | ((tcol0 + c) & 15));
            uint32_t addr = base + ((attr & 1) ? gap : 0) + chr * 16 + (uint32_t)(dy & 7);
            uint32_t p01 = vword(addr), p23 = vword(addr + 8);
            uint8_t b[11];
            b[0] = (uint8_t)p01; b[1] = (uint8_t)(p01 >> 8);
            b[2] = (uint8_t)p23; b[3] = (uint8_t)(p23 >> 8);
            if (!(attr & 0x40))
                for (int q = 0; q < 4; q++) b[q] = b[q];
            else
                for (int q = 0; q < 4; q++) b[q] = rev8(b[q]);
            for (int q = 4; q < 8; q++) b[q] = ((pal >> q) & 1) ? 0xFF : 0;
            b[8] = b[0] | b[1] | b[2] | b[3];
            b[9] = (pri & 1) ? 0xFF : 0;
            b[10] = (pri & 2) ? 0xFF : 0;
            obj_place(o, sx, b);
        }
    }
}

/* The window build's keep masks per layer (0-3 BG1-4, 4 OBJ) and group,
 * as the DSP builds them when a window slot changes: each window's range
 * as a group mask, inverted, the two combined by the layer's logic,
 * complemented; all kept where TMW does not have the layer. */
static uint32_t range_mask(int l, int r, int g)
{
    int lo = g * 16, hi = lo + 15;
    if (l > r || r < lo || l > hi) return 0;
    int a = (l > lo ? l : lo) - lo, b = (r < hi ? r : hi) - lo;
    return (0xFFFFu >> a) & (0xFFFFu << (15 - b)) & 0xFFFF;
}

static void win_keep(const uint32_t *slot, uint32_t keep[5][16])
{
    uint32_t wsel = slot[DSP_SLOT_WSEL], wlog = slot[DSP_SLOT_WLOG];
    int l1 = slot[DSP_SLOT_WH01] & 0xFF, r1 = (slot[DSP_SLOT_WH01] >> 8) & 0xFF;
    int l2 = slot[DSP_SLOT_WH23] & 0xFF, r2 = (slot[DSP_SLOT_WH23] >> 8) & 0xFF;
    for (int i = 0; i < 5; i++) {
        int sel = (int)(wsel >> (i * 4)) & 15;       /* W12SEL, W34SEL, WOBJSEL nibbles */
        int logic = i == 4 ? (int)(wlog >> 8) & 3 : (int)(wlog >> (i * 2)) & 3;
        int on = (wlog >> 16 >> i) & 1;
        for (int g = 0; g < 16; g++) {
            uint32_t a = range_mask(l1, r1, g) ^ ((sel & 1) ? 0xFFFF : 0);
            uint32_t b = range_mask(l2, r2, g) ^ ((sel & 4) ? 0xFFFF : 0);
            uint32_t h;
            if (!on || !(sel & 0x0A)) h = 0;
            else if (!(sel & 8)) h = a;
            else if (!(sel & 2)) h = b;
            else if (logic == 0) h = a | b;
            else if (logic == 1) h = a & b;
            else if (logic == 2) h = a ^ b;
            else h = ~(a ^ b) & 0xFFFF;
            keep[i][g] = ~h & 0xFFFF;
        }
    }
}

/* Composition passes, front to back. */
enum { P_BG0 = 0, P_OBJ = 8 };          /* BG i priority h: 2i + h; OBJ p: 8 + p */

/* One screen's passes for group g: its planes, the pixels it covers, and
 * (math) the pixels whose layer takes part in colour math (sub: CGADSUB
 * enables << 8 as in the SUB slot; OBJ only palettes 4-7: plane 6). */
static void passes(const uint32_t *slot, Strip *bg, const ObjRow *o, uint32_t keep[5][16], int layers,
                   uint32_t sub, int g, uint32_t acc[8], uint32_t *cover, uint32_t *math);

static void compose(const uint32_t *slot, Strip *bg, const ObjRow *o, uint32_t keep[5][16],
                    uint16_t *out, int row, const uint8_t *reloc, int nrel)
{
    uint32_t sub = slot[DSP_SLOT_SUB];
    if (sub & 0x10000) {
        uint32_t d = (row & 1) ? 0x5555 : 0xAAAA;   /* x + row even */
        for (int g = 0; g < 16; g++) {
            uint32_t ma[8] = {0}, sa[8] = {0}, mc, sc, mm, dummy;
            passes(slot, bg, o, keep, (int)slot[DSP_SLOT_TM], sub, g, ma, &mc, &mm);
            passes(slot, bg, o, keep, (int)(sub & 0x1F), 0, g, sa, &sc, &dummy);
            uint32_t back = ~mc & 0xFFFF;
            if ((sub >> 13) & 1) mm |= back;
            uint32_t ex = (sub & 0x20000) ? back & sc : 0;
            uint32_t md = mm & sc & d & ~ex;
            if (sub & 0x40000) {                /* only the relocated blocks, moved */
                uint32_t inc = 0, r[4] = {0, 0, 0, 0};
                for (int k = 0; k < nrel; k++) {
                    int b = reloc[k * 2], f = reloc[k * 2 + 1];
                    uint32_t mt = 0xFFFF;
                    for (int q = 0; q < 4; q++) mt &= ((b >> q) & 1) ? sa[4 + q] : ~sa[4 + q];
                    inc |= mt;
                    for (int q = 0; q < 4; q++)
                        if ((f >> q) & 1) r[q] |= mt;
                }
                md &= inc;
                for (int q = 0; q < 8; q++) {
                    uint32_t sv = q < 4 ? sa[q] : r[q - 4];
                    out[g * 8 + q] = (uint16_t)((ma[q] & ~(md | ex)) | (sv & md) | (sa[q] & ex));
                }
            } else {
                for (int q = 0; q < 8; q++)
                    out[g * 8 + q] = (uint16_t)((ma[q] & ~(md | ex)) | (sa[q] & (md | ex)));
            }
        }
        return;
    }
    int mode = slot[DSP_SLOT_BGMODE] & 7, tm = slot[DSP_SLOT_TM];
    int bg3top = (slot[DSP_SLOT_BGMODE] & 8) != 0;
    int order[16], n = 0;
#define BGP(i, h) order[n++] = 2 * (i) + (h)
#define OBJ(p) order[n++] = P_OBJ + (p)
    if (mode == 7) {
        OBJ(3); OBJ(2); OBJ(1); BGP(0, 0); OBJ(0);
    } else if (mode == 0) {
        OBJ(3); BGP(0, 1); BGP(1, 1); OBJ(2); BGP(0, 0); BGP(1, 0);
        OBJ(1); BGP(2, 1); BGP(3, 1); OBJ(0); BGP(2, 0); BGP(3, 0);
    } else if (mode >= 2) {                 /* modes 2-6 (snes_ppu.c front) */
        OBJ(3); BGP(0, 1); OBJ(2); BGP(1, 1); OBJ(1); BGP(0, 0); OBJ(0); BGP(1, 0);
    } else {
        if (bg3top) BGP(2, 1);
        OBJ(3); BGP(0, 1); BGP(1, 1); OBJ(2); BGP(0, 0); BGP(1, 0);
        OBJ(1);
        if (!bg3top) BGP(2, 1);
        OBJ(0); BGP(2, 0);
    }
#undef BGP
#undef OBJ
    int nbg = mode == 0 ? 4 : mode == 7 ? 1 : mode == 1 ? 3 : 2;
    for (int g = 0; g < 16; g++) {
        uint32_t cover = 0, acc[8] = {0};
        for (int k = 0; k < n && cover != 0xFFFF; k++) {
            int ps = order[k];
            uint32_t m, src[8];
            if (ps >= P_OBJ) {
                if (!(tm & 0x10)) continue;
                int p = ps - P_OBJ;
                uint32_t p0 = o->w[9][g + 1], p1 = o->w[10][g + 1];
                m = o->w[8][g + 1] & ((p & 1) ? p0 : ~p0) & ((p & 2) ? p1 : ~p1) & keep[4][g];
                for (int q = 0; q < 8; q++) src[q] = o->w[q][g + 1];
            } else {
                int i = ps >> 1;
                if (i >= nbg || !(tm & (1 << i))) continue;
                Strip *st = &bg[i];
                m = group((ps & 1) ? st->mhi : st->mlo, st->fine, g) & keep[i][g];
                for (int q = 0; q < 8; q++) src[q] = group(st->plane[q], st->fine, g);
            }
            uint32_t eff = m & ~cover;
            cover |= m;
            for (int q = 0; q < 8; q++) acc[q] |= src[q] & eff;
        }
        for (int q = 0; q < 8; q++) out[g * 8 + q] = (uint16_t)acc[q];
    }
}

static void passes(const uint32_t *slot, Strip *bg, const ObjRow *o, uint32_t keep[5][16], int layers,
                   uint32_t sub, int g, uint32_t acc[8], uint32_t *cover, uint32_t *math)
{
    int mode = slot[DSP_SLOT_BGMODE] & 7;
    int bg3top = (slot[DSP_SLOT_BGMODE] & 8) != 0;
    int order[16], n = 0;
#define BGP(i, h) order[n++] = 2 * (i) + (h)
#define OBJ(p) order[n++] = P_OBJ + (p)
    if (mode == 0) {
        OBJ(3); BGP(0, 1); BGP(1, 1); OBJ(2); BGP(0, 0); BGP(1, 0);
        OBJ(1); BGP(2, 1); BGP(3, 1); OBJ(0); BGP(2, 0); BGP(3, 0);
    } else if (mode >= 2) {                 /* modes 2-6 (snes_ppu.c front) */
        OBJ(3); BGP(0, 1); OBJ(2); BGP(1, 1); OBJ(1); BGP(0, 0); OBJ(0); BGP(1, 0);
    } else {
        if (bg3top) BGP(2, 1);
        OBJ(3); BGP(0, 1); BGP(1, 1); OBJ(2); BGP(0, 0); BGP(1, 0);
        OBJ(1);
        if (!bg3top) BGP(2, 1);
        OBJ(0); BGP(2, 0);
    }
#undef BGP
#undef OBJ
    int nbg = mode == 0 ? 4 : mode == 1 ? 3 : 2;
    uint32_t cov = 0, mm = 0;
    for (int k = 0; k < n && cov != 0xFFFF; k++) {
        int ps = order[k];
        uint32_t m, src[8], en;
        if (ps >= P_OBJ) {
            if (!(layers & 0x10)) continue;
            int p = ps - P_OBJ;
            uint32_t p0 = o->w[9][g + 1], p1 = o->w[10][g + 1];
            m = o->w[8][g + 1] & ((p & 1) ? p0 : ~p0) & ((p & 2) ? p1 : ~p1) & keep[4][g];
            for (int q = 0; q < 8; q++) src[q] = o->w[q][g + 1];
            en = ((sub >> 12) & 1) ? src[6] : 0;
        } else {
            int i = ps >> 1;
            if (i >= nbg || !(layers & (1 << i))) continue;
            Strip *st = &bg[i];
            m = group((ps & 1) ? st->mhi : st->mlo, st->fine, g) & keep[i][g];
            for (int q = 0; q < 8; q++) src[q] = group(st->plane[q], st->fine, g);
            en = ((sub >> (8 + i)) & 1) ? 0xFFFF : 0;
        }
        uint32_t eff = m & ~cov;
        cov |= m;
        mm |= eff & en;
        for (int q = 0; q < 8; q++) acc[q] |= src[q] & eff;
    }
    *cover = cov & 0xFFFF;
    *math = mm & 0xFFFF;
}

int dsp_model_frame(const uint32_t *w, int n, int kernel, const uint8_t *vr, uint16_t *planar,
                    uint8_t *pages)
{
    vram = vr;
    touched = pages;
    int at = 0;
    if (n < 1 || w[at++] != DSP_CMD_FRAME) return -1;
    uint32_t slot[DSP_NSLOT_WIN] = {0};
    int nslot = kernel == DSP_KERNEL_WIN ? DSP_NSLOT_WIN : DSP_NSLOT;
    for (int i = 0; i < nslot; i++) slot[i] = w[at++];
    static uint32_t keep[5][16];
    win_keep(slot, keep);                   /* no window slots: keeps everything */
    int nchange = (int)w[at++];
    const uint32_t *chg = w + at;
    at += nchange * 2;
    int nseg = (int)w[at++];
    const uint32_t *seg[16];
    int segrow[16], segn[16];
    for (int k = 0; k < nseg && k < 16; k++) {
        segrow[k] = (int)w[at++];
        segn[k] = (int)w[at++];
        seg[k] = w + at;
        at += segn[k] * 2;
    }
    uint8_t reloc[32];
    int nrel = 0;
    if (kernel == DSP_KERNEL_WIN) {            /* the window build: the relocations */
        if (at >= n) return -1;
        nrel = (int)w[at++];
        if (nrel > 16 || at + nrel > n) return -1;
        for (int k = 0; k < nrel; k++) {
            reloc[k * 2] = (uint8_t)(w[at] & 15);
            reloc[k * 2 + 1] = (uint8_t)((w[at] >> 4) & 15);
            at++;
        }
    }
    const uint32_t *opt = 0;                /* the tile programs: offset-per-tile */
    if (kernel != DSP_KERNEL_M7) {
        if (at >= n) return -1;
        int nopt = (int)w[at++];
        if ((nopt != 0 && nopt != 66) || at + nopt > n) return -1;
        if (nopt) opt = w + at;
        at += nopt;
    }
    const uint32_t *m7 = at < n ? w + at : 0;  /* the Mode 7 program's section */
    if (!m7 && at != n) return -1;
    static Strip bg[4];
    static ObjRow o;
    int ci = 0, si = -1, skipped = 0;
    for (int row = 0; row < PPU_H; row++) {
        while (ci < nchange && (int)(chg[ci * 2] >> 8) <= row) {
            int k = (int)(chg[ci * 2] & 0xFF);
            if (k >= nslot) return -1;
            slot[k] = chg[ci * 2 + 1];
            if (k >= DSP_NSLOT) win_keep(slot, keep);
            ci++;
        }
        while (si + 1 < nseg && segrow[si + 1] <= row) si++;
        uint16_t *out = planar + row * 128;
        int mode = slot[DSP_SLOT_BGMODE] & 7, tm = slot[DSP_SLOT_TM];
        if (m7) {
            if (m7 + 1 > w + n) return -1;
            m7 += m7_strip(m7, &bg[0]);   /* one entry per row, whatever the row */
            if (m7 > w + n) return -1;
        }
        if (slot[DSP_SLOT_INIDISP] & 0x80) {
            for (int i = 0; i < 128; i++) out[i] = 0;
            continue;
        }
        if (mode == 7 && !m7) {
            skipped++;
            continue;
        }
        if (mode == 7) {
            /* layer 0's strip came from the Mode 7 section */
        } else {
            int both = tm | ((slot[DSP_SLOT_SUB] & 0x10000) ? (int)(slot[DSP_SLOT_SUB] & 0x1F) : 0);
            if (mode == 0) {
                for (int i = 0; i < 4; i++)
                    if (both & (1 << i)) strip_build(slot, i, 2, i * 32, row, &bg[i], 0);
            } else {
                const uint32_t *o2 = mode == 2 ? opt : 0;
                if (both & 1) strip_build(slot, 0, 4, 0, row, &bg[0], o2);
                if (both & 2) strip_build(slot, 1, 4, 0, row, &bg[1], o2 ? o2 + 33 : 0);
                if (mode == 1 && (both & 4)) strip_build(slot, 2, 2, 0, row, &bg[2], 0);
            }
            tm = both;                          /* (the sprites below) */
        }
        if ((tm & 0x10) && si >= 0) obj_row(slot, seg[si], segn[si], row, &o);
        else for (int q = 0; q < 11; q++) for (int g = 0; g < 18; g++) o.w[q][g] = 0;
        compose(slot, bg, &o, keep, out, row, reloc, nrel);
    }
    if (m7 && m7 != w + n) return -1;
    return skipped;
}
