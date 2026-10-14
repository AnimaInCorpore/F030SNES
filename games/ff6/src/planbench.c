/* PLANBENCH: the 68030 side's planning (dsp_plan) of stored real frames, the same
 * frames in every build, for a CPU profile in the patched Hatari
 * (scripts/tc_bench/planbench.py). Freestanding; falcon/planbench.s is the glue.
 * The frames are snapshots (build/bench15/F*.BIN) packed by planbench.py: u32 count,
 * then per frame u32 length and the snapshot; the ROM follows as tc_rom. */
#include <stddef.h>
#include "ppu_dsp.h"
#include "ppu_tc.h"

extern void m7t_print(const char *s);
extern void m7t_hex(uint32_t v);
extern const uint8_t pb_frames[];
extern const uint8_t pb_rom[];

void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memset(void *d, int v, unsigned long n)
{
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)v;
    return d;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

#define MAXF 24
#ifndef ROWS
#define ROWS 0
#endif
#define PAL_SNAP_BYTES_V3 406

static PpuInput in[MAXF];
static PpuBus bus[MAXF];
static PpuWrite logs[MAXF][256];
static PpuOamSeg oamseg[MAXF][PPU_OAMSEG_MAX];
static uint16_t bgofs[MAXF][8], m7[MAXF][6], m7ofs[MAXF][2];
static uint8_t ident[256];
static uint32_t hist[256];
static DspPlan plan;
static uint8_t vd[DSP_VPAGES];
static const uint8_t *vrams[MAXF];
static int nfr;

static int parse(int f, const uint8_t *snap)
{
    const uint8_t *p = snap + 12;
    const uint8_t *vram = p; p += 65536;
    const uint8_t *cgram = p; p += 512;
    const uint8_t *oam = p; p += 544;
    const uint8_t *io = p; p += 0x6000;
    for (int i = 0; i < 8; i++) { bgofs[f][i] = be16(p); p += 2; }
    for (int i = 0; i < 6; i++) { m7[f][i] = be16(p); p += 2; }
    for (int i = 0; i < 2; i++) { m7ofs[f][i] = be16(p); p += 2; }
    uint32_t nlog = be32(p); p += 4;
    for (int i = 0; i < 256; i++) {
        logs[f][i].line = be16(p); logs[f][i].reg = be16(p + 2); logs[f][i].value = p[4]; logs[f][i].pad = 0;
        p += 6;
    }
    const uint8_t *wram = p; p += 131072;
    const uint8_t *sram = p; p += 8192;
    uint32_t planes = be32(p); p += 4;
    p += ppu_snap_frame(planes) + 2 * PAL_SNAP_BYTES_V3;
    uint32_t noamseg = be32(p); p += 4;
    for (int i = 0; i < PPU_OAMSEG_MAX; i++) {
        oamseg[f][i].line = be16(p);
        for (int k = 0; k < PPU_OAM_BYTES; k++) oamseg[f][i].oam[k] = p[4 + k];
        p += 4 + PPU_OAM_BYTES;
    }
    if (noamseg > PPU_OAMSEG_MAX) noamseg = PPU_OAMSEG_MAX;
    bus[f].wram = wram;
    bus[f].sram = sram;
    bus[f].rom = pb_rom;
    PpuInput t = {vram, cgram, oam, io, bgofs[f], m7[f], m7ofs[f], ppu_abus_hirom, &bus[f],
                  logs[f], (int)(nlog > 256 ? 256 : nlog), ident, hist, NULL, oamseg[f], (int)noamseg,
                  ppu_aptr_hirom, NULL};
    in[f] = t;
    vrams[f] = vram;
    return 0;
}

/* the pages written since the previous frame, as the runtime's vdirty */
__attribute__((noinline)) static void bench_dirty(int f)
{
    const uint8_t *a = vrams[f], *b = vrams[(f + nfr - 1) % nfr];
    for (int pg = 0; pg < DSP_VPAGES; pg++) {
        int d = 0;
        for (int i = 0; i < 512 && !d; i += 4)
            d = *(const uint32_t *)(const void *)(a + pg * 512 + i) != *(const uint32_t *)(const void *)(b + pg * 512 + i);
        vd[pg] = (uint8_t)d;
    }
}

static DspRows rowsr;
static uint8_t pdirty[DSP_VPAGES];

/* the change detection of partial rendering (dsp_rows), then the picture is the reference */
__attribute__((noinline)) static void bench_rows(int f)
{
    for (int pg = 0; pg < DSP_VPAGES; pg++) pdirty[pg] = vd[pg];
    rowsr.vdirty = pdirty;
    dsp_rows(&in[f], &rowsr);
    dsp_rows_commit();
    for (int pg = 0; pg < DSP_VPAGES; pg++) vd[pg] = 0;        /* (the runtime's plan gets no page flags: pr_begin took them) */
}

/* one picture, as smkx_ppu_plan does: the whole frame, or a run of each kind when it mixes */
__attribute__((noinline)) static int bench_plan(int f)
{
    plan.band0 = plan.band1 = 0;
    plan.vdirty = vd;
    plan.tc = 1;
    plan.prefer = DSP_KERNEL_TC;
    int bad = dsp_plan(&in[f], &plan);
    if (bad && plan.fail == 6) {
        int run[3][2], nr = dsp_tc_runs(plan.rowkind, run, 3);
        for (int k = 0; k < nr; k++) {
            plan.band0 = run[k][0];
            plan.band1 = run[k][1];
            bad = dsp_plan(&in[f], &plan);
        }
    }
    return bad;
}

__attribute__((noinline)) void bench_go(void) { __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) void bench_done(void) { __asm__ volatile("" ::: "memory"); }

int tctest_main(void)
{
    m7t_print("PLANBENCH start\n");
    for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
    const uint8_t *p = pb_frames;
    uint32_t n = be32(p);
    p += 4;
    if (n > MAXF) n = MAXF;
    for (uint32_t f = 0; f < n; f++) {
        uint32_t len = be32(p);
        p += 4;
        parse((int)f, p);
        p += len;
    }
    nfr = (int)n;
    for (int pg = 0; pg < DSP_VPAGES; pg++) vd[pg] = 1;
    int bad = 0;
    for (int f = 0; f < (int)n; f++) bench_plan(f);       /* one warm-up pass: the block cache */
    bench_go();
    for (int pass = 0; pass < 2; pass++)
        for (int f = 0; f < (int)n; f++) {
            bench_dirty(f);
            if (ROWS) bench_rows(f);
            bad += bench_plan(f) != 0;
        }
    bench_done();
    m7t_print("PLANBENCH frames ");
    m7t_hex(n);
    m7t_print(" failed ");
    m7t_hex((uint32_t)bad);
    m7t_print("\n");
    return 0;
}
