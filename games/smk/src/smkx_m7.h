/* The DSP renderer on the Falcon (falcon/dsp/ppu.asm): see smkx_m7.c. */
#ifndef SMKX_M7_H
#define SMKX_M7_H

#include "snes_ppu.h"

#define SMKX_LINE_WORDS 64          /* a DSP line: the 64 plane words */
#define SMKX_HIST_WORDS 256         /* then, after the last line, the frame's
                                       samples per CGRAM index */

extern uint8_t smkx_m7_on;          /* the DSP renders whole frames */
extern uint32_t smkx_m7_rows, smkx_m7_segs, smkx_m7_pages, smkx_m7_timeouts;
extern uint32_t smkx_m7_spins, smkx_m7_wait;
extern uint32_t smkx_m7_t_up, smkx_m7_t_stream, smkx_m7_npages;

/* After Dsp_ExecBoot and the SSI -> DMA record route (supervisor): loads
 * the program beyond the boot image, checks the answer, sets the Videl's
 * line offset for the samples. 1 when the DSP renders. */
int smkx_m7_init(void);

/* A frame on the DSP into screen (SMKX_LINE_WORDS words a line): dirty
 * VRAM pages and a changed slot map go up now, the frame's register
 * stream then goes out from an interrupt while the 68030 goes on. */
void smkx_m7_start(const PpuInput *in, uint16_t *screen);

/* 1 once the frame is in screen, 0 while it is not, -1 when the DSP
 * stalled (the CPU renders from then on). */
int smkx_m7_poll(void);

/* The finished frame's samples into hist. */
void smkx_m7_samples(const uint16_t *screen, uint32_t *hist);

#endif
