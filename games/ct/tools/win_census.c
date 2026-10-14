/* Which of CT's screens use windows and colour math: replays each
 * snapshot's frame (HDMA, IRQ writes) with every register walked and
 * counts the visible rows where
 *   mask   windows hide a main-screen layer (TMW, TM and a window select),
 *   clip   colour window clips the main screen to black (CGWSEL bits 7-6),
 *   math   colour math is on (CGADSUB layers, CGWSEL bits 5-4 not "never"),
 *   sub    the sub screen has layers (TS),
 *   cwin   a colour window is enabled (WOBJSEL bits 5/7),
 * and how many different window edge sets (WH0-WH3) the rows show.
 *
 *   build/win_census [-v] build/ctx/ct.sfc build/tl/F*.BIN
 *
 * -v also prints each snapshot's rows whenever the window state changes. */
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
    int vis, mask, clip, math, sub, cwin;
    uint8_t masklayers, tmw, w12, w34, wobj, cgwsel, cgadsub, ts, tsw;
    uint32_t edges[PPU_H];
    int nedges;
    uint8_t last[16];
    int verbose;
} Count;

static int win_enabled(const PpuRegs *s, int layer)
{
    uint8_t sel = layer == 4 ? s->r[0x25] : (uint8_t)(s->r[0x23 + (layer >> 1)] >> ((layer & 1) * 4));
    return (sel & 0x0A) != 0;
}

static void row_fn(void *ctx, int row, const PpuRegs *s)
{
    Count *c = ctx;
    const uint8_t *r = s->r;
    if (c->verbose) {
        uint8_t now[16] = {r[0x00], r[0x05], r[0x2C], r[0x2D], r[0x2E], r[0x2F], r[0x23], r[0x24],
                           r[0x25], r[0x26], r[0x27], r[0x28], r[0x29], r[0x2A], r[0x30], r[0x31]};
        if (row == 0 || memcmp(now, c->last, 16)) {
            printf("  row %3d INIDISP %02X mode %d TM %02X TS %02X TMW %02X TSW %02X sel %02X %02X %02X"
                   " WH %3d-%3d %3d-%3d log %02X CGWSEL %02X CGADSUB %02X\n",
                   row, now[0], now[1] & 7, now[2], now[3], now[4], now[5], now[6], now[7], now[8],
                   now[9], now[10], now[11], now[12], now[13], now[14], now[15]);
            memcpy(c->last, now, 16);
        }
    }
    if (r[0x00] & 0x80) return;                 /* forced blank */
    c->vis++;
    uint8_t tm = r[0x2C], tmw = r[0x2E];
    uint8_t m = 0;
    for (int i = 0; i < 5; i++)
        if ((tm & tmw & (1 << i)) && win_enabled(s, i)) m |= (uint8_t)(1 << i);
    if (m) {
        c->mask++;
        c->masklayers |= m;
        uint32_t e = (uint32_t)r[0x26] << 24 | (uint32_t)r[0x27] << 16 | (uint32_t)r[0x28] << 8 | r[0x29];
        int k = 0;
        while (k < c->nedges && c->edges[k] != e) k++;
        if (k == c->nedges) c->edges[c->nedges++] = e;
    }
    if (r[0x30] >> 6) c->clip++;
    if (((r[0x30] >> 4) & 3) != 3 && (r[0x31] & 0x3F)) c->math++;
    if (r[0x2D] & 0x1F) c->sub++;
    if (r[0x25] & 0xA0) c->cwin++;
    c->tmw |= tmw; c->w12 |= r[0x23]; c->w34 |= r[0x24]; c->wobj |= r[0x25];
    c->cgwsel |= r[0x30]; c->cgadsub |= r[0x31]; c->ts |= r[0x2D]; c->tsw |= r[0x2F];
}

int main(int argc, char **argv)
{
    int verbose = 0, a = 1;
    if (a < argc && !strcmp(argv[a], "-v")) { verbose = 1; a++; }
    if (a + 1 >= argc) {
        fprintf(stderr, "usage: %s [-v] rom.sfc SNAP.BIN...\n", argv[0]);
        return 2;
    }
    long romsize;
    uint8_t *rom = load(argv[a++], &romsize);
    ppu_walk_all = 1;
    int tot = 0, nmask = 0, nclip = 0, nmath = 0, nsub = 0, ncwin = 0;
    printf("%-14s %4s %4s %4s %4s %4s %4s %4s  layers edges  TMW  W12  W34 WOBJ CGWS CGAD   TS  TSW\n",
           "snapshot", "mode", "vis", "mask", "clip", "math", "sub", "cwin");
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
        c.verbose = verbose;
        const char *name = strrchr(argv[a], '/');
        name = name ? name + 1 : argv[a];
        if (verbose) printf("%s\n", name);
        ppu_replay(&in, row_fn, &c);
        printf("%-14s %4d %4d %4d %4d %4d %4d %4d  %02X     %5d   %02X   %02X   %02X   %02X   %02X   %02X   %02X   %02X\n",
               name, io[0x105] & 7, c.vis, c.mask, c.clip, c.math, c.sub, c.cwin, c.masklayers, c.nedges,
               c.tmw, c.w12, c.w34, c.wobj, c.cgwsel, c.cgadsub, c.ts, c.tsw);
        tot++;
        nmask += c.mask > 0; nclip += c.clip > 0; nmath += c.math > 0; nsub += c.sub > 0; ncwin += c.cwin > 0;
        free(snap);
    }
    printf("%d snapshots: mask %d, clip %d, math %d, sub %d, colour window %d\n",
           tot, nmask, nclip, nmath, nsub, ncwin);
    return 0;
}
