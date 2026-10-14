/* Fast row composer (from F030SMW's src/ppu_fast.c, 2026-09-24) for SMK's
 * 16-colour display: rows are composed as CGRAM indices, then mapped to
 * the adaptive palette's slots with the same histogram counts as the
 * reference (ppu_ref_row), so the output is byte for byte the reference's;
 * tools/ppu_snap.c checks that on every snapshot.
 *
 * Instead of resolving priorities per pixel, layers are painted back to
 * front in the mode's priority order, each 8-pixel character row at once:
 * the four (or two) bit planes of a row become eight colour bytes through
 * a spread table, and only opaque bytes are written. Decoded character
 * rows are cached per VRAM word address until the runtime marks their
 * page written. Sprites come from per-row lists built once per frame and
 * OAM segment. Mode 7 rows that go to the DSP get only their sprites here;
 * Mode 7 rows the CPU draws use the reference. Freestanding: no libc. */
#include "ppu_internal.h"
#include "ppu_fast.h"

/* A line with 8 bytes of slack on both sides: character rows start up to
 * 7 pixels left of the screen and end up to 7 right of it. */
#define PAD 8

static uint32_t spread_hi[256], spread_lo[256];   /* bit 7-k of b -> byte k = 1 */
static uint32_t mask_hi[256], mask_lo[256];       /* byte k = $FF where bit 7-k */
static uint32_t rmask_hi[256], rmask_lo[256];
static uint8_t tables_ready;

static void make_tables(void)
{
    for (int b = 0; b < 256; b++) {
        uint32_t h = 0, l = 0, rh = 0, rl = 0;     /* rh/rl: mirrored, for the masks */
        for (int k = 0; k < 8; k++) {
            uint32_t on = (b >> (7 - k)) & 1;          /* pixel k from the left */
            uint32_t ron = (b >> k) & 1;               /* pixel k, mirrored */
            if (k < 4) {
                h |= on << (8 * (3 - k));
                rh |= ron << (8 * (3 - k));
            } else {
                l |= on << (8 * (7 - k));
                rl |= ron << (8 * (7 - k));
            }
        }
        spread_hi[b] = h;
        spread_lo[b] = l;
        mask_hi[b] = h * 0xFF;
        mask_lo[b] = l * 0xFF;
        rmask_hi[b] = rh * 0xFF;
        rmask_lo[b] = rl * 0xFF;
    }
    tables_ready = 1;
}

static inline uint16_t vword(const uint8_t *vram, uint32_t word)
{
    word &= 0x7FFF;
    return (uint16_t)(vram[word * 2] | (vram[word * 2 + 1] << 8));
}

/* Eight chunky pixels as two longs, most significant byte leftmost. The
 * 68030 takes misaligned long accesses; the host assembles bytes. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline uint32_t get32(const uint8_t *p) { return *(const uint32_t *)(const void *)p; }
static inline void put32(uint8_t *p, uint32_t v) { *(uint32_t *)(void *)p = v; }
static inline uint16_t get16(const uint8_t *p) { return *(const uint16_t *)(const void *)p; }
#else
static inline uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
#endif

/* ---- tile-row cache ----------------------------------------------------
 * A character row decoded once: eight colour bytes (0-15, palette not
 * added), unflipped, and its opaque mask, per VRAM word address of the
 * row's first plane word, for 2 and 4 bits per pixel. Entries of a 256-word
 * VRAM page are dropped when the page is written (ppu_fast_invalidate_page,
 * driven by the runtime's page marks). An h-flip is a byte reversal. */
typedef struct { uint32_t ch, cl; } Row8;
static Row8 rcache[2][32768];
static uint8_t rmask[2][32768], rvalid[2][32768];

void ppu_fast_invalidate_page(int page)
{
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 256; i++) rvalid[b][(page & 127) * 256 + i] = 0;
}

void ppu_fast_invalidate_all(void)
{
    for (int p = 0; p < 128; p++) ppu_fast_invalidate_page(p);
}

/* The row at VRAM word addr (4 bpp: planes 2-3 eight words on). */
static inline const Row8 *row_at(const uint8_t *vram, int b4, uint32_t addr, uint8_t *m)
{
    if (!rvalid[b4][addr]) {
        uint16_t w01 = get16(vram + addr * 2);             /* p0 << 8 | p1 */
        uint8_t p0 = (uint8_t)(w01 >> 8), p1 = (uint8_t)w01, p2 = 0, p3 = 0;
        if (b4) {
            uint16_t w23 = get16(vram + ((addr + 8) & 0x7FFF) * 2);
            p2 = (uint8_t)(w23 >> 8);
            p3 = (uint8_t)w23;
        }
        Row8 *r = &rcache[b4][addr];
        r->ch = spread_hi[p0] | spread_hi[p1] << 1 | spread_hi[p2] << 2 | spread_hi[p3] << 3;
        r->cl = spread_lo[p0] | spread_lo[p1] << 1 | spread_lo[p2] << 2 | spread_lo[p3] << 3;
        rmask[b4][addr] = p0 | p1 | p2 | p3;
        rvalid[b4][addr] = 1;
    }
    *m = rmask[b4][addr];
    return &rcache[b4][addr];
}

/* One decoded character row of a layer: screen x (-7..255), the eight
 * colour bytes with the palette already added, and the opaque mask. */
typedef struct {
    int16_t x;
    uint8_t hi, full;           /* priority bit; all eight pixels opaque */
    uint32_t ch, cl, mh, ml;
} Cell;

typedef struct {
    int n;
    Cell c[33];
} Layer;

/* Decodes the character rows of one BG layer that have an opaque pixel. */
static void bg_decode(const PpuInput *in, const Regs *s, int bg, int bpp, int palbase,
                      int row, Layer *L)
{
    const uint8_t *vram = in->vram;
    uint8_t sc = s->r[0x07 + bg];
    uint32_t mapbase = (uint32_t)(sc & 0xFC) << 8;
    int wide = sc & 1, tall = (sc >> 1) & 1;
    uint8_t nba = s->r[0x0B + (bg >> 1)];
    uint32_t chrbase = (uint32_t)((bg & 1) ? (nba >> 4) : (nba & 15)) << 12;
    int big = (s->r[0x05] >> (4 + bg)) & 1;
    int tshift = big ? 4 : 3, tile = 1 << tshift;
    int hofs = s->bgofs[bg * 2] & 0x3FF, vofs = s->bgofs[bg * 2 + 1] & 0x3FF;
    int mapw = (wide ? 64 : 32) << tshift, maph = (tall ? 64 : 32) << tshift;
    int y = (row + 1 + vofs) & (maph - 1);
    int ty = y >> tshift, fy0 = y & (tile - 1);
    uint32_t rowbase = mapbase;
    if (ty >= 32) { rowbase += wide ? 0x800 : 0x400; ty -= 32; }
    rowbase += (uint32_t)ty * 32;
    int words = bpp * 4, b4 = bpp == 4;
    int x = -(hofs & 7);
    int px = hofs & ~7;
    Cell *c = L->c;
    for (int cell = 0; cell < 33; cell++, x += 8, px += 8) {
        int mpx = px & (mapw - 1);
        int tx = mpx >> tshift;
        uint32_t scr = rowbase;
        if (tx >= 32) { scr += 0x400; tx -= 32; }
        uint16_t e = vword(vram, scr + (uint32_t)tx);
        int hflip = (e >> 14) & 1;
        int fx = mpx & (tile - 1);             /* 0, or 8 in a 16-pixel tile */
        int fy = fy0;
        if (hflip) fx = tile - 1 - fx;
        if (e & 0x8000) fy = tile - 1 - fy;
        uint32_t chr = e & 0x3FF;
        if (big) {
            if (fx >= 8) chr += 1;
            if (fy >= 8) chr += 16;
        }
        uint32_t addr = (chrbase + (chr & 0x3FF) * (uint32_t)words + (uint32_t)(fy & 7)) & 0x7FFF;
        uint8_t m;
        const Row8 *r = row_at(vram, b4, addr, &m);
        if (!m)
            continue;
        uint32_t pal = (uint32_t)((palbase + ((e >> 10) & 7) * (1 << bpp)) & 0xFF) * 0x01010101u;
        c->x = (int16_t)x;
        c->hi = (uint8_t)((e >> 13) & 1);
        c->full = m == 0xFF;
        if (hflip) {
            c->ch = __builtin_bswap32(r->cl) | pal;
            c->cl = __builtin_bswap32(r->ch) | pal;
            c->mh = rmask_hi[m];
            c->ml = rmask_lo[m];
        } else {
            c->ch = r->ch | pal;
            c->cl = r->cl | pal;
            c->mh = mask_hi[m];
            c->ml = mask_lo[m];
        }
        c++;
    }
    L->n = (int)(c - L->c);
}

/* Paints a decoded layer's character rows of priority `hi`. */
static void bg_paint(const Layer *L, int hi, uint8_t *line)
{
    const Cell *c = L->c, *end = c + L->n;
    for (; c < end; c++) {
        if (c->hi != hi)
            continue;
        uint8_t *d = line + PAD + c->x;
        if (c->full) {
            put32(d, c->ch);
            put32(d + 4, c->cl);
        } else {
            put32(d, (get32(d) & ~c->mh) | (c->ch & c->mh));
            put32(d + 4, (get32(d + 4) & ~c->ml) | (c->cl & c->ml));
        }
    }
}

/* ---- sprites --------------------------------------------------------- */

static const uint8_t obj_sizes[8][2][2] = {   /* as in snes_ppu.c */
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

/* Per-row lists of the first 32 sprites in range, in OAM order, built once
 * per frame and OAM segment (the reference scans all 128 per row). */
static uint8_t rowlist[PPU_H][32], rowcount[PPU_H];
static const uint8_t *built_oam;
static uint8_t built_obsel;
static uint32_t built_gen, frame_gen;

static void obj_buckets(const Regs *s)
{
    uint8_t obsel = s->r[0x01];
    const uint8_t (*sz)[2] = obj_sizes[obsel >> 5];
    for (int r = 0; r < PPU_H; r++) rowcount[r] = 0;
    for (int i = 0; i < 128; i++) {
        const uint8_t *e = s->oam + i * 4;
        uint8_t hi = (s->oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int w = sz[hi >> 1][0], h = sz[hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        if (x <= -w || x >= 256) continue;
        for (int dy = 0; dy < h; dy++) {
            int r = (e[1] + dy) & 0xFF;            /* rows where (row - y) & $FF == dy */
            if (r < PPU_H && rowcount[r] < 32)
                rowlist[r][rowcount[r]++] = (uint8_t)i;
        }
    }
    built_oam = s->oam;
    built_obsel = obsel;
    built_gen = frame_gen;
}

/* The row's sprite pixels as two byte lines with PAD bytes of slack on both
 * sides: the CGRAM index, and $80 | priority where a sprite is opaque (0
 * elsewhere): ppu_obj_line's result. Slivers go in eight bytes at a time
 * under their opaque mask, from the last sprite in range to the first, so
 * the lowest OAM index ends on top. [*x0, *x1) bounds the pixels written;
 * what the previous row wrote is cleared first. */
static uint8_t ocol[PPU_W + 2 * PAD + 4], opri[PPU_W + 2 * PAD + 4];   /* +4: clears run in longs */
static int dirty0 = 0, dirty1 = PPU_W + 2 * PAD;   /* opri bytes to clear */

static void fast_obj_clear(void)
{
    for (int x = dirty0; x < dirty1; x += 4) put32(opri + x, 0);
    dirty0 = PPU_W + 2 * PAD;
    dirty1 = 0;
}

static void fast_obj_line(const PpuInput *in, const Regs *s, int row, int *x0, int *x1)
{
    fast_obj_clear();
    *x0 = PPU_W;
    *x1 = 0;
    if (built_gen != frame_gen || built_oam != s->oam || built_obsel != s->r[0x01])
        obj_buckets(s);
    uint8_t obsel = s->r[0x01];
    uint32_t base = (uint32_t)(obsel & 7) << 13;
    uint32_t gap = (uint32_t)(((obsel >> 3) & 3) + 1) << 12;
    const uint8_t (*sz)[2] = obj_sizes[obsel >> 5];
    int slivers = 0, lo = PPU_W, hi_x = -8;
    for (int k = rowcount[row] - 1; k >= 0; k--) {
        int i = rowlist[row][k];
        const uint8_t *e = s->oam + i * 4;
        uint8_t hi = (s->oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int w = sz[hi >> 1][0], h = sz[hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        int dy = (row - e[1]) & 0xFF;
        uint8_t attr = e[3];
        if (attr & 0x80) dy = h - 1 - dy;
        int tcol0 = e[2] & 15, trow0 = e[2] >> 4;
        uint32_t tbase = base + ((attr & 1) ? gap : 0);
        uint32_t pv = (uint32_t)(0x80 | ((attr >> 4) & 3)) * 0x01010101u;
        uint32_t pal = (uint32_t)(128 + ((attr >> 1) & 7) * 16) * 0x01010101u;
        int hflip = attr & 0x40;
        for (int col = 0; col < w / 8; col++) {
            int sx = x + col * 8;
            if (sx <= -8 || sx >= 256) continue;
            if (++slivers > 34) goto done;
            int c = hflip ? (w / 8 - 1 - col) : col;
            int chr = (((trow0 + (dy >> 3)) & 15) << 4) | ((tcol0 + c) & 15);
            uint32_t addr = (tbase + (uint32_t)chr * 16 + (uint32_t)(dy & 7)) & 0x7FFF;
            uint8_t m;
            const Row8 *r = row_at(in->vram, 1, addr, &m);
            if (!m) continue;
            uint32_t ch, cl, mh, ml;
            if (hflip) {
                ch = __builtin_bswap32(r->cl);
                cl = __builtin_bswap32(r->ch);
                mh = rmask_hi[m];
                ml = rmask_lo[m];
            } else {
                ch = r->ch;
                cl = r->cl;
                mh = mask_hi[m];
                ml = mask_lo[m];
            }
            uint8_t *dc = ocol + PAD + sx, *dp = opri + PAD + sx;
            put32(dc, (get32(dc) & ~mh) | ((ch | pal) & mh));
            put32(dc + 4, (get32(dc + 4) & ~ml) | ((cl | pal) & ml));
            put32(dp, (get32(dp) & ~mh) | (pv & mh));
            put32(dp + 4, (get32(dp + 4) & ~ml) | (pv & ml));
            if (sx < lo) lo = sx;
            if (sx > hi_x) hi_x = sx;
        }
    }
done:
    if (hi_x >= lo) {
        dirty0 = (PAD + lo) & ~3;
        dirty1 = PAD + hi_x + 8;
        *x0 = lo < 0 ? 0 : lo;
        *x1 = hi_x + 8 > PPU_W ? PPU_W : hi_x + 8;
    }
}

/* Paints the sprite pixels of priority p: bytes of opri equal to $80 | p,
 * four at a time (exact zero-byte test on the XOR). */
static void obj_pass(int x0, int x1, int p, uint8_t *line)
{
    uint32_t want = (uint32_t)(0x80 | p) * 0x01010101u;
    for (int x = x0 & ~3; x < x1; x += 4) {
        uint32_t v = get32(opri + PAD + x) ^ want;
        uint32_t z = ~(((v & 0x7F7F7F7Fu) + 0x7F7F7F7Fu) | v) & 0x80808080u;
        if (!z)
            continue;
        uint32_t m = (z >> 7) * 0xFFu;
        uint8_t *d = line + PAD + x;
        put32(d, (get32(d) & ~m) | (get32(ocol + PAD + x) & m));
    }
}

void ppu_fast_row(const PpuInput *in, const Regs *s, int row, uint8_t *out)
{
    uint8_t inidisp = s->r[0x00];
    int mode = s->r[0x05] & 7;
    uint8_t tm = s->r[0x2C];
    if (!tables_ready)
        make_tables();
    if (!(inidisp & 0x80) && ppu_win_layers(s)) {
        /* windows hide a layer somewhere on this row (menus): the
         * reference composes it */
        ppu_ref_row(in, s, row, out);
        return;
    }
    if (mode == 7) {
        if ((inidisp & 0x80) || !(tm & 1) || !in->m7sink) {
            ppu_ref_row(in, s, row, out);
            return;
        }
        /* A Mode 7 row for the DSP: only its sprites are composed here, as
         * in the reference's compose_m7_ext. */
        PpuM7Line l;
        PROF_START();
        ppu_mode7_setup(s, row, &l);
        in->m7sink->m7_row(in->m7sink->ctx, row, &l);
        PROF_MARK(PPU_PROF_M7FEED);
        int x0 = PPU_W, x1 = 0;
        if (tm & 0x10)
            fast_obj_line(in, s, row, &x0, &x1);
        else
            fast_obj_clear();
        PROF_MARK(PPU_PROF_OBJ);
        const uint8_t *map = in->map;
        uint32_t *hist = in->hist;
        uint8_t ext[8];
        int32_t vx = l.vx, vy = l.vy, ax8 = l.a * 8, cx8 = l.c * 8;
        for (int x = 0; x < PPU_W; x += 8, vx += ax8, vy += cx8) {
            /* the block's first pixel: the histogram's sample */
            int idx0 = -1;
            if (x + 8 > x0 && x < x1) {        /* sprite pixels in this block */
                for (int k = 0; k < 8; k++) {
                    uint8_t o = opri[PAD + x + k];
                    int idx = -1;
                    if (o & 0x80) {
                        if (o & 3) idx = ocol[PAD + x + k];
                        else if (!ppu_m7_texel(in->vram, l.sel, l.vx + l.a * (x + k), l.vy + l.c * (x + k)))
                            idx = ocol[PAD + x + k];
                    }
                    ext[k] = idx < 0 ? PPU_EXT : map[idx];
                    if (k == 0) idx0 = idx;
                }
                for (int k = 0; k < 8; k++) out[x + k] = ext[k];
            } else {
                put32(out + x, 0xFFFFFFFFu);        /* PPU_EXT */
                put32(out + x + 4, 0xFFFFFFFFu);
            }
            if (!(x & 15)) {                     /* every 16th pixel counts 16 */
                if (idx0 < 0) idx0 = ppu_m7_texel(in->vram, l.sel, vx, vy);
                hist[idx0] += 16;
            }
        }
        PROF_MARK(PPU_PROF_COMPOSE);
        return;
    }
    if (in->m7sink) in->m7sink->cpu_row(in->m7sink->ctx, row);
    if (inidisp & 0x80) {
        /* forced blank: the runtime blacks the palette; pixels keep the
         * backdrop's slot */
        for (int x = 0; x < PPU_W; x++) out[x] = in->map[0];
        in->hist[0] += 256;
        return;
    }
    static uint8_t line[PPU_W + 2 * PAD];
    PROF_START();
    for (int x = 0; x < PPU_W + 2 * PAD; x += 4) put32(line + x, 0);   /* backdrop: index 0 */
    int x0 = PPU_W, x1 = 0;
    if (tm & 0x10)
        fast_obj_line(in, s, row, &x0, &x1);
    else
        fast_obj_clear();
    PROF_MARK(PPU_PROF_OBJ);
    static Layer L[4];
    if (mode == 0) {
        /* back to front: BG4L BG3L OBJ0 BG4H BG3H OBJ1 BG2L BG1L OBJ2 BG2H BG1H OBJ3 */
        for (int i = 0; i < 4; i++) {
            L[i].n = 0;
            if (tm & (1 << i)) bg_decode(in, s, i, 2, i * 32, row, &L[i]);
        }
        bg_paint(&L[3], 0, line);
        bg_paint(&L[2], 0, line);
        obj_pass(x0, x1, 0, line);
        bg_paint(&L[3], 1, line);
        bg_paint(&L[2], 1, line);
        obj_pass(x0, x1, 1, line);
        bg_paint(&L[1], 0, line);
        bg_paint(&L[0], 0, line);
        obj_pass(x0, x1, 2, line);
        bg_paint(&L[1], 1, line);
        bg_paint(&L[0], 1, line);
        obj_pass(x0, x1, 3, line);
    } else {
        /* back to front: BG3L OBJ0 [BG3H] OBJ1 BG2L BG1L OBJ2 BG2H BG1H OBJ3 [BG3H] */
        int bg3top = (s->r[0x05] & 8) != 0;
        for (int i = 0; i < 3; i++) {
            L[i].n = 0;
            if (tm & (1 << i)) bg_decode(in, s, i, i == 2 ? 2 : 4, 0, row, &L[i]);
        }
        bg_paint(&L[2], 0, line);
        obj_pass(x0, x1, 0, line);
        if (!bg3top) bg_paint(&L[2], 1, line);
        obj_pass(x0, x1, 1, line);
        bg_paint(&L[1], 0, line);
        bg_paint(&L[0], 0, line);
        obj_pass(x0, x1, 2, line);
        bg_paint(&L[1], 1, line);
        bg_paint(&L[0], 1, line);
        obj_pass(x0, x1, 3, line);
        if (bg3top) bg_paint(&L[2], 1, line);
    }
    PROF_MARK(PPU_PROF_BG);
    const uint8_t *map = in->map;
    uint32_t *hist = in->hist;
    const uint8_t *p = line + PAD;
    for (int x = 0; x < PPU_W; x++) out[x] = map[p[x]];
    for (int x = 0; x < PPU_W; x += 16) hist[p[x]] += 16;  /* as the DSP samples */
    PROF_MARK(PPU_PROF_COMPOSE);
}

void ppu_fast_render(const PpuInput *in, uint8_t *out)
{
    frame_gen++;                /* OAM may have changed since the last frame */
    ppu_render_rows(in, out, ppu_fast_row);
}

/* Chunky palette slots (0-15) to the Videl's 4-plane words, 16 pixels at a
 * time: two shift-ors pack nibbles, one merge between the two registers
 * and three index-bit swaps inside each finish the transposition. After
 * it, x = plane 1 << 16 | plane 0 and y = plane 3 << 16 | plane 2
 * (checked against ppu_planar by tools/ppu_snap.c). `groups` counts
 * 16-pixel groups. */
static inline uint32_t swapbits(uint32_t v, int d, uint32_t m)
{
    uint32_t t = ((v >> d) ^ v) & m;
    return v ^ t ^ (t << d);
}

void ppu_c2p4(const uint8_t *chunky, uint16_t *planar, int groups)
{
    for (; groups > 0; groups--, chunky += 16, planar += 4) {
        uint32_t x = get32(chunky + 8) << 4 | get32(chunky + 12);
        uint32_t y = get32(chunky) << 4 | get32(chunky + 4);
        uint32_t t = ((x >> 2) ^ y) & 0x33333333u;
        y ^= t;
        x ^= t << 2;
        x = swapbits(x, 14, 0x0000CCCCu);
        y = swapbits(y, 14, 0x0000CCCCu);
        x = swapbits(x, 8, 0x0000FF00u);
        y = swapbits(y, 8, 0x0000FF00u);
        x = swapbits(x, 15, 0x0000AAAAu);
        y = swapbits(y, 15, 0x0000AAAAu);
        planar[0] = (uint16_t)x;
        planar[1] = (uint16_t)(x >> 16);
        planar[2] = (uint16_t)y;
        planar[3] = (uint16_t)(y >> 16);
    }
}
