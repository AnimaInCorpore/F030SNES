/* Falcon side of the DSP tile renderer (falcon/dsp/ppu.asm): see smkx_ppu.c. */
#ifndef SMKX_PPU_H
#define SMKX_PPU_H

#include "ppu_dsp.h"

#define PPU_SCREEN_BYTES (PPU_H * 256)      /* eight planes */

/* Counters the runtime's status line prints, in this order. Times in 1/38400 s. */
enum { PPU_ST_FRAMES, PPU_ST_UPLOADS, PPU_ST_FALLBACKS, PPU_ST_T_PLAN, PPU_ST_T_UPLOAD, PPU_ST_T_SEND,
       PPU_ST_T_WAIT, PPU_ST_BAD_SIZE, PPU_ST_BAD_KIND, PPU_ST_N };
extern uint32_t smkx_ppu_stat[PPU_ST_N];

/* Resets the DSP into a renderer program (DSP_KERNEL_TILE: ppu.asm, DSP_KERNEL_WIN: its window build);
 * 1 when it answers PING. The VRAM page cache starts empty. Supervisor mode. */
int smkx_ppu_boot(int kernel);
int smkx_ppu_loaded(void);

/* Plans the frame: the program it needs, or -1 when the DSP cannot take it (Mode 7 rows, too many pages,
 * too long a stream, mixed rows): nothing was sent. The 68030 only. */
int smkx_ppu_plan(const PpuInput *in);

/* The list compositor's version: DSP_KERNEL_LC or DSP_KERNEL_LCW (windows), or -1; see smkx_ppu.c. */
int smkx_ppu_plan_lc(const PpuInput *in, int force_win);

/* Sets the planned frame aside as number k (0 or 1) / takes it back (frames planned before the first is sent). */
void smkx_ppu_keep(int k);
void smkx_ppu_restore(int k);

/* Brings the planned frame's VRAM pages into the DSP, arms DMA record on screen and sends the frame. */
void smkx_ppu_send(uint16_t *screen);

/* The LC build (DSP_KERNEL_LC): as smkx_ppu_send, but LCMASK first (one word a row) and the list comes back into
 * list, words words long. */
void smkx_ppu_send_lc(const uint16_t *mask, uint16_t *list, uint32_t words);

/* 1 once the frame is in the screen buffer (clears the data cache). */
int smkx_ppu_done(void);

#endif
