/* Host model of the DSP's true-colour renderer (falcon/dsp/tc.asm): the
 * DSP program's executable description. See src/ppu_tc.h.
 *
 * Per row, per pixel a key: depth << 16 | source << 8 | CGRAM index;
 * source 0-3 BG1-4, 4 OBJ; key 0 is the backdrop. Depths are snes9x
 * 1.63's (gfx.cpp, RenderScreen and DrawOBJS): OBJ priority p is 4 + 4p.
 *   1. Each BG layer on the main or sub screen: its row into a layer line
 *      of keys (0 transparent), sliver by sliver from the converted tile
 *      rows; a horizontally flipped sliver is written right to left.
 *   2. The sprites into an OBJ line (the lowest OAM index last, so on top).
 *   3. Every line into the main line where TM shows it and TMW does not
 *      hide it, and into the sub line likewise (TS, TSW): the greater key.
 *   4. Per pixel the colour: main and sub keys, the colour window, CGWSEL,
 *      CGADSUB and the fixed colour, brightness, as ppu_render_565.
 */
#include "tc_model.h"
#include "ppu_tc.h"

#include <string.h>

static const uint8_t obj_sizes[8][2][2] = {   /* as snes_ppu.c */
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

/* BG depth by mode, layer, priority (snes9x's DO_BG Zh/Zl); mode 1's BG3
 * high is 17 with BGMODE bit 3 */
static const uint8_t depth[8][4][2] = {
    {{11, 15}, {10, 14}, {3, 7}, {2, 6}}, {{11, 15}, {10, 14}, {3, 7}, {0, 0}},
    {{7, 15}, {3, 11}}, {{7, 15}, {3, 11}}, {{7, 15}, {3, 11}}, {{7, 15}, {3, 11}}, {{7, 15}},
    {{6, 6}},
};
static const uint8_t mode_bpp[7][4] = {
    {2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0}, {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0},
};

uint32_t *tc_dbg_keys;                      /* debugging: main, sub keys per pixel */
int tc_model_band0, tc_model_band1;          /* rows drawn (the BAND command); 0, 0: all */
static const uint8_t *vram;
static uint8_t *fmtread;
static TcStats *st;
static uint32_t conv[128][5][256];
static uint8_t have[128][5];

static int fidx(int f) { return f == TC_F2 ? 0 : f == TC_F4 ? 1 : f == TC_F8 ? 2 : f == TC_RAW ? 3 : 4; }

/* word w of VRAM in conversion f, as the DSP holds the page */
static uint32_t cword(uint32_t w, int f)
{
    uint32_t pg = (w & 0x7FFF) >> 8;
    int i = fidx(f);
    if (!have[pg][i]) {
        tc_convert(vram + pg * 512, f, conv[pg][i]);
        have[pg][i] = 1;
    }
    fmtread[pg] |= (uint8_t)f;
    return conv[pg][i][w & 0xFF];
}

/* The 8 colour numbers of a character row (fields, ppu_tc.h); returns
 * its flags (TC_CLEAR: the DSP skips the row). */
static int row_pixels(uint32_t chrbase, uint32_t chr, int bpp, int fy, int c[8])
{
    if (bpp == 2) {
        uint32_t v = cword(chrbase + chr * 8 + (uint32_t)fy, TC_F2);
        if (!v) return TC_CLEAR;
        for (int x = 0; x < 8; x++) c[x] = (int)(v >> (21 - 3 * x)) & 7;
        return 0;
    }
    if (bpp == 4) {
        uint32_t a = cword(chrbase + chr * 16 + (uint32_t)fy, TC_F4);
        if (a & TC_CLEAR) return TC_CLEAR;
        uint32_t b = cword(chrbase + chr * 16 + 8 + (uint32_t)fy, TC_F4);
        for (int x = 0; x < 4; x++) {
            c[x] = (int)(a >> (19 - 5 * x)) & 31;
            c[x + 4] = (int)(b >> (19 - 5 * x)) & 31;
        }
        return (int)(a & 3);
    }
    uint32_t v[4];
    for (int q = 0; q < 4; q++) v[q] = cword(chrbase + chr * 32 + (uint32_t)(8 * q + fy), TC_F8);
    if (v[0] & TC_CLEAR) return TC_CLEAR;
    for (int q = 0; q < 4; q++) {
        c[2 * q] = (int)(v[q] >> 15) & 511;
        c[2 * q + 1] = (int)(v[q] >> 6) & 511;
    }
    return (int)(v[0] & 3);
}

/* Step 1: a BG layer's row into lk (keys, 0 transparent). */
static void layer_line(const uint32_t *slot, int mode, int bg, int row, const uint32_t *opt, uint32_t *lk)
{
    int bpp = mode_bpp[mode][bg], palbase = mode == 0 ? bg * 32 : 0;
    uint32_t sc = slot[DSP_SLOT_SC + bg];
    uint32_t nba = slot[DSP_SLOT_NBA + (bg >> 1)];
    uint32_t mapbase = (sc & 0xFC) << 8;
    int wide = sc & 1, tall = (sc >> 1) & 1;
    uint32_t chrbase = ((bg & 1) ? (nba >> 4) & 15 : nba & 15) << 12;
    int big = (slot[DSP_SLOT_BGMODE] >> (4 + bg)) & 1;
    int tile = big ? 16 : 8;
    int hofs = slot[DSP_SLOT_OFS + bg * 2] & 0x3FF, vofs = slot[DSP_SLOT_OFS + bg * 2 + 1] & 0x3FF;
    int mapw = (wide ? 64 : 32) * tile, maph = (tall ? 64 : 32) * tile;
    int y0 = (row + 1 + vofs) & (maph - 1);
    int px0 = hofs & (mapw - 1), fine = px0 & 7;
    uint32_t hi = (uint32_t)depth[mode][bg][1] << 16 | (uint32_t)bg << 8;
    uint32_t lo = (uint32_t)depth[mode][bg][0] << 16 | (uint32_t)bg << 8;
    if (mode == 1 && bg == 2 && (slot[DSP_SLOT_BGMODE] & 8)) hi = 17u << 16 | 2u << 8;
    for (int x = 0; x < PPU_W; x++) lk[x] = 0;
    for (int k = 0; k < 33; k++) {
        int hc = px0 & ~7, y = y0;
        if (opt && (opt[k] & (1u << 23))) hc = (int)(opt[k] & 0x3F8);
        if (opt && (opt[k] & (1u << 22))) y = (row + 1 + (int)((opt[k] >> 10) & 0x3FF)) & (maph - 1);
        int px = (hc + 8 * k) & (mapw - 1);
        int tx = px / tile, ty = y / tile;
        uint32_t scr = mapbase;
        if (tx >= 32) { scr += 0x400; tx -= 32; }
        if (ty >= 32) { scr += wide ? 0x800 : 0x400; ty -= 32; }
        uint32_t e = cword(scr + (uint32_t)(ty * 32 + tx), TC_RAW);
        int half = (px >> 3) & 1, fy = y & (tile - 1);
        if (e & 0x4000) half ^= 1;
        if (e & 0x8000) fy = tile - 1 - fy;
        uint32_t chr = e & 0x3FF;
        if (big) {
            if (half) chr += 1;
            if (fy >= 8) chr += 16;
        }
        int c[8];
        int fl = row_pixels(chrbase, chr & 0x3FF, bpp, fy & 7, c);
        if (st) {
            st->tilerows++;
            st->clear += fl == TC_CLEAR;
            st->opaque += fl == TC_SOLID;
        }
        if (fl == TC_CLEAR) continue;           /* the line holds 0 there already */
        uint32_t key = (e & 0x2000) ? hi : lo;
        int base = palbase + (bpp == 8 ? 0 : (int)((e >> 10) & 7) * (1 << bpp));
        for (int i = 0; i < 8; i++) {
            int x = 8 * k - fine + ((e & 0x4000) ? 7 - i : i);
            if (x < 0 || x >= PPU_W) continue;
            lk[x] = c[i] ? key | (uint32_t)((base + c[i]) & 0xFF) : 0;
        }
    }
}

/* Step 2: the sprites of the row's OAM segment into ok. */
static void obj_line(const uint32_t *slot, const uint32_t *spr, int nspr, int row, uint32_t *ok)
{
    for (int x = 0; x < PPU_W; x++) ok[x] = 0;
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
        uint32_t key = (uint32_t)(4 + 4 * ((attr >> 4) & 3)) << 16 | 4u << 8;
        int pal = 128 + (int)((attr >> 1) & 7) * 16;
        for (int col = 0; col < w / 8; col++) {
            int sx = x + col * 8;
            if (sx <= -8 || sx >= 256) continue;
            if (++slivers > 34) return;
            int cc = (attr & 0x40) ? (w / 8 - 1 - col) : col;
            uint32_t chr = (uint32_t)(((trow0 + (dy >> 3)) & 15) << 4 | ((tcol0 + cc) & 15));
            int c[8];
            int fl = row_pixels(base + ((attr & 1) ? gap : 0), chr, 4, dy & 7, c);
            if (st) {
                st->objslivers++;
                st->objclear += fl == TC_CLEAR;
                st->objopaque += fl == TC_SOLID;
            }
            if (fl == TC_CLEAR) continue;
            for (int i = 0; i < 8; i++) {
                int X = sx + ((attr & 0x40) ? 7 - i : i);
                if (X < 0 || X >= PPU_W || !c[i]) continue;
                ok[X] = key | (uint32_t)(pal + c[i]);
            }
        }
    }
}

/* A Mode 7 row from its entry (ppu_dsp.h, as dsp_model.c samples it) into
 * lk; returns the words the entry took. */
static int m7_line(const uint32_t *e, uint32_t *lk)
{
    int at = 0, x = 0, nseg = (int)e[at++];
    for (int k = 0; k < nseg; k++) {
        uint32_t kc = e[at++];
        int kind = (int)(kc >> 16), count = (int)(kc & 0xFFFF);
        if (kind == 3) {
            for (int i = 0; i < count && x < 256; i++) lk[x++] = 0;
            continue;
        }
        uint64_t a = ((uint64_t)(e[at] & 0xFFFFFF) << 24) | (e[at + 1] & 0xFFFFFF);
        uint64_t d = ((uint64_t)(e[at + 2] & 0xFFFFFF) << 24) | (e[at + 3] & 0xFFFFFF);
        at += 4;
        for (int i = 0; i < count && x < 256; i++) {
            uint32_t v = (uint32_t)(a >> 24) & 0xFFFFFF, u = ((uint32_t)a & 0xFFFFFF) - 0x800000;
            uint32_t tv = (v >> 8) & 1023, tu = (u >> 8) & 1023;
            uint32_t p = tc_m7_p(0);                 /* TILE0: character 0 */
            if (kind == 1) p = cword((tv >> 3) * 128 + (tu >> 3), TC_M7) >> 8;
            uint32_t w = p - tc_m7_p(0) + (tv & 7) * 8 + (tu & 7);   /* back to a plane word */
            if (p >= 0x5000) w -= 0x1000;
            uint8_t idx = (uint8_t)cword(w, TC_M7);
            lk[x++] = idx ? 6u << 16 | idx : 0;
            a = (a + d) & 0xFFFFFFFFFFFFULL;
        }
    }
    while (x < 256) lk[x++] = 0;
    return at;
}

/* Step 3: lk into line where the layer (0-4) is on and its window does not
 * hide it. */
static void merge(const PpuRegs *rr, int layer, uint8_t on, uint8_t mask, const uint32_t *lk, uint32_t *line)
{
    if (!(on & (1 << layer))) return;
    uint8_t hid[PPU_W];
    memset(hid, 0, sizeof hid);
    uint16_t sp[3][2];
    for (int k = 0, n = ppu_win_spans_on(rr, layer, mask, sp); k < n; k++)
        memset(hid + sp[k][0], 1, sp[k][1]);
    for (int x = 0; x < PPU_W; x++)
        if (!hid[x] && lk[x] > line[x]) line[x] = lk[x];
}

int tc_model_frame(const uint32_t *w, int n, int kernel, const uint8_t *vr, uint16_t *out, uint8_t *fmt,
                   TcStats *stats)
{
    vram = vr;
    fmtread = fmt;
    st = stats;
    memset(have, 0, sizeof have);
    int at = 0;
    if (n < 1 || w[at++] != DSP_CMD_FRAME) return -1;
    uint32_t slot[DSP_NSLOT_TC];
    for (int i = 0; i < DSP_NSLOT_TC; i++) slot[i] = w[at++];
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
    if (at >= n) return -1;
    int nopt = (int)w[at++];
    if ((nopt != 0 && nopt != 66) || at + nopt + 256 > n) return -1;
    const uint32_t *opt = nopt ? w + at : 0;
    at += nopt;
    uint16_t cg[256];
    for (int e = 0; e < 256; e++) cg[e] = (uint16_t)w[at++];
    const uint32_t *m7 = kernel == DSP_KERNEL_TC7 ? w + at : 0;
    if (!m7 && at != n) return -1;
    static uint32_t lk[5][PPU_W], mainl[PPU_W], subl[PPU_W];
    int ci = 0, si = -1;
    for (int row = 0; row < PPU_H; row++) {
        while (ci < nchange && (int)(chg[ci * 2] >> 8) <= row) {
            int k = (int)(chg[ci * 2] & 0xFF);
            uint32_t v = chg[ci * 2 + 1];
            if (k == DSP_SLOT_CG) cg[(v >> 16) & 0xFF] = (uint16_t)(v & 0x7FFF);
            else if (k < DSP_NSLOT_TC) slot[k] = v;
            else return -1;
            ci++;
        }
        while (si + 1 < nseg && segrow[si + 1] <= row) si++;
        uint16_t *o = out + row * PPU_W;
        int mode = slot[DSP_SLOT_BGMODE] & 7;
        uint8_t tm = slot[DSP_SLOT_TM] & 0x1F, ts = slot[DSP_SLOT_TS] & 0x1F, both = tm | ts;
        if (m7) {
            m7 += m7_line(m7, lk[0]);          /* one entry per row, whatever the row */
            if (m7 > w + n) return -1;
        }
        if (tc_model_band1 > tc_model_band0 && (row < tc_model_band0 || row >= tc_model_band1))
            continue;                           /* the BAND command: not drawn */
        if (slot[DSP_SLOT_INIDISP] & 0x80) {
            for (int x = 0; x < PPU_W; x++) o[x] = 0;
            continue;
        }
        if (mode == 7 && !m7) return -1;
        /* the registers the windows read */
        PpuRegs rr;
        memset(&rr, 0, sizeof rr);
        rr.r[0x23] = (uint8_t)slot[DSP_SLOT_WSEL];
        rr.r[0x24] = (uint8_t)(slot[DSP_SLOT_WSEL] >> 8);
        rr.r[0x25] = (uint8_t)(slot[DSP_SLOT_WSEL] >> 16);
        rr.r[0x26] = (uint8_t)slot[DSP_SLOT_WH01];
        rr.r[0x27] = (uint8_t)(slot[DSP_SLOT_WH01] >> 8);
        rr.r[0x28] = (uint8_t)slot[DSP_SLOT_WH23];
        rr.r[0x29] = (uint8_t)(slot[DSP_SLOT_WH23] >> 8);
        rr.r[0x2A] = (uint8_t)slot[DSP_SLOT_WLOG];
        rr.r[0x2B] = (uint8_t)(slot[DSP_SLOT_WLOG] >> 8);
        rr.r[0x2C] = tm;
        rr.r[0x2D] = ts;
        rr.r[0x2E] = (uint8_t)(slot[DSP_SLOT_WLOG] >> 16);
        rr.r[0x2F] = (uint8_t)slot[DSP_SLOT_TSW];
        /* steps 1-3 */
        for (int x = 0; x < PPU_W; x++) mainl[x] = subl[x] = 0;
        if (mode == 7) {
            if (both & 1) {
                merge(&rr, 0, tm, rr.r[0x2E], lk[0], mainl);
                merge(&rr, 0, ts, rr.r[0x2F], lk[0], subl);
            }
        } else {
            for (int i = 0; i < 4; i++) {
                if (!mode_bpp[mode][i] || !(both & (1 << i))) continue;
                layer_line(slot, mode, i, row, (mode == 2 || mode == 4 || mode == 6) && i < 2 && opt ?
                           opt + 33 * i : 0, lk[i]);
                if (st) st->layerpix[i]++;
                merge(&rr, i, tm, rr.r[0x2E], lk[i], mainl);
                merge(&rr, i, ts, rr.r[0x2F], lk[i], subl);
            }
        }
        if ((both & 0x10) && si >= 0) {
            obj_line(slot, seg[si], segn[si], row, lk[4]);
            if (st) st->layerpix[4]++;
            merge(&rr, 4, tm, rr.r[0x2E], lk[4], mainl);
            merge(&rr, 4, ts, rr.r[0x2F], lk[4], subl);
        }
        if (tc_dbg_keys)
            for (int x = 0; x < PPU_W; x++) {
                tc_dbg_keys[(row * PPU_W + x) * 2] = mainl[x];
                tc_dbg_keys[(row * PPU_W + x) * 2 + 1] = subl[x];
            }
        /* step 4 */
        uint8_t cw[PPU_W];
        memset(cw, 0, sizeof cw);
        uint16_t sp[3][2];
        for (int k = 0, nn = ppu_win_spans_on(&rr, 5, 0, sp); k < nn; k++) memset(cw + sp[k][0], 1, sp[k][1]);
        uint8_t cgwsel = (uint8_t)slot[DSP_SLOT_MATH], cgadsub = (uint8_t)(slot[DSP_SLOT_MATH] >> 8);
        int clipmode = cgwsel >> 6, mathmode = (cgwsel >> 4) & 3, bright = slot[DSP_SLOT_INIDISP] & 15;
        int fixed[3] = {(int)(slot[DSP_SLOT_FIXED] & 31), (int)((slot[DSP_SLOT_FIXED] >> 5) & 31),
                        (int)((slot[DSP_SLOT_FIXED] >> 10) & 31)};
        for (int x = 0; x < PPU_W; x++) {
            int mi = (int)(mainl[x] & 0xFF), ms = mainl[x] ? (int)((mainl[x] >> 8) & 7) : 5;
            int si2 = (int)(subl[x] & 0xFF), ss = subl[x] ? (int)((subl[x] >> 8) & 7) : 5;
            if (!mainl[x]) mi = 0;
            uint16_t mc = cg[mi];
            int v[3] = {mc & 31, (mc >> 5) & 31, (mc >> 10) & 31};
            int inw = cw[x];
            int clip = clipmode == 3 || (clipmode == 2 && inw) || (clipmode == 1 && !inw);
            int mathok = mathmode == 0 || (mathmode == 1 && inw) || (mathmode == 2 && !inw);
            if (clip) v[0] = v[1] = v[2] = 0;
            int en = ms == 5 ? (cgadsub >> 5) & 1 : ms == 4 ? ((cgadsub >> 4) & 1) && mi >= 192 : (cgadsub >> ms) & 1;
            if (mathok && en) {
                int ww[3], half = (cgadsub >> 6) & 1;
                if ((cgwsel & 2) && ss != 5) {
                    uint16_t sc = cg[si2];
                    ww[0] = sc & 31; ww[1] = (sc >> 5) & 31; ww[2] = (sc >> 10) & 31;
                } else {
                    for (int k = 0; k < 3; k++) ww[k] = fixed[k];
                    if (cgwsel & 2) half = 0;
                }
                if (clip) half = 0;
                for (int k = 0; k < 3; k++) {
                    int r = (cgadsub & 0x80) ? v[k] - ww[k] : v[k] + ww[k];
                    if (r < 0) r = 0;
                    if (half) r >>= 1;
                    if (r > 31) r = 31;
                    v[k] = r;
                }
                if (st) st->mathpix++;
            }
            if (st) st->pix++;
            int r = v[0] * (bright + 1) >> 4, g = v[1] * (bright + 1) >> 4, b = v[2] * (bright + 1) >> 4;
            o[x] = (uint16_t)(r << 11 | (g << 1 | g >> 4) << 5 | b);
        }
    }
    if (m7 && m7 != w + n) return -1;
    return 0;
}
