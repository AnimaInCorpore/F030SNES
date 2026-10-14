/* Random frames for the true-colour renderer, every BG mode: each frame
 * through dsp_plan() (DspPlan.tc) and the model (tools/tc_model.c) against
 * ppu_render_565(). What real frames rarely show: modes 3-6 (8 bpp,
 * offset-per-tile in 2, 4 and 6), 16x16 tiles, BG3 on top, wide and tall
 * maps, both screens with their window masks and the colour window, colour
 * math of every kind (add, subtract, half, clip, the fixed colour or the
 * sub screen), brightness, register and CGRAM writes mid-frame, sprites at
 * the edges. Every eighth frame is Mode 7 (the TC7 program).
 *
 *   build/tc_rand FRAMES SEED [pack.bin]
 *
 * pack.bin: the frames for TCTEST (src/tctest.c), as tools/tc_snap.c -pack
 * writes them. */
#include "tc_model.h"
#include "ppu_tc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint8_t abus_zero(uint32_t a, void *ctx) { (void)a; (void)ctx; return 0; }

void tc_pack(FILE *f, const DspPlan *plan, const uint8_t *vram, const uint16_t *pic);

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s FRAMES SEED [pack.bin]\n", argv[0]);
        return 2;
    }
    int frames = atoi(argv[1]);
    rng = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    FILE *pk = 0;
    if (argc == 4) {
        pk = fopen(argv[3], "wb");
        if (!pk) { perror(argv[3]); return 2; }
        fwrite("TCP1", 1, 4, pk);
        for (int i = 0; i < 4; i++) fputc((frames >> (24 - 8 * i)) & 255, pk);
    }
    static uint8_t vram[65536], cgram[512], oam[544], io[0x6000];
    static uint8_t ident[256];
    static uint16_t ref[PPU_W * PPU_H], mod[PPU_W * PPU_H];
    static uint32_t hist[256];
    static DspPlan plan;
    static PpuWrite log[32];
    for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
    int bad = 0, tries = 0;
    int permode[8] = {0};
    for (int fr = 0; fr < frames; fr++) {
        for (int i = 0; i < 65536; i++) vram[i] = (uint8_t)rnd();
        memset(io, 0, sizeof io);
        uint8_t *r = io + 0x100;                     /* $2100.. */
        r[0x00] = (uint8_t)((rnd() & 3) ? 0x0F : rnd() & 15);
        int m7f = !getenv("TCR_NOM7") && (fr & 7) == 7;
        /* every 16th frame mixed: a tile mode on top, Mode 7 from a line on
         * (DSP runs, smkx_tc.c) */
        int mixed = !getenv("TCR_NOM7") && (fr & 15) == 3;
        int mode = m7f ? 7 : (int)(rnd() % 7);
        r[0x05] = (uint8_t)(mode | ((rnd() & 1) << 3) | ((rnd() & 15) << 4));
        r[0x2C] = (uint8_t)(rnd() & 0x1F);
        r[0x2D] = (uint8_t)(rnd() & 0x1F);
        r[0x01] = (uint8_t)((rnd() & 7) << 5 | (rnd() & 3) << 3 | 3);   /* OBJ at $6000 */
        /* maps at $4000-$5FFF, characters at $0000 or $1000 */
        for (int i = 0; i < 4; i++) r[0x07 + i] = (uint8_t)((0x40 + i * 8) | (rnd() & 1));
        r[0x0B] = (uint8_t)(rnd() & 0x11);
        r[0x0C] = (uint8_t)(rnd() & 0x11);
        uint16_t bgofs[8], m7[6] = {0}, m7ofs[2] = {0};
        for (int i = 0; i < 8; i++) bgofs[i] = (uint16_t)(rnd() & 0x3FF);
        if (m7f || mixed) {
            static const uint8_t sels[] = {0x00, 0x80, 0xC0};
            if (m7f) r[0x05] = 7;
            r[0x1A] = (uint8_t)(sels[rnd() % 3] | (rnd() & 3));
            for (int i = 0; i < 4; i++) {
                int v = (int)(rnd() % 256) - 128;
                if (i == 0 || i == 3) v = (int)(128 + rnd() % 384) * ((rnd() & 1) ? 1 : -1);
                m7[i] = (uint16_t)v;
            }
            m7[4] = (uint16_t)(rnd() & 0x1FFF);
            m7[5] = (uint16_t)(rnd() & 0x1FFF);
            m7ofs[0] = (uint16_t)(rnd() & 0x1FFF);
            m7ofs[1] = (uint16_t)(rnd() & 0x1FFF);
        }
        /* map entries: characters 0-127 (so the frame's pages fit), 8 bpp:
         * 0-63 */
        for (int w = 0x4000; w < 0x6000; w++) {
            uint16_t e = (uint16_t)(vram[w * 2] | vram[w * 2 + 1] << 8);
            e = (uint16_t)((e & 0xFC00) | (e & 0x3F) | ((mode == 3 || mode == 4) ? 0 : (e >> 1) & 0x40));
            vram[w * 2] = (uint8_t)e;
            vram[w * 2 + 1] = (uint8_t)(e >> 8);
        }
        /* half the characters' rows empty, a quarter solid, as real tiles */
        for (int w = 0; w < 0x4000; w++)
            if ((rnd() & 3) == 0) vram[w * 2] = vram[w * 2 + 1] = 0;
        for (int i = 0; i < 512; i++) cgram[i] = (uint8_t)rnd();
        for (int i = 0; i < 128; i++) {
            uint8_t *e = oam + i * 4;
            int x = (int)(rnd() % 320) - 32;
            e[0] = (uint8_t)x;
            e[1] = (uint8_t)(rnd() % 240);
            e[2] = (uint8_t)(rnd() & 0x3F);
            e[3] = (uint8_t)(rnd() & 0xFF);
            int hi = ((x < 0) ? 1 : 0) | (int)(rnd() & 2);
            oam[512 + (i >> 2)] = (uint8_t)((oam[512 + (i >> 2)] & ~(3 << ((i & 3) * 2))) | (hi << ((i & 3) * 2)));
        }
        /* windows (both masks, the colour window) and colour math */
        for (int n = 0x23; n <= 0x2B; n++) r[n] = (uint8_t)rnd();
        for (int n = 0x26; n <= 0x29; n++)
            if (rnd() & 1) r[n] = (uint8_t)((rnd() & 1) ? (rnd() & 15) : 255 - (rnd() & 15));
        r[0x2E] = (uint8_t)(rnd() & 0x1F);
        r[0x2F] = (uint8_t)(rnd() & 0x1F);
        r[0x30] = (uint8_t)rnd();
        r[0x31] = (uint8_t)rnd();
        for (int k = 0; k < 3; k++) io[0x1F0 + k] = (uint8_t)(rnd() & 31);
        /* writes mid-frame: scroll, TM/TS, BGMODE, brightness, math, the
         * fixed colour, CGRAM (address, then two data writes) */
        int nlog = 0;
        if (mixed) {                                /* Mode 7 from this line on */
            uint16_t line = (uint16_t)(40 + rnd() % 140);
            log[nlog++] = (PpuWrite){line, 0x2105, 7, 0};
        }
        for (int i = 0, n = mixed ? 0 : (int)(rnd() % 8); i < n && nlog < 29; i++) {
            static const uint16_t regs[] = {0x210D, 0x210E, 0x2111, 0x2112, 0x212C, 0x212D, 0x2100, 0x2130,
                                            0x2131, 0x2132, 0x2126, 0x2127, 0x2105, 0x2121};
            static const uint16_t regs7[] = {0x211B, 0x211C, 0x211E, 0x211A, 0x212C, 0x2100, 0x2131, 0x2121};
            uint16_t line = (uint16_t)(10 + i * 26 + (rnd() % 20));
            uint16_t reg = m7f ? regs7[rnd() % 8] : regs[rnd() % 14];
            uint8_t v = (uint8_t)rnd();
            if (m7f && reg >= 0x211B && reg <= 0x2120) v = (uint8_t)(rnd() & 1 ? 0 : 0xFF);
            if (reg == 0x2105) v = (uint8_t)((v & 0xF8) | (m7f ? 7 : mode));   /* no mode change: one program */
            if (reg == 0x2100) v &= 0x0F;
            log[nlog++] = (PpuWrite){line, reg, v, 0};
            if (reg == 0x2121) {
                log[nlog++] = (PpuWrite){line, 0x2122, (uint8_t)rnd(), 0};
                log[nlog++] = (PpuWrite){line, 0x2122, (uint8_t)rnd(), 0};
            }
        }
        PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, abus_zero, NULL,
                       log, nlog, ident, hist, NULL, NULL, 0, NULL, NULL};
        memset(&plan, 0, sizeof plan);
        plan.tc = 1;
        int fail = dsp_plan(&in, &plan);
        int npg = 0;
        for (int pg = 0; pg < DSP_VPAGES; pg++)
            for (int b = 0; b < 5; b++) npg += (plan.fmt[pg] >> b) & 1;
        if (mixed) {
            /* no single program: runs, each planned for its band and drawn
             * by the model for its rows only */
            int run[4][2], nr = fail && plan.fail == 6 ? dsp_tc_runs(plan.rowkind, run, 4) : 0;
            if (!nr) {
                if (++tries < 200) { fr--; continue; }
                fprintf(stderr, "frame %d: mixed, but no runs (fail %d)\n", fr, plan.fail);
                return 1;
            }
            tries = 0;
            ppu_render_565(&in, ref);
            int bad2 = 0, out2 = 0, worst = 0;
            for (int k = 0; k < nr; k++) {
                static DspPlan bp;
                memset(&bp, 0, sizeof bp);
                bp.tc = 1;
                bp.band0 = run[k][0];
                bp.band1 = run[k][1];
                uint8_t bf[DSP_VPAGES] = {0};
                tc_model_band0 = bp.band0;
                tc_model_band1 = bp.band1;
                bad2 |= dsp_plan(&in, &bp) || tc_model_frame(bp.w, bp.n, bp.kernel, vram, mod, bf, NULL);
                tc_model_band0 = tc_model_band1 = 0;
                int sl = 0;
                for (int pg = 0; pg < DSP_VPAGES; pg++) {
                    out2 += (bf[pg] & ~bp.fmt[pg]) != 0;
                    for (int b = 0; b < 5; b++) sl += (bp.fmt[pg] >> b) & 1;
                }
                if (sl > worst) worst = sl;
            }
            int d = 0;
            for (int i = 0; i < PPU_W * PPU_H; i++) d += ref[i] != mod[i];
            if (d || out2 || bad2) {
                fprintf(stderr, "  frame %d (mixed, %d runs): %d pixels differ, %d pages outside, %s\n", fr, nr, d,
                        out2, bad2 ? "no plan" : "planned");
                bad++;
            }
            permode[7]++;
            printf("random frame %d: mixed, %d runs (first %d-%d), at most %d page slots a run: %s\n", fr, nr,
                   run[0][0], run[0][1], worst, d || out2 || bad2 ? "DIFFERS" : "exact");
            continue;
        }
        if ((fail || npg > TC_SLOTS_TILE) && ++tries < 200) {
            fr--;                                   /* too big for the program: another */
            continue;
        }
        tries = 0;
        if (fail) {
            fprintf(stderr, "frame %d: no plan\n", fr);
            return 1;
        }
        ppu_render_565(&in, ref);
        uint8_t fmt[DSP_VPAGES] = {0};
        int bs = tc_model_frame(plan.w, plan.n, plan.kernel, vram, mod, fmt, NULL);
        int d = 0, first = -1, out = 0;
        for (int i = 0; i < PPU_W * PPU_H; i++)
            if (ref[i] != mod[i]) { d++; if (first < 0) first = i; }
        for (int pg = 0; pg < DSP_VPAGES; pg++) out += (fmt[pg] & ~plan.fmt[pg]) != 0;
        if (d || out || bs) {
            fprintf(stderr, "  frame %d (mode %d): %d pixels differ (first row %d, x %d), %d pages outside the plan%s\n",
                    fr, mode, d, first / PPU_W, first % PPU_W, out, bs ? ", bad stream" : "");
            bad++;
        }
        permode[mode]++;
        printf("random frame %d: mode %d BGMODE $%02X TM $%02X TS $%02X CGWSEL $%02X CGADSUB $%02X writes %d "
               "sprites %d page slots %d words %d: %s\n", fr, mode, r[0x05], r[0x2C], r[0x2D], r[0x30], r[0x31],
               nlog, plan.sprites, npg, plan.n, d || out || bs ? "DIFFERS" : "exact");
        if (pk) tc_pack(pk, &plan, vram, ref);
    }
    printf("frames per mode:");
    for (int m = 0; m < 8; m++) printf(" %d", permode[m]);
    printf("; %d differ\n", bad);
    if (pk) fclose(pk);
    return bad != 0;
}

