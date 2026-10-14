/* Mode 7 line planner for the DSP kernel: see m7_plan.c. */
#ifndef M7_PLAN_H
#define M7_PLAN_H

#include "snes_ppu.h"

enum { M7_INSIDE = 1, M7_TILE0 = 2, M7_FILL = 3 };

typedef struct {
    uint8_t kind;
    uint16_t count;
    int32_t u, v;      /* 1/256 texels at the segment's first pixel */
    int32_t du, dv;    /* per-pixel steps */
} M7Seg;

/* A line needs at most 3 segments when clipped; a wrapping line at most
 * 2 + 2 * 32 cuts with 16-bit steps. */
#define M7_MAX_SEGS 72

/* Splits the line into segments; returns their number. */
int m7_plan(const PpuM7Line *l, M7Seg *segs);

#endif
