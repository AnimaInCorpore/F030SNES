/* The list compositor's planner: see lc_plan.h. */
#include "lc_plan.h"

/* Checkpoint timing (1/38400 s, Falcon builds only: LC_PROF set by the build): [0] replay, [1] VRAM, [2] layers,
 * [3] sprites, [4] masks; inside the layers [5] moved layers, [6] character/map dirty sets, [7] the cell loop;
 * [8] the map-only path, and the groups sent to the blitter (src/smkx_lc.c adds them); [9] layer scans. */
#ifdef LC_PROF
#include "smkx_prof.h"
uint32_t lc_prof[10];
#define T0() uint32_t t_ = prof_now()
#define TK(i) do { uint32_t n_ = prof_now(); lc_prof[i] += n_ - t_; t_ = n_; } while (0)
#define lc_prof_n(i) (lc_prof[i]++)
#define SPT() uint32_t st_ = prof_now()
#define SPK(i) do { uint32_t n_ = prof_now(); lc_prof[i] += n_ - st_; st_ = n_; } while (0)
#else
#define T0() do {} while (0)
#define TK(i) do {} while (0)
#define lc_prof_n(i) do {} while (0)
#define SPT() do {} while (0)
#define SPK(i) do {} while (0)
#endif

#define N LC_N
#define RH LC_RH
#define NGR LC_NGR
#define WINW LC_WINW

/* The pixels a window hides, per layer (BG1-4, OBJ) of one row: ppu_win_spans' spans {x, length}. */
typedef struct { uint8_t n[5]; uint16_t sp[5][3][2]; } WinRow;
static WinRow winrows[2][PPU_H];        /* per plan, two sets: this frame's and the last one's */
static uint8_t winwl[2][PPU_H];         /* per row, the layers a window masks (bit i); valid when State.wany */
typedef struct {
    uint8_t r[0x34];
    uint16_t bgofs[8], m7ofs[2];
    int16_t m7[6];
    uint8_t fixed[3];
} FixedRow;

typedef struct {
    uint8_t r[0x100];
    uint16_t bgofs[8];
    uint8_t oam[PPU_OAM_BYTES];
    const uint8_t *oam_p;               /* the OAM buffer the first row was composed with */
    int uniform, vis, rows_win, rows_mos, blank;
    int math_bad, cgram_bad;             /* visible rows need colour changes the ring cannot represent */
    int fixed, gfx_bad, cur_kind, color;
    int split;                          /* lc_split_ring: rows differ in mode/layers/vertical origin but the ring still serves the anchor rows */
    uint8_t odd[PPU_H];                 /* split: rows whose configuration is not the anchor's: composed whenever anything changed */
    uint8_t rowkind[PPU_H];
    FixedRow cur_fixed, fixedrows[PPU_H];
    int nofill;                         /* lc_covers: fill_rows leaves the planner's buffers alone */
    int brow;                           /* from this row on forced blank (an IRQ's INIDISP write) shows nothing: PPU_H when never */
    uint8_t varmask;                    /* layers whose horizontal scroll differs between rows (HDMA) */
    uint8_t wb, wany;                   /* the window rows' buffer (winrows[wb], winwl[wb]); some row has a window masking a layer */
    uint8_t cur_wl;                     /* the window state the rows being filled have */
    WinRow cur_win;
    uint16_t cur_h[4];
    int last_row;
    uint16_t rowh[4][PPU_H];            /* those layers' horizontal scroll by row (filled for all four once any varies) */
} State;

typedef struct {
    State st;
    uint8_t vram[65536];
    int valid;                          /* the pages hold frame N-1 (and N-2) */
    int wx, wy;                         /* world origin of the window, unwrapped */
    uint32_t bits[N / 32];              /* the ring's groups the last frame composed (bit i: group i) */
    int nset;                           /* how many */
    uint32_t blank[N / 32];             /* the ring's groups the last picture showed in its blank rows (forced blank from an IRQ) */
    int nblank;                         /* any */
} Prev;

static Prev pv;

/* the non-blank cells of the layers that move in the ring, below */
typedef struct {
    int valid;                          /* rows describe the state of the last plan */
    uint32_t sig[3];                    /* map base and size, character base, bpp */
    uint32_t rows[64][2];
} NbCache;

static NbCache nbc[4];
static uint32_t dw[PPU_H];              /* this frame's groups to compose: a bit a group, by window row */
static uint8_t vdirty[2048];            /* 32-byte VRAM units written since the last frame */

void lc_plan_reset(void)
{
    pv.valid = 0;
    for (int b = 0; b < 4; b++) nbc[b].valid = 0;
}

/* (long words where both are 4-aligned: these run on a 68030) */
static void copy_bytes(void *d, const void *s, unsigned n)
{
    if ((((uintptr_t)d | (uintptr_t)s | n) & 3) == 0) {
        uint32_t *dd = d;
        const uint32_t *ss = s;
        for (n >>= 2; n; n--) *dd++ = *ss++;
        return;
    }
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
}

static int differs(const uint8_t *a, const uint8_t *b, int n)
{
    if ((((uintptr_t)a | (uintptr_t)b | (unsigned)n) & 3) == 0) {
        const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
        for (n >>= 2; n; n--)
            if (*x++ != *y++) return 1;
        return 0;
    }
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 1;
    return 0;
}

/* ---- the frame's register state ----------------------------------------- */

/* Replay captures visible row configurations, including row-0 HDMA. The
 * retained ring handles scroll/windows; mode, layer and vertical-origin
 * changes select a conservative fixed screen plan. */
/* 1: HDMA splits of mode, layers or vertical origin keep the ring: the most common row configuration anchors it, the other
 * rows (the HUD, a changed colour band) are composed whenever anything changed (analysis/list-compositor-sciv.md). */
#ifndef LC_SPLIT_RING
#define LC_SPLIT_RING 0
#endif
int lc_split_ring = LC_SPLIT_RING;
uint32_t lc_fixed_why;                  /* why the last frame took the fixed plan: 1 Mode 7 row, 2 colour math, 3 a register that decides the layout, 4 a vertical scroll */
uint32_t lc_diff;                       /* the first register that made rows differ: reg << 16 | row << 8 | value; 1 << 24: a vertical scroll */

/* The window state of a replay state: the layers masked and their hidden spans. */
static void win_of(const PpuRegs *s, uint8_t *wl, WinRow *wr)
{
    *wl = (uint8_t)ppu_win_layers(s);
    for (int i = 0; i < 5; i++) wr->n[i] = 0;
    if (!*wl) return;
    for (int i = 0; i < 5; i++) {
        for (int k = 0; k < 3; k++) wr->sp[i][k][0] = wr->sp[i][k][1] = 0;
        if ((*wl >> i) & 1) wr->n[i] = (uint8_t)ppu_win_spans(s, i, wr->sp[i]);
    }
}

/* Rows y0..y1-1 take the state being captured: the horizontal scrolls and the windows. */
static void fill_rows(State *c, int y0, int y1)
{
    if (c->nofill) return;
    for (int y = y0; y < y1; y++) {
        c->rowkind[y] = (uint8_t)c->cur_kind;
        copy_bytes(&c->fixedrows[y], &c->cur_fixed, sizeof c->cur_fixed);
        for (int b = 0; b < 4; b++) c->rowh[b][y] = c->cur_h[b];
        winwl[c->wb][y] = c->cur_wl;
        if (c->cur_wl) winrows[c->wb][y] = c->cur_win;
    }
}

/* Exact visible configuration, excluding transfer addresses and write
 * latches. Retaining fixed rows must compare replayed HDMA, not table pointers. */
static void fixed_row(FixedRow *d, const PpuRegs *s)
{
    static const uint8_t regs[] = {0,1,5,6,7,8,9,10,11,12,0x1A,
        0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x33};
    for (unsigned i = 0; i < sizeof regs; i++) d->r[regs[i]] = s->r[regs[i]];
    copy_bytes(d->bgofs, s->bgofs, sizeof d->bgofs);
    copy_bytes(d->m7, s->m7, sizeof d->m7);
    copy_bytes(d->m7ofs, s->m7ofs, sizeof d->m7ofs);
    copy_bytes(d->fixed, s->fixed, sizeof d->fixed);
}

static void cap_fn(void *ctx, int row, const PpuRegs *s)
{
    State *c = ctx;
    int mode = s->r[0x05] & 7;
    if (!(s->r[0x00] & 0x80)) {
        if (mode != 0 && mode != 1 && mode != 7) c->gfx_bad |= 0x20;
        if (mode != 7 && (s->r[0x05] & 0xF0)) c->gfx_bad |= 0x40;
        if ((s->r[0x06] & 0xF0) && (s->r[0x06] & 0x0F & s->r[0x2C])) c->rows_mos = 1;
        if (mode == 7) {
            c->fixed = 1;
            lc_fixed_why = 1;
            if (ppu_win_layers(s) || (s->r[0x33] & 0x40)) c->gfx_bad |= 0x20; /* no M7 windows/EXTBG */
        }
    }
    if (lc_refuse_math && !(s->r[0x00] & 0x80)) {
        if (((s->r[0x30] >> 4) & 3) != 3 && (s->r[0x31] & 0x3F)) {
            uint32_t sub = ppu_sub_cfg(s, s->cgram);
            if (sub && !(sub & 0x40000)) { if (lc_split_ring) c->split = 1; else { c->fixed = 1; lc_fixed_why = 2; } c->color = 1; }
            else c->math_bad = 1;
        }
        if (s->cgwrites) c->cgram_bad = 1;
    }
    if (c->vis++) {
        /* an IRQ that forces blank (CT's, at the bottom of the field screen, with its VRAM writes) ends the picture: nothing
         * from this row on is visible, so no register matters any more, unless the display comes back on */
        if (c->brow < PPU_H) {
            if (!(s->r[0x00] & 0x80)) { c->uniform = 0; lc_diff = (uint32_t)row << 8; }
            return;
        }
        if ((s->r[0x00] & 0x80) && !(c->r[0x00] & 0x80)) {
            fill_rows(c, c->last_row, row);
            c->last_row = row;
            c->brow = row;
            return;
        }
        /* later rows: only the horizontal scroll of a layer may differ (an HDMA channel on $210D-$2114's H registers), the
         * window registers ($2123-$212B, $212E, $212F: their effect is in the window rows) and the registers that hold no
         * picture state (the CGRAM and scroll latches); anything else makes the rows differ */
        for (int i = 0; i < 0x100; i++) {
            if ((i >= 0x0D && i <= 0x14) || i == 0x21 || i == 0x22 || (i >= 0x23 && i <= 0x2B) || i == 0x2E || i == 0x2F) continue;
            if (i == 0x05 || (i >= 0x07 && i <= 0x0C) || (i >= 0x1A && i <= 0x20) || i == 0x2C || i == 0x30 || i == 0x31) {
                if (s->r[i] != c->r[i]) { if (lc_split_ring) c->split = 1; else { c->fixed = 1; lc_fixed_why = 3; } }
                continue;
            }
            if (s->r[i] != c->r[i]) { c->uniform = 0; lc_diff = (uint32_t)i << 16 | (uint32_t)row << 8 | s->r[i]; return; }
        }
        for (int b = 0; b < 4; b++)
            if (s->bgofs[b * 2 + 1] != c->bgofs[b * 2 + 1]) { if (lc_split_ring) c->split = 1; else { c->fixed = 1; lc_fixed_why = 4; } }
        fill_rows(c, c->last_row, row);
        c->last_row = row;
        c->cur_kind = mode == 7 ? 2 : 1;
        fixed_row(&c->cur_fixed, s);
        for (int b = 0; b < 4; b++) {
            if (s->bgofs[b * 2] != c->cur_h[b]) c->varmask |= (uint8_t)(1u << b);
            c->cur_h[b] = s->bgofs[b * 2];
        }
        win_of(s, &c->cur_wl, &c->cur_win);
        if (c->cur_wl) c->wany = 1;
        return;
    }
    copy_bytes(c->r, s->r, sizeof c->r);
    copy_bytes(c->bgofs, s->bgofs, sizeof c->bgofs);
    copy_bytes(c->oam, s->oam, sizeof c->oam);
    c->oam_p = s->oam;
    for (int b = 0; b < 4; b++) c->cur_h[b] = s->bgofs[b * 2];
    c->last_row = 0;
    c->cur_kind = (s->r[0x00] & 0x80) ? 0 : mode == 7 ? 2 : 1;
    fixed_row(&c->cur_fixed, s);
    if (s->r[0x00] & 0x80) c->blank++;
    win_of(s, &c->cur_wl, &c->cur_win);
    if (c->cur_wl) c->wany = 1;
    if ((s->r[0x06] & 0xF0) && (s->r[0x06] & 0x0F & s->r[0x2C])) c->rows_mos++;
}

/* After the replay: the rest of the rows. */
static void cap_done(State *c)
{
    fill_rows(c, c->last_row, c->brow);
    c->cur_kind = 0;
    fill_rows(c, c->brow, PPU_H);
}

/* ---- layers: where a cell's map word and tile live ---------------------- */

static int layer_vis(const State *st, int b) { return (st->r[0x2C] >> b) & 1; }

static int bpp_of(int mode, int b) { return mode == 0 ? 2 : (b == 2 ? 2 : 4); }

static uint32_t char_base(const State *st, int b)
{
    int nba = st->r[0x0B + (b >> 1)];
    return (uint32_t)((b & 1) ? nba >> 4 : nba & 15) << 12;
}

static int tile_words_nonblank(const uint8_t *vram, uint32_t a, int words)
{
    /* a character's words are 4-aligned and lie in one piece (8 or 16 words, at a multiple of their size) */
    const uint32_t *p = (const uint32_t *)(const void *)(vram + (a & 0x7FFF) * 2);
    for (int i = 0; i < words >> 1; i++)
        if (p[i]) return 1;
    return 0;
}

static int popcount17(uint32_t v)
{
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

/* ---- ring geometry ------------------------------------------------------ */

static int lin(int Y, int X) { return ((Y & (RH - 1)) * NGR + X) & (N - 1); }

/* The current window: world origin, its first group x0 and the groups a row shows. */
static int cur_wy, cur_x0, cur_jmax;

/* World group (Y, X) to the window's row and group, or 0 when it is not in the window. */
static int mark(int Y, int X)
{
    int y = (Y - cur_wy) & (RH - 1), j = X - cur_x0;
    if (y >= PPU_H || j < 0 || j >= cur_jmax) return 0;
    dw[y] |= 1u << j;
    return 1;
}

typedef struct { int x, y, w, h; uint16_t tile; uint8_t idx; } Spr;

static int parse_oam(const uint8_t *oam, uint8_t obsel, Spr *out)
{
    static const uint8_t sz[8][2][2] = {
        {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
        {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
    };
    int n = 0;
    for (int i = 0; i < 128; i++) {
        const uint8_t *e = oam + i * 4;
        int hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        Spr sp;
        sp.w = sz[obsel >> 5][hi >> 1][0];
        sp.h = sz[obsel >> 5][hi >> 1][1];
        sp.x = e[0] | ((hi & 1) << 8);
        if (sp.x >= 256 + 16) sp.x -= 512;                /* x 256-271: in the window's extra group (the DSP draws them there) */
        sp.y = e[1];
        sp.tile = (uint16_t)(e[2] | ((e[3] & 1) << 8));
        sp.idx = (uint8_t)i;
        int rows = 0;
        for (int k = 0; k < sp.h; k++) rows |= ((sp.y + k) & 255) < PPU_H;
        if (!rows || sp.x <= -sp.w || sp.x >= 256 + 16) continue;
        out[n++] = sp;
    }
    return n;
}

static void footprint(const Spr *sp, int wx, int wy)
{
    for (int k = 0; k < sp->h; k++) {
        int sy = (sp->y + k) & 255;
        if (sy >= PPU_H) continue;
        for (int X = (wx + sp->x) >> 4; X <= (wx + sp->x + sp->w - 1) >> 4; X++) mark(wy + sy, X);
    }
}

static int sprite_tile_dirty(const Spr *sp, uint8_t obsel, uint8_t attr)
{
    uint32_t base = (uint32_t)(obsel & 7) << 13, gap = (uint32_t)(((obsel >> 3) & 3) + 1) << 12;
    int tw = sp->w / 8, th = sp->h / 8;
    for (int i = 0; i < th; i++)
        for (int j = 0; j < tw; j++) {
            int col = ((sp->tile & 15) + j) & 15, row = (((sp->tile >> 4) & 15) + i) & 15;
            uint32_t a = base + (uint32_t)((row << 4 | col)) * 16 + ((attr & 1) ? gap : 0);
            if (vdirty[(a >> 4) & 0x7FF]) return 1;
        }
    return 0;
}

static int oam_entry_changed(const State *a, const State *b, int oi)
{
    return differs(a->oam + oi * 4, b->oam + oi * 4, 4) ||
           (((a->oam[512 + (oi >> 2)] ^ b->oam[512 + (oi >> 2)]) >> ((oi & 3) * 2)) & 3);
}

/* One layer's cells against the written characters and map words. chardirty: per character of the layer, a
 * unit of its data was written; blank[]: 0 unknown, 1 no pixel in the character, 2 some pixel. */
static uint8_t chardirty[1024], blank[1024];
static uint16_t dlist[2048];            /* the dirty 32-byte units, in order */
static int ndl;

static int char_nonblank(const uint8_t *vr, uint32_t base, int c, int bpp)
{
    if (!blank[c]) blank[c] = tile_words_nonblank(vr, base + (uint32_t)c * (uint32_t)(bpp * 4), bpp * 4) ? 2 : 1;
    return blank[c] == 2;
}

static void clear4(void *d, unsigned n)
{
    uint32_t *p = d;
    for (n >>= 2; n; n--) *p++ = 0;
}

/* The groups (window bits) the cell column cx of a layer at scroll lsx covers, in a world whose window is at ox. */
static uint32_t cell_mask(int cx, int lsx, int ox)
{
    int wxp = ox + cx * 8 - (lsx & 7);
    int lo = (wxp >> 4) - cur_x0, hi = ((wxp + 7) >> 4) - cur_x0;
    if (lo < 0) lo = 0;
    if (hi >= cur_jmax) hi = cur_jmax - 1;
    if (lo > hi) return 0;
    return ((2u << hi) - 1) & ~((1u << lo) - 1);
}

/* The 8 lines of cell row cy: the groups m marked on each of them (a world whose window top is oy). */
static void strip(int cy, int lsy, int oy, uint32_t m)
{
    if (!m) return;
    int screen_y = cy * 8 - (lsy & 7) - 1;           /* BG line: scroll + y + 1 */
    for (int k = 0; k < 8; k++) {
        int yy = screen_y + k;
        if (yy < 0 || yy >= PPU_H) continue;
        int y = (oy + yy - cur_wy) & (RH - 1);
        if (y < PPU_H) dw[y] |= m;
    }
}

/* One layer's cells against the written characters and map words. chardirty: per character of the layer, a
 * unit of its data was written; blank[]: 0 unknown, 1 no pixel in the character, 2 some pixel. */
static void scan_layer(const State *ss, const uint8_t *vr, int b, int bpp, int ox, int oy)
{
    lc_prof_n(9);
    uint32_t cbase = char_base(ss, b);
    int lsx = ss->bgofs[b * 2] & 1023, lsy = ss->bgofs[b * 2 + 1] & 1023;
    int tx0 = lsx >> 3, ty0 = lsy >> 3;
    int sc = ss->r[0x07 + b], sz = sc & 3;
    uint32_t mbase = (uint32_t)(sc & 0xFC) << 8;
    SPT();
    clear4(blank, sizeof blank);
    /* the characters whose data units were written, and whether the layer's map was */
    int tw = bpp * 4, nch = 0, mapdirty = 0, nblk = sz == 0 ? 1 : sz == 3 ? 4 : 2;
    clear4(chardirty, sizeof chardirty);
    for (int i = 0; i < ndl; i++) {
        uint32_t w = (uint32_t)dlist[i] * 16;
        uint32_t o = (w - cbase) & 0x7FFF;
        if (o < (uint32_t)(1024 * tw)) {
            int c1 = (int)((o + 15) / (uint32_t)tw);
            if (c1 > 1023) c1 = 1023;
            for (int c = (int)(o / (uint32_t)tw); c <= c1; c++) chardirty[c] = 1;
            nch++;
        }
        if (((w - mbase) & 0x7FFF) < (uint32_t)(nblk * 0x400)) mapdirty = 1;
    }
    SPK(6);
    if (nch) {
        for (int cy = 0; cy <= 28; cy++) {
            uint32_t m = 0;
            int ty = (ty0 + cy) & 63;
            uint32_t rb = mbase + (uint32_t)(ty & 31) * 32 + (ty >= 32 ? (sz == 2 ? 0x400u : sz == 3 ? 0x800u : 0) : 0);
            for (int cx = -2; cx <= 34; cx++) {
                int tx = (tx0 + cx) & 63;
                uint32_t a = (rb + (uint32_t)(tx & 31) + ((sz & 1) && tx >= 32 ? 0x400u : 0)) & 0x7FFF;
                if (!(chardirty[(vr[a * 2] | (vr[a * 2 + 1] << 8)) & 0x3FF] | vdirty[(a >> 4) & 0x7FF])) continue;
                m |= cell_mask(cx, lsx, ox);
            }
            strip(cy, lsy, oy, m);
        }
        SPK(7);
        return;
    }
    if (!mapdirty) return;
    /* only map words were written: the cells those words are */
    for (int i = 0; i < ndl; i++) {
        uint32_t w0 = (uint32_t)dlist[i] * 16;
        if (((w0 - mbase) & 0x7FFF) >= (uint32_t)(nblk * 0x400)) continue;
        for (int k = 0; k < 16; k++) {
            uint32_t rel = (w0 + (uint32_t)k - mbase) & 0x7FFF;
            int blk = (int)(rel >> 10), tin = (int)((rel >> 5) & 31), sin_ = (int)(rel & 31);
            int bx = sz & 1 ? blk & 1 : -1, by = sz == 3 ? blk >> 1 : sz == 2 ? blk : -1;
            for (int qy = 0; qy < 2; qy++) {
                if (by >= 0 && qy != by) continue;
                int dy = ((tin + 32 * qy) - ty0) & 63;
                if (dy > 28) continue;
                for (int qx = 0; qx < 2; qx++) {
                    if (bx >= 0 && qx != bx) continue;
                    int dx = ((sin_ + 32 * qx) - tx0) & 63;
                    int cx = dx >= 62 ? dx - 64 : dx;
                    if (cx > 34) continue;
                    strip(dy, lsy, oy, cell_mask(cx, lsx, ox));
                }
            }
        }
    }
    SPK(8);
}

/* A layer other than the ring's moves in the ring when its scroll relative to the ring's changed; a layer whose horizontal
 * scroll differs between rows (this frame's or the last one's) when any row's did. */
static int layer_moved(const State *cs, int b, int ring, int dx, int dy)
{
    if (b == ring) return 0;
    int vn = (cs->varmask >> b) & 1, vp = (pv.st.varmask >> b) & 1;
    if (((cs->bgofs[b * 2 + 1] - pv.st.bgofs[b * 2 + 1] - dy) & 1023)) return 1;
    if (!vn && !vp) return ((cs->bgofs[b * 2] - pv.st.bgofs[b * 2] - dx) & 1023) != 0;
    for (int y = 0; y < PPU_H; y++) {
        int a = vn ? cs->rowh[b][y] : cs->bgofs[b * 2], p = vp ? pv.st.rowh[b][y] : pv.st.bgofs[b * 2];
        if ((a - p - dx) & 1023) return 1;
    }
    return 0;
}

/* Whether a layer other than the ring's has moved in the ring since the last plan (ring scroll unchanged here). */
static int pv_moved(const State *cs, int mode, int ring)
{
    for (int b = 0; b < 4; b++) {
        if (b == ring || !layer_vis(cs, b) || (b == 3 && mode == 1)) continue;
        if (layer_moved(cs, b, ring, 0, 0)) return 1;
    }
    return 0;
}

/* The pixels of a row's 16 at screen x xs..xs+15 that a window hides on a layer (bit k: pixel xs + k). */
static uint16_t hid16(const WinRow *wr, int layer, int xs)
{
    uint32_t m = 0;
    for (int k = 0; k < wr->n[layer]; k++) {
        int a = wr->sp[layer][k][0], e = a + wr->sp[layer][k][1];
        int lo = a > xs ? a : xs, hi = e < xs + 16 ? e : xs + 16;
        if (lo < hi) m |= (((1u << (hi - lo)) - 1) << (lo - xs));
    }
    return (uint16_t)m;
}

/* Windows move the picture where the pixels a window hides differ between the last frame (at the same world place) and this
 * one: per row and layer, group by group (the window's edges are screen positions, the ring is the world's). Content that
 * changes under a window is marked by the layer scans as before (more than it shows, never less). */
static void win_dirty(const State *cs, int dx, int wx, int wy)
{
    int dl = wx & 15;
    for (int y = 0; y < PPU_H; y++) {
        int yo = (wy + y - pv.wy) & (RH - 1);
        if (yo >= PPU_H) continue;                      /* a row the last window did not show: exposed, composed anyway */
        uint8_t wn = cs->wany ? winwl[cs->wb][y] : 0, wo = pv.st.wany ? winwl[pv.st.wb][yo] : 0;
        if (!(wn | wo)) continue;
        const WinRow *rn = &winrows[cs->wb][y], *ro = &winrows[pv.st.wb][yo];
        if (!dx && wn == wo && !differs((const uint8_t *)rn, (const uint8_t *)ro, sizeof(WinRow))) continue;
        for (int i = 0; i < 5; i++) {
            if (!(((wn | wo) >> i) & 1)) continue;
            for (int j = 0; j < cur_jmax; j++) {
                uint16_t hn = (wn >> i) & 1 ? hid16(rn, i, 16 * j - dl) : 0;
                uint16_t ho = (wo >> i) & 1 ? hid16(ro, i, 16 * j - dl + dx) : 0;
                if (hn != ho) dw[y] |= 1u << j;
            }
        }
    }
}

/* ---- layers that move in the ring ------------------------------------------ */

/* A layer that moves relative to the ring shows all of its non-blank cells at its old and its new place. Its non-blank
 * cells, a row of the 64 x 64 cell map a pair of words (bit tx), are kept from one frame to the next: the walk over
 * the window is then a few bit operations a cell row, and the map and characters are read again only when a unit of
 * them was written. */
static uint32_t nb_new[64][2], nb_old[64][2];

static void nb_sig(const State *ss, int b, int bpp, uint32_t *sig)
{
    sig[0] = ((uint32_t)(ss->r[0x07 + b] & 0xFC) << 8) | (uint32_t)(ss->r[0x07 + b] & 3);
    sig[1] = char_base(ss, b);
    sig[2] = (uint32_t)bpp;
}

/* Whether a written unit lies in the layer's map or its characters. */
static int layer_dirty(const uint32_t *sig)
{
    uint32_t mbase = sig[0] & 0xFC00, sz = sig[0] & 3, cbase = sig[1];
    uint32_t nblk = sz == 0 ? 1 : sz == 3 ? 4 : 2, tw = sig[2] * 4;
    for (int i = 0; i < ndl; i++) {
        uint32_t w = (uint32_t)dlist[i] * 16;
        if (((w - mbase) & 0x7FFF) < nblk * 0x400 || ((w - cbase) & 0x7FFF) < 1024 * tw) return 1;
    }
    return 0;
}

static void nb_build(const uint32_t *sig, const uint8_t *vr, uint32_t (*rows)[2])
{
    uint32_t mbase = sig[0] & 0xFC00, sz = sig[0] & 3, cbase = sig[1];
    int bpp = (int)sig[2];
    uint32_t blkbits[4][32];
    int have[4] = {0, 0, 0, 0};
    clear4(blank, sizeof blank);
    for (int qy = 0; qy < 2; qy++)
        for (int qx = 0; qx < 2; qx++) {
            int blk = sz == 0 ? 0 : sz == 1 ? qx : sz == 2 ? qy : qx + 2 * qy;
            if (!have[blk]) {
                have[blk] = 1;
                for (int r = 0; r < 32; r++) {
                    uint32_t bits = 0, base = mbase + (uint32_t)blk * 0x400 + (uint32_t)r * 32;
                    for (int t = 0; t < 32; t++) {
                        uint32_t a = (base + (uint32_t)t) & 0x7FFF;
                        int c = (vr[a * 2] | (vr[a * 2 + 1] << 8)) & 0x3FF;
                        if (blank[c] == 2 || (!blank[c] && char_nonblank(vr, cbase, c, bpp))) bits |= 1u << t;
                    }
                    blkbits[blk][r] = bits;
                }
            }
            for (int r = 0; r < 32; r++) rows[qy * 32 + r][qx] = blkbits[blk][r];
        }
}

/* One placement of a moved layer: the window groups its non-blank cells cover. */
static void nb_mark(uint32_t (*rows)[2], int lsx, int lsy, int ox, int oy)
{
    uint32_t cm[37];
    for (int c = 0; c < 37; c++) cm[c] = cell_mask(c - 2, lsx, ox);
    int tx0 = lsx >> 3, ty0 = lsy >> 3, sh = (tx0 - 2) & 63;
    for (int cy = 0; cy <= 28; cy++) {
        int ty = (ty0 + cy) & 63;
        uint64_t R = ((uint64_t)rows[ty][1] << 32) | rows[ty][0];
        if (sh) R = (R >> sh) | (R << (64 - sh));
        uint32_t lo = (uint32_t)R, hi = (uint32_t)(R >> 32) & 31, m = 0;
        for (int c = 0; lo; c++, lo >>= 1)
            if (lo & 1) m |= cm[c];
        for (int c = 32; hi; c++, hi >>= 1)
            if (hi & 1) m |= cm[c];
        strip(cy, lsy, oy, m);
    }
}

/* The same for a layer whose horizontal scroll differs between rows: line by line. */
static void nb_mark_rows(uint32_t (*rows)[2], const uint16_t *rowx, int lsy, int ox, int oy)
{
    static uint32_t cmt[8][37];
    int have = 0;
    for (int yy = 0; yy < PPU_H; yy++) {
        int y = (oy + yy - cur_wy) & (RH - 1);
        if (y >= PPU_H) continue;
        int sxr = rowx[yy] & 1023, ph = sxr & 7;
        if (!((have >> ph) & 1)) {
            for (int c = 0; c < 37; c++) cmt[ph][c] = cell_mask(c - 2, sxr, ox);
            have |= 1 << ph;
        }
        int ty = ((lsy + yy + 1) >> 3) & 63, sh = ((sxr >> 3) - 2) & 63;
        uint64_t R = ((uint64_t)rows[ty][1] << 32) | rows[ty][0];
        if (sh) R = (R >> sh) | (R << (64 - sh));
        uint32_t lo = (uint32_t)R, hi = (uint32_t)(R >> 32) & 31, m = 0;
        for (int c = 0; lo; c++, lo >>= 1)
            if (lo & 1) m |= cmt[ph][c];
        for (int c = 32; hi; c++, hi >>= 1)
            if (hi & 1) m |= cmt[ph][c];
        dw[y] |= m;
    }
}

static void moved_layer(const State *cs, const uint8_t *cvr, int b, int bpp, int wx, int wy)
{
    uint32_t sig[3], psig[3];
    nb_sig(cs, b, bpp, sig);
    nb_sig(&pv.st, b, bpp, psig);
    NbCache *c = &nbc[b];
    uint32_t (*old)[2] = c->rows;
    if (!c->valid || c->sig[0] != psig[0] || c->sig[1] != psig[1] || c->sig[2] != psig[2]) {
        nb_build(psig, pv.vram, nb_old);
        old = nb_old;
    }
    uint32_t (*nw)[2] = old;
    if (sig[0] != psig[0] || sig[1] != psig[1] || sig[2] != psig[2] || layer_dirty(sig)) {
        nb_build(sig, cvr, nb_new);
        nw = nb_new;
    }
    int lsx = cs->bgofs[b * 2] & 1023, lsy = cs->bgofs[b * 2 + 1] & 1023;
    int plsx = pv.st.bgofs[b * 2] & 1023, plsy = pv.st.bgofs[b * 2 + 1] & 1023;
    if ((cs->varmask >> b) & 1) nb_mark_rows(nw, cs->rowh[b], lsy, wx, wy);
    else nb_mark(nw, lsx, lsy, wx, wy);
    if ((pv.st.varmask >> b) & 1) nb_mark_rows(old, pv.st.rowh[b], plsy, pv.wx, pv.wy);
    else nb_mark(old, plsx, plsy, pv.wx, pv.wy);
    if (nw != c->rows) copy_bytes(c->rows, nw, sizeof c->rows);
    c->valid = 1;
    for (int i = 0; i < 3; i++) c->sig[i] = sig[i];
}

/* ---- the plan ------------------------------------------------------------ */

/* The registers a replay's State depends on: not the ports and addresses the game writes every frame (OAM data, VRAM
 * and CGRAM addresses and data, the APU ports, the read-only results). */
static int reg_key_same(const uint8_t *a, const uint8_t *b)
{
    static const uint8_t range[][2] = {{0x00, 0x04}, {0x05, 0x15}, {0x1A, 0x21}, {0x23, 0x34}, {0xF0, 0xF4}};
    for (unsigned r = 0; r < sizeof range / sizeof range[0]; r++)
        for (int i = range[r][0]; i < range[r][1]; i++)
            if (a[i] != b[i]) return 0;
    return 1;
}

static int ring_varies(const State *cs)
{
    int ring = layer_vis(cs, 0) ? 0 : layer_vis(cs, 1) ? 1 : layer_vis(cs, 2) ? 2 : -1;
    return ring >= 0 && ((cs->varmask >> ring) & 1);
}

/* Split frames (lc_split_ring): one configuration anchors the ring. The anchor is the most common visible row configuration
 * (registers, vertical scrolls, and the horizontal scroll of the layer the ring follows); the State then holds it, so the ring
 * logic below sees an ordinary frame, and the rows that differ are marked odd. Idempotent (the State may be reused). */
static const uint8_t anchor_regs[] = {0,1,5,6,7,8,9,10,11,12,0x1A,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x33};
static int ring_of(uint8_t tm) { return tm & 1 ? 0 : tm & 2 ? 1 : tm & 4 ? 2 : -1; }
static int row_eq(const FixedRow *a, const FixedRow *b, int ring)
{
    for (unsigned i = 0; i < sizeof anchor_regs; i++)
        if (a->r[anchor_regs[i]] != b->r[anchor_regs[i]]) return 0;
    for (int l = 0; l < 4; l++)
        if (a->bgofs[l * 2 + 1] != b->bgofs[l * 2 + 1]) return 0;
    if (ring >= 0 && a->bgofs[ring * 2] != b->bgofs[ring * 2]) return 0;
    return 1;
}
static void adopt_anchor(State *c)
{
    if (!c->split || c->fixed) return;
    int best = -1, bestn = 0;
    for (int y = 0; y < PPU_H; y++) {
        if (c->rowkind[y] != 1) continue;
        int ring = ring_of(c->fixedrows[y].r[0x2C]), n = 0;
        for (int z = 0; z < PPU_H; z++)
            if (c->rowkind[z] == 1 && row_eq(&c->fixedrows[y], &c->fixedrows[z], ring)) n++;
        if (n > bestn) { bestn = n; best = y; }
    }
    if (best < 0) return;
    const FixedRow *an = &c->fixedrows[best];
    int ring = ring_of(an->r[0x2C]);
    for (unsigned i = 0; i < sizeof anchor_regs; i++) c->r[anchor_regs[i]] = an->r[anchor_regs[i]];
    copy_bytes(c->bgofs, an->bgofs, sizeof c->bgofs);
    c->varmask = 0;
    int first = -1;
    for (int y = 0; y < PPU_H; y++) {
        c->odd[y] = c->rowkind[y] == 1 && !row_eq(&c->fixedrows[y], an, ring);
        if (c->rowkind[y] != 1 || c->odd[y]) continue;
        if (first < 0) first = y;
        for (int b = 0; b < 4; b++)
            if (c->rowh[b][y] != c->rowh[b][first]) c->varmask |= (uint8_t)(1u << b);
    }
}

uint32_t lc_why;                        /* why the last frame was not covered: bit 0 no frame, 1 per-line changes, 2 (windows: covered now),
                                         * 3 mosaic, 4 forced blank, 5 mode, 6 16x16 tiles, 7 OAM segments, 8 the ring layer's
                                         * scroll differs between rows, 9 colour math, 10 mid-frame CGRAM (lc_refuse_math) */

/* OAM segments (the OAM an IRQ left, from its line on) the picture shows: one at line 0, and none later than the blank row
 * (a segment at line L acts from row L - 1; nothing is visible from the blank row on). */
static int segs_ok(const State *st, const PpuInput *in)
{
    for (int k = 0; k < in->noamseg; k++) {
        int line = in->oamseg[k].line;
        if (!(k == 0 && line == 0) && line - 1 < st->brow) return 0;
    }
    return 1;
}

/* Colour math is left out of the compositor's pictures (the DSP draws indices with the palette of the frame start): a
 * frame the old paths would draw with it shows without. lc_refuse_math (LC_REFUSE_MATH=1 at build time, SCIVX_LC_MATH=1)
 * sends such frames there: CGADSUB names a layer and CGWSEL does not say "never". */
#ifndef LC_REFUSE_MATH
#define LC_REFUSE_MATH 0
#endif
int lc_refuse_math = LC_REFUSE_MATH;

static uint32_t why_bits(const State *st, const PpuInput *in)
{
    return st->cgram_bad << 10 | st->math_bad << 9 | (st->vis < 1) | (!st->uniform) << 1 | (st->rows_mos != 0) << 3 | (st->blank != 0) << 4 |
           st->gfx_bad | (!segs_ok(st, in)) << 7 | (!st->fixed && ring_varies(st)) << 8;
}

/* With exact colour required, inspect HDMA/IRQ colour writes as well as the
 * frame-start registers. Suppressing them here hid mid-frame effects. */
static void capture(const PpuInput *in, State *st)
{
    int nocg = ppu_replay_nocg, walk = ppu_walk_all;
    ppu_replay_nocg = !lc_refuse_math;
    if (lc_refuse_math) ppu_walk_all = 1;
    ppu_replay_changes(in, cap_fn, st);
    ppu_replay_nocg = nocg;
    ppu_walk_all = walk;
    cap_done(st);
    adopt_anchor(st);
}

/* Whether lc_plan would cover this frame (no state of the planner is touched): the runtime asks before it boots the DSP
 * into the compositor's program, which drops the page cache of the others. */
int lc_covers(const PpuInput *in)
{
    static State cs;
    clear4(&cs, (sizeof cs + 3) & ~3u);
    cs.uniform = 1;
    cs.brow = PPU_H;
    cs.nofill = 1;                      /* the window and scroll rows are the planner's: this call only looks at the registers */
    capture(in, &cs);
    return why_bits(&cs, in) == 0;
}

static void fixed_bg_dirty(const PpuInput *in, const State *st)
{
    int plane_dirty = 0;
    for (int u = 0; u < 1024; u++) plane_dirty |= vdirty[u];
    for (int y = 0; y < PPU_H; y++) {
        if (!st->rowkind[y] || dw[y] == 0xFFFF) continue;
        if (st->rowkind[y] == 2) { if (plane_dirty) dw[y] = 0xFFFF; continue; }
        const FixedRow *r = &st->fixedrows[y];
        int mode = r->r[5] & 7;
        int layers = r->r[0x2C] | (st->color ? r->r[0x2D] : 0);
        for (int b = 0; b < (mode ? 3 : 4); b++) {
            if (!(layers >> b & 1)) continue;
            int bpp = bpp_of(mode, b), sc = r->r[7 + b];
            int h = r->bgofs[b * 2] & 1023;
            int py = (y + 1 + r->bgofs[b * 2 + 1]) & ((sc & 2) ? 511 : 255);
            uint32_t chrbase = (uint32_t)((r->r[0x0B + (b >> 1)] >> ((b & 1) * 4)) & 15) << 12;
            for (int x = -(h & 7); x < PPU_W; x += 8) {
                int tx = ((x + h) >> 3) & ((sc & 1) ? 63 : 31), ty = py >> 3;
                uint32_t a = (((uint32_t)(sc & 0xFC) << 8) + (uint32_t)(ty & 31) * 32 + (uint32_t)(tx & 31) +
                    (tx >= 32 ? 0x400u : 0) + (ty >= 32 ? (sc & 1 ? 0x800u : 0x400u) : 0)) & 0x7FFF;
                int dirty = vdirty[a >> 4];
                uint16_t tile = (uint16_t)(in->vram[a * 2] | in->vram[a * 2 + 1] << 8);
                int cy = tile & 0x8000 ? 7 - (py & 7) : py & 7;
                uint32_t ca = chrbase + (uint32_t)(tile & 1023) * (uint32_t)(bpp * 4) + (uint32_t)cy;
                for (int pair = 0; pair < bpp / 2; pair++) dirty |= vdirty[((ca + (uint32_t)pair * 8) & 0x7FFF) >> 4];
                if (dirty) {
                    int lo = x < 0 ? 0 : x >> 4, hi = (x + 7) >> 4;
                    if (hi > 15) hi = 15;
                    for (int j = lo; j <= hi; j++) dw[y] |= 1u << j;
                }
            }
        }
    }
}

static void fixed_plan(const PpuInput *in, const State *st, LcPlan *plan, const uint8_t *vpages)
{
    int cold = !pv.valid || !pv.st.fixed, vrchanged = 0;
    clear4(vdirty, sizeof vdirty);
    for (int pg = 0; pg < 128; pg++) {
        if (!cold && vpages && !vpages[pg]) continue;
        for (int u = pg * 16; u < pg * 16 + 16; u++) {
            vdirty[u] = (uint8_t)(cold || differs(pv.vram + u * 32, in->vram + u * 32, 32));
            vrchanged |= vdirty[u];
        }
        copy_bytes(pv.vram + pg * 512, in->vram + pg * 512, 512);
    }
    cur_wy = cur_x0 = 0;
    cur_jmax = 16;
    for (int y = 0; y < PPU_H; y++) {
        dw[y] = cold || st->rowkind[y] != pv.st.rowkind[y] ||
                differs((const uint8_t *)&st->fixedrows[y], (const uint8_t *)&pv.st.fixedrows[y], sizeof(FixedRow)) ? 0xFFFF : 0;
    }
    if (!cold) {
        if (vrchanged) fixed_bg_dirty(in, st);
        Spr cur[128], old[128];
        int nc = parse_oam(st->oam, st->r[1], cur), no = parse_oam(pv.st.oam, pv.st.r[1], old);
        for (int i = 0; i < nc; i++) if (oam_entry_changed(st, &pv.st, cur[i].idx) ||
            (vrchanged && sprite_tile_dirty(&cur[i], st->r[1], st->oam[cur[i].idx * 4 + 3]))) footprint(&cur[i], 0, 0);
        for (int i = 0; i < no; i++) if (oam_entry_changed(st, &pv.st, old[i].idx)) footprint(&old[i], 0, 0);
    }
    plan->S = plan->dl = plan->wx = plan->wy = 0;
    plan->cold = cold;
    plan->ngroups = plan->ncopies = 0;
    for (int y = 0; y < PPU_H; y++) {
        uint32_t m = dw[y] & 0xFFFF, cp = cold ? 0 : pv.bits[y] & ~m;
        plan->rowkind[y] = st->rowkind[y];
        plan->maskA[y] = (uint16_t)m;
        plan->maskB[y] = 0;
        plan->cmask[y] = cp;
        plan->ngroups += popcount17(m);
        for (int j = 0; j < 16; j++) if (cp >> j & 1) plan->copies[plan->ncopies++] = (uint16_t)(y * NGR + j);
    }
    clear4(pv.bits, sizeof pv.bits);
    for (int y = 0; y < PPU_H; y++) pv.bits[y] = dw[y] & 0xFFFF;
    pv.nset = plan->ngroups;
    pv.nblank = 0;
    clear4(pv.blank, sizeof pv.blank);
    pv.wx = pv.wy = 0;
    copy_bytes(&pv.st, st, sizeof *st);
    pv.valid = 1;
    for (int b = 0; b < 4; b++) nbc[b].valid = 0;
}

int lc_plan(const PpuInput *in, LcPlan *plan, const uint8_t *vpages)
{
    static State st;
    T0();
    /* A replay that read only ROM is the same for the same registers, scroll and IRQ log: the last one's State stands
     * (only the OAM is the frame's own). The key is what the replay reads besides tables and VRAM. */
    static struct {
        int valid, nlog, refuse_math;
        uint8_t io[0x100], hdma[0x80], en[4];
        uint16_t bgofs[8];
        uint16_t m7[6], m7ofs[2];
        PpuWrite log[256];
    } key;
    int same = key.valid && key.refuse_math == lc_refuse_math && key.nlog == in->nlog && in->nlog <= 256 &&
               reg_key_same(key.io, in->io + 0x100) && !differs(key.hdma, in->io + 0x2300, 0x80) &&
               !differs(key.en, in->io + 0x220C, 4) && !differs((const uint8_t *)key.bgofs, (const uint8_t *)in->bgofs, 16) &&
               !differs((const uint8_t *)key.m7, (const uint8_t *)in->m7, 12) &&
               !differs((const uint8_t *)key.m7ofs, (const uint8_t *)in->m7ofs, 4) &&
               !differs((const uint8_t *)key.log, (const uint8_t *)in->log, (int)sizeof(PpuWrite) * in->nlog);
    if (same && !st.wany) {
        /* only the OAM is the frame's own: the one the first row is composed with (an IRQ's segments act later) */
        const uint8_t *oam = in->noamseg && in->oamseg[0].line == 0 ? in->oamseg[0].oam : in->oam;
        copy_bytes(st.oam, oam, sizeof st.oam);
        st.oam_p = oam;
    } else {
        clear4(&st, (sizeof st + 3) & ~3u);
        st.uniform = 1;
        st.brow = PPU_H;
        st.wb = pv.valid ? (uint8_t)(pv.st.wb ^ 1) : 0;
        lc_fixed_why = 0;
        capture(in, &st);
        key.valid = 0;
        if (!ppu_replay_ram && in->nlog <= 256) {
            key.valid = 1;
            key.refuse_math = lc_refuse_math;
            key.nlog = in->nlog;
            copy_bytes(key.io, in->io + 0x100, 0x100);
            copy_bytes(key.hdma, in->io + 0x2300, 0x80);
            copy_bytes(key.en, in->io + 0x220C, 4);
            copy_bytes(key.bgofs, in->bgofs, 16);
            copy_bytes(key.m7, in->m7, 12);
            copy_bytes(key.m7ofs, in->m7ofs, 4);
            copy_bytes(key.log, in->log, (unsigned)sizeof(PpuWrite) * (unsigned)in->nlog);
        }
    }
    TK(0);
    int mode = st.r[0x05] & 7;
    lc_why = why_bits(&st, in);
    if (lc_why == 0x10) return 1;        /* forced blank and nothing else: the NMI's own blank, not a picture; nothing changes */
    if (lc_why) {
        pv.valid = 0;
        for (int b = 0; b < 4; b++) nbc[b].valid = 0;
        return -1;
    }
    plan->fixed = st.fixed;
    plan->color = st.color;
    if (st.fixed) {
        /* A ring anchored to one layer cannot retain rows with another mode,
         * layer selection or vertical origin. Render in screen coordinates;
         * tiled and Mode 7 passes have disjoint row masks, one presentation. */
        fixed_plan(in, &st, plan, vpages);
        return 0;
    }
    int ring = layer_vis(&st, 0) ? 0 : layer_vis(&st, 1) ? 1 : layer_vis(&st, 2) ? 2 : -1;
    int full = !pv.valid || pv.st.fixed, dx = 0, dy = 0;
    int wx = pv.wx, wy = pv.wy;
    if (ring >= 0 && !full) {
        int cx = st.bgofs[ring * 2] & 1023, cy = st.bgofs[ring * 2 + 1] & 1023;
        int px = pv.st.bgofs[ring * 2] & 1023, py = pv.st.bgofs[ring * 2 + 1] & 1023;
        dx = ((cx - px + 512) & 1023) - 512;
        dy = ((cy - py + 512) & 1023) - 512;
        wx += dx;
        wy += dy;
    } else if (ring >= 0) {
        wx = st.bgofs[ring * 2] & 1023;
        wy = st.bgofs[ring * 2 + 1] & 1023;
    } else {
        wx = wy = 0;
    }
    if (!full) {
        /* any register that decides what a cell or sprite looks like, except scroll and brightness */
        static const uint8_t keys[] = {0x01, 0x05, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x2C, 0x2D, 0x02, 0x03, 0x33};
        for (unsigned i = 0; i < sizeof keys; i++)
            if (pv.st.r[keys[i]] != st.r[keys[i]]) full = 1;
        if (dx > 96 || dx < -96 || dy > 48 || dy < -48) full = 1;
    }
    if (full)
        for (int b = 0; b < 4; b++) nbc[b].valid = 0;
    cur_wy = wy;
    cur_x0 = wx >> 4;
    cur_jmax = (wx & 15) ? WINW : WINW - 1;
    for (int y = 0; y < PPU_H; y++) dw[y] = 0;
    /* VRAM written since the last frame, in 32-byte units: only in the pages the runtime says were written (all
     * of them for the first frame, or when it says nothing) */
    int any_v = 0;
    clear4(vdirty, sizeof vdirty);
    ndl = 0;
    for (int pg = 0; pg < 128; pg++) {
        if (pv.valid && vpages && !vpages[pg]) continue;
        for (int u = pg * 16; u < pg * 16 + 16; u++) {
            vdirty[u] = pv.valid ? differs(pv.vram + u * 32, in->vram + u * 32, 32) : 1;
            any_v |= vdirty[u];
            if (vdirty[u]) dlist[ndl++] = (uint16_t)u;
        }
        copy_bytes(pv.vram + pg * 512, in->vram + pg * 512, 512);
    }
    TK(1);
    int quiet = 0;
    if (full) {
        for (int y = 0; y < PPU_H; y++) dw[y] = (1u << cur_jmax) - 1;
    } else if (!dx && !dy && !any_v && !st.wany && !pv.st.wany && !pv_moved(&st, mode, ring) &&
               !differs(st.oam, pv.st.oam, sizeof st.oam)) {
        /* nothing moved, nothing written, the same OAM: no group changes */
        quiet = 1;
    } else {
        /* exposed: the groups that were not in the last window */
        int px0 = pv.wx >> 4, pjmax = (pv.wx & 15) ? WINW : WINW - 1;
        int jlo = px0 - cur_x0, jhi = px0 + pjmax - cur_x0;
        if (jlo < 0) jlo = 0;
        if (jlo > cur_jmax) jlo = cur_jmax;
        if (jhi < 0) jhi = 0;
        if (jhi > cur_jmax) jhi = cur_jmax;
        uint32_t all = (1u << cur_jmax) - 1, edge = ((1u << jlo) - 1) | (all & ~((1u << jhi) - 1));
        for (int y = 0; y < PPU_H; y++) {
            int py = (wy + y - pv.wy) & (RH - 1);
            dw[y] |= py >= PPU_H ? all : edge;
        }
        for (int b = 0; b < 4; b++) {
            if (!layer_vis(&st, b) || (b == 3 && mode == 1)) {
                nbc[b].valid = 0;
                continue;
            }
            int bpp = bpp_of(mode, b);
            int moved = layer_moved(&st, b, ring, dx, dy);
            if (!moved && any_v && b != ring && ((st.varmask | pv.st.varmask) >> b & 1)) {
                uint32_t sg[3];
                nb_sig(&st, b, bpp, sg);
                moved = layer_dirty(sg);                /* rows with their own scroll: the row-wise path knows them */
            }
            if (moved) {
                SPT();
                moved_layer(&st, in->vram, b, bpp, wx, wy);
                SPK(5);
                continue;
            }
            nbc[b].valid = 0;
            if (any_v) scan_layer(&st, in->vram, b, bpp, wx, wy);
        }
        TK(2);
        static Spr cur[128], old[128];
        int nc = parse_oam(st.oam, st.r[0x01], cur), no = parse_oam(pv.st.oam, pv.st.r[0x01], old);
        for (int i = 0; i < nc; i++)
            if (oam_entry_changed(&st, &pv.st, cur[i].idx) || dx || dy ||
                (any_v && sprite_tile_dirty(&cur[i], st.r[0x01], st.oam[cur[i].idx * 4 + 3])))
                footprint(&cur[i], wx, wy);
        for (int i = 0; i < no; i++)
            if (oam_entry_changed(&st, &pv.st, old[i].idx) || dx || dy) footprint(&old[i], pv.wx, pv.wy);
        if (st.wany || pv.st.wany) win_dirty(&st, dx, wx, wy);
    }
    if (st.split && !full) {
        /* rows with a configuration of their own (the HUD, a colour band): the ring does not describe them, so they are composed
         * whenever anything that could change them did: the ring's scroll (the Videl moves them too), VRAM, OAM, their own registers */
        int any = dx || dy || any_v || differs(st.oam, pv.st.oam, sizeof st.oam) || !pv.st.split;
        for (int y = 0; y < PPU_H; y++) {
            if (!st.odd[y]) continue;
            if (any || !pv.st.odd[y] || differs((const uint8_t *)&st.fixedrows[y], (const uint8_t *)&pv.st.fixedrows[y], sizeof(FixedRow))) {
                dw[y] = (1u << cur_jmax) - 1;
                quiet = 0;
            }
        }
    }
    TK(3);
    /* Forced blank from row brow on (an IRQ's, CT's): those rows show index 0, whatever the world holds there. A ring
     * group that showed blank last time holds no world content and has to be composed when it comes into a visible row;
     * a group that comes into a blank row is composed blank unless it showed blank already. */
    int S = lin(wy, cur_x0), brow = st.brow, bchg = 0;
    uint32_t allj = (1u << cur_jmax) - 1;
    if (!full && (brow < PPU_H || pv.nblank)) {
        for (int y = 0; y < PPU_H; y++) {
            int i = (S + y * NGR) & (N - 1), w = i >> 5, off = i & 31;
            uint32_t pb = pv.blank[w] >> off;
            if (off > 15) pb |= pv.blank[(w + 1) & (N / 32 - 1)] << (32 - off);
            pb &= allj;
            uint32_t d = dw[y], nd = y >= brow ? allj & ~pb : d | pb;
            bchg |= nd != d;
            dw[y] = nd;
        }
    }
    /* what this picture shows blank, for the next */
    static uint32_t nblk[N / 32];
    if (brow < PPU_H || pv.nblank) {
        clear4(nblk, sizeof nblk);
        for (int y = brow; y < PPU_H; y++) {
            int i = (S + y * NGR) & (N - 1), w = i >> 5, off = i & 31;
            nblk[w] |= allj << off;
            if (off > 15) nblk[(w + 1) & (N / 32 - 1)] |= allj >> (32 - off);
        }
        copy_bytes(pv.blank, nblk, sizeof nblk);
    }
    pv.nblank = brow < PPU_H;
    /* the plan: masks of the groups to compose, the groups the other page missed */
    plan->dl = wx & 15;
    plan->wx = wx;
    plan->wy = wy;
    plan->S = S;
    plan->cold = full;
    plan->ngroups = plan->ncopies = 0;
    if (quiet && pv.nset == 0 && !bchg) {
        /* nothing composed last frame either: nothing to draw, nothing for the other page to copy */
        clear4(plan->maskA, sizeof plan->maskA);
        clear4(plan->maskB, sizeof plan->maskB);
        clear4(plan->cmask, sizeof plan->cmask);
        plan->ngroups = 0;
        goto planned;
    }
    static uint32_t nbits[N / 32];
    clear4(nbits, sizeof nbits);
    int npd = 0;
    for (int y = 0; y < PPU_H; y++) {
        uint32_t m = dw[y];
        plan->maskA[y] = (uint16_t)m;
        plan->maskB[y] = (uint16_t)((m >> 16) & 1);
        if (!m) continue;
        npd += popcount17(m);
        int i = (plan->S + y * NGR) & (N - 1), w = i >> 5, off = i & 31;
        nbits[w] |= m << off;
        if (off > 15) nbits[(w + 1) & (N / 32 - 1)] |= m >> (32 - off);
    }
    plan->ngroups = npd;
    for (int y = 0; y < PPU_H; y++) plan->cmask[y] = 0;
    if (pv.valid)
        for (int y = 0; y < PPU_H; y++) {
            int i = (plan->S + y * NGR) & (N - 1), w = i >> 5, off = i & 31;
            uint32_t f = pv.bits[w] >> off;
            if (off > 15) f |= pv.bits[(w + 1) & (N / 32 - 1)] << (32 - off);
            f &= ((1u << cur_jmax) - 1) & ~dw[y];
            plan->cmask[y] = f;
            while (f) {
                int j = 0;
                while (!((f >> j) & 1)) j++;
                f &= f - 1;
                plan->copies[plan->ncopies++] = (uint16_t)((i + j) & (N - 1));
            }
        }
    copy_bytes(pv.bits, nbits, sizeof nbits);
    pv.nset = npd;
planned:
    pv.wx = wx;
    pv.wy = wy;
    copy_bytes(&pv.st, &st, sizeof st);
    pv.valid = 1;
    TK(4);
    return 0;
}
