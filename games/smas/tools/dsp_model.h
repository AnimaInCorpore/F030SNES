#ifndef DSP_MODEL_H
#define DSP_MODEL_H

#include "ppu_dsp.h"

/* Composes the frame a dsp_plan() stream for kernel (DSP_KERNEL_*)
 * describes into eight-plane words (128 per row). pages[] gets a 1 for
 * every 256-word VRAM page read. Returns the number of rows left alone
 * (Mode 7), or -1 for a bad stream. */
int dsp_model_frame(const uint32_t *w, int n, int kernel, const uint8_t *vram, uint16_t *planar,
                    uint8_t *pages);

#endif
