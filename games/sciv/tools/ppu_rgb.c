/* A snapshot as pictures: the Falcon's frame (eight planes: CGRAM indices
 * with the frame-start CGRAM, the planner's colour math palette when it
 * has one (dsp_math_palette), brightness) beside the frame as the SNES
 * shows it (ppu_render_rgb: sub screen, colour math, windows).
 *
 *   build/ppu_rgb rom.sfc SNAP.BIN out.ppm
 *
 * out.ppm is 512 x 224: the Falcon left, the SNES right. Prints how many
 * pixels differ in colour. With PPU_RGB_REF=1 in the environment the left
 * picture is the reference's (ppu_render: what the Falcon aims for), not
 * the one stored in the snapshot. */
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

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s rom.sfc SNAP.BIN out.ppm\n", argv[0]);
        return 2;
    }
    long romsize, n;
    uint8_t *rom = load(argv[1], &romsize);
    uint8_t *snap = load(argv[2], &n);
    if (n < 12 || memcmp(snap, "SMKXSNAP", 8) || be32(snap + 8) != PPU_SNAP_VERSION) {
        fprintf(stderr, "%s: not a version %d SMKX snapshot\n", argv[2], PPU_SNAP_VERSION);
        return 2;
    }
    int a = 2;
    (void)a;
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
        uint32_t planes = be32(p); p += 4;
        const uint8_t *falcon = p; p += ppu_snap_frame(planes);
        p += 2 * PAL_SNAP_BYTES;
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
        PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_lorom, &bus,
                       log, (int)(nlog > 256 ? 256 : nlog), ident, hist, NULL, oamseg, (int)noamseg,
                       ppu_aptr_lorom, NULL};
    static uint32_t snes[PPU_W * PPU_H], fal[PPU_W * PPU_H];
    ppu_render_rgb(&in, snes);
    static DspPlan plan;
    static uint8_t pal[512];
    plan.prefer = DSP_KERNEL_TILE;
    int planned = dsp_plan(&in, &plan) == 0;
    memcpy(pal, cgram, 512);
    if (planned) {
        dsp_math_palette(&plan, cgram, pal);
        dsp_sub_palette(&plan, cgram, pal);
    }
    int bright = io[0x100] & 15, blank = (io[0x100] & 0x80) != 0;
    static uint8_t refpix[PPU_W * PPU_H];
    int useref = getenv("PPU_RGB_REF") != NULL;
    if (useref) {
        static uint8_t ident[256];
        static uint32_t h[256];
        for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
        PpuInput r = in;
        r.map = ident;
        r.hist = h;
        r.subreloc = planned ? plan.subreloc : NULL;
        ppu_render(&r, refpix);
    }
    for (int i = 0; i < PPU_W * PPU_H; i++) {
        int v = 0;
        if (useref) v = refpix[i];
        else if (planes == 8) {
            const uint8_t *w = falcon + (i / 16) * 16;
            int bit = 15 - (i & 15);
            for (int b = 0; b < 8; b++) v |= (((w[b * 2] << 8 | w[b * 2 + 1]) >> bit) & 1) << b;
        }
        uint16_t c = (uint16_t)(pal[v * 2] | pal[v * 2 + 1] << 8);
        int rgb[3] = {c & 31, (c >> 5) & 31, (c >> 10) & 31};
        uint32_t o = 0;
        for (int k = 0; k < 3; k++) o = o << 8 | (uint32_t)(blank ? 0 : rgb[k] * 255 / 31 * (bright + 1) / 16);
        fal[i] = planes == 8 || useref ? o : 0x404040;
    }
    FILE *f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); return 2; }
    fprintf(f, "P6\n%d %d\n255\n", 2 * PPU_W, PPU_H);
    int diff = 0;
    for (int y = 0; y < PPU_H; y++) {
        for (int x = 0; x < PPU_W; x++) {
            uint32_t c = fal[y * PPU_W + x];
            fputc((int)(c >> 16) & 255, f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f);
        }
        for (int x = 0; x < PPU_W; x++) {
            uint32_t c = snes[y * PPU_W + x];
            diff += c != fal[y * PPU_W + x];
            fputc((int)(c >> 16) & 255, f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f);
        }
    }
    fclose(f);
    printf("%s: planes %u, %d of %d pixels differ from the SNES picture%s\n", argv[2], planes, diff,
           PPU_W * PPU_H, planned && plan.mathpal ? " (colour math palette)" : "");
    return 0;
}
