/* DSP Mode 7 path on the Falcon: see smkx_m7.c. */
#ifndef SMKX_M7_H
#define SMKX_M7_H

#include "snes_ppu.h"

#define M7_PAGES 64                 /* VRAM words 0-$3FFF in 256-word pages */

extern uint8_t smkx_m7_on;
extern uint32_t smkx_m7_rows, smkx_m7_segs, smkx_m7_pages, smkx_m7_timeouts;
extern uint32_t smkx_m7_spins, smkx_m7_wait;

/* Resets the DSP into the kernel (rt_dsp_boot) and checks it: 1 when it
 * answers. The SSI -> DMA record route must be set up. */
int smkx_m7_boot(void);
int smkx_m7_init(void);

/* Before ppu_render: uploads dirty VRAM pages and a changed slot map;
 * returns the sink, or NULL when the CPU composes everything. */
const PpuM7Sink *smkx_m7_begin(uint16_t *planar, const uint8_t *map);

/* After ppu_render: waits for the record window, then writes the CPU's rows
 * and lays its pixels over the DSP rows. Also used without the DSP. */
void smkx_m7_finish(const uint8_t *chunky);

/* Profiling: the last frame's lines through the kernel again, into a
 * scratch frame, with nothing else running; returns 1/38400 s. */
uint32_t smkx_m7_replay(uint16_t *scratch);

#endif
