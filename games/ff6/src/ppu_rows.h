/* Partial rendering, the bookkeeping shared by the Falcon runtime (src/smkx_tc.c) and the host test
 * (tools/tc_snap.c -rowbuf): which rows of a picture must be drawn into the screen buffer it goes
 * into. dsp_rows (ppu_dsp.h) says which rows differ from the last committed picture; two screen
 * buffers alternate, each holds a picture of one version, and row_ver[r] is the version whose picture
 * last changed row r: a row is drawn when its row_ver is newer than the buffer's version. A picture
 * that changes no row of the newest one is not drawn. Freestanding. */
#ifndef PPU_ROWS_H
#define PPU_ROWS_H

#include "ppu_dsp.h"

typedef struct {
    int mode;                               /* 0: every row every picture; 1: changed rows, no picture skipped; 2: both */
    uint32_t pic_id;
    uint32_t row_ver[PPU_H];
    const void *buf_addr[2];
    uint32_t buf_ver[2];
    uint8_t pic_dirty[DSP_VPAGES];          /* VRAM pages written since the last committed picture */
    DspRows rows;
    uint8_t need[PPU_H];                    /* rows to draw into the buffer with the older picture */
    int changed;                            /* the picture differs from the last committed one (whatever the mode) */
    int skipped;                            /* 0; 1: nothing to draw, the newest picture is current and
                                               stays on screen; 2: (sync) nothing to draw, the older buffer
                                               is current: present it */
} PartialState;

/* The picture in `in`: fills s->need and s->skipped. page_dirty: the VRAM pages written since the
 * last call (cleared here, they join pic_dirty). a, b: the two screen buffers (in any order). sync: the
 * caller shows the older buffer (a snapshot), else the newest is what stays on screen. When
 * nothing is to be drawn the picture is committed here. Returns 0. */
int pr_begin(PartialState *s, const PpuInput *in, uint8_t *page_dirty, const void *a, const void *b, int sync);

/* The picture is drawn (or sent) into `buf`: it is the one later pictures are compared with */
void pr_commit(PartialState *s, const void *buf);

/* All of `buf` was drawn by another path: nothing to compare later pictures with */
void pr_drawn_full(PartialState *s, const void *buf);

/* The buffer with the newer picture */
const void *pr_newest(PartialState *s, const void *a, const void *b);

/* Runs of rows to draw from s->need: gaps under `gap` rows are drawn too, at most max runs (the
 * smallest gaps are closed first); a run of nearly the whole picture becomes the whole. Returns the
 * number of runs. */
int pr_runs(const uint8_t *need, int gap, int max, int (*run)[2]);

#endif
