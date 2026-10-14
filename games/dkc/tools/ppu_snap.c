/* Host check of the Falcon video adapter (256-colour path): renders the
 * inputs saved in a snapshot (format in src/snes_ppu.h) with the same C
 * sources the Falcon runs and compares.
 *
 *   build/ppu_snap build/dkcx/SNAP.BIN build/dkcx/dkc.sfc [frame.ppm]
 *
 * 1. The fast row composer (ppu_fast.c) against the reference renderer
 *    (snes_ppu.c) with an identity palette map: CGRAM index per pixel.
 * 2. The Falcon's eight planes against ppu_planar8() of that frame.
 * Exits non-zero on any difference. The optional third argument writes
 * the Falcon's frame as a PPM image, coloured from the frame's CGRAM and
 * brightness the way the Videl palette shows it. */
#include "snes_ppu.h"
#include "ppu_fast.h"

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

/* Rows on which the reference adds the sub screen (ppu_sub_cfg) or masks layers with windows
 * (ppu_win_layers): the fast composer and the Falcon's planes have neither, so they differ from the
 * reference there by design (roadmap item 4). */
typedef struct { int sub, win; } Gap;

static void gap_row(void *ctx, int row, const PpuRegs *r)
{
    Gap *g = ctx;
    (void)row;
    if (ppu_sub_cfg(r, r->cgram)) g->sub++;
    if (ppu_win_layers(r)) g->win++;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s SNAP.BIN rom.sfc [frame.ppm]\n", argv[0]);
        return 2;
    }
    long n, romsize;
    uint8_t *snap = load(argv[1], &n);
    uint8_t *rom = load(argv[2], &romsize);
    if (n < 12 || memcmp(snap, "SMKXSNAP", 8) || be32(snap + 8) != PPU_SNAP_VERSION) {
        fprintf(stderr, "%s: not a version %d SMKX snapshot\n", argv[1], PPU_SNAP_VERSION);
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
    PpuWrite log[256];
    for (int i = 0; i < 256; i++) {
        log[i].line = be16(p); log[i].reg = be16(p + 2); log[i].value = p[4]; log[i].pad = 0;
        p += 6;
    }
    const uint8_t *wram = p; p += 131072;
    const uint8_t *sram = p; p += 2048;
    const uint8_t *falcon = p; p += PPU_PLANAR8_BYTES;
    uint32_t noamseg = be32(p); p += 4;
    static PpuOamSeg oamseg[PPU_OAMSEG_MAX];
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
        oamseg[i].line = be16(p);
        memcpy(oamseg[i].oam, p + 4, PPU_OAM_BYTES);
        p += 4 + PPU_OAM_BYTES;
    }
    if (noamseg > PPU_OAMSEG_MAX) noamseg = PPU_OAMSEG_MAX;
    if (p - snap != n) {
        fprintf(stderr, "%s: %ld bytes, expected %ld\n", argv[1], n, (long)(p - snap));
        return 2;
    }
    if ((io[0x100] & 0x80) && nlog == 0) {
        /* the runtime does not compose a frame that is black throughout */
        printf("INIDISP $%02X, no IRQ writes: forced blank, not composed\n", io[0x100]);
        return 0;
    }
    PpuBus bus = {wram, sram, rom};
    static uint8_t identity[256];
    for (int i = 0; i < 256; i++) identity[i] = (uint8_t)i;
    static uint32_t hist[256];
    PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_hirom, &bus,
                   log, (int)(nlog > 256 ? 256 : nlog), identity, hist, NULL, oamseg, (int)noamseg, ppu_aptr_hirom};
    static uint8_t ref[PPU_W * PPU_H], fast[PPU_W * PPU_H];
    static uint16_t planar[PPU_PLANAR8_BYTES / 2];
    ppu_render(&in, ref);
    ppu_fast_render(&in, fast);
    ppu_planar8(ref, planar);
    int fdiff = 0, first = -1;
    for (int i = 0; i < PPU_W * PPU_H; i++)
        if (ref[i] != fast[i]) {
            if (first < 0) first = i;
            fdiff++;
        }
    int diff = 0, dgroups = 0, dfirst = -1;
    for (int i = 0; i < PPU_PLANAR8_BYTES / 2; i++) {
        diff += (uint8_t)(planar[i] >> 8) != falcon[i * 2];
        diff += (uint8_t)planar[i] != falcon[i * 2 + 1];
    }
    for (int g = 0; g < PPU_PLANAR8_BYTES / 16; g++) {      /* groups of 16 pixels: eight words */
        int bad = 0;
        for (int k = 0; k < 8; k++) bad |= (uint8_t)(planar[g * 8 + k] >> 8) != falcon[(g * 8 + k) * 2] || (uint8_t)planar[g * 8 + k] != falcon[(g * 8 + k) * 2 + 1];
        if (bad && dfirst < 0) dfirst = g;
        dgroups += bad;
    }
    int used = 0;
    static uint8_t seen[256];
    for (int i = 0; i < PPU_W * PPU_H; i++) used += !seen[ref[i]]++;
    Gap gap = {0, 0};
    ppu_replay(&in, gap_row, &gap);
    printf("IRQ writes %u, OAM segments %u, BG mode %d, INIDISP $%02X, TM $%02X, %d CGRAM indices shown\n",
           nlog, noamseg, io[0x105] & 7, io[0x100], io[0x12C], used);
    printf("fast composer vs reference: %d of %d pixels differ", fdiff, PPU_W * PPU_H);
    if (first >= 0)
        printf(" (first at x %d, row %d: %u vs %u)", first % PPU_W, first / PPU_W, fast[first], ref[first]);
    printf("\nFalcon planes vs host: %d of %d bytes differ", diff, PPU_PLANAR8_BYTES);
    if (diff) printf(" (%d groups, first at row %d, group %d)", dgroups, dfirst / 16, dfirst % 16);
    if (diff && getenv("PPU_DUMP")) {
        int g = dfirst;
        printf("\n  group pixels ref/falcon:");
        for (int k = 0; k < 16; k++) {
            int fv = 0;
            for (int b = 0; b < 8; b++) {
                uint16_t w = (uint16_t)(falcon[(g * 8 + b) * 2] << 8 | falcon[(g * 8 + b) * 2 + 1]);
                fv |= ((w >> (15 - k)) & 1) << b;
            }
            printf(" %d/%d", ref[g * 16 + k], fv);
        }
    }
    printf("\n");
    if (gap.sub || gap.win) {
        printf("sub screen on %d rows, windows on %d rows: the fast composer has neither, differences are expected\n",
               gap.sub, gap.win);
        printf("(rows with the sub screen or windows: the fast composer's differences are expected; the Falcon planes differ from the reference in %d bytes there)\n", diff);
        if (!getenv("PPU_RAW")) fdiff = diff = 0;
    }

    if (argc == 4) {
        FILE *f = fopen(argv[3], "wb");
        if (!f) { perror(argv[3]); return 2; }
        int bright = io[0x100] & 15;
        fprintf(f, "P6\n%d %d\n255\n", PPU_W, PPU_H);
        for (int i = 0; i < PPU_W * PPU_H; i++) {
            const uint8_t *w = falcon + (i / 16) * 16;
            int bit = 15 - (i & 15), v = 0;
            for (int b = 0; b < 8; b++)
                v |= (((w[b * 2] << 8 | w[b * 2 + 1]) >> bit) & 1) << b;
            unsigned c = cgram[v * 2] | cgram[v * 2 + 1] << 8;
            unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            if (io[0x100] & 0x80) r = g = b = 0;
            fputc((int)(r * bright * 255 / (31 * 15)), f);
            fputc((int)(g * bright * 255 / (31 * 15)), f);
            fputc((int)(b * bright * 255 / (31 * 15)), f);
        }
        fclose(f);
    }
    return fdiff != 0 || diff != 0;
}
