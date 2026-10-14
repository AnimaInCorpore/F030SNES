/* Host check of the list compositor's eight-plane pictures (F030SMK): renders the inputs saved in a snapshot
 * with the reference renderer of src/tile (ppu_render with the identity colour map, packed by ppu_planar8)
 * and compares the planes with the frame the Falcon wrote.
 *
 *   build/tile/ppu_snap SNAP.BIN rom.sfc [frame.ppm]
 *
 * SNAP.BIN is the shared version 4 layout (src/tile/snes_ppu.h): what the runtime writes when the list
 * compositor's ring window is the picture on the Videl (falcon/smkx_rt.s, snap_blocks8). Snapshots of SMK's own
 * four-plane renderer (version 3) are not eight-plane frames: they are reported and skipped (build/ppu_snap
 * checks those). Exits non-zero on any differing byte. */
#include "snes_ppu.h"

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

#define PAL_SNAP_BYTES_V3 406   /* src/palette.h: PAL_SNAP_BYTES */

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s SNAP.BIN rom.sfc [frame.ppm]\n", argv[0]);
        return 2;
    }
    long n, romsize;
    uint8_t *snap = load(argv[1], &n);
    uint8_t *rom = load(argv[2], &romsize);
    if (n < 12 || memcmp(snap, "SMKXSNAP", 8)) {
        fprintf(stderr, "%s: not an SMKX snapshot\n", argv[1]);
        return 2;
    }
    if (be32(snap + 8) == 3) {
        printf("%s: version 3 (SMK's four-plane renderer): not an eight-plane frame, skipped\n", argv[1]);
        return 0;
    }
    if (be32(snap + 8) != PPU_SNAP_VERSION) {
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
    const uint8_t *sram = p; p += 8192;
    uint32_t planes = be32(p); p += 4;
    const uint8_t *falcon = p; p += ppu_snap_frame(planes) + 2 * PAL_SNAP_BYTES_V3;
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
    if (planes != 8) {
        printf("%s: planes %u, not an eight-plane frame, skipped\n", argv[1], planes);
        return 0;
    }
    PpuBus bus = {wram, sram, rom};
    static uint8_t identity[256];
    for (int i = 0; i < 256; i++) identity[i] = (uint8_t)i;
    static uint32_t hist[256];
    PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_hirom, &bus,
                   log, (int)(nlog > 256 ? 256 : nlog), identity, hist, NULL, oamseg, (int)noamseg, ppu_aptr_hirom, NULL};
    static uint8_t ref[PPU_W * PPU_H];
    static uint16_t planar[PPU_SNAP_PLANAR / 2];
    ppu_render(&in, ref);
    ppu_planar8(ref, planar);
    int diff = 0, dgroups = 0, dfirst = -1;
    for (int i = 0; i < PPU_SNAP_PLANAR / 2; i++) {
        diff += (uint8_t)(planar[i] >> 8) != falcon[i * 2];
        diff += (uint8_t)planar[i] != falcon[i * 2 + 1];
    }
    for (int g = 0; g < PPU_SNAP_PLANAR / 16; g++) {      /* groups of 16 pixels: eight words */
        int bad = 0;
        for (int k = 0; k < 8; k++)
            bad |= (uint8_t)(planar[g * 8 + k] >> 8) != falcon[(g * 8 + k) * 2] ||
                   (uint8_t)planar[g * 8 + k] != falcon[(g * 8 + k) * 2 + 1];
        if (bad && dfirst < 0) dfirst = g;
        dgroups += bad;
    }
    int used = 0;
    static uint8_t seen[256];
    for (int i = 0; i < PPU_W * PPU_H; i++) used += !seen[ref[i]]++;
    printf("IRQ writes %u, OAM segments %u, BG mode %d, INIDISP $%02X, TM $%02X, %d CGRAM indices shown\n",
           nlog, noamseg, io[0x105] & 7, io[0x100], io[0x12C], used);
    printf("Falcon planes vs host: %d of %d bytes differ", diff, PPU_SNAP_PLANAR);
    if (diff) printf(" (%d groups, first at row %d, group %d)", dgroups, dfirst / 16, dfirst % 16);
    printf("\n");

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
    return diff != 0;
}
