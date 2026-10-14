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


typedef struct { const PpuInput *in; int vis; int nx[4], ny[4]; uint16_t x0[4], y0[4]; int seen[4]; uint8_t tm; int first; uint16_t xs[4][224], ys[4][224]; int row; } C;
static void row_fn(void *ctx, int row, const PpuRegs *s)
{
    C *c = ctx;
    if (s->r[0x00] & 0x80) return;
    c->vis++; c->tm = s->r[0x2C];
    for (int l = 0; l < 4; l++) {
        uint16_t x = s->bgofs[l*2], y = s->bgofs[l*2+1];
        c->xs[l][row] = x; c->ys[l][row] = y;
        if (!c->seen[l]) { c->seen[l] = 1; c->x0[l] = x; c->y0[l] = y; }
        if (x != c->x0[l]) c->nx[l]++;
        if (y != c->y0[l]) c->ny[l]++;
    }
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
        for (int q = 0; q < 256; q++) ident[q] = (uint8_t)q;
        static C c;
        memset(&c, 0, sizeof c);
        c.in = &in;
        const char *name = strrchr(argv[a], '/');
        name = name ? name + 1 : argv[a];

        ppu_replay(&in, row_fn, &c);
        static uint8_t full[PPU_H*PPU_W], lay[PPU_H*PPU_W];
        static uint8_t io2[0x6000];
        ppu_render(&in, full);
        memcpy(io2, io, 0x6000);
        PpuInput in2 = in; in2.io = io2;
        int cnt[5] = {0}, same[5] = {0};
        uint8_t tmv = io[0x12C];
        int ntm = 0;
        for (int l = 0; l < 5; l++) {
            io2[0x12C] = io2[0x12D] = (uint8_t)(1 << l);
            ppu_render(&in2, lay);
            for (int k = 0; k < PPU_H*PPU_W; k++) {
                if (lay[k] != lay[0] || 1) {}
            }
            /* opaque = differs from the all-layers-off render */
            static uint8_t off[PPU_H*PPU_W];
            io2[0x12C] = io2[0x12D] = 0; ppu_render(&in2, off); io2[0x12C] = io2[0x12D] = (uint8_t)(1 << l); ppu_render(&in2, lay);
            for (int k = 0; k < PPU_H*PPU_W; k++) if (lay[k] != off[k]) { cnt[l]++; if (full[k] == lay[k]) same[l]++; }
        }
        (void)tmv; (void)ntm;
        {
            static uint8_t b1[PPU_H*PPU_W];
            io2[0x12C] = io2[0x12D] = 1; ppu_render(&in2, b1);
            char o[256]; snprintf(o, sizeof o, "dump/%s.raw", name);
            FILE *f = fopen(o, "wb");
            uint16_t sc[2] = {c.xs[0][0], c.ys[0][0]};
            fwrite(sc, 2, 2, f); fwrite(full, 1, PPU_H*PPU_W, f); fwrite(b1, 1, PPU_H*PPU_W, f);
            fclose(f);
        }
        free(snap);
    }
    return 0;
}
