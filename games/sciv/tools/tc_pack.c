/* The pack format of TCTEST (src/tctest.c), for tools/tc_snap.c and
 * tools/tc_rand.c: "TCP1", frames u32, then per frame the program u32,
 * stream words u32 and the words, page slots u32 and per slot page | fmt
 * << 8 u16 and its 256 raw VRAM words (u16; the DSP converts them), then
 * the expected RGB565 picture (PPU_W * PPU_H u16). Big-endian. */
#include "ppu_dsp.h"

#include <stdio.h>

static void be(FILE *f, uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; i--) fputc((int)(v >> (i * 8)) & 255, f);
}

void tc_pack(FILE *f, const DspPlan *plan, const uint8_t *vram, const uint16_t *pic)
{
    be(f, (uint32_t)plan->kernel, 4);
    be(f, (uint32_t)plan->n, 4);
    for (int i = 0; i < plan->n; i++) be(f, plan->w[i], 4);
    int slots = 0;
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        for (int b = 0; b < 5; b++) slots += (plan->fmt[pg] >> b) & 1;
    be(f, (uint32_t)slots, 4);
    /* the Mode 7 plane first, in page order (plane page p in slot p) */
    for (int pass = 0; pass < 2; pass++)
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        for (int b = 0; b < 5; b++) {
            if ((b == 4) != (pass == 0)) continue;
            if (!((plan->fmt[pg] >> b) & 1)) continue;
            be(f, (uint32_t)(pg | (1 << b) << 8), 2);
            for (int i = 0; i < 256; i++) {
                int a = (pg * 256 + i) * 2;
                be(f, (uint32_t)(vram[a] | vram[a + 1] << 8), 2);
            }
        }
    for (int i = 0; i < PPU_W * PPU_H; i++) be(f, pic[i], 2);
}

/* the header's frame count, once known */
void tc_pack_count(FILE *f, uint32_t frames)
{
    fseek(f, 4, SEEK_SET);
    be(f, frames, 4);
    fseek(f, 0, SEEK_END);
}
