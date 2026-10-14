/* Research probe: the DSP model's render of a frame shifted right by dx pixels (as src/smkx_lc.c renders a ring window at fine scroll dl)
 * against the reference renderer with the same shift, on real snapshots.
 *
 *   build/shift_probe rom.sfc DX[,DX...] SNAP.BIN...      (colour math left out: CGADSUB cleared, replay without CG writes) */
#define main plan_bench_main
#include "plan_bench.c"
#undef main
#include "dsp_model.h"

static void shift_oam(uint8_t *oam, int dx)
{
    for (int i = 0; i < 128; i++) {
        int hi = (oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        int x = oam[i * 4] | ((hi & 1) << 8);
        x = (x + dx) & 0x1FF;
        oam[i * 4] = (uint8_t)x;
        oam[512 + (i >> 2)] = (uint8_t)((oam[512 + (i >> 2)] & ~(1 << ((i & 3) * 2))) | ((x >> 8) << ((i & 3) * 2)));
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s rom.sfc DX[,DX...] SNAP.BIN...\n", argv[0]); return 2; }
    long rn;
    uint8_t *rom = load(argv[1], &rn);
    int dxs[16], ndx = 0;
    for (const char *q = argv[2]; *q && ndx < 16;) { dxs[ndx++] = atoi(q); while (*q && *q != ',') q++; if (*q == ',') q++; }
    int nocg = !getenv("SHIFT_COLOR");
    static DspPlan plan;
    static uint16_t model[PPU_H * 128], ref[PPU_H * 128];
    static uint8_t pixels[PPU_H * PPU_W], io2[0x6000], soam[PPU_OAM_BYTES];
    static PpuOamSeg sseg[PPU_OAMSEG_MAX];
    long frames = 0, bad = 0;
    for (int a = 3; a < argc; a++) {
        Fr f;
        if (!fr_load(&f, argv[a], rom)) { fprintf(stderr, "%s: bad snapshot\n", argv[a]); return 2; }
        for (int k = 0; k < ndx; k++) {
            int dx = dxs[k];
            memcpy(io2, f.in.io, sizeof io2);
            if (nocg) io2[0x131] = 0;
            memcpy(soam, f.in.oam, sizeof soam);
            shift_oam(soam, dx);
            PpuInput in = f.in;
            in.io = io2;
            in.oam = soam;
            for (int i = 0; i < f.in.noamseg; i++) { sseg[i] = f.oamseg[i]; shift_oam(sseg[i].oam, dx); }
            in.oamseg = sseg;
            memset(&plan, 0, sizeof plan);
            plan.prefer = DSP_KERNEL_TILE;
            ppu_replay_nocg = nocg;
            ppu_replay_dx = dx;
            int pr = dsp_plan(&in, &plan);
            if (pr) { printf("%s dx %d: plan failed (%d)\n", argv[a], dx, plan.fail); ppu_replay_dx = 0; ppu_replay_nocg = 0; continue; }
            uint8_t pages[DSP_VPAGES] = {0};
            memset(model, 0, sizeof model);
            if (dsp_model_frame(plan.w, plan.n, plan.kernel, in.vram, model, pages) < 0) { printf("%s dx %d: bad stream\n", argv[a], dx); ppu_replay_dx = 0; ppu_replay_nocg = 0; continue; }
            PpuInput ri = in;
            ri.subreloc = plan.subreloc;
            ppu_render(&ri, pixels);
            ppu_replay_dx = 0;
            ppu_replay_nocg = 0;
            ppu_planar8(pixels, ref);
            int nd = 0, rows = 0, fy = -1, band[3] = {0, 0, 0};
            for (int y = 0; y < PPU_H; y++) {
                int d = 0;
                for (int g = 0; g < 16; g++) {
                    int dg = 0;
                    for (int b = 0; b < 8; b++) dg |= model[y * 128 + g * 8 + b] != ref[y * 128 + g * 8 + b];
                    if (dg) { nd++; d = 1; band[y < 31 ? 0 : y < 191 ? 1 : 2]++; }
                }
                rows += d;
                if (d && fy < 0) fy = y;
            }
            frames++;
            bad += nd != 0;
            printf("%s dx %4d: kernel %d, %d groups differ in %d rows (bands %d/%d/%d), first row %d\n", argv[a], dx, plan.kernel, nd, rows,
                   band[0], band[1], band[2], fy);
        }
        free(f.snap);
    }
    printf("%ld renders, %ld differ\n", frames, bad);
    return bad != 0;
}
