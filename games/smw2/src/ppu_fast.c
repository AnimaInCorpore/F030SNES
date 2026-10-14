/* Fast row composer for the 256-colour display: every pixel is its CGRAM
 * index, which the 8-plane Videl mode shows through a palette loaded
 * from CGRAM. Output is byte for byte what the reference (ppu_ref_row)
 * produces with an identity palette map; tools/ppu_snap.c checks that.
 *
 * Instead of resolving priorities per pixel, layers are painted back to
 * front in the mode's priority order, each 8-pixel character row at once:
 * the four (or two) bit planes of a row become eight colour bytes through
 * a spread table, and only opaque bytes are written. Fully transparent rows
 * are skipped, fully opaque rows stored as two longs. BG modes 0-6 (2-6
 * as mode 1, like the reference) take this path; Mode 7 rows use the
 * reference. Freestanding: no libc. */
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

/* ppu_obj_line's result for the pixels [*x0, *x1) it touched; the rest of
 * out[] stays 0 from the previous call. */
static void fast_obj_line(const PpuInput *in, const Regs *s, int row, uint16_t *out, int *x0, int *x1)
{
    static int last0 = 0, last1 = PPU_W;
    for (int x = last0; x < last1; x++) out[x] = 0;
    last0 = PPU_W;
    last1 = 0;
    if (built_gen != frame_gen || built_oam != s->oam || built_obsel != s->r[0x01])
        obj_buckets(s);
    uint8_t obsel = s->r[0x01];
    uint32_t base = (uint32_t)(obsel & 7) << 13;
    uint32_t gap = (uint32_t)(((obsel >> 3) & 3) + 1) << 12;
    const uint8_t (*sz)[2] = obj_sizes[obsel >> 5];
    int slivers = 0;
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
        uint16_t tag = (uint16_t)((((attr >> 4) & 3) << 8) | 0x400);
        uint8_t pal = (uint8_t)(128 + ((attr >> 1) & 7) * 16);
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
            uint32_t ch = hflip ? __builtin_bswap32(r->cl) : r->ch;
            uint32_t cl = hflip ? __builtin_bswap32(r->ch) : r->cl;
            for (int px = 0; px < 8; px++) {
                int X = sx + px;
                uint32_t v = px < 4 ? (ch >> (24 - 8 * px)) & 15 : (cl >> (56 - 8 * px)) & 15;
                if (!v || X < 0 || X >= 256) continue;
                out[X] = (uint16_t)((pal + v) | tag);
                if (X < last0) last0 = X;
                if (X >= last1) last1 = X + 1;
            }
        }
    }
done:
    *x0 = last0;
    *x1 = last1;
}

/* Paints the sprite pixels of priority p. */
static void obj_pass(const uint16_t *obj, int x0, int x1, int p, uint8_t *line)
{
    for (int x = x0; x < x1; x++) {
        uint16_t o = obj[x];
        if ((o & 0x400) && ((o >> 8) & 3) == p)
            line[PAD + x] = (uint8_t)o;
    }
}

void ppu_fast_row(const PpuInput *in, const Regs *s, int row, uint8_t *out)
{
    uint8_t inidisp = s->r[0x00];
    int mode = s->r[0x05] & 7;
    uint8_t tm = s->r[0x2C];
    if (inidisp & 0x80) {
        for (int x = 0; x < PPU_W; x++) out[x] = 0;   /* black through the CLUT */
        return;
    }
    if (mode == 7) {
        ppu_ref_row(in, s, row, out);
        return;
    }
    if (!tables_ready)
        make_tables();
    static uint8_t line[PPU_W + 2 * PAD];
    static uint16_t obj[PPU_W];
    for (int x = 0; x < PPU_W + 2 * PAD; x += 4) put32(line + x, 0);   /* backdrop: index 0 */
    int x0 = PPU_W, x1 = 0;
    if (tm & 0x10)
        fast_obj_line(in, s, row, obj, &x0, &x1);
    static Layer L[4];
    if (mode == 0) {
        /* back to front: BG4L BG3L OBJ0 BG4H BG3H OBJ1 BG2L BG1L OBJ2 BG2H BG1H OBJ3 */
        for (int i = 0; i < 4; i++) {
            L[i].n = 0;
            if (tm & (1 << i)) bg_decode(in, s, i, 2, i * 32, row, &L[i]);
        }
        bg_paint(&L[3], 0, line);
        bg_paint(&L[2], 0, line);
        obj_pass(obj, x0, x1, 0, line);
        bg_paint(&L[3], 1, line);
        bg_paint(&L[2], 1, line);
        obj_pass(obj, x0, x1, 1, line);
        bg_paint(&L[1], 0, line);
        bg_paint(&L[0], 0, line);
        obj_pass(obj, x0, x1, 2, line);
        bg_paint(&L[1], 1, line);
        bg_paint(&L[0], 1, line);
        obj_pass(obj, x0, x1, 3, line);
    } else {
        /* back to front: BG3L OBJ0 [BG3H] OBJ1 BG2L BG1L OBJ2 BG2H BG1H OBJ3 [BG3H] */
        int bg3top = (s->r[0x05] & 8) != 0;
        for (int i = 0; i < 3; i++) {
            L[i].n = 0;
            if (tm & (1 << i)) bg_decode(in, s, i, i == 2 ? 2 : 4, 0, row, &L[i]);
        }
        bg_paint(&L[2], 0, line);
        obj_pass(obj, x0, x1, 0, line);
        if (!bg3top) bg_paint(&L[2], 1, line);
        obj_pass(obj, x0, x1, 1, line);
        bg_paint(&L[1], 0, line);
        bg_paint(&L[0], 0, line);
        obj_pass(obj, x0, x1, 2, line);
        bg_paint(&L[1], 1, line);
        bg_paint(&L[0], 1, line);
        obj_pass(obj, x0, x1, 3, line);
        if (bg3top) bg_paint(&L[2], 1, line);
    }
    for (int x = 0; x < PPU_W; x += 4) put32(out + x, get32(line + PAD + x));
}

void ppu_fast_render(const PpuInput *in, uint8_t *out)
{
    frame_gen++;                /* OAM may have changed since the last frame */
    ppu_render_rows(in, out, ppu_fast_row);
}

/* ppu_planar8 (the bit-by-bit packing, the reference) is in snes_ppu.c. */

/* The same packing, 32 pixels at a time through a merge network (verified
 * symbolically, and against ppu_planar8 by tools/c2p_test.c). The eight
 * 4-pixel chunks load into registers numbered from their chunk index g =
 * i4 i3 i2 as (~i3, ~i4, ~i2); five swap stages exchange the chunk bits
 * with the plane bits, after which register n holds plane
 * (n&1)*4 + (n>>1&1)*2 + (n>>2&1): pixels 0-15 in the high word (first
 * 16-pixel group), 16-31 in the low word (second group). */
#define MERGE(x, y, s, m) do { uint32_t t_ = (((x) >> (s)) ^ (y)) & (m); \
                               (y) ^= t_; (x) ^= t_ << (s); } while (0)

void ppu_c2p8(const uint8_t *chunky, uint16_t *planar)
{
    for (int i = 0; i < PPU_W * PPU_H; i += 32, chunky += 32, planar += 16) {
        /* chunk g -> register (~i3)<<2 | (~i4)<<1 | ~i2 */
        uint32_t r7 = get32(chunky + 0), r6 = get32(chunky + 4);
        uint32_t r3 = get32(chunky + 8), r2 = get32(chunky + 12);
        uint32_t r5 = get32(chunky + 16), r4 = get32(chunky + 20);
        uint32_t r1 = get32(chunky + 24), r0 = get32(chunky + 28);
        MERGE(r0, r4, 8, 0x00FF00FFu); MERGE(r1, r5, 8, 0x00FF00FFu);
        MERGE(r2, r6, 8, 0x00FF00FFu); MERGE(r3, r7, 8, 0x00FF00FFu);
        MERGE(r0, r4, 1, 0x55555555u); MERGE(r1, r5, 1, 0x55555555u);
        MERGE(r2, r6, 1, 0x55555555u); MERGE(r3, r7, 1, 0x55555555u);
        MERGE(r0, r2, 16, 0x0000FFFFu); MERGE(r1, r3, 16, 0x0000FFFFu);
        MERGE(r4, r6, 16, 0x0000FFFFu); MERGE(r5, r7, 16, 0x0000FFFFu);
        MERGE(r0, r2, 2, 0x33333333u); MERGE(r1, r3, 2, 0x33333333u);
        MERGE(r4, r6, 2, 0x33333333u); MERGE(r5, r7, 2, 0x33333333u);
        MERGE(r0, r1, 4, 0x0F0F0F0Fu); MERGE(r2, r3, 4, 0x0F0F0F0Fu);
        MERGE(r4, r5, 4, 0x0F0F0F0Fu); MERGE(r6, r7, 4, 0x0F0F0F0Fu);
        /* planes 0..7 are registers 0, 4, 2, 6, 1, 5, 3, 7 */
        planar[0] = (uint16_t)(r0 >> 16); planar[8 + 0] = (uint16_t)r0;
        planar[1] = (uint16_t)(r4 >> 16); planar[8 + 1] = (uint16_t)r4;
        planar[2] = (uint16_t)(r2 >> 16); planar[8 + 2] = (uint16_t)r2;
        planar[3] = (uint16_t)(r6 >> 16); planar[8 + 3] = (uint16_t)r6;
        planar[4] = (uint16_t)(r1 >> 16); planar[8 + 4] = (uint16_t)r1;
        planar[5] = (uint16_t)(r5 >> 16); planar[8 + 5] = (uint16_t)r5;
        planar[6] = (uint16_t)(r3 >> 16); planar[8 + 6] = (uint16_t)r3;
        planar[7] = (uint16_t)(r7 >> 16); planar[8 + 7] = (uint16_t)r7;
    }
}
