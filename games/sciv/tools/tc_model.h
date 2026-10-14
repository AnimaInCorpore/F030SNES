#ifndef TC_MODEL_H
#define TC_MODEL_H

#include "ppu_dsp.h"

/* Composes the frame a true-colour dsp_plan() stream (DspPlan.tc, kernel
 * DSP_KERNEL_TC or DSP_KERNEL_TC7) describes into RGB565 pixels
 * (PPU_W * PPU_H). fmt[page] gets the conversions (ppu_tc.h) it read.
 * stats, if not NULL, counts what the DSP would do (tc_model.c). Returns
 * 0, or -1 for a bad stream. */
typedef struct {
    uint32_t tilerows, clear, opaque;       /* BG tile rows drawn: all 0, none 0 */
    uint32_t objslivers, objclear, objopaque;
    uint32_t mathpix, pix;                  /* pixels with colour math, all */
    uint32_t layerpix[5];                   /* layer lines drawn, per layer (OBJ 4) */
} TcStats;

int tc_model_frame(const uint32_t *w, int n, int kernel, const uint8_t *vram, uint16_t *out,
                   uint8_t *fmt, TcStats *stats);

/* Only rows [tc_model_band0, tc_model_band1) are drawn, as the DSP's BAND
 * command draws them (the rest of out untouched); 0, 0: every row. */
extern int tc_model_band0, tc_model_band1;

#endif
