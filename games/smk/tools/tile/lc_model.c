/* Host model of the list compositor (analysis/list-compositor.md): a retained
 * frame buffer per Videl page that is brought up to date with a list of
 * operations instead of being composed again.
 *
 * The exact renderer stays the reference. For every frame the model takes
 * from ppu_render (identity map, so CGRAM indices)
 *   F  the frame as the exact renderer shows it,
 *   B  the same frame with OBJ off (the clean background the list restores
 *      from; on the Falcon this is the exact tile path or the tile cache),
 * and rasterizes the sprites itself (the same sliver order and 32/34 limits
 * as snes_ppu.c's obj_line) to learn which OAM entry owns each pixel. The
 * sprite pixels that show (F != B) form disjoint per-row pieces, one per OAM
 * entry and row; BG-over-sprite priority is taken from F, which the DSP
 * would compute from the BG priority bits it already reads. A piece's
 * signature is a hash of where it draws and what.
 *
 * A page holds the picture of an earlier frame (two frames back with two
 * pages). Updating it for frame t, per row:
 *   dirty groups (16-pixel Videl plane groups) =
 *       groups whose clean background changed since the page was drawn
 *     + footprint of every piece that appeared, vanished or changed
 *   restore the dirty groups from the clean background (RLE runs), then
 *   draw every piece that touches them, clipped to them (blitter rows).
 * Nothing else is touched; the result must equal F bit for bit.
 *
 * The page is a virtual screen with one group of margin each side, so
 * sprite overhang needs no clipping in the blitter rows.
 *
 * Two models run side by side on every update. The abstract one keeps the page
 * as pixels. The concrete one keeps it as the Videl's eight interleaved planes
 * and executes the list the way the hardware would: restores as linear copies,
 * sprite rows as blitter runs (plane-major source rows, skew, FXSR/NFSR,
 * endmasks) through blit_run(), a port of Hatari's blitter word loop. The
 * planar page must equal the packed pixel page (margins too) and
 * ppu_planar8() of the exact renderer's frame.
 *
 * Costs are counted as list items, not time: restore runs and groups,
 * blitter rows (one blit per piece run: first and last group masked freely,
 * the groups between full), list bytes at BLIT_REC per blitter row.
 *
 * F030SMW's copy (LoROM, snapshots converted by tools/lc/smw_to_ct_snap.py, built against F030CT's
 * src/snes_ppu.c): LC_SMW=1 switches on the rules SMW needs, all of them leaving the exactness checks
 * as they were:
 *   - the HDMA source bus is LoROM (CT's is HiROM);
 *   - colour math that changes no index of the frame ppu_render composes does not skip a frame:
 *     math against a black fixed colour without sub screen or halving, backdrop-only math the
 *     renderer composes itself (ppu_sub_cfg bit 0x20000, which the "sub screen" skip then also
 *     lets through), and a colour window that is empty or full (inside: never or always,
 *     outside: always or never). SMW sets CGWSEL $02/$12/$22, CGADSUB $20 in every level.
 *
 *   build/host/lc_model census  rom.sfc SNAP.BIN...
 *   build/host/lc_model seq     rom.sfc SNAP.BIN...        (as consecutive frames)
 *   build/host/lc_model sim     rom.sfc SNAP.BIN frames movers dx dy sx sy
 *       synthetic motion from one real frame: `movers` visible OAM entries
 *       move (dx, dy) per frame, the BG scroll moves (sx, sy) per frame
 *   build/host/lc_model blitfuzz pieces seed
 *       random pieces through the blit-row generator on a noise page
 *   build/host/lc_model hsim    rom.sfc SNAP.BIN frames movers dx dy sx sy
 *       the same generator on the virtual screen with hardware scrolling (ring
 *       pages, clean ring, Videl fetch simulation); the scroll also patches the
 *       HDMA tables that drive it, as CT's frames use them
 *   build/host/lc_model hreal   rom.sfc SNAP.BIN...
 *       the same on consecutive real frames (snapshots from a runtime built with
 *       SMKX_SNAP_EVERY=1 and SMKX_SNAP_FROM=n), with no synthetic motion
 *   build/host/lc_model dump    rom.sfc SNAP.BIN prefix
 *   LC_SEED=n changes the noise (unused source bits, the blitter's leftovers).
 *   LC_WORLD=x,y  start the ring model at another world origin (same content,
 *                 another place in the ring); LC_SHIFT=x,y start the moving
 *                 sprites that far from the snapshot's; LC_RIGID=1 prints how
 *                 rigidly the background translated between frames.
 *   LC_CHOOSER=cost|exact  hsim: pick the standing set by an estimated cost (see cw) and not by
 *                 opaque pixels; exact estimates from the pieces. LC_COST=g,b,c,h unit costs in
 *                 microseconds (assumed, not measured), LC_HORIZON=n frames a switch has to pay
 *                 back in (default 32); the run prints the model cost per update.
 *   LC_LAYER_SX / LC_LAYER_SY = a,b,c,d  scroll per frame of BG1-4 in hsim
 *                 (default sx, sy for all); LC_STANDING=all or a layer bit mask
 *                 forces the standing set; LC_LAYERS=1 prints each layer's
 *                 opaque pixels and cells.
 *   LC_OVERLAY=ring  overlay layer pieces are blitted from a ring of their own per
 *                 layer (draw_piece_ring); LC_LWORLD=x,y moves the layers' world origin
 *   LC_INSTANCE=1  hsim: sprite rows as instance packets (call / define of a program);
 *                 2: whole instances, unclipped; LC_PROG_KB=n the program store (default 64);
 *                 LC_PROG_LRU=1 slots instead of a store that is emptied, LC_PROG_POLICY=
 *                 lru (default), random or bypass
 *   LC_PKSTAT=1    with hsim: the packed list's parts, records, sprite program estimate
 *   LC_CACHE_KB=n  the source row cache of n KB (content-addressed rows, see
 *                 below; off by default, every update then builds its rows afresh)
 *   -DLC_MUT_SKEW/FXSR/NFSR/MASK/EXT (blit rows), LC_MUT_DUP/EXPO/SPILL (ring),
 *   LC_MUT_OVERLAY (overlay pieces dropped), LC_MUT_KEY (cache key ignores
 *   colours), LC_MUT_LRSKEW/LRFXSR/LRDUP (layer ring blits) and LC_MUT_PK/PKRANGE
 *   (packed records), LC_MUT_INSTG0 (instance calls) and LC_MUT_SLOT (slot ids) build deliberately wrong
 *   generators: the checks must fail.
 *
 * Not modelled (yet): the DSP's own visibility computation, windows,
 * colour math (frames that use them are reported and skipped), any timing; the
 * standing-set choice weighs opaque pixels, not measured costs; the source row
 * cache has no eviction policy (it is emptied when full) and keys on the visible
 * pixel pattern (overlay layers: their whole row), so its hit rates are a floor;
 * the choice between cache and layer rings is not automatic. blit_run() follows Hatari's source; it has not been compared
 * with Hatari's output or a Falcon. The Videl's pixel shift in eight planes
 * (HSCROLL, the extra group fetched) is assumed to work as on the STE. */
#include "snes_ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MG 1                       /* margin groups each side */
#define GW (PPU_W / 16)            /* window groups per row */
#define BWG (GW + 2 * MG)          /* page groups per row */
#define BWP (BWG * 16)             /* page pixels per row */
#define PG 19                      /* groups a piece row can span: 288 px window plus margin at any shift */
#define MAXP 40                    /* pieces per row: the PPU's 32 sprites per line and up to 4 overlay layers */
#define BLIT_REC 20                /* list bytes per blitter row: src, dst, xcnt, src/dst Y increment,
                                      endmask 1 and 3, skew and flags; the rest (Y count 8, X increments,
                                      endmask 2, HOP/OP) is set once per pass */
#define RUN_REC 4                  /* list bytes per restore run */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint8_t *load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)*size);
    if (!d || fread(d, 1, (size_t)*size, f) != (size_t)*size) { perror(path); exit(2); }
    fclose(f);
    return d;
}

/* ---- one PPU state, owned ------------------------------------------------ */

typedef struct {
    uint8_t vram[65536], cgram[512], oam[PPU_OAM_BYTES], io[0x6000];
    uint16_t bgofs[8], m7[6], m7ofs[2];
    PpuWrite log[256];
    int nlog;
    PpuOamSeg oamseg[PPU_OAMSEG_MAX];
    int noamseg;
    uint8_t wram[131072], sram[8192];
    PpuBus bus;
    const uint8_t *rom;
    char name[64];
} Snap;

/* LC_SMW: LoROM HDMA source map (as ppu_abus_lorom in F030SMW's src/snes_ppu.c) */
static int smw_rules;
static int lc_hirom = 1;            /* F030SMK: the HDMA source bus is HiROM whatever the rules (SMW: LoROM) */
static uint8_t abus_lorom(uint32_t a, void *ctx)
{
    const PpuBus *bus = ctx;
    uint32_t bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) return bus->wram[((bank - 0x7E) << 16) | off];
    if (off >= 0x8000) return bus->rom[(((bank & 0x7F) << 15) | (off & 0x7FFF)) & 0x7FFFF];
    if ((bank & 0x7F) < 0x40) return off < 0x2000 ? bus->wram[off] : 0;
    if ((bank & 0x7F) >= 0x70) return bus->sram[off & 0x7FF];
    return bus->rom[(((bank & 0x7F) << 15) | off) & 0x7FFFF];
}
static const uint8_t *aptr_lorom(uint32_t a, void *ctx, uint32_t *len)
{
    const PpuBus *bus = ctx;
    uint32_t bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) { *len = 0x10000 - off; return bus->wram + (((bank - 0x7E) << 16) | off); }
    if (off >= 0x8000) { *len = 0x10000 - off; return bus->rom + ((((bank & 0x7F) << 15) | (off & 0x7FFF)) & 0x7FFFF); }
    if ((bank & 0x7F) < 0x40) { if (off < 0x2000) { *len = 0x2000 - off; return bus->wram + off; } *len = 0; return 0; }
    if ((bank & 0x7F) >= 0x70) { *len = 0; return 0; }
    *len = 0x8000 - off;
    return bus->rom + ((((bank & 0x7F) << 15) | off) & 0x7FFFF);
}

static Snap *snap_load(const char *path, const uint8_t *rom)
{
    long n;
    uint8_t *d = load(path, &n);
    if (n < 12 || memcmp(d, "SMKXSNAP", 8) || be32(d + 8) != PPU_SNAP_VERSION) {
        fprintf(stderr, "%s: not a version %d SMKX snapshot\n", path, PPU_SNAP_VERSION);
        exit(2);
    }
    Snap *s = calloc(1, sizeof *s);
    const uint8_t *p = d + 12;
    memcpy(s->vram, p, 65536); p += 65536;
    memcpy(s->cgram, p, 512); p += 512;
    memcpy(s->oam, p, PPU_OAM_BYTES); p += PPU_OAM_BYTES;
    memcpy(s->io, p, 0x6000); p += 0x6000;
    for (int i = 0; i < 8; i++) { s->bgofs[i] = be16(p); p += 2; }
    for (int i = 0; i < 6; i++) { s->m7[i] = be16(p); p += 2; }
    for (int i = 0; i < 2; i++) { s->m7ofs[i] = be16(p); p += 2; }
    uint32_t nlog = be32(p); p += 4;
    for (int i = 0; i < 256; i++) {
        s->log[i].line = be16(p); s->log[i].reg = be16(p + 2); s->log[i].value = p[4];
        p += 6;
    }
    s->nlog = (int)(nlog > 256 ? 256 : nlog);
    memcpy(s->wram, p, 131072); p += 131072;
    memcpy(s->sram, p, 8192); p += 8192;
    uint32_t planes = be32(p);
    p += 4 + ppu_snap_frame(planes) + 96 + 48 + 256 + 4 + 2 + 96 + 48 + 256 + 4 + 2;
    uint32_t noamseg = be32(p); p += 4;
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
        s->oamseg[i].line = be16(p);
        memcpy(s->oamseg[i].oam, p + 4, PPU_OAM_BYTES);
        p += 4 + PPU_OAM_BYTES;
    }
    s->noamseg = (int)(noamseg > PPU_OAMSEG_MAX ? PPU_OAMSEG_MAX : noamseg);
    s->bus.wram = s->wram; s->bus.sram = s->sram; s->bus.rom = rom;
    const char *b = strrchr(path, '/');
    const char *b2 = strrchr(path, '\\');
    if (b2 > b) b = b2;
    snprintf(s->name, sizeof s->name, "%s", b ? b + 1 : path);
    free(d);
    return s;
}

static Snap *snap_clone(const Snap *s)
{
    Snap *c = malloc(sizeof *c);
    memcpy(c, s, sizeof *c);
    c->bus.wram = c->wram; c->bus.sram = c->sram;
    return c;
}

static PpuInput snap_input(const Snap *s, const uint8_t *io)
{
    static uint8_t ident[256];
    static uint32_t hist[256];
    for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
    PpuInput in = {s->vram, s->cgram, s->oam, io, s->bgofs, s->m7, s->m7ofs, (smw_rules && !lc_hirom) ? abus_lorom : ppu_abus_hirom, (void *)&s->bus,
                   s->log, s->nlog, ident, hist, NULL, s->oamseg, s->noamseg, (smw_rules && !lc_hirom) ? aptr_lorom : ppu_aptr_hirom, NULL};
    return in;
}

/* ---- row facts from the register replay ---------------------------------- */

typedef struct {
    const uint8_t *oam[PPU_H];
    uint8_t obsel[PPU_H];
    int vis, bad_mode, bad_tm, math, win, mos, sub, tm0, have_tm;
} Rows;

static void row_fn(void *ctx, int row, const PpuRegs *s)
{
    Rows *c = ctx;
    const uint8_t *r = s->r;
    c->oam[row] = s->oam;
    c->obsel[row] = r[0x01];
    if (r[0x00] & 0x80) return;
    c->vis++;
    if ((r[0x05] & 7) > 1) c->bad_mode++;
    int tm = r[0x2C] & 0x1F;
    if (!c->have_tm) { c->tm0 = tm; c->have_tm = 1; }
    else if (tm != c->tm0) c->bad_tm++;
    int mm = (r[0x30] >> 4) & 3, none = mm == 3;
    PpuRegs t = *s;
    if (smw_rules) {
        uint16_t csp[3][2];
        int cn = ppu_win_spans_on(s, 5, 0x3F, csp);
        int cempty = cn == 0, cfull = cn == 1 && csp[0][0] == 0 && csp[0][1] == 256;
        if ((mm == 1 && cempty) || (mm == 2 && cfull)) none = 1;
        if ((mm == 1 && cfull) || (mm == 2 && cempty)) t.r[0x30] &= ~0x30;
    }
    uint32_t sc = ppu_sub_cfg(&t, s->cgram);
    if (!none && (r[0x31] & 0x3F)) {
        int fixed0 = !(t.r[0x30] & 2) && !(r[0x31] & 0x40) && !s->fixed[0] && !s->fixed[1] && !s->fixed[2];
        if (!(smw_rules && (fixed0 || (sc & 0x20000)))) c->math++;
    }
    if ((r[0x06] & 0xF0) && (r[0x06] & 0x0F & tm)) c->mos++;
    if (ppu_win_layers(s)) c->win++;
    if (sc && !(smw_rules && (sc & 0x20000))) c->sub++;
}

/* ---- sprite rasterizer (mirrors snes_ppu.c obj_line) ---------------------- */

static const uint8_t obj_sizes[8][2][2] = {
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

static uint16_t vram_word(const uint8_t *vram, uint32_t word)
{
    word &= 0x7FFF;
    return (uint16_t)(vram[word * 2] | (vram[word * 2 + 1] << 8));
}

/* Per page pixel (X + 16 * MG): owning OAM entry (-1: none) and CGRAM index.
 * Pixels of slivers that touch the window, including the overhang into the
 * margins; the 32 per line and 34 sliver limits as the reference. */
/* cnt (when not NULL): per OAM entry, the opaque pixels it has on the row in the page range,
 * whether it wins them or not (the entry's silhouette). */
static void raster_row(const Snap *s, uint8_t obsel, const uint8_t *oam, int row, int16_t *win, uint8_t *val, uint8_t *cnt)
{
    for (int k = 0; k < BWP; k++) { win[k] = -1; val[k] = 0; }
    uint32_t base = (uint32_t)(obsel & 7) << 13;
    uint32_t gap = (uint32_t)(((obsel >> 3) & 3) + 1) << 12;
    const uint8_t (*sz)[2] = obj_sizes[obsel >> 5];
    int list[32], n = 0;
    for (int i = 0; i < 128 && n < 32; i++) {
        const uint8_t *e = oam + i * 4;
        uint8_t hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int w = sz[hi >> 1][0], h = sz[hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        int dy = (row - e[1]) & 0xFF;
        if (dy >= h) continue;
        if (x <= -w || x >= 256) continue;
        list[n++] = i;
    }
    int slivers = 0;
    for (int k = n - 1; k >= 0; k--) {
        int i = list[k];
        const uint8_t *e = oam + i * 4;
        uint8_t hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int w = sz[hi >> 1][0], h = sz[hi >> 1][1];
        int x = e[0] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        int dy = (row - e[1]) & 0xFF;
        uint8_t attr = e[3];
        if (attr & 0x80) dy = h - 1 - dy;
        int tcol0 = e[2] & 15, trow0 = e[2] >> 4;
        int table = attr & 1;
        int pal = 128 + ((attr >> 1) & 7) * 16;
        for (int col = 0; col < w / 8; col++) {
            int sx = x + col * 8;
            if (sx <= -8 || sx >= 256) continue;
            if (++slivers > 34) return;
            int c = (attr & 0x40) ? (w / 8 - 1 - col) : col;
            int chr = (((trow0 + (dy >> 3)) & 15) << 4) | ((tcol0 + c) & 15);
            uint32_t addr = base + (table ? gap : 0) + (uint32_t)chr * 16;
            uint16_t p01 = vram_word(s->vram, addr + (dy & 7));
            uint16_t p23 = vram_word(s->vram, addr + 8 + (dy & 7));
            for (int px = 0; px < 8; px++) {
                int X = sx + px;
                if (X < -16 * MG || X >= PPU_W + 16 * MG) continue;
                int bit = (attr & 0x40) ? px : 7 - px;
                int v = ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1) |
                        (((p23 >> bit) & 1) << 2) | (((p23 >> (bit + 8)) & 1) << 3);
                if (v) {
                    if (cnt) cnt[i]++;
                    win[X + 16 * MG] = (int16_t)i;
                    val[X + 16 * MG] = (uint8_t)(pal + v);
                }
            }
        }
    }
}

static void oam_hide(uint8_t *oam)
{
    for (int i = 0; i < 128; i++) {
        oam[i * 4] = 0;
        oam[512 + (i >> 2)] |= (uint8_t)(1 << ((i & 3) * 2));
    }
}

/* ---- a frame as the model sees it ------------------------------------------ */

typedef struct {
    int16_t i;                     /* OAM entry */
    int lam0;                      /* linear page group of local group 0 (ring mode) */
    uint8_t sil;                   /* the entry's opaque pixels on the row (silhouette), sprites only */
    const uint8_t *full;           /* overlay layer piece: the layer's own window row (PPU_W pixels),
                                      valid while the frame is being drawn; NULL for sprites */
    int fbase;                     /* local pixel index of window pixel 0 when full is set */
    uint16_t gm[PG];               /* per group: the pixels drawn, bit 15 leftmost */
    uint32_t foot;                 /* groups touched */
    uint32_t h;                    /* signature of position and pixels */
    uint8_t v[PG * 16];            /* the pixels' CGRAM indices */
} Piece;

typedef struct {
    int ok;
    char why[64];
    uint8_t F[PPU_H][PPU_W];
    uint8_t B[PPU_H][PPU_W];
    uint8_t bgw[PPU_H][BWP];       /* the clean background on the page, margins 0 */
    Piece pc[PPU_H][MAXP];
    uint8_t np[PPU_H];
    int mism;                      /* pixels where rasterizer and renderer disagree */
    int pieces, instances;
} Frame;

static uint32_t fnv(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static void frame_build(const Snap *s, Frame *f)
{
    memset(f, 0, sizeof *f);
    PpuInput in = snap_input(s, s->io);
    static Rows rows;
    memset(&rows, 0, sizeof rows);
    ppu_walk_all = 0;
    ppu_replay(&in, row_fn, &rows);
    const char *why = NULL;
    if (!rows.vis) why = "forced blank";
    else if (rows.bad_mode) why = "mode";
    else if (rows.bad_tm) why = "TM varies";
    else if (rows.math) why = "colour math";
    else if (rows.win) why = "windows";
    else if (rows.mos) why = "mosaic";
    else if (rows.sub) why = "sub screen";
    if (why) { snprintf(f->why, sizeof f->why, "%s", why); return; }
    f->ok = 1;

    PpuInput inf = snap_input(s, s->io);
    ppu_render(&inf, &f->F[0][0]);
    /* OBJ off: every OAM entry (in all segments) moved to x = -256, where no
     * sprite can reach the screen. TM cannot be used: CT's HDMA and IRQ
     * handlers write it per line. */
    Snap *t = snap_clone(s);
    oam_hide(t->oam);
    for (int g = 0; g < t->noamseg; g++) oam_hide(t->oamseg[g].oam);
    PpuInput inb = snap_input(t, t->io);
    ppu_render(&inb, &f->B[0][0]);
    free(t);
    const uint8_t *bflat = &f->B[0][0];
    for (int k = 0; k < PPU_H * PPU_W; k++)
        if (bflat[k] >= 128) {
            f->ok = 0;
            snprintf(f->why, sizeof f->why, "BG index >= 128");
            return;
        }

    static uint8_t inst_seen[128];
    memset(inst_seen, 0, sizeof inst_seen);
    for (int y = 0; y < PPU_H; y++) {
        memset(f->bgw[y], 0, BWP);
        memcpy(f->bgw[y] + 16 * MG, f->B[y], PPU_W);
        if (!(rows.tm0 & 0x10)) continue;
        static int16_t win[BWP];
        static uint8_t val[BWP];
        raster_row(s, rows.obsel[y], rows.oam[y], y, win, val, NULL);
        /* which entries show on this row */
        int slot[128];
        for (int i = 0; i < 128; i++) slot[i] = -1;
        for (int k = 0; k < BWP; k++) {
            int X = k - 16 * MG;
            int w = win[k];
            if (w < 0) {
                if (X >= 0 && X < PPU_W && f->F[y][X] != f->B[y][X]) f->mism++;
                continue;
            }
            int show;
            if (X >= 0 && X < PPU_W) {
                show = f->F[y][X] != f->B[y][X];
                if (show && f->F[y][X] != val[k]) f->mism++;
            } else {
                show = 1;                      /* overhang: drawn, never seen */
            }
            if (!show) continue;
            if (slot[w] < 0) {
                if (f->np[y] >= MAXP) { f->mism++; continue; }
                slot[w] = f->np[y]++;
                Piece *p = &f->pc[y][slot[w]];
                memset(p, 0, sizeof *p);
                p->i = (int16_t)w;
                inst_seen[w] = 1;
            }
            Piece *p = &f->pc[y][slot[w]];
            p->gm[k >> 4] |= (uint16_t)(0x8000u >> (k & 15));
            p->v[k] = val[k];
        }
        for (int q = 0; q < f->np[y]; q++) {
            Piece *p = &f->pc[y][q];
            uint32_t h = 2166136261u;
            for (int g = 0; g < BWG; g++) {
                if (!p->gm[g]) continue;
                p->foot |= 1u << g;
                h = fnv(h, (uint32_t)g << 16 | p->gm[g]);
                for (int b = 0; b < 16; b++)
                    if (p->gm[g] & (0x8000u >> b)) h = fnv(h, p->v[g * 16 + b]);
            }
            p->h = h;
            f->pieces++;
        }
    }
    for (int i = 0; i < 128; i++) f->instances += inst_seen[i];
}

/* ---- the pages and the list -------------------------------------------------- */

typedef struct {
    int init;
    uint8_t *pm;                   /* the planar page the blitter writes (PAGEBYTES in `mem`) */
    uint8_t px[PPU_H][BWP];        /* what the page shows, as pixels (the abstract model) */
    uint8_t bg[PPU_H][BWP];        /* clean background the page was last drawn from */
    Piece pc[PPU_H][MAXP];         /* pieces drawn */
    uint8_t np[PPU_H];
} Page;

typedef struct {
    long dirty_groups, bg_groups, restore_runs, blits, blit_groups, pieces, changed, lb_groups, entry_bytes;
    long c_hits, c_misses, c_flushes;     /* source row cache, this update */
    uint32_t c_used;
    long pk_bytes;                 /* the update's list in packed records (and 3 bytes per restore run) */
    int bad;                       /* window pixels different from F after the update */
    int pbad;                      /* planar words different from the pixel page or from ppu_planar8(F) */
} Stats;

/* ---- the planar page and the blitter ------------------------------------------
 *
 * Eight interleaved planes as the Videl reads them: a row is BWG groups of
 * eight big-endian plane words (16 bytes), plane 0 first, pixel 0 of a group in
 * bit 15. `mem` holds two pages and then the scratch where each update builds
 * its sprite source rows (the cache entries of the design; here one per piece
 * and row, not reused).
 *
 * blit_run() is the Atari blitter's word loop as Hatari's blitter.c has it
 * (checked there against a real STE and Falcon, including the NFSR / FXSR
 * cases), for HOP 2 (source) and OP 3 (D = S), which is all the list uses: the
 * 32-bit source shifter and its persistent content, skew, FXSR, NFSR, the three
 * endmasks, the read-modify-write whenever a mask is not all ones, and the word
 * left on the bus. Its state survives from blit to blit as on the hardware and
 * starts from noise, so a list that relies on leftovers shows up as wrong pixels.
 * Source and destination are byte offsets into `mem`. */
#define PSTRIDE (BWG * 16)
#define PAGEBYTES (PPU_H * PSTRIDE)
#define SCRATCH_BYTES (1 << 20)
#define MEMBYTES (2 * PAGEBYTES + SCRATCH_BYTES)
#define SCRATCH_BASE (2 * PAGEBYTES)

static uint32_t memsz = MEMBYTES;
static uint32_t scratch_base = SCRATCH_BASE;
static uint32_t scratch_size = SCRATCH_BYTES;

static uint8_t *mem;
static uint32_t scratch_used;
static uint32_t rng = 0x9E3779B9u;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

typedef struct {
    uint32_t buffer;               /* the source shifter */
    uint16_t bus_word;             /* the last word on the bus */
} BlitHw;

typedef struct {
    uint32_t src, dst;
    uint16_t xcnt, ycnt;
    int16_t sxi, syi, dxi, dyi;
    uint16_t em1, em2, em3;
    uint8_t fxsr, nfsr, skew;
} BlitReg;

static BlitHw hw;

/* ---- the recorder -----------------------------------------------------------
 *
 * LC_REC=file.lcp with hsim/hreal writes what the update did to `mem` as a stream a
 * player can execute: DATA (bytes the DSP would send: source rows, clean and layer
 * ring groups), COPY (the CPU's restores and ring-seam copies inside mem), BLIT (one
 * blitter run's registers), PAL (the 256 palette longwords) and END of a frame (which
 * page, where the Videl starts, HSCROLL, checksums). All big-endian. `lc_model play`
 * executes it on a fresh memory and compares every frame with the recorded
 * checksums (the page window, the displayed pixels from the exact renderer, and the
 * whole of memory); falcon/lcplay.s executes the same stream on the Falcon. */
#define REC_DATA 1
#define REC_COPY 2
#define REC_BLIT 3
#define REC_PAL 4
static uint8_t *rec_buf;
static size_t rec_len, rec_cap;
static int rec_on, rec_in_blit, rec_frames;
static int rec_kind;                   /* the pending op: 0 none, REC_DATA, REC_COPY */
static uint32_t rec_dst, rec_src, rec_n;
static uint8_t rec_pend[1 << 16];

static void rec_put(const void *p, size_t n)
{
    if (rec_len + n > rec_cap) {
        rec_cap = (rec_len + n) * 2 + 4096;
        rec_buf = realloc(rec_buf, rec_cap);
    }
    memcpy(rec_buf + rec_len, p, n);
    rec_len += n;
}
static void rec_u8(unsigned v) { uint8_t b = (uint8_t)v; rec_put(&b, 1); }
static void rec_u16(unsigned v) { uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v}; rec_put(b, 2); }
static void rec_u32(uint32_t v) { rec_u16(v >> 16); rec_u16(v & 0xFFFF); }

static void rec_flush(void)
{
    if (rec_kind == REC_DATA) {
        rec_u16(REC_DATA);
        rec_u32(rec_dst);
        rec_u16(rec_n);
        rec_put(rec_pend, rec_n);
        if (rec_n & 1) rec_u8(0);
    } else if (rec_kind == REC_COPY) {
        rec_u16(REC_COPY);
        rec_u32(rec_dst);
        rec_u32(rec_src);
        rec_u16(rec_n);
    }
    rec_kind = 0;
}

static void rec_data(uint32_t dst, const uint8_t *p, uint32_t n)
{
    if (!rec_on) return;
    if (rec_kind == REC_DATA && dst == rec_dst + rec_n && rec_n + n <= sizeof rec_pend - 2) {
        memcpy(rec_pend + rec_n, p, n);
        rec_n += n;
        return;
    }
    rec_flush();
    rec_kind = REC_DATA;
    rec_dst = dst;
    rec_n = n;
    memcpy(rec_pend, p, n);
}

static void rec_copy(uint32_t dst, uint32_t src, uint32_t n)
{
    if (!rec_on) return;
    if (rec_kind == REC_COPY && dst == rec_dst + rec_n && src == rec_src + rec_n && rec_n + n < 65536) {
        rec_n += n;
        return;
    }
    rec_flush();
    rec_kind = REC_COPY;
    rec_dst = dst;
    rec_src = src;
    rec_n = n;
}

static uint16_t mem_rw(uint32_t a)
{
    if (a + 1 >= memsz) { fprintf(stderr, "blitter read outside memory: %u\n", a); exit(3); }
    return (uint16_t)(mem[a] << 8 | mem[a + 1]);
}

static void mem_ww(uint32_t a, uint16_t v)
{
    if (a + 1 >= memsz) { fprintf(stderr, "blitter write outside memory: %u\n", a); exit(3); }
    mem[a] = (uint8_t)(v >> 8);
    mem[a + 1] = (uint8_t)v;
    if (rec_on && !rec_in_blit) {
        uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
        rec_data(a, b, 2);
    }
}

static uint16_t bus_read(uint32_t a) { hw.bus_word = mem_rw(a); return hw.bus_word; }
static void bus_write(uint32_t a, uint16_t v) { hw.bus_word = v; mem_ww(a, v); }

static void blit_run(const BlitReg *r)
{
    if (rec_on) {
        rec_flush();
        rec_u16(REC_BLIT);
        rec_u32(r->src);
        rec_u32(r->dst);
        rec_u16(r->xcnt);
        rec_u16(r->ycnt);
        rec_u16((uint16_t)r->sxi);
        rec_u16((uint16_t)r->syi);
        rec_u16((uint16_t)r->dxi);
        rec_u16((uint16_t)r->dyi);
        rec_u16(r->em1);
        rec_u16(r->em2);
        rec_u16(r->em3);
        rec_u8(r->fxsr);
        rec_u8(r->nfsr);
        rec_u8(r->skew);
        rec_u8(0);
        rec_in_blit++;
    }
    uint32_t src = r->src, dst = r->dst;
    uint32_t x = r->xcnt, y = r->ycnt, xreset = r->xcnt;
    uint8_t st_nfsr = 0, st_fxsr = 0, have_fxsr = 0;
    while (y > 0) {
        int first = x == xreset;
        uint16_t mask;
        if (first || xreset == 1) mask = r->em1;
        else if (x == 1) mask = r->em3;
        else mask = r->em2;
        if (first) { st_nfsr = 0; st_fxsr = r->fxsr; }
        int need_dst = mask != 0xFFFF, fetch_src = 0;
        uint16_t dstw = 0;
        if (st_fxsr && !have_fxsr) {
            hw.buffer <<= 16;
            hw.buffer |= bus_read(src);
            src += (uint32_t)(int32_t)r->sxi;
            have_fxsr = 1;
        }
        if (!st_nfsr) {
            hw.buffer <<= 16;
            hw.buffer |= bus_read(src);
            fetch_src = 1;
        }
        if (need_dst) dstw = bus_read(dst);
        if (r->nfsr && x == 1) { hw.buffer <<= 16; hw.buffer |= hw.bus_word; }
        uint16_t lop = (uint16_t)(hw.buffer >> r->skew);
        uint16_t out = mask != 0xFFFF ? (uint16_t)((lop & mask) | (dstw & ~mask)) : lop;
        bus_write(dst, out);
        if (r->nfsr && x == 1) { hw.buffer <<= 16; hw.buffer |= hw.bus_word; }
        if (x == 2 && r->nfsr) st_nfsr = 1;
        if (fetch_src) {
            if (x == 1 || st_nfsr) src += (uint32_t)(int32_t)r->syi;
            else src += (uint32_t)(int32_t)r->sxi;
        }
        if (x == 1) {
            have_fxsr = 0;
            y--;
            x = xreset;
            dst += (uint32_t)(int32_t)r->dyi;
        } else {
            x--;
            dst += (uint32_t)(int32_t)r->dxi;
        }
    }
    if (rec_on) rec_in_blit--;
}

/* Plane words of a row of pixels, as the page holds them. */
static void pack_row(const uint8_t *px, uint8_t *out)
{
    for (int g = 0; g < BWG; g++)
        for (int p = 0; p < 8; p++) {
            unsigned w = 0;
            for (int b = 0; b < 16; b++) w |= (unsigned)((px[g * 16 + b] >> p) & 1) << (15 - b);
            out[g * 16 + p * 2] = (uint8_t)(w >> 8);
            out[g * 16 + p * 2 + 1] = (uint8_t)w;
        }
}

/* A row's blitter runs: maximal groups ga..gb of non-zero masks where the
 * groups between the first and the last are full (the middle endmask is one
 * constant). ng groups. Returns the count. */
static int decompose(const uint16_t *m, int ng, int *ga, int *gb)
{
    int nb = 0, g = 0;
    while (g < ng) {
        if (!m[g]) { g++; continue; }
        int end = g;
        while (end + 1 < ng && m[end + 1]) {
            if (end > g && m[end] != 0xFFFF) break;   /* would become an inner group: must be full */
            end++;
        }
        ga[nb] = g;
        gb[nb] = end;
        nb++;
        g = end + 1;
    }
    return nb;
}

static int count_blits(const uint16_t *m, int ng, int *groups)
{
    int ga[PG], gb[PG];
    int nb = decompose(m, ng, ga, gb);
    for (int i = 0; i < nb; i++) *groups += gb[i] - ga[i] + 1;
    return nb;
}

/* ---- the source row cache -------------------------------------------------------
 *
 * A source row (what draw_piece_planar() builds for a piece row: its visible span
 * unshifted, W words per plane) is content-addressed: the key is the pixel pattern
 * of the span, invisible pixels as 0 (visible ones are never 0), so the same row
 * found again at another x, another frame or another sprite slot is the same entry.
 * The shift is the blitter's skew and the masks are in the blit record, so none of
 * that is part of the entry. A miss means the DSP sends the row (16 * W bytes) and
 * the CPU stores it; a hit sends nothing.
 *
 * LC_CACHE_KB=n switches it on (off: every update builds its rows afresh, as before)
 * with n KB of entries. The region is a bump allocator that is emptied at the start
 * of an update when less than a quarter of it is left (and inside an update only
 * when a row does not fit), which is what a simple implementation would do; there
 * is no eviction policy, so the hit rates are a floor. The DSP would keep the table
 * of keys and addresses. */
static int cache_on;
static uint32_t cache_cap, cache_reserve, cache_used;
typedef struct { uint64_t key; uint32_t addr; uint16_t W; uint8_t used; } CEnt;
#define CTAB (1 << 15)
static CEnt ctab[CTAB];
static int cache_entries;
static struct { long hits, misses, miss_bytes, flushes; uint32_t peak; } cs;

static void prog_flush(void);
static int cache_epoch;

static void cache_flush(void)
{
    memset(ctab, 0, sizeof ctab);
    cache_used = 0;
    cache_entries = 0;
    cs.flushes++;
    cache_epoch++;
    prog_flush();         /* a program points at entries: it cannot outlive them */
}

static void cache_init(void)
{
    const char *kb = getenv("LC_CACHE_KB");
    cache_cap = kb ? (uint32_t)strtoul(kb, NULL, 0) * 1024 : 0;
    if (cache_cap > scratch_size) cache_cap = scratch_size;
    cache_reserve = cache_cap / 4;
    cache_on = cache_cap != 0;
    memset(ctab, 0, sizeof ctab);
    cache_used = 0;
    cache_entries = 0;
    memset(&cs, 0, sizeof cs);
}

static void cache_frame_start(void)
{
    if (cache_on && cache_used + cache_reserve > cache_cap) cache_flush();
}

static int cache_lookup(uint64_t key, int W, uint32_t *addr)
{
    for (uint32_t i = (uint32_t)key & (CTAB - 1);; i = (i + 1) & (CTAB - 1)) {
        if (!ctab[i].used) return 0;
        if (ctab[i].key == key && ctab[i].W == W) {
            *addr = scratch_base + ctab[i].addr;
            cs.hits++;
            return 1;
        }
    }
}

static uint32_t cache_alloc(uint64_t key, int W)
{
    uint32_t z = (uint32_t)(16 * W);
    if (cache_used + z > cache_cap || cache_entries >= CTAB * 3 / 4) cache_flush();
    uint32_t i = (uint32_t)key & (CTAB - 1);
    while (ctab[i].used) i = (i + 1) & (CTAB - 1);
    ctab[i].key = key;
    ctab[i].W = (uint16_t)W;
    ctab[i].addr = cache_used;
    ctab[i].used = 1;
    cache_entries++;
    uint32_t addr = scratch_base + cache_used;
    cache_used += z;
    if (cache_used > cs.peak) cs.peak = cache_used;
    cs.misses++;
    cs.miss_bytes += z;
    return addr;
}

/* The groups the last draw_piece_planar() call wrote through blitter runs, as
 * linear group indices (lam0 + local index, not reduced: a run may cross the
 * end of a ring into its duplicate). */
static int touched[PG];
static int ntouched;

/* ---- the list records, packed -------------------------------------------------
 *
 * One packet per piece row, the blitter runs of that row in it. What the DSP
 * sends and the CPU's loop reads (a routine per record kind, picked by the first
 * byte, as in Chorensha's jump displacements; the loop derives the increments and
 * the flags from the run's size and the row's skew, which is cheap):
 *
 *   entry row   byte  kind 0 | runs - 1
 *               16    lam0, the page's linear group of the row's first local group
 *               16    index of the source row (16-byte units from the cache base)
 *               16    W (5) | gd0 (5) | skew (4) | 2 spare: words per plane of the
 *                     source row, the first destination group it covers, p0 & 15
 *   ring row    byte  kind 1 | runs - 1, or kind 2 | layer | runs - 1 (continue)
 *               16    lam0                   (full form only)
 *               16    ring group of the source for local group 0, adjusted
 *                     for the shift          (full form only)
 *               byte  layer (2) | r (4)      (full form only)
 *               A continuation stands for the layer's next row: lam0 and the
 *               source group both advance by one ring row, the skew stays.
 *   each run    16    a (5) | n (5) | em1 is not full (1) | em3 is not full (1)
 *                     then em1 and em3 (16 each) when they are not full
 *
 * `a` is the run's first destination group, `n` its length. A mask that is full
 * is not sent (an inner run of a dense layer mostly has full ends at most on one
 * side); em3 is only sent for a run of two or more groups or one the single-word
 * NFSR case widens. Every packet is written to the update's stream, read back
 * from the bytes the way the CPU would, its registers derived again, and compared
 * with the ones that were executed. */
#define PK_ENTRY 0
#define PK_RING 1
typedef struct {
    int kind, nruns, layer, W, gd0, s, r;
    uint32_t lam0, idx;
    int a[PG], n[PG];
    uint16_t em1[PG], em3[PG];
} Packet;

static uint8_t pk[1 << 21];
static uint32_t pk_len;
static long pk_total[2];          /* bytes written, by kind (cumulative) */
static long pk_stat[8];           /* ring packets, runs, em1 words, em3 words, full headers, continuations */

/* what the next ring row of a layer continues from, on the writing side and on
 * the reading side (they are updated alike) */
typedef struct { int valid; uint32_t lam0, base; int r; } RingState;
static RingState enc_rs[4], dec_rs[4];

/* what each entry row of the update looked like, for the sprite program estimate:
 * owner (OAM entry), the row's first group, its record's length and a hash of the
 * record without that group */
typedef struct { int owner; uint32_t lam0, len; int gd0; uint64_t h; } EntLog;
static EntLog elog[8192];
static int nelog, pk_owner;

static void pk_reset(void)
{
    nelog = 0;
    pk_len = 0;
    memset(enc_rs, 0, sizeof enc_rs);
    memset(dec_rs, 0, sizeof dec_rs);
}

static void pk_put16(uint32_t v)
{
    if (pk_len + 2 > sizeof pk) { fprintf(stderr, "record stream full\n"); exit(3); }
    pk[pk_len++] = (uint8_t)(v >> 8);
    pk[pk_len++] = (uint8_t)v;
}

static uint32_t pk_get16(const uint8_t **p)
{
    uint32_t v = (uint32_t)((*p)[0] << 8 | (*p)[1]);
    *p += 2;
    return v;
}

static int reg_eq(const BlitReg *x, const BlitReg *y)
{
    return x->src == y->src && x->dst == y->dst && x->xcnt == y->xcnt && x->ycnt == y->ycnt && x->sxi == y->sxi &&
           x->syi == y->syi && x->dxi == y->dxi && x->dyi == y->dyi && x->em1 == y->em1 && x->em2 == y->em2 &&
           x->em3 == y->em3 && x->fxsr == y->fxsr && x->nfsr == y->nfsr && x->skew == y->skew;
}

/* The single-word NFSR case: a run of one group that holds only the tail of the
 * source row is widened to its left. */
static int entry_widens(int s, int gd0, int W, int a, int b)
{
#ifdef LC_MUT_EXT
    (void)s; (void)gd0; (void)W; (void)a; (void)b;
    return 0;
#else
    return s > 0 && b - gd0 == W && a == b;
#endif
}

/* The blitter registers of one run of an entry row: run groups a..b, masks as
 * the row's clipped masks give them. *a_eff: the run's first group after the
 * widening. */
static void entry_run_regs(uint32_t page, uint32_t lam0, uint32_t ent, int W, int gd0, int s, int a, int b,
                           uint16_t em1, uint16_t em3, BlitReg *r, int *a_eff)
{
    if (entry_widens(s, gd0, W, a, b)) { a--; em1 = 0; }
    int n = b - a + 1, jj_a = a - gd0, jj_b = b - gd0;
    if (n == 1) em3 = 0xFFFF;                /* not used: one word uses endmask 1 */
    int fxsr = 0, nfsr = 0, first = jj_a, nreads = n;
    if (s > 0) {
        fxsr = jj_a > 0;
        nfsr = jj_b == W;
        first = fxsr ? jj_a - 1 : jj_a;
        nreads = n + fxsr - nfsr;
    }
    r->src = ent + (uint32_t)(first * 2);
    r->dst = page + (lam0 + (uint32_t)a) * 16;
    r->xcnt = (uint16_t)n;
    r->ycnt = 8;
    r->sxi = 2;
    r->syi = (int16_t)(2 * (W - nreads + 1));
    r->dxi = 16;
    r->dyi = (int16_t)(2 - 16 * (n - 1));
    r->em1 = em1;
    r->em2 = 0xFFFF;
    r->em3 = em3;
    r->fxsr = (uint8_t)fxsr;
    r->nfsr = (uint8_t)nfsr;
    r->skew = (uint8_t)s;
#ifdef LC_MUT_SKEW
    r->skew = (uint8_t)((s + 1) & 15);
#endif
#ifdef LC_MUT_FXSR
    r->fxsr = 0;
#endif
#ifdef LC_MUT_NFSR
    r->nfsr = 0;
#endif
#ifdef LC_MUT_MASK
    r->em3 = 0xFFFF;
#endif
    *a_eff = a;
}

/* A mask that is one run of ones (any position and length). */
static int mask_is_range(uint16_t m)
{
    if (!m) return 0;
    while (!(m & 1)) m >>= 1;
    return (m & (m + 1)) == 0;
}

/* A mask as sent: 0 full (nothing), 1 one run of ones (a byte: leftmost pixel
 * << 4 | length - 1), 2 any other (16 bits). */
static int mask_type(uint16_t m)
{
    if (m == 0xFFFF) return 0;
    return mask_is_range(m) ? 1 : 2;
}

static void mask_put(int type, uint16_t m)
{
    if (type == 1) {
        int p = 0;
        while (!(m & (0x8000u >> p))) p++;
        int l = 0;
        while (p + l < 16 && (m & (0x8000u >> (p + l)))) l++;
#ifdef LC_MUT_PKRANGE
        l = l > 1 ? l - 1 : l;               /* a range one pixel short */
#endif
        pk[pk_len++] = (uint8_t)(p << 4 | (l - 1));
    } else if (type == 2) {
        pk_put16(m);
    }
}

static uint16_t mask_get(int type, const uint8_t **p)
{
    if (type == 0) return 0xFFFF;
    if (type == 2) return (uint16_t)pk_get16(p);
    int pp = **p >> 4, l = (**p & 15) + 1;
    (*p)++;
    return (uint16_t)(((1u << l) - 1) << (16 - pp - l));
}

/* The runs of a row: one byte each (adjacent to the previous run | type of em1
 * (2) | type of em3 (2) | n - 1, 7 meaning a byte with n follows), a byte with a
 * when the run does not start right after the previous one, then the masks. */
static void pk_runs_put(const Packet *k, int widen_possible)
{
    for (int i = 0; i < k->nruns; i++) {
        int a = k->a[i], n = k->n[i], b = a + n - 1;
        int adj = i > 0 && a == k->a[i - 1] + k->n[i - 1];
        int has3 = n > 1 || (widen_possible && entry_widens(k->s, k->gd0, k->W, a, b));
        int t1 = mask_type(k->em1[i]), t3 = has3 ? mask_type(k->em3[i]) : 0;
#ifdef LC_MUT_PK
        t3 = 0;                              /* the last mask is never sent */
#endif
        int nc = n - 1 < 7 ? n - 1 : 7;
        pk[pk_len++] = (uint8_t)(adj << 7 | t1 << 5 | t3 << 3 | nc);
        if (!adj) pk[pk_len++] = (uint8_t)a;
        if (nc == 7) pk[pk_len++] = (uint8_t)n;
        mask_put(t1, k->em1[i]);
        mask_put(t3, k->em3[i]);
        pk_stat[1]++;
        pk_stat[2] += t1 != 0;
        pk_stat[6] += t1 == 1;
        pk_stat[3] += t3 != 0;
        pk_stat[7] += t3 == 1;
    }
}

static void pk_runs_get(const uint8_t **p, Packet *d)
{
    for (int i = 0; i < d->nruns; i++) {
        uint8_t b0 = *(*p)++;
        int adj = b0 >> 7, t1 = (b0 >> 5) & 3, t3 = (b0 >> 3) & 3, nc = b0 & 7;
        d->a[i] = adj ? d->a[i - 1] + d->n[i - 1] : *(*p)++;
        d->n[i] = nc == 7 ? *(*p)++ : nc + 1;
        d->em1[i] = mask_get(t1, p);
        d->em3[i] = mask_get(t3, p);
    }
}

/* Writes the packet of an entry row, reads it back and checks the registers. */
static void pk_check_entry(uint32_t page, const Packet *k, const BlitReg *exec)
{
    uint32_t start = pk_len;
    pk[pk_len++] = (uint8_t)((k->nruns - 1) & 15);
    pk_put16(k->lam0);
    pk_put16(k->idx);
    pk_put16((uint32_t)(k->W << 11 | k->gd0 << 6 | k->s << 2));
    pk_runs_put(k, 1);
    pk_total[PK_ENTRY] += pk_len - start;
    if (nelog < 8192) {
        /* the row without where it is: runs relative to the row's first group */
        uint64_t h = 1469598103934665603ull;
#define MIXH(v) h = (h ^ (uint64_t)(uint32_t)(v)) * 1099511628211ull
        MIXH(k->W);
        MIXH(k->s);
        MIXH(k->idx);
        MIXH(k->nruns);
        for (int i = 0; i < k->nruns; i++) {
            MIXH(k->a[i] - k->gd0);
            MIXH(k->n[i]);
            MIXH(k->em1[i]);
            MIXH(k->em3[i]);
        }
#undef MIXH
        elog[nelog].owner = pk_owner;
        elog[nelog].lam0 = k->lam0;
        elog[nelog].gd0 = k->gd0;
        elog[nelog].len = pk_len - start;
        elog[nelog].h = h;
        nelog++;
    }
    /* the CPU's side, from the bytes alone */
    const uint8_t *p = pk + start;
    Packet d;
    memset(&d, 0, sizeof d);
    d.nruns = (p[0] & 15) + 1;
    p++;
    d.lam0 = pk_get16(&p);
    d.idx = pk_get16(&p);
    uint32_t h = pk_get16(&p);
    d.W = (int)(h >> 11);
    d.gd0 = (int)((h >> 6) & 31);
    d.s = (int)((h >> 2) & 15);
    pk_runs_get(&p, &d);
    if (p != pk + pk_len) { fprintf(stderr, "record has a different length than written\n"); exit(3); }
    for (int i = 0; i < d.nruns; i++) {
        BlitReg r;
        int a_eff;
        entry_run_regs(page, d.lam0, scratch_base + d.idx * 16, d.W, d.gd0, d.s, d.a[i], d.a[i] + d.n[i] - 1, d.em1[i],
                       d.em3[i], &r, &a_eff);
        if (!reg_eq(&r, &exec[i])) { fprintf(stderr, "a record does not give the registers that were executed\n"); exit(3); }
    }
}

/* Draws the piece's clipped masks m[] (ng groups whose first is linear group
 * lam0 of the page at byte offset page) with blitter runs. The source is the
 * piece's row as a cache entry would hold it: its visible span from the
 * leftmost visible pixel p0, unshifted, W words per plane, plane after plane;
 * what is not visible is noise. The shift is the blitter's skew s = p0 & 15.
 * Destination word j of a run takes the tail of source word j-1 and the head of
 * word j (FXSR reads the word before the run's first source word; NFSR drops
 * the read past the last one when the run's last word holds only a tail). A
 * run that would be one word with NFSR (the blitter's "weird" single-word case)
 * is widened by a word to its left with a zero mask. */
/* One drawn entry row: its packet fields and the registers that were executed. */
typedef struct {
    Packet pkt;
    BlitReg regs[PG];
} RowOut;

/* With ro set the row's record is handed back (for an instance packet) instead of
 * being written as a packet of its own. */
static void draw_piece_planar_ex(uint32_t page, uint32_t lam0, int ng, const Piece *p, const uint16_t *m, Stats *st,
                                 RowOut *ro)
{
    int p0 = -1, p1 = -1;
    for (int k = 0; k < ng * 16; k++)
        if (p->gm[k >> 4] & (0x8000u >> (k & 15))) { if (p0 < 0) p0 = k; p1 = k; }
    /* an overlay layer's entry is the layer's whole window row, whatever is visible of it:
     * it then does not change when what lies under the layer does */
    int full = cache_on && p->full != NULL;
    if (full) { p0 = p->fbase; p1 = p0 + PPU_W - 1; }
    int w = p1 - p0 + 1, W = (w + 15) / 16, s = p0 & 15, gd0 = p0 >> 4;
    uint32_t ent = 0;
    uint64_t key = 0;
    int hit = 0;
    if (cache_on) {
        key = 1469598103934665603ull;
        key = (key ^ (uint64_t)w) * 1099511628211ull;
        for (int q = 0; q < w; q++) {
            int ka = p0 + q;
            uint8_t v = full ? p->full[q] : (p->gm[ka >> 4] & (0x8000u >> (ka & 15))) ? p->v[ka] : 0;
#ifdef LC_MUT_KEY
            v = v != 0;                   /* the shape only: other colours would hit */
#endif
            key = (key ^ v) * 1099511628211ull;
        }
        hit = cache_lookup(key, W, &ent);
    }
    if (!hit) {
        if (cache_on) ent = cache_alloc(key, W);
        else {
            ent = scratch_base + scratch_used;
            scratch_used += (uint32_t)(8 * W * 2);
            if (scratch_used > scratch_size) { fprintf(stderr, "scratch full\n"); exit(3); }
        }
        st->entry_bytes += 8 * W * 2;
        for (int pl = 0; pl < 8; pl++)
            for (int k = 0; k < W; k++) {
                unsigned word = 0;
                for (int b = 0; b < 16; b++) {
                    int q = k * 16 + b, bit;
                    if (q < w) {
                        int ka = p0 + q;
                        bit = full ? (p->full[q] >> pl) & 1
                                   : (p->gm[ka >> 4] & (0x8000u >> (ka & 15))) ? (p->v[ka] >> pl) & 1 : (int)(rnd() & 1);
                    } else bit = (int)(rnd() & 1);
                    word |= (unsigned)bit << (15 - b);
                }
                mem_ww(ent + (uint32_t)(pl * W * 2 + k * 2), (uint16_t)word);
            }
    } else {
        /* the entry must hold every visible pixel of this row (a wrong hit would show) */
        for (int pl = 0; pl < 8; pl++)
            for (int k = 0; k < W; k++) {
                unsigned have = mem_rw(ent + (uint32_t)(pl * W * 2 + k * 2));
                for (int b = 0; b < 16; b++) {
                    int q = k * 16 + b, ka = p0 + q;
                    if (q < w && (p->gm[ka >> 4] & (0x8000u >> (ka & 15))) &&
                        ((have >> (15 - b)) & 1) != (unsigned)((p->v[ka] >> pl) & 1)) {
                        fprintf(stderr, "cache entry does not hold the row\n");
                        exit(3);
                    }
                }
            }
    }
    int ga[PG], gb[PG];
    int nb = decompose(m, ng, ga, gb);
    ntouched = 0;
    pk_owner = p->i;
    if (ro) ro->pkt.nruns = 0;
    if (!nb) return;                       /* nothing to draw: no record */
    Packet pkt;
    BlitReg regs[PG];
    memset(&pkt, 0, sizeof pkt);
    pkt.kind = PK_ENTRY;
    pkt.nruns = nb;
    pkt.lam0 = lam0;
    if (ent < scratch_base || (ent - scratch_base) % 16 || (ent - scratch_base) / 16 > 0xFFFF) {
        fprintf(stderr, "source row outside what a record can address\n");
        exit(3);
    }
    pkt.idx = (ent - scratch_base) / 16;
    pkt.W = W;
    pkt.gd0 = gd0;
    pkt.s = s;
    for (int i = 0; i < nb; i++) {
        int a = ga[i], b = gb[i];
        for (int k = a + 1; k < b; k++)
            if (m[k] != 0xFFFF) { fprintf(stderr, "inner group not full\n"); exit(3); }
        pkt.a[i] = a;
        pkt.n[i] = b - a + 1;
        pkt.em1[i] = m[a];
        pkt.em3[i] = m[b];
        int a_eff;
        entry_run_regs(page, lam0, ent, W, gd0, s, a, b, m[a], m[b], &regs[i], &a_eff);
        blit_run(&regs[i]);
        for (int k = a_eff; k <= b; k++) touched[ntouched++] = (int)(lam0 + (uint32_t)k);
    }
    if (ro) {
        ro->pkt = pkt;
        memcpy(ro->regs, regs, sizeof(BlitReg) * (size_t)nb);
    } else {
        pk_check_entry(page, &pkt, regs);
    }
}

static void draw_piece_planar(uint32_t page, uint32_t lam0, int ng, const Piece *p, const uint16_t *m, Stats *st)
{
    draw_piece_planar_ex(page, lam0, ng, p, m, st, NULL);
}

static void page_update(Page *pg, const Frame *f, Stats *st, int full)
{
    memset(st, 0, sizeof *st);
    scratch_used = 0;
    pk_reset();
    cache_frame_start();
    long c_h0 = cs.hits, c_m0 = cs.misses, c_f0 = cs.flushes;
    static uint8_t bgp[PAGEBYTES];                 /* the clean background, planar */
    for (int y = 0; y < PPU_H; y++) pack_row(f->bgw[y], bgp + y * PSTRIDE);
    for (int y = 0; y < PPU_H; y++) {
        /* what a pixel comparison would need: window groups that differ */
        for (int g = 0; g < GW; g++)
            if (memcmp(pg->px[y] + (g + MG) * 16, f->F[y] + g * 16, 16)) st->lb_groups++;
        uint32_t dirty = 0, bgd = 0;
        if (full || !pg->init) {
            dirty = bgd = (1u << BWG) - 1;
        } else {
            for (int g = 0; g < BWG; g++)
                if (memcmp(pg->bg[y] + g * 16, f->bgw[y] + g * 16, 16)) bgd |= 1u << g;
            dirty = bgd;
            const Piece *o = pg->pc[y], *n = f->pc[y];
            int no = pg->np[y], nn = f->np[y];
            for (int a = 0; a < no; a++) {
                int b = 0;
                while (b < nn && n[b].i != o[a].i) b++;
                if (b == nn) {
#ifndef LC_MUTATE
                    dirty |= o[a].foot;
#endif
                    st->changed++;
                }
                else if (n[b].h != o[a].h) { dirty |= o[a].foot | n[b].foot; st->changed++; }
            }
            for (int b = 0; b < nn; b++) {
                int a = 0;
                while (a < no && o[a].i != n[b].i) a++;
                if (a == no) { dirty |= n[b].foot; st->changed++; }
            }
        }
        /* restore */
        int inrun = 0;
        for (int g = 0; g < BWG; g++) {
            if (dirty & (1u << g)) {
                memcpy(pg->px[y] + g * 16, f->bgw[y] + g * 16, 16);
                memcpy(pg->pm + y * PSTRIDE + g * 16, bgp + y * PSTRIDE + g * 16, 16);
                st->dirty_groups++;
                if (!inrun) { st->restore_runs++; inrun = 1; }
            } else inrun = 0;
        }
        for (int g = 0; g < BWG; g++) st->bg_groups += (bgd >> g) & 1;
        /* draw every piece that touches a dirty group, clipped to the dirty groups */
        for (int q = 0; q < f->np[y]; q++) {
            const Piece *p = &f->pc[y][q];
            if (!(p->foot & dirty)) continue;
            uint16_t m[BWG];
            for (int g = 0; g < BWG; g++) m[g] = (dirty & (1u << g)) ? p->gm[g] : 0;
            int gr = 0;
            st->blits += count_blits(m, BWG, &gr);
            st->blit_groups += gr;
            st->pieces++;
            for (int g = 0; g < BWG; g++)
                for (int b = 0; b < 16; b++)
                    if (m[g] & (0x8000u >> b)) pg->px[y][g * 16 + b] = p->v[g * 16 + b];
            draw_piece_planar((uint32_t)(pg->pm - mem), (uint32_t)(y * BWG), BWG, p, m, st);
        }
        memcpy(pg->bg[y], f->bgw[y], BWP);
        memcpy(pg->pc[y], f->pc[y], sizeof(Piece) * f->np[y]);
        pg->np[y] = f->np[y];
        for (int x = 0; x < PPU_W; x++)
            if (pg->px[y][x + 16 * MG] != f->F[y][x]) st->bad++;
    }
    /* the planar page against the pixel page (margins too) and against the
     * exact renderer's own packing of F (the window) */
    static uint16_t fp[PPU_H * PPU_W / 2];
    ppu_planar8(&f->F[0][0], fp);
    for (int y = 0; y < PPU_H; y++) {
        uint8_t row[PSTRIDE];
        pack_row(pg->px[y], row);
        for (int i = 0; i < PSTRIDE; i++)
            if (row[i] != pg->pm[y * PSTRIDE + i]) st->pbad++;
        for (int g = 0; g < GW; g++)
            for (int pl = 0; pl < 8; pl++) {
                const uint8_t *b = pg->pm + y * PSTRIDE + (g + MG) * 16 + pl * 2;
                if ((uint16_t)(b[0] << 8 | b[1]) != fp[y * 128 + g * 8 + pl]) st->pbad++;
            }
    }
    st->c_hits = cs.hits - c_h0;
    st->c_misses = cs.misses - c_m0;
    st->c_flushes = cs.flushes - c_f0;
    st->c_used = cache_used;
    st->pk_bytes = (long)pk_len + 3 * st->restore_runs;
    pg->init = 1;
}

/* ---- drivers ------------------------------------------------------------------ */

typedef struct { long n, bad, pbad, dirty, bg, runs, blits, bgroups, changed, lb, pieces, bytes, entry, chits, cmiss, cfl, pkb; } Sum;

static void sum_add(Sum *t, const Stats *s)
{
    t->n++;
    t->bad += s->bad; t->pbad += s->pbad; t->entry += s->entry_bytes; t->dirty += s->dirty_groups; t->bg += s->bg_groups; t->runs += s->restore_runs;
    t->blits += s->blits; t->bgroups += s->blit_groups; t->changed += s->changed; t->lb += s->lb_groups;
    t->pieces += s->pieces;
    t->chits += s->c_hits; t->cmiss += s->c_misses; t->cfl += s->c_flushes; t->pkb += s->pk_bytes;
    t->bytes += s->blits * BLIT_REC + s->restore_runs * RUN_REC;
}

static void print_header(void)
{
    printf("%-12s %5s %5s %6s %6s %6s %6s %6s %6s %6s %6s %s\n", "frame", "bad", "pbad", "lbGrp", "dirty",
           "bgGrp", "runs", "blits", "blkGrp", "piece", "bytes", "");
}

static void print_stats(const char *name, const Stats *s, const char *note)
{
    printf("%-12s %5d %5d %6ld %6ld %6ld %6ld %6ld %6ld %6ld %6ld %s\n", name, s->bad, s->pbad, s->lb_groups,
           s->dirty_groups, s->bg_groups, s->restore_runs, s->blits, s->blit_groups, s->pieces,
           s->blits * BLIT_REC + s->restore_runs * RUN_REC, note);
}

static void print_sum(const char *what, const Sum *t)
{
    if (!t->n) return;
    double n = (double)t->n;
    printf("%s: %ld updates, mean per update: lower bound %.0f groups, dirty %.0f (bg %.0f), restore runs %.0f,"
           " blitter rows %.0f, list bytes %.0f, source rows built %.0f bytes; wrong pixels %ld, wrong planar bytes %ld\n",
           what, t->n, t->lb / n, t->dirty / n, t->bg / n, t->runs / n, t->blits / n, t->bytes / n, t->entry / n,
           t->bad, t->pbad);
    printf("  list in packed records: %.0f bytes per update (against %.0f)\n", t->pkb / n, t->bytes / n);
    if (cache_on)
        printf("  source row cache %u KB: %.1f rows per update, %.1f%% hits, %.0f new bytes per update, peak %u KB, %ld flushes (all updates)\n",
               cache_cap / 1024, (double)(t->chits + t->cmiss) / n,
               t->chits + t->cmiss ? 100.0 * (double)t->chits / (double)(t->chits + t->cmiss) : 0.0,
               (double)t->entry / n, cs.peak / 1024, cs.flushes);
}

static Frame *fr[2];
static Page pages[2];

static void fr_init(void)
{
    fr[0] = malloc(sizeof(Frame));
    fr[1] = malloc(sizeof(Frame));
    mem = calloc(1, MEMBYTES);
    pages[0].pm = mem;
    pages[1].pm = mem + PAGEBYTES;
    const char *seed = getenv("LC_SEED");
    rng = 0x9E3779B9u ^ (seed ? (uint32_t)strtoul(seed, NULL, 0) * 2654435761u : 0);
    if (!rng) rng = 1;
    hw.buffer = rnd();          /* the hardware's leftovers */
    hw.bus_word = (uint16_t)rnd();
    cache_init();
}

static int cmd_census(const uint8_t *rom, int argc, char **argv)
{
    fr_init();
    int ok = 0, skipped = 0;
    long tp = 0, ti = 0, tb = 0, tbytes = 0, mism = 0, bad = 0;
    printf("%-14s %3s %5s %5s %6s %6s %6s %6s  %s\n", "snapshot", "ok", "inst", "piece", "blits", "fullGr",
           "bytes", "mism", "reason");
    for (int a = 0; a < argc; a++) {
        Snap *s = snap_load(argv[a], rom);
        Frame *f = fr[0];
        frame_build(s, f);
        if (!f->ok) {
            printf("%-14s %3s %5s %5s %6s %6s %6s %6s  %s\n", s->name, "no", "", "", "", "", "", "", f->why);
            skipped++;
            free(s);
            continue;
        }
        Page *pg = &pages[0];
        memset(pg, 0, sizeof *pg);
        pg->pm = mem;
        memset(pg->pm, 0, PAGEBYTES);
        Stats st;
        page_update(pg, f, &st, 1);       /* cold page: everything */
        printf("%-14s %3s %5d %5d %6ld %6ld %6ld %6d\n", s->name, "yes", f->instances, f->pieces, st.blits,
               st.dirty_groups, st.blits * BLIT_REC + st.restore_runs * RUN_REC, f->mism);
        ok++; ti += f->instances; tp += f->pieces; tb += st.blits; tbytes += st.blits * BLIT_REC + st.restore_runs * RUN_REC;
        mism += f->mism;
        if (st.bad || st.pbad) { printf("  %d wrong pixels after the cold update\n", st.bad); bad++; }
        free(s);
    }
    printf("%d frames modelled, %d skipped; mean instances %.1f, pieces %.0f, blitter rows %.0f (cold, all sprites),"
           " list bytes %.0f; rasterizer/renderer disagreements %ld pixels\n",
           ok, skipped, ok ? (double)ti / ok : 0, ok ? (double)tp / ok : 0, ok ? (double)tb / ok : 0,
           ok ? (double)tbytes / ok : 0, mism);
    return bad != 0;
}

static int cmd_seq(const uint8_t *rom, int argc, char **argv)
{
    fr_init();
    Sum tot = {0};
    int t = 0;
    print_header();
    for (int a = 0; a < argc; a++) {
        Snap *s = snap_load(argv[a], rom);
        Frame *f = fr[t & 1];
        frame_build(s, f);
        if (!f->ok) { printf("%-12s skipped: %s\n", s->name, f->why); free(s); continue; }
        Stats st;
        page_update(&pages[t & 1], f, &st, 0);
        print_stats(s->name, &st, f->mism ? "raster mismatch" : "");
        if (t >= 2) sum_add(&tot, &st);
        t++;
        free(s);
    }
    print_sum("warm pages (every snapshot is a scene change)", &tot);
    return tot.bad != 0 || tot.pbad != 0;
}

static int oam_hi(const uint8_t *oam, int i) { return (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3; }

static void oam_move(uint8_t *oam, int i, int dx, int dy)
{
    int hi = oam_hi(oam, i);
    int x = oam[i * 4] | ((hi & 1) << 8);
    if (x >= 256) x -= 512;
    x = (x + dx) & 0x1FF;
    oam[i * 4] = (uint8_t)x;
    oam[i * 4 + 1] = (uint8_t)(oam[i * 4 + 1] + dy);
    int nhi = (hi & 2) | ((x >> 8) & 1);
    uint8_t *b = &oam[512 + (i >> 2)];
    int sh = (i & 3) * 2;
    *b = (uint8_t)((*b & ~(3 << sh)) | (nhi << sh));
}

static int hdma_scroll(const Snap *s);
static int patch_hdma_scroll(Snap *s, int dh, int dv);

static int cmd_sim(const uint8_t *rom, int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "sim: SNAP.BIN frames movers dx dy sx sy\n");
        return 2;
    }
    fr_init();
    Snap *base = snap_load(argv[0], rom);
    int frames = atoi(argv[1]), movers = atoi(argv[2]);
    int dx = atoi(argv[3]), dy = atoi(argv[4]), sx = atoi(argv[5]), sy = atoi(argv[6]);
    Frame *f0 = fr[0];
    frame_build(base, f0);
    if (!f0->ok) { printf("%s: not modelled (%s)\n", base->name, f0->why); return 1; }
    /* the entries that show */
    int mv[128], nm = 0;
    {
        uint8_t seen[128] = {0};
        for (int y = 0; y < PPU_H; y++)
            for (int q = 0; q < f0->np[y]; q++) {
                int i = f0->pc[y][q].i;
                if (!seen[i] && nm < movers) { seen[i] = 1; mv[nm++] = i; }
            }
    }
    printf("%s: %d entries show (%d pieces); moving %d of them by (%d,%d) per frame, scroll (%d,%d) per frame\n",
           base->name, f0->instances, f0->pieces, nm, dx, dy, sx, sy);
    print_header();
    Sum tot = {0};
    for (int t = 0; t < frames; t++) {
        Snap *s = snap_clone(base);
        for (int k = 0; k < nm; k++) {
            oam_move(s->oam, mv[k], dx * t, dy * t);
            for (int g = 0; g < s->noamseg; g++) oam_move(s->oamseg[g].oam, mv[k], dx * t, dy * t);
        }
        for (int b = 0; b < 4; b++) {
            s->bgofs[b * 2] = (uint16_t)(s->bgofs[b * 2] + sx * t);
            s->bgofs[b * 2 + 1] = (uint16_t)(s->bgofs[b * 2 + 1] + sy * t);
        }
        if ((sx || sy) && hdma_scroll(base)) patch_hdma_scroll(s, sx * t, sy * t);
        Frame *f = fr[t & 1];
        frame_build(s, f);
        if (!f->ok) { printf("frame %d skipped: %s\n", t, f->why); free(s); continue; }
        Stats st;
        page_update(&pages[t & 1], f, &st, 0);
        char nm_[16];
        snprintf(nm_, sizeof nm_, "t%d", t);
        print_stats(nm_, &st, t < 2 ? "cold page" : (f->mism ? "raster mismatch" : ""));
        if (t >= 2) sum_add(&tot, &st);
        free(s);
    }
    print_sum("warm", &tot);
    return tot.bad != 0 || tot.pbad != 0;
}

/* dump: F, B and the owner map as raw files for inspection: out.F out.B (224x256 CGRAM indices) out.cg */
static int cmd_dump(const uint8_t *rom, int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "dump: SNAP.BIN outprefix\n"); return 2; }
    fr_init();
    Snap *s = snap_load(argv[0], rom);
    Frame *f = fr[0];
    frame_build(s, f);
    if (!f->ok) { printf("%s: not modelled (%s)\n", s->name, f->why); return 1; }
    char path[512];
    const char *ext[3] = {"F", "B", "cg"};
    const void *data[3] = {f->F, f->B, s->cgram};
    size_t len[3] = {sizeof f->F, sizeof f->B, 512};
    for (int k = 0; k < 3; k++) {
        snprintf(path, sizeof path, "%s.%s", argv[1], ext[k]);
        FILE *o = fopen(path, "wb");
        if (!o) { perror(path); return 2; }
        fwrite(data[k], 1, len[k], o);
        fclose(o);
    }
    for (int y = 0; y < PPU_H; y++) {
        if (!f->np[y]) continue;
        printf("row %3d:", y);
        for (int q = 0; q < f->np[y]; q++) {
            const Piece *p = &f->pc[y][q];
            int x0 = -1, x1 = -1;
            for (int k = 0; k < BWP; k++) if (p->gm[k >> 4] & (0x8000u >> (k & 15))) { if (x0 < 0) x0 = k - 16 * MG; x1 = k - 16 * MG; }
            printf(" #%d[%d..%d]", p->i, x0, x1);
        }
        printf("\n");
    }
    return 0;
}

/* ---- virtual screen with hardware scrolling -------------------------------------
 *
 * A page is a ring: RH rows of RWG groups, addressed as one linear sequence of
 * groups with the rows back to back ("helix": group u of row v is linear group
 * v * RWG + u, and u may run past RWG into the next row). The page shows a
 * window of it, 256 x 224 pixels at world position (Wx, Wy): the Videl starts
 * at linear group Lam0 = (Wy mod RH) * RWG + (Wx >> 4) with pixel shift
 * Wx & 15 (one extra group is fetched when it is not 0) and adds the ring's row
 * length to its address after every line. The world position is the scroll
 * summed up over the frames, so a world point always sits at the same linear
 * group: standing content never moves, the window moves over it.
 *
 * The Videl reads linearly, so a window at the end of the ring reads past it:
 * the first DUPG groups of a page are kept a second time behind the ring. Every
 * group written in that part is written twice (restores) or synced (blits,
 * which may cross the ring's end into the copy).
 *
 * One clean ring (the background alone) is shared by both pages. It is brought
 * up to date once per frame from the exact renderer's background: the window's
 * groups are compared and the ones that differ are written, which is all a
 * scroll step exposes (one column of groups per 16 pixels, one row per pixel
 * row). The pixels of the groups beyond the window's edges come from the same
 * background rendered with every layer shifted by 16 pixels.
 *
 * A page update, per page and frame:
 *   exposed  groups now in the window that were not in the page's last window
 *   bg       groups whose clean content changed since the page took it
 *   pieces   old pieces (by OAM entry and page row) that changed or vanished
 *            dirty their groups in the window; new or changed pieces dirty
 *            theirs. Sprites are screen-anchored, so when the window moves all
 *            of them change; a sprite that scrolls with the background keeps its
 *            place in the ring and costs nothing.
 *   dirty groups are restored from the clean ring and every piece on them is
 *   drawn clipped to them, exactly as with the fixed page.
 * Groups outside the window are left alone: they are restored when they enter it.
 * After the update the Videl's fetch is simulated from the page memory and must
 * show ppu_planar8() of the exact frame. */
#ifndef LC_RH
#define LC_RH 448
#endif
#define RWG 32
#define RH LC_RH
#define TOT (RWG * RH)
#define DUPG (223 * RWG + 17)
#define PAGE_GROUPS (TOT + DUPG)

static int pmod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

typedef struct {
    int ok;
    char why[64];
    int Wx, Wy, s, u0, nwg;
    int S, vis;                    /* standing layers, layers shown (bit per BG layer) */
    char cls[48];
    /* each overlay layer's own picture from its window's first group to its last,
     * at the world position its own scroll gives it (for the layer ring estimate) */
    int lnwg[4], lu0[4], ls[4], lWx[4], lWy[4];
    uint8_t lwide[4][PPU_H][17 * 16];
    uint8_t F[PPU_H][PPU_W];
    uint8_t wide[PPU_H][17 * 16];  /* the background from the window's first group to its last */
    Piece pc[PPU_H][MAXP];
    uint8_t np[PPU_H];
    int mism, pieces, instances, ov_pieces;
} RFrame;

typedef struct {
    uint32_t off;                  /* byte offset of the page in mem */
    int init;
    int pver[TOT];                 /* the clean version the group was restored from */
    uint8_t valid[TOT];            /* in the window at this page's last update */
    Piece rc[RH][MAXP];            /* pieces drawn, by page row */
    uint8_t rnp[RH];
} RPage;

static int cver[TOT];
static uint32_t clean_off;
/* one ring per overlay layer in `mem` (TOT groups and DUPS more behind them: a
 * source run may cross the ring's end), and whether overlay pieces blit from them */
#define DUPS 24
static uint32_t lring_off[4];
static int overlay_ring;

typedef struct {
    long exposed, bgd, dirty, runs, blits, blit_groups, pieces, changed, dup_groups, dup_runs, clean_groups, entry_bytes, spills,
        ov_blits, lr_groups;
    long sp_inst, sp_hits, sp_inv, sp_new, sp_cur;   /* sprite programs, see prog_stats() */
    long c_hits, c_misses, c_flushes;     /* source row cache, this update */
    uint32_t c_used;
    long pk_bytes;                        /* the update's list in packed records (and 3 bytes per restore run) */
    long pk_e, pk_r;                      /* of it: entry rows, ring rows */
    long pi_calls, pi_defs, pi_once, pi_cb, pi_db;   /* sprite instance packets, see pk_instance() */
    long sp_pure, sp_pure_hit, sp_occ, sp_occ_hit;   /* instances that nothing occludes, and the others; calls among them */
    long sp_occ_bytes, sp_occ_pot;                   /* what the occluded ones cost, and at best with silhouette programs */
    int pbad;
} RStats;

static int ring_lam(int wy_row, int u) { return pmod(pmod(wy_row, RH) * RWG + u, TOT); }

/* The data blocks of one enabled HDMA channel, as WRAM offsets: the four (or
 * fewer) bytes each line group writes. Direct tables hold them in the table,
 * indirect ones (DMAPx bit 6) hold a 16-bit pointer per entry into the bank in
 * $43x7; every block is listed once. Tables or data in ROM give 0 blocks. */
static int hdma_blocks(const Snap *s, int ch, uint32_t *out, int max)
{
    int ctl = s->io[0x4300 + ch * 16 - 0x2000];
    int ind = (ctl & 0x40) != 0;
    uint32_t addr = (uint32_t)(s->io[0x4302 + ch * 16 - 0x2000] | s->io[0x4303 + ch * 16 - 0x2000] << 8);
    int bank = s->io[0x4304 + ch * 16 - 0x2000];
    int dbank = s->io[0x4307 + ch * 16 - 0x2000];
    int len = (ctl & 7) == 0 ? 1 : (ctl & 7) == 1 || (ctl & 7) == 2 ? 2 : 4;
    if (bank != 0x7E && bank != 0x7F) return 0;
    if (ind && dbank != 0x7E && dbank != 0x7F) return 0;
    uint32_t p = ((uint32_t)(bank & 1) << 16) | addr;
    int n = 0;
    for (int guard = 0; guard < 4096; guard++) {
        int cnt = s->wram[p++ & 0x1FFFF];
        if (!cnt) break;
        int lines = cnt & 0x7F ? cnt & 0x7F : 128;
        int entries = (cnt & 0x80) ? lines : 1;
        for (int e = 0; e < entries; e++) {
            uint32_t dp;
            if (ind) {
                dp = ((uint32_t)(dbank & 1) << 16) | (uint32_t)(s->wram[p & 0x1FFFF] | s->wram[(p + 1) & 0x1FFFF] << 8);
                p += 2;
            } else {
                dp = p & 0x1FFFF;
                p += (uint32_t)len;
            }
            int seen = 0;
            for (int k = 0; k < n; k++) seen |= out[k] == dp;
            if (!seen && n < max) out[n++] = dp;
        }
    }
    return n;
}

/* Adds (dh[b], dv[b]) to layer b's BG scroll values in the HDMA tables that
 * drive them (mode 3 to $210D/$210F/$2111/$2113: H then V, each written
 * twice), in the snapshot's WRAM copy. Returns the number of channels patched. */
static int patch_hdma_layers(Snap *s, const int *dh, const int *dv)
{
    uint8_t en = s->io[0x420C - 0x2000];
    int patched = 0;
    for (int ch = 0; ch < 8; ch++) {
        if (!(en & (1 << ch))) continue;
        int ctl = s->io[0x4300 + ch * 16 - 0x2000], dest = s->io[0x4301 + ch * 16 - 0x2000];
        if ((ctl & 7) != 3) continue;
        if (dest != 0x0D && dest != 0x0F && dest != 0x11 && dest != 0x13) continue;
        int b = (dest - 0x0D) / 2;
        uint32_t blk[512];
        int n = hdma_blocks(s, ch, blk, 512);
        for (int k = 0; k < n; k++) {
            uint8_t *d = &s->wram[blk[k] & 0x1FFFF];
            int h = (d[0] | d[1] << 8) + dh[b], v = (d[2] | d[3] << 8) + dv[b];
            d[0] = (uint8_t)h; d[1] = (uint8_t)((h >> 8) & 3);
            d[2] = (uint8_t)v; d[3] = (uint8_t)((v >> 8) & 3);
        }
        patched += n > 0;
    }
    return patched;
}

/* The same shift for every layer. */
static int patch_hdma_scroll(Snap *s, int dh, int dv)
{
    int h[4] = {dh, dh, dh, dh}, v[4] = {dv, dv, dv, dv};
    return patch_hdma_layers(s, h, v);
}

/* Keeps only the main-screen layers in `keep` (bit 0-3 BG1-4, bit 4 OBJ): TM in
 * the register shadow, in the IRQ write log and in the HDMA tables that write it
 * (modes 0, 1, 3 and 4 to $212C). */
static void snap_keep_layers(Snap *t, int keep)
{
    uint8_t m = (uint8_t)(keep | 0xE0);
    t->io[0x12C] &= m;
    for (int k = 0; k < t->nlog; k++)
        if (t->log[k].reg == 0x212C) t->log[k].value &= m;
    uint8_t en = t->io[0x420C - 0x2000];
    for (int ch = 0; ch < 8; ch++) {
        if (!(en & (1 << ch))) continue;
        int mode = t->io[0x4300 + ch * 16 - 0x2000] & 7, dest = t->io[0x4301 + ch * 16 - 0x2000];
        if (dest != 0x2C || (mode != 0 && mode != 1 && mode != 3 && mode != 4)) continue;
        uint32_t blk[512];
        int n = hdma_blocks(t, ch, blk, 512);
        for (int k = 0; k < n; k++) {
            uint8_t *d = &t->wram[blk[k] & 0x1FFFF];
            d[0] &= m;
            if (mode == 3) d[1] &= m;
        }
    }
}

/* OAM hidden, only the BG layers in `keep` shown, every layer's horizontal
 * scroll moved by dh (in the latched registers and in the HDMA tables that
 * overwrite them). */
static void render_layers(const Snap *s, int dh, int keep, uint8_t *out)
{
    Snap *t = snap_clone(s);
    if (dh) patch_hdma_scroll(t, dh, 0);
    oam_hide(t->oam);
    for (int g = 0; g < t->noamseg; g++) oam_hide(t->oamseg[g].oam);
    snap_keep_layers(t, keep & 0xF);
    for (int b = 0; b < 4; b++) t->bgofs[b * 2] = (uint16_t)((t->bgofs[b * 2] + dh) & 0x3FF);
    PpuInput in = snap_input(t, t->io);
    ppu_render(&in, out);
    free(t);
}

typedef struct { uint16_t bgofs0[8]; int have, var; } ScrollRows;

static void scroll_fn(void *ctx, int row, const PpuRegs *s)
{
    ScrollRows *c = ctx;
    (void)row;
    if (s->r[0x00] & 0x80) return;
    if (!c->have) { memcpy(c->bgofs0, s->bgofs, sizeof c->bgofs0); c->have = 1; }
    else if (memcmp(s->bgofs, c->bgofs0, sizeof c->bgofs0)) c->var++;
}

/* 1 when an enabled HDMA channel writes the BG scroll registers ($210D-$2114):
 * the layer's scroll then comes from a table and not from the latched value. */
static int hdma_scroll(const Snap *s)
{
    uint8_t en = s->io[0x420C - 0x2000];
    int found = 0;
    for (int ch = 0; ch < 8; ch++) {
        if (!(en & (1 << ch))) continue;
        int dest = s->io[0x4301 + ch * 16 - 0x2000];
        if (dest >= 0x0A && dest <= 0x14) found = 1;
    }
    return found;
}

/* What the ring model remembers from frame to frame. */
typedef struct {
    int have, anchor, all_mode;    /* anchor: the lowest layer of the standing set; all_mode: every layer stands */
    int pscroll[8];                /* every layer's effective scroll registers */
    int Wx, Wy;                    /* world position of the window */
    int lhave[4], lWx[4], lWy[4];  /* the same for each overlay layer on its own */
    int pbg_have;
    int cm_key;                    /* the chooser's committed choice (key: see rframe_build) */
    double sav[32];                /* smoothed saving of each other candidate against it */
    uint8_t pbg[PPU_H][PPU_W];     /* the last frame's BG layers without sprites (chooser) */
} Track;

static int popcount4(int v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1); }

/* Unit costs in microseconds (LC_COST=g,b,c,h): one background group produced by the DSP, one
 * blitter row, one group copied, one byte sent. ASSUMED, not measured: g is the exact
 * renderer's BG work per group in Hatari's DSP timing (dsp-renderer-budget.md: about 26), b a
 * guess for a row with its pokes and eight planes of a few words, c a guess for copying 16
 * bytes, h a long read of the host port (16 cycles of a 16 MHz 68030 is 0.25 per byte). */
static double cw[4] = {26.0, 13.0, 4.0, 0.25};
static double chooser_horizon = 24.0;   /* LC_HORIZON: frames a choice is expected to last */
static int chooser_cost;           /* LC_CHOOSER=cost (1: from each layer's own picture) or exact (2: from the pieces) */

/* Groups a scroll step of (dx, dy) pixels a frame exposes in the window: a column of 224 every 16
 * pixels across, a row of 17 per pixel down. */
static double scroll_expo(int dx, int dy)
{
    double e = (dx < 0 ? -dx : dx) / 16.0 * 224.0 + (dy < 0 ? -dy : dy) * 17.0;
    return e > 3808.0 ? 3808.0 : e;
}

/* Builds a frame for the ring model.
 *
 * Classification: the layers shown on the main screen are grouped by how far
 * their effective scroll moved since the last frame. The group that becomes the
 * standing set S (what the Videl scrolls, what the clean ring holds) is the one
 * that leaves the fewest opaque BG pixels to the other layers (the overlays);
 * the group of the last frame's anchor layer is kept unless that costs more
 * than 512 pixels. The world position follows S's scroll. Nothing needs S to
 * stay the same from frame to frame: the clean ring is compared and rewritten
 * and the pages take what changed, so a switch only costs redrawing.
 *
 * Pieces: a pixel needs painting where the exact frame F differs from the
 * standing background B_S. Its owner is the sprite the rasterizer has there if
 * that sprite's pixel equals F, otherwise the first overlay layer (BG1 to BG4)
 * whose own picture equals F. An overlay layer behind S is fine: only the pixels
 * where it shows (where S is transparent) need painting, which the comparison
 * with F finds. */
static void rframe_build(const Snap *s, Track *tr, RFrame *f)
{
    memset(f, 0, sizeof *f);
    PpuInput in = snap_input(s, s->io);
    static Rows rows;
    memset(&rows, 0, sizeof rows);
    ppu_walk_all = 0;
    ppu_replay(&in, row_fn, &rows);
    static ScrollRows sr;
    memset(&sr, 0, sizeof sr);
    ppu_replay(&in, scroll_fn, &sr);
    const char *why = NULL;
    if (!rows.vis) why = "forced blank";
    else if (rows.bad_mode) why = "mode";
    else if (rows.bad_tm) why = "TM varies";
    else if (rows.math) why = "colour math";
    else if (rows.win) why = "windows";
    else if (rows.mos) why = "mosaic";
    else if (rows.sub) why = "sub screen";
    else if (sr.var) why = "scroll varies per line";
    if (why) { snprintf(f->why, sizeof f->why, "%s", why); return; }
    int vis = rows.tm0 & 0xF;
    int cur[8];
    for (int i = 0; i < 8; i++) cur[i] = sr.bgofs0[i] & 0x3FF;

    /* each shown layer on its own: its pixels, and how much it covers */
    static uint8_t R[4][PPU_H][PPU_W];
    long opaque[4] = {0, 0, 0, 0};
    for (int b = 0; b < 4; b++) {
        if (!(vis & (1 << b))) continue;
        render_layers(s, 0, 1 << b, &R[b][0][0]);
        const uint8_t *rf = &R[b][0][0];
        for (int k = 0; k < PPU_H * PPU_W; k++) {
            if (rf[k] >= 128) { snprintf(f->why, sizeof f->why, "BG index >= 128"); return; }
            opaque[b] += rf[k] != 0;
        }
    }
    if (getenv("LC_LAYERS")) {
        for (int b = 0; b < 4; b++) {
            if (!(vis & (1 << b))) continue;
            long cells = 0;
            for (int y = 0; y < PPU_H; y++)
                for (int g = 0; g < GW; g++) {
                    int any = 0;
                    for (int x = 0; x < 16; x++) any |= R[b][y][g * 16 + x] != 0;
                    cells += any;
                }
            fprintf(stderr, "layer BG%d: %ld opaque pixels, %ld cells of %d\n", b + 1, opaque[b], cells, PPU_H * GW);
        }
    }
    int dh[4] = {0, 0, 0, 0}, dv[4] = {0, 0, 0, 0};
    if (tr->have)
        for (int b = 0; b < 4; b++) {
            dh[b] = ((cur[b * 2] - tr->pscroll[b * 2] + 512) & 0x3FF) - 512;
            dv[b] = ((cur[b * 2 + 1] - tr->pscroll[b * 2 + 1] + 512) & 0x3FF) - 512;
        }
    int S = 0;
    if (vis) {
        long bestcost = -1;
        int bestS = 0;
        for (int b = 0; b < 4; b++) {
            if (!(vis & (1 << b))) continue;
            int g = 0;
            long cost = 0;
            for (int c = 0; c < 4; c++) {
                if (!(vis & (1 << c))) continue;
                if (dh[c] == dh[b] && dv[c] == dv[b]) g |= 1 << c;
                else cost += opaque[c];
            }
            if (bestcost < 0 || cost < bestcost || (cost == bestcost && popcount4(g) > popcount4(bestS))) {
                bestcost = cost;
                bestS = g;
            }
        }
        S = bestS;
        if (tr->have && (vis & (1 << tr->anchor))) {
            int g = 0;
            long cost = 0;
            for (int c = 0; c < 4; c++) {
                if (!(vis & (1 << c))) continue;
                if (dh[c] == dh[tr->anchor] && dv[c] == dv[tr->anchor]) g |= 1 << c;
                else cost += opaque[c];
            }
            if (cost <= bestcost + 512) S = g;
        }
    }
    /* LC_CHOOSER=cost: the standing set by an estimate of what each choice costs, in microseconds
     * at the unit costs cw (LC_COST=g,b,c,h: one BG group produced by the DSP, one blitter row, one
     * group copied, one byte sent). Candidates: each group of layers with one scroll as the standing
     * set, the others overlays from their rings (ov); or every layer standing with that group's
     * scroll, the others' changed cells produced by the DSP and restored (all). The estimate is
     * from the layers' own pictures (cells, runs) and the scrolls, before any pieces are built. */
    int force_anchor = -1, all_mode = 0;
    if (chooser_cost && vis && popcount4(vis) > 1) {
        long cells[4] = {0, 0, 0, 0}, runs[4] = {0, 0, 0, 0};
        for (int b = 0; b < 4; b++) {
            if (!(vis & (1 << b))) continue;
            for (int y = 0; y < PPU_H; y++) {
                uint16_t mk[GW];
                for (int g = 0; g < GW; g++) {
                    unsigned v = 0;
                    for (int x = 0; x < 16; x++) v |= (unsigned)(R[b][y][g * 16 + x] != 0) << (15 - x);
                    mk[g] = (uint16_t)v;
                    cells[b] += v != 0;
                }
                int gr = 0;
                runs[b] += count_blits(mk, GW, &gr);
            }
        }
        long spr_rows = 0;
        if (rows.tm0 & 0x10)
            for (int y = 0; y < PPU_H; y++) {
                static int16_t w_[BWP];
                static uint8_t v_[BWP];
                uint8_t cn[128];
                memset(cn, 0, sizeof cn);
                raster_row(s, rows.obsel[y], rows.oam[y], y, w_, v_, cn);
                for (int i = 0; i < 128; i++) spr_rows += cn[i] != 0;
            }
        /* exact: what a candidate really has to paint or produce, from the frame itself */
        static uint8_t Fbg[PPU_H][PPU_W], Bg[PPU_H][PPU_W];
        if (chooser_cost == 2) render_layers(s, 0, vis, &Fbg[0][0]);
        double gg = cw[0] + 16 * cw[3], bb = cw[1], cc = cw[2];
        /* Candidates, keyed: a set M of layers that share one scroll, standing on the hardware with
         * the others as overlays (key M, 1..15); or every layer standing, anchored to layer b (key
         * 16 + b), the layers with another scroll having their changed cells produced by the DSP.
         * A candidate's cost is for this frame only. What decides is the choice the model has
         * committed to (tr->cm_key) against the others' smoothed saving: most frames of a game
         * change nothing, and a choice made on an idle frame flips at the next moving one, each
         * time rewriting the clean ring. */
        int nk = 0, ckey[32], cbl[32], cinf[32];
        double ccost[32];
        for (int key = 1; key < 32; key++) {
            int mode = key >= 16, M = mode ? 0 : key, b;
            if (mode) {
                b = key - 16;
                if (b > 3 || !(vis & (1 << b))) continue;
            } else {
                if (M & ~vis) continue;
                b = 0;
                while (!(M & (1 << b))) b++;
                int same = 1;
                for (int c2 = 0; c2 < 4; c2++)
                    if ((M & (1 << c2)) && (dh[c2] != dh[b] || dv[c2] != dv[b])) same = 0;
                if (!same) {
                    /* the layers have different scrolls: this set cannot stand as it is. It costs
                     * what every layer standing costs (filled in below), and is still a candidate:
                     * leaving it out would show no evidence against a set that only works on the
                     * frames where the layers happen to move together */
                    ckey[nk] = key;
                    cbl[nk] = b;
                    ccost[nk] = 0;
                    cinf[nk] = 1;
                    nk++;
                    continue;
                }
            }
            double prod = scroll_expo(dh[b], dv[b]);
            double rest = 0, blits = 0;
            for (int c2 = 0; c2 < 4; c2++) {
                if (!(vis & (1 << c2))) continue;
                if (mode == 0 && !(M & (1 << c2))) {
                    prod += scroll_expo(dh[c2], dv[c2]);
                    rest += (double)cells[c2];
                    blits += (double)runs[c2];
                } else if (mode == 1 && (dh[c2] != dh[b] || dv[c2] != dv[b])) {
                    prod += (double)cells[c2];
                    rest += (double)cells[c2];
                }
            }
            if (chooser_cost == 2) {
                long nc = 0, nr = 0;
                if (mode == 0 && (vis & ~M)) {
                    render_layers(s, 0, M, &Bg[0][0]);
                    for (int y = 0; y < PPU_H; y++) {
                        /* pieces are cut by owner: the first overlay layer (BG1 up) whose own picture is the
                         * pixel, so each overlay's mask is ragged where a layer above shows through */
                        uint16_t own[4][GW];
                        memset(own, 0, sizeof own);
                        uint16_t any[GW];
                        memset(any, 0, sizeof any);
                        for (int x = 0; x < PPU_W; x++) {
                            if (Fbg[y][x] == Bg[y][x]) continue;
                            for (int c2 = 0; c2 < 4; c2++)
                                if ((vis & ~M & (1 << c2)) && R[c2][y][x] == Fbg[y][x]) {
                                    own[c2][x >> 4] |= (uint16_t)(0x8000u >> (x & 15));
                                    break;
                                }
                            any[x >> 4] |= (uint16_t)(0x8000u >> (x & 15));
                        }
                        for (int q = 0; q < GW; q++) nc += any[q] != 0;
                        for (int c2 = 0; c2 < 4; c2++) {
                            int gr = 0;
                            nr += count_blits(own[c2], GW, &gr);
                        }
                    }
                    prod = scroll_expo(dh[b], dv[b]);
                    for (int c2 = 0; c2 < 4; c2++)
                        if (vis & ~M & (1 << c2)) prod += scroll_expo(dh[c2], dv[c2]);
                    rest = (double)nc;
                    blits = (double)nr;
                } else if (mode == 1 && tr->pbg_have) {
                    /* all standing: the cells that differ from the last frame moved by the anchor's scroll */
                    for (int y = 0; y < PPU_H; y++)
                        for (int q = 0; q < GW; q++) {
                            int diff = 0;
                            for (int x = 0; x < 16 && !diff; x++) {
                                int py = y + dv[b], px = q * 16 + x + dh[b];
                                int pv = (py >= 0 && py < PPU_H && px >= 0 && px < PPU_W) ? tr->pbg[py][px] : -1;
                                diff = pv != Fbg[y][q * 16 + x];
                            }
                            nc += diff;
                        }
                    prod = scroll_expo(dh[b], dv[b]) + (double)nc;
                    rest = (double)nc;
                }
            }
            double spr = (dh[b] || dv[b]) ? (double)spr_rows : 0;
            double cost = gg * prod + cc * rest + bb * blits + (bb + cc) * spr;
            if (getenv("LC_CHOOSER_DEBUG"))
                fprintf(stderr, "cand key=%d prod=%.0f rest=%.0f blits=%.0f spr=%.0f cost=%.0f\n", key, prod, rest, blits, spr, cost);
            ckey[nk] = key;
            cbl[nk] = b;
            ccost[nk] = cost;
            cinf[nk] = 0;
            nk++;
        }
        for (int k = 0; k < nk; k++)
            if (cinf[k])
                for (int k2 = 0; k2 < nk; k2++)
                    if (ckey[k2] == 16 + cbl[k]) ccost[k] = ccost[k2];
        /* the committed choice this frame: itself if it is a candidate, else every layer standing
         * anchored to its lowest shown layer (where its layers with another scroll end up) */
        int ci = -1;
        if (tr->have && tr->cm_key) {
            for (int k = 0; k < nk; k++)
                if (ckey[k] == tr->cm_key) ci = k;
            if (ci < 0) {
                int lay = tr->cm_key >= 16 ? tr->cm_key - 16 : 0;
                if (tr->cm_key < 16)
                    while (lay < 4 && !(tr->cm_key & vis & (1 << lay))) lay++;
                if (lay < 4)
                    for (int k = 0; k < nk; k++)
                        if (ckey[k] == 16 + lay) ci = k;
            }
        }
        int best_k = -1;
        for (int k = 0; k < nk; k++)
            if (!cinf[k] && (best_k < 0 || ccost[k] < ccost[best_k])) best_k = k;
        /* an idle frame (no layer scrolled, the BG picture unchanged) costs nothing whatever the
         * choice, so it says nothing about it: no evidence, no switch */
        int idle = tr->have;
        for (int c2 = 0; c2 < 4; c2++)
            if ((vis & (1 << c2)) && (dh[c2] || dv[c2])) idle = 0;
        if (idle && chooser_cost == 2 && (!tr->pbg_have || memcmp(Fbg, tr->pbg, sizeof Fbg))) idle = 0;
        int pick = ci;
        if (ci < 0) {
            pick = best_k;
            memset(tr->sav, 0, sizeof tr->sav);
            tr->cm_key = ckey[pick];
        } else if (!idle) {
            /* smoothed saving of each other candidate against the committed one */
            for (int key = 0; key < 32; key++) tr->sav[key] *= 0.875;
            for (int k = 0; k < nk; k++)
                if (k != ci) tr->sav[ckey[k]] += 0.125 * (ccost[ci] - ccost[k]);
            int bk = -1;
            for (int k = 0; k < nk; k++)
                if (ckey[k] != tr->cm_key && (bk < 0 || tr->sav[ckey[k]] > tr->sav[ckey[bk]])) bk = k;
            /* a switch rewrites the clean ring, the window's groups from the DSP: worth it only if
             * the smoothed saving pays that back within `horizon` frames */
            if (bk >= 0 && tr->sav[ckey[bk]] * chooser_horizon > gg * (double)(PPU_H * GW)) {
                pick = bk;
                tr->cm_key = ckey[pick];
                memset(tr->sav, 0, sizeof tr->sav);
            }
        }
        if (ckey[pick] >= 16 || cinf[pick]) {
            S = vis;
            force_anchor = cbl[pick];
            all_mode = 1;
        } else {
            S = ckey[pick];
            force_anchor = cbl[pick];
            all_mode = 0;
        }
    }
    if (chooser_cost == 2 && vis) {
        render_layers(s, 0, vis, &tr->pbg[0][0]);
        tr->pbg_have = 1;
    }
    /* LC_STANDING=all, or a bit mask of BG layers (1 = BG1 ... 8 = BG4): force the standing set */
    if (getenv("LC_STANDING")) {
        const char *fs = getenv("LC_STANDING");
        S = strcmp(fs, "all") ? (int)strtol(fs, NULL, 0) & vis : vis;
    }
    int anchor = 0;
    while (anchor < 4 && !(S & (1 << anchor))) anchor++;
    if (anchor == 4) anchor = 0;
    if (force_anchor >= 0 && !getenv("LC_STANDING")) anchor = force_anchor;
    f->S = S;
    f->vis = vis;
    if (!tr->have) {
        /* LC_WORLD=x,y moves the world origin: same content, another place in the ring */
        int ox = 0, oy = 0;
        const char *w = getenv("LC_WORLD");
        if (w) sscanf(w, "%d,%d", &ox, &oy);
        f->Wx = cur[anchor * 2] + ox;
        f->Wy = cur[anchor * 2 + 1] + oy;
    } else {
        f->Wx = tr->Wx + (S ? dh[anchor] : 0);
        f->Wy = tr->Wy + (S ? dv[anchor] : 0);
    }
    snprintf(f->cls, sizeof f->cls, "S=%s%s%s%s ov=%s%s%s%s", S & 1 ? "1" : "", S & 2 ? "2" : "", S & 4 ? "3" : "",
             S & 8 ? "4" : "", (vis & ~S) & 1 ? "1" : "", (vis & ~S) & 2 ? "2" : "", (vis & ~S) & 4 ? "3" : "",
             (vis & ~S) & 8 ? "4" : "");
    tr->have = 1;
    tr->anchor = anchor;
    tr->all_mode = all_mode;
    for (int i = 0; i < 8; i++) tr->pscroll[i] = cur[i];
    tr->Wx = f->Wx;
    tr->Wy = f->Wy;
    f->s = pmod(f->Wx, 16);
    f->u0 = (f->Wx - f->s) / 16;
    f->nwg = f->s ? 17 : 16;
    f->ok = 1;

    PpuInput inf = snap_input(s, s->io);
    ppu_render(&inf, &f->F[0][0]);
    static uint8_t B[PPU_H][PPU_W], Bl[PPU_H][PPU_W], Br[PPU_H][PPU_W];
    render_layers(s, 0, S, &B[0][0]);
    if (f->s) {
        render_layers(s, -16, S, &Bl[0][0]);
        render_layers(s, 16, S, &Br[0][0]);
    }
    for (int y = 0; y < PPU_H; y++)
        for (int q = 0; q < f->nwg * 16; q++) {
            int X = q - f->s;
            f->wide[y][q] = X < 0 ? Bl[y][X + 16] : X >= PPU_W ? Br[y][X - 16] : B[y][X];
        }

    /* the overlay layers on their own, for the layer ring estimate */
    for (int b = 0; b < 4; b++) {
        if (!((vis & ~S) & (1 << b))) { tr->lhave[b] = 0; continue; }
        if (!tr->lhave[b]) {
            /* LC_LWORLD=x,y moves the layers' world origin (another place in their rings) */
            int lox = 0, loy = 0;
            if (getenv("LC_LWORLD")) sscanf(getenv("LC_LWORLD"), "%d,%d", &lox, &loy);
            tr->lWx[b] = cur[b * 2] + lox;
            tr->lWy[b] = cur[b * 2 + 1] + loy;
            tr->lhave[b] = 1;
        } else {
            tr->lWx[b] += dh[b];
            tr->lWy[b] += dv[b];
        }
        f->lWx[b] = tr->lWx[b];
        f->lWy[b] = tr->lWy[b];
        f->ls[b] = pmod(f->lWx[b], 16);
        f->lu0[b] = (f->lWx[b] - f->ls[b]) / 16;
        f->lnwg[b] = f->ls[b] ? 17 : 16;
        static uint8_t Ll[PPU_H][PPU_W], Lr[PPU_H][PPU_W];
        if (f->ls[b]) {
            render_layers(s, -16, 1 << b, &Ll[0][0]);
            render_layers(s, 16, 1 << b, &Lr[0][0]);
        }
        for (int y = 0; y < PPU_H; y++)
            for (int q = 0; q < f->lnwg[b] * 16; q++) {
                int X = q - f->ls[b];
                f->lwide[b][y][q] = X < 0 ? Ll[y][X + 16] : X >= PPU_W ? Lr[y][X - 16] : R[b][y][X];
            }
    }

    static uint8_t inst_seen[128];
    memset(inst_seen, 0, sizeof inst_seen);
    int ov = vis & ~S;
    for (int y = 0; y < PPU_H; y++) {
        static int16_t win[BWP];
        static uint8_t val[BWP];
        for (int k = 0; k < BWP; k++) { win[k] = -1; val[k] = 0; }
        uint8_t cnt[128];
        memset(cnt, 0, sizeof cnt);
        if (rows.tm0 & 0x10) raster_row(s, rows.obsel[y], rows.oam[y], y, win, val, cnt);
        int lam0 = ring_lam(f->Wy + y, f->u0 - 1);
        int slot[132];
        for (int i = 0; i < 132; i++) slot[i] = -1;
        for (int k = 0; k < BWP; k++) {
            int X = k - 16 * MG, w = win[k], owner = -1;
            if (X >= 0 && X < PPU_W) {
                if (f->F[y][X] == B[y][X]) continue;           /* the clean ring already shows it */
                if (w >= 0 && val[k] == f->F[y][X]) owner = w;
                else
                    for (int b = 0; b < 4 && owner < 0; b++)
                        if ((ov & (1 << b)) && R[b][y][X] == f->F[y][X]) owner = 128 + b;
                if (owner < 0) { f->mism++; continue; }
            } else if (w >= 0) owner = w;                      /* overhang: drawn, never seen */
            if (owner < 0) continue;
#ifdef LC_MUT_OVERLAY
            if (owner >= 128) continue;
#endif
            if (slot[owner] < 0) {
                if (f->np[y] >= MAXP) { f->mism++; continue; }
                slot[owner] = f->np[y]++;
                Piece *p = &f->pc[y][slot[owner]];
                memset(p, 0, sizeof *p);
                p->i = (int16_t)owner;
                p->lam0 = lam0;
                if (owner >= 128) {
                    p->full = &R[owner - 128][y][0];
                    p->fbase = 16 * MG + f->s;
                }
                if (owner < 128) inst_seen[owner] = 1;
            }
            Piece *p = &f->pc[y][slot[owner]];
            int kk = k + f->s;                                 /* pixel in the row's local group space */
            p->gm[kk >> 4] |= (uint16_t)(0x8000u >> (kk & 15));
            p->v[kk] = owner < 128 ? val[k] : f->F[y][X];
        }
        for (int q = 0; q < f->np[y]; q++) {
            Piece *p = &f->pc[y][q];
            uint32_t h = 2166136261u;
            for (int g = 0; g < PG; g++) {
                if (!p->gm[g]) continue;
                p->foot |= 1u << g;
                h = fnv(h, (uint32_t)((p->lam0 + g) % TOT) << 16 | p->gm[g]);
                for (int b = 0; b < 16; b++)
                    if (p->gm[g] & (0x8000u >> b)) h = fnv(h, p->v[g * 16 + b]);
            }
            p->h = h;
            f->pieces++;
            if (p->i >= 128) f->ov_pieces++;
            else p->sil = cnt[p->i];
        }
    }
    for (int i = 0; i < 128; i++) f->instances += inst_seen[i];
}

/* The clean ring, once per frame. */
static void clean_update(const RFrame *f, RStats *st)
{
    for (int r = 0; r < PPU_H; r++)
        for (int k = 0; k < f->nwg; k++) {
            int lam = ring_lam(f->Wy + r, f->u0 + k);
            uint8_t g[16];
            for (int p = 0; p < 8; p++) {
                unsigned w = 0;
                for (int b = 0; b < 16; b++) w |= (unsigned)((f->wide[r][k * 16 + b] >> p) & 1) << (15 - b);
                g[p * 2] = (uint8_t)(w >> 8);
                g[p * 2 + 1] = (uint8_t)w;
            }
            uint8_t *c = mem + clean_off + (uint32_t)lam * 16;
            if (memcmp(c, g, 16)) {
                memcpy(c, g, 16);
                rec_data((uint32_t)(c - mem), g, 16);
                cver[lam]++;
                st->clean_groups++;
            }
        }
}

/* Layer rings: each overlay layer kept in a ring of its own, world-aligned at its
 * own scroll and laid out like a page (group-interleaved planes), as the clean
 * ring holds the standing set. Each frame the groups of the layer's window that
 * differ from its ring are rewritten; that is all the DSP sends for the layer
 * (16 bytes each), and a fixed layer sends nothing after its first frame. The
 * first DUPS groups exist a second time behind the ring so a source run that
 * crosses its end reads on. With LC_OVERLAY=ring the layer's pieces are blitted
 * from its ring (draw_piece_ring); without it the ring is only counted. */
static void lring_update(const RFrame *f, RStats *st)
{
    for (int b = 0; b < 4; b++) {
        if (!((f->vis & ~f->S) & (1 << b))) continue;
        for (int r = 0; r < PPU_H; r++)
            for (int k = 0; k < f->lnwg[b]; k++) {
                int lam = ring_lam(f->lWy[b] + r, f->lu0[b] + k);
                uint8_t g[16];
                for (int p = 0; p < 8; p++) {
                    unsigned w = 0;
                    for (int bit = 0; bit < 16; bit++) w |= (unsigned)((f->lwide[b][r][k * 16 + bit] >> p) & 1) << (15 - bit);
                    g[p * 2] = (uint8_t)(w >> 8);
                    g[p * 2 + 1] = (uint8_t)w;
                }
                uint8_t *c = mem + lring_off[b] + (uint32_t)lam * 16;
                if (memcmp(c, g, 16)) {
                    memcpy(c, g, 16);
                    rec_data((uint32_t)(c - mem), g, 16);
#ifndef LC_MUT_LRDUP
                    if (lam < DUPS) {
                        memcpy(c + (uint32_t)TOT * 16, g, 16);
                        rec_data((uint32_t)(c - mem) + (uint32_t)TOT * 16, g, 16);
                    }
#endif
                    st->lr_groups++;
                }
            }
    }
}

/* Draws an overlay layer's piece row from the layer's ring. The page's pixel x
 * of a layer pixel at layer-world x is x + delta, delta = Wx - Wx_layer, so
 * destination word c takes the head of the ring's group c - q and the tail of
 * group c - q - 1 with skew r, delta = 16 q + r. The ring holds real content
 * beyond the window, so a run never needs the no-final-read case: it reads one
 * word before its first destination word (FXSR, when r > 0) and one per
 * destination word. The source is group-interleaved like the page: X increment 16,
 * one line per plane. The masks in m[] are the piece's clipped masks as before. */
static void ring_run_regs(uint32_t page, uint32_t lam0, uint32_t ringbase, uint32_t base, int r, int a, int n,
                          uint16_t em1, uint16_t em3, BlitReg *reg)
{
    int fxsr = r > 0, nreads = n + fxsr;
    uint32_t lin = (uint32_t)pmod((int)base + a - fxsr, TOT);
    if (n == 1) em3 = 0xFFFF;                /* not used: one word uses endmask 1 */
    reg->src = ringbase + lin * 16;
    reg->dst = page + (lam0 + (uint32_t)a) * 16;
    reg->xcnt = (uint16_t)n;
    reg->ycnt = 8;
    reg->sxi = 16;
    reg->syi = (int16_t)(2 - 16 * (nreads - 1));
    reg->dxi = 16;
    reg->dyi = (int16_t)(2 - 16 * (n - 1));
    reg->em1 = em1;
    reg->em2 = 0xFFFF;
    reg->em3 = em3;
    reg->fxsr = (uint8_t)fxsr;
    reg->nfsr = 0;
    reg->skew = (uint8_t)r;
#ifdef LC_MUT_LRSKEW
    reg->skew = (uint8_t)((r + 1) & 15);
#endif
#ifdef LC_MUT_LRFXSR
    reg->fxsr = 0;
#endif
}

/* Writes the packet of a ring row, reads it back and checks the registers. The
 * source group for local group 0 is k->idx, adjusted for the shift. */
static void pk_check_ring(uint32_t page, uint32_t ringbase, const Packet *k, const BlitReg *exec)
{
    uint32_t start = pk_len;
    RingState *e = &enc_rs[k->layer];
    int cont = e->valid && k->lam0 == (e->lam0 + RWG) % TOT && k->idx == (e->base + RWG) % TOT && k->r == e->r;
    pk_stat[0]++;
    if (cont) {
        pk[pk_len++] = (uint8_t)(0x80 | k->layer << 4 | ((k->nruns - 1) & 15));
        pk_stat[5]++;
    } else {
        pk_stat[4]++;
        pk[pk_len++] = (uint8_t)(0x40 | ((k->nruns - 1) & 15));
        pk_put16(k->lam0);
        pk_put16(k->idx);
        pk[pk_len++] = (uint8_t)(k->layer << 4 | k->r);
    }
    e->valid = 1;
    e->lam0 = k->lam0;
    e->base = k->idx;
    e->r = k->r;
    pk_runs_put(k, 0);
    pk_total[PK_RING] += pk_len - start;
    /* the CPU's side, from the bytes alone */
    const uint8_t *p = pk + start;
    Packet d;
    memset(&d, 0, sizeof d);
    uint8_t b0 = *p++;
    d.nruns = (b0 & 15) + 1;
    if (b0 & 0x80) {
        d.layer = (b0 >> 4) & 3;
        RingState *s = &dec_rs[d.layer];
        if (!s->valid) { fprintf(stderr, "a continued row has nothing to continue\n"); exit(3); }
        d.lam0 = (s->lam0 + RWG) % TOT;
        d.idx = (s->base + RWG) % TOT;
        d.r = s->r;
    } else {
        d.lam0 = pk_get16(&p);
        d.idx = pk_get16(&p);
        uint8_t lb = *p++;
        d.layer = lb >> 4;
        d.r = lb & 15;
    }
    dec_rs[d.layer].valid = 1;
    dec_rs[d.layer].lam0 = d.lam0;
    dec_rs[d.layer].base = d.idx;
    dec_rs[d.layer].r = d.r;
    pk_runs_get(&p, &d);
    if (p != pk + pk_len) { fprintf(stderr, "record has a different length than written\n"); exit(3); }
    for (int i = 0; i < d.nruns; i++) {
        BlitReg r;
        ring_run_regs(page, d.lam0, ringbase, d.idx, d.r, d.a[i], d.n[i], d.em1[i], d.em3[i], &r);
        if (!reg_eq(&r, &exec[i])) { fprintf(stderr, "a ring record does not give the registers that were executed\n"); exit(3); }
    }
}

static void draw_piece_ring(uint32_t page, uint32_t lam0, int ng, const uint16_t *m, uint32_t ringbase, int layer_row,
                            int u0, int delta, int layer)
{
    int r = pmod(delta, 16), q = (delta - r) / 16;
    int ga[PG], gb[PG];
    int nb = decompose(m, ng, ga, gb);
    ntouched = 0;
    Packet pkt;
    BlitReg regs[PG];
    memset(&pkt, 0, sizeof pkt);
    pkt.kind = PK_RING;
    pkt.nruns = nb;
    pkt.layer = layer;
    pkt.lam0 = lam0;
    pkt.r = r;
    pkt.idx = (uint32_t)pmod(pmod(layer_row, RH) * RWG + (u0 - 1 - q), TOT);
    for (int i = 0; i < nb; i++) {
        int a = ga[i], b = gb[i], n = b - a + 1;
        pkt.a[i] = a;
        pkt.n[i] = n;
        pkt.em1[i] = m[a];
        pkt.em3[i] = m[b];
        ring_run_regs(page, lam0, ringbase, pkt.idx, r, a, n, m[a], m[b], &regs[i]);
        blit_run(&regs[i]);
        for (int k = a; k <= b; k++) touched[ntouched++] = (int)(lam0 + (uint32_t)k);
    }
    pk_check_ring(page, ringbase, &pkt, regs);
}

/* The window as the Videl fetches it from the page memory. */
static int videl_verify(const RPage *pg, const RFrame *f)
{
    static uint16_t fp[PPU_H * 128];
    ppu_planar8(&f->F[0][0], fp);
    int bad = 0;
    int lam0 = ring_lam(f->Wy, f->u0);
    for (int r = 0; r < PPU_H; r++) {
        int ga = lam0 + r * RWG;
        if (ga + f->nwg > PAGE_GROUPS) { fprintf(stderr, "Videl reads past the page\n"); exit(3); }
        uint8_t pix[17 * 16];
        for (int k = 0; k < f->nwg; k++)
            for (int b = 0; b < 16; b++) {
                int v = 0;
                for (int p = 0; p < 8; p++) {
                    const uint8_t *q = mem + pg->off + (uint32_t)(ga + k) * 16 + p * 2;
                    v |= (((q[0] << 8 | q[1]) >> (15 - b)) & 1) << p;
                }
                pix[k * 16 + b] = (uint8_t)v;
            }
        for (int g = 0; g < GW; g++)
            for (int p = 0; p < 8; p++) {
                unsigned w = 0;
                for (int b = 0; b < 16; b++) w |= (unsigned)((pix[f->s + g * 16 + b] >> p) & 1) << (15 - b);
                if ((uint16_t)w != fp[r * 128 + g * 8 + p]) bad++;
            }
    }
    return bad;
}

/* ---- sprite instance packets ---------------------------------------------------
 *
 * LC_INSTANCE=1: the sprite rows of an update are gathered per OAM entry (the order
 * of the blits does not matter, the masks are disjoint) and each instance, the
 * consecutive rows of one entry drawn this update, goes out as one packet:
 *
 *   call     0xC0, program number (16), first row's lam0 (16), G0 (byte)         6 bytes
 *   define   0xC1 (kept) or 0xC2 (run once), rows - 1 (byte), lam0 (16), G0      5 bytes,
 *            then the rows, each: runs - 1 (byte), source row number (16),
 *            W (5) | first group - G0 (5) | skew (4) | 2 spare (16), its runs (as
 *            above, first groups counted from G0)
 *
 * G0 is the first destination group of the instance's leftmost row, the program's
 * groups are relative to it, so one program serves the sprite at every x with the
 * same shift (the skew is in it); the rows follow each other by one ring row. A
 * program is what a precreated stefastbobs2 routine would be, as a record the CPU
 * loop interprets (no code is generated, the cache-resident loop stays). The
 * programs are kept in a store that is emptied with the source row cache (a
 * program holds source row numbers) and when it fills; a program defined while the
 * cache flushed under it is run once and not kept. The CPU side is modelled as its
 * own store, filled from the bytes of the define packets, and every packet is
 * checked against the registers that were executed. */
#define PROG_MAX 4096
static int instance_on;
static uint32_t prog_cap, prog_used;
static uint16_t prog_hash[1 << 13];
static uint64_t prog_key[PROG_MAX];
static int prog_n;
static uint8_t dec_mem[1 << 20];
static uint32_t dec_off[PROG_MAX], dec_len[PROG_MAX], dec_used;
static int dec_rows[PROG_MAX], dec_n;
static long pi_stat[6];           /* calls, kept defines, run-once defines, call bytes, define bytes, store flushes */

/* LC_PROG_LRU=1: the program store in slots of four sizes (a share of the store each), a
 * program in the smallest slot that holds it, the least recently used program of the
 * size evicted when none is free; define packets name their slot (0xC3). The slots are
 * the same on the CPU side, which keeps the bytes of each in its own copy. */
#define PCLASSES 4
static const uint32_t pc_size[PCLASSES] = {128, 256, 512, 1024};
static const int pc_share[PCLASSES] = {3, 3, 2, 1};
typedef struct { uint64_t key; uint32_t stamp, len; int used, rows; } PSlot;
static int prog_lru, nslot, pc_first[PCLASSES], pc_count[PCLASSES];
static PSlot *pslot;
static uint8_t *dec_slot;         /* CPU side: 1 KB per slot */
static uint32_t pstamp;
static long pi_evict;
static int prog_random;           /* LC_PROG_POLICY=random: a random victim, not the least recently used */
static int prog_bypass;           /* LC_PROG_POLICY=bypass: never evict, a program that finds no free slot runs once */
static uint32_t prog_rng = 0x1234567u;

static void prog_init_lru(void)
{
    int total = 0;
    for (int c = 0; c < PCLASSES; c++) {
        pc_first[c] = total;
        pc_count[c] = (int)((uint64_t)prog_cap * (uint64_t)pc_share[c] / 9 / pc_size[c]);
        total += pc_count[c];
    }
    nslot = total;
    free(pslot);
    free(dec_slot);
    pslot = calloc((size_t)(nslot ? nslot : 1), sizeof *pslot);
    dec_slot = calloc((size_t)(nslot ? nslot : 1), 1024);
}

static void prog_flush(void)
{
    memset(prog_hash, 0, sizeof prog_hash);
    prog_n = 0;
    prog_used = 0;
    dec_n = 0;
    dec_used = 0;
    if (prog_lru && pslot) {
        memset(pslot, 0, (size_t)nslot * sizeof *pslot);
        memset(dec_rows, 0, sizeof dec_rows);
    }
    pi_stat[5]++;
}

static void pk_rows_put(const RowOut *rows, int n, int G0)
{
    for (int i = 0; i < n; i++) {
        Packet t = rows[i].pkt;
        t.gd0 -= G0;
        for (int j = 0; j < t.nruns; j++) t.a[j] -= G0;
        pk[pk_len++] = (uint8_t)((t.nruns - 1) & 15);
        pk_put16(t.idx);
        pk_put16((uint32_t)(t.W << 11 | t.gd0 << 6 | t.s << 2));
        pk_runs_put(&t, 1);
    }
}

static void pk_rows_get(const uint8_t **p, int n, Packet *out)
{
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].nruns = (*(*p)++ & 15) + 1;
        out[i].idx = pk_get16(p);
        uint32_t h = pk_get16(p);
        out[i].W = (int)(h >> 11);
        out[i].gd0 = (int)((h >> 6) & 31);
        out[i].s = (int)((h >> 2) & 15);
        pk_runs_get(p, &out[i]);
    }
}

/* Writes the packet of an instance (n consecutive rows, drawn), reads it back and
 * checks every register. epoch0: the cache epoch before the first row was drawn. */
static int pk_instance(uint32_t page, const RowOut *rows, int n, int epoch0)
{
    int was_call = 0;
    int G0 = 99;
    for (int i = 0; i < n; i++)
        if (rows[i].pkt.gd0 < G0) G0 = rows[i].pkt.gd0;
    uint32_t lam_first = rows[0].pkt.lam0;
    uint32_t start = pk_len;
    pk[pk_len++] = 0xC1;
    pk[pk_len++] = (uint8_t)(n - 1);
    pk_put16(lam_first);
    pk[pk_len++] = (uint8_t)G0;
    uint32_t rs = pk_len;
    pk_rows_put(rows, n, G0);
    uint32_t re = pk_len;
    uint64_t key = (1469598103934665603ull ^ (uint64_t)n) * 1099511628211ull;
    for (uint32_t j = rs; j < re; j++) key = (key ^ pk[j]) * 1099511628211ull;
    uint32_t hi = (uint32_t)key & 8191;
    int id = -1;
    if (prog_lru) {
        for (int k = 0; k < nslot; k++)
            if (pslot[k].used && pslot[k].key == key && pslot[k].rows == n) { id = k; break; }
        if (id >= 0) pslot[id].stamp = ++pstamp;
    } else {
        while (prog_hash[hi]) {
            if (prog_key[prog_hash[hi] - 1] == key) { id = prog_hash[hi] - 1; break; }
            hi = (hi + 1) & 8191;
        }
    }
    if (id >= 0) {
        pk_len = start;
        pk[pk_len++] = 0xC0;
        pk_put16((uint32_t)id);
        pk_put16(lam_first);
#ifdef LC_MUT_INSTG0
        pk[pk_len++] = (uint8_t)(G0 + 1);
#else
        pk[pk_len++] = (uint8_t)G0;
#endif
        pi_stat[0]++;
        was_call = 1;
        pi_stat[3] += pk_len - start;
    } else {
        uint32_t len = re - rs;
        if (prog_lru) {
            int c = 0;
            while (c < PCLASSES && pc_size[c] < len) c++;
            int free_slot = -1;
            if (c < PCLASSES)
                for (int k = pc_first[c]; k < pc_first[c] + pc_count[c]; k++)
                    if (!pslot[k].used) { free_slot = k; break; }
            if (epoch0 == cache_epoch && c < PCLASSES && pc_count[c] > 0 && (free_slot >= 0 || !prog_bypass)) {
                int best = -1;
                for (int k = pc_first[c]; k < pc_first[c] + pc_count[c]; k++) {
                    if (!pslot[k].used) { best = k; break; }
                    if (best < 0 || pslot[k].stamp < pslot[best].stamp) best = k;
                }
                if (prog_random && pslot[best].used) {
                    /* every slot of the size is in use: a random victim instead of the oldest */
                    prog_rng ^= prog_rng << 13;
                    prog_rng ^= prog_rng >> 17;
                    prog_rng ^= prog_rng << 5;
                    best = pc_first[c] + (int)(prog_rng % (uint32_t)pc_count[c]);
                }
                if (pslot[best].used) pi_evict++;
                pslot[best].used = 1;
                pslot[best].key = key;
                pslot[best].len = len;
                pslot[best].rows = n;
                pslot[best].stamp = ++pstamp;
                id = best;
                /* a define that names its slot: two bytes more in the header */
                memmove(pk + rs + 2, pk + rs, re - rs);
                pk_len += 2;
                pk[start] = 0xC3;
                pk[start + 1] = (uint8_t)(n - 1);
                pk[start + 2] = (uint8_t)(id >> 8);
#ifdef LC_MUT_SLOT
                pk[start + 3] = (uint8_t)(id + 1);
#else
                pk[start + 3] = (uint8_t)id;
#endif
                pk[start + 4] = (uint8_t)(lam_first >> 8);
                pk[start + 5] = (uint8_t)lam_first;
                pk[start + 6] = (uint8_t)G0;
                pi_stat[1]++;
            } else {
                pk[start] = 0xC2;
                pi_stat[2]++;
            }
        } else if (epoch0 == cache_epoch && prog_n < PROG_MAX && prog_used + len <= prog_cap) {
            id = prog_n++;
            prog_key[id] = key;
            prog_hash[hi] = (uint16_t)(id + 1);
            prog_used += len;
            pi_stat[1]++;
        } else {
            pk[start] = 0xC2;
            pi_stat[2]++;
        }
        pi_stat[4] += pk_len - start;
    }
    pk_total[PK_ENTRY] += pk_len - start;

    /* the CPU's side, from the bytes and its own store */
    const uint8_t *p = pk + start;
    uint8_t kind = *p++;
    static Packet d[64];
    int dn, G;
    uint32_t lamf;
    if (kind == 0xC0) {
        uint32_t pid = pk_get16(&p);
        lamf = pk_get16(&p);
        G = *p++;
        if (prog_lru ? ((int)pid >= nslot || !dec_rows[pid]) : (int)pid >= dec_n) {
            fprintf(stderr, "a call names a program the CPU does not have\n");
            exit(3);
        }
        dn = dec_rows[pid];
        const uint8_t *qb = prog_lru ? dec_slot + (size_t)pid * 1024 : dec_mem + dec_off[pid];
        const uint8_t *q = qb;
        pk_rows_get(&q, dn, d);
        if (q != qb + dec_len[pid]) { fprintf(stderr, "a stored program is read with another length\n"); exit(3); }
    } else {
        dn = *p++ + 1;
        uint32_t spid = 0;
        if (kind == 0xC3) spid = pk_get16(&p);
        lamf = pk_get16(&p);
        G = *p++;
        const uint8_t *q0 = p;
        pk_rows_get(&p, dn, d);
        if (kind == 0xC3) {
            uint32_t len = (uint32_t)(p - q0);
            if ((int)spid >= nslot || len > 1024 || (int)spid != id) {
                fprintf(stderr, "the CPU's slots and the DSP's disagree\n");
                exit(3);
            }
            memcpy(dec_slot + (size_t)spid * 1024, q0, len);
            dec_len[spid] = len;
            dec_rows[spid] = dn;
        }
        if (kind == 0xC1) {
            uint32_t len = (uint32_t)(p - q0);
            if (dec_n != id || dec_used + len > sizeof dec_mem) { fprintf(stderr, "the CPU's store and the DSP's disagree\n"); exit(3); }
            memcpy(dec_mem + dec_used, q0, len);
            dec_off[dec_n] = dec_used;
            dec_len[dec_n] = len;
            dec_rows[dec_n] = dn;
            dec_used += len;
            dec_n++;
        }
    }
    if (p != pk + pk_len) { fprintf(stderr, "instance record has a different length than written\n"); exit(3); }
    if (dn != n) { fprintf(stderr, "instance has another number of rows than written\n"); exit(3); }
    for (int i = 0; i < dn; i++) {
        uint32_t lam0 = (lamf + (uint32_t)i * RWG) % TOT;
        if (d[i].nruns != rows[i].pkt.nruns) { fprintf(stderr, "a row has another number of runs than written\n"); exit(3); }
        for (int j = 0; j < d[i].nruns; j++) {
            BlitReg r;
            int a_eff;
            entry_run_regs(page, lam0, scratch_base + d[i].idx * 16, d[i].W, G + d[i].gd0, d[i].s, G + d[i].a[j],
                           G + d[i].a[j] + d[i].n[j] - 1, d[i].em1[j], d[i].em3[j], &r, &a_eff);
            if (!reg_eq(&r, &rows[i].regs[j])) {
                fprintf(stderr, "an instance record does not give the registers that were executed\n");
                exit(3);
            }
        }
    }
    return was_call;
}

/* What a blit wrote near the end of a ring goes to its other copy. */
static void sync_touched(uint8_t *page, uint8_t *dupmark, RStats *st)
{
    for (int k = 0; k < ntouched; k++) {
        int L = touched[k];
        if (L >= TOT) {
            st->spills++;
#ifndef LC_MUT_SPILL
            memcpy(page + (uint32_t)(L - TOT) * 16, page + (uint32_t)L * 16, 16);
            rec_copy((uint32_t)(page - mem) + (uint32_t)(L - TOT) * 16, (uint32_t)(page - mem) + (uint32_t)L * 16, 16);
            dupmark[L - TOT] = 1;
#endif
        } else if (L < DUPG) {
            memcpy(page + (uint32_t)(L + TOT) * 16, page + (uint32_t)L * 16, 16);
            rec_copy((uint32_t)(page - mem) + (uint32_t)(L + TOT) * 16, (uint32_t)(page - mem) + (uint32_t)L * 16, 16);
            dupmark[L] = 1;
        }
    }
}

typedef struct { const Piece *p; int r; uint16_t m[PG]; } SItem;
static SItem sitems[8192];
static int nsitems;

/* Draws the gathered sprite rows instance by instance. */
static void draw_instances(uint32_t page_off, uint8_t *page, uint8_t *dupmark, RStats *st)
{
    static RowOut rowbuf[64];
    int inst_idx[64];
    for (int o = 0; o < 128; o++) {
        int n = 0, prev_r = -5, epoch0 = cache_epoch;
        for (int j = 0; j <= nsitems; j++) {
            int end = j == nsitems;
            if (!end && sitems[j].p->i != o) continue;
            int cont = !end && n > 0 && sitems[j].r == prev_r + 1 && n < 64;
            if (n > 0 && (end || !cont)) {
                uint32_t before_len = pk_len;
                int call = pk_instance(page_off, rowbuf, n, epoch0);
                long inst_bytes = (long)(pk_len - before_len);
                /* pure: every row shows all of the entry's opaque pixels (nothing occludes it) and is
                 * drawn whole, so its program does not depend on what lies over or beside it */
                int pure = 1;
                for (int k = 0; k < n && pure; k++) {
                    int pc = 0;
                    for (int g = 0; g < PG; g++) pc += __builtin_popcount(sitems[inst_idx[k]].p->gm[g]);
                    if (pc != sitems[inst_idx[k]].p->sil || memcmp(sitems[inst_idx[k]].m, sitems[inst_idx[k]].p->gm, sizeof sitems[0].m)) pure = 0;
                }
                if (pure) { st->sp_pure++; st->sp_pure_hit += call; }
                else {
                    st->sp_occ++;
                    st->sp_occ_hit += call;
                    st->sp_occ_bytes += inst_bytes;
                    /* at best, with the silhouette's program always found: a call, and the rows that are not
                     * the silhouette's sent explicitly (a row index and the row's record) */
                    uint32_t save = pk_len;
                    long pot = 7;
                    for (int k = 0; k < n; k++) {
                        const Piece *pp = sitems[inst_idx[k]].p;
                        int pc = 0;
                        for (int g = 0; g < PG; g++) pc += __builtin_popcount(pp->gm[g]);
                        if (pc == pp->sil && !memcmp(sitems[inst_idx[k]].m, pp->gm, sizeof sitems[0].m)) continue;
                        pk_len = save;
                        pk_rows_put(&rowbuf[k], 1, rowbuf[k].pkt.gd0);
                        pot += 1 + (long)(pk_len - save);
                    }
                    pk_len = save;
                    st->sp_occ_pot += pot;
                }
                n = 0;
            }
            if (end) break;
            if (n == 0) epoch0 = cache_epoch;
            Stats sc;
            memset(&sc, 0, sizeof sc);
            draw_piece_planar_ex(page_off, (uint32_t)sitems[j].p->lam0, PG, sitems[j].p, sitems[j].m, &sc, &rowbuf[n]);
            st->entry_bytes += sc.entry_bytes;
            sync_touched(page, dupmark, st);
            inst_idx[n] = j;
            n++;
            prev_r = sitems[j].r;
        }
    }
}

/* Estimate of precreated sprite programs (the stefastbobs2 idea, driven by the
 * DSP): the rows one sprite has drawn this update, consecutive rows of one OAM
 * entry, form an instance. Its program is the sequence of its row records
 * without the destination position, which a program does not contain (the
 * destination is relative to a base it is called with). An instance whose
 * program was seen before costs a call (opcode, program number, base: 5 bytes);
 * one that was not costs the program once (its rows' records without their
 * first-group field, plus 4 bytes) and the call. Rows drawn clipped to the dirty
 * groups have other masks and so are other programs. As bytes of record, not as
 * code: generated code for a run takes several times more than its record. */
static uint64_t prog_seen[1 << 16];

static void prog_stats(RStats *st)
{
    for (int o = 0; o < 128; o++) {
        int prev = -1, first_gd0 = 0;
        uint64_t key = 0;
        long rows = 0, bytes = 0;
        for (int j = 0; j <= nelog; j++) {
            int end = j == nelog;
            if (!end && elog[j].owner != o) continue;
            int cont = !end && prev >= 0 && elog[j].lam0 == (elog[prev].lam0 + RWG) % TOT;
            if (prev >= 0 && (end || !cont)) {             /* the instance so far ends */
                uint32_t i = (uint32_t)key & 0xFFFF;
                int hit = 0;
                while (prog_seen[i]) {
                    if (prog_seen[i] == (key | 1)) { hit = 1; break; }
                    i = (i + 1) & 0xFFFF;
                }
                st->sp_inst++;
                st->sp_cur += bytes;
                st->sp_inv += 5;
                if (hit) st->sp_hits++;
                else {
                    prog_seen[i] = key | 1;
                    st->sp_new += bytes - 2 * rows + 4;
                }
                prev = -1;
                key = 0;
                rows = 0;
                bytes = 0;
            }
            if (end) break;
            if (prev < 0) { key = 1469598103934665603ull; first_gd0 = elog[j].gd0; }
            key = (key ^ elog[j].h) * 1099511628211ull;
            key = (key ^ (uint64_t)(uint32_t)(elog[j].gd0 - first_gd0)) * 1099511628211ull;
            rows++;
            bytes += elog[j].len;
            prev = j;
        }
    }
}

static void rpage_update(RPage *pg, const RFrame *f, RStats *st)
{
    static uint8_t inwin[TOT], dirty[TOT], dupmark[TOT];
    uint8_t *page = mem + pg->off;
    memset(inwin, 0, sizeof inwin);
    memset(dirty, 0, sizeof dirty);
    memset(dupmark, 0, sizeof dupmark);
    scratch_used = 0;
    pk_reset();
    long pke0 = pk_total[0], pkr0 = pk_total[1];
    cache_frame_start();
    if (instance_on && prog_used + prog_cap / 4 > prog_cap) prog_flush();
    long pi0[6];
    memcpy(pi0, pi_stat, sizeof pi0);
    long c_h0 = cs.hits, c_m0 = cs.misses, c_f0 = cs.flushes;
    for (int r = 0; r < PPU_H; r++)
        for (int k = 0; k < f->nwg; k++) inwin[ring_lam(f->Wy + r, f->u0 + k)] = 1;
    for (int l = 0; l < TOT; l++) {
        if (!inwin[l]) continue;
#ifdef LC_MUT_EXPO
        if (!pg->valid[l]) { st->exposed++; }
#else
        if (!pg->valid[l]) { dirty[l] = 1; st->exposed++; }
#endif
        else if (pg->pver[l] != cver[l]) { dirty[l] = 1; st->bgd++; }
    }
    /* pieces, page row by page row */
    for (int v = 0; v < RH; v++) {
        int r = pmod(v - f->Wy, RH);
        int inw = r < PPU_H;
        const Piece *n = inw ? f->pc[r] : NULL;
        int nn = inw ? f->np[r] : 0, no = pg->rnp[v];
        const Piece *o = pg->rc[v];
        for (int a = 0; a < no; a++) {
            int b = 0;
            while (b < nn && n[b].i != o[a].i) b++;
            if (b < nn && n[b].h == o[a].h) continue;
            st->changed++;
            for (int g = 0; g < PG; g++)
                if (o[a].gm[g]) {
                    int l = (o[a].lam0 + g) % TOT;
                    if (inwin[l]) dirty[l] = 1;
                }
        }
        for (int b = 0; b < nn; b++) {
            int a = 0;
            while (a < no && o[a].i != n[b].i) a++;
            if (a < no && o[a].h == n[b].h) continue;
            for (int g = 0; g < PG; g++)
                if (n[b].gm[g]) {
                    int l = (n[b].lam0 + g) % TOT;
                    if (inwin[l]) dirty[l] = 1;
                }
        }
    }
    /* restore (both copies where a group has one) */
    int prev = -2;
    for (int l = 0; l < TOT; l++) {
        if (!dirty[l]) continue;
        memcpy(page + (uint32_t)l * 16, mem + clean_off + (uint32_t)l * 16, 16);
        rec_copy((uint32_t)(page - mem) + (uint32_t)l * 16, clean_off + (uint32_t)l * 16, 16);
#ifndef LC_MUT_DUP
        if (l < DUPG) {
            memcpy(page + (uint32_t)(l + TOT) * 16, page + (uint32_t)l * 16, 16);
            rec_copy((uint32_t)(page - mem) + (uint32_t)(l + TOT) * 16, (uint32_t)(page - mem) + (uint32_t)l * 16, 16);
            dupmark[l] = 1;
        }
#endif
        st->dirty++;
        if (l != prev + 1) st->runs++;
        prev = l;
    }
    /* draw */
    Stats scratch_stats;
    nsitems = 0;
    static uint8_t act[128];
    memset(act, 0, sizeof act);
    if (instance_on == 2)
        for (int r = 0; r < PPU_H; r++)
            for (int q = 0; q < f->np[r]; q++) {
                const Piece *p = &f->pc[r][q];
                if (p->i >= 128) continue;
                for (int g = 0; g < PG; g++)
                    if (p->gm[g] && dirty[(p->lam0 + g) % TOT]) act[p->i] = 1;
            }
    for (int r = 0; r < PPU_H; r++)
        for (int q = 0; q < f->np[r]; q++) {
            const Piece *p = &f->pc[r][q];
            uint16_t m[PG];
            int any = 0;
            for (int g = 0; g < PG; g++) {
                m[g] = dirty[(p->lam0 + g) % TOT] ? p->gm[g] : 0;
                any |= m[g] != 0;
            }
            if (instance_on == 2 && p->i < 128) {
                /* the whole instance, unclipped, when any of it touches a dirty group: drawing a
                 * piece's visible pixels over groups that are not dirty changes nothing there */
                if (!act[p->i]) continue;
                memcpy(m, p->gm, sizeof m);
                any = 1;
            }
            if (!any) continue;
            int gr = 0, nbl = count_blits(m, PG, &gr);
            st->blits += nbl;
            if (p->i >= 128) st->ov_blits += nbl;
            st->blit_groups += gr;
            st->pieces++;
            if (instance_on && p->i < 128) {           /* drawn below, instance by instance */
                if (nsitems >= 8192) { fprintf(stderr, "too many sprite rows\n"); exit(3); }
                sitems[nsitems].p = p;
                sitems[nsitems].r = r;
                memcpy(sitems[nsitems].m, m, sizeof m);
                nsitems++;
                continue;
            }
            memset(&scratch_stats, 0, sizeof scratch_stats);
            if (overlay_ring && p->i >= 128) {
                int lb = p->i - 128;
                draw_piece_ring(pg->off, (uint32_t)p->lam0, PG, m, lring_off[lb], f->lWy[lb] + r, f->u0, f->Wx - f->lWx[lb], lb);
            } else {
                draw_piece_planar(pg->off, (uint32_t)p->lam0, PG, p, m, &scratch_stats);
                st->entry_bytes += scratch_stats.entry_bytes;
            }
            sync_touched(page, dupmark, st);
        }
    if (instance_on) draw_instances(pg->off, page, dupmark, st);
    prev = -2;
    for (int l = 0; l < TOT; l++) {
        if (!dupmark[l]) continue;
        st->dup_groups++;
        if (l != prev + 1) st->dup_runs++;
        prev = l;
    }
    st->c_hits = cs.hits - c_h0;
    st->c_misses = cs.misses - c_m0;
    st->c_flushes = cs.flushes - c_f0;
    st->c_used = cache_used;
    /* the duplicate copies follow by rule from the restores and the blits (the CPU's
     * restore loop copies a group below DUPG twice, and syncs what a blit wrote
     * there), so they need no record */
    st->pk_bytes = (long)pk_len + 3 * st->runs;
    st->pk_e = pk_total[0] - pke0;
    st->pk_r = pk_total[1] - pkr0;
    st->pi_calls = pi_stat[0] - pi0[0];
    st->pi_defs = pi_stat[1] - pi0[1];
    st->pi_once = pi_stat[2] - pi0[2];
    st->pi_cb = pi_stat[3] - pi0[3];
    st->pi_db = pi_stat[4] - pi0[4];
    prog_stats(st);
    for (int l = 0; l < TOT; l++) {
        if (dirty[l]) pg->pver[l] = cver[l];
        pg->valid[l] = inwin[l];
    }
    for (int v = 0; v < RH; v++) {
        int r = pmod(v - f->Wy, RH);
        if (r < PPU_H) {
            memcpy(pg->rc[v], f->pc[r], sizeof(Piece) * f->np[r]);
            pg->rnp[v] = f->np[r];
        } else pg->rnp[v] = 0;
    }
    pg->init = 1;
    st->pbad = videl_verify(pg, f);
}

static void ring_init(void)
{
    uint32_t page_bytes = (uint32_t)PAGE_GROUPS * 16;
    clean_off = 2 * page_bytes;
    uint32_t lr_bytes = (uint32_t)(TOT + DUPS) * 16;
    for (int b = 0; b < 4; b++) lring_off[b] = clean_off + (uint32_t)TOT * 16 + (uint32_t)b * lr_bytes;
    scratch_base = lring_off[3] + lr_bytes;
    scratch_size = 1 << 20;
    /* LC_COST=g,b,c,h: unit costs for the estimate (see cw); LC_CHOOSER=cost: choose the standing set by it */
    if (getenv("LC_COST")) sscanf(getenv("LC_COST"), "%lf,%lf,%lf,%lf", &cw[0], &cw[1], &cw[2], &cw[3]);
    if (getenv("LC_HORIZON")) chooser_horizon = atof(getenv("LC_HORIZON"));
    chooser_cost = getenv("LC_CHOOSER") ? (!strcmp(getenv("LC_CHOOSER"), "cost") ? 1 : !strcmp(getenv("LC_CHOOSER"), "exact") ? 2 : 0) : 0;
    /* LC_OVERLAY=ring: overlay layers are blitted from rings of their own */
    overlay_ring = getenv("LC_OVERLAY") && !strcmp(getenv("LC_OVERLAY"), "ring");
    /* LC_INSTANCE=1: sprite rows go out as instance packets; LC_PROG_KB: the program store */
    instance_on = getenv("LC_INSTANCE") ? atoi(getenv("LC_INSTANCE")) : 0;   /* 2: whole instances, unclipped */
    prog_cap = (getenv("LC_PROG_KB") ? (uint32_t)strtoul(getenv("LC_PROG_KB"), NULL, 0) : 64) * 1024;
    if (prog_cap > (1u << 19)) prog_cap = 1u << 19;
    /* LC_PROG_LRU=1: slots with least-recently-used eviction instead of a store that is emptied */
    prog_lru = getenv("LC_PROG_LRU") && atoi(getenv("LC_PROG_LRU")) != 0;
    prog_random = getenv("LC_PROG_POLICY") && !strcmp(getenv("LC_PROG_POLICY"), "random");
    prog_bypass = getenv("LC_PROG_POLICY") && !strcmp(getenv("LC_PROG_POLICY"), "bypass");
    pi_evict = 0;
    pstamp = 0;
    if (prog_lru) prog_init_lru();
    memsz = scratch_base + scratch_size;
    mem = calloc(1, memsz);
    const char *seed = getenv("LC_SEED");
    rng = 0x9E3779B9u ^ (seed ? (uint32_t)strtoul(seed, NULL, 0) * 2654435761u : 0);
    if (!rng) rng = 1;
    hw.buffer = rnd();
    hw.bus_word = (uint16_t)rnd();
    cache_init();
    memset(pi_stat, 0, sizeof pi_stat);
    prog_flush();
    pi_stat[5] = 0;
}

/* Bytes the program store holds (in slots: the slots' sizes). */
static uint32_t prog_held(void)
{
    if (!prog_lru) return prog_used;
    uint32_t t = 0;
    for (int c = 0; c < PCLASSES; c++)
        for (int k = pc_first[c]; k < pc_first[c] + pc_count[c]; k++)
            if (pslot[k].used) t += pc_size[c];
    return t;
}

/* hsim: the sim generator (movers, scroll) on the ring model. */
static uint32_t rec_crc(uint32_t c, unsigned w) { return ((c << 1) | (c >> 31)) + w; }

/* the Falcon palette longword the runtime loads for a CGRAM entry (load_clut8 in src/smkx_video.c) */
static void rec_palette(const Snap *s, uint32_t *pal)
{
    int bright = s->io[0x2100 - 0x2000] & 15, blank = s->io[0x2100 - 0x2000] & 0x80;
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)(s->cgram[i * 2] | s->cgram[i * 2 + 1] << 8);
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        if (blank) r = g = b = 0;
        r = (r * (uint32_t)(bright + 1) / 16) << 3;
        g = (g * (uint32_t)(bright + 1) / 16) << 3;
        b = (b * (uint32_t)(bright + 1) / 16) << 3;
        pal[i] = (r << 24) | (g << 16) | b;
    }
}

/* the window's bytes as the page holds them, from the first group the Videl fetches */
static uint32_t rec_window_crc(const uint8_t *m, uint32_t off, int lam0, int nwg)
{
    uint32_t c = 0;
    for (int r = 0; r < PPU_H; r++)
        for (int k = 0; k < nwg; k++) {
            const uint8_t *q = m + off + (uint32_t)(lam0 + r * RWG + k) * 16;
            for (int i = 0; i < 16; i += 2) c = rec_crc(c, (unsigned)(q[i] << 8 | q[i + 1]));
        }
    return c;
}

static uint32_t rec_mem_crc(const uint8_t *m)
{
    uint32_t c = 0;
    for (uint32_t a = 0; a + 1 < memsz; a += 2) c = rec_crc(c, (unsigned)(m[a] << 8 | m[a + 1]));
    return c;
}

/* the displayed words (HSCROLL applied) of a page, in the order ppu_planar8 has them */
static uint32_t rec_display_crc(const uint8_t *m, uint32_t off, int lam0, int nwg, int hs)
{
    uint32_t c = 0;
    for (int r = 0; r < PPU_H; r++) {
        uint8_t pix[17 * 16];
        for (int k = 0; k < nwg; k++)
            for (int b = 0; b < 16; b++) {
                int v = 0;
                for (int p = 0; p < 8; p++) {
                    const uint8_t *q = m + off + (uint32_t)(lam0 + r * RWG + k) * 16 + p * 2;
                    v |= (((q[0] << 8 | q[1]) >> (15 - b)) & 1) << p;
                }
                pix[k * 16 + b] = (uint8_t)v;
            }
        for (int g = 0; g < GW; g++)
            for (int p = 0; p < 8; p++) {
                unsigned w = 0;
                for (int b = 0; b < 16; b++) w |= (unsigned)((pix[hs + g * 16 + b] >> p) & 1) << (15 - b);
                c = rec_crc(c, w);
            }
    }
    return c;
}

static void rec_frame(const RPage *pg, const RFrame *f, const Snap *s)
{
    rec_flush();
    static uint32_t lastpal[256];
    static int havepal;
    uint32_t pal[256];
    rec_palette(s, pal);
    if (!havepal || memcmp(pal, lastpal, sizeof pal)) {
        rec_u16(REC_PAL);
        rec_u16(0);
        for (int i = 0; i < 256; i++) rec_u32(pal[i]);
        memcpy(lastpal, pal, sizeof pal);
        havepal = 1;
    }
    int lam0 = ring_lam(f->Wy, f->u0);
    static uint16_t fp[PPU_H * 128];
    ppu_planar8(&f->F[0][0], fp);
    uint32_t crcA = 0;
    for (int i = 0; i < PPU_H * 128; i++) crcA = rec_crc(crcA, fp[i]);
    if (getenv("LC_DUMPPIC") && !rec_frames) {
        /* the first frame as the Videl should show it: for a player test picture */
        char nm[512];
        snprintf(nm, sizeof nm, "%s.planar", getenv("LC_DUMPPIC"));
        FILE *fo = fopen(nm, "wb");
        for (int i = 0; i < PPU_H * 128; i++) { fputc(fp[i] >> 8, fo); fputc(fp[i] & 255, fo); }
        fclose(fo);
        snprintf(nm, sizeof nm, "%s.pal", getenv("LC_DUMPPIC"));
        fo = fopen(nm, "wb");
        for (int i = 0; i < 256; i++) { fputc(pal[i] >> 24, fo); fputc(pal[i] >> 16, fo); fputc(pal[i] >> 8, fo); fputc(pal[i], fo); }
        fclose(fo);
    }
    rec_u16(0);                                    /* END */
    rec_u16(pg->off ? 1 : 0);
    rec_u32(pg->off + (uint32_t)lam0 * 16);
    rec_u16((unsigned)f->s);
    rec_u16((unsigned)f->nwg);
    rec_u32(rec_window_crc(mem, pg->off, lam0, f->nwg));
    rec_u32(crcA);
    rec_u32(rec_mem_crc(mem));
    rec_frames++;
}

static void rec_write(const char *path)
{
    FILE *fo = fopen(path, "wb");
    if (!fo) { perror(path); exit(2); }
    uint8_t h[32];
    memset(h, 0, sizeof h);
    memcpy(h, "LCP1", 4);
    #define HP32(o, v) do { h[o] = (uint8_t)((v) >> 24); h[o + 1] = (uint8_t)((v) >> 16); h[o + 2] = (uint8_t)((v) >> 8); h[o + 3] = (uint8_t)(v); } while (0)
    HP32(4, (uint32_t)rec_frames);
    HP32(8, memsz);
    HP32(12, 0);
    HP32(16, (uint32_t)PAGE_GROUPS * 16);
    HP32(20, (uint32_t)RWG);
    HP32(24, (uint32_t)RH);
    fwrite(h, 1, sizeof h, fo);
    fwrite(rec_buf, 1, rec_len, fo);
    fclose(fo);
    fprintf(stderr, "recorded %d frames, %zu bytes -> %s\n", rec_frames, rec_len + sizeof h, path);
}

/* play: execute a recorded stream on a fresh memory and check every frame */
static int cmd_play(const char *path)
{
    FILE *fi = fopen(path, "rb");
    if (!fi) { perror(path); return 2; }
    fseek(fi, 0, SEEK_END);
    long sz = ftell(fi);
    fseek(fi, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    if (fread(d, 1, (size_t)sz, fi) != (size_t)sz) return 2;
    fclose(fi);
    #define G16(p) ((unsigned)((p)[0] << 8 | (p)[1]))
    #define G32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (uint32_t)(p)[3])
    if (memcmp(d, "LCP1", 4)) { fprintf(stderr, "not a stream\n"); return 2; }
    uint32_t nframes = G32(d + 4);
    memsz = G32(d + 8);
    uint32_t page1 = G32(d + 16);
    mem = calloc(1, memsz);
    rng = 0x9E3779B9u ^ 12345u;
    hw.buffer = rnd();
    hw.bus_word = (uint16_t)rnd();
    (void)page1;
    const uint8_t *q = d + 32;
    const uint8_t *end = d + sz;
    long bad = 0, blits = 0, copies = 0, datas = 0;
    uint32_t fr = 0;
    while (q < end) {
        unsigned op = G16(q);
        q += 2;
        if (op == REC_DATA) {
            uint32_t dst = G32(q);
            unsigned n = G16(q + 4);
            q += 6;
            memcpy(mem + dst, q, n);
            q += n + (n & 1);
            datas++;
        } else if (op == REC_COPY) {
            uint32_t dst = G32(q), src = G32(q + 4);
            unsigned n = G16(q + 8);
            q += 10;
            memmove(mem + dst, mem + src, n);
            copies++;
        } else if (op == REC_BLIT) {
            BlitReg r;
            r.src = G32(q);
            r.dst = G32(q + 4);
            r.xcnt = (uint16_t)G16(q + 8);
            r.ycnt = (uint16_t)G16(q + 10);
            r.sxi = (int16_t)G16(q + 12);
            r.syi = (int16_t)G16(q + 14);
            r.dxi = (int16_t)G16(q + 16);
            r.dyi = (int16_t)G16(q + 18);
            r.em1 = (uint16_t)G16(q + 20);
            r.em2 = (uint16_t)G16(q + 22);
            r.em3 = (uint16_t)G16(q + 24);
            r.fxsr = q[26];
            r.nfsr = q[27];
            r.skew = q[28];
            q += 30;
            blit_run(&r);
            blits++;
        } else if (op == REC_PAL) {
            q += 2 + 1024;
        } else if (op == 0) {
            unsigned page = G16(q);
            uint32_t addr = G32(q + 2);
            unsigned hs = G16(q + 6), nwg = G16(q + 8);
            uint32_t crcB = G32(q + 10), crcA = G32(q + 14), crcM = G32(q + 18);
            q += 22;
            (void)page;
            uint32_t poff = addr >= (uint32_t)PAGE_GROUPS * 16 ? (uint32_t)PAGE_GROUPS * 16 : 0;
            int lam0 = (int)((addr - poff) / 16);
            int b1 = rec_window_crc(mem, poff, lam0, (int)nwg) != crcB;
            int b2 = rec_display_crc(mem, poff, lam0, (int)nwg, (int)hs) != crcA;
            int b3 = rec_mem_crc(mem) != crcM;
            if (b1 || b2 || b3) {
                if (bad < 10) fprintf(stderr, "frame %u: window %s, display %s, memory %s\n", fr, b1 ? "WRONG" : "ok", b2 ? "WRONG" : "ok", b3 ? "WRONG" : "ok");
                bad++;
            }
            fr++;
        } else {
            fprintf(stderr, "bad op %u at %ld\n", op, (long)(q - d));
            return 2;
        }
    }
    printf("played %u of %u frames: %ld DATA, %ld COPY, %ld BLIT ops; %ld frames differ\n", fr, nframes, datas, copies, blits, bad);
    return bad != 0;
}

static char **real_files;              /* hreal: the snapshots of consecutive frames */

static int cmd_hsim(const uint8_t *rom, int argc, char **argv)
{
    if (argc != 7) { fprintf(stderr, "hsim: SNAP.BIN frames movers dx dy sx sy\n"); return 2; }
    ring_init();
    const char *recpath = getenv("LC_REC");
    if (recpath) rec_on = 1;
    static RPage *pages_r[2];
    for (int i = 0; i < 2; i++) {
        pages_r[i] = calloc(1, sizeof(RPage));
        pages_r[i]->off = (uint32_t)i * PAGE_GROUPS * 16;
        for (int l = 0; l < TOT; l++) pages_r[i]->pver[l] = -1;
    }
    RFrame *f = malloc(sizeof *f);
    Snap *base = snap_load(argv[0], rom);
    int frames = atoi(argv[1]), movers = atoi(argv[2]);
    int dx = atoi(argv[3]), dy = atoi(argv[4]), sx = atoi(argv[5]), sy = atoi(argv[6]);
    int via_hdma = real_files ? 0 : hdma_scroll(base);
    /* LC_LAYER_SX / LC_LAYER_SY = a,b,c,d: scroll per frame of BG1-4 (default: sx, sy for all) */
    int sxl[4] = {sx, sx, sx, sx}, syl[4] = {sy, sy, sy, sy};
    if (getenv("LC_LAYER_SX")) sscanf(getenv("LC_LAYER_SX"), "%d,%d,%d,%d", &sxl[0], &sxl[1], &sxl[2], &sxl[3]);
    if (getenv("LC_LAYER_SY")) sscanf(getenv("LC_LAYER_SY"), "%d,%d,%d,%d", &syl[0], &syl[1], &syl[2], &syl[3]);
    int any_scroll = 0;
    for (int b = 0; b < 4; b++) any_scroll |= sxl[b] || syl[b];
    /* LC_SHIFT=x,y: the moving entries start that far from where the snapshot has them */
    int sox = 0, soy = 0;
    if (getenv("LC_SHIFT")) sscanf(getenv("LC_SHIFT"), "%d,%d", &sox, &soy);
    int mv[128], nm = 0;
    {
        Frame *f0 = malloc(sizeof(Frame));
        frame_build(base, f0);
        if (!f0->ok) { printf("%s: not modelled (%s)\n", base->name, f0->why); return 1; }
        uint8_t seen[128] = {0};
        for (int y = 0; y < PPU_H; y++)
            for (int q = 0; q < f0->np[y]; q++) {
                int i = f0->pc[y][q].i;
                if (!seen[i] && nm < movers) { seen[i] = 1; mv[nm++] = i; }
            }
        free(f0);
    }
    printf("%s: ring %d x %d groups (%d KB per page), moving %d entries by (%d,%d) per frame, scroll (%d,%d) per frame\n",
           base->name, RWG, RH, PAGE_GROUPS * 16 / 1024, nm, dx, dy, sx, sy);
    printf("%-5s %5s %5s %5s %5s %6s %5s %5s %6s %5s %5s %6s %s\n", "frame", "pbad", "expo", "bg", "dirty", "runs",
           "blits", "ovblt", "pieces", "dup", "clean", "bytes", "");
    Track tr;
    memset(&tr, 0, sizeof tr);
    char lastcls[48] = "";
    RStats tot = {0};
    long n = 0, tbytes = 0;
    double tcost = 0;
    for (int t = 0; t < frames; t++) {
        Snap *s = real_files ? snap_load(real_files[t], rom) : snap_clone(base);
        if (real_files) { rframe_build(s, &tr, f); goto built; }
        for (int k = 0; k < nm; k++) {
            oam_move(s->oam, mv[k], dx * t + sox, dy * t + soy);
            for (int g = 0; g < s->noamseg; g++) oam_move(s->oamseg[g].oam, mv[k], dx * t + sox, dy * t + soy);
        }
        int dhl[4], dvl[4];
        for (int b = 0; b < 4; b++) {
            dhl[b] = sxl[b] * t;
            dvl[b] = syl[b] * t;
            s->bgofs[b * 2] = (uint16_t)(s->bgofs[b * 2] + dhl[b]);
            s->bgofs[b * 2 + 1] = (uint16_t)(s->bgofs[b * 2 + 1] + dvl[b]);
        }
        if (via_hdma && any_scroll && !patch_hdma_layers(s, dhl, dvl)) {
            printf("%s: the scroll comes from HDMA tables that cannot be patched\n", base->name);
            return 1;
        }
        rframe_build(s, &tr, f);
built:
        if (!f->ok) {
            printf("frame %d skipped: %s\n", t, f->why);
            free(s);
            continue;
        }
        if (getenv("LC_RIGID")) {
            static uint8_t pw[PPU_H][17 * 16];
            static int pu0, pwy, pnwg, have_p;
            if (have_p) {
                long cmp = 0, diff = 0;
                int dy0 = 0, dy1 = 0, dx0 = 0, dx1 = 0;
                for (int y = 0; y < PPU_H; y++) {
                    int wrow = f->Wy + y, py = wrow - pwy;
                    if (py < 0 || py >= PPU_H) continue;
                    for (int q = 0; q < f->nwg * 16; q++) {
                        int wxp = f->u0 * 16 + q - pu0 * 16;
                        if (wxp < 0 || wxp >= pnwg * 16) continue;
                        cmp++;
                        if (pw[py][wxp] != f->wide[y][q]) {
                            if (!diff) { dy0 = dy1 = y; dx0 = dx1 = q - f->s; }
                            diff++;
                            if (y < dy0) dy0 = y;
                            if (y > dy1) dy1 = y;
                            if (q - f->s < dx0) dx0 = q - f->s;
                            if (q - f->s > dx1) dx1 = q - f->s;
                        }
                    }
                }
                fprintf(stderr, "rigid t%d: %ld world pixels in common, %ld differ in rows %d-%d, window x %d..%d (W %d,%d)\n", t, cmp, diff, dy0, dy1, dx0, dx1, f->Wx, f->Wy);
            }
            memcpy(pw, f->wide, sizeof pw);
            pu0 = f->u0; pwy = f->Wy; pnwg = f->nwg; have_p = 1;
        }
        RStats st;
        memset(&st, 0, sizeof st);
        clean_update(f, &st);
        lring_update(f, &st);
        rpage_update(pages_r[t & 1], f, &st);
        if (rec_on) rec_frame(pages_r[t & 1], f, s);
        long bytes = st.blits * BLIT_REC + (st.runs + st.dup_runs) * RUN_REC;
        printf("t%-4d %5d %5ld %5ld %5ld %6ld %5ld %5ld %6ld %5ld %5ld %6ld %s%s\n", t, st.pbad, st.exposed, st.bgd,
               st.dirty, st.runs, st.blits, st.ov_blits, st.pieces, st.dup_groups, st.clean_groups, bytes,
               t < 2 ? "cold page " : "", strcmp(lastcls, f->cls) ? f->cls : "");
        snprintf(lastcls, sizeof lastcls, "%s", f->cls);
        if (t >= 2) {
            n++;
            tcost += cw[0] * (double)(st.clean_groups + st.lr_groups) +
                     cw[3] * (16.0 * (double)(st.clean_groups + st.lr_groups) + (double)st.pk_bytes + (double)st.entry_bytes) +
                     cw[1] * (double)st.blits + cw[2] * (double)(st.dirty + st.dup_groups);
            tot.pbad += st.pbad;
            tot.exposed += st.exposed;
            tot.bgd += st.bgd;
            tot.dirty += st.dirty;
            tot.runs += st.runs;
            tot.blits += st.blits;
            tot.ov_blits += st.ov_blits;
            tot.pieces += st.pieces;
            tot.dup_groups += st.dup_groups;
            tot.dup_runs += st.dup_runs;
            tot.clean_groups += st.clean_groups;
            tot.entry_bytes += st.entry_bytes;
            tot.spills += st.spills;
            tot.c_hits += st.c_hits;
            tot.c_misses += st.c_misses;
            tot.lr_groups += st.lr_groups;
            tot.pk_bytes += st.pk_bytes;
            tot.pk_e += st.pk_e;
            tot.pk_r += st.pk_r;
            tot.pi_calls += st.pi_calls;
            tot.pi_defs += st.pi_defs;
            tot.pi_once += st.pi_once;
            tot.pi_cb += st.pi_cb;
            tot.pi_db += st.pi_db;
            tot.sp_pure += st.sp_pure;
            tot.sp_pure_hit += st.sp_pure_hit;
            tot.sp_occ += st.sp_occ;
            tot.sp_occ_hit += st.sp_occ_hit;
            tot.sp_occ_bytes += st.sp_occ_bytes;
            tot.sp_occ_pot += st.sp_occ_pot;
            tot.sp_inst += st.sp_inst;
            tot.sp_hits += st.sp_hits;
            tot.sp_inv += st.sp_inv;
            tot.sp_new += st.sp_new;
            tot.sp_cur += st.sp_cur;
            tbytes += bytes;
        }
        free(s);
    }
    if (n)
        printf("warm: %ld updates, mean per update: exposed %.0f, bg-dirty %.0f, dirty %.0f, restore runs %.0f, blitter rows %.0f (overlay layers %.0f),"
               " dup groups %.0f, clean-ring groups %.0f, list bytes %.0f, source row bytes %.0f; wrong Videl words %d, groups written across the ring end %ld\n", n,
               (double)tot.exposed / n, (double)tot.bgd / n, (double)tot.dirty / n, (double)tot.runs / n,
               (double)tot.blits / n, (double)tot.ov_blits / n, (double)tot.dup_groups / n, (double)tot.clean_groups / n, (double)tbytes / n,
               (double)tot.entry_bytes / n, tot.pbad, tot.spills);
    if (n)
        printf("  list in packed records: %.0f bytes per update (against %.0f)\n", (double)tot.pk_bytes / n, (double)tbytes / n);
    if (n)
        printf("  model cost: %.0f microseconds per update at g=%.0f b=%.0f c=%.1f h=%.2f (assumed unit costs, not measured)\n",
               tcost / n, cw[0], cw[1], cw[2], cw[3]);
    if (n && instance_on)
        printf("  sprite instance packets: %.1f instances per update, %.1f%% calls of a kept program (%.0f B), %.1f kept defines"
               " + %.1f run once (%.0f B) per update; program store %u KB%s (%u KB held at the end), %ld flushes, %ld evictions\n",
               (double)(tot.pi_calls + tot.pi_defs + tot.pi_once) / n,
               tot.pi_calls + tot.pi_defs + tot.pi_once
                   ? 100.0 * (double)tot.pi_calls / (double)(tot.pi_calls + tot.pi_defs + tot.pi_once) : 0.0,
               (double)tot.pi_cb / n, (double)tot.pi_defs / n, (double)tot.pi_once / n, (double)tot.pi_db / n,
               prog_cap / 1024, prog_lru ? " in slots, LRU" : "", prog_held() / 1024, pi_stat[5], pi_evict);
    if (n && instance_on && getenv("LC_PKSTAT"))
        printf("  instances nothing occludes: %.1f per update, %.1f%% calls; occluded or clipped: %.1f per update, %.1f%% calls\n",
               (double)tot.sp_pure / n, tot.sp_pure ? 100.0 * (double)tot.sp_pure_hit / (double)tot.sp_pure : 0.0,
               (double)tot.sp_occ / n, tot.sp_occ ? 100.0 * (double)tot.sp_occ_hit / (double)tot.sp_occ : 0.0);
    if (n && instance_on && getenv("LC_PKSTAT"))
        printf("  the occluded ones cost %.0f B per update; with the silhouette's program always found and their own rows sent"
               " explicitly at most %.0f B\n", (double)tot.sp_occ_bytes / n, (double)tot.sp_occ_pot / n);
    if (n && getenv("LC_PKSTAT"))
        printf("  of it: entry rows %.0f B, ring rows %.0f B, restore and duplicate runs %.0f B\n", (double)tot.pk_e / n,
               (double)tot.pk_r / n, (double)(tot.pk_bytes - tot.pk_e - tot.pk_r) / n);
    if (n && getenv("LC_PKSTAT"))
        printf("  sprite programs (estimate): %.1f instances per update, %.1f%% found, calls %.0f B + new programs %.0f B per update,"
               " against %.0f B of entry rows now\n", (double)tot.sp_inst / n,
               tot.sp_inst ? 100.0 * (double)tot.sp_hits / (double)tot.sp_inst : 0.0, (double)tot.sp_inv / n,
               (double)tot.sp_new / n, (double)tot.sp_cur / n);
    if (n && getenv("LC_PKSTAT"))
        printf("  records, all updates: entry rows %ld B, ring rows %ld B; ring packets %ld (%ld full headers, %ld continued),"
               " runs %ld, em1 words %ld (%ld one run of ones), em3 words %ld (%ld)\n", pk_total[0], pk_total[1], pk_stat[0],
               pk_stat[4], pk_stat[5], pk_stat[1], pk_stat[2], pk_stat[6], pk_stat[3], pk_stat[7]);
    if (n && tot.lr_groups)
        printf("  %s overlay layers in rings of their own: %.0f groups = %.0f bytes per update for the DSP to send\n",
               overlay_ring ? "blitted from" : "estimate,", (double)tot.lr_groups / n, 16.0 * (double)tot.lr_groups / n);
    if (rec_on) rec_write(recpath);
    if (n && cache_on)
        printf("  source row cache %u KB: %.1f rows per update, %.1f%% hits, peak %u KB, %ld flushes (all updates)\n",
               cache_cap / 1024, (double)(tot.c_hits + tot.c_misses) / n,
               tot.c_hits + tot.c_misses ? 100.0 * (double)tot.c_hits / (double)(tot.c_hits + tot.c_misses) : 0.0,
               cs.peak / 1024, cs.flushes);
    return tot.pbad != 0;
}

/* blitfuzz: random pieces (random position, width, holes, pixels) drawn through
 * draw_piece_planar() under random dirty masks onto a page full of noise. The
 * row must equal the noise with exactly the masked pixels replaced, and nothing
 * else on the page may change. The blitter's leftovers are noise as well. */
static int cmd_blitfuzz(int iters, long seed)
{
    fr_init();
    rng = 0x9E3779B9u ^ (uint32_t)seed * 2654435761u;
    if (!rng) rng = 1;
    hw.buffer = rnd();
    hw.bus_word = (uint16_t)rnd();
    static Piece p;
    static uint8_t before[PAGEBYTES], exp_row[PSTRIDE];
    uint8_t *page = mem;
    long bad = 0, blits = 0, single = 0;
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < PAGEBYTES; i++) page[i] = (uint8_t)rnd();
        memcpy(before, page, PAGEBYTES);
        int y = (int)(rnd() % PPU_H);
        /* with the source row cache on, half of the pieces repeat the previous one's
         * pixels at another place, which has to hit and still draw right */
        static int sw;
        static uint8_t son[80], sv[80];
        int reuse = cache_on && it > 0 && (rnd() % 2);
        int w = reuse ? sw : 1 + (int)(rnd() % 80);
        int p0 = (int)(rnd() % (unsigned)(BWP - w + 1));
        memset(&p, 0, sizeof p);
        int mode = (int)(rnd() % 4);
        for (int k = p0; k < p0 + w; k++) {
            int on = mode == 0 ? 1 : mode == 1 ? (rnd() % 4 != 0) : mode == 2 ? (rnd() % 2) : ((k / 8) % 3 != 1);
            if (k == p0 || k == p0 + w - 1) on = 1;
            uint8_t v = (uint8_t)(1 + rnd() % 255);
            if (reuse) { on = son[k - p0]; v = sv[k - p0]; }
            else { son[k - p0] = (uint8_t)on; sv[k - p0] = v; }
            if (on) p.gm[k >> 4] |= (uint16_t)(0x8000u >> (k & 15));
            p.v[k] = v;
        }
        sw = w;
        uint16_t m[BWG];
        int dmode = (int)(rnd() % 3);
        for (int g = 0; g < BWG; g++) {
            int on = dmode == 0 ? 1 : dmode == 1 ? (rnd() % 4 != 0) : (rnd() % 2);
            m[g] = on ? p.gm[g] : 0;
        }
        /* the expectation, in pixels */
        uint8_t px[BWP];
        for (int g = 0; g < BWG; g++)
            for (int b = 0; b < 16; b++) {
                int v = 0;
                for (int pl = 0; pl < 8; pl++) {
                    const uint8_t *q = before + y * PSTRIDE + g * 16 + pl * 2;
                    v |= (((q[0] << 8 | q[1]) >> (15 - b)) & 1) << pl;
                }
                px[g * 16 + b] = (uint8_t)v;
                if (m[g] & (0x8000u >> b)) px[g * 16 + b] = p.v[g * 16 + b];
            }
        pack_row(px, exp_row);
        Stats st;
        memset(&st, 0, sizeof st);
        scratch_used = 0;
        pk_reset();
        draw_piece_planar((uint32_t)(page - mem), (uint32_t)(y * BWG), BWG, &p, m, &st);
        int ga[BWG], gb[BWG];
        blits += decompose(m, BWG, ga, gb);
        int diff = memcmp(page + y * PSTRIDE, exp_row, PSTRIDE) != 0;
        for (int i = 0; i < PAGEBYTES && !diff; i++)
            if ((i < y * PSTRIDE || i >= (y + 1) * PSTRIDE) && page[i] != before[i]) diff = 1;
        if (diff && bad++ < 5)
            printf("fuzz %d: y %d p0 %d w %d (skew %d) mode %d dmode %d differs\n", it, y, p0, w, p0 & 15, mode, dmode);
        single += decompose(m, BWG, ga, gb) == 1 && ga[0] == gb[0];
    }
    printf("blitfuzz: %d pieces, %ld blits (%ld single-group rows), %ld pages wrong\n", iters, blits, single, bad);
    if (cache_on)
        printf("  source row cache %u KB: %ld hits, %ld misses, %ld flushes\n", cache_cap / 1024, cs.hits, cs.misses,
               cs.flushes);
    return bad != 0;
}

int main(int argc, char **argv)
{
    smw_rules = getenv("LC_SMW") != NULL;
    if (argc >= 3 && !strcmp(argv[1], "hsim")) {
        long n;
        const uint8_t *rom = load(argv[2], &n);
        ppu_init();
        return cmd_hsim(rom, argc - 3, argv + 3);
    }
    if (argc == 3 && !strcmp(argv[1], "play")) return cmd_play(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "blitfuzz")) {
        return cmd_blitfuzz(atoi(argv[2]), strtol(argv[3], NULL, 0));
    }
    if (argc < 4) {
        fprintf(stderr, "usage: %s census|seq|sim rom.sfc SNAP.BIN...\n", argv[0]);
        return 2;
    }
    long n;
    const uint8_t *rom = load(argv[2], &n);
    ppu_init();
    if (!strcmp(argv[1], "census")) return cmd_census(rom, argc - 3, argv + 3);
    if (!strcmp(argv[1], "seq")) return cmd_seq(rom, argc - 3, argv + 3);
    if (!strcmp(argv[1], "hreal") && argc > 4) {
        /* the ring model on consecutive real frames: hreal rom.sfc SNAP.BIN... */
        /* leading frames the model does not cover (the exact renderer draws those) are skipped */
        int first = 0;
        for (; first < argc - 3; first++) {
            Snap *x = snap_load(argv[3 + first], rom);
            Frame *f0 = malloc(sizeof(Frame));
            frame_build(x, f0);
            int ok = f0->ok;
            free(f0);
            free(x);
            if (ok) break;
        }
        if (first == argc - 3) { printf("no frame the model covers\n"); return 1; }
        if (first) printf("%d leading frames not modelled, starting at %s\n", first, argv[3 + first]);
        static char nb[16];
        snprintf(nb, sizeof nb, "%d", argc - 3 - first);
        char *a[7] = {argv[3 + first], nb, "0", "0", "0", "0", "0"};
        real_files = argv + 3 + first;
        return cmd_hsim(rom, 7, a);
    }
    if (!strcmp(argv[1], "sim")) return cmd_sim(rom, argc - 3, argv + 3);
    if (!strcmp(argv[1], "dump")) return cmd_dump(rom, argc - 3, argv + 3);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
