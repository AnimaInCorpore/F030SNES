/* The list compositor's planner (analysis/list-compositor-sciv.md): from the frame's inputs (VRAM
 * written since the last frame, OAM, registers, scroll) and nothing else it decides which 16-pixel groups of
 * the scroll ring the frame has to compose, which groups the other page missed, and where the Videl window
 * sits. Geometry: two pages of N groups in linear "helix" order (world group (X, Y) is group
 * (Y * 32 + X) mod N, X unbounded), the first LC_DUP groups mirrored behind the ring; the window is 224
 * rows of 16 (17 with HSCROLL) groups starting at S.
 *
 * The DSP renders the frame shifted right by dl pixels, so that the world's groups line up with its own: its
 * group j of row y is ring group (S + y * 32 + j) mod N; the window's 17th group (j = 16) comes from a second
 * render shifted by dl - 256. Used by falcon's runtime (src/smkx_lc.c) and by tools/lc/lcd_model.c, which checks
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
    int fixed;                         /* screen coordinates: mode/layer splits or Mode 7; retain unchanged groups */
    int color;                         /* preserve supported half-subscreen composition */
    uint8_t rowkind[PPU_H];             /* fixed: 0 blank, 1 tiled, 2 Mode 7 */
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
 * else): nothing is planned and nothing is forgotten; -1: not covered (mosaic, unsupported per-line changes,
 * other modes, 16x16 tiles, visible OAM segments, colour effects): the exact renderer draws it and the next covered frame starts
 * cold. Mode/layer/vertical-scroll splits and Mode 7 use a fixed screen plan;
 * half-subscreen addition uses the reference's indexed checkerboard. Other
 * colour math and Mode 7 windows/EXTBG remain unsupported.
 * vpages: per 256-word VRAM page, 1 when the game wrote it since the last frame (NULL: compare all pages,
 * which the host model does); pages not flagged are taken as unchanged. */
int lc_plan(const PpuInput *in, LcPlan *plan, const uint8_t *vpages);
extern uint32_t lc_diff;
extern uint32_t lc_why;
extern uint32_t lc_fixed_why;           /* why the last frame took the fixed plan: 1 Mode 7 row, 2 colour math, 3 a layout register, 4 a vertical scroll */
extern int lc_refuse_math;              /* 1: visible colour math or mid-frame CGRAM writes use the exact fallback */
extern int lc_split_ring;               /* 1: HDMA splits of mode, layers or vertical origin keep the ring; rows of another
                                         * configuration are composed whenever anything changed */

/* 1 when lc_plan would cover the frame (it replays the frame's registers; touches no state of the planner). */
int lc_covers(const PpuInput *in);

/* The next frame starts cold (the pages are not known to hold the last frame). */
void lc_plan_reset(void);

#endif
