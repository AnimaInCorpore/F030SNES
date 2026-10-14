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


/* Host timing of dsp_plan() on consecutive snapshots, as the runtime calls it: p.vdirty marks the 512-byte VRAM pages that
 * differ from the previous snapshot (the runtime's rt_plan_dirty), the plan's page cache stays between calls.
 *
 *   build/plan_bench rom.sfc REPS SNAP.BIN...      (oldest first; prints ms per plan) */
#include <time.h>
typedef struct { uint8_t *snap; PpuInput in; PpuBus bus; PpuWrite log[256]; PpuOamSeg oamseg[PPU_OAMSEG_MAX]; uint16_t bgofs[8], m7[6], m7ofs[2]; uint8_t ident[256]; uint32_t hist[256]; const uint8_t *vram; } Fr;

static int fr_load(Fr *f, const char *path, const uint8_t *rom)
{
    long n;
    f->snap = load(path, &n);
    uint8_t *snap = f->snap;
    if (n < 12 || memcmp(snap, "SMKXSNAP", 8) || be32(snap + 8) != PPU_SNAP_VERSION) return 0;
    const uint8_t *p = snap + 12;
    const uint8_t *vram = p; p += 65536;
    const uint8_t *cgram = p; p += 512;
    const uint8_t *oam = p; p += 544;
    const uint8_t *io = p; p += 0x6000;
    for (int i = 0; i < 8; i++) { f->bgofs[i] = be16(p); p += 2; }
    for (int i = 0; i < 6; i++) { f->m7[i] = be16(p); p += 2; }
    for (int i = 0; i < 2; i++) { f->m7ofs[i] = be16(p); p += 2; }
    uint32_t nlog = be32(p); p += 4;
    for (int i = 0; i < 256; i++) { f->log[i].line = be16(p); f->log[i].reg = be16(p + 2); f->log[i].value = p[4]; f->log[i].pad = 0; p += 6; }
    const uint8_t *wram = p; p += 131072;
    const uint8_t *sram = p; p += 8192;
    uint32_t planes = be32(p); p += 4;
    p += ppu_snap_frame(planes) + 2 * PAL_SNAP_BYTES_V3;
    uint32_t noamseg = be32(p); p += 4;
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) { f->oamseg[i].line = be16(p); memcpy(f->oamseg[i].oam, p + 4, PPU_OAM_BYTES); p += 4 + PPU_OAM_BYTES; }
    if (noamseg > PPU_OAMSEG_MAX) noamseg = PPU_OAMSEG_MAX;
    f->bus = (PpuBus){wram, sram, rom};
    for (int i = 0; i < 256; i++) f->ident[i] = (uint8_t)i;
    f->in = (PpuInput){vram, cgram, oam, io, f->bgofs, f->m7, f->m7ofs, ppu_abus_lorom, &f->bus,
                       f->log, (int)(nlog > 256 ? 256 : nlog), f->ident, f->hist, NULL, f->oamseg, (int)noamseg, ppu_aptr_lorom, NULL};
    f->vram = vram;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s rom.sfc REPS SNAP.BIN...\n", argv[0]); return 2; }
    long rn;
    uint8_t *rom = load(argv[1], &rn);
    int reps = atoi(argv[2]), nf = argc - 3;
    Fr *fr = calloc((size_t)nf, sizeof *fr);
    for (int i = 0; i < nf; i++) if (!fr_load(&fr[i], argv[3 + i], rom)) { fprintf(stderr, "%s: bad snapshot\n", argv[3 + i]); return 2; }
    static DspPlan plan;
    static uint8_t vdirty[DSP_VPAGES];
    double total = 0, worst = 0;
    long calls = 0, pages = 0, fails = 0;
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < nf; i++) {
            for (int pg = 0; pg < DSP_VPAGES; pg++)
                vdirty[pg] = (r == 0 && i == 0) || (i > 0 ? memcmp(fr[i].vram + pg * 512, fr[i - 1].vram + pg * 512, 512) != 0 : 0);
            for (int pg = 0; pg < DSP_VPAGES; pg++) pages += vdirty[pg];
            plan.vdirty = vdirty;
            plan.prefer = DSP_KERNEL_TILE;
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            int bad = dsp_plan(&fr[i].in, &plan);
            clock_gettime(CLOCK_MONOTONIC, &b);
            double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
            fails += bad != 0;
            if (getenv("PLAN_V"))
                printf("%s: kernel %d (0 tile, 1 Mode 7, 2 window) window rows %d sub rows %d math rows %d m7 rows %d sprites %d pages %d stream %d words\n",
                       argv[3 + i], plan.kernel, plan.winrows, plan.subrows, plan.mathrows, plan.m7rows, plan.sprites, plan.nneed, plan.n);
            total += ms;
            if (ms > worst) worst = ms;
            calls++;
        }
    printf("%ld plans (%ld failed), mean %.4f ms, worst %.4f ms, dirty pages per plan %.2f; stream %d words, %d pages needed\n",
           calls, fails, total / (double)calls, worst, (double)pages / (double)calls, plan.n, plan.nneed);
    return 0;
}
