/* The DSP renderer: what the CPU hands the DSP for one frame.
 *
 * The CPU runs the frame's register replay (ppu_replay: HDMA, IRQ writes,
 * OAM segments) and turns it into a word stream (24-bit words, the host
 * port's unit): the registers at row 0, then every change with the row it
 * takes effect from, then the sprites of each OAM segment. The DSP composes
 * every row from that and VRAM, and sends each row's eight plane words per
 * 16-pixel group over the SSI into DMA record. Pixels are CGRAM indices:
 * the Videl's 256 colours are CGRAM as the frame started.
 *
 * Reference: ppu_render() with the identity map + ppu_planar8(). The host
 * model (tools/dsp_model.c) is the DSP program's executable description;
 * falcon/dsp/ppu.asm follows it. Freestanding.
 *
 * Stream:
 *   DSP_CMD_FRAME
 *   DSP_NSLOT words: the slots at row 0
 *   nchange, then per change: row << 8 | slot, value (ascending row)
 *   nseg, then per OAM segment: first row, nspr, then per sprite
 *     x (9 bits) | y << 9 | large << 17,  tile | attr << 8
 *     in OAM order; sprites that are in range on none of the segment's
 *     rows, or never on screen horizontally, are left out.
 *
 * Two DSP programs take the stream: ppu.asm for the tile modes and its
 * Mode 7 build (M7K) for frames whose visible rows are all Mode 7. A frame
 * mixing both is left to the CPU (dsp_plan fails). For the Mode 7 program
 * the stream goes on after the OAM segments with one entry per row
 * (0..223): nseg, then per segment (m7_plan.h)
 *     kind << 16 | count
 *     for INSIDE and TILE0: v, u + $800000 (a1, a0), then the 48-bit step
 *     (dv << 24) + du as its high and low 24 bits (x1, x0)
 * Rows that are blank, not Mode 7 or have BG1 off have nseg 0. It reads
 * VRAM pages 0-63 (the Mode 7 plane) and the sprites' pages.
 *
 * The window build of the tile program (WIN) takes frames whose rows use
 * main-screen windows (ppu_win_layers): its row-0 slots go on with the
 * four window slots (DSP_NSLOT_WIN in all), and its changes may name
 * them. The other programs' streams have neither.
 *
 * Both tile programs' streams end with offset-per-tile: a count, 0 or 66,
 * then for BG1 and BG2 33 words each, ppu_opt_slivers' for the frame's
 * BG mode 2 rows (one table for the frame; a frame whose table changes
 * between rows, or with BG modes 3-6, is left to the CPU).
 */
#ifndef PPU_DSP_H
#define PPU_DSP_H

#include "snes_ppu.h"

enum {
    DSP_SLOT_INIDISP, DSP_SLOT_BGMODE, DSP_SLOT_TM, DSP_SLOT_OBSEL,
    DSP_SLOT_SC,                           /* BG1SC..BG4SC: 4 slots */
    DSP_SLOT_NBA = DSP_SLOT_SC + 4,        /* BG12NBA, BG34NBA */
    DSP_SLOT_OFS = DSP_SLOT_NBA + 2,       /* bgofs[0..7]: H1 V1 H2 V2 ... */
    DSP_NSLOT = DSP_SLOT_OFS + 8,
    /* the window build's: W12SEL | W34SEL << 8 | WOBJSEL << 16, WH0 | WH1
     * << 8, WH2 | WH3 << 8, WBGLOG | WOBJLOG << 8 | TMW << 16 */
    DSP_SLOT_WSEL = DSP_NSLOT, DSP_SLOT_WH01, DSP_SLOT_WH23, DSP_SLOT_WLOG,
    DSP_SLOT_SUB,                          /* the row's ppu_sub_cfg */
    DSP_NSLOT_WIN,
    /* the true-colour program's (ppu_tc.h): TS, TSW, CGWSEL | CGADSUB << 8,
     * the fixed colour r | g << 5 | b << 10 */
    DSP_SLOT_TS = DSP_NSLOT_WIN, DSP_SLOT_TSW, DSP_SLOT_MATH, DSP_SLOT_FIXED,
    DSP_NSLOT_TC
};
#define DSP_SLOT_CG 0xFF                   /* a TC change: CGRAM index << 16 | BGR555 */

#define DSP_CMD_FRAME 0x10

enum { DSP_KERNEL_TILE, DSP_KERNEL_M7, DSP_KERNEL_WIN, DSP_KERNEL_TC, DSP_KERNEL_TC7, DSP_KERNEL_LC, DSP_KERNEL_LCW };

/* The tile program's VRAM page slots: 45 in P:$1300-$3FFF, 32 in
 * P:$6000-$7FFF, 16 in P:$4700-$56FF; its stream holds 1520 words. */
#define DSP_TILE_SLOTS 93
#define DSP_TILE_STREAM_MAX 1520
/* The window build gives the last four to its routines (P:$5300-$54FF)
 * and tables (X:$1500-$16FF). */
#define DSP_WIN_SLOTS 89
static inline uint32_t dsp_tile_slot_p(int k)
{
    return k < 45 ? 0x1300u + (uint32_t)k * 256
         : k < 77 ? 0x6000u + (uint32_t)(k - 45) * 256
                  : 0x4700u + (uint32_t)(k - 77) * 256;
}

/* The LC build (ppu.asm, LCB=1: the list compositor's group composer, analysis/list-compositor-dsp-plan.md)
 * composes only the groups a per-row mask names and sends them as list records. Its masks and variables
 * (X:$0500-$05EF) take the end of the stream's room: the stream may hold 1024 words, and all 93 page slots of the tile
 * program stay (games/smw/F030DKC have them in X:$3000-$30FF = P:$7000, which leaves 77). */
#define DSP_LC_SLOTS DSP_TILE_SLOTS
#define DSP_LC_STREAM_MAX 1024
static inline uint32_t dsp_lc_slot_p(int k)
{
    return dsp_tile_slot_p(k);
}
/* The list compositor's build with the window build in it (LCB=1 WIN=1): the window build's routines and tables take the last four
 * slots (P:$5300-$56FF), as in the window build: 89 stay (games/smw/F030DKC have 73 of their 77). */
#define DSP_LCW_SLOTS DSP_WIN_SLOTS
#define DSP_CMD_LCMASK 0x30                /* 224 words: per row, bit g = compose group g */

/* The Mode 7 program's room (falcon/dsp/ppu.asm, M7K): VRAM pages 0-63 at
 * P:$1300 + 256 * page, packed for sampling as (tile * 64) << 8 | texel;
 * 12 pages for sprites at P:$7400-$7FFF, raw, where the sprites' pages go,
 * those below 64 as a second copy (a name table that wraps into the Mode
 * 7 plane); a longer stream. */
#define DSP_M7_PLANE_P 0x1300
#define DSP_M7_SPRITE_P 0x7400
#define DSP_M7_SPRITE_PAGES 12
#define DSP_M7_STREAM_MAX 1999

#define DSP_VPAGES 128                    /* 256-word VRAM pages */
#define DSP_MAX_WORDS 8192

/* Partial rendering: which rows of a picture differ from the picture last
 * committed (dsp_rows_commit), from the registers and HDMA each row is drawn
 * with, the CGRAM, the OAM and the VRAM the row's layers and sprites read. A
 * row not marked is drawn the same as it was: its pixels equal the reference's
 * for the committed picture (tc_snap -rowclean checks this against
 * ppu_render_565). */
typedef struct DspRows {
    uint8_t dirty[PPU_H];                 /* out: 1 = draw this row again */
    int ndirty;                           /* out: rows marked */
    int all;                              /* out: nothing to compare with (the first picture, a
                                             state too complex, a mid-frame CGRAM write): all rows marked */
    const uint8_t *vdirty;                /* in: per VRAM page, written since the last commit */
} DspRows;

typedef struct {
    uint32_t w[DSP_MAX_WORDS];
    int n;
    uint8_t need[DSP_VPAGES];             /* pages this frame may read */
    int nneed;
    uint8_t sprneed[DSP_VPAGES];          /* of them, the sprites' (the Mode 7 program
                                             keeps a raw copy of those below page 64) */
    int nsprneed;
    int m7rows;                           /* visible Mode 7 rows */
    int kernel;                           /* DSP_KERNEL_TILE, _M7 or _WIN */
    int winrows;                          /* visible rows windows cut (or the sub
                                             screen: subrows) */
    int changes, segs, sprites;
    /* Input: per 256-word VRAM page, nonzero when written since the
     * planner last looked (the runtime's rt_plan_dirty; the planner clears
     * what it consumed), or NULL. With it, map blocks cached from clean
     * pages are trusted; without it every cached block is compared with
     * VRAM. */
    uint8_t *vdirty;
    int force_win;                        /* input: a window build's stream even when no row needs it (the list compositor's
                                             second render must use the program of the first) */
    int tc;                               /* input: plan for the true-colour
                                             programs (ppu_tc.h) */
    uint8_t fmt[DSP_VPAGES];              /* tc: per page, the conversions read
                                             (TC_F2 | TC_F4 | TC_F8 | TC_RAW) */
    int prefer;                           /* input: the program for a frame with no
                                             visible row (the one loaded) */
    uint8_t rowkind[PPU_H];               /* per row: 0 blank, 1 tile mode,
                                             2 Mode 7 (a frame mixing them goes
                                             in runs of bands, smkx_tc.c) */
    int fail;                             /* why dsp_plan failed: 1 too many
                                             changes, 2 OAM segments, 3 offset-per-
                                             tile differs, 4 BG modes 3-6 (planar),
                                             5 Mode 7 words, 6 Mode 7 and tile rows,
                                             7 Mode 7 with windows (planar), 8 too
                                             many stream words; smkx_tc.c: 9 the
                                             stream longer than the program's */
    int band0, band1;                     /* input, tc: only rows [band0, band1)
                                             are drawn (the DSP's BAND command):
                                             need, fmt and the counts cover
                                             those; 0, 0: the whole frame */
    /* Colour math the palette can show exactly: every visible row applies
     * the fixed colour the same way (CGWSEL: always, no clip, the fixed
     * colour or an empty sub screen), and the CGRAM entries it changes are
     * drawn by no shown layer it leaves alone. */
    /* The sub screen (ppu_sub_cfg rows): rows, and per 16-entry CGRAM block
     * the free block its lightened copy goes to (0xFF: none), with the
     * average main colour the copies are lightened by. */
    int subrows;
    uint8_t subreloc[16];
    uint8_t subk[3];
    int mathpal;                          /* 1: dsp_math_palette applies */
    int use_events;                       /* input: replay the register changes dsp_rows recorded for this picture
                                             (0 if it recorded none): the HDMA tables the replay reads are live
                                             game memory, so two replays a moment apart can differ */
    int mathrows;                         /* visible rows with colour math */
    uint32_t mathmask[8];                 /* the entries it changes */
    uint8_t mathop;                       /* bit 7 subtract, bit 6 half */
    uint8_t mathfixed[3];
} DspPlan;

/* Plans one frame for the program it needs (p->kernel); 0, or -1 when the
 * stream does not fit or the frame mixes Mode 7 and tile rows. */
int dsp_plan(const PpuInput *in, DspPlan *p);

/* Detects the rows of the picture in `in` that differ from the last committed picture
 * (r->dirty); 0, or -1 when the picture cannot be planned (r->all is then set). It changes
 * no state that dsp_plan uses, except the map block cache (kept valid for r->vdirty). */
int dsp_rows(const PpuInput *in, DspRows *r);
/* The picture last passed to dsp_rows is now the one later pictures are compared with. */
void dsp_rows_commit(void);
/* No picture to compare with: the next dsp_rows marks every row. */
void dsp_rows_forget(void);
/* 1 when dsp_rows recorded every register change of its picture (dsp_plan then replays those, use_events) */
int dsp_rows_events_ok(void);

/* The frame's palette with its colour math (p->mathpal): the CGRAM
 * entries in p->mathmask get the fixed colour added or subtracted
 * (halved), as the SNES does it per pixel. out may be cgram. */
void dsp_math_palette(const DspPlan *p, const uint8_t *cgram, uint8_t *out);

/* The lightened copies for the sub screen's checkerboard (p->subreloc):
 * block b's colours c as min(31, 2 c + subk) in block subreloc[b]. */
void dsp_sub_palette(const DspPlan *p, const uint8_t *cgram, uint8_t *out);

/* A frame mixing Mode 7 and tile rows: runs of one kind of rows from
 * DspPlan.rowkind (blank rows join a neighbour), each for its own program
 * with DspPlan.band0/1; the number of runs, 0 if more than max. */
int dsp_tc_runs(const uint8_t *rowkind, int (*run)[2], int max);

/* Forgets the cached map blocks (VRAM changed without vdirty seeing it,
 * e.g. while another DSP program cleared the flags). */
void dsp_plan_reset(void);

#endif
