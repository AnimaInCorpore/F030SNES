/* Host check of the Falcon video adapter: renders the inputs saved in
 * SNAP.BIN (format in src/snes_ppu.h) with the same C source the Falcon
 * runs, and compares the resulting planes with the planes the Falcon wrote.
 *
 *   build/ppu_snap build/smkx/SNAP.BIN build/smkx/smk.sfc build/smkx/oklab555.bin
 *
 * Renders with the palette the Falcon used, compares the planes, then runs
 * palette_update() on the host's own pixel counts and compares the result
 * with the palette the Falcon produced. Prints slot use; exits non-zero on
 * any difference.
 *
 * It also renders the frame a second time through a PpuM7Sink, filling the
 * stage's pixels from ppu_m7_pixels(), and checks that the split path (the
 * one the Falcon takes with the DSP) gives the same pixels and counts.
 * An optional fourth argument writes the Falcon's frame as a PPM image. */
#include "ppu_dsp.h"
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

/* Host stand-in for the DSP: keeps each row's line. */
static PpuM7Line rowline[PPU_H];
static uint8_t rowext[PPU_H];
static void host_m7_row(void *ctx, int row, const PpuM7Line *l) { (void)ctx; rowline[row] = *l; rowext[row] = 1; }
static void host_cpu_row(void *ctx, int row) { (void)ctx; rowext[row] = 0; }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s SNAP.BIN rom.sfc oklab555.bin [frame.ppm]\n", argv[0]);
        return 2;
    }
    long n, romsize, oksize;
    uint8_t *snap = load(argv[1], &n);
    uint8_t *rom = load(argv[2], &romsize);
    uint8_t *okraw = load(argv[3], &oksize);
    static int16_t oklab[32768 * 3];
    for (int i = 0; i < 32768 * 3; i++) oklab[i] = (int16_t)(okraw[i * 2] << 8 | okraw[i * 2 + 1]);
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
    const uint8_t *sram = p; p += 8192;
    uint32_t planes = be32(p); p += 4;
    const uint8_t *falcon = p; p += ppu_snap_frame(planes);
    const uint8_t *pal_used = p; p += PAL_SNAP_BYTES;
    const uint8_t *pal_next = p; p += PAL_SNAP_BYTES;
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
    PpuBus bus = {wram, sram, rom};
    if (planes == 8) {
        /* The DSP renderer's frame (or the CPU's, eight planes): CGRAM
         * indices, against the reference with the identity map. */
        static uint8_t ident[256];
        static uint32_t h8[256];
        for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
        PpuInput in8 = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_lorom, &bus,
                        log, (int)(nlog > 256 ? 256 : nlog), ident, h8, NULL, oamseg, (int)noamseg, ppu_aptr_lorom, NULL};
        static uint8_t c8[PPU_W * PPU_H];
        static uint16_t p8[PPU_W * PPU_H / 2];
        static DspPlan plan;                /* the sub screen's copies, as the Falcon shows them */
        plan.prefer = DSP_KERNEL_TILE;
        if (!dsp_plan(&in8, &plan)) in8.subreloc = plan.subreloc;
        ppu_render(&in8, c8);
        ppu_planar8(c8, p8);
        int d8 = 0, rows = 0;
        for (int row = 0; row < PPU_H; row++) {
            int d = 0;
            for (int i = row * 128; i < row * 128 + 128; i++)
                d += (uint16_t)(falcon[i * 2] << 8 | falcon[i * 2 + 1]) != p8[i];
            d8 += d;
            rows += d != 0;
        }
        printf("eight planes, BG mode %d, INIDISP $%02X, TM $%02X\n", io[0x105] & 7, io[0x100], io[0x12C]);
        if (argc == 5) {
            FILE *f = fopen(argv[4], "wb");
            if (!f) { perror(argv[4]); return 2; }
            fprintf(f, "P6\n%d %d\n255\n", PPU_W, PPU_H);
            int bright = io[0x100] & 15;
            for (int i = 0; i < PPU_W * PPU_H; i++) {
                const uint8_t *w = falcon + (i / 16) * 16;
                int bit = 15 - (i & 15), v = 0;
                for (int b = 0; b < 8; b++) v |= (((w[b * 2] << 8 | w[b * 2 + 1]) >> bit) & 1) << b;
                uint16_t c = (uint16_t)(cgram[v * 2] | cgram[v * 2 + 1] << 8);
                int rgb[3] = {c & 31, (c >> 5) & 31, (c >> 10) & 31};
                for (int k = 0; k < 3; k++)
                    fputc((io[0x100] & 0x80) ? 0 : rgb[k] * 255 / 31 * (bright + 1) / 16, f);
            }
            fclose(f);
        }
        printf("Falcon planes vs host render: %d of 28672 words differ in %d rows\n", d8, rows);
        return d8 != 0;
    }
    Palette used, next;
    palette_load(&used, pal_used);
    static uint32_t hist[256];
    PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_lorom, &bus,
                   log, (int)(nlog > 256 ? 256 : nlog), used.map, hist, NULL, oamseg, (int)noamseg, ppu_aptr_lorom, NULL};
    static uint8_t chunky[PPU_W * PPU_H];
    static uint16_t planar[PPU_W * PPU_H / 4];
    ppu_init();
    ppu_render(&in, chunky);
    ppu_planar(chunky, planar);
    int diff = 0;
    for (int i = 0; i < PPU_W * PPU_H / 4; i++) {
        uint8_t hi = (uint8_t)(planar[i] >> 8), lo = (uint8_t)planar[i];
        diff += hi != falcon[i * 2];
        diff += lo != falcon[i * 2 + 1];
    }
    long slots[16] = {0};
    for (int i = 0; i < PPU_W * PPU_H; i++) slots[chunky[i]]++;
    printf("IRQ writes %u, OAM segments %u, BG mode %d, INIDISP $%02X, TM $%02X\n", nlog,
           noamseg, io[0x105] & 7, io[0x100], io[0x12C]);
    printf("slot use:");
    for (int i = 0; i < 16; i++) printf(" %ld", slots[i]);

    /* the split path */
    static uint32_t hist2[256];
    static uint8_t chunky2[PPU_W * PPU_H];
    PpuM7Sink sink = {host_m7_row, host_cpu_row, 0};
    in.hist = hist2;
    in.m7sink = &sink;
    ppu_render(&in, chunky2);
    int m7rows = 0, sdiff = 0, hdiff = 0;
    for (int row = 0; row < PPU_H; row++) {
        uint8_t *c = chunky2 + row * PPU_W;
        if (rowext[row]) {
            uint8_t idx[PPU_W];
            m7rows++;
            ppu_m7_pixels(vram, &rowline[row], idx);
            for (int x = 0; x < PPU_W; x++)
                if (c[x] == PPU_EXT) c[x] = used.map[idx[x]];
        }
        for (int x = 0; x < PPU_W; x++) sdiff += c[x] != chunky[row * PPU_W + x];
    }
    for (int i = 0; i < 256; i++) hdiff += hist[i] != hist2[i];
    printf("\nMode 7 rows %d; split path vs whole: %d pixels, %d counts differ", m7rows, sdiff, hdiff);

    if (argc == 5) {
        FILE *f = fopen(argv[4], "wb");
        if (!f) { perror(argv[4]); return 2; }
        uint32_t rgb[16];
        palette_scaled(&used, io[0x100] & 15, rgb);
        fprintf(f, "P6\n%d %d\n255\n", PPU_W, PPU_H);
        for (int i = 0; i < PPU_W * PPU_H; i++) {
            const uint8_t *w = falcon + (i / 16) * 8;
            int bit = 15 - (i & 15), v = 0;
            for (int b = 0; b < 4; b++)
                v |= (((w[b * 2] << 8 | w[b * 2 + 1]) >> bit) & 1) << b;
            uint32_t c = (io[0x100] & 0x80) ? 0 : rgb[v];
            fputc((int)(c >> 16) & 255, f);
            fputc((int)(c >> 8) & 255, f);
            fputc((int)c & 255, f);
        }
        fclose(f);
    }

    next = used;
    palette_update(&next, hist, cgram, oklab);
    uint8_t mine[PAL_SNAP_BYTES];
    palette_save(&next, mine);
    int pdiff = 0;
    for (int i = 0; i < PAL_SNAP_BYTES; i++) pdiff += mine[i] != pal_next[i];
    printf("\nFalcon planes vs host render: %d of 28672 bytes differ\n", diff);
    printf("next palette vs host palette_update: %d of %d bytes differ\n", pdiff, PAL_SNAP_BYTES);
    return diff != 0 || pdiff != 0 || sdiff != 0 || hdiff != 0;
}
