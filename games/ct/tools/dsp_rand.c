/* Random frames for PPUTEST: what real frames rarely show (flipped and
 * large sprites at the screen's edges, 16x16 tiles, mode 0, BG3 on top,
 * wide and tall maps, fine scroll, register writes mid-frame, main-screen
 * windows: every fourth frame, the window build; the sub screen added:
 * every eighth, the window build too). Each frame
 * is planned (dsp_plan), composed by the reference (ppu_render with the
 * identity map + ppu_planar8), checked against the host model, and written
 * in the pack format of tools/dsp_snap.c.
 *
 *   build/dsp_rand FRAMES SEED pack.bin
 *
 * VRAM stays within what the DSP holds at once: map entries name the
 * first 128 characters, sprites the first four character rows of each
 * name table. */
#include "dsp_model.h"

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

static void be(FILE *f, uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; i--) fputc((int)(v >> (i * 8)) & 255, f);
}

static uint8_t abus_zero(uint32_t a, void *ctx) { (void)a; (void)ctx; return 0; }

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s FRAMES SEED pack.bin\n", argv[0]);
        return 2;
    }
    int frames = atoi(argv[1]);
    rng = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    FILE *f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); return 2; }
    fwrite("PPU2", 1, 4, f);
    be(f, (uint32_t)frames, 4);
    static uint8_t vram[65536], cgram[512], oam[544], io[0x6000];
    static uint8_t ident[256], chunky[PPU_W * PPU_H];
    static uint16_t ref[PPU_W * PPU_H / 2], mod[PPU_W * PPU_H / 2];
    static uint32_t hist[256];
    static DspPlan plan;
    static PpuWrite log[16];
    for (int i = 0; i < 256; i++) ident[i] = (uint8_t)i;
    int bad = 0, m7tries = 0;
    for (int fr = 0; fr < frames; fr++) {
        for (int i = 0; i < 65536; i++) vram[i] = (uint8_t)rnd();
        memset(io, 0, sizeof io);
        uint8_t *r = io + 0x100;                     /* $2100.. */
        r[0x00] = 0x0F;
        /* every fourth frame Mode 7 (the Mode 7 program), unless DR_NOM7 */
        int m7f = !getenv("DR_NOM7") && (fr & 3) == 3;
        int mode = m7f ? 7 : (rnd() & 3) == 0 ? 0 : 1;
        r[0x05] = (uint8_t)(mode | ((rnd() & 1) << 3) | ((rnd() & 15) << 4));
        if (getenv("DR_NOBIG")) r[0x05] &= 0x0F;
        r[0x2C] = (uint8_t)(0x10 | (rnd() & 15));
        r[0x01] = (uint8_t)((rnd() & 7) << 5 | (rnd() & 3) << 3 | 3);  /* OBJ at $6000 */
        /* maps at $4000-$5FFF (32x32 or 64x32), characters at $0000 or $1000 */
        for (int i = 0; i < 4; i++) r[0x07 + i] = (uint8_t)((0x40 + i * 8) | (rnd() & 1));
        r[0x0B] = (uint8_t)(rnd() & 0x11);
        r[0x0C] = (uint8_t)(rnd() & 0x11);
        uint16_t bgofs[8], m7[6] = {0}, m7ofs[2] = {0};
        for (int i = 0; i < 8; i++) bgofs[i] = (uint16_t)(rnd() & 0x3FF);
        if (m7f) {
            static const uint8_t sels[] = {0x00, 0x80, 0xC0};
            r[0x05] = 7;
            r[0x2C] = (uint8_t)((rnd() & 1) | 0x10);
            r[0x1A] = (uint8_t)(sels[rnd() % 3] | (rnd() & 3));
            for (int i = 0; i < 4; i++) {          /* A, D: scale 0.5-2; B, C: shear */
                int v = (int)(rnd() % 256) - 128;
                if (i == 0 || i == 3) v = (int)(128 + rnd() % 384) * ((rnd() & 1) ? 1 : -1);
                m7[i] = (uint16_t)v;
            }
            m7[4] = (uint16_t)(rnd() & 0x1FFF);
            m7[5] = (uint16_t)(rnd() & 0x1FFF);
            m7ofs[0] = (uint16_t)(rnd() & 0x1FFF);
            m7ofs[1] = (uint16_t)(rnd() & 0x1FFF);
        }
        /* map entries: characters 0-127 (0-63 for 16x16 tiles, whose
         * neighbours +1, +16, +17 must stay inside too); every fourth tile
         * frame ("wide") any character, so the frame needs most of the
         * tile program's 93 page slots */
        int wide = !m7f && (fr & 3) == 1;
        for (int w = 0x4000; w < 0x6000; w++) {
            uint16_t e = (uint16_t)(vram[w * 2] | vram[w * 2 + 1] << 8);
            if (!wide) e = (uint16_t)((e & 0xFC00) | (e & 0x3F) | ((e >> 1) & 0x40));
            vram[w * 2] = (uint8_t)e;
            vram[w * 2 + 1] = (uint8_t)(e >> 8);
        }
        for (int i = 0; i < 512; i++) cgram[i] = (uint8_t)rnd();
        /* sprites: tiles in character rows 0-3, positions around the edges */
        for (int i = 0; i < 128; i++) {
            uint8_t *e = oam + i * 4;
            uint32_t v = rnd();
            int x = (int)(v % 320) - 32;
            e[0] = (uint8_t)x;
            e[1] = (uint8_t)(rnd() % 240);
            if (getenv("DR_NOSPR")) e[1] = 240;
            e[2] = (uint8_t)(rnd() & 0x3F);
            e[3] = (uint8_t)(rnd() & 0xFF);
            int hi = ((x < 0) ? 1 : 0) | (int)(rnd() & 2);
            oam[512 + (i >> 2)] = (uint8_t)((oam[512 + (i >> 2)] & ~(3 << ((i & 3) * 2))) |
                                            (hi << ((i & 3) * 2)));
        }
        /* windows: random selects, logic, TMW and edges (near each other
         * and the ends as often as not), unless DR_NOWIN */
        int winf = !m7f && !getenv("DR_NOWIN") && (fr & 3) == 2;
        if (winf) {
            for (int n = 0x23; n <= 0x2B; n++) r[n] = (uint8_t)rnd();
            r[0x2E] = (uint8_t)(rnd() | 1);
            for (int n = 0x26; n <= 0x29; n++)
                if (rnd() & 1) r[n] = (uint8_t)((rnd() & 1) ? (rnd() & 15) : 255 - (rnd() & 15));
        }
        /* the sub screen added with colour math (ppu_sub_cfg), unless
         * DR_NOSUB: BG1/BG2/BG4 maps and sprites on palettes 0-3, BG3's on
         * 4-7, so free CGRAM blocks take the lightened copies */
        int subf = !m7f && !getenv("DR_NOSUB") && (fr & 7) == 4;
        if (subf) {
            r[0x30] = 0x02;
            r[0x31] = (uint8_t)((rnd() & 0x3F) | 1 | ((rnd() & 3) == 0 ? 0x40 : 0));
            r[0x2D] = (uint8_t)((rnd() & 0x1F) | 4);
            for (int w = 0x4000; w < 0x6000; w++) {
                uint8_t *hi = &vram[w * 2 + 1];
                *hi = (uint8_t)((*hi & ~0x1C) | ((w >= 0x4800 && w < 0x4C00) ? 0x10 : 0) | (*hi & 0x0C));
            }
            for (int i = 0; i < 128; i++) oam[i * 4 + 3] &= (uint8_t)~0x08;
            if (rnd() & 1) cgram[0] = cgram[1] = 0;
        }
        /* a few register writes mid-frame: scroll, TM, BGMODE; window
         * edges and selects in window frames */
        int nlog = (int)(rnd() % 6);
        if (getenv("DR_NOLOG")) nlog = 0;
        for (int i = 0; i < nlog; i++) {
            static const uint16_t regs[] = {0x210D, 0x210E, 0x210F, 0x2110, 0x2111, 0x2112, 0x212C, 0x2105};
            /* Mode 7: the matrix and M7SEL change mid-frame */
            static const uint16_t regs7[] = {0x211B, 0x211C, 0x211D, 0x211E, 0x211F, 0x2120, 0x211A, 0x210D};
            log[i].line = (uint16_t)(20 + i * 30 + (rnd() % 20));
            static const uint16_t regsw[] = {0x2126, 0x2127, 0x2128, 0x2129, 0x2123, 0x2125, 0x212A, 0x212E};
            log[i].reg = m7f ? regs7[rnd() % 8] : winf && (rnd() & 1) ? regsw[rnd() % 8] : regs[rnd() % 8];
            log[i].value = (uint8_t)rnd();
            if (m7f && log[i].reg >= 0x211B && log[i].reg <= 0x2120 && (log[i].reg & 1))
                log[i].value = (uint8_t)(rnd() & 1 ? 0 : 0xFF);   /* matrix high bytes stay small */
            if (log[i].reg == 0x2105) log[i].value = (uint8_t)((log[i].value & 0xF8) | (rnd() & 1));
            if (log[i].reg == 0x212C) log[i].value |= 0x10;
            log[i].pad = 0;
        }
        PpuInput in = {vram, cgram, oam, io, bgofs, m7, m7ofs, abus_zero, NULL,
                       log, nlog, ident, hist, NULL, NULL, 0, NULL, NULL};
        int fail = dsp_plan(&in, &plan);
        in.subreloc = plan.subreloc;
        ppu_render(&in, chunky);
        ppu_planar8(chunky, ref);
        if (m7f && !fail && (plan.nsprneed > DSP_M7_SPRITE_PAGES || plan.n - 1 > DSP_M7_STREAM_MAX) &&
            ++m7tries < 50) {
            fr--;                                   /* a Mode 7 frame too long for the program: another */
            continue;
        }
        if (wide && !fail && plan.nneed > DSP_TILE_SLOTS && ++m7tries < 200) {
            fr--;                                   /* too many pages even for 93 slots: another */
            continue;
        }
        m7tries = 0;
        int slots = plan.kernel == DSP_KERNEL_WIN ? DSP_WIN_SLOTS : DSP_TILE_SLOTS;
        if (fail || (plan.kernel == DSP_KERNEL_M7 ? plan.nsprneed > DSP_M7_SPRITE_PAGES ||
                                                     plan.n - 1 > DSP_M7_STREAM_MAX
                                                   : plan.nneed > slots || plan.n - 1 > DSP_TILE_STREAM_MAX)) {
            fprintf(stderr, "frame %d: does not fit (pages %d, words %d)\n", fr, plan.nneed, plan.n);
            return 1;
        }
        uint8_t pages[DSP_VPAGES] = {0};
        memset(mod, 0, sizeof mod);
        dsp_model_frame(plan.w, plan.n, plan.kernel, vram, mod, pages);
        int d = 0, dp = 0, first = -1;
        for (int i = 0; i < PPU_W * PPU_H / 2; i++)
            if (ref[i] != mod[i]) { d++; if (first < 0) first = i; }
        for (int i = 0; i < DSP_VPAGES; i++) dp += pages[i] && !plan.need[i];
        if (d || dp) fprintf(stderr, "  frame %d: %d words differ (first row %d), %d pages read outside the plan\n",
                             fr, d, first / 128, dp);
        d += dp;
        bad += d != 0;
        int nrel = 0;
        for (int b = 0; b < 16; b++) nrel += plan.subreloc[b] != 0xFF;
        printf("random frame %d%s: mode %d BGMODE $%02X TM $%02X writes %d sprites %d pages %d words %d relocated %d model %s\n",
               fr, wide ? " (wide)" : subf && plan.subrows ? " (sub screen)" : plan.kernel == DSP_KERNEL_WIN ? " (windows)" : "", mode, r[0x05], r[0x2C], nlog, plan.sprites, plan.nneed, plan.n, nrel, d ? "DIFFERS" : "exact");
        be(f, (uint32_t)plan.kernel, 4);
        be(f, (uint32_t)plan.n, 4);
        for (int i = 0; i < plan.n; i++) be(f, plan.w[i], 4);
        /* the Mode 7 program: the sprites' pages below 64 once more, raw */
        int copies = 0;
        if (plan.kernel == DSP_KERNEL_M7)
            for (int pg = 0; pg < 64; pg++) copies += plan.sprneed[pg];
        be(f, (uint32_t)(plan.nneed + copies), 4);
        for (int pg = 0; pg < DSP_VPAGES + 64; pg++) {
            int q = pg < DSP_VPAGES ? pg : pg - DSP_VPAGES;
            if (pg < DSP_VPAGES ? !plan.need[q] : plan.kernel != DSP_KERNEL_M7 || !plan.sprneed[q]) continue;
            be(f, (uint32_t)(pg < DSP_VPAGES ? q : q | 0x8000), 2);
            for (int i = 0; i < 256; i++) {
                int a = (q * 256 + i) * 2;
                be(f, (uint32_t)(vram[a] | vram[a + 1] << 8), 2);
            }
        }
        for (int i = 0; i < PPU_H * 128; i++) be(f, ref[i], 2);
    }
    fclose(f);
    return bad != 0;
}
