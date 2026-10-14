/* Falcon side of the DSP renderer (falcon/dsp/ppu.asm): see smkx_ppu.c. */
#ifndef SMKX_PPU_H
#define SMKX_PPU_H

#include "ppu_dsp.h"

#if SMKX_TC
#define PPU_SCREEN_BYTES (PPU_H * 512)      /* RGB565 (src/smkx_tc.c) */
#else
#define PPU_SCREEN_BYTES (PPU_H * 256)      /* eight planes */
#endif

/* Counters the runtime's status line prints, in this order: frames, VRAM
 * pages uploaded, frames the DSP could not take; then the time (1/38400 s)
 * spent planning, uploading and sending. */
enum { PPU_ST_FRAMES, PPU_ST_UPLOADS, PPU_ST_FALLBACKS, PPU_ST_T_PLAN, PPU_ST_T_UPLOAD,
       PPU_ST_T_SEND, PPU_ST_BAD_SIZE, PPU_ST_BAD_KIND, PPU_ST_SSI, PPU_ST_MATHPAL, PPU_ST_SUB,
       PPU_ST_BANDED, PPU_ST_SKIPPED, PPU_ST_PARTIAL, PPU_ST_ROWS, PPU_ST_CHK, PPU_ST_CHK_BAD, PPU_ST_CHANGED, PPU_ST_T_PRE, PPU_ST_PRE_LOST, PPU_ST_EARLY, PPU_ST_T_GAP, PPU_ST_T_GAPE, PPU_ST_N };
/* The last frame no program took: pages << 16 | stream words; then its
 * program << 16 | sprite pages ($FF << 16: the plan failed, e.g. mixed
 * Mode 7 and tile rows). Then the frames sent by DMA playback (SSI), those whose colour
 * math the palette shows (smkx_ppu_palette), and those with sub-screen
 * rows; true colour (smkx_tc.c): the frames drawn in bands. */
extern uint32_t smkx_ppu_stat[PPU_ST_N];

/* Resets the DSP into a renderer program (DSP_KERNEL_TILE: ppu.asm,
 * DSP_KERNEL_M7: its Mode 7 build); 1 when it answers PING. The VRAM page
 * cache starts empty. Supervisor mode. smkx_ppu_loaded: the program in the
 * DSP, or -1. */
int smkx_ppu_boot(int kernel);
int smkx_ppu_loaded(void);

/* Plans the frame, brings the VRAM pages it reads into the DSP, arms DMA
 * record on screen and sends the frame; 0 when the DSP cannot take it
 * (Mode 7 rows, too many pages, too long a stream): nothing was sent. */
int smkx_ppu_start(const PpuInput *in, uint16_t *screen);

/* The same in two steps, so the next frame can be planned while the DSP
 * still draws the last one: smkx_ppu_plan uses the 68030 only (-1: the DSP
 * cannot take the frame); smkx_ppu_send needs the DSP idle (the last frame
 * done) and sends the planned frame into screen. smkx_ppu_stale: 1 when
 * the game wrote a VRAM page the plan reads since it was made. */
int smkx_ppu_plan(const PpuInput *in);   /* the program it needs, or -1 */
int smkx_ppu_stale(void);
void smkx_ppu_send(uint16_t *screen);

/* The list compositor (src/smkx_lc.c, DSP_KERNEL_LC): sets the planned frame aside as number k (0-3) / takes it back
 * (frames planned before the first is sent); smkx_ppu_send_lc is smkx_ppu_send for the LC build: LCMASK first (one
 * word a row), and the list comes back into list, words words long. */
int smkx_ppu_plan_lc(const PpuInput *in, int force_win);   /* DSP_KERNEL_LC or DSP_KERNEL_LCW (windows), or -1 */
void smkx_ppu_keep(int k);
void smkx_ppu_restore(int k);
void smkx_ppu_send_lc(const uint16_t *mask, uint16_t *list, uint32_t words);

/* The planned frame's palette: its colour math where the palette shows
 * it exactly (DspPlan.mathpal), the sub screen's lightened copies
 * (DspPlan.subreloc); applied to cgram in place. */
void smkx_ppu_palette(uint8_t *cgram);

/* true colour (smkx_tc.c), partial rendering: smkx_ppu_plan finds the rows that differ from the
 * picture the buffer it draws into holds (dsp_rows, and the versions of the two screen buffers)
 * and plans only those. smkx_ppu_skipped: 1 when the last plan found nothing to draw for the picture
 * on screen (nothing was planned, nothing is sent, nothing is to be presented). smkx_ppu_sync: set
 * by the caller for a frame that must be in the back buffer (snapshots). smkx_ppu_drawn_full: the
 * CPU (or another path) drew all of screen: every later picture starts from that. */
int smkx_ppu_skipped(void);
uint16_t *smkx_ppu_newest(void);         /* the buffer with the newest picture (drawn or in flight) */
extern int smkx_ppu_sync;
void smkx_ppu_drawn_full(uint16_t *screen);
#if SMKX_PARTIAL_CHECK == 2
int smkx_ppu_check_full(void);                /* the planned picture drawn whole into a scratch buffer, waited for */
void smkx_ppu_check_compare(const uint16_t *buf);
const uint16_t *smkx_ppu_check_screen(void);
uint32_t smkx_ppu_pic(void);
#endif

/* 1 once the frame is in the screen buffer (clears the data cache). */
int smkx_ppu_done(void);

#endif
