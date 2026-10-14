/* The CPU side of the DSP renderer: see ppu_dsp.h. Freestanding. */
#include "ppu_dsp.h"
#include "ppu_tc.h"
#include "m7_plan.h"

static const uint8_t obj_sizes[8][2][2] = {   /* as snes_ppu.c */
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

#define MAX_CHANGES 1024
#define MAX_SEGS 16
#define M7_WORDS 6000                       /* the Mode 7 section, at most */

/* A layer's configuration, cached until a register changes. */
typedef struct {
    int on, bpp, big, tsh, wide;
    uint32_t mapbase, chrbase, words, key;
    int mw, mh, vofs;                       /* masks in tiles, V scroll */
    int hofs;                               /* H scroll (tc: the columns shown) */
    uint32_t cells[64][2];                  /* tc: map cells (row, column) marked this frame */
    uint32_t done[2];                       /* map rows marked, for key */
    uint32_t chr[4];                        /* character pages from chrbase, not yet marked
                                               (8 bpp: up to 128) */
    uint32_t raw[4];                        /* tc: map pages read (page number bits), not yet marked */
    uint32_t dchr[4];                       /* rows: the character pages (from dchrbase) written since the
                                               last committed picture, as page number bits */
    uint32_t dchrbase;
    int dvalid;
    int bbase, bshift;                      /* palette p -> CGRAM block bbase + (p >> bshift) */
    uint16_t blk;                           /* CGRAM blocks its map rows name, this frame */
    uint8_t pals;                           /* the palettes they name */
    int role;                               /* this frame: 1 main screen, 2 sub screen */
    int pend0, pend1;                       /* tc: rows whose cells are not marked yet,
                                               all with the fields above (pend_add) */
    int popt;                               /* and whether offset-per-tile applies */
} Layer;

typedef struct {
    const PpuInput *in;
    DspPlan *p;
    uint32_t slot[DSP_NSLOT_TC], first[DSP_NSLOT_TC];
    uint8_t cg[512];                        /* tc: CGRAM as the last row saw it */
    uint8_t cg0[512];                       /* tc: CGRAM at row 0 (the stream's) */
    uint16_t cgw;                           /* tc: PpuRegs.cgwrites the last row saw */
    uint32_t change[MAX_CHANGES * 2];
    int nchange, overflow;
    struct { int row; const uint8_t *oam; uint8_t obsel; } seg[MAX_SEGS];
    int nseg;
    int blank, mode7, bg1, nlayers;
    int win;                                /* windows cut the open span's rows */
    int sub;                                /* the open span's rows add the sub screen */
    int objrole;                            /* sprites this frame: 1 main, 2 sub */
    int boost;                              /* a sub row adds without halving */
    uint8_t objpals;                        /* the palettes of the sprites sent */
    int mseen, mbad;                        /* colour math: a row seen, not the palette's */
    uint32_t men[8], mdis[8];               /* entries changed, entries shown unchanged */
    uint8_t mop, mfixed[3];
    int cur;                                /* first row of the open span */
    int tilerows;                           /* visible rows in a tile mode */
    int band0, band1;                       /* tc: the rows drawn (DspPlan.band0/1) */
    Layer layer[4];
    int optspan;                            /* the open span is BG mode 2 */
    int hasopt;                             /* opt holds the frame's offset-per-tile */
    uint32_t opt[66];                       /* per layer (BG1, BG2) 33 slivers */
    uint32_t optcur[66];                    /* the open span's (update), recorded into
                                               opt where the span meets the band */
    PpuRegs m7regs;                         /* the registers of the open Mode 7 span */
    uint32_t m7w[M7_WORDS];                 /* the Mode 7 section (ppu_dsp.h) */
    int nm7;
} Plan;

static void need(DspPlan *p, uint32_t word);

/* tc: the page is read in conversion f (ppu_tc.h) */
static void need_f(DspPlan *p, uint32_t word, int f)
{
    need(p, word);
    p->fmt[(word & 0x7FFF) >> 8] |= (uint8_t)f;
}

static void need(DspPlan *p, uint32_t word)
{
    if (p->defer_pages) return;
    uint32_t pg = (word & 0x7FFF) >> 8;
    if (!p->need[pg]) {
        p->need[pg] = 1;
        p->nneed++;
    }
}

/* Mark the character pages a layer collected, from its chrbase. */
static void flush_chr(DspPlan *p, Layer *l)
{
    int f = l->bpp == 2 ? TC_F2 : l->bpp == 4 ? TC_F4 : TC_F8;
    for (int w = 0; w < 4; w++) {
        uint32_t b = l->chr[w];
        for (int i = 0; b; i++, b >>= 1)
            if (b & 1) need_f(p, l->chrbase + (uint32_t)(w * 32 + i) * 256, f);
        l->chr[w] = 0;
        b = l->raw[w];
        for (int i = 0; b; i++, b >>= 1)
            if (b & 1) need_f(p, (uint32_t)(w * 32 + i) * 256, TC_RAW);
        l->raw[w] = 0;
    }
}

static void pend_flush(Plan *pl, Layer *l, int i);

static void layer_config(Plan *pl, Layer *l, const PpuRegs *s, int bg, int bpp)
{
    DspPlan *p = pl->p;
    uint8_t sc = s->r[0x07 + bg];
    uint8_t nba = s->r[0x0B + (bg >> 1)];
    int big = (s->r[0x05] >> (4 + bg)) & 1;
    uint32_t chr = (uint32_t)((bg & 1) ? (nba >> 4) : (nba & 15));
    uint32_t key = (uint32_t)sc | chr << 8 | (uint32_t)bpp << 12 | (uint32_t)big << 16 | 1u << 20;
    /* tc: the rows so far with the old fields get their cells now */
    if (p->tc && (key != l->key || (int)(s->bgofs[bg * 2] & 0x3FF) != l->hofs ||
                  (int)(s->bgofs[bg * 2 + 1] & 0x3FF) != l->vofs || (pl->optspan && bg < 2) != l->popt))
        pend_flush(pl, l, bg);
    if (key != l->key) {
        flush_chr(p, l);                    /* with the old chrbase */
        l->key = key;
        l->done[0] = l->done[1] = 0;
        for (int r = 0; r < 64; r++) l->cells[r][0] = l->cells[r][1] = 0;
    }
    l->on = 1;
    l->bpp = bpp;
    l->big = big;
    l->tsh = big ? 4 : 3;
    l->wide = sc & 1;
    l->mapbase = (uint32_t)(sc & 0xFC) << 8;
    l->chrbase = chr << 12;
    l->words = (uint32_t)bpp * 4;
    l->mw = (sc & 1) ? 63 : 31;
    l->mh = (sc & 2) ? 63 : 31;
    l->vofs = s->bgofs[bg * 2 + 1] & 0x3FF;
    l->hofs = s->bgofs[bg * 2] & 0x3FF;
    l->bshift = bpp == 4 ? 0 : 2;
    l->bbase = (s->r[0x05] & 7) == 0 ? 2 * bg : 0;
}

/* The character pages of one 32-entry map block, as bits from chrbase (a
 * character never crosses a page: page = (chr * words) >> 8, at most 64),
 * kept with a copy of the block: maps rarely change, and Zeal's per-line
 * scroll touches nearly every map row of three layers each frame. */
typedef struct {
    uint8_t valid, sh, big, pals;           /* pals: the palettes its entries name */
    uint32_t bits[4];
    uint16_t copy[32];
} Block;
static Block blocks[0x8000 / 32];

static int trust;                           /* vdirty given: skip the compare */

void dsp_plan_reset(void)
{
    for (unsigned i = 0; i < sizeof blocks / sizeof blocks[0]; i++) blocks[i].valid = 0;
}

static const Block *block_rebuild(const uint8_t *vram, uint32_t a, int sh, int big)
{
    const uint16_t *m = (const uint16_t *)(const void *)(vram + a * 2);
    Block *c = &blocks[a >> 5];
    if (c->valid && c->sh == sh && c->big == big) {    /* (with trust, block_bits has returned it) */
        int i = 0;
        while (i < 32 && c->copy[i] == m[i]) i++;
        if (i == 32) return c;
    }
    uint32_t bb[4] = {0, 0, 0, 0};
    uint8_t pals = 0;
    uint8_t *cb = (uint8_t *)c->copy;
    const uint8_t *mb = (const uint8_t *)m;
    if (big) {
        for (int i = 0; i < 32; i++, mb += 2) {
            cb[i * 2] = mb[0];
            cb[i * 2 + 1] = mb[1];
            uint32_t chr = (uint32_t)mb[0] | (uint32_t)(mb[1] & 3) << 8;   /* VRAM is little-endian */
            pals |= (uint8_t)(1 << ((mb[1] >> 2) & 7));
            uint32_t pg = chr >> sh;
            bb[pg >> 5] |= 1u << (pg & 31);
            pg = ((chr + 1) & 0x3FF) >> sh;
            bb[pg >> 5] |= 1u << (pg & 31);
            pg = ((chr + 16) & 0x3FF) >> sh;
            bb[pg >> 5] |= 1u << (pg & 31);
            pg = ((chr + 17) & 0x3FF) >> sh;
            bb[pg >> 5] |= 1u << (pg & 31);
        }
    } else {
        for (int i = 0; i < 32; i++, mb += 2) {
            cb[i * 2] = mb[0];
            cb[i * 2 + 1] = mb[1];
            uint32_t pg = ((uint32_t)mb[0] | (uint32_t)(mb[1] & 3) << 8) >> sh;
            pals |= (uint8_t)(1 << ((mb[1] >> 2) & 7));
            bb[pg >> 5] |= 1u << (pg & 31);
        }
    }
    for (int w = 0; w < 4; w++) c->bits[w] = bb[w];
    c->pals = pals;
    c->valid = 1;
    c->sh = (uint8_t)sh;
    c->big = (uint8_t)big;
    return c;
}

/* The cached bits of the map block at word a (32-aligned): straight from the cache when the
 * VRAM writes are tracked (trust) and the block is valid, else compared/rebuilt */
static inline const Block *block_bits(const uint8_t *vram, uint32_t a, int sh, int big)
{
    const Block *c = &blocks[a >> 5];
    if (trust && c->valid && c->sh == sh && c->big == big) return c;
    return block_rebuild(vram, a, sh, big);
}

/* The pages a layer reads on this row: every entry of its map row (whatever
 * the horizontal scroll) and their characters (all four of a 16x16 tile),
 * once per map row and frame. */
static void need_maprow(Plan *pl, Layer *l, int ty)
{
    if (l->done[ty >> 5] & (1u << (ty & 31))) return;
    l->done[ty >> 5] |= 1u << (ty & 31);
    uint32_t base = l->mapbase;
    int t = ty;
    if (t >= 32) { base += l->wide ? 0x800 : 0x400; t -= 32; }
    const uint8_t *vram = pl->in->vram;
    int sh = l->words == 8 ? 5 : l->words == 16 ? 4 : 3;   /* chr -> page offset from chrbase */
    for (int blk = 0; blk <= (l->mw >> 5); blk++) {
        uint32_t a = (base + (blk ? 0x400u : 0) + (uint32_t)t * 32) & 0x7FFF;
        need_f(pl->p, a, TC_RAW);           /* 32 entries: one page */
        const Block *b = block_bits(vram, a, sh, l->big);
        for (int w = 0; w < 4; w++) l->chr[w] |= b->bits[w];
        l->pals |= b->pals;
        for (int p = 0; p < 8; p++)
            if ((b->pals >> p) & 1) l->blk |= (uint16_t)(1 << (l->bbase + (p >> l->bshift)));
    }
}

/* tc: the pages of a character's rows (all four of a 16x16 tile), as bits
 * from the layer's chrbase (flush_chr marks them once) */
static inline void chr_bits(Layer *l, uint32_t c, int csh)
{
    uint32_t pg = c >> csh;
    l->chr[pg >> 5] |= 1u << (pg & 31);
    if (l->big) {
        pg = ((c + 1) & 0x3FF) >> csh;
        l->chr[pg >> 5] |= 1u << (pg & 31);
        pg = ((c + 16) & 0x3FF) >> csh;
        l->chr[pg >> 5] |= 1u << (pg & 31);
        pg = ((c + 17) & 0x3FF) >> csh;
        l->chr[pg >> 5] |= 1u << (pg & 31);
    }
}

/* tc: rows [r0, r1) of layer l: the cells its 33 slivers show, each with
 * its own scroll where offset-per-tile gives one (opt: 33 words or 0).
 * Without offset-per-tile the slivers show one run of columns on a run of
 * map rows: per map row, the cells not marked yet this frame, straight
 * from their map entries (need_cell's work without its calls). */
static void need_span_tc(Plan *pl, Layer *l, int r0, int r1, const uint32_t *opt)
{
    int tile = l->big ? 16 : 8, tsh = l->tsh;
    int mapw = (l->mw + 1) * tile;
    int px0 = l->hofs & (mapw - 1);
    int own = 0;
    for (int k = 0; opt && k < 33; k++) own |= (opt[k] & (3u << 22)) != 0;
    const uint8_t *vram = pl->in->vram;
    int csh = l->words == 8 ? 5 : l->words == 16 ? 4 : 3;      /* character -> page */
    if (own) {
        for (int k = 0; k < 33; k++) {
            int hc = px0 & ~7, vofs = l->vofs;
            if (opt[k] & (1u << 23)) hc = (int)(opt[k] & 0x3F8);
            if (opt[k] & (1u << 22)) vofs = (int)((opt[k] >> 10) & 0x3FF);
            int tx = ((hc + 8 * k) & (mapw - 1)) / tile;
            int t0 = (r0 + 1 + vofs) >> tsh, t1 = (r1 + vofs) >> tsh;
            if (t1 - t0 > l->mh) t1 = t0 + l->mh;
            /* column tx on rows t0..t1: what mark_cell does, with the layer's
             * fields taken out of the loop */
            uint32_t bit = 1u << (tx & 31), col = l->mapbase + (tx >= 32 ? 0x400u : 0) + (uint32_t)(tx & 31);
            uint32_t tall = l->wide ? 0x800u : 0x400u, pals = 0;
            uint32_t *cp = &l->cells[0][tx >> 5];
            int mh = l->mh, big = l->big;
            for (int t = t0; t <= t1; t++) {
                int ty = t & mh;
                if (cp[ty * 2] & bit) continue;
                cp[ty * 2] |= bit;
                uint32_t a = (col + (uint32_t)(ty & 31) * 32 + (ty >= 32 ? tall : 0)) & 0x7FFF;
                l->raw[a >> 13] |= 1u << ((a >> 8) & 31);
                const uint8_t *m = vram + a * 2;
                uint32_t c = m[0] | (uint32_t)(m[1] & 3) << 8, pg = c >> csh;
                l->chr[pg >> 5] |= 1u << (pg & 31);
                if (big) {
                    pg = ((c + 1) & 0x3FF) >> csh;
                    l->chr[pg >> 5] |= 1u << (pg & 31);
                    pg = ((c + 16) & 0x3FF) >> csh;
                    l->chr[pg >> 5] |= 1u << (pg & 31);
                    pg = ((c + 17) & 0x3FF) >> csh;
                    l->chr[pg >> 5] |= 1u << (pg & 31);
                }
                pals |= 1u << ((m[1] >> 2) & 7);
            }
            l->pals |= (uint8_t)pals;
        }
        return;
    }
    uint32_t cols[2] = {0, 0};              /* map columns shown (64 at most) */
    for (int k = 0; k < 33; k++) {
        int tx = (((px0 & ~7) + 8 * k) & (mapw - 1)) >> tsh;
        cols[tx >> 5] |= 1u << (tx & 31);
    }
    int t0 = (r0 + 1 + l->vofs) >> tsh, t1 = (r1 + l->vofs) >> tsh;
    if (t1 - t0 > l->mh) t1 = t0 + l->mh;
    for (int t = t0; t <= t1; t++) {
        int ty = t & l->mh;
        for (int b = 0; b <= (l->mw >> 5); b++) {
            uint32_t want = cols[b] & ~l->cells[ty][b];
            if (!want) continue;
            l->cells[ty][b] |= want;
            uint32_t a = l->mapbase + (b ? 0x400u : 0) + (ty >= 32 ? (l->wide ? 0x800u : 0x400u) : 0) +
                         (uint32_t)(ty & 31) * 32;
            uint32_t pg = (a & 0x7FFF) >> 8;
            l->raw[pg >> 5] |= 1u << (pg & 31);
            if (want == 0xFFFFFFFFu) {      /* the whole map row block: its cached page bits */
                const Block *bk = block_bits(vram, a & 0x7FFF, csh, l->big);
                l->pals |= bk->pals;
                for (int w = 0; w < 4; w++) l->chr[w] |= bk->bits[w];
                continue;
            }
            for (int x = 0; want; x++, want >>= 1) {
                if (!(want & 1)) continue;
                const uint8_t *m = vram + ((a + (uint32_t)x) & 0x7FFF) * 2;
                chr_bits(l, m[0] | (uint32_t)(m[1] & 3) << 8, csh);
                l->pals |= (uint8_t)(1 << ((m[1] >> 2) & 7));
            }
        }
    }
}

/* tc: a layer's cells are marked per run of rows with the same fields, not
 * per span: HDMA that writes a register every line (colour gradients,
 * window edges) would make every row a span of its own. */
static void pend_flush(Plan *pl, Layer *l, int i)
{
    if (l->pend1 > l->pend0)
        need_span_tc(pl, l, l->pend0, l->pend1, l->popt ? pl->opt + 33 * i : 0);
    l->pend0 = l->pend1 = 0;
}

static void pend_add(Plan *pl, Layer *l, int i, int r0, int r1, int popt)
{
    if (l->pend1 > l->pend0 && (l->pend1 != r0 || l->popt != popt)) pend_flush(pl, l, i);
    if (l->pend1 <= l->pend0) {
        l->pend0 = r0;
        l->popt = popt;
    }
    l->pend1 = r1;
}

/* Rows [r0, r1) with one configuration: row r shows map row
 * ((r + 1 + vofs) >> tsh) & mh, a contiguous run of map rows. */
static void need_span(Plan *pl, Layer *l, int r0, int r1)
{
    int t0 = (r0 + 1 + l->vofs) >> l->tsh, t1 = (r1 + l->vofs) >> l->tsh;
    if (t1 - t0 > l->mh) t1 = t0 + l->mh;   /* every map row */
    for (int t = t0; t <= t1; t++) need_maprow(pl, l, t & l->mh);
}

/* Colour math on a visible row, for the palette (DspPlan.mathpal): which
 * CGRAM entries it changes and which shown layers draw unchanged. */
static void mark(uint32_t *m, int lo, int hi)
{
    for (int e = lo; e <= hi; e++) m[e >> 5] |= 1u << (e & 31);
}

static void math_row(Plan *pl, const PpuRegs *s)
{
    uint8_t cgwsel = s->r[0x30], cgadsub = s->r[0x31];
    uint8_t tm = s->r[0x2C] & 0x1F, ts = s->r[0x2D] & 0x1F;
    int mode = s->r[0x05] & 7, mathmode = (cgwsel >> 4) & 3;
    int active = mathmode != 3 && (cgadsub & 0x3F);
    if (cgwsel >> 6) { pl->mbad = 1; return; }  /* clip to black */
    if (active && (mathmode != 0 || ((cgwsel & 2) && ts))) { pl->mbad = 1; return; }
    uint32_t en[8] = {0}, *dis = pl->mdis;
    for (int i = 0; i < 4; i++) {
        if (!(tm & (1 << i))) continue;
        int lo, hi;
        if (mode == 0) { lo = i ? 32 * i : 1; hi = 32 * i + 31; }
        else if (mode == 7) { if (i) continue; lo = 1; hi = 255; }
        else if (i < 2) { lo = 1; hi = 127; }
        else if (i == 2) { lo = 1; hi = 31; }
        else continue;
        mark(active && ((cgadsub >> i) & 1) ? en : dis, lo, hi);
    }
    if (tm & 0x10) {
        mark(dis, 128, 191);
        mark(active && (cgadsub & 0x10) ? en : dis, 192, 255);
    }
    mark(active && (cgadsub & 0x20) ? en : dis, 0, 0);
    uint8_t op = (uint8_t)(cgadsub & 0x80) | ((cgadsub & 0x40) && !(cgwsel & 2) ? 0x40 : 0);
    int any = 0;
    for (int k = 0; k < 8; k++) any |= en[k] != 0;
    if (any) pl->p->mathrows++;
    if (!pl->mseen) {
        pl->mseen = 1;
        for (int k = 0; k < 8; k++) pl->men[k] = en[k];
        pl->mop = op;
        for (int k = 0; k < 3; k++) pl->mfixed[k] = s->fixed[k];
        return;
    }
    for (int k = 0; k < 8; k++)
        if (en[k] != pl->men[k]) pl->mbad = 1;
    if (any && (op != pl->mop || s->fixed[0] != pl->mfixed[0] || s->fixed[1] != pl->mfixed[1] ||
                s->fixed[2] != pl->mfixed[2]))
        pl->mbad = 1;
}

void dsp_sub_palette(const DspPlan *p, const uint8_t *cgram, uint8_t *out)
{
    for (int b = 0; b < 16; b++) {
        if (p->subreloc[b] == 0xFF) continue;
        for (int c = 0; c < 16; c++) {
            int e = b * 16 + c, d = p->subreloc[b] * 16 + c;
            uint16_t v = (uint16_t)(cgram[e * 2] | cgram[e * 2 + 1] << 8), o = 0;
            for (int k = 0; k < 3; k++) {
                int r = 2 * ((v >> (5 * k)) & 31) + p->subk[k];
                o |= (uint16_t)((r > 31 ? 31 : r) << (5 * k));
            }
            out[d * 2] = (uint8_t)o;
            out[d * 2 + 1] = (uint8_t)(o >> 8);
        }
    }
}

void dsp_math_palette(const DspPlan *p, const uint8_t *cgram, uint8_t *out)
{
    for (int e = 0; e < 256; e++) {
        uint16_t c = (uint16_t)(cgram[e * 2] | cgram[e * 2 + 1] << 8);
        if (p->mathpal && ((p->mathmask[e >> 5] >> (e & 31)) & 1)) {
            int v[3] = {c & 31, (c >> 5) & 31, (c >> 10) & 31};
            for (int k = 0; k < 3; k++) {
                int r = (p->mathop & 0x80) ? v[k] - p->mathfixed[k] : v[k] + p->mathfixed[k];
                if (r < 0) r = 0;
                if (p->mathop & 0x40) r >>= 1;
                if (r > 31) r = 31;
                v[k] = r;
            }
            c = (uint16_t)(v[0] | v[1] << 5 | v[2] << 10);
        }
        out[e * 2] = (uint8_t)c;
        out[e * 2 + 1] = (uint8_t)(c >> 8);
    }
}

/* What changed at this row: the slots, the layers, the OAM segment. */
static void update(Plan *pl, int row, const PpuRegs *s)
{
    uint32_t v[DSP_NSLOT_TC];
    v[DSP_SLOT_INIDISP] = s->r[0x00];
    v[DSP_SLOT_BGMODE] = s->r[0x05];
    v[DSP_SLOT_TM] = s->r[0x2C];
    v[DSP_SLOT_OBSEL] = s->r[0x01];
    for (int i = 0; i < 4; i++) v[DSP_SLOT_SC + i] = s->r[0x07 + i];
    v[DSP_SLOT_NBA] = s->r[0x0B];
    v[DSP_SLOT_NBA + 1] = s->r[0x0C];
    for (int i = 0; i < 8; i++) v[DSP_SLOT_OFS + i] = s->bgofs[i];
    v[DSP_SLOT_WSEL] = s->r[0x23] | (uint32_t)s->r[0x24] << 8 | (uint32_t)s->r[0x25] << 16;
    v[DSP_SLOT_WH01] = s->r[0x26] | (uint32_t)s->r[0x27] << 8;
    v[DSP_SLOT_WH23] = s->r[0x28] | (uint32_t)s->r[0x29] << 8;
    v[DSP_SLOT_WLOG] = s->r[0x2A] | (uint32_t)s->r[0x2B] << 8 | (uint32_t)s->r[0x2E] << 16;
    v[DSP_SLOT_SUB] = ppu_sub_cfg(s, pl->in->cgram);
    v[DSP_SLOT_TS] = s->r[0x2D];
    v[DSP_SLOT_TSW] = s->r[0x2F];
    v[DSP_SLOT_MATH] = s->r[0x30] | (uint32_t)s->r[0x31] << 8;
    v[DSP_SLOT_FIXED] = s->fixed[0] | (uint32_t)s->fixed[1] << 5 | (uint32_t)s->fixed[2] << 10;
    if (row == 0) {
        for (int i = 0; i < DSP_NSLOT_TC; i++) pl->first[i] = pl->slot[i] = v[i];
        if (pl->p->tc)
            for (int i = 0; i < 512; i++) pl->cg[i] = pl->cg0[i] = s->cgram[i];
        pl->cgw = s->cgwrites;
    } else {
        if (pl->p->tc && s->cgwrites != pl->cgw)    /* CGRAM written mid-frame */
            for (int e = 0; e < 256; e++) {
                if (s->cgram[e * 2] == pl->cg[e * 2] && s->cgram[e * 2 + 1] == pl->cg[e * 2 + 1]) continue;
                pl->cg[e * 2] = s->cgram[e * 2];
                pl->cg[e * 2 + 1] = s->cgram[e * 2 + 1];
                if (pl->nchange == MAX_CHANGES) { pl->overflow = 1; continue; }
                pl->change[pl->nchange * 2] = (uint32_t)row << 8 | DSP_SLOT_CG;
                pl->change[pl->nchange * 2 + 1] = (uint32_t)e << 16 | pl->cg[e * 2] |
                                                  (uint32_t)(pl->cg[e * 2 + 1] & 0x7F) << 8;
                pl->nchange++;
            }
        for (int i = 0; i < DSP_NSLOT_TC; i++) {
            if (v[i] == pl->slot[i]) continue;
            pl->slot[i] = v[i];
            if (pl->nchange == MAX_CHANGES) { pl->overflow = 1; continue; }
            pl->change[pl->nchange * 2] = (uint32_t)row << 8 | (uint32_t)i;
            pl->change[pl->nchange * 2 + 1] = v[i];
            pl->nchange++;
        }
    }
    uint8_t inidisp = s->r[0x00], tm = s->r[0x2C];
    int mode = s->r[0x05] & 7;
    pl->blank = (inidisp & 0x80) != 0;
    pl->mode7 = mode == 7;
    pl->bg1 = tm & 1;
    pl->win = !pl->blank && ppu_win_layers(s) != 0;
    if (pl->p->tc) pl->cgw = s->cgwrites;
    if (!pl->blank && !pl->p->tc) math_row(pl, s);   /* the palette's colour math (not tc) */
    if (pl->mode7 && !pl->blank) pl->m7regs = *s;
    /* the sub screen's layers are drawn too where it is added (tc: its
     * layers, always) */
    uint32_t sub = v[DSP_SLOT_SUB];
    uint8_t ts = pl->p->tc ? s->r[0x2D] & 0x1F : (uint8_t)(sub & 0x1F), both = (uint8_t)(tm | ts);
    if (pl->p->tc) pl->bg1 = both & 1;      /* Mode 7's BG1 on either screen */
    pl->sub = sub != 0;
    if (sub & 0x40000) pl->boost = 1;
    for (int i = 0; i < 4; i++) pl->layer[i].on = 0;
    /* BG modes 3-6 (8 bpp, hi-res) are not in the DSP programs: the CPU
     * composes such a frame. Mode 2 draws BG1 and BG2 (4 bpp) with
     * offset-per-tile: one table for the frame (dsp_plan), or the CPU. */
    pl->optspan = !pl->blank && (mode == 2 || (pl->p->tc && (mode == 4 || mode == 6)));
    if (!pl->blank && mode >= 3 && mode <= 6 && !pl->p->tc) pl->overflow = 4;
    if (pl->optspan) {
        ppu_opt_slivers(pl->in, s, 0, pl->optcur);
        ppu_opt_slivers(pl->in, s, 1, pl->optcur + 33);
    }
    if (!pl->blank && mode != 7) {
        if (mode == 0) {
            for (int i = 0; i < 4; i++)
                if (both & (1 << i)) layer_config(pl, &pl->layer[i], s, i, 2);
        } else {
            /* BG1-3 by mode (snes_ppu.c mode_bpp) */
            static const uint8_t bpp[7][3] = {{0}, {4, 4, 2}, {4, 4, 0}, {8, 4, 0}, {8, 2, 0}, {4, 2, 0}, {4, 0, 0}};
            for (int i = 0; i < 3; i++)
                if (bpp[mode][i] && (both & (1 << i))) layer_config(pl, &pl->layer[i], s, i, bpp[mode][i]);
        }
        for (int i = 0; i < 4; i++)
            pl->layer[i].role |= ((tm >> i) & 1) | ((ts >> i) & 1) << 1;
    }
    if (!pl->blank) pl->objrole |= ((tm >> 4) & 1) | ((ts >> 4) & 1) << 1;
    if (pl->blank || !(both & 0x10)) return;
    if (pl->nseg == 0 || pl->seg[pl->nseg - 1].oam != s->oam ||
        pl->seg[pl->nseg - 1].obsel != s->r[0x01]) {
        if (pl->nseg == MAX_SEGS) { pl->overflow = 2; return; }
        pl->seg[pl->nseg].row = row;
        pl->seg[pl->nseg].oam = s->oam;
        pl->seg[pl->nseg].obsel = s->r[0x01];
        pl->nseg++;
    }
}

/* The rows since the last change, [pl->cur, row), with the configuration
 * update() left. */
static void m7_word(Plan *pl, uint32_t w)
{
    if (pl->nm7 == M7_WORDS) { pl->overflow = 5; return; }
    pl->m7w[pl->nm7++] = w & 0xFFFFFF;
}

/* n rows without a Mode 7 entry: one zero word each */
static void m7_zeros(Plan *pl, int n)
{
    if (pl->nm7 + n > M7_WORDS) { pl->overflow = 5; return; }
    uint32_t *d = pl->m7w + pl->nm7;
    for (int i = 0; i < n; i++) d[i] = 0;
    pl->nm7 += n;
}

/* A Mode 7 row's entry: its line cut into segments (m7_plan). */
static void m7_entry(Plan *pl, int row)
{
    PpuM7Line l;
    M7Seg sg[M7_MAX_SEGS];
    ppu_m7_setup(&pl->m7regs, row, &l);
    int n = m7_plan(&l, sg);
    m7_word(pl, (uint32_t)n);
    for (int k = 0; k < n; k++) {
        m7_word(pl, (uint32_t)sg[k].kind << 16 | sg[k].count);
        if (sg[k].kind == M7_FILL) continue;
        /* (dv << 24) + du as 48 bits: high = dv + floor(du / 2^24) */
        m7_word(pl, (uint32_t)sg[k].v);
        m7_word(pl, (uint32_t)sg[k].u + 0x800000);
        m7_word(pl, (uint32_t)(sg[k].dv + (sg[k].du >> 24)));
        m7_word(pl, (uint32_t)sg[k].du);
    }
}

/* The rows since the last change, [pl->cur, row), with the configuration
 * update() left. */
static void close_span(Plan *pl, int row)
{
    int r0 = pl->cur;
    pl->cur = row;
    if (row <= r0) return;
    for (int r = r0; r < row; r++)
        pl->p->rowkind[r] = (uint8_t)(pl->blank ? 0 : pl->mode7 ? 2 : 1);
    /* the rows counted: those of the band (tc; else every row) */
    int b0 = r0 > pl->band0 ? r0 : pl->band0, b1 = row < pl->band1 ? row : pl->band1;
    int inb = b1 > b0 ? b1 - b0 : 0;
    if (pl->blank || !pl->mode7) {
        m7_zeros(pl, row - r0);
        if (pl->blank) return;
    }
    if (pl->win) pl->p->winrows += row - r0;
    if (pl->sub) pl->p->subrows += row - r0;
    if (pl->mode7) {
        pl->p->m7rows += inb;
        for (int r = r0; r < row; r++) {
            if (pl->bg1) m7_entry(pl, r);
            else m7_word(pl, 0);
        }
        return;
    }
    pl->tilerows += inb;
    if (pl->optspan && inb) {                   /* one table for the band's rows */
        for (int k = 0; k < 66; k++) {
            if (!pl->hasopt) pl->opt[k] = pl->optcur[k];
            else if (pl->opt[k] != pl->optcur[k]) pl->overflow = 3;
        }
        pl->hasopt = 1;
    }
    if (pl->p->defer_pages) return;
    for (int i = 0; i < 4; i++) {
        Layer *l = &pl->layer[i];
        if (!l->on) continue;
        if (pl->p->tc) {
            int b0 = r0 > pl->band0 ? r0 : pl->band0, b1 = row < pl->band1 ? row : pl->band1;
            if (b0 < b1) pend_add(pl, l, i, b0, b1, pl->optspan && i < 2);
            continue;
        }
        need_span(pl, l, r0, row);
        if (!pl->optspan || i > 1) continue;
        int vofs = l->vofs;                 /* the columns' own vertical scroll */
        for (int k = 1; k < 33; k++)
            if (pl->opt[i * 33 + k] & (1u << 22)) {
                l->vofs = (int)(pl->opt[i * 33 + k] >> 10) & 0x3FF;
                need_span(pl, l, r0, row);
            }
        l->vofs = vofs;
    }
}

/* ppu_replay_changes: row 0 and the rows where something changed. */
static void plan_row(void *ctx, int row, const PpuRegs *s)
{
    Plan *pl = (Plan *)ctx;
    close_span(pl, row);
    if (pl->p->lc_kind && (((s->r[0x05] & 7) == 7 ? 2 : 1) != pl->p->lc_kind)) {
        PpuRegs blank = *s;
        blank.r[0x00] |= 0x80;
        update(pl, row, &blank);
        return;
    }
    update(pl, row, s);
}

static int put(DspPlan *p, uint32_t w)
{
    if (p->n == DSP_MAX_WORDS) return p->fail = 8, -1;
    p->w[p->n++] = w & 0xFFFFFF;
    return 0;
}

/* Is any row of [r0, r1) in [y, y + h) mod 256? */
static int rows_hit(int y, int h, int r0, int r1)
{
    int end = y + h;
    if (end <= 256) return y < r1 && end > r0;
    return (y < r1 && 256 > r0) || (0 < r1 && end - 256 > r0);
}

/* The palettes of the sprites put_seg sends for segment k (the same test). */
static uint8_t seg_pals(const Plan *pl, int k, int r1)
{
    const uint8_t *oam = pl->seg[k].oam;
    const uint8_t (*sz)[2] = obj_sizes[pl->seg[k].obsel >> 5];
    int r0 = pl->seg[k].row;
    uint8_t pals = 0;
    for (int i = 0; i < 128; i++) {
        const uint8_t *e = oam + i * 4;
        uint8_t hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int w = sz[hi >> 1][0], h = sz[hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        if (x <= -w || x >= 256) continue;
        if (!rows_hit(e[1], h, r0, r1)) continue;
        pals |= (uint8_t)(1 << ((e[3] >> 1) & 7));
    }
    return pals;
}

static int put_seg(Plan *pl, int k, int r1)
{
    DspPlan *p = pl->p;
    const uint8_t *oam = pl->seg[k].oam;
    uint8_t obsel = pl->seg[k].obsel;
    const uint8_t (*sz)[2] = obj_sizes[obsel >> 5];
    uint32_t base = (uint32_t)(obsel & 7) << 13;
    uint32_t gap = (uint32_t)(((obsel >> 3) & 3) + 1) << 12;
    int r0 = pl->seg[k].row;
    if (put(p, (uint32_t)r0)) return -1;
    int at = p->n;
    if (put(p, 0)) return -1;
    int n = 0;
    int w0 = sz[0][0], h0 = sz[0][1], w1 = sz[1][0], h1 = sz[1][1];
    int b0 = r0 > pl->band0 ? r0 : pl->band0, b1 = r1 < pl->band1 ? r1 : pl->band1;
    uint32_t pgs[4] = {0, 0, 0, 0};             /* the sprite pages read, as page number bits */
    for (int j = 0; j < 32; j++) {
        unsigned hb = oam[512 + j];
        for (int k = 0; k < 4; k++, hb >>= 2) {
            const uint8_t *e = oam + (j * 4 + k) * 4;
            unsigned hi = hb & 3;
            int w = hi & 2 ? w1 : w0, h = hi & 2 ? h1 : h0;
            int x = e[0] | ((hi & 1) << 8);
            if (x >= 256) x -= 512;
            if (x <= -w || x >= 256) continue;
            if (!rows_hit(e[1], h, r0, r1)) continue;
            if (put(p, (uint32_t)(e[0] | (hi & 1) << 8) | (uint32_t)e[1] << 9 | (uint32_t)(hi >> 1) << 17) ||
                put(p, (uint32_t)e[2] | (uint32_t)e[3] << 8))
                return -1;
            n++;
            /* its characters: a row of 16 characters is one page (in a band:
             * when it is in range on one of the band's rows) */
            if (b0 >= b1 || !rows_hit(e[1], h, b0, b1)) continue;
            int trow0 = e[2] >> 4;
            uint32_t tb = base + ((e[3] & 1) ? gap : 0);
            for (int r = 0; r < h / 8; r++) {
                uint32_t pg = ((tb + (uint32_t)((trow0 + r) & 15) * 256) & 0x7FFF) >> 8;
                pgs[pg >> 5] |= 1u << (pg & 31);
            }
        }
    }
    for (int w = 0; w < 4; w++)
        for (uint32_t b = pgs[w], i = 0; b; i++, b >>= 1)
            if (b & 1) {
                uint32_t pg = (uint32_t)(w * 32) + i;
                need_f(p, pg << 8, TC_F4);
                if (!p->sprneed[pg]) {
                    p->sprneed[pg] = 1;
                    p->nsprneed++;
                }
            }
    p->w[at] = (uint32_t)n;
    p->sprites += n;
    return 0;
}

/* The free CGRAM blocks for the sub screen's lightened copies (see
 * dsp_plan): p->subreloc and p->subk; the number of blocks relocated. A
 * sub block is relocated when its colours are bright enough to show
 * (their average luminance at least 8 of 31); a dim one adds little, and
 * the main pixel stays there. None when the free blocks run out. */
static int relocate(const Plan *pl, const PpuInput *in, DspPlan *p)
{
    uint16_t mainb = 1, subb = 0;               /* block 0: the backdrop */
    for (int i = 0; i < 4; i++) {
        if (pl->layer[i].role & 1) mainb |= pl->layer[i].blk;
        if (pl->layer[i].role & 2) subb |= pl->layer[i].blk;
    }
    uint16_t objb = (uint16_t)(pl->objpals << 8);
    if (pl->objrole & 1) mainb |= objb;
    if (pl->objrole & 2) subb |= objb;
    uint16_t freeb = (uint16_t)~(mainb | subb);
    /* each sub block's luminance (x 100) over the palettes the sub
     * screen's layers and sprites name */
    uint32_t lum[16] = {0}, cnt[16] = {0};
    const uint8_t *cg = in->cgram;
#define LUM(e) (30u * (cg[(e) * 2] & 31) + 59u * ((uint32_t)(cg[(e) * 2] | cg[(e) * 2 + 1] << 8) >> 5 & 31) + \
                11u * ((cg[(e) * 2 + 1] >> 2) & 31))
    for (int i = 0; i < 4; i++) {
        const Layer *l = &pl->layer[i];
        if (!(l->role & 2)) continue;
        for (int q = 0; q < 8; q++) {
            if (!((l->pals >> q) & 1)) continue;
            int colours = l->bshift ? 4 : 16;
            int e0 = l->bbase * 16 + q * colours;
            for (int e = e0 + 1; e < e0 + colours; e++) {
                lum[e >> 4] += LUM(e);
                cnt[e >> 4]++;
            }
        }
    }
    if (pl->objrole & 2)
        for (int q = 0; q < 8; q++)
            if ((pl->objpals >> q) & 1)
                for (int e = 128 + q * 16 + 1; e < 128 + q * 16 + 16; e++) {
                    lum[e >> 4] += LUM(e);
                    cnt[e >> 4]++;
                }
#undef LUM
    uint8_t rel[16];
    int nrel = 0;
    for (int b = 0; b < 16; b++) {
        rel[b] = 0xFF;
        if (!((subb >> b) & 1) || !cnt[b] || lum[b] < 800 * cnt[b]) continue;
        int f = 0;
        while (f < 16 && !((freeb >> f) & 1)) f++;
        if (f == 16) return 0;
        rel[b] = (uint8_t)f;
        freeb &= (uint16_t)~(1u << f);
        nrel++;
    }
    uint32_t sum[3] = {0, 0, 0}, n = 0;
    for (int b = 0; b < 16; b++) {
        if (!((mainb >> b) & 1)) continue;
        for (int e = b * 16; e < b * 16 + 16; e++) {
            uint16_t c = (uint16_t)(cg[e * 2] | cg[e * 2 + 1] << 8);
            sum[0] += c & 31;
            sum[1] += (c >> 5) & 31;
            sum[2] += (c >> 10) & 31;
            n++;
        }
    }
    for (int b = 0; b < 16; b++) p->subreloc[b] = rel[b];
    for (int k = 0; k < 3; k++) p->subk[k] = (uint8_t)(sum[k] / n / 2);
    return nrel;
}

/* The register changes of the picture dsp_rows replayed, kept for dsp_plan (see DspPlan.use_events) */
#ifndef SMKX_PARTIAL_CHECK
#define SMKX_PARTIAL_CHECK 0
#endif
#if SMKX_PARTIAL_CHECK == 2
#define EV_MAX 256                          /* (the check plans the whole picture too: it needs them all) */
#else
#define EV_MAX 96
#endif
typedef struct { int row; PpuRegs s; } RowEv;
static RowEv evbuf[EV_MAX];
static int nev, ev_ok;

static void ev_record(int row, const PpuRegs *s)
{
    if (nev >= EV_MAX) {
        ev_ok = 0;
        return;
    }
    RowEv *e = &evbuf[nev++];
    e->row = row;
    const uint32_t *x = (const uint32_t *)(const void *)s;
    uint32_t *y = (uint32_t *)(void *)&e->s;
    for (unsigned i = 0; i < sizeof(PpuRegs) / 4; i++) y[i] = x[i];
}

int dsp_rows_events_ok(void)
{
    return ev_ok;
}

int dsp_plan(const PpuInput *in, DspPlan *p)
{
    static Plan pl;
    pl.in = in;
    pl.p = p;
    pl.nchange = pl.overflow = pl.nseg = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t *cc = &pl.layer[i].cells[0][0];
        for (int r = 0; r < 128; r += 4) cc[r] = cc[r + 1] = cc[r + 2] = cc[r + 3] = 0;
        pl.layer[i].key = 0;
        pl.layer[i].on = 0;
        pl.layer[i].blk = 0;
        pl.layer[i].pals = 0;
        pl.layer[i].role = 0;
    }
    pl.sub = pl.objrole = pl.boost = 0;
    pl.objpals = 0;
    p->n = 0;
    p->fail = 0;
    for (int r = 0; r < PPU_H; r++) p->rowkind[r] = 0;
    p->nneed = 0;
    p->m7rows = 0;
    p->winrows = 0;
    p->subrows = 0;
    p->mathrows = 0;
    p->sprites = 0;
    for (int i = 0; i < DSP_VPAGES; i++) p->need[i] = p->sprneed[i] = p->fmt[i] = 0;
    p->nsprneed = 0;
    trust = p->vdirty != 0;
    if (trust)
        for (int pg = 0; pg < DSP_VPAGES; pg++)
            if (p->vdirty[pg]) {
                p->vdirty[pg] = 0;          /* a write from now on marks it again */
                for (int b = 0; b < 8; b++) blocks[pg * 8 + b].valid = 0;
            }
    for (int i = 0; i < 4; i++) {
        for (int w = 0; w < 4; w++) pl.layer[i].chr[w] = pl.layer[i].raw[w] = 0;
        pl.layer[i].pend0 = pl.layer[i].pend1 = pl.layer[i].popt = 0;
    }
    pl.cur = 0;
    pl.blank = 1;
    pl.win = 0;
    pl.mseen = pl.mbad = 0;
    for (int k = 0; k < 8; k++) pl.mdis[k] = 0;
    pl.mode7 = 0;
    pl.optspan = pl.hasopt = 0;
    pl.tilerows = 0;
    pl.band0 = p->band1 > p->band0 ? p->band0 : 0;
    pl.band1 = p->band1 > p->band0 ? p->band1 : PPU_H;
    pl.nm7 = 0;
    int walk = ppu_walk_all;
    if (p->tc) ppu_walk_all = 1;           /* windows, colour math: every row */
    if (p->use_events && ev_ok)
        for (int i = 0; i < nev; i++) plan_row(&pl, evbuf[i].row, &evbuf[i].s);
    else
        ppu_replay_changes(in, plan_row, &pl);
    ppu_walk_all = walk;
    close_span(&pl, PPU_H);
    if (p->tc)
        for (int i = 0; i < 4; i++) pend_flush(&pl, &pl.layer[i], i);
    for (int i = 0; i < 4; i++) flush_chr(p, &pl.layer[i]);
    if (pl.overflow) return p->fail = pl.overflow, -1;
    p->mathpal = 0;
    if (p->mathrows && !pl.mbad) {
        int clash = 0;
        for (int k = 0; k < 8; k++) clash |= (pl.men[k] & pl.mdis[k]) != 0;
        if (!clash) {
            p->mathpal = 1;
            for (int k = 0; k < 8; k++) p->mathmask[k] = pl.men[k];
            p->mathop = pl.mop;
            for (int k = 0; k < 3; k++) p->mathfixed[k] = pl.mfixed[k];
        }
    }
    if (p->m7rows && pl.tilerows) return p->fail = 6, -1; /* Mode 7 and tile rows: neither program */
    if (p->m7rows && p->winrows && !p->tc) return p->fail = 7, -1;   /* the planar Mode 7 build has no windows */
    /* The sub screen's lightened copies go into CGRAM blocks nothing in the
     * frame draws: not the backdrop's, not a block a shown layer's map rows
     * name (their palettes), not a block of a sprite sent. */
    for (int b = 0; b < 16; b++) p->subreloc[b] = 0xFF;
    int nrel = 0;
    if (p->subrows && pl.boost) {           /* (the palettes of the sprites: only relocate reads them) */
        for (int k = 0; k < pl.nseg; k++)
            pl.objpals |= seg_pals(&pl, k, k + 1 < pl.nseg ? pl.seg[k + 1].row : PPU_H);
        nrel = relocate(&pl, in, p);
    }
    /* Rows that add the sub screen with no block relocated and no black
     * backdrop keep the main screen (as the reference does): off. */
    if (p->subrows && !nrel) {
        int still = (pl.first[DSP_SLOT_SUB] & 0x40000) && !(pl.first[DSP_SLOT_SUB] & 0x20000) ?
                    (pl.first[DSP_SLOT_SUB] = 0) : pl.first[DSP_SLOT_SUB] != 0;
        for (int i = 0; i < pl.nchange; i++) {
            if ((int)(pl.change[i * 2] & 0xFF) != DSP_SLOT_SUB) continue;
            uint32_t *v = &pl.change[i * 2 + 1];
            if ((*v & 0x40000) && !(*v & 0x20000)) *v = 0;
            still |= *v != 0;
        }
        if (!still) p->subrows = 0;
    }
    if (p->tc)                              /* no visible BG row: the one loaded */
        p->kernel = p->m7rows || (!pl.tilerows && p->prefer == DSP_KERNEL_TC7) ? DSP_KERNEL_TC7 : DSP_KERNEL_TC;
    else
        p->kernel = p->m7rows ? DSP_KERNEL_M7 : p->winrows || p->subrows || p->force_win ? DSP_KERNEL_WIN
                  : pl.tilerows ? DSP_KERNEL_TILE : p->prefer;
    int nslot = p->tc ? DSP_NSLOT_TC : p->kernel == DSP_KERNEL_WIN ? DSP_NSLOT_WIN : DSP_NSLOT;
    if (p->kernel == DSP_KERNEL_M7)
        for (int pg = 0; pg < 64; pg++) need(p, (uint32_t)pg << 8);   /* the Mode 7 plane */
    if (p->kernel == DSP_KERNEL_TC7)
        for (int pg = 0; pg < 64; pg++) need_f(p, (uint32_t)pg << 8, TC_M7);
    /* tc, a band: the state at its first row as the row-0 slots and CGRAM
     * (the changes before it applied), none after it, and only the OAM
     * segments its rows use: the stream carries the band's rows */
    int fold = p->tc && pl.band0 > 0;
    if (fold)
        for (int i = 0; i < pl.nchange; i++) {
            if ((int)(pl.change[i * 2] >> 8) >= pl.band0) break;
            int k = (int)(pl.change[i * 2] & 0xFF);
            uint32_t v = pl.change[i * 2 + 1];
            if (k == DSP_SLOT_CG) {
                pl.cg0[((v >> 16) & 0xFF) * 2] = (uint8_t)v;
                pl.cg0[((v >> 16) & 0xFF) * 2 + 1] = (uint8_t)(v >> 8);
            } else if (k < DSP_NSLOT_TC) {
                pl.first[k] = v;
            }
        }
    if (put(p, DSP_CMD_FRAME)) return -1;
    for (int i = 0; i < nslot; i++)
        if (put(p, pl.first[i])) return -1;
    int at = p->n, nch = 0;
    if (put(p, 0)) return -1;
    for (int i = 0; i < pl.nchange; i++) {
        int k = (int)(pl.change[i * 2] & 0xFF), r = (int)(pl.change[i * 2] >> 8);
        if (k >= nslot && !(p->tc && k == DSP_SLOT_CG)) continue;   /* window slots: WIN only */
        if (p->tc && (r < (fold ? pl.band0 : 0) || r >= pl.band1)) continue;
        if (put(p, pl.change[i * 2]) || put(p, pl.change[i * 2 + 1])) return -1;
        nch++;
    }
    p->w[at] = (uint32_t)nch;
    int s0 = 0, s1 = pl.nseg;
    if (p->tc) {
        while (s0 + 1 < s1 && pl.seg[s0 + 1].row <= pl.band0) s0++;   /* the one active at band0 on */
        while (s1 > s0 && pl.seg[s1 - 1].row >= pl.band1) s1--;
    }
    if (put(p, (uint32_t)(s1 - s0))) return -1;
    for (int k = s0; k < s1; k++)
        if (put_seg(&pl, k, k + 1 < pl.nseg ? pl.seg[k + 1].row : PPU_H)) return -1;
    if (p->kernel == DSP_KERNEL_M7)
        for (int i = 0; i < pl.nm7; i++)
            if (put(p, pl.m7w[i])) return -1;
    if (p->tc) {                                /* ppu_tc.h: the table, CGRAM, Mode 7 */
        if (put(p, pl.hasopt ? 66u : 0u)) return -1;
        for (int k = 0; pl.hasopt && k < 66; k++)
            if (put(p, pl.opt[k])) return -1;
        if (p->n + 256 > DSP_MAX_WORDS) return p->fail = 8, -1;
        uint32_t *cw = p->w + p->n;
        const uint8_t *cg = pl.cg0;
        for (int e = 0; e < 256; e++, cg += 2) cw[e] = cg[0] | (uint32_t)(cg[1] & 0x7F) << 8;
        p->n += 256;
        if (p->kernel == DSP_KERNEL_TC7)
            for (int i = 0; i < pl.nm7; i++)
                if (put(p, pl.m7w[i])) return -1;
        p->changes = nch;
        p->segs = pl.nseg;
        return 0;
    }
    if (p->kernel == DSP_KERNEL_WIN) {          /* the window build: the relocations */
        if (put(p, (uint32_t)nrel)) return -1;
        for (int b = 0; b < 16; b++)
            if (p->subreloc[b] != 0xFF && put(p, (uint32_t)b | (uint32_t)p->subreloc[b] << 4)) return -1;
    }
    if (p->kernel != DSP_KERNEL_M7) {           /* the tile programs: offset-per-tile */
        if (put(p, pl.hasopt ? 66u : 0u)) return -1;
        for (int k = 0; pl.hasopt && k < 66; k++)
            if (put(p, pl.opt[k])) return -1;
    }
    p->changes = nch;
    p->segs = pl.nseg;
    return 0;
}


/* ---- partial rendering: which rows differ from the last committed picture (dsp_rows) ----
 * Its own replay of the frame's register changes: the state a row is drawn with is compared as it
 * is, not as dsp_plan derives it, and what a row reads of VRAM only when a page was written or a
 * palette changed. */

#define SPAN_MAX 64
#define SEG_KEEP 4
#define SPW 21                              /* words of a span's state */
typedef struct {
    int valid, usable;
    int nsp, sp_row[SPAN_MAX + 1];
    uint32_t sp_v[SPAN_MAX][SPW];
    uint8_t cg0[512] __attribute__((aligned(4)));
    int cgch;                               /* CGRAM written between rows */
    int nseg, seg_row[SEG_KEEP];
    uint8_t seg_obsel[SEG_KEEP];
    uint8_t seg_oam[SEG_KEEP][PPU_OAM_BYTES] __attribute__((aligned(4)));
} RowState;
static RowState rs[2];
static int rs_cur;

typedef struct {
    DspRows *R;
    const PpuInput *in;
    int cur;                                /* first row of the open span */
    int blank, mode7, optspan, sc3;
    int need;                               /* a page was written or a palette changed: check what rows read */
    uint16_t cgd;                           /* 16-colour CGRAM blocks that differ from the last committed picture's */
    int cgw, over;
    Layer layer[4];
    struct { int row; const uint8_t *oam; uint8_t obsel; } seg[MAX_SEGS];
    int nseg;
    uint32_t last[SPW];
} RowCtx;
static RowCtx rc;

static void rmark(DspRows *R, int a, int b)
{
    if (a < 0) a = 0;
    if (b > PPU_H) b = PPU_H;
    for (int r = a; r < b; r++) R->dirty[r] = 1;
}

/* rows [y, y + h) mod 256 within [a, b) */
static void rows_extent(DspRows *R, int y, int h, int a, int b)
{
    int end = y + h;
    if (end <= 256) {
        rmark(R, y > a ? y : a, end < b ? end : b);
    } else {
        rmark(R, y > a ? y : a, b);
        rmark(R, a, end - 256 < b ? end - 256 : b);
    }
}

/* map row ty of layer l: a block of it written, its characters written or its palettes changed */
static int rows_block_dirty(RowCtx *q, const Layer *l, int ty, int csh, uint8_t pm)
{
    for (int blk = 0; blk <= (l->mw >> 5); blk++) {
        uint32_t a = (l->mapbase + (blk ? 0x400u : 0) + (ty >= 32 ? (l->wide ? 0x800u : 0x400u) : 0) +
                      (uint32_t)(ty & 31) * 32) & 0x7FFF;
        if (q->R->vdirty[a >> 8]) return 1;
        const Block *bk = block_bits(q->in->vram, a, csh, l->big);
        if (bk->pals & pm) return 1;
        for (int w = 0; w < 4; w++)
            if (bk->bits[w] & l->dchr[w]) return 1;
    }
    return 0;
}

/* Rows [r0, r1) of layer l (one configuration): the map rows they show, block by block */
static void rows_layer(RowCtx *q, Layer *l, int r0, int r1, int optlayer)
{
    int csh = l->words == 8 ? 5 : l->words == 16 ? 4 : 3;
    if (!l->dvalid || l->dchrbase != l->chrbase) {
        for (int w = 0; w < 4; w++) l->dchr[w] = 0;
        uint32_t cpg = l->chrbase >> 8;
        for (uint32_t k = 0; k < 128; k++)
            if (q->R->vdirty[(cpg + k) & 0x7F]) l->dchr[k >> 5] |= 1u << (k & 31);
        l->dchrbase = l->chrbase;
        l->dvalid = 1;
    }
    uint8_t pm = 0;
    if (q->cgd) {
        if (l->bpp == 8) pm = 0xFF;
        else
            for (int k = 0; k < 8; k++)
                if ((q->cgd >> (l->bbase + (k >> l->bshift))) & 1) pm |= (uint8_t)(1 << k);
    }
    if (pm == 0) {                          /* no palette of the layer's changed: only a page written can matter */
        int any = (int)(l->dchr[0] | l->dchr[1] | l->dchr[2] | l->dchr[3]);
        uint32_t mp = l->mapbase >> 8, np = 4u * (l->mw > 31 ? 2u : 1u) * (l->mh > 31 ? 2u : 1u);
        for (uint32_t k = 0; k < np && !any; k++) any = q->R->vdirty[(mp + k) & 0x7F];
        if (!any) return;
    }
    if (optlayer) {                         /* offset-per-tile: any map row can show */
        for (int ty = 0; ty <= l->mh; ty++)
            if (rows_block_dirty(q, l, ty, csh, pm)) {
                rmark(q->R, r0, r1);
                return;
            }
        return;
    }
    int t0 = (r0 + 1 + l->vofs) >> l->tsh, t1 = (r1 + l->vofs) >> l->tsh;
    for (int t = t0; t <= t1; t++) {
        if (!rows_block_dirty(q, l, t & l->mh, csh, pm)) continue;
        int lo = (t << l->tsh) - l->vofs - 1, hi = ((t + 1) << l->tsh) - l->vofs - 1;
        rmark(q->R, lo > r0 ? lo : r0, hi < r1 ? hi : r1);
    }
}

/* offset-per-tile rows: BG3's map holds the offsets */
static int rows_optmap_dirty(RowCtx *q)
{
    uint32_t sc = (uint32_t)q->sc3, base = (sc & 0xFC) << 8;
    for (int ty = 0; ty <= ((sc & 2) ? 63 : 31); ty++)
        for (int blk = 0; blk <= ((sc & 1) ? 1 : 0); blk++) {
            uint32_t a = (base + (blk ? 0x400u : 0) + (ty >= 32 ? ((sc & 1) ? 0x800u : 0x400u) : 0) +
                          (uint32_t)(ty & 31) * 32) & 0x7FFF;
            if (q->R->vdirty[a >> 8]) return 1;
        }
    return 0;
}

/* the span [cur, row) is over: what its layers read */
static void rows_close(RowCtx *q, int row)
{
    int r0 = q->cur;
    if (row <= r0) return;
    q->cur = row;
    if (!q->need || q->blank) return;
    if (q->mode7) {                         /* the plane is all of VRAM, the palette all of CGRAM */
        rmark(q->R, r0, row);
        return;
    }
    if (q->optspan && rows_optmap_dirty(q)) rmark(q->R, r0, row);
    for (int i = 0; i < 4; i++)
        if (q->layer[i].on) rows_layer(q, &q->layer[i], r0, row, q->optspan && i < 2);
}

static const uint8_t rows_regs[28] = {0x00, 0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x1A, 0x23, 0x24, 0x25,
                                      0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33};

static void rows_row(void *ctx, int row, const PpuRegs *s)
{
    RowCtx *q = (RowCtx *)ctx;
    RowState *c = &rs[rs_cur];
    const RowState *ref = &rs[rs_cur ^ 1];
#if SMKX_PARTIAL_CHECK != 2
    if (q->over) ev_ok = 0;                 /* too many spans to compare: the plan replays for itself */
    else
#endif
        ev_record(row, s);
    if (row == 0) {
        c->nsp = 0;
        c->cgch = 0;
        q->cgw = s->cgwrites;
        for (int i = 0; i < 128; i++) ((uint32_t *)(void *)c->cg0)[i] = ((const uint32_t *)(const void *)s->cgram)[i];
        q->cgd = 0xFFFF;
        if (ref->valid) {
            q->cgd = 0;
            for (int b = 0; b < 16; b++) {
                const uint32_t *x = (const uint32_t *)(const void *)(c->cg0 + b * 32);
                const uint32_t *y = (const uint32_t *)(const void *)(ref->cg0 + b * 32);
                uint32_t d = 0;
                for (int k = 0; k < 8; k++) d |= x[k] ^ y[k];
                if (d) q->cgd |= (uint16_t)(1u << b);
            }
        }
        int any = 0;
        for (int pg = 0; pg < DSP_VPAGES; pg++) any |= q->R->vdirty[pg] != 0;
        q->need = any || q->cgd != 0;
    }
    if (q->over) return;
    rows_close(q, row);
    int mode = s->r[0x05] & 7, tm = s->r[0x2C], ts = s->r[0x2D], both = tm | ts;
    q->blank = (s->r[0x00] & 0x80) != 0;
    q->mode7 = mode == 7;
    q->optspan = !q->blank && (mode == 2 || mode == 4 || mode == 6);
    q->sc3 = s->r[0x09];
    uint32_t v[SPW];
    for (int i = 0; i < 7; i++) v[i] = 0;
    for (int k = 0; k < 28; k++) v[k >> 2] |= (uint32_t)s->r[rows_regs[k]] << ((k & 3) * 8);
    for (int i = 0; i < 4; i++) v[7 + i] = (uint32_t)s->bgofs[2 * i] | (uint32_t)s->bgofs[2 * i + 1] << 16;
    v[11] = (uint32_t)s->fixed[0] | (uint32_t)s->fixed[1] << 8 | (uint32_t)s->fixed[2] << 16;
    for (int i = 12; i < SPW; i++) v[i] = 0;
    if (q->mode7 && !q->blank) {            /* what ppu_m7_setup reads */
        v[12] = (uint32_t)s->r[0x1A];
        for (int i = 0; i < 6; i++) v[13 + i] = (uint16_t)s->m7[i];
        v[19] = s->m7ofs[0];
        v[20] = s->m7ofs[1];
    }
    int same = row != 0;
    for (int i = 0; i < SPW && same; i++) same = v[i] == q->last[i];
    if (!same) {
        if (c->nsp < SPAN_MAX) {
            c->sp_row[c->nsp] = row;
            for (int i = 0; i < SPW; i++) c->sp_v[c->nsp][i] = v[i];
        }
        c->nsp++;
        for (int i = 0; i < SPW; i++) q->last[i] = v[i];
        if (c->nsp > SPAN_MAX) {            /* too many spans to compare: every row is drawn */
            q->over = 1;
            return;
        }
    }
    if (row > 0 && s->cgwrites != q->cgw) c->cgch = 1;
    /* the layers of the span that starts here (what rows_close reads) */
    for (int i = 0; i < 4; i++) q->layer[i].on = 0;
    if (q->need && !q->blank && !q->mode7) {
        static const uint8_t bt[7][4] = {{2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0}, {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}};
        for (int i = 0; i < 4; i++) {
            int bpp = bt[mode][i];
            if (!bpp || !((both >> i) & 1)) continue;
            Layer *l = &q->layer[i];
            int sc = s->r[0x07 + i], nba = s->r[0x0B + (i >> 1)], big = (s->r[0x05] >> (4 + i)) & 1;
            l->on = 1;
            l->bpp = bpp;
            l->big = big;
            l->tsh = big ? 4 : 3;
            l->wide = sc & 1;
            l->mapbase = (uint32_t)(sc & 0xFC) << 8;
            l->chrbase = (uint32_t)((i & 1) ? (nba >> 4) : (nba & 15)) << 12;
            l->words = (uint32_t)bpp * 4;
            l->mw = (sc & 1) ? 63 : 31;
            l->mh = (sc & 2) ? 63 : 31;
            l->vofs = s->bgofs[i * 2 + 1] & 0x3FF;
            l->hofs = s->bgofs[i * 2] & 0x3FF;
            l->bshift = bpp == 4 ? 0 : 2;
            l->bbase = mode == 0 ? 2 * i : 0;
        }
    }
    if (!q->blank && (both & 0x10) &&
        (q->nseg == 0 || q->seg[q->nseg - 1].oam != s->oam || q->seg[q->nseg - 1].obsel != s->r[0x01])) {
        if (q->nseg == MAX_SEGS) { q->over = 1; return; }
        q->seg[q->nseg].row = row;
        q->seg[q->nseg].oam = s->oam;
        q->seg[q->nseg].obsel = s->r[0x01];
        q->nseg++;
    }
}

static int spr_extent(const uint8_t *oam, int i, const uint8_t (*sz)[2], int *y, int *h)
{
    const uint8_t *e = oam + i * 4;
    int hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    int w = sz[hi >> 1][0];
    int x = e[0] | ((hi & 1) << 8);
    if (x >= 256) x -= 512;
    if (x <= -w || x >= 256) return 0;
    *y = e[1];
    *h = sz[hi >> 1][1];
    return 1;
}

/* Sprites: an entry that changed marks the rows it was on and the rows it is on; one that did not,
 * when its character pages or its palette were written */
static void rows_sprites(RowCtx *q, const RowState *cur, const RowState *ref)
{
    DspRows *R = q->R;
    for (int k = 0; k < cur->nseg; k++) {
        const uint8_t *oc = cur->seg_oam[k], *orf = ref->seg_oam[k];
        int a = cur->seg_row[k], b = k + 1 < cur->nseg ? cur->seg_row[k + 1] : PPU_H;
        if (cur->seg_obsel[k] != ref->seg_obsel[k]) {
            rmark(R, a, b);
            continue;
        }
        int obsel = cur->seg_obsel[k];
        const uint8_t (*sz)[2] = obj_sizes[obsel >> 5];
        uint32_t base = (uint32_t)(obsel & 7) << 13, gap = (uint32_t)(((obsel >> 3) & 3) + 1) << 12;
        int pgdirty = 0;                    /* the sprites' character pages: two name tables of 16 pages */
        for (uint32_t k2 = 0; k2 < 16 && !pgdirty; k2++)
            pgdirty = R->vdirty[((base + k2 * 256) & 0x7FFF) >> 8] | R->vdirty[((base + gap + k2 * 256) & 0x7FFF) >> 8];
        int paldirty = (q->cgd & 0xFF00) != 0;
        const uint32_t *x = (const uint32_t *)(const void *)oc, *y = (const uint32_t *)(const void *)orf;
        int hichg = 0;
        for (int i = 128; i < PPU_OAM_BYTES / 4; i++) hichg |= x[i] != y[i];
        for (int i = 0; i < 128; i++) {
            int changed = x[i] != y[i];
            if (!changed && hichg)          /* the size or bit 8 of x changed */
                changed = ((oc[512 + (i >> 2)] >> ((i & 3) * 2)) & 3) != ((orf[512 + (i >> 2)] >> ((i & 3) * 2)) & 3);
            if (!changed && !pgdirty && !paldirty) continue;
            const uint8_t *ec = oc + i * 4;
            int yc = 0, hc = 0, yr = 0, hr = 0;
            int vc = spr_extent(oc, i, sz, &yc, &hc);
            if (changed) {
                int vr = spr_extent(orf, i, sz, &yr, &hr);
                if (vc) rows_extent(R, yc, hc, a, b);
                if (vr) rows_extent(R, yr, hr, a, b);
                continue;
            }
            if (!vc) continue;
            int dirty = paldirty && ((q->cgd >> (8 + ((ec[3] >> 1) & 7))) & 1);
            if (!dirty && pgdirty) {
                int trow0 = ec[2] >> 4;
                uint32_t tb = base + ((ec[3] & 1) ? gap : 0);
                for (int r = 0; r < hc / 8 && !dirty; r++)
                    dirty = R->vdirty[((tb + (uint32_t)((trow0 + r) & 15) * 256) & 0x7FFF) >> 8] != 0;
            }
            if (dirty) rows_extent(R, yc, hc, a, b);
        }
    }
}

static void rows_span_cmp(DspRows *R, int nA, const int *rowA, const uint32_t *vA, int nB, const int *rowB,
                          const uint32_t *vB)
{
    int i = 0, j = 0, r = 0;
    while (r < PPU_H) {
        int endA = i + 1 < nA ? rowA[i + 1] : PPU_H, endB = j + 1 < nB ? rowB[j + 1] : PPU_H;
        int e = endA < endB ? endA : endB;
        const uint32_t *x = vA + i * SPW, *y = vB + j * SPW;
        uint32_t d = 0;
        for (int k = 0; k < SPW; k++) d |= x[k] ^ y[k];
        if (d) rmark(R, r, e);
        r = e;
        if (r == endA) i++;
        if (r == endB) j++;
    }
}

int dsp_rows(const PpuInput *in, DspRows *R)
{
    RowCtx *q = &rc;
    RowState *c = &rs[rs_cur];
    const RowState *ref = &rs[rs_cur ^ 1];
    q->R = R;
    q->in = in;
    q->cur = 0;
    q->over = 0;
    q->nseg = 0;
    q->blank = 1;
    q->mode7 = q->optspan = 0;
    for (int i = 0; i < 4; i++) {
        q->layer[i].on = 0;
        q->layer[i].dvalid = 0;
        q->layer[i].key = 0;
    }
    for (int r = 0; r < PPU_H; r++) R->dirty[r] = 0;
    R->all = R->ndirty = 0;
    nev = 0;
    ev_ok = 1;
    trust = 1;                              /* the blocks of the pages written since the last commit are stale */
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        if (R->vdirty[pg])
            for (int b = 0; b < 8; b++) blocks[pg * 8 + b].valid = 0;
    int walk = ppu_walk_all;
    ppu_walk_all = 1;                       /* windows, colour math: every row */
    ppu_replay_changes(in, rows_row, q);
    ppu_walk_all = walk;
    rows_close(q, PPU_H);
    /* what the picture was drawn with, kept for the next */
    c->nseg = q->nseg;
    if (q->nseg <= SEG_KEEP)
        for (int k = 0; k < q->nseg; k++) {
            c->seg_row[k] = q->seg[k].row;
            c->seg_obsel[k] = q->seg[k].obsel;
            const uint8_t *o = q->seg[k].oam;
            if (((uintptr_t)o & 3) == 0)
                for (int i = 0; i < PPU_OAM_BYTES / 4; i++)
                    ((uint32_t *)(void *)c->seg_oam[k])[i] = ((const uint32_t *)(const void *)o)[i];
            else
                for (int i = 0; i < PPU_OAM_BYTES; i++) c->seg_oam[k][i] = o[i];
        }
    c->usable = !(q->over || c->nsp > SPAN_MAX || q->nseg > SEG_KEEP);
    int all = !ref->valid || !c->usable || c->cgch || ref->cgch || c->nseg != ref->nseg;
    for (int k = 0; !all && k < c->nseg; k++) all = c->seg_row[k] != ref->seg_row[k];
    if (!all) all = c->cg0[0] != ref->cg0[0] || c->cg0[1] != ref->cg0[1];      /* the backdrop */
    R->all = all;
    if (all) {
        for (int r = 0; r < PPU_H; r++) R->dirty[r] = 1;
        R->ndirty = PPU_H;
        return q->over ? -1 : 0;
    }
    rows_span_cmp(R, c->nsp, c->sp_row, &c->sp_v[0][0], ref->nsp, ref->sp_row, &ref->sp_v[0][0]);
    if (c->nseg) rows_sprites(q, c, ref);
    for (int r = 0; r < PPU_H; r++) R->ndirty += R->dirty[r];
    return 0;
}

void dsp_rows_commit(void)
{
    if (rs[rs_cur].usable) {
        rs[rs_cur].valid = 1;
        rs_cur ^= 1;
    } else {
        rs[0].valid = rs[1].valid = 0;
    }
}

void dsp_rows_forget(void)
{
    rs[0].valid = rs[1].valid = 0;
}

/* A frame mixing Mode 7 and tile rows (dsp_plan's fail 6), cut into runs
 * of one kind from a plan's rowkind: blank rows join the run they follow
 * (at the top, the first run). run[k] = first row, end; returns the number
 * of runs (at most max; 0 when more would be needed). */
int dsp_tc_runs(const uint8_t *rowkind, int (*run)[2], int max)
{
    int n = 0, kind = 0;
    for (int r = 0; r < PPU_H; r++) {
        int k = rowkind[r];
        if (n == 0) {
            run[0][0] = 0;
            run[0][1] = r + 1;
            kind = k;
            n = 1;
            continue;
        }
        if (k == 0 || k == kind || kind == 0) {
            if (kind == 0) kind = k;
            run[n - 1][1] = r + 1;
            continue;
        }
        if (n == max) return 0;
        run[n][0] = r;
        run[n][1] = r + 1;
        kind = k;
        n++;
    }
    return n;
}

