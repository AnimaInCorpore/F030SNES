/* What of the PPU CT's screens use beyond what the renderers draw: replays
 * each snapshot's frame (HDMA, IRQ writes) with every register walked and
 * counts per frame the visible rows with
 *   m2-6   BG mode 2-6 (drawn as mode 1)
 *   mathF  colour math with the fixed colour (CGWSEL bit 1 clear)
 *   mathS  colour math with the sub screen (bit 1 set, TS has layers)
 *   half / sub   CGADSUB bits 6 / 7 on those rows
 *   mos    mosaic on a shown layer ($2106)
 *   cg     CGRAM changed since the frame started (HDMA, IRQ writes)
 *   bri    brightness other than row 0's
 *   ini    SETINI ($2133) nonzero
 *   dc     direct colour (CGWSEL bit 0) in mode 3, 4 or 7
 * and the distinct COLDATA writes seen (a gradient when many).
 *
 *   build/ppu_census rom.sfc SNAP.BIN... */
#include "snes_ppu.h"
#include "palette.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

typedef struct {
    const PpuInput *in;
    int vis, m26, mathF, mathS, half, sub, mos, cg, bri, ini, dc;
    int bri0;
    uint8_t modes, cgadsub, ts, coldata[256], ncol;
} Count;

static void row_fn(void *ctx, int row, const PpuRegs *s)
{
    Count *c = ctx;
    const uint8_t *r = s->r;
    if (row == 0) c->bri0 = r[0x00] & 15;
    if (r[0x00] & 0x80) return;
    c->vis++;
    int mode = r[0x05] & 7;
    c->modes |= (uint8_t)(1 << mode);
    if (mode >= 2 && mode <= 6) c->m26++;
    uint8_t tm = r[0x2C] & 0x1F, ts = r[0x2D] & 0x1F;
    int gate = (r[0x30] >> 4) & 3;             /* 3: colour math never */
    if (gate != 3 && (r[0x31] & 0x3F)) {
        if (r[0x30] & 2) { if (ts) c->mathS++; else c->mathF++; }
        else c->mathF++;
        if (r[0x31] & 0x40) c->half++;
        if (r[0x31] & 0x80) c->sub++;
        c->cgadsub |= r[0x31];
        c->ts |= ts;
    }
    if ((r[0x06] & 0xF0) && (r[0x06] & 0x0F & tm)) c->mos++;
    if (memcmp(s->cgram, c->in->cgram, 512)) c->cg++;
    if ((r[0x00] & 15) != c->bri0) c->bri++;
    if (r[0x33]) c->ini++;
    if ((r[0x30] & 1) && (mode == 3 || mode == 4 || mode == 7)) c->dc++;
    uint8_t v = r[0x32];
    int k = 0;
    while (k < c->ncol && c->coldata[k] != v) k++;
    if (k == c->ncol && c->ncol < 255) c->coldata[c->ncol++] = v;
}

int main(int argc, char **argv)
{
    int a = 1;
    if (a + 1 >= argc) {
        fprintf(stderr, "usage: %s rom.sfc SNAP.BIN...\n", argv[0]);
        return 2;
    }
    long romsize;
    uint8_t *rom = load(argv[a++], &romsize);
    ppu_walk_all = 1;
    int tot = 0, t26 = 0, tF = 0, tS = 0, tmos = 0, tcg = 0, tbri = 0, tini = 0, tdc = 0;
    printf("%-14s %5s %4s %4s %5s %5s %4s %4s %4s %4s %4s %4s %4s  CGAD  TS  col\n",
           "snapshot", "modes", "vis", "m2-6", "mathF", "mathS", "half", "sub", "mos", "cg", "bri", "ini", "dc");
    for (; a < argc; a++) {
        long n;
        uint8_t *snap = load(argv[a], &n);
        if (n < 12 || memcmp(snap, "SMKXSNAP", 8) || be32(snap + 8) != PPU_SNAP_VERSION) {
            fprintf(stderr, "%s: not a version %d SMKX snapshot\n", argv[a], PPU_SNAP_VERSION);
            return 2;
        }
        const uint8_t *p = snap + 12;
        const uint8_t *vram = p; p += 65536;
        const uint8_t *cgram = p; p += 512;
        const uint8_t *oam = p; p += 544;
        const uint8_t *io = p; p += 0x6000;
        uint16_t bgofs[8], m7[6], m7ofs[2];
        for (int i = 0; i < 8; i++) { bgofs[i] = be16(p); p += 2; }
        for (int i = 0; i < 6; i++) { m7[i] = be16(p); p += 2; }
        for (int i = 0; i < 2; i++) { m7ofs[i] = be16(p); p += 2; }
        uint32_t nlog = be32(p); p += 4;
        static PpuWrite log[256];
        for (int i = 0; i < 256; i++) {
            log[i].line = be16(p); log[i].reg = be16(p + 2); log[i].value = p[4]; log[i].pad = 0;
            p += 6;
        }
        const uint8_t *wram = p; p += 131072;
        const uint8_t *sram = p; p += 8192;
        p += 4 + ppu_snap_frame(be32(p)) + 2 * PAL_SNAP_BYTES;
        uint32_t noamseg = be32(p); p += 4;
        static PpuOamSeg oamseg[PPU_OAMSEG_MAX];
        for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
            oamseg[i].line = be16(p);
            memcpy(oamseg[i].oam, p + 4, PPU_OAM_BYTES);
            p += 4 + PPU_OAM_BYTES;
        }
        if (noamseg > PPU_OAMSEG_MAX) noamseg = PPU_OAMSEG_MAX;
        PpuBus bus = {wram, sram, rom};
        static uint8_t ident[256];
        static uint32_t hist[256];
        PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_hirom, &bus,
                       log, (int)(nlog > 256 ? 256 : nlog), ident, hist, NULL, oamseg, (int)noamseg,
                       ppu_aptr_hirom, NULL};
        static Count c;
        memset(&c, 0, sizeof c);
        c.in = &in;
        const char *name = strrchr(argv[a], '/');
        name = name ? name + 1 : argv[a];
        ppu_replay(&in, row_fn, &c);
        printf("%-14s %5X %4d %4d %5d %5d %4d %4d %4d %4d %4d %4d %4d  %02X   %02X  %d\n",
               name, c.modes, c.vis, c.m26, c.mathF, c.mathS, c.half, c.sub, c.mos, c.cg, c.bri, c.ini, c.dc,
               c.cgadsub, c.ts, c.ncol);
        tot++;
        t26 += c.m26 > 0; tF += c.mathF > 0; tS += c.mathS > 0; tmos += c.mos > 0; tcg += c.cg > 0;
        tbri += c.bri > 0; tini += c.ini > 0; tdc += c.dc > 0;
        free(snap);
    }
    printf("%d snapshots: modes 2-6 %d, colour math fixed %d / sub screen %d, mosaic %d, CGRAM mid-frame %d,"
           " brightness mid-frame %d, SETINI %d, direct colour %d\n", tot, t26, tF, tS, tmos, tcg, tbri, tini, tdc);
    return 0;
}
