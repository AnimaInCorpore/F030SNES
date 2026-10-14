/* The DSP renderer's frame stream (falcon/dsp/ppu.asm, FRAME): see
 * ppu_dsp.c. */
#ifndef PPU_DSP_H
#define PPU_DSP_H

#include "snes_ppu.h"

/* Words to the DSP's host port, 24 bits each. */
typedef void (*PpuDspPut)(uint32_t word);

/* Sends one frame: the FRAME command, the register changes line by line,
 * OAM segments, END. The DSP draws the 224 lines and sends their planes
 * over the SSI. VRAM and the colour tables must be up already. */
void ppu_dsp_frame(const PpuInput *in, PpuDspPut put);

/* The same stream as SSI slots (two per word: bits 23-8, then 7-0) from
 * out on, with a fast path for plain Mode 7 lines; returns the end, or 0
 * when it would pass end. */
uint16_t *ppu_dsp_frame_slots(const PpuInput *in, uint16_t *out, uint16_t *end);

/* The frame's sprites as OAMSET's words (without the command), for the
 * given OAM and OBSEL; returns the number of words written to w (at most
 * 1 + 128 * 2). */
int ppu_dsp_oam(const uint8_t *oam, uint8_t obsel, uint32_t *w);

#endif
