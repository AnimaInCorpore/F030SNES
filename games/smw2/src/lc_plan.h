/* The list compositor's planner (analysis/list-compositor-dsp-plan.md): from the frame's inputs (VRAM
 * written since the last frame, OAM, registers, scroll) and nothing else it decides which 16-pixel groups of
 * the scroll ring the frame has to compose, which groups the other page missed, and where the Videl window
 * sits. Geometry: two pages of N groups in linear "helix" order (world group (X, Y) is group
 * (Y * 32 + X) mod N, X unbounded), the first LC_DUP groups mirrored behind the ring; the window is 224
 * rows of 16 (17 with HSCROLL) groups starting at S.
 *
 * The DSP renders the frame shifted right by dl pixels, so that the world's groups line up with its own: its
 * group j of row y is ring group (S + y * 32 + j) mod N; the window's 17th group (j = 16) comes from a second
 * render shifted by dl - 16. Used by falcon's runtime (src/smkx_lc.c) and by tools/lc/lcd_model.c, which checks
 * it against the reference. Freestanding. */
#ifndef LC_PLAN_H
#define LC_PLAN_H

#include "snes_ppu.h"

#define LC_RH 512
#define LC_NGR 32
#define LC_N (LC_RH * LC_NGR)
#define LC_DUP (223 * LC_NGR + 17)
#define LC_WINW 17

typedef struct {
    int S, dl;                          /* window start group (0..N-1), HSCROLL */
    int wx, wy;                         /* world origin of the window (unwrapped scroll of the ring layer) */
    int cold;                           /* every group of the window is composed */
    int ngroups;                        /* groups to compose (both renders) */
    uint16_t maskA[PPU_H];              /* per row: bit j = compose group j of the render shifted by dl */
    uint16_t maskB[PPU_H];              /* per row: bit 0 = compose group 16 (the render shifted by dl - 16) */
    uint32_t cmask[PPU_H];              /* per row: bit j = copy group j from the other page (copies[], by row) */
    int ncopies;
    uint16_t copies[LC_N];              /* linear groups to copy from the other page */
} LcPlan;

/* 0: the frame is covered and plan is filled; 1: the frame is not a picture (forced blank at its start, and nothing
 * else): nothing is planned and nothing is forgotten; -1: not covered (windows, mosaic, per-line changes, other
 * modes, forced blank, several OAM segments): the exact renderer draws it and the next covered frame starts
 * cold. vpages: per 256-word VRAM page, 1 when the game wrote it since the last frame (NULL: compare all pages,
 * which the host model does); pages not flagged are taken as unchanged. */
int lc_plan(const PpuInput *in, LcPlan *plan, const uint8_t *vpages);
extern uint32_t lc_diff;
extern uint32_t lc_why;                 /* the reasons the last call returned -1 (bit mask, see lc_plan.c) */

/* 1 when lc_plan would cover the frame, judged from the registers' replay alone (no state is touched). */
int lc_covers(const PpuInput *in);

/* The next frame starts cold (the pages are not known to hold the last frame). */
void lc_plan_reset(void);

#endif
