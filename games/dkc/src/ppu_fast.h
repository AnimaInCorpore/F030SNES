/* The 256-colour display path (src/ppu_fast.c): rows composed a
 * character row at a time, pixels as CGRAM indices. */
#ifndef PPU_FAST_H
#define PPU_FAST_H

#include "snes_ppu.h"

#define PPU_PLANAR8_BYTES (PPU_W * PPU_H)   /* 8 planes: one byte per pixel */

/* Composes one frame into out[PPU_H][PPU_W] as CGRAM indices; equal to
 * ppu_render() with an identity map. in->map and in->hist are used only
 * for Mode 7 rows (reference path): pass an identity map and a scratch
 * histogram. */
void ppu_fast_render(const PpuInput *in, uint8_t *out);

/* The tile-row cache (decoded character rows) holds on to VRAM contents:
 * drop a 256-word page after writing it, or everything. */
void ppu_fast_invalidate_page(int page);
void ppu_fast_invalidate_all(void);

/* Packs a chunky frame into the Videl's 8-plane words: ppu_planar8 bit by
 * bit (the reference), ppu_c2p8 32 pixels at a time (the display path). */
void ppu_planar8(const uint8_t *chunky, uint16_t *planar);
void ppu_c2p8(const uint8_t *chunky, uint16_t *planar);

#endif
