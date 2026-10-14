/* Partial rendering bookkeeping: see ppu_rows.h. Freestanding. */
#include "ppu_rows.h"

static uint32_t *verp(PartialState *s, const void *b)
{
    for (int i = 0; i < 2; i++)
        if (s->buf_addr[i] == b) return &s->buf_ver[i];
    for (int i = 0; i < 2; i++)
        if (!s->buf_addr[i]) {
            s->buf_addr[i] = b;
            s->buf_ver[i] = 0;
            return &s->buf_ver[i];
        }
    s->buf_addr[0] = b;                     /* a third buffer: forget the others */
    s->buf_ver[0] = 0;
    s->buf_addr[1] = 0;
    return &s->buf_ver[0];
}

void pr_commit(PartialState *s, const void *buf)
{
    uint32_t v = ++s->pic_id;
    for (int r = 0; r < PPU_H; r++)
        if (s->rows.dirty[r]) s->row_ver[r] = v;
    if (buf) *verp(s, buf) = v;
    dsp_rows_commit();
    for (int pg = 0; pg < DSP_VPAGES; pg++) s->pic_dirty[pg] = 0;
}

void pr_drawn_full(PartialState *s, const void *buf)
{
    uint32_t v = ++s->pic_id;
    for (int r = 0; r < PPU_H; r++) s->row_ver[r] = v;
    *verp(s, buf) = v;
    dsp_rows_forget();
    for (int pg = 0; pg < DSP_VPAGES; pg++) s->pic_dirty[pg] = 0;
}

const void *pr_newest(PartialState *s, const void *a, const void *b)
{
    return *verp(s, a) >= *verp(s, b) ? a : b;
}

int pr_begin(PartialState *s, const PpuInput *in, uint8_t *page_dirty, const void *a, const void *b, int sync)
{
    s->skipped = 0;
    for (int pg = 0; pg < DSP_VPAGES; pg++)
        if (page_dirty[pg]) {
            s->pic_dirty[pg] = 1;
            page_dirty[pg] = 0;
        }
    s->rows.vdirty = s->pic_dirty;
    if (s->mode == 0) {                     /* no detection: every row, and nothing to compare later pictures with */
        dsp_rows_forget();
        s->rows.all = 1;
        s->rows.ndirty = PPU_H;
        for (int r = 0; r < PPU_H; r++) s->rows.dirty[r] = 1;
    } else if (dsp_rows(in, &s->rows) < 0) {
        s->rows.all = 1;
        for (int r = 0; r < PPU_H; r++) s->rows.dirty[r] = 1;
    }
    /* the picture goes into the buffer with the older picture (a frame in flight is going into the
     * other: it counts as the newest); what is shown when the flight is over is the newest */
    uint32_t va = *verp(s, a), vc = *verp(s, b);
    uint32_t vb = va < vc ? va : vc, vf = va < vc ? vc : va;
    s->changed = s->rows.all || s->rows.ndirty > 0;
    int nb = 0, nf = 0;
    for (int r = 0; r < PPU_H; r++) {
        s->need[r] = s->rows.dirty[r] || s->row_ver[r] > vb;
        nb += s->need[r];
        nf += s->rows.dirty[r] || s->row_ver[r] > vf;
    }
    if (s->mode == 1 && nb == 0) {          /* (no skipping: draw a row) */
        s->need[0] = 1;
        nb = 1;
    }
    if (s->mode == 1 && nf == 0) nf = 1;
    if (nb == 0 || (nf == 0 && !sync)) {
        /* nothing to draw: the newest picture is current (skipped 1: it stays on screen), or for a caller
         * that presents the older buffer (sync), that one is (skipped 2) */
        s->skipped = nf == 0 && !sync ? 1 : 2;
        pr_commit(s, s->skipped == 2 ? (va < vc ? a : b) : 0);
    }
    return 0;
}

int pr_runs(const uint8_t *need, int gap, int max, int (*run)[2])
{
    int nr = 0;
    for (int r = 0; r < PPU_H;) {
        if (!need[r]) { r++; continue; }
        int a = r, b = r + 1, g = 0;
        for (r = b; r < PPU_H; r++) {
            if (need[r]) { b = r + 1; g = 0; }
            else if (++g >= gap) break;
        }
        run[nr][0] = a;
        run[nr][1] = b;
        nr++;
    }
    while (nr > max) {                      /* close the smallest gap */
        int best = 0;
        for (int k = 1; k + 1 < nr; k++)
            if (run[k + 1][0] - run[k][1] < run[best + 1][0] - run[best][1]) best = k;
        run[best][1] = run[best + 1][1];
        for (int k = best + 1; k + 1 < nr; k++) { run[k][0] = run[k + 1][0]; run[k][1] = run[k + 1][1]; }
        nr--;
    }
    if (nr == 1 && run[0][1] - run[0][0] >= PPU_H - 3) { run[0][0] = 0; run[0][1] = PPU_H; }
    return nr;
}
