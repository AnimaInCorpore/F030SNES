/* Host check of the DSP renderer's design: for each SNAP.BIN, the frame
 * through dsp_plan() and the DSP model (tools/dsp_model.c) against
 * ppu_render() with the identity map + ppu_planar8(), and what the frame
 * asks of the DSP (stream words, VRAM pages).
 *
 *   build/dsp_snap rom.sfc SNAP.BIN... [-ppm DIR] [-pack FILE]
 *
 * -pack writes the frames for PPUTEST.TOS (src/pputest.c), all fields
 * big-endian: "PPU2", frames u32, then per frame: the program u32
 * (DSP_KERNEL_TILE, DSP_KERNEL_M7), stream words u32 and the words (u32
 * each), pages u32 and per page its index u16 and 256 VRAM words (u16,
 * raw: PPUTEST packs the Mode 7 plane itself), then the expected planes
 * (PPU_H * 128 u16).
 *
 * Exits non-zero on any difference, or when the model read a VRAM page
 * the plan did not ask for. With -pack the Falcon's planes stored in a
 * snapshot are only reported: a pack is made from the model, and a
 * snapshot from an older build may show what that build drew. */
#include "dsp_model.h"

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

#define PAL_SNAP_BYTES_V3 406   /* palette.h: PAL_SNAP_BYTES */

static void write_ppm(const char *path, const uint16_t *planar, const uint8_t *cgram, uint8_t inidisp)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "P6\n%d %d\n255\n", PPU_W, PPU_H);
    int bright = inidisp & 15;
    for (int i = 0; i < PPU_W * PPU_H; i++) {
        const uint16_t *w = planar + (i / 16) * 8;
        int bit = 15 - (i & 15), v = 0;
        for (int b = 0; b < 8; b++) v |= ((w[b] >> bit) & 1) << b;
        uint16_t c = (uint16_t)(cgram[v * 2] | cgram[v * 2 + 1] << 8);
        int rgb[3] = {c & 31, (c >> 5) & 31, (c >> 10) & 31};
        for (int k = 0; k < 3; k++) {
            int x = (inidisp & 0x80) ? 0 : rgb[k] * 255 / 31 * (bright + 1) / 16;
            fputc(x, f);
        }
    }
    fclose(f);
}

static FILE *pack, *packlc;
static int packed, packed_lc;

static void be(FILE *f, uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; i--) fputc((int)(v >> (i * 8)) & 255, f);
}

static void pack_frame(const DspPlan *plan, const uint8_t *vram, const uint16_t *planes)
{
    be(pack, (uint32_t)plan->kernel, 4);
    be(pack, (uint32_t)plan->n, 4);
    for (int i = 0; i < plan->n; i++) be(pack, plan->w[i], 4);
    /* the Mode 7 program: the sprites' pages below 64 once more, raw
     * (index | $8000) */
    int copies = 0;
    if (plan->kernel == DSP_KERNEL_M7)
        for (int pg = 0; pg < 64; pg++) copies += plan->sprneed[pg];
    be(pack, (uint32_t)(plan->nneed + copies), 4);
    for (int pg = 0; pg < DSP_VPAGES + 64; pg++) {
        int q = pg < DSP_VPAGES ? pg : pg - DSP_VPAGES;
        if (pg < DSP_VPAGES ? !plan->need[q] : plan->kernel != DSP_KERNEL_M7 || !plan->sprneed[q]) continue;
        be(pack, (uint32_t)(pg < DSP_VPAGES ? q : q | 0x8000), 2);
        for (int i = 0; i < 256; i++) {
            int a = (q * 256 + i) * 2;
            be(pack, (uint32_t)(vram[a] | vram[a + 1] << 8), 2);
        }
    }
    for (int i = 0; i < PPU_H * 128; i++) be(pack, planes[i], 2);
    packed++;
}

/* The list compositor's test frames (LCD1): per frame the stream and pages as above for the LC build
 * (DSP_KERNEL_LC), the 224 row masks, and the list the DSP must send: for each row with a mask, each set
 * group in order: its eight plane words. Mask patterns by frame: all groups, about 15% at random, rows 40-79, about 2% at random. */
static uint32_t lc_seed = 0x1234567;
static uint32_t lc_rnd(void) { lc_seed = lc_seed * 1103515245u + 12345u; return lc_seed >> 16; }

static void pack_lc(const DspPlan *plan, const uint8_t *vram, const uint16_t *planes, int pat)
{
    be(packlc, plan->kernel == DSP_KERNEL_WIN ? DSP_KERNEL_LCW : DSP_KERNEL_LC, 4);
    be(packlc, (uint32_t)plan->n, 4);
    for (int i = 0; i < plan->n; i++) be(packlc, plan->w[i], 4);
    be(packlc, (uint32_t)plan->nneed, 4);
    for (int pg = 0; pg < DSP_VPAGES; pg++) {
        if (!plan->need[pg]) continue;
        be(packlc, (uint32_t)pg, 2);
        for (int i = 0; i < 256; i++) {
            int a = (pg * 256 + i) * 2;
            be(packlc, (uint32_t)(vram[a] | vram[a + 1] << 8), 2);
        }
    }
    uint16_t mask[PPU_H];
    for (int y = 0; y < PPU_H; y++) mask[y] = 0;
    if (pat <= 3) {
        for (int y = 0; y < PPU_H; y++)
            for (int g = 0; g < 16; g++) {
                int on = pat == 0 ? 1 : pat == 1 ? (lc_rnd() % 100) < 15 : pat == 2 ? (y >= 40 && y < 80) : (lc_rnd() % 100) < 2;
                mask[y] |= (uint16_t)(on << g);
            }
    } else if (pat == 4 || pat == 5) {                       /* one column of groups: group 0 or group 15, every row */
        for (int y = 0; y < PPU_H; y++) mask[y] = (uint16_t)(pat == 4 ? 1 : 0x8000);
    } else if (pat == 6) {                                   /* runs of one to three groups in half the rows */
        for (int y = 0; y < PPU_H; y++)
            if (lc_rnd() & 1) {
                int w = 1 + lc_rnd() % 3, g0 = lc_rnd() % (17 - w);
                for (int g = g0; g < g0 + w; g++) mask[y] |= (uint16_t)(1u << g);
            }
    } else {                                                 /* sprite boxes: 16 rows by two or three groups, at random */
        for (int k = 0; k < 4; k++) {
            int y0 = lc_rnd() % (PPU_H - 16), g0 = lc_rnd() % 14, w = 2 + lc_rnd() % 2;
            for (int y = y0; y < y0 + 16; y++)
                for (int g = g0; g < g0 + w; g++) mask[y] |= (uint16_t)(1u << g);
        }
    }
    for (int y = 0; y < PPU_H; y++) be(packlc, mask[y], 2);
    static uint16_t list[PPU_H * 16 * 9 + 16];
    int n = 0;
    for (int y = 0; y < PPU_H; y++)
        for (int g = 0; g < 16; g++) {
            if (!(mask[y] >> g & 1)) continue;
            for (int b = 0; b < 8; b++) list[n++] = planes[y * 128 + g * 8 + b];
        }
    be(packlc, (uint32_t)n, 4);
    for (int i = 0; i < n; i++) be(packlc, list[i], 2);
    packed_lc++;
}

static int check(const char *path, const uint8_t *rom, const char *ppmdir)
{
    long n;
    uint8_t *snap = load(path, &n);
    if (n < 12 || memcmp(snap, "SMKXSNAP", 8) || be32(snap + 8) != PPU_SNAP_VERSION) {
        fprintf(stderr, "%s: not a version %d SMKX snapshot\n", path, PPU_SNAP_VERSION);
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
    uint32_t planes = be32(p); p += 4;
    const uint8_t *falcon = p;
    p += ppu_snap_frame(planes) + 2 * PAL_SNAP_BYTES_V3;   /* the Falcon's frame and palettes */
    uint32_t noamseg = be32(p); p += 4;
    static PpuOamSeg oamseg[PPU_OAMSEG_MAX];
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
        oamseg[i].line = be16(p);
        memcpy(oamseg[i].oam, p + 4, PPU_OAM_BYTES);
        p += 4 + PPU_OAM_BYTES;
    }
    if (noamseg > PPU_OAMSEG_MAX) noamseg = PPU_OAMSEG_MAX;
    if (p - snap != n) {
        fprintf(stderr, "%s: %ld bytes, expected %ld\n", path, n, (long)(p - snap));
        return 2;
    }
    PpuBus bus = {wram, sram, rom};
    static uint8_t ident[256];
    for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
    static uint32_t hist[256];
    PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, ppu_abus_hirom, &bus,
                   log, (int)(nlog > 256 ? 256 : nlog), ident, hist, NULL, oamseg, (int)noamseg, ppu_aptr_hirom, NULL};

    /* DSP_SHIFT=dx: the frame as the list compositor's DSP renders it (shifted right by dx, CGADSUB cleared, no colour math or
     * CGRAM in the replay), against the reference with the same shift */
    if (getenv("DSP_SHIFT")) {
        int dx = atoi(getenv("DSP_SHIFT"));
        static uint8_t oam2[PPU_OAM_BYTES];
        static PpuOamSeg seg2[PPU_OAMSEG_MAX];
        memcpy(oam2, oam, sizeof oam2);
        memcpy(seg2, oamseg, sizeof seg2);
        for (int k = -1; k < (int)noamseg; k++) {
            uint8_t *o = k < 0 ? oam2 : seg2[k].oam;
            for (int i = 0; i < 128; i++) {
                int hi = (o[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
                int x = (o[i * 4] | ((hi & 1) << 8));
                x = (x + dx) & 0x1FF;
                o[i * 4] = (uint8_t)x;
                o[512 + (i >> 2)] = (uint8_t)((o[512 + (i >> 2)] & ~(1 << ((i & 3) * 2))) | ((x >> 8) << ((i & 3) * 2)));
            }
        }
        in.oam = oam2;
        in.oamseg = seg2;
        ((uint8_t *)io)[0x131] = 0;
        ppu_replay_dx = dx;
        ppu_replay_nocg = 1;
    }
    static uint8_t chunky[PPU_W * PPU_H];
    static uint16_t ref[PPU_W * PPU_H / 2], dsp[PPU_W * PPU_H / 2];
    static DspPlan plan;
    if (dsp_plan(&in, &plan)) {
        printf("%s: stream does not fit\n", path);
        free(snap);
        return 1;
    }
    in.subreloc = plan.subreloc;                /* the sub screen's copies, as the Falcon shows them */
    ppu_render(&in, chunky);
    ppu_planar8(chunky, ref);
    uint8_t pages[DSP_VPAGES] = {0};
    memset(dsp, 0, sizeof dsp);
    int skipped = dsp_model_frame(plan.w, plan.n, plan.kernel, vram, dsp, pages);
    if (skipped < 0) {
        printf("%s: bad stream\n", path);
        free(snap);
        return 1;
    }
    int diff = 0, rows = 0, extra = 0, used = 0;
    for (int row = 0; row < PPU_H; row++) {
        int d = 0;
        for (int i = 0; i < 128; i++) d += ref[row * 128 + i] != dsp[row * 128 + i];
        diff += d;
        rows += d != 0;
    }
    for (int i = 0; i < DSP_VPAGES; i++) {
        used += pages[i];
        extra += pages[i] && !plan.need[i];
    }
    int fdiff = -1;                           /* the Falcon's eight planes vs the model */
    if (planes == 8) {
        fdiff = 0;
        for (int i = 0; i < PPU_W * PPU_H / 2; i++)
            fdiff += (uint16_t)(falcon[i * 2] << 8 | falcon[i * 2 + 1]) != dsp[i];
    }
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    printf("%s mode %d TM $%02X: words %4d changes %3d segs %d sprites %3d  pages need %3d read %3d"
           "  m7 rows %3d  diff %d words in %d rows%s\n",
           name, io[0x105] & 7, io[0x12C], plan.n, plan.changes, plan.segs, plan.sprites,
           plan.nneed, used, skipped, diff, rows, extra ? "  READ OUTSIDE PLAN" : "");
    if (fdiff >= 0) printf("    Falcon's eight planes vs the model: %d words differ\n", fdiff);
    if (ppmdir) {
        char out[1024];
        snprintf(out, sizeof out, "%s/%.*s.ppm", ppmdir, (int)(strcspn(name, ".")), name);
        write_ppm(out, dsp, cgram, io[0x100]);
    }
    if (pack) pack_frame(&plan, vram, dsp);
    if (packlc) {
        /* the list compositor leaves colour math out: CGADSUB cleared, then the tile program takes the frame (the window build,
         * LCW, when rows have windows) */
        ((uint8_t *)io)[0x131] = 0;
        static DspPlan plan2;
        static uint16_t dsp2[PPU_W * PPU_H / 2];
        uint8_t pages2[DSP_VPAGES] = {0};
        if (!dsp_plan(&in, &plan2) &&
            ((plan2.kernel == DSP_KERNEL_TILE && plan2.nneed <= DSP_LC_SLOTS) || (plan2.kernel == DSP_KERNEL_WIN && plan2.nneed <= DSP_LCW_SLOTS)) &&
            dsp_model_frame(plan2.w, plan2.n, plan2.kernel, vram, dsp2, pages2) >= 0) {
            if (getenv("LC_DEBUG")) fprintf(stderr, "packlc %s kernel %d\n", name, plan2.kernel);
            for (int pat = 0; pat < 8; pat++) pack_lc(&plan2, vram, dsp2, pat);
        }
    }
    free(snap);
    return diff != 0 || extra != 0 || (fdiff > 0 && !pack);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s rom.sfc SNAP.BIN... [-ppm DIR]\n", argv[0]);
        return 2;
    }
    long romsize;
    uint8_t *rom = load(argv[1], &romsize);
    const char *ppmdir = NULL;
    int fail = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-ppm") && i + 1 < argc) { ppmdir = argv[++i]; continue; }
        if (!strcmp(argv[i], "-packlc") && i + 1 < argc) {
            packlc = fopen(argv[++i], "wb");
            if (!packlc) { perror(argv[i]); return 2; }
            fwrite("LCD1\0\0\0\0", 1, 8, packlc);
        }
        if (!strcmp(argv[i], "-pack") && i + 1 < argc) {
            pack = fopen(argv[++i], "wb");
            if (!pack) { perror(argv[i]); return 2; }
            fwrite("PPU2\0\0\0\0", 1, 8, pack);
        }
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-ppm") || !strcmp(argv[i], "-pack") || !strcmp(argv[i], "-packlc")) { i++; continue; }
        fail |= check(argv[i], rom, ppmdir);
    }
    if (packlc) {
        fseek(packlc, 4, SEEK_SET);
        be(packlc, (uint32_t)packed_lc, 4);
        fclose(packlc);
    }
    if (pack) {
        fseek(pack, 4, SEEK_SET);
        be(pack, (uint32_t)packed, 4);
        fclose(pack);
    }
    return fail;
}
