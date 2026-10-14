/* Host test of the compositor's block writer (src/smkx_lc.c: put_block, put_group, emit): the code under test is the Falcon's
 * file itself, included, with C versions of the assembly copy routines and stubs for the DSP driver.
 *
 *   lcput_test [iterations]
 *
 * Two kinds of blocks into a ring page at random positions, lengths and heights (they cross the ring's end and the mirror behind
 * it): a walk (source: a list, rows one after the other) and a copy (source: the other page, the same ring groups); the page's
 * mirror (ring groups 0 .. LC_DUP - 1 again at LC_N ..) must equal the ring. A copy that ran on into the source page's mirror
 * as its source for the destination's mirror wrote garbage there: found in F030CT in the game (a stale mirror under the
 * window rows past the ring's end), by the snapshots, then by this. */
#define SMKX_PROF_H
#include <stdint.h>
#define PROF_HZ 38400u
static inline uint32_t prof_now(void) { return 0; }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/smkx_lc.c"

uint8_t rt_plan_dirty[128];
uint32_t smkx_ppu_stat[PPU_ST_N];
uint32_t lc_prof[10];
void smkx_vid_set_window(uint32_t addr, int hscroll, int lineoff) { (void)addr; (void)hscroll; (void)lineoff; }
void smkx_set_palette(const uint8_t *cgram, uint8_t inidisp) { (void)cgram; (void)inidisp; }
int smkx_ppu_boot(int kernel) { (void)kernel; return 1; }
int smkx_ppu_loaded(void) { return 0; }
int smkx_ppu_plan_lc(const PpuInput *in, int force_win) { (void)in; (void)force_win; return -1; }
void smkx_ppu_keep(int k) { (void)k; }
void smkx_ppu_restore(int k) { (void)k; }
void smkx_ppu_send_lc(const uint16_t *mask, uint16_t *list, uint32_t words) { (void)mask; (void)list; (void)words; }
int smkx_ppu_done(void) { return 1; }
void smkx_lc_cpu(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t groups, uint32_t rows)
{
    for (uint32_t r = 0; r < rows; r++) memcpy(dst + r * 512, src + r * sstride, groups * 16);
}
void smkx_lc_blit(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t groups, uint32_t rows) { smkx_lc_cpu(dst, src, sstride, groups, rows); }
int smkx_lc_blit_busy(void) { return 0; }
void smkx_lc_dcache_clear(void) {}

static uint8_t pageA[PAGE_GROUPS * 16], pageB[PAGE_GROUPS * 16], srcbuf[PAGE_GROUPS * 16];
static uint32_t rs = 7;
static uint32_t frnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 20000, bad = 0, bad2 = 0;
    for (int it = 0; it < iters; it++) {
        int len = 1 + (int)(frnd() % 17), rows = 1 + (int)(frnd() % 224), L0 = (int)(frnd() % LC_N);
        for (int i = 0; i < PAGE_GROUPS * 16; i++) srcbuf[i] = (uint8_t)frnd();
        memset(pageA, 0, sizeof pageA);
        put_block(pageA, srcbuf, (uint32_t)len * 16, 0, L0, len, rows);
        bq_flush();
        for (int r = 0; r < rows; r++)
            for (int j = 0; j < len; j++) {
                int L = (L0 + r * 32 + j) & (LC_N - 1);
                const uint8_t *e = srcbuf + (r * len + j) * 16;
                if (memcmp(pageA + L * 16, e, 16) && bad++ < 5) printf("walk, ring wrong: L0 %d len %d rows %d r %d j %d\n", L0, len, rows, r, j);
                if (L < LC_DUP && memcmp(pageA + (L + LC_N) * 16, e, 16) && bad++ < 5) printf("walk, mirror wrong: L0 %d len %d rows %d r %d j %d\n", L0, len, rows, r, j);
            }
    }
    for (int it = 0; it < iters; it++) {
        int len = 1 + (int)(frnd() % 17), rows = 1 + (int)(frnd() % 224), L0 = (int)(frnd() % LC_N);
        for (int i = 0; i < LC_N * 16; i++) pageA[i] = (uint8_t)frnd();
        memcpy(pageA + LC_N * 16, pageA, LC_DUP * 16);            /* a consistent source page */
        memset(pageB, 0, sizeof pageB);
        put_block(pageB, pageA + L0 * 16, 512, (uint32_t)LC_N * 16, L0, len, rows);
        bq_flush();
        for (int r = 0; r < rows; r++)
            for (int j = 0; j < len; j++) {
                int L = (L0 + r * 32 + j) & (LC_N - 1);
                if (memcmp(pageB + L * 16, pageA + L * 16, 16) && bad2++ < 5) printf("copy, ring wrong: L0 %d len %d rows %d r %d j %d\n", L0, len, rows, r, j);
                if (L < LC_DUP && memcmp(pageB + (L + LC_N) * 16, pageA + L * 16, 16) && bad2++ < 5) printf("copy, mirror wrong: L0 %d len %d rows %d r %d j %d\n", L0, len, rows, r, j);
            }
    }
    printf("put_block: walks %d bad groups, copies %d bad groups (%d blocks each)\n", bad, bad2, iters);
    /* whole jobs: a frame's masks (random runs and bands, the 17th group, a few rows empty), the list of the composed groups in row order,
     * the groups the other page missed (cmask: not among the composed): walk() and copy_groups() must leave the page as the plan says */
    int bad3 = 0, jobs = iters / 20;
    static LcPlan plan;
    static uint8_t list[PPU_H * 17 * 16], listb[PPU_H * 16], expect[PAGE_GROUPS * 16];
    for (int it = 0; it < jobs; it++) {
        int S = (int)(frnd() % LC_N);
        for (int i = 0; i < LC_N * 16; i++) pageA[i] = (uint8_t)frnd();
        memcpy(pageA + LC_N * 16, pageA, LC_DUP * 16);
        memcpy(pageB, pageA, sizeof pageB);                      /* the page to be updated: starts as the other's, then differs below */
        for (int i = 0; i < LC_N * 16; i += 16) if (frnd() % 3 == 0) pageB[i] ^= 0x55;
        memcpy(pageB + LC_N * 16, pageB, LC_DUP * 16);
        memcpy(expect, pageB, sizeof expect);
        plan.S = S;
        uint16_t mask_b[PPU_H];
        int nl = 0, nb = 0, band = 0, prev = 0;
        uint32_t mode = frnd() % 4;
        for (int y = 0; y < PPU_H; y++) {
            uint32_t m;
            if (band > 0) { m = (uint32_t)prev; band--; }
            else {
                m = mode == 0 ? 0xFFFFu : mode == 1 ? (uint32_t)(frnd() & frnd() & 0xFFFF) : mode == 2 ? (uint32_t)(1u << (frnd() % 16)) << (frnd() % 2) : (frnd() % 3 ? 0u : (frnd() & 0xFFFF));
                m &= 0xFFFFu;
                if (frnd() % 4 == 0) { band = 1 + (int)(frnd() % 30); prev = (int)m; }
            }
            plan.maskA[y] = (uint16_t)m;
            plan.maskB[y] = mask_b[y] = (uint16_t)(frnd() % 5 == 0);
            /* groups the other page missed: not composed here */
            uint32_t all = 0x1FFFFu & ~(m | ((uint32_t)mask_b[y] << 16));
            plan.cmask[y] = (uint32_t)(frnd() & frnd()) & all & ((frnd() % 2) ? 0x1FFFFu : 0xFFFFu);
        }
        /* the list and the expected page */
        for (int y = 0; y < PPU_H; y++) {
            for (int j = 0; j < 17; j++) {
                int L = (S + y * LC_NGR + j) & (LC_N - 1);
                int on = j < 16 ? (plan.maskA[y] >> j) & 1 : plan.maskB[y] & 1;
                if (on) {
                    uint8_t *dst = (j < 16 ? list + nl * 16 : listb + nb * 16);
                    for (int k = 0; k < 16; k++) dst[k] = (uint8_t)(0x80 + (y * 7 + j * 3 + k + it) );
                    if (j < 16) nl++; else nb++;
                    memcpy(expect + L * 16, dst, 16);
                    if (L < LC_DUP) memcpy(expect + (L + LC_N) * 16, dst, 16);
                } else if ((plan.cmask[y] >> j) & 1) {
                    memcpy(expect + L * 16, pageA + L * 16, 16);
                    if (L < LC_DUP) memcpy(expect + (L + LC_N) * 16, pageA + L * 16, 16);
                }
            }
        }
        walk((const uint16_t *)(const void *)list, pageB, S, 0, plan.maskA);
        walk((const uint16_t *)(const void *)listb, pageB, S, 16, mask_b);
        copy_groups(&plan, pageB, pageA);
        bq_flush();
        if (memcmp(pageB, expect, PAGE_GROUPS * 16)) {
            int first = -1, nbad = 0;
            for (int g = 0; g < PAGE_GROUPS; g++) if (memcmp(pageB + g * 16, expect + g * 16, 16)) { if (first < 0) first = g; nbad++; }
            if (bad3++ < 5) printf("job wrong: S %d mode %u: %d groups differ, first ring group %d (%s)\n", S, mode, nbad, first, first >= LC_N ? "mirror" : "ring");
        }
    }
    printf("whole jobs (walk + copy_groups): %d of %d wrong\n", bad3, jobs);
    return bad || bad2 || bad3;
}
